#include "pc/audio.h"
#include "pc/log.h"
#include "p3d/fileio.h"
#include "psp/psp_perf.h"
#include "snd/adpcm.h"

#include <pspaudiolib.h>
#include <pspaudio_kernel.h>
#include <pspkernel.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr u32 kMaxSamples = 128;
constexpr u32 kMaxVoices = 32;
constexpr u32 kOutputRate = 44100;
constexpr u32 kOutputFrames = 1024;
constexpr u32 kMusicChunkBlocks = 2048;
constexpr u32 kMusicChunkBytes = kMusicChunkBlocks * 16;
constexpr u32 kMusicChunkSamples = kMusicChunkBlocks * 28;
constexpr u32 kInvalidPair = 0xFFFFFFFFu;
constexpr u32 kInvalidBuffer = 0xFFFFFFFFu;
// Allegrex has no hardware double precision. Software double operations in
// every output sample can exceed the 23.22 ms audio period and starve the
// lower-priority game/decoder threads. A 34.30 cursor retains sub-sample
// precision without losing accuracy on long tracks or calling libgcc per PCM
// sample. The spare high bits also keep cursor + step safely below u64 overflow.
constexpr u32 kCursorFractionBits = 30;
constexpr u64 kCursorOne = 1ull << kCursorFractionBits;
constexpr u32 kCursorFractionMask = static_cast<u32>(kCursorOne - 1u);
// The callback thread created by pspaudiolib runs at 0x12. Keep the decoder
// below it, but above the game thread (crt0 default 0x20) so the next pair is
// prepared during the current pair instead of arriving late at a boundary.
// At 0x24 it only ran while the game thread blocked: with triple buffering
// that thread no longer sleeps on VBlank, and most music callbacks underran.
constexpr int kMusicWorkerPriority = 0x1C;
constexpr int kMusicWorkerStackSize = 0x4000;

struct Sample {
    std::vector<s16> pcm;
    u32 frames = 0;
    u32 rate = kOutputRate;
    u32 channels = 1;
    bool loaded = false;
};

struct Voice {
    AudioSample sample = AUDIO_SAMPLE_INVALID;
    u64 cursor = 0;
    float volume = 1.0f;
    float targetVolume = 1.0f;
    float pan = 0.0f;
    float pitch = 1.0f;
    float minDistance = 0.0f;
    float maxDistance = 10000.0f;
    LVector position{};
    u8 priority = 0;
    u32 fadeFrames = 0;
    // Cursor step cached for (stepRate, stepPitchBits); see CachedCursorStep.
    u64 step = 0;
    u32 stepRate = 0;
    u32 stepPitchBits = 0;
    bool loop = false;
    bool reserved = false;
    bool active = false;
};

struct MusicChunk {
    u32 pair = kInvalidPair;
    std::array<s16, kMusicChunkSamples> left = {};
    std::array<s16, kMusicChunkSamples> right = {};
};

// FAG stores alternating 32 KB mono ADPCM chunks (left, right). The audio
// callback must never touch the Memory Stick or decode ADPCM: either operation
// can exceed the PSP audio period and cause an audible underrun. A kernel
// worker keeps one decoded pair playing while it fills the other pair ahead of
// the cursor.
struct MusicStream {
    std::unique_ptr<p3d::io::FileHandle> file;
    u32 dataOffset = 0;
    u32 dataEnd = 0;
    u32 pairCount = 0;
    u32 framesPerPair = kMusicChunkSamples;
    u32 totalFrames = 0;
    u32 currentPair = kInvalidPair;
    std::atomic_uint currentBuffer{kInvalidBuffer};
    std::atomic_uint readyBuffer{kInvalidBuffer};
    std::atomic_uint readyPair{kInvalidPair};
    std::atomic_uint requestedPair{kInvalidPair};
    std::atomic_bool stopWorker{false};
    std::atomic_bool workerError{false};
    SceUID workerThread = -1;
    SceUID requestSema = -1;
    u64 cursor = 0;
    float volume = 1.0f;
    float targetVolume = 1.0f;
    u32 fadeFrames = 0;
    // Last mixed music sample is used as a bounded fallback if the worker
    // misses a pair boundary. Holding the tail is less destructive than
    // inserting a full 1024-frame hole in the output.
    float holdLeft = 0.0f;
    float holdRight = 0.0f;
    bool loop = true;
    std::atomic_bool active{false};
    std::array<u8, kMusicChunkBytes * 2> compressedPair = {};
    std::array<MusicChunk, 2> chunks = {};
};

std::array<Sample, kMaxSamples> g_samples;
std::array<Voice, kMaxVoices> g_voices;
bool g_initialized = false;
bool g_mono = false;
bool g_surround = false;
float g_masterVolume = 1.0f;
AudioVoice g_musicVoice = AUDIO_VOICE_INVALID;
MusicStream g_musicStream;
std::atomic_uint g_musicMixers{0};
LVector g_listener{};
s32 g_listenerYaw = 0;

static Sample* GetSample(AudioSample handle) {
    if (handle == AUDIO_SAMPLE_INVALID || handle > g_samples.size()) return nullptr;
    Sample& sample = g_samples[handle - 1];
    return sample.loaded ? &sample : nullptr;
}

static Voice* GetVoice(AudioVoice handle) {
    if (handle == AUDIO_VOICE_INVALID || handle > g_voices.size()) return nullptr;
    Voice& voice = g_voices[handle - 1];
    return voice.active || voice.reserved ? &voice : nullptr;
}

static u32 ReadU32(const u8* data) {
    u32 value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

// Kept out of line: the Allegrex has no double-precision FPU, so this runs on
// soft-float helpers and must stay off the audio callback's common path.
__attribute__((noinline)) static u64 VoiceCursorStep(u32 rate, float pitch) {
    const float finitePitch = std::isfinite(pitch) ? std::max(pitch, 0.05f) : 1.0f;
    // Only recomputed when rate or pitch changes (CachedCursorStep). Bound
    // pathological input before converting to an integer; normal playback
    // keeps its pitch. The double precision is deliberate (exact step).
    const double frames = static_cast<double>(rate) / kOutputRate * static_cast<double>(finitePitch);
    return static_cast<u64>(std::min(frames, static_cast<double>(UINT32_MAX)) * kCursorOne);
}

// VoiceCursorStep is a pure function of (rate, pitch). Pitch and rate only
// change on voice start, SetVoicePitch or a sample reload, so recompute it only
// when either input changes (pitch compared bit-for-bit, so NaN is stable;
// stepRate 0 never matches a real rate, so a fresh voice always computes it).
// The step is bit-identical to calling VoiceCursorStep every callback.
static u64 CachedCursorStep(Voice& voice, u32 rate) {
    u32 pitchBits = 0;
    static_assert(sizeof(pitchBits) == sizeof(voice.pitch), "float bit pattern");
    std::memcpy(&pitchBits, &voice.pitch, sizeof(pitchBits));
    if (voice.stepRate != rate || voice.stepPitchBits != pitchBits) {
        voice.step = VoiceCursorStep(rate, voice.pitch);
        voice.stepRate = rate;
        voice.stepPitchBits = pitchBits;
    }
    return voice.step;
}

// The worker runs above the game thread; a whole pair (~19 ms of decode on
// the PSP-3000) in one piece took that time out of a single frame. Yield
// between slices instead: a pair still decodes in a fraction of its ~2.6 s.
constexpr u32 kMusicDecodeSliceBlocks = 128;
constexpr SceUInt kMusicDecodeYieldUs = 500;

// Returns the decode time, sleeps excluded.
static u32 DecodeMusicChunk(const u8* chunkData, std::array<s16, kMusicChunkSamples>& output) {
    s32 prev0 = 0;
    s32 prev1 = 0;
    u32 busyUs = 0;
    for (u32 slice = 0; slice < kMusicChunkBlocks; slice += kMusicDecodeSliceBlocks) {
        const u64 sliceStart = sceKernelGetSystemTimeWide();
        const u32 sliceEnd = std::min(slice + kMusicDecodeSliceBlocks, kMusicChunkBlocks);
        for (u32 block = slice; block < sliceEnd; ++block) {
            s16 blockOutput[28];
            SpuAdpcm::DecodeBlock(chunkData + block * 16, blockOutput, prev0, prev1);
            std::memcpy(output.data() + block * 28, blockOutput, sizeof(blockOutput));
        }
        busyUs += static_cast<u32>(sceKernelGetSystemTimeWide() - sliceStart);
        if (sliceEnd < kMusicChunkBlocks) sceKernelDelayThread(kMusicDecodeYieldUs);
    }
    return busyUs;
}

static bool PrepareMusicPair(u32 pair, MusicChunk& destination) {
    if (pair >= g_musicStream.pairCount) {
        return false;
    }
    const u32 chunk = pair * 2;
    const u32 leftOffset = g_musicStream.dataOffset + chunk * kMusicChunkBytes;
    if (leftOffset > g_musicStream.dataEnd
        || g_musicStream.dataEnd - leftOffset < kMusicChunkBytes * 2
        || !g_musicStream.file
        || !g_musicStream.file->Seek(leftOffset, SEEK_SET)
        || g_musicStream.file->Read(g_musicStream.compressedPair.data(),
                                    g_musicStream.compressedPair.size())
            != g_musicStream.compressedPair.size()) {
        return false;
    }

    const u32 decodeUs = DecodeMusicChunk(g_musicStream.compressedPair.data(), destination.left)
        + DecodeMusicChunk(g_musicStream.compressedPair.data() + kMusicChunkBytes, destination.right);
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::RecordMusicDecode(decodeUs);
#else
    (void)decodeUs;
#endif
    destination.pair = pair;
    return true;
}

static int MusicWorkerEntry(SceSize, void*) {
    while (!g_musicStream.stopWorker.load(std::memory_order_acquire)) {
        if (g_musicStream.requestSema < 0
            || sceKernelWaitSema(g_musicStream.requestSema, 1, nullptr) < 0) {
            break;
        }
        if (g_musicStream.stopWorker.load(std::memory_order_acquire)) break;

        const u32 pair = g_musicStream.requestedPair.load(std::memory_order_acquire);
        if (pair == kInvalidPair || pair >= g_musicStream.pairCount) continue;
        if (g_musicStream.readyPair.load(std::memory_order_acquire) == pair) continue;

        // With two buffers, one is being consumed and the other is either free
        // or already queued for the callback. Never overwrite either one.
        const u32 current = g_musicStream.currentBuffer.load(std::memory_order_acquire);
        const u32 queued = g_musicStream.readyBuffer.load(std::memory_order_acquire);
        if (queued != kInvalidBuffer) {
            // The callback signals the semaphore again as soon as it promotes
            // the queued pair and releases this buffer.
            continue;
        }
        const u32 target = current == 0u ? 1u : 0u;
        if (target >= g_musicStream.chunks.size()) continue;
        if (!PrepareMusicPair(pair, g_musicStream.chunks[target])) {
            g_musicStream.workerError.store(true, std::memory_order_release);
            PspPerf::RecordAudioWorkerError();
            g_musicStream.active.store(false, std::memory_order_release);
            continue;
        }

        // Publish the buffer only after both channel arrays are complete.
        g_musicStream.readyBuffer.store(target, std::memory_order_release);
        g_musicStream.readyPair.store(pair, std::memory_order_release);
    }
    return sceKernelExitThread(0);
}

static void RequestMusicPair(u32 pair) {
    if (pair == kInvalidPair || g_musicStream.requestSema < 0) return;
    g_musicStream.requestedPair.store(pair, std::memory_order_release);
    // The request value is atomic, so a failed signal merely means the worker
    // is already awake and will observe the newest requested pair.
    sceKernelSignalSema(g_musicStream.requestSema, 1);
}

static bool StartMusicWorker() {
    g_musicStream.stopWorker.store(false, std::memory_order_release);
    g_musicStream.workerError.store(false, std::memory_order_release);
    g_musicStream.requestSema = sceKernelCreateSema(
        "ReChainMusic", 0, 0, 1, nullptr);
    if (g_musicStream.requestSema < 0) return false;

    g_musicStream.workerThread = sceKernelCreateThread(
        "ReChainMusic", MusicWorkerEntry, kMusicWorkerPriority,
        kMusicWorkerStackSize, 0, nullptr);
    if (g_musicStream.workerThread < 0) {
        sceKernelDeleteSema(g_musicStream.requestSema);
        g_musicStream.requestSema = -1;
        return false;
    }
    if (sceKernelStartThread(g_musicStream.workerThread, 0, nullptr) < 0) {
        sceKernelDeleteThread(g_musicStream.workerThread);
        g_musicStream.workerThread = -1;
        sceKernelDeleteSema(g_musicStream.requestSema);
        g_musicStream.requestSema = -1;
        return false;
    }
    return true;
}

static void StopMusicWorker() {
    g_musicStream.stopWorker.store(true, std::memory_order_release);
    if (g_musicStream.requestSema >= 0) {
        sceKernelSignalSema(g_musicStream.requestSema, 1);
    }
    if (g_musicStream.workerThread >= 0) {
        SceUInt timeout = 5000000;
        if (sceKernelWaitThreadEnd(g_musicStream.workerThread, &timeout) < 0) {
            sceKernelTerminateThread(g_musicStream.workerThread);
            sceKernelWaitThreadEnd(g_musicStream.workerThread, nullptr);
        }
        sceKernelDeleteThread(g_musicStream.workerThread);
        g_musicStream.workerThread = -1;
    }
    if (g_musicStream.requestSema >= 0) {
        sceKernelDeleteSema(g_musicStream.requestSema);
        g_musicStream.requestSema = -1;
    }
}

static bool WaitForMusicPair(u32 pair) {
    for (u32 attempt = 0; attempt < 5000; ++attempt) {
        if (g_musicStream.readyPair.load(std::memory_order_acquire) == pair) return true;
        if (g_musicStream.workerError.load(std::memory_order_acquire)) return false;
        sceKernelDelayThread(1000);
    }
    return false;
}

static bool PromoteMusicPair(u32 pair) {
    if (g_musicStream.readyPair.load(std::memory_order_acquire) != pair) return false;
    const u32 buffer = g_musicStream.readyBuffer.load(std::memory_order_acquire);
    if (buffer >= g_musicStream.chunks.size()) return false;
    g_musicStream.currentBuffer.store(buffer, std::memory_order_release);
    g_musicStream.currentPair = pair;
    g_musicStream.readyPair.store(kInvalidPair, std::memory_order_release);
    g_musicStream.readyBuffer.store(kInvalidBuffer, std::memory_order_release);
    return true;
}

// Fixed-point mixing. The callback preempts the game thread every 23.2 ms;
// its float path (int/float conversions and a clamp per voice and sample)
// took ~3.2 ms of each period on the PSP-3000, about 14% of the CPU.
// Voices and music add into a 24.8 accumulator (PCM level * 256), rounded
// toward zero and clamped once per output sample, as the float path's s16
// conversion did for a single source.
constexpr u32 kMixFractionBits = 8;
constexpr u32 kGainBits = 16;  // 1.0 == 65536
constexpr u32 kLerpBits = 15;  // interpolation fraction
std::array<s32, kOutputFrames * 2> g_mixAccumulator;

static s32 GainToFixed(float gain) {
    // Gains are non-negative products of volumes, pan scales and the master
    // volume; bound them so the 64-bit product below cannot overflow.
    const float scaled = std::clamp(gain, 0.0f, 256.0f) * static_cast<float>(1u << kGainBits);
    return static_cast<s32>(scaled + 0.5f);
}

// Q15 value of a sample at `value` plus a fraction of the way to `next`.
static s32 LerpQ15(s32 value, s32 next, u32 fraction) {
    return value * (1 << kLerpBits) + (next - value) * static_cast<s32>(fraction);
}

static s32 ScaleToMix(s32 valueQ15, s32 gain) {
    constexpr u32 shift = kLerpBits + kGainBits - kMixFractionBits;
    return static_cast<s32>((static_cast<s64>(valueQ15) * gain) >> shift);
}

static u32 CursorLerpFraction(u64 cursor) {
    return (static_cast<u32>(cursor) & kCursorFractionMask) >> (kCursorFractionBits - kLerpBits);
}

// `count` music frames inside one decoded pair at a steady gain, the cursor
// split into the pair-local frame and its 30-bit fraction (the stream always
// advances by half a source frame). Returns the last frame's mix values.
template <bool Mono>
static void MixMusicRun(s32* mix, u32 count, const MusicChunk& chunk, u32 framesPerPair,
                        u32 localFrame, u32 fraction, s32 gain, s32& lastLeft, s32& lastRight) {
    const s16* left = chunk.left.data();
    const s16* right = chunk.right.data();
    const u32 lastLocal = framesPerPair - 1;
    s32 outLeft = 0;
    s32 outRight = 0;
    for (u32 frame = 0; frame < count; ++frame) {
        const u32 nextFrame = std::min<u32>(localFrame + 1, lastLocal);
        const u32 lerp = fraction >> (kCursorFractionBits - kLerpBits);
        outLeft = ScaleToMix(LerpQ15(left[localFrame], left[nextFrame], lerp), gain);
        outRight = ScaleToMix(LerpQ15(right[localFrame], right[nextFrame], lerp), gain);
        if (Mono) outLeft = outRight = (outLeft + outRight) / 2;
        mix[frame * 2] += outLeft;
        mix[frame * 2 + 1] += outRight;
        fraction += static_cast<u32>(kCursorOne / 2u);
        localFrame += fraction >> kCursorFractionBits;
        fraction &= kCursorFractionMask;
    }
    lastLeft = outLeft;
    lastRight = outRight;
}

static void MixMusicStream(s32* mix, u32 requestedFrames) {
    g_musicMixers.fetch_add(1, std::memory_order_acquire);
    if (!g_musicStream.active.load(std::memory_order_acquire)
        || g_musicStream.totalFrames == 0) {
        g_musicMixers.fetch_sub(1, std::memory_order_release);
        return;
    }

    bool underrun = false;
    u32 underrunFrame = requestedFrames;
    u32 mixedFrames = 0;
    s32 holdLeft = 0;
    s32 holdRight = 0;
    bool held = false;
    const u64 streamEnd = static_cast<u64>(g_musicStream.totalFrames) * kCursorOne;
    // Linear fade: constant increment per callback (see the voice mixer).
    const float musicFadeStep = g_musicStream.fadeFrames > 0
        ? (g_musicStream.targetVolume - g_musicStream.volume) / static_cast<float>(g_musicStream.fadeFrames)
        : 0.0f;
    const u32 framesPerPair = g_musicStream.framesPerPair;
    s32 gain = GainToFixed(g_musicStream.volume * g_masterVolume);
    // First source frame of the pair being read: the per-frame pair/offset
    // division only runs when the cursor leaves it.
    u32 knownPair = kInvalidPair;
    u32 knownFirst = 0;
    for (u32 frame = 0; frame < requestedFrames; ++frame) {
        if (g_musicStream.cursor >= streamEnd) {
            if (!g_musicStream.loop) {
                g_musicStream.active.store(false, std::memory_order_release);
                break;
            }
            g_musicStream.cursor = 0;
            // A one-pair track is already resident in the current buffer.
            // Invalidating it here would make the callback wait for a pair
            // that the worker will never request again.
            if (g_musicStream.pairCount > 1u) {
                g_musicStream.currentPair = kInvalidPair;
            }
        }

        const u32 sourceFrame = static_cast<u32>(g_musicStream.cursor >> kCursorFractionBits);
        if (knownPair == kInvalidPair || knownPair != g_musicStream.currentPair
            || sourceFrame - knownFirst >= framesPerPair) {
            const u32 pair = sourceFrame / framesPerPair;
            if (pair >= g_musicStream.pairCount) break;
            if (pair != g_musicStream.currentPair) {
                if (!PromoteMusicPair(pair)) {
                    // A slow Memory Stick or worker can miss a period. Keep
                    // the stream alive and hold the last sample for this
                    // period instead of producing a long zero-filled hole.
                    underrun = true;
                    underrunFrame = frame;
                    break;
                }
                const u32 nextPair = pair + 1u < g_musicStream.pairCount ? pair + 1u : 0u;
                if (g_musicStream.pairCount > 1u) RequestMusicPair(nextPair);
            }
            knownPair = pair;
            knownFirst = pair * framesPerPair;
        }

        const u32 currentBuffer = g_musicStream.currentBuffer.load(std::memory_order_acquire);
        if (currentBuffer >= g_musicStream.chunks.size()) {
            underrun = true;
            underrunFrame = frame;
            break;
        }
        const MusicChunk& chunkData = g_musicStream.chunks[currentBuffer];
        const u32 localFrame = sourceFrame - knownFirst;
        if (g_musicStream.fadeFrames == 0) {
            // Steady gain: mix every frame left in this pair (and before the
            // stream end) in one run; the checks above only change there.
            const u64 pairEnd = static_cast<u64>(knownFirst + framesPerPair) * kCursorOne;
            const u64 limit = std::min(pairEnd, streamEnd);
            constexpr u64 kHalf = kCursorOne / 2u;
            const u64 untilLimit = (limit - g_musicStream.cursor + kHalf - 1u) / kHalf;
            const u32 count = static_cast<u32>(std::min<u64>(untilLimit, requestedFrames - frame));
            const u32 fraction = static_cast<u32>(g_musicStream.cursor) & kCursorFractionMask;
            if (g_mono) {
                MixMusicRun<true>(mix + frame * 2, count, chunkData, framesPerPair, localFrame,
                                  fraction, gain, holdLeft, holdRight);
            }
            else {
                MixMusicRun<false>(mix + frame * 2, count, chunkData, framesPerPair, localFrame,
                                   fraction, gain, holdLeft, holdRight);
            }
            held = true;
            g_musicStream.cursor += kHalf * count;
            mixedFrames += count;
            frame += count - 1u;  // the loop's ++frame completes the run
            continue;
        }
        const u32 nextFrame = std::min<u32>(localFrame + 1, framesPerPair - 1);
        const u32 fraction = CursorLerpFraction(g_musicStream.cursor);
        const s32 left = LerpQ15(chunkData.left[localFrame], chunkData.left[nextFrame], fraction);
        const s32 right = LerpQ15(chunkData.right[localFrame], chunkData.right[nextFrame], fraction);
        s32 outLeft = ScaleToMix(left, gain);
        s32 outRight = ScaleToMix(right, gain);
        if (g_mono) {
            outLeft = outRight = (outLeft + outRight) / 2;
        }
        const u32 outputIndex = frame * 2;
        mix[outputIndex] += outLeft;
        mix[outputIndex + 1] += outRight;
        holdLeft = outLeft;
        holdRight = outRight;
        held = true;

        g_musicStream.cursor += kCursorOne / 2u;
        ++mixedFrames;
        if (g_musicStream.fadeFrames > 0) {
            --g_musicStream.fadeFrames;
            g_musicStream.volume = g_musicStream.fadeFrames == 0
                ? g_musicStream.targetVolume
                : g_musicStream.volume + musicFadeStep;
            gain = GainToFixed(g_musicStream.volume * g_masterVolume);
        }
    }
    if (held) {
        constexpr float kMixScale = 1.0f / static_cast<float>(1u << kMixFractionBits);
        g_musicStream.holdLeft = static_cast<float>(holdLeft) * kMixScale;
        g_musicStream.holdRight = static_cast<float>(holdRight) * kMixScale;
    }

    if (underrun) {
        PspPerf::RecordAudioUnderrun();
        const s32 fillLeft = static_cast<s32>(static_cast<s16>(std::clamp(
            g_musicStream.holdLeft, -32768.0f, 32767.0f))) * (1 << kMixFractionBits);
        const s32 fillRight = static_cast<s32>(static_cast<s16>(std::clamp(
            g_musicStream.holdRight, -32768.0f, 32767.0f))) * (1 << kMixFractionBits);
        // Only the music adds after the voices: fill the remainder where the
        // voices left silence with a stable tail sample, so an underrun does
        // not click.
        for (u32 frame = underrunFrame; frame < requestedFrames; ++frame) {
            const u32 outputIndex = frame * 2;
            if (mix[outputIndex] == 0) mix[outputIndex] = fillLeft;
            if (mix[outputIndex + 1] == 0) mix[outputIndex + 1] = fillRight;
        }
    }

    PspPerf::RecordAudioMusicMix(mixedFrames);

    g_musicMixers.fetch_sub(1, std::memory_order_release);
}

static void ResetMusicStream() {
    // Deactivate first, then wait for an already-running callback before
    // releasing the file it may still be reading.
    g_musicStream.active.store(false, std::memory_order_release);
    while (g_musicMixers.load(std::memory_order_acquire) != 0) {
        sceKernelDelayThread(0);
    }
    StopMusicWorker();
    g_musicStream.file.reset();
    g_musicStream.dataOffset = 0;
    g_musicStream.dataEnd = 0;
    g_musicStream.pairCount = 0;
    g_musicStream.totalFrames = 0;
    g_musicStream.currentPair = kInvalidPair;
    g_musicStream.currentBuffer.store(kInvalidBuffer, std::memory_order_release);
    g_musicStream.readyBuffer.store(kInvalidBuffer, std::memory_order_release);
    g_musicStream.readyPair.store(kInvalidPair, std::memory_order_release);
    g_musicStream.requestedPair.store(kInvalidPair, std::memory_order_release);
    g_musicStream.cursor = 0;
    g_musicStream.holdLeft = 0.0f;
    g_musicStream.holdRight = 0.0f;
}

// Q15 source value: mono as is, stereo as the exact average, wider layouts as
// their mean (Channels 0: any count above two, read from `channels`).
template <u32 Channels>
static s32 ReadVoiceQ15(const s16* pcm, u32 channels, u32 source, u32 next, u32 fraction) {
    if (Channels == 1) return LerpQ15(pcm[source], pcm[next], fraction);
    if (Channels == 2) {
        const s32 current = pcm[source * 2] + pcm[source * 2 + 1];
        const s32 following = pcm[next * 2] + pcm[next * 2 + 1];
        return current * (1 << (kLerpBits - 1))
            + static_cast<s32>((static_cast<s64>(following - current) * fraction) >> 1);
    }
    if (channels == 1) return ReadVoiceQ15<1>(pcm, channels, source, next, fraction);
    if (channels == 2) return ReadVoiceQ15<2>(pcm, channels, source, next, fraction);
    s32 current = 0;
    s32 following = 0;
    for (u32 channel = 0; channel < channels; ++channel) {
        current += pcm[source * channels + channel];
        following += pcm[next * channels + channel];
    }
    const s64 value = static_cast<s64>(current) * (1 << kLerpBits)
        + static_cast<s64>(following - current) * fraction;
    return static_cast<s32>(value / static_cast<s32>(channels));
}

// A voice's cursor below its sample end, split into the source frame and the
// 30-bit fraction: adding the split step with a carry is exactly the u64 add,
// and "cursor >= end" is "frame >= frames", without 64-bit work per sample.
struct VoiceRun {
    const s16* pcm;
    u32 channels;
    u32 frames;
    u32 lastFrame;
    u32 stepFrame;
    u32 stepFraction;
    bool loop;
    u32 frame;
    u32 fraction;
};

// Advances one output frame: wraps or ends at the sample end first, like the
// u64 loop. Returns false when a one-shot voice ended (nothing mixed).
static bool BeginVoiceFrame(VoiceRun& run) {
    if (run.frame >= run.frames) {
        if (!run.loop) return false;
        run.frame -= run.frames;
    }
    return true;
}

static void AdvanceVoiceFrame(VoiceRun& run) {
    run.fraction += run.stepFraction;
    run.frame += run.stepFrame + (run.fraction >> kCursorFractionBits);
    run.fraction &= kCursorFractionMask;
}

// Constant gains over `count` frames. Returns the frames mixed (fewer when a
// one-shot voice ended).
template <u32 Channels>
static u32 MixVoiceSteady(s32* mix, u32 count, VoiceRun& run, s32 leftGain, s32 rightGain) {
    VoiceRun local = run;
    u32 frame = 0;
    for (; frame < count; ++frame) {
        if (!BeginVoiceFrame(local)) break;
        const u32 nextFrame = std::min<u32>(local.frame + 1, local.lastFrame);
        const s32 value = ReadVoiceQ15<Channels>(local.pcm, local.channels, local.frame, nextFrame,
            local.fraction >> (kCursorFractionBits - kLerpBits));
        mix[frame * 2] += ScaleToMix(value, leftGain);
        mix[frame * 2 + 1] += ScaleToMix(value, rightGain);
        AdvanceVoiceFrame(local);
    }
    run = local;
    return frame;
}

static u32 MixVoiceSteady(s32* mix, u32 count, VoiceRun& run, s32 leftGain, s32 rightGain) {
    if (leftGain == 0 && rightGain == 0) {
        // Silent (both gains round to zero): every product is zero, so only
        // the cursor moves.
        u32 frame = 0;
        for (; frame < count; ++frame) {
            if (!BeginVoiceFrame(run)) break;
            AdvanceVoiceFrame(run);
        }
        return frame;
    }
    if (run.channels == 1) return MixVoiceSteady<1>(mix, count, run, leftGain, rightGain);
    if (run.channels == 2) return MixVoiceSteady<2>(mix, count, run, leftGain, rightGain);
    return MixVoiceSteady<0>(mix, count, run, leftGain, rightGain);  // rare: 3+ channels
}

static void MixVoices(s32* mix, u32 requestedFrames) {
    for (Voice& voice : g_voices) {
        if (!voice.active) continue;
        Sample* sample = GetSample(voice.sample);
        if (!sample || sample->frames == 0) {
            voice.active = false;
            continue;
        }

        // Pan scales are fixed for the callback; only the volume fades.
        const float clampedPan = std::clamp(voice.pan, -1.0f, 1.0f);
        const float leftScale = g_masterVolume * (1.0f - clampedPan) * 0.5f;
        const float rightScale = g_masterVolume * (1.0f + clampedPan) * 0.5f;
        float left = voice.volume * leftScale;
        float right = voice.volume * rightScale;
        if (g_mono) right = left = (left + right) * 0.5f;
        s32 leftGain = GainToFixed(left);
        s32 rightGain = GainToFixed(right);
        const u64 sampleEnd = static_cast<u64>(sample->frames) * kCursorOne;
        u64 step = CachedCursorStep(voice, sample->rate);
        // Reduce large looping strides once. The inner loop only needs one
        // subtraction to wrap, including one-frame clips and extreme pitch.
        if (voice.loop) {
            if (step >= sampleEnd) step %= sampleEnd;
            if (voice.cursor >= sampleEnd) voice.cursor %= sampleEnd;
        }
        if (requestedFrames == 0) continue;
        // A one-shot voice at or past its end stops before mixing anything.
        if (voice.cursor >= sampleEnd) {
            voice.active = false;
            continue;
        }

        // The per-frame fade v += (t - v) / remaining is a linear ramp, so its
        // increment is constant: compute it once per callback (re-derived each
        // callback, and the last fade frame lands exactly on the target).
        const float fadeStep = voice.fadeFrames > 0
            ? (voice.targetVolume - voice.volume) / static_cast<float>(voice.fadeFrames)
            : 0.0f;

        const auto fadeAfterFrame = [&]() {
            --voice.fadeFrames;
            voice.volume = voice.fadeFrames == 0 ? voice.targetVolume : voice.volume + fadeStep;
            // Matches the previous mixer: fade updates use the panned
            // (non-mono-averaged) gains.
            leftGain = GainToFixed(voice.volume * leftScale);
            rightGain = GainToFixed(voice.volume * rightScale);
        };

        const u32 cursorFrame = static_cast<u32>(voice.cursor >> kCursorFractionBits);
        const u32 cursorFraction = static_cast<u32>(voice.cursor) & kCursorFractionMask;
        if (step >= sampleEnd) {
            // A one-shot stride of at least the whole clip (extreme pitch):
            // it mixes its current frame and the next frame is past the end.
            const s32 value = ReadVoiceQ15<0>(sample->pcm.data(), sample->channels, cursorFrame,
                std::min<u32>(cursorFrame + 1, sample->frames - 1),
                cursorFraction >> (kCursorFractionBits - kLerpBits));
            mix[0] += ScaleToMix(value, leftGain);
            mix[1] += ScaleToMix(value, rightGain);
            voice.cursor += step;
            if (voice.fadeFrames > 0) fadeAfterFrame();
            if (requestedFrames > 1) voice.active = false;
            continue;
        }

        // cursor < end and step < end: the split frame stays below
        // 2 * frames, which fits in 32 bits for any loadable clip.
        VoiceRun run = {
            sample->pcm.data(), sample->channels, sample->frames, sample->frames - 1,
            static_cast<u32>(step >> kCursorFractionBits),
            static_cast<u32>(step) & kCursorFractionMask, voice.loop,
            cursorFrame, cursorFraction,
        };
        u32 frame = 0;
        bool ended = false;
        // Fade frames: the gains follow the volume after every frame.
        while (frame < requestedFrames && voice.fadeFrames > 0) {
            if (!BeginVoiceFrame(run)) {
                ended = true;
                break;
            }
            const u32 nextFrame = std::min<u32>(run.frame + 1, run.lastFrame);
            const s32 value = ReadVoiceQ15<0>(run.pcm, run.channels, run.frame, nextFrame,
                run.fraction >> (kCursorFractionBits - kLerpBits));
            mix[frame * 2] += ScaleToMix(value, leftGain);
            mix[frame * 2 + 1] += ScaleToMix(value, rightGain);
            AdvanceVoiceFrame(run);
            ++frame;
            fadeAfterFrame();
        }
        if (!ended && frame < requestedFrames) {
            const u32 count = requestedFrames - frame;
            ended = MixVoiceSteady(mix + frame * 2, count, run, leftGain, rightGain) < count;
        }
        voice.cursor = (static_cast<u64>(run.frame) << kCursorFractionBits) | run.fraction;
        if (ended) voice.active = false;
    }
}

static void MixPeriod(s16* output, u32 frames) {
    s32* mix = g_mixAccumulator.data();
    std::memset(mix, 0, frames * 2 * sizeof(s32));
    MixVoices(mix, frames);
    MixMusicStream(mix, frames);
    for (u32 index = 0; index < frames * 2; ++index) {
        // Toward zero, as the float path's s16 conversion.
        const s32 value = mix[index];
        const s32 level = (value + ((value >> 31) & ((1 << kMixFractionBits) - 1))) >> kMixFractionBits;
        output[index] = static_cast<s16>(std::clamp<s32>(level, -32768, 32767));
    }
}

static void AudioCallback(void* buffer, unsigned int requestedFrames, void*) {
#if defined(PSP_PERF_TELEMETRY)
    const u64 callbackStart = sceKernelGetSystemTimeWide();
#endif
    auto* output = static_cast<s16*>(buffer);
    // pspaudiolib asks for kOutputFrames; split anything longer.
    for (u32 done = 0; done < requestedFrames;) {
        const u32 frames = std::min<u32>(requestedFrames - done, kOutputFrames);
        MixPeriod(output + done * 2, frames);
        done += frames;
    }
#if defined(PSP_PERF_TELEMETRY)
    const u64 callbackUs = sceKernelGetSystemTimeWide() - callbackStart;
    PspPerf::RecordAudioCallback(static_cast<u32>(std::min<u64>(callbackUs, 0xFFFFFFFFull)));
#endif
}

static AudioVoice AllocateVoice(u8 priority) {
    for (u32 i = 0; i < g_voices.size(); ++i) {
        if (!g_voices[i].active && !g_voices[i].reserved) {
            // Reset the slot: a finished one-shot leaves its cursor at the end of
            // the old sample, so the next (shorter) sound would start past its end
            // and be silent on the first frame.
            g_voices[i] = {};
            return i + 1;
        }
    }
    u32 candidate = 0;
    for (u32 i = 1; i < g_voices.size(); ++i) {
        if (!g_voices[i].reserved && g_voices[i].priority <= g_voices[candidate].priority) candidate = i;
    }
    if (g_voices[candidate].reserved || g_voices[candidate].priority > priority) return AUDIO_VOICE_INVALID;
    g_voices[candidate] = {};
    return candidate + 1;
}

static AudioVoice StartVoice(AudioSample sample, float volume, float pan, bool loop, u8 priority) {
    if (!GetSample(sample)) return AUDIO_VOICE_INVALID;
    const AudioVoice handle = AllocateVoice(priority);
    if (!handle) return handle;
    Voice& voice = g_voices[handle - 1];
    voice.sample = sample;
    voice.volume = std::max(volume, 0.0f);
    voice.targetVolume = voice.volume;
    voice.pan = std::clamp(pan, -1.0f, 1.0f);
    voice.priority = priority;
    voice.loop = loop;
    voice.active = true;
    return handle;
}

} // namespace

bool AudioEngine::Init() {
#if defined(PSP_DISABLE_AUDIO_DIAGNOSTIC)
    return false; // Isolation build: separate audio scheduling from GU stalls.
#endif
    if (g_initialized) return true;
    const int audioInitResult = pspAudioInit();
    if (audioInitResult < 0) {
        LOG("[PSP Audio] pspAudioInit failed: %d", audioInitResult);
        return false;
    }
    // pspaudiolib reserves the channel first. Setting the frequency before
    // that reservation is rejected by some firmware/emulator versions and
    // used to make the entire audio engine silently fail to start.
    const int frequencyResult = sceAudioSetFrequency(static_cast<int>(kOutputRate));
    if (frequencyResult < 0) {
        LOG("[PSP Audio] sceAudioSetFrequency(44100) failed: %d; using library default",
            frequencyResult);
    }
    pspAudioSetChannelCallback(0, AudioCallback, nullptr);
    pspAudioSetVolume(0, PSP_VOLUME_MAX, PSP_VOLUME_MAX);
    PspPerf::SetAudioMemory(static_cast<u32>(sizeof(MusicStream)));
    LOG("[PSP Audio] initialized: output=%u Hz callback=%u frames stream_ram=%u",
        kOutputRate, kOutputFrames, static_cast<unsigned int>(sizeof(MusicStream)));
    g_initialized = true;
    return true;
}

void AudioEngine::Shutdown() {
    if (!g_initialized) return;
    StopMusic();
    StopAllVoices();
    pspAudioEnd();
    UnloadAllSamples();
    g_initialized = false;
}

bool AudioEngine::IsInitialized() { return g_initialized; }

AudioSample AudioEngine::LoadSample(const s16* data, u32 frames, u32 rate, u32 channels) {
    if (!data || !frames || !channels) return AUDIO_SAMPLE_INVALID;
    for (u32 i = 0; i < g_samples.size(); ++i) {
        if (g_samples[i].loaded) continue;
        Sample& sample = g_samples[i];
        sample.pcm.assign(data, data + static_cast<size_t>(frames) * channels);
        sample.frames = frames;
        sample.rate = rate ? rate : kOutputRate;
        sample.channels = std::min<u32>(channels, 2u);
        sample.loaded = true;
        return i + 1;
    }
    return AUDIO_SAMPLE_INVALID;
}

AudioSample AudioEngine::LoadSampleOwned(std::vector<s16>&& data, u32 frames, u32 rate, u32 channels) {
    if (data.empty() || !frames || !channels) return AUDIO_SAMPLE_INVALID;
    for (u32 i = 0; i < g_samples.size(); ++i) {
        if (g_samples[i].loaded) continue;
        Sample& sample = g_samples[i];
        sample.pcm = std::move(data);
        sample.frames = frames;
        sample.rate = rate ? rate : kOutputRate;
        sample.channels = std::min<u32>(channels, 2u);
        sample.loaded = true;
        return i + 1;
    }
    return AUDIO_SAMPLE_INVALID;
}

void AudioEngine::UnloadSample(AudioSample handle) {
    Sample* sample = GetSample(handle);
    if (!sample) return;
    for (Voice& voice : g_voices) if (voice.sample == handle) voice.active = false;
    *sample = {};
}

void AudioEngine::UnloadAllSamples() {
    StopAllVoices();
    for (Sample& sample : g_samples) sample = {};
}

AudioVoice AudioEngine::PlaySample(AudioSample sample, f32 volume, f32 pan, bool loop, u8 priority) {
    return StartVoice(sample, volume, pan, loop, priority);
}

AudioVoice AudioEngine::PlaySample3D(AudioSample sample, const LVector& position, f32 volume,
                                     bool loop, bool applyDistance, f32 minDistance,
                                     f32 maxDistance, u8 priority) {
    float dx = static_cast<float>(position.x - g_listener.x);
    float dz = static_cast<float>(position.z - g_listener.z);
    const float distance = std::sqrt(dx * dx + dz * dz);
    float attenuation = 1.0f;
    if (applyDistance && distance > minDistance) {
        attenuation = std::clamp(1.0f - (distance - minDistance) / std::max(maxDistance - minDistance, 1.0f), 0.0f, 1.0f);
    }
    const float angle = std::atan2(dx, dz) - static_cast<float>(g_listenerYaw) * (6.28318530718f / 4096.0f);
    const AudioVoice voice = StartVoice(sample, volume * attenuation, std::sin(angle), loop, priority);
    if (Voice* state = GetVoice(voice)) {
        state->position = position;
        state->minDistance = minDistance;
        state->maxDistance = maxDistance;
    }
    return voice;
}

AudioVoice AudioEngine::ReserveVoice(u8 priority) {
    const AudioVoice handle = AllocateVoice(priority);
    if (handle) {
        g_voices[handle - 1] = {};
        g_voices[handle - 1].reserved = true;
        g_voices[handle - 1].priority = priority;
    }
    return handle;
}

void AudioEngine::ReleaseVoice(AudioVoice voice) { if (Voice* state = GetVoice(voice)) *state = {}; }

AudioVoice AudioEngine::PlayOnReservedVoice(AudioVoice voice, AudioSample sample, f32 volume, f32 pan) {
    Voice* state = GetVoice(voice);
    if (!state || !state->reserved || !GetSample(sample)) return AUDIO_VOICE_INVALID;
    state->sample = sample; state->cursor = 0; state->volume = volume; state->targetVolume = volume;
    state->pan = pan; state->loop = false; state->active = true;
    return voice;
}

void AudioEngine::StopVoice(AudioVoice voice) { if (Voice* state = GetVoice(voice)) state->active = false; }
void AudioEngine::StopAllVoices() { for (Voice& voice : g_voices) if (!voice.reserved) voice.active = false; }
bool AudioEngine::IsVoicePlaying(AudioVoice voice) { const Voice* state = GetVoice(voice); return state && state->active; }
void AudioEngine::SetVoiceVolume(AudioVoice voice, f32 volume) { if (Voice* s = GetVoice(voice)) s->volume = s->targetVolume = std::max(volume, 0.0f); }
void AudioEngine::FadeVoiceVolume(AudioVoice voice, f32 target, u32 ms) { if (Voice* s = GetVoice(voice)) { s->targetVolume = target; s->fadeFrames = ms * kOutputRate / 1000; } }
void AudioEngine::SetVoicePan(AudioVoice voice, f32 pan) { if (Voice* s = GetVoice(voice)) s->pan = std::clamp(pan, -1.0f, 1.0f); }
void AudioEngine::SetVoicePitch(AudioVoice voice, f32 pitch) { if (Voice* s = GetVoice(voice)) s->pitch = pitch; }
void AudioEngine::SetVoicePosition(AudioVoice voice, const LVector& pos) { if (Voice* s = GetVoice(voice)) s->position = pos; }
void AudioEngine::SetVoiceDistanceRange(AudioVoice voice, f32 min, f32 max) { if (Voice* s = GetVoice(voice)) { s->minDistance = min; s->maxDistance = max; } }
void AudioEngine::SetListener(const LVector& position, s32 yaw16) { g_listener = position; g_listenerYaw = yaw16; }

bool AudioEngine::PlayMusic(const char* path, f32 volume, bool loop) {
    return PlayMusicSong(path, 0, volume, loop);
}

bool AudioEngine::PlayMusicSong(const char* path, u32 songIndex, f32 volume, bool loop) {
    if (!g_initialized || !path) return false;

    StopMusic();
    auto file = std::make_unique<p3d::io::FileHandle>();
    if (!file->Open(path, p3d::io::FileHandle::Mode::Read)) return false;
    const int64_t fileSize = file->Size();
    if (fileSize < 12 || fileSize > 0xFFFFFFFFll) return false;

    u8 songCountBytes[4] = {};
    if (file->Read(songCountBytes, sizeof(songCountBytes)) != sizeof(songCountBytes)) return false;
    const u32 songCount = ReadU32(songCountBytes);
    const u64 headerSize = 4ull + static_cast<u64>(songCount) * 8ull;
    if (songCount == 0 || songCount > 64 || headerSize > static_cast<u64>(fileSize)) return false;

    std::array<u8, 4 + 64 * 8> header = {};
    if (!file->Seek(0, SEEK_SET)
        || file->Read(header.data(), static_cast<size_t>(headerSize)) != headerSize) {
        return false;
    }
    if (songIndex >= songCount) songIndex = 0;
    const u32 dataOffset = ReadU32(header.data() + 4 + songIndex * 4);
    const u32 dataEndRaw = ReadU32(header.data() + 4 + songCount * 4 + songIndex * 4);
    if (dataOffset >= static_cast<u32>(fileSize) || dataEndRaw <= dataOffset) return false;
    const u32 dataEnd = std::min<u32>(dataEndRaw, static_cast<u32>(fileSize));
    const u32 dataSize = dataEnd - dataOffset;
    const u32 chunkCount = (dataSize / 16) / kMusicChunkBlocks;
    const u32 pairCount = chunkCount / 2;
    if (pairCount == 0) return false;

    g_musicStream.file = std::move(file);
    g_musicStream.dataOffset = dataOffset;
    g_musicStream.dataEnd = dataEnd;
    g_musicStream.pairCount = pairCount;
    g_musicStream.totalFrames = pairCount * kMusicChunkSamples;
    g_musicStream.currentPair = kInvalidPair;
    g_musicStream.currentBuffer.store(kInvalidBuffer, std::memory_order_release);
    g_musicStream.readyBuffer.store(kInvalidBuffer, std::memory_order_release);
    g_musicStream.readyPair.store(kInvalidPair, std::memory_order_release);
    g_musicStream.cursor = 0;
    g_musicStream.volume = std::max(volume, 0.0f);
    g_musicStream.targetVolume = g_musicStream.volume;
    g_musicStream.fadeFrames = 0;
    g_musicStream.loop = loop;
    if (!StartMusicWorker()) {
        ResetMusicStream();
        return false;
    }
    RequestMusicPair(0);
    if (!WaitForMusicPair(0) || !PromoteMusicPair(0)) {
        ResetMusicStream();
        return false;
    }
    // Publish the fully initialized stream only after its first decoded pair
    // is ready, so the callback never sees partial metadata or buffers.
    g_musicStream.active.store(true, std::memory_order_release);
    PspPerf::RecordAudioMusicStart();
    if (pairCount > 1u) RequestMusicPair(1u);
    LOG("[PSP Audio] music stream ready: path=%s song=%u pairs=%u source_hz=22050 output_hz=%u",
        path, songIndex, pairCount, kOutputRate);
    return true;
}
bool AudioEngine::PlayMusicSample(AudioSample sample, f32 volume, bool loop) {
    StopMusic();
    g_musicVoice = StartVoice(sample, volume, 0.0f, loop, AUDIO_PRIO_AMBIENCE);
    return g_musicVoice != AUDIO_VOICE_INVALID;
}
void AudioEngine::StopMusic() {
    if (g_musicVoice) StopVoice(g_musicVoice);
    g_musicVoice = AUDIO_VOICE_INVALID;
    ResetMusicStream();
}
void AudioEngine::SetMusicVolume(f32 volume) {
    if (g_musicStream.active.load(std::memory_order_acquire)) {
        g_musicStream.volume = g_musicStream.targetVolume = std::max(volume, 0.0f);
    }
    else {
        SetVoiceVolume(g_musicVoice, volume);
    }
}
void AudioEngine::FadeMusicVolume(f32 volume, u32 fadeMs) {
    if (g_musicStream.active.load(std::memory_order_acquire)) {
        g_musicStream.targetVolume = std::max(volume, 0.0f);
        g_musicStream.fadeFrames = fadeMs * kOutputRate / 1000;
    }
    else {
        FadeVoiceVolume(g_musicVoice, volume, fadeMs);
    }
}
bool AudioEngine::IsMusicPlaying() {
    return g_musicStream.active.load(std::memory_order_acquire) || IsVoicePlaying(g_musicVoice);
}
void AudioEngine::SetMasterVolume(f32 volume) { g_masterVolume = std::clamp(volume, 0.0f, 1.0f); }
f32 AudioEngine::GetMasterVolume() { return g_masterVolume; }
void AudioEngine::SetOutputMono(bool mono) { g_mono = mono; }
bool AudioEngine::GetOutputMono() { return g_mono; }
u32 AudioEngine::GetOutputChannels() { return 2; }
void AudioEngine::SetSurroundEnabled(bool enabled) { g_surround = enabled; }
bool AudioEngine::GetSurroundEnabled() { return g_surround; }

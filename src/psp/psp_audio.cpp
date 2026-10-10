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

static float CursorFraction(u64 cursor) {
    return static_cast<float>(static_cast<u32>(cursor) & kCursorFractionMask)
        * (1.0f / static_cast<float>(kCursorOne));
}

static void DecodeMusicChunk(const u8* chunkData, std::array<s16, kMusicChunkSamples>& output) {
    s32 prev0 = 0;
    s32 prev1 = 0;
    for (u32 block = 0; block < kMusicChunkBlocks; ++block) {
        s16 blockOutput[28];
        SpuAdpcm::DecodeBlock(chunkData + block * 16, blockOutput, prev0, prev1);
        std::memcpy(output.data() + block * 28, blockOutput, sizeof(blockOutput));
    }
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

#if defined(PSP_PERF_TELEMETRY)
    const u64 decodeStart = sceKernelGetSystemTimeWide();
#endif
    DecodeMusicChunk(g_musicStream.compressedPair.data(), destination.left);
    DecodeMusicChunk(g_musicStream.compressedPair.data() + kMusicChunkBytes, destination.right);
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::RecordMusicDecode(static_cast<u32>(sceKernelGetSystemTimeWide() - decodeStart));
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

static void MixMusicStream(s16* output, u32 requestedFrames) {
    g_musicMixers.fetch_add(1, std::memory_order_acquire);
    if (!g_musicStream.active.load(std::memory_order_acquire)
        || g_musicStream.totalFrames == 0) {
        g_musicMixers.fetch_sub(1, std::memory_order_release);
        return;
    }

    bool underrun = false;
    u32 underrunFrame = requestedFrames;
    u32 mixedFrames = 0;
    const u64 streamEnd = static_cast<u64>(g_musicStream.totalFrames) * kCursorOne;
    // Linear fade: constant increment per callback (see the voice mixer).
    const float musicFadeStep = g_musicStream.fadeFrames > 0
        ? (g_musicStream.targetVolume - g_musicStream.volume) / static_cast<float>(g_musicStream.fadeFrames)
        : 0.0f;
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
        const u32 pair = sourceFrame / g_musicStream.framesPerPair;
        if (pair >= g_musicStream.pairCount) break;
        if (pair != g_musicStream.currentPair) {
            if (!PromoteMusicPair(pair)) {
                // A slow Memory Stick or worker can miss a period. Keep the
                // stream alive and hold the last sample for this period
                // instead of producing a long zero-filled hole.
                underrun = true;
                underrunFrame = frame;
                break;
            }
            const u32 nextPair = pair + 1u < g_musicStream.pairCount ? pair + 1u : 0u;
            if (g_musicStream.pairCount > 1u) RequestMusicPair(nextPair);
        }

        const u32 currentBuffer = g_musicStream.currentBuffer.load(std::memory_order_acquire);
        if (currentBuffer >= g_musicStream.chunks.size()) {
            underrun = true;
            underrunFrame = frame;
            break;
        }
        const MusicChunk& chunkData = g_musicStream.chunks[currentBuffer];
        const u32 localFrame = sourceFrame % g_musicStream.framesPerPair;
        const u32 nextFrame = std::min<u32>(localFrame + 1, g_musicStream.framesPerPair - 1);
        const float fraction = CursorFraction(g_musicStream.cursor);
        const float left = static_cast<float>(chunkData.left[localFrame])
            + (static_cast<float>(chunkData.left[nextFrame])
                - static_cast<float>(chunkData.left[localFrame])) * fraction;
        const float right = static_cast<float>(chunkData.right[localFrame])
            + (static_cast<float>(chunkData.right[nextFrame])
                - static_cast<float>(chunkData.right[localFrame])) * fraction;
        const float gain = g_musicStream.volume * g_masterVolume;
        const u32 outputIndex = frame * 2;
        float outLeft = left * gain;
        float outRight = right * gain;
        if (g_mono) {
            outLeft = outRight = (outLeft + outRight) * 0.5f;
        }
        output[outputIndex] = static_cast<s16>(std::clamp(
            static_cast<float>(output[outputIndex]) + outLeft, -32768.0f, 32767.0f));
        output[outputIndex + 1] = static_cast<s16>(std::clamp(
            static_cast<float>(output[outputIndex + 1]) + outRight, -32768.0f, 32767.0f));

        g_musicStream.holdLeft = outLeft;
        g_musicStream.holdRight = outRight;

        g_musicStream.cursor += kCursorOne / 2u;
        ++mixedFrames;
        if (g_musicStream.fadeFrames > 0) {
            --g_musicStream.fadeFrames;
            g_musicStream.volume = g_musicStream.fadeFrames == 0
                ? g_musicStream.targetVolume
                : g_musicStream.volume + musicFadeStep;
        }
    }

    if (underrun) {
        PspPerf::RecordAudioUnderrun();
        const s16 holdLeft = static_cast<s16>(std::clamp(
            g_musicStream.holdLeft, -32768.0f, 32767.0f));
        const s16 holdRight = static_cast<s16>(std::clamp(
            g_musicStream.holdRight, -32768.0f, 32767.0f));
        // The callback output was cleared before voice/music mixing. Fill the
        // remainder with a stable tail sample to avoid a periodic hard click.
        for (u32 frame = underrunFrame; frame < requestedFrames; ++frame) {
            const u32 outputIndex = frame * 2;
            if (output[outputIndex] == 0) output[outputIndex] = holdLeft;
            if (output[outputIndex + 1] == 0) output[outputIndex + 1] = holdRight;
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

static void AudioCallback(void* buffer, unsigned int requestedFrames, void*) {
#if defined(PSP_PERF_TELEMETRY)
    const u64 callbackStart = sceKernelGetSystemTimeWide();
#endif
    auto* output = static_cast<s16*>(buffer);
    std::memset(output, 0, requestedFrames * 2 * sizeof(s16));

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
        const u64 sampleEnd = static_cast<u64>(sample->frames) * kCursorOne;
        u64 step = CachedCursorStep(voice, sample->rate);
        // Reduce large looping strides once. The inner loop only needs one
        // subtraction to wrap, including one-frame clips and extreme pitch.
        if (voice.loop) {
            if (step >= sampleEnd) step %= sampleEnd;
            if (voice.cursor >= sampleEnd) voice.cursor %= sampleEnd;
        }

        // The per-frame fade v += (t - v) / remaining is a linear ramp, so its
        // increment is constant: compute it once per callback (re-derived each
        // callback, and the last fade frame lands exactly on the target).
        const float fadeStep = voice.fadeFrames > 0
            ? (voice.targetVolume - voice.volume) / static_cast<float>(voice.fadeFrames)
            : 0.0f;

        const s16* pcm = sample->pcm.data();
        const u32 channels = sample->channels;
        const u32 lastFrame = sample->frames - 1;
        const float invChannels = channels > 2 ? 1.0f / static_cast<float>(channels) : 1.0f;
        const auto read = [&](u32 source) -> float {
            if (channels == 1) return pcm[source];
            const u32 index = source * channels;
            // (a + b) * 0.5f is bit-identical to (a + b) / 2.0f.
            if (channels == 2) return (static_cast<float>(pcm[index]) + static_cast<float>(pcm[index + 1])) * 0.5f;
            float sum = 0.0f;
            for (u32 channel = 0; channel < channels; ++channel) sum += pcm[index + channel];
            return sum * invChannels;
        };

        for (u32 frame = 0; frame < requestedFrames; ++frame) {
            if (!voice.active) break;
            if (voice.cursor >= sampleEnd) {
                if (voice.loop) voice.cursor -= sampleEnd;
                else {
                    voice.active = false;
                    break;
                }
            }

            const u32 sourceFrame = static_cast<u32>(voice.cursor >> kCursorFractionBits);
            const u32 nextFrame = std::min<u32>(sourceFrame + 1, lastFrame);
            const float fraction = CursorFraction(voice.cursor);
            const float current = read(sourceFrame);
            const float value = current + (read(nextFrame) - current) * fraction;
            const u32 outputIndex = frame * 2;
            output[outputIndex] = static_cast<s16>(std::clamp(output[outputIndex] + value * left, -32768.0f, 32767.0f));
            output[outputIndex + 1] = static_cast<s16>(std::clamp(output[outputIndex + 1] + value * right, -32768.0f, 32767.0f));
            voice.cursor += step;
            if (voice.fadeFrames > 0) {
                --voice.fadeFrames;
                voice.volume = voice.fadeFrames == 0 ? voice.targetVolume : voice.volume + fadeStep;
                // Matches the previous mixer: fade updates use the panned
                // (non-mono-averaged) gains.
                left = voice.volume * leftScale;
                right = voice.volume * rightScale;
            }
        }
    }

    MixMusicStream(output, requestedFrames);
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

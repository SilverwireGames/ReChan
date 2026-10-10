// The real PSP callback, with only kernel/device/file I/O stubbed. Timing is
// measured on Allegrex in PPSSPP, not inferred from this host test.
#include "../../src/psp/psp_audio.cpp"
#include "audio_mixer_reference.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace p3d::io {
FileHandle::~FileHandle() = default;
bool FileHandle::Open(const std::string&, Mode, bool) { return false; }
bool FileHandle::Seek(int64_t, int) { return false; }
size_t FileHandle::Read(void*, size_t) { return 0; }
}
namespace PspPerf {
void SetAudioMemory(u32) {}
void RecordAudioWorkerError() {}
void RecordAudioUnderrun() {}
void RecordAudioMusicMix(u32) {}
void RecordAudioMusicStart() {}
}
static void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "Audio regression: %s\n", why); std::abort(); }
}
static void reset() {
    for (auto& voice : g_voices) voice = {};
    for (auto& sample : g_samples) sample = {};
    g_musicStream.active.store(false);
    g_masterVolume = 1.0f;
    g_mono = false;
}

// --- Bit-exact equivalence with the per-frame mixer (audio_mixer_reference.h)
struct MixerState {
    std::array<Voice, kMaxVoices> voices;
    u64 cursor;
    float volume;
    float targetVolume;
    u32 fadeFrames;
    u32 currentPair;
    u32 currentBuffer;
    u32 readyBuffer;
    u32 readyPair;
    u32 requestedPair;
    bool active;
    float holdLeft;
    float holdRight;
};

static MixerState SaveMixer() {
    return {g_voices, g_musicStream.cursor, g_musicStream.volume, g_musicStream.targetVolume,
            g_musicStream.fadeFrames, g_musicStream.currentPair, g_musicStream.currentBuffer.load(),
            g_musicStream.readyBuffer.load(), g_musicStream.readyPair.load(),
            g_musicStream.requestedPair.load(), g_musicStream.active.load(),
            g_musicStream.holdLeft, g_musicStream.holdRight};
}

static void RestoreMixer(const MixerState& state) {
    g_voices = state.voices;
    g_musicStream.cursor = state.cursor;
    g_musicStream.volume = state.volume;
    g_musicStream.targetVolume = state.targetVolume;
    g_musicStream.fadeFrames = state.fadeFrames;
    g_musicStream.currentPair = state.currentPair;
    g_musicStream.currentBuffer.store(state.currentBuffer);
    g_musicStream.readyBuffer.store(state.readyBuffer);
    g_musicStream.readyPair.store(state.readyPair);
    g_musicStream.requestedPair.store(state.requestedPair);
    g_musicStream.active.store(state.active);
    g_musicStream.holdLeft = state.holdLeft;
    g_musicStream.holdRight = state.holdRight;
}

static bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

static bool SameMixer(const MixerState& a, const MixerState& b) {
    for (u32 i = 0; i < kMaxVoices; ++i) {
        const Voice& x = a.voices[i];
        const Voice& y = b.voices[i];
        if (x.cursor != y.cursor || !SameBits(x.volume, y.volume) || x.fadeFrames != y.fadeFrames
            || x.active != y.active || x.step != y.step || x.stepRate != y.stepRate) {
            std::fprintf(stderr, "voice %u: cursor %llu/%llu active %d/%d fade %u/%u\n", i,
                static_cast<unsigned long long>(x.cursor), static_cast<unsigned long long>(y.cursor),
                x.active, y.active, x.fadeFrames, y.fadeFrames);
            return false;
        }
    }
    return a.cursor == b.cursor && SameBits(a.volume, b.volume) && a.fadeFrames == b.fadeFrames
        && a.currentPair == b.currentPair && a.currentBuffer == b.currentBuffer
        && a.readyBuffer == b.readyBuffer && a.readyPair == b.readyPair
        && a.requestedPair == b.requestedPair && a.active == b.active
        && SameBits(a.holdLeft, b.holdLeft) && SameBits(a.holdRight, b.holdRight);
}

static u32 g_random = 0x12345678u;
static u32 Random(u32 limit) {
    g_random = g_random * 1664525u + 1013904223u;
    return static_cast<u32>((static_cast<u64>(g_random >> 8) * limit) >> 24);
}
template <typename T, std::size_t N>
static T Pick(const T (&values)[N]) { return values[Random(N)]; }

static u32 CheckReferenceEquivalence() {
    std::vector<s32> referenceMix(kOutputFrames * 2);
    std::vector<s32> segmentedMix(kOutputFrames * 2);
    for (MusicChunk& chunk : g_musicStream.chunks) {
        for (s16& value : chunk.left) value = static_cast<s16>(Random(65536) - 32768);
        for (s16& value : chunk.right) value = static_cast<s16>(Random(65536) - 32768);
    }
    const u32 frameCounts[] = {1u, 2u, 3u, 17u, 257u, 1000u, 5000u};
    const u32 rates[] = {8000u, 11025u, 22050u, 44100u, 48000u, 96000u};
    const float pitches[] = {0.05f, 0.5f, 1.0f, 1.37f, 3.0f, 12.0f, 1.0e6f, 1.0e9f};
    const float volumes[] = {0.0f, 1.0e-6f, 0.3f, 1.0f, 2.5f};
    const u32 callbackFrames[] = {1u, 2u, 37u, 512u, kOutputFrames, kOutputFrames};
    u32 callbacks = 0;
    for (u32 scenario = 0; scenario < 1500; ++scenario) {
        reset();
        g_mono = Random(4) == 0;
        g_masterVolume = Random(3) == 0 ? 0.37f : 1.0f;
        AudioSample samples[4] = {};
        for (AudioSample& sample : samples) {
            const u32 channels = 1u + Random(3);
            const u32 frames = Pick(frameCounts);
            std::vector<s16> pcm(frames * channels);
            for (s16& value : pcm) {
                value = Random(8) == 0 ? (Random(2) ? 32767 : -32768)
                                       : static_cast<s16>(Random(65536) - 32768);
            }
            sample = AudioEngine::LoadSample(pcm.data(), frames, Pick(rates), channels);
        }
        const u32 voiceCount = 1u + Random(8);
        for (u32 i = 0; i < voiceCount; ++i) {
            const AudioSample sample = samples[Random(4)];
            const AudioVoice voice = AudioEngine::PlaySample(sample, Pick(volumes),
                (static_cast<float>(Random(201)) - 100.0f) / 100.0f, Random(2) == 0);
            if (voice == AUDIO_VOICE_INVALID) continue;
            AudioEngine::SetVoicePitch(voice, Random(10) == 0
                ? std::numeric_limits<float>::infinity() : Pick(pitches));
            Voice& state = g_voices[voice - 1];
            const u64 end = static_cast<u64>(g_samples[sample - 1].frames) * kCursorOne;
            const u32 cursorKind = Random(4);
            if (cursorKind == 1) state.cursor = end - 1u - Random(1u << 20) % end;
            if (cursorKind == 2) state.cursor = end + Random(1u << 30);
            if (cursorKind == 3) state.cursor = (static_cast<u64>(Random(1u << 20)) << 20) % end;
            if (Random(3) == 0) {
                AudioEngine::FadeVoiceVolume(voice, Pick(volumes), Random(60));
            }
        }
        if (Random(3) != 0) {
            const u32 pairs = 1u + Random(3);
            g_musicStream.pairCount = pairs;
            g_musicStream.framesPerPair = kMusicChunkSamples;
            g_musicStream.totalFrames = pairs * kMusicChunkSamples - Random(3) * Random(700);
            g_musicStream.loop = Random(2) == 0;
            const u32 pair = Random(pairs);
            g_musicStream.currentPair = pair;
            g_musicStream.currentBuffer.store(Random(2));
            const u32 lastFrame = std::min(g_musicStream.totalFrames, (pair + 1u) * kMusicChunkSamples);
            const u32 nearEnd = Random(1500);
            const u32 first = lastFrame > nearEnd ? lastFrame - nearEnd : pair * kMusicChunkSamples;
            g_musicStream.cursor = static_cast<u64>(std::max(first, pair * kMusicChunkSamples)) * kCursorOne
                + (Random(2) ? kCursorOne / 2u : 0u);
            if (Random(4) != 0) {
                g_musicStream.readyPair.store(pair + 1u < pairs ? pair + 1u : 0u);
                g_musicStream.readyBuffer.store(1u - g_musicStream.currentBuffer.load());
            }
            g_musicStream.volume = Pick(volumes);
            g_musicStream.targetVolume = Pick(volumes);
            g_musicStream.fadeFrames = Random(3) == 0 ? Random(3000) : 0u;
            g_musicStream.holdLeft = 0.0f;
            g_musicStream.holdRight = 0.0f;
            g_musicStream.active.store(true);
        }
        for (u32 callback = 0; callback < 4; ++callback) {
            const u32 frames = Pick(callbackFrames);
            const MixerState before = SaveMixer();
            std::fill(referenceMix.begin(), referenceMix.end(), 0);
            ReferenceMixVoices(referenceMix.data(), frames);
            ReferenceMixMusicStream(referenceMix.data(), frames);
            const MixerState reference = SaveMixer();
            RestoreMixer(before);
            std::fill(segmentedMix.begin(), segmentedMix.end(), 0);
            MixVoices(segmentedMix.data(), frames);
            MixMusicStream(segmentedMix.data(), frames);
            const MixerState segmented = SaveMixer();
            if (referenceMix != segmentedMix) {
                u32 first = 0;
                while (referenceMix[first] == segmentedMix[first]) ++first;
                std::fprintf(stderr, "scenario %u callback %u frames %u: mix[%u] %d != %d\n",
                    scenario, callback, frames, first, referenceMix[first], segmentedMix[first]);
                require(false, "segmented mixer output differs from the per-frame mixer");
            }
            if (!SameMixer(reference, segmented)) {
                std::fprintf(stderr, "scenario %u callback %u frames %u\n", scenario, callback, frames);
                require(false, "segmented mixer state differs from the per-frame mixer");
            }
            ++callbacks;
        }
    }
    reset();
    return callbacks;
}

int main() {
    std::array<s16, kOutputFrames * 2> output{};
    int maximumError = 0;
    // Compare with the previous double-cursor interpolation over multiple
    // callback boundaries, pitch/rate combinations and mono/stereo inputs.
    for (u32 channels : {1u, 2u}) for (u32 rate : {11025u, 22050u, 44100u, 48000u, 96000u}) {
        for (float pitch : {0.05f, 0.75f, 1.0f, 1.5f, 12.0f}) {
            reset();
            std::vector<s16> pcm(257 * channels);
            for (u32 i = 0; i < 257; ++i) for (u32 c = 0; c < channels; ++c)
                // Matching endpoints avoid a discontinuity at the loop seam:
                // sub-sample quantization can place a seam on either side of
                // one output sample without changing playback rate.
                pcm[i * channels + c] = static_cast<s16>(std::sin(i * 6.283185307179586 / 256.0) * (12000 - c * 2000));
            const auto sample = AudioEngine::LoadSample(pcm.data(), 257, rate, channels);
            const auto voice = AudioEngine::PlaySample(sample, 0.8f, 0.25f, true);
            AudioEngine::SetVoicePitch(voice, pitch);
            double reference = 0;
            for (int batch = 0; batch < 16; ++batch) {
                AudioCallback(output.data(), kOutputFrames, nullptr);
                for (u32 frame = 0; frame < kOutputFrames; ++frame) {
                    reference = std::fmod(reference, 257.0);
                    const u32 source = static_cast<u32>(reference);
                    const u32 next = std::min(source + 1, 256u);
                    const float fraction = static_cast<float>(reference - source);
                    const auto read = [&](u32 index) {
                        float sum = 0;
                        for (u32 c = 0; c < channels; ++c) sum += pcm[index * channels + c];
                        return sum / channels;
                    };
                    const float value = read(source) + (read(next) - read(source)) * fraction;
                    const s16 left = static_cast<s16>(value * (0.8f * 0.75f * 0.5f));
                    const s16 right = static_cast<s16>(value * (0.8f * 1.25f * 0.5f));
                    maximumError = std::max({maximumError, std::abs(output[frame * 2] - left), std::abs(output[frame * 2 + 1] - right)});
                    reference += static_cast<double>(rate) / kOutputRate * std::max(pitch, 0.05f);
                }
            }
        }
    }
    require(maximumError <= 2, "PCM differs from double reference by more than two levels");
    reset();
    const s16 one = 12000;
    const auto sample = AudioEngine::LoadSample(&one, 1, 44100, 1);
    const auto voice = AudioEngine::PlaySample(sample, 1, 0, true);
    // A recycled voice can carry an old cursor past the new clip's length.
    g_voices[voice - 1].cursor = (1ull << 32) * kCursorOne + kCursorOne / 2;
    AudioEngine::SetVoicePitch(voice, std::numeric_limits<float>::max());
    AudioCallback(output.data(), kOutputFrames, nullptr);
    require(std::all_of(output.begin(), output.end(), [](s16 v) { return v == 6000; }), "one-frame/high-pitch wrap");
    for (float pitch : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        AudioEngine::SetVoicePitch(voice, pitch);
        AudioCallback(output.data(), kOutputFrames, nullptr);
        require(output.front() == 6000, "non-finite pitch");
    }
    g_voices[voice - 1] = {};
    AudioEngine::PlaySample(sample, 1, 0, false);
    AudioCallback(output.data(), kOutputFrames, nullptr);
    require(output[0] == 6000 && output[2] == 0 && !g_voices[0].active, "non-loop termination");
    // A long streaming track must still advance by exactly half a source
    // frame. A float cursor would stop representing halves beyond 2^23.
    reset();
    const u32 base = 1u << 25;
    g_musicStream.totalFrames = base + 200000;
    g_musicStream.pairCount = 1000;
    g_musicStream.currentPair = base / kMusicChunkSamples;
    g_musicStream.currentBuffer.store(0);
    g_musicStream.cursor = static_cast<u64>(base) * kCursorOne;
    g_musicStream.volume = 1;
    g_musicStream.active.store(true);
    g_musicStream.chunks[0].left.fill(4000);
    g_musicStream.chunks[0].right.fill(-4000);
    AudioCallback(output.data(), kOutputFrames, nullptr);
    require(g_musicStream.cursor == (static_cast<u64>(base) + 512) * kCursorOne, "long-track half-step precision");
    require(output.front() == 4000 && output.back() == -4000, "stream stereo mixing");
    // Crossing a decoded pair retains the published two-buffer handoff.
    g_musicStream.cursor = (static_cast<u64>(g_musicStream.currentPair + 1) * kMusicChunkSamples - 1) * kCursorOne;
    g_musicStream.readyPair.store(g_musicStream.currentPair + 1);
    g_musicStream.readyBuffer.store(1);
    g_musicStream.chunks[1].left.fill(8000);
    g_musicStream.chunks[1].right.fill(-8000);
    AudioCallback(output.data(), kOutputFrames, nullptr);
    require(output[0] == 4000 && output[4] == 8000 && output.back() == -8000, "stream pair boundary");
    g_musicStream.active.store(false);
    const u32 equivalenceCallbacks = CheckReferenceEquivalence();
    std::printf("PASS: PSP mixer, reference PCM max error=%d, pitch/wrap/long-track/pair handoff, "
                "%u callbacks bit-exact with the per-frame mixer\n",
                maximumError, equivalenceCallbacks);
}

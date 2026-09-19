// The real PSP callback, with only kernel/device/file I/O stubbed. Timing is
// measured on Allegrex in PPSSPP, not inferred from this host test.
#include "../../src/psp/psp_audio.cpp"
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
    std::printf("PASS: PSP mixer, reference PCM max error=%d, pitch/wrap/long-track/pair handoff\n", maximumError);
}

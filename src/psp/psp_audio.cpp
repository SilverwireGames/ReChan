#include "pc/audio.h"
#include "p3d/fileio.h"
#include "snd/adpcm.h"

#include <pspaudiolib.h>
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

struct Sample {
    std::vector<s16> pcm;
    u32 frames = 0;
    u32 rate = kOutputRate;
    u32 channels = 1;
    bool loaded = false;
};

struct Voice {
    AudioSample sample = AUDIO_SAMPLE_INVALID;
    double cursor = 0.0;
    float volume = 1.0f;
    float targetVolume = 1.0f;
    float pan = 0.0f;
    float pitch = 1.0f;
    float minDistance = 0.0f;
    float maxDistance = 10000.0f;
    LVector position{};
    u8 priority = 0;
    u32 fadeFrames = 0;
    bool loop = false;
    bool reserved = false;
    bool active = false;
};

// FAG stores alternating 32 KB mono ADPCM chunks (left, right). Keep the file
// open and decode only the current stereo pair. This avoids allocating the
// complete compressed or decoded music track on the PSP heap.
struct MusicStream {
    std::unique_ptr<p3d::io::FileHandle> file;
    u32 dataOffset = 0;
    u32 dataEnd = 0;
    u32 pairCount = 0;
    u32 framesPerPair = kMusicChunkSamples;
    u32 totalFrames = 0;
    u32 currentPair = 0xFFFFFFFFu;
    double cursor = 0.0;
    float volume = 1.0f;
    float targetVolume = 1.0f;
    u32 fadeFrames = 0;
    bool loop = true;
    std::atomic_bool active{false};
    std::array<u8, kMusicChunkBytes * 2> compressedPair = {};
    std::array<s16, kMusicChunkSamples> left = {};
    std::array<s16, kMusicChunkSamples> right = {};
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

static void DecodeMusicChunk(const u8* chunkData, std::array<s16, kMusicChunkSamples>& output) {
    s32 prev0 = 0;
    s32 prev1 = 0;
    for (u32 block = 0; block < kMusicChunkBlocks; ++block) {
        s16 blockOutput[28];
        SpuAdpcm::DecodeBlock(chunkData + block * 16, blockOutput, prev0, prev1);
        std::memcpy(output.data() + block * 28, blockOutput, sizeof(blockOutput));
    }
}

static bool PrepareMusicPair(u32 pair) {
    if (pair >= g_musicStream.pairCount) {
        return false;
    }
    if (g_musicStream.currentPair == pair) {
        return true;
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

    DecodeMusicChunk(g_musicStream.compressedPair.data(), g_musicStream.left);
    DecodeMusicChunk(g_musicStream.compressedPair.data() + kMusicChunkBytes, g_musicStream.right);
    g_musicStream.currentPair = pair;
    return true;
}

static void MixMusicStream(s16* output, u32 requestedFrames) {
    g_musicMixers.fetch_add(1, std::memory_order_acquire);
    if (!g_musicStream.active.load(std::memory_order_acquire)
        || g_musicStream.totalFrames == 0) {
        g_musicMixers.fetch_sub(1, std::memory_order_release);
        return;
    }

    for (u32 frame = 0; frame < requestedFrames; ++frame) {
        if (g_musicStream.cursor >= g_musicStream.totalFrames) {
            if (!g_musicStream.loop) {
                g_musicStream.active.store(false, std::memory_order_release);
                break;
            }
            g_musicStream.cursor = 0.0;
            g_musicStream.currentPair = 0xFFFFFFFFu;
        }

        const u32 sourceFrame = static_cast<u32>(g_musicStream.cursor);
        const u32 pair = sourceFrame / g_musicStream.framesPerPair;
        if (pair >= g_musicStream.pairCount || !PrepareMusicPair(pair)) {
            g_musicStream.active.store(false, std::memory_order_release);
            break;
        }

        const u32 localFrame = sourceFrame % g_musicStream.framesPerPair;
        const u32 nextFrame = std::min<u32>(localFrame + 1, g_musicStream.framesPerPair - 1);
        const float fraction = static_cast<float>(g_musicStream.cursor - sourceFrame);
        const float left = static_cast<float>(g_musicStream.left[localFrame])
            + (static_cast<float>(g_musicStream.left[nextFrame])
                - static_cast<float>(g_musicStream.left[localFrame])) * fraction;
        const float right = static_cast<float>(g_musicStream.right[localFrame])
            + (static_cast<float>(g_musicStream.right[nextFrame])
                - static_cast<float>(g_musicStream.right[localFrame])) * fraction;
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

        g_musicStream.cursor += 0.5;
        if (g_musicStream.fadeFrames > 0) {
            --g_musicStream.fadeFrames;
            g_musicStream.volume += (g_musicStream.targetVolume - g_musicStream.volume)
                / static_cast<float>(g_musicStream.fadeFrames + 1);
        }
    }

    g_musicMixers.fetch_sub(1, std::memory_order_release);
}

static void ResetMusicStream() {
    // Deactivate first, then wait for an already-running callback before
    // releasing the file it may still be reading.
    g_musicStream.active.store(false, std::memory_order_release);
    while (g_musicMixers.load(std::memory_order_acquire) != 0) {
        sceKernelDelayThread(0);
    }
    g_musicStream.file.reset();
    g_musicStream.dataOffset = 0;
    g_musicStream.dataEnd = 0;
    g_musicStream.pairCount = 0;
    g_musicStream.totalFrames = 0;
    g_musicStream.currentPair = 0xFFFFFFFFu;
    g_musicStream.cursor = 0.0;
}

static void AudioCallback(void* buffer, unsigned int requestedFrames, void*) {
    auto* output = static_cast<s16*>(buffer);
    std::memset(output, 0, requestedFrames * 2 * sizeof(s16));

    for (Voice& voice : g_voices) {
        if (!voice.active) continue;
        Sample* sample = GetSample(voice.sample);
        if (!sample || sample->frames == 0) {
            voice.active = false;
            continue;
        }

        float left = voice.volume * g_masterVolume * (1.0f - std::clamp(voice.pan, -1.0f, 1.0f)) * 0.5f;
        float right = voice.volume * g_masterVolume * (1.0f + std::clamp(voice.pan, -1.0f, 1.0f)) * 0.5f;
        if (g_mono) right = left = (left + right) * 0.5f;

        for (u32 frame = 0; frame < requestedFrames; ++frame) {
            if (!voice.active) break;
            if (voice.cursor >= sample->frames) {
                if (voice.loop) voice.cursor = std::fmod(voice.cursor, static_cast<double>(sample->frames));
                else {
                    voice.active = false;
                    break;
                }
            }

            const u32 sourceFrame = static_cast<u32>(voice.cursor);
            const u32 nextFrame = std::min<u32>(sourceFrame + 1, sample->frames - 1);
            const float fraction = static_cast<float>(voice.cursor - sourceFrame);
            const auto read = [&](u32 source) -> float {
                if (sample->channels == 1) return sample->pcm[source];
                const u32 index = source * sample->channels;
                float sum = 0.0f;
                for (u32 channel = 0; channel < sample->channels; ++channel) sum += sample->pcm[index + channel];
                return sum / static_cast<float>(sample->channels);
            };
            const float value = read(sourceFrame) + (read(nextFrame) - read(sourceFrame)) * fraction;
            const u32 outputIndex = frame * 2;
            output[outputIndex] = static_cast<s16>(std::clamp(output[outputIndex] + value * left, -32768.0f, 32767.0f));
            output[outputIndex + 1] = static_cast<s16>(std::clamp(output[outputIndex + 1] + value * right, -32768.0f, 32767.0f));
            voice.cursor += static_cast<double>(sample->rate) / kOutputRate * std::max(voice.pitch, 0.05f);
            if (voice.fadeFrames > 0) {
                --voice.fadeFrames;
                voice.volume += (voice.targetVolume - voice.volume) / static_cast<float>(voice.fadeFrames + 1);
                left = voice.volume * g_masterVolume * (1.0f - std::clamp(voice.pan, -1.0f, 1.0f)) * 0.5f;
                right = voice.volume * g_masterVolume * (1.0f + std::clamp(voice.pan, -1.0f, 1.0f)) * 0.5f;
            }
        }
    }

    MixMusicStream(output, requestedFrames);
}

static AudioVoice AllocateVoice(u8 priority) {
    for (u32 i = 0; i < g_voices.size(); ++i) {
        if (!g_voices[i].active && !g_voices[i].reserved) return i + 1;
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
    if (g_initialized) return true;
    if (pspAudioInit() < 0) return false;
    pspAudioSetChannelCallback(0, AudioCallback, nullptr);
    pspAudioSetVolume(0, PSP_VOLUME_MAX, PSP_VOLUME_MAX);
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
    state->sample = sample; state->cursor = 0.0; state->volume = volume; state->targetVolume = volume;
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
    g_musicStream.currentPair = 0xFFFFFFFFu;
    g_musicStream.cursor = 0.0;
    g_musicStream.volume = std::max(volume, 0.0f);
    g_musicStream.targetVolume = g_musicStream.volume;
    g_musicStream.fadeFrames = 0;
    g_musicStream.loop = loop;
    if (!PrepareMusicPair(0)) {
        ResetMusicStream();
        return false;
    }
    // Publish the fully initialized stream only after its first decoded pair
    // is ready, so the callback never sees partial metadata or buffers.
    g_musicStream.active.store(true, std::memory_order_release);
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

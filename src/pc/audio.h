#pragma once
#include "core.h"
#include <vector>

// Opaque handle types
using AudioSample = u32;  // handle to a loaded sample in the engine
using AudioVoice = u32;   // handle to a playing voice
static constexpr AudioSample AUDIO_SAMPLE_INVALID = 0;
static constexpr AudioVoice AUDIO_VOICE_INVALID = 0;

// PSX: rsdAllocVoice__FUc priority byte (RSDBACH.CPP). Higher wins; steals lower.
enum AudioPriority : u8 {
    AUDIO_PRIO_AMBIENCE = 16,
    AUDIO_PRIO_SFX      = 64,   // default for PlaySample / PlaySample3D
    AUDIO_PRIO_SFX_HIGH = 128,
    AUDIO_PRIO_DIALOGUE = 255,
};

// AudioEngine - PC audio abstraction over miniaudio
// All game code accesses audio through this interface only
class AudioEngine {
public:
    // Init/shutdown
    static bool Init();
    static void Shutdown();
    static bool IsInitialized();

    // Sample management (preloaded PCM in memory)
    // data: interleaved s16 PCM, takes ownership via copy
    static AudioSample LoadSample(const s16* data, u32 numFrames, u32 sampleRate, u32 channels);
#if defined(RC_PLATFORM_PSP)
    // PSP-only fast path: transfer ownership of a decoded track instead of
    // copying a multi-megabyte music buffer into the sample table.
    static AudioSample LoadSampleOwned(std::vector<s16>&& data, u32 numFrames,
                                       u32 sampleRate, u32 channels);
#endif
    static void UnloadSample(AudioSample handle);
    static void UnloadAllSamples();

    // Voice playback
    static AudioVoice PlaySample(AudioSample sample, f32 volume = 1.0f, f32 pan = 0.0f, bool loop = false,
        u8 priority = AUDIO_PRIO_SFX);
    static AudioVoice PlaySample3D(
        AudioSample sample,
        const LVector& position,
        f32 volume = 1.0f,
        bool loop = false,
        bool applyDistanceAttenuation = true,
        f32 minDistance = 0.0f,
        f32 maxDistance = 10000.0f,
        u8 priority = AUDIO_PRIO_SFX);

    // PSX: reserved/locked voice - jcsSetLevelDialog captures gp+1220 via rsdLockVoice.
    static AudioVoice ReserveVoice(u8 priority);
    static void ReleaseVoice(AudioVoice reserved);
    static AudioVoice PlayOnReservedVoice(AudioVoice reserved, AudioSample sample,
        f32 volume = 1.0f, f32 pan = 0.0f);

    static void StopVoice(AudioVoice voice);
    static void StopAllVoices();
    static bool IsVoicePlaying(AudioVoice voice);
    static void SetVoiceVolume(AudioVoice voice, f32 volume);
    static void FadeVoiceVolume(AudioVoice voice, f32 targetVolume, u32 fadeMs);
    static void SetVoicePan(AudioVoice voice, f32 pan);
    static void SetVoicePitch(AudioVoice voice, f32 pitch);
    static void SetVoicePosition(AudioVoice voice, const LVector& position);
    static void SetVoiceDistanceRange(AudioVoice voice, f32 minDistance, f32 maxDistance);

    // Listener state (used by 3D voices)
    static void SetListener(const LVector& position, s32 yaw16);

    // Streaming music (reads from file, decoded on the fly)
    static bool PlayMusic(const char* path, f32 volume = 1.0f, bool loop = true);
#if defined(RC_PLATFORM_PSP)
    // Select a song entry from a multi-song FAG without decoding the whole file.
    static bool PlayMusicSong(const char* path, u32 songIndex,
                              f32 volume = 1.0f, bool loop = true);
#endif
    static bool PlayMusicSample(AudioSample sample, f32 volume = 1.0f, bool loop = true);
    static void StopMusic();
    static void SetMusicVolume(f32 volume);
    static void FadeMusicVolume(f32 targetVolume, u32 fadeMs);
    static bool IsMusicPlaying();

    // Master volume
    static void SetMasterVolume(f32 volume);
    static f32 GetMasterVolume();

    // Output controls
    static void SetOutputMono(bool mono);
    static bool GetOutputMono();
    static u32 GetOutputChannels();
    static void SetSurroundEnabled(bool enabled);
    static bool GetSurroundEnabled();
};

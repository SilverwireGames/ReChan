#pragma once
#include "gen/config.h"
#include "gen/manager.h"
#include "pc/audio.h"

#include <vector>

// WAX banks: RS0000..RS0015 = 22 banks total (hex names)
// PSX architecture: all banks share SPU base 0x1010, 71 descriptors each.
// Only ONE bank is active at a time, selected by sound location.
static constexpr u32 MAX_WAX_BANKS = 22;
static constexpr u32 MAX_SAMPLES_PER_BANK = 71;

// Sound (44 bytes on PSX) - audio manager
// PSX layout:
//   +0:  Manager base (28 bytes: ccNode(24) + isOpen(s16))
//   +28: callback ref (u32)
//   +32: flags (3 x s16, init 0xFFFF)
//   +38: padding
//   +40: activeFlag (u32, init 1)
class Sound : public Manager {
public:
    s16 flag0 = -1;     // +32: init 0xFFFF
    s16 flag1 = -1;     // +34: init 0xFFFF
    s16 flag2 = -1;     // +36: init 0xFFFF
    u32 activeFlag = 1; // +40: 1 = active

    // PC: per-bank sample handles (22 banks x 71 slots)
    struct BankInfo {
        AudioSample samples[MAX_SAMPLES_PER_BANK] = {};
        // Raw descriptor word at +8 from RSDBACH tag 0x11 table (used by rsdAllocPhonograph).
        u32 descriptorWord2[MAX_SAMPLES_PER_BANK] = {};
        u32 numSamples = 0;
#if defined(RC_PLATFORM_PSP)
        // PSP on-demand WAX: where this sample's ADPCM starts and how to play it.
        u32 spuAddr[MAX_SAMPLES_PER_BANK] = {};
        u32 sampleRate[MAX_SAMPLES_PER_BANK] = {};
        u32 adpcmSpuBase = 0;
        u32 adpcmFileOffset = 0;
        u32 adpcmLength = 0;
        // The bank's ADPCM region, kept from the boot-time read when the large
        // heap allows it, so playing a new effect never touches the Memory Stick.
        std::vector<u8> residentAdpcm;
#endif
    };

#if defined(RC_PLATFORM_PSP)
    // Mixer slots are scarce on PSP (128 total): WAX samples live in a small
    // round-robin pool and are unloaded (UnloadSample stops any voice using them)
    // to make room for the next one.
    static constexpr u32 kPspWaxPoolSize = 48;
    struct PspWaxPoolSlot {
        AudioSample handle = AUDIO_SAMPLE_INVALID;
        u32 bank = 0;
        u32 sample = 0;
    };
    PspWaxPoolSlot pspWaxPool[kPspWaxPoolSize] = {};
    u32 pspWaxNextSlot = 0;
    AudioSample LoadPspWaxSample(u32 bankIndex, u32 sampleIndex);
#endif
    BankInfo banks[MAX_WAX_BANKS] = {};
    u32 numWaxBanks = 0;

    // PSX: only one bank active at a time. Index into banks[].
    // Set by RS_SET_LOCATION event. Location 0-20 -> bank 0-20, 21+ -> bank 21.
    s32 activeSfxBank = -1;

    // PC: music sample (decoded FAG loaded as one big sample for dedicated music mix)
    AudioSample musicSample = AUDIO_SAMPLE_INVALID;
    AudioVoice musicVoice = AUDIO_VOICE_INVALID;
    bool musicPlaying = false;
    bool musicMuted = false;
    f32 musicVolume = 0.64f;  // default: 100 * 0.8 / 125

    Sound();
    ~Sound() override;
    void InternalOpen() override;
    void InternalClose() override;
    void SetupSound();
    void CleanupSound();

    // PC: play a specific sample from a WAX bank
    AudioVoice PlayWaxSample(u32 bankIndex, u32 sampleIndex, f32 volume = 1.0f, f32 pan = 0.0f);
    u32 GetBankSampleCount(u32 bankIndex) const;
#if defined(RC_PLATFORM_PSP)
    // PSP decodes WAX samples on first use (see LoadPspWaxSample), so this is not const.
    AudioSample GetBankSample(u32 bankIndex, u32 sampleIndex);
#else
    AudioSample GetBankSample(u32 bankIndex, u32 sampleIndex) const;
#endif

    // PC: volume multipliers (0.0-1.0), applied when playing
    f32 effectsVolume = 1.0f;
    f32 dialogVolume = 1.0f;

    // PC: music control
    bool PlayMusicTrack(const char* fagPath, f32 volume = 1.0f);
    bool PlayMusicTrackSong(const char* fagPath, u32 songIndex, f32 volume = 1.0f);

    // PC: decode/load a track without starting playback (so the slow file
    // read + decode can happen while still hidden behind a loading screen),
    // then start it cheaply later with StartPreloadedMusic().
    AudioSample pendingMusicSample = AUDIO_SAMPLE_INVALID;
    f32 pendingMusicVolume = 1.0f;
    bool PreloadMusicTrack(const char* fagPath, f32 volume = 1.0f);
    bool StartPreloadedMusic();
#if MOD_LOADER
    bool TryPlayMusicOverride(const char* fagPath, f32 volume);
#endif
    void StopMusic();
    void SetMusicVolume(f32 volume);
    void SetEffectsVolume(f32 volume);
    void SetDialogVolume(f32 volume);
    void MuteMusic();
    void UnmuteMusic();
};

// PSX: gp-relative global, defined in sound.cpp
extern Sound* g_sound;

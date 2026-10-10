#include "gen/common.h"
#include "snd/sound.h"
#include "snd/rsevent.h"
#include "snd/rsdformat.h"
#include "snd/hmndsnd.h"
#include "xclib/xcfile.h"
#include <algorithm>
#include <utility>
#if defined(RC_PLATFORM_PSP)
#include "p3d/fileio.h"
#endif
#if MOD_LOADER
#include "extra/modloader.h"
#include <cctype>
#include <string>
#endif

// PSX: gp-relative global
Sound* g_sound = nullptr;

// PSX sample rate constants
// SPU pitch 0x0400 = 11025 Hz (SFX), SPU pitch 0x0800 = 22050 Hz (music)
static constexpr u32 PSX_SFX_RATE = 11025;
static constexpr u32 PSX_MUSIC_RATE = 22050;
static constexpr u32 PSX_MUSIC_FADE_MS = 300;

// Helper: read entire file into memory
static u8* ReadFileBytes(const char* path, u32& outSize) {
    u8* data = nullptr;
    if (!xcReadFileLow(path, &data, &outSize)) {
        return nullptr;
    }
    return data;
}

#if MOD_LOADER
static std::string FagPathToOverrideName(const char* fagPath) {
    std::string name = fagPath;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.erase(dot);
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    return name;
}
#endif

// PSX: __5Sound (SOUND.CPP, 0x80059794)
Sound::Sound() {
    MARKFUNCTION(0x80059794);
    for (BankInfo& bank : banks) {
        bank = {};
    }
}

// PSX: _._5Sound (SOUND.CPP, 0x800597E8)
Sound::~Sound() {
    MARKFUNCTION(0x800597E8);
    if (g_sound == this) {
        g_sound = nullptr;
    }
}

// PSX: InternalOpen__5Sound (0x80059818) - allocates callback nodes for load/unload
void Sound::InternalOpen() {
    MARKFUNCTION(0x80059818);
    g_sound = this;
    // PC: initialize audio engine and load sound data
    // PSX: soundLoadFunc callback calls SetupSound() later; PC: call directly
    AudioEngine::Init();
    SetupSound();
    rsEvent(RS_INITIALIZE, 0, 0, 0);
}

// PSX: InternalClose__5Sound
void Sound::InternalClose() {
    rsEvent(RS_TERMINATE, 0, 0, 0);
    // PC: shutdown audio engine
    CleanupSound();
    AudioEngine::Shutdown();
    if (g_sound == this) {
        g_sound = nullptr;
    }
}

// PSX: SetupSound__5Sound (0x800598D8) - loads sound data
void Sound::SetupSound() {
    MARKFUNCTION(0x800598D8);

    // Load WAX sound effect banks (RS0000..RS0015 = 0x00..0x15 = 22 banks, hex naming)
    numWaxBanks = 0;
    u32 totalSamples = 0;
    char path[256];
    for (u32 i = 0; i <= 0x15; i++) {
        snprintf(path, sizeof(path), "SOUND/FX/RS%04X.WAX", static_cast<unsigned int>(i));
        u32 fileSize = 0;
        u8* fileData = ReadFileBytes(path, fileSize);
        if (!fileData) continue;

#if defined(RC_PLATFORM_PSP)
        // PSP: index only. Decoding all ~1.5k samples up front exceeds the 128-slot
        // mixer and most effects stayed silent; samples load on first use instead.
        {
            RsdFormat::WaxIndex index = RsdFormat::ParseWaxIndex(fileData, fileSize);
            delete[] fileData;

            u32 count = (u32)index.sampleDescs.size();
            if (count > MAX_SAMPLES_PER_BANK) count = MAX_SAMPLES_PER_BANK;
            if (count == 0) continue;

            BankInfo& info = banks[i];
            info.numSamples = count;
            info.adpcmSpuBase = index.spuBase;
            info.adpcmFileOffset = index.adpcmFileOffset;
            info.adpcmLength = index.adpcmLength;
            for (u32 j = 0; j < count; j++) {
                const RsdFormat::SampleDesc& desc = index.sampleDescs[j];
                info.samples[j] = AUDIO_SAMPLE_INVALID;
                info.descriptorWord2[j] = desc.flags;
                info.spuAddr[j] = desc.spuAddr;
                const u16 spuPitch = (u16)(desc.params & 0xFFFF);
                info.sampleRate[j] = spuPitch > 0 ? 44100u * spuPitch / 4096u : PSX_SFX_RATE;
            }
            numWaxBanks++;
            continue;
        }
#endif

        RsdFormat::WaxBank bank = RsdFormat::LoadWax(fileData, fileSize);
        delete[] fileData;

        u32 count = (u32)bank.pcmSamples.size();
        if (count > MAX_SAMPLES_PER_BANK) count = MAX_SAMPLES_PER_BANK;
        banks[i].numSamples = count;

        for (u32 j = 0; j < count; j++) {
            if (j < bank.sampleDescs.size()) {
                banks[i].descriptorWord2[j] = bank.sampleDescs[j].flags;
            }

#if MOD_LOADER
            // Mod override layout matches the asset exporter's own output:
            // sounds/rs0000/sample00.wav -> scope "rs0000", name "sample00".
            // Checked before the empty-sample skip below so a mod can also
            // add a sound at a slot the original bank left empty.
            {
                char bankName[16];
                snprintf(bankName, sizeof(bankName), "rs%04x", i);
                char sampleName[16];
                snprintf(sampleName, sizeof(sampleName), "sample%02u", j);

                std::vector<s16> overridePcm;
                u32 overrideRate = 0, overrideChannels = 0;
                if (ModLoader::Instance().GetSoundOverridePCM(
                        bankName, sampleName, overridePcm, overrideRate, overrideChannels)) {
                    const u32 overrideFrames = (u32)overridePcm.size() / overrideChannels;
                    banks[i].samples[j] = AudioEngine::LoadSample(
                        overridePcm.data(), overrideFrames, overrideRate, overrideChannels);
                    if (banks[i].samples[j] != AUDIO_SAMPLE_INVALID) {
                        totalSamples++;
                    }
                    continue;
                }
            }
#endif

            if (bank.pcmSamples[j].empty()) continue;
            u32 numFrames = (u32)bank.pcmSamples[j].size();

            // Per-sample rate from descriptor pitch (SPU pitch 0x1000 = 44100 Hz)
            u32 sampleRate = PSX_SFX_RATE;
            if (j < bank.sampleDescs.size()) {
                u16 spuPitch = (u16)(bank.sampleDescs[j].params & 0xFFFF);
                if (spuPitch > 0) {
                    sampleRate = 44100u * spuPitch / 4096u;
                }
            }

            banks[i].samples[j] = AudioEngine::LoadSample(
                bank.pcmSamples[j].data(), numFrames, sampleRate, 1);
            if (banks[i].samples[j] != AUDIO_SAMPLE_INVALID) {
                totalSamples++;
            }
        }

        if (count > 0) {
            numWaxBanks++;
            LOG("Sound: WAX bank 0x%02X: %u decoded samples, %u bytes ADPCM (SPU base 0x%X)",
                i, count, bank.adpcmSize, bank.adpcmOffset);
        }
    }

    LOG("Sound: loaded %u WAX banks (%u total samples)", numWaxBanks, totalSamples);

    // PSX: called from CSoundFactory::Setup; PC: call here during sound init
    CHumanoidSound::LoadHumanoidSoundScripts();
}

// PSX: CleanupSound__5Sound (0x800599B0) - unloads sound data
void Sound::CleanupSound() {
    MARKFUNCTION(0x800599B0);

    CHumanoidSound::UnloadHumanoidSoundScripts();

    StopMusic();
    AudioEngine::StopAllVoices();
    AudioEngine::UnloadAllSamples();

    for (BankInfo& bank : banks) {
        bank = {};
    }
    musicSample = AUDIO_SAMPLE_INVALID;
    musicVoice = AUDIO_VOICE_INVALID;
    numWaxBanks = 0;
}

// PC: play a specific sample from a WAX bank
AudioVoice Sound::PlayWaxSample(u32 bankIndex, u32 sampleIndex, f32 volume, f32 pan) {
    if (bankIndex >= MAX_WAX_BANKS) return AUDIO_VOICE_INVALID;
    if (sampleIndex >= banks[bankIndex].numSamples) return AUDIO_VOICE_INVALID;
    AudioSample s = banks[bankIndex].samples[sampleIndex];
    if (s == AUDIO_SAMPLE_INVALID) return AUDIO_VOICE_INVALID;
    return AudioEngine::PlaySample(s, volume * effectsVolume, pan, false);
}

u32 Sound::GetBankSampleCount(u32 bankIndex) const {
    if (bankIndex >= MAX_WAX_BANKS) return 0;
    return banks[bankIndex].numSamples;
}

#if defined(RC_PLATFORM_PSP)
AudioSample Sound::GetBankSample(u32 bankIndex, u32 sampleIndex) {
    if (bankIndex >= MAX_WAX_BANKS) return AUDIO_SAMPLE_INVALID;
    if (sampleIndex >= banks[bankIndex].numSamples) return AUDIO_SAMPLE_INVALID;
    if (banks[bankIndex].samples[sampleIndex] != AUDIO_SAMPLE_INVALID) {
        return banks[bankIndex].samples[sampleIndex];
    }
    return LoadPspWaxSample(bankIndex, sampleIndex);
}

// PSP: reads one sample's ADPCM from its bank file up to the END block, decodes it
// and loads it into a mixer slot. The round-robin pool keeps the number of resident
// effects bounded; the oldest entry is unloaded (stopping any voice using it).
AudioSample Sound::LoadPspWaxSample(u32 bankIndex, u32 sampleIndex) {
    BankInfo& info = banks[bankIndex];
    if (info.adpcmLength == 0) return AUDIO_SAMPLE_INVALID;

    const u32 spuAddr = info.spuAddr[sampleIndex];
    if (spuAddr < info.adpcmSpuBase) return AUDIO_SAMPLE_INVALID;
    const u32 byteOff = spuAddr - info.adpcmSpuBase;
    if (byteOff >= info.adpcmLength) return AUDIO_SAMPLE_INVALID;

    char path[32];
    snprintf(path, sizeof(path), "SOUND/FX/RS%04X.WAX", static_cast<unsigned int>(bankIndex));
    p3d::io::FileHandle file;
    if (!file.Open(path, p3d::io::FileHandle::Mode::Read)) return AUDIO_SAMPLE_INVALID;
    if (!file.Seek((int64_t)(info.adpcmFileOffset + byteOff), SEEK_SET)) return AUDIO_SAMPLE_INVALID;

    // Gather whole 16-byte ADPCM blocks until (and including) the END-flagged block.
    std::vector<u8> adpcm;
    u8 chunk[16 * 32];
    u32 remaining = info.adpcmLength - byteOff;
    bool ended = false;
    while (!ended && remaining >= 16) {
        const u32 readBytes = std::min<u32>(remaining, sizeof(chunk)) & ~15u;
        if (file.Read(chunk, readBytes) != readBytes) break;
        for (u32 off = 0; off < readBytes && !ended; off += 16) {
            adpcm.insert(adpcm.end(), chunk + off, chunk + off + 16);
            ended = (chunk[off + 1] & 0x01) != 0;
        }
        remaining -= readBytes;
    }
    if (adpcm.empty()) return AUDIO_SAMPLE_INVALID;

    std::vector<s16> pcm = SpuAdpcm::Decode(adpcm.data(), (u32)adpcm.size(), true);
    if (pcm.empty()) return AUDIO_SAMPLE_INVALID;
    const u32 frames = (u32)pcm.size();

    // Recycle the oldest pool entry before loading, so the pool never grows past its size.
    PspWaxPoolSlot& slot = pspWaxPool[pspWaxNextSlot];
    pspWaxNextSlot = (pspWaxNextSlot + 1) % kPspWaxPoolSize;
    if (slot.handle != AUDIO_SAMPLE_INVALID) {
        AudioEngine::UnloadSample(slot.handle);
        banks[slot.bank].samples[slot.sample] = AUDIO_SAMPLE_INVALID;
    }
    slot = {};

    const AudioSample handle = AudioEngine::LoadSampleOwned(
        std::move(pcm), frames, info.sampleRate[sampleIndex], 1);
    if (handle == AUDIO_SAMPLE_INVALID) return handle;

    slot.handle = handle;
    slot.bank = bankIndex;
    slot.sample = sampleIndex;
    info.samples[sampleIndex] = handle;
    return handle;
}
#else
AudioSample Sound::GetBankSample(u32 bankIndex, u32 sampleIndex) const {
    if (bankIndex >= MAX_WAX_BANKS) return AUDIO_SAMPLE_INVALID;
    if (sampleIndex >= banks[bankIndex].numSamples) return AUDIO_SAMPLE_INVALID;
    return banks[bankIndex].samples[sampleIndex];
}
#endif

#if MOD_LOADER
// Shared by both PlayMusicTrack variants: looks up a mod override WAV named
// after the FAG's filename stem (matches the asset exporter's own output,
// e.g. "title.wav" for SOUND/MUSIC/TITLE.FAG) and, if found, loads and plays
// it directly through the dedicated music mix. Returns true if it played.
bool Sound::TryPlayMusicOverride(const char* fagPath, f32 volume) {
    std::vector<s16> overridePcm;
    u32 overrideRate = 0, overrideChannels = 0;
    const std::string overrideName = FagPathToOverrideName(fagPath);
    if (!ModLoader::Instance().GetSoundOverridePCM(
            nullptr, overrideName.c_str(), overridePcm, overrideRate, overrideChannels)) {
        return false;
    }

    StopMusic();
    const u32 overrideFrames = (u32)overridePcm.size() / overrideChannels;
    musicSample = AudioEngine::LoadSample(overridePcm.data(), overrideFrames, overrideRate, overrideChannels);
    if (musicSample == AUDIO_SAMPLE_INVALID) return false;

    musicVolume = volume;
    f32 playVol = musicMuted ? 0.0f : volume;
    musicVoice = AUDIO_VOICE_INVALID;
    musicPlaying = AudioEngine::PlayMusicSample(musicSample, playVol, true);
    LOG("Sound: playing mod override '%s' for '%s' (%u frames)", overrideName.c_str(), fagPath, overrideFrames);
    return musicPlaying;
}
#endif

// PC/PSP: decode FAG to PCM, load as AudioSample, play through the dedicated music mix.
bool Sound::PlayMusicTrackSong(const char* fagPath, u32 songIndex, f32 volume) {
#if MOD_LOADER
    if (TryPlayMusicOverride(fagPath, volume)) return true;
#endif

#if defined(RC_PLATFORM_PSP)
    StopMusic();
    musicVolume = volume;
    musicPlaying = AudioEngine::PlayMusicSong(
        fagPath, songIndex, musicMuted ? 0.0f : volume, true);
    return musicPlaying;
#endif

    u32 fileSize = 0;
    u8* fileData = ReadFileBytes(fagPath, fileSize);
    if (!fileData) { return false; }

    RsdFormat::FagTrack track = RsdFormat::LoadFag(fileData, fileSize, songIndex);
    delete[] fileData;
    if (track.pcmData.empty()) return false;

    StopMusic();
#if defined(RC_PLATFORM_PSP)
    musicSample = AudioEngine::LoadSampleOwned(
        std::move(track.pcmData), track.numFrames, track.sampleRate, track.channels);
#else
    musicSample = AudioEngine::LoadSample(
        track.pcmData.data(), track.numFrames, track.sampleRate, track.channels);
#endif
    if (musicSample == AUDIO_SAMPLE_INVALID) return false;

    musicVolume = volume;
    f32 playVol = musicMuted ? 0.0f : volume;
    musicVoice = AUDIO_VOICE_INVALID;
    musicPlaying = AudioEngine::PlayMusicSample(musicSample, playVol, true);
    LOG("Sound: playing '%s' song=%u (%u frames)", fagPath, songIndex, track.numFrames);
    return musicPlaying;
}

bool Sound::PlayMusicTrack(const char* fagPath, f32 volume) {
#if MOD_LOADER
    if (TryPlayMusicOverride(fagPath, volume)) return true;
#endif

#if defined(RC_PLATFORM_PSP)
    StopMusic();
    musicVolume = volume;
    musicPlaying = AudioEngine::PlayMusic(fagPath, musicMuted ? 0.0f : volume, true);
    return musicPlaying;
#endif

    u32 fileSize = 0;
    u8* fileData = ReadFileBytes(fagPath, fileSize);
    if (!fileData) {
        LOG("Sound: failed to load music '%s'", fagPath);
        return false;
    }

    RsdFormat::FagTrack track = RsdFormat::LoadFag(fileData, fileSize);
    delete[] fileData;

    if (track.pcmData.empty()) {
        LOG("Sound: failed to decode music '%s'", fagPath);
        return false;
    }

    // Stop any existing music
    StopMusic();

    // Load decoded PCM as an AudioSample (stereo for FAG)
#if defined(RC_PLATFORM_PSP)
    musicSample = AudioEngine::LoadSampleOwned(
        std::move(track.pcmData), track.numFrames, track.sampleRate, track.channels);
#else
    musicSample = AudioEngine::LoadSample(
        track.pcmData.data(), track.numFrames, track.sampleRate, track.channels);
#endif
    if (musicSample == AUDIO_SAMPLE_INVALID) {
        LOG("Sound: failed to load music sample");
        return false;
    }

    // Route music through the dedicated music mixer so gameplay voices cannot steal it.
    musicVolume = volume;
    f32 playVol = musicMuted ? 0.0f : volume;
    musicVoice = AUDIO_VOICE_INVALID;
    musicPlaying = AudioEngine::PlayMusicSample(musicSample, playVol, true);

    LOG("Sound: playing music '%s' (%u frames, %u ch @ %u Hz, sample=%u, dedicated=%d)",
        fagPath, track.numFrames, track.channels, PSX_MUSIC_RATE, musicSample, musicPlaying ? 1 : 0);
    return musicPlaying;
}

// PC: same decode work as PlayMusicTrack, but stops at LoadSample - playback
// is kicked off later (and cheaply) via StartPreloadedMusic().
bool Sound::PreloadMusicTrack(const char* fagPath, f32 volume) {
#if defined(RC_PLATFORM_PSP)
    // Streaming starts immediately; the compressed FAG stays resident and no
    // decoded preload buffer is allocated during the loading-screen phase.
    return PlayMusicTrack(fagPath, volume);
#endif

    if (pendingMusicSample != AUDIO_SAMPLE_INVALID) {
        AudioEngine::UnloadSample(pendingMusicSample);
        pendingMusicSample = AUDIO_SAMPLE_INVALID;
    }

    u32 fileSize = 0;
    u8* fileData = ReadFileBytes(fagPath, fileSize);
    if (!fileData) {
        LOG("Sound: failed to load music '%s'", fagPath);
        return false;
    }

    RsdFormat::FagTrack track = RsdFormat::LoadFag(fileData, fileSize);
    delete[] fileData;

    if (track.pcmData.empty()) {
        LOG("Sound: failed to decode music '%s'", fagPath);
        return false;
    }

#if defined(RC_PLATFORM_PSP)
    pendingMusicSample = AudioEngine::LoadSampleOwned(
        std::move(track.pcmData), track.numFrames, track.sampleRate, track.channels);
#else
    pendingMusicSample = AudioEngine::LoadSample(
        track.pcmData.data(), track.numFrames, track.sampleRate, track.channels);
#endif
    if (pendingMusicSample == AUDIO_SAMPLE_INVALID) {
        LOG("Sound: failed to load music sample (preload)");
        return false;
    }

    pendingMusicVolume = volume;
    LOG("Sound: preloaded music '%s' (%u frames, %u ch @ %u Hz, sample=%u)",
        fagPath, track.numFrames, track.channels, PSX_MUSIC_RATE, pendingMusicSample);
    return true;
}

// PC: start music that was already decoded by PreloadMusicTrack - no file
// IO or decode here, so it's safe to call from inside a fade-in.
bool Sound::StartPreloadedMusic() {
#if defined(RC_PLATFORM_PSP)
    return musicPlaying;
#endif

    if (pendingMusicSample == AUDIO_SAMPLE_INVALID) {
        return false;
    }

    StopMusic();
    musicSample = pendingMusicSample;
    pendingMusicSample = AUDIO_SAMPLE_INVALID;

    musicVolume = pendingMusicVolume;
    f32 playVol = musicMuted ? 0.0f : musicVolume;
    musicVoice = AUDIO_VOICE_INVALID;
    musicPlaying = AudioEngine::PlayMusicSample(musicSample, playVol, true);

    LOG("Sound: starting preloaded music (sample=%u, dedicated=%d)", musicSample, musicPlaying ? 1 : 0);
    return musicPlaying;
}

void Sound::StopMusic() {
    AudioEngine::StopMusic();

    if (musicVoice != AUDIO_VOICE_INVALID) {
        AudioEngine::StopVoice(musicVoice);
        musicVoice = AUDIO_VOICE_INVALID;
    }
    if (musicSample != AUDIO_SAMPLE_INVALID) {
        AudioEngine::UnloadSample(musicSample);
        musicSample = AUDIO_SAMPLE_INVALID;
    }
    musicPlaying = false;
    musicMuted = false;
}

void Sound::SetMusicVolume(f32 volume) {
    musicVolume = volume;
    AudioEngine::SetMusicVolume(musicMuted ? 0.0f : volume);
}

void Sound::SetEffectsVolume(f32 volume) {
    effectsVolume = volume;
}

void Sound::SetDialogVolume(f32 volume) {
    dialogVolume = volume;
}

void Sound::MuteMusic() {
    musicMuted = true;
    AudioEngine::FadeMusicVolume(0.0f, PSX_MUSIC_FADE_MS);
}

void Sound::UnmuteMusic() {
    musicMuted = false;
    AudioEngine::FadeMusicVolume(musicVolume, PSX_MUSIC_FADE_MS);
}

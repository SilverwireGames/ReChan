#include "snd/rsdworld.h"
#include "snd/sound.h"
#include "snd/sndmath.h"
#include "gen/config.h"
#include "gen/common.h"
#include "gen/display.h"
#include "gen/camera.h"
#include "gen/psxmath_helpers.h"
#include "ai/player.h"
#include "p3d/p3dmath.h"
#include "pc/audio.h"
#include <vector>
#include <mutex>
#include <algorithm>

// These are defined inline by p3d math headers; declare here to keep tooling resolution stable.
s32 rmSin16(s32 angle);
s32 rmMag3ff(s32 a1, s32 a2, s32 a3);

static constexpr s32 MIN_AUDIBLE_DIST = 0;
static constexpr s32 MAX_AUDIBLE_DIST = 10000;
static constexpr s32 STEREO_SEPARATION = 1024;

static f32 ModernSpatialMaxDistance(u32 extraRange) {
    return (f32)(MAX_AUDIBLE_DIST + (s32)extraRange);
}

static bool GetListenerState(LVector& outPos, s32& outYaw) {
    if (!g_display) {
        return false;
    }

    Camera* cam = g_display->GetCamera();
    if (!cam) {
        return false;
    }

    // PSX jcsSetListener receives &player->pos and &camera->camAngleY.
    // Using the camera position here can put a nearby voice outside the
    // audible range when the camera is pulled away from the player.
    outPos = Player::s_player ? Player::s_player->pos : cam->GetPosition();
    outYaw = cam->GetCamAngleY();
    return true;
}

static void UpdateListenerInAudioEngine() {
    LVector listenerPos = {};
    s32 listenerYaw = 0;
    if (GetListenerState(listenerPos, listenerYaw)) {
        AudioEngine::SetListener(listenerPos, listenerYaw);
    }
}

static std::vector<rsdPersistent*> g_persistentSounds;
static std::mutex g_persistentMutex;
static bool g_persistentMuted = false;

void rsdWorld::StopAllPersistentSounds() {
    std::lock_guard<std::mutex> lock(g_persistentMutex);
    g_persistentMuted = false;
    for (rsdPersistent* snd : g_persistentSounds) {
        if (!snd) {
            continue;
        }

        if (snd->voiceHandle) {
            AudioVoice v = static_cast<AudioVoice>(reinterpret_cast<uintptr_t>(snd->voiceHandle));
            AudioEngine::StopVoice(v);
            snd->voiceHandle = nullptr;
        }

        snd->posPtr = nullptr;
    }

    g_persistentSounds.clear();
}

// PSX: rsdPersistent::FadeOutAll (RSDBACH.CPP) - mute all persistent sounds in place.
void rsdWorld::MuteAllPersistentSounds() {
    std::lock_guard<std::mutex> lock(g_persistentMutex);
    g_persistentMuted = true;
    for (rsdPersistent* snd : g_persistentSounds) {
        if (snd) {
            snd->SetMuted(true);
        }
    }
}

// PSX: rsdPersistent::FadeInAll (RSDBACH.CPP) - restore all persistent sounds from mute.
void rsdWorld::UnmuteAllPersistentSounds() {
    std::lock_guard<std::mutex> lock(g_persistentMutex);
    g_persistentMuted = false;
    for (rsdPersistent* snd : g_persistentSounds) {
        if (snd) {
            snd->SetMuted(false);
        }
    }
}

static void GetObjectVolumesPsx(u16 baseVol, const LVector* objPos, u16& outVolL, u16& outVolR, u32 extraRange) {
    LVector listener = {};
    s32 yaw = 0;
    if (!objPos || !GetListenerState(listener, yaw)
        || (listener.x == objPos->x && listener.y == objPos->y && listener.z == objPos->z)) {
        outVolL = baseVol;
        outVolR = baseVol;
        return;
    }

    const s32 sepX = PsxMulShift16Signed(rmSin16(yaw - 0x4000), STEREO_SEPARATION);
    const s32 sepZ = PsxMulShift16Signed(rmSin16(yaw), STEREO_SEPARATION);

    const s32 lx = listener.x - sepX;
    const s32 lz = listener.z - sepZ;
    const s32 rx = listener.x + sepX;
    const s32 rz = listener.z + sepZ;

    const u32 distL = (u32)rmMag3ff(lx - objPos->x, listener.y - objPos->y, lz - objPos->z);
    const u32 distR = (u32)rmMag3ff(rx - objPos->x, listener.y - objPos->y, rz - objPos->z);
    const u32 maxDist = (u32)(MAX_AUDIBLE_DIST + (s32)extraRange);

    auto attenuate = [&](u32 dist) -> u16 {
        if (dist >= (u32)MIN_AUDIBLE_DIST) {
            if (dist > maxDist) {
                return 0;
            }

            const u32 span = maxDist - (u32)MIN_AUDIBLE_DIST;
            if (span == 0) {
                return 0;
            }

            return (u16)(((maxDist - dist) * (u32)baseVol) / span);
        }

        return baseVol;
    };

    outVolL = attenuate(distL);
    outVolR = attenuate(distR);
}

void rsdWorld::ComputeObjectVolumes(u16 baseVol, const LVector* objPos, u16& outVolL, u16& outVolR, u32 extraRange) {
    GetObjectVolumesPsx(baseVol, objPos, outVolL, outVolR, extraRange);
}

// PC: resolve rsd sample ID to the active WAX bank.
// PSX loads one bank at a time; rsd sample IDs (0-70) index directly
// into the single active bank's 71-entry descriptor table.
static bool RsdSampleToWax(u32 rsdSampleId, u32& outBank, u32& outSample) {
    if (!g_sound || g_sound->activeSfxBank < 0) {
        return false;
    }

    u32 bank = (u32)g_sound->activeSfxBank;
    if (bank >= MAX_WAX_BANKS) {
        return false;
    }
    if (rsdSampleId >= g_sound->GetBankSampleCount(bank)) {
        return false;
    }

    outBank = bank;
    outSample = rsdSampleId;
    return true;
}

// PSX: PlayTransient__8rsdWorldlPC10tagLVectorUsUsUsUl (0x80080234)
// On PSX: GetObjectVolumes -> rsdGetVoice -> rsdSetVolume -> rsdSetPitch -> rsdVoiceOn
// On PC: map sample, convert params, play via AudioEngine
static bool g_isDialogTransient = false;
void rsdWorld::SetDialogTransient(bool isDialog) { g_isDialogTransient = isDialog; }

s32 rsdWorld::PlayTransientPositional(u32 sampleId, void* posPtr, u16 volume, s16 pitch, u16 pan, u32 flags) {
    MARKFUNCTION(0x80080234);

    const bool isDialog = g_isDialogTransient;
    g_isDialogTransient = false;

    if (volume == 0 || !g_sound) {
        return 0;
    }

    u32 bank, sample;
    if (!RsdSampleToWax(sampleId, bank, sample)) {
        return 0;
    }

    const LVector* objPos = static_cast<const LVector*>(posPtr);
    u16 volL = 0;
    u16 volR = 0;
    GetObjectVolumesPsx(volume, objPos, volL, volR, flags);

    if (volL == 0 && volR == 0) {
        return 0;
    }

    f32 fVolL = PsxVolToFloat(volL);
    f32 fVolR = PsxVolToFloat(volR);
    f32 chanVol = isDialog ? g_sound->dialogVolume : g_sound->effectsVolume;
    f32 vol = (fVolL + fVolR) * 0.5f * chanVol;
    f32 pcPan = 0.0f;
    if (fVolL + fVolR > 0.0f) {
        pcPan = (fVolR - fVolL) / (fVolL + fVolR);
    }

    f32 pitchF = PsxPitchToFloat(pitch);

    AudioSample s = g_sound->GetBankSample(bank, sample);
    if (s == AUDIO_SAMPLE_INVALID) {
        return 0;
    }

    AudioVoice v = AUDIO_VOICE_INVALID;
#if MODERN_SPATIAL_AUDIO
    if (objPos != nullptr) {
        const f32 vol = PsxVolToFloat(volume) * chanVol;
        v = AudioEngine::PlaySample3D(s, *objPos, vol, false, true, 0.0f, ModernSpatialMaxDistance(flags));
    }
    else
#endif
    v = AudioEngine::PlaySample(s, vol, pcPan, false);

    if (v != AUDIO_VOICE_INVALID && pitchF != 1.0f) {
        AudioEngine::SetVoicePitch(v, pitchF);
    }

    return 0;
}

// PSX: PlayTransient__8rsdWorldlUsUsUsUi (0x80080334)
// Non-positional: separate L/R volumes, no 3D spatialization
s32 rsdWorld::PlayTransientNonPositional(u32 sampleId, u16 volL, u16 volR, s16 pitch, u16 pan) {
    MARKFUNCTION(0x80080334);

    if ((volL == 0 && volR == 0) || !g_sound) {
        return 0;
    }

    u32 bank, sample;
    if (!RsdSampleToWax(sampleId, bank, sample)) {
        return 0;
    }

    // PC: approximate L/R by averaging for volume and computing pan
    f32 fVolL = PsxVolToFloat(volL);
    f32 fVolR = PsxVolToFloat(volR);
    f32 vol = (fVolL + fVolR) * 0.5f * g_sound->effectsVolume;
    f32 pcPan = 0.0f;
    if (fVolL + fVolR > 0.0f) {
        pcPan = (fVolR - fVolL) / (fVolL + fVolR);
    }
    f32 pitchF = PsxPitchToFloat(pitch);

    AudioSample s = g_sound->GetBankSample(bank, sample);
    if (s == AUDIO_SAMPLE_INVALID) {
        return 0;
    }

    AudioVoice v = AudioEngine::PlaySample(s, vol, pcPan, false);
    if (v != AUDIO_VOICE_INVALID && pitchF != 1.0f) {
        AudioEngine::SetVoicePitch(v, pitchF);
    }

    return 0;
}

// rsdPersistent - persistent looping sound

// PSX: _13rsdPersistentlPC10tagLVectorUlUsUsUl (0x80080508)
rsdPersistent::rsdPersistent(u32 sampleId_, void* posPtr, u8 reverb, u16 volume_, s16 pitch, u16 flags) {
    sampleId = sampleId_;
    volume = volume_;
    this->posPtr = static_cast<const LVector*>(posPtr);
    spatialFlags = flags;
    voiceHandle = nullptr;

    u32 bank, sample;
    if (!RsdSampleToWax(sampleId_, bank, sample)) {
        return;
    }

    AudioSample s = g_sound->GetBankSample(bank, sample);
    if (s == AUDIO_SAMPLE_INVALID) {
        return;
    }

    f32 vol = 0.0f;
    if (this->posPtr != nullptr) {
#if MODERN_SPATIAL_AUDIO
        vol = PsxVolToFloat(volume_);
#else
        u16 volL = 0;
        u16 volR = 0;
        GetObjectVolumesPsx(volume_, this->posPtr, volL, volR, flags);
        vol = (PsxVolToFloat(volL) + PsxVolToFloat(volR)) * 0.5f;
#endif
    }
    else {
        vol = PsxVolToFloat(volume_);
    }

    vol *= g_sound->effectsVolume;
    muted = g_persistentMuted;
    if (muted) {
        vol = 0.0f;
    }
    f32 pitchF = PsxPitchToFloat(pitch);

    AudioVoice v = AUDIO_VOICE_INVALID;
    f32 pcPan = 0.0f;
#if !MODERN_SPATIAL_AUDIO
    if (this->posPtr != nullptr) {
        u16 volL = 0;
        u16 volR = 0;
        GetObjectVolumesPsx(volume_, this->posPtr, volL, volR, flags);
        const f32 fVolL = PsxVolToFloat(volL);
        const f32 fVolR = PsxVolToFloat(volR);
        if (fVolL + fVolR > 0.0f) {
            pcPan = (fVolR - fVolL) / (fVolL + fVolR);
        }
    }
#endif

#if MODERN_SPATIAL_AUDIO
    if (this->posPtr != nullptr) {
        v = AudioEngine::PlaySample3D(s, *this->posPtr, vol, true, true, 0.0f, ModernSpatialMaxDistance(flags));
    }
    else
#endif
    v = AudioEngine::PlaySample(s, vol, pcPan, true);

    if (v != AUDIO_VOICE_INVALID) {
        if (pitchF != 1.0f) {
            AudioEngine::SetVoicePitch(v, pitchF);
        }
        voiceHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(v));

        std::lock_guard<std::mutex> lock(g_persistentMutex);
        g_persistentSounds.push_back(this);
    }
}

rsdPersistent::~rsdPersistent() {
    End();
}

void rsdPersistent::End() {
    if (voiceHandle) {
        AudioVoice v = static_cast<AudioVoice>(reinterpret_cast<uintptr_t>(voiceHandle));
        AudioEngine::StopVoice(v);
        voiceHandle = nullptr;
    }

    posPtr = nullptr;

    std::lock_guard<std::mutex> lock(g_persistentMutex);
    auto it = std::remove(g_persistentSounds.begin(), g_persistentSounds.end(), this);
    g_persistentSounds.erase(it, g_persistentSounds.end());
}

// PSX: ObjectExists__13rsdPersistentP13rsdPersistent
bool rsdPersistent::ObjectExists(rsdPersistent* obj) {
    if (!obj) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_persistentMutex);
        if (std::find(g_persistentSounds.begin(), g_persistentSounds.end(), obj) == g_persistentSounds.end()) {
            return false;
        }
    }

    if (!obj->voiceHandle) {
        return false;
    }

    AudioVoice v = static_cast<AudioVoice>(reinterpret_cast<uintptr_t>(obj->voiceHandle));
    return AudioEngine::IsVoicePlaying(v);
}

void rsdPersistent::SetVolume(u16 psxVol) {
    volume = psxVol;
    UpdateSpatial();
}

void rsdPersistent::SetMuted(bool muted_) {
    muted = muted_;
    UpdateSpatial();
}

void rsdPersistent::UpdateSpatial() {
    if (voiceHandle) {
        AudioVoice v = static_cast<AudioVoice>(reinterpret_cast<uintptr_t>(voiceHandle));

        const u16 effectiveVolume = muted ? 0 : volume;
#if MODERN_SPATIAL_AUDIO
        f32 vol = PsxVolToFloat(effectiveVolume);
        if (g_sound) {
            vol *= g_sound->effectsVolume;
        }

        if (posPtr != nullptr) {
            AudioEngine::SetVoicePosition(v, *posPtr);
            AudioEngine::SetVoiceDistanceRange(v, 0.0f, ModernSpatialMaxDistance(spatialFlags));
        }

        AudioEngine::SetVoiceVolume(v, vol);
#else
        u16 volL = effectiveVolume;
        u16 volR = effectiveVolume;
        if (posPtr != nullptr) {
            GetObjectVolumesPsx(effectiveVolume, posPtr, volL, volR, spatialFlags);
        }

        const f32 fVolL = PsxVolToFloat(volL);
        const f32 fVolR = PsxVolToFloat(volR);
        f32 vol = (fVolL + fVolR) * 0.5f;
        f32 pcPan = 0.0f;
        if (fVolL + fVolR > 0.0f) {
            pcPan = (fVolR - fVolL) / (fVolL + fVolR);
        }

        if (g_sound) {
            vol *= g_sound->effectsVolume;
        }
        AudioEngine::SetVoiceVolume(v, vol);
        AudioEngine::SetVoicePan(v, pcPan);
#endif
    }
}

void rsdWorld::UpdateSpatialAudioState() {
    UpdateListenerInAudioEngine();

    std::lock_guard<std::mutex> lock(g_persistentMutex);
    for (rsdPersistent* snd : g_persistentSounds) {
        if (snd != nullptr) {
            snd->UpdateSpatial();
        }
    }
}

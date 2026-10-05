#include "snd/ambience.h"

#include "gen/common.h"
#include "gen/time.h"
#include "snd/sound.h"
#include "snd/adpcm.h"
#include "xclib/xcfile.h"
#include "p3d/p3dmath.h"

#include <cstdio>

// PSX ambience clips are SPU ADPCM authored for the default SFX playback rate.
static constexpr u32 AMBIENCE_RATE = 11025;

// PSX ambience master volume is only 15% of the SFX slider (jcs sound config:
// ambience = 15 * vol / 100, vs music 80 / dialog 83; SetVoiceVol__11rsdAmbiance
// then attenuates further). We fold that master scale in here. Tunable by ear.
static constexpr f32 AMBIENCE_MASTER_SCALE = 0.15f;

static inline u32 Rd32(const u8* p, u32 off) {
    return (u32)p[off] | ((u32)p[off + 1] << 8) | ((u32)p[off + 2] << 16) | ((u32)p[off + 3] << 24);
}

Ambiance::~Ambiance() {
    Close();
}

// PSX: Open__11rsdAmbiancePCclll (0x80080E0C)
s32 Ambiance::Open(const char* baseName) {
    MARKFUNCTION(0x80080E0C);

    if (opened) return 100;
    if (!baseName || !g_sound) return -1;

    char path[128];

    // CLP: decode every clip to an AudioSample
    // Layout: u32 count, u32 start[count], u32 end[count], then 16-byte SPU
    // ADPCM blocks. Clip i data = file[start[i] .. end[i]).
    snprintf(path, sizeof(path), "SOUND/AMBIENCE/%s.CLP", baseName);
    u8* clp = nullptr;
    u32 clpSize = 0;
    if (!xcReadFileLow(path, &clp, &clpSize) || !clp || clpSize < 4) {
        if (clp) delete[] clp;
        LOG("[Ambiance] failed to open %s", path);
        return -1;
    }

    u32 count = Rd32(clp, 0);
    if (4u + 8u * count > clpSize) {
        delete[] clp;
        LOG("[Ambiance] %s: bad clip header (count=%u)", path, count);
        return -1;
    }

    clips.clear();
    clips.reserve(count);
    for (u32 i = 0; i < count; i++) {
        u32 start = Rd32(clp, 4 + 4 * i);
        u32 end = Rd32(clp, 4 + 4 * count + 4 * i);
        AudioSample sample = AUDIO_SAMPLE_INVALID;
        if (end > start && end <= clpSize) {
            u32 len = (end - start) & ~15u; // whole 16-byte ADPCM blocks
            std::vector<s16> pcm = SpuAdpcm::Decode(clp + start, len, false);
            if (!pcm.empty()) {
                sample = AudioEngine::LoadSample(pcm.data(), (u32)pcm.size(), AMBIENCE_RATE, 1);
            }
        }
        clips.push_back(sample);
    }
    delete[] clp;

    // AMS: parse spaces
    // 6-word header [defaultSpace, w1, w2, spaceCount, totalBg, totalFg], then
    // per space a 9-word header + bgCount 1-word entries + fgCount 3-word entries.
    snprintf(path, sizeof(path), "SOUND/AMBIENCE/%s.AMS", baseName);
    u8* ams = nullptr;
    u32 amsSize = 0;
    if (!xcReadFileLow(path, &ams, &amsSize) || !ams || amsSize < 24) {
        if (ams) delete[] ams;
        for (AudioSample s : clips) if (s != AUDIO_SAMPLE_INVALID) AudioEngine::UnloadSample(s);
        clips.clear();
        LOG("[Ambiance] failed to open %s", path);
        return -1;
    }

    defaultSpace = (s32)Rd32(ams, 0);
    u32 spaceCount = Rd32(ams, 12);
    spaces.clear();
    spaces.reserve(spaceCount);

    u32 off = 24; // after 6-word header
    for (u32 s = 0; s < spaceCount && off + 36 <= amsSize; s++) {
        Space sp;
        sp.reverbMode = (s32)Rd32(ams, off + 0);
        sp.reverbDepth = (s32)Rd32(ams, off + 4);
        u32 bgCount = Rd32(ams, off + 8);
        u32 fgCount = Rd32(ams, off + 12);
        sp.volPct = (s32)Rd32(ams, off + 16);
        // off+20 pan and off+24..32 timing params are unused in v1 (bed is
        // played centered/continuous; see ambience.h).
        off += 36; // 9-word space header

        for (u32 i = 0; i < bgCount && off + 4 <= amsSize; i++) {
            sp.bg.push_back(Rd32(ams, off));
            off += 4;
        }
        for (u32 i = 0; i < fgCount && off + 12 <= amsSize; i++) {
            sp.fg.push_back(Rd32(ams, off)); // 3-word entry: [clipIndex, param, param]
            off += 12;
        }
        spaces.push_back(std::move(sp));
    }
    delete[] ams;

    if (spaces.empty()) {
        for (AudioSample s : clips) if (s != AUDIO_SAMPLE_INVALID) AudioEngine::UnloadSample(s);
        clips.clear();
        return -1;
    }

    opened = true;
    curSpace = 0;
    LOG("[Ambiance] Open %s: %u clips, %u spaces", baseName, count, (u32)spaces.size());
    return 100;
}

// PSX: Close__11rsdAmbiance (0x8008132C)
void Ambiance::Close() {
    MARKFUNCTION(0x8008132C);

    if (!opened) return;
    Stop();
    for (AudioSample s : clips) if (s != AUDIO_SAMPLE_INVALID) AudioEngine::UnloadSample(s);
    clips.clear();
    spaces.clear();
    opened = false;
}

// PSX: Start__11rsdAmbianceUl (0x80081464)
s32 Ambiance::Start(s32 space) {
    MARKFUNCTION(0x80081464);

    if (!opened || running) return -1;

    curSpace = (space == -1) ? defaultSpace : space;
    if (curSpace < 0 || curSpace >= (s32)spaces.size()) curSpace = 0;

    prevSpace = -1;
    mix = 1.0f;
    crossfading = false;
    globalFade = 1.0f;
    globalFadeStep = 0.0f;
    globalFadeDir = 0;
    bgCur = Layer{};
    fgCur = Layer{};
    bgPrev = Layer{};
    fgPrev = Layer{};
    running = true;

    LOG("[Ambiance] Start (space=%d)", curSpace);
    return 100;
}

// PSX: Stop__11rsdAmbiance (0x8008158C)
s32 Ambiance::Stop() {
    MARKFUNCTION(0x8008158C);

    if (!running) return 100;
    StopVoices();
    running = false;
    return 100;
}

void Ambiance::StopLayer(Layer& layer) {
    if (layer.voice != AUDIO_VOICE_INVALID) {
        AudioEngine::StopVoice(layer.voice);
        layer.voice = AUDIO_VOICE_INVALID;
    }
    layer.lastClip = -1;
}

void Ambiance::StopVoices() {
    StopLayer(bgCur);
    StopLayer(fgCur);
    StopLayer(bgPrev);
    StopLayer(fgPrev);
}

// PSX: SetSpace__11rsdAmbianceUl (0x800816C0)
void Ambiance::SetSpace(s32 space) {
    MARKFUNCTION(0x800816C0);

    if (!running || space < 0 || space >= (s32)spaces.size()) return;
    if (space == curSpace && !crossfading) return;

    // Promote the active bed to "outgoing" (it keeps playing, fading out) and
    // start a fresh bed for the new space that fades in over the top.
    StopLayer(bgPrev);
    StopLayer(fgPrev);
    bgPrev = bgCur;
    fgPrev = fgCur;
    bgCur = Layer{};
    fgCur = Layer{};
    prevSpace = curSpace;
    curSpace = space;
    mix = 0.0f;
    crossfading = true;
}

// PSX: SetCrossFadeDuration__11rsdAmbiancel (0x80081828)
void Ambiance::SetCrossFadeDuration(s32 frames) {
    MARKFUNCTION(0x80081828);

    if (frames < 1) frames = 1;
    crossFadeStep = 1.0f / (f32)frames;
}

// PSX: FadeIn__11rsdAmbianceUlbUl (0x800818C8)
void Ambiance::FadeIn(u32 durationMs) {
    MARKFUNCTION(0x800818C8);

    if (!opened) return;
    if (!running) {
        Start(-1);
        globalFade = 0.0f; // start silent so the bed fades up from nothing
    }

    u32 frames = durationMs * 60u / 1000u;
    if (frames < 1) frames = 1;
    globalFadeStep = 1.0f / (f32)frames;
    globalFadeDir = 1;
}

// PSX: FadeOut__11rsdAmbianceUlb (0x800819C0)
void Ambiance::FadeOut(u32 durationMs) {
    MARKFUNCTION(0x800819C0);

    if (!running) return;

    u32 frames = durationMs * 60u / 1000u;
    if (frames < 1) frames = 1;
    globalFadeStep = 1.0f / (f32)frames;
    globalFadeDir = -1;
    // Stay running at silence so a later FadeIn resumes without a reload.
}

f32 Ambiance::MasterGain() const {
    f32 v = globalFade * AMBIENCE_MASTER_SCALE;
    if (g_sound) v *= g_sound->effectsVolume;
    return v;
}

void Ambiance::ServiceLayer(Layer& layer, const std::vector<u32>& clipList, f32 gain, bool retrigger) {
    if (clipList.empty()) return;

    if (layer.voice == AUDIO_VOICE_INVALID || !AudioEngine::IsVoicePlaying(layer.voice)) {
        // An outgoing (crossfading-out) bed does not chain a new clip - let it
        // fall silent once its current clip ends.
        if (!retrigger) {
            layer.voice = AUDIO_VOICE_INVALID;
            return;
        }

        // Pick next clip: random, avoid an immediate repeat when there's a
        // choice (PSX GetForeground/BackgroundSample re-roll once).
        s32 idx = (s32)rmRangedRandom((s32)clipList.size());
        if (clipList.size() > 1 && (s32)clipList[idx] == layer.lastClip) {
            idx = (s32)rmRangedRandom((s32)clipList.size());
        }

        u32 clipIndex = clipList[idx];
        layer.lastClip = (s32)clipIndex;
        if (clipIndex < clips.size() && clips[clipIndex] != AUDIO_SAMPLE_INVALID) {
            layer.voice = AudioEngine::PlaySample(clips[clipIndex], gain, 0.0f, false);
        }
        else {
            layer.voice = AUDIO_VOICE_INVALID;
        }
    }
    else {
        AudioEngine::SetVoiceVolume(layer.voice, gain);
    }
}

// PSX: AmbianceTask__11rsdAmbiance (0x80081EB0)
void Ambiance::Update() {
    MARKFUNCTION(0x80081EB0);

    if (!running) return;

    // The fade steps are sized for 60 calls per second, but this is called once per
    // rendered frame. Scale by real elapsed time so fades last what was asked for.
    static f64 s_lastUpdateTime = 0.0;
    const f64 now = Time::GetTimeInSeconds();
    f64 elapsed = now - s_lastUpdateTime;
    s_lastUpdateTime = now;
    if (elapsed < 0.0 || elapsed > 0.1) {
        elapsed = 1.0 / 60.0;
    }
    const f32 stepScale = (f32)(elapsed * 60.0);

    // Global fade gate (level enter/exit, engine mute).
    if (globalFadeDir != 0) {
        globalFade += globalFadeStep * stepScale * (f32)globalFadeDir;
        if (globalFade <= 0.0f) {
            globalFade = 0.0f;
            globalFadeDir = 0;
        }
        else if (globalFade >= 1.0f) {
            globalFade = 1.0f;
            globalFadeDir = 0;
        }
    }

    // Space crossfade: advance mix; the outgoing bed is stopped once fully out.
    if (crossfading) {
        mix += crossFadeStep * stepScale;
        if (mix >= 1.0f) {
            mix = 1.0f;
            crossfading = false;
            StopLayer(bgPrev);
            StopLayer(fgPrev);
            prevSpace = -1;
        }
    }

    const f32 base = MasterGain();

    const Space& cs = spaces[curSpace];
    const f32 curGain = base * ((f32)cs.volPct / 100.0f) * mix;
    ServiceLayer(bgCur, cs.bg, curGain, true);
    ServiceLayer(fgCur, cs.fg, curGain, true);

    if (crossfading && prevSpace >= 0 && prevSpace < (s32)spaces.size()) {
        const Space& ps = spaces[prevSpace];
        const f32 prevGain = base * ((f32)ps.volPct / 100.0f) * (1.0f - mix);
        ServiceLayer(bgPrev, ps.bg, prevGain, false);
        ServiceLayer(fgPrev, ps.fg, prevGain, false);
    }
}

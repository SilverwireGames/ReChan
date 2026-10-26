#include "gen/common.h"
#include "pc/audio.h"
#include "gen/config.h"
#include "p3d/fileio.h"
#include "miniaudio.h"
#include <vector>
#include <mutex>
#include <cmath>
#include <chrono>

#if defined(RC_PLATFORM_SWITCH)
// Switch output goes through libnx audout (48kHz, 2ch, s16) instead of
// miniaudio (which has no Horizon OS backend). The whole mixing/voice engine
// -- audioCallback and everything below -- is reused unchanged; only the
// device output layer differs. See the RC_PLATFORM_SWITCH branches in
// Init/Shutdown and SwitchAudioThreadFunc.
#include <switch.h>
#include <malloc.h>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#endif

// Internal sample storage
struct InternalSample {
    s16* data = nullptr;
    u32 numFrames = 0;
    u32 sampleRate = 0;
    u32 channels = 0;
};

// Internal voice
struct InternalVoice {
    AudioSample sample = AUDIO_SAMPLE_INVALID;
    f64 positionF = 0.0; // fractional frame position (for sample rate conversion)
    f32 volume = 1.0f;
    f32 fadeStartVolume = 1.0f;
    f32 fadeTargetVolume = 1.0f;
    u32 fadeFramesTotal = 0;
    u32 fadeFramesRemaining = 0;
    f32 pan = 0.0f;
    f32 pitch = 1.0f;
    LVector worldPos = {};
    f32 minDistance = 0.0f;
    f32 maxDistance = 10000.0f;
    bool spatial = false;
    bool applyDistanceAttenuation = true;
    bool loop = false;
    bool active = false;
    f64 startTimeSec = 0.0;
    bool overdueLogged = false;
    u32 generation = 0;
    u8 priority = 0;      // rsdAllocVoice priority; higher resists stealing
    bool locked = false;  // reserved voice (rsdLockVoice): never allocated/stolen by the pool
};

struct ListenerState {
    LVector pos = {};
    s32 yaw16 = 0;
    bool valid = false;
};

struct Biquad {
    f32 b0 = 1.0f;
    f32 b1 = 0.0f;
    f32 b2 = 0.0f;
    f32 a1 = 0.0f;
    f32 a2 = 0.0f;
};

struct BiquadState {
    f32 z1 = 0.0f;
    f32 z2 = 0.0f;
};

// Engine state
static constexpr u32 MAX_SAMPLES = 4096;
static constexpr u32 MAX_VOICES = 32;
static constexpr u32 ENGINE_SAMPLE_RATE = 44100; // fallback, actual rate from device
static constexpr u32 DEFAULT_MIX_CHANNELS = 2;
static constexpr u32 MUSIC_CHANNELS = 2;

static ma_device g_device;
static bool g_initialized = false;
static f32 g_masterVolume = 0.5f;
static u32 g_deviceSampleRate = ENGINE_SAMPLE_RATE; // actual device sample rate
static u32 g_outputChannels = DEFAULT_MIX_CHANNELS;
static bool g_outputMono = false;
static constexpr bool g_smoothResampling = true;
static constexpr s32 g_bassBoostAmount = 50;
static constexpr s32 g_trebleBoostAmount = 5;
static BiquadState g_bassShelfState[8] = {};
static BiquadState g_trebleShelfState[8] = {};
static f32 g_exciterLowpassState[8] = {};
static bool g_surroundEnabled = true;

static InternalSample g_samples[MAX_SAMPLES];
static u32 g_nextSampleId = 1;

static InternalVoice g_voices[MAX_VOICES];
static u32 g_nextVoiceId = 1;
static std::mutex g_voiceMutex;

// Public AudioVoice handles pack (generation << kVoiceSlotBits) | (slot + 1) so a
// handle can only ever resolve to the exact allocation it was issued for - see
// InternalVoice::generation. Must hold g_voiceMutex before calling.
static constexpr u32 kVoiceSlotBits = 8; // headroom above MAX_VOICES (32)
static constexpr u32 kVoiceSlotMask = (1u << kVoiceSlotBits) - 1u;

static InternalVoice* ResolveVoice(AudioVoice voice, u32* outSlot = nullptr) {
    if (voice == AUDIO_VOICE_INVALID) return nullptr;
    const u32 slot = (voice & kVoiceSlotMask) - 1u;
    const u32 generation = voice >> kVoiceSlotBits;
    if (slot >= MAX_VOICES) return nullptr;
    InternalVoice& v = g_voices[slot];
    if (!v.active || v.generation != generation) return nullptr;
    if (outSlot) *outSlot = slot;
    return &v;
}

// PSX: rsdAllocVoice__FUc (RSDBACH.CPP). Free-unlocked first, else steal first
// unlocked voice with priority < requested. MAX_VOICES on failure. Holds g_voiceMutex.
static u32 AllocVoiceSlot(u8 priority) {
    for (u32 i = 0; i < MAX_VOICES; i++) {
        if (!g_voices[i].active && !g_voices[i].locked) return i;
    }
    for (u32 i = 0; i < MAX_VOICES; i++) {
        if (!g_voices[i].locked && g_voices[i].priority < priority) return i;
    }
    return MAX_VOICES;
}

static ListenerState g_listener;
static std::mutex g_listenerMutex;

// Music state
static ma_decoder g_musicDecoder;
static bool g_musicActive = false;
static AudioSample g_musicSample = AUDIO_SAMPLE_INVALID;
static bool g_musicSampleActive = false;
static bool g_musicLoop = false;
static f64 g_musicPositionF = 0.0;
static f32 g_musicVolume = 1.0f;
static f32 g_musicFadeStartVolume = 1.0f;
static f32 g_musicFadeTargetVolume = 1.0f;
static u32 g_musicFadeFramesTotal = 0;
static u32 g_musicFadeFramesRemaining = 0;
static std::mutex g_musicMutex;

static f64 AudioNowSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point s_start = clock::now();
    return std::chrono::duration<f64>(clock::now() - s_start).count();
}

static inline f32 clampf(f32 v, f32 lo, f32 hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline f32 Pcm16ToFloat(s16 sample) {
    return (f32)sample / 32768.0f;
}

static inline s32 clampi(s32 v, s32 lo, s32 hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline f32 dbToLinear(f32 db) {
    return std::pow(10.0f, db / 20.0f);
}

static inline f32 equalPowerLeft(f32 pan) {
    const f32 angle = (clampf(pan, -1.0f, 1.0f) + 1.0f) * (3.14159265358979323846f * 0.25f);
    return std::cos(angle);
}

static inline f32 equalPowerRight(f32 pan) {
    const f32 angle = (clampf(pan, -1.0f, 1.0f) + 1.0f) * (3.14159265358979323846f * 0.25f);
    return std::sin(angle);
}

static inline f32 ProcessBiquad(const Biquad& filter, BiquadState& state, f32 sample) {
    const f32 out = filter.b0 * sample + state.z1;
    state.z1 = filter.b1 * sample - filter.a1 * out + state.z2;
    state.z2 = filter.b2 * sample - filter.a2 * out;
    return out;
}

static Biquad MakeLowShelf(f32 sampleRate, f32 frequency, f32 gainDb) {
    const f32 A = dbToLinear(gainDb * 0.5f);
    const f32 w0 = 2.0f * 3.14159265358979323846f * frequency / sampleRate;
    const f32 cosW0 = std::cos(w0);
    const f32 sinW0 = std::sin(w0);
    const f32 sqrtA = std::sqrt(A);
    const f32 alpha = sinW0 * 0.70710678118f;
    const f32 twoSqrtAAlpha = 2.0f * sqrtA * alpha;

    const f32 b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAAlpha);
    const f32 b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0);
    const f32 b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAAlpha);
    const f32 a0 = (A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAAlpha;
    const f32 a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0);
    const f32 a2 = (A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAAlpha;

    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

static Biquad MakeHighShelf(f32 sampleRate, f32 frequency, f32 gainDb) {
    const f32 A = dbToLinear(gainDb * 0.5f);
    const f32 w0 = 2.0f * 3.14159265358979323846f * frequency / sampleRate;
    const f32 cosW0 = std::cos(w0);
    const f32 sinW0 = std::sin(w0);
    const f32 sqrtA = std::sqrt(A);
    const f32 alpha = sinW0 * 0.70710678118f;
    const f32 twoSqrtAAlpha = 2.0f * sqrtA * alpha;

    const f32 b0 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAAlpha);
    const f32 b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0);
    const f32 b2 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAAlpha);
    const f32 a0 = (A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAAlpha;
    const f32 a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0);
    const f32 a2 = (A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAAlpha;

    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

static inline f32 ReadSampleChannel(const InternalSample& smp, u32 frame, u32 channel) {
    if (frame >= smp.numFrames || smp.channels == 0) {
        return 0.0f;
    }

    if (channel >= smp.channels) {
        channel = smp.channels - 1;
    }

    return Pcm16ToFloat(smp.data[frame * smp.channels + channel]);
}

static inline f32 ReadSampleChannelNearest(const InternalSample& smp, f64 frameF, u32 channel, bool loop) {
    if (!smp.data || smp.numFrames == 0 || smp.channels == 0) {
        return 0.0f;
    }

    u32 frame = (u32)frameF;
    if (frame >= smp.numFrames) {
        frame = loop ? 0 : (smp.numFrames - 1);
    }
    return ReadSampleChannel(smp, frame, channel);
}

static inline u32 ResolveSampleFrame(const InternalSample& smp, s32 frame, bool loop) {
    if (loop) {
        const s32 count = (s32)smp.numFrames;
        frame %= count;
        if (frame < 0) {
            frame += count;
        }
        return (u32)frame;
    }

    if (frame < 0) {
        return 0;
    }
    if ((u32)frame >= smp.numFrames) {
        return smp.numFrames - 1;
    }
    return (u32)frame;
}

static inline f32 CubicInterpolate(f32 s0, f32 s1, f32 s2, f32 s3, f32 t) {
    const f32 a0 = -0.5f * s0 + 1.5f * s1 - 1.5f * s2 + 0.5f * s3;
    const f32 a1 = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
    const f32 a2 = -0.5f * s0 + 0.5f * s2;
    return ((a0 * t + a1) * t + a2) * t + s1;
}

static inline f32 ReadSampleChannelInterpolated(const InternalSample& smp, f64 frameF, u32 channel, bool loop) {
    if (!smp.data || smp.numFrames == 0 || smp.channels == 0) {
        return 0.0f;
    }

    s32 frame1 = (s32)frameF;
    f32 frac = (f32)(frameF - (f64)frame1);
    if (frame1 < 0) {
        frame1 = 0;
        frac = 0.0f;
    }
    if ((u32)frame1 >= smp.numFrames) {
        frame1 = (s32)smp.numFrames - 1;
        frac = 0.0f;
    }

    const f32 s0 = ReadSampleChannel(smp, ResolveSampleFrame(smp, frame1 - 1, loop), channel);
    const f32 s1 = ReadSampleChannel(smp, ResolveSampleFrame(smp, frame1, loop), channel);
    const f32 s2 = ReadSampleChannel(smp, ResolveSampleFrame(smp, frame1 + 1, loop), channel);
    const f32 s3 = ReadSampleChannel(smp, ResolveSampleFrame(smp, frame1 + 2, loop), channel);
    return clampf(CubicInterpolate(s0, s1, s2, s3, frac), -1.0f, 1.0f);
}

static inline f32 ReadSampleMonoInterpolated(const InternalSample& smp, f64 frameF, bool loop) {
    if (smp.channels == 1) {
        return ReadSampleChannelInterpolated(smp, frameF, 0, loop);
    }

    f32 sum = 0.0f;
    for (u32 ch = 0; ch < smp.channels; ch++) {
        sum += ReadSampleChannelInterpolated(smp, frameF, ch, loop);
    }
    return sum / (f32)smp.channels;
}

static inline f32 ReadSampleMono(const InternalSample& smp, f64 frameF, bool loop) {
    if (g_smoothResampling) {
        return ReadSampleMonoInterpolated(smp, frameF, loop);
    }

    if (smp.channels == 1) {
        return ReadSampleChannelNearest(smp, frameF, 0, loop);
    }

    f32 sum = 0.0f;
    for (u32 ch = 0; ch < smp.channels; ch++) {
        sum += ReadSampleChannelNearest(smp, frameF, ch, loop);
    }
    return sum / (f32)smp.channels;
}

static inline f32 ReadSampleStereoChannel(const InternalSample& smp, f64 frameF, u32 channel, bool loop) {
    return g_smoothResampling
        ? ReadSampleChannelInterpolated(smp, frameF, channel, loop)
        : ReadSampleChannelNearest(smp, frameF, channel, loop);
}

static void ApplyMastering(f32* out, u32 frameCount, u32 outChannels) {
    if ((g_bassBoostAmount <= 0 && g_trebleBoostAmount <= 0) || outChannels == 0) {
        return;
    }

    const f32 bassDb = ((f32)g_bassBoostAmount / 100.0f) * 12.0f;
    const f32 trebleDb = ((f32)g_trebleBoostAmount / 100.0f) * 10.0f;
    const f32 exciterAmount = ((f32)g_trebleBoostAmount / 100.0f) * 0.18f;
    const Biquad bass = MakeLowShelf((f32)g_deviceSampleRate, 135.0f, bassDb);
    const Biquad treble = MakeHighShelf((f32)g_deviceSampleRate, 3200.0f, trebleDb);
    const f32 exciterCutoffHz = 2800.0f;
    const f32 exciterAlpha = (2.0f * 3.14159265358979323846f * exciterCutoffHz)
        / ((2.0f * 3.14159265358979323846f * exciterCutoffHz) + (f32)g_deviceSampleRate);
    const u32 channels = (outChannels < 8) ? outChannels : 8;

    for (u32 frame = 0; frame < frameCount; frame++) {
        for (u32 ch = 0; ch < channels; ch++) {
            f32& sample = out[frame * outChannels + ch];
            f32 processed = sample;
            if (g_bassBoostAmount > 0) {
                processed = ProcessBiquad(bass, g_bassShelfState[ch], processed);
            }
            if (g_trebleBoostAmount > 0) {
                processed = ProcessBiquad(treble, g_trebleShelfState[ch], processed);
                g_exciterLowpassState[ch] += exciterAlpha * (processed - g_exciterLowpassState[ch]);
                const f32 high = processed - g_exciterLowpassState[ch];
                processed += std::tanh(high * 2.5f) * exciterAmount;
            }
            sample = processed;
        }
    }
}

static void ApplySurroundUpmix(f32* out, u32 frameCount, u32 outChannels) {
    if (!g_surroundEnabled || outChannels < 4) {
        return;
    }

    const bool hasFiveOneLayout = outChannels >= 6;
    const u32 centerChannel = hasFiveOneLayout ? 2 : 0;
    const u32 lfeChannel = hasFiveOneLayout ? 3 : 0;
    const u32 rearLeftChannel = hasFiveOneLayout ? 4 : 2;
    const u32 rearRightChannel = hasFiveOneLayout ? 5 : 3;

    for (u32 frame = 0; frame < frameCount; frame++) {
        f32* dst = out + frame * outChannels;
        const f32 left = dst[0];
        const f32 right = dst[1];
        const f32 mono = (left + right) * 0.5f;
        const f32 ambience = (left - right) * 0.5f;

        if (hasFiveOneLayout) {
            dst[centerChannel] += mono * 0.45f;
            dst[lfeChannel] += mono * 0.18f;
        }

        dst[rearLeftChannel] += ambience * 0.70f;
        dst[rearRightChannel] -= ambience * 0.70f;
    }
}

static inline void StepMusicFade() {
    if (g_musicFadeFramesRemaining == 0) {
        return;
    }

    const u32 step = g_musicFadeFramesTotal - g_musicFadeFramesRemaining + 1;
    const f32 t = (f32)step / (f32)g_musicFadeFramesTotal;
    g_musicVolume = g_musicFadeStartVolume + (g_musicFadeTargetVolume - g_musicFadeStartVolume) * t;
    g_musicFadeFramesRemaining--;
    if (g_musicFadeFramesRemaining == 0) {
        g_musicVolume = g_musicFadeTargetVolume;
    }
}

static void ComputeSpatialGains(
    const InternalVoice& voice,
    const ListenerState& listener,
    u32 outChannels,
    f32 outGains[8]) {
    for (u32 i = 0; i < 8; i++) {
        outGains[i] = 0.0f;
    }

    if (!listener.valid || outChannels == 0) {
        if (outChannels >= 2) {
            outGains[0] = 0.70710678118f;
            outGains[1] = 0.70710678118f;
        }
        else {
            outGains[0] = 1.0f;
        }
        return;
    }

    const f32 dx = (f32)(voice.worldPos.x - listener.pos.x);
    const f32 dy = (f32)(voice.worldPos.y - listener.pos.y);
    const f32 dz = (f32)(voice.worldPos.z - listener.pos.z);
    const f32 distSq = dx * dx + dy * dy + dz * dz;
    const f32 dist = std::sqrt(distSq);

    f32 distGain = 1.0f;
    if (voice.applyDistanceAttenuation && voice.maxDistance > voice.minDistance) {
        if (dist >= voice.maxDistance) {
            distGain = 0.0f;
        }
        else if (dist > voice.minDistance) {
            distGain = (voice.maxDistance - dist) / (voice.maxDistance - voice.minDistance);
        }
    }

    if (distGain <= 0.0f) {
        return;
    }

    f32 invLen = 1.0f;
    if (dist > 0.0001f) {
        invLen = 1.0f / dist;
    }

    const f32 dirX = dx * invLen;
    const f32 dirZ = dz * invLen;

    const f32 yawRad = (f32)listener.yaw16 * (6.28318530717958647692f / 65536.0f);
    const f32 sinYaw = std::sin(yawRad);
    const f32 cosYaw = std::cos(yawRad);
    const f32 localX = dirX * cosYaw - dirZ * sinYaw;
    const f32 localZ = dirX * sinYaw + dirZ * cosYaw;
    // The PSX listener basis places its left virtual microphone along
    // (cos(yaw), -sin(yaw)); localX is positive in that direction. AudioEngine
    // uses positive pan for the right channel, so invert the sign here.
    const f32 side = clampf(-localX * 0.45f, -1.0f, 1.0f);
    const f32 front = clampf(localZ, -1.0f, 1.0f);
    const f32 rear = clampf(-front, 0.0f, 1.0f);
    const f32 frontAmount = clampf(front, 0.0f, 1.0f);
    const f32 frontRearSum = frontAmount + rear;
    const f32 frontW = (frontRearSum > 0.0001f) ? (frontAmount / frontRearSum) : 0.70710678118f;
    const f32 rearW = (frontRearSum > 0.0001f) ? (rear / frontRearSum) : 0.29289321882f;
    const f32 leftGain = equalPowerLeft(side);
    const f32 rightGain = equalPowerRight(side);

    if (outChannels == 1) {
        outGains[0] = distGain;
        return;
    }

    if (outChannels == 2) {
        outGains[0] = leftGain * distGain;
        outGains[1] = rightGain * distGain;
        return;
    }

    if (outChannels == 4) {
        outGains[0] = leftGain * frontW * distGain;
        outGains[1] = rightGain * frontW * distGain;
        outGains[2] = leftGain * rearW * distGain;
        outGains[3] = rightGain * rearW * distGain;
        return;
    }

    outGains[0] = leftGain * frontW * distGain;
    outGains[1] = rightGain * frontW * distGain;

    if (outChannels >= 3) {
        outGains[2] = clampf(1.0f - std::fabs(side), 0.0f, 1.0f) * frontW * distGain;
    }
    if (outChannels >= 4) {
        outGains[3] = 0.0f; // LFE not synthesized.
    }
    if (outChannels >= 6) {
        outGains[4] = leftGain * rearW * distGain;
        outGains[5] = rightGain * rearW * distGain;
    }
    if (outChannels >= 8) {
        outGains[6] = leftGain * rearW * 0.5f * distGain;
        outGains[7] = rightGain * rearW * 0.5f * distGain;
    }
}

// Audio callback - mixes all active voices + music into output
static void audioCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount) {
    const u32 outChannels = (device && device->playback.channels > 0)
        ? (u32)device->playback.channels
        : g_outputChannels;

    f32* out = (f32*)output;
    memset(out, 0, frameCount * outChannels * sizeof(f32));

    ListenerState listener = {};
    {
        std::lock_guard<std::mutex> lock(g_listenerMutex);
        listener = g_listener;
    }

    // Mix voices
    {
        std::lock_guard<std::mutex> lock(g_voiceMutex);
        for (u32 v = 0; v < MAX_VOICES; v++) {
            InternalVoice& voice = g_voices[v];
            if (!voice.active) continue;

            AudioSample sid = voice.sample;
            if (sid == AUDIO_SAMPLE_INVALID || sid > MAX_SAMPLES) {
                voice.active = false;
                continue;
            }
            InternalSample& smp = g_samples[sid - 1];
            if (!smp.data) {
                voice.active = false;
                continue;
            }

            f32 gains[8] = {};

            if (voice.spatial) {
                ComputeSpatialGains(voice, listener, outChannels, gains);
            }
            else {
                if (outChannels >= 2) {
                    gains[0] = equalPowerLeft(voice.pan);
                    gains[1] = equalPowerRight(voice.pan);
                }
                else {
                    gains[0] = 1.0f;
                }
            }

            // Rate ratio: how many source frames per output frame
            f64 rateRatio = (f64)smp.sampleRate / (f64)g_deviceSampleRate;
            f64 advance = rateRatio * (f64)voice.pitch;

            for (u32 i = 0; i < frameCount; i++) {
                if (voice.fadeFramesRemaining > 0) {
                    const u32 step = voice.fadeFramesTotal - voice.fadeFramesRemaining + 1;
                    const f32 t = (f32)step / (f32)voice.fadeFramesTotal;
                    voice.volume = voice.fadeStartVolume + (voice.fadeTargetVolume - voice.fadeStartVolume) * t;
                    voice.fadeFramesRemaining--;
                    if (voice.fadeFramesRemaining == 0) {
                        voice.volume = voice.fadeTargetVolume;
                    }
                }

                const f32 vol = voice.volume * g_masterVolume;

                u32 pos = (u32)voice.positionF;
                if (pos >= smp.numFrames) {
                    if (voice.loop) {
                        voice.positionF = 0.0;
                        pos = 0;
                    }
                    else {
                        voice.active = false;
                        break;
                    }
                }

                const f32 sMono = ReadSampleMono(smp, voice.positionF, voice.loop);

                if (voice.spatial) {
                    for (u32 ch = 0; ch < outChannels && ch < 8; ch++) {
                        out[i * outChannels + ch] += sMono * vol * gains[ch];
                    }
                }
                else {
                    if (outChannels == 1) {
                        out[i] += sMono * vol;
                    }
                    else {
                        f32 sampleL = sMono;
                        f32 sampleR = sMono;
                        if (smp.channels >= 2) {
                            sampleL = ReadSampleStereoChannel(smp, voice.positionF, 0, voice.loop);
                            sampleR = ReadSampleStereoChannel(smp, voice.positionF, 1, voice.loop);
                        }

                        out[i * outChannels] += sampleL * vol * gains[0];
                        out[i * outChannels + 1] += sampleR * vol * gains[1];
                    }
                }

                voice.positionF += advance;
            }
        }
    }

    // Mix music
    {
        std::lock_guard<std::mutex> lock(g_musicMutex);
        if (g_musicSampleActive) {
            if (g_musicSample == AUDIO_SAMPLE_INVALID || g_musicSample > MAX_SAMPLES) {
                g_musicSampleActive = false;
            }
            else {
                InternalSample& smp = g_samples[g_musicSample - 1];
                if (!smp.data) {
                    g_musicSampleActive = false;
                }
                else {
                    const f64 rateRatio = (f64)smp.sampleRate / (f64)g_deviceSampleRate;
                    for (u32 i = 0; i < frameCount; i++) {
                        StepMusicFade();

                        u32 pos = (u32)g_musicPositionF;
                        if (pos >= smp.numFrames) {
                            if (g_musicLoop) {
                                g_musicPositionF = 0.0;
                                pos = 0;
                            }
                            else {
                                g_musicSampleActive = false;
                                break;
                            }
                        }

                        const f32 mvol = g_musicVolume * g_masterVolume;
                        const f32 left = ReadSampleStereoChannel(smp, g_musicPositionF, 0, g_musicLoop);
                        const f32 right =
                            (smp.channels >= 2)
                            ? ReadSampleStereoChannel(smp, g_musicPositionF, 1, g_musicLoop)
                            : left;

                        if (outChannels == 1) {
                            out[i] += 0.5f * (left + right) * mvol;
                        }
                        else {
                            const u32 dst = i * outChannels;
                            out[dst] += left * mvol;
                            out[dst + 1] += right * mvol;
                        }

                        g_musicPositionF += rateRatio;
                    }
                }
            }
        }
        else if (g_musicActive) {
            // Read into temp buffer as f32
            f32 temp[4096];
            u32 remaining = frameCount;
            u32 offset = 0;
            while (remaining > 0) {
                u32 toRead = remaining;
                if (toRead > 2048) toRead = 2048; // 2048 frames * 2 channels = 4096 floats
                ma_uint64 framesRead = 0;
                ma_decoder_read_pcm_frames(&g_musicDecoder, temp, toRead, &framesRead);
                if (framesRead == 0) {
                    if (g_musicLoop) {
                        ma_decoder_seek_to_pcm_frame(&g_musicDecoder, 0);
                        continue;
                    }
                    else {
                        g_musicActive = false;
                        break;
                    }
                }
                for (u32 i = 0; i < (u32)framesRead; i++) {
                    StepMusicFade();
                    const f32 mvol = g_musicVolume * g_masterVolume;
                    const f32 l = temp[i * MUSIC_CHANNELS] * mvol;
                    const f32 r = temp[i * MUSIC_CHANNELS + 1] * mvol;

                    if (outChannels == 1) {
                        out[offset + i] += 0.5f * (l + r);
                    }
                    else {
                        const u32 dst = offset + i * outChannels;
                        out[dst] += l;
                        out[dst + 1] += r;
                    }
                }
                offset += (u32)framesRead * outChannels;
                remaining -= (u32)framesRead;
            }
        }
    }

    if (g_outputMono && outChannels > 1) {
        for (u32 frame = 0; frame < frameCount; frame++) {
            f32 mono = 0.0f;
            for (u32 ch = 0; ch < outChannels; ch++) {
                mono += out[frame * outChannels + ch];
            }
            mono /= (f32)outChannels;
            for (u32 ch = 0; ch < outChannels; ch++) {
                out[frame * outChannels + ch] = mono;
            }
        }
    }

#if !MODERN_SPATIAL_AUDIO
    ApplySurroundUpmix(out, frameCount, outChannels);
#endif
    ApplyMastering(out, frameCount, outChannels);

    // Clamp output
    for (u32 i = 0; i < frameCount * outChannels; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

#if defined(RC_PLATFORM_SWITCH)
// --- libnx audout output backend -------------------------------------------
// A dedicated thread double-buffers audio: wait for a played buffer to free,
// run the shared mixer (audioCallback) to produce f32, convert to s16, and
// re-queue it. 1024 frames @ 48kHz ~= 21ms per buffer.
static constexpr u32 kSwitchAudioFrames = 1024;
static std::thread g_switchAudioThread;
static std::atomic<bool> g_switchAudioRun{false};
static AudioOutBuffer g_switchBuffers[2] = {};
static void* g_switchBufferMem[2] = { nullptr, nullptr };
static f32* g_switchMixBuffer = nullptr;

static void SwitchAudioThreadFunc() {
    const u32 channels = g_outputChannels;
    const u32 frames = kSwitchAudioFrames;
    const u32 samples = frames * channels;

    while (g_switchAudioRun.load(std::memory_order_relaxed)) {
        AudioOutBuffer* released = nullptr;
        u32 releasedCount = 0;
        // 100ms timeout so shutdown (run flag cleared) is noticed promptly
        // even if playback stalls.
        Result rc = audoutWaitPlayFinish(&released, &releasedCount, 100000000ULL);
        if (R_FAILED(rc) || releasedCount == 0 || !released) {
            continue;
        }

        // Reuse the platform-independent mixer. Passing nullptr for the
        // device makes it fall back to g_outputChannels (see audioCallback).
        audioCallback(nullptr, g_switchMixBuffer, nullptr, frames);

        s16* dst = static_cast<s16*>(released->buffer);
        for (u32 i = 0; i < samples; i++) {
            f32 s = g_switchMixBuffer[i];
            s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
            dst[i] = static_cast<s16>(s * 32767.0f);
        }
        released->data_size = samples * sizeof(s16);
        audoutAppendAudioOutBuffer(released);
    }
}
#endif // RC_PLATFORM_SWITCH

// AudioEngine implementation

bool AudioEngine::Init() {
    if (g_initialized) return true;

    // Clear state before starting device (callback may fire immediately)
    memset(g_samples, 0, sizeof(g_samples));
    memset(g_voices, 0, sizeof(g_voices));
    g_nextSampleId = 1;
    g_nextVoiceId = 1;
    g_masterVolume = 0.5f;
    g_outputMono = false;
    memset(g_bassShelfState, 0, sizeof(g_bassShelfState));
    memset(g_trebleShelfState, 0, sizeof(g_trebleShelfState));
    memset(g_exciterLowpassState, 0, sizeof(g_exciterLowpassState));
    g_surroundEnabled = true;
    g_outputChannels = DEFAULT_MIX_CHANNELS;
    g_musicActive = false;
    g_musicSample = AUDIO_SAMPLE_INVALID;
    g_musicSampleActive = false;
    g_musicPositionF = 0.0;
    g_musicLoop = false;
    g_musicVolume = 1.0f;
    g_musicFadeStartVolume = 1.0f;
    g_musicFadeTargetVolume = 1.0f;
    g_musicFadeFramesTotal = 0;
    g_musicFadeFramesRemaining = 0;
    g_listener = {};
    g_listener.valid = false;

#if defined(RC_PLATFORM_SWITCH)
    // Real audio on Switch via libnx audout (RC_PLATFORM_NULL is also defined
    // on this build to gate the auto-updater etc., but audio is now live, so
    // this branch takes precedence over the null no-op below).
    if (R_FAILED(audoutInitialize())) {
        LOG("AudioEngine: audoutInitialize failed");
        return false;
    }
    if (R_FAILED(audoutStartAudioOut())) {
        LOG("AudioEngine: audoutStartAudioOut failed");
        audoutExit();
        return false;
    }

    g_deviceSampleRate = audoutGetSampleRate();     // 48000
    g_outputChannels   = audoutGetChannelCount();   // 2
    if (g_outputChannels == 0) g_outputChannels = DEFAULT_MIX_CHANNELS;

    const u32 frames = kSwitchAudioFrames;
    const size_t dataBytes = (size_t)frames * g_outputChannels * sizeof(s16);
    const size_t alignedBytes = (dataBytes + 0xFFF) & ~(size_t)0xFFF; // audout wants 0x1000 alignment/size

    g_switchMixBuffer = static_cast<f32*>(malloc((size_t)frames * g_outputChannels * sizeof(f32)));
    bool alloc_ok = (g_switchMixBuffer != nullptr);
    for (int i = 0; i < 2 && alloc_ok; i++) {
        g_switchBufferMem[i] = memalign(0x1000, alignedBytes);
        if (!g_switchBufferMem[i]) { alloc_ok = false; break; }
        memset(g_switchBufferMem[i], 0, alignedBytes);
        g_switchBuffers[i].next        = nullptr;
        g_switchBuffers[i].buffer      = g_switchBufferMem[i];
        g_switchBuffers[i].buffer_size = alignedBytes;
        g_switchBuffers[i].data_size   = dataBytes;
        g_switchBuffers[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&g_switchBuffers[i]);
    }
    if (!alloc_ok) {
        LOG("AudioEngine: Switch audio buffer alloc failed");
        audoutStopAudioOut();
        audoutExit();
        free(g_switchMixBuffer); g_switchMixBuffer = nullptr;
        for (int i = 0; i < 2; i++) { free(g_switchBufferMem[i]); g_switchBufferMem[i] = nullptr; }
        return false;
    }

    g_switchAudioRun.store(true, std::memory_order_relaxed);
    g_switchAudioThread = std::thread(SwitchAudioThreadFunc);

    g_initialized = true;
    LOG("AudioEngine: initialized (Switch audout %u Hz, %u ch, %u frames/buf)",
        g_deviceSampleRate, g_outputChannels, frames);
    return true;
#elif defined(RC_PLATFORM_NULL)
    g_initialized = true;
    LOG("AudioEngine: initialized (headless/null backend, no device)");
    return true;
#else
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 0; // Native device channel layout.
    config.sampleRate = 0; // use native device rate to avoid resampling
    config.dataCallback = audioCallback;
    config.periodSizeInFrames = 512;
    config.periods = 3;

    if (ma_device_init(nullptr, &config, &g_device) != MA_SUCCESS) {
        LOG("AudioEngine: failed to init device");
        return false;
    }

    // Store actual device sample rate (may differ from ENGINE_SAMPLE_RATE)
    g_deviceSampleRate = g_device.sampleRate;
    g_outputChannels = (g_device.playback.channels > 0) ? (u32)g_device.playback.channels : DEFAULT_MIX_CHANNELS;

    if (ma_device_start(&g_device) != MA_SUCCESS) {
        LOG("AudioEngine: failed to start device");
        ma_device_uninit(&g_device);
        return false;
    }

    g_initialized = true;

    LOG("AudioEngine: initialized (device %u Hz, %u ch, period %u frames)",
        g_deviceSampleRate, g_outputChannels, config.periodSizeInFrames);
    return true;
#endif
}

void AudioEngine::Shutdown() {
    if (!g_initialized) return;

#if defined(RC_PLATFORM_SWITCH)
    // Stop the audio thread first (so it stops touching voices/samples), then
    // tear down audout and the buffers.
    g_switchAudioRun.store(false, std::memory_order_relaxed);
    if (g_switchAudioThread.joinable()) g_switchAudioThread.join();
#endif

    StopAllVoices();
    StopMusic();
    UnloadAllSamples();

#if defined(RC_PLATFORM_SWITCH)
    audoutStopAudioOut();
    audoutExit();
    free(g_switchMixBuffer); g_switchMixBuffer = nullptr;
    for (int i = 0; i < 2; i++) { free(g_switchBufferMem[i]); g_switchBufferMem[i] = nullptr; }
#elif !defined(RC_PLATFORM_NULL)
    ma_device_uninit(&g_device);
#endif
    g_initialized = false;

    LOG("AudioEngine: shutdown");
}

bool AudioEngine::IsInitialized() {
    return g_initialized;
}

AudioSample AudioEngine::LoadSample(const s16* data, u32 numFrames, u32 sampleRate, u32 channels) {
    if (!g_initialized || !data || numFrames == 0) return AUDIO_SAMPLE_INVALID;

    u32 totalSamples = numFrames * channels;
    s16* buffer = new s16[totalSamples];
    memcpy(buffer, data, totalSamples * sizeof(s16));

    std::lock_guard<std::mutex> lock(g_voiceMutex);

    // Find free slot
    for (u32 i = 0; i < MAX_SAMPLES; i++) {
        if (g_samples[i].data == nullptr) {
            g_samples[i].data = buffer;
            g_samples[i].numFrames = numFrames;
            g_samples[i].sampleRate = sampleRate;
            g_samples[i].channels = channels;
            return i + 1; // 1-based handle
        }
    }

    delete[] buffer;
    LOG("AudioEngine: no free sample slots");
    return AUDIO_SAMPLE_INVALID;
}

void AudioEngine::UnloadSample(AudioSample handle) {
    if (handle == AUDIO_SAMPLE_INVALID || handle > MAX_SAMPLES) return;

    s16* oldData = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_voiceMutex);
        InternalSample& smp = g_samples[handle - 1];
        oldData = smp.data;
        smp.data = nullptr;
        smp.numFrames = 0;
    }
    delete[] oldData;
}

void AudioEngine::UnloadAllSamples() {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    for (u32 i = 0; i < MAX_SAMPLES; i++) {
        delete[] g_samples[i].data;
        g_samples[i].data = nullptr;
        g_samples[i].numFrames = 0;
    }
}

AudioVoice AudioEngine::PlaySample(AudioSample sample, f32 volume, f32 pan, bool loop, u8 priority) {
    if (!g_initialized || sample == AUDIO_SAMPLE_INVALID) return AUDIO_VOICE_INVALID;

    std::lock_guard<std::mutex> lock(g_voiceMutex);

    const u32 i = AllocVoiceSlot(priority);
    if (i >= MAX_VOICES) {
        LOG("AudioEngine: no free voice slots (priority=%u)", priority);
        return AUDIO_VOICE_INVALID;
    }

    g_voices[i].sample = sample;
    g_voices[i].positionF = 0.0;
    g_voices[i].volume = volume;
    g_voices[i].fadeStartVolume = volume;
    g_voices[i].fadeTargetVolume = volume;
    g_voices[i].fadeFramesTotal = 0;
    g_voices[i].fadeFramesRemaining = 0;
    g_voices[i].pan = pan;
    g_voices[i].pitch = 1.0f;
    g_voices[i].worldPos = {};
    g_voices[i].minDistance = 0.0f;
    g_voices[i].maxDistance = 10000.0f;
    g_voices[i].spatial = false;
    g_voices[i].applyDistanceAttenuation = false;
    g_voices[i].loop = loop;
    g_voices[i].active = true;
    g_voices[i].startTimeSec = AudioNowSeconds();
    g_voices[i].overdueLogged = false;
    g_voices[i].priority = priority;
    g_voices[i].generation++;
    return (g_voices[i].generation << kVoiceSlotBits) | (i + 1);
}

AudioVoice AudioEngine::PlaySample3D(
    AudioSample sample,
    const LVector& position,
    f32 volume,
    bool loop,
    bool applyDistanceAttenuation,
    f32 minDistance,
    f32 maxDistance,
    u8 priority) {
    if (!g_initialized || sample == AUDIO_SAMPLE_INVALID) return AUDIO_VOICE_INVALID;

    std::lock_guard<std::mutex> lock(g_voiceMutex);

    const u32 i = AllocVoiceSlot(priority);
    if (i >= MAX_VOICES) {
        LOG("AudioEngine: no free voice slots (priority=%u)", priority);
        return AUDIO_VOICE_INVALID;
    }

    g_voices[i].sample = sample;
    g_voices[i].positionF = 0.0;
    g_voices[i].volume = volume;
    g_voices[i].fadeStartVolume = volume;
    g_voices[i].fadeTargetVolume = volume;
    g_voices[i].fadeFramesTotal = 0;
    g_voices[i].fadeFramesRemaining = 0;
    g_voices[i].pan = 0.0f;
    g_voices[i].pitch = 1.0f;
    g_voices[i].worldPos = position;
    g_voices[i].minDistance = minDistance;
    g_voices[i].maxDistance = (maxDistance > minDistance) ? maxDistance : (minDistance + 1.0f);
    g_voices[i].spatial = true;
    g_voices[i].applyDistanceAttenuation = applyDistanceAttenuation;
    g_voices[i].loop = loop;
    g_voices[i].active = true;
    g_voices[i].startTimeSec = AudioNowSeconds();
    g_voices[i].overdueLogged = false;
    g_voices[i].priority = priority;
    g_voices[i].generation++;
    return (g_voices[i].generation << kVoiceSlotBits) | (i + 1);
}

// PSX: jcsSetLevelDialog captures gp+1220 via rsdGetVoice/rsdLockVoice.
AudioVoice AudioEngine::ReserveVoice(u8 priority) {
    if (!g_initialized) return AUDIO_VOICE_INVALID;

    std::lock_guard<std::mutex> lock(g_voiceMutex);

    const u32 i = AllocVoiceSlot(priority);
    if (i >= MAX_VOICES) {
        LOG("AudioEngine: ReserveVoice failed, no slot (priority=%u)", priority);
        return AUDIO_VOICE_INVALID;
    }

    g_voices[i] = InternalVoice{};
    g_voices[i].priority = priority;
    g_voices[i].locked = true;
    g_voices[i].active = false;
    g_voices[i].generation++;
    return (g_voices[i].generation << kVoiceSlotBits) | (i + 1);
}

void AudioEngine::ReleaseVoice(AudioVoice reserved) {
    if (reserved == AUDIO_VOICE_INVALID) return;
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    const u32 slot = (reserved & kVoiceSlotMask) - 1u;
    if (slot >= MAX_VOICES) return;
    g_voices[slot].active = false;
    g_voices[slot].locked = false;
}

// Resolves by slot only (ignores generation) so the reservation handle stays valid across lines.
AudioVoice AudioEngine::PlayOnReservedVoice(AudioVoice reserved, AudioSample sample, f32 volume, f32 pan) {
    if (!g_initialized || sample == AUDIO_SAMPLE_INVALID || reserved == AUDIO_VOICE_INVALID) {
        return AUDIO_VOICE_INVALID;
    }

    std::lock_guard<std::mutex> lock(g_voiceMutex);

    const u32 i = (reserved & kVoiceSlotMask) - 1u;
    if (i >= MAX_VOICES || !g_voices[i].locked) return AUDIO_VOICE_INVALID;

    const u8 priority = g_voices[i].priority;
    g_voices[i].sample = sample;
    g_voices[i].positionF = 0.0;
    g_voices[i].volume = volume;
    g_voices[i].fadeStartVolume = volume;
    g_voices[i].fadeTargetVolume = volume;
    g_voices[i].fadeFramesTotal = 0;
    g_voices[i].fadeFramesRemaining = 0;
    g_voices[i].pan = pan;
    g_voices[i].pitch = 1.0f;
    g_voices[i].worldPos = {};
    g_voices[i].minDistance = 0.0f;
    g_voices[i].maxDistance = 10000.0f;
    g_voices[i].spatial = false;
    g_voices[i].applyDistanceAttenuation = false;
    g_voices[i].loop = false;
    g_voices[i].active = true;
    g_voices[i].startTimeSec = AudioNowSeconds();
    g_voices[i].overdueLogged = false;
    g_voices[i].priority = priority;
    g_voices[i].locked = true;
    g_voices[i].generation++;
    return (g_voices[i].generation << kVoiceSlotBits) | (i + 1);
}

void AudioEngine::StopVoice(AudioVoice voice) {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    InternalVoice* v = ResolveVoice(voice);
    if (v) v->active = false;
}

void AudioEngine::StopAllVoices() {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    for (u32 i = 0; i < MAX_VOICES; i++) {
        g_voices[i].active = false;
    }
}

bool AudioEngine::IsVoicePlaying(AudioVoice voice) {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    u32 slot = 0;
    InternalVoice* vp = ResolveVoice(voice, &slot);
    if (!vp) {
        return false; // normal end-of-voice: handle no longer resolves
    }
    InternalVoice& v = *vp;

    if (!v.loop && v.sample != AUDIO_SAMPLE_INVALID && v.sample <= MAX_SAMPLES) {
        const InternalSample& smp = g_samples[v.sample - 1];
        if (!smp.data || smp.numFrames == 0 || smp.sampleRate == 0) {
            LOG("[AudioTrace] IsVoicePlaying voice=%u slot=%u sample=%u INVALID SAMPLE DATA (data=%p numFrames=%u sampleRate=%u) - stopping",
                voice, slot, v.sample, (void*)smp.data, smp.numFrames, smp.sampleRate);
            v.active = false;
            return false;
        }

        const f64 elapsed = AudioNowSeconds() - v.startTimeSec;
        const f64 playedFrames = elapsed * static_cast<f64>(smp.sampleRate) * static_cast<f64>(v.pitch);
        const f64 expectedDur = static_cast<f64>(smp.numFrames) / (static_cast<f64>(smp.sampleRate) * static_cast<f64>(v.pitch));
        if (!v.overdueLogged && expectedDur > 0.0 && elapsed > expectedDur * 3.0) {
            v.overdueLogged = true;
            LOG("[AudioTrace] voice=%u slot=%u OVERDUE elapsed=%.3fs expectedDur=%.3fs numFrames=%u sampleRate=%u pitch=%.2f loop=%d sample=%u",
                voice, slot, elapsed, expectedDur, smp.numFrames, smp.sampleRate, v.pitch, v.loop ? 1 : 0, v.sample);
        }
        if (playedFrames >= static_cast<f64>(smp.numFrames)) {
            v.active = false;
            return false;
        }
    }
    else if (v.loop) {
        static bool loopLoggedOnce[MAX_VOICES] = {};
        if (!loopLoggedOnce[slot]) {
            loopLoggedOnce[slot] = true;
            LOG("[AudioTrace] voice=%u slot=%u is LOOPING (never auto-expires) sample=%u", voice, slot, v.sample);
        }
    }

    return true;
}

void AudioEngine::SetVoiceVolume(AudioVoice voice, f32 volume) {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    InternalVoice* v = ResolveVoice(voice);
    if (!v) return;
    v->volume = volume;
    v->fadeStartVolume = volume;
    v->fadeTargetVolume = volume;
    v->fadeFramesTotal = 0;
    v->fadeFramesRemaining = 0;
}

void AudioEngine::FadeVoiceVolume(AudioVoice voice, f32 targetVolume, u32 fadeMs) {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    InternalVoice* vp = ResolveVoice(voice);
    if (!vp) {
        return;
    }
    InternalVoice& v = *vp;

    const f32 clampedTarget = clampf(targetVolume, 0.0f, 1.0f);
    if (fadeMs == 0) {
        v.volume = clampedTarget;
        v.fadeStartVolume = clampedTarget;
        v.fadeTargetVolume = clampedTarget;
        v.fadeFramesTotal = 0;
        v.fadeFramesRemaining = 0;
        return;
    }

    u32 fadeFrames = (g_deviceSampleRate * fadeMs) / 1000;
    if (fadeFrames == 0) {
        fadeFrames = 1;
    }

    v.fadeStartVolume = v.volume;
    v.fadeTargetVolume = clampedTarget;
    v.fadeFramesTotal = fadeFrames;
    v.fadeFramesRemaining = fadeFrames;
}

void AudioEngine::SetVoicePan(AudioVoice voice, f32 pan) {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    InternalVoice* v = ResolveVoice(voice);
    if (!v) return;
    v->pan = pan;
    v->spatial = false;
}

void AudioEngine::SetVoicePitch(AudioVoice voice, f32 pitch) {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    InternalVoice* v = ResolveVoice(voice);
    if (v) v->pitch = pitch;
}

void AudioEngine::SetVoicePosition(AudioVoice voice, const LVector& position) {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    InternalVoice* v = ResolveVoice(voice);
    if (!v) return;
    v->worldPos = position;
    v->spatial = true;
}

void AudioEngine::SetVoiceDistanceRange(AudioVoice voice, f32 minDistance, f32 maxDistance) {
    std::lock_guard<std::mutex> lock(g_voiceMutex);
    InternalVoice* v = ResolveVoice(voice);
    if (!v) return;
    v->minDistance = minDistance;
    v->maxDistance = (maxDistance > minDistance) ? maxDistance : (minDistance + 1.0f);
}

void AudioEngine::SetListener(const LVector& position, s32 yaw16) {
    std::lock_guard<std::mutex> lock(g_listenerMutex);
    g_listener.pos = position;
    g_listener.yaw16 = yaw16;
    g_listener.valid = true;
}

bool AudioEngine::PlayMusic(const char* path, f32 volume, bool loop) {
    if (!g_initialized || !path) return false;

    StopMusic();

    std::lock_guard<std::mutex> lock(g_musicMutex);

    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, MUSIC_CHANNELS, g_deviceSampleRate);
    const std::string resolvedPath = p3d::io::ResolvePath(path);
    if (ma_decoder_init_file(resolvedPath.c_str(), &decoderConfig, &g_musicDecoder) != MA_SUCCESS) {
        LOG("AudioEngine: failed to load music '%s'", path);
        return false;
    }

    g_musicVolume = volume;
    g_musicFadeStartVolume = volume;
    g_musicFadeTargetVolume = volume;
    g_musicFadeFramesTotal = 0;
    g_musicFadeFramesRemaining = 0;
    g_musicLoop = loop;
    g_musicActive = true;

    LOG("AudioEngine: playing music '%s'", path);
    return true;
}

bool AudioEngine::PlayMusicSample(AudioSample sample, f32 volume, bool loop) {
    if (!g_initialized || sample == AUDIO_SAMPLE_INVALID || sample > MAX_SAMPLES) {
        return false;
    }

    StopMusic();

    std::lock_guard<std::mutex> lock(g_musicMutex);
    InternalSample& smp = g_samples[sample - 1];
    if (!smp.data || smp.numFrames == 0) {
        return false;
    }

    g_musicSample = sample;
    g_musicSampleActive = true;
    g_musicPositionF = 0.0;
    g_musicLoop = loop;
    g_musicVolume = volume;
    g_musicFadeStartVolume = volume;
    g_musicFadeTargetVolume = volume;
    g_musicFadeFramesTotal = 0;
    g_musicFadeFramesRemaining = 0;
    return true;
}

void AudioEngine::StopMusic() {
    std::lock_guard<std::mutex> lock(g_musicMutex);
    if (g_musicActive) {
        g_musicActive = false;
        ma_decoder_uninit(&g_musicDecoder);
    }
    g_musicSample = AUDIO_SAMPLE_INVALID;
    g_musicSampleActive = false;
    g_musicPositionF = 0.0;
    g_musicFadeFramesTotal = 0;
    g_musicFadeFramesRemaining = 0;
}

void AudioEngine::SetMusicVolume(f32 volume) {
    std::lock_guard<std::mutex> lock(g_musicMutex);
    g_musicVolume = volume;
    g_musicFadeStartVolume = volume;
    g_musicFadeTargetVolume = volume;
    g_musicFadeFramesTotal = 0;
    g_musicFadeFramesRemaining = 0;
}

void AudioEngine::FadeMusicVolume(f32 targetVolume, u32 fadeMs) {
    std::lock_guard<std::mutex> lock(g_musicMutex);
    if (!g_musicActive && !g_musicSampleActive) {
        return;
    }

    if (fadeMs == 0) {
        g_musicVolume = targetVolume;
        g_musicFadeStartVolume = targetVolume;
        g_musicFadeTargetVolume = targetVolume;
        g_musicFadeFramesTotal = 0;
        g_musicFadeFramesRemaining = 0;
        return;
    }

    const u32 frames = (u32)(((u64)fadeMs * (u64)g_deviceSampleRate) / 1000u);
    if (frames == 0) {
        g_musicVolume = targetVolume;
        g_musicFadeStartVolume = targetVolume;
        g_musicFadeTargetVolume = targetVolume;
        g_musicFadeFramesTotal = 0;
        g_musicFadeFramesRemaining = 0;
        return;
    }

    g_musicFadeStartVolume = g_musicVolume;
    g_musicFadeTargetVolume = targetVolume;
    g_musicFadeFramesTotal = frames;
    g_musicFadeFramesRemaining = frames;
}

bool AudioEngine::IsMusicPlaying() {
    std::lock_guard<std::mutex> lock(g_musicMutex);
    return g_musicActive || g_musicSampleActive;
}

void AudioEngine::SetMasterVolume(f32 volume) {
    g_masterVolume = volume;
}

f32 AudioEngine::GetMasterVolume() {
    return g_masterVolume;
}

void AudioEngine::SetOutputMono(bool mono) {
    g_outputMono = mono;
}

bool AudioEngine::GetOutputMono() {
    return g_outputMono;
}

u32 AudioEngine::GetOutputChannels() {
    return g_outputChannels;
}

void AudioEngine::SetSurroundEnabled(bool enabled) {
    g_surroundEnabled = enabled;
}

bool AudioEngine::GetSurroundEnabled() {
    return g_surroundEnabled;
}

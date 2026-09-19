#include "gen/time.h"
#include <cmath>
#if defined(RC_PLATFORM_PSP)
#include <pspkernel.h>
#else
#include <chrono>
#include <thread>
#endif

// PSX: gp-relative global
Time* g_time = nullptr;

FrameProfile g_frameProfile;

Time::Time() { MARKFUNCTION(0x80044950); }
Time::~Time() { MARKFUNCTION(0x800449E8); }

void Time::InternalOpen() { MARKFUNCTION(0x80044A10); }
void Time::InternalClose() { MARKFUNCTION(0x80044A18); }

void Time::InternalReset() {
    MARKFUNCTION(0x80044A38);
    frameCounter = 0;
#if HIGH_FPS_PLAY_PRESENTATION
    ResetPlayPresentationState();
#endif
}

void Time::Step() {
    MARKFUNCTION(0x80044A40);
    frameCounter++;
}

void Time::Tick(f32 realDt) {
    if (realDt < 0.0001f) realDt = 0.0001f;
    if (realDt > 0.25f)  realDt = 0.25f;
    deltaTime = realDt;
    fps = 1.0f / realDt;
}

#if HIGH_FPS_PLAY_PRESENTATION
void Time::ResetPlayPresentationState() {
    playLogicAccumulator = 0.0f;
    playPresentationAlpha = 0.0f;
    playLogicStepCount = 1;
}

s32 Time::BeginPlayFixedStep() {
    static constexpr f32 kPlayLogicDt = 1.0f / 30.0f;
#if defined(RC_PLATFORM_PSP)
    // A PSP frame cannot afford a multi-step catch-up after a streamed asset
    // load. Limit catch-up to one 30 Hz simulation step per presentation
    // frame, retaining the fractional remainder for interpolation.
    static constexpr s32 kMaxLogicStepsPerFrame = 1;
#else
    static constexpr s32 kMaxLogicStepsPerFrame = 4;
#endif
#if !defined(RC_PLATFORM_PSP)
    static constexpr f32 kMaxAccumulatedTime = kPlayLogicDt * (f32)kMaxLogicStepsPerFrame;
#endif

    if (targetFPS == 30) {
        playLogicAccumulator = 0.0f;
        playPresentationAlpha = 1.0f;
        playLogicStepCount = 1;
        return playLogicStepCount;
    }

    playLogicAccumulator += deltaTime;
#if defined(RC_PLATFORM_PSP)
    // Drop only whole overdue ticks. Clamping the entire accumulator to one
    // tick loses the remainder on ordinary frames (e.g. 20+16 ms), slowing
    // simulation at 50 Hz and repeatedly snapping interpolation to zero.
    if (playLogicAccumulator >= 2.0f * kPlayLogicDt) {
        playLogicAccumulator = kPlayLogicDt + std::fmod(playLogicAccumulator, kPlayLogicDt);
    }
#else
    if (playLogicAccumulator > kMaxAccumulatedTime) {
        playLogicAccumulator = kMaxAccumulatedTime;
    }
#endif

    playLogicStepCount = 0;
    while (playLogicAccumulator >= kPlayLogicDt && playLogicStepCount < kMaxLogicStepsPerFrame) {
        playLogicAccumulator -= kPlayLogicDt;
        ++playLogicStepCount;
    }

    playPresentationAlpha = playLogicAccumulator / kPlayLogicDt;
    if (playPresentationAlpha < 0.0f) {
        playPresentationAlpha = 0.0f;
    }
    if (playPresentationAlpha > 1.0f) {
        playPresentationAlpha = 1.0f;
    }

    return playLogicStepCount;
}
#endif

f64 Time::GetTimeInSeconds() {
#if defined(RC_PLATFORM_PSP)
    return static_cast<f64>(sceKernelGetSystemTimeWide()) / 1000000.0;
#else
    static auto sStart = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<f64>(now - sStart).count();
#endif
}

void Time::Sleep(f32 seconds) {
#if defined(RC_PLATFORM_PSP)
    if (seconds > 0.0f) {
        sceKernelDelayThread(static_cast<unsigned int>(seconds * 1000000.0f));
    }
#else
    if (seconds > 0.0f) {
        std::this_thread::sleep_for(std::chrono::duration<f32>(seconds));
    }
#endif
}

void Time::WaitForFrameEnd(f64 frameStart) const {
#if defined(RC_PLATFORM_PSP)
    // pspDisplay::SwapBuffers() already submits the GU list and blocks on the
    // next vblank. Sleeping again here would add a second frame limiter after
    // the synchronized swap, producing jitter and occasionally dropping the
    // presentation cadence below 60 Hz. Keep the argument for the shared API.
    (void)frameStart;
    return;
#else
    f32 target = GetTargetDt();
    if (target <= 0.0f) {
        return;
    }

    // Adaptive 1ms sleep estimator keeps limiter accurate across platforms,
    // including schedulers with coarse default sleep granularity.
    static f64 sleepMean = 0.001f;
    static f64 sleepM2 = 0.0;
    static u32 sleepSamples = 1;
    static f64 sleepEstimate = 0.002f;

    const f64 frameEnd = frameStart + (f64)target;
    while (true) {
        f64 now = GetTimeInSeconds();
        f64 remaining = frameEnd - now;
        if (remaining <= 0.0) {
            break;
        }

        if (remaining > sleepEstimate) {
            f64 sleepStart = GetTimeInSeconds();
            Sleep(0.001f);
            f64 observed = GetTimeInSeconds() - sleepStart;

            sleepSamples++;
            f64 delta = observed - sleepMean;
            sleepMean += delta / (f64)sleepSamples;
            f64 delta2 = observed - sleepMean;
            sleepM2 += delta * delta2;

            f64 variance = sleepM2 / (f64)(sleepSamples - 1);
            if (variance < 0.0) {
                variance = 0.0;
            }
            sleepEstimate = sleepMean + std::sqrt(variance);
        }
        else {
            std::this_thread::yield();
        }
    }
#endif
}

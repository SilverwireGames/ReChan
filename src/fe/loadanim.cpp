#include "gen/common.h"
#include "fe/loadanim.h"
#include "gen/display.h"
#include "pc/tim.h"
#include "gen/config.h"
#include "gen/time.h"
#include "p3d/context.h"
#include "p3d/input.h"
#include "p3d/texture.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"

#include <cmath>

#if CUSTOM_MENU
#include "extra/fecustommenumgr.h"
#endif

// PSX: VBlankLogo object (32 bytes on PSX, gp+1356)
static struct {
    tTexture* bgTexture;
    u8 currentFill;
    u8 targetFill;
    bool active;
} s_logo = {};

// PSX: color pulse state (gp+1360..gp+1364)
// gp+1360 = R channel (5-bit, pulses 5-28), gp+1361 = G (0), gp+1362 = B (0)
// PSX advanced this +-1 per VBlank (~30Hz), a 23-wide range each way. Now that
// the present loop isn't throttled to a fixed cadence (see PresentLoadingFrameFaded),
// a per-call increment would pulse at whatever rate the loop actually runs -
// drive it from wall-clock time instead so the glow speed is fps-independent.
static u8 s_pulseR = 28;

static u8 ComputePulseR() {
    constexpr f32 kPulseMin = 5.0f;
    constexpr f32 kPulseMax = 28.0f;
    constexpr f32 kHalfCycleSec = 23.0f / 30.0f; // matches the original 1-unit/VBlank cadence
    const f32 t = std::fmod((f32)Time::GetTimeInSeconds(), kHalfCycleSec * 2.0f) / kHalfCycleSec;
    const f32 tri = (t < 1.0f) ? t : (2.0f - t); // 0 -> 1 -> 0
    return (u8)(kPulseMin + (kPulseMax - kPulseMin) * tri);
}

// PSX bar position from Update__10VBlankLogoP6_RTASK math:
// Y = dispEnv.y + 140, tile 8x6, scale gp+3540 = 2.28 (16.16)
// Left edge: ((-50.0 * scale) >> 32) + 233 = 119
// Right edge at 100%: computed from FillMeter target = 339
static constexpr f32 kBarY = 138.5f;
static constexpr f32 kBarH = 6.0f;
static constexpr f32 kBarLeftX = 119.0f;
static constexpr f32 kBarMaxW = 229.0f;

// PSX: Update__10VBlankLogoP6_RTASK (LOADANIM.CPP:85, 0x80047778)
static void DrawLoadingScreen(u8 fill) {
    s_pulseR = ComputePulseR();

    bool usedCustomScreen = false;
#if CUSTOM_MENU
    if (g_feCustomMenuMgr) {
        usedCustomScreen = g_feCustomMenuMgr->DrawLoadingScreen(fill, static_cast<u8>(s_pulseR << 3));
    }
    if (!usedCustomScreen) {
        ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 255);
    }
#else
    if (!s_logo.bgTexture)
        return;

    ScreenDraw::DrawFullscreen(s_logo.bgTexture);
#endif

    if (!usedCustomScreen && fill != 0) {
        if (fill > 100)
            fill = 100;

        f32 nx = SCALE_AND_CENTER_X(kBarLeftX);
        f32 ny = SCREEN_SCALE_Y(kBarY);
        f32 nw = SCREEN_SCALE_X(kBarMaxW * (fill / 100.0f));
        f32 nh = SCREEN_SCALE_Y(kBarH);

        u8 r8 = s_pulseR << 3;
        ScreenDraw::DrawColoredRect(nx, ny, nw, nh, r8, 0, 0, 255);
    }
}

// fill/blackAlpha are driven by elapsed real time at each call site (see
// FillMeter, StartLogo, StopLogo), so this just presents as fast as the
// display allows (vsync-limited in the common case) - no artificial per-call
// throttle, which would otherwise cap how many distinct fill/alpha values
// actually get sampled and shown during a fixed-duration animation, making it
// look stepped rather than smooth. blackAlpha overlays a black quad on top
// (255 = fully black), used to fade the loading screen in/out instead of
// cutting to/from it instantly.
static void PresentLoadingFrameFaded(u8 fill, u8 blackAlpha) {
    if (p3d::display) {
        p3d::display->PollEvents();
    }

    if (p3d::input) {
        p3d::input->ServiceInput();
    }
    g_display->BeginFrame();
    DrawLoadingScreen(fill);
    if (blackAlpha > 0) {
        ScreenDraw::DrawColoredQuad(0, 0, 0, blackAlpha);
    }
    g_display->EndFrame();
}

static void PresentLoadingFrame(u8 fill) {
    PresentLoadingFrameFaded(fill, 0);
}

static void PresentLoadingFrame_Tex(tTexture* tex) {
    if (p3d::display) {
        p3d::display->PollEvents();
    }
    if (p3d::input) {
        p3d::input->ServiceInput();
    }
    g_display->BeginFrame();
    ScreenDraw::DrawFullscreen(tex);
    g_display->EndFrame();
}

// PSX: StartLogo__10VBlankLogol (LOADANIM.CPP:167, 0x80047968)
void StartLogo(const char* timFile) {
    MARKFUNCTION(0x80047968);

    // Load background TIM
    if (!s_logo.bgTexture) {
        TimImage* img = Tim::LoadFromFile(timFile);
        if (img) {
            s_logo.bgTexture = Tim::CreateTexture(img);
            delete img;
        }
    }

    // PSX: obj+8 = 0, obj+12 = 0, SetActive(1)
    s_logo.currentFill = 0;
    s_logo.targetFill = 0;
    s_logo.active = true;

    // PSX: initial data values from SLUS binary at gp+1360/1364
    s_pulseR = 28;

#if CUSTOM_MENU
    // Fade in from black instead of cutting straight to the loading screen.
    // Matches Game::FadeBegin's pacing (s_fadeStep=17, 255/17 steps @ 30Hz =
    // 0.5s) so this reads as the same fade the rest of the game already uses.
    constexpr f64 kFadeInSec = 0.5;
    const f64 fadeStart = Time::GetTimeInSeconds();
    for (;;) {
        const f32 t = (f32)((Time::GetTimeInSeconds() - fadeStart) / kFadeInSec);
        if (t >= 1.0f) {
            break;
        }
        PresentLoadingFrameFaded(0, (u8)(255.0f * (1.0f - t)));
    }
#endif

    // Present initial loading screen with empty bar
    PresentLoadingFrame(0);
}

static constexpr f64 kFillMeterDurationSec = 0.35;

// PSX: FillMeter__10VBlankLogoUc (LOADANIM.CPP:207, 0x80047A68)
void FillMeter(u8 target) {
    MARKFUNCTION(0x80047A68);

    const s32 startFill = s_logo.currentFill;
    const s32 delta = (s32)target - startFill;
    if (delta == 0) {
        s_logo.targetFill = target;
        return;
    }

    const f64 duration = kFillMeterDurationSec * (std::abs(delta) / 100.0);
    const f64 fillStart = Time::GetTimeInSeconds();
    for (;;) {
        f32 t = (f32)((Time::GetTimeInSeconds() - fillStart) / duration);
        if (t >= 1.0f) {
            s_logo.currentFill = target;
            s_logo.targetFill = target;
            PresentLoadingFrame(target);
            break;
        }

        t = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
        s_logo.currentFill = (u8)(startFill + (s32)((f32)delta * t));
        s_logo.targetFill = s_logo.currentFill;
        PresentLoadingFrame(s_logo.currentFill);
    }
}

void PumpLoadingScreen() {
    if (!s_logo.active) {
        return;
    }
    PresentLoadingFrame(s_logo.currentFill);
}

// PSX: StopLogo__10VBlankLogo (LOADANIM.CPP:183, 0x800479BC)
void StopLogo() {
    MARKFUNCTION(0x800479BC);

    if (!s_logo.active)
        return;

#if defined(RC_PLATFORM_PSP)
    // The PSP has no reason to spend several vblanks on the desktop-style
    // fade/hold animation. Present the final meter state once, release the
    // loading texture, and return immediately so level transition cannot
    // stall before PrePlay on a slow Memory Stick/renderer path.
    if (s_logo.currentFill != s_logo.targetFill) {
        s_logo.currentFill = s_logo.targetFill;
        PresentLoadingFrame(s_logo.targetFill);
    }
    if (s_logo.bgTexture) {
        s_logo.bgTexture->Release();
        s_logo.bgTexture = nullptr;
    }
    s_logo.active = false;
    return;
#endif

    // Snap to the final value (no-op if FillMeter already got there).
    if (s_logo.currentFill != s_logo.targetFill) {
        FillMeter(s_logo.targetFill);
    }

    // Brief hold so the completed bar registers before the screen cuts away.
    // PSX held for ~10 VBlanks (1/3s); kept short here for the same reason
    // FillMeter is duration-based now, not a fixed PSX frame count.
    constexpr f64 kHoldSec = 0.2;
    const f64 holdStart = Time::GetTimeInSeconds();
    do {
        PresentLoadingFrame(s_logo.targetFill);
    } while (Time::GetTimeInSeconds() - holdStart < kHoldSec);

#if CUSTOM_MENU
    // Fade to black before cutting away, mirroring the fade-in in StartLogo
    // (and matching Game::FadeBegin's 0.5s pacing - see StartLogo's comment).
    constexpr f64 kFadeOutSec = 0.5;
    const f64 fadeStart = Time::GetTimeInSeconds();
    for (;;) {
        const f32 t = (f32)((Time::GetTimeInSeconds() - fadeStart) / kFadeOutSec);
        if (t >= 1.0f) {
            break;
        }
        PresentLoadingFrameFaded(s_logo.targetFill, (u8)(255.0f * t));
    }
    g_display->BeginFrame();
    ScreenDraw::DrawColoredQuad(0, 0, 0, 255);
    g_display->EndFrame();
#endif

    // PSX: SetActive(0), destructor frees tile buffer + removes VBlank task
    if (s_logo.bgTexture) {
        s_logo.bgTexture->Release();
        s_logo.bgTexture = nullptr;
    }
    s_logo.active = false;
}

// PSX: DisplayTIM__FPCc (GAME.CPP:862, 0x80029200)
// PSX: ClearImage + LoadImage (direct VRAM ops). PC: draw fullscreen quad.
void DisplayTIM(const char* filename) {
    MARKFUNCTION(0x80029200);

    TimImage* img = Tim::LoadFromFile(filename);
    if (!img) {
        LOG("[LoadAnim] DisplayTIM: failed to load %s", filename);
        return;
    }
    tTexture* tex = Tim::CreateTexture(img);
    delete img;
    if (!tex)
        return;

    // Present the image (double-buffer: draw twice, swap once)
    PresentLoadingFrame_Tex(tex);

    tex->Release();
}

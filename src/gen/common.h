#pragma once
#include "config.h"
#include "pc/log.h"
#include "gen/display.h"

#include <cctype>

// Locale-independent ASCII comparison for asset and data identifiers.
static inline int rcStricmp(const char* lhs, const char* rhs) {
    while (*lhs && *rhs) {
        const int a = std::tolower(static_cast<unsigned char>(*lhs));
        const int b = std::tolower(static_cast<unsigned char>(*rhs));
        if (a != b) return a - b;
        ++lhs;
        ++rhs;
    }
    return static_cast<unsigned char>(*lhs) - static_cast<unsigned char>(*rhs);
}

#if defined(RC_PLATFORM_PSP)
#define JCS_TARGET_WIDTH 480
#define JCS_TARGET_HEIGHT 272
#else
#define JCS_TARGET_WIDTH 1280
#define JCS_TARGET_HEIGHT 720
#endif

// Native resolution (overlay coordinate space)
#define DEFAULT_SCREEN_WIDTH ((f32)512)
#define DEFAULT_SCREEN_HEIGHT ((f32)240)
#define DEFAULT_ASPECT_RATIO (4.0f / 3.0f)

// Font/text coordinate space
#define DEFAULT_FONT_WIDTH ((f32)320)
#define DEFAULT_FONT_HEIGHT ((f32)240)

#define SCREEN_WIDTH (g_display ? (f32)g_display->GetScreenWidth() : DEFAULT_SCREEN_WIDTH)
#define SCREEN_HEIGHT (g_display ? (f32)g_display->GetScreenHeight() : DEFAULT_SCREEN_HEIGHT)
#define ASPECT_RATIO (g_display ? g_display->GetAspectRatio() : DEFAULT_ASPECT_RATIO)

// PSX pixel coordinates to screen space.
#define SCREEN_STRETCH_X(a) ((a) * (float)SCREEN_WIDTH / DEFAULT_SCREEN_WIDTH)
#define SCREEN_STRETCH_Y(a) ((a) * (float)SCREEN_HEIGHT / DEFAULT_SCREEN_HEIGHT)
#define SCREEN_STRETCH_FROM_RIGHT(a) (SCREEN_WIDTH - SCREEN_STRETCH_X(a))
#define SCREEN_STRETCH_FROM_BOTTOM(a) (SCREEN_HEIGHT - SCREEN_STRETCH_Y(a))

#define SCREEN_SCALE_X(a) SCREEN_SCALE_AR(SCREEN_STRETCH_X(a))
#define SCREEN_SCALE_Y(a) SCREEN_STRETCH_Y(a)
#define SCREEN_SCALE_FROM_RIGHT(a) (SCREEN_WIDTH - SCREEN_SCALE_X(a))
#define SCREEN_SCALE_FROM_BOTTOM(a) (SCREEN_HEIGHT - SCREEN_SCALE_Y(a))

#define TARGET_ASPECT_RATIO (16.0f / 9.0f)

// Effective width limited to 16:9
#define SCREEN_EFFECTIVE_WIDTH \
    ((SCREEN_WIDTH > SCREEN_HEIGHT * TARGET_ASPECT_RATIO) ? \
    (SCREEN_HEIGHT * TARGET_ASPECT_RATIO) : (float)SCREEN_WIDTH)

// Offset to center the clamped area
#define SCREEN_EFFECTIVE_OFFSET_X \
    ((SCREEN_WIDTH - SCREEN_EFFECTIVE_WIDTH) * 0.5f)

#if FIX_ASPECT_RATIO
#define SCREEN_SCALE_AR(a) ((a) * DEFAULT_ASPECT_RATIO / ASPECT_RATIO)
#define SCALE_AND_CENTER_X(x) ((SCREEN_WIDTH == DEFAULT_SCREEN_WIDTH) ? (x) : (SCREEN_WIDTH - SCREEN_SCALE_X(DEFAULT_SCREEN_WIDTH)) / 2 + SCREEN_SCALE_X((x)))
#else
#define SCREEN_SCALE_AR(a) (a)
#define SCALE_AND_CENTER_X(x) SCREEN_STRETCH_X(x)
#endif

static f32 HudSafeWidth() {
    const f32 screenW = SCREEN_WIDTH;
    const f32 screenH = SCREEN_HEIGHT;
    const f32 maxW16x9 = screenH * TARGET_ASPECT_RATIO;
    return (screenW > maxW16x9) ? maxW16x9 : screenW;
}

static f32 HudSafeOffsetX() {
    const f32 safeW = HudSafeWidth();
    const f32 screenW = SCREEN_WIDTH;
    if (screenW > safeW) {
        return (screenW - safeW) * 0.5f;
    }
    return 0.0f;
}

[[maybe_unused]] static f32 HudX(f32 x) {
    return HudSafeOffsetX() + (x / DEFAULT_SCREEN_WIDTH) * HudSafeWidth();
}

[[maybe_unused]] static f32 HudY(f32 y) {
    return SCREEN_SCALE_Y(y);
}

[[maybe_unused]] static f32 HudW(f32 w) {
    return (w / DEFAULT_SCREEN_WIDTH) * HudSafeWidth();
}

[[maybe_unused]] static f32 HudH(f32 h) {
    return SCREEN_SCALE_Y(h);
}

template <typename T>
static inline T Clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

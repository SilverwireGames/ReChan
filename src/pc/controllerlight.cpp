// controllerlight.cpp — health-based DualSense/DualShock light bar colour.
#include "pc/controllerlight.h"

#include "ai/player.h"
#include "gen/control.h"
#include "gen/time.h"
#include "p3d/context.h"
#include "pddi/pddidev.h"

#include <cmath>

namespace {
    // Duration of the red flash triggered when the player lands a hit.
    constexpr f32 kHitFlashSeconds = 0.18f;
    constexpr f32 kDamageFlashSeconds = 0.45f;
    // Below this health ratio the bar starts pulsing to warn the player.
    constexpr f32 kCriticalRatio = 0.25f;

    f32 s_hitFlashTimer = 0.0f;
    f32 s_damageFlashTimer = 0.0f;
    f64 s_lastTime = -1.0;
}

void ControllerLight::FlashHit() {
    s_hitFlashTimer = kHitFlashSeconds;
}

void ControllerLight::FlashDamage() {
    s_damageFlashTimer = kDamageFlashSeconds;
}

void ControllerLight::Update() {
#if defined(RC_PLATFORM_PSP)
    // The PSP has no light bar: pspGamepad inherits the no-op
    // pddiGamepad::SetLight, so the colour/flash state computed below could
    // never reach any hardware. Skip it instead of paying its per-frame
    // (double precision, soft-float) timing math.
    return;
#endif
    if (!p3d::context) {
        return;
    }
    pddiGamepad* pad = p3d::gamepad;
    if (!pad) {
        return;
    }

    const f64 now = Time::GetTimeInSeconds();
    f32 dt = (s_lastTime < 0.0) ? 0.0f : static_cast<f32>(now - s_lastTime);
    s_lastTime = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    if (s_hitFlashTimer > 0.0f) {
        s_hitFlashTimer = (s_hitFlashTimer > dt) ? (s_hitFlashTimer - dt) : 0.0f;
    }
    if (s_damageFlashTimer > 0.0f) {
        s_damageFlashTimer = (s_damageFlashTimer > dt) ? (s_damageFlashTimer - dt) : 0.0f;
    }

    static bool sLightbarOff = false;
    if (!GetLightbarEnabled()) {
        if (!sLightbarOff) {
            sLightbarOff = true;
            pad->SetLight(0, 0, 0);
        }
        return;
    }
    sLightbarOff = false;

    u8 r = 0;
    u8 g = 0;
    u8 b = 0;

    Player* player = Player::s_player;
    if (player && player->maxHealth > 0) {
        f32 ratio = static_cast<f32>(player->health) / static_cast<f32>(player->maxHealth);
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;

        // red (0.0) -> yellow (0.5) -> green (1.0)
        if (ratio > 0.5f) {
            const f32 t = (ratio - 0.5f) / 0.5f;
            r = static_cast<u8>(255.0f * (1.0f - t));
            g = 255;
        }
        else {
            const f32 t = ratio / 0.5f;
            r = 255;
            g = static_cast<u8>(200.0f * t);
        }

        if (ratio < kCriticalRatio) {
            const f32 pulse = 0.55f + 0.45f * std::sin(static_cast<f32>(now) * 8.0f);
            r = static_cast<u8>(r * pulse);
            g = static_cast<u8>(g * pulse);
        }
    }
    else {
        // Not in play (menus / loading): dim PlayStation-style blue.
        r = 0;
        g = 40;
        b = 120;
    }

    if (s_hitFlashTimer > 0.0f) {
        r = 255;
        g = 0;
        b = 0;
    }
    if (s_damageFlashTimer > 0.0f) {
        // Damage: blink red so it stands out from the hit flash.
        const bool on = (static_cast<s32>(now * 12.0) & 1) == 0;
        r = 255;
        g = on ? 60 : 0;
        b = 0;
    }

    pad->SetLight(r, g, b);
}

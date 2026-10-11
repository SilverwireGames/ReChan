#pragma once
#include "core.h"
#include "pc/apppaths.h"

struct GameSettings {
    bool Load(const char* path);
    bool Save(const char* path);
};

extern GameSettings g_settings;

#if defined(RC_PLATFORM_PSP)
// Cinema mode: a steady 30 fps (one 30 Hz logic tick per frame, no
// interpolation) instead of up to 60. Saved with the PSP settings.
bool PspIsCinemaMode();
void PspSetCinemaMode(bool enabled);

// Follow-camera distance: 0 = the level's original framing, 1-3 = closer.
static constexpr s32 kPspCameraDistanceCount = 4;
s32 PspGetCameraDistance();
void PspSetCameraDistance(s32 level);
#endif

static constexpr const char* SETTINGS_PATH = apppaths::kSettingsPath;

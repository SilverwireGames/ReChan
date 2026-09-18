#include "pc/settings.h"

GameSettings g_settings;

bool GameSettings::Load(const char*) {
    // The PSP build intentionally keeps the original title's fixed defaults;
    // platform controls and video mode are not user-editable at runtime.
    return true;
}

bool GameSettings::Save(const char*) { return true; }

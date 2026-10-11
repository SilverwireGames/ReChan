#include "pc/settings.h"

#include "extra/customhudmgr.h"
#include "gen/camera.h"
#include "gen/ccfile.h"
#include "gen/time.h"
#include "extra/customtext.h"
#include "p3d/fileio.h"
#include "pddi/psp/psprender.h"
#include "psp/psp_perf.h"

#include <psputility.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

GameSettings g_settings;

// The PSP build keeps the original title's fixed defaults for audio,
// controls and video. Only the port's own presentation choices persist, as
// key=value lines in a small file beside the save slots. It is not the
// desktop game.ini, which packaged trees may carry along.
static const char kPspSettingsPath[] = "userfiles/pspconf.ini";
static const char kHudStyleKey[] = "hud_style=";
static const char kCinemaKey[] = "cinema=";
static const char kCameraKey[] = "camera=";
static const char kLanguageKey[] = "language=";
static const char kSetupKey[] = "setup=";

static bool s_cinemaMode = false;
static s32 s_cameraDistance = 0;
static bool s_setupDone = false;

// Eye-to-look-at distance per camera option, as a 16.16 fraction of the
// level's own framing: original, then 80%, 65% and 50%.
static const s32 kCameraDistanceScale[kPspCameraDistanceCount] = {
    0x10000, 0xCCCD, 0xA666, 0x8000,
};

// Settings are saved after every menu adjustment; skip Memory Stick writes
// that would store what the file already holds.
static s32 s_savedHudStyle = -1;
static s32 s_savedCinema = -1;
static s32 s_savedCamera = -1;
static s32 s_savedLanguage = -1;
static s32 s_savedSetup = -1;

// Before the first-boot choice: the PSP's own system language when the game
// has it, English otherwise.
static s32 SystemLanguage() {
    int value = 0;
    if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &value) < 0) return (s32)LangEnglish;
    switch (value) {
        case PSP_SYSTEMPARAM_LANGUAGE_GERMAN: return (s32)LangGerman;
        case PSP_SYSTEMPARAM_LANGUAGE_FRENCH: return (s32)LangFrench;
        case PSP_SYSTEMPARAM_LANGUAGE_ITALIAN: return (s32)LangItalian;
        case PSP_SYSTEMPARAM_LANGUAGE_SPANISH: return (s32)LangSpanish;
        case PSP_SYSTEMPARAM_LANGUAGE_PORTUGUESE: return (s32)LangPortuguese;
        default: return (s32)LangEnglish;
    }
}

bool PspIsFirstBootSetupPending() {
    return !s_setupDone;
}

void PspFinishFirstBootSetup() {
    s_setupDone = true;
    g_settings.Save(nullptr);
}

s32 PspGetCameraDistance() {
    return s_cameraDistance;
}

void PspSetCameraDistance(s32 level) {
    if (level < 0 || level >= kPspCameraDistanceCount) {
        level = 0;
    }
    s_cameraDistance = level;
    Camera::SetFollowDistanceScale(kCameraDistanceScale[level]);
}

bool PspIsCinemaMode() {
    return s_cinemaMode;
}

void PspSetCinemaMode(bool enabled) {
    s_cinemaMode = enabled;
    // The display holds each frame for two refreshes and the game takes one
    // 30 Hz tick per frame (Time's 30 fps path): no interpolation needed.
    SetPspSwapInterval(enabled ? 2u : 1u);
    PspPerf::SetFrameTarget(enabled ? 30u : 60u);
    if (g_time) {
        g_time->targetFPS = enabled ? 30 : 60;
        g_time->ResetPlayPresentationState();
    }
}

bool GameSettings::Load(const char*) {
    s32 hudStyle = HudStyle_Modern;
    s32 cinema = 0;
    s32 camera = 0;
    s32 language = SystemLanguage();
    s32 setup = 0;

    ccFile file;
    if (file.Open(kPspSettingsPath, ccFile::OPEN_READ)) {
        char text[256] = {};
        s32 length = file.GetLength();
        if (length > (s32)sizeof(text) - 1) {
            length = (s32)sizeof(text) - 1;
        }
        const s32 bytes = (length > 0) ? file.Read(text, (u32)length) : 0;
        file.Close();
        text[(bytes > 0) ? bytes : 0] = '\0';

        for (const char* line = text; line && *line; ) {
            if (std::strncmp(line, kHudStyleKey, sizeof(kHudStyleKey) - 1) == 0) {
                hudStyle = (s32)std::atoi(line + sizeof(kHudStyleKey) - 1);
            }
            else if (std::strncmp(line, kCinemaKey, sizeof(kCinemaKey) - 1) == 0) {
                cinema = (std::atoi(line + sizeof(kCinemaKey) - 1) != 0) ? 1 : 0;
            }
            else if (std::strncmp(line, kCameraKey, sizeof(kCameraKey) - 1) == 0) {
                camera = (s32)std::atoi(line + sizeof(kCameraKey) - 1);
            }
            else if (std::strncmp(line, kLanguageKey, sizeof(kLanguageKey) - 1) == 0) {
                language = (s32)std::atoi(line + sizeof(kLanguageKey) - 1);
            }
            else if (std::strncmp(line, kSetupKey, sizeof(kSetupKey) - 1) == 0) {
                setup = (std::atoi(line + sizeof(kSetupKey) - 1) != 0) ? 1 : 0;
            }
            line = std::strchr(line, '\n');
            if (line) {
                line++;
            }
        }
        s_savedHudStyle = hudStyle;
        s_savedCinema = cinema;
        s_savedCamera = camera;
        s_savedLanguage = language;
        s_savedSetup = setup;
    }

    if (language < 0 || language >= (s32)NumLanguages) language = (s32)LangEnglish;
    g_customText.SetLanguage((GameLanguage)language);
    s_setupDone = setup != 0;
    SetHudStyle(hudStyle);
    PspSetCinemaMode(cinema != 0);
    PspSetCameraDistance(camera);
    return true;
}

bool GameSettings::Save(const char*) {
    const s32 hudStyle = (s32)GetHudStyle();
    const s32 cinema = s_cinemaMode ? 1 : 0;
    const s32 camera = s_cameraDistance;
    const s32 language = (s32)g_customText.GetLanguage();
    const s32 setup = s_setupDone ? 1 : 0;
    if (hudStyle == s_savedHudStyle && cinema == s_savedCinema && camera == s_savedCamera
        && language == s_savedLanguage && setup == s_savedSetup) {
        return true;
    }

    p3d::io::CreateDirectories(apppaths::kUserFilesDir);

    char text[128] = {};
    const s32 length = (s32)std::snprintf(text, sizeof(text), "%s%d\n%s%d\n%s%d\n%s%d\n%s%d\n",
        kHudStyleKey, (int)hudStyle, kCinemaKey, (int)cinema, kCameraKey, (int)camera,
        kLanguageKey, (int)language, kSetupKey, (int)setup);

    ccFile file;
    if (!file.Open(kPspSettingsPath, ccFile::OPEN_WRITE)) {
        return false;
    }
    const s32 written = file.Write(text, (u32)length);
    file.Close();
    if (written != length) {
        return false;
    }

    s_savedHudStyle = hudStyle;
    s_savedCinema = cinema;
    s_savedCamera = camera;
    s_savedLanguage = language;
    s_savedSetup = setup;
    return true;
}

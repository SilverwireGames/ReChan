#include "pc/settings.h"

#include "extra/customhudmgr.h"
#include "gen/ccfile.h"
#include "p3d/fileio.h"

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

// Settings are saved after every menu adjustment; skip Memory Stick writes
// that would store what the file already holds.
static s32 s_savedHudStyle = -1;

bool GameSettings::Load(const char*) {
    s32 hudStyle = HudStyle_Modern;

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
            line = std::strchr(line, '\n');
            if (line) {
                line++;
            }
        }
        s_savedHudStyle = hudStyle;
    }

    SetHudStyle(hudStyle);
    return true;
}

bool GameSettings::Save(const char*) {
    const s32 hudStyle = (s32)GetHudStyle();
    if (hudStyle == s_savedHudStyle) {
        return true;
    }

    p3d::io::CreateDirectories(apppaths::kUserFilesDir);

    char text[64] = {};
    const s32 length = (s32)std::snprintf(text, sizeof(text), "%s%d\n", kHudStyleKey, (int)hudStyle);

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
    return true;
}

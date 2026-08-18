#include "gen/common.h"
#include "fecustommenumgr.h"
#include <cstdlib>
#include "gen/display.h"
#include "pc/inputaction.h"
#include "pc/tim.h"
#include "gen/game.h"
#include "ai/fevolume.h"
#include "fe/femenumgr.h"
#include "fe/gamemenu.h"
#include "snd/fesnd.h"
#include "snd/rsevent.h"
#include "snd/sound.h"
#include "pc/settings.h"
#include "pc/textmgr.h"
#include "gen/time.h"
#include "xclib/xccolour.h"
#include "xclib/xclib.h"
#include "gen/scoremgr.h"
#include "gen/world.h"
#include "p3d/texture.h"
#include "pddi/pdditex.h"
#include "pddi/pddishad.h"
#include "p3d/context.h"
#include "p3d/input.h"
#if NEW_CHEATS
#include "extra/cheats.h"
#endif

#ifdef MOD_LOADER
#include "extra/modloader.h"
#endif

#if AUTO_UPDATER
#include "extra/autoupdater.h"
#endif
#include "version.h"

#include "extra/dialogpreview.h"
#include "extra/prompticons.h"
#include "extra/shadowcsm.h"

feCustomMenuMgr* g_feCustomMenuMgr = nullptr;

static f32 UiAnimSeconds() {
    return (f32)Time::GetTimeInSeconds();
}

// Symmetric 0 -> 1 -> 0 triangle wave with the given period in seconds.
static f32 UiTriangle01(f32 periodSeconds) {
    if (periodSeconds <= 0.0f) {
        return 0.0f;
    }
    const f32 t = std::fmod(UiAnimSeconds(), periodSeconds) / periodSeconds;
    return (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);
}

static constexpr s32 kFrameRateOptionValues[] = { 30, 60, 120, 0 };
static constexpr s32 kMsaaOptionValues[] = { 0, 2, 4, 8, 16 };
static constexpr s32 kKeyBindingActionCount = ACTION_OPEN_CLOSE_MENU;
static constexpr u32 HASH_LEVEL_SCREEN = 0x0598ABF8;
static constexpr u32 HASH_LEVEL_EXECUTE = 0x893DA6B2;
static constexpr u32 HASH_LOCATION_OVERLAY = 42519405;
static constexpr u32 HASH_TEXT_LEVELNAME = 0x39AA5899;

static constexpr f32 DEF_MENU_TITLE_SCALE = 0.34f;
static constexpr f32 DEF_MENU_TEXT_SCALE = 0.4f;
static constexpr f32 DEF_MENU_WINDOW_TITLE_SCALE = 0.4f;
static constexpr f32 DEF_REDEFINE_KEY_TEXT_SCALE = 0.3f;
static constexpr f32 DEF_MENU_PROMPT_SCALE = 0.3f;
static constexpr f32 DEF_MENU_DRAGON_COUNT_SCALE = 0.5f;
static constexpr f32 DEF_CONTROLLER_ACTION_SCALE = 0.2f;

static f32 GetSaveDisplayRowOffset(s32 displayIndex) {
    return (f32)(displayIndex * DEF_SAVE_ROW_H
                 + (displayIndex > 0 ? DEF_SAVE_AUTOSAVE_GAP : 0));
}

static f32 GetSaveDisplayScrollOffset(f32 displayPosition) {
    if (displayPosition <= 0.0f) return 0.0f;
    const s32 lower = (s32)std::floor(displayPosition);
    const f32 fraction = displayPosition - (f32)lower;
    const f32 a = GetSaveDisplayRowOffset(lower);
    const f32 b = GetSaveDisplayRowOffset(lower + 1);
    return a + (b - a) * fraction;
}

static void SmoothScrollToward(f32& visual, f32 target) {
    const f32 dt = g_time ? g_time->GetDeltaTime() : (1.0f / 30.0f);
    visual += (target - visual) * std::min(1.0f, dt * 14.0f);
    if (std::fabs(target - visual) < 0.002f) visual = target;
}

// Rounds the clip bounds outward (floor the top, ceil the bottom) so the scissor rect
// always fully contains the row geometry it's meant to bound - rounding to nearest
// pixel here can land the scissor a pixel inside the first row, clipping a sliver off it.
static void SetVerticalScissorPSX(f32 topY, f32 h) {
    if (!p3d::context) return;
    const f32 top = SCREEN_SCALE_Y(topY);
    const f32 bottom = SCREEN_SCALE_Y(topY + h);
    const s32 sy = (s32)top;
    const s32 sBottom = (s32)bottom + ((bottom > (f32)(s32)bottom) ? 1 : 0);
    ScreenDraw::SetScissor(0, sy, (s32)(SCREEN_WIDTH + 0.5f), sBottom - sy);
}

#if AUTO_UPDATER
// Derived from the page's actual frameH so a window resize can't desync from how many
// lines are rendered (a fixed line-count constant previously drifted out of sync with this).
static s32 ComputeChangelogVisibleLines(s32 frameH) {
    const s32 bodyAvailH = frameH - DEF_TITLE_BAR_H - DEF_BOTTOM_BAR_H - DEF_CONTENT_TOP_PAD - DEF_CONTENT_BOTTOM_PAD * 2;
    const s32 lines = bodyAvailH / 8;
    return (lines > 1) ? lines : 1;
}
#endif

struct LocationRuntimeInfo {
    s32 levelID = 0;
    s32 levelIndex = 0;
    s32 subLevel = 0;
    s32 collectCount = 0;
    bool hasGoldDragon = false;
    bool showDragons = true;
    u8 grade = 1;
    bool hasGrade = false;
    const char* levelName = nullptr;
};

const char* GetSpecialLocationToken(s32 levelID) {
    switch (levelID) {
        case 1:
        case 11: return "FE_LOC_CHN";
        case 2:
        case 12: return "FE_LOC_WTR";
        case 3:
        case 13: return "FE_LOC_SWR";
        case 4:
        case 14: return "FE_LOC_ROF";
        case 5:
        case 8: return "FE_LOC_FAC";
        case 6: return "FE_LOC_TMP";
        case 7: return "FE_LOC_DST";
        default: return nullptr;
    }
}

static const char* ResolveLocationSpecialTitle(s32 levelIndex) {
    if (!g_feMenuMgr) {
        return nullptr;
    }

    xcSection* section = g_feMenuMgr->GetSection();
    if (!section) {
        return nullptr;
    }

    xcOverlayData* overlay = g_feMenuMgr->FindOverlay(HASH_LOCATION_OVERLAY);
    if (!overlay) {
        return nullptr;
    }

    xcTextPrim* levelNameText = (xcTextPrim*)overlay->GetTextObj(HASH_TEXT_LEVELNAME, section->rawData);
    if (!levelNameText) {
        return nullptr;
    }

    xcTextPrim copy = *levelNameText;
    copy.paletteIdx = (u8)levelIndex;
    const u32 strHash = copy.GetStringHash();
    return section->FindString(strHash);
}

static const char* GradeToLetter(u8 grade) {
    switch (grade) {
        case 1:
            return "D";
        case 2:
            return "C";
        case 3:
            return "B";
        case 4:
            return "A";
        default:
            if (grade >= 5) {
                return "A+";
            }
            break;
    }

    return " ";
}

static bool ResolveLocationRuntimeInfo(LocationRuntimeInfo* outInfo) {
    if (!outInfo || !g_game) {
        return false;
    }

    World* world = g_game->GetWorld();
    if (!world) {
        return false;
    }

    s32 levelIndex = 0;
    s32 subLevel = 0;
    bool resolvedFromMenu = false;

    if (g_feMenuMgr) {
        hdMenu* levelMenu = g_feMenuMgr->FindMenu(g_feMenuMgr->startScreenHashes[1]);
        if (levelMenu) {
            hdMenuItem* execute = levelMenu->FindItem(HASH_LEVEL_EXECUTE);
            if (execute) {
                u32 packedLevel = 0;
                u32 packedPetal = 0;
                World::UnpackLevelName(execute->itemFlags, packedLevel, packedPetal);
                levelIndex = (s32)packedLevel;
                subLevel = (s32)packedPetal;
                resolvedFromMenu = true;
            }
        }

        if (!resolvedFromMenu && g_feMenuMgr->frontEndVolume) {
            const s32 levelCode = g_feMenuMgr->frontEndVolume->levelCode;
            const s32 levelID = levelCode / 10;
            levelIndex = world->LevelIDToIndex(levelID);
            subLevel = levelCode % 10;
            resolvedFromMenu = true;
        }
    }

    if (!resolvedFromMenu) {
        levelIndex = (s32)world->GetCurrentLevelIndex();
        subLevel = (s32)world->GetCurrentPetalIndex();
    }

    if (levelIndex < 0) {
        levelIndex = 0;
    }
    if (subLevel < 0) {
        subLevel = 0;
    }

    const s32 petalCount = world->GetLevelPetalCountFromIndex((u32)levelIndex);
    if (petalCount > 0 && subLevel >= petalCount) {
        subLevel = petalCount - 1;
    }

    outInfo->levelIndex = levelIndex;
    outInfo->subLevel = subLevel;
    outInfo->levelID = world->GetLevelIDFromIndex((u32)levelIndex);
    outInfo->showDragons = (outInfo->levelID >= 1 && outInfo->levelID <= 5);
    outInfo->levelName = world->GetLevelNameFromIndex((u32)levelIndex);

    if (g_scoreManager) {
        const s32 statIndex = levelIndex * 3 + subLevel;
        if (statIndex >= 0 && statIndex < 21) {
            const PetalStats& ps = g_scoreManager->petalStats[statIndex];
            outInfo->collectCount = ps.collectCount;
            outInfo->hasGoldDragon = g_scoreManager->CalcGDrags(ps.collectCount);
            outInfo->hasGrade = (ps.fightScore >= 0);
            outInfo->grade = outInfo->hasGrade ? ps.grade : 1;
        }
    }

    return true;
}

static void BeginNewGameReset() {
    if (g_scoreManager) {
        g_scoreManager->HandleGameBegin();
    }

    if (g_game) {
        World* world = g_game->GetWorld();
        if (world) {
            world->Unload();
            world->UnloadLevelPart2();
            world->UnloadPermanent();
        }
    }

    if (g_scoreManager) {
        g_scoreManager->drunkenMasterUnlocked = 0;
    }
}

static bool IsKeyBindingAction(Action action) {
    return action >= ACTION_JUMP && action < ACTION_OPEN_CLOSE_MENU;
}

static void SetDesktopBindingCodeUnique(Action action, s32 slot, s32 code) {
    if (!g_actionInput) {
        return;
    }

    if (!IsKeyBindingAction(action) || slot < 0 || slot >= DEF_KEYBIND_SLOT_COUNT) {
        return;
    }

    if (code != 0) {
        for (s32 actionIndex = 0; actionIndex < kKeyBindingActionCount; actionIndex++) {
            const Action currentAction = (Action)actionIndex;
            for (s32 currentSlot = 0; currentSlot < DEF_KEYBIND_SLOT_COUNT; currentSlot++) {
                if (currentAction == action && currentSlot == slot) {
                    continue;
                }

                if (g_actionInput->GetDesktopBindingCode(currentAction, currentSlot) == code) {
                    g_actionInput->SetDesktopBindingCode(currentAction, currentSlot, 0);
                }
            }
        }
    }

    g_actionInput->SetDesktopBindingCode(action, slot, code);
}

static void BuildActionTokenFallbackLabel(const char* token, char* outText, s32 outTextLen) {
    if (!outText || outTextLen <= 0) {
        return;
    }

    outText[0] = '\0';

    if (!token) {
        return;
    }

    s32 write = 0;
    for (s32 i = 0; token[i] != '\0' && write + 1 < outTextLen; i++) {
        char ch = token[i];
        if (ch == '_') {
            ch = ' ';
        }
        outText[write++] = ch;
    }
    outText[write] = '\0';
}

static const char* GetControllerLogicalLabelToken(u8 logicalIndex) {
    switch (logicalIndex) {
        case 0: return "FE_CSD";
        case 1: return "FE_CST";
        case 2: return "FE_CCT";
        case 3: return "FE_CDR";
        case 4: return "FE_CKK";
        case 5: return "FE_CGR";
        case 6: return "FE_CJP";
        case 7: return "FE_CPN";
        default: return nullptr;
    }
}

static void BuildDesktopBindingPromptText(Action action, s32 slot, char* outText, s32 outTextLen) {
    if (!outText || outTextLen <= 0) {
        return;
    }

    outText[0] = '\0';

    if (!g_actionInput || slot < 0 || slot >= DEF_KEYBIND_SLOT_COUNT) {
        snprintf(outText, outTextLen, "-");
        return;
    }

    const s32 code = g_actionInput->GetDesktopBindingCode(action, slot);
    if (code == 0) {
        snprintf(outText, outTextLen, "-");
        return;
    }

    snprintf(outText, outTextLen, "<BND:%d>", code);
}

static void SetPromptText(char* dst, s32 dstLen, const char* fmt, const char* a = nullptr, const char* b = nullptr, const char* c = nullptr) {
    if (!dst || dstLen <= 0) {
        return;
    }

    if (!fmt) {
        dst[0] = '\0';
        return;
    }

    if (a && b && c) {
        snprintf(dst, dstLen, fmt, a, b, c);
    }
    else if (a && b) {
        snprintf(dst, dstLen, fmt, a, b);
    }
    else if (a) {
        snprintf(dst, dstLen, fmt, a);
    }
    else {
        snprintf(dst, dstLen, "%s", fmt);
    }
}

static s32 ClampFrameRateOptionIndex(s32 index) {
    if (index < 0) {
        return 0;
    }

    const s32 maxIndex = (s32)(sizeof(kFrameRateOptionValues) / sizeof(kFrameRateOptionValues[0])) - 1;
    if (index > maxIndex) {
        return maxIndex;
    }
    return index;
}

static s32 FrameRateOptionIndexToValue(s32 index) {
    return kFrameRateOptionValues[ClampFrameRateOptionIndex(index)];
}

static s32 FrameRateValueToOptionIndex(s32 fps) {
    if (fps <= 0) {
        return (s32)(sizeof(kFrameRateOptionValues) / sizeof(kFrameRateOptionValues[0])) - 1;
    }

    s32 bestIndex = 0;
    s32 bestDist = 0x7fffffff;

    for (u32 i = 0; i < (u32)(sizeof(kFrameRateOptionValues) / sizeof(kFrameRateOptionValues[0])); i++) {
        const s32 candidate = kFrameRateOptionValues[i];
        if (candidate <= 0) {
            continue;
        }

        const s32 dist = (fps > candidate) ? (fps - candidate) : (candidate - fps);
        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = (s32)i;
        }
    }

    return bestIndex;
}

static const char* GetFrameRateDisplayToken(s32 index) {
    switch (FrameRateOptionIndexToValue(index)) {
        case 30: return "FE_FR30";
        case 60: return "FE_FR60";
        case 120: return "FE_FR120";
        case 0: return "FE_MAX";
        default: return nullptr;
    }
}

static s32 ClampMsaaOptionIndex(s32 index) {
    if (index < 0) {
        return 0;
    }

    const s32 maxIndex = (s32)(sizeof(kMsaaOptionValues) / sizeof(kMsaaOptionValues[0])) - 1;
    if (index > maxIndex) {
        return maxIndex;
    }
    return index;
}

static s32 MsaaOptionIndexToSamples(s32 index) {
    return kMsaaOptionValues[ClampMsaaOptionIndex(index)];
}

static s32 MsaaSamplesToOptionIndex(s32 samples) {
    s32 bestIndex = 0;
    s32 bestDist = 0x7fffffff;

    for (u32 i = 0; i < (u32)(sizeof(kMsaaOptionValues) / sizeof(kMsaaOptionValues[0])); i++) {
        const s32 candidate = kMsaaOptionValues[i];
        const s32 dist = (samples > candidate) ? (samples - candidate) : (candidate - samples);
        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = (s32)i;
        }
    }

    return bestIndex;
}

static s32 WrapStepValue(s32 current, s32 step, s32 lo, s32 hi, s32 dir) {
    if (lo > hi) {
        const s32 tmp = lo;
        lo = hi;
        hi = tmp;
    }

    if (step <= 0) {
        step = 1;
    }

    s32 v = current + ((dir > 0) ? step : -step);
    if (v < lo) {
        return hi;
    }
    if (v > hi) {
        return lo;
    }
    return v;
}

static s32 WrapStepValueSlider(s32 current, s32 step, s32 lo, s32 hi, s32 dir) {
    if (lo > hi) {
        const s32 tmp = lo;
        lo = hi;
        hi = tmp;
    }

    if (step <= 0) {
        step = 1;
    }

    s32 v = current + ((dir > 0) ? step : -step);
    return v;
}

static const char* GetMsaaDisplayToken(s32 index) {
    switch (MsaaOptionIndexToSamples(index)) {
        case 2:  return "FE_A2X";
        case 4:  return "FE_A4X";
        case 8:  return "FE_A8X";
        case 16: return "FE_A16X";
        default: return nullptr;
    }
}

static const char* GetLanguageDisplayToken(s32 index) {
    switch ((GameLanguage)index) {
        case LangEnglish: return "FE_LGEN";
        case LangGerman: return "FE_LGER";
        case LangFrench: return "FE_LFRE";
        case LangItalian: return "FE_LITA";
        case LangSpanish: return "FE_LSPA";
        default: return nullptr;
    }
}

static bool IsVolumeSliderBinding(EntryBinding binding) {
    return binding == EntryBinding_MusicVol
        || binding == EntryBinding_EffectsVol
        || binding == EntryBinding_DialogVol;
}

static s32 GetValueChangeSoundId(const Entry& entry) {
    if (entry.type == EntryType_Slider && IsVolumeSliderBinding(entry.binding)) {
        return FE_SND_MENU_8;
    }

    // Non-volume value adjustments should use the same sound as Enter/confirm.
    return FE_SND_MENU_5;
}

static bool IsSaveSlotPage(MenuPage page) {
    return page == MenuPage_LoadSlots
        || page == MenuPage_SaveSlots
        || page == MenuPage_DeleteSlots;
}

static s32 SaveSlotToDisplayIndex(s32 slotIndex);
static s32 SaveDisplayIndexToSlot(s32 displayIndex);

static bool IsAutoEntryPosition(const Entry& entry) {
    return entry.posX == 0 && entry.posY == 0;
}

void feCustomMenuMgr::BuildPages() {
    auto& feTitle = AddPage(MenuPage_Title, "FE_TTL", "Menu_Title", MenuPage_None, 0, false, -1, -1);
    if (SaveGameHasAutosave()) {
        SetEntries(feTitle, {
            Button("FE_CONT", EntryEvent_Continue),
            Button("FE_STG", EntryEvent_GoPage, MenuPage_StartGame),
            Button("FE_OPT", EntryEvent_GoPage, MenuPage_Options),
            Button("FE_QTG", EntryEvent_GoPage, MenuPage_QuitConfirm),
                   });
    }
    else {
        SetEntries(feTitle, {
            Button("FE_STG", EntryEvent_GoPage, MenuPage_StartGame),
            Button("FE_OPT", EntryEvent_GoPage, MenuPage_Options),
            Button("FE_QTG", EntryEvent_GoPage, MenuPage_QuitConfirm),
                   });
    }

    auto& feMain = AddPage(MenuPage_Frontend, "FE_MNM", "Menu_Title", MenuPage_None, 0, false, -1, -1);
    SetEntries(feMain, {
        Button("FE_RSM", EntryEvent_Resume),
        Button("FE_STG", EntryEvent_GoPage, MenuPage_StartGame),
        Button("FE_LDG", EntryEvent_Load),
        Button("FE_SVG", EntryEvent_Save),
        Button("FE_OPT", EntryEvent_GoPage, MenuPage_Options),
        Button("FE_QTG", EntryEvent_GoPage, MenuPage_QuitConfirm),
               });

    auto& feStart = AddPage(MenuPage_StartGame, "FE_STG", "Menu_GameOption", MenuPage_Frontend, 1, false, -1, -1);
    SetEntries(feStart, {
        Button("FE_NWG", EntryEvent_NewGame),
        Button("FE_LDG", EntryEvent_Load),
        Button("FE_SVG", EntryEvent_Save),
        Button("FE_DLG", EntryEvent_Delete),
        Button("FE_BCK", EntryEvent_Back),
               });

    auto& feOpts = AddPage(MenuPage_Options, "FE_OPT", "Menu_GameOption", MenuPage_Frontend, 2, false, -1, -1);
    SetEntries(feOpts, {
        Button("FE_CTL", EntryEvent_GoPage, MenuPage_Controller),
        // Key Bindings is a keyboard-remapping page; no console has a keyboard.
#if !defined(RC_PLATFORM_SWITCH)
        Button("FE_KBD", EntryEvent_GoPage, MenuPage_KeyBindings),
#endif
        Button("FE_DIS", EntryEvent_GoPage, MenuPage_Display),
        Button("FE_SND", EntryEvent_GoPage, MenuPage_Sound),
#if AUTO_UPDATER
        Button("FE_UPD", EntryEvent_GoPage, MenuPage_Update),
#endif
        //Button("FE_CRE", EntryEvent_Credits),
#if NEW_CHEATS
        Button("FE_CHEATS", EntryEvent_GoPage, MenuPage_Cheats),
#endif
#ifdef MOD_LOADER
        Button("FE_MODS", EntryEvent_GoPage, MenuPage_Mods),
#endif
        Button("FE_BCK", EntryEvent_Back),
               });

    auto& feCtrl = AddPage(MenuPage_Controller, "FE_CTL", "Menu_Controller", MenuPage_Options, 0, false, DEF_CONTROLLER_WINDOW_W, DEF_CONTROLLER_WINDOW_H);
    SetEntries(feCtrl, {
        Toggle("FE_CSH", EntryBinding_Shock),
        List("FE_CCF", EntryBinding_PlayerConfig, 1, 0, 2),
        List("FE_BPTS", EntryBinding_ControllerPromptStyle, 1, 0, ControllerPromptStyle_Count - 1),
        Button("FE_BCK", EntryEvent_Back),
               }, 0, 42);

    auto& feKeyBindings = AddPage(MenuPage_KeyBindings, "FE_KBD", "Menu_Controller", MenuPage_Controller, 1, false,
                                  DEF_KEYBIND_WINDOW_W, DEF_KEYBIND_WINDOW_H);
    SetEntries(feKeyBindings, {
                Button("FE_BCK", EntryEvent_Back),
               }, 0, 64);

    auto& feDisplay = AddPage(MenuPage_Display, "FE_DIS", "Menu_GameOption", MenuPage_Options, 2, false, -1, -1);
    SetEntries(feDisplay, {
#if !defined(RC_PLATFORM_SWITCH)
        List("FE_RES", EntryBinding_DisplayResolution, 1, 0, 64),
        List("FE_FSC", EntryBinding_DisplayScreenMode, 1, 0, 2),
#endif
        Toggle("FE_VYS", EntryBinding_DisplayVsync),
#if HIGH_FPS_PLAY_PRESENTATION
        List("FE_FPS", EntryBinding_DisplayFrameRate, 1, 0, 3),
#endif
#if !defined(RC_PLATFORM_SWITCH)
        List("FE_MSA", EntryBinding_DisplayMsaa, 1, 0, 4),
#endif
#if MODERN_GRAPHICS && !defined(RC_PLATFORM_SWITCH)
        List("FE_DSH", EntryBinding_DisplayShadowQuality, 1, 0, 4),
#endif
        List("FE_LNG", EntryBinding_Language, 1, 0, (s32)NumLanguages - 1),
        Button("FE_BCK", EntryEvent_Back),
               });

    auto& feSnd = AddPage(MenuPage_Sound, "FE_SND", "Menu_Sound", MenuPage_Options, 3, false, -1, -1);
    SetEntries(feSnd, {
        Slider("FE_EFV", EntryBinding_EffectsVol, 10, 0, 100),
        Slider("FE_MSV", EntryBinding_MusicVol,   10, 0, 100),
        Slider("FE_VCV", EntryBinding_DialogVol,  10, 0, 100),
        Toggle("FE_STR", EntryBinding_Stereo),
        Button("FE_BCK", EntryEvent_Back),
               });

#if NEW_CHEATS
    auto& feCheats = AddPage(MenuPage_Cheats, "FE_CHEATS", "Menu_GameOption", MenuPage_Options, 0, false, -1, -1);
    SetEntries(feCheats, {
        Toggle("FE_CH_DRAG", EntryBinding_CheatAllDragons),
        Toggle("FE_CH_LEVL", EntryBinding_CheatAllLevels),
        Toggle("FE_CH_GOD", EntryBinding_CheatGodMode),
        Toggle("FE_CH_PNCH", EntryBinding_CheatOnePunchMan),
        Toggle("FE_CH_HVN", EntryBinding_CheatHeavenBound),
        Toggle("FE_CH_BOBL", EntryBinding_CheatBobbleHead),
        Toggle("FE_CH_QUAKE", EntryBinding_CheatStuntquake),
        Toggle("FE_CH_MIRR", EntryBinding_CheatMirrorWorld),
        Button("FE_BCK", EntryEvent_Back),
               });
#endif

#ifdef MOD_LOADER
    auto& feMods = AddPage(MenuPage_Mods, "FE_MODS", "Menu_GameOption", MenuPage_Options, 0, false,
                           DEF_MODS_WINDOW_W, DEF_MODS_WINDOW_H);
    SetEntries(feMods, {
        Button("FE_BCK", EntryEvent_Back),
               }, 0, 64);
#endif

    auto& feLoadSlots = AddPage(MenuPage_LoadSlots, "FE_LDG", "Menu_GameOption", MenuPage_StartGame, 1, false, 480, 220);
    SetEntries(feLoadSlots, {
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_NTD", EntryEvent_Load),
        Button("FE_BCK", EntryEvent_Back),
               });

    auto& feSaveSlots = AddPage(MenuPage_SaveSlots, "FE_SVG", "Menu_GameOption", MenuPage_StartGame, 2, false, 480, 220);
    SetEntries(feSaveSlots, {
        Button("FE_NTD", EntryEvent_Save),
        Button("FE_NTD", EntryEvent_Save),
        Button("FE_NTD", EntryEvent_Save),
        Button("FE_NTD", EntryEvent_Save),
        Button("FE_NTD", EntryEvent_Save),
        Button("FE_NTD", EntryEvent_Save),
        Button("FE_NTD", EntryEvent_Save),
        Button("FE_NTD", EntryEvent_Save),
        Info(""),
        Button("FE_BCK", EntryEvent_Back),
               });

    auto& feDeleteSlots = AddPage(MenuPage_DeleteSlots, "FE_DLG", "Menu_GameOption", MenuPage_StartGame, 3, false, 480, 220);
    SetEntries(feDeleteSlots, {
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_NTD", EntryEvent_Delete),
        Button("FE_BCK", EntryEvent_Back),
               });

    auto& feLoadConfirm = AddPage(MenuPage_LoadConfirm, "FE_LDG", "Menu_Confirmation", MenuPage_LoadSlots, 0, false, -1, -1);
    SetEntries(feLoadConfirm, {
        Info("FE_LDQ"),
        Button("FE_NO", EntryEvent_Back),
        Button("FE_YS", EntryEvent_LoadConfirmYes),
               });

    auto& feSaveConfirm = AddPage(MenuPage_SaveConfirm, "FE_SVG", "Menu_Confirmation", MenuPage_SaveSlots, 0, false, -1, -1);
    SetEntries(feSaveConfirm, {
        Info("FE_SVQ"),
        Button("FE_NO", EntryEvent_Back),
        Button("FE_YS", EntryEvent_SaveConfirmYes),
               });

    auto& feDeleteConfirm = AddPage(MenuPage_DeleteConfirm, "FE_DLG", "Menu_Confirmation", MenuPage_DeleteSlots, 0, false, -1, -1);
    SetEntries(feDeleteConfirm, {
        Info("FE_DLQ"),
        Button("FE_NO", EntryEvent_Back),
        Button("FE_YS", EntryEvent_DeleteConfirmYes),
               });

    auto& feSaveDone = AddPage(MenuPage_SaveDone, "FE_SVG", "Menu_Confirmation", MenuPage_SaveSlots, 0, false, -1, -1);
    SetEntries(feSaveDone, {
        Info("FE_SVD"),
        Button("FE_OK", EntryEvent_Back),
               });

    auto& feDeleteDone = AddPage(MenuPage_DeleteDone, "FE_DLG", "Menu_Confirmation", MenuPage_DeleteSlots, 0, false, -1, -1);
    SetEntries(feDeleteDone, {
        Info("FE_DLD"),
        Button("FE_OK", EntryEvent_Back),
               });

    auto& feNewGame = AddPage(MenuPage_NewGameConfirm, "FE_NWG", "Menu_Confirmation", MenuPage_Frontend, 0, false, -1, -1);
    SetEntries(feNewGame, {
        Info("FE_NGQ"),
        Button("FE_NO", EntryEvent_Back),
        Button("FE_YS", EntryEvent_NewGame),
               });

    auto& feExitLevel = AddPage(MenuPage_ExitLevelConfirm, "FE_EXL", "Menu_Confirmation", MenuPage_Pause, 2, true, -1, -1);
    SetEntries(feExitLevel, {
        Info("FE_EXLR"),
        Button("FE_NO", EntryEvent_Back),
        Button("FE_YS", EntryEvent_ExitToHub),
               });

    auto& feQuit = AddPage(MenuPage_QuitConfirm, "FE_QTG", "Menu_Confirmation", MenuPage_None, 0, false, -1, -1);
    SetEntries(feQuit, {
        Info("FE_XGM"),
        Button("FE_NO", EntryEvent_Back),
        Button("FE_YS", EntryEvent_QuitGame),
               });

    auto& feQuitting = AddPage(MenuPage_Quitting, "FE_QTG", "Menu_Confirmation", MenuPage_None, 0, false, -1, -1);
    SetEntries(feQuitting, {
        Info("FE_QUI"),
               });

    auto& pauseMain = AddPage(MenuPage_Pause, "FE_PSD", "Menu_GameOption", MenuPage_None, 0, true, -1, -1);
    SetEntries(pauseMain, {
        Button("FE_RSG", EntryEvent_Resume),
        Button("FE_SVG", EntryEvent_Save),
        Button("FE_LDG", EntryEvent_Load),
        Button("FE_OPT", EntryEvent_GoPage, MenuPage_Options),
        Button("FE_EXL", EntryEvent_GoPage, MenuPage_ExitLevelConfirm),
        Button("FE_QTG", EntryEvent_GoPage, MenuPage_QuitConfirm),
               });

    auto& feLocation = AddPage(MenuPage_Location, "", "Menu_Location", MenuPage_None, 0, false, DEF_WINDOW_W, 130);
    SetEntries(feLocation, {
        Button("", EntryEvent_LocationSelect),
               });

#if AUTO_UPDATER
    AddPage(MenuPage_Update, "FE_UPD", "Menu_GameOption", MenuPage_Title, 0, false, 380, -1);
    RefreshUpdatePageEntries();

    // Fully custom page: body text + scrollbar are hand-drawn in RenderChangelogBody,
    // and input is handled in its own Invoke() block, so it carries no real entries.
    AddPage(MenuPage_Changelog, "FE_UPD_NOTES", "Menu_GameOption", MenuPage_Update, 0, false,
            DEF_CHANGELOG_WINDOW_W, DEF_CHANGELOG_WINDOW_H);
#endif

    // Entries are populated by RefreshAssetPageEntries() via SetPage()'s refresh hook the
    // first time this page is actually shown - g_psxDiscExtractor doesn't exist yet here.
    AddPage(MenuPage_AssetMissing, "FE_ASSET_TITLE", "Menu_GameOption", MenuPage_None, 0, false, 380, -1);
}

void feCustomMenuMgr::BuildPopups() {
    auto& feAutosaveNotice = AddPopup(PopupKind_AutosaveNotice, "FE_WARN", "Menu_GameOption", 380);
    SetEntries(feAutosaveNotice, {
        Info("FE_AUTO_WARN"),
               });

#if AUTO_UPDATER
    auto& feCheckingUpdate = AddPopup(PopupKind_CheckingUpdate, "FE_UPD", "Menu_GameOption", 280);
    SetEntries(feCheckingUpdate, {
        Info("FE_UPD_CHK"),
               });
#endif

    auto& feAssetScanning = AddPopup(PopupKind_AssetScanning, "FE_ASSET_TITLE", "Menu_GameOption", 280);
    SetEntries(feAssetScanning, {
        Info("FE_ASSET_SCAN"),
               });

    auto& feAssetExtracting = AddPopup(PopupKind_AssetExtracting, "FE_ASSET_TITLE", "Menu_GameOption", 320);
    SetEntries(feAssetExtracting, {
        Info("FE_ASSET_EXTG"),
               });
}

void feCustomMenuMgr::Init(CustomText* textSystem) {
    m_text = textSystem;

    BuildPages();
    BuildPopups();
    LoadControllerOverlayTexture();
    LoadMenuOrnamentTexture();
    LoadSplashTextures();
    LoadSliderTextures();

    m_pulse.Start();
    SetPage(MenuPage_None);
}

void feCustomMenuMgr::Shutdown() {
    ReleaseTitleScreenEffects();

    if (m_titleScreenBackgroundTexture) {
        m_titleScreenBackgroundTexture->Release();
        m_titleScreenBackgroundTexture = nullptr;
    }
    if (m_titleScreenJackieTexture) {
        m_titleScreenJackieTexture->Release();
        m_titleScreenJackieTexture = nullptr;
    }
    if (m_titleScreenLogoTexture) {
        m_titleScreenLogoTexture->Release();
        m_titleScreenLogoTexture = nullptr;
    }
    if (m_gameOverTexture) {
        m_gameOverTexture->Release();
        m_gameOverTexture = nullptr;
    }
    if (m_loadingBarTexture) {
        m_loadingBarTexture->Release();
        m_loadingBarTexture = nullptr;
    }
    if (m_controllerTexture) {
        m_controllerTexture->Release();
        m_controllerTexture = nullptr;
    }
    if (m_menuOrnamentTexture) {
        m_menuOrnamentTexture->Release();
        m_menuOrnamentTexture = nullptr;
    }
    if (m_redDragonTex) {
        m_redDragonTex->Release();
        m_redDragonTex = nullptr;
    }
    if (m_goldDragonTex) {
        m_goldDragonTex->Release();
        m_goldDragonTex = nullptr;
    }
    if (m_greyDragonTex) {
        m_greyDragonTex->Release();
        m_greyDragonTex = nullptr;
    }
    if (m_takeTex) {
        m_takeTex->Release();
        m_takeTex = nullptr;
    }
    if (m_sliderOTex) {
        m_sliderOTex->Release();
        m_sliderOTex = nullptr;
    }
    if (m_sliderFTex) {
        m_sliderFTex->Release();
        m_sliderFTex = nullptr;
    }

    m_titleScreenTextureTried = false;
    m_gameOverTextureTried = false;
    m_loadingScreenTexturesTried = false;
    m_text = nullptr;
}

void feCustomMenuMgr::ReleaseTitleScreenEffects() {
    if (m_titleScreenLogoSeed) {
        m_titleScreenLogoSeed->Release();
        m_titleScreenLogoSeed = nullptr;
    }
    if (m_titleScreenGlow) {
        m_titleScreenGlow->Release();
        m_titleScreenGlow = nullptr;
    }
    if (m_titleScreenGodRays) {
        m_titleScreenGodRays->Release();
        m_titleScreenGodRays = nullptr;
    }
    if (m_titleScreenGlowShader) {
        m_titleScreenGlowShader->Release();
        m_titleScreenGlowShader = nullptr;
    }
    if (m_titleScreenGodRaysShader) {
        m_titleScreenGodRaysShader->Release();
        m_titleScreenGodRaysShader = nullptr;
    }
    if (m_titleScreenCompositeShader) {
        m_titleScreenCompositeShader->Release();
        m_titleScreenCompositeShader = nullptr;
    }
    if (m_titleScreenLogoTiltShader) {
        m_titleScreenLogoTiltShader->Release();
        m_titleScreenLogoTiltShader = nullptr;
    }
    if (m_titleDebrisDotShader) {
        m_titleDebrisDotShader->Release();
        m_titleDebrisDotShader = nullptr;
    }
    m_titleEffectTargetW = 0;
    m_titleEffectTargetH = 0;
}

bool feCustomMenuMgr::EnsureTitleScreenEffects(f32 drawW, f32 drawH) {
    if (m_titleScreenEffectsUnavailable || !p3d::device || !p3d::context) {
        return false;
    }

    if (!m_titleScreenGlowShader) {
        m_titleScreenGlowShader = p3d::device->NewShader("glow");
    }
    if (!m_titleScreenGodRaysShader) {
        m_titleScreenGodRaysShader = p3d::device->NewShader("godrays");
    }
    if (!m_titleScreenCompositeShader) {
        m_titleScreenCompositeShader = p3d::device->NewShader("simple");
    }
    if (!m_titleScreenLogoTiltShader) {
        m_titleScreenLogoTiltShader = p3d::device->NewShader("tilt");
    }
    if (!m_titleDebrisDotShader) {
        m_titleDebrisDotShader = p3d::device->NewShader("dot");
    }
    if (!m_titleScreenGlowShader || !m_titleScreenGodRaysShader
        || !m_titleScreenCompositeShader || !m_titleScreenLogoTiltShader
        || !m_titleDebrisDotShader) {
        m_titleScreenEffectsUnavailable = true;
        return false;
    }

    // The seed/glow/ray buffers are intentionally soft and never need full
    // display resolution. Preserve the splash rect's aspect while capping
    // the size.
    const f32 targetScale = std::min(1.0f,
                                     std::min(960.0f / std::max(drawW, 1.0f),
                                     540.0f / std::max(drawH, 1.0f)));
    const s32 targetW = std::max(1, (s32)(drawW * targetScale + 0.5f));
    const s32 targetH = std::max(1, (s32)(drawH * targetScale + 0.5f));

    if (m_titleScreenLogoSeed && m_titleScreenGlow && m_titleScreenGodRays
        && targetW == m_titleEffectTargetW && targetH == m_titleEffectTargetH) {
        return true;
    }

    if (m_titleScreenLogoSeed) {
        m_titleScreenLogoSeed->Release();
        m_titleScreenLogoSeed = nullptr;
    }
    if (m_titleScreenGlow) {
        m_titleScreenGlow->Release();
        m_titleScreenGlow = nullptr;
    }
    if (m_titleScreenGodRays) {
        m_titleScreenGodRays->Release();
        m_titleScreenGodRays = nullptr;
    }

    m_titleScreenLogoSeed = p3d::context->CreateRenderTarget(
        targetW, targetH, PDDI_RENDER_TARGET_RGBA16F);
    m_titleScreenGlow = p3d::context->CreateRenderTarget(
        targetW, targetH, PDDI_RENDER_TARGET_RGBA16F);
    m_titleScreenGodRays = p3d::context->CreateRenderTarget(
        targetW, targetH, PDDI_RENDER_TARGET_RGBA16F);
    if (!m_titleScreenLogoSeed || !m_titleScreenGlow || !m_titleScreenGodRays) {
        ReleaseTitleScreenEffects();
        m_titleScreenEffectsUnavailable = true;
        return false;
    }

    m_titleEffectTargetW = targetW;
    m_titleEffectTargetH = targetH;
    // Freshly (re)created render targets hold undefined GPU memory - force an
    // immediate refresh on the next draw instead of compositing garbage for up
    // to one throttle interval.
    m_titleEffectsLastUpdateSec = -1.0f;
    return true;
}

void feCustomMenuMgr::UpdateTitleDebrisParticles(f32 dt, f32 emitX, f32 emitY, f32 spanW) {
    constexpr f32 kSpawnRate = 6; // particles per second -- an intensive sideways spray
    constexpr f32 kGravity = 20.0f;
    constexpr f32 kDragPerSec = 0.8f; // velocity multiplier retained per second -- mild, so they carry far
    constexpr f32 kSpawnJitter = 256.0f; // px, keeps the burst centered but not pinpoint
    constexpr f32 kSideConeDeg = 32.0f; // spread either side of due left/due right

    // Speed is scaled to the logo's own width so the sparks travel all the
    // way out across it (and a bit past) regardless of screen resolution.
    const f32 speedBase = spanW * 0.04f;
    const f32 speedRange = spanW * 0.08f;

    m_titleDebrisSpawnAccum += dt * kSpawnRate;
    while (m_titleDebrisSpawnAccum >= 1.0f) {
        m_titleDebrisSpawnAccum -= 1.0f;

        for (s32 i = 0; i < kMaxTitleDebrisParticles; i++) {
            TitleDebrisParticle& p = m_titleDebrisParticles[i];
            if (p.life > 0.0f) {
                continue;
            }

            // Fire mostly due left or due right, with a narrow cone of spread
            // rather than a uniform circle, so the burst reads as sparks
            // shooting sideways out of the center instead of a soft puff.
            const f32 side = (rand() % 2 == 0) ? 0.0f : 180.0f;
            const f32 spreadDeg = side + ((f32)(rand() % (s32)(kSideConeDeg * 2.0f + 1.0f)) - kSideConeDeg);
            const f32 angle = spreadDeg * 0.0174532925f;
            const f32 speed = speedBase + (f32)(rand() % 1000) * 0.001f * speedRange;

            p.x = emitX + ((f32)(rand() % (s32)(kSpawnJitter * 2.0f + 1.0f)) - kSpawnJitter);
            p.y = emitY; //+ ((f32)(rand() % (s32)(kSpawnJitter * 2.0f + 1.0f)) - kSpawnJitter);
            p.vx = std::cos(angle) * speed;
            p.vy = std::sin(angle) * speed - 30.0f;
            p.maxLife = 5.0f;//0.5f + (f32)(rand() % 60) * 0.01f;
            p.life = p.maxLife;
            p.size = 4.0f + (f32)(rand() % 22); // big, randomly sized chips (4-25px)
            p.shapeSeed = (f32)(rand() % 1000) * 0.01f;
            p.r = 255;
            p.g = (u8)(165 + rand() % 55);
            p.b = (u8)(30 + rand() % 65);
            break;
        }
    }

    const f32 drag = std::pow(kDragPerSec, dt);
    for (s32 i = 0; i < kMaxTitleDebrisParticles; i++) {
        TitleDebrisParticle& p = m_titleDebrisParticles[i];
        if (p.life <= 0.0f) {
            continue;
        }

        p.life -= dt;
        if (p.life <= 0.0f) {
            continue;
        }

        p.vy += kGravity * dt;
        p.vx *= drag;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
    }
}

void feCustomMenuMgr::DrawTitleDebrisParticles() const {
    for (s32 i = 0; i < kMaxTitleDebrisParticles; i++) {
        const TitleDebrisParticle& p = m_titleDebrisParticles[i];
        if (p.life <= 0.0f) {
            continue;
        }

        const f32 t01 = std::min(1.0f, p.life / p.maxLife);
        const u8 a = (u8)(255.0f * t01);
        const f32 dotSize = p.size * (0.5f + 0.5f * t01);

        if (m_titleDebrisDotShader) {
            m_titleDebrisDotShader->SetColour(0, pddiColour(p.r, p.g, p.b, a));
            m_titleDebrisDotShader->SetFloat("uShapeSeed", p.shapeSeed);
            ScreenDraw::DrawShaderQuad(m_titleDebrisDotShader,
                                       p.x - dotSize * 0.5f, p.y - dotSize * 0.5f, dotSize, dotSize,
                                       0.0f, 0.0f, 1.0f, 1.0f, PDDI_BLEND_ALPHA);
        }
        else {
            ScreenDraw::DrawColoredRect(p.x - dotSize * 0.5f, p.y - dotSize * 0.5f,
                                        dotSize, dotSize, p.r, p.g, p.b, a);
        }
    }
}

// Tracks mouse movement/clicks to keep cursor visibility in sync on every page,
// including the early-return "popup" pages below (Quitting, CheckingUpdate,
// Changelog, Asset Scanning/Extracting) that don't run the generic list-page
// input loop further down - without this, pressing a key while one of those is
// showing hides the cursor and it never gets reactivated since those blocks
// return before reaching the generic mouse-move/click handling.
void feCustomMenuMgr::UpdateMouseCursorVisibility() {
    if (!g_actionInput)
        return;

    double sx = 0.0;
    double sy = 0.0;
    g_actionInput->GetMousePosition(sx, sy);

    bool mouseMoved = false;
    if (!m_mousePosInitialized) {
        m_lastMouseX = sx;
        m_lastMouseY = sy;
        m_mousePosInitialized = true;
    }
    else if (m_mouseInputActive) {
        mouseMoved = (std::fabs(sx - m_lastMouseX) > 0.5) || (std::fabs(sy - m_lastMouseY) > 0.5);
        m_lastMouseX = sx;
        m_lastMouseY = sy;
    }
    else {
        mouseMoved = (std::fabs(sx - m_lastMouseX) > 4.0) || (std::fabs(sy - m_lastMouseY) > 4.0);
        if (mouseMoved) {
            m_lastMouseX = sx;
            m_lastMouseY = sy;
        }
    }

    const bool leftClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Left);
    const bool rightClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Right);
    const bool nonMouseInput = g_actionInput->HadKeyboardInputThisFrame() || g_actionInput->HadGamepadInputThisFrame();

    if ((mouseMoved || leftClick || rightClick) && !nonMouseInput) {
        if (!m_mouseInputActive && g_display) {
            g_display->SetCursorVisible(true);
        }
        m_mouseInputActive = true;
    }

    if (nonMouseInput && m_mouseInputActive) {
        m_mouseInputActive = false;
        if (g_display) {
            g_display->SetCursorVisible(false);
        }
    }
}

static constexpr f32 kPopupFadeSec = 0.15f;
static constexpr f32 kPopupCloseGapSec = 0.2f;

void feCustomMenuMgr::OpenPopup(PopupKind kind, f32 minTimerSec, std::function<s32()> poll) {
    m_activePopup = kind;
    m_popupFadeSec = 0.0f;
    m_popupClosing = false;
    m_popupMinTimer = minTimerSec;
    m_popupPoll = std::move(poll);
}

void feCustomMenuMgr::ClosePopup(s32 closeResult) {
    if (m_popupClosing)
        return;
    m_popupClosing = true;
    m_popupFadeSec = 0.0f;
    m_popupCloseResult = closeResult;
}

f32 feCustomMenuMgr::GetPopupFadeAlpha() const {
    if (m_activePopup == PopupKind_None)
        return 1.0f;

    f32 t = (kPopupFadeSec > 0.0f) ? (m_popupFadeSec / kPopupFadeSec) : 1.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return m_popupClosing ? (1.0f - t) : t;
}

s32 feCustomMenuMgr::Invoke() {
    if (!m_active)
        return (s32)GameResult::ResumePlay;

    DialogPreview::Update();

    m_result = 1;
    UpdateMouseCursorVisibility();

    // Popup overlay active: hold all normal page input/navigation until it
    // closes. Closing fade (+ post-fade gap) plays out first; GetPopupFadeAlpha()
    // clamps its ratio to kPopupFadeSec, so alpha stays at 0 for the gap portion.
    if (m_activePopup != PopupKind_None) {
        const f32 dt = g_time ? g_time->GetDeltaTime() : (1.0f / 30.0f);
        if (m_popupClosing) {
            m_popupFadeSec += dt;
            if (m_popupFadeSec >= kPopupFadeSec + kPopupCloseGapSec) {
                m_popupClosing = false;
                m_popupFadeSec = 0.0f;
                m_activePopup = PopupKind_None;
                m_result = m_popupCloseResult;
                m_popupPoll = nullptr;
                // An async scan/extract just finished behind the popup; rebuild the
                // asset page so its buttons reflect the new state (e.g. ScanFoundOne
                // -> real Extract button) instead of the stale pre-scan entries.
                if (m_currPage == MenuPage_AssetMissing) {
                    RefreshAssetPageEntries();
                }
            }
            return m_result;
        }

        // Fade-in ramp while open and not yet closing (harmless once it
        // overshoots kPopupFadeSec - GetPopupFadeAlpha() clamps to 1.0).
        m_popupFadeSec += dt;
        if (m_popupMinTimer > 0.0f) {
            m_popupMinTimer -= dt;
        }
        if (m_popupMinTimer <= 0.0f && m_popupPoll) {
            const s32 r = m_popupPoll();
            if (r >= 0) {
                ClosePopup(r);
            }
        }
        return m_result;
    }

    // Quitting countdown: no input processed, just tick down then close.
    if (m_currPage == MenuPage_Quitting) {
        if (m_quitTimerSec > 0.0f) {
            const f32 dt = g_time ? g_time->GetDeltaTime() : (1.0f / 30.0f);
            m_quitTimerSec -= dt;
        }
        if (m_quitTimerSec <= 0.0f) {
            m_quitTimerSec = 0.0f;
            if (g_game)
                g_game->SetState(GameState::End);
        }
        return m_result;
    }

#if AUTO_UPDATER
    // Changelog: read-only scrolling text, no cursor/selection - handle Up/Down/Back directly.
    if (m_currPage == MenuPage_Changelog) {
        if (!g_actionInput)
            return m_result;

        const s32 totalLines = (s32)m_changelogLines.size();
        const s32 visibleLines = ComputeChangelogVisibleLines(m_pages[MenuPage_Changelog].frameH);
        const s32 maxScrollTop = (totalLines > visibleLines) ? (totalLines - visibleLines) : 0;
        if (!m_scrollBarDragging) SmoothScrollToward(m_changelogScrollVisual, (f32)m_changelogScrollTop);
        const s32 scroll = g_actionInput->ConsumeScrollDelta();
        bool scrollBarUsed = false;

        if (m_mouseInputActive) {
            double sx = 0.0, sy = 0.0;
            g_actionInput->GetMousePosition(sx, sy);
            const f32 screenW = g_display ? (f32)g_display->GetScreenWidth() : DEFAULT_SCREEN_WIDTH;
            const f32 screenH = g_display ? (f32)g_display->GetScreenHeight() : DEFAULT_SCREEN_HEIGHT;
            const f32 aspect = g_display ? g_display->GetAspectRatio() : (4.0f / 3.0f);
#if FIX_ASPECT_RATIO
            const f32 effectiveW = screenW * DEFAULT_ASPECT_RATIO / aspect;
            const f32 offsetX = (screenW - effectiveW) * 0.5f;
            const f32 psxX = ((f32)sx - offsetX) * DEFAULT_SCREEN_WIDTH / effectiveW;
#else
            const f32 psxX = (f32)sx * DEFAULT_SCREEN_WIDTH / screenW;
#endif
            const f32 psxY = (f32)sy * DEFAULT_SCREEN_HEIGHT / screenH;
            const PageDef& page = m_pages[MenuPage_Changelog];
            const s32 panelX = DEF_WINDOW_CENTER_X - page.frameW / 2;
            const s32 panelY = DEF_WINDOW_CENTER_Y - page.frameH / 2;
            const s32 contentTop = panelY + DEF_TITLE_BAR_H + DEF_CONTENT_TOP_PAD;
            scrollBarUsed = HandleScrollBarMouse((f32)(panelX + page.frameW - 12),
                                                 (f32)(contentTop + DEF_CONTENT_PAD),
                                                 (f32)(visibleLines * 8), totalLines, visibleLines,
                                                 &m_changelogScrollTop, &m_changelogScrollVisual, psxX, psxY,
                                                 g_actionInput->IsMouseButtonTriggered(MouseBtn::Left),
                                                 g_actionInput->IsMouseButtonDown(MouseBtn::Left));
        }

        if (scrollBarUsed) {
            // Dragging and track clicks already updated the offset.
        }
        else if (g_actionInput->JustPressed(ACTION_MENU_UP) && m_changelogScrollTop > 0) {
            m_changelogScrollTop--;
            PlaySound(FE_SND_MENU_7);
        }
        else if (g_actionInput->JustPressed(ACTION_MENU_DOWN) && m_changelogScrollTop < maxScrollTop) {
            m_changelogScrollTop++;
            PlaySound(FE_SND_MENU_7);
        }
        else if (scroll != 0) {
            // Mouse-wheel scrolling plays no sound (unlike Up/Down keys above).
            const s32 dir = (scroll > 0) ? -1 : 1;
            const s32 scrollSteps = (scroll > 0) ? scroll : -scroll;

            for (s32 step = 0; step < scrollSteps; step++) {
                const s32 next = m_changelogScrollTop + dir;
                if (next < 0 || next > maxScrollTop) break;
                m_changelogScrollTop = next;
            }
        }
        else if (g_actionInput->JustPressed(ACTION_MENU_BACK) || g_actionInput->JustPressed(ACTION_MENU_CONFIRM)
                 || g_actionInput->IsMouseButtonTriggered(MouseBtn::Right)) {
            PlaySound(FE_SND_MENU_5);
            GoBack();
        }
        return m_result;
    }

    if (m_currPage == MenuPage_Update && g_autoUpdater && g_autoUpdater->GetState() != m_lastUpdateState) {
        RefreshUpdatePageEntries();
    }
#endif

    if (!g_actionInput)
        return m_result;

    if (IsSaveSlotPage(m_currPage)) {
        const PageDef& page = m_pages[m_currPage];
        const bool autosaveSelectable = m_currPage != MenuPage_SaveSlots;
        const s32 backIndex = SAVEGAME_VISIBLE_SLOT_COUNT;
        if (!m_scrollBarDragging) SmoothScrollToward(m_saveSlotScrollVisual, (f32)m_saveSlotScrollTop);

        auto moveSelection = [&](s32 dir) {
            if (dir < 0) {
                if (m_cursor == backIndex) m_cursor = SAVEGAME_SLOT_COUNT - 1;
                else if (m_cursor > 0 && m_cursor < SAVEGAME_SLOT_COUNT) --m_cursor;
                else if (m_cursor == 0) m_cursor = autosaveSelectable ? SAVEGAME_AUTOSAVE_SLOT : backIndex;
                else if (m_cursor == SAVEGAME_AUTOSAVE_SLOT) m_cursor = backIndex;
                else m_cursor = autosaveSelectable ? SAVEGAME_AUTOSAVE_SLOT : 0;
            }
            else {
                if (m_cursor == SAVEGAME_AUTOSAVE_SLOT) m_cursor = 0;
                else if (m_cursor >= 0 && m_cursor < SAVEGAME_SLOT_COUNT - 1) ++m_cursor;
                else if (m_cursor == SAVEGAME_SLOT_COUNT - 1) m_cursor = backIndex;
                else if (m_cursor == backIndex) m_cursor = autosaveSelectable ? SAVEGAME_AUTOSAVE_SLOT : 0;
                else m_cursor = autosaveSelectable ? SAVEGAME_AUTOSAVE_SLOT : 0;
            }
            ClampSaveSlotScroll();
        };

        ClampSaveSlotScroll();
        const bool nonMouseInput = g_actionInput->HadKeyboardInputThisFrame()
            || g_actionInput->HadGamepadInputThisFrame();
        const s32 scroll = g_actionInput->ConsumeScrollDelta();
        const bool leftClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Left);
        const bool leftDown = g_actionInput->IsMouseButtonDown(MouseBtn::Left);
        const bool rightClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Right);
        s32 hoverManualRow = -1;
        bool scrollBarUsed = false;

        double sx = 0.0, sy = 0.0;
        g_actionInput->GetMousePosition(sx, sy);
        if (m_mouseInputActive) {
            const f32 screenW = g_display ? (f32)g_display->GetScreenWidth() : DEFAULT_SCREEN_WIDTH;
            const f32 screenH = g_display ? (f32)g_display->GetScreenHeight() : DEFAULT_SCREEN_HEIGHT;
            const f32 aspect = g_display ? g_display->GetAspectRatio() : (4.0f / 3.0f);
#if FIX_ASPECT_RATIO
            const f32 effectiveW = screenW * DEFAULT_ASPECT_RATIO / aspect;
            const f32 offsetX = (screenW - effectiveW) * 0.5f;
            const f32 psxX = ((f32)sx - offsetX) * DEFAULT_SCREEN_WIDTH / effectiveW;
#else
            const f32 psxX = (f32)sx * DEFAULT_SCREEN_WIDTH / screenW;
#endif
            const f32 psxY = (f32)sy * DEFAULT_SCREEN_HEIGHT / screenH;
            const s32 panelX = DEF_WINDOW_CENTER_X - page.frameW / 2;
            const s32 panelY = DEF_WINDOW_CENTER_Y - page.frameH / 2;
            s32 hovered = -1;
            scrollBarUsed = HandleScrollBarMouse((f32)(panelX + page.frameW - 12),
                                                 (f32)(panelY + 48),
                                                 (f32)(DEF_SAVE_VISIBLE_ROWS * DEF_SAVE_ROW_H
                                                 + DEF_SAVE_AUTOSAVE_GAP),
                                                 SAVEGAME_VISIBLE_SLOT_COUNT, DEF_SAVE_VISIBLE_ROWS,
                                                 &m_saveSlotScrollTop, &m_saveSlotScrollVisual, psxX, psxY,
                                                 leftClick, leftDown);
            if (scrollBarUsed && m_cursor >= 0 && m_cursor < SAVEGAME_VISIBLE_SLOT_COUNT) {
                s32 displayIndex = SaveSlotToDisplayIndex(m_cursor);
                if (displayIndex < m_saveSlotScrollTop) displayIndex = m_saveSlotScrollTop;
                if (displayIndex >= m_saveSlotScrollTop + DEF_SAVE_VISIBLE_ROWS) {
                    displayIndex = m_saveSlotScrollTop + DEF_SAVE_VISIBLE_ROWS - 1;
                }
                m_cursor = SaveDisplayIndexToSlot(displayIndex);
                if (!autosaveSelectable && m_cursor == SAVEGAME_AUTOSAVE_SLOT) {
                    m_cursor = 0;
                }
            }

            if (!scrollBarUsed && psxX >= panelX + DEF_SAVE_TABLE_SIDE_PAD
                && psxX < panelX + page.frameW - DEF_SAVE_TABLE_SIDE_PAD) {
                const s32 manualTop = panelY + 48;
                const f32 viewportBottom = (f32)(manualTop + DEF_SAVE_VISIBLE_ROWS * DEF_SAVE_ROW_H
                                                 + DEF_SAVE_AUTOSAVE_GAP);
                for (s32 displayIndex = 0; displayIndex < SAVEGAME_VISIBLE_SLOT_COUNT; ++displayIndex) {
                    const f32 rowTop = (f32)manualTop + GetSaveDisplayRowOffset(displayIndex)
                        - GetSaveDisplayScrollOffset(m_saveSlotScrollVisual);
                    if (rowTop < manualTop || rowTop + DEF_SAVE_ROW_H > viewportBottom) continue;
                    if (psxY >= rowTop && psxY < rowTop + DEF_SAVE_ROW_H - 2.0f) {
                        hoverManualRow = displayIndex - m_saveSlotScrollTop;
                        const s32 candidate = SaveDisplayIndexToSlot(displayIndex);
                        if (candidate != SAVEGAME_AUTOSAVE_SLOT || autosaveSelectable) {
                            hovered = candidate;
                        }
                        break;
                    }
                }

                const s32 backTop = panelY + 188;
                if (psxY >= backTop && psxY < backTop + 14) {
                    hovered = backIndex;
                }
            }

            if (hovered >= 0 && hovered != m_cursor) {
                m_cursor = hovered;
                ClampSaveSlotScroll();
                PlaySound(FE_SND_MENU_7);
            }
            if (!scrollBarUsed && leftClick && hovered >= 0) {
                PlaySound(FE_SND_MENU_5);
                Confirm();
                return m_result;
            }
            if (rightClick) {
                PlaySound(FE_SND_MENU_5);
                GoBack();
                return m_result;
            }
        }

        if (scroll != 0) {
            const s32 oldScrollTop = m_saveSlotScrollTop;
            const s32 maxScrollTop = SAVEGAME_VISIBLE_SLOT_COUNT - DEF_SAVE_VISIBLE_ROWS;
            const s32 dir = scroll > 0 ? -1 : 1;
            const s32 steps = scroll > 0 ? scroll : -scroll;
            for (s32 step = 0; step < steps; ++step) {
                m_saveSlotScrollTop += dir;
                if (m_saveSlotScrollTop < 0) {
                    m_saveSlotScrollTop = 0;
                    break;
                }
                if (m_saveSlotScrollTop > maxScrollTop) {
                    m_saveSlotScrollTop = maxScrollTop;
                    break;
                }
            }

            if (m_saveSlotScrollTop != oldScrollTop) {
                // Match the key-binding table: the viewport moves while the
                // highlight stays under the same physical mouse row.
                if (hoverManualRow >= 0) {
                    m_cursor = SaveDisplayIndexToSlot(m_saveSlotScrollTop + hoverManualRow);
                    if (!autosaveSelectable && m_cursor == SAVEGAME_AUTOSAVE_SLOT) {
                        m_cursor = 0;
                    }
                }
                else if (m_cursor >= 0 && m_cursor < SAVEGAME_VISIBLE_SLOT_COUNT) {
                    s32 displayIndex = SaveSlotToDisplayIndex(m_cursor);
                    if (displayIndex < m_saveSlotScrollTop) displayIndex = m_saveSlotScrollTop;
                    if (displayIndex >= m_saveSlotScrollTop + DEF_SAVE_VISIBLE_ROWS) {
                        displayIndex = m_saveSlotScrollTop + DEF_SAVE_VISIBLE_ROWS - 1;
                    }
                    m_cursor = SaveDisplayIndexToSlot(displayIndex);
                }
                if (!autosaveSelectable && m_cursor == SAVEGAME_AUTOSAVE_SLOT) {
                    m_cursor = 0;
                }
                // Mouse-wheel scrolling plays no sound (unlike keyboard/gamepad nav below).
            }
        }
        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_UP)) {
            moveSelection(-1);
            PlaySound(FE_SND_MENU_7);
        }
        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_DOWN)) {
            moveSelection(1);
            PlaySound(FE_SND_MENU_7);
        }
        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_CONFIRM)) {
            PlaySound(FE_SND_MENU_5);
            Confirm();
        }
        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_BACK)) {
            PlaySound(FE_SND_MENU_5);
            GoBack();
        }
        if (nonMouseInput && m_mouseInputActive) {
            m_mouseInputActive = false;
            if (g_display) g_display->SetCursorVisible(false);
        }
        return m_result;
    }

    if (m_currPage == MenuPage_KeyBindings) {
        if (!m_scrollBarDragging) SmoothScrollToward(m_keyBindScrollVisual, (f32)m_keyBindScrollTop);
        if (m_keyBindActionCursor < 0 || m_keyBindActionCursor >= kKeyBindingActionCount) {
            m_keyBindActionCursor = 0;
        }

        if (m_keyBindSlotCursor < 0 || m_keyBindSlotCursor >= DEF_KEYBIND_SLOT_COUNT) {
            m_keyBindSlotCursor = 0;
        }

        auto isKeyBindBackSelected = [this]() {
            return m_cursor == 0;
        };

        auto setKeyBindBackSelected = [this](bool selected) {
            m_cursor = selected ? 0 : -1;
        };

        auto clampKeyBindScroll = [this]() {
            if (m_keyBindActionCursor < m_keyBindScrollTop) {
                m_keyBindScrollTop = m_keyBindActionCursor;
            }
            if (m_keyBindActionCursor >= m_keyBindScrollTop + DEF_KEYBIND_VISIBLE_ROWS) {
                m_keyBindScrollTop = m_keyBindActionCursor - (DEF_KEYBIND_VISIBLE_ROWS - 1);
            }

            const s32 maxScrollTop = (kKeyBindingActionCount > DEF_KEYBIND_VISIBLE_ROWS)
                ? (kKeyBindingActionCount - DEF_KEYBIND_VISIBLE_ROWS)
                : 0;
            if (m_keyBindScrollTop < 0) {
                m_keyBindScrollTop = 0;
            }
            if (m_keyBindScrollTop > maxScrollTop) {
                m_keyBindScrollTop = maxScrollTop;
            }
        };

        double sx = 0.0;
        double sy = 0.0;
        g_actionInput->GetMousePosition(sx, sy);

        bool mouseMoved = false;
        if (!m_mousePosInitialized) {
            m_lastMouseX = sx;
            m_lastMouseY = sy;
            m_mousePosInitialized = true;
        }
        else if (m_mouseInputActive) {
            mouseMoved = (std::fabs(sx - m_lastMouseX) > 0.5) || (std::fabs(sy - m_lastMouseY) > 0.5);
            m_lastMouseX = sx;
            m_lastMouseY = sy;
        }
        else {
            mouseMoved = (std::fabs(sx - m_lastMouseX) > 4.0) || (std::fabs(sy - m_lastMouseY) > 4.0);
            if (mouseMoved) {
                m_lastMouseX = sx;
                m_lastMouseY = sy;
            }
        }

        const bool leftClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Left);
        const bool leftDown = g_actionInput->IsMouseButtonDown(MouseBtn::Left);
        const bool rightClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Right);
        const s32 scroll = g_actionInput->ConsumeScrollDelta();
        // Key bindings page should respond only to keyboard/gamepad for non-mouse gating.
        const bool nonMouseInput = g_actionInput->HadKeyboardInputThisFrame() || g_actionInput->HadGamepadInputThisFrame();

        if ((mouseMoved || leftClick || rightClick || scroll != 0) && !nonMouseInput) {
            if (!m_mouseInputActive && g_display) {
                g_display->SetCursorVisible(true);
            }
            m_mouseInputActive = true;
        }

        if (m_keyBindCaptureActive) {
            if (m_keyBindCaptureBlockFrames > 0) {
                m_keyBindCaptureBlockFrames--;
                return m_result;
            }

            const s32 capturedKey = g_actionInput->GetTriggeredKeyThisFrame();
            const s32 capturedMouse = g_actionInput->GetTriggeredMouseButtonThisFrame();
            const Action action = (Action)m_keyBindActionCursor;

            if (capturedKey == KEY_ESCAPE ||
                (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_BACK))) {
                m_keyBindCaptureActive = false;
                PlaySound(FE_SND_MENU_5);
                return m_result;
            }

            if ((nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_CLEAR)) ||
                capturedKey == KEY_DELETE || capturedKey == KEY_BACKSPACE) {
                SetDesktopBindingCodeUnique(action, m_keyBindSlotCursor, 0);
                g_settings.Save(SETTINGS_PATH);
                m_keyBindCaptureActive = false;
                PlaySound(FE_SND_MENU_5);
                return m_result;
            }

            if (capturedKey != 0) {
                SetDesktopBindingCodeUnique(action, m_keyBindSlotCursor, ActionInput::EncodeKeyboardBindingCode(capturedKey));
                g_settings.Save(SETTINGS_PATH);
                m_keyBindCaptureActive = false;
                PlaySound(FE_SND_MENU_5);
                return m_result;
            }

            if (capturedMouse != MouseBtn::NONE) {
                SetDesktopBindingCodeUnique(action, m_keyBindSlotCursor, ActionInput::EncodeMouseBindingCode(capturedMouse));
                g_settings.Save(SETTINGS_PATH);
                m_keyBindCaptureActive = false;
                PlaySound(FE_SND_MENU_5);
                return m_result;
            }

            if (nonMouseInput && m_mouseInputActive) {
                m_mouseInputActive = false;
                if (g_display) {
                    g_display->SetCursorVisible(false);
                }
            }

            return m_result;
        }

        s32 hoverActionIndex = -1;
        s32 hoverMenuEntry = -1;
        s32 hoverRowIndex = -1;
        s32 hoverSlotIndex = m_keyBindSlotCursor;
        bool scrollBarUsed = false;

        if (m_mouseInputActive) {
            const f32 screenW = g_display ? (f32)g_display->GetScreenWidth() : DEFAULT_SCREEN_WIDTH;
            const f32 screenH = g_display ? (f32)g_display->GetScreenHeight() : DEFAULT_SCREEN_HEIGHT;
            const f32 aspect = g_display ? g_display->GetAspectRatio() : (4.0f / 3.0f);
#if FIX_ASPECT_RATIO
            const f32 effectiveW = screenW * DEFAULT_ASPECT_RATIO / aspect;
            const f32 offsetX = (screenW - effectiveW) * 0.5f;
            const f32 psxX = ((f32)sx - offsetX) * DEFAULT_SCREEN_WIDTH / effectiveW;
#else
            const f32 psxX = (f32)sx * DEFAULT_SCREEN_WIDTH / screenW;
#endif
            const f32 psxY = (f32)sy * DEFAULT_SCREEN_HEIGHT / screenH;

            const PageDef* page = &m_pages[m_currPage];
            const s32 panelX = DEF_WINDOW_CENTER_X - page->frameW / 2;
            const s32 panelY = DEF_WINDOW_CENTER_Y - page->frameH / 2;
            const s32 contentTop = panelY + DEF_TITLE_BAR_H + DEF_CONTENT_TOP_PAD;
            const s32 labelX = panelX + DEF_LABEL_X_PAD + DEF_KEYBIND_X_PAD;
            const s32 headerY = contentTop + DEF_CONTENT_PAD + DEF_TEXT_Y_OFF;
            const s32 firstRowY = headerY + DEF_KEYBIND_ROW_STEP;
            const s32 slotW = DEF_KEYBIND_SLOT_W;
            const s32 slotGap = DEF_KEYBIND_SLOT_GAP;
            const s32 slot2Right = panelX + page->frameW - DEF_VALUE_X_PAD - DEF_KEYBIND_X_PAD;
            const s32 slot2Left = slot2Right - slotW;
            const s32 slot1Right = slot2Left - slotGap;
            const s32 slot1Left = slot1Right - slotW;
            const s32 actionLabelRight = slot1Left - DEF_KEYBIND_ACTION_COL_GAP;
            scrollBarUsed = HandleScrollBarMouse((f32)(panelX + page->frameW - 12),
                                                 (f32)(firstRowY - DEF_KEYBIND_ROW_TOP_PAD),
                                                 (f32)(DEF_KEYBIND_VISIBLE_ROWS * DEF_KEYBIND_ROW_STEP),
                                                 kKeyBindingActionCount, DEF_KEYBIND_VISIBLE_ROWS,
                                                 &m_keyBindScrollTop, &m_keyBindScrollVisual, psxX, psxY,
                                                 leftClick, leftDown);
            if (scrollBarUsed) {
                if (m_keyBindActionCursor < m_keyBindScrollTop) m_keyBindActionCursor = m_keyBindScrollTop;
                if (m_keyBindActionCursor >= m_keyBindScrollTop + DEF_KEYBIND_VISIBLE_ROWS) {
                    m_keyBindActionCursor = m_keyBindScrollTop + DEF_KEYBIND_VISIBLE_ROWS - 1;
                }
                setKeyBindBackSelected(false);
            }

            if (!scrollBarUsed && psxX >= (f32)panelX && psxX < (f32)(panelX + page->frameW)) {
                const f32 viewportTop = (f32)(firstRowY - DEF_KEYBIND_ROW_TOP_PAD);
                const f32 viewportBottom = viewportTop + DEF_KEYBIND_VISIBLE_ROWS * DEF_KEYBIND_ROW_STEP;
                for (s32 actionIndex = 0; actionIndex < kKeyBindingActionCount; ++actionIndex) {
                    const f32 rowTop = (f32)firstRowY
                        + ((f32)actionIndex - m_keyBindScrollVisual) * DEF_KEYBIND_ROW_STEP
                        - DEF_KEYBIND_ROW_TOP_PAD;
                    const f32 rowBottom = rowTop + DEF_KEYBIND_ROW_STEP;
                    if (rowTop < viewportTop || rowBottom > viewportBottom
                        || psxY < rowTop || psxY >= rowBottom) {
                        continue;
                    }
                    hoverRowIndex = actionIndex - m_keyBindScrollTop;

                    if (psxX >= (f32)(slot2Left - DEF_KEYBIND_HIT_PAD) && psxX < (f32)(slot2Left + slotW + DEF_KEYBIND_HIT_PAD)) {
                        hoverSlotIndex = 1;
                    }
                    else if (psxX >= (f32)(slot1Left - DEF_KEYBIND_HIT_PAD) && psxX < (f32)(slot1Left + slotW + DEF_KEYBIND_HIT_PAD)) {
                        hoverSlotIndex = 0;
                    }
                    else if (psxX >= (f32)(labelX - DEF_KEYBIND_CELL_PAD - DEF_KEYBIND_HIT_PAD) && psxX < (f32)actionLabelRight) {
                        hoverSlotIndex = m_keyBindSlotCursor;
                    }
                    else {
                        continue;
                    }

                    hoverActionIndex = actionIndex;
                    break;
                }

                if (page->numEntries > 0) {
                    const s32 rowSpan = (page->numEntries > 0) ? ((page->numEntries - 1) * DEF_ROW_STEP) : 0;
                    const s32 extraH = CalcPageExtraHeight(*page);
                    const s32 entryBlockH = DEF_CONTENT_PAD + rowSpan + DEF_ROW_TEXT_H + extraH;
                    const s32 bodyAvailH = page->frameH - DEF_TITLE_BAR_H - DEF_BOTTOM_BAR_H - DEF_CONTENT_TOP_PAD - DEF_CONTENT_BOTTOM_PAD;
                    const s32 bodyCenterPad = (bodyAvailH > entryBlockH) ? ((bodyAvailH - entryBlockH) / 2) : 0;
                    const s32 menuFirstY = panelY + DEF_TITLE_BAR_H + DEF_CONTENT_TOP_PAD + bodyCenterPad + DEF_CONTENT_PAD;
                    const s32 menuLabelX = panelX + DEF_LABEL_X_PAD;
                    const s32 menuValueX = panelX + page->frameW - DEF_VALUE_X_PAD;
                    const s32 menuCenterX = DEF_WINDOW_CENTER_X;

                    s32 backRowTop = 0;
                    ResolveEntryLayout(*page, 0,
                                       menuFirstY, menuLabelX, menuValueX, menuCenterX,
                                       &backRowTop, nullptr, nullptr, nullptr, nullptr);

                    const s32 backRowH = DEF_ROW_STEP + GetEntryExtraHeight(*page, page->entries[0]);
                    if (psxY >= (f32)backRowTop && psxY < (f32)(backRowTop + backRowH)) {
                        hoverMenuEntry = 0;
                    }
                }
            }

            if (hoverActionIndex >= 0 && isKeyBindBackSelected()) {
                setKeyBindBackSelected(false);
                PlaySound(FE_SND_MENU_7);
            }

            if (hoverActionIndex >= 0 && hoverActionIndex != m_keyBindActionCursor) {
                m_keyBindActionCursor = hoverActionIndex;
                clampKeyBindScroll();
                PlaySound(FE_SND_MENU_7);
            }

            if (hoverActionIndex >= 0 && hoverSlotIndex != m_keyBindSlotCursor) {
                m_keyBindSlotCursor = hoverSlotIndex;
                PlaySound(FE_SND_MENU_7);
            }

            if (hoverActionIndex < 0 && hoverMenuEntry == 0 && !isKeyBindBackSelected()) {
                setKeyBindBackSelected(true);
                PlaySound(FE_SND_MENU_7);
            }

            if (scroll != 0) {
                if (isKeyBindBackSelected()) {
                    setKeyBindBackSelected(false);
                }

                const s32 oldScrollTop = m_keyBindScrollTop;
                const s32 maxScrollTop = (kKeyBindingActionCount > DEF_KEYBIND_VISIBLE_ROWS)
                    ? (kKeyBindingActionCount - DEF_KEYBIND_VISIBLE_ROWS)
                    : 0;
                const s32 dir = (scroll > 0) ? -1 : 1;
                const s32 scrollSteps = (scroll > 0) ? scroll : -scroll;

                for (s32 step = 0; step < scrollSteps; step++) {
                    m_keyBindScrollTop += dir;
                    if (m_keyBindScrollTop < 0) {
                        m_keyBindScrollTop = 0;
                        break;
                    }
                    if (m_keyBindScrollTop > maxScrollTop) {
                        m_keyBindScrollTop = maxScrollTop;
                        break;
                    }
                }

                if (m_keyBindScrollTop != oldScrollTop) {
                    if (hoverRowIndex >= 0) {
                        m_keyBindActionCursor = m_keyBindScrollTop + hoverRowIndex;
                    }

                    if (m_keyBindActionCursor < m_keyBindScrollTop) {
                        m_keyBindActionCursor = m_keyBindScrollTop;
                    }
                    if (m_keyBindActionCursor >= m_keyBindScrollTop + DEF_KEYBIND_VISIBLE_ROWS) {
                        m_keyBindActionCursor = m_keyBindScrollTop + (DEF_KEYBIND_VISIBLE_ROWS - 1);
                    }
                    if (m_keyBindActionCursor >= kKeyBindingActionCount) {
                        m_keyBindActionCursor = kKeyBindingActionCount - 1;
                    }

                    // Mouse-wheel scrolling plays no sound (unlike keyboard/gamepad nav).
                }
            }

            if (!scrollBarUsed && leftClick && hoverActionIndex >= 0) {
                setKeyBindBackSelected(false);
                m_keyBindCaptureActive = true;
                m_keyBindCaptureBlockFrames = 1;
                PlaySound(FE_SND_MENU_5);
                return m_result;
            }

            if (!scrollBarUsed && leftClick && hoverMenuEntry == 0) {
                setKeyBindBackSelected(true);
                PlaySound(FE_SND_MENU_5);
                Confirm();
                return m_result;
            }

            if (rightClick) {
                PlaySound(FE_SND_MENU_5);
                GoBack();
                return m_result;
            }
        }

        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_UP)) {
            if (isKeyBindBackSelected()) {
                setKeyBindBackSelected(false);
                m_keyBindActionCursor = kKeyBindingActionCount - 1;
                clampKeyBindScroll();
            }
            else if (m_keyBindActionCursor > 0) {
                m_keyBindActionCursor--;
                clampKeyBindScroll();
            }
            else {
                setKeyBindBackSelected(true);
            }
            PlaySound(FE_SND_MENU_7);
        }

        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_DOWN)) {
            if (isKeyBindBackSelected()) {
                setKeyBindBackSelected(false);
                m_keyBindActionCursor = 0;
                clampKeyBindScroll();
            }
            else if (m_keyBindActionCursor < (kKeyBindingActionCount - 1)) {
                m_keyBindActionCursor++;
                clampKeyBindScroll();
            }
            else {
                setKeyBindBackSelected(true);
            }
            PlaySound(FE_SND_MENU_7);
        }

        if (nonMouseInput && !isKeyBindBackSelected() && g_actionInput->JustPressed(ACTION_MENU_LEFT)) {
            m_keyBindSlotCursor--;
            if (m_keyBindSlotCursor < 0) {
                m_keyBindSlotCursor = DEF_KEYBIND_SLOT_COUNT - 1;
            }
            PlaySound(FE_SND_MENU_7);
        }

        if (nonMouseInput && !isKeyBindBackSelected() && g_actionInput->JustPressed(ACTION_MENU_RIGHT)) {
            m_keyBindSlotCursor++;
            if (m_keyBindSlotCursor >= DEF_KEYBIND_SLOT_COUNT) {
                m_keyBindSlotCursor = 0;
            }
            PlaySound(FE_SND_MENU_7);
        }

        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_CONFIRM)) {
            PlaySound(FE_SND_MENU_5);
            if (isKeyBindBackSelected()) {
                Confirm();
            }
            else {
                m_keyBindCaptureActive = true;
                m_keyBindCaptureBlockFrames = 1;
            }
        }

        const s32 clearKey = g_actionInput->GetTriggeredKeyThisFrame();
        if (!isKeyBindBackSelected() && ((nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_CLEAR)) ||
            clearKey == KEY_DELETE || clearKey == KEY_BACKSPACE)) {
            const Action action = (Action)m_keyBindActionCursor;
            SetDesktopBindingCodeUnique(action, m_keyBindSlotCursor, 0);
            g_settings.Save(SETTINGS_PATH);
            PlaySound(FE_SND_MENU_5);
        }

        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_BACK)) {
            PlaySound(FE_SND_MENU_5);
            GoBack();
        }

        if (nonMouseInput && m_mouseInputActive) {
            m_mouseInputActive = false;
            if (g_display) {
                g_display->SetCursorVisible(false);
            }
        }

        return m_result;
    }

#ifdef MOD_LOADER
    if (m_currPage == MenuPage_Mods) {
        if (!m_scrollBarDragging) SmoothScrollToward(m_modScrollVisual, (f32)m_modScrollTop);
        const auto& mods = ModLoader::Instance().GetMods();
        const s32 count = static_cast<s32>(mods.size());
        if (count == 0) {
            m_cursor = 0;
        }
        else if (m_modCursor < 0 || m_modCursor >= count) {
            m_modCursor = 0;
        }
        ClampModsScroll();

        const bool nonMouseInput = g_actionInput->HadKeyboardInputThisFrame()
            || g_actionInput->HadGamepadInputThisFrame();
        const s32 scroll = g_actionInput->ConsumeScrollDelta();

        double sx = 0.0, sy = 0.0;
        g_actionInput->GetMousePosition(sx, sy);
        const bool leftClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Left);
        const bool leftDown = g_actionInput->IsMouseButtonDown(MouseBtn::Left);
        const bool rightClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Right);
        bool scrollBarUsed = false;
        if (m_mouseInputActive) {
            const f32 screenW = g_display ? static_cast<f32>(g_display->GetScreenWidth()) : DEFAULT_SCREEN_WIDTH;
            const f32 screenH = g_display ? static_cast<f32>(g_display->GetScreenHeight()) : DEFAULT_SCREEN_HEIGHT;
            const f32 aspect = g_display ? g_display->GetAspectRatio() : (4.0f / 3.0f);
#if FIX_ASPECT_RATIO
            const f32 effectiveW = screenW * DEFAULT_ASPECT_RATIO / aspect;
            const f32 offsetX = (screenW - effectiveW) * 0.5f;
            const f32 psxX = (static_cast<f32>(sx) - offsetX) * DEFAULT_SCREEN_WIDTH / effectiveW;
#else
            const f32 psxX = static_cast<f32>(sx) * DEFAULT_SCREEN_WIDTH / screenW;
#endif
            const f32 psxY = static_cast<f32>(sy) * DEFAULT_SCREEN_HEIGHT / screenH;
            const PageDef& page = m_pages[MenuPage_Mods];
            const s32 panelX = DEF_WINDOW_CENTER_X - page.frameW / 2;
            const s32 panelY = DEF_WINDOW_CENTER_Y - page.frameH / 2;
            const f32 firstRowY = static_cast<f32>(panelY + DEF_TITLE_BAR_H + DEF_CONTENT_PAD + 8);
            scrollBarUsed = HandleScrollBarMouse((f32)(panelX + page.frameW - 12),
                                                 firstRowY - DEF_KEYBIND_ROW_TOP_PAD,
                                                 (f32)(DEF_MODS_VISIBLE_ROWS * DEF_MODS_ROW_STEP),
                                                 count, DEF_MODS_VISIBLE_ROWS, &m_modScrollTop, &m_modScrollVisual,
                                                 psxX, psxY, leftClick, leftDown);
            if (scrollBarUsed && count > 0) {
                if (m_modCursor < m_modScrollTop) m_modCursor = m_modScrollTop;
                if (m_modCursor >= m_modScrollTop + DEF_MODS_VISIBLE_ROWS) {
                    m_modCursor = m_modScrollTop + DEF_MODS_VISIBLE_ROWS - 1;
                }
                m_cursor = -1;
            }

            if (!scrollBarUsed && psxX >= panelX && psxX < panelX + page.frameW) {
                const f32 viewportTop = firstRowY - DEF_KEYBIND_ROW_TOP_PAD;
                const f32 viewportBottom = viewportTop + DEF_MODS_VISIBLE_ROWS * DEF_MODS_ROW_STEP;
                for (s32 hovered = 0; hovered < count; ++hovered) {
                    const f32 top = firstRowY + ((f32)hovered - m_modScrollVisual) * DEF_MODS_ROW_STEP
                        - DEF_KEYBIND_ROW_TOP_PAD;
                    if (top + DEF_MODS_ROW_STEP <= viewportTop || top >= viewportBottom) continue;
                    if (psxY >= top && psxY < top + DEF_MODS_ROW_STEP) {
                        if (m_cursor == 0 || m_modCursor != hovered) PlaySound(FE_SND_MENU_7);
                        m_cursor = -1;
                        m_modCursor = hovered;
                        break;
                    }
                }

                const s32 backTop = panelY + page.frameH - DEF_BOTTOM_BAR_H - DEF_ROW_STEP - 2;
                if (psxY >= backTop && psxY < backTop + DEF_ROW_STEP) {
                    if (m_cursor != 0) PlaySound(FE_SND_MENU_7);
                    m_cursor = 0;
                }
            }

            if (!scrollBarUsed && leftClick) {
                PlaySound(FE_SND_MENU_5);
                if (m_cursor == 0) Confirm();
                else if (!ToggleSelectedMod()) PlaySound(FE_SND_MENU_7);
                return m_result;
            }
            if (rightClick) {
                PlaySound(FE_SND_MENU_5);
                GoBack();
                return m_result;
            }
        }

        if (scroll != 0 && count > 0) {
            // Mouse-wheel scrolling plays no sound (unlike keyboard/gamepad nav below).
            m_cursor = -1;
            m_modCursor += (scroll > 0) ? -1 : 1;
            if (m_modCursor < 0) m_modCursor = 0;
            if (m_modCursor >= count) m_modCursor = count - 1;
            ClampModsScroll();
        }

        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_UP)) {
            if (m_cursor == 0 && count > 0) {
                m_cursor = -1;
                m_modCursor = count - 1;
            }
            else if (m_modCursor > 0) {
                --m_modCursor;
            }
            else {
                m_cursor = 0;
            }
            ClampModsScroll();
            PlaySound(FE_SND_MENU_7);
        }
        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_DOWN)) {
            if (m_cursor == 0 && count > 0) {
                m_cursor = -1;
                m_modCursor = 0;
            }
            else if (m_modCursor + 1 < count) {
                ++m_modCursor;
            }
            else {
                m_cursor = 0;
            }
            ClampModsScroll();
            PlaySound(FE_SND_MENU_7);
        }

        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_CONFIRM)) {
            PlaySound(FE_SND_MENU_5);
            if (m_cursor == 0) Confirm();
            else if (!ToggleSelectedMod()) PlaySound(FE_SND_MENU_7);
        }
        if (nonMouseInput && g_actionInput->JustPressed(ACTION_MENU_BACK)) {
            PlaySound(FE_SND_MENU_5);
            GoBack();
        }

        if (nonMouseInput && m_mouseInputActive) {
            m_mouseInputActive = false;
            if (g_display) g_display->SetCursorVisible(false);
        }
        return m_result;
    }
#endif

    // Reactivation/deactivation already happened in UpdateMouseCursorVisibility()
    // at the top of Invoke() - just read current position/click state here.
    double sx = 0.0;
    double sy = 0.0;
    g_actionInput->GetMousePosition(sx, sy);

    const bool leftClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Left);
    const bool rightClick = g_actionInput->IsMouseButtonTriggered(MouseBtn::Right);
    const s32 scroll = g_actionInput->ConsumeScrollDelta();
    const Entry* e = &m_pages[m_currPage].entries[m_cursor];
    auto prevVal = GetBoundValue(*e);

    // Mouse hover and click
    if (m_mouseInputActive) {
        // Convert screen to PSX coordinate space
        const f32 screenW = g_display ? (f32)g_display->GetScreenWidth() : DEFAULT_SCREEN_WIDTH;
        const f32 screenH = g_display ? (f32)g_display->GetScreenHeight() : DEFAULT_SCREEN_HEIGHT;
        const f32 aspect = g_display ? g_display->GetAspectRatio() : (4.0f / 3.0f);
#if FIX_ASPECT_RATIO
        const f32 effectiveW = screenW * DEFAULT_ASPECT_RATIO / aspect;
        const f32 offsetX = (screenW - effectiveW) * 0.5f;
        const f32 psxX = ((f32)sx - offsetX) * DEFAULT_SCREEN_WIDTH / effectiveW;
#else
        const f32 psxX = (f32)sx * DEFAULT_SCREEN_WIDTH / screenW;
#endif
        const f32 psxY = (f32)sy * DEFAULT_SCREEN_HEIGHT / screenH;

        // Row hit test
        const PageDef* page = &m_pages[m_currPage];
        const s32 panelX = DEF_WINDOW_CENTER_X - page->frameW / 2;
        const s32 panelY = DEF_WINDOW_CENTER_Y - page->frameH / 2;
        const s32 rowSpan = (page->numEntries > 0) ? ((page->numEntries - 1) * DEF_ROW_STEP) : 0;
        const s32 extraH = CalcPageExtraHeight(*page);
        const s32 entryBlockH = DEF_CONTENT_PAD + rowSpan + DEF_ROW_TEXT_H + extraH;
        const s32 bodyAvailH = page->frameH - DEF_TITLE_BAR_H - DEF_BOTTOM_BAR_H - DEF_CONTENT_TOP_PAD - DEF_CONTENT_BOTTOM_PAD;
        const s32 bodyCenterPad = (bodyAvailH > entryBlockH) ? ((bodyAvailH - entryBlockH) / 2) : 0;
        const s32 firstY = panelY + DEF_TITLE_BAR_H + DEF_CONTENT_TOP_PAD + bodyCenterPad + DEF_CONTENT_PAD;
        const s32 baseLabelX = panelX + DEF_LABEL_X_PAD;
        const s32 baseValueX = panelX + page->frameW - DEF_VALUE_X_PAD;
        const s32 baseCenterX = DEF_WINDOW_CENTER_X;

        if (psxX >= (f32)panelX && psxX < (f32)(panelX + page->frameW)) {
            for (s32 i = 0; i < page->numEntries; i++) {
                s32 rowTop = 0;
                ResolveEntryLayout(*page, i,
                                   firstY, baseLabelX, baseValueX, baseCenterX,
                                   &rowTop, nullptr, nullptr, nullptr, nullptr);
                const s32 rowH = DEF_ROW_STEP + GetEntryExtraHeight(*page, page->entries[i]);
                if (psxY >= (f32)rowTop && psxY < (f32)(rowTop + rowH)) {
                    if (i != m_cursor && page->entries[i].type != EntryType_Info) {
                        // Discard staged display values when leaving that row via mouse.
                        const Entry& prev = page->entries[m_cursor];
                        if (prev.binding == EntryBinding_DisplayResolution) {
                            m_pendingResolutionActive = false;
                        }
                        if (prev.binding == EntryBinding_DisplayScreenMode) {
                            m_pendingScreenModeActive = false;
                        }
                        if (prev.binding == EntryBinding_DisplayMsaa) {
                            m_pendingMsaaActive = false;
                        }
                        m_cursor = i;
                        PlaySound(FE_SND_MENU_7);
                    }
                    break;
                }
            }
        }

        if (leftClick) {
            PlaySound(FE_SND_MENU_5);
            Confirm();
        }
        if (rightClick) {
            if (m_currPage != MenuPage_Frontend && m_currPage != MenuPage_Pause) {
                PlaySound(FE_SND_MENU_5);
            }
            GoBack();
        }
        if (scroll != 0) {
            Adjust(scroll > 0 ? 1 : -1);

            if (prevVal != GetBoundValue(*e)) {
                PlayValueChangeFeedback(*e);
            }
        }
    }

    if (g_actionInput->JustPressed(ACTION_MENU_UP)) {
        MoveCursor(-1);
        PlaySound(FE_SND_MENU_7);
    }

    if (g_actionInput->JustPressed(ACTION_MENU_DOWN)) {
        MoveCursor(1);
        PlaySound(FE_SND_MENU_7);
    }

    if (g_actionInput->JustPressed(ACTION_MENU_LEFT)) {
        Adjust(-1);

        if (prevVal != GetBoundValue(*e)) {
            PlayValueChangeFeedback(*e);
        }
    }
    if (g_actionInput->JustPressed(ACTION_MENU_RIGHT)) {
        Adjust(1);

        if (prevVal != GetBoundValue(*e)) {
            PlayValueChangeFeedback(*e);
        }
    }

    if (g_actionInput->JustPressed(ACTION_MENU_CONFIRM)) {
        PlaySound(FE_SND_MENU_5); Confirm();
    }

    if (g_actionInput->JustPressed(ACTION_MENU_BACK)) {
        // FE_SND_MENU_SPECIAL_4 triggers HandleCursorEvent(5) -> jcsFadeOutEngine(2),
        // which can leave audio faded out in this custom flow.
        // Use a non-fade back sound for submenu navigation.
        if (m_currPage != MenuPage::MenuPage_Frontend && m_currPage != MenuPage::MenuPage_Pause) {
            PlaySound(FE_SND_MENU_5);
        }
        GoBack();
    }
    else if (g_actionInput->IsGamepadActive() && g_actionInput->JustPressed(ACTION_OPEN_CLOSE_MENU)) {
        Deactivate();
    }

    return m_result;
}

void feCustomMenuMgr::SetPage(MenuPage page) {
    m_prevPage = m_currPage;
    m_currPage = page;
    m_cursor = 0;
    m_popupFadeSec = 0.0f;
    m_popupClosing = false;
    m_result = 1;
    m_pendingResolutionActive = false;
    m_pendingScreenModeActive = false;
    m_pendingMsaaActive = false;
    m_scrollBarDragging = false;
    m_keyBindCaptureActive = false;
    m_keyBindCaptureBlockFrames = 0;

    if (m_currPage == MenuPage_Title) {
        if (SaveGameHasAutosave()) {
            SetEntries(m_pages[MenuPage_Title], {
                Button("FE_CONT", EntryEvent_Continue),
                Button("FE_STG", EntryEvent_GoPage, MenuPage_StartGame),
                Button("FE_OPT", EntryEvent_GoPage, MenuPage_Options),
                Button("FE_QTG", EntryEvent_GoPage, MenuPage_QuitConfirm),
                       });
        }
        else {
            SetEntries(m_pages[MenuPage_Title], {
                Button("FE_STG", EntryEvent_GoPage, MenuPage_StartGame),
                Button("FE_OPT", EntryEvent_GoPage, MenuPage_Options),
                Button("FE_QTG", EntryEvent_GoPage, MenuPage_QuitConfirm),
                       });
        }
    }

#ifdef MOD_LOADER
    if (m_currPage == MenuPage_Mods && m_prevPage != MenuPage_Mods) {
        ModLoader::Instance().EnsureStorage();
        ModLoader::Instance().Reload();
        m_modCursor = 0;
        m_modScrollTop = 0;
        m_modScrollVisual = 0.0f;
        m_cursor = ModLoader::Instance().GetMods().empty() ? 0 : -1;
    }
#endif

    if (m_currPage == MenuPage_Quitting && m_prevPage != MenuPage_Quitting) {
        // Jackie character 0, quit line 75, fixed variant.
        DialogPreview::Play(0, 75, 0);
    }

    if (m_currPage == MenuPage_None) {
        m_pendingLoadSlot = -1;
        m_pendingSaveSlot = -1;
        m_pendingDeleteSlot = -1;
    }

    if (m_currPage == MenuPage_KeyBindings) {
        m_keyBindActionCursor = 0;
        m_keyBindSlotCursor = 0;
        m_keyBindScrollTop = 0;
        m_keyBindScrollVisual = 0.0f;
    }

    if (m_currPage == MenuPage_StartGame) {
        const bool showSave = (m_pages[MenuPage_StartGame].parentPage == MenuPage_Frontend);
        if (showSave) {
            SetEntries(m_pages[MenuPage_StartGame], {
                Button("FE_NWG", EntryEvent_NewGame),
                Button("FE_LDG", EntryEvent_Load),
                Button("FE_SVG", EntryEvent_Save),
                Button("FE_DLG", EntryEvent_Delete),
                Button("FE_BCK", EntryEvent_Back),
                       });
        }
        else {
            SetEntries(m_pages[MenuPage_StartGame], {
                Button("FE_NWG", EntryEvent_NewGame),
                Button("FE_LDG", EntryEvent_Load),
                Button("FE_DLG", EntryEvent_Delete),
                Button("FE_BCK", EntryEvent_Back),
                       });
        }
    }

    if (IsSaveSlotPage(m_currPage)) {
        RefreshSaveSlots();
        m_saveSlotScrollTop = 0;
        m_saveSlotScrollVisual = 0.0f;
        m_cursor = (m_currPage == MenuPage_SaveSlots) ? 0 : SAVEGAME_AUTOSAVE_SLOT;
    }

#if AUTO_UPDATER
    if (m_currPage == MenuPage_Update) {
        RefreshUpdatePageEntries();
    }
    if (m_currPage == MenuPage_Changelog) {
        RebuildChangelogLines();
    }
#endif

    if (m_currPage == MenuPage_AssetMissing) {
        RefreshAssetPageEntries();
    }

    if (m_currPage != MenuPage_None) {
        PageDef& mutablePage = m_pages[m_currPage];
        if (mutablePage.autoFrameH) {
            const s32 extraH = CalcPageExtraHeight(mutablePage);
            mutablePage.frameH = CalcAutoFrameHeight(mutablePage.numEntries, extraH);
        }
    }

    // Advance cursor past any leading Info entries.
    if (m_currPage != MenuPage_None) {
        const PageDef& pg = m_pages[m_currPage];
        while (m_cursor >= 0 && m_cursor < pg.numEntries && pg.entries[m_cursor].type == EntryType_Info) {
            m_cursor++;
        }
    }

    if (m_currPage == MenuPage_KeyBindings) {
        // Key bindings uses its own row/slot selection; keep Back unselected until navigated.
        m_cursor = -1;
    }
}

void feCustomMenuMgr::Activate(MenuPage startPage) {
    m_active = true;
    m_cursor = 0;
    LoadControllerOverlayTexture();

    if (g_display) {
        g_display->SetCursorCaptured(false);
        g_display->SetCursorVisible(false);
    }

    m_mouseInputActive = false;
    m_mousePosInitialized = false;
    if (startPage != MenuPage_Title) {
        rsEvent(RS_MUTE, 0, 0, 0);
    }

    if (startPage == MenuPage_Pause && g_game && g_game->GetState() == GameState::Play) {
        s32 track = 23;
        DialogPreview::Play(0, track);
    }
    else if (startPage == MenuPage_Location) {
        World* world = g_game ? g_game->GetWorld() : nullptr;
        s32 track = (world && world->GetCurLevelID() == 7) ? 18 : 23;
        DialogPreview::Play(0, track);
    }

    SetPage(startPage);

    PlaySound(FE_SND_MENU_MOVE);
}

void feCustomMenuMgr::Deactivate() {
    PlaySound(FE_SND_MENU_ACCEPT);
    if (m_currPage != MenuPage_Title) {
        rsEvent(RS_UNMUTE, 0, 0, 0);
    }
    if (g_display) g_display->SetCursorCaptured(true);

    m_active = false;
    m_cursor = 0;
    SetPage(MenuPage_None);
    m_result = (s32)GameResult::ResumePlay;
}

void feCustomMenuMgr::MoveCursor(s32 dir) {
    const s32 prevCursor = m_cursor;
    const PageDef& pg = m_pages[m_currPage];
    s32 next = m_cursor + dir;
    // Wrap around.
    if (next < 0) next = pg.numEntries - 1;
    if (next >= pg.numEntries) next = 0;
    // Skip over Info entries.
    const s32 limit = pg.numEntries;
    for (s32 tries = 0; tries < limit; tries++) {
        if (pg.entries[next].type != EntryType_Info) break;
        next += dir;
        if (next < 0) next = pg.numEntries - 1;
        if (next >= pg.numEntries) next = 0;
    }
    m_cursor = next;

    // Leaving a staged display row without confirming discards staged value.
    if (prevCursor != m_cursor) {
        const Entry& prev = m_pages[m_currPage].entries[prevCursor];
        if (prev.binding == EntryBinding_DisplayResolution) {
            m_pendingResolutionActive = false;
        }
        if (prev.binding == EntryBinding_DisplayScreenMode) {
            m_pendingScreenModeActive = false;
        }
        if (prev.binding == EntryBinding_DisplayMsaa) {
            m_pendingMsaaActive = false;
        }
    }
}

bool feCustomMenuMgr::InvokeLocationSelection() {
    if (!g_feMenuMgr) {
        return false;
    }

    hdMenu* levelMenu = g_feMenuMgr->FindMenu(HASH_LEVEL_SCREEN);
    if (!levelMenu) {
        LOG("[CustomMenu] Location select failed: level menu not found");
        return false;
    }

    hdMenuItem* executeItem = levelMenu->FindItem(HASH_LEVEL_EXECUTE);
    if (!executeItem || !executeItem->callback) {
        LOG("[CustomMenu] Location select failed: execute callback missing");
        return false;
    }

    const s32 callbackResult = executeItem->callback(executeItem);
    if (callbackResult == 4 || callbackResult == 8) {
        m_result = callbackResult;
        return true;
    }

    LOG("[CustomMenu] Location select callback returned unexpected result=%d", callbackResult);
    return false;
}

void feCustomMenuMgr::Confirm() {
    const Entry* e = &m_pages[m_currPage].entries[m_cursor];

    if (e->type == EntryType_Toggle) {
        const s32 v = GetBoundValue(*e) ? 0 : 1;
        ApplyValue(*e, v);
        return;
    }

    if (e->type == EntryType_List) {
        if (e->binding == EntryBinding_DisplayResolution && m_pendingResolutionActive) {
            ApplyValue(*e, m_pendingResolutionIndex);
            m_pendingResolutionActive = false;
        }
        else if (e->binding == EntryBinding_DisplayScreenMode && m_pendingScreenModeActive) {
            ApplyValue(*e, m_pendingScreenMode);
            m_pendingScreenModeActive = false;
        }
        else if (e->binding == EntryBinding_DisplayMsaa && m_pendingMsaaActive) {
            ApplyValue(*e, m_pendingMsaaIndex);
            m_pendingMsaaActive = false;
        }
        return;
    }

    // Button
    switch (e->event) {
        case EntryEvent_GoPage:
            m_pages[e->goPage].parentPage = m_currPage;
            m_pages[e->goPage].parentEntry = m_cursor;
            SetPage(e->goPage);
            break;
        case EntryEvent_Resume:
            m_result = 8;
            break;
        case EntryEvent_Back:
            GoBack();
            break;
        case EntryEvent_NewGame:
        {
            const bool fromTitle = g_game && g_game->GetState() == GameState::TitleLoop;
            if (!fromTitle && m_currPage != MenuPage_NewGameConfirm) {
                m_pages[MenuPage_NewGameConfirm].parentPage = m_currPage;
                m_pages[MenuPage_NewGameConfirm].parentEntry = m_cursor;
                SetPage(MenuPage_NewGameConfirm);
                break;
            }

            BeginNewGameReset();

            if (!fromTitle && g_game) {
                g_game->QueueTitleNewGameStart();
                g_game->SetState(GameState::Init);
            }
            m_result = 4;
            break;
        }
        case EntryEvent_Continue:
            if (SaveGameLoadAutosave()) {
                m_result = (s32)GameResult::StateChange;
            }
            else {
                PlaySound(FE_SND_MENU_7);
            }
            break;
        case EntryEvent_ExitToHub:
            if (g_game)
                g_game->SetState(GameState::OpenLocationMenu);
            m_result = 4;
            break;
        case EntryEvent_QuitGame:
            m_quitTimerSec = DEF_QUIT_TIMER_SEC;
            SetPage(MenuPage_Quitting);
            break;
        case EntryEvent_Credits:
        {
            if (!g_game) break;

            const bool onTitleScreen = (g_game->GetState() == GameState::TitleLoop);
            const s32 savedLocation = g_currentSoundLocation;

            rsEvent(RS_STOP_MUSIC, 0, 0, 0);
            g_game->PlayMovie("credits.str", 1, 0);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            g_currentSoundLocation = savedLocation;
            rsEvent(RS_LEVEL_BEGIN, 0, 0, 0);

            if (!onTitleScreen) {
                rsEvent(RS_MUTE, 0, 0, 0);
            }
            break;
        }
        case EntryEvent_Load:
            if (m_currPage != MenuPage_LoadSlots) {
                m_pages[MenuPage_LoadSlots].parentPage = m_currPage;
                m_pages[MenuPage_LoadSlots].parentEntry = m_cursor;
                SetPage(MenuPage_LoadSlots);
                break;
            }

            if (m_cursor >= 0 && m_cursor < SAVEGAME_VISIBLE_SLOT_COUNT) {
                const bool fromTitle = g_game && g_game->GetState() == GameState::TitleLoop;
                if (fromTitle) {
                    const bool loaded = (m_cursor == SAVEGAME_AUTOSAVE_SLOT)
                        ? SaveGameLoadAutosave()
                        : SaveGameLoadSlot(m_cursor);
                    if (!loaded) {
                        PlaySound(16);
                        break;
                    }
                    m_result = 4;
                    break;
                }

                if (!m_saveSlots[m_cursor].occupied) {
                    PlaySound(FE_SND_MENU_7);
                    break;
                }

                m_pendingLoadSlot = m_cursor;
                m_pages[MenuPage_LoadConfirm].parentPage = MenuPage_LoadSlots;
                m_pages[MenuPage_LoadConfirm].parentEntry = m_cursor;
                SetPage(MenuPage_LoadConfirm);
            }
            break;
        case EntryEvent_Save:
            if (m_currPage != MenuPage_SaveSlots) {
                m_pages[MenuPage_SaveSlots].parentPage = m_currPage;
                m_pages[MenuPage_SaveSlots].parentEntry = m_cursor;
                SetPage(MenuPage_SaveSlots);
                break;
            }

            if (m_cursor >= 0 && m_cursor < SAVEGAME_SLOT_COUNT) {
                m_pendingSaveSlot = m_cursor;
                m_pages[MenuPage_SaveConfirm].parentPage = MenuPage_SaveSlots;
                m_pages[MenuPage_SaveConfirm].parentEntry = m_cursor;
                SetPage(MenuPage_SaveConfirm);
            }
            break;
        case EntryEvent_Delete:
            if (m_currPage != MenuPage_DeleteSlots) {
                m_pages[MenuPage_DeleteSlots].parentPage = m_currPage;
                m_pages[MenuPage_DeleteSlots].parentEntry = m_cursor;
                SetPage(MenuPage_DeleteSlots);
                break;
            }

            if (m_cursor >= 0 && m_cursor < SAVEGAME_VISIBLE_SLOT_COUNT) {
                if (!m_saveSlots[m_cursor].occupied) {
                    PlaySound(16);
                    break;
                }

                m_pendingDeleteSlot = m_cursor;
                m_pages[MenuPage_DeleteConfirm].parentPage = MenuPage_DeleteSlots;
                m_pages[MenuPage_DeleteConfirm].parentEntry = m_cursor;
                SetPage(MenuPage_DeleteConfirm);
            }
            break;
        case EntryEvent_LoadConfirmYes:
            if (m_pendingLoadSlot >= 0 && m_pendingLoadSlot < SAVEGAME_VISIBLE_SLOT_COUNT) {
                const bool loaded = (m_pendingLoadSlot == SAVEGAME_AUTOSAVE_SLOT)
                    ? SaveGameLoadAutosave()
                    : SaveGameLoadSlot(m_pendingLoadSlot);
                if (loaded && SaveGameApplyPendingLoad(g_game) && g_game) {
                    m_pendingLoadSlot = -1;
                    g_game->SetState(GameState::QueueLevelLoad);
                    m_result = 4;
                }
                else {
                    PlaySound(FE_SND_MENU_7);
                }
            }
            else {
                PlaySound(FE_SND_MENU_7);
            }
            break;
        case EntryEvent_SaveConfirmYes:
            if (m_pendingSaveSlot >= 0 && m_pendingSaveSlot < SAVEGAME_SLOT_COUNT) {
                if (SaveGameWriteSlot(m_pendingSaveSlot)) {
                    RefreshSaveSlots();
                    const s32 savedSlot = m_pendingSaveSlot;
                    m_pendingSaveSlot = -1;
                    m_pages[MenuPage_SaveDone].parentPage = MenuPage_SaveSlots;
                    m_pages[MenuPage_SaveDone].parentEntry = savedSlot;
                    SetPage(MenuPage_SaveDone);
                }
                else {
                    PlaySound(FE_SND_MENU_7);
                }
            }
            else {
                PlaySound(FE_SND_MENU_7);
            }
            break;
        case EntryEvent_DeleteConfirmYes:
            if (m_pendingDeleteSlot >= 0 && m_pendingDeleteSlot < SAVEGAME_VISIBLE_SLOT_COUNT) {
                const bool deleted = (m_pendingDeleteSlot == SAVEGAME_AUTOSAVE_SLOT)
                    ? SaveGameDeleteAutosave()
                    : SaveGameDeleteSlot(m_pendingDeleteSlot);
                if (deleted) {
                    RefreshSaveSlots();
                    const s32 deletedSlot = m_pendingDeleteSlot;
                    m_pendingDeleteSlot = -1;
                    m_pages[MenuPage_DeleteDone].parentPage = MenuPage_DeleteSlots;
                    m_pages[MenuPage_DeleteDone].parentEntry = deletedSlot;
                    SetPage(MenuPage_DeleteDone);
                }
                else {
                    PlaySound(FE_SND_MENU_7);
                }
            }
            else {
                PlaySound(FE_SND_MENU_7);
            }
            break;
        case EntryEvent_LocationSelect:
            if (!InvokeLocationSelection()) {
                PlaySound(FE_SND_MENU_7);
            }
            break;
#if AUTO_UPDATER
        case EntryEvent_StartUpdate:
            if (g_autoUpdater) {
                g_autoUpdater->StartDownload();
            }
            break;
        case EntryEvent_DismissUpdate:
            GoBack();
            break;
        case EntryEvent_ShowChangelog:
            m_pages[MenuPage_Changelog].parentPage = m_currPage;
            m_pages[MenuPage_Changelog].parentEntry = m_cursor;
            SetPage(MenuPage_Changelog);
            break;
        case EntryEvent_CancelUpdate:
            if (g_autoUpdater) {
                g_autoUpdater->CancelDownload();
            }
            break;
        case EntryEvent_CheckForUpdate:
            if (g_autoUpdater) {
                g_autoUpdater->CheckAsync();
                OpenPopup(PopupKind_CheckingUpdate, 1.0f, [this]() -> s32 {
                    if (!g_autoUpdater || !g_autoUpdater->IsCheckComplete())
                        return -1;
                    return (m_currPage == MenuPage_None) ? (s32)GameResult::ResumePlay : 1;
                });
                PlaySound(FE_SND_MENU_OPEN);
            }
            break;
        case EntryEvent_InstallUpdate:
            if (g_autoUpdater) {
                g_autoUpdater->InstallAndRelaunch();
            }
            break;
#endif
        case EntryEvent_ScanForAssets:
            if (g_psxDiscExtractor) {
                g_psxDiscExtractor->ScanAsync();
                OpenPopup(PopupKind_AssetScanning, 2.0f, [this]() -> s32 {
                    if (!g_psxDiscExtractor || g_psxDiscExtractor->GetState() == PsxDiscExtractor::State::Scanning)
                        return -1;
                    return 1;
                });
                PlaySound(FE_SND_MENU_OPEN);
            }
            break;
        case EntryEvent_ExtractAssets:
            if (g_psxDiscExtractor) {
                g_psxDiscExtractor->StartExtractAsync();
                OpenPopup(PopupKind_AssetExtracting, 2.0f, [this]() -> s32 {
                    if (!g_psxDiscExtractor)
                        return -1;
                    const PsxDiscExtractor::State state = g_psxDiscExtractor->GetState();
                    if (state == PsxDiscExtractor::State::Done)
                        return (s32)GameResult::ResumePlay;
                    if (state == PsxDiscExtractor::State::Error)
                        return 1;
                    return -1;
                });
                PlaySound(FE_SND_MENU_OPEN);
            }
            break;
        case EntryEvent_QuitFromAssetCheck:
            m_quitTimerSec = DEF_QUIT_TIMER_SEC;
            SetPage(MenuPage_Quitting);
            break;
    }
}

void feCustomMenuMgr::GoBack() {
    if (m_currPage == MenuPage_LoadConfirm) {
        m_pendingLoadSlot = -1;
    }
    if (m_currPage == MenuPage_SaveConfirm) {
        m_pendingSaveSlot = -1;
    }
    if (m_currPage == MenuPage_DeleteConfirm) {
        m_pendingDeleteSlot = -1;
    }

    if (m_currPage == MenuPage_Location) {
        m_result = 8;
        return;
    }

    // No sane "back" target while gating boot on missing assets - use the on-screen
    // Quit/Extract/Scan buttons instead.
    if (m_currPage == MenuPage_AssetMissing) {
        return;
    }

    if (m_currPage == MenuPage::MenuPage_Frontend || m_currPage == MenuPage::MenuPage_Pause || m_currPage == MenuPage::MenuPage_Title) {
        Deactivate();
        return;
    }

    auto targetPage = m_pages[m_currPage].parentPage;
    auto parentEntry = m_pages[m_currPage].parentEntry;
    if (targetPage != MenuPage_None)
        SetPage(targetPage);
    else
        SetPage(m_prevPage);

    m_cursor = parentEntry;
}

void feCustomMenuMgr::Adjust(s32 dir) {
    const Entry* e = &m_pages[m_currPage].entries[m_cursor];
    if (e->binding == EntryBinding_None || dir == 0)
        return;

    if (e->type == EntryType_Toggle) {
        const s32 v = GetBoundValue(*e) ? 0 : 1;
        ApplyValue(*e, v);
    }
    else if (e->type == EntryType_List) {
        if (e->binding == EntryBinding_PlayerConfig) {
            const s32 v = WrapStepValue(GetBoundValue(*e), e->step, e->lo, e->hi, dir);
            ApplyValue(*e, v);
            return;
        }

        if (e->binding == EntryBinding_Language) {
            const s32 current = GetBoundValue(*e);
            const s32 v = WrapStepValue(current, e->step, e->lo, e->hi, dir);
            ApplyValue(*e, v);
            return;
        }

        if (e->binding == EntryBinding_DisplayResolution) {
            const s32 current = m_pendingResolutionActive ? m_pendingResolutionIndex : GetBoundValue(*e);
            s32 maxIndex = 0;
            if (g_display) {
                const s32 count = g_display->GetResolutionCount();
                maxIndex = (count > 0) ? (count - 1) : 0;
            }
            const s32 v = WrapStepValue(current, e->step, 0, maxIndex, dir);
            m_pendingResolutionIndex = v;
            m_pendingResolutionActive = true;
            return;
        }

        if (e->binding == EntryBinding_DisplayScreenMode) {
            const s32 current = m_pendingScreenModeActive ? m_pendingScreenMode : GetBoundValue(*e);
            const s32 v = WrapStepValue(current, e->step, e->lo, e->hi, dir);
            m_pendingScreenMode = v;
            m_pendingScreenModeActive = true;
            return;
        }

        if (e->binding == EntryBinding_DisplayMsaa) {
            const s32 current = m_pendingMsaaActive ? m_pendingMsaaIndex : GetBoundValue(*e);
            const s32 v = WrapStepValue(current, e->step, e->lo, e->hi, dir);
            m_pendingMsaaIndex = v;
            m_pendingMsaaActive = true;
            return;
        }

        if (e->binding == EntryBinding_DisplayFrameRate) {
            const s32 current = GetBoundValue(*e);
            const s32 v = WrapStepValue(current, e->step, e->lo, e->hi, dir);
            ApplyValue(*e, v);
            return;
        }

        if (e->binding == EntryBinding_ControllerPromptStyle) {
            const s32 current = GetBoundValue(*e);
            const s32 v = WrapStepValue(current, e->step, e->lo, e->hi, dir);
            ApplyValue(*e, v);
            return;
        }

#if MODERN_GRAPHICS
        if (e->binding == EntryBinding_DisplayShadowQuality) {
            const s32 current = GetBoundValue(*e);
            const s32 v = WrapStepValue(current, e->step, e->lo, e->hi, dir);
            ApplyValue(*e, v);
            return;
        }
#endif
    }
    else if (e->type == EntryType_Slider) {
        if (e->binding == EntryBinding_MusicVol ||
            e->binding == EntryBinding_EffectsVol ||
            e->binding == EntryBinding_DialogVol) {
            static constexpr s32 kSegments = DEF_SLIDER_CIRCLE_SEGMENTS;
            s32 current = GetBoundValue(*e);
            if (current < 0) current = 0;
            if (current > 100) current = 100;

            s32 seg = (current * kSegments) / 100;
            seg = WrapStepValueSlider(seg, 1, 0, kSegments, dir);

            s32 v = 0;
            if (seg <= 0) {
                v = 0;
            }
            else if (seg >= kSegments) {
                v = 100;
            }
            else {
                // Pick the midpoint of this segment's value bucket so render quantization is stable.
                const s32 lo = ((seg * 100) + (kSegments - 1)) / kSegments;
                const s32 hi = ((((seg + 1) * 100) + (kSegments - 1)) / kSegments) - 1;
                v = (lo + hi) / 2;
            }
            ApplyValue(*e, v);
            return;
        }

        const s32 v = WrapStepValue(GetBoundValue(*e), e->step, e->lo, e->hi, dir);
        ApplyValue(*e, v);
    }
}

s32 feCustomMenuMgr::GetBoundValue(const Entry& e) const {
    switch (e.binding) {
        case EntryBinding_MusicVol: return g_sound ? (s32)g_sound->flag0 : 100;
        case EntryBinding_EffectsVol: return g_sound ? (s32)g_sound->flag2 : 100;
        case EntryBinding_DialogVol: return g_sound ? (s32)g_sound->flag1 : 100;
        case EntryBinding_Stereo: return (g_sound && g_sound->activeFlag) ? 1 : 0;
        case EntryBinding_Shock: return GetShock() ? 1 : 0;
        case EntryBinding_PlayerConfig: return g_inputManager ? (s32)g_inputManager->GetPlayerConfig() : 0;
        case EntryBinding_ControllerPromptStyle: return ControllerPromptManager::GetStyle();
        case EntryBinding_Language: return (s32)g_customText.GetLanguage();
        case EntryBinding_DisplayResolution: return g_display ? g_display->GetResolutionIndex() : 0;
        case EntryBinding_DisplayScreenMode: return g_display ? g_display->GetScreenMode() : Display::GetDefaultScreenMode();
        case EntryBinding_DisplayVsync: return g_display ? g_display->GetVsync() : Display::GetDefaultVsync();
        case EntryBinding_DisplayFrameRate:
        {
            const s32 fps = g_time ? g_time->targetFPS : 30;
            return FrameRateValueToOptionIndex(fps);
        }
        case EntryBinding_DisplayMsaa:
        {
            const s32 samples = g_display ? g_display->GetMSAA() : Display::GetDefaultMSAA();
            return MsaaSamplesToOptionIndex(samples);
        }
#if MODERN_GRAPHICS
        case EntryBinding_DisplayShadowQuality: return (s32)ShadowCSM::GetQuality();
#endif
#if NEW_CHEATS
        case EntryBinding_CheatAllDragons: return IsCheatEnabled(CheatOption::AllDragons) ? 1 : 0;
        case EntryBinding_CheatAllLevels: return IsCheatEnabled(CheatOption::AllLevels) ? 1 : 0;
        case EntryBinding_CheatGodMode: return IsCheatEnabled(CheatOption::GodMode) ? 1 : 0;
        case EntryBinding_CheatOnePunchMan: return IsCheatEnabled(CheatOption::OnePunchMan) ? 1 : 0;
        case EntryBinding_CheatHeavenBound: return IsCheatEnabled(CheatOption::HeavenBound) ? 1 : 0;
        case EntryBinding_CheatBobbleHead: return IsCheatEnabled(CheatOption::BobbleHead) ? 1 : 0;
        case EntryBinding_CheatStuntquake: return IsCheatEnabled(CheatOption::Stuntquake) ? 1 : 0;
        case EntryBinding_CheatMirrorWorld: return IsCheatEnabled(CheatOption::MirrorWorld) ? 1 : 0;
#endif
        default: return 0;
    }
}

void feCustomMenuMgr::ApplyValue(const Entry& e, s32 v) {
    if (v < e.lo) v = e.lo;
    if (v > e.hi) v = e.hi;

    if (e.binding == EntryBinding_MusicVol) {
        rsEvent(RS_SET_MUSIC_VOL, v, 0, 0);
        if (g_sound) g_sound->flag0 = (s16)v;
    }
    else if (e.binding == EntryBinding_EffectsVol) {
        rsEvent(RS_SET_EFFECTS_VOL, v, 0, 0);
        rsEvent(RS_SET_EFFECTS_VOL_AUX, v, 0, 0);
        if (g_sound) g_sound->flag2 = (s16)v;
    }
    else if (e.binding == EntryBinding_DialogVol) {
        rsEvent(RS_SET_DIALOG_VOL, v, 0, 0);
        if (g_sound) g_sound->flag1 = (s16)v;
    }
    else if (e.binding == EntryBinding_Stereo) {
        rsEvent(v ? RS_SET_STEREO : RS_SET_MONO, 0, 0, 0);
        if (g_sound) g_sound->activeFlag = v;
    }
    else if (e.binding == EntryBinding_Shock) {
        SetShock(v);
        if (v) {
            // Menu feedback: full-strength pulse that survives high-FPS frame pacing.
            SetActuator(0, 255, 60);
            UpdateActuator(0);
        }
        else {
            Shock(SHOCK_CLEAR);
        }
    }
    else if (e.binding == EntryBinding_PlayerConfig) {
        if (g_inputManager) {
            const s16* currentMode[2] = {
                g_inputManager->controls[0].modeMap,
                g_inputManager->controls[1].modeMap,
            };

            g_inputManager->SetPlayerConfig((u8)v);
            const u8* playerMap = g_inputManager->PlayerMapArray();
            for (s16 padIndex = 0; padIndex < 2; ++padIndex) {
                g_inputManager->SetControlMapArray(padIndex, playerMap);
                if (currentMode[padIndex]) {
                    g_inputManager->SetControlModeArray(padIndex, currentMode[padIndex]);
                }
            }
        }
    }
    else if (e.binding == EntryBinding_Language) {
        if (v < 0 || v >= (s32)NumLanguages) {
            v = (s32)LangEnglish;
        }
        g_customText.SetLanguage((GameLanguage)v);
    }
    else if (e.binding == EntryBinding_ControllerPromptStyle) {
        if (ControllerPromptManager::SetStyle(v)) {
            ReloadControllerPromptTextures();
        }
    }
    else if (e.binding == EntryBinding_DisplayScreenMode) {
        if (g_display) g_display->SetScreenMode(v);
        Display::SetDefaultScreenMode(v);
    }
    else if (e.binding == EntryBinding_DisplayVsync) {
        if (g_display) g_display->SetVsync(v);
        Display::SetDefaultVsync(v);
    }
    else if (e.binding == EntryBinding_DisplayFrameRate) {
        const s32 fps = FrameRateOptionIndexToValue(v);
        if (g_time) {
            g_time->targetFPS = fps;
        }
    }
    else if (e.binding == EntryBinding_DisplayMsaa) {
        const s32 samples = MsaaOptionIndexToSamples(v);
        if (g_display) {
            g_display->SetMSAA(samples);
        }
        else {
            Display::SetDefaultMSAA(samples);
        }
    }
    else if (e.binding == EntryBinding_DisplayResolution) {
        if (g_display) g_display->SetResolutionIndex(v);
    }
#if MODERN_GRAPHICS
    else if (e.binding == EntryBinding_DisplayShadowQuality) {
        ShadowCSM::SetQuality((ShadowQuality)v);
    }
#endif
#if NEW_CHEATS
    else if (e.binding == EntryBinding_CheatAllDragons) {
        SetCheatEnabled(CheatOption::AllDragons, v != 0);
    }
    else if (e.binding == EntryBinding_CheatAllLevels) {
        SetCheatEnabled(CheatOption::AllLevels, v != 0);
    }
    else if (e.binding == EntryBinding_CheatGodMode) {
        SetCheatEnabled(CheatOption::GodMode, v != 0);
    }
    else if (e.binding == EntryBinding_CheatOnePunchMan) {
        SetCheatEnabled(CheatOption::OnePunchMan, v != 0);
    }
    else if (e.binding == EntryBinding_CheatHeavenBound) {
        SetCheatEnabled(CheatOption::HeavenBound, v != 0);
    }
    else if (e.binding == EntryBinding_CheatBobbleHead) {
        SetCheatEnabled(CheatOption::BobbleHead, v != 0);
    }
    else if (e.binding == EntryBinding_CheatStuntquake) {
        SetCheatEnabled(CheatOption::Stuntquake, v != 0);
    }
    else if (e.binding == EntryBinding_CheatMirrorWorld) {
        SetCheatEnabled(CheatOption::MirrorWorld, v != 0);
    }
#endif

#if NEW_CHEATS
    if (e.binding >= EntryBinding_CheatAllDragons
        && e.binding <= EntryBinding_CheatMirrorWorld) {
        return;
    }
#endif
    g_settings.Save(SETTINGS_PATH);
}

void feCustomMenuMgr::PlayValueChangeFeedback(const Entry& entry) {
    if (entry.type == EntryType_Slider && entry.binding == EntryBinding_DialogVol) {
        DialogPreview::Play(0, 23, 0, 0.3f);
        return;
    }

    PlaySound(GetValueChangeSoundId(entry));
}

void feCustomMenuMgr::PlaySound(s32 id) const {
    if (g_frontEndSound)
        g_frontEndSound->ProcessSoundEvent(id);
}

void feCustomMenuMgr::RefreshSaveSlots() {
    for (s32 i = 0; i < SAVEGAME_SLOT_COUNT; i++) {
        SaveGameSlotInfo info = {};
        SaveGameQuerySlotInfo(i, &info);
        m_saveSlots[i] = info;
    }

    SaveGameSlotInfo autosaveInfo = {};
    SaveGameQueryAutosaveInfo(&autosaveInfo);
    m_saveSlots[SAVEGAME_AUTOSAVE_SLOT] = autosaveInfo;
}

static s32 SaveSlotToDisplayIndex(s32 slotIndex) {
    return slotIndex == SAVEGAME_AUTOSAVE_SLOT ? 0 : slotIndex + 1;
}

static s32 SaveDisplayIndexToSlot(s32 displayIndex) {
    return displayIndex == 0 ? SAVEGAME_AUTOSAVE_SLOT : displayIndex - 1;
}

void feCustomMenuMgr::ClampSaveSlotScroll() {
    if (m_cursor >= 0 && m_cursor < SAVEGAME_VISIBLE_SLOT_COUNT) {
        const s32 displayIndex = SaveSlotToDisplayIndex(m_cursor);
        if (displayIndex < m_saveSlotScrollTop) {
            m_saveSlotScrollTop = displayIndex;
        }
        if (displayIndex >= m_saveSlotScrollTop + DEF_SAVE_VISIBLE_ROWS) {
            m_saveSlotScrollTop = displayIndex - DEF_SAVE_VISIBLE_ROWS + 1;
        }
    }

    const s32 maxTop = SAVEGAME_VISIBLE_SLOT_COUNT - DEF_SAVE_VISIBLE_ROWS;
    if (m_saveSlotScrollTop < 0) m_saveSlotScrollTop = 0;
    if (m_saveSlotScrollTop > maxTop) m_saveSlotScrollTop = maxTop;
}

#if AUTO_UPDATER
void feCustomMenuMgr::RefreshUpdatePageEntries() {
    if (!g_autoUpdater) {
        return;
    }

    m_lastUpdateState = g_autoUpdater->GetState();
    PageDef& page = m_pages[MenuPage_Update];

    switch (m_lastUpdateState) {
        case AutoUpdater::State::UpdateAvailable:
            SetEntries(page, {
                Info("FE_UPD_CUR"),
                Info("FE_UPD_LAT"),
                Button("FE_UPD_NOTES", EntryEvent_ShowChangelog),
                Button("FE_UPD_DL", EntryEvent_StartUpdate),
                Button("FE_UPD_CHKNOW", EntryEvent_CheckForUpdate),
                Button("FE_BCK", EntryEvent_Back),
                       });
            break;
        case AutoUpdater::State::Downloading:
            SetEntries(page, {
                Info("FE_UPD_PRG"),
                Button("FE_UPD_CNL", EntryEvent_CancelUpdate),
                       });
            break;
        case AutoUpdater::State::ReadyToInstall:
            SetEntries(page, {
                Info("FE_UPD_RDY"),
                Button("FE_UPD_INST", EntryEvent_InstallUpdate),
                Button("FE_BCK", EntryEvent_DismissUpdate),
                       });
            break;
        case AutoUpdater::State::Installing:
            SetEntries(page, {
                Info("FE_UPD_INSTG"),
                       });
            break;
        case AutoUpdater::State::UpToDate:
            SetEntries(page, {
                Info("FE_UPD_UTD"),
                Button("FE_UPD_CHKNOW", EntryEvent_CheckForUpdate),
                Button("FE_BCK", EntryEvent_Back),
                       });
            break;
        case AutoUpdater::State::Error:
            SetEntries(page, {
                Info("FE_UPD_ERR"),
                Button("FE_UPD_CHKNOW", EntryEvent_CheckForUpdate),
                Button("FE_BCK", EntryEvent_Back),
                       });
            break;
        case AutoUpdater::State::Idle:
        case AutoUpdater::State::Checking:
        default:
            SetEntries(page, {
                Info("FE_UPD_CHK"),
                Button("FE_BCK", EntryEvent_Back),
                       });
            break;
    }

    // A refresh mid-visit can shrink the entry list out from under the cursor.
    if (m_cursor < 0 || m_cursor >= page.numEntries || page.entries[m_cursor].type == EntryType_Info) {
        m_cursor = 0;
        while (m_cursor < page.numEntries && page.entries[m_cursor].type == EntryType_Info) {
            m_cursor++;
        }
    }
}

bool feCustomMenuMgr::BuildUpdateInfoText(const char* token, char* outText, s32 outTextLen) const {
    if (!g_autoUpdater || !token) {
        return false;
    }

    if (!strcmp(token, "FE_UPD_CUR")) {
        const char* fmt = Localize("FE_UPD_CUR");
        if (!fmt) fmt = "Current Version: %s";
        snprintf(outText, outTextLen, fmt, g_autoUpdater->GetCurrentVersion());
        return true;
    }
    if (!strcmp(token, "FE_UPD_LAT")) {
        const char* fmt = Localize("FE_UPD_LAT");
        if (!fmt) fmt = "New Version Available: %s";
        snprintf(outText, outTextLen, fmt, g_autoUpdater->GetLatestVersionTag());
        return true;
    }
    if (!strcmp(token, "FE_UPD_ERR")) {
        const char* fmt = Localize("FE_UPD_ERR");
        if (!fmt) fmt = "Update check failed: %s";
        snprintf(outText, outTextLen, fmt, g_autoUpdater->GetError());
        return true;
    }
    if (!strcmp(token, "FE_UPD_PRG")) {
        const char* fmt = Localize("FE_UPD_PRG");
        if (!fmt) fmt = "Downloading... %d%%";
        snprintf(outText, outTextLen, fmt, (s32)(g_autoUpdater->GetDownloadProgress() * 100.0f));
        return true;
    }
    return false;
}

// Width reserved on the right of the text column for the scrollbar.
static constexpr s32 kChangelogScrollColumnW = 28;

void feCustomMenuMgr::RebuildChangelogLines() {
    m_changelogLines.clear();
    m_changelogScrollTop = 0;
    m_changelogScrollVisual = 0.0f;

    std::string notes = g_autoUpdater ? g_autoUpdater->GetReleaseNotes() : std::string();
    if (notes.empty()) {
        m_changelogLines.push_back(std::string());
        return;
    }

    if (!g_textManager || !g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        m_changelogLines.push_back(notes);
        return;
    }

    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE), SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE));
    g_textManager->SetWrapWidth(0.0f);
    const f32 maxWidth = SCREEN_SCALE_X((f32)(DEF_CHANGELOG_WINDOW_W - DEF_LABEL_X_PAD * 2 - kChangelogScrollColumnW));

    size_t pos = 0;
    while (pos <= notes.size()) {
        size_t newlinePos = notes.find('\n', pos);
        std::string paragraph = (newlinePos == std::string::npos) ? notes.substr(pos) : notes.substr(pos, newlinePos - pos);

        if (paragraph.empty()) {
            m_changelogLines.push_back(std::string());
        }
        else {
            size_t wordStart = 0;
            std::string currentLine;
            while (wordStart < paragraph.size()) {
                size_t wordEnd = paragraph.find(' ', wordStart);
                if (wordEnd == std::string::npos) wordEnd = paragraph.size();
                std::string word = paragraph.substr(wordStart, wordEnd - wordStart);

                std::string candidate = currentLine.empty() ? word : (currentLine + " " + word);
                if (g_textManager->MeasureString(candidate.c_str()).width <= maxWidth) {
                    currentLine = candidate;
                }
                else {
                    if (!currentLine.empty()) {
                        m_changelogLines.push_back(currentLine);
                        currentLine.clear();
                    }

                    // The word alone doesn't fit even on an empty line - hard-split it by
                    // character so it can never overflow the box (e.g. a long unbroken URL/token).
                    while (!word.empty() && g_textManager->MeasureString(word.c_str()).width > maxWidth) {
                        size_t splitLen = word.size() - 1;
                        while (splitLen > 1 && g_textManager->MeasureString(word.substr(0, splitLen).c_str()).width > maxWidth) {
                            splitLen--;
                        }
                        m_changelogLines.push_back(word.substr(0, splitLen));
                        word = word.substr(splitLen);
                    }
                    currentLine = word;
                }

                wordStart = wordEnd + 1;
            }
            if (!currentLine.empty()) {
                m_changelogLines.push_back(currentLine);
            }
        }

        if (newlinePos == std::string::npos) break;
        pos = newlinePos + 1;
    }

    if (m_changelogLines.empty()) {
        m_changelogLines.push_back(std::string());
    }
}

void feCustomMenuMgr::RenderChangelogBody(s32 panelX, s32 panelY, s32 panelW, s32 panelH, s32 contentTop) const {
    if (!g_textManager || !g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        return;
    }

    const s32 totalLines = (s32)m_changelogLines.size();
    const bool isEmpty = (totalLines == 0) || (totalLines == 1 && m_changelogLines[0].empty());
    const s32 maxVisibleLines = ComputeChangelogVisibleLines(panelH);
    const s32 visibleLines = (totalLines < maxVisibleLines) ? totalLines : maxVisibleLines;

    g_textManager->SetFontByName("Legal");
    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_CHANGELOG_TEXT_W), SCREEN_SCALE_Y(DEF_CHANGELOG_TEXT_H));
    g_textManager->SetAlignment(TextAlign_Left);
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(false);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);
    g_textManager->SetColor(255, 255, 255);

    const s32 textX = panelX + DEF_LABEL_X_PAD;
    if (isEmpty) {
        const char* empty = Localize("FE_UPD_NOCHANGE");
        if (!empty) empty = "No changelog available.";
        g_textManager->PrintString(empty,
                                   SCALE_AND_CENTER_X((f32)textX),
                                   SCREEN_SCALE_Y((f32)(contentTop + DEF_CONTENT_PAD)));
    }
    else {
        const f32 viewportY = (f32)(contentTop + DEF_CONTENT_PAD);
        const f32 viewportH = (f32)(maxVisibleLines * 8);
        SetVerticalScissorPSX(viewportY, viewportH);
        for (s32 lineIndex = 0; lineIndex < totalLines; ++lineIndex) {
            const f32 rowY = viewportY + ((f32)lineIndex - m_changelogScrollVisual) * 8.0f;
            if (rowY + 8.0f <= viewportY || rowY >= viewportY + viewportH) continue;
            g_textManager->PrintString(m_changelogLines[lineIndex].c_str(),
                                       SCALE_AND_CENTER_X((f32)textX),
                                       SCREEN_SCALE_Y(rowY));
        }
        ScreenDraw::SetScissor(0, 0, (s32)(SCREEN_WIDTH + 0.5f), (s32)(SCREEN_HEIGHT + 0.5f));
    }
    g_textManager->SetPromptsEnabled(true);

    if (totalLines > visibleLines) {
        char rangeText[32];
        snprintf(rangeText, sizeof(rangeText), "%d-%d/%d",
                 m_changelogScrollTop + 1,
                 m_changelogScrollTop + visibleLines,
                 totalLines);

        g_textManager->SetFontByName("Menu");
        g_textManager->SetScale(SCREEN_SCALE_Y(DEF_REDEFINE_KEY_TEXT_SCALE), SCREEN_SCALE_Y(DEF_REDEFINE_KEY_TEXT_SCALE));
        g_textManager->SetAlignment(TextAlign_Right);
        g_textManager->SetColor(DEF_TEXT_NORM_R, DEF_TEXT_NORM_G, DEF_TEXT_NORM_B);
        g_textManager->PrintString(rangeText,
                                   SCALE_AND_CENTER_X((f32)(panelX + panelW - DEF_VALUE_X_PAD)),
                                   SCREEN_SCALE_Y((f32)(panelY + panelH - DEF_BOTTOM_BAR_H - DEF_CONTENT_BOTTOM_PAD - DEF_ROW_TEXT_H + DEF_TEXT_Y_OFF)));
    }

    DrawScrollBar((f32)(panelX + panelW - 12), (f32)(contentTop + DEF_CONTENT_PAD),
                  (f32)(maxVisibleLines * 8), totalLines, maxVisibleLines, m_changelogScrollVisual);
}
#endif

PageDef& feCustomMenuMgr::AddPage(
    MenuPage id, const char* title,
    const char* overlay, MenuPage parent, s32 parentEntry, bool pause,
    s32 frameW, s32 frameH) {
    PageDef def;
    std::snprintf(def.titleToken, sizeof(def.titleToken), "%s", title);
    std::snprintf(def.overlayName, sizeof(def.overlayName), "%s", overlay);
    def.parentPage = parent;
    def.parentEntry = parentEntry;
    def.isPause = pause;
    def.autoFrameH = (frameH == -1);
    def.frameW = (frameW == -1) ? DEF_WINDOW_W : frameW;
    def.frameH = def.autoFrameH ? DEF_WINDOW_H : frameH;
    def.entriesOffsetX = 0;
    def.entriesOffsetY = 0;
    def.numEntries = 0;

    m_pages[id] = def;
    return m_pages[id];
}

PageDef& feCustomMenuMgr::AddPopup(PopupKind id, const char* title, const char* overlay,
                                   s32 frameW, s32 frameH) {
    PageDef def;
    std::snprintf(def.titleToken, sizeof(def.titleToken), "%s", title);
    std::snprintf(def.overlayName, sizeof(def.overlayName), "%s", overlay);
    def.parentPage = MenuPage_None;
    def.parentEntry = 0;
    def.isPause = false;
    def.autoFrameH = (frameH == -1);
    def.frameW = (frameW == -1) ? DEF_WINDOW_W : frameW;
    def.frameH = def.autoFrameH ? DEF_WINDOW_H : frameH;
    def.entriesOffsetX = 0;
    def.entriesOffsetY = 0;
    def.numEntries = 0;

    m_popups[id] = def;
    return m_popups[id];
}

void feCustomMenuMgr::SetEntries(PageDef& page, std::initializer_list<Entry> list,
                                 s32 entriesOffsetX, s32 entriesOffsetY) {
    s32 n = 0;
    for (const Entry& e : list) {
        if (n >= MAX_ENTRIES_PER_MENU)
            break;
        page.entries[n++] = e;
    }
    page.numEntries = n;
    page.entriesOffsetX = entriesOffsetX;
    page.entriesOffsetY = entriesOffsetY;
    if (page.autoFrameH) {
        const s32 extraH = CalcPageExtraHeight(page);
        page.frameH = CalcAutoFrameHeight(page.numEntries, extraH);
    }
}

Entry feCustomMenuMgr::Button(const char* tok, EntryEvent ev, MenuPage go) {
    Entry e;
    memset(e.token, 0, sizeof(e.token));
    std::snprintf(e.token, sizeof(e.token), "%s", tok);
    e.type = EntryType_Button;
    e.event = ev;
    e.goPage = go;
    e.binding = EntryBinding_None;
    e.step = 0; e.lo = 0; e.hi = 0;
    e.posX = 0; e.posY = 0;
    return e;
}

Entry feCustomMenuMgr::Slider(const char* tok, EntryBinding binding, s32 step, s32 lo, s32 hi) {
    Entry e;
    memset(e.token, 0, sizeof(e.token));
    std::snprintf(e.token, sizeof(e.token), "%s", tok);
    e.type = EntryType_Slider;
    e.event = EntryEvent_None;
    e.goPage = MenuPage_None;
    e.binding = binding;
    e.step = step; e.lo = lo; e.hi = hi;
    e.posX = 0; e.posY = 0;
    return e;
}

Entry feCustomMenuMgr::List(const char* tok, EntryBinding binding, s32 step, s32 lo, s32 hi) {
    Entry e;
    memset(e.token, 0, sizeof(e.token));
    std::snprintf(e.token, sizeof(e.token), "%s", tok);
    e.type = EntryType_List;
    e.event = EntryEvent_None;
    e.goPage = MenuPage_None;
    e.binding = binding;
    e.step = step; e.lo = lo; e.hi = hi;
    e.posX = 0; e.posY = 0;
    return e;
}

Entry feCustomMenuMgr::Toggle(const char* tok, EntryBinding binding) {
    Entry e;
    memset(e.token, 0, sizeof(e.token));
    std::snprintf(e.token, sizeof(e.token), "%s", tok);
    e.type = EntryType_Toggle;
    e.event = EntryEvent_None;
    e.goPage = MenuPage_None;
    e.binding = binding;
    e.step = 1; e.lo = 0; e.hi = 1;
    e.posX = 0; e.posY = 0;
    return e;
}

Entry feCustomMenuMgr::Info(const char* tok) {
    Entry e;
    memset(e.token, 0, sizeof(e.token));
    std::snprintf(e.token, sizeof(e.token), "%s", tok);
    e.type = EntryType_Info;
    e.event = EntryEvent_None;
    e.goPage = MenuPage_None;
    e.binding = EntryBinding_None;
    e.step = 0; e.lo = 0; e.hi = 0;
    e.posX = 0; e.posY = 0;
    return e;
}

s32 feCustomMenuMgr::CalcAutoFrameHeight(s32 numEntries, s32 extraH) {
    if (numEntries <= 0) {
        return DEF_WINDOW_H;
    }

    const s32 rowSpan = (numEntries - 1) * DEF_ROW_STEP;
    const s32 bodyHeight = DEF_CONTENT_TOP_PAD + DEF_CONTENT_PAD + rowSpan + DEF_ROW_TEXT_H + DEF_CONTENT_BOTTOM_PAD + extraH;
    return DEF_TITLE_BAR_H + DEF_BOTTOM_BAR_H + bodyHeight;
}

s32 feCustomMenuMgr::GetWrappedLineCount(const PageDef& page, const char* label,
                                         const char* fontName, f32 scale) const {
    const f32 wrapWidth = SCREEN_SCALE_X((f32)(page.frameW - DEF_LABEL_X_PAD * 2));
    const f32 useScale = (scale > 0.0f) ? scale : DEF_MENU_TEXT_SCALE;
    s32 lines = 1;
    if (g_textManager && g_textManager->SetFontByName(fontName ? fontName : DEF_MENU_FONT_NAME)) {
        g_textManager->SetScale(SCREEN_SCALE_Y(useScale), SCREEN_SCALE_Y(useScale));
        g_textManager->SetWrapWidth(wrapWidth);
        g_textManager->SetLineSpacing(0);
        g_textManager->SetPromptsEnabled(true);
        lines = g_textManager->CountWrappedLines(label);
        g_textManager->SetWrapWidth(0.0f);
    }
    return (lines < 1) ? 1 : lines;
}

s32 feCustomMenuMgr::GetEntryExtraHeight(const PageDef& page, const Entry& entry) const {
    if (entry.type != EntryType_Info)
        return 0;

    const char* label = Localize(entry.token);
    if (!label)
        label = entry.token;

#if AUTO_UPDATER
    char updateInfoText[256];
    if (BuildUpdateInfoText(entry.token, updateInfoText, (s32)sizeof(updateInfoText))) {
        label = updateInfoText;
    }
#endif

    char assetInfoText[256];
    if (BuildAssetInfoText(entry.token, assetInfoText, (s32)sizeof(assetInfoText))) {
        label = assetInfoText;
    }

    const s32 lines = GetWrappedLineCount(page, label);
    s32 extra = DEF_INFO_ROW_EXTRA + (lines - 1) * DEF_ROW_STEP;

    if (!strcmp(entry.token, "FE_AUTO_WARN")) {
        // The notice draws a 23px-tall spinner below its wrapped info text.
        // Reserve two rows so the auto-sized window encloses it and its padding.
        extra += DEF_ROW_STEP * 2;
    }

#if AUTO_UPDATER
    if (!strcmp(entry.token, "FE_UPD_PRG") || !strcmp(entry.token, "FE_UPD_CHK")) {
        extra += DEF_ROW_STEP; // reserve room for the progress bar drawn below this row
    }
#endif

    if (!strcmp(entry.token, "FE_ASSET_SCAN") || !strcmp(entry.token, "FE_ASSET_EXTG")) {
        extra += DEF_ROW_STEP; // reserve room for the progress bar drawn below this row
    }

    return extra;
}

s32 feCustomMenuMgr::CalcEntryYExtra(const PageDef& page, s32 upToIndex) const {
    s32 extra = 0;
    for (s32 i = 0; i < upToIndex; i++) {
        extra += GetEntryExtraHeight(page, page.entries[i]);
    }
    return extra;
}

s32 feCustomMenuMgr::CalcPageExtraHeight(const PageDef& page) const {
    s32 extra = 0;
    for (s32 i = 0; i < page.numEntries; i++) {
        extra += GetEntryExtraHeight(page, page.entries[i]);
    }
    return extra;
}

void feCustomMenuMgr::ResolveEntryLayout(const PageDef& page, s32 entryIndex,
                                         s32 firstY, s32 baseLabelX, s32 baseValueX, s32 baseCenterX,
                                         s32* outRowTop, s32* outRowTextY,
                                         s32* outLabelX, s32* outValueX, s32* outCenterX) const {
    const Entry& entry = page.entries[entryIndex];
    const s32 autoRowTop = firstY + entryIndex * DEF_ROW_STEP + CalcEntryYExtra(page, entryIndex);
    const s32 autoRowTextY = autoRowTop + DEF_TEXT_Y_OFF;

    s32 rowTop = autoRowTop;
    s32 rowTextY = autoRowTextY;
    s32 labelX = baseLabelX;
    s32 valueX = baseValueX;
    s32 centerX = baseCenterX;

    if (IsAutoEntryPosition(entry)) {
        rowTop += page.entriesOffsetY;
        rowTextY += page.entriesOffsetY;
        labelX += page.entriesOffsetX;
        valueX += page.entriesOffsetX;
        centerX += page.entriesOffsetX;
    }
    else {
        const s32 rowShiftX = entry.posX - baseCenterX;
        centerX = entry.posX;
        labelX = baseLabelX + rowShiftX;
        valueX = baseValueX + rowShiftX;
        rowTextY = entry.posY;
        rowTop = rowTextY - DEF_TEXT_Y_OFF;
    }

    if (outRowTop) {
        *outRowTop = rowTop;
    }
    if (outRowTextY) {
        *outRowTextY = rowTextY;
    }
    if (outLabelX) {
        *outLabelX = labelX;
    }
    if (outValueX) {
        *outValueX = valueX;
    }
    if (outCenterX) {
        *outCenterX = centerX;
    }
}

void feCustomMenuMgr::LoadControllerOverlayTexture() {
    if (m_controllerTexture) {
        return;
    }

    char path[128];
    ControllerPromptManager::BuildOverlayPath(path, (s32)sizeof(path));
    m_controllerTexture = tTexture::LoadFromImagePath(path);
    if (!m_controllerTexture) {
        LOG("[CustomMenu] Failed to load %s", path);
    }
}

void feCustomMenuMgr::ReloadControllerPromptTextures() {
    if (m_controllerTexture) {
        m_controllerTexture->Release();
        m_controllerTexture = nullptr;
    }
    LoadControllerOverlayTexture();
    PromptIcons::ResetGamepadSheet();
}

void feCustomMenuMgr::LoadMenuOrnamentTexture() {
    m_menuOrnamentTexture = tTexture::LoadFromImagePath(kMenuOrnamentTexturePath);
    if (!m_menuOrnamentTexture) {
        LOG("[CustomMenu] Failed to load %s", kMenuOrnamentTexturePath);
    }

    m_redDragonTex = tTexture::LoadFromImagePath(kRedDragonTexturePath);
    if (!m_redDragonTex) {
        LOG("[CustomMenu] Failed to load %s", kRedDragonTexturePath);
    }
    m_goldDragonTex = tTexture::LoadFromImagePath(kGoldDragonTexturePath);
    if (!m_goldDragonTex) {
        LOG("[CustomMenu] Failed to load %s", kGoldDragonTexturePath);
    }
    m_greyDragonTex = tTexture::LoadFromImagePath(kGreyDragonTexturePath);
    if (!m_greyDragonTex) {
        LOG("[CustomMenu] Failed to load %s", kGreyDragonTexturePath);
    }
    m_takeTex = tTexture::LoadFromImagePath(kTakeTexturePath);
    if (!m_takeTex) {
        LOG("[CustomMenu] Failed to load %s", kTakeTexturePath);
    }
}

void feCustomMenuMgr::LoadSplashTextures() {
    if (!m_titleScreenTextureTried) {
        m_titleScreenTextureTried = true;

        m_titleScreenBackgroundTexture = tTexture::LoadFromImagePath(kTitleScreenBackgroundTexturePath);
        if (m_titleScreenBackgroundTexture && m_titleScreenBackgroundTexture->GetTexture()) {
            m_titleScreenBackgroundTexture->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);
        }
        if (!m_titleScreenBackgroundTexture) {
            LOG("[CustomMenu] Failed to load title splash background texture (%s)", kTitleScreenBackgroundTexturePath);
        }

        m_titleScreenJackieTexture = tTexture::LoadFromImagePath(kTitleScreenJackieTexturePath);
        if (m_titleScreenJackieTexture && m_titleScreenJackieTexture->GetTexture()) {
            m_titleScreenJackieTexture->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);
        }
        if (!m_titleScreenJackieTexture) {
            LOG("[CustomMenu] Failed to load title splash Jackie texture (%s)", kTitleScreenJackieTexturePath);
        }

        m_titleScreenLogoTexture = tTexture::LoadFromImagePath(kTitleScreenLogoTexturePath);
        if (m_titleScreenLogoTexture && m_titleScreenLogoTexture->GetTexture()) {
            m_titleScreenLogoTexture->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);
        }
        if (!m_titleScreenLogoTexture) {
            LOG("[CustomMenu] Failed to load title splash logo texture (%s)", kTitleScreenLogoTexturePath);
        }
    }

    if (!m_gameOverTextureTried) {
        m_gameOverTextureTried = true;
        m_gameOverTexture = tTexture::LoadFromImagePath(kGameOverTexturePath);
        if (m_gameOverTexture && m_gameOverTexture->GetTexture()) {
            m_gameOverTexture->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);
        }
        if (!m_gameOverTexture) {
            LOG("[CustomMenu] Failed to load game over texture (%s)", kGameOverTexturePath);
        }
    }

    if (!m_loadingScreenTexturesTried) {
        m_loadingScreenTexturesTried = true;
        m_loadingBarTexture = tTexture::LoadFromImagePath(kLoadingBarTexturePath);
        if (m_loadingBarTexture && m_loadingBarTexture->GetTexture()) {
            m_loadingBarTexture->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);
        }
        if (!m_loadingBarTexture) {
            LOG("[CustomMenu] Failed to load loading bar texture (%s)", kLoadingBarTexturePath);
        }
    }
}

void feCustomMenuMgr::LoadSliderTextures() {
    if (!m_sliderOTex) {
        m_sliderOTex = tTexture::LoadFromImagePath(kSliderOTexturePath);
        m_sliderOTex->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);

        if (!m_sliderOTex) {
            LOG("[CustomMenu] Failed to load slider empty texture (%s)", kSliderOTexturePath);
        }
    }

    if (!m_sliderFTex) {
        m_sliderFTex = tTexture::LoadFromImagePath(kSliderFTexturePath);
        m_sliderFTex->GetTexture()->SetFilterMode(PDDI_FILTER_BILINEAR);

        if (!m_sliderFTex) {
            LOG("[CustomMenu] Failed to load slider filled texture (%s)", kSliderFTexturePath);
        }
    }
}

bool feCustomMenuMgr::GetSplashScreenRect(f32* outX, f32* outY, f32* outW, f32* outH) const {
    const f32 screenW = SCREEN_WIDTH;
    const f32 screenH = SCREEN_HEIGHT;
    if (screenW <= 0.0f || screenH <= 0.0f) {
        return false;
    }

    f32 drawW = screenW;
    f32 drawH = drawW / kSplashScreenAspect;
    if (drawH > screenH) {
        drawH = screenH;
        drawW = drawH * kSplashScreenAspect;
    }

    const f32 drawX = (screenW - drawW) * 0.5f;
    const f32 drawY = (screenH - drawH) * 0.5f;

    if (outX) {
        *outX = drawX;
    }
    if (outY) {
        *outY = drawY;
    }
    if (outW) {
        *outW = drawW;
    }
    if (outH) {
        *outH = drawH;
    }

    return true;
}

void feCustomMenuMgr::ResetTitleIntro() {
    m_titleIntroSec = 0.0f;
    m_titleContentHidden = false;
}

void feCustomMenuMgr::HideTitleContent() {
    m_titleContentHidden = true;
}

void feCustomMenuMgr::BeginTitleStartTransition() {
    m_titleTransitionActive = true;
    m_titleTransitionFinished = false;
    m_titleTransitionSec = 0.0f;
}

void feCustomMenuMgr::EndTitleStartTransition() {
    m_titleTransitionActive = false;
    m_titleTransitionFinished = false;
    m_titleTransitionSec = 0.0f;
}

bool feCustomMenuMgr::DrawTitleScreen() {
    LoadSplashTextures();
    if (!m_titleScreenBackgroundTexture || !m_titleScreenJackieTexture || !m_titleScreenLogoTexture) {
        return false;
    }

    f32 drawX = 0.0f;
    f32 drawY = 0.0f;
    f32 drawW = 0.0f;
    f32 drawH = 0.0f;
    if (!GetSplashScreenRect(&drawX, &drawY, &drawW, &drawH)) {
        return false;
    }

    // Drives the glow/ray motion below and the logo's idle bob; tracked
    // unconditionally so the logo still feels alive even if the off-screen
    // effects are unavailable (see EnsureTitleScreenEffects).
    const f32 dt = g_time ? g_time->GetDeltaTime() : (1.0f / 30.0f);
    const f32 clampedDt = std::min(std::max(dt, 0.0f), 0.1f);
    m_titleScreenAnimSec += clampedDt;

    // Intro zoom: the logo starts very close to the camera (large) and eases
    // out down to its idle scale over kTitleIntroDuration.
    f32 introScale = 1.0f;
    if (m_titleIntroSec < kTitleIntroDuration) {
        m_titleIntroSec += clampedDt;
        const f32 p = std::min(m_titleIntroSec / kTitleIntroDuration, 1.0f);
        const f32 ease = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
        introScale = 3.0f - ease * 2.0f;
    }

    // Press-start transition: fades the logo/Jackie out over
    // kTitleStartTransitionDuration before the menu actually activates.
    if (m_titleTransitionActive && !m_titleTransitionFinished) {
        m_titleTransitionSec += clampedDt;
        if (m_titleTransitionSec >= kTitleStartTransitionDuration) {
            m_titleTransitionSec = kTitleStartTransitionDuration;
            m_titleTransitionFinished = true;
        }
    }
    const f32 transitionP = m_titleTransitionActive
        ? (m_titleTransitionSec / kTitleStartTransitionDuration) : 0.0f;
    const u8 titleFadeAlpha = (u8)(255.0f * (1.0f - transitionP));

    // Idle logo motion: a gentle continuous sway plus a periodic little pop
    // (position + scale), and a true pseudo-3D pitch/yaw/roll tilt on top,
    // so the logo never sits dead still. Computed once here and reused for
    // both the on-screen draw and the warped seed the glow/rays sample from,
    // so the effects track the logo's motion instead of staying static.
    const f32 t = m_titleScreenAnimSec;
    const f32 sway = std::sin(t * 0.9f);
    const f32 bump = std::pow(0.5f + 0.5f * std::sin(t * 1.7f + 0.6f), 2.0f);
    const f32 logoOffsetY = -(sway * 0.006f + bump * 0.014f) * drawH;
    const f32 logoScale = (1.0f + bump * 0.05f) * introScale;
    const f32 logoHalfW = (drawW * logoScale) * 0.5f;
    const f32 logoHalfH = (drawH * logoScale) * 0.5f;
    const f32 logoLocalCenterX = drawW * 0.5f;
    const f32 logoLocalCenterY = drawH * 0.5f + logoOffsetY;
    const f32 tiltPitch = std::sin(t * 0.5f) * 0.05f;
    const f32 tiltYaw = 0;//std::sin(t * 0.6f + 0.3f) * 0.09f;
    const f32 tiltRoll = std::sin(t * 2.5f + 1.1f) * 0.015f;
    const f32 tiltFocal = logoHalfH * 6.0f;

    ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 255);
    ScreenDraw::DrawQuad(m_titleScreenBackgroundTexture, drawX, drawY, drawW, drawH);

    if (IsActive() || m_titleContentHidden)
        return true;

    if (EnsureTitleScreenEffects(drawW, drawH)) {
        const bool firstRun = (m_titleEffectsLastUpdateSec < 0.0f);
        const bool refreshEffects = firstRun
            || (m_titleScreenAnimSec - m_titleEffectsLastUpdateSec) >= (1.0f / kTitleEffectsUpdateHz);

        if (refreshEffects) {
            m_titleEffectsLastUpdateSec = m_titleScreenAnimSec;

            // Render the tilted/bobbing logo into its own small seed buffer so
            // the glow and rays below sample the same animated shape as the
            // on-screen logo instead of its static source texture.
            if (p3d::context->SetRenderTarget(m_titleScreenLogoSeed)) {
                m_titleScreenLogoTiltShader->SetTexture(0, m_titleScreenLogoTexture->GetTexture());
                m_titleScreenLogoTiltShader->SetVector("uTiltRect",
                                                       logoLocalCenterX, logoLocalCenterY, logoHalfW, logoHalfH);
                m_titleScreenLogoTiltShader->SetVector("uTiltAngles",
                                                       tiltPitch, tiltYaw, tiltRoll, tiltFocal);
                ScreenDraw::DrawShaderQuad(m_titleScreenLogoTiltShader, 0.0f, 0.0f, drawW, drawH,
                                           0.0f, 0.0f, 1.0f, 1.0f, PDDI_BLEND_NONE, drawW, drawH);
                p3d::context->SetRenderTarget(nullptr);
            }

            // Pass 1: soft halo glow behind Jackie and the logo
            if (p3d::context->SetRenderTarget(m_titleScreenGlow)) {
                m_titleScreenGlowShader->SetVector("uGlowParams", 0.025f, 0.8f, 0.0f, 0.0f);
                m_titleScreenGlowShader->SetVector("uGlowMotion", 0.05f, 0.0f, 0.2f, 0.0f);
                m_titleScreenGlowShader->SetFloat("uTime", m_titleScreenAnimSec);

                // The logo seed is a render target (vertically inverted relative
                // to image textures), hence the flipped v0/v1 here.
                m_titleScreenGlowShader->SetTexture(0, m_titleScreenLogoSeed->GetTexture());
                ScreenDraw::DrawShaderQuad(m_titleScreenGlowShader, 0.0f, 0.0f, drawW, drawH,
                                           0.0f, 1.0f, 1.0f, 1.0f, PDDI_BLEND_NONE, drawW, drawH);

                m_titleScreenGlowShader->SetTexture(0, m_titleScreenJackieTexture->GetTexture());
                ScreenDraw::DrawShaderQuad(m_titleScreenGlowShader, 0.0f, 0.0f, drawW, drawH,
                                           0.0f, 0.0f, 1.0f, 1.0f, PDDI_BLEND_ADD, drawW, drawH);

                p3d::context->SetRenderTarget(nullptr);
            }

            // Pass 2: god rays on top of the glow
            if (p3d::context->SetRenderTarget(m_titleScreenGodRays)) {
                // The render target's pixel size is capped (see
                // EnsureTitleScreenEffects) and can differ from drawW/drawH, so
                // the quad's projection must use drawW/drawH as its own canvas
                // instead of the live screen size -- otherwise this pass
                // scales/positions wrong whenever the window isn't exactly 16:9
                // and the splash rect gets letterboxed.
                m_titleScreenGodRaysShader->SetVector("uRayParams", 0.5f, 0.5f, 0.3f, 0.94f);
                m_titleScreenGodRaysShader->SetVector("uRayMotion", 0.0f, 0.0f, 0.04f, 1.5f);
                m_titleScreenGodRaysShader->SetFloat("uExposure", 4.5f);
                m_titleScreenGodRaysShader->SetFloat("uTime", m_titleScreenAnimSec);

                // The logo seed is a render target (vertically inverted relative
                // to image textures), hence the flipped v0/v1 here.
                m_titleScreenGodRaysShader->SetTexture(0, m_titleScreenLogoSeed->GetTexture());
                ScreenDraw::DrawShaderQuad(m_titleScreenGodRaysShader, 0.0f, 0.0f, drawW, drawH,
                                           0.0f, 1.0f, 1.0f, 0.0f, PDDI_BLEND_NONE, drawW, drawH);

                m_titleScreenGodRaysShader->SetVector("uRayParams", 0.55f, 0.45f, 1.9f, 0.92f);
                m_titleScreenGodRaysShader->SetVector("uRayMotion", 0.0f, 0.0f, 0.01f, 1.0f);
                m_titleScreenGodRaysShader->SetFloat("uExposure", 1.8f);
                m_titleScreenGodRaysShader->SetFloat("uTime", m_titleScreenAnimSec);

                m_titleScreenGodRaysShader->SetTexture(0, m_titleScreenJackieTexture->GetTexture());
                ScreenDraw::DrawShaderQuad(m_titleScreenGodRaysShader, 0.0f, 0.0f, drawW, drawH,
                                           0.0f, 0.0f, 1.0f, 1.0f, PDDI_BLEND_ADD, drawW, drawH);

                p3d::context->SetRenderTarget(nullptr);
            }
        }

        m_titleScreenCompositeShader->SetTexture(0, m_titleScreenGlow->GetTexture());
        m_titleScreenCompositeShader->SetColour(0, pddiColour(titleFadeAlpha, titleFadeAlpha, titleFadeAlpha, 255));
        ScreenDraw::DrawShaderQuad(m_titleScreenCompositeShader, drawX, drawY, drawW, drawH,
                                   0.0f, 1.0f, 1.0f, 0.0f, PDDI_BLEND_ADD);

        m_titleScreenCompositeShader->SetTexture(0, m_titleScreenGodRays->GetTexture());
        m_titleScreenCompositeShader->SetColour(0, pddiColour(titleFadeAlpha, titleFadeAlpha, titleFadeAlpha, 255));
        ScreenDraw::DrawShaderQuad(m_titleScreenCompositeShader, drawX, drawY, drawW, drawH,
                                   0.0f, 1.0f, 1.0f, 0.0f, PDDI_BLEND_ADD);
    }

    ScreenDraw::DrawQuad(m_titleScreenJackieTexture, drawX, drawY, drawW, drawH,
                         0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, titleFadeAlpha);

    if (m_titleScreenLogoTiltShader) {
        m_titleScreenLogoTiltShader->SetTexture(0, m_titleScreenLogoTexture->GetTexture());
        m_titleScreenLogoTiltShader->SetVector("uTiltRect",
                                               drawX + logoLocalCenterX, drawY + logoLocalCenterY, logoHalfW, logoHalfH);
        m_titleScreenLogoTiltShader->SetVector("uTiltAngles",
                                               tiltPitch, tiltYaw, tiltRoll, tiltFocal);
        m_titleScreenLogoTiltShader->SetColour(0, pddiColour(255, 255, 255, titleFadeAlpha));
        ScreenDraw::DrawShaderQuad(m_titleScreenLogoTiltShader, drawX, drawY, drawW, drawH,
                                   0.0f, 0.0f, 1.0f, 1.0f, PDDI_BLEND_ALPHA);
    }
    else {
        // Fallback without the 3D tilt shader: plain bob/scale, no tilt.
        const f32 logoW = logoHalfW * 2.0f;
        const f32 logoH = logoHalfH * 2.0f;
        const f32 logoX = drawX - (logoW - drawW) * 0.5f;
        const f32 logoY = drawY + logoOffsetY - (logoH - drawH) * 0.5f;
        ScreenDraw::DrawQuad(m_titleScreenLogoTexture, logoX, logoY, logoW, logoH,
                             0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, titleFadeAlpha);
    }

    // Gold debris: small fading dots sprayed left/right out of the screen
    // center, with their own gravity/drag.
    const f32 debrisX = drawX + drawW * kTitleDebrisCenterU;
    const f32 debrisY = drawY + drawH * kTitleDebrisCenterV;
    UpdateTitleDebrisParticles(dt, debrisX, debrisY, drawW);
    DrawTitleDebrisParticles();

    return true;
}

void feCustomMenuMgr::DrawTitleStartPrompt(s32 baseX, s32 baseY) {
    f32 bgX = 0.0f;
    f32 bgY = 0.0f;
    f32 bgW = 0.0f;
    f32 bgH = 0.0f;
    if (!GetSplashScreenRect(&bgX, &bgY, &bgW, &bgH)) {
        return;
    }

    const f32 refW = SCREEN_SCALE_X(DEFAULT_SCREEN_WIDTH);
    const f32 refH = SCREEN_SCALE_Y(DEFAULT_SCREEN_HEIGHT);
    if (refW <= 0.0f || refH <= 0.0f) {
        return;
    }

    const f32 splashScaleX = bgW / refW;
    const f32 splashScaleY = bgH / refH;

    if (!g_textManager || !g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        return;
    }

    m_pulse.Update();
    const xcColour1555 pulseColor = m_pulse.GetColor();

    // While the press-start transition is running, zoom the prompt in
    // slightly and fade it out alongside the logo/Jackie.
    f32 transitionP = 0.0f;
    if (m_titleTransitionActive) {
        transitionP = std::min(m_titleTransitionSec / kTitleStartTransitionDuration, 1.0f);
    }
    const f32 promptAlphaScale = 1.0f - transitionP;
    if (promptAlphaScale <= 0.0f) {
        return;
    }
    const f32 promptZoom = 1.0f + transitionP * 0.4f;

    const char* promptText = Localize("FE_PST");
    const f32 promptX = bgX + SCREEN_SCALE_X((f32)baseX) * splashScaleX;
    const f32 promptY = bgY + SCREEN_SCALE_Y((f32)baseY) * splashScaleY;

    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TITLE_SCALE) * promptZoom,
                            SCREEN_SCALE_Y(DEF_MENU_TITLE_SCALE) * promptZoom);
    g_textManager->SetAlignment(TextAlign_Center);
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(true);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);
    g_textManager->SetColor(pulseColor.GetRed8(),
                            pulseColor.GetGreen8(),
                            pulseColor.GetBlue8(),
                            (u8)(255.0f * promptAlphaScale));
    g_textManager->PrintString(promptText, promptX, promptY);
}

bool feCustomMenuMgr::DrawGameOverScreen() {
    LoadSplashTextures();
    if (!m_titleScreenBackgroundTexture || !m_gameOverTexture) {
        return false;
    }

    f32 drawX = 0.0f;
    f32 drawY = 0.0f;
    f32 drawW = 0.0f;
    f32 drawH = 0.0f;
    if (!GetSplashScreenRect(&drawX, &drawY, &drawW, &drawH)) {
        return false;
    }

    ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 255);
    ScreenDraw::DrawQuad(m_titleScreenBackgroundTexture, drawX, drawY, drawW, drawH);
    ScreenDraw::DrawQuad(m_gameOverTexture, drawX, drawY, drawW, drawH);
    return true;
}

void feCustomMenuMgr::DrawGameOverContinuePrompt(s32 baseX, s32 baseY, u8 r, u8 g, u8 b, u8 a) {
    f32 bgX = 0.0f;
    f32 bgY = 0.0f;
    f32 bgW = 0.0f;
    f32 bgH = 0.0f;
    if (!GetSplashScreenRect(&bgX, &bgY, &bgW, &bgH)) {
        return;
    }

    const f32 refW = SCREEN_SCALE_X(DEFAULT_SCREEN_WIDTH);
    const f32 refH = SCREEN_SCALE_Y(DEFAULT_SCREEN_HEIGHT);
    if (refW <= 0.0f || refH <= 0.0f) {
        return;
    }

    const f32 splashScaleX = bgW / refW;
    const f32 splashScaleY = bgH / refH;

    if (!g_textManager || !g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        return;
    }

    const char* promptText = Localize("FE_TLC");
    const f32 promptX = bgX + SCREEN_SCALE_X((f32)baseX) * splashScaleX;
    const f32 promptY = bgY + SCREEN_SCALE_Y((f32)baseY) * splashScaleY;

    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TITLE_SCALE), SCREEN_SCALE_Y(DEF_MENU_TITLE_SCALE));
    g_textManager->SetAlignment(TextAlign_Center);
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(true);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);
    g_textManager->SetColor(r, g, b, a);
    g_textManager->PrintString(promptText, promptX, promptY);
}

bool feCustomMenuMgr::DrawLoadingScreen(u8 fill, u8 pulseR8) {
    LoadSplashTextures();
    if (!m_titleScreenBackgroundTexture || !m_loadingBarTexture) {
        return false;
    }

    f32 drawX = 0.0f;
    f32 drawY = 0.0f;
    f32 drawW = 0.0f;
    f32 drawH = 0.0f;
    if (!GetSplashScreenRect(&drawX, &drawY, &drawW, &drawH)) {
        return false;
    }

    // Clear to black first so narrower/taller targets produce clean letterbox bars.
    ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 255);
    ScreenDraw::DrawQuad(m_titleScreenBackgroundTexture, drawX, drawY, drawW, drawH);
    ScreenDraw::DrawQuad(m_loadingBarTexture, drawX, drawY, drawW, drawH);

    if (fill > 0) {
        if (fill > 100) {
            fill = 100;
        }

        static constexpr f32 kLoadingBarFillLeftPx = 597.0f;
        static constexpr f32 kLoadingBarFillTopPx = 635.0f;
        static constexpr f32 kLoadingBarFillWidthPx = 638.0f;
        static constexpr f32 kLoadingBarFillHeightPx = 23.0f;

        const f32 scaleX = drawW / 1920.0f;
        const f32 scaleY = drawH / 1080.0f;
        const f32 fillX = drawX + kLoadingBarFillLeftPx * scaleX;
        const f32 fillY = drawY + kLoadingBarFillTopPx * scaleY;
        const f32 fillW = kLoadingBarFillWidthPx * scaleX * (fill / 100.0f);
        const f32 fillH = kLoadingBarFillHeightPx * scaleY;
        ScreenDraw::DrawColoredRect(fillX, fillY, fillW, fillH, pulseR8, 0, 0, 255);
    }

    return true;
}

void feCustomMenuMgr::DrawLegalScreen(f32 alpha01) {
    ScreenDraw::DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 255);

    if (alpha01 <= 0.0f) {
        return;
    }
    if (alpha01 > 1.0f) {
        alpha01 = 1.0f;
    }

    if (!g_textManager || !g_textManager->SetFontByName("Legal")) {
        return;
    }

    static constexpr f32 kColumnWidth = 420.0f;
    static constexpr f32 kVerticalUpOffset = 4.0f;

    g_textManager->SetScale(SCREEN_SCALE_Y(0.2f), SCREEN_SCALE_Y(0.2f));
    g_textManager->SetAlignment(TextAlign_Left);
    g_textManager->SetWrapWidth(SCREEN_SCALE_X(kColumnWidth));
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(false);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(false);
    g_textManager->SetColor(255, 255, 255, (u8)(alpha01 * 255.0f));

    const TextBounds bounds = g_textManager->MeasureString(LEGAL_TEXT);
    const f32 columnLeftX = SCALE_AND_CENTER_X(DEFAULT_SCREEN_WIDTH / 2.0f - kColumnWidth / 2.0f);
    const f32 topY = SCREEN_SCALE_Y(DEFAULT_SCREEN_HEIGHT / 2.0f - kVerticalUpOffset) - bounds.height / 2.0f;

    g_textManager->PrintString(LEGAL_TEXT, columnLeftX, topY);
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetPromptsEnabled(true);
}

// PC: scales an alpha constant by a 0..1 fade multiplier, e.g. for popup-page
// fade in/out (DrawPopupWindow and its content render functions below).
static u8 ScaleAlphaU8(u8 a, f32 alpha01) {
    if (a == 0 || alpha01 >= 1.0f) {
        return a;
    }
    if (alpha01 <= 0.0f) {
        return 0;
    }
    const s32 scaled = (s32)((f32)a * alpha01 + 0.5f);
    return (scaled < 0) ? 0 : ((scaled > 255) ? 255 : (u8)scaled);
}

static void DrawGouraudRectPSX(f32 x, f32 y, f32 w, f32 h,
                               u8 topR, u8 topG, u8 topB,
                               u8 bottomR, u8 bottomG, u8 bottomB,
                               u8 alpha) {
    const f32 x0 = SCALE_AND_CENTER_X(x);
    const f32 y0 = SCREEN_SCALE_Y(y);
    const f32 x1 = SCALE_AND_CENTER_X(x + w);
    const f32 y1 = SCREEN_SCALE_Y(y + h);

    ScreenDraw::DrawGouraudQuad(
        x0, y0, topR, topG, topB, alpha,
        x1, y0, topR, topG, topB, alpha,
        x0, y1, bottomR, bottomG, bottomB, alpha,
        x1, y1, bottomR, bottomG, bottomB, alpha);

}

static void DrawGouraudRectPSX(s32 x, s32 y, s32 w, s32 h,
                               u8 topR, u8 topG, u8 topB,
                               u8 bottomR, u8 bottomG, u8 bottomB,
                               u8 alpha) {
    DrawGouraudRectPSX((f32)x, (f32)y, (f32)w, (f32)h,
                       topR, topG, topB,
                       bottomR, bottomG, bottomB,
                       alpha);
}

static f32 GetMenuBorderPx() {
    return SCREEN_SCALE_Y((f32)DEF_BORDER_W);
}

static void DrawRectPSX(f32 x, f32 y, f32 w, f32 h, u8 r, u8 g, u8 b, u8 a) {
    if (w <= 0 || h <= 0)
        return;

    ScreenDraw::DrawColoredRect(
        SCALE_AND_CENTER_X(x),
        SCREEN_SCALE_Y(y),
        SCREEN_SCALE_X(w),
        SCREEN_SCALE_Y(h),
        r, g, b, a);
}

static bool IsNeutralMenuColor(const xcColour1555& color) {
    return color.GetRed8() == 128 && color.GetGreen8() == 128 && color.GetBlue8() == 128;
}

static void DrawSliderCircleMeterPSX(f32 rightX, f32 textY, f32 value, tTexture* sliderOTex, tTexture* sliderFTex) {
    static constexpr s32 kSegments = DEF_SLIDER_CIRCLE_SEGMENTS;
    static constexpr f32 kSliderIconSize = 12.0f;

    if (value < 0)
        value = 0;
    if (value > 100)
        value = 100;

    s32 filled = (s32)((value * (f32)kSegments) / 100.0f);
    if (filled < 0) filled = 0;
    if (filled > kSegments) filled = kSegments;

    if (!sliderOTex || !sliderFTex) {
        return;
    }

    const f32 step = DEF_SLIDER_CIRCLE_STEP;
    const f32 baseX = rightX - kSegments * step;
    const f32 baseY = textY;

    for (s32 i = 0; i < kSegments; i++) {
        const bool isFilled = (i < filled);
        tTexture* tex = isFilled ? sliderFTex : sliderOTex;
        const f32 drawX = baseX + i * step + (step - kSliderIconSize) / 2;
        ScreenDraw::DrawQuad(tex,
                             SCALE_AND_CENTER_X(drawX), SCREEN_SCALE_Y(baseY),
                             SCREEN_SCALE_Y(kSliderIconSize), SCREEN_SCALE_Y(kSliderIconSize),
                             0.0f, 0.0f, 1.0f, 1.0f,
                             255, 255, 255, 255);
    }
}

static void DrawSliderCircleMeterPSX(s32 rightX, s32 textY, s32 value, tTexture* sliderOTex, tTexture* sliderFTex) {
    DrawSliderCircleMeterPSX((f32)rightX, (f32)textY, (f32)value, sliderOTex, sliderFTex);
}

static void DrawUniformBorderRectPSX(f32 x, f32 y, f32 w, f32 h, f32 borderPx,
                                     u8 r, u8 g, u8 b, u8 a) {
    if (w <= 0 || h <= 0 || borderPx <= 0.0f)
        return;

    const f32 x0 = SCALE_AND_CENTER_X(x);
    const f32 y0 = SCREEN_SCALE_Y(y);
    const f32 x1 = SCALE_AND_CENTER_X(x + w);
    const f32 y1 = SCREEN_SCALE_Y(y + h);
    const f32 rectW = x1 - x0;
    const f32 rectH = y1 - y0;
    if (rectW <= 0.0f || rectH <= 0.0f)
        return;

    const f32 t = borderPx;
    ScreenDraw::DrawColoredRect(x0, y0, rectW, t, r, g, b, a);
    ScreenDraw::DrawColoredRect(x0, y1 - t, rectW, t, r, g, b, a);
    ScreenDraw::DrawColoredRect(x0, y0, t, rectH, r, g, b, a);
    ScreenDraw::DrawColoredRect(x1 - t, y0, t, rectH, r, g, b, a);
}

static void DrawUniformBorderRectPSX(s32 x, s32 y, s32 w, s32 h, f32 borderPx,
                                     u8 r, u8 g, u8 b, u8 a) {
    DrawUniformBorderRectPSX((f32)x, (f32)y, (f32)w, (f32)h, borderPx, r, g, b, a);
}

static void DrawUniformHLinePSX(f32 x, f32 y, f32 w, f32 linePx,
                                u8 r, u8 g, u8 b, u8 a) {
    if (w <= 0 || linePx <= 0.0f)
        return;

    const f32 x0 = SCALE_AND_CENTER_X(x);
    const f32 x1 = SCALE_AND_CENTER_X(x + w);
    const f32 y0 = SCREEN_SCALE_Y(y);
    const f32 drawW = x1 - x0;
    if (drawW <= 0.0f)
        return;

    ScreenDraw::DrawColoredRect(x0, y0, drawW, linePx, r, g, b, a);
}

static void DrawUniformHLinePSX(s32 x, s32 y, s32 w, f32 linePx,
                                u8 r, u8 g, u8 b, u8 a) {
    DrawUniformHLinePSX((f32)x, (f32)y, (f32)w, linePx, r, g, b, a);
}

static void DrawUniformVLinePSX(f32 x, f32 y, f32 h, f32 linePx,
                                u8 r, u8 g, u8 b, u8 a) {
    if (h <= 0 || linePx <= 0.0f)
        return;

    const f32 x0 = SCALE_AND_CENTER_X(x);
    const f32 y0 = SCREEN_SCALE_Y(y);
    const f32 y1 = SCREEN_SCALE_Y(y + h);
    const f32 drawH = y1 - y0;
    if (drawH <= 0.0f)
        return;

    ScreenDraw::DrawColoredRect(x0, y0, linePx, drawH, r, g, b, a);
}

static void DrawUniformVLinePSX(s32 x, s32 y, s32 h, f32 linePx,
                                u8 r, u8 g, u8 b, u8 a) {
    DrawUniformVLinePSX((f32)x, (f32)y, (f32)h, linePx, r, g, b, a);
}

static void DrawUniformBorderFillRectPSX(f32 x, f32 y, f32 w, f32 h, f32 borderPx,
                                         u8 borderR, u8 borderG, u8 borderB, u8 borderA,
                                         u8 fillR, u8 fillG, u8 fillB, u8 fillA) {
    if (w <= 0 || h <= 0)
        return;

    const f32 x0 = SCALE_AND_CENTER_X(x);
    const f32 y0 = SCREEN_SCALE_Y(y);
    const f32 x1 = SCALE_AND_CENTER_X(x + w);
    const f32 y1 = SCREEN_SCALE_Y(y + h);
    const f32 rectW = x1 - x0;
    const f32 rectH = y1 - y0;
    if (rectW <= 0.0f || rectH <= 0.0f)
        return;

    ScreenDraw::DrawColoredRect(x0, y0, rectW, rectH, borderR, borderG, borderB, borderA);

    if (borderPx <= 0.0f)
        return;

    const f32 innerX = x0 + borderPx;
    const f32 innerY = y0 + borderPx;
    const f32 innerW = rectW - borderPx * 2.0f;
    const f32 innerH = rectH - borderPx * 2.0f;
    if (innerW > 0.0f && innerH > 0.0f) {
        ScreenDraw::DrawColoredRect(innerX, innerY, innerW, innerH, fillR, fillG, fillB, fillA);
    }
}

static void DrawUniformBorderFillRectPSX(s32 x, s32 y, s32 w, s32 h, f32 borderPx,
                                         u8 borderR, u8 borderG, u8 borderB, u8 borderA,
                                         u8 fillR, u8 fillG, u8 fillB, u8 fillA) {
    DrawUniformBorderFillRectPSX((f32)x, (f32)y, (f32)w, (f32)h, borderPx,
                                 borderR, borderG, borderB, borderA,
                                 fillR, fillG, fillB, fillA);
}

static void DrawGouraudRectPSXVertical(f32 x, f32 y, f32 w, f32 h,
                                       u8 leftR, u8 leftG, u8 leftB,
                                       u8 rightR, u8 rightG, u8 rightB,
                                       u8 alpha) {
    const f32 x0 = SCALE_AND_CENTER_X(x);
    const f32 y0 = SCREEN_SCALE_Y(y);
    const f32 x1 = SCALE_AND_CENTER_X(x + w);
    const f32 y1 = SCREEN_SCALE_Y(y + h);

    ScreenDraw::DrawGouraudQuad(
        x0, y0, leftR, leftG, leftB, alpha,
        x1, y0, rightR, rightG, rightB, alpha,
        x0, y1, leftR, leftG, leftB, alpha,
        x1, y1, rightR, rightG, rightB, alpha);
}

static void DrawMenuOrnament(tTexture* symbolTex, f32 x, f32 y, u8 alpha = DEF_ORN_A) {
    if (!symbolTex) {
        return;
    }

    ScreenDraw::DrawQuad(
        symbolTex,
        SCALE_AND_CENTER_X(x),
        SCREEN_SCALE_Y(y),
        SCREEN_SCALE_X((f32)DEF_ORN_W),
        SCREEN_SCALE_Y((f32)DEF_ORN_H),
        0.0f, 0.0f, 1.0f, 1.0f,
        DEF_ORN_R, DEF_ORN_G, DEF_ORN_B, alpha);
}

static void DrawMenuOrnament(tTexture* symbolTex, s32 x, s32 y, u8 alpha = DEF_ORN_A) {
    DrawMenuOrnament(symbolTex, (f32)x, (f32)y, alpha);
}

void feCustomMenuMgr::DrawMenuWindow(s32 x, s32 y, s32 w, s32 h, const char* title, f32 alpha01) const {
    const s32 titleY0 = y;
    const s32 titleY1 = y + DEF_TITLE_BAR_H;
    const s32 bodyY0 = titleY1;
    const s32 bodyY1 = y + h - DEF_BOTTOM_BAR_H;
    const s32 bottomY0 = bodyY1;
    const s32 titleInsetX = DEF_TITLE_INSET_X;
    const s32 titleInsetY = DEF_TITLE_INSET_Y;
    const s32 titleInsetH = DEF_TITLE_INSET_H;
    const s32 titleInsetW = w - titleInsetX * 2;
    const f32 framePx = GetMenuBorderPx();

    // Outer red frame
    DrawUniformBorderRectPSX(x, y, w, h, framePx, DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01));

    // Gold bars: dark edge -> bright center -> dark edge (two quads per bar)
    {
        const s32 bx = x + DEF_BORDER_W;
        const s32 bw = w - DEF_BORDER_W * 2;
        const s32 titleInnerH = DEF_TITLE_BAR_H - DEF_BORDER_W;
        const s32 titleHalf = titleInnerH / 2;
        const u8 barAlpha = ScaleAlphaU8(DEF_BAR_ALPHA, alpha01);
        DrawGouraudRectPSX(bx, titleY0 + DEF_BORDER_W, bw, titleHalf,
                           DEF_BAR_EDGE_R, DEF_BAR_EDGE_G, DEF_BAR_EDGE_B,
                           DEF_BAR_MID_R, DEF_BAR_MID_G, DEF_BAR_MID_B,
                           barAlpha);
        DrawGouraudRectPSX(bx, titleY0 + DEF_BORDER_W + titleHalf, bw, titleInnerH - titleHalf,
                           DEF_BAR_MID_R, DEF_BAR_MID_G, DEF_BAR_MID_B,
                           DEF_BAR_EDGE_R, DEF_BAR_EDGE_G, DEF_BAR_EDGE_B,
                           barAlpha);

        const s32 botInnerH = DEF_BOTTOM_BAR_H - DEF_BORDER_W;
        const s32 botHalf = botInnerH / 2;
        DrawGouraudRectPSX(bx, bottomY0, bw, botHalf,
                           DEF_BAR_EDGE_R, DEF_BAR_EDGE_G, DEF_BAR_EDGE_B,
                           DEF_BAR_MID_R, DEF_BAR_MID_G, DEF_BAR_MID_B,
                           barAlpha);
        DrawGouraudRectPSX(bx, bottomY0 + botHalf, bw, botInnerH - botHalf,
                           DEF_BAR_MID_R, DEF_BAR_MID_G, DEF_BAR_MID_B,
                           DEF_BAR_EDGE_R, DEF_BAR_EDGE_G, DEF_BAR_EDGE_B,
                           barAlpha);
    }

    // Body fill
    DrawRect((f32)(x + DEF_BORDER_W), (f32)bodyY0, (f32)(w - DEF_BORDER_W * 2), (f32)(bodyY1 - bodyY0),
             DEF_BODY_R, DEF_BODY_G, DEF_BODY_B, ScaleAlphaU8(DEF_BODY_A, alpha01));

    // Frame
    DrawUniformHLinePSX(x + DEF_BORDER_W, bodyY0, w - DEF_BORDER_W * 2, framePx, DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01));
    DrawUniformHLinePSX(x + DEF_BORDER_W, bodyY1 - DEF_BORDER_W, w - DEF_BORDER_W * 2, framePx, DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01));

    // Black inset title box
    DrawUniformBorderFillRectPSX(x + titleInsetX, y + titleInsetY, titleInsetW, titleInsetH, framePx,
                                 DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01),
                                 DEF_TITLE_INSET_FILL_R, DEF_TITLE_INSET_FILL_G, DEF_TITLE_INSET_FILL_B, ScaleAlphaU8(DEF_TITLE_INSET_FILL_A, alpha01));

    // Decorative bar marks
    tTexture* ornamentTex = m_menuOrnamentTexture;
    const u8 ornAlpha = ScaleAlphaU8(DEF_ORN_A, alpha01);
    DrawMenuOrnament(ornamentTex, x + 18, y + 10, ornAlpha);
    DrawMenuOrnament(ornamentTex, x + w - 32, y + 10, ornAlpha);
    for (s32 i = 0; i < DEF_BOTTOM_ORN_COUNT; i++) {
        const s32 leftX = x + 18 + i * DEF_BOTTOM_ORN_STEP;
        DrawMenuOrnament(ornamentTex, leftX, bottomY0 + DEF_BOTTOM_ORN_Y_OFF, ornAlpha);
    }
    for (s32 i = 0; i < DEF_BOTTOM_ORN_COUNT; i++) {
        const s32 rightX = x + w - (i * DEF_BOTTOM_ORN_STEP) - 32;
        DrawMenuOrnament(ornamentTex, rightX, bottomY0 + DEF_BOTTOM_ORN_Y_OFF, ornAlpha);
    }

    // Title text
    if (title && g_textManager && g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_WINDOW_TITLE_SCALE), SCREEN_SCALE_Y(DEF_MENU_WINDOW_TITLE_SCALE));
        g_textManager->SetAlignment(TextAlign_Center);
        g_textManager->SetWrapWidth(0.0f);
        g_textManager->SetLineSpacing(0);
        g_textManager->SetPromptsEnabled(true);
        g_textManager->SetShadow(false);
        g_textManager->SetOutline(false);
        const s32 titleTextY = y + titleInsetH / 2;
        const f32 titleX = SCALE_AND_CENTER_X((f32)DEF_WINDOW_CENTER_X);
        const f32 titleY = SCREEN_SCALE_Y((f32)titleTextY);
        g_textManager->SetColor(DEF_TITLE_TEXT_R, DEF_TITLE_TEXT_G, DEF_TITLE_TEXT_B, ScaleAlphaU8(255, alpha01));
        g_textManager->PrintString(title, titleX, titleY);
    }
}


void feCustomMenuMgr::DrawPopupWindow(s32 x, s32 y, s32 w, s32 h, const char* title, f32 alpha01) const {
    const s32 titleY0 = y;
    const s32 titleY1 = y + DEF_TITLE_BAR_H;
    const s32 bodyY0 = titleY1;
    const s32 bodyY1 = y + h - DEF_BOTTOM_BAR_H;
    const s32 bottomY0 = bodyY1;
    const s32 titleInsetX = DEF_TITLE_INSET_X;
    const s32 titleInsetY = DEF_TITLE_INSET_Y;
    const s32 titleInsetH = DEF_TITLE_INSET_H;
    const s32 titleInsetW = w - titleInsetX * 2;
    const f32 framePx = GetMenuBorderPx();

    // Gold bars: dark edge -> bright center -> dark edge (two quads per bar)
    {
        const s32 bx = x + DEF_BORDER_W;
        const s32 bw = w - DEF_BORDER_W * 2;
        const s32 titleInnerH = DEF_TITLE_BAR_H - DEF_BORDER_W;
        const s32 titleHalf = titleInnerH / 2;
        DrawGouraudRectPSX(bx, titleY0 + DEF_BORDER_W, bw, titleHalf,
                           DEF_BAR_EDGE_R, DEF_BAR_EDGE_G, DEF_BAR_EDGE_B,
                           DEF_BAR_MID_R, DEF_BAR_MID_G, DEF_BAR_MID_B,
                           ScaleAlphaU8(DEF_BAR_ALPHA, alpha01));
        DrawGouraudRectPSX(bx, titleY0 + DEF_BORDER_W + titleHalf, bw, titleInnerH - titleHalf,
                           DEF_BAR_MID_R, DEF_BAR_MID_G, DEF_BAR_MID_B,
                           DEF_BAR_EDGE_R, DEF_BAR_EDGE_G, DEF_BAR_EDGE_B,
                           ScaleAlphaU8(DEF_BAR_ALPHA, alpha01));
    }

    // Body fill
    DrawRect((f32)(x + DEF_BORDER_W), (f32)bodyY0, (f32)(w - DEF_BORDER_W * 2), (f32)(bodyY1 - bodyY0),
             DEF_BODY_R, DEF_BODY_G, DEF_BODY_B, ScaleAlphaU8(DEF_BODY_A, alpha01));

    // Black inset title box
    DrawUniformBorderFillRectPSX(x + titleInsetX, y + titleInsetY, titleInsetW, titleInsetH, framePx,
                                 DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01),
                                 DEF_TITLE_INSET_FILL_R, DEF_TITLE_INSET_FILL_G, DEF_TITLE_INSET_FILL_B, ScaleAlphaU8(DEF_TITLE_INSET_FILL_A, alpha01));

    // Decorative bar marks
    tTexture* ornamentTex = m_menuOrnamentTexture;
    DrawMenuOrnament(ornamentTex, x + 18, y + 10, ScaleAlphaU8(DEF_ORN_A, alpha01));
    DrawMenuOrnament(ornamentTex, x + w - 32, y + 10, ScaleAlphaU8(DEF_ORN_A, alpha01));

    // Title text
    if (title && g_textManager && g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_WINDOW_TITLE_SCALE), SCREEN_SCALE_Y(DEF_MENU_WINDOW_TITLE_SCALE));
        g_textManager->SetAlignment(TextAlign_Center);
        g_textManager->SetWrapWidth(0.0f);
        g_textManager->SetLineSpacing(0);
        g_textManager->SetPromptsEnabled(true);
        g_textManager->SetShadow(false);
        g_textManager->SetOutline(false);
        const s32 titleTextY = y + titleInsetH / 2;
        const f32 titleX = SCALE_AND_CENTER_X((f32)DEF_WINDOW_CENTER_X);
        const f32 titleY = SCREEN_SCALE_Y((f32)titleTextY);
        g_textManager->SetColor(DEF_TITLE_TEXT_R, DEF_TITLE_TEXT_G, DEF_TITLE_TEXT_B, ScaleAlphaU8(255, alpha01));
        g_textManager->PrintString(title, titleX, titleY);
    }

    // Outer red frame
    DrawUniformBorderRectPSX(x, y, w, h - DEF_BOTTOM_BAR_H, framePx, DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01));
}

void feCustomMenuMgr::RenderKeyBindingsPage(s32 panelX, s32 panelY, s32 panelW, s32 panelH,
                                            const xcColour1555& normalColor,
                                            const xcColour1555& selectedColor) const {
    if (!g_textManager || !g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        return;
    }
    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_REDEFINE_KEY_TEXT_SCALE), SCREEN_SCALE_Y(DEF_REDEFINE_KEY_TEXT_SCALE));
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(true);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);

    const f32 contentTop = (f32)(panelY + DEF_TITLE_BAR_H + DEF_CONTENT_TOP_PAD);
    const f32 labelX = (f32)(panelX + DEF_LABEL_X_PAD + DEF_KEYBIND_X_PAD);
    const f32 headerY = contentTop + DEF_CONTENT_PAD + DEF_TEXT_Y_OFF;
    const f32 firstRowY = headerY + DEF_KEYBIND_ROW_STEP;
    const f32 slotW = (f32)DEF_KEYBIND_SLOT_W;
    const f32 slotGap = (f32)DEF_KEYBIND_SLOT_GAP;
    const f32 slot2Right = (f32)(panelX + panelW - DEF_VALUE_X_PAD - DEF_KEYBIND_X_PAD);
    const f32 slot2Left = slot2Right - slotW;
    const f32 slot1Right = slot2Left - slotGap;
    const f32 slot1Left = slot1Right - slotW;
    const s32 visibleRows = (kKeyBindingActionCount - m_keyBindScrollTop < DEF_KEYBIND_VISIBLE_ROWS)
        ? (kKeyBindingActionCount - m_keyBindScrollTop)
        : DEF_KEYBIND_VISIBLE_ROWS;

    const f32 tableLeft = labelX - DEF_KEYBIND_TABLE_SIDE_PAD;
    const f32 tableRight = slot2Left + slotW + DEF_KEYBIND_TABLE_SIDE_PAD;
    const f32 tableW = tableRight - tableLeft;

    const f32 headerYScreen = SCREEN_SCALE_Y(headerY);
    const char* actionHeader = Localize("FE_KBACT");
    const char* bind1Header = Localize("FE_KBBN1");
    const char* bind2Header = Localize("FE_KBBN2");

    g_textManager->SetAlignment(TextAlign_Left);
    g_textManager->SetColor(normalColor.GetRed8(), normalColor.GetGreen8(), normalColor.GetBlue8());
    g_textManager->PrintString(actionHeader ? actionHeader : "Action", SCALE_AND_CENTER_X(labelX), headerYScreen);
    g_textManager->SetAlignment(TextAlign_Center);
    g_textManager->PrintString(bind1Header ? bind1Header : "Bind 1", SCALE_AND_CENTER_X(slot1Left + slotW / 2), headerYScreen);
    g_textManager->PrintString(bind2Header ? bind2Header : "Bind 2", SCALE_AND_CENTER_X(slot2Left + slotW / 2), headerYScreen);

    const bool backSelected = (m_cursor == 0);

    const f32 keyViewportY = firstRowY - DEF_KEYBIND_ROW_TOP_PAD;
    const f32 keyViewportH = (f32)(DEF_KEYBIND_VISIBLE_ROWS * DEF_KEYBIND_ROW_STEP);
    SetVerticalScissorPSX(keyViewportY + DEF_KEYBIND_ROW_TOP_PAD, keyViewportH);
    for (s32 actionIndex = 0; actionIndex < kKeyBindingActionCount; actionIndex++) {
        const Action action = (Action)actionIndex;
        const bool selectedRow = !backSelected && (actionIndex == m_keyBindActionCursor);
        const f32 rowY = firstRowY + ((f32)actionIndex - m_keyBindScrollVisual) * DEF_KEYBIND_ROW_STEP;
        if (rowY + DEF_KEYBIND_ROW_STEP <= keyViewportY || rowY - DEF_KEYBIND_ROW_TOP_PAD >= keyViewportY + keyViewportH) continue;
        const f32 rowTextY = rowY + 0.8f;

        if ((actionIndex & 1) == 0) {
            DrawRect(tableLeft, (f32)(rowY - DEF_KEYBIND_ROW_TOP_PAD), tableW, (f32)DEF_KEYBIND_ROW_STEP,
                     DEF_KEYBIND_STRIPE_DARK_R, DEF_KEYBIND_STRIPE_DARK_G, DEF_KEYBIND_STRIPE_DARK_B, DEF_KEYBIND_STRIPE_DARK_A);
        }
        else {
            DrawRect(tableLeft, (f32)(rowY - DEF_KEYBIND_ROW_TOP_PAD), tableW, (f32)DEF_KEYBIND_ROW_STEP,
                     DEF_KEYBIND_STRIPE_WARM_R, DEF_KEYBIND_STRIPE_WARM_G, DEF_KEYBIND_STRIPE_WARM_B, DEF_KEYBIND_STRIPE_WARM_A);
        }

        if (selectedRow) {
            const f32 cellLeft = (m_keyBindSlotCursor == 0) ? slot1Left : slot2Left;
            DrawRect((f32)(cellLeft - DEF_KEYBIND_CELL_PAD), (f32)(rowY - DEF_KEYBIND_ROW_TOP_PAD),
                     (f32)(slotW + DEF_KEYBIND_CELL_PAD * 2), (f32)DEF_KEYBIND_ROW_STEP,
                     DEF_KEYBIND_ACTIVE_FILL_R, DEF_KEYBIND_ACTIVE_FILL_G, DEF_KEYBIND_ACTIVE_FILL_B, DEF_KEYBIND_ACTIVE_FILL_A);
            DrawUniformBorderRectPSX((f32)(cellLeft - DEF_KEYBIND_CELL_PAD), (f32)(rowY - DEF_KEYBIND_ROW_TOP_PAD),
                                     (f32)(slotW + DEF_KEYBIND_CELL_PAD * 2), (f32)DEF_KEYBIND_ROW_STEP,
                                     GetMenuBorderPx(), m_pulse.GetRed8(), m_pulse.GetGreen8(), m_pulse.GetBlue8(), 255);
        }

        char actionName[64] = {};
        char slot0Label[32] = {};
        char slot1Label[32] = {};
        const char* actionToken = ActionToToken(action);
        const char* localizedAction = actionToken ? Localize(actionToken) : nullptr;
        if (localizedAction && localizedAction[0] != '\0') {
            snprintf(actionName, (s32)sizeof(actionName), "%s", localizedAction);
        }
        else {
            BuildActionTokenFallbackLabel(actionToken, actionName, (s32)sizeof(actionName));
        }
        BuildDesktopBindingPromptText(action, 0, slot0Label, (s32)sizeof(slot0Label));
        BuildDesktopBindingPromptText(action, 1, slot1Label, (s32)sizeof(slot1Label));

        if (selectedRow && m_keyBindCaptureActive) {
            if (m_keyBindSlotCursor == 0) {
                snprintf(slot0Label, (s32)sizeof(slot0Label), "?");
            }
            else {
                snprintf(slot1Label, (s32)sizeof(slot1Label), "?");
            }
        }

        const bool selectedSlot0 = selectedRow && m_keyBindSlotCursor == 0;
        const bool selectedSlot1 = selectedRow && m_keyBindSlotCursor == 1;
        const f32 rowScreenY = SCREEN_SCALE_Y(rowTextY);
        g_textManager->SetAlignment(TextAlign_Left);
        g_textManager->SetColor(normalColor.GetRed8(), normalColor.GetGreen8(), normalColor.GetBlue8());
        g_textManager->PrintString(actionName, SCALE_AND_CENTER_X(labelX), rowScreenY);
        g_textManager->SetAlignment(TextAlign_Center);
        g_textManager->SetColor(selectedSlot0 ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                selectedSlot0 ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                selectedSlot0 ? selectedColor.GetBlue8() : normalColor.GetBlue8());
        g_textManager->PrintString(slot0Label, SCALE_AND_CENTER_X((slot1Left + slotW / 2)), rowScreenY);
        g_textManager->SetColor(selectedSlot1 ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                selectedSlot1 ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                selectedSlot1 ? selectedColor.GetBlue8() : normalColor.GetBlue8());
        g_textManager->PrintString(slot1Label, SCALE_AND_CENTER_X((slot2Left + slotW / 2)), rowScreenY);
    }
    ScreenDraw::SetScissor(0, 0, (s32)(SCREEN_WIDTH + 0.5f), (s32)(SCREEN_HEIGHT + 0.5f));

    char scrollText[32] = {};
    snprintf(scrollText, (s32)sizeof(scrollText), "%d-%d/%d",
             m_keyBindScrollTop + 1,
             m_keyBindScrollTop + visibleRows,
             kKeyBindingActionCount);
    g_textManager->SetAlignment(TextAlign_Right);
    g_textManager->SetColor(normalColor.GetRed8(), normalColor.GetGreen8(), normalColor.GetBlue8());
    g_textManager->PrintString(scrollText,
                               SCALE_AND_CENTER_X((panelX + panelW - DEF_VALUE_X_PAD)),
                               SCREEN_SCALE_Y((panelY + panelH - DEF_BOTTOM_BAR_H - DEF_CONTENT_BOTTOM_PAD - DEF_ROW_TEXT_H + DEF_TEXT_Y_OFF)));

    DrawScrollBar((f32)(panelX + panelW - 12), firstRowY - DEF_KEYBIND_ROW_TOP_PAD,
                  (f32)(DEF_KEYBIND_VISIBLE_ROWS * DEF_KEYBIND_ROW_STEP),
                  kKeyBindingActionCount, DEF_KEYBIND_VISIBLE_ROWS, m_keyBindScrollVisual);
}

void feCustomMenuMgr::RenderSaveSlotsPage(s32 panelX, s32 panelY, s32 panelW, s32 panelH,
                                          const xcColour1555& selectedColor) const {
    if (!g_textManager || !g_textManager->SetFontByName("Legal")) {
        return;
    }

    const f32 tableLeft = (f32)(panelX + 14);
    const f32 tableRight = (f32)(panelX + panelW - 20);
    const f32 tableW = tableRight - tableLeft;
    const f32 badgeX = tableLeft + 4.0f;
    const f32 badgeW = 38.0f;
    const f32 contentX = badgeX + badgeW + 8.0f;
    const s32 manualTop = panelY + 48;
    const s32 backTop = panelY + 188;
    const s32 backIndex = SAVEGAME_VISIBLE_SLOT_COUNT;
    const bool autosaveDisabled = m_currPage == MenuPage_SaveSlots;

    g_textManager->SetScale(SCREEN_SCALE_Y(0.25f), SCREEN_SCALE_Y(0.25f));
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(true);
    g_textManager->SetShadow(true, 1.0f, 1.0f, 0, 0, 0, 210);
    g_textManager->SetOutline(false);

    auto drawSectionHeader = [&](const char* text, f32 y) {
        DrawRect(tableLeft, y - 2.0f, tableW, 12.0f, 105, 30, 0, 210);
        DrawUniformHLinePSX(tableLeft, y + 9.0f, tableW, GetMenuBorderPx(), 222, 98, 16, 235);
        g_textManager->SetAlignment(TextAlign_Left);
        g_textManager->SetColor(255, 204, 92);
        g_textManager->PrintString(text, SCALE_AND_CENTER_X(tableLeft + 8.0f), SCREEN_SCALE_Y(y));
    };

    drawSectionHeader(Localize("FE_SVMAN"), (f32)(panelY + 33));

    auto drawSlotRow = [&](s32 slotIndex, f32 rowTop, bool alternate, bool disabled) {
        const SaveGameSlotInfo& info = m_saveSlots[slotIndex];
        const bool selected = m_cursor == slotIndex && !disabled;
        g_textManager->SetScale(SCREEN_SCALE_Y(0.24f), SCREEN_SCALE_Y(0.24f));
        if (alternate) {
            DrawRect(tableLeft, rowTop, tableW, (f32)DEF_SAVE_ROW_H - 2.0f, 78, 39, 8, 155);
        }
        else {
            DrawRect(tableLeft, rowTop, tableW, (f32)DEF_SAVE_ROW_H - 2.0f, 7, 7, 10, 210);
        }
        if (disabled) {
            DrawRect(tableLeft, rowTop, tableW, (f32)DEF_SAVE_ROW_H - 2.0f, 10, 10, 12, 175);
        }
        if (selected) {
            DrawRect(tableLeft, rowTop, tableW, (f32)DEF_SAVE_ROW_H - 2.0f, 34, 14, 0, 230);
            DrawUniformBorderRectPSX(tableLeft, rowTop, tableW, (f32)DEF_SAVE_ROW_H - 2.0f,
                                     GetMenuBorderPx(), selectedColor.GetRed8(), selectedColor.GetGreen8(),
                                     selectedColor.GetBlue8(), 255);
        }

        char slotText[16] = {};
        snprintf(slotText, sizeof(slotText), "%s", slotIndex == SAVEGAME_AUTOSAVE_SLOT
                 ? Localize("FE_SVAUTO_SHORT") : "");
        if (slotIndex != SAVEGAME_AUTOSAVE_SLOT) {
            snprintf(slotText, sizeof(slotText), "%02d", slotIndex + 1);
        }

        const u8 muted = disabled ? 92 : 168;
        const u8 primaryR = disabled ? muted : (selected ? selectedColor.GetRed8() : 255);
        const u8 primaryG = disabled ? muted : (selected ? selectedColor.GetGreen8() : 188);
        const u8 primaryB = disabled ? muted : (selected ? selectedColor.GetBlue8() : 64);
        DrawRect(badgeX, rowTop + 4.0f, badgeW, 22.0f,
                 disabled ? 28 : (selected ? selectedColor.GetRed8() : 92),
                 disabled ? 28 : (selected ? selectedColor.GetGreen8() : 28),
                 disabled ? 30 : (selected ? selectedColor.GetBlue8() : 5), 230);
        DrawUniformBorderRectPSX(badgeX, rowTop + 4.0f, badgeW, 22.0f, GetMenuBorderPx(),
                                 disabled ? 65 : 160, disabled ? 65 : 63, disabled ? 65 : 8, 235);

        g_textManager->SetAlignment(TextAlign_Center);
        g_textManager->SetColor(primaryR, primaryG, primaryB);
        g_textManager->PrintString(slotText, SCALE_AND_CENTER_X(badgeX + badgeW * 0.5f),
                                   SCREEN_SCALE_Y(rowTop + 10.0f));

        if (!info.occupied) {
            g_textManager->SetAlignment(TextAlign_Left);
            g_textManager->SetColor(muted, muted, muted);
            g_textManager->PrintString(Localize("FE_SVEMPTY"), SCALE_AND_CENTER_X(contentX),
                                       SCREEN_SCALE_Y(rowTop + 10.0f));
            return;
        }

        char livesText[12] = {};
        char redText[12] = {};
        char goldText[12] = {};
        snprintf(livesText, sizeof(livesText), "%d", info.livesLeft - 1);
        snprintf(redText, sizeof(redText), "%u", (u32)info.redDragons);
        snprintf(goldText, sizeof(goldText), "%u", (u32)info.goldDragons);

        char zoneFallback[32] = {};
        const char* zoneName = nullptr;
        World* world = g_game ? g_game->GetWorld() : nullptr;
        if (world) {
            const s32 levelID = world->GetLevelIDFromIndex(info.nextLevelIndex);
            const char* zoneToken = GetSpecialLocationToken(levelID);
            if (zoneToken) zoneName = Localize(zoneToken);
            if (!zoneName || !zoneName[0]) zoneName = world->GetLevelNameFromIndex(info.nextLevelIndex);
        }
        if (!zoneName || !zoneName[0]) {
            const char* levelFmt = Localize("FE_LVL");
            if (!levelFmt || !levelFmt[0]) levelFmt = "Level %d";
            snprintf(zoneFallback, sizeof(zoneFallback), levelFmt, info.nextLevelIndex + 1);
            zoneName = zoneFallback;
        }

        const u8 iconTint = disabled ? muted : 255;
        auto drawStatIcon = [&](tTexture* texture, f32 iconX) {
            if (!texture) return;
            ScreenDraw::DrawQuad(texture,
                                 SCALE_AND_CENTER_X(iconX), SCREEN_SCALE_Y(rowTop + 12.0f),
                                 SCREEN_SCALE_Y(12.0f), SCREEN_SCALE_Y(12.0f),
                                 0.0f, 0.0f, 1.0f, 1.0f,
                                 iconTint, iconTint, iconTint, disabled ? 150 : 255);
        };
        const f32 livesIconX = contentX;
        const f32 redIconX = contentX + 58.0f;
        const f32 goldIconX = contentX + 116.0f;
        const f32 statCardY = rowTop + 10.0f;
        const f32 statCardW = 48.0f;
        const f32 statCardH = 16.0f;
        auto drawStatCard = [&](f32 iconX) {
            const f32 cardX = iconX - 4.0f;
            DrawRect(cardX, statCardY, statCardW, statCardH, 0, 0, 0, disabled ? 70 : 130);
            DrawUniformBorderRectPSX(cardX, statCardY, statCardW, statCardH, GetMenuBorderPx(),
                                     disabled ? 58 : 116, disabled ? 58 : 48,
                                     disabled ? 58 : 10, disabled ? 150 : 225);
        };
        drawStatCard(livesIconX);
        drawStatCard(redIconX);
        drawStatCard(goldIconX);
        drawStatIcon(m_takeTex, livesIconX);
        drawStatIcon(m_redDragonTex ? m_redDragonTex : m_greyDragonTex, redIconX);
        drawStatIcon(m_goldDragonTex ? m_goldDragonTex : m_greyDragonTex, goldIconX);

        const u8 body = disabled ? muted : 218;
        char destinationLabel[48] = {};
        snprintf(destinationLabel, sizeof(destinationLabel), "%s:", Localize("FE_SVC_ZONE"));
        g_textManager->SetScale(SCREEN_SCALE_Y(0.18f), SCREEN_SCALE_Y(0.18f));
        g_textManager->SetAlignment(TextAlign_Left);
        g_textManager->SetColor(disabled ? muted : 174, disabled ? muted : 124, disabled ? muted : 58);
        g_textManager->PrintString(destinationLabel, SCALE_AND_CENTER_X(contentX - 4.0f), SCREEN_SCALE_Y(rowTop + 3.0f));
        const f32 labelScaleX = SCREEN_SCALE_X(1.0f);
        const f32 labelWidth = labelScaleX > 0.0f
            ? g_textManager->MeasureString(destinationLabel).width / labelScaleX
            : 0.0f;
        g_textManager->SetScale(SCREEN_SCALE_Y(0.23f), SCREEN_SCALE_Y(0.23f));
        g_textManager->SetColor(disabled ? muted : 255, disabled ? muted : 190, disabled ? muted : 72);
        g_textManager->PrintString(zoneName, SCALE_AND_CENTER_X(contentX + labelWidth + 2.0f), SCREEN_SCALE_Y(rowTop + 2.0f));

        g_textManager->SetScale(SCREEN_SCALE_Y(0.21f), SCREEN_SCALE_Y(0.21f));
        g_textManager->SetAlignment(TextAlign_Left);
        g_textManager->SetColor(disabled ? muted : 236, disabled ? muted : 236, disabled ? muted : 210);
        g_textManager->PrintString(livesText, SCALE_AND_CENTER_X(livesIconX + 18.0f), SCREEN_SCALE_Y(rowTop + 15.0f));
        g_textManager->SetColor(disabled ? muted : 242, disabled ? muted : 88, disabled ? muted : 62);
        g_textManager->PrintString(redText, SCALE_AND_CENTER_X(redIconX + 18.0f), SCREEN_SCALE_Y(rowTop + 15.0f));
        g_textManager->SetColor(disabled ? muted : 255, disabled ? muted : 201, disabled ? muted : 40);
        g_textManager->PrintString(goldText, SCALE_AND_CENTER_X(goldIconX + 18.0f), SCREEN_SCALE_Y(rowTop + 15.0f));
        g_textManager->SetAlignment(TextAlign_Right);
        g_textManager->SetColor(body, body, body);
        g_textManager->PrintString(info.dateText[0] ? info.dateText : "--", SCALE_AND_CENTER_X(tableRight - 8.0f),
                                   SCREEN_SCALE_Y(rowTop + 20.0f));
    };

    g_textManager->SetScale(SCREEN_SCALE_Y(0.24f), SCREEN_SCALE_Y(0.24f));
    g_textManager->SetShadow(true, 1.0f, 1.0f, 0, 0, 0, 210);
    const f32 viewportH = (f32)(DEF_SAVE_VISIBLE_ROWS * DEF_SAVE_ROW_H + DEF_SAVE_AUTOSAVE_GAP);
    SetVerticalScissorPSX((f32)manualTop, viewportH);
    for (s32 displayIndex = 0; displayIndex < SAVEGAME_VISIBLE_SLOT_COUNT; ++displayIndex) {
        const s32 slotIndex = SaveDisplayIndexToSlot(displayIndex);
        const bool disabled = autosaveDisabled && slotIndex == SAVEGAME_AUTOSAVE_SLOT;
        const f32 rowTop = (f32)manualTop + GetSaveDisplayRowOffset(displayIndex)
            - GetSaveDisplayScrollOffset(m_saveSlotScrollVisual);
        if (rowTop >= manualTop + viewportH || rowTop + DEF_SAVE_ROW_H <= manualTop) continue;
        if (displayIndex == 1) {
            // Keep the divider near the autosave edge so slot 1 has the larger
            // share of the empty space below it.
            const f32 autosaveBottom = (f32)manualTop + GetSaveDisplayRowOffset(0)
                - GetSaveDisplayScrollOffset(m_saveSlotScrollVisual) + (f32)DEF_SAVE_ROW_H - 2.0f;
            const f32 dividerY = autosaveBottom + 6.0f;
            DrawUniformHLinePSX(tableLeft + 8.0f, dividerY, tableW - 16.0f,
                                GetMenuBorderPx(), 151, 61, 8, 220);
        }
        drawSlotRow(slotIndex, rowTop, (displayIndex & 1) != 0, disabled);
    }
    ScreenDraw::SetScissor(0, 0, (s32)(SCREEN_WIDTH + 0.5f), (s32)(SCREEN_HEIGHT + 0.5f));

    char rangeText[24] = {};
    snprintf(rangeText, sizeof(rangeText), "%d-%d / %d", m_saveSlotScrollTop + 1,
             m_saveSlotScrollTop + DEF_SAVE_VISIBLE_ROWS, SAVEGAME_VISIBLE_SLOT_COUNT);
    g_textManager->SetScale(SCREEN_SCALE_Y(0.22f), SCREEN_SCALE_Y(0.22f));
    g_textManager->SetShadow(false);
    g_textManager->SetAlignment(TextAlign_Right);
    g_textManager->SetColor(185, 124, 50);
    g_textManager->PrintString(rangeText, SCALE_AND_CENTER_X(tableRight - 5.0f), SCREEN_SCALE_Y((f32)(panelY + 34)));

    DrawScrollBar((f32)(panelX + panelW - 12), (f32)manualTop,
                  (f32)(DEF_SAVE_VISIBLE_ROWS * DEF_SAVE_ROW_H + DEF_SAVE_AUTOSAVE_GAP),
                  SAVEGAME_VISIBLE_SLOT_COUNT, DEF_SAVE_VISIBLE_ROWS, m_saveSlotScrollVisual);

    if (g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        g_textManager->SetScale(SCREEN_SCALE_Y(0.36f), SCREEN_SCALE_Y(0.36f));
        g_textManager->SetShadow(false);
        g_textManager->SetOutline(true);
        g_textManager->SetAlignment(TextAlign_Center);
        const bool backSelected = m_cursor == backIndex;
        g_textManager->SetColor(backSelected ? selectedColor.GetRed8() : 190,
                                backSelected ? selectedColor.GetGreen8() : 125,
                                backSelected ? selectedColor.GetBlue8() : 40);
        const char* back = Localize("FE_BCK");
        g_textManager->PrintString(back ? back : "Back", SCALE_AND_CENTER_X((f32)(panelX + panelW / 2)),
                                   SCREEN_SCALE_Y((f32)backTop));
    }
}

#ifdef MOD_LOADER
void feCustomMenuMgr::ClampModsScroll() {
    const s32 count = static_cast<s32>(ModLoader::Instance().GetMods().size());
    if (count <= 0) {
        m_modCursor = 0;
        m_modScrollTop = 0;
        return;
    }
    if (m_modCursor < 0) m_modCursor = 0;
    if (m_modCursor >= count) m_modCursor = count - 1;
    if (m_modCursor < m_modScrollTop) m_modScrollTop = m_modCursor;
    if (m_modCursor >= m_modScrollTop + DEF_MODS_VISIBLE_ROWS) {
        m_modScrollTop = m_modCursor - DEF_MODS_VISIBLE_ROWS + 1;
    }
    const s32 maxTop = (count > DEF_MODS_VISIBLE_ROWS) ? count - DEF_MODS_VISIBLE_ROWS : 0;
    if (m_modScrollTop < 0) m_modScrollTop = 0;
    if (m_modScrollTop > maxTop) m_modScrollTop = maxTop;
}

bool feCustomMenuMgr::ToggleSelectedMod() {
    const auto& mods = ModLoader::Instance().GetMods();
    if (m_modCursor < 0 || m_modCursor >= static_cast<s32>(mods.size())) return false;
    const std::string folder = mods[m_modCursor].folder;
    const bool enable = !mods[m_modCursor].enabled;
    if (!ModLoader::Instance().SetModEnabled(folder, enable)) return false;
    ClampModsScroll();
    return true;
}

void feCustomMenuMgr::RenderModsPage(s32 panelX, s32 panelY, s32 panelW, s32 panelH,
                                     const xcColour1555& normalColor,
                                     const xcColour1555& selectedColor) const {
    if (!g_textManager) return;
    const auto& mods = ModLoader::Instance().GetMods();
    const s32 count = static_cast<s32>(mods.size());
    const s32 visibleRows = std::min(DEF_MODS_VISIBLE_ROWS, std::max(0, count - m_modScrollTop));
    const f32 labelX = static_cast<f32>(panelX + DEF_LABEL_X_PAD + DEF_KEYBIND_X_PAD);
    const f32 valueX = static_cast<f32>(panelX + panelW - DEF_VALUE_X_PAD - DEF_KEYBIND_X_PAD);
    const f32 firstRowY = static_cast<f32>(panelY + DEF_TITLE_BAR_H + DEF_CONTENT_PAD + 8);
    const f32 tableLeft = labelX - DEF_KEYBIND_TABLE_SIDE_PAD;
    const f32 tableW = valueX - tableLeft + DEF_KEYBIND_TABLE_SIDE_PAD;

    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_REDEFINE_KEY_TEXT_SCALE), SCREEN_SCALE_Y(DEF_REDEFINE_KEY_TEXT_SCALE));
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetOutline(true);

    if (count == 0) {
        g_textManager->SetAlignment(TextAlign_Center);
        g_textManager->SetColor(normalColor.GetRed8(), normalColor.GetGreen8(), normalColor.GetBlue8());
        const char* emptyText = Localize("FE_MOD_NONE");
        g_textManager->PrintString(emptyText ? emptyText : "No mods installed",
                                   SCALE_AND_CENTER_X(static_cast<f32>(panelX + panelW / 2)),
                                   SCREEN_SCALE_Y(firstRowY + 32.0f));
        return;
    }

    const f32 modViewportY = firstRowY - DEF_KEYBIND_ROW_TOP_PAD;
    const f32 modViewportH = (f32)(DEF_MODS_VISIBLE_ROWS * DEF_MODS_ROW_STEP);
    SetVerticalScissorPSX(modViewportY, modViewportH);
    for (s32 index = 0; index < count; ++index) {
        const ModInfo& mod = mods[index];
        const bool selected = m_cursor != 0 && index == m_modCursor;
        const f32 rowY = firstRowY + ((f32)index - m_modScrollVisual) * DEF_MODS_ROW_STEP;
        if (rowY + DEF_MODS_ROW_STEP <= modViewportY || rowY - DEF_KEYBIND_ROW_TOP_PAD >= modViewportY + modViewportH) continue;
        if ((index & 1) == 0) {
            DrawRect(tableLeft, rowY - DEF_KEYBIND_ROW_TOP_PAD, tableW, DEF_MODS_ROW_STEP,
                     DEF_KEYBIND_STRIPE_DARK_R, DEF_KEYBIND_STRIPE_DARK_G,
                     DEF_KEYBIND_STRIPE_DARK_B, DEF_KEYBIND_STRIPE_DARK_A);
        }
        else {
            DrawRect(tableLeft, rowY - DEF_KEYBIND_ROW_TOP_PAD, tableW, DEF_MODS_ROW_STEP,
                     DEF_KEYBIND_STRIPE_WARM_R, DEF_KEYBIND_STRIPE_WARM_G,
                     DEF_KEYBIND_STRIPE_WARM_B, DEF_KEYBIND_STRIPE_WARM_A);
        }
        if (selected) {
            DrawHighlight(tableLeft, rowY - DEF_KEYBIND_ROW_TOP_PAD, tableW, DEF_MODS_ROW_STEP);
        }

        const u8 r = selected ? selectedColor.GetRed8() : normalColor.GetRed8();
        const u8 g = selected ? selectedColor.GetGreen8() : normalColor.GetGreen8();
        const u8 b = selected ? selectedColor.GetBlue8() : normalColor.GetBlue8();
        g_textManager->SetColor(r, g, b);
        g_textManager->SetAlignment(TextAlign_Left);
        g_textManager->PrintString(mod.name.c_str(), SCALE_AND_CENTER_X(labelX), SCREEN_SCALE_Y(rowY));
        g_textManager->SetAlignment(TextAlign_Right);
        const char* stateText = Localize(mod.enabled ? "FE_ON" : "FE_OFF");
        g_textManager->PrintString(stateText ? stateText : (mod.enabled ? "ON" : "OFF"),
                                   SCALE_AND_CENTER_X(valueX), SCREEN_SCALE_Y(rowY));
    }
    ScreenDraw::SetScissor(0, 0, (s32)(SCREEN_WIDTH + 0.5f), (s32)(SCREEN_HEIGHT + 0.5f));

    char range[32] = {};
    snprintf(range, sizeof(range), "%d-%d/%d", m_modScrollTop + 1, m_modScrollTop + visibleRows, count);
    g_textManager->SetAlignment(TextAlign_Right);
    g_textManager->SetColor(normalColor.GetRed8(), normalColor.GetGreen8(), normalColor.GetBlue8());
    g_textManager->PrintString(range, SCALE_AND_CENTER_X(valueX),
                               SCREEN_SCALE_Y(static_cast<f32>(panelY + panelH - DEF_BOTTOM_BAR_H - DEF_CONTENT_BOTTOM_PAD - DEF_ROW_TEXT_H)));

    DrawScrollBar((f32)(panelX + panelW - 12), firstRowY - DEF_KEYBIND_ROW_TOP_PAD,
                  (f32)(DEF_MODS_VISIBLE_ROWS * DEF_MODS_ROW_STEP),
                  count, DEF_MODS_VISIBLE_ROWS, m_modScrollVisual);
}
#endif

void feCustomMenuMgr::Render() {
    if (!m_active)
        return;

    // One overlay batch for the whole menu page + any popup overlay: collapses
    // the per-quad projection rebuilds / GL state toggles across every border
    // rect and text string into a single setup, which is what restores high
    // frame rates on text-heavy menu pages.
    ScreenDraw::Batch uiBatch;

    m_pulse.Update();

    if (m_currPage != MenuPage_None) {
        RenderCurrentPage();
    }

    if (m_activePopup != PopupKind_None) {
        RenderActivePopup();
    }
}

void feCustomMenuMgr::RenderCurrentPage() {
    const PageDef* page = &m_pages[m_currPage];

    const s32 panelX = DEF_WINDOW_CENTER_X - page->frameW / 2;
    s32 panelY = DEF_WINDOW_CENTER_Y - page->frameH / 2;
    const s32 panelW = page->frameW;
    const s32 panelH = page->frameH;

    const char* title = Localize(page->titleToken);
    if (!title)
        title = page->titleToken;
#ifdef MOD_LOADER
    if (m_currPage == MenuPage_Mods && (!title || strcmp(title, "FE_MODS") == 0)) {
        title = "Mods";
    }
#endif
    char locationTitle[64] = {};

    // For the location page the title bar must show the selected destination name.
    if (m_currPage == MenuPage_Location) {
        LocationRuntimeInfo info = {};
        if (ResolveLocationRuntimeInfo(&info)) {
            if (info.levelID >= 1 && info.levelID <= 5) {
                const char* levelFmt = Localize("FE_LVL");
                if (!levelFmt || levelFmt[0] == '\0') {
                    levelFmt = "Level %d";
                }

                snprintf(locationTitle, sizeof(locationTitle), levelFmt, info.subLevel + 1);
                title = locationTitle;
            }
            else {
                bool hasSpecialTitle = false;
                const char* specialToken = GetSpecialLocationToken(info.levelID);
                if (specialToken) {
                    const char* localizedSpecial = Localize(specialToken);
                    if (localizedSpecial && localizedSpecial[0] != '\0') {
                        title = localizedSpecial;
                        hasSpecialTitle = true;
                    }
                }

                if (!hasSpecialTitle) {
                    const char* specialTitle = ResolveLocationSpecialTitle(info.levelIndex);
                    if (specialTitle && specialTitle[0] != '\0') {
                        title = specialTitle;
                        hasSpecialTitle = true;
                    }
                }

                if (!hasSpecialTitle && info.levelName && info.levelName[0] != '\0') {
                    title = info.levelName;
                    hasSpecialTitle = true;
                }

                if (!hasSpecialTitle) {
                    const char* levelFmt = Localize("FE_LVL");
                    if (!levelFmt || levelFmt[0] == '\0') {
                        levelFmt = "Level %d";
                    }

                    snprintf(locationTitle, sizeof(locationTitle), levelFmt, info.subLevel + 1);
                    title = locationTitle;
                }
            }
        }
    }

    DrawMenuWindow(panelX, panelY, panelW, panelH, title, 1.0f);

    // Build normalColor directly (PSX scale: 128 = neutral/1.0 for the tint shader)
    const xcColour1555 normalColor{ DEF_TEXT_NORM_R, DEF_TEXT_NORM_G, DEF_TEXT_NORM_B };
    const xcColour1555 selectedColor = m_pulse.GetColor();
    if (!g_textManager || !g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        return;
    }
    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE), SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE));
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(true);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);

    // Dragon panel: present on pause page only
    static constexpr s32 DRAGON_PANEL_W = 88;
    const bool hasDragonPanel = (m_currPage == MenuPage_Pause);
    const s32 dragonBoxW = hasDragonPanel ? DRAGON_PANEL_W : 0;
    const s32 dragonBoxX = panelX + panelW - dragonBoxW;

    const s32 contentTop = panelY + DEF_TITLE_BAR_H + DEF_CONTENT_TOP_PAD;
    const s32 labelX = panelX + DEF_LABEL_X_PAD;
    // When the dragon panel is present, clamp valueX so it doesn't overlap.
    const s32 valueX = hasDragonPanel ? (dragonBoxX - DEF_VALUE_X_PAD) : (panelX + panelW - DEF_VALUE_X_PAD);
    const s32 rowSpan = (page->numEntries > 0) ? ((page->numEntries - 1) * DEF_ROW_STEP) : 0;
    const s32 extraH = CalcPageExtraHeight(*page);
    const s32 entryBlockH = DEF_CONTENT_PAD + rowSpan + DEF_ROW_TEXT_H + extraH;
    const s32 bodyAvailH = panelH - DEF_TITLE_BAR_H - DEF_BOTTOM_BAR_H - DEF_CONTENT_TOP_PAD - DEF_CONTENT_BOTTOM_PAD;
    const s32 bodyCenterPad = (bodyAvailH > entryBlockH) ? ((bodyAvailH - entryBlockH) / 2) : 0;
    const s32 firstY = contentTop + bodyCenterPad + DEF_CONTENT_PAD;
    // Shift entry center left to the midpoint of the usable content area.
    const s32 contentCenterX = hasDragonPanel
        ? (panelX + DEF_BORDER_W + (dragonBoxX - DEF_BORDER_W - panelX) / 2)
        : DEF_WINDOW_CENTER_X;

    switch (m_currPage) {
        case MenuPage_KeyBindings:
            RenderKeyBindingsPage(panelX, panelY, panelW, panelH, normalColor, selectedColor);
            break;
        case MenuPage_LoadSlots:
        case MenuPage_SaveSlots:
        case MenuPage_DeleteSlots:
            RenderSaveSlotsPage(panelX, panelY, panelW, panelH, selectedColor);
            break;
#ifdef MOD_LOADER
        case MenuPage_Mods:
            RenderModsPage(panelX, panelY, panelW, panelH, normalColor, selectedColor);
            break;
#endif
        case MenuPage_Controller:
            RenderControllerOverlay(panelX, panelY);
            break;
        case MenuPage_Location:
            RenderLocationPage();
            break;
#if AUTO_UPDATER
        case MenuPage_Update:
            if (g_autoUpdater && g_autoUpdater->GetState() == AutoUpdater::State::Downloading) {
                RenderUpdateProgressBar(panelX, panelW, firstY);
            }
            break;
        case MenuPage_Changelog:
            RenderChangelogBody(panelX, panelY, panelW, panelH, contentTop);
            break;
#endif
        default:
            break;
    }

    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE), SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE));

    if (m_currPage != MenuPage_Location && !IsSaveSlotPage(m_currPage)
#if AUTO_UPDATER
        && m_currPage != MenuPage_Changelog
#endif
        ) {
        for (s32 i = 0; i < page->numEntries; i++) {
            const Entry& item = page->entries[i];
            const bool selected = (i == m_cursor);

            s32 rowY = 0;
            s32 rowLabelX = labelX;
            s32 rowValueX = valueX;
            s32 rowCenterX = contentCenterX;
            ResolveEntryLayout(*page, i,
                               firstY, labelX, valueX, contentCenterX,
                               nullptr, &rowY, &rowLabelX, &rowValueX, &rowCenterX);

            const char* label = Localize(item.token);
            if (!label) label = item.token;
#ifdef MOD_LOADER
            if (strcmp(item.token, "FE_MODS") == 0 && strcmp(label, "FE_MODS") == 0) {
                label = "Mods";
            }
#endif

#if AUTO_UPDATER
            char updateInfoText[256];
            if (BuildUpdateInfoText(item.token, updateInfoText, (s32)sizeof(updateInfoText))) {
                label = updateInfoText;
            }
#endif

            char assetInfoText[256];
            if (BuildAssetInfoText(item.token, assetInfoText, (s32)sizeof(assetInfoText))) {
                label = assetInfoText;
            }

            const f32 rowScreenY = SCREEN_SCALE_Y((f32)rowY);
            const f32 labelScreenX = SCALE_AND_CENTER_X((f32)rowLabelX);
            const f32 valueScreenX = SCALE_AND_CENTER_X((f32)rowValueX);
            const f32 centerScreenX = SCALE_AND_CENTER_X((f32)rowCenterX);

            if (item.type == EntryType_Info) {
                const f32 wrapWidth = SCREEN_SCALE_X((f32)(page->frameW - DEF_LABEL_X_PAD * 2));
                g_textManager->SetAlignment(TextAlign_Center);
                g_textManager->SetWrapWidth(wrapWidth);
                g_textManager->SetColor(DEF_INFO_TEXT_R, DEF_INFO_TEXT_G, DEF_INFO_TEXT_B);
                g_textManager->PrintString(label, centerScreenX, rowScreenY);
                g_textManager->SetWrapWidth(0.0f);
            }
            else if (item.type == EntryType_List && item.binding != EntryBinding_None) {
                g_textManager->SetAlignment(TextAlign_Left);
                g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                        selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                        selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                g_textManager->PrintString(label, labelScreenX, rowScreenY);

                if (item.binding == EntryBinding_DisplayResolution) {
                    s32 idx = GetBoundValue(item);
                    if (selected && m_pendingResolutionActive) {
                        idx = m_pendingResolutionIndex;
                    }

                    const char* resText = nullptr;

                    char resTextBuf[32];
                    if (g_display) {
                        pddiVideoMode mode;
                        if (g_display->GetResolutionMode(idx, mode)) {
                            std::snprintf(resTextBuf, sizeof(resTextBuf), "%dx%d", mode.width, mode.height);
                            resText = resTextBuf;
                        }
                    }

                    if (!resText) {
                        resText = Localize("FE_AUT");
                    }

                    if (!resText) {
                        continue;
                    }

                    g_textManager->SetAlignment(TextAlign_Right);
                    g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                            selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                            selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                    g_textManager->PrintString(resText, valueScreenX, rowScreenY);
                }
                else if (item.binding == EntryBinding_DisplayScreenMode) {
                    s32 mode = GetBoundValue(item);
                    if (selected && m_pendingScreenModeActive) {
                        mode = m_pendingScreenMode;
                    }
                    const char* modeText = Localize("FE_SCF");
                    if (mode == ScreenMode_Borderless) {
                        modeText = Localize("FE_SCB");
                    }
                    else if (mode == ScreenMode_Windowed) {
                        modeText = Localize("FE_SCW");
                    }

                    g_textManager->SetAlignment(TextAlign_Right);
                    g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                            selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                            selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                    g_textManager->PrintString(modeText, valueScreenX, rowScreenY);
                }
                else if (item.binding == EntryBinding_DisplayMsaa) {
                    s32 msaaIndex = GetBoundValue(item);
                    if (selected && m_pendingMsaaActive) {
                        msaaIndex = m_pendingMsaaIndex;
                    }
                    const s32 msaaSamples = MsaaOptionIndexToSamples(msaaIndex);
                    const char* msaaToken = (msaaSamples == 0)
                        ? "FE_OFF"
                        : GetMsaaDisplayToken(msaaIndex);
                    const char* msaaText = msaaToken ? Localize(msaaToken) : nullptr;

                    if (!msaaText) {
                        continue;
                    }

                    g_textManager->SetAlignment(TextAlign_Right);
                    g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                            selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                            selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                    g_textManager->PrintString(msaaText, valueScreenX, rowScreenY);
                }
#if MODERN_GRAPHICS
                else if (item.binding == EntryBinding_DisplayShadowQuality) {
                    const s32 quality = GetBoundValue(item);
                    const char* qualityToken = "FE_OFF";
                    if (quality == (s32)SHADOW_QUALITY_LOW) qualityToken = "FE_LOW";
                    else if (quality == (s32)SHADOW_QUALITY_MEDIUM) qualityToken = "FE_MED";
                    else if (quality == (s32)SHADOW_QUALITY_HIGH) qualityToken = "FE_HIG";
                    else if (quality == (s32)SHADOW_QUALITY_VERY_HIGH) qualityToken = "FE_VHI";
                    const char* qualityText = Localize(qualityToken);

                    if (!qualityText) {
                        continue;
                    }

                    g_textManager->SetAlignment(TextAlign_Right);
                    g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                            selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                            selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                    g_textManager->PrintString(qualityText, valueScreenX, rowScreenY);
                }
#endif
                else if (item.binding == EntryBinding_DisplayFrameRate) {
                    const char* frameRateToken = GetFrameRateDisplayToken(GetBoundValue(item));
                    const char* frameRateText = frameRateToken ? Localize(frameRateToken) : nullptr;

                    if (!frameRateText) {
                        continue;
                    }

                    g_textManager->SetAlignment(TextAlign_Right);
                    g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                            selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                            selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                    g_textManager->PrintString(frameRateText, valueScreenX, rowScreenY);
                }
                else if (item.binding == EntryBinding_ControllerPromptStyle) {
                    const s32 style = ClampControllerPromptStyle(GetBoundValue(item));
                    const char* styleText = Localize(kControllerPromptStyles[style].displayToken);

                    if (!styleText) {
                        continue;
                    }

                    g_textManager->SetAlignment(TextAlign_Right);
                    g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                            selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                            selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                    g_textManager->PrintString(styleText, valueScreenX, rowScreenY);
                }
                else if (item.binding == EntryBinding_PlayerConfig) {
                    const s32 cfg = GetBoundValue(item);
                    const char* cfgToken = (cfg == 0) ? "FE_CF1"
                        : (cfg == 1) ? "FE_CF2"
                        : "FE_CF3";
                    const char* cfgText = Localize(cfgToken);

                    if (!cfgText) {
                        continue;
                    }

                    g_textManager->SetAlignment(TextAlign_Right);
                    g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                            selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                            selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                    g_textManager->PrintString(cfgText, valueScreenX, rowScreenY);
                }
                else if (item.binding == EntryBinding_Language) {
                    const char* langToken = GetLanguageDisplayToken(GetBoundValue(item));
                    const char* langText = langToken ? Localize(langToken) : nullptr;

                    if (!langText) {
                        continue;
                    }

                    g_textManager->SetAlignment(TextAlign_Right);
                    g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                            selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                            selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                    g_textManager->PrintString(langText, valueScreenX, rowScreenY);
                }
            }
            else if (item.type == EntryType_Slider && item.binding != EntryBinding_None) {
                g_textManager->SetAlignment(TextAlign_Left);
                g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                        selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                        selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                g_textManager->PrintString(label, labelScreenX, rowScreenY);

                DrawSliderCircleMeterPSX(
                    rowValueX,
                    rowY,
                    GetBoundValue(item),
                    m_sliderOTex,
                    m_sliderFTex);
            }
            else if (item.type == EntryType_Toggle && item.binding != EntryBinding_None) {
                const s32 toggle = GetBoundValue(item);
                const char* toggleToken = toggle ? "FE_ON" : "FE_OFF";
                const char* toggleText = Localize(toggleToken);

                if (!toggleText) {
                    continue;
                }

                g_textManager->SetAlignment(TextAlign_Left);
                g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                        selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                        selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                g_textManager->PrintString(label, labelScreenX, rowScreenY);

                g_textManager->SetAlignment(TextAlign_Right);
                g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                        selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                        selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                g_textManager->PrintString(toggleText, valueScreenX, rowScreenY);
            }
            else {
                g_textManager->SetAlignment(TextAlign_Center);
                g_textManager->SetColor(selected ? selectedColor.GetRed8() : normalColor.GetRed8(),
                                        selected ? selectedColor.GetGreen8() : normalColor.GetGreen8(),
                                        selected ? selectedColor.GetBlue8() : normalColor.GetBlue8());
                g_textManager->PrintString(label, centerScreenX, rowScreenY);
            }
        }
    }

    // Gold dragon count panel (pause menu only)
    if (hasDragonPanel) {
        const s32 dragonBodyY = panelY + DEF_TITLE_BAR_H;
        const s32 dragonBodyH = panelH - DEF_TITLE_BAR_H - DEF_BOTTOM_BAR_H;
        const s32 dragonBackHalf = dragonBodyH / 2;
        const s32 dragonInset = 6;
        const s32 dragonInnerX = dragonBoxX + dragonInset;
        const s32 dragonInnerY = dragonBodyY + dragonInset;
        const s32 dragonInnerW = dragonBoxW - dragonInset * 2;
        const s32 dragonInnerH = dragonBodyH - dragonInset * 2;
        const f32 dragonFramePx = GetMenuBorderPx();

        // Back layer: full gold box.
        DrawGouraudRectPSX(dragonBoxX, dragonBodyY, dragonBoxW, dragonBackHalf,
                           DEF_BAR_EDGE_R, DEF_BAR_EDGE_G, DEF_BAR_EDGE_B,
                           DEF_BAR_MID_R, DEF_BAR_MID_G, DEF_BAR_MID_B,
                           DEF_BAR_ALPHA);
        DrawGouraudRectPSX(dragonBoxX, dragonBodyY + dragonBackHalf, dragonBoxW, dragonBodyH - dragonBackHalf,
                           DEF_BAR_MID_R, DEF_BAR_MID_G, DEF_BAR_MID_B,
                           DEF_BAR_EDGE_R, DEF_BAR_EDGE_G, DEF_BAR_EDGE_B,
                           DEF_BAR_ALPHA);
        DrawUniformHLinePSX(dragonBoxX, dragonBodyY + dragonBodyH - DEF_BORDER_W, dragonBoxW, dragonFramePx,
                            DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, DEF_FRAME_A);
        DrawUniformBorderRectPSX(dragonBoxX, dragonBodyY, dragonBoxW, dragonBodyH, dragonFramePx,
                                 DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, DEF_FRAME_A);

        // Front layer: smaller pure-black panel with red outline.
        DrawUniformBorderFillRectPSX(dragonInnerX, dragonInnerY, dragonInnerW, dragonInnerH, dragonFramePx,
                                     DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, DEF_FRAME_A,
                                     0, 0, 0, 255);

        const s32 dragonCenterX = dragonInnerX + dragonInnerW / 2;
        const s32 dragonMidY = dragonInnerY + dragonInnerH / 2;
        const s32 dragonIconY = dragonMidY - 22;
        const s32 dragonCountY = dragonIconY + 32;
        ScreenDraw::DrawQuad(m_goldDragonTex,
                             SCALE_AND_CENTER_X((f32)dragonCenterX - 24.0f), SCREEN_SCALE_Y(dragonIconY + DEF_TEXT_Y_OFF),
                             SCREEN_SCALE_Y(32), SCREEN_SCALE_Y(32),
                             0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255);

        s32 totalGold = g_scoreManager ? g_scoreManager->GetTotalGoldDragon() : 0;
        if (totalGold > 99) totalGold = 99;
        char dragonCountStr[8];
        std::snprintf(dragonCountStr, sizeof(dragonCountStr), "%d", totalGold);

        g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_DRAGON_COUNT_SCALE), SCREEN_SCALE_Y(DEF_MENU_DRAGON_COUNT_SCALE));
        g_textManager->SetAlignment(TextAlign_Center);
        g_textManager->SetWrapWidth(0.0f);
        g_textManager->SetLineSpacing(0);
        g_textManager->SetPromptsEnabled(true);
        g_textManager->SetShadow(false);
        g_textManager->SetOutline(true);
        g_textManager->SetColor(200, 200, 200);
        g_textManager->PrintString(dragonCountStr, SCALE_AND_CENTER_X((f32)dragonCenterX), SCREEN_SCALE_Y((f32)(dragonCountY + DEF_TEXT_Y_OFF)));
    }

    // Help prompts in the bottom bar (hidden while a popup overlay covers the page).
    if (g_textManager && g_textManager->SetFontByName(DEF_MENU_FONT_NAME)
        && m_currPage != MenuPage_Quitting && m_activePopup == PopupKind_None) {
        f32 helpScale = DEF_MENU_PROMPT_SCALE;
        f32 promptGap = DEF_HELP_GROUP_GAP_PX;

        g_textManager->SetScale(SCREEN_SCALE_Y(helpScale), SCREEN_SCALE_Y(helpScale));
        g_textManager->SetAlignment(TextAlign_Left);
        g_textManager->SetWrapWidth(0.0f);
        g_textManager->SetLineSpacing(0);
        g_textManager->SetPromptsEnabled(true);
        g_textManager->SetShadow(false);
        g_textManager->SetOutline(false);
        const Entry* selectedEntry = nullptr;
        if (m_currPage >= 0 && m_currPage < MenuPage_Count) {
            const PageDef& currentPage = m_pages[m_currPage];
            if (m_cursor >= 0 && m_cursor < currentPage.numEntries) {
                selectedEntry = &currentPage.entries[m_cursor];
            }
        }

        char prompts[6][96] = {};
        s32 promptCount = 0;
        auto pushPrompt = [&](const char* token, const char* fallback) {
            if (promptCount >= (s32)(sizeof(prompts) / sizeof(prompts[0]))) {
                return;
            }

            const char* localized = Localize(token);
            SetPromptText(prompts[promptCount], (s32)sizeof(prompts[promptCount]), "%s", localized ? localized : fallback);
            promptCount++;
        };

        if (m_currPage == MenuPage_KeyBindings) {
            if (m_keyBindCaptureActive) {
                pushPrompt("FE_KBPR", "Press key or mouse");
                pushPrompt("FE_KBCLR", "<ACT:MENU_CLEAR> Unbind");
                pushPrompt("FE_KBCAN", "<ACT:MENU_BACK> Cancel");
            }
            else if (m_cursor == 0) {
                pushPrompt("FE_HPSEL", "<ACT:MENU_CONFIRM> Select");
                pushPrompt("FE_KBBCK", "<ACT:MENU_BACK> Back");
            }
            else {
                pushPrompt("FE_KBSLT", "<ACT:MENU_LEFT>/<ACT:MENU_RIGHT> Slot");
                pushPrompt("FE_KBBND", "<ACT:MENU_CONFIRM> Bind");
                pushPrompt("FE_KBCLR", "<ACT:MENU_CLEAR> Unbind");
                pushPrompt("FE_KBBCK", "<ACT:MENU_BACK> Back");
            }
        }
#ifdef MOD_LOADER
        else if (m_currPage == MenuPage_Mods) {
            if (static_cast<s32>(ModLoader::Instance().GetMods().size()) > DEF_MODS_VISIBLE_ROWS) {
                pushPrompt("FE_HPSCR", "<ACT:MENU_UP>/<ACT:MENU_DOWN> Scroll");
            }
            if (m_cursor != 0) {
                pushPrompt("FE_HPTGL", "<ACT:MENU_CONFIRM> Toggle");
            }
            pushPrompt("FE_HPBCK", "<ACT:MENU_BACK> Back");
        }
#endif
#if AUTO_UPDATER
        else if (m_currPage == MenuPage_Changelog) {
            if ((s32)m_changelogLines.size() > ComputeChangelogVisibleLines(m_pages[MenuPage_Changelog].frameH)) {
                pushPrompt("FE_HPSCR", "<ACT:MENU_UP>/<ACT:MENU_DOWN> Scroll");
            }
            pushPrompt("FE_HPBCK", "<ACT:MENU_BACK> Back");
        }
#endif
        else if (m_currPage == MenuPage_AssetMissing) {
            // Back is a no-op while gating boot on missing assets (use the on-screen
            // buttons instead), so don't advertise it here.
            if (selectedEntry) {
                pushPrompt("FE_HPSEL", "<ACT:MENU_CONFIRM> Select");
            }
        }
        else {
            if (selectedEntry) {
                if (selectedEntry->type == EntryType_Slider) {
                    pushPrompt("FE_HPADJ", "<ACT:MENU_LEFT> / <ACT:MENU_RIGHT> Adjust");
                }
                else if (selectedEntry->type == EntryType_List) {
                    pushPrompt("FE_HPADJ", "<ACT:MENU_LEFT> / <ACT:MENU_RIGHT> Adjust");
                    pushPrompt("FE_HPSET", "<ACT:MENU_CONFIRM> Set");
                }
                else if (selectedEntry->type == EntryType_Toggle) {
                    pushPrompt("FE_HPADJ", "<ACT:MENU_LEFT> / <ACT:MENU_RIGHT> Adjust");
                    pushPrompt("FE_HPTGL", "<ACT:MENU_CONFIRM> Toggle");
                }
                else {
                    pushPrompt("FE_HPSEL", "<ACT:MENU_CONFIRM> Select");
                }
            }

            pushPrompt("FE_HPBCK", "<ACT:MENU_BACK> Back");
        }

        const s32 bottomBarY = panelY + panelH - DEF_BOTTOM_BAR_H;
        const f32 helpY = SCREEN_SCALE_Y((f32)(bottomBarY + DEF_HELP_Y_PAD));
        const f32 centerScreenX = SCALE_AND_CENTER_X((f32)DEF_WINDOW_CENTER_X);

        f32 totalWidth = 0.0f;
        for (s32 i = 0; i < promptCount; i++) {
            totalWidth += g_textManager->MeasureString(prompts[i]).width;
            if (i + 1 < promptCount) {
                totalWidth += promptGap;
            }
        }

        f32 cursorX = centerScreenX - totalWidth * 0.5f;
        g_textManager->SetColor(DEF_HELP_TEXT_R, DEF_HELP_TEXT_G, DEF_HELP_TEXT_B);
        for (s32 i = 0; i < promptCount; i++) {
            g_textManager->PrintString(prompts[i], cursorX, helpY);
            cursorX += g_textManager->MeasureString(prompts[i]).width;

            if (i + 1 < promptCount) {
                cursorX += promptGap;
            }
        }
    }

#if AUTO_UPDATER
    if (m_currPage == MenuPage_Title) {
        DrawVersionOverlay();
    }
#endif
}

void feCustomMenuMgr::RenderActivePopup() {
    const PageDef& popup = m_popups[m_activePopup];

    const s32 panelX = DEF_WINDOW_CENTER_X - popup.frameW / 2;
    const s32 panelY = DEF_WINDOW_CENTER_Y - popup.frameH / 2 + 10;
    const s32 panelW = popup.frameW;
    const s32 panelH = popup.frameH;
    const f32 popupAlpha = GetPopupFadeAlpha();

    const char* title = Localize(popup.titleToken);
    if (!title)
        title = popup.titleToken;

    DrawPopupWindow(panelX, panelY, panelW, panelH, title, popupAlpha);

    if (!g_textManager || !g_textManager->SetFontByName(DEF_MENU_FONT_NAME))
        return;

    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE), SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE));
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(true);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);

    const s32 contentTop = panelY + DEF_TITLE_BAR_H + DEF_CONTENT_TOP_PAD;
    const s32 extraH = CalcPageExtraHeight(popup);
    const s32 entryBlockH = DEF_CONTENT_PAD + DEF_ROW_TEXT_H + extraH;
    const s32 bodyAvailH = panelH - DEF_TITLE_BAR_H - DEF_BOTTOM_BAR_H - DEF_CONTENT_TOP_PAD - DEF_CONTENT_BOTTOM_PAD;
    const s32 bodyCenterPad = (bodyAvailH > entryBlockH) ? ((bodyAvailH - entryBlockH) / 2) : 0;
    const s32 firstY = contentTop + bodyCenterPad + DEF_CONTENT_PAD;

    if (popup.numEntries > 0) {
        const Entry& infoEntry = popup.entries[0];
        const char* label = Localize(infoEntry.token);
        if (!label) label = infoEntry.token;
#if AUTO_UPDATER
        char updateInfoText[256];
        if (BuildUpdateInfoText(infoEntry.token, updateInfoText, (s32)sizeof(updateInfoText))) {
            label = updateInfoText;
        }
#endif
        char assetInfoText[256];
        if (BuildAssetInfoText(infoEntry.token, assetInfoText, (s32)sizeof(assetInfoText))) {
            label = assetInfoText;
        }

        const f32 wrapWidth = SCREEN_SCALE_X((f32)(popup.frameW - DEF_LABEL_X_PAD * 2));
        if (g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
            g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE), SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE));
            g_textManager->SetShadow(false);
            g_textManager->SetOutline(false);
            g_textManager->SetAlignment(TextAlign_Center);
            g_textManager->SetWrapWidth(wrapWidth);
            g_textManager->SetColor(DEF_INFO_TEXT_R, DEF_INFO_TEXT_G, DEF_INFO_TEXT_B, ScaleAlphaU8(255, popupAlpha));
            g_textManager->PrintString(label, SCALE_AND_CENTER_X((f32)DEF_WINDOW_CENTER_X), SCREEN_SCALE_Y((f32)firstY));
            g_textManager->SetWrapWidth(0.0f);
            g_textManager->SetFontByName(DEF_MENU_FONT_NAME);
        }
    }

    RenderActivePopupContent(panelX, panelW, firstY, popupAlpha);

    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE), SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE));
}

void feCustomMenuMgr::RenderActivePopupContent(s32 panelX, s32 panelW, s32 firstY, f32 popupAlpha) const {
    static constexpr s32 kPopupBarExtraGap = 6;

    switch (m_activePopup) {
        case PopupKind_AutosaveNotice:
        {
            const PageDef& popup = m_popups[PopupKind_AutosaveNotice];
            s32 lines = 1;
            if (popup.numEntries > 0) {
                const char* notice = Localize(popup.entries[0].token);
                if (!notice) notice = popup.entries[0].token;
                lines = GetWrappedLineCount(popup, notice, DEF_MENU_FONT_NAME, DEF_MENU_TEXT_SCALE);
            }
            const s32 spinnerY = firstY + DEF_ROW_TEXT_H
                + (lines - 1) * DEF_ROW_STEP + DEF_INFO_ROW_EXTRA + 12;
            RenderAutosaveSpinner(SCALE_AND_CENTER_X(panelX + panelW / 2), SCREEN_SCALE_Y(spinnerY), popupAlpha);
            break;
        }
#if AUTO_UPDATER
        case PopupKind_CheckingUpdate:
            RenderUpdateIndeterminateBar(panelX, panelW, firstY + kPopupBarExtraGap, popupAlpha);
            break;
#endif
        case PopupKind_AssetScanning:
            RenderAssetScanSweep(panelX, panelW, firstY + kPopupBarExtraGap, popupAlpha);
            break;
        case PopupKind_AssetExtracting:
            RenderAssetExtractProgressBar(panelX, panelW, firstY + kPopupBarExtraGap, popupAlpha);
            break;
        default:
            break;
    }
}

void feCustomMenuMgr::RenderAutosaveSpinner(s32 centerX, s32 centerY, f32 alpha01) const {
    static constexpr s32 kSegments = 8;
    static constexpr s32 kCircleSegments = 12;

    static constexpr f32 kRadius = 6.0f;
    static constexpr f32 kDotRadius = 1.75f;

    static constexpr f32 kUnitCircle[kSegments][2] = {
        {  1.000000f,  0.000000f },
        {  0.707107f,  0.707107f },
        {  0.000000f,  1.000000f },
        { -0.707107f,  0.707107f },
        { -1.000000f,  0.000000f },
        { -0.707107f, -0.707107f },
        {  0.000000f, -1.000000f },
        {  0.707107f, -0.707107f },
    };

    const f32 scaleX = SCREEN_SCALE_X(1.0f);
    const f32 scaleY = SCREEN_SCALE_Y(1.0f);

    // Keeps the spinner visually round when X/Y screen scaling differs.
    const f32 xCompensation = (scaleX > 0.0f) ? (scaleY / scaleX) : 1.0f;

    const f32 radiusX = kRadius * xCompensation;
    const f32 radiusY = kRadius;

    const f32 dotRadiusX = kDotRadius * xCompensation;
    const f32 dotRadiusY = kDotRadius;

    // 15 segments/sec, matching original frame / 2 behavior at 30 fps.
    const s32 head = (s32)((s64)(UiAnimSeconds() * 15.0f) % kSegments);

    for (s32 i = 0; i < kSegments; ++i) {
        const s32 distance = (head - i + kSegments) % kSegments;

        // Stronger fade than the old square spinner, because circles look better with a longer trail.
        const s32 alphaBase = 255 - distance * 30;
        if (alphaBase <= 0) {
            continue;
        }

        const u8 alpha = ScaleAlphaU8((u8)alphaBase, alpha01);
        if (alpha == 0) {
            continue;
        }

        // Slight emphasis on the current active dot.
        const f32 pulse = (distance == 0) ? 1.25f : 1.0f;

        const f32 x = (f32)centerX + SCREEN_SCALE_X(kUnitCircle[i][0] * radiusX);
        const f32 y = (f32)centerY + SCREEN_SCALE_Y(kUnitCircle[i][1] * radiusY);

        ScreenDraw::DrawFilledCircle(
            x,
            y,
            SCREEN_SCALE_X(dotRadiusX * pulse),
            SCREEN_SCALE_Y(dotRadiusY * pulse),
            0.0f,
            0.0f,
            1.0f,
            1.0f,
            kCircleSegments,
            255,
            224,
            96,
            alpha
        );
    }
}

void feCustomMenuMgr::RenderLocationPage() const {
    LocationRuntimeInfo info = {};
    if (!ResolveLocationRuntimeInfo(&info)) {
        return;
    }

    if (!g_textManager || !g_textManager->SetFontByName(DEF_MENU_FONT_NAME)) {
        return;
    }
    g_textManager->SetScale(SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE), SCREEN_SCALE_Y(DEF_MENU_TEXT_SCALE));
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(true);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);

    static constexpr s32 LOC_ICON_SIZE = 28;
    static constexpr s32 LOC_ICON_GAP = 14;

    const PageDef& locationPage = m_pages[MenuPage_Location];
    const s32 panelW = locationPage.frameW;
    const s32 panelH = locationPage.frameH;
    const s32 panelX = DEF_WINDOW_CENTER_X - panelW / 2;
    const s32 panelY = DEF_WINDOW_CENTER_Y - panelH / 2;
    const s32 bodyY0 = panelY + DEF_TITLE_BAR_H;
    const s32 bodyY1 = panelY + panelH - DEF_BOTTOM_BAR_H;
    const f32 framePx = GetMenuBorderPx();

    const s32 lineX = panelX + DEF_BORDER_W;
    const s32 lineW = panelW - DEF_BORDER_W * 2;

    const s32 topLineY = bodyY0;
    const s32 gradeTopY = bodyY0;
    const s32 midLineY = gradeTopY + LOC_ICON_SIZE;
    const s32 row1TopY = midLineY;
    const s32 row2TopY = row1TopY + LOC_ICON_SIZE;
    const s32 botLineY = row2TopY + LOC_ICON_SIZE + 2;

    const s32 gridW = 5 * LOC_ICON_SIZE + 4 * LOC_ICON_GAP;
    const s32 gridLeftX = DEF_WINDOW_CENTER_X - gridW / 2;

    const s32 labelX = panelX - DEF_LABEL_X_PAD + DEF_WINDOW_W / 2;
    const s32 gradeBoxX = labelX + 12;
    const s32 gradeBoxW = 64;
    const s32 gradeBoxH = 18;
    const s32 goldIconX = gradeBoxX + gradeBoxW + 12;
    const s32 goldIconY = gradeTopY;

    // Red separator lines
    DrawUniformHLinePSX(lineX, midLineY, lineW, framePx, DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, DEF_FRAME_A);

    // Grade black box
    DrawUniformBorderFillRectPSX(gradeBoxX, gradeTopY + 5, gradeBoxW, gradeBoxH, framePx,
                                 DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, DEF_FRAME_A,
                                 0, 0, 0, 255);

    // Grade label
    {
        const char* lbl = Localize("FE_GRD");
        const f32 lx = SCALE_AND_CENTER_X((f32)labelX);
        const f32 ly = SCREEN_SCALE_Y((f32)(gradeTopY + 8));
        g_textManager->SetAlignment(TextAlign_Right);
        g_textManager->SetColor(DEF_TEXT_NORM_R, DEF_TEXT_NORM_G, DEF_TEXT_NORM_B);
        g_textManager->PrintString(lbl, lx, ly);
    }

    // Grade letter from stored per-petal score stats.
    {
        const char* text = GradeToLetter(info.hasGrade ? info.grade : 0);
        const f32 cx = SCALE_AND_CENTER_X((f32)(gradeBoxX + gradeBoxW / 2));
        const f32 cy = SCREEN_SCALE_Y((f32)(gradeTopY + 8));
        g_textManager->SetAlignment(TextAlign_Center);
        g_textManager->SetColor(DEF_TITLE_TEXT_R, DEF_TITLE_TEXT_G, DEF_TITLE_TEXT_B);
        g_textManager->PrintString(text, cx, cy);
    }

    if (info.showDragons) {
        // Gold dragon icon at right of grade row, derived from collect count.
        {
            tTexture* iconTex = info.hasGoldDragon ? m_goldDragonTex : m_greyDragonTex;
            if (iconTex) {
                ScreenDraw::DrawQuad(iconTex,
                                     SCALE_AND_CENTER_X((f32)goldIconX), SCREEN_SCALE_Y((f32)goldIconY),
                                     SCREEN_SCALE_Y((f32)LOC_ICON_SIZE), SCREEN_SCALE_Y((f32)LOC_ICON_SIZE),
                                     0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255);
            }
        }

        // Dragon bar icons from stored collect count.
        {
            const s32 rowTopY[2] = { row1TopY, row2TopY };
            const s32 unlocked = (info.collectCount < 0) ? 0 : ((info.collectCount > 10) ? 10 : info.collectCount);
            for (s32 row = 0; row < 2; row++) {
                for (s32 col = 0; col < 5; col++) {
                    const s32 idx = row * 5 + col;
                    tTexture* tex = (idx < unlocked) ? m_redDragonTex : m_greyDragonTex;
                    if (!tex)
                        continue;
                    const s32 ix = gridLeftX + col * (LOC_ICON_SIZE + LOC_ICON_GAP);
                    ScreenDraw::DrawQuad(tex,
                                         SCALE_AND_CENTER_X((f32)ix), SCREEN_SCALE_Y((f32)rowTopY[row]),
                                         SCREEN_SCALE_Y((f32)LOC_ICON_SIZE), SCREEN_SCALE_Y((f32)LOC_ICON_SIZE),
                                         0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255);
                }
            }
        }
    }
}

void feCustomMenuMgr::DrawRect(f32 x, f32 y, f32 w, f32 h, u8 r, u8 g, u8 b, u8 a) const {
    const f32 nx = SCALE_AND_CENTER_X(x);
    const f32 ny = SCREEN_SCALE_Y(y);
    const f32 nw = SCREEN_SCALE_X(w);
    const f32 nh = SCREEN_SCALE_Y(h);
    ScreenDraw::DrawColoredRect(nx, ny, nw, nh, r, g, b, a);
}

void feCustomMenuMgr::DrawHighlight(f32 x, f32 y, f32 w, f32 h) const {
    DrawRect(x, y, w, h,
             m_pulse.GetRed8(), m_pulse.GetGreen8(), m_pulse.GetBlue8(), 55);
}

void feCustomMenuMgr::DrawScrollBar(f32 x, f32 y, f32 h, s32 totalItems,
                                    s32 visibleItems, f32 scrollTop) const {
    if (h <= 0.0f || totalItems <= visibleItems || visibleItems <= 0) {
        return;
    }

    static constexpr f32 kTrackW = 8.0f;
    static constexpr f32 kInset = 1.0f;
    static constexpr f32 kMinThumbH = 12.0f;
    const f32 innerH = h - kInset * 2.0f;
    f32 thumbH = innerH * (f32)visibleItems / (f32)totalItems;
    if (thumbH < kMinThumbH) thumbH = kMinThumbH;
    if (thumbH > innerH) thumbH = innerH;

    const s32 maxTop = totalItems - visibleItems;
    if (scrollTop < 0) scrollTop = 0;
    if (scrollTop > (f32)maxTop) scrollTop = (f32)maxTop;
    const f32 travel = innerH - thumbH;
    const f32 thumbY = y + kInset + (maxTop > 0 ? travel * (f32)scrollTop / (f32)maxTop : 0.0f);

    DrawRect(x, y, kTrackW, h, DEF_SCROLLBAR_FILL_R / 2, DEF_SCROLLBAR_FILL_G / 2, DEF_SCROLLBAR_FILL_B / 2, DEF_SCROLLBAR_FILL_A / 2);
    DrawUniformBorderRectPSX(x, y, kTrackW, h, GetMenuBorderPx(), 105, 44, 8, 240);
    DrawRect(x + kInset, thumbY, kTrackW - kInset * 2.0f, thumbH,
             DEF_SCROLLBAR_FILL_R, DEF_SCROLLBAR_FILL_G, DEF_SCROLLBAR_FILL_B, DEF_SCROLLBAR_FILL_A);
}

bool feCustomMenuMgr::HandleScrollBarMouse(f32 x, f32 y, f32 h, s32 totalItems,
                                           s32 visibleItems, s32* scrollTop, f32* visualScrollTop,
                                           f32 mouseX, f32 mouseY,
                                           bool leftPressed, bool leftDown) {
    if (!scrollTop || !visualScrollTop || h <= 0.0f || totalItems <= visibleItems || visibleItems <= 0) {
        m_scrollBarDragging = false;
        return false;
    }

    static constexpr f32 kTrackW = 8.0f;
    static constexpr f32 kInset = 1.0f;
    static constexpr f32 kMinThumbH = 12.0f;
    const f32 innerH = h - kInset * 2.0f;
    f32 thumbH = innerH * (f32)visibleItems / (f32)totalItems;
    if (thumbH < kMinThumbH) thumbH = kMinThumbH;
    if (thumbH > innerH) thumbH = innerH;
    const s32 maxTop = totalItems - visibleItems;
    if (*visualScrollTop < 0.0f) *visualScrollTop = 0.0f;
    if (*visualScrollTop > (f32)maxTop) *visualScrollTop = (f32)maxTop;
    const f32 travel = innerH - thumbH;
    const f32 thumbY = y + kInset + (maxTop > 0 ? travel * *visualScrollTop / (f32)maxTop : 0.0f);
    const bool overTrack = mouseX >= x && mouseX < x + kTrackW
        && mouseY >= y && mouseY < y + h;

    if (leftPressed && overTrack) {
        m_scrollBarDragging = true;
        if (mouseY >= thumbY && mouseY < thumbY + thumbH) {
            m_scrollBarGrabOffset = mouseY - thumbY;
        }
        else {
            m_scrollBarGrabOffset = thumbH * 0.5f;
        }
    }

    if (!leftDown) {
        const bool wasDragging = m_scrollBarDragging;
        m_scrollBarDragging = false;
        return wasDragging || (leftPressed && overTrack);
    }

    if (!m_scrollBarDragging) {
        return false;
    }

    const f32 desiredThumbY = mouseY - m_scrollBarGrabOffset;
    f32 ratio = travel > 0.0f ? (desiredThumbY - y - kInset) / travel : 0.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    *visualScrollTop = ratio * (f32)maxTop;
    *scrollTop = (s32)(*visualScrollTop + 0.5f);
    return true;
}

const char* feCustomMenuMgr::Localize(const char* token) const {
    if (!m_text || !token)
        return nullptr;

    return m_text->GetString(token);
}

void feCustomMenuMgr::RenderControllerOverlay(s32 panelX, s32 panelY) const {
    if (!m_controllerTexture) {
        return;
    }

    const s32 texW = m_controllerTexture->GetWidth();
    const s32 texH = m_controllerTexture->GetHeight();
    const f32 screenTexH = SCREEN_SCALE_Y(128);
    const f32 screenTexW = (texH > 0) ? screenTexH * ((f32)texW / (f32)texH) : screenTexH;
    const f32 screenTexX = SCALE_AND_CENTER_X(DEFAULT_SCREEN_WIDTH / 2);
    const f32 screenTexY = SCREEN_SCALE_Y(panelY + 32.0f);

    ScreenDraw::DrawQuad(m_controllerTexture, screenTexX - screenTexW / 2, screenTexY, screenTexW, screenTexH,
                         0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255);

    struct ButtonLabel {
        s32 physicalIndex;
        const char* token;
        f32 relX;
        f32 relY;
        TextAlign alignment;
    };

    ButtonLabel kButtonsDefault[] = {
        { 0,  nullptr,  -104.0f,  7.0f,  TextAlign_Right },
        { 2,  nullptr,  -104.0f,  16.0f, TextAlign_Right },
        { -1, "FE_CMV", -104.0f, 37.5f, TextAlign_Right },
        { -1, "FE_CMV", -104.0f, 55.5f, TextAlign_Right },
        { -1, "FE_CNU", -104.0f, 71.5f, TextAlign_Right },

        { 1,  nullptr,  104.0f,   7.0f,  TextAlign_Left },
        { 3,  nullptr,  104.0f,   16.0f, TextAlign_Left },
        { 7,  nullptr,  104.0f,   23.0f, TextAlign_Left },
        { 4,  nullptr,  104.0f,   29.5f, TextAlign_Left },
        { 5,  nullptr,  104.0f,   36.5f, TextAlign_Left },
        { 6,  nullptr,  104.0f,   44.0f, TextAlign_Left },
        { -1, "FE_CNU", 104.0f,  54.0f, TextAlign_Left },
        { -1, "FE_CMO", 104.0f,  71.5f, TextAlign_Left },
    };

    ButtonLabel kButtonsSwitch[] = {
        { 0,  nullptr,  -162.0f,  8.0f,  TextAlign_Right },
        { 2,  nullptr,  -162.0f,  19.0f, TextAlign_Right },
        { -1, "FE_CNU", -162.0f, 26.0f, TextAlign_Right },
        { -1, "FE_CMV", -162.0f, 38.0f, TextAlign_Right },
        { -1, "FE_CMV", -162.0f, 53.0f, TextAlign_Right },

        { 1,  nullptr,  162.0f,   6.0f,  TextAlign_Left },
        { 3,  nullptr,  162.0f,   19.0f, TextAlign_Left },
        { -1, "FE_CMO", 162.0f,   26.0f, TextAlign_Left },
        { 4,  nullptr,  162.0f,   31.5f, TextAlign_Left },
        { 5,  nullptr,  162.0f,   37.0f, TextAlign_Left },
        { 6,  nullptr,  162.0f,   42.0f, TextAlign_Left },
        { 7,  nullptr,  162.0f,   47.5f, TextAlign_Left },
        { -1, "FE_CNU", 162.0f,  55.5f, TextAlign_Left },
    };

    const bool useSwitchLabels = ControllerPromptManager::GetStyle() == (s32)ControllerPromptStyle_Switch;
    const ButtonLabel* buttons = useSwitchLabels ? kButtonsSwitch : kButtonsDefault;
    const s32 buttonCount = useSwitchLabels
        ? (s32)(sizeof(kButtonsSwitch) / sizeof(kButtonsSwitch[0]))
        : (s32)(sizeof(kButtonsDefault) / sizeof(kButtonsDefault[0]));

    if (!g_textManager || !g_textManager->SetFontByName("Legal")) {
        return;
    }

    if (useSwitchLabels)
        g_textManager->SetScale(SCREEN_SCALE_Y(DEF_CONTROLLER_ACTION_SCALE * 0.8f), SCREEN_SCALE_Y(DEF_CONTROLLER_ACTION_SCALE * 0.8f));
    else
        g_textManager->SetScale(SCREEN_SCALE_Y(DEF_CONTROLLER_ACTION_SCALE), SCREEN_SCALE_Y(DEF_CONTROLLER_ACTION_SCALE));

    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(true);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);
    const u8* playerMap = g_inputManager ? g_inputManager->PlayerMapArray() : nullptr;

    for (s32 btnIdx = 0; btnIdx < buttonCount; btnIdx++) {
        const ButtonLabel& btn = buttons[btnIdx];
        char displayName[32] = {};
        const char* text = nullptr;

        if (btn.token) {
            text = Localize(btn.token);
        }
        else if (btn.physicalIndex >= 0 && playerMap && btn.physicalIndex < 16) {
            text = Localize(GetControllerLogicalLabelToken(playerMap[btn.physicalIndex]));
        }

        if (!text) {
            displayName[0] = '\0';
        }
        else {
            snprintf(displayName, sizeof(displayName), "%s", text);
        }

        if (displayName[0] != '\0') {
            const f32 labelScreenX = screenTexX + SCREEN_SCALE_X(btn.relX);
            const f32 labelScreenY = screenTexY + SCREEN_SCALE_Y(btn.relY);

            g_textManager->SetAlignment(btn.alignment);
            g_textManager->SetColor(DEF_TEXT_NORM_R, DEF_TEXT_NORM_G, DEF_TEXT_NORM_B);
            g_textManager->PrintString(displayName, labelScreenX, labelScreenY);
        }
    }

    g_textManager->SetScale(1.0f, 1.0f);
    g_textManager->SetFontByName(DEF_MENU_FONT_NAME);
}

#if AUTO_UPDATER
void feCustomMenuMgr::DrawVersionOverlay() {
    if (!g_textManager)
        return;

    static constexpr f32 kOverlayX = 8.0f;
    static constexpr f32 kOverlayY = 6.0f;
    static constexpr f32 kOverlayScale = 0.2f;
    static constexpr f32 kOverlayLineStep = 5.0f;

    g_textManager->SetFontByName("Legal");
    g_textManager->SetScale(SCREEN_SCALE_Y(kOverlayScale), SCREEN_SCALE_Y(kOverlayScale));
    g_textManager->SetAlignment(TextAlign_Left);
    g_textManager->SetWrapWidth(0.0f);
    g_textManager->SetLineSpacing(0);
    g_textManager->SetPromptsEnabled(false);
    g_textManager->SetShadow(false);
    g_textManager->SetOutline(true);
    g_textManager->SetColor(255, 255, 255);
    g_textManager->PrintString(GAME_VERSION, HudX(kOverlayX), HudY(kOverlayY));

    if (g_autoUpdater && g_autoUpdater->IsUpdateAvailable()) {
        const char* text = Localize("FE_UPD_AVAIL");
        if (!text)
            text = "Update available";

        static constexpr f32 kFadeCycleSec = 2.0f;
        if (g_time) {
            m_updateAvailFadeTimer += g_time->GetDeltaTime();
            while (m_updateAvailFadeTimer >= kFadeCycleSec) {
                m_updateAvailFadeTimer -= kFadeCycleSec;
            }
        }

        const f32 half = kFadeCycleSec * 0.5f;
        f32 t = (m_updateAvailFadeTimer < half)
            ? (m_updateAvailFadeTimer / half)
            : ((kFadeCycleSec - m_updateAvailFadeTimer) / half);
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        const u8 alpha = (u8)(t * 255.0f);

        g_textManager->SetColor(DEF_TEXT_NORM_R, DEF_TEXT_NORM_G, DEF_TEXT_NORM_B, alpha);
        g_textManager->PrintString(text, HudX(kOverlayX), HudY(kOverlayY + kOverlayLineStep));
    }

    g_textManager->SetPromptsEnabled(true);
}

void feCustomMenuMgr::RenderUpdateProgressBar(s32 panelX, s32 panelW, s32 rowTop) const {
    if (!g_autoUpdater) return;

    // Offset below the "FE_UPD_PRG" Info row by however many lines it actually
    // wrapped to, so longer translated/interpolated text doesn't overlap the bar.
    const PageDef& page = m_pages[MenuPage_Update];
    s32 lines = 1;
    if (page.numEntries > 0) {
        const Entry& infoEntry = page.entries[0];
        const char* label = Localize(infoEntry.token);
        if (!label) label = infoEntry.token;
        char updateInfoText[256];
        if (BuildUpdateInfoText(infoEntry.token, updateInfoText, (s32)sizeof(updateInfoText))) {
            label = updateInfoText;
        }
        lines = GetWrappedLineCount(page, label);
    }

    const s32 barX = panelX + DEF_LABEL_X_PAD;
    const s32 barY = rowTop + DEF_ROW_TEXT_H + (lines - 1) * DEF_ROW_STEP + DEF_INFO_ROW_EXTRA;
    const s32 barW = panelW - DEF_LABEL_X_PAD * 2;
    const s32 barH = DEF_METER_H;

    DrawUniformBorderRectPSX(barX, barY, barW, barH, GetMenuBorderPx(),
                             DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, DEF_FRAME_A);
    DrawRect((f32)barX, (f32)barY, (f32)barW, (f32)barH,
             DEF_SLIDER_TRACK_R, DEF_SLIDER_TRACK_G, DEF_SLIDER_TRACK_B, DEF_SLIDER_TRACK_A);

    f32 progress = g_autoUpdater->GetDownloadProgress();
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    const s32 fillW = (s32)((f32)barW * progress);
    if (fillW > 0) {
        DrawRect((f32)barX, (f32)barY, (f32)fillW, (f32)barH,
                 DEF_SLIDER_FILL_R, DEF_SLIDER_FILL_G, DEF_SLIDER_FILL_B, DEF_SLIDER_FILL_A);
    }
}

void feCustomMenuMgr::RenderUpdateIndeterminateBar(s32 panelX, s32 panelW, s32 rowTop, f32 alpha01) const {
    const PageDef& page = m_popups[PopupKind_CheckingUpdate];
    s32 lines = 1;
    if (page.numEntries > 0) {
        const Entry& infoEntry = page.entries[0];
        const char* label = Localize(infoEntry.token);
        if (!label) label = infoEntry.token;
        lines = GetWrappedLineCount(page, label, DEF_MENU_FONT_NAME, DEF_MENU_TEXT_SCALE);
    }

    const s32 barX = panelX + DEF_LABEL_X_PAD;
    const s32 barY = rowTop + DEF_ROW_TEXT_H + (lines - 1) * DEF_ROW_STEP + DEF_INFO_ROW_EXTRA;
    const s32 barW = panelW - DEF_LABEL_X_PAD * 2;
    const s32 barH = DEF_METER_H;

    DrawUniformBorderRectPSX(barX, barY, barW, barH, GetMenuBorderPx(),
                             DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01));
    DrawRect((f32)barX, (f32)barY, (f32)barW, (f32)barH,
             DEF_SLIDER_TRACK_R, DEF_SLIDER_TRACK_G, DEF_SLIDER_TRACK_B, ScaleAlphaU8(DEF_SLIDER_TRACK_A, alpha01));

    // No real progress to report yet, so sweep a block back and forth across the track.
    // 2.0s round-trip (matches the original 60-frame cycle at 30 fps), wall-clock paced.
    const s32 blockW = barW / 3;
    const f32 frac = UiTriangle01(2.0f);
    const s32 blockX = barX + (s32)((f32)(barW - blockW) * frac);

    DrawRect((f32)blockX, (f32)barY, (f32)blockW, (f32)barH,
             DEF_SLIDER_FILL_R, DEF_SLIDER_FILL_G, DEF_SLIDER_FILL_B, ScaleAlphaU8(DEF_SLIDER_FILL_A, alpha01));
}
#endif

void feCustomMenuMgr::RefreshAssetPageEntries() {
    if (!g_psxDiscExtractor) {
        return;
    }

    PageDef& page = m_pages[MenuPage_AssetMissing];

    switch (g_psxDiscExtractor->GetState()) {
        case PsxDiscExtractor::State::ScanNoneFound:
            SetEntries(page, {
                Info("FE_ASSET_NONE"),
                Button("FE_ASSET_RESCAN", EntryEvent_ScanForAssets),
                Button("FE_QTG", EntryEvent_QuitFromAssetCheck),
                       });
            break;
        case PsxDiscExtractor::State::ScanMultipleFound:
            SetEntries(page, {
                Info("FE_ASSET_MULTI"),
                Button("FE_ASSET_RESCAN", EntryEvent_ScanForAssets),
                Button("FE_QTG", EntryEvent_QuitFromAssetCheck),
                       });
            break;
        case PsxDiscExtractor::State::ScanFoundOne:
            SetEntries(page, {
                Info("FE_ASSET_ONE"),
                Button("FE_ASSET_EXT", EntryEvent_ExtractAssets),
                Button("FE_QTG", EntryEvent_QuitFromAssetCheck),
                       });
            break;
        case PsxDiscExtractor::State::Error:
            SetEntries(page, {
                Info("FE_ASSET_ERR"),
                Button("FE_ASSET_RESCAN", EntryEvent_ScanForAssets),
                Button("FE_QTG", EntryEvent_QuitFromAssetCheck),
                       });
            break;
        default:
            SetEntries(page, {
                Info("FE_ASSET_MISS"),
                Button("FE_ASSET_EXT", EntryEvent_ScanForAssets),
                Button("FE_QTG", EntryEvent_QuitFromAssetCheck),
                       });
            break;
    }

    // A refresh mid-visit can shrink the entry list out from under the cursor.
    if (m_cursor < 0 || m_cursor >= page.numEntries || page.entries[m_cursor].type == EntryType_Info) {
        m_cursor = 0;
        while (m_cursor < page.numEntries && page.entries[m_cursor].type == EntryType_Info) {
            m_cursor++;
        }
    }
}

bool feCustomMenuMgr::BuildAssetInfoText(const char* token, char* outText, s32 outTextLen) const {
    if (!g_psxDiscExtractor || !token) {
        return false;
    }

    if (!strcmp(token, "FE_ASSET_ERR")) {
        const char* fmt = Localize("FE_ASSET_ERR");
        if (!fmt) fmt = "Extraction failed: %s";
        snprintf(outText, outTextLen, fmt, g_psxDiscExtractor->GetError());
        return true;
    }
    return false;
}

void feCustomMenuMgr::RenderAssetExtractProgressBar(s32 panelX, s32 panelW, s32 rowTop, f32 alpha01) const {
    if (!g_psxDiscExtractor) return;

    // Offset below the "FE_ASSET_EXTG" Info row by however many lines it actually
    // wrapped to, so longer translated text doesn't overlap the bar.
    const PageDef& page = m_popups[PopupKind_AssetExtracting];
    s32 lines = 1;
    if (page.numEntries > 0) {
        const Entry& infoEntry = page.entries[0];
        const char* label = Localize(infoEntry.token);
        if (!label) label = infoEntry.token;
        char assetInfoText[256];
        if (BuildAssetInfoText(infoEntry.token, assetInfoText, (s32)sizeof(assetInfoText))) {
            label = assetInfoText;
        }
        lines = GetWrappedLineCount(page, label, DEF_MENU_FONT_NAME, DEF_MENU_TEXT_SCALE);
    }

    const s32 barX = panelX + DEF_LABEL_X_PAD;
    const s32 barY = rowTop + DEF_ROW_TEXT_H + (lines - 1) * DEF_ROW_STEP + DEF_INFO_ROW_EXTRA;
    const s32 barW = panelW - DEF_LABEL_X_PAD * 2;
    const s32 barH = DEF_METER_H;

    DrawUniformBorderRectPSX(barX, barY, barW, barH, GetMenuBorderPx(),
                             DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01));
    DrawRect((f32)barX, (f32)barY, (f32)barW, (f32)barH,
             DEF_SLIDER_TRACK_R, DEF_SLIDER_TRACK_G, DEF_SLIDER_TRACK_B, ScaleAlphaU8(DEF_SLIDER_TRACK_A, alpha01));

    f32 progress = g_psxDiscExtractor->GetProgress();
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    const s32 fillW = (s32)((f32)barW * progress);
    if (fillW > 0) {
        DrawRect((f32)barX, (f32)barY, (f32)fillW, (f32)barH,
                 DEF_SLIDER_FILL_R, DEF_SLIDER_FILL_G, DEF_SLIDER_FILL_B, ScaleAlphaU8(DEF_SLIDER_FILL_A, alpha01));
    }
}

void feCustomMenuMgr::RenderAssetScanSweep(s32 panelX, s32 panelW, s32 rowTop, f32 alpha01) const {
    // Offset below the "FE_ASSET_SCAN" Info row by however many lines it actually
    // wrapped to, so longer translated text doesn't overlap the bar.
    const PageDef& page = m_popups[PopupKind_AssetScanning];
    s32 lines = 1;
    if (page.numEntries > 0) {
        const Entry& infoEntry = page.entries[0];
        const char* label = Localize(infoEntry.token);
        if (!label) label = infoEntry.token;
        lines = GetWrappedLineCount(page, label, DEF_MENU_FONT_NAME, DEF_MENU_TEXT_SCALE);
    }

    const s32 barX = panelX + DEF_LABEL_X_PAD;
    const s32 barY = rowTop + DEF_ROW_TEXT_H + (lines - 1) * DEF_ROW_STEP + DEF_INFO_ROW_EXTRA;
    const s32 barW = panelW - DEF_LABEL_X_PAD * 2;
    const s32 barH = DEF_METER_H;

    DrawUniformBorderRectPSX(barX, barY, barW, barH, GetMenuBorderPx(),
                             DEF_FRAME_R, DEF_FRAME_G, DEF_FRAME_B, ScaleAlphaU8(DEF_FRAME_A, alpha01));
    DrawRect((f32)barX, (f32)barY, (f32)barW, (f32)barH,
             DEF_SLIDER_TRACK_R, DEF_SLIDER_TRACK_G, DEF_SLIDER_TRACK_B, ScaleAlphaU8(DEF_SLIDER_TRACK_A, alpha01));

    // Scanning a single folder is effectively instant - sweep a block, same as the
    // update-checker's indeterminate bar, rather than faking a 0..1 progress value.
    // 2.0s round-trip (matches the original 60-frame cycle at 30 fps), wall-clock paced.
    const s32 blockW = barW / 3;
    const f32 frac = UiTriangle01(2.0f);
    const s32 blockX = barX + (s32)((f32)(barW - blockW) * frac);

    DrawRect((f32)blockX, (f32)barY, (f32)blockW, (f32)barH,
             DEF_SLIDER_FILL_R, DEF_SLIDER_FILL_G, DEF_SLIDER_FILL_B, ScaleAlphaU8(DEF_SLIDER_FILL_A, alpha01));
}

#pragma once
#include "core.h"
#include "gen/config.h"
#include "gen/savegame.h"
#include "extra/customtext.h"
#include "extra/colorpulse.h"
#include "extra/controllerpromptstyle.h"
#include <functional>
#include <initializer_list>
#include <vector>

#if AUTO_UPDATER
#include "extra/autoupdater.h"
#endif

#include "extra/psxdiscextractor.h"

class tTexture;
class pddiBaseShader;
class pddiRenderTarget;

const char* GetSpecialLocationToken(s32 levelID);

enum MenuPage : s32 {
    MenuPage_None,
    MenuPage_Frontend,
    MenuPage_Pause,
    MenuPage_Title,
    MenuPage_StartGame,
    MenuPage_Options,
    MenuPage_Controls,
    MenuPage_Controller,
    MenuPage_KeyBindings,
    MenuPage_Display,
    MenuPage_Sound,
#if NEW_CHEATS
    MenuPage_Cheats,
#endif
#if MOD_LOADER
    MenuPage_Mods,
#endif
    MenuPage_LoadSlots,
    MenuPage_SaveSlots,
    MenuPage_DeleteSlots,
    MenuPage_LoadConfirm,
    MenuPage_SaveConfirm,
    MenuPage_DeleteConfirm,
    MenuPage_SaveDone,
    MenuPage_DeleteDone,
    MenuPage_NewGameConfirm,
    MenuPage_ExitLevelConfirm,
    MenuPage_QuitConfirm,
    MenuPage_Quitting,
    MenuPage_Location,
    MenuPage_Update,
    MenuPage_Changelog,
    MenuPage_AssetMissing,
    MenuPage_Count,
};

// Fade-popup overlays: drawn on top of whatever page (or nothing) is
// currently active, never via SetPage(). Adding a new one only needs an
// AddPopup() entry plus an OpenPopup() call with a completion lambda - no
// MenuPage enum value, no m_pages slot, no Invoke()/Render() switch case.
enum PopupKind : s32 {
    PopupKind_None,
    PopupKind_AutosaveNotice,
    PopupKind_CheckingUpdate,
    PopupKind_AssetScanning,
    PopupKind_AssetExtracting,
    PopupKind_Count,
};

enum EntryType : u8 {
    EntryType_Button,
    EntryType_Slider,
    EntryType_List,
    EntryType_Toggle,
    EntryType_Info,
};

enum EntryBinding : u8 {
    EntryBinding_None,
    EntryBinding_MusicVol,
    EntryBinding_EffectsVol,
    EntryBinding_DialogVol,
    EntryBinding_Stereo,
    EntryBinding_Shock,
    EntryBinding_Vibration,
    EntryBinding_Lightbar,
    EntryBinding_PlayerConfig,
    EntryBinding_ControllerPromptStyle,
    EntryBinding_Language,
    EntryBinding_HudStyle,
    EntryBinding_DisplayResolution,
    EntryBinding_DisplayScreenMode,
    EntryBinding_DisplayVsync,
    EntryBinding_DisplayFrameRate,
    EntryBinding_DisplayMsaa,
#if MODERN_GRAPHICS
    EntryBinding_DisplayShadowQuality,
#endif
#if NEW_CHEATS
    EntryBinding_CheatAllDragons,
    EntryBinding_CheatAllLevels,
    EntryBinding_CheatGodMode,
    EntryBinding_CheatOnePunchMan,
    EntryBinding_CheatHeavenBound,
    EntryBinding_CheatBobbleHead,
    EntryBinding_CheatStuntquake,
    EntryBinding_CheatMirrorWorld,
#endif
};

enum EntryEvent : u8 {
    EntryEvent_None,
    EntryEvent_GoPage,
    EntryEvent_Resume,
    EntryEvent_Back,
    EntryEvent_NewGame,
    EntryEvent_Continue,
    EntryEvent_ExitToHub,
    EntryEvent_QuitGame,
    EntryEvent_Credits,
    EntryEvent_Load,
    EntryEvent_Save,
    EntryEvent_Delete,
    EntryEvent_LoadConfirmYes,
    EntryEvent_SaveConfirmYes,
    EntryEvent_DeleteConfirmYes,
    EntryEvent_LocationSelect,
    EntryEvent_StartUpdate,
    EntryEvent_DismissUpdate,
    EntryEvent_CancelUpdate,
    EntryEvent_InstallUpdate,
    EntryEvent_ShowChangelog,
    EntryEvent_CheckForUpdate,
    EntryEvent_ScanForAssets,
    EntryEvent_ExtractAssets,
    EntryEvent_QuitFromAssetCheck,
};

#define MAX_ENTRIES_PER_MENU (12)
#define MAX_CUSTOM_MENU_TOKEN (16)

#if defined(RC_PLATFORM_PSP)
// PSP menu profile: the 4:3 layout spans ~362 of the 480 pixels at 1.13x its
// height, so windows are wider and rows/bars taller to carry 18px entries
// and titles and 14px hints (exact font strikes; see ApplyHudStyleFonts).
#define DEF_WINDOW_W 420
#else
#define DEF_WINDOW_W 340
#endif
#define DEF_WINDOW_H 108

#define DEF_MENU_FONT_NAME ("Menu")

// Layout tuning for auto-sized custom FE windows.
#define DEF_WINDOW_CENTER_X 256
#define DEF_WINDOW_CENTER_Y 114
#if defined(RC_PLATFORM_PSP)
#define DEF_ROW_STEP 19
#define DEF_TITLE_BAR_H 32
#define DEF_BOTTOM_BAR_H 19
#else
#define DEF_ROW_STEP 16
#define DEF_TITLE_BAR_H 30
#define DEF_BOTTOM_BAR_H 16
#endif
#define DEF_BORDER_W 1
#define DEF_CONTENT_PAD 4
#define DEF_CONTENT_TOP_PAD 4
#define DEF_CONTENT_BOTTOM_PAD 4
#if defined(RC_PLATFORM_PSP)
#define DEF_ROW_TEXT_H 16
#else
#define DEF_ROW_TEXT_H 12
#endif
#define DEF_INFO_ROW_EXTRA (0)

#define DEF_LABEL_X_PAD 18
#define DEF_VALUE_X_PAD 18
#define DEF_METER_W 74
#define DEF_METER_H 8

#define DEF_TITLE_INSET_X 48
#define DEF_TITLE_INSET_Y 6
#if defined(RC_PLATFORM_PSP)
#define DEF_TITLE_INSET_H 22
#else
#define DEF_TITLE_INSET_H 20
#endif

#define DEF_HELP_Y_PAD 3
#define DEF_HELP_LEFT_X_OFF (-28)
#define DEF_HELP_RIGHT_X_OFF 42

// Prompt/help spacing tuning
#define DEF_HELP_GROUP_GAP_PX 14.0f

// Controller page tuning
#define DEF_CONTROLLER_WINDOW_W 480
#define DEF_CONTROLLER_WINDOW_H 202

// Changelog page tuning
#define DEF_CHANGELOG_WINDOW_W 380
#define DEF_CHANGELOG_WINDOW_H 200

#define DEF_CHANGELOG_TEXT_W 0.25f
#define DEF_CHANGELOG_TEXT_H 0.25f

// Key Bindings page tuning
#define DEF_KEYBIND_WINDOW_W 420
#define DEF_KEYBIND_WINDOW_H 198
#define DEF_KEYBIND_SLOT_COUNT 2
#define DEF_KEYBIND_VISIBLE_ROWS 8
#define DEF_KEYBIND_ROW_STEP 14
#define DEF_KEYBIND_SLOT_W 76
#define DEF_KEYBIND_SLOT_GAP 8
#define DEF_KEYBIND_TABLE_SIDE_PAD 8
#define DEF_KEYBIND_ACTION_COL_GAP 8
#define DEF_KEYBIND_HIT_PAD 2
#define DEF_KEYBIND_ROW_TOP_PAD 2
#define DEF_KEYBIND_CELL_PAD 2
#define DEF_KEYBIND_X_PAD 18

// Save/load/delete table tuning.
#if defined(RC_PLATFORM_PSP)
// Taller rows for 12-14px slot text; three slots visible at a time.
#define DEF_SAVE_VISIBLE_ROWS 3
#define DEF_SAVE_ROW_H 40
#else
#define DEF_SAVE_VISIBLE_ROWS 4
#define DEF_SAVE_ROW_H 32
#endif
#define DEF_SAVE_AUTOSAVE_GAP 12
#define DEF_SAVE_TABLE_SIDE_PAD 12

#if MOD_LOADER
#define DEF_MODS_WINDOW_W 400
#define DEF_MODS_WINDOW_H 198
#define DEF_MODS_VISIBLE_ROWS 8
#define DEF_MODS_ROW_STEP 14
#endif

// Key Bindings row stripe colors (transparent black / warm yellow)
#define DEF_KEYBIND_STRIPE_DARK_R 0
#define DEF_KEYBIND_STRIPE_DARK_G 0
#define DEF_KEYBIND_STRIPE_DARK_B 0
#define DEF_KEYBIND_STRIPE_DARK_A 100
#define DEF_KEYBIND_STRIPE_WARM_R 100
#define DEF_KEYBIND_STRIPE_WARM_G 60
#define DEF_KEYBIND_STRIPE_WARM_B 0
#define DEF_KEYBIND_STRIPE_WARM_A 100

// Selected binding cell background (pure black, opaque)
#define DEF_KEYBIND_ACTIVE_FILL_R 0
#define DEF_KEYBIND_ACTIVE_FILL_G 0
#define DEF_KEYBIND_ACTIVE_FILL_B 0
#define DEF_KEYBIND_ACTIVE_FILL_A 255

#define DEF_BOTTOM_ORN_COUNT 1
#define DEF_BOTTOM_ORN_STEP 24
#define DEF_BOTTOM_ORN_LEFT_X 16
#define DEF_BOTTOM_ORN_RIGHT_X 104
#define DEF_BOTTOM_ORN_Y_OFF 2

#define DEF_ORN_W 16
#define DEF_ORN_H 10
#define DEF_ORN_R 255
#define DEF_ORN_G 255
#define DEF_ORN_B 255
#define DEF_ORN_A 255

// Gouraud bar gradient colors: bright center highlight, dark at top/bottom edges
#define DEF_BAR_MID_R 255
#define DEF_BAR_MID_G 255
#define DEF_BAR_MID_B 24
#define DEF_BAR_EDGE_R 199
#define DEF_BAR_EDGE_G 80
#define DEF_BAR_EDGE_B 0
#define DEF_BAR_ALPHA 255

// Outer border / separator lines
#define DEF_FRAME_R 130
#define DEF_FRAME_G 0
#define DEF_FRAME_B 0
#define DEF_FRAME_A 255

// Body fill
#define DEF_BODY_R 0
#define DEF_BODY_G 0
#define DEF_BODY_B 0
#define DEF_BODY_A 140

// Title inset box: outer border then inner fill
#define DEF_TITLE_INSET_FILL_R 0
#define DEF_TITLE_INSET_FILL_G 0
#define DEF_TITLE_INSET_FILL_B 0
#define DEF_TITLE_INSET_FILL_A 255
#define DEF_TITLE_TEXT_R 255
#define DEF_TITLE_TEXT_G 255
#define DEF_TITLE_TEXT_B 255
#define DEF_TITLE_TEXT_A 255

// Text colors converted from PSX 128-based scale to 0-255 using (X / 128 * 255).
#define DEF_TEXT_NORM_R 215
#define DEF_TEXT_NORM_G 135
#define DEF_TEXT_NORM_B 0
#define DEF_TEXT_LOCK_R 163
#define DEF_TEXT_LOCK_G 116
#define DEF_TEXT_LOCK_B 40
#define DEF_TEXT_Y_OFF (-2)

// Info entry text (confirmation/detail lines)
#define DEF_INFO_TEXT_R 215
#define DEF_INFO_TEXT_G 135
#define DEF_INFO_TEXT_B 0
#define DEF_INFO_TEXT_A 255

// Help bar text (dark, near-black)
#define DEF_HELP_TEXT_R 0
#define DEF_HELP_TEXT_G 0
#define DEF_HELP_TEXT_B 0
#define DEF_HELP_TEXT_A 255

// Slider colors
#define DEF_SLIDER_FRAME_R 255
#define DEF_SLIDER_FRAME_G 0
#define DEF_SLIDER_FRAME_B 0
#define DEF_SLIDER_FRAME_A 255

#define DEF_SLIDER_TRACK_R 20
#define DEF_SLIDER_TRACK_G 20
#define DEF_SLIDER_TRACK_B 20
#define DEF_SLIDER_TRACK_A 220

#define DEF_SLIDER_FILL_R 215
#define DEF_SLIDER_FILL_G 135
#define DEF_SLIDER_FILL_B 0
#define DEF_SLIDER_FILL_A 255

// Custom slider circles (custom FE menu path)
#define DEF_SLIDER_CIRCLE_SEGMENTS 6
#define DEF_SLIDER_CIRCLE_STEP 24.0f

#define DEF_QUIT_TIMER_SEC 1.0f

#define DEF_SCROLLBAR_FILL_R 215
#define DEF_SCROLLBAR_FILL_G 135
#define DEF_SCROLLBAR_FILL_B 0
#define DEF_SCROLLBAR_FILL_A 255

struct Entry {
    char token[MAX_CUSTOM_MENU_TOKEN + 1];
    EntryType type;
    EntryEvent event;
    MenuPage goPage;
    EntryBinding binding;
    s32 step;
    s32 lo;
    s32 hi;
    s32 posX;
    s32 posY;
};

struct PageDef {
    char titleToken[MAX_CUSTOM_MENU_TOKEN + 1];
    char overlayName[24];
    MenuPage parentPage;
    s32 parentEntry;
    bool isPause;
    bool autoFrameH;
    s32 frameW;
    s32 frameH;
    s32 entriesOffsetX;
    s32 entriesOffsetY;
    s32 numEntries;
    Entry entries[MAX_ENTRIES_PER_MENU];

    PageDef() {
        memset(titleToken, 0, sizeof(titleToken));
        memset(overlayName, 0, sizeof(overlayName));
        parentPage = MenuPage_None;
        parentEntry = 0;
        isPause = false;
        autoFrameH = false;
        frameW = 260;
        frameH = 150;
        entriesOffsetX = 0;
        entriesOffsetY = 0;
        numEntries = 0;
        memset(entries, 0, sizeof(entries));
    }
};

struct TitleDebrisParticle {
    f32 x = 0.0f, y = 0.0f;
    f32 vx = 0.0f, vy = 0.0f;
    f32 size = 4.0f;
    f32 life = 0.0f, maxLife = 1.0f;
    f32 shapeSeed = 0.0f; // randomizes the dot shader's edge wobble per particle
    u8 r = 255, g = 200, b = 60;
};

#if defined(RC_PLATFORM_PSP)
// Normalized UV rect of a texture's non-transparent texels (plus a one-texel
// bilinear margin). Defaults to the whole texture.
struct TitleLayerBounds {
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
};
#endif

class feCustomMenuMgr {
public:
    void Init(CustomText* textSystem);
    void Shutdown();

    s32 Invoke(); // 1 = stay in menu, 4 = state change, 8 = resume play
    void Render();
    bool DrawTitleScreen();
    bool DrawLoadingScreen(u8 fill, u8 pulseR8);
    void DrawTitleStartPrompt(s32 baseX, s32 baseY);

    void ResetTitleIntro();
    void HideTitleContent();
    void BeginTitleStartTransition();
    void EndTitleStartTransition();
    bool IsTitleStartTransitionRunning() const { return m_titleTransitionActive; }
    bool IsTitleStartTransitionFinished() const { return m_titleTransitionFinished; }
    bool DrawGameOverScreen();
    void DrawGameOverContinuePrompt(s32 baseX, s32 baseY, u8 r, u8 g, u8 b, u8 a);
    void DrawVersionOverlay();
    void DrawLegalScreen(f32 alpha01);
    bool GetSplashScreenRect(f32* outX, f32* outY, f32* outW, f32* outH) const;

    void Activate(MenuPage startPage = MenuPage_Frontend);
    void Deactivate();

    // Fade-popup overlay (AutosaveNotice/CheckingUpdate/AssetScanning/AssetExtracting):
    // drawn on top of whatever page (or nothing) is active, never via SetPage().
    void OpenPopup(PopupKind kind, f32 minTimerSec, std::function<s32()> poll);
    bool IsPopupActive() const { return m_activePopup != PopupKind_None; }
    PopupKind GetActivePopup() const { return m_activePopup; }
    bool IsActive() const { return m_active; }
    MenuPage GetCurrentPage() const { return m_currPage; }

    void RenderAutosaveSpinner(s32 centerX, s32 centerY, f32 alpha01 = 1.0f) const;

    // Called by settings.cpp when the controller-prompt-style setting changes,
    // so the Controller page's diagram and any cached gamepad button-prompt
    // sheet reload from the new style's textures.
    void ReloadControllerPromptTextures();

private:
    void BuildPages();
    PageDef& AddPage(MenuPage id, const char* title,
                     const char* overlay, MenuPage parent, s32 parentEntry, bool pause,
                     s32 frameW = 260, s32 frameH = 150);
    PageDef& AddPopup(PopupKind id, const char* title, const char* overlay,
                      s32 frameW, s32 frameH = -1);
    void BuildPopups();
    static s32 CalcAutoFrameHeight(s32 numEntries, s32 extraH = 0);
    s32 GetEntryExtraHeight(const PageDef& page, const Entry& entry) const;
    s32 GetWrappedLineCount(const PageDef& page, const char* label,
                            const char* fontName = nullptr, f32 scale = -1.0f) const;
    s32 CalcEntryYExtra(const PageDef& page, s32 upToIndex) const;
    s32 CalcPageExtraHeight(const PageDef& page) const;
    void ResolveEntryLayout(const PageDef& page, s32 entryIndex,
                            s32 firstY, s32 baseLabelX, s32 baseValueX, s32 baseCenterX,
                            s32* outRowTop, s32* outRowTextY,
                            s32* outLabelX, s32* outValueX, s32* outCenterX) const;
    void SetEntries(PageDef& page, std::initializer_list<Entry> list,
                    s32 entriesOffsetX = 0, s32 entriesOffsetY = 0);
    static Entry Button(const char* tok, EntryEvent ev, MenuPage go = MenuPage::MenuPage_None);
    static Entry Slider(const char* tok, EntryBinding binding, s32 step, s32 lo, s32 hi);
    Entry List(const char* tok, EntryBinding binding, s32 step, s32 lo, s32 hi);
    static Entry Toggle(const char* tok, EntryBinding binding);
    static Entry Info(const char* tok);

    void SetPage(MenuPage page);
    void MoveCursor(s32 dir);
    void Confirm();
    void GoBack();
    void Adjust(s32 dir);
    void UpdateMouseCursorVisibility(); // mouse-move/click reactivation + keyboard/gamepad hide, shared by every page including non-interactive popups
    s32 GetBoundValue(const Entry& e) const;
    void ApplyValue(const Entry& e, s32 v);
    void PlayValueChangeFeedback(const Entry& entry);
    void PlaySound(s32 id) const;
    void RefreshSaveSlots();
    void ClampSaveSlotScroll();
    void RenderSaveSlotsPage(s32 panelX, s32 panelY, s32 panelW, s32 panelH,
                             const xcColour1555& selectedColor) const;

#if AUTO_UPDATER
    void RefreshUpdatePageEntries();
    void RenderUpdateProgressBar(s32 panelX, s32 panelW, s32 rowTop) const;
    void RenderUpdateIndeterminateBar(s32 panelX, s32 panelW, s32 rowTop, f32 alpha01 = 1.0f) const;
    bool BuildUpdateInfoText(const char* token, char* outText, s32 outTextLen) const;
    void RebuildChangelogLines();
    void RenderChangelogBody(s32 panelX, s32 panelY, s32 panelW, s32 panelH, s32 contentTop) const;
#endif

    void RefreshAssetPageEntries();
    void RenderAssetExtractProgressBar(s32 panelX, s32 panelW, s32 rowTop, f32 alpha01 = 1.0f) const;
    void RenderAssetScanSweep(s32 panelX, s32 panelW, s32 rowTop, f32 alpha01 = 1.0f) const;
    bool BuildAssetInfoText(const char* token, char* outText, s32 outTextLen) const;

    // Fade in/out for the no-input popup overlays (AutosaveNotice, CheckingUpdate,
    // AssetScanning, AssetExtracting) instead of an instant cut on open/close.
    void ClosePopup(s32 closeResult);
    f32 GetPopupFadeAlpha() const;
    void RenderCurrentPage();
    void RenderActivePopup();
    void RenderActivePopupContent(s32 panelX, s32 panelW, s32 firstY, f32 popupAlpha) const;

    void LoadControllerOverlayTexture();
#if defined(RC_PLATFORM_PSP)
    void ReleaseControllerOverlayTexture();
#endif
    void LoadMenuOrnamentTexture();
    void LoadSplashTextures();
    bool EnsureTitleScreenEffects(f32 drawW, f32 drawH);
    void ReleaseTitleScreenEffects();
    void UpdateTitleDebrisParticles(f32 dt, f32 emitX, f32 emitY, f32 spanW);
    void DrawTitleDebrisParticles() const;
    void LoadSliderTextures();

    // Center of the gold debris burst within the splash rect (0..1, UV-style).
    static constexpr f32 kTitleDebrisCenterU = 0.5f;
    static constexpr f32 kTitleDebrisCenterV = 0.5f;

    // Extra focused god-ray light source placed at Jackie's fist (0..1,
    // UV-style within the splash rect). Tune these if it doesn't line up.
    static constexpr f32 kTitleFistLightU = 0.62f;
    static constexpr f32 kTitleFistLightV = 0.18f;
    static constexpr f32 kTitleFistLightExposure = 3.0f;

    // frontend
#if defined(RC_PLATFORM_PSP)
    // PSP menu icons are packed at their on-screen size (package_psp.sh) so the
    // whole resident menu set costs ~11 KiB instead of 128 KiB per texture.
    static constexpr const char* kMenuOrnamentTexturePath = "psp/textures/frontend/menu/menu_ornament.ptex";
    static constexpr const char* kPspControllerOverlayTexturePath = "psp/textures/frontend/menu/controller_overlay_ps.ptex";
#else
    static constexpr const char* kMenuOrnamentTexturePath = "pc/textures/frontend/menu_ornament.png";
#endif
#if defined(RC_PLATFORM_PSP)
    // PSP packages contain a pre-scaled 480x272 frontend set.  Loading the
    // desktop 1920x1080 PNGs would need several megabytes of transient RGBA
    // memory before the GU texture is even created.
    static constexpr const char* kTitleScreenBackgroundTexturePath = "psp/textures/frontend/background.ptex";
    static constexpr const char* kTitleScreenJackieTexturePath = "psp/textures/frontend/jackie.ptex";
    static constexpr const char* kTitleScreenLogoTexturePath = "psp/textures/frontend/jcslogo.ptex";
    // PSP-only "PSP EDITION" badge, authored at 4x and packed to 16-bit .ptex.
    static constexpr const char* kTitleScreenEditionTexturePath = "psp/textures/frontend/psp_edition.ptex";
#else
    static constexpr const char* kTitleScreenBackgroundTexturePath = "pc/textures/frontend/background.png";
    static constexpr const char* kTitleScreenJackieTexturePath = "pc/textures/frontend/jackie.png";
    static constexpr const char* kTitleScreenLogoTexturePath = "pc/textures/frontend/jcslogo.png";
#endif
#if defined(RC_PLATFORM_PSP)
    static constexpr const char* kGameOverTexturePath = "psp/textures/frontend/game_over.ptex";
    static constexpr const char* kLoadingBarTexturePath = "psp/textures/frontend/loading_bar.ptex";
#else
    static constexpr const char* kGameOverTexturePath = "pc/textures/frontend/game_over.png";
    static constexpr const char* kLoadingBarTexturePath = "pc/textures/frontend/loading_bar.png";
#endif
#if defined(RC_PLATFORM_PSP)
    static constexpr const char* kSliderOTexturePath = "psp/textures/frontend/menu/slider_o.ptex";
    static constexpr const char* kSliderFTexturePath = "psp/textures/frontend/menu/slider_f.ptex";
#else
    static constexpr const char* kSliderOTexturePath = "pc/textures/frontend/slider_o.png";
    static constexpr const char* kSliderFTexturePath = "pc/textures/frontend/slider_f.png";
#endif

    // collectibles
#if defined(RC_PLATFORM_PSP)
    static constexpr const char* kRedDragonTexturePath = "psp/textures/frontend/menu/red_dragon.ptex";
    static constexpr const char* kGoldDragonTexturePath = "psp/textures/frontend/menu/gold_dragon.ptex";
    static constexpr const char* kGreyDragonTexturePath = "psp/textures/frontend/menu/grey_dragon.ptex";
#else
    static constexpr const char* kRedDragonTexturePath = "pc/textures/collectibles/red_dragon.png";
    static constexpr const char* kGoldDragonTexturePath = "pc/textures/collectibles/gold_dragon.png";
    static constexpr const char* kGreyDragonTexturePath = "pc/textures/collectibles/grey_dragon.png";
#endif
    static constexpr const char* kBowlTexturePath = "pc/textures/collectibles/bowl.png";
    static constexpr const char* kMilkTexturePath = "pc/textures/collectibles/milk.png";
    static constexpr const char* kNoodleBoxTexturePath = "pc/textures/collectibles/noodlebox.png";
#if defined(RC_PLATFORM_PSP)
    static constexpr const char* kTakeTexturePath = "psp/textures/frontend/menu/take.ptex";
#else
    static constexpr const char* kTakeTexturePath = "pc/textures/collectibles/take.png";
#endif

    static constexpr f32 kSplashScreenAspect = 16.0f / 9.0f;

    void DrawMenuWindow(s32 x, s32 y, s32 w, s32 h, const char* title, f32 alpha01 = 1.0f) const;
    void DrawPopupWindow(s32 x, s32 y, s32 w, s32 h, const char* title, f32 alpha01 = 1.0f) const;

    void DrawRect(f32 x, f32 y, f32 w, f32 h, u8 r, u8 g, u8 b, u8 a) const;
    void DrawHighlight(f32 x, f32 y, f32 w, f32 h) const;
    void DrawScrollBar(f32 x, f32 y, f32 h, s32 totalItems, s32 visibleItems, f32 scrollTop) const;
    bool HandleScrollBarMouse(f32 x, f32 y, f32 h, s32 totalItems, s32 visibleItems,
                              s32* scrollTop, f32* visualScrollTop, f32 mouseX, f32 mouseY,
                              bool leftPressed, bool leftDown);

    const char* Localize(const char* token) const;

    void RenderControllerOverlay(s32 panelX, s32 panelY) const;
    void RenderKeyBindingsPage(s32 panelX, s32 panelY, s32 panelW, s32 panelH,
                               const xcColour1555& normalColor,
                               const xcColour1555& selectedColor) const;
#if MOD_LOADER
    void RenderModsPage(s32 panelX, s32 panelY, s32 panelW, s32 panelH,
                        const xcColour1555& normalColor,
                        const xcColour1555& selectedColor) const;
    void ClampModsScroll();
    bool ToggleSelectedMod();
#endif
    void RenderLocationPage() const;
    bool InvokeLocationSelection();

    CustomText* m_text = nullptr;
    PageDef m_pages[MenuPage_Count];
    PageDef m_popups[PopupKind_Count];
    tTexture* m_titleScreenBackgroundTexture = nullptr;
    tTexture* m_titleScreenJackieTexture = nullptr;
    tTexture* m_titleScreenLogoTexture = nullptr;
#if defined(RC_PLATFORM_PSP)
    tTexture* m_titleScreenEditionTexture = nullptr;
    // Opaque UV rect of the full-screen Jackie/logo layers (~18% of the
    // screen each). Drawing only this rect skips ~80% of their RAM-sourced
    // texture fetch and blend fill on the GE without changing the output.
    TitleLayerBounds m_titleJackieBounds;
    TitleLayerBounds m_titleLogoBounds;
    // Soft white disc tinted per spark: one batched quad per layer instead of
    // two unbatched 8-segment triangle fans (and 64 sin/cos) per particle.
    tTexture* m_titleDebrisDotTexture = nullptr;
    void LoadTitleDebrisDotTexture();
#endif
    pddiBaseShader* m_titleScreenGlowShader = nullptr;
    pddiBaseShader* m_titleScreenGodRaysShader = nullptr;
    pddiBaseShader* m_titleScreenCompositeShader = nullptr;
    pddiBaseShader* m_titleScreenLogoTiltShader = nullptr;
    pddiBaseShader* m_titleDebrisDotShader = nullptr;
    pddiRenderTarget* m_titleScreenLogoSeed = nullptr;
    pddiRenderTarget* m_titleScreenGlow = nullptr;
    pddiRenderTarget* m_titleScreenGodRays = nullptr;
    f32 m_titleScreenAnimSec = 0.0f;
    s32 m_titleEffectTargetW = 0;
    s32 m_titleEffectTargetH = 0;

    f32 m_titleEffectsLastUpdateSec = -1.0f;
    static constexpr f32 kTitleEffectsUpdateHz = 30.0f;

    // Intro zoom: logo starts huge (near camera) and eases down to idle scale.
    f32 m_titleIntroSec = 0.0f;
    static constexpr f32 kTitleIntroDuration = 0.9f;

    // Once the player confirms Continue/New Game from the title menu, the
    // logo/Jackie/effects stay hidden (background only) through the
    // subsequent fade into the game instead of popping back in.
    bool m_titleContentHidden = false;

    // Press-start transition: logo/Jackie fade out and the prompt zooms in
    // briefly before the custom menu is actually activated, so the swap to
    // the menu isn't an instant single-frame cut.
    bool m_titleTransitionActive = false;
    bool m_titleTransitionFinished = false;
    f32 m_titleTransitionSec = 0.0f;
    static constexpr f32 kTitleStartTransitionDuration = 0.5f;
    static constexpr s32 kMaxTitleDebrisParticles = 32;
    TitleDebrisParticle m_titleDebrisParticles[kMaxTitleDebrisParticles];
    f32 m_titleDebrisSpawnAccum = 0.0f;
    bool m_titleScreenEffectsUnavailable = false;
    tTexture* m_gameOverTexture = nullptr;
    tTexture* m_loadingBarTexture = nullptr;
    mutable tTexture* m_controllerTexture = nullptr;
    tTexture* m_menuOrnamentTexture = nullptr;
    mutable tTexture* m_redDragonTex = nullptr;
    mutable tTexture* m_goldDragonTex = nullptr;
    mutable tTexture* m_greyDragonTex = nullptr;
    mutable tTexture* m_takeTex = nullptr;
    tTexture* m_sliderOTex = nullptr;
    tTexture* m_sliderFTex = nullptr;
    bool m_titleScreenTextureTried = false;
    bool m_gameOverTextureTried = false;
    bool m_loadingScreenTexturesTried = false;
    MenuPage m_currPage = MenuPage_None;
    MenuPage m_prevPage = MenuPage_None;
    s32 m_cursor = 0;
    s32 m_result = 1;
    ColorPulse m_pulse{ xcColour1555(132, 0, 0), xcColour1555(224, 146, 0), 0.5f };
    // Resolution selection is staged while focused and committed on confirm.
    bool m_pendingResolutionActive = false;
    s32 m_pendingResolutionIndex = 0;

    // Screen mode selection is staged while focused and committed on confirm.
    bool m_pendingScreenModeActive = false;
    s32 m_pendingScreenMode = 0;

    // MSAA selection is staged while focused and committed on confirm.
    bool m_pendingMsaaActive = false;
    s32 m_pendingMsaaIndex = 0;

    // Input mode tracking: when false, mouse hover is ignored until mouse moves/clicks.
    bool m_mouseInputActive = true;
    bool m_mousePosInitialized = false;
    double m_lastMouseX = 0.0;
    double m_lastMouseY = 0.0;
    bool m_scrollBarDragging = false;
    f32 m_scrollBarGrabOffset = 0.0f;

    // Key bindings page selection/capture state.
    s32 m_keyBindActionCursor = 0;
    s32 m_keyBindSlotCursor = 0;
    s32 m_keyBindScrollTop = 0;
    f32 m_keyBindScrollVisual = 0.0f;
    bool m_keyBindCaptureActive = false;
    s32 m_keyBindCaptureBlockFrames = 0;

#if MOD_LOADER
    s32 m_modCursor = 0;
    s32 m_modScrollTop = 0;
    f32 m_modScrollVisual = 0.0f;
#endif

    SaveGameSlotInfo m_saveSlots[SAVEGAME_VISIBLE_SLOT_COUNT] = {};
    s32 m_saveSlotScrollTop = 0;
    f32 m_saveSlotScrollVisual = 0.0f;
    s32 m_pendingLoadSlot = -1;
    s32 m_pendingSaveSlot = -1;
    s32 m_pendingDeleteSlot = -1;

    bool m_active = 0;
    f32 m_quitTimerSec = 0.0f; // seconds remaining before game actually closes

    // Popup overlay (AutosaveNotice/CheckingUpdate/AssetScanning/AssetExtracting) state.
    PopupKind m_activePopup = PopupKind_None;
    f32 m_popupFadeSec = 0.0f;
    bool m_popupClosing = false;
    f32 m_popupMinTimer = 0.0f;
    // Polled once m_popupMinTimer elapses: returns -1 while still waiting, else
    // the m_result value Invoke() should resolve to once the close-fade ends.
    std::function<s32()> m_popupPoll;
    s32 m_popupCloseResult = 1;

#if AUTO_UPDATER
    AutoUpdater::State m_lastUpdateState = AutoUpdater::State::Idle;
    std::vector<std::string> m_changelogLines;
    s32 m_changelogScrollTop = 0;
    f32 m_changelogScrollVisual = 0.0f;
    f32 m_updateAvailFadeTimer = 0.0f; // drives the "Update available" fade in/out
#endif
};

extern feCustomMenuMgr* g_feCustomMenuMgr;

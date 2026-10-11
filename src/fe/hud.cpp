#include "fe/hud.h"
#include "gen/common.h"
#include "gen/game.h"
#include "gen/world.h"
#include "gen/scoremgr.h"
#include "gen/time.h"
#if CUSTOM_MENU
#include "extra/customtext.h"
#include "extra/customhudmgr.h"
#endif
#include "xclib/xclib.h"
#include "ai/humanoid.h"
#include "ai/player.h"
#include "gen/ai.h"
#include "pc/inputaction.h"
#include "p3d/context.h"
#include "pddi/pddidev.h"
#if defined(RC_PLATFORM_PSP)
#include "psp/psp_perf.h"
#endif

#include <cctype>

HUD* g_hud = nullptr;
char HUD::szBossStatic[32] = {};

// PSX: gp+1084 - screen names table
static const char* s_hudScreenNames[] = { "HUD", nullptr };

static void SetAllTextPrimStrings(xcTextPrim* text, u32 stringToken) {
    if (!text || text->numStrings == 0) {
        return;
    }

    u32* hashes = text->StringHashes();
    for (u8 i = 0; i < text->numStrings; i++) {
        hashes[i] = stringToken;
    }
}

static s32 ScoreHubPromptTextPrim(const xcTextPrim* text) {
    if (!text) {
        return -1;
    }

    return ((s32)text->colorA << 16) + ((s32)text->colorR << 8) + (s32)text->colorG + (s32)text->colorB;
}

static bool IsValidHudAsciiName(const char* text) {
    if (!text || text[0] == '\0') {
        return false;
    }

    for (u32 i = 0; i < sizeof(HUD::szBossStatic) - 1; i++) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == 0) {
            return true;
        }
        if (ch < 32 || ch > 126) {
            return false;
        }
    }

    return false;
}

static void CopyUpperHudName(char* dst, u32 dstSize, const char* src) {
    if (!dst || dstSize == 0) {
        return;
    }

    dst[0] = 0;
    if (!src) {
        return;
    }

    const u32 maxCopy = dstSize - 1;
    u32 i = 0;
    for (; i < maxCopy && src[i] != 0; i++) {
        const unsigned char ch = static_cast<unsigned char>(src[i]);
        dst[i] = static_cast<char>(std::toupper(ch));
    }
    dst[i] = 0;
}

// PSX: _3HUD (HUD.CPP:318, 0x8003F44C)
HUD::HUD() {
    MARKFUNCTION(0x8003F44C);
    currentFoe = nullptr;
    bossHandle = nullptr;
    visible = 1;
    dragonShowState = 0;
    foeTracking = 0;
}

// PSX: __3HUD (HUD.CPP:331, 0x8003F53C)
HUD::~HUD() {
    MARKFUNCTION(0x8003F53C);
#if CUSTOM_MENU
    g_customHudMgr.Shutdown();
#endif
}

// PSX: InternalReset__3HUD (HUD.CPP:350, 0x8003F650)
void HUD::InternalReset() {
    MARKFUNCTION(0x8003F650);
}

// PSX: Display__3HUD (HUD.CPP:357, 0x8003F67C)
void HUD::Display() {
    MARKFUNCTION(0x8003F67C);
#if defined(RC_PLATFORM_PSP)
    PspPerf::ScopeTimer hudTimer(PspPerf::RendererCounter::HudUs);
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: hud begin");
#endif
    // PSX: EnterLayer(view0, 4)
#if HIGH_FPS_PLAY_PRESENTATION
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: hud step flag begin");
#endif
    const bool isPlayRenderOnlyFrame =
        (g_game && g_game->GetState() == GameState::Play && g_time && !g_time->DidPlayLogicStepThisFrame());
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: hud step flag done");
#endif
    if (!isPlayRenderOnlyFrame) {
        if (tally.state == 9) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            PspPerf::Trace("render: hud game data begin");
#endif
            GetGameData();
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            PspPerf::Trace("render: hud game data done");
#endif
        }
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("render: hud update begin");
#endif
        Update();
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("render: hud update done");
#endif
    }
#else
    if (tally.state == 9) {
        GetGameData();
    }
    Update();
#endif
    // Custom HUD rendering path replaces FE section overlays while preserving
    // all original HUD state/update behavior.
// The PSP offers the original section HUD as the Classic HUD style.
#if CUSTOM_MENU
    if (IsCustomHudActive()) {
    #if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("render: hud custom begin");
    #endif
        g_customHudMgr.Render(*this);
    #if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("render: hud custom done");
    #endif
    }
    else
#endif
    {
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("render: hud section begin");
#endif
        if (section) {
#if FIX_ASPECT_RATIO
            widescreenPatches.DrawSection(section);
#else
            section->Draw();
#endif
        }
#if CUSTOM_MENU
        g_customHudMgr.RenderClassicOverlays();
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("render: hud section done");
#endif
    }
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: hud done");
#endif
    // PSX: ExitLayer(view0, 4)
}

// PSX: SetHUDVisible__3HUDii (HUD.CPP:367, 0x8003F6F8)
void HUD::SetHUDVisible(s32 vis, s32 arg3) {
    MARKFUNCTION(0x8003F6F8);
    visible = vis;
    EnableInput(vis);
    if (visible) {
        World* world = g_game ? g_game->GetWorld() : nullptr;
        if (world && world->GetCurLevelID() == 7) {
            return;
        }
        playerHealth.SetVisible(1);
        hits.SetVisible(1);
        tally.Show(0);
        destSelect.Hide();
        takes.SetPauseState(dragonShowState, 1);
        dragon.SetPauseState(dragonShowState, 1);
        if (dragonShowState) {
            takes.Play();
            dragon.Play();
        }
    } else {
        playerHealth.SetVisible(0);
        bossHealth.SetVisible(0);
        foeHealth.SetVisible(0);
        hits.SetVisible(0);
        tally.Show(0);
        hits.hitCount = 0;
        destSelect.Hide();
        takes.SetPauseState(0, 1);
        dragon.SetPauseState(0, arg3);
    }
    if (g_scoreManager) {
        dragon.SetNum(g_scoreManager->currentCollectCount);
        dragon.SetGoldDragons(g_scoreManager->currentGoldDragons);
    }
    if (Player::s_player) {
        takes.SetNumber(Player::s_player->livesLeft - 1);
    }
}

// PSX: DisplayTally__3HUDi (HUD.CPP:413, 0x8003F8A4)
void HUD::DisplayTally(s32 tallyType) {
    MARKFUNCTION(0x8003F8A4);
    SetHUDVisible(0, 1);
    tally.Start(tallyType);
}

// PSX: ShowDestLevel__3HUD (HUD.CPP:418, 0x8003F8D0)
void HUD::ShowDestLevel() {
    MARKFUNCTION(0x8003F8D0);
    SetHUDVisible(0, 1);
    s32 totalGold = 0;
    if (g_scoreManager) {
        totalGold = g_scoreManager->GetTotalGoldDragon();
    }
    destSelect.Start(totalGold);
}

// PSX: SelfInit__3HUD (HUD.CPP:431, 0x8003F918)
void HUD::SelfInit() {
    MARKFUNCTION(0x8003F918);
    currentFoe = nullptr;
    bossHandle = nullptr;
    foeTracking = 0;
    EnableInput(1);
    playerHealth.SetMax(205);
    DebugDisplay(0);

    u8* raw = section ? section->rawData : nullptr;
    xcOverlayData* playerHealthOvl = FindOverlay((u32)0xD6549407);
    xcOverlayData* bossHealthOvl = FindOverlay((u32)0xA7160677);
    xcOverlayData* foeHealthOvl = FindOverlay((u32)0x8A0C569E);
    xcOverlayData* takesOvl = FindOverlay((u32)0x0A88DB30);
    xcOverlayData* dragonOvl = FindOverlay((u32)0xC98817DB);

#if FIX_ASPECT_RATIO
    widescreenPatches.Initialize(section, raw);
#endif

    // playerHealth (+120)
    playerHealth.rawData = raw;
    playerHealth.Init(playerHealthOvl);

    // bossHealth (+156)
    bossHealth.rawData = raw;
    bossHealth.Init(bossHealthOvl);
    bossHealth.SetVisible(0);
    if (bossHealth.textObj) {
        LOG("[HUD] bossHealth init: ovl=%p bar=%p width=%d text=%p numStrings=%u paletteIdx=%u font=0x%08X",
            bossHealthOvl,
            bossHealth.healthBar,
            (s32)bossHealth.barWidth,
            bossHealth.textObj,
            (u32)bossHealth.textObj->numStrings,
            (u32)bossHealth.textObj->paletteIdx,
            bossHealth.textObj->fontHash);
    }
    else {
        LOG("[HUD] bossHealth init: ovl=%p bar=%p width=%d text=null",
            bossHealthOvl,
            bossHealth.healthBar,
            (s32)bossHealth.barWidth);
    }

    // foeHealth (+192)
    foeHealth.rawData = raw;
    foeHealth.Init(foeHealthOvl);
    foeHealth.SetVisible(0);

    // takes (+292)
    takes.rawData = raw;
    takes.Init(takesOvl);
    takes.SetAnimInfo(-150, 0, 15, 15);
    takes.SetVisible(1);

    // dragon (+420)
    dragon.rawData = raw;
    dragon.Init(dragonOvl);
    dragon.SetAnimInfo(0, 60, 15, 15);
    dragon.SetVisible(1);

    // hdHits (+228)
    hits.Init(this);
    hits.hitCount = 0;

    // destSelect (+560)
    destSelect.Init(this);

    // tally (+584)
    tally.Init(this);
    tally.takesOvlPtr = &takes;
}

// PSX: EnableInput__3HUDi (HUD.CPP:542, 0x8003FB84)
void HUD::EnableInput(s32 enable) {
    MARKFUNCTION(0x8003FB84);
    inputEnabled = enable;
}

// PSX: DebugDisplay__3HUDi (HUD.CPP:553, 0x8003FBD0)
void HUD::DebugDisplay(s32 flag) {
    MARKFUNCTION(0x8003FBD0);
    if (flag) {
        return;
    }

    u8* raw = section ? section->rawData : nullptr;
    if (!raw) {
        return;
    }

    xcOverlayData* ovl = FindOverlay((u32)0x050794E7);
    if (!ovl) {
        return;
    }

    static u32 emptyToken = xcRegisterRuntimeString("");
    const xcOverlayItem* items = ovl->GetItems();
    for (u32 i = 0; i < ovl->primCount; i++) {
        u8* primData = raw + items[i].dataOffset;
        xcPrimHeader* hdr = reinterpret_cast<xcPrimHeader*>(primData);
        if (hdr->subtype == 5 || hdr->type != XC_PRIM_TEXT) {
            continue;
        }

        xcTextPrim* text = reinterpret_cast<xcTextPrim*>(primData);
        SetAllTextPrimStrings(text, emptyToken);
    }
}

// PSX: SelfUpdate__3HUD (HUD.CPP:611, 0x8003FC10)
void HUD::SelfUpdate() {
    MARKFUNCTION(0x8003FC10);
    u8* raw = section ? section->rawData : nullptr;
#if FIX_ASPECT_RATIO
    if (raw) {
        widescreenPatches.BeginUpdate(section, raw);
    }
#endif

    // PSX uses InputManager button callbacks for logical button 0 (L2 / Status Display).
    // Until that callback layer is reversed on PC, mirror the same trigger here.
    if (inputEnabled && g_inputManager) {
        World* world = g_game ? g_game->GetWorld() : nullptr;
        if (world && world->GetCurLevelID() != 7) {
            Button* statusButton = g_inputManager->GetButtonForBit(0, 0);
            if (statusButton && statusButton->rawInput && !statusButton->prevInput) {
                ToggleShowAll();
            }
        }
    }

    hits.Update();
    foeHealth.Update();
    playerHealth.Update();
    bossHealth.Update();
    takes.Update();
    dragon.Update();
    tally.Update();
    destSelect.Update();

    if (!foeHealth.IsVisible()) {
        hits.TriggerUpdate();
    }

#if FIX_ASPECT_RATIO
    if (raw) {
        widescreenPatches.EndUpdate(section, raw);
    }
#endif
}

// PSX: FindScreen__3HUDUl (HUD.CPP:633, 0x8003FCA0)
uintptr_t HUD::FindScreen(u32 id) {
    MARKFUNCTION(0x8003FCA0);
    if (id) {
        // PSX: returns this+48 (embedded oxScreen)
        return id;
    }
    return 0;
}

// PSX: GetScreenNames__3HUD (HUD.CPP:640, 0x8003FCB4)
const char** HUD::GetScreenNames() {
    MARKFUNCTION(0x8003FCB4);
    return s_hudScreenNames;
}

// PSX: GetGameData__3HUD (HUD.CPP:645, 0x8003FCC0)
void HUD::GetGameData() {
    MARKFUNCTION(0x8003FCC0);
    s32 foeHP = -1;
    if (foeTracking && currentFoe) {
        foeHP = currentFoe->health;
    }
    s32 playerHP = 0;
    if (Player::s_player) {
        playerHP = Player::s_player->health;
    }
    UpdateHealth(playerHP, foeHP);
    foeTracking = 0;

    if (!g_scoreManager) {
        return;
    }

    s32 changed = 0;
    if (dragon.dragonCount != g_scoreManager->currentCollectCount ||
        dragon.goldDragonFlag != g_scoreManager->currentGoldDragons) {
        changed = 1;
    }
    if (changed) {
        dragon.SetNum(g_scoreManager->currentCollectCount);
        dragon.SetGoldDragons(g_scoreManager->currentGoldDragons);
        dragon.Play();
    }
}

// PSX: UpdateBonusScore__3HUDllRC10tagLVector (HUD.CPP:690, 0x8003FDF8)
void HUD::UpdateBonusScore() {
    MARKFUNCTION(0x8003FDF8);
    hits.IncrementHits();
}

// PSX: TriggerBonusUpdate__3HUD (HUD.CPP:695, 0x8003FE18)
void HUD::TriggerBonusUpdate() {
    MARKFUNCTION(0x8003FE18);
    hits.TriggerUpdate();
}

// PSX: DisplayTake__3HUDib (HUD.CPP:700, 0x8003FE38)
void HUD::DisplayTake(s32 takeCount, s32 showAnim) {
    MARKFUNCTION(0x8003FE38);
    takes.SetNumber(takeCount - 1);
    takes.SetPauseState(showAnim, 1);
    takes.Play();
}

// PSX: DisplayExtraTake__3HUDRC10tagLVector (HUD.CPP:708, 0x8003FE7C)
void HUD::DisplayExtraTake() {
    MARKFUNCTION(0x8003FE7C);
    if (Player::s_player) {
        takes.SetNumber(Player::s_player->livesLeft - 1);
    }
    takes.Play();
}

// PSX: UpdateHealth__3HUDll (HUD.CPP:719, 0x8003FEB8)
void HUD::UpdateHealth(s32 playerHP, s32 foeHP) {
    MARKFUNCTION(0x8003FEB8);
    // Player health bar
    s32 pval = 0;
    if (playerHP) {
        pval = playerHP + 5;
    }
    playerHealth.SetValue(pval);

    // Foe health bar (bonus from max health scaling)
    u32 foeBonus = 0;
    if (foeTracking && currentFoe) {
        foeBonus = (u16)currentFoe->maxHealth / 40;
    }
    if (foeHP >= 0) {
        s32 fval;
        if (foeHP) {
            fval = foeHP + foeBonus;
            if (fval < 6) {
                fval = 5;
            }
        } else {
            fval = 0;
        }
        foeHealth.SetValue(fval);
        foeHealth.SetTtlive(60);
    }

    // Boss health bar
    if (bossHandle) {
        Thing* boss = bossHandle->owner;
        if (boss) {
            bossHealth.SetValue(boss->health);
        } else {
            bossHealth.SetVisible(0);
            bossHandle->refCount--;
            if (bossHandle->refCount == 0) {
                delete bossHandle;
            }
            bossHandle = nullptr;
        }
    }
}

// PSX: SetFoe__3HUDP8Humanoid (HUD.CPP:767, 0x8003FFF4)
void HUD::SetFoe(Humanoid* foe) {
    MARKFUNCTION(0x8003FFF4);
    if (!foe) {
        return;
    }
    // Check if foe is a boss type - bosses don't use the foe health bar
    s32 isBoss = 0;
    switch (foe->thingType) {
        case AITypes::TT_GRONTAR:
        case AITypes::TT_PAUL:
        case AITypes::TT_OSCAR:
        case AITypes::TT_DANTE:
        case AITypes::TT_BUTCH:
            isBoss = 1;
            break;
        default:
            isBoss = 0;
            break;
    }
    if (!isBoss) {
        foeHealth.SetText(nullptr);
        foeHealth.SetMax(foe->maxHealth);
        currentFoe = foe;
        foeTracking = 1;
    }
}

// PSX: UpdateFoe__3HUDP8Humanoid (HUD.CPP:793)
void HUD::UpdateFoe(Humanoid* foe) {
    MARKFUNCTION(0x80040064);
    if (foeHealth.IsVisible()) {
        if (foe == currentFoe) {
            foeTracking = 1;
        }
    }
}

// PSX: ShowBossHealth__3HUDPCc (HUD.CPP:820, 0x80040184)
void HUD::ShowBossHealth(const char* name) {
    MARKFUNCTION(0x80040184);
    if (bossHealth.IsVisible()) {
        return;
    }
    if (!g_ai || !name) {
        return;
    }

    u32 hash = p3dHash(name);
    Thing* boss = (Thing*)g_ai->humanoidList.FindNodeCRC(hash);
    if (!boss) {
        LOG("[HUD] ShowBossHealth: lookup failed for '%s' hash=0x%08X", name ? name : "(null)", hash);
        return;
    }

    bossHealth.SetVisible(1);
    bossHealth.SetMax(boss->maxHealth);
    bossHealth.SetValue(boss->health);

    // PSX overwrites the scripted label with Thing::GetName() if present.
    // In host builds some runtime names may not be clean text; fall back to
    // scripted name to avoid rendering garbage glyphs.
    const char* displayName = name;
    const char* bossName = boss->GetName();
    if (IsValidHudAsciiName(bossName)) {
        displayName = bossName;
    }
    if (IsValidHudAsciiName(displayName)) {
        CopyUpperHudName(szBossStatic, static_cast<u32>(sizeof(szBossStatic)), displayName);
        bossHealth.SetText(szBossStatic);
    }
    else {
        bossHealth.SetText(nullptr);
    }

    bossHandle = boss->GetThingHandle();
    bossHandle->refCount++;
}

// PSX: ToggleShowAll__3HUD (HUD.CPP:853, 0x80040150)
void HUD::ToggleShowAll() {
    MARKFUNCTION(0x80040150);
    if (!visible) {
        return;
    }
    dragonShowState = (dragonShowState == 0) ? 1 : 0;
    dragon.SetPauseState(dragonShowState, 1);
    if (g_scoreManager) {
        dragon.SetNum(g_scoreManager->currentCollectCount);
    }
    dragon.Play();
    takes.SetPauseState(dragonShowState, 1);
    if (Player::s_player) {
        takes.SetNumber(Player::s_player->livesLeft - 1);
    }
    takes.Play();
}

// PSX: OnLoadLevel__3HUD (HUD.CPP:887, 0x8004028C)
void HUD::OnLoadLevel() {
    MARKFUNCTION(0x8004028C);

#if CUSTOM_MENU
    g_customHudMgr.OnLevelLoad();
#endif

    u8* raw = section ? section->rawData : nullptr;
#if FIX_ASPECT_RATIO
    if (raw) {
        widescreenPatches.OnLevelLoad(section, raw);
    }
#endif
}

// PSX: OnUnloadLevel__3HUD (HUD.CPP:891, 0x80040294)
void HUD::OnUnloadLevel() {
    MARKFUNCTION(0x80040294);

#if CUSTOM_MENU
    g_customHudMgr.OnLevelUnload();
#endif

    u8* raw = section ? section->rawData : nullptr;
#if FIX_ASPECT_RATIO
    if (raw) {
        widescreenPatches.OnLevelUnload(raw);
    }
#endif
    if (bossHandle) {
        bossHandle->refCount--;
        if (bossHandle->refCount == 0) {
            delete bossHandle;
        }
        bossHandle = nullptr;
    }
}

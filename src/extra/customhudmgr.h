#pragma once
#include "core.h"
#include "extra/colorpulse.h"

class HUD;
class tTexture;
class Thing;

class CustomHudMgr {
public:
    CustomHudMgr();
    ~CustomHudMgr();

    void Render(const HUD& hud);
    // Port-only overlays (autosave) drawn over the classic section HUD.
    void RenderClassicOverlays();
    void Shutdown();
    void OnLevelLoad();
    void OnLevelUnload();

    void SetDebugTallyPreviewEnabled(bool enabled);
    void SetDebugTallyPreviewValues(s32 fightScore,
                                    s32 comboScore,
                                    s32 styleScore,
                                    s32 redDragons,
                                    s32 goldDragons,
                                    s32 grade,
                                    bool showMovieBonus);
    void RestartDebugTallyPreview();

private:
    tTexture* m_takeTex = nullptr;
    tTexture* m_redDragonTex = nullptr;
    tTexture* m_goldDragonTex = nullptr;
    tTexture* m_greyDragonTex = nullptr;
    tTexture* m_clockTex = nullptr;

    bool m_assetsLoadTried = false;
    bool m_fontLoadTried = false;
    bool m_levelLoaded = false;

    f32 m_playerHealthRatio = -1.0f;
    f32 m_playerHealthDamageRatio = -1.0f;
    f32 m_playerDamageHoldTimer = 0.0f;
    f32 m_playerHealthShakeTimer = 0.0f;

    // Boss health bar: stays active for the whole boss fight.
    f32 m_bossHealthRatio = -1.0f;
    f32 m_bossHealthDamageRatio = -1.0f;
    f32 m_bossDamageHoldTimer = 0.0f;
    f32 m_bossHealthShakeTimer = 0.0f;
    const Thing* m_bossAnimTarget = nullptr;

    // Foe health bar: independent of the boss bar, shown only while the
    // tracked regular enemy has been hit recently (existing behavior).
    f32 m_foeHealthRatio = -1.0f;
    f32 m_foeHealthDamageRatio = -1.0f;
    f32 m_foeDamageHoldTimer = 0.0f;
    f32 m_foeHealthShakeTimer = 0.0f;
    const Thing* m_foeAnimTarget = nullptr;

    f32 m_gameplayHudAlpha = 1.0f;

    s32 m_prevRedDragonCount = -1;
    s32 m_prevGoldDragonCount = -1;
    s32 m_prevLivesCount = -1;
    f32 m_redPulseTimer = 0.0f;
    f32 m_goldPulseTimer = 0.0f;
    f32 m_livesPulseTimer = 0.0f;

    s32 m_prevHitCount = 0;
    f32 m_hitTextShakeTimer = 0.0f;
    // Last screen position of the hit counter over the enemy being hit.
    f32 m_hitsAnchorX = 0.0f;
    f32 m_hitsAnchorY = 0.0f;
    bool m_hitsAnchorValid = false;

    f32 m_levelZoneToastTimer = -1.0f;
    f32 m_levelZoneReleaseTimer = -1.0f;

    bool m_levelZonePendingAutoShow = false;

    bool m_tallyVisible = false;
    s32 m_tallyFightDisplay = 0;
    s32 m_tallyComboDisplay = 0;
    s32 m_tallyStyleDisplay = 0;
    s32 m_tallyRDragonDisplay = 0;
    s32 m_tallyGDragonDisplay = 0;
    f32 m_tallyGradeDelayTimer = -1.0f;
    bool m_tallyGradeVisible = false;
    f32 m_tallyRDragonDelayTimer = -1.0f;
    bool m_tallyRDragonVisible = false;
    f32 m_tallyGoldBonusAnimTimer = 0.0f;
    f32 m_tallyGoldLeadInTimer = -1.0f;
    bool m_tallyGoldBonusTriggered = false;
    f32 m_tallyMovieBonusAnimTimer = 0.0f;
    bool m_tallyMovieBonusWasVisible = false;
    f32 m_tallyMovieBonusShakePhase = 0.0f;
    ColorPulse m_tallyPromptPulse{ xcColour1555(132, 0, 0), xcColour1555(224, 146, 0), 0.5f };

    bool m_debugTallyEnabled = false;
    bool m_debugTallyRestartRequested = false;
    s32 m_debugTallyFightTarget = 12000;
    s32 m_debugTallyComboTarget = 8500;
    s32 m_debugTallyStyleTarget = 6200;
    s32 m_debugTallyRDragonTarget = 18;
    s32 m_debugTallyGDragonTarget = 9;
    s32 m_debugTallyGrade = 3;
    bool m_debugTallyShowMovieBonus = false;
    s32 m_debugTallySoundStage = 0;
    s32 m_debugTallyPersistentRow = 0;

    void EnsureAssetsLoaded();
    void EnsureFontsLoaded();

    void DrawGameplayHud(const HUD& hud);
    void DrawPlayerHealthCard(const HUD& hud);
    void DrawBossHealthCard(const HUD& hud);
    void DrawFoeHealthCard(const HUD& hud);
    void DrawInventoryCard(const HUD& hud);
    void DrawHitsCard(const HUD& hud);
    void DrawDestinationBanner(const HUD& hud) const;
    void DrawHubPromptBar() const;
    void DrawTallyOverlay(const HUD& hud);
    void DrawAutosaveOverlay() const;
    void DrawLevelZoneOverlay(const HUD& hud);
};

extern CustomHudMgr g_customHudMgr;

// Gameplay HUD presentation. The PSP lets the player choose; desktop always
// draws the PC HUD.
enum HudStyle : s32 {
    HudStyle_Modern = 0,
    HudStyle_Classic,
    HudStyle_Count,
};

HudStyle GetHudStyle();
// Classic also sets the menu typeface to the original-looking AdLib when
// that font is present in the local data tree (it is never distributed).
void SetHudStyle(s32 style);
// True when the PC HUD, not the original section HUD, is on screen.
bool IsCustomHudActive();

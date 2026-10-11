#include "gen/common.h"
#include "fe/hditem.h"
#include "fe/oxscrmgr.h"
#include "xclib/xclib.h"
#include "xclib/xccolour.h"
#include "gen/control.h"
#include "gen/scoremgr.h"
#include "gen/game.h"
#include "gen/world.h"
#if CUSTOM_MENU
#include "extra/customhudmgr.h"
#include "pc/inputaction.h"
#endif
#include "ai/player.h"
#include "snd/fesnd.h"
#include "p3d/p3dmath.h"

static char s_dragonCountBuf[32];
static const char s_hdHitSingular[] = "hit";
static const char s_hdHitPlural[] = "hits";
static s32 s_hdTallyFastForward = 0;

// The PC HUD plays the tally cues itself, in step with its own count-up, so
// the original cues only sound while the classic section HUD is on screen.
static bool PlayLegacyTallySounds() {
#if CUSTOM_MENU
    return !IsCustomHudActive();
#else
    return true;
#endif
}

static void ApplyHdHitsColor(xcTextPrim* textPrim, s16 colorR, s16 colorG, s16 colorB) {
    if (!textPrim) {
        return;
    }

    xcColour1555 color;
    color.Set8((u8)colorR, (u8)colorG, (u8)colorB);
    textPrim->colorR = color.GetRed8();
    textPrim->colorG = color.GetGreen8();
    textPrim->colorB = color.GetBlue8();
    textPrim->colorA = color.GetAlpha8();
}

// PSX: _8hdTtlive (HDITEM.CPP:295, 0x8008EF44)
hdTtlive::hdTtlive() {
    MARKFUNCTION(0x8008EF44);
    timer = -1;
}

// PSX: __8hdTtlive (HDITEM.CPP, 0x8008ED30 via hdHealth pattern)
hdTtlive::~hdTtlive() {
    MARKFUNCTION(0);
}

// PSX: Update__8hdTtlive (HDITEM.CPP:283, 0x8008EF0C)
void hdTtlive::Update() {
    MARKFUNCTION(0x8008EF0C);
    s32 t = timer;
    if (t >= 0) {
        t = t - 1;
        timer = t;
        if (t == 0) {
            SetVisible(0);
        }
    }
}

// PSX: SetTtlive__8hdTtlivel (HDITEM.CPP:301, 0x8008EF80)
void hdTtlive::SetTtlive(s32 frames) {
    MARKFUNCTION(0x8008EF80);
    timer = frames;
    SetVisible(1);
}

hdHealth::hdHealth() {
    MARKFUNCTION(0x8008ED00);
}

hdHealth::~hdHealth() {
}

void hdHealth::SelfInit() {
    MARKFUNCTION(0x8008ED58);
    if (overlay && rawData) {
        healthBar = (xcPolyG4Prim*)overlay->GetPrimObj(0x282435AB, XC_PRIM_POLYG4, rawData);
        if (healthBar) {
            barWidth = healthBar->x2 - healthBar->x0;
        }
        textObj = (xcTextPrim*)overlay->GetTextObj(0x002C70A1, rawData);
        if (!textObj) {
            const xcOverlayItem* items = overlay->GetItems();
            for (u32 i = 0; i < overlay->primCount; i++) {
                u8* primData = rawData + items[i].dataOffset;
                xcPrimHeader* hdr = reinterpret_cast<xcPrimHeader*>(primData);
                if (hdr->subtype == 5 || hdr->type != XC_PRIM_TEXT) {
                    continue;
                }

                textObj = reinterpret_cast<xcTextPrim*>(primData);
                break;
            }
        }
    }
}

void hdHealth::SetMax(s32 max) {
    MARKFUNCTION(0x8008EDB0);
    maxHealth = max;
}

void hdHealth::SetValue(s32 value) {
    MARKFUNCTION(0x8008EDB8);
    if (value > maxHealth) {
        value = maxHealth;
    }
    if (value < 0) {
        value = 0;
    }

    if (healthBar && maxHealth > 0) {
        s16 newWidth = (s16)(barWidth * value / maxHealth);
        healthBar->x2 = healthBar->x0 + newWidth;
        healthBar->x3 = healthBar->x0 + newWidth;
    }

    if (value != lastValue) {
        flashAlpha = 255;
    }
    lastValue = value;
}

void hdHealth::SetText(const char* text) {
    MARKFUNCTION(0x8008EE50);
    if (textObj) {
        u32* hashes = textObj->StringHashes();
        const u8 idx = (textObj->paletteIdx < textObj->numStrings) ? textObj->paletteIdx : 0;
        if (text) {
            hashes[idx] = xcRegisterRuntimeString(text);
        }
        else {
            hashes[idx] = 0;
        }
    }
}

void hdHealth::Update() {
    MARKFUNCTION(0x8008EE98);
    hdTtlive::Update();

    if (healthBar) {
        u8 alpha = (u8)flashAlpha;
        healthBar->r0 = 255;
        healthBar->g0 = 255;
        healthBar->b0 = alpha;
        healthBar->r2 = 255;
        healthBar->g2 = 255;
        healthBar->b2 = alpha;
    }

    if (flashAlpha > 0) {
        flashAlpha -= 16;
        if (flashAlpha < 0) {
            flashAlpha = 0;
        }
    }
}

hdTextOvl::hdTextOvl() {
    MARKFUNCTION(0x8008F2A0);
    numberBuf[0] = 0;
}

hdTextOvl::~hdTextOvl() {
}

void hdTextOvl::SelfInit() {
    MARKFUNCTION(0x8008F2EC);
    if (overlay && rawData) {
        textPrim = (xcTextPrim*)overlay->GetTextObj(0x002FCD65, rawData);
        if (textPrim) {
            textPrim->StringHashes()[textPrim->paletteIdx] = xcRegisterRuntimeString(numberBuf);
        }
    }
}

void hdTextOvl::SetNumber(s32 num) {
    MARKFUNCTION(0x8008F334);
    sprintf(numberBuf, "%d", num);
}

hdAnimTextOvl::hdAnimTextOvl() {
    MARKFUNCTION(0x8008F360);
    stepX = 0;
    stepY = 0;
    animDuration = 0;
    pauseDuration = 0;
    currentFrame = 0;
    isPlaying = 0;
    pauseFlag = 0;
}

hdAnimTextOvl::~hdAnimTextOvl() {
}

void hdAnimTextOvl::SelfInit() {
    MARKFUNCTION(0x8008F3B0);
    if (overlay && rawData) {
        numPrims = (s16)overlay->primCount;
        if (numPrims > 16) {
            numPrims = 16;
        }

        xcOverlayItem* items = overlay->GetItems();
        for (s32 i = 0; i < numPrims; i++) {
            s16 x = 0;
            s16 y = 0;
            u8* prim = rawData + items[i].dataOffset;
            GetPrimPos(prim, x, y);
            origPosX[i] = x;
            origPosY[i] = y;
        }
    }
    hdTextOvl::SelfInit();
}

void hdAnimTextOvl::SetAnimInfo(s32 offsetX, s32 offsetY, s32 dur, s32 pause) {
    MARKFUNCTION(0x8008F550);
    if (isPlaying) {
        Reset();
    }

    animDuration = dur;
    if (dur != 0) {
        stepX = (offsetX + dur / 2) / dur;
        stepY = (offsetY + dur / 2) / dur;
    }
    else {
        stepX = 0;
        stepY = 0;
    }

    pauseDuration = pause;
    GoToMinPos();
    if (isPlaying) {
        Play();
    }
}

void hdAnimTextOvl::SetPos(s32 deltaX, s32 deltaY) {
    MARKFUNCTION(0x8008F454);
    if (!overlay || !rawData) {
        return;
    }

    xcOverlayItem* items = overlay->GetItems();
    for (s32 i = 0; i < numPrims; i++) {
        u8* prim = rawData + items[i].dataOffset;
        SetPrimPos(prim, (s16)(origPosX[i] + deltaX), (s16)(origPosY[i] + deltaY));
    }
}

void hdAnimTextOvl::GoToMinPos() {
    MARKFUNCTION(0x8008F508);
    SetPos(-stepX * animDuration, -stepY * animDuration);
}

void hdAnimTextOvl::Update() {
    MARKFUNCTION(0x8008F664);
    if (isPlaying) {
        s32 dur = animDuration;
        s32 next = currentFrame + 1;
        s32 total = 2 * dur + pauseDuration;
        s32 baseX = -stepX * dur;
        s16 baseY = (s16)(-stepY * dur);

        currentFrame = next;
        if (dur >= next) {
            s32 x = baseX + stepX * next;
            s32 y = baseY + stepY * next;
            SetPos(x, y);
        }
        else {
            s32 holdEnd = dur + pauseDuration;
            if (holdEnd < next) {
                if (total < next) {
                    pauseFlag = 0;
                    currentFrame = 0;
                    isPlaying = 0;
                }
                else {
                    s32 out = total - currentFrame;
                    s32 x = baseX + stepX * out;
                    s32 y = baseY + stepY * out;
                    pauseFlag = 0;
                    SetPos((s16)x, y);
                }
            }
            else if (pauseFlag) {
                currentFrame = holdEnd - 1;
            }
        }
    }

    hdTtlive::Update();
}

void hdAnimTextOvl::Play() {
    MARKFUNCTION(0x8008F790);
    isPlaying = 1;
}

void hdAnimTextOvl::Reset() {
    MARKFUNCTION(0x8008F65C);
    currentFrame = 0;
}

void hdAnimTextOvl::SetPauseState(s32 state, s32 arg) {
    MARKFUNCTION(0x8008F79C);
    if (isPlaying && arg) {
        s32 mid = animDuration + pauseDuration / 2;
        if (state) {
            s32 cf = currentFrame;
            s32 v = cf - mid;
            if (cf >= mid) {
                currentFrame = mid - v;
            }
        }
        else {
            s32 cf = currentFrame;
            s32 v = cf + 1;
            if (cf < mid) {
                currentFrame = mid + mid - v;
            }
        }
    }
    pauseFlag = state;
}

hdDragon::hdDragon() {
    MARKFUNCTION(0x8008F84C);
    dragonCount = 0;
}

hdDragon::~hdDragon() {
}

void hdDragon::SelfInit() {
    MARKFUNCTION(0x8008F884);
    if (overlay && rawData) {
        goldTextObj = (xcTextPrim*)overlay->GetTextObj(0x054D73C1, rawData);
    }
    goldDragonFlag = 0;
    hdAnimTextOvl::SelfInit();
}

void hdDragon::SetNum(s32 num) {
    MARKFUNCTION(0x8008F834);
    dragonCount = (s16)num;
    sprintf(numberBuf, "%d", dragonCount);
}

void hdDragon::SetGoldDragons(s16 flag) {
    MARKFUNCTION(0x8008F8C4);
    goldDragonFlag = flag;
    if (goldTextObj) {
        if (flag) {
            goldTextObj->paletteIdx = 1;
        } else {
            goldTextObj->paletteIdx = 0;
        }
    }
}

hdHits::hdHits() {
    MARKFUNCTION(0x80090DC4);
    hitCount = 0;
    colorR = 255;
    colorG = 255;
    colorB = 255;
}

void hdHits::Init(oxScreenManager* scrmgr) {
    MARKFUNCTION(0x8008F93C);

    xcSection* sec = scrmgr->GetSection();
    rawData = sec ? sec->rawData : nullptr;

    overlay = scrmgr->FindOverlay((u32)0x71AB6E45);
    if (overlay && rawData) {
        oxOvl helper;
        helper.overlay = overlay;
        hitsTextObj = (xcTextPrim*)overlay->GetTextObj(0xAFA07175, rawData);
        hitsTextObj2 = (xcTextPrim*)overlay->GetTextObj(0xA3B1A82A, rawData);
        if (hitsTextObj2) {
            hitsTextObj2->hdr.subtype = 5;
        }
        if (hitsTextObj) {
            hitsTextObj->StringHashes()[hitsTextObj->paletteIdx] = xcRegisterRuntimeString(hitsBuf);
            helper.GetPrimPos(reinterpret_cast<u8*>(hitsTextObj), origPosX, origPosY);
        }
    }
    if (overlay) {
        overlay->visibility = 0;
    }

    fontHashLo = xcHash("Beats_lo");
    fontHashMid = xcHash("Beats_mid");
    fontHashXl = xcHash("Beats_xl");
}

void hdHits::Update() {
    MARKFUNCTION(0x8008FA2C);
    colorR = 255;
    if (colorG < 255) {
        colorG += 8;
        if (colorG > 255) {
            colorG = 255;
        }
    }
    if (colorB < 255) {
        colorB += 8;
        if (colorB > 255) {
            colorB = 255;
        }
    }

    ApplyHdHitsColor(hitsTextObj, colorR, colorG, colorB);

    if (hitsTextObj) {
        oxOvl helper;
        helper.overlay = overlay;
        const s32 shakeRange = (hitCount < 5) ? hitCount : 5;
        const s32 offsetX = (s32)rmRangedRandom((u32)shakeRange) - shakeRange;
        const s32 offsetY = (s32)rmRangedRandom((u32)shakeRange) - (shakeRange / 2);
        helper.SetPrimPos(reinterpret_cast<u8*>(hitsTextObj), (s16)(origPosX + offsetX), (s16)(origPosY + offsetY));
    }
}

void hdHits::IncrementHits() {
    MARKFUNCTION(0x8008FBEC);
    hitCount++;
    if (hitCount >= 2 && overlay) {
        overlay->visibility = 1;
    }

    const char* suffix = (hitCount < 2) ? s_hdHitSingular : s_hdHitPlural;
    snprintf(hitsBuf, sizeof(hitsBuf), "%2d %s", hitCount, suffix);

    colorR = 255;
    colorG = 0;
    colorB = 0;

    ApplyHdHitsColor(hitsTextObj, colorR, colorG, colorB);

    const u32 fontHash = (hitCount < 3) ? fontHashLo : fontHashMid;
    if (hitsTextObj) {
        hitsTextObj->fontHash = fontHash;
    }
    if (hitsTextObj2) {
        hitsTextObj2->fontHash = fontHash;
    }
}

void hdHits::TriggerUpdate() {
    MARKFUNCTION(0x8008FBC4);
    hitCount = 0;
    if (overlay) {
        overlay->visibility = 0;
    }
}

void hdHits::SetVisible(s32 /*vis*/) {
    MARKFUNCTION(0x80090DD8);
    if (overlay) {
        overlay->visibility = 0;
    }
}

// PSX: DoScoreTally__7hdTallyPcb (HDITEM.CPP:794, 0x800900A0)
static s32 DoScoreTally(hdTally* tally, char* scoreBuf, s32 scoreBufLen, bool fastForward) {
    MARKFUNCTION(0x800900A0);

    if (!scoreBuf || scoreBufLen <= 0) {
        return 1;
    }

    if (fastForward) {
        std::snprintf(scoreBuf, (size_t)scoreBufLen, "%d", tally->targetScore);
        return 1;
    }

    const s32 target = tally->targetScore;
    const s32 current = tally->currentScore;
    if (current >= target) {
        if (target == 0) {
            scoreBuf[0] = '0';
            scoreBuf[1] = 0;
        }
        return 1;
    }

    if ((target - current) < 452) {
        tally->currentScore = target;
    }
    else {
        tally->currentScore = current + 451;
    }

    std::snprintf(scoreBuf, (size_t)scoreBufLen, "%d", tally->currentScore);
    return 0;
}

// PSX: UpdateCombo__7hdTallyb (HDITEM.CPP:831, 0x8009013C)
static s32 UpdateCombo(hdTally* tally, bool fastForward) {
    MARKFUNCTION(0x8009013C);

    if (tally->frameCounter != 0) {
        tally->frameCounter -= 1;
        tally->comboOvl.SetVisible(1);
        if (tally->frameCounter == 0 || fastForward) {
            if (PlayLegacyTallySounds()) {
                if (!s_hdTallyFastForward && g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(26);
                }
            }

            tally->currentScore = 0;
            tally->frameCounter = 0;
            tally->targetScore = g_scoreManager ? g_scoreManager->currentComboScore : 0;
        }
    }

    if (tally->frameCounter == 0 && DoScoreTally(tally, tally->comboScoreBuf, (s32)sizeof(tally->comboScoreBuf), fastForward)) {
        if (PlayLegacyTallySounds()) {
            if (g_frontEndSound) {
                g_frontEndSound->ProcessSoundEvent(27);
            }
        }
        tally->state = 0;
        tally->frameCounter = 15;
    }

    return tally->frameCounter;
}

// PSX: UpdateFight__7hdTallyb (HDITEM.CPP:867, 0x80090220)
static s32 UpdateFight(hdTally* tally, bool fastForward) {
    MARKFUNCTION(0x80090220);

    if (tally->frameCounter != 0) {
        tally->frameCounter -= 1;
        tally->comboOvl.SetVisible(1);
        if (tally->frameCounter == 0 || fastForward) {
            if (!s_hdTallyFastForward && g_frontEndSound) {
                if (PlayLegacyTallySounds()) {
                    g_frontEndSound->ProcessSoundEvent(28);
                }
            }

            tally->currentScore = 0;
            tally->frameCounter = 0;
            tally->targetScore = g_scoreManager ? g_scoreManager->currentFightScore : 0;
        }
    }

    if (tally->frameCounter == 0 && DoScoreTally(tally, tally->fightScoreBuf, (s32)sizeof(tally->fightScoreBuf), fastForward)) {
        if (PlayLegacyTallySounds()) {
            if (g_frontEndSound) {
                g_frontEndSound->ProcessSoundEvent(29);
            }
        }
        tally->state = 2;
        tally->frameCounter = 15;
    }

    return tally->frameCounter;
}

// PSX: UpdateStyle__7hdTallyb (HDITEM.CPP:902, 0x80090308)
static s32 UpdateStyle(hdTally* tally, bool fastForward) {
    MARKFUNCTION(0x80090308);

    if (tally->frameCounter != 0) {
        tally->frameCounter -= 1;
        tally->comboOvl.SetVisible(1);
        if (tally->frameCounter == 0 || fastForward) {
            if (PlayLegacyTallySounds()) {
                if (!s_hdTallyFastForward && g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(30);
                }
            }

            tally->currentScore = 0;
            tally->frameCounter = 0;
            tally->delayCounter = 15;
            tally->targetScore = g_scoreManager ? g_scoreManager->currentStyleScore : 0;
        }
    }

    if (tally->frameCounter == 0 && DoScoreTally(tally, tally->styleScoreBuf, (s32)sizeof(tally->styleScoreBuf), fastForward)) {
        if (PlayLegacyTallySounds()) {
            if (tally->delayCounter == 15 && g_frontEndSound) {
                g_frontEndSound->ProcessSoundEvent(31);
            }
        }

        if (tally->delayCounter == 0) {
            tally->state = 3;
            tally->frameCounter = 15;
        }
        else {
            tally->delayCounter -= 1;
        }
    }

    return tally->frameCounter;
}

// PSX: UpdateGrade__7hdTallyb (HDITEM.CPP:948, 0x8009041C)
static s32 UpdateGrade(hdTally* tally, bool fastForward) {
    MARKFUNCTION(0x8009041C);

    if (tally->frameCounter != 0) {
        tally->frameCounter -= 1;
        tally->gradeOvl.SetVisible(1);
        if (tally->frameCounter == 0 || fastForward) {
            const u8 grade = g_scoreManager ? g_scoreManager->CalcGrade() : 0;
            const s32 livesBefore = Player::s_player ? Player::s_player->livesLeft : 0;
            const s32 bonusLives = g_scoreManager ? g_scoreManager->CalcGradeXTakes(grade) : 0;

            if (Player::s_player) {
                Player::s_player->SetLivesLeft(livesBefore + bonusLives);
            }

            if (tally->gradeOvl.overlay && tally->rawData) {
                xcTextPrim* gradeText = (xcTextPrim*)tally->gradeOvl.overlay->GetTextObj((u32)-1623425080, tally->rawData);
                if (gradeText) {
                    gradeText->paletteIdx = grade;
                }
            }

            if (tally->takesOvlPtr && Player::s_player) {
                tally->takesOvlPtr->SetNumber(Player::s_player->livesLeft - 1);
            }

            tally->frameCounter = 0;
            tally->delayCounter = 30;

            if (PlayLegacyTallySounds()) {
                if (!s_hdTallyFastForward && g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(22);
                }
            }
        }
    }

    if (tally->frameCounter == 0) {
        if (tally->delayCounter == 0 || fastForward) {
            s32 nextState = 4;
            World* world = g_game ? g_game->GetWorld() : nullptr;
            if (world && world->GetCurLevelID() == 6) {
                if (tally->doneOvl.overlay && tally->rawData && g_scoreManager) {
                    xcTextPrim* doneText = (xcTextPrim*)tally->doneOvl.overlay->GetTextObj(499091404u, tally->rawData);
                    if (doneText) {
                        doneText->paletteIdx = (u8)(g_scoreManager->GetTotalGoldDragon()
                            + (g_scoreManager->CalcGDrags(g_scoreManager->currentCollectCount) ? 1 : 0) >= 20);
                    }
                }
                nextState = 7;
            }

            tally->state = nextState;
            tally->frameCounter = 15;
        }
        else {
            tally->delayCounter -= 1;
        }
    }

    return tally->frameCounter;
}

// PSX: UpdateRdragon__7hdTallyb (HDITEM.CPP:1013, 0x800905DC)
static s32 UpdateRdragon(hdTally* tally, bool fastForward) {
    MARKFUNCTION(0x800905DC);

    if (tally->frameCounter != 0) {
        tally->frameCounter -= 1;
        tally->rdragonOvl.SetVisible(1);
        if (tally->frameCounter == 0 || fastForward) {
            if (PlayLegacyTallySounds()) {
                if (!s_hdTallyFastForward && g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(23);
                }
            }

            tally->frameCounter = 0;
            const s32 collectCount = g_scoreManager ? g_scoreManager->currentCollectCount : 0;
            std::snprintf(tally->rdragonBuf, sizeof(tally->rdragonBuf), "%d", collectCount);
            tally->delayCounter = (g_scoreManager && g_scoreManager->CalcGDrags(collectCount)) ? 10 : 30;
        }
    }

    if (tally->frameCounter == 0) {
        if (tally->delayCounter == 0 || fastForward) {
            const bool hasGoldDragon = g_scoreManager && g_scoreManager->CalcGDrags(g_scoreManager->currentCollectCount);
            tally->state = hasGoldDragon ? 5 : 6;
            tally->frameCounter = hasGoldDragon ? 1 : 15;
        }
        else {
            tally->delayCounter -= 1;
        }
    }

    return tally->frameCounter;
}

// PSX: UpdateRdragonBonus__7hdTallyb (HDITEM.CPP:1069, 0x80090718)
static s32 UpdateRdragonBonus(hdTally* tally, bool fastForward) {
    MARKFUNCTION(0x80090718);

    if (tally->frameCounter != 0) {
        tally->frameCounter -= 1;
        tally->rdragonOvl.SetVisible(1);
        if (tally->frameCounter == 0 || fastForward) {
            if (PlayLegacyTallySounds()) {
                if (!s_hdTallyFastForward && g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(25);
                }
            }

            tally->rdragonBonusOvl.SetVisible(1);
            tally->movieBonusOvl.SetVisible(1);
            tally->delayCounter = 30;
            tally->frameCounter = 0;
        }
    }

    if (tally->delayCounter == 0 || fastForward) {
        tally->state = 6;
        tally->frameCounter = 15;
    }
    else {
        tally->delayCounter -= 1;
    }

    return tally->frameCounter;
}

// PSX: UpdateGdragon__7hdTallyb (HDITEM.CPP:1106, 0x800907F0)
static s32 UpdateGdragon(hdTally* tally, bool fastForward) {
    MARKFUNCTION(0x800907F0);

    if (tally->frameCounter != 0) {
        tally->frameCounter -= 1;
        tally->gdragonOvl.SetVisible(1);
        if (tally->frameCounter == 0 || fastForward) {
            if (PlayLegacyTallySounds()) {
                if (!s_hdTallyFastForward && g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(24);
                }
            }

            tally->movieBonusOvl.SetVisible(0);
            const s32 totalGoldDragons = g_scoreManager ? g_scoreManager->GetTotalGoldDragon() : 0;
            const s32 levelGoldDragon = (g_scoreManager && g_scoreManager->CalcGDrags(g_scoreManager->currentCollectCount)) ? 1 : 0;
            std::snprintf(tally->gdragonBuf, sizeof(tally->gdragonBuf), "%d", totalGoldDragons + levelGoldDragon);
            tally->delayCounter = 30;
            tally->frameCounter = 0;
        }
    }

    if (tally->frameCounter == 0) {
        if (tally->delayCounter == 0 || fastForward) {
            tally->state = 8;
            tally->frameCounter = 15;
        }
        else {
            tally->delayCounter -= 1;
        }
    }

    return tally->frameCounter;
}

// PSX: UpdateMovieBonus__7hdTallyb (HDITEM.CPP:1144, 0x80090914)
static s32 UpdateMovieBonus(hdTally* tally, bool fastForward) {
    MARKFUNCTION(0x80090914);

    if (tally->frameCounter != 0) {
        tally->frameCounter -= 1;
        if (tally->frameCounter == 0 || fastForward) {
            if (PlayLegacyTallySounds()) {
                if (!s_hdTallyFastForward && g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(25);
                }
            }

            tally->doneOvl.SetVisible(1);
            tally->delayCounter = 30;
            tally->frameCounter = 0;
        }
    }

    if (tally->frameCounter == 0) {
        if (tally->delayCounter == 0 || fastForward) {
            tally->state = 8;
        }
        else {
            tally->delayCounter -= 1;
        }
    }

    return tally->frameCounter;
}

hdTally::hdTally() {
    MARKFUNCTION(0x8008FDB4);
    state = 0;
}

void hdTally::Init(oxScreenManager* scrmgr) {
    MARKFUNCTION(0x8008FEC0);

    xcSection* sec = scrmgr->GetSection();
    rawData = sec ? sec->rawData : nullptr;

    fightOvl.Init(scrmgr->FindOverlay((u32)0x4EF9135A));
    comboOvl.Init(scrmgr->FindOverlay((u32)0x734E176A));
    gradeOvl.Init(scrmgr->FindOverlay((u32)0x5FD7B608));
    rdragonOvl.Init(scrmgr->FindOverlay((u32)0x53CF0BA0));
    gdragonOvl.Init(scrmgr->FindOverlay((u32)0x006D3006));
    rdragonBonusOvl.Init(scrmgr->FindOverlay((u32)0x30106267));
    movieBonusOvl.Init(scrmgr->FindOverlay((u32)0xACE2F970));
    doneOvl.Init(scrmgr->FindOverlay((u32)0x1DBF87CC));

    if (comboOvl.overlay && rawData) {
        xcTextPrim* tp;
        tp = (xcTextPrim*)comboOvl.overlay->GetTextObj(0xB4B11177, rawData);
        if (tp) {
            tp->StringHashes()[tp->paletteIdx] = xcRegisterRuntimeString(fightScoreBuf);
        }
        tp = (xcTextPrim*)comboOvl.overlay->GetTextObj(0xA04AB9F5, rawData);
        if (tp) {
            tp->StringHashes()[tp->paletteIdx] = xcRegisterRuntimeString(comboScoreBuf);
        }
        tp = (xcTextPrim*)comboOvl.overlay->GetTextObj(0x37B46CB6, rawData);
        if (tp) {
            tp->StringHashes()[tp->paletteIdx] = xcRegisterRuntimeString(styleScoreBuf);
        }
    }

    if (rdragonOvl.overlay && rawData) {
        xcTextPrim* tp = (xcTextPrim*)rdragonOvl.overlay->GetTextObj(0x7ABBECC0, rawData);
        if (tp) {
            tp->StringHashes()[tp->paletteIdx] = xcRegisterRuntimeString(rdragonBuf);
        }
    }

    if (gdragonOvl.overlay && rawData) {
        xcTextPrim* tp = (xcTextPrim*)gdragonOvl.overlay->GetTextObj(0x7ABBECC0, rawData);
        if (tp) {
            tp->StringHashes()[tp->paletteIdx] = xcRegisterRuntimeString(gdragonBuf);
        }
    }

    if (fightOvl.overlay && rawData) {
        doneTextPrim = (xcTextPrim*)fightOvl.overlay->GetTextObj(0xCFCAFC6A, rawData);
    }

    Show(0);
}

void hdTally::Show(s32 vis) {
    MARKFUNCTION(0x80090D2C);
    fightOvl.SetVisible((s16)vis);
    comboOvl.SetVisible((s16)vis);
    gradeOvl.SetVisible((s16)vis);
    rdragonOvl.SetVisible((s16)vis);
    gdragonOvl.SetVisible((s16)vis);
    rdragonBonusOvl.SetVisible((s16)vis);
    movieBonusOvl.SetVisible((s16)vis);
    doneOvl.SetVisible((s16)vis);
    state = 9;
}

void hdTally::Start(s32 tally) {
    MARKFUNCTION(0x80090C58);

    fightOvl.SetVisible(1);
    if (tally) {
        if (gradeOvl.overlay && rawData) {
            xcTextPrim* gradeText = (xcTextPrim*)gradeOvl.overlay->GetTextObj((u32)-1623425080, rawData);
            if (gradeText) {
                gradeText->paletteIdx = 8;
            }
        }

        fightScoreBuf[0] = ' ';
        fightScoreBuf[1] = 0;
        comboScoreBuf[0] = ' ';
        comboScoreBuf[1] = 0;
        styleScoreBuf[0] = ' ';
        styleScoreBuf[1] = 0;
        rdragonBuf[0] = ' ';
        rdragonBuf[1] = 0;
        gdragonBuf[0] = ' ';
        gdragonBuf[1] = 0;
        state = 1;
        frameCounter = 15;

        if (takesOvlPtr) {
            takesOvlPtr->SetPauseState(1, 1);
            takesOvlPtr->Play();
        }

        if (g_frontEndSound) {
            if (PlayLegacyTallySounds()) {
                g_frontEndSound->ProcessSoundEvent(21);
            }
        }
    } else {
        state = 9;
    }

    MenuColorStart(doneColor);
}

void hdTally::Update() {
    MARKFUNCTION(0x80090AC0);

    bool fastForward = false;
    if (state != 8 && state != 9) {
#if CUSTOM_MENU
        bool handledByCustomActionInput = false;
        if (g_actionInput) {
            handledByCustomActionInput = true;
            fastForward = g_actionInput->IsHeld(ACTION_MENU_CONFIRM);
            if (g_actionInput->JustPressed(ACTION_MENU_CONFIRM)) {
                if (g_frontEndSound) {
                    g_frontEndSound->ProcessSoundEvent(FE_SND_MENU_OPEN);
                }
                s_hdTallyFastForward = 1;
            }
        }

        if (!handledByCustomActionInput)
#endif
        {
            if (g_inputManager) {
                g_inputManager->Step();

                const u32 controlVal = g_inputManager->GetControlVal(0);
                const u8* playerMap = g_inputManager->PlayerMapArray();
                const u32 mappedMask = playerMap ? (1u << playerMap[6]) : 0;

                fastForward = (controlVal & mappedMask) != 0;
                if ((controlVal & PsxPad::Start) != 0) {
                    if (g_frontEndSound) {
                        g_frontEndSound->ProcessSoundEvent(FE_SND_MENU_OPEN);
                    }
                    s_hdTallyFastForward = 1;
                }
            }
        }

        if (s_hdTallyFastForward) {
            fastForward = true;
        }
    }

    switch (state) {
        case 0:
            UpdateFight(this, fastForward);
            break;

        case 1:
            UpdateCombo(this, fastForward);
            break;

        case 2:
            UpdateStyle(this, fastForward);
            break;

        case 3:
            UpdateGrade(this, fastForward);
            break;

        case 4:
            UpdateRdragon(this, fastForward);
            break;

        case 5:
            UpdateRdragonBonus(this, fastForward);
            break;

        case 6:
            UpdateGdragon(this, fastForward);
            break;

        case 7:
            UpdateMovieBonus(this, fastForward);
            break;

        case 8:
            DoDoneStuff();
            s_hdTallyFastForward = 0;
            break;

        default:
            break;
    }
}

void hdTally::DoDoneStuff() {
    MARKFUNCTION(0x800909D0);

    MenuColorNext(doneColor);
    if (doneTextPrim) {
        doneTextPrim->colorR = doneColor.GetRed8();
        doneTextPrim->colorG = doneColor.GetGreen8();
        doneTextPrim->colorB = doneColor.GetBlue8();
        doneTextPrim->colorA = doneColor.GetAlpha8();
    }

    bool donePressed = false;

#if CUSTOM_MENU
    if (g_actionInput) {
        donePressed = g_actionInput->JustPressed(ACTION_MENU_CONFIRM);
    }
#endif

    if (!donePressed) {
        if (!g_inputManager) {
            return;
        }

        g_inputManager->Step();
        const u32 controlVal = g_inputManager->GetControlVal(0);
        const u8* playerMap = g_inputManager->PlayerMapArray();
        u32 doneMask = PsxPad::Start;
        if (playerMap) {
            doneMask |= 1u << playerMap[6];
        }
        donePressed = (controlVal & doneMask) != 0;
    }

    if (!donePressed) {
        return;
    }

    if (g_frontEndSound) {
        g_frontEndSound->ProcessSoundEvent(20);
    }
    state = 9;
    if (takesOvlPtr) {
        takesOvlPtr->SetPauseState(0, 1);
    }
}

hdDestSelect::hdDestSelect() {
    MARKFUNCTION(0x8008EFA4);
}

// PSX: Init__12hdDestSelectP15oxScreenManager (HDITEM.CPP:327, 0x8008F05C)
void hdDestSelect::Init(oxScreenManager* scrmgr) {
    MARKFUNCTION(0x8008F05C);

    xcSection* sec = scrmgr->GetSection();
    rawData = sec ? sec->rawData : nullptr;
    section = sec;

    titleOvl1 = scrmgr->FindOverlay((u32)0xD3DA1590);
    if (titleOvl1) {
        titleOvl1->visibility = 0;
    }

    titleOvl2 = scrmgr->FindOverlay((u32)0x4F1CC7C3);
    if (titleOvl2) {
        titleOvl2->visibility = 0;
    }

    xcOverlayData* levelOvl = scrmgr->FindOverlay((u32)0x4F1CC7C2);
    ttlive.Init(levelOvl);
    ttlive.SetVisible(0);
}

// PSX: Start__12hdDestSelectl (HDITEM.CPP:312, 0x8008EFD0)
void hdDestSelect::Start(s32 totalGoldDragon) {
    MARKFUNCTION(0x8008EFD0);

    if (titleOvl1) {
        titleOvl1->visibility = 1;
    }
    if (titleOvl2) {
        titleOvl2->visibility = 1;
    }
    ttlive.SetVisible(0);

    currentLevel = 0;
    if (totalGoldDragon < 0) {
        totalGoldDragon = 0;
    }
    else if (totalGoldDragon > 99) {
        totalGoldDragon = 99;
    }
    sprintf(s_dragonCountBuf, "%d", totalGoldDragon);

    if (titleOvl1 && rawData) {
        xcTextPrim* textObj = (xcTextPrim*)titleOvl1->GetTextObj((u32)0x7ABBECC0, rawData);
        if (textObj) {
            u8 idx = textObj->paletteIdx;
            u32* hashes = textObj->StringHashes();
            hashes[idx] = xcRegisterRuntimeString(s_dragonCountBuf);
        }
    }
}

// PSX: Hide__12hdDestSelect (HDITEM.CPP:342, 0x8008F0F4)
void hdDestSelect::Hide() {
    MARKFUNCTION(0x8008F0F4);
    if (titleOvl1) {
        titleOvl1->visibility = 0;
    }
    if (titleOvl2) {
        titleOvl2->visibility = 0;
    }
    ttlive.SetVisible(0);
}

// PSX: ShowLevel__12hdDestSelecti (HDITEM.CPP:349, 0x8008F138)
void hdDestSelect::ShowLevel(s32 level) {
    MARKFUNCTION(0x8008F138);

    if (currentLevel == level) {
        return;
    }
    currentLevel = level;

    if (level <= 0) {
        if (titleOvl1) {
            titleOvl1->visibility = 1;
        }
        if (titleOvl2) {
            titleOvl2->visibility = 1;
        }
        ttlive.SetVisible(0);
        return;
    }

    World* world = g_game ? g_game->GetWorld() : nullptr;
    s32 levelIndex = 0;
    if (world) {
        levelIndex = world->LevelIDToIndex(level);
    }

    xcOverlayData* ovl = ttlive.overlay;
    if (ovl && rawData) {
        xcTextPrim* textObj1 = (xcTextPrim*)ovl->GetTextObj((u32)0xD0324E92, rawData);
        if (textObj1) {
            textObj1->paletteIdx = (u8)levelIndex;
        }

        xcTextPrim* textObj2 = (xcTextPrim*)ovl->GetTextObj((u32)0x7E309671, rawData);
        if (textObj2) {
            u8 idx2 = textObj2->paletteIdx;
            u32 strHash = textObj2->StringHashes()[idx2];

            s32 completed = 0;
            if (g_scoreManager) {
                PetalStats* ps = &g_scoreManager->petalStats[levelIndex * 3];
                if (ps[0].goldDragons || ps[1].goldDragons || ps[2].goldDragons) {
                    completed = 1;
                }
            }

            // PSX writes first byte of resolved string pointer.
            // PC: resolve hash to string in rawData and modify in-place.
            if (section) {
                char* str = (char*)section->FindString(strHash);
                if (str) {
                    *str = completed ? '1' : '0';
                }
            }
        }
    }

    if (titleOvl1) {
        titleOvl1->visibility = 0;
    }
    if (titleOvl2) {
        titleOvl2->visibility = 0;
    }
    ttlive.SetTtlive(90);
}

// PSX: Update__12hdDestSelect (HDITEM.CPP:390, 0x8008F26C)
void hdDestSelect::Update() {
    MARKFUNCTION(0x8008F26C);
    ttlive.Update();
}

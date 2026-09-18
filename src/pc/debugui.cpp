#include "gen/common.h"
#include "pc/debugui.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "gen/game.h"
#include "gen/world.h"
#include "gen/camera.h"
#include "gen/animstruct.h"
#include "gen/model.h"
#include "gen/effects.h"
#include "gen/particle.h"
#include "gen/weffect.h"
#include "gen/pweffect.h"
#include "gen/charmgr.h"
#include "gen/director.h"
#include "gen/scoremgr.h"
#include "gen/time.h"
#include "ai/player.h"
#include "ai/humanoid.h"
#include "ai/thing.h"
#include "snd/sound.h"
#include "pc/audio.h"
#include "p3d/lvector.h"
#include "fe/femenumgr.h"
#include "gen/ai.h"
#include "ai/obstacle.h"
#include "gen/levelmgr.h"
#include "gen/blockmgr.h"
#include "gen/display.h"
#include "extra/fecustommenumgr.h"
#include "extra/customhudmgr.h"
#include "pc/log.h"
#include "fe/xcfont.h"
#include "p3d/hash.h"
#include "extra/modloader.h"
#include "extra/assetexporter.h"
#include "extra/assetexporter_ui.h"
#include "p3d/context.h"
#include "extra/shadowcsm.h"
#include "snd/snddrct.h"

static bool sEnabled = false;
static bool sShowPlayer = false;
static bool sShowCamera = false;
static bool sShowAudio = false;
static bool sShowAnimation = false;
static bool sShowGame = false;
static bool sShowParticles = false;
static bool sShowDebugging = false;
static bool sShowEffects = false;
static char sEffectsFilter[128] = {};
static bool sShowConsoleNotes = false;
static bool sShowImGuiDemo = false;
static s32 sAnimSelectedEnum = 0;
static s32 sAnimSelectedLoopType = ANIM_LOOP;
static bool sInputRoutingOverride = false;
static s32 sInputRoutingSelection = 0; // 0 = Camera input, 1 = Player input
static char sConsoleNoteInput[1024] = {};
static char sLastConsoleNote[1024] = {};
static bool sCursorForcedByDebugUI = false;
static bool sShowAssetExporter = false;
static bool sShowMods = false;
#if MODERN_GRAPHICS
static bool sShowShadows = false;
static s32 sShadowDebugMode = 0;
#endif
static bool sAssetExporterCatalogBuilt = false;
static char sAssetExporterFilter[128] = {};
static int sAssetExporterCategoryFilter = 0;
static char sAssetExporterOutputDir[256] = "~mods/ExportedAssets";

struct DebugParticleChoice {
    const char* label;
    u32 hash;
};

static const DebugParticleChoice kDebugParticleChoices[] = {
    { "PBITS", PARTICLE_PBITS },
    { "PBLKSMOKE", PARTICLE_PBLKSMOKE },
    { "PCRDUST", PARTICLE_PCRDUST },
    { "PCRUMB", PARTICLE_PCRUMB },
    { "PDFRAG", PARTICLE_PDFRAG },
    { "PDFRAGS", PARTICLE_PDFRAGS },
    { "PEXPLBLK", PARTICLE_PEXPLBLK },
    { "PEXPLFRG", PARTICLE_PEXPLFRG },
    { "PEXPLOR3A", PARTICLE_PEXPLOR3A },
    { "PFALLMIST", PARTICLE_PFALLMIST },
    { "PFEATHER", PARTICLE_PFEATHER },
    { "PFLAMEVENT", PARTICLE_PFLAMEVENT },
    { "PFLIMPCT", PARTICLE_PFLIMPCT },
    { "PGLASSHAT", PARTICLE_PGLASSHAT },
    { "PGOO_L", PARTICLE_PGOO_L },
    { "PGOO_L2", PARTICLE_PGOO_L2 },
    { "PGOO_L3", PARTICLE_PGOO_L3 },
    { "PGOO_R", PARTICLE_PGOO_R },
    { "PGOO_R2", PARTICLE_PGOO_R2 },
    { "PGOO_R3", PARTICLE_PGOO_R3 },
    { "PGRILSMK", PARTICLE_PGRILSMK },
    { "PLCRUMBL", PARTICLE_PLCRUMBL },
    { "PROOFSMOKE", PARTICLE_PROOFSMOKE },
    { "PSHARD", PARTICLE_PSHARD },
    { "PSHARD2", PARTICLE_PSHARD2 },
    { "PVENTSTEAM", PARTICLE_PVENTSTEAM },
};

static s32 sDebugParticleChoiceIndex = 0;
static s32 sDebugParticleSpawnDistance = 1024;
static s32 sDebugParticleSpawnHeight = 0;
static s32 sDebugParticleLifeFrames = 30;
static s32 sDebugParticleLastSpawnResult = -1;
static bool sDebugShowHumanoidNames3D = false;
static bool sDebugShowAllAIThingNames3D = false;
static bool sDebugShowEffectNames3D = false;
static bool sDebugShowDetailedEffectNames3D = true;
static bool sDebugShowThingLabelHoverTooltip = true;
static u16 sDebugSelectedThingUniqueID = INVALID_HANDLE;
static bool sDisableHumanoidDamage = false;
static s32 sDebugHumanoidSwitcherIndex = 0;
static u32 sDebugHumanoidLoadedType = 0;    // type the switcher loaded into an NPC slot (0 = none)
static bool sDebugTallyPreviewEnabled = false;
static s32 sDebugTallyFightScore = 4000;
static s32 sDebugTallyComboScore = 5500;
static s32 sDebugTallyStyleScore = 6200;
static s32 sDebugTallyRedDragons = 10;
static s32 sDebugTallyGoldDragons = 0;
static s32 sDebugTallyGrade = 5;
static bool sDebugTallyShowMovieBonus = false;

struct DebugLevelSelectorEntry {
    s32 levelID;
    s32 petalIndex;
    const char* label;
};

static const DebugLevelSelectorEntry kDebugLevelSelectorEntries[] = {
    { 1, 0, "Chinatown 1" },
    { 1, 1, "Chinatown 2" },
    { 1, 2, "Chinatown 3" },
    { 11, 0, "Chinatown Boss" },
    { 2, 0, "Waterfront 1" },
    { 2, 1, "Waterfront 2" },
    { 2, 2, "Waterfront 3" },
    { 12, 0, "Waterfront Boss" },
    { 3, 0, "Sewer 1" },
    { 3, 1, "Sewer 2" },
    { 3, 2, "Sewer 3" },
    { 13, 0, "Sewer Boss" },
    { 4, 0, "Roof Top 1" },
    { 4, 1, "Roof Top 2" },
    { 4, 2, "Roof Top 3" },
    { 14, 0, "Roof Top Boss" },
    { 5, 0, "Factory 1" },
    { 5, 1, "Factory 2" },
    { 5, 2, "Factory 3" },
    { 8, 0, "Factory Boss" },
    { 6, 0, "Special Level" },
};

static s32 sDebugLevelSelectorChoice = 0;
static bool sDebugLevelSelectorSpawnAtCheckpoint = false;
static bool sDebugLevelSelectorUseCustomCheckpointName = false;
static s32 sDebugLevelSelectorCheckpointIndex = 1;
static char sDebugLevelSelectorCustomCheckpointName[32] = "gotoa1";

struct DebugPendingCheckpointWarp {
    bool active = false;
    u32 targetLevelIndex = 0xFFFFFFFFu;
    u32 targetPetalIndex = 0xFFFFFFFFu;
    u32 checkpointCRC = 0;
    s32 retryFrames = 0;
    char checkpointName[32] = {};
};

static DebugPendingCheckpointWarp sDebugPendingCheckpointWarp = {};

struct DebugVisibleThingLabel {
    Thing* thing = nullptr;
    char label[128] = {};
    f32 screenX = 0.0f;
    f32 screenY = 0.0f;
};

static constexpr s32 MAX_DEBUG_VISIBLE_THING_LABELS = 2048;
static DebugVisibleThingLabel sDebugVisibleThingLabels[MAX_DEBUG_VISIBLE_THING_LABELS] = {};
static s32 sDebugVisibleThingLabelCount = 0;

static void ApplyCursorPolicy() {
    if (!g_display) {
        return;
    }

    if (sEnabled) {
        // Debug UI requires a usable mouse pointer: no clipping/capture and visible cursor.
        g_display->SetCursorCaptured(false);
        g_display->SetCursorVisible(true);
        sCursorForcedByDebugUI = true;
        return;
    }

    if (!sCursorForcedByDebugUI) {
        return;
    }

    // Restore whichever mode owns the cursor outside Debug UI.
    if (g_feCustomMenuMgr && g_feCustomMenuMgr->IsActive()) {
        g_display->SetCursorCaptured(false);
        g_display->SetCursorVisible(false);
    }
    else {
        g_display->SetCursorCaptured(true);
    }
    sCursorForcedByDebugUI = false;
}

static bool ShouldBlockGameInputFromImGui() {
    if (!sEnabled || !ImGui::GetCurrentContext()) {
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantCaptureMouse || io.WantTextInput;
}

bool DebugUI::ShouldBlockGameInput() {
    return ShouldBlockGameInputFromImGui();
}

bool DebugUI::IsPlayerInputAllowed() {
    if (ShouldBlockGameInputFromImGui()) {
        return false;
    }

    if (!sInputRoutingOverride) {
        return true;
    }
    return sInputRoutingSelection == 1;
}

bool DebugUI::IsDebugCameraInputAllowed() {
    if (ShouldBlockGameInputFromImGui()) {
        return false;
    }

    if (!sInputRoutingOverride) {
        return true;
    }
    return sInputRoutingSelection == 0;
}

static const char* GameStateName(GameState s) {
    switch (s) {
        case GameState::Null: return "Null";
        case GameState::Intro: return "Intro";
        case GameState::Title: return "Title";
        case GameState::TitleLoop: return "TitleLoop";
        case GameState::Init: return "Init";
        case GameState::OpenFE: return "OpenFE";
        case GameState::FE: return "FE";
        case GameState::PrePlay: return "PrePlay";
        case GameState::Play: return "Play";
        case GameState::EndLevel: return "EndLevel";
        case GameState::EndLevelLoop: return "EndLevelLoop";
        case GameState::EndLevelExit: return "EndLevelExit";
        case GameState::PlayMovieCredits: return "PlayMovieCredits";
        case GameState::DbgMenu: return "DbgMenu";
        case GameState::Menu: return "Menu";
        case GameState::Error: return "Error";
        case GameState::ErrorLoop: return "ErrorLoop";
        case GameState::ErrorExit: return "ErrorExit";
        case GameState::LocationMenu: return "LocationMenu";
        case GameState::OpenLocationMenu: return "OpenLocationMenu";
        case GameState::QueueLevelLoad: return "QueueLevelLoad";
        case GameState::QueuePetalLoad: return "QueuePetalLoad";
        case GameState::QueueLevelPetalLoad: return "QueueLevelPetalLoad";
        case GameState::DetermineNextGameState: return "DetermineNextGameState";
        case GameState::DetermineGameOverState: return "DetermineGameOverState";
        case GameState::EndGame: return "EndGame";
        case GameState::EndGameLoop: return "EndGameLoop";
        case GameState::End: return "End";
        default: return "Unknown";
    }
}

static const char* ActionStateName(s32 s) {
    switch (s) {
        case AS_INACTIVE_IDLE: return "InactiveIdle";
        case AS_STAND: return "Stand";
        case AS_STAND_ANIM: return "StandAnim";
        case AS_WALL_JUMP_TAUNT: return "WallJumpTaunt";
        case AS_DIVE_ROLL: return "DiveRoll";
        case AS_PAUSE: return "Pause/RunJump";
        case AS_JUMP: return "Jump";
        case AS_WALL_JUMP: return "WallJump";
        case AS_RUN: return "Run";
        case AS_STRAFE: return "Strafe";
        case AS_FALL: return "Fall";
        case AS_HARDFALL: return "HardFall";
        case AS_HARDLAND: return "HardLand";
        case AS_FLIP: return "Flip";
        case AS_FLIP_VARIANT: return "FlipVariant";
        case AS_POLE_IDLE: return "PoleIdle";
        case AS_PUSH_OBJECT: return "PushObject";
        case AS_SLOPE_SLIDE: return "SlopeSlide";
        case AS_TABLE_ROLL: return "TableRoll";
        case AS_LEDGE_LATCH: return "LedgeLatch";
        case AS_LEDGE_PULLUP: return "LedgePullup";
        case AS_PUNCH_ATTACK: return "PunchAttack";
        case AS_KICK_ATTACK: return "KickAttack";
        case AS_COMBAT_IDLE: return "CombatIdle";
        case AS_BACK_GRAB_LATCH: return "BackGrabLatch";
        case AS_BACK_GRAB: return "BackGrab";
        case AS_BACK_GRAB_RELEASE: return "BackGrabRelease";
        case AS_COUNTER_ATTACK_PRE_LATCH: return "CounterAttackPreLatch";
        case AS_COUNTER_ATTACK_LATCH: return "CounterAttackLatch";
        case AS_COUNTER_ATTACK: return "CounterAttack";
        case AS_COUNTER_ATTACK_RECOVERY: return "CounterAttackRecovery";
        case AS_PICKUP: return "Pickup";
        case AS_THROW_PICKUP: return "ThrowPickup";
        case AS_THROW_CHARACTER_RECEIVE: return "ThrowCharacterReceive";
        case SD_COUNTER_ATTACK_PRE_LATCH: return "CounterAttackPreLatch";
        case SD_COUNTER_ATTACK_LATCH: return "CounterAttackLatch";
        case SD_COUNTER_ATTACK: return "CounterAttack";
        case SD_COUNTER_ATTACK_RECOVERY: return "CounterAttackRecovery";
        case AS_BACK_GRAB_RECEIVE_PRE_LATCH: return "BackGrabReceivePreLatch";
        case AS_BACK_GRAB_RECEIVE_LATCH: return "BackGrabReceiveLatch";
        case AS_THROW_FREE_FALL: return "ThrowFreeFall";
        case AS_FLYING_BACK_LAND: return "FlyingBackLand";
        case AS_BACK_GRAB_RECEIVE: return "BackGrabReceive";
        case AS_GET_UP: return "GetUp";
        case AS_FLYING_BACK_CHECK: return "FlyingBackCheck";
        case AS_SPIN_BACK_RECOVER: return "SpinBackRecover";
        case AS_DEAD: return "Dead";
        case AS_HIT_EXPLOSION: return "HitExplosion";
        case AS_HIT_ENVIRONMENT: return "HitEnvironment";
        case AS_MISSILE_ATTACK: return "MissileAttack";
        case AS_MISSILE_PREPARE: return "MissilePrepare";
        case AS_TARGET_MISSILE_ATTACK: return "TargetMissileAttack";
        default: return "Unknown";
    }
}

static bool ResolveDebugLevelSelectorEntry(
    World* world,
    s32 entryIndex,
    u32* outLevelIndex,
    u32* outPetalIndex
) {
    if (!world) {
        return false;
    }

    const s32 entryCount = (s32)(sizeof(kDebugLevelSelectorEntries) / sizeof(kDebugLevelSelectorEntries[0]));
    if ((u32)entryIndex >= (u32)entryCount) {
        return false;
    }

    const DebugLevelSelectorEntry& entry = kDebugLevelSelectorEntries[entryIndex];
    const s32 levelIndex = world->LevelIDToIndex(entry.levelID);
    if (levelIndex < 0) {
        return false;
    }

    const u32 levelIndexU = (u32)levelIndex;
    if (world->GetLevelIDFromIndex(levelIndexU) != entry.levelID) {
        return false;
    }

    if (outLevelIndex) {
        *outLevelIndex = levelIndexU;
    }

    s32 petalCount = world->GetLevelPetalCountFromIndex(levelIndexU);
    if (petalCount < 1) {
        petalCount = 1;
    }

    if (entry.petalIndex < 0 || entry.petalIndex >= petalCount) {
        return false;
    }

    if (outPetalIndex) {
        *outPetalIndex = (u32)entry.petalIndex;
    }

    return true;
}

static void BuildAutoCheckpointName(u32 petalIndex, s32 checkpointIndex, char* outName, s32 outNameSize) {
    if (!outName || outNameSize <= 0) {
        return;
    }

    s32 safeIndex = checkpointIndex;
    if (safeIndex < 0) {
        safeIndex = 0;
    }

    u32 safePetal = petalIndex;
    if (safePetal > 25u) {
        safePetal = 0;
    }

    const char petalLetter = static_cast<char>('a' + safePetal);
    std::snprintf(outName, static_cast<size_t>(outNameSize), "goto%c%d", petalLetter, safeIndex);
}

static void BuildSelectedCheckpointName(u32 targetPetalIndex, char* outName, s32 outNameSize) {
    if (!outName || outNameSize <= 0) {
        return;
    }

    if (sDebugLevelSelectorUseCustomCheckpointName && sDebugLevelSelectorCustomCheckpointName[0] != '\0') {
        std::snprintf(outName, static_cast<size_t>(outNameSize), "%s", sDebugLevelSelectorCustomCheckpointName);
        return;
    }

    BuildAutoCheckpointName(targetPetalIndex, sDebugLevelSelectorCheckpointIndex, outName, outNameSize);
}

static void QueueDebugCheckpointWarp(u32 targetLevelIndex, u32 targetPetalIndex, const char* checkpointName) {
    sDebugPendingCheckpointWarp.active = false;
    sDebugPendingCheckpointWarp.targetLevelIndex = targetLevelIndex;
    sDebugPendingCheckpointWarp.targetPetalIndex = targetPetalIndex;
    sDebugPendingCheckpointWarp.retryFrames = 0;

    const char* name = (checkpointName && checkpointName[0] != '\0') ? checkpointName : "gotoa1";
    std::snprintf(
        sDebugPendingCheckpointWarp.checkpointName,
        static_cast<size_t>(sizeof(sDebugPendingCheckpointWarp.checkpointName)),
        "%s",
        name);
    sDebugPendingCheckpointWarp.checkpointCRC = p3dHash(sDebugPendingCheckpointWarp.checkpointName);
    sDebugPendingCheckpointWarp.active = true;
}

static void ProcessPendingCheckpointWarp() {
    if (!sDebugPendingCheckpointWarp.active) {
        return;
    }

    if (!g_game) {
        return;
    }

    World* world = g_game->GetWorld();
    Player* player = Player::s_player;
    if (!world || !player) {
        return;
    }

    if (world->GetCurrentLevelIndex() != sDebugPendingCheckpointWarp.targetLevelIndex ||
        world->GetCurrentPetalIndex() != sDebugPendingCheckpointWarp.targetPetalIndex) {
        return;
    }

    const GameState gameState = g_game->GetState();
    if (gameState != GameState::PrePlay && gameState != GameState::Play) {
        return;
    }

    WorldPointNode* checkpointPoint = WorldPoints_GetNISPoint(sDebugPendingCheckpointWarp.checkpointCRC);
    if (!checkpointPoint) {
        sDebugPendingCheckpointWarp.retryFrames++;
        if (sDebugPendingCheckpointWarp.retryFrames > 180) {
            LOG(
                "[DebugUI] Checkpoint warp failed: point='%s' crc=0x%08X not found in levelIndex=%u petal=%u",
                sDebugPendingCheckpointWarp.checkpointName,
                sDebugPendingCheckpointWarp.checkpointCRC,
                sDebugPendingCheckpointWarp.targetLevelIndex,
                sDebugPendingCheckpointWarp.targetPetalIndex);
            sDebugPendingCheckpointWarp.active = false;
        }
        return;
    }

    player->pos = checkpointPoint->pos;
    player->homePos = checkpointPoint->pos;
    player->UpdatePosition();

    s32 checkpointBlockNum = checkpointPoint->parValue;
    if (g_blockManager) {
        const u16 resolvedBlock = g_blockManager->GetBlockNumber(checkpointPoint->pos);
        if (resolvedBlock != BLOCK_UNASSIGNED) {
            checkpointBlockNum = static_cast<s32>(resolvedBlock);
        }
    }

    if (checkpointBlockNum >= 0 && checkpointBlockNum <= 0xFFFF) {
        player->blockNum = static_cast<u16>(checkpointBlockNum);
    }

    player->checkpoint.field0 = checkpointPoint->pos.x;
    player->checkpoint.field4 = checkpointPoint->pos.y;
    player->checkpoint.field8 = checkpointPoint->pos.z;
    player->checkpoint.field12 = player->orientation.x;
    player->checkpoint.field16 = player->orientation.y;
    player->checkpoint.field20 = player->orientation.z;
    player->checkpoint.field24 = (checkpointBlockNum >= 0) ? checkpointBlockNum : static_cast<s32>(player->blockNum);
    player->checkpoint.field28 = 0;
    player->checkpoint.SetValidState(1);
    player->checkpoint.levelIndex = static_cast<s32>(sDebugPendingCheckpointWarp.targetLevelIndex);
    player->checkpoint.petalIndex = static_cast<s32>(sDebugPendingCheckpointWarp.targetPetalIndex);

    if (g_display && g_display->GetCamera()) {
        g_display->GetCamera()->SetLookAtTarget(player, 1);
    }

    LOG(
        "[DebugUI] Applied checkpoint warp: point='%s' crc=0x%08X pos=(%d,%d,%d) block=%d levelIndex=%u petal=%u",
        sDebugPendingCheckpointWarp.checkpointName,
        sDebugPendingCheckpointWarp.checkpointCRC,
        checkpointPoint->pos.x,
        checkpointPoint->pos.y,
        checkpointPoint->pos.z,
        checkpointBlockNum,
        sDebugPendingCheckpointWarp.targetLevelIndex,
        sDebugPendingCheckpointWarp.targetPetalIndex);

    sDebugPendingCheckpointWarp.active = false;
}

static void RefreshPlayerDrunkenMasterState() {
    if (!g_scoreManager || !g_characterManager) {
        return;
    }

    const s32 desiredMeshType = g_scoreManager->IsDrunkenMasterSuitEnabled() ? 1 : 0;
    if (!g_characterManager->IsCharacterLoaded(0)) {
        return;
    }

    Player* player = Player::s_player;
    Model* model = (player && player->model) ? static_cast<Model*>(player->model) : nullptr;
    AnimStructure* anim = model ? static_cast<AnimStructure*>(model->animStructure) : nullptr;
    const s32 animEnum = anim ? anim->animEnum : 0;
    const s32 loopType = anim ? anim->loopTypeField : ANIM_LOOP;

    if (desiredMeshType != *GetPlayerMeshType()) {
        g_characterManager->ReloadCharacter(0, desiredMeshType, nullptr);
    }
    else {
        g_characterManager->LoadCharTexture(0);
    }

    if (!model) {
        return;
    }

    SModel* sModel = static_cast<SModel*>(model);
    if (sModel) {
        sModel->SetupModelCallbacks();
    }

    AnimStructure* updatedAnim = model->animStructure ? static_cast<AnimStructure*>(model->animStructure) : nullptr;
    if (updatedAnim) {
        updatedAnim->ReAttachTree(0, animEnum);
    }

    model->ApplyAnimToModel(0, animEnum, loopType, 0, 0);
}

static const char* StateDispatchName(u16 d) {
    switch (d) {
        case SD_NONE: return "None";
        case SD_STAND: return "Stand";
        case SD_DIVE_ROLL: return "DiveRoll";
        case SD_PAUSE: return "Pause";
        case SD_RUN: return "Run";
        case SD_STRAFE: return "Strafe";
        case SD_DIVE_ROLL_CHAIN: return "StrafeChain";
        case SD_JUMP: return "Jump";
        case SD_FALL: return "Fall";
        case SD_GOT_HIT_HIGH: return "GotHitHigh";
        case SD_GOT_HIT_MED: return "GotHitMed";
        case SD_GOT_HIT_LOW: return "GotHitLow";
        case SD_WALLJUMP: return "WallJump";
        case SD_COLLAPSE: return "Collapse";
        case SD_DEAD: return "Dead";
        case SD_SPIN_BACK: return "SpinBack";
        case SD_FLYING_BACK: return "FlyingBack";
        case SD_STUNNED: return "Stunned";
        case SD_THROW: return "Throw";
        case SD_GOT_HIT_FREEFORM: return "GotHitFreeForm";
        case SD_GET_UP: return "GetUp";
        case SD_HORIZONTAL_POLE: return "HorizontalPole";
        case SD_SLOPE_SLIDE: return "SlopeSlide";
        case SD_DEAD_PLAYER: return "DeadPlayer";
        case SD_CLIMB_LADDER: return "ClimbLadder";
        case SD_LADDER_DISMOUNT: return "LadderDismount";
        case SD_HARDFALL: return "HardFall";
        case SD_HARDLAND: return "HardLand";
        case SD_FLIP: return "Flip";
        case SD_INACTIVE_IDLE: return "InactiveIdle";
        case SD_PUSH_OBJECT: return "PushObject";
        case SD_TABLE_ROLL: return "TableRoll";
        case SD_LEDGE_LATCH: return "LedgeLatch";
        case SD_LEDGE_PULLUP: return "LedgePullup";
        case SD_DO_STAND: return "DoStand";
        default: return "Unknown";
    }
}

static const char* AnimLoadStateName(s32 s) {
    switch (s) {
        case 0: return "Stopped";
        case 1: return "Playing";
        case 2: return "Paused";
        default: return "Unknown";
    }
}

static const char* AnimLoopTypeName(s32 t) {
    switch (t) {
        case ANIM_LOOP: return "Loop";
        case ANIM_LOOP_REVERSE: return "LoopReverse";
        case ANIM_RUN_TO_LAST: return "RunToLast";
        case ANIM_HOLD_FIRST: return "HoldFirst";
        case ANIM_HOLD_LAST: return "HoldLast";
        case ANIM_BLEND: return "Blend";
        case ANIM_DEC_FRAME: return "DecFrame";
        case ANIM_BLEND2: return "Blend2";
        case ANIM_STOP: return "Stop";
        default: return "Unknown";
    }
}

static const char* CameraModeName(CameraMode m) {
    switch (m) {
        case CAM_MODE_DEFAULT: return "Debug";
        case CAM_MODE_FOLLOW: return "Follow";
        case CAM_MODE_RIGID: return "Rigid";
        default: return "Unknown";
    }
}

static void LVectorText(const char* label, const LVector& v) {
    ImGui::Text("%s: %d, %d, %d (%.2f, %.2f, %.2f)",
                label, v.x, v.y, v.z,
                v.x / 4096.0f, v.y / 4096.0f, v.z / 4096.0f);
}

static void SubmitConsoleNote() {
    const char* text = sConsoleNoteInput;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }

    if (*text == '\0') {
        sConsoleNoteInput[0] = '\0';
        return;
    }

    if (g_time) {
        LOG("[ConsoleNote][frame=%u] %s", g_time->GetFrameCounter(), text);
    }
    else {
        LOG("[ConsoleNote] %s", text);
    }

    std::snprintf(sLastConsoleNote, sizeof(sLastConsoleNote), "%s", text);
    sConsoleNoteInput[0] = '\0';
}

static bool BuildDebugParticleSpawnPos(LVector* outPos) {
    if (!outPos || !Player::s_player) {
        return false;
    }

    const Player* player = Player::s_player;
    *outPos = player->pos;

    const s16 angle = static_cast<s16>(player->orientation.y);
    const s32 sinY = rmSin16(angle);
    const s32 cosY = rmSin16(static_cast<s16>(angle + 0x4000));

    outPos->x += static_cast<s32>((static_cast<s64>(sinY) * sDebugParticleSpawnDistance) >> 16);
    outPos->z += static_cast<s32>((static_cast<s64>(cosY) * sDebugParticleSpawnDistance) >> 16);
    outPos->y += sDebugParticleSpawnHeight;
    return true;
}

static const char* DebugParticleNameByHash(u32 hash) {
    const char* runtimeName = ParticleSystem_GetNameByHash(hash);
    if (runtimeName && runtimeName[0] != '\0') {
        return runtimeName;
    }

    for (s32 i = 0; i < IM_ARRAYSIZE(kDebugParticleChoices); i++) {
        if (kDebugParticleChoices[i].hash == hash) {
            return kDebugParticleChoices[i].label;
        }
    }

    return nullptr;
}

static const char* DebugEffectTypeName(s32 effectType) {
    switch (effectType) {
        case 1: return "WEffect";
        case 2: return "PWEffect";
        case 3: return "FPWEffect";
        case 4: return "FW/GEffect";
        case 5: return "LensFlare";
        case 6: return "CBVEffect";
        case 7: return "SpotLight";
        default: return "Effect";
    }
}

static void BuildDebugEffectLabel(char* outLabel, s32 outSize, const Effects* effect, bool detailed) {
    if (!outLabel || outSize <= 0 || !effect) {
        return;
    }

    outLabel[0] = '\0';

    const char* effectTypeName = DebugEffectTypeName(effect->effectType);
    const char* effectName = effect->GetName();
    const char* particleName = DebugParticleNameByHash(effect->nameCRC);

    u32 sourceHash = 0;
    u32 geoHash = 0;

    if (effect->effectType == 1 || effect->effectType == 4 || effect->effectType == 5 || effect->effectType == 7) {
        sourceHash = WEffect_DebugGetComEffectResourceHash(effect);
        geoHash = WEffect_DebugGetComEffectGeoHash(effect);
    }
    else if (effect->effectType == 2 || effect->effectType == 3) {
        sourceHash = PWEffect_DebugGetParticleSystemHash(effect);
    }

    if (!detailed) {
        if (particleName && particleName[0] != '\0') {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "%s [%s type=%d]",
                          particleName,
                          effectTypeName,
                          effect->effectType);
            return;
        }

        if (effectName && effectName[0] != '\0') {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "%s [%s type=%d]",
                          effectName,
                          effectTypeName,
                          effect->effectType);
            return;
        }

        if (effect->nameCRC != 0) {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "0x%08X [%s type=%d]",
                          effect->nameCRC,
                          effectTypeName,
                          effect->effectType);
            return;
        }

        std::snprintf(outLabel,
                      static_cast<size_t>(outSize),
                      "%s type=%d",
                      effectTypeName,
                      effect->effectType);
        return;
    }

    if (particleName && particleName[0] != '\0' && effectName && effectName[0] != '\0') {
        if ((effect->effectType == 1 || effect->effectType == 4 || effect->effectType == 5 || effect->effectType == 7) && sourceHash != 0) {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "%s | %s | %s(%d) crc=0x%08X blk=%d res=0x%08X geo=0x%08X",
                          particleName,
                          effectName,
                          effectTypeName,
                          effect->effectType,
                          effect->nameCRC,
                          effect->blockNum,
                          sourceHash,
                          geoHash);
            return;
        }

        if ((effect->effectType == 2 || effect->effectType == 3) && sourceHash != 0) {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "%s | %s | %s(%d) crc=0x%08X blk=%d psys=0x%08X",
                          particleName,
                          effectName,
                          effectTypeName,
                          effect->effectType,
                          effect->nameCRC,
                          effect->blockNum,
                          sourceHash);
            return;
        }

        std::snprintf(outLabel,
                      static_cast<size_t>(outSize),
                      "%s | %s | %s(%d) crc=0x%08X blk=%d",
                      particleName,
                      effectName,
                      effectTypeName,
                      effect->effectType,
                      effect->nameCRC,
                      effect->blockNum);
        return;
    }

    if (particleName && particleName[0] != '\0') {
        if ((effect->effectType == 1 || effect->effectType == 4 || effect->effectType == 5 || effect->effectType == 7) && sourceHash != 0) {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "%s | %s(%d) crc=0x%08X blk=%d res=0x%08X geo=0x%08X",
                          particleName,
                          effectTypeName,
                          effect->effectType,
                          effect->nameCRC,
                          effect->blockNum,
                          sourceHash,
                          geoHash);
            return;
        }

        if ((effect->effectType == 2 || effect->effectType == 3) && sourceHash != 0) {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "%s | %s(%d) crc=0x%08X blk=%d psys=0x%08X",
                          particleName,
                          effectTypeName,
                          effect->effectType,
                          effect->nameCRC,
                          effect->blockNum,
                          sourceHash);
            return;
        }

        std::snprintf(outLabel,
                      static_cast<size_t>(outSize),
                      "%s | %s(%d) crc=0x%08X blk=%d",
                      particleName,
                      effectTypeName,
                      effect->effectType,
                      effect->nameCRC,
                      effect->blockNum);
        return;
    }

    if (effectName && effectName[0] != '\0') {
        if ((effect->effectType == 1 || effect->effectType == 4 || effect->effectType == 5 || effect->effectType == 7) && sourceHash != 0) {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "%s | %s(%d) crc=0x%08X blk=%d res=0x%08X geo=0x%08X",
                          effectName,
                          effectTypeName,
                          effect->effectType,
                          effect->nameCRC,
                          effect->blockNum,
                          sourceHash,
                          geoHash);
            return;
        }

        if ((effect->effectType == 2 || effect->effectType == 3) && sourceHash != 0) {
            std::snprintf(outLabel,
                          static_cast<size_t>(outSize),
                          "%s | %s(%d) crc=0x%08X blk=%d psys=0x%08X",
                          effectName,
                          effectTypeName,
                          effect->effectType,
                          effect->nameCRC,
                          effect->blockNum,
                          sourceHash);
            return;
        }

        std::snprintf(outLabel,
                      static_cast<size_t>(outSize),
                      "%s | %s(%d) crc=0x%08X blk=%d",
                      effectName,
                      effectTypeName,
                      effect->effectType,
                      effect->nameCRC,
                      effect->blockNum);
        return;
    }

    if ((effect->effectType == 1 || effect->effectType == 4 || effect->effectType == 5 || effect->effectType == 7) && sourceHash != 0) {
        std::snprintf(outLabel,
                      static_cast<size_t>(outSize),
                      "crc=0x%08X | %s(%d) blk=%d res=0x%08X geo=0x%08X",
                      effect->nameCRC,
                      effectTypeName,
                      effect->effectType,
                      effect->blockNum,
                      sourceHash,
                      geoHash);
        return;
    }

    if ((effect->effectType == 2 || effect->effectType == 3) && sourceHash != 0) {
        std::snprintf(outLabel,
                      static_cast<size_t>(outSize),
                      "crc=0x%08X | %s(%d) blk=%d psys=0x%08X",
                      effect->nameCRC,
                      effectTypeName,
                      effect->effectType,
                      effect->blockNum,
                      sourceHash);
        return;
    }

    std::snprintf(outLabel,
                  static_cast<size_t>(outSize),
                  "crc=0x%08X | %s(%d) blk=%d",
                  effect->nameCRC,
                  effectTypeName,
                  effect->effectType,
                  effect->blockNum);
}

static const char* DebugParticleSpawnResultText(s32 result) {
    switch (result) {
        case 1: return "Spawned";
        case -1: return "Failed: invalid position";
        case -2: return "Failed: particle hash not loaded";
        case -3: return "Failed: spawn outside collision sectors";
        case -4: return "Failed: effect allocation";
        case -5: return "Failed: manager allocation";
        case -6: return "Failed: direction allocation";
        default: return "Failed";
    }
}

static u32 DebugUIColor(u8 r, u8 g, u8 b, u8 a) {
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

static void ClampDebugTallyPreviewValues() {
    if (sDebugTallyFightScore < 0) {
        sDebugTallyFightScore = 0;
    }
    if (sDebugTallyComboScore < 0) {
        sDebugTallyComboScore = 0;
    }
    if (sDebugTallyStyleScore < 0) {
        sDebugTallyStyleScore = 0;
    }
    if (sDebugTallyRedDragons < 0) {
        sDebugTallyRedDragons = 0;
    }
    if (sDebugTallyGoldDragons < 0) {
        sDebugTallyGoldDragons = 0;
    }
    if (sDebugTallyGrade < 0) {
        sDebugTallyGrade = 0;
    }
    if (sDebugTallyGrade > 5) {
        sDebugTallyGrade = 5;
    }
}

static const char* DebugAIThingTypeName(u16 type) {
    switch (type) {
        case AITypes::TT_PLAYER: return "Player";
        case AITypes::TT_COLLECTIBLE: return "CollectiblePickup";
        case AITypes::TT_PLATFORM: return "Platform";
        case AITypes::TT_LAUNCHER: return "Launcher";
        case AITypes::TT_CONVEYOR: return "Conveyor";
        case AITypes::TT_SLIPPERYFLOOR: return "SlipperyFloor";
        case AITypes::TT_HORIZONTALPOLE: return "HorizontalPole";
        case AITypes::TT_EXPLOSIVE: return "Explosive";
        case AITypes::TT_DESTRUCTIBLE: return "Destructible";
        case AITypes::TT_CRUSHER: return "Crusher";
        case AITypes::TT_KNOCKDOWN: return "KnockDown";
        case AITypes::TT_STACK: return "Stack";
        case AITypes::TT_KICKNROLL: return "KickNRoll";
        case AITypes::TT_DESTRUCTIBLE_DP: return "DestructibleDP";
        case AITypes::TT_UNTOUCHABLE: return "Untouchable";
        case AITypes::TT_COLLECTIBLE_OBJ: return "Collectible";
        case AITypes::TT_TRIGGERTHING: return "TriggerThing";
        case AITypes::TT_GENERATOR: return "Generator";
        case AITypes::TT_ENEMYGENERATOR: return "EnemyGenerator";
        case AITypes::TT_EXPLOSIVE_OBJ: return "ExplosiveObj";
        case AITypes::TT_BLAST: return "Blast";
        case AITypes::TT_THROWGENERATOR: return "ThrowGenerator";
        case AITypes::TT_PUSHABLE: return "Pushable";
        case AITypes::TT_DOOR: return "Door";
        case AITypes::TT_TELEPORTER: return "Teleporter";
        case AITypes::TT_PENDULUM: return "Pendulum";
        case AITypes::TT_TRAPDOOR: return "TrapDoor";
        case AITypes::TT_TABLE: return "Table";
        case AITypes::TT_BOSS: return "FrontEndVolume";
        case AITypes::TT_LADDER: return "Ladder";
        case AITypes::TT_CHAIR: return "Chair";
        case AITypes::TT_ARROW: return "Arrow";
        default:
            break;
    }

    if (type >= AITypes::TT_HUMANOID_FIRST && type <= AITypes::TT_HUMANOID_LAST) {
        return "Humanoid";
    }
    if (type >= AITypes::TT_OBSTACLE_FIRST && type <= AITypes::TT_OBSTACLE_LAST) {
        return "Pickup";
    }
    return nullptr;
}

static bool DebugAIThingTypeLikelyWIP(u16 type) {
    return type == AITypes::TT_STACK;
}

static const char* ResolveDebugThingModelName(const Thing* thing) {
    if (!thing || thing->modelHash == 0 || !g_levelManager) {
        return nullptr;
    }

    OriginalBasic* original = g_levelManager->FindModel(thing->modelHash);
    if (!original) {
        return nullptr;
    }

    const char* modelName = original->GetName();
    if (!modelName || modelName[0] == '\0') {
        return nullptr;
    }

    return modelName;
}

static void BuildDebugThingLabel(char* outLabel, s32 outSize, const Thing* thing, const char* fallbackPrefix) {
    if (!outLabel || outSize <= 0 || !thing) {
        return;
    }

    outLabel[0] = '\0';
    const char* typeName = DebugAIThingTypeName(thing->thingType);
    const char* modelName = ResolveDebugThingModelName(thing);
    const char* wipSuffix = DebugAIThingTypeLikelyWIP(thing->thingType) ? " [WIP]" : "";

    const char* name = thing->GetName();
    if (name && name[0] != '\0') {
        if (typeName && typeName[0] != '\0') {
            std::snprintf(outLabel, (size_t)outSize, "%s (%s)%s", name, typeName, wipSuffix);
        }
        else {
            std::snprintf(outLabel, (size_t)outSize, "%s", name);
        }
        return;
    }

    if (modelName && modelName[0] != '\0') {
        if (typeName && typeName[0] != '\0') {
            std::snprintf(outLabel, (size_t)outSize, "%s (%s)%s", modelName, typeName, wipSuffix);
        }
        else {
            std::snprintf(outLabel, (size_t)outSize, "%s", modelName);
        }
        return;
    }

    if (thing->nameCRC != 0) {
        if (typeName && typeName[0] != '\0') {
            std::snprintf(outLabel, (size_t)outSize, "%s 0x%08X%s", typeName, thing->nameCRC, wipSuffix);
        }
        else {
            std::snprintf(outLabel, (size_t)outSize, "%s 0x%08X", fallbackPrefix, thing->nameCRC);
        }
        return;
    }

    if (typeName && typeName[0] != '\0') {
        std::snprintf(outLabel, (size_t)outSize, "%s id:%u%s",
                      typeName,
                      (u32)thing->uniqueID,
                      wipSuffix);
        return;
    }

    std::snprintf(outLabel, (size_t)outSize, "%s type:%u id:%u",
                  fallbackPrefix,
                  (u32)thing->thingType,
                  (u32)thing->uniqueID);
}

static void ResetDebugVisibleThingLabels() {
    sDebugVisibleThingLabelCount = 0;
}

static void RegisterDebugVisibleThingLabel(Thing* thing, const char* label, f32 screenX, f32 screenY) {
    if (!thing || !label || label[0] == '\0') {
        return;
    }

    if (sDebugVisibleThingLabelCount >= MAX_DEBUG_VISIBLE_THING_LABELS) {
        return;
    }

    DebugVisibleThingLabel& entry = sDebugVisibleThingLabels[sDebugVisibleThingLabelCount++];
    entry.thing = thing;
    std::snprintf(entry.label, sizeof(entry.label), "%s", label);
    entry.screenX = screenX;
    entry.screenY = screenY;
}

static bool DebugMouseToScreenPos(f32* outX, f32* outY) {
    if (!outX || !outY || !ImGui::GetCurrentContext()) {
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) {
        return false;
    }

    f32 scaleX = io.DisplayFramebufferScale.x;
    f32 scaleY = io.DisplayFramebufferScale.y;
    if (scaleX <= 0.0f) {
        scaleX = 1.0f;
    }
    if (scaleY <= 0.0f) {
        scaleY = 1.0f;
    }

    const ImVec2 mouse = io.MousePos;
    *outX = (mouse.x - viewport->Pos.x) * scaleX;
    *outY = (mouse.y - viewport->Pos.y) * scaleY;
    return true;
}

static const DebugVisibleThingLabel* FindNearestDebugVisibleThingLabel(f32 screenX, f32 screenY, f32 maxDistancePixels) {
    if (sDebugVisibleThingLabelCount <= 0) {
        return nullptr;
    }

    const f32 maxDistSq = maxDistancePixels * maxDistancePixels;
    f32 bestDistSq = maxDistSq;
    const DebugVisibleThingLabel* best = nullptr;

    for (s32 i = 0; i < sDebugVisibleThingLabelCount; i++) {
        const DebugVisibleThingLabel& entry = sDebugVisibleThingLabels[i];
        if (!entry.thing) {
            continue;
        }

        const f32 dx = screenX - entry.screenX;
        const f32 dy = screenY - entry.screenY;
        const f32 distSq = dx * dx + dy * dy;
        if (distSq <= bestDistSq) {
            bestDistSq = distSq;
            best = &entry;
        }
    }

    return best;
}

static void DrawDebugThingHoverTooltip() {
    if (!sEnabled || !sDebugShowThingLabelHoverTooltip || !ImGui::GetCurrentContext()) {
        return;
    }

    if (sDebugVisibleThingLabelCount <= 0) {
        return;
    }

    f32 mouseScreenX = 0.0f;
    f32 mouseScreenY = 0.0f;
    if (!DebugMouseToScreenPos(&mouseScreenX, &mouseScreenY)) {
        return;
    }

    const DebugVisibleThingLabel* hovered = FindNearestDebugVisibleThingLabel(mouseScreenX, mouseScreenY, 20.0f);
    if (!hovered || !hovered->thing) {
        return;
    }

    const Thing* thing = hovered->thing;
    const char* typeName = DebugAIThingTypeName(thing->thingType);
    const char* modelName = ResolveDebugThingModelName(thing);

    ImGui::BeginTooltip();
    ImGui::Text("%s", hovered->label);
    ImGui::Separator();
    ImGui::Text("Type: %u (%s)", (u32)thing->thingType, typeName ? typeName : "Unknown");
    ImGui::Text("ID: %u", (u32)thing->uniqueID);
    ImGui::Text("Name CRC: 0x%08X", thing->nameCRC);
    ImGui::Text("Model Hash: 0x%08X", thing->modelHash);
    if (modelName && modelName[0] != '\0') {
        ImGui::Text("Model Name: %s", modelName);
    }
    ImGui::Text("Pos: %d, %d, %d", thing->pos.x, thing->pos.y, thing->pos.z);
    ImGui::EndTooltip();
}

static void DrawDebugThingLabelInspectorPanel() {
    ImGui::SeparatorText("3D Label Inspect");
    ImGui::Checkbox("Label hover tooltip", &sDebugShowThingLabelHoverTooltip);
    ImGui::Text("Visible AI labels: %d", sDebugVisibleThingLabelCount);

    if (sDebugVisibleThingLabelCount <= 0) {
        ImGui::TextDisabled("No visible AI labels in current camera view.");
        return;
    }

    s32 selectedIndex = -1;
    for (s32 i = 0; i < sDebugVisibleThingLabelCount; i++) {
        Thing* thing = sDebugVisibleThingLabels[i].thing;
        if (thing && thing->uniqueID == sDebugSelectedThingUniqueID) {
            selectedIndex = i;
            break;
        }
    }

    if (selectedIndex < 0) {
        selectedIndex = 0;
        if (sDebugVisibleThingLabels[0].thing) {
            sDebugSelectedThingUniqueID = sDebugVisibleThingLabels[0].thing->uniqueID;
        }
    }

    const DebugVisibleThingLabel& selected = sDebugVisibleThingLabels[selectedIndex];
    const char* previewLabel = selected.label[0] != '\0' ? selected.label : "<unnamed>";

    if (ImGui::BeginCombo("Visible Thing", previewLabel)) {
        for (s32 i = 0; i < sDebugVisibleThingLabelCount; i++) {
            const DebugVisibleThingLabel& entry = sDebugVisibleThingLabels[i];
            if (!entry.thing) {
                continue;
            }

            const bool isSelected = entry.thing->uniqueID == sDebugSelectedThingUniqueID;
            char itemLabel[196] = {};
            std::snprintf(itemLabel, sizeof(itemLabel), "%s##thing_%u", entry.label, (u32)entry.thing->uniqueID);

            if (ImGui::Selectable(itemLabel, isSelected)) {
                sDebugSelectedThingUniqueID = entry.thing->uniqueID;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    Thing* thing = selected.thing;
    if (!thing) {
        ImGui::TextDisabled("Selected label has no live Thing pointer.");
        return;
    }

    const char* typeName = DebugAIThingTypeName(thing->thingType);
    const char* modelName = ResolveDebugThingModelName(thing);
    const char* rawName = thing->GetName();

    ImGui::Separator();
    ImGui::Text("Label: %s", selected.label);
    ImGui::Text("Type: %u (%s)", (u32)thing->thingType, typeName ? typeName : "Unknown");
    if (DebugAIThingTypeLikelyWIP(thing->thingType)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Reversal status: WIP");
    }
    ImGui::Text("Unique ID: %u", (u32)thing->uniqueID);
    ImGui::Text("Name: %s", (rawName && rawName[0] != '\0') ? rawName : "<none>");
    ImGui::Text("Name CRC: 0x%08X", thing->nameCRC);
    ImGui::Text("Model Hash: 0x%08X", thing->modelHash);
    if (modelName && modelName[0] != '\0') {
        ImGui::Text("Model Name: %s", modelName);
    }
    ImGui::Text("Block: %u", (u32)thing->blockNum);
    ImGui::Text("Collision Radius: %u", (u32)thing->collisionRadius);
    ImGui::Text("Health: %u / %u", (u32)thing->health, (u32)thing->maxHealth);
    ImGui::Text("Flags: 0x%08X", thing->flags);
    ImGui::Text("Flags2: 0x%08X", thing->flags2);
    LVectorText("Pos", thing->pos);
    LVectorText("Orientation", thing->orientation);

    if (ImGui::Button("Log Selected Thing")) {
        LOG("[DebugUI] ThingInspect label='%s' type=%u id=%u name='%s' nameCRC=0x%08X modelHash=0x%08X modelName='%s' pos=(%d,%d,%d)",
            selected.label,
            (u32)thing->thingType,
            (u32)thing->uniqueID,
            (rawName && rawName[0] != '\0') ? rawName : "",
            thing->nameCRC,
            thing->modelHash,
            (modelName && modelName[0] != '\0') ? modelName : "",
            thing->pos.x,
            thing->pos.y,
            thing->pos.z);
    }
}

static xcFont* ResolveDebugLabelFont() {
    if (!g_oxFontFile) {
        return nullptr;
    }

    xcFont* font = g_oxFontFile->FindFont("Beats_lo");
    if (!font) {
        font = g_oxFontFile->FindFont("Beats_mid");
    }
    if (!font) {
        font = g_oxFontFile->FindFont("Red_dr");
    }
    if (!font) {
        font = g_oxFontFile->FindFont("Gold_dr");
    }
    return font;
}

static void DrawDebugLabelText(f32 x, f32 y, const char* text, u32 color, xcFont* font, ImDrawList* fallbackDrawList) {
    if (!text || text[0] == '\0') {
        return;
    }

    if (font) {
        font->DrawText(text, x, y, color, 0, 0);
        return;
    }

    if (fallbackDrawList) {
        fallbackDrawList->AddText(ImVec2(x, y), (ImU32)color, text);
    }
}

static void DrawDebug3DLabels() {
    ResetDebugVisibleThingLabels();

    if (!g_display || (!sDebugShowHumanoidNames3D && !sDebugShowAllAIThingNames3D && !sDebugShowEffectNames3D)) {
        return;
    }

    Camera* camera = g_display->GetCamera();
    if (!camera) {
        return;
    }

    xcFont* labelFont = ResolveDebugLabelFont();

    ImDrawList* fallbackDrawList = nullptr;
    f32 fallbackOffsetX = 0.0f;
    f32 fallbackOffsetY = 0.0f;
    f32 fallbackScaleX = 1.0f;
    f32 fallbackScaleY = 1.0f;

    if (!labelFont && ImGui::GetCurrentContext()) {
        fallbackDrawList = ImGui::GetForegroundDrawList();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (viewport) {
            fallbackOffsetX = viewport->Pos.x;
            fallbackOffsetY = viewport->Pos.y;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplayFramebufferScale.x > 0.0f) {
            fallbackScaleX = io.DisplayFramebufferScale.x;
        }
        if (io.DisplayFramebufferScale.y > 0.0f) {
            fallbackScaleY = io.DisplayFramebufferScale.y;
        }
    }

    if (!labelFont && !fallbackDrawList) {
        return;
    }

    const f32 prevScaleX = labelFont ? labelFont->scaleX : 1.0f;
    const f32 prevScaleY = labelFont ? labelFont->scaleY : 1.0f;
    const f32 prevWrapX = labelFont ? labelFont->wrapX : 0.0f;

    if (labelFont) {
        labelFont->SetScale(1.0f, 1.0f);
        labelFont->SetWrapX(0.0f);
    }

    auto drawLabel = [&](f32 screenX, f32 screenY, const char* text, u32 color) {
        f32 drawX = screenX;
        f32 drawY = screenY;

        if (!labelFont) {
            drawX = screenX / fallbackScaleX + fallbackOffsetX;
            drawY = screenY / fallbackScaleY + fallbackOffsetY;
        }

        DrawDebugLabelText(drawX, drawY, text, color, labelFont, fallbackDrawList);
    };

    const f32 labelYOffset = (labelFont && labelFont->lineHeight > 0)
        ? (f32)labelFont->lineHeight + 2.0f
        : 14.0f;

    auto drawThingLabel = [&](Thing* thing, u32 color, const char* fallbackPrefix) {
        if (!thing) {
            return;
        }

        f32 screenX = 0.0f;
        f32 screenY = 0.0f;
        if (!camera->WorldToScreen(thing->pos, &screenX, &screenY)) {
            return;
        }

        char label[128] = {};
        BuildDebugThingLabel(label, (s32)sizeof(label), thing, fallbackPrefix);
        const f32 labelScreenX = screenX;
        const f32 labelScreenY = screenY - labelYOffset;
        drawLabel(labelScreenX, labelScreenY, label, color);
        RegisterDebugVisibleThingLabel(thing, label, labelScreenX, labelScreenY);
    };

    if (sDebugShowAllAIThingNames3D && g_ai) {
        for (ccMinNode* node = g_ai->humanoidList.head; node; node = node->next) {
            Thing* thing = static_cast<Thing*>(static_cast<ccNode*>(node));
            drawThingLabel(thing, DebugUIColor(255, 240, 120, 255), "Humanoid");
        }

        for (ccMinNode* node = g_ai->moveList.head; node; node = node->next) {
            Thing* thing = static_cast<Thing*>(static_cast<ccNode*>(node));
            drawThingLabel(thing, DebugUIColor(120, 255, 140, 255), "Thing");
        }

        for (ccMinNode* node = g_ai->pickupList.head; node; node = node->next) {
            Thing* thing = static_cast<Thing*>(static_cast<ccNode*>(node));
            drawThingLabel(thing, DebugUIColor(255, 200, 120, 255), "Pickup");
        }
    }

    if (!sDebugShowAllAIThingNames3D && sDebugShowHumanoidNames3D && g_ai) {
        for (ccMinNode* node = g_ai->humanoidList.head; node; node = node->next) {
            Thing* thing = static_cast<Thing*>(static_cast<ccNode*>(node));
            if (!thing) {
                continue;
            }
            drawThingLabel(thing, DebugUIColor(255, 240, 120, 255), "Humanoid");
        }
    }

    if (sDebugShowEffectNames3D) {
        static constexpr s32 kMaxEffects = 1024;
        Effects* effects[kMaxEffects] = {};
        const s32 effectCount = Effects_DebugGetActive(effects, kMaxEffects);

        for (s32 i = 0; i < effectCount && i < kMaxEffects; i++) {
            Effects* effect = effects[i];
            if (!effect) {
                continue;
            }

            LVector worldPos = {};
            if (!effect->GetDebugWorldPos(&worldPos)) {
                continue;
            }

            f32 screenX = 0.0f;
            f32 screenY = 0.0f;
            if (!camera->WorldToScreen(worldPos, &screenX, &screenY)) {
                continue;
            }

            char label[256] = {};
            BuildDebugEffectLabel(label,
                                  static_cast<s32>(sizeof(label)),
                                  effect,
                                  sDebugShowDetailedEffectNames3D);

            drawLabel(screenX, screenY - labelYOffset, label, DebugUIColor(120, 220, 255, 255));
        }
    }

    if (labelFont) {
        labelFont->SetScale(prevScaleX, prevScaleY);
        labelFont->SetWrapX(prevWrapX);
    }
}

void DebugUI::Init() {}

bool DebugUI::IsEnabled() {
    return sEnabled;
}

bool DebugUI::IsHumanoidDamageDisabled() {
    return sDisableHumanoidDamage;
}

void DebugUI::Draw() {
    if (ImGui::IsKeyPressed(ImGuiKey_M, false) && ImGui::GetIO().KeyCtrl) {
        sEnabled = !sEnabled;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && ImGui::GetIO().KeyCtrl) {
        g_drawDebugConsole = !g_drawDebugConsole;
    }

    ApplyCursorPolicy();

    if (ImGui::IsKeyPressed(ImGuiKey_B, false) && ImGui::GetIO().KeyCtrl) {
        Camera* cam = g_display->GetCamera();

        if (cam->GetMode() == CAM_MODE_DEFAULT) {
            cam->SetMode(CAM_MODE_FOLLOW);
            sInputRoutingOverride = false;
        }
        else {
            cam->SetMode(CAM_MODE_DEFAULT);
            sInputRoutingOverride = true;
        }
    }

    DrawDebug3DLabels();

    ProcessPendingCheckpointWarp();

    if (!sEnabled) {
        sDebugTallyPreviewEnabled = false;
        g_customHudMgr.SetDebugTallyPreviewEnabled(false);
        return;
    }

    ClampDebugTallyPreviewValues();
    g_customHudMgr.SetDebugTallyPreviewEnabled(sDebugTallyPreviewEnabled);
    if (sDebugTallyPreviewEnabled) {
        g_customHudMgr.SetDebugTallyPreviewValues(
            sDebugTallyFightScore,
            sDebugTallyComboScore,
            sDebugTallyStyleScore,
            sDebugTallyRedDragons,
            sDebugTallyGoldDragons,
            sDebugTallyGrade,
            sDebugTallyShowMovieBonus);
    }

    DrawDebugThingHoverTooltip();

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem("Game", nullptr, &sShowGame);
            ImGui::MenuItem("Player", nullptr, &sShowPlayer);
            ImGui::MenuItem("Particles", nullptr, &sShowParticles);
            ImGui::MenuItem("Effects", nullptr, &sShowEffects);
            ImGui::MenuItem("Debugging", nullptr, &sShowDebugging);
#if MODERN_GRAPHICS
            ImGui::MenuItem("Shadows (CSM)", nullptr, &sShowShadows);
#endif
            ImGui::MenuItem("Camera", nullptr, &sShowCamera);
            ImGui::MenuItem("Animation", nullptr, &sShowAnimation);
            ImGui::MenuItem("Audio", nullptr, &sShowAudio);
            ImGui::MenuItem("Console Notes", nullptr, &sShowConsoleNotes);
            ImGui::Separator();
            ImGui::MenuItem("Asset Exporter", nullptr, &sShowAssetExporter);
            ImGui::MenuItem("Mods", nullptr, &sShowMods);
#if REAL_TEXTURE_RENDERING
            {
                bool realTextureMode = p3d::context && p3d::context->IsRealTextureModeEnabled();
                if (ImGui::MenuItem("Real Textures", nullptr, &realTextureMode)) {
                    if (p3d::context) p3d::context->SetRealTextureMode(realTextureMode);
                }
            }
#endif
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &sShowImGuiDemo);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (sShowGame && g_game) {
        if (ImGui::Begin("Game", &sShowGame)) {
            ImGui::Text("State: %s (%d)", GameStateName(g_game->GetState()), (s32)g_game->GetState());
            ImGui::Text("Prev State: %s (%d)", GameStateName(g_game->GetPrevState()), (s32)g_game->GetPrevState());
            if (g_time) {
                ImGui::Separator();
                ImGui::Text("Target Dt: %.4f", g_time->GetTargetDt());
            }
            if (g_scoreManager) {
                ImGui::Separator();
                ImGui::Text("Fight Score: %d", g_scoreManager->currentFightScore);
                ImGui::Text("Combo Score: %d", g_scoreManager->currentComboScore);
                ImGui::Text("Style Score: %d", g_scoreManager->currentStyleScore);
                ImGui::Text("Grade: %d", g_scoreManager->currentGrade);
                ImGui::Text("Gold Dragons: %d", g_scoreManager->currentGoldDragons);
                ImGui::Text("Collectibles: %d", g_scoreManager->collectibleCount);

                ImGui::SeparatorText("Level Unlock");
                for (s32 level = 0; level < 7; level++) {
                    bool unlocked = g_scoreManager->petalStats[level * 3].fightScore >= -1;
                    char label[32];
                    std::snprintf(label, sizeof(label), "Level %d", level + 1);
                    if (ImGui::Checkbox(label, &unlocked)) {
                        if (unlocked) {
                            g_scoreManager->petalStats[level * 3].fightScore = -1;
                        } else {
                            g_scoreManager->petalStats[level * 3].fightScore = -2;
                        }
                    }
                }
                if (ImGui::Button("Unlock All Levels")) {
                    g_scoreManager->OpenAllLevels();
                    g_scoreManager->GiveAllDragons();
                }
                ImGui::SameLine();
                if (ImGui::Button("Lock All Levels")) {
                    for (s32 level = 0; level < 7; level++) {
                        for (s32 petal = 0; petal < 3; petal++) {
                            g_scoreManager->petalStats[level * 3 + petal].fightScore = -2;
                        }
                    }
                    g_scoreManager->petalStats[0].fightScore = -1;

                    // Keep hub gate state in sync with newly-locked progression.
                    if (g_ai) {
                        for (ccMinNode* n = g_ai->moveList.head; n; n = n->next) {
                            Thing* thing = static_cast<Thing*>(static_cast<ccNode*>(n));
                            if (thing->thingType == AITypes::TT_DOOR ||
                                thing->thingType == AITypes::TT_TELEPORTER ||
                                thing->thingType == AITypes::TT_TRAPDOOR ||
                                thing->thingType == AITypes::TT_PLATFORM ||
                                thing->thingType == AITypes::TT_LADDER) {
                                thing->Reset();
                            }
                        }
                    }
                }

                ImGui::SeparatorText("Hub Gates");
                if (ImGui::Button("Open Unlocked Gates")) {
                    if (g_feMenuMgr) {
                        g_feMenuMgr->OpenDoors();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset All Gates")) {
                    if (g_ai) {
                        for (ccMinNode* n = g_ai->moveList.head; n; n = n->next) {
                            Thing* thing = static_cast<Thing*>(static_cast<ccNode*>(n));
                            if (thing->thingType == AITypes::TT_DOOR ||
                                thing->thingType == AITypes::TT_TELEPORTER ||
                                thing->thingType == AITypes::TT_TRAPDOOR ||
                                thing->thingType == AITypes::TT_PLATFORM ||
                                thing->thingType == AITypes::TT_LADDER) {
                                thing->Reset();
                            }
                        }
                    }
                }
            }

            ImGui::SeparatorText("Level Selector");
            World* world = g_game->GetWorld();
            if (!world) {
                ImGui::Text("World: null");
            }
            else {
                const s32 entryCount = (s32)(sizeof(kDebugLevelSelectorEntries) / sizeof(kDebugLevelSelectorEntries[0]));
                if ((u32)sDebugLevelSelectorChoice >= (u32)entryCount) {
                    sDebugLevelSelectorChoice = 0;
                }

                char previewLabel[192] = {};
                {
                    const DebugLevelSelectorEntry& selectedEntry = kDebugLevelSelectorEntries[sDebugLevelSelectorChoice];
                    const bool selectedResolved = ResolveDebugLevelSelectorEntry(
                        world,
                        sDebugLevelSelectorChoice,
                        nullptr,
                        nullptr);

                    if (selectedResolved) {
                        std::snprintf(
                            previewLabel,
                            sizeof(previewLabel),
                            "%s [ID %d, petal %d]",
                            selectedEntry.label,
                            selectedEntry.levelID,
                            selectedEntry.petalIndex);
                    }
                    else {
                        std::snprintf(
                            previewLabel,
                            sizeof(previewLabel),
                            "%s [ID %d, petal %d, unavailable]",
                            selectedEntry.label,
                            selectedEntry.levelID,
                            selectedEntry.petalIndex);
                    }
                }

                if (ImGui::BeginCombo("Target", previewLabel)) {
                    for (s32 i = 0; i < entryCount; i++) {
                        const DebugLevelSelectorEntry& entry = kDebugLevelSelectorEntries[i];
                        const bool selected = (i == sDebugLevelSelectorChoice);

                        u32 levelIndex = 0;
                        const bool resolved = ResolveDebugLevelSelectorEntry(world, i, &levelIndex, nullptr);

                        char itemLabel[192] = {};
                        if (resolved) {
                            std::snprintf(
                                itemLabel,
                                sizeof(itemLabel),
                                "%s [ID %d, petal %d]",
                                entry.label,
                                entry.levelID,
                                entry.petalIndex);
                        }
                        else {
                            std::snprintf(
                                itemLabel,
                                sizeof(itemLabel),
                                "%s [ID %d, petal %d, unavailable]",
                                entry.label,
                                entry.levelID,
                                entry.petalIndex);
                        }

                        if (!resolved) {
                            ImGui::BeginDisabled();
                        }

                        if (ImGui::Selectable(itemLabel, selected)) {
                            sDebugLevelSelectorChoice = i;
                        }

                        if (!resolved) {
                            ImGui::EndDisabled();
                        }
                    }

                    ImGui::EndCombo();
                }

                u32 targetLevelIndex = 0;
                u32 targetPetalIndex = 0;
                const bool targetResolved = ResolveDebugLevelSelectorEntry(
                    world,
                    sDebugLevelSelectorChoice,
                    &targetLevelIndex,
                    &targetPetalIndex);

                if (targetResolved) {
                    const DebugLevelSelectorEntry& selectedEntry = kDebugLevelSelectorEntries[sDebugLevelSelectorChoice];
                    ImGui::Text(
                        "Resolved: %s [ID %d] -> index %u, petal %u",
                        selectedEntry.label,
                        selectedEntry.levelID,
                        targetLevelIndex,
                        targetPetalIndex);

                    ImGui::Checkbox("Spawn at checkpoint after load", &sDebugLevelSelectorSpawnAtCheckpoint);
                    if (sDebugLevelSelectorSpawnAtCheckpoint) {
                        if (sDebugLevelSelectorCheckpointIndex < 0) {
                            sDebugLevelSelectorCheckpointIndex = 0;
                        }

                        ImGui::Checkbox("Use custom point name", &sDebugLevelSelectorUseCustomCheckpointName);
                        if (sDebugLevelSelectorUseCustomCheckpointName) {
                            ImGui::InputText(
                                "Checkpoint point",
                                sDebugLevelSelectorCustomCheckpointName,
                                static_cast<size_t>(sizeof(sDebugLevelSelectorCustomCheckpointName)));
                        }
                        else {
                            ImGui::InputInt("Checkpoint index", &sDebugLevelSelectorCheckpointIndex, 1, 5);
                        }

                        char checkpointPreviewName[32] = {};
                        BuildSelectedCheckpointName(
                            targetPetalIndex,
                            checkpointPreviewName,
                            static_cast<s32>(sizeof(checkpointPreviewName)));
                        ImGui::Text(
                            "Checkpoint target: %s (crc=0x%08X)",
                            checkpointPreviewName,
                            p3dHash(checkpointPreviewName));
                    }

                    if (ImGui::Button("Load Selected Level")) {
                        world->SetTargetLevelPetal(targetLevelIndex, targetPetalIndex);
                        world->ResetLevel();

                        if (sDebugLevelSelectorSpawnAtCheckpoint) {
                            char checkpointName[32] = {};
                            BuildSelectedCheckpointName(
                                targetPetalIndex,
                                checkpointName,
                                static_cast<s32>(sizeof(checkpointName)));
                            QueueDebugCheckpointWarp(targetLevelIndex, targetPetalIndex, checkpointName);
                        }
                        else {
                            sDebugPendingCheckpointWarp.active = false;
                        }

                        g_game->SetState(GameState::QueueLevelPetalLoad);
                        LOG(
                            "[DebugUI] Queued level load: target=%s levelID=%d levelIndex=%u petal=%u checkpoint=%s",
                            selectedEntry.label,
                            selectedEntry.levelID,
                            targetLevelIndex,
                            targetPetalIndex,
                            (sDebugLevelSelectorSpawnAtCheckpoint
                                ? sDebugPendingCheckpointWarp.checkpointName
                                : "<disabled>"));
                    }
                }
                else {
                    ImGui::Text("Selected level is unavailable in this world table.");
                }
            }

            ImGui::SeparatorText("HUD Tally Preview");
            ImGui::Checkbox("Force tally preview overlay", &sDebugTallyPreviewEnabled);
            ImGui::TextDisabled("Uses synthetic values to run tally count-up animation without finishing a level.");

            if (sDebugTallyPreviewEnabled) {
                ImGui::InputInt("Fight score", &sDebugTallyFightScore, 100, 1000);
                ImGui::InputInt("Combo score", &sDebugTallyComboScore, 100, 1000);
                ImGui::InputInt("Style score", &sDebugTallyStyleScore, 100, 1000);
                ImGui::InputInt("Red dragons", &sDebugTallyRedDragons, 1, 5);
                ImGui::InputInt("Gold dragons", &sDebugTallyGoldDragons, 1, 5);
                ImGui::InputInt("Grade (0..5)", &sDebugTallyGrade, 1, 1);
                ImGui::Checkbox("Show movie bonus text", &sDebugTallyShowMovieBonus);

                ClampDebugTallyPreviewValues();

                if (ImGui::Button("Replay tally animation")) {
                    g_customHudMgr.RestartDebugTallyPreview();
                }
            }
        }
        ImGui::End();
    }

    if (sShowPlayer) {
        Player* p = Player::s_player;
        if (ImGui::Begin("Player", &sShowPlayer)) {
            if (p) {
                Model* m = p->model ? static_cast<Model*>(p->model) : nullptr;
                AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
                u32 cb = (u32)p->commandBits;

                ImGui::SeparatorText("Position");
                LVectorText("Pos", p->pos);
                LVectorText("Orientation", p->orientation);
                LVectorText("Velocity", p->velocity);

                ImGui::SeparatorText("State");
                ImGui::Text("Action State: %s (%d)", ActionStateName(p->actionState), p->actionState);
                ImGui::Text("Attack Joint: %d", p->attackJointIndex);
                ImGui::Text("Prev Joint: %d", p->prevAttackJointIndex);
                ImGui::Text("Dispatch: %s (%u)", StateDispatchName(p->stateDispatch), (u32)p->stateDispatch);
                ImGui::Text("Face Angle: %d", p->faceAngle);
                ImGui::Text("Orientation Y: %d", p->orientation.y);
                ImGui::Text("On Ground: %s", (p->flags & TF_ON_GROUND) ? "yes" : "no");

                ImGui::SeparatorText("Input Bits");
                ImGui::Text("Command Bits: 0x%08X", cb);
                ImGui::Text("Run:%d Jump:%d Guard:%d Strafe:%d", (cb >> 2) & 1, (cb >> 3) & 1, (cb >> 4) & 1, (cb >> 5) & 1);
                ImGui::Text("Strafe2:%d Attack:%d Pickup:%d", (cb >> 6) & 1, (cb >> 7) & 1, (cb >> 15) & 1);

                ImGui::SeparatorText("Stats");
                ImGui::Text("Health: %d", p->health);
                ImGui::Text("Lives: %d", p->livesLeft);
                ImGui::Text("Combo: %d (timer: %d)", p->hitCombo, p->comboTimer);
                ImGui::Text("Anim Requested: %d", p->currentAnimEnum);
                ImGui::Text("Anim Load State: %s (%d)", AnimLoadStateName(p->animLoadState), p->animLoadState);
                ImGui::Text("Jump Phase: %d", p->field700);
                ImGui::Text("Force Accum: %d", p->forceAccum);
                ImGui::Text("Idle Timer: %d", p->idleTimer);
                ImGui::Text("Turn Flag/Timer: %d / %d", p->turnAroundFlag, p->turnAroundTimer);
                ImGui::Text("Flags: 0x%04X", p->flags);
                ImGui::Text("Flags2: 0x%04X", p->flags2);
                ImGui::Text("Player Flags: 0x%08X", p->playerFlags);

                ImGui::SeparatorText("Drunken Master");
                if (g_scoreManager) {
                    ImGui::Text("Total Gold Dragons: %d", g_scoreManager->GetTotalGoldDragon());
                    ImGui::Text("Permanent Latch: %s", g_scoreManager->drunkenMasterUnlocked ? "yes" : "no");
                    ImGui::Text("Suit Enabled: %s", g_scoreManager->IsDrunkenMasterSuitEnabled() ? "yes" : "no");
                    ImGui::Text("Player Mesh Type: %d", *GetPlayerMeshType());

                    bool drunkenMasterLatched = g_scoreManager->drunkenMasterUnlocked != 0;
                    if (ImGui::Checkbox("Latch Drunken Master Unlock", &drunkenMasterLatched)) {
                        g_scoreManager->drunkenMasterUnlocked = drunkenMasterLatched ? 1 : 0;
                        RefreshPlayerDrunkenMasterState();
                    }

                    if (ImGui::Button("Reload Player Suit")) {
                        RefreshPlayerDrunkenMasterState();
                    }
                }
                else {
                    ImGui::Text("ScoreManager: null");
                }

                ImGui::SeparatorText("Animation Runtime");

                if (anim) {
                    ImGui::Text("Anim Current: %d", anim->animEnum);
                    ImGui::Text("Loop Type: %s (%d)", AnimLoopTypeName(anim->loopTypeField), anim->loopTypeField);
                    ImGui::Text("Loop Count: %d", anim->loopCount);
                    ImGui::Text("Frame: %d / %d", anim->currentFrame >> 16, anim->endFrame >> 16);
                    ImGui::Text("Raw Frame: %d / %d", anim->currentFrame, anim->endFrame);
                    ImGui::Text("Speed: %.3f (raw: %d)", anim->speed / 65536.0f, anim->speed);
                    ImGui::Text("Ticks prev/cur: %d / %d", anim->prevTick, anim->currentTick);
                }
                else {
                    ImGui::Text("AnimStructure: null");
                }

                ImGui::SeparatorText("Humanoid Model Switcher");
                {
                    // If a level reload cleared the NPC slot under us, reset stale state
                    if (p->debugModelCharType != 0 && g_characterManager) {
                        bool slotStillExists = false;
                        for (s32 si = 1; si < CHAR_MAX_SLOTS; si++) {
                            if (g_characterManager->slots[si].thingType == p->debugModelCharType) {
                                slotStillExists = true;
                                break;
                            }
                        }
                        if (!slotStillExists) {
                            p->debugModelCharType = 0;
                            sDebugHumanoidLoadedType = 0;
                        }
                    }

                    // Dropdown over all 28 humanoid types (indices 1..28 in g_charNameTable)
                    static constexpr s32 kHumanoidTypeCount = (s32)(AITypes::TT_HUMANOID_LAST - AITypes::TT_HUMANOID_FIRST + 1);
                    if (sDebugHumanoidSwitcherIndex < 0 || sDebugHumanoidSwitcherIndex >= kHumanoidTypeCount) {
                        sDebugHumanoidSwitcherIndex = 0;
                    }

                    const u32 selectedType = (u32)(AITypes::TT_HUMANOID_FIRST + sDebugHumanoidSwitcherIndex);
                    const char* selectedName = g_charNameTable[selectedType];

                    if (ImGui::BeginCombo("Humanoid Type", selectedName)) {
                        for (s32 i = 0; i < kHumanoidTypeCount; i++) {
                            const u32 t = (u32)(AITypes::TT_HUMANOID_FIRST + i);
                            const bool isSel = (i == sDebugHumanoidSwitcherIndex);
                            char itemBuf[48] = {};
                            std::snprintf(itemBuf, sizeof(itemBuf), "[%u] %s", t, g_charNameTable[t]);
                            if (ImGui::Selectable(itemBuf, isSel)) {
                                sDebugHumanoidSwitcherIndex = i;
                            }
                            if (isSel) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    if (ImGui::Button("Apply Model") && g_characterManager && g_levelManager) {
                        // Unload previously debug-loaded type to free its NPC slot.
                        // Restore Jackie drawable+anims first so nothing points at freed memory.
                        if (sDebugHumanoidLoadedType != 0 && sDebugHumanoidLoadedType != selectedType) {
                            OriginalSTree* jackieOriginal = static_cast<OriginalSTree*>(g_characterManager->slots[0].model);
                            SModel* sm = static_cast<SModel*>(p->model);
                            if (sm && jackieOriginal) {
                                p->debugModelCharType = 0;
                                sm->SetOriginalSTree(jackieOriginal);
                                sm->ApplyAnimToModel(0, 0, ANIM_LOOP, 0, 0);
                            }
                            g_characterManager->UnloadCharacter(sDebugHumanoidLoadedType);
                            g_characterManager->CloseCharacter(sDebugHumanoidLoadedType);
                            sDebugHumanoidLoadedType = 0;
                        }

                        // Load if not already in a slot
                        bool inSlot = false;
                        for (s32 si = 1; si < CHAR_MAX_SLOTS; si++) {
                            if (g_characterManager->slots[si].thingType == selectedType &&
                                g_characterManager->slots[si].model) {
                                inSlot = true;
                                break;
                            }
                        }
                        if (!inSlot) {
                            g_characterManager->OpenCharacter(selectedType);
                            g_characterManager->LoadCharacter(selectedType, nullptr);
                            sDebugHumanoidLoadedType = selectedType;
                        }

                        // Grab the OriginalSTree from the NPC slot
                        OriginalSTree* original = nullptr;
                        for (s32 si = 1; si < CHAR_MAX_SLOTS; si++) {
                            if (g_characterManager->slots[si].thingType == selectedType) {
                                original = static_cast<OriginalSTree*>(g_characterManager->slots[si].model);
                                break;
                            }
                        }

                        if (original) {
                            SModel* sm = static_cast<SModel*>(p->model);
                            if (sm) {
                                p->debugModelCharType = selectedType;
                                sm->SetOriginalSTree(original);
                                sm->ApplyAnimToModel((s32)selectedType, 0, ANIM_LOOP, 0, 0);
                                LOG("[DebugUI] Switched player model+anims to humanoid type %u (%s)",
                                    selectedType, selectedName);
                            }
                        }
                        else {
                            LOG("[DebugUI] Could not find loaded OriginalSTree for humanoid type %u (%s) - all NPC slots may be full",
                                selectedType, selectedName);
                        }
                    }
                }
            }
            else {
                ImGui::Text("No player");
            }
        }
        ImGui::End();
    }

    if (sShowParticles) {
        if (ImGui::Begin("Particles", &sShowParticles)) {
            if (!Player::s_player) {
                ImGui::Text("No player");
            }
            else {
                if (sDebugParticleChoiceIndex < 0 || sDebugParticleChoiceIndex >= IM_ARRAYSIZE(kDebugParticleChoices)) {
                    sDebugParticleChoiceIndex = 0;
                }

                if (ImGui::BeginCombo("Particle Type", kDebugParticleChoices[sDebugParticleChoiceIndex].label)) {
                    for (s32 i = 0; i < IM_ARRAYSIZE(kDebugParticleChoices); i++) {
                        const bool selected = (i == sDebugParticleChoiceIndex);
                        if (ImGui::Selectable(kDebugParticleChoices[i].label, selected)) {
                            sDebugParticleChoiceIndex = i;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                const DebugParticleChoice& choice = kDebugParticleChoices[sDebugParticleChoiceIndex];
                ImGui::Text("Hash: 0x%08X", choice.hash);

                ImGui::InputInt("Spawn Distance", &sDebugParticleSpawnDistance, 64, 256);
                ImGui::InputInt("Spawn Height", &sDebugParticleSpawnHeight, 64, 256);
                ImGui::InputInt("Life Frames", &sDebugParticleLifeFrames, 1, 10);

                if (sDebugParticleSpawnDistance < 0) {
                    sDebugParticleSpawnDistance = 0;
                }
                if (sDebugParticleLifeFrames < 1) {
                    sDebugParticleLifeFrames = 1;
                }

                LVector spawnPreview = {};
                if (BuildDebugParticleSpawnPos(&spawnPreview)) {
                    ImGui::Text("Spawn Pos: %d, %d, %d", spawnPreview.x, spawnPreview.y, spawnPreview.z);
                }

                if (ImGui::Button("Spawn Particle In Front")) {
                    LVector spawnPos = {};
                    if (BuildDebugParticleSpawnPos(&spawnPos)) {
                        sDebugParticleLastSpawnResult = FPWEffect_DebugSpawnParticle(
                            choice.hash,
                            &spawnPos,
                            nullptr,
                            sDebugParticleLifeFrames);

                        LOG("[DebugUI] SpawnParticle hash=0x%08X pos=(%d,%d,%d) distance=%d life=%d result=%d",
                            choice.hash,
                            spawnPos.x,
                            spawnPos.y,
                            spawnPos.z,
                            sDebugParticleSpawnDistance,
                            sDebugParticleLifeFrames,
                            sDebugParticleLastSpawnResult);
                    }
                    else {
                        sDebugParticleLastSpawnResult = -1;
                    }
                }

                ImGui::Text("Last Spawn Result: %d (%s)",
                            sDebugParticleLastSpawnResult,
                            DebugParticleSpawnResultText(sDebugParticleLastSpawnResult));
            }
        }
        ImGui::End();
    }

    if (sShowEffects) {
        if (ImGui::Begin("Effects", &sShowEffects)) {
            static constexpr s32 kMaxListedEffects = 1024;
            static Effects* s_effectsList[kMaxListedEffects] = {};
            const s32 effectCount = Effects_DebugGetActive(s_effectsList, kMaxListedEffects);

            struct EffectRow {
                Effects* effect;
                f32 distance;
                bool hasPos;
            };
            static EffectRow s_rows[kMaxListedEffects] = {};
            s32 rowCount = 0;

            const bool havePlayerPos = Player::s_player != nullptr;
            LVector playerPos = havePlayerPos ? Player::s_player->pos : LVector{};

            for (s32 i = 0; i < effectCount && i < kMaxListedEffects; i++) {
                Effects* effect = s_effectsList[i];
                if (!effect) {
                    continue;
                }

                EffectRow& row = s_rows[rowCount++];
                row.effect = effect;
                row.hasPos = false;
                row.distance = 0.0f;

                LVector worldPos = {};
                if (havePlayerPos && effect->GetDebugWorldPos(&worldPos)) {
                    const f32 dx = static_cast<f32>(worldPos.x - playerPos.x);
                    const f32 dy = static_cast<f32>(worldPos.y - playerPos.y);
                    const f32 dz = static_cast<f32>(worldPos.z - playerPos.z);
                    row.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    row.hasPos = true;
                }
            }

            std::sort(s_rows, s_rows + rowCount, [](const EffectRow& a, const EffectRow& b) {
                if (a.hasPos != b.hasPos) {
                    return a.hasPos;
                }
                return a.distance < b.distance;
            });

            ImGui::Text("Active: %d", effectCount);
            ImGui::SameLine();
            if (ImGui::Button("Enable All")) {
                for (s32 i = 0; i < rowCount; i++) {
                    s_rows[i].effect->debugDisabled = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Disable All")) {
                for (s32 i = 0; i < rowCount; i++) {
                    s_rows[i].effect->debugDisabled = true;
                }
            }
            ImGui::InputText("Filter", sEffectsFilter, sizeof(sEffectsFilter));
            if (!havePlayerPos) {
                ImGui::TextDisabled("No player - distances unavailable, list unsorted.");
            }

            if (ImGui::BeginTable("EffectsTable", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 400))) {
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (s32 i = 0; i < rowCount; i++) {
                    Effects* effect = s_rows[i].effect;

                    char label[256] = {};
                    BuildDebugEffectLabel(label, static_cast<s32>(sizeof(label)), effect, true);

                    if (sEffectsFilter[0] != '\0' && !std::strstr(label, sEffectsFilter)) {
                        continue;
                    }

                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    bool enabled = !effect->debugDisabled;
                    ImGui::PushID(effect);
                    if (ImGui::Checkbox("##on", &enabled)) {
                        effect->debugDisabled = !enabled;
                    }
                    ImGui::PopID();

                    ImGui::TableSetColumnIndex(1);
                    if (s_rows[i].hasPos) {
                        ImGui::Text("%.0f", s_rows[i].distance);
                    }
                    else {
                        ImGui::TextDisabled("--");
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s", DebugEffectTypeName(effect->effectType));

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(label);
                }

                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    if (sShowDebugging) {
        if (ImGui::Begin("Debugging", &sShowDebugging)) {
            ImGui::SeparatorText("Combat Testing");
            ImGui::Checkbox("Disable player/enemy HP loss", &sDisableHumanoidDamage);

            ImGui::SeparatorText("3D Labels");
            ImGui::Checkbox("Humanoid names", &sDebugShowHumanoidNames3D);
            ImGui::Checkbox("All AI Thing names", &sDebugShowAllAIThingNames3D);
            ImGui::Checkbox("Effects/Particles names", &sDebugShowEffectNames3D);
            if (sDebugShowEffectNames3D) {
                ImGui::Checkbox("Detailed effect labels", &sDebugShowDetailedEffectNames3D);
                ImGui::TextDisabled("Shown type value is effectType enum, not effect instance index.");
            }

            DrawDebugThingLabelInspectorPanel();
        }
        ImGui::End();
    }

#if MODERN_GRAPHICS
    if (sShowShadows) {
        if (ImGui::Begin("Shadows (CSM)", &sShowShadows)) {
            static const char* kQualityNames[] = { "Off (blob shadow)", "Low", "Medium", "High", "Very High" };
            s32 qualityIdx = (s32)ShadowCSM::GetQuality();
            ImGui::Text("Quality:");
            for (s32 q = 0; q < 5; q++) {
                if (ImGui::RadioButton(kQualityNames[q], qualityIdx == q)) {
                    ShadowCSM::SetQuality((ShadowQuality)q);
                }
            }
            ImGui::Separator();
            ImGui::Text("Cascade resolution: %d", ShadowCSM::GetCascadeResolution());
            ImGui::Text("Depth format: %s", ShadowCSM::GetCascadeDepthFormatName());
            ImGui::Text("Filter: %s", ShadowCSM::GetFilterQualityName());
            ImGui::Text("Frame prepared: %s", ShadowCSM::IsFramePrepared() ? "yes" : "no");
            ImGui::Text("Casters submitted last frame: %d", ShadowCSM::GetCasterCount());
            if (qualityIdx == SHADOW_QUALITY_OFF) {
                ImGui::TextDisabled("Quality is Off: CSM is inactive, legacy blob shadow is used instead.");
            }
            else if (ShadowCSM::GetCasterCount() == 0) {
                ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1),
                    "No casters submitted -- check Model::Show's modelFlags 0x10/0x20 gating.");
            }

            ImGui::Separator();
            ImGui::Text("Cascade depth maps (white = near light, black = far/empty):");
            for (s32 i = 0; i < 3; i++) {
                if (i > 0) {
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
                ImGui::Text("Cascade %d", i);
                u32 handle = ShadowCSM::GetCascadeTextureHandle(i);
                if (handle != 0) {
                    ImGui::Image((ImTextureID)(u64)handle, ImVec2(192, 192));
                }
                else {
                    ImGui::TextDisabled("(not created)");
                }
                ImGui::EndGroup();
            }

            ImGui::Separator();
            ImGui::Text("Debug visualization:");
            ImGui::RadioButton("Off##shadowdbg", &sShadowDebugMode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Tint by cascade##shadowdbg", &sShadowDebugMode, 1);
            ImGui::SameLine();
            ImGui::RadioButton("Force-darken receivers##shadowdbg", &sShadowDebugMode, 2);
            ImGui::TextWrapped(
                "Tint by cascade: every shadow-receiving fragment (world block geometry) is "
                "colored red/green/blue by which cascade it sampled -- if you see no tint at "
                "all on world geometry, uReceiveShadows/uWorldMatrix wiring is the problem, "
                "not the depth comparison. Force-darken: every receiving fragment is darkened "
                "unconditionally, regardless of the actual depth test -- if this also shows "
                "nothing, world geometry isn't being marked as a shadow receiver at all.");
            if (p3d::context) {
                p3d::context->SetShadowDebugMode(sShadowDebugMode);
            }
        }
        ImGui::End();
    }
#endif

    if (sShowCamera && g_game) {
        if (ImGui::Begin("Camera", &sShowCamera)) {
            Camera& cam = g_game->GetCamera();
            ImGui::Text("Mode: %s", CameraModeName(cam.GetMode()));

            s32 camModeIdx = (s32)cam.GetMode();
            if (camModeIdx < CAM_MODE_DEFAULT || camModeIdx > CAM_MODE_RIGID) {
                camModeIdx = CAM_MODE_FOLLOW;
            }
            if (ImGui::BeginCombo("Camera Mode", CameraModeName((CameraMode)camModeIdx))) {
                for (s32 mode = CAM_MODE_DEFAULT; mode <= CAM_MODE_RIGID; mode++) {
                    bool selected = (camModeIdx == mode);
                    if (ImGui::Selectable(CameraModeName((CameraMode)mode), selected)) {
                        cam.SetMode((CameraMode)mode);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SeparatorText("Input Routing Override");
            ImGui::Checkbox("Override input owner while Debug UI open", &sInputRoutingOverride);
            if (sInputRoutingOverride) {
                ImGui::RadioButton("Camera input only", &sInputRoutingSelection, 0);
                ImGui::SameLine();
                ImGui::RadioButton("Player input only", &sInputRoutingSelection, 1);
            }

            LVectorText("Position", cam.GetPosition());
            LVectorText("Target", cam.GetTargetPos());
            ImGui::Text("FOV: %d (desired: %d)", cam.GetCurFOV(), cam.GetDesiredFOV());
        }
        ImGui::End();
    }

    if (sShowAnimation) {
        Player* p = Player::s_player;
        if (ImGui::Begin("Animation", &sShowAnimation)) {
            if (p && p->model) {
                Model* m = (Model*)p->model;
                AnimStructure* anim = (AnimStructure*)m->animStructure;
                if (sAnimSelectedEnum < 0) {
                    sAnimSelectedEnum = 0;
                }
                if (sAnimSelectedEnum >= (s32)CharSlot::ANIM_TABLE_SIZE) {
                    sAnimSelectedEnum = (s32)CharSlot::ANIM_TABLE_SIZE - 1;
                }
                if (sAnimSelectedLoopType < ANIM_LOOP || sAnimSelectedLoopType > ANIM_STOP) {
                    sAnimSelectedLoopType = ANIM_LOOP;
                }

                ImGui::SeparatorText("Controls");
                ImGui::InputInt("Anim Enum", &sAnimSelectedEnum, 1, 10);
                if (sAnimSelectedEnum < 0) {
                    sAnimSelectedEnum = 0;
                }
                if (sAnimSelectedEnum >= (s32)CharSlot::ANIM_TABLE_SIZE) {
                    sAnimSelectedEnum = (s32)CharSlot::ANIM_TABLE_SIZE - 1;
                }

                if (ImGui::BeginCombo("Loop Type", AnimLoopTypeName(sAnimSelectedLoopType))) {
                    for (s32 lt = ANIM_LOOP; lt <= ANIM_STOP; lt++) {
                        bool selected = (sAnimSelectedLoopType == lt);
                        if (ImGui::Selectable(AnimLoopTypeName(lt), selected)) {
                            sAnimSelectedLoopType = lt;
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                const bool overrideActive = p->Debug_IsAnimationOverrideActive();
                ImGui::Text("Override: %s", overrideActive ? "Active" : "Off");

                if (ImGui::Button("Load")) {
                    if (g_characterManager) {
                        g_characterManager->LoadAnimationBatch(p->debugModelCharType, sAnimSelectedEnum, nullptr);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Play")) {
                    p->Debug_PlayAnimation(sAnimSelectedEnum, sAnimSelectedLoopType);
                }
                ImGui::SameLine();
                if (ImGui::Button("Pause")) {
                    p->Debug_PauseAnimation();
                }
                ImGui::SameLine();
                if (ImGui::Button("Resume")) {
                    p->Debug_ResumeAnimation();
                }
                ImGui::SameLine();
                if (ImGui::Button("Stop")) {
                    p->Debug_StopAnimation();
                }

                if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_Space)) {
                    if (sAnimSelectedEnum == p->debugAnimOverrideEnum &&
                        p->debugAnimOverrideActive && !p->debugAnimOverridePaused) {
                        p->Debug_PauseAnimation();
                    }
                    else if (sAnimSelectedEnum == p->debugAnimOverrideEnum &&
                             p->debugAnimOverridePaused) {
                        p->Debug_ResumeAnimation();
                    }
                    else
                        p->Debug_PlayAnimation(sAnimSelectedEnum, sAnimSelectedLoopType);
                }

                const u32 animCharType = p->debugModelCharType;
                CharSlot* animSlot = nullptr;
                if (g_characterManager) {
                    for (s32 i = 0; i < CHAR_MAX_SLOTS; i++) {
                        if (g_characterManager->slots[i].thingType == animCharType) {
                            animSlot = &g_characterManager->slots[i];
                            break;
                        }
                    }
                }

                s32 maxAnimEnum = (s32)CharSlot::ANIM_TABLE_SIZE - 1;
                if (animSlot && animSlot->charFile && animSlot->charFile->rrHeaderEntries >= 10) {
                    s32 rrMax = (animSlot->charFile->rrHeaderEntries - 10) / 2;
                    if (rrMax < maxAnimEnum) {
                        maxAnimEnum = rrMax;
                    }
                }
                if (maxAnimEnum < 0) {
                    maxAnimEnum = 0;
                }

                s32 loadedCount = 0;
                if (animSlot && g_characterManager) {
                    for (s32 e = 0; e <= maxAnimEnum; e++) {
                        u8 handle = animSlot->animIndexTable[e];
                        if (handle != 0xFF && handle < CHAR_MAX_ANIMS && g_characterManager->animPtrs[handle]) {
                            loadedCount++;
                        }
                    }
                }

                ImGui::SeparatorText("Available Animations");
                if (animCharType != 0) {
                    ImGui::Text("Character: %s (type %u)", g_charNameTable[animCharType], animCharType);
                }
                ImGui::Text("Available enums: 0..%d", maxAnimEnum);
                ImGui::Text("Loaded animations: %d", loadedCount);

                if (ImGui::BeginChild("AnimList", ImVec2(0, 220), true)) {
                    ImGuiListClipper clipper;
                    clipper.Begin(maxAnimEnum + 1);
                    while (clipper.Step()) {
                        for (s32 e = clipper.DisplayStart; e < clipper.DisplayEnd; e++) {
                            bool loaded = false;
                            u8 handle = 0xFF;
                            if (animSlot && g_characterManager) {
                                handle = animSlot->animIndexTable[e];
                                loaded = (handle != 0xFF && handle < CHAR_MAX_ANIMS && g_characterManager->animPtrs[handle] != nullptr);
                            }

                            char label[64];
                            std::snprintf(label, sizeof(label), "%03d %s", e, loaded ? "[loaded]" : "");

                            if (ImGui::Selectable(label, sAnimSelectedEnum == e)) {
                                sAnimSelectedEnum = e;
                            }
                        }
                    }
                    ImGui::EndChild();
                }

                ImGui::SeparatorText("Current Playback");
                ImGui::Text("Current Anim Enum: %d", p->currentAnimEnum);
                ImGui::Text("Anim State: %s", p->Debug_IsAnimationPaused() ? "Paused" : "Playing");

                if (anim) {
                    ImGui::Text("Frame: %d (0x%X)", anim->currentFrame >> 16, anim->currentFrame);
                    ImGui::Text("Start/End: %d / %d", anim->startFrame >> 16, anim->endFrame >> 16);
                    ImGui::Text("Anim Frames: %d", anim->animation ? anim->animation->numFrames : 0);
                    ImGui::Text("Prev Frame: %d", anim->prevFrame >> 16);
                    ImGui::Text("Speed: %d (%.2fx)", anim->speed, anim->speed / 65536.0f);
                    ImGui::Text("Mode: %d", anim->mode);
                    ImGui::Text("Loop Type: %s (%d)", AnimLoopTypeName(anim->loopTypeField), anim->loopTypeField);
                    ImGui::Text("Loop Count: %d", anim->loopCount);
                    ImGui::Text("Has Flip: %s", anim->flip ? "yes" : "no");
                    ImGui::Text("Has Anim: %s", anim->animation ? "yes" : "no");

                    if (anim->endFrame > anim->startFrame) {
                        f32 progress = (f32)(anim->currentFrame - anim->startFrame) /
                            (f32)(anim->endFrame - anim->startFrame);
                        if (progress < 0.0f) progress = 0.0f;
                        if (progress > 1.0f) progress = 1.0f;
                        ImGui::ProgressBar(progress, ImVec2(-1, 0), "");
                    }
                }
                else {
                    ImGui::Text("No AnimStructure");
                }
            }
            else {
                ImGui::Text("No player model");
            }
        }
        ImGui::End();
    }

    if (sShowAudio) {
        if (ImGui::Begin("Audio", &sShowAudio)) {
            if (AudioEngine::IsInitialized()) {
                f32 master = AudioEngine::GetMasterVolume();
                if (ImGui::SliderFloat("Master Volume", &master, 0.0f, 1.0f)) {
                    AudioEngine::SetMasterVolume(master);
                }
                ImGui::Text("Music Playing: %s", AudioEngine::IsMusicPlaying() ? "yes" : "no");
            }
            else {
                ImGui::Text("Audio not initialized");
            }
            if (g_sound) {
                ImGui::Separator();
                ImGui::Text("Active SFX Bank: %d", g_sound->activeSfxBank);
                ImGui::Text("WAX Banks: %d", g_sound->numWaxBanks);
                ImGui::Text("Music: %s", g_sound->musicPlaying ? "yes" : "no");
                ImGui::Text("Active: %d", g_sound->activeFlag);
            }
            static s32 soundIndex = 0;
            static s32 soundStep = 10;

            ImGui::InputInt("Sound ID", &soundIndex);

            if (ImGui::Button("Play Sound") || ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_Space)) {
                CSoundDirect::PlayTransient(soundIndex, nullptr, 0, 0);
            }

        }
        ImGui::End();
    }

    if (sShowConsoleNotes) {
        if (ImGui::Begin("Console Notes", &sShowConsoleNotes)) {
            ImGui::TextWrapped("Type a comment and press Enter or Submit. It is printed to console and appended to rechan.log.");

            bool submit = ImGui::InputText("Comment", sConsoleNoteInput, sizeof(sConsoleNoteInput), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Submit")) {
                submit = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                sConsoleNoteInput[0] = '\0';
            }

            if (submit) {
                SubmitConsoleNote();
                ImGui::SetKeyboardFocusHere(-1);
            }

            if (sLastConsoleNote[0] != '\0') {
                ImGui::Separator();
                ImGui::TextWrapped("Last saved note: %s", sLastConsoleNote);
            }
        }
        ImGui::End();
    }

    if (sShowImGuiDemo) {
        ImGui::ShowDemoWindow(&sShowImGuiDemo);
    }

    if (sShowAssetExporter) { DrawAssetExporterWindow(&sShowAssetExporter); }
    if (sShowMods) { DrawModsWindow(&sShowMods); }

}

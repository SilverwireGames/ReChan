#include "ai/player.h"
#include "ai/colfight.h"
#include "ai/obstacle.h"
#include "ai/pickup.h"
#include "gen/model.h"
#include "gen/mplayer.h"
#include "gen/animmat.h"
#include "gen/animstruct.h"
#include "gen/charmgr.h"
#include "gen/control.h"
#include "gen/colsect.h"
#include "gen/director.h"
#include "gen/trail.h"
#include "snd/rsevent.h"
#include "snd/hmndsnd.h"
#include "p3d/p3dmath.h"
#include "pc/log.h"
#include "gen/ai.h"
#include "gen/blockmgr.h"
#include "gen/camera.h"
#include "gen/display.h"
#include "gen/geffect.h"
#include "gen/scoremgr.h"
#include "gen/game.h"
#include "gen/world.h"
#include "snd/snddrct.h"
#include "fe/hud.h"
#if NEW_CHEATS
#include "extra/cheats.h"
#endif

// Command bit masks - derived from GameAction enum bit positions.
// RequestAction does: commandBits |= (1 << actionID)
static constexpr s32 CB_GUARD_RELEASE = (1 << GA_GUARD_RELEASE); // bit 1
static constexpr s32 CB_MOVE = (1 << GA_MOVE);          // bit 2: directional movement
static constexpr s32 CB_JUMP = (1 << GA_JUMP);          // bit 3: jump (Cross tap)
static constexpr s32 CB_JUMP_DIR = (1 << GA_JUMP_DIRECTIONAL); // bit 4: jump + direction
static constexpr s32 CB_DIVE_ROLL = (1 << GA_DIVE_ROLL);    // bit 5: dive roll (R1)
static constexpr s32 CB_STRAFE = (1 << GA_STRAFE);        // bit 6: strafe (R2)
static constexpr s32 CB_GRAB = (1 << GA_GRAB);          // bit 7: grab/throw (Circle)
static constexpr s32 CB_PUNCH = (1 << GA_PUNCH);         // bit 8: punch (Square)
static constexpr s32 CB_KICK = (1 << GA_KICK);          // bit 9: kick (Triangle)
static constexpr s32 CB_GRAB_FWD = (1 << GA_GRAB_FORWARD);  // bit 15: grab forward

// Movement tuning constants (PSX original values)
// PSX uses a ramping force accumulator (gp+392) that gradually increases from 0
// to runSpeed. AddForce accumulates into DynamicThing::force (80% damped per frame).
static constexpr s32 PLAYER_RUN_FORCE = 3000;  // per-frame AddForce magnitude
static constexpr s32 PLAYER_JUMP_FORCE = 17000;  // upward contactForce on jump (gp+460, PSX 0x4268)
static constexpr s32 PLAYER_RUNNING_JUMP_BONUS = -1500; // running jump Y offset (gp+464, PSX 0xFFFFFA24)
static constexpr s32 PLAYER_RUNNING_JUMP_BURST = 2500; // initial horizontal burst (runJumpHold[0], PSX 0x9C4)
static constexpr s16 TABLE_ROLL_HANGTIME_START = 0;
static constexpr s16 TABLE_ROLL_HANGTIME_END = 100;
static constexpr s32 TABLE_LOOK_AHEAD_DISTANCE = 200;
static constexpr s32 STRAFE_MOVE_SPEED = 0xC000;
static constexpr s32 WALL_JUMP_TRACE_DISTANCE = 256;
static constexpr s32 WALL_JUMP_COLLISION_RADIUS = 16;
static constexpr s32 WALL_JUMP_COLLISION_HEIGHT = 500;
static constexpr s32 WALL_JUMP_MIN_HEIGHT_ABOVE_FLOOR = 256;
static constexpr u32 WALL_JUMP_MIN_WALL_HEIGHT = 0;
static constexpr u32 WALL_JUMP_MIN_COLLISION_RATIO = 0;
static constexpr s32 WALL_JUMP_MIN_ANGLE = 0;
static constexpr s32 PICKUP_MOVE_COMPARE_OFFSET = 384;
static constexpr s32 PLAYER_STRAFE_TARGET_RANGE = 0x514;
static constexpr s32 PLAYER_STRAFE_TARGET_HALF_ANGLE = 0x5C71;
static constexpr s32 THROW_FAR_RANGE = 0x4B0;
static constexpr s32 THROW_FAR_FIELD = (30 << 16) / 360;

// PSX jump parameter tables at 0x800D652C-0x800D6558: [force, gravity, maxFallDivisor]
static const s32 s_runJumpTap[3] = { 2000,  20480, 20 };  // 0x7D0, 0x5000, 0x14
static const s32 s_runJumpHold[3] = { 2500,  16000, 23 };  // 0x9C4, 0x3E80, 0x17
static const s32 s_standingJumpTap[3] = { 150,   4096,  20 };  // 0x96,  0x1000, 0x14
static const s32 s_standingJumpHold[3] = { 150,   4096,  25 };  // 0x96,  0x1000, 0x19

// PSX: gp+492 - hard-fall velocity threshold (velocity.y must be <= this to trigger)
static constexpr s32 g_hardFallThreshold = -8192;

static s32 GetWeaponPickupDialog(s32 weaponType);

static void RemovePlayerMirrorFlag(Model* model) {
    SModel* sm = static_cast<SModel*>(model);
    DrawableSTree* drawable = sm && sm->drawable ? static_cast<DrawableSTree*>(sm->drawable) : nullptr;
    if (drawable && (drawable->mirrorFlags & 1u)) {
        sm->MirrorTree();
    }
}

// PSX: playerStraif animation data at 0x800D91E0
// Array of [animIndex, loopType] pairs for strafe animations based on movement direction
// Angle difference = ClipAngle360(orientation.y - faceAngle) determines which animation plays
// loopType 0 = ANIM_LOOP (forward), loopType 1 = ANIM_LOOP_REVERSE (backward playback)
static s32 s_playerStraif[] = {
    22, 0,  // [0,1]: idle when not moving
    51, 0,  // [2,3]: forward strafe 0-45° or 315-360°
    51, 1,  // [4,5]: back-side strafe 135-225° (anim 51 played in reverse)
    52, 0,  // [6,7]: backward strafe 225-315°
    52, 1   // [8,9]: side strafe 45-135° (anim 52 played in reverse)
};

static s32 CheckWallCollisionForJump(
    const Player* player,
    s16 angle,
    s32 distance,
    s32 radius,
    s32 height,
    s32& outCollisionRatio,
    LVector& outNormal,
    LVector& outHitPos,
    s32& outVerticalSpan) {
    if (!player) {
        return 0;
    }

    LVector start = player->pos;
    LVector end = start;
    end.x = start.x + (s32)(((s64)rmSin16(angle) * distance) >> 16);
    end.z = start.z + (s32)(((s64)rmSin16((s16)(angle + 0x4000)) * distance) >> 16);

    LVector searchMin = {};
    LVector searchMax = {};

    if (start.x >= end.x) {
        searchMin.x = end.x - radius;
        searchMax.x = start.x + radius;
    }
    else {
        searchMin.x = start.x - radius;
        searchMax.x = end.x + radius;
    }

    if (start.y >= end.y) {
        searchMin.y = end.y;
        searchMax.y = start.y + height;
    }
    else {
        searchMin.y = start.y;
        searchMax.y = end.y + height;
    }

    if (start.z >= end.z) {
        searchMin.z = end.z - radius;
        searchMax.z = start.z + radius;
    }
    else {
        searchMin.z = start.z - radius;
        searchMax.z = end.z + radius;
    }

    Wall* wallArray[64] = {};
    s32 wallCount = CollisionSector::FillWorldWallArray(searchMin, searchMax, wallArray, 64);
    if (wallCount > 64) {
        wallCount = 64;
    }

    s32 hit = CollisionSector::CheckArrayWallCollision(
        wallArray,
        wallCount,
        start,
        end,
        radius,
        0,
        height,
        1);

    outCollisionRatio = g_wallCollisionInfo.collisionRatio;
    outNormal = g_wallCollisionInfo.wallNormal;
    outHitPos = g_wallCollisionInfo.hitPoint;
    outVerticalSpan = g_wallCollisionInfo.wallVerticalMax - g_wallCollisionInfo.wallVerticalMin;
    return hit;
}

static s32 CheckWallConstraintForJump(
    const Player* player,
    u32 minCollisionRatio,
    s32 distance,
    s32 minAngle,
    s32& outWallAngle,
    LVector& outHitPos) {
    if (!player) {
        return 0;
    }

    s32 collisionRatio = 0;
    LVector wallNormal = {};
    LVector hitPos = {};
    s32 wallVerticalSpan = 0;
    s32 hit = CheckWallCollisionForJump(
        player,
        (s16)(player->orientation.y & 0xFFFF),
        distance,
        WALL_JUMP_COLLISION_RADIUS,
        WALL_JUMP_COLLISION_HEIGHT,
        collisionRatio,
        wallNormal,
        hitPos,
        wallVerticalSpan);
    if (!hit) {
        return 0;
    }

    if (minCollisionRatio >= (u32)collisionRatio) {
        return 0;
    }

    s32 wallAngle = 0;
    if (wallNormal.x != 0) {
        if (wallNormal.z != 0) {
            s32 a = 0x4000 - (s32)rmATan216((f32)(-wallNormal.x), (f32)(-wallNormal.z));
            s32 absA = (a < 0) ? -a : a;
            if (absA > 0x7FFF) {
                if (a <= 0) {
                    a += 0xFFFF;
                }
                else {
                    a -= 0xFFFF;
                }
            }
            wallAngle = a;
        }
        else {
            wallAngle = (wallNormal.x > 0) ? 49152 : 0x4000;
        }
    }
    else {
        wallAngle = (wallNormal.z > 0) ? 0x8000 : 0;
    }

    s32 diff = player->faceAngle - wallAngle;
    s32 wrapped = diff + 0x8000;
    s32 absWrapped = (wrapped < 0) ? -wrapped : wrapped;
    if (absWrapped > 0x7FFF) {
        if (wrapped <= 0) {
            wrapped = diff + 98303;
        }
        else {
            wrapped = diff - 0x7FFF;
        }
    }
    s32 absFinal = (wrapped < 0) ? -wrapped : wrapped;
    if (absFinal < minAngle) {
        return 0;
    }

    outWallAngle = wallAngle;
    outHitPos = hitPos;
    return 1;
}

static bool EnsurePlayerAnimationLoaded(s32 animEnum, u32 charType = 0) {
    if (!g_characterManager || animEnum < 0) {
        return false;
    }
    if (g_characterManager->GetAnimation(charType, animEnum)) {
        return true;
    }
    g_characterManager->LoadAnimationBatch(charType, animEnum, nullptr);
    return g_characterManager->GetAnimation(charType, animEnum) != nullptr;
}

Player* Player::s_player = nullptr;

// PSX: __6PlayerPC10tagLVector (PLAYER.CPP:1014)
Player::Player(const LVector* initialPos)
    : Humanoid(initialPos, AITypes::TT_PLAYER) {
    MARKFUNCTION(0x8002FA80);

    maxHealth = 200;
    moveSpeed = 3000;
    comboCount = 1;

    // PSX: set faceAngleData to playerStraif array for strafe animations
    faceAngleData = s_playerStraif;

    // PSX: embedded CheckpointInfo init (+636..+688)
    checkpoint = {};
    checkpoint.field28 = -1;

    hitCombo = 0;
    comboTimer = 0;
    playerFlags = 0;
    field704 = 0;
    field706 = 0;
    field712 = nullptr;
    field720 = 0;
    field724 = 0;
    field728 = 0;
    actionStateFlag = 0;
    animCallbackData = 0;
    animCallbackVtable = nullptr;
    currentAnimEnum = 0;
    animLoadState = 0;

    // PSX: gp+3432 = this (global player pointer)
    s_player = this;

    // Store spawn ground level for simple floor clamping
    jumpReturnHeight = homePos.y;

    SetActionState(AS_INACTIVE_IDLE, 0);

    // PSX PLAYER.CPP:1014 creates Behaviour in Player ctor.
    if (!behaviour) {
        behaviour = new Behaviour(this, AITypes::TT_PLAYER, 0);
    }

    // PSX PLAYER.CPP:1014 allocates a 16-segment trail pool at Humanoid +512.
    if (!trails) {
        trails = new Trails(16);
    }
}

// PSX: _._6Player (PLAYER.CPP:1050)
Player::~Player() {
    MARKFUNCTION(0x8002FBC0);
    if (s_player == this) {
        s_player = nullptr;
    }
}

// PSX: Think__6Player (PLAYER.CPP:1155)
void Player::Think() {
    MARKFUNCTION(0x8002FE30);
#if NEW_CHEATS
    if (IsCheatEnabled(CheatOption::GodMode)) health = maxHealth;
#endif
    // PSX: CHumanoidSound think, encounter check, behaviour process,
    // ProcessAction, Move, combo tracking, input read
    if (humanoidSound) {
        humanoidSound->Think();
    }
    if (PlayerSingleEncounterCheak()) {
        PlayPlayerTauntResponse();
    }

    if (model) {
        HumanoidModel* hm = static_cast<HumanoidModel*>(model);
        hm->attackHandRadius = 72;
        hm->attackFootRadius = 100;
    }

    flags &= ~TF_BIT1;
    flags2 &= ~TF2_BIT3;

    ProcessControl();

    // Match PSX Think__6Player: update runSpeed from movement before
    // ProcessAction, since wall-context kicks use it to reject idle kicks.
    s64 dt = (s64)moveSpeed * (s64)deltaTime;
    s32 scaledRange = (s32)((u64)dt >> 16);
    runSpeed = scaledRange;
    deltaTime = FIX16_ONE;

    ProcessAction();
    Move();

    field368 = 0;

    // Combo tracking
    if (hitCombo < 3) {
        // not enough for combo
    }
    else if (comboTimer < 1800) {
        comboTimer++;
    }
    else {
        hitCombo = 0;
        comboTimer = 0;
    }

    Debug_ApplyForcedAnimation();
}

// PSX: Reset__6Player (PLAYER.CPP:1056)
void Player::Reset() {
    MARKFUNCTION(0x8002FC24);
    Humanoid::Reset();

    stateCounter = 100;
    velocity = {};
    contactForce = {};
    lastPos = orientation;

    health = 200;
    turnRate = 5500;
    SetDesiredMoveDirection(orientation.y);
    flags |= TF_DYNAMIC | TF_BIT5 | TF_BIT3;
    flags &= ~0x80;
    // PSX: runSpeed set from humanoid data (GetHumanoidData). Default for Player.
    runSpeed = PLAYER_RUN_FORCE;
    field616 = 0;
    field620 = 0;
    hitCombo = 0;
    comboTimer = 0;
    playerFlags |= PF_COMBAT_READY;
    field704 = 0;
    field706 = 0;
    field712 = nullptr;
    field720 = 0;
    field724 = 0;
    field728 = 0;
    debugAnimOverrideActive = false;
    debugAnimOverridePaused = false;
    debugAnimOverrideApplying = false;
    debugAnimOverrideEnum = -1;
    debugAnimOverrideLoopType = ANIM_LOOP;

    SetActionState(AS_STAND, 0);

    if (behaviour) {
        behaviour->handlerThisOffset = 0;
        behaviour->handlerDispatch = -1;
        behaviour->handler = Behaviour::PlayerUserControl;
    }
}

// PSX: Move__6Player (PLAYER.CPP:1408, 0x80030100)
void Player::Move() {
    MARKFUNCTION(0x80030100);
    Humanoid::Move();
}

// PSX: CreateModel__6PlayerPCc (PLAYER.CPP:1111, 0x8002FD34)
// PSX: creates PlayerModel if not exists, calls Thing::CreateModel (NOT Humanoid::CreateModel),
// stores OriginalSTree_omPlayer global, calls InitBlendPose
void Player::CreateModel(const char* name) {
    MARKFUNCTION(0x8002FD34);

    // PSX: if model == null, create PlayerModel(136)
    if (!model) {
        PlayerModel* pm = new PlayerModel();
        model = pm;
        pm->backPtr = this;
    }

    // PSX: calls Thing::CreateModel directly (skips Humanoid::CreateModel)
    Thing::CreateModel(name);

    // PSX: virtual calls for animation setup (ApplyAnimToModel etc.)

    // PSX: OriginalSTree_omPlayer = model->drawable->original
    // Used for suit-change system - skip global for now

    // PSX: InitBlendPose - animation blending
    // Not yet reversed - skip

    // PSX: InitSemiTransMode (called via Humanoid path, also needed here)
    Model* m = static_cast<Model*>(model);
    if (m) {
        m->ApplyAnimToModel(0, 0, 2, 0, 0);
        SModel* sm = static_cast<SModel*>(m);
        sm->InitSemiTransMode();
    }

    // PSX: (*(a1[2] + 232))(a1, 1, 0) - SetActionState(1, 0)
    SetActionState(AS_STAND, 0);

    // PSX: InitBlendPose (not yet reversed)

    // PSX: (*(a1[2] + 212))(a1) - vtable call to CreateSound
    CreateSound();
}

// PSX: SetActionState__6PlayerUll (PLAYER.CPP:1579, 0x800303BC)
// PSX: 25 Player-specific cases, rest delegate to Humanoid::SetActionState.
void Player::SetActionState(u32 state, s32 param) {
    MARKFUNCTION(0x800303BC);
    actionStateFlag = 0;
    flags |= TF_DYNAMIC;

    // PSX preamble
    combatFlag = 0;
    flags |= TF_DYNAMIC;
    if (humanoidSound) {
        humanoidSound->EndAllSounds();
    }

    // PSX: if previous actionState was 3, unload stored animation
    if (actionState == (s32)AS_WALL_JUMP_TAUNT) {
        if (g_characterManager) {
            g_characterManager->UnloadAnimationBatch(0, currentAnimEnum);
        }
    }

    switch (state) {
        case AS_INACTIVE_IDLE:
        {
            // PSX case 0: clear animations, set dispatch to none
            playerFlags &= ~4;
            field344 = 0;
            stateDispatch = SD_NONE;
            field348 = 0;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_INACTIVE_IDLE, param, 0, 0);
            }
            actionState = (s32)state;
            return;
        }
        case AS_STAND:
        {
            // PSX case 1: stateDispatch=22, SetIdleAnimation, reset fields, playerFlags|=4
            field344 = 0;
            stateDispatch = SD_STAND;
            turnAroundTimer = 0;
            turnAroundFlag = 0;
            field348 = 8;
            SetIdleAnimation(param, 1);
            if (model) {
                // PSX: if drawable->mirrorFlags bit 0 is set, MirrorTree toggles it off.
                RemovePlayerMirrorFlag(static_cast<Model*>(model));
            }
            field616 = 0;
            idleTimer = 0;
            field420 = nullptr;
            field424 = 0;
            playerFlags |= PF_COMBAT_READY;
            flags |= TF_DYNAMIC;
            flags2 &= ~0x70; // clear bits 4,5,6
            actionState = (s32)state;
            stateTimer = 0;
            return;
        }
        case AS_WALL_JUMP_TAUNT:
        {
            // PSX case 3: select anim 308-315 based on health ratio/random,
            // preload it, and transition to InactiveIdle callback flow.
            const s32 healthRatio = rmDiv16i((s32)((u32)health << 16), (s32)((u32)maxHealth << 16));
            s32 dialogID = 33;

            if (healthRatio <= 45874) {
                if (healthRatio <= 39320) {
                    dialogID = 34;
                    if (healthRatio < 19660) {
                        dialogID = 31;
                        if (healthRatio < 13107) {
                            currentAnimEnum = 315;
                        }
                        else {
                            dialogID = 32;
                            currentAnimEnum = 314;
                        }
                    }
                    else {
                        currentAnimEnum = 313;
                    }
                }
                else {
                    currentAnimEnum = 312;
                }
            }
            else {
                currentAnimEnum = (s32)rmRangedRandom(4) + 308;
                dialogID = 36;
                if (currentAnimEnum != 309) {
                    dialogID = 35;
                    if (currentAnimEnum >= 310) {
                        dialogID = 38;
                        if (currentAnimEnum != 310) {
                            dialogID = (currentAnimEnum == 311) ? 37 : 35;
                        }
                    }
                }
            }

            field344 = 0;
            stateDispatch = SD_INACTIVE_IDLE;
            animCallbackData = 0;
            animLoadState = 2;

            if (g_characterManager) {
                g_characterManager->LoadAnimationBatch(AITypes::TT_PLAYER, currentAnimEnum, nullptr);
                animCallbackData = g_characterManager->GetAnimation(AITypes::TT_PLAYER, currentAnimEnum) ? 1 : 0;
            }

            LoadDialog((u32)dialogID, 0x36);
            actionState = (s32)state;
            return;
        }
        case AS_PAUSE:
        {
            // PSX case 6: running jump (from _Run context).
            // stateDispatch=28(SD_JUMP), DoJump with combined base+running force,
            // field704=1 (hold flag), runJumpHold table, AddForce initial burst.
            // PSX case 6: model->MirrorTree() (vtable+80) when carrying no pickups —
            // the running jump TOGGLES the model mirror (PSX facing-flip system).
            if (!rightHandObj && !leftHandObj && model) {
                static_cast<SModel*>(model)->MirrorTree();
            }
            field344 = 0;
            stateDispatch = SD_JUMP;
            field348 = 8;
            field704 = 1;  // jump hold flag (running jump has hold detection)
            field700 = 0;
            field706 = 0;
            field712 = s_runJumpHold;
            DoJump(PLAYER_JUMP_FORCE + PLAYER_RUNNING_JUMP_BONUS);

            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_ID_36, 0, 0, 0);
            }

            // PSX: AddForce(jumpTable[0], (s16*)&orientation) - rotY comes from orientation.y
            SVector dir;
            dir.x = (s16)(orientation.x & 0xFFFF);
            dir.y = (s16)((orientation.x >> 16) & 0xFFFF);
            dir.z = (s16)(orientation.y & 0xFFFF);
            dir.pad = (s16)((orientation.y >> 16) & 0xFFFF);
            AddForce(PLAYER_RUNNING_JUMP_BURST, &dir);

            turnRate = 1500;
            playerFlags = (playerFlags & ~3) | 2;
            actionState = (s32)state;
            return;
        }
        case AS_JUMP:
        {
            // PSX case 8: standing jump. DoJump, set direction cosines, playerFlags|=3.
            field344 = 0;
            stateDispatch = SD_JUMP;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_FALL, 0, 0, 0);
            }

            DoJump();

            field700 = 0;
            field712 = nullptr;
            field704 = 0;
            field706 = 0;
            playerFlags |= 3;

            // PSX: store jump direction cosines
            field720 = rmSin16(orientation.y);
            field724 = 0;
            field728 = rmSin16((s16)(orientation.y + 0x4000));

            field616 = 0;
            jumpReturnHeight = homePos.y;
            actionState = (s32)state;
            stateTimer = 0;
            return;
        }
        case AS_WALL_JUMP:
        {
            // PSX case 9: wall jump sequence.
            field344 = 0;
            stateDispatch = SD_WALLJUMP;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_WALL_JUMP_START, param, 0, 0);
            }
            velocity = {};
            contactForce = {};
            maxFallDivisor = 0;
            playerFlags &= ~2;
            field616 = 0;
            actionState = (s32)state;
            return;
        }
        case AS_RUN:
        {
            // PSX case 10: turnRate=4000, field616=0, gp+392=0
            turnRate = 4000;
            field616 = 0;
            forceAccum = 0;
            // Fall through to Humanoid for stateDispatch + animation setup
            break;
        }
        case AS_STRAFE:
        {
            // PSX case 11: face enemy + strafe init.
            // FindFoe, SetHumanoidTarget, stateDispatch=26(SD_STRAFE)
            stateTimer = 0;
            SetHumanoidTarget(FindFoe(PLAYER_STRAFE_TARGET_RANGE, PLAYER_STRAFE_TARGET_HALF_ANGLE, 0));
            field344 = 0;
            stateDispatch = SD_STRAFE;
            field348 = 8;
            actionState = (s32)state;
            return;
        }
        case AS_TAUNT_ENTRY:
        {
            // PSX Player switch has no dedicated case 4; use shared Humanoid mapping.
            Humanoid::SetActionState(state, param);
            return;
        }
        case AS_DIVE_ROLL:
        {
            // PSX case 12 uses dispatch slot 27 (strafe-chain) with anim 24.
            field344 = 0;
            stateDispatch = SD_DIVE_ROLL_CHAIN;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_STRAFE, param, 1, 0);
                // PSX case 12: un-mirror if mirrored (model->MirrorTree via vtable+80).
                RemovePlayerMirrorFlag(m);
            }
            field488 = 0;
            actionState = (s32)state;
            return;
        }
        case AS_FALL:
        {
            // PSX case 13: stateDispatch=29, play anim 39 frame 5,
            // field616=0, playerFlags|=1, jumpReturnHeight=homePos.y
            field344 = 0;
            stateDispatch = SD_FALL;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_FALL, param, 0, 0);
                AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                if (anim) {
                    anim->ForceFrame(5);
                }
            }
            field616 = 0;
            playerFlags |= 1;
            jumpReturnHeight = homePos.y;
            if (humanoidSound) {
                humanoidSound->Fall();
            }
            actionState = (s32)state;
            stateTimer = 0;
            return;
        }
        case AS_HARDFALL:
        {
            // PSX case 14: stateDispatch=-1, HardFall handler
            field344 = 0;
            stateDispatch = SD_HARDFALL;
            actionState = (s32)state;
            stateTimer = 0;
            return;
        }
        case AS_HARDLAND:
        {
            // PSX case 15: stateDispatch=-1, HardLand handler
            field344 = 0;
            stateDispatch = SD_HARDLAND;
            actionState = (s32)state;
            stateTimer = 0;
            return;
        }
        case AS_FLIP:
        {
            // PSX case 16: stateDispatch=-1, Flip handler, play anim 295, playerFlags|=2
            playerFlags |= 2;
            field344 = 0;
            stateDispatch = SD_FLIP;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_FLIP_VARIANT, param, 1, 0);
            }
            field616 = 0;
            if (humanoidSound) {
                humanoidSound->FrontFlip();
            }
            actionState = (s32)state;
            return;
        }
        case AS_FLIP_VARIANT:
        {
            // PSX case 17: stateDispatch=-1, Flip handler, play default anim
            field344 = 0;
            stateDispatch = SD_FLIP;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_ID_296, param, 1, 0);
            }
            actionState = (s32)state;
            return;
        }
        case AS_POLE_IDLE:
        {
            // PSX case 18: stateDispatch=44, zero velocity/force, pole idle setup
            field344 = 0;
            stateDispatch = SD_HORIZONTAL_POLE;
            field348 = 8;
            velocity = {};
            contactForce = {};
            flags2 &= ~0x70; // clear bits 4,5,6
            field616 = 0;
            actionStateFlag = 1;
            field424 = -38000;
            field420 = nullptr;
            actionState = (s32)state;
            return;
        }
        case AS_PUSH_OBJECT:
        {
            // PSX case 19: stateDispatch=-1, PushObject handler, anim=41 loopType=3.
            field344 = 0;
            stateDispatch = SD_PUSH_OBJECT;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(41, 3, 0, 0);
            }
            actionState = (s32)state;
            return;
        }
        case AS_SLOPE_SLIDE:
        {
            // PSX case 20: stateDispatch=47, play anim 34 frame 3, field616=0
            field344 = 0;
            stateDispatch = SD_SLOPE_SLIDE;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_ID_34, 3, 0, 0);
            }
            field616 = 0;
            if (humanoidSound) {
                humanoidSound->BeginSlideOnSurface((CSoundMaterial)field436);
            }
            actionState = (s32)state;
            return;
        }
        case AS_TABLE_ROLL:
        {
            // PSX case 21: stateDispatch=-1, TableRoll handler.
            // Zero velocity/force/maxFallDivisor, setup flags2.
            u32 f2 = (u32)flags2;
            if (((f2 >> 4) & 1) == 0 || (((f2 >> 5) & 1) != 0 && ((f2 >> 6) & 1) != 0)) {
                flags2 = (flags2 & ~0x70) | 0x10;
                field516 = 0;
                field520 = 0;
                field524 = 0;
            }
            // PSX: model->SetAnim(PLAYER_ANIM_TABLE_ROLL, ...) - start table roll animation
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_TABLE_ROLL, 0, 0, 0);
            }
            velocity = {};
            contactForce = {};
            maxFallDivisor = 0;
            field344 = 0;
            stateDispatch = SD_TABLE_ROLL;
            actionState = (s32)state;
            return;
        }
        case AS_PUSH:
        {
            // PSX: Player state 22 is configured by Humanoid::SetActionState
            // and routes to Humanoid::_TableThrow.
            Humanoid::SetActionState(state, param);
            return;
        }
        case AS_LEDGE_LATCH:
        {
            // PSX case 23: ledge latch setup.
            // PSX calls model->Animate() first to finalize the previous animation
            // before switching, preventing a one-frame position glitch.
            if (model) {
                Model* m = static_cast<Model*>(model);
                if (m->drawable && (m->drawable->displayFlag & 1)) {
                    m->Animate();
                }
            }
            field344 = 0;
            stateDispatch = SD_LEDGE_LATCH;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_LEDGE_LATCH, 0, 0, 0);
            }
            velocity = {};
            contactForce = {};
            field616 = 0;
            actionState = (s32)state;
            return;
        }
        case AS_LEDGE_PULLUP:
        {
            Humanoid::SetActionState(state, param);
            return;
        }
        case AS_PUNCH_ATTACK:
        case AS_KICK_ATTACK:
        {
            // PSX cases 32,34: acquire a foe and attempt to enter combat combo.
            if (rightHandObj || leftHandObj) {
                const Pickup* heldPickup = rightHandObj
                    ? static_cast<const Pickup*>(rightHandObj)
                    : static_cast<const Pickup*>(leftHandObj);
                LOG(
                    "[PickupCombat] player set state pickupType=%u nextState=%d cmd=0x%08X fromState=%d",
                    heldPickup ? static_cast<u32>(heldPickup->thingType) : 0u,
                    static_cast<s32>(state),
                    static_cast<u32>(commandBits),
                    actionState);
            }
            SetHumanoidTarget(FindFoe(750, 10922, 0));
            if (!EnterCombatCombo()) {
                return;
            }
            actionState = (s32)state;
            return;
        }
        case AS_PICKUP:
        {
            const u32 nearInteractionFlags = (u32)field368;
            if (((nearInteractionFlags >> 6) & 1u) != 0 || ((nearInteractionFlags >> 1) & 1u) != 0) {
                return;
            }

            // PSX case 44: pickup object - play weapon pickup dialog when present.
            s32 pickupAnim = 44;
            if (rightHandObj) {
                Pickup* pickup = static_cast<Pickup*>(rightHandObj);
                pickup->SetPickupMove(pos.y + PICKUP_MOVE_COMPARE_OFFSET);
                pickupAnim = static_cast<s32>(pickup->GetPickupMove());

                s32 dialogID = GetWeaponPickupDialog((s32)pickup->thingType);
                if (dialogID != 0 && LoadDialog((u32)dialogID, 0x33) != 0) {
                    PlayDialog((u32)dialogID, 0x3C);
                }
            }

            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(pickupAnim, param, 0, 0);
            }

            field344 = 0;
            // PSX case 44 uses direct callback Pickup__8Humanoid.
            stateDispatch = SD_NONE;
            actionState = (s32)state;
            return;
        }
        case AS_THROW_PICKUP:
        {
            const u32 nearInteractionFlags = (u32)field368;
            if (((nearInteractionFlags >> 6) & 1u) != 0 || ((nearInteractionFlags >> 1) & 1u) != 0) {
                DropPickup(1, 1);
                return;
            }

            // PSX case 45: preload throw voice line for throw release timing.
            LoadDialog(84, 0x33);
            SetHumanoidTarget(FindFoe(750, 10922, 0));
            if (!field256) {
                SetHumanoidTarget(FindFoe(THROW_FAR_RANGE, THROW_FAR_FIELD, 0));
            }

            if (model && rightHandObj) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(static_cast<s32>(static_cast<Pickup*>(rightHandObj)->GetThrowMove()), param, 0, 0);
            }

            field344 = 0;
            stateDispatch = SD_THROW;
            actionState = (s32)state;
            return;
        }
        case AS_GET_UP:
        {
            // PSX case 68: get up from knockdown.
            flags2 &= ~0x70;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(29, 0, 0, 0);
            }
            PlayDialogBasedOnPriority(0, 512);
            field344 = 0;
            stateDispatch = SD_GET_UP;
            field348 = 8;
            field616 = 0;
            field468 = 0;
            stateTimer = 0;
            actionState = (s32)state;
            return;
        }
        case AS_DEAD:
        {
            // PSX case 72: player death. Preserve carry-over motion for specific
            // incoming states, otherwise switch to player death anim 17.
            const s32 prevState = actionState;
            const bool preserveDeathMotion = (prevState >= 56 && prevState <= 59)
                || (prevState >= 69 && prevState <= 72);

            if (prevState == 64) {
                LoadDialog(55, 0xFF);
            }
            else if (prevState == 65) {
                LoadDialog(56, 0xFF);
            }
            else if (!preserveDeathMotion && model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(17, param, 0, 0);
            }

            if (model) {
                Model* m = static_cast<Model*>(model);
                AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                if (anim) {
                    anim->humanoidCB = {};
                }
            }

            field616 = 0;
            thinkCounter = 0;
            stateTimer = 0;
            flags |= 0x80;
            field344 = 0;
            stateDispatch = SD_DEAD_PLAYER;
            field348 = 8;
            actionState = (s32)state;
            return;
        }
        default:
            break;
    }

    // Delegate to Humanoid for the core state mapping and remaining cases
    Humanoid::SetActionState(state, param);

    // Animation fallbacks for states that go through Humanoid path
    switch (state) {
        case AS_RUN:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(PLAYER_ANIM_RUN, param, 0, 0);
            }
            break;
        default:
            break;
    }
}

// PSX: ProcessAction dispatches via method thunk (field344/346/348).
// PSX stateDispatch > 0 = vtable index, stateDispatch < 0 = direct function pointer.
// On PC, Player::ProcessAction handles all Player-specific dispatches via switch,
// then falls through to Humanoid::ProcessAction for shared Humanoid dispatches.
void Player::ProcessAction() {
    switch (stateDispatch) {
        // PSX class-specific slot dispatch: on Player, slots 23/27 both execute dive-roll handler.
        case SD_DIVE_ROLL:     _DiveRoll(); return;
        case SD_DIVE_ROLL_CHAIN: _DiveRoll(); return;
        // Player-specific handlers (PSX: direct function pointer, stateDispatch = -1)
        case SD_HARDFALL:      _HardFall(); return;
        case SD_HARDLAND:      _HardLand(); return;
        case SD_FLIP:          _Flip(); return;
        case SD_INACTIVE_IDLE: _InactiveIdle(); return;
        case SD_PUSH:          Humanoid::_TableThrow(); return;
        case SD_PUSH_OBJECT:   _PushObject(); return;
        case SD_TABLE_ROLL:    _TableRoll(); return;
        case SD_DO_STAND:      _DoStand(); return;
        // Player-specific handlers (PSX: vtable index dispatch)
        case SD_GET_UP:        Humanoid::_CrouchUp(); return;
        case SD_HORIZONTAL_POLE: _HorizontalPoleSwing(); return;
        case SD_LEDGE_LATCH:   _LedgeLatch(); return;
        case SD_LEDGE_PULLUP:  _LedgePullup(); return;
        case SD_SLOPE_SLIDE:   _SlopeSlide(); return;
        case SD_DEAD_PLAYER:   _Dead(); return;
        case SD_CLIMB_LADDER:  _ClimbLadder(); return;
        case SD_LADDER_DISMOUNT: _LadderDismount(); return;
        case SD_WALLJUMP:      _WallJump(); return;
        case SD_TEETER:        _Teetering(); return;
        default: Humanoid::ProcessAction(); return;
    }
}

// PSX: GetViewSpot__6PlayerP10tagLVectorT1 (PLAYER.CPP:1460)
void Player::GetViewSpot(LVector* outPos, LVector* outTarget) {
    MARKFUNCTION(0x8003027C);

    s32 state = actionState;

    if (outPos) {
        if (state == 18) {
            *outPos = pos;
        }
        else {
            *outPos = homePos;
        }
    }

    if (outTarget) {
        if (state == 18) {
            *outTarget = pos;
        }
        else if (state == (s32)AS_LEDGE_LATCH) {
            bool usedAnimMatrix = false;

            if (model) {
                Model* baseModel = static_cast<Model*>(model);
                HumanoidModel* humanoidModel = static_cast<HumanoidModel*>(baseModel);
                if (humanoidModel->animMatrices) {
                    const s32* joint5 = humanoidModel->animMatrices->GetMatrix(5);
                    if (joint5) {
                        const s32 tx = joint5[5];
                        const s32 ty = joint5[6];
                        const s32 tz = joint5[7];

                        const s32 dx = tx - homePos.x;
                        const s32 dy = ty - homePos.y;
                        const s32 dz = tz - homePos.z;
                        if ((dx >= -4096 && dx <= 4096) &&
                            (dy >= -4096 && dy <= 4096) &&
                            (dz >= -4096 && dz <= 4096)) {
                            outTarget->x = tx;
                            outTarget->y = ty;
                            outTarget->z = tz;
                            usedAnimMatrix = true;
                        }
                    }
                }
            }

            if (!usedAnimMatrix) {
                *outTarget = homePos;
                outTarget->y += 450;
            }
        }
        else {
            *outTarget = homePos;
            outTarget->y += 450;
        }
    }
}

// PSX: SignalEnemyGetUp__6Player (PLAYER.CPP:1382)
void Player::SignalEnemyGetUp() {
    MARKFUNCTION(0x800300B0);
    hitCombo++;
}

// PSX: DoJump__6Player (PLAYER.CPP:1424, 0x80030120)
void Player::DoJump() {
    MARKFUNCTION(0x80030120);
    // PSX: contactForce += {0, gp+460, 0}
    contactForce.y += PLAYER_JUMP_FORCE;
    flags &= ~TF_ON_GROUND;
    // PSX: sets field344=0, stateDispatch=28, field348=8
    field344 = 0;
    stateDispatch = SD_JUMP;
    field348 = 8;
    jumpReturnHeight = homePos.y;
}

// PSX: DoJump__6Playerl (PLAYER.CPP:1437, 0x800301D8)
// PSX: adds height to contactForce.y, clears TF_ON_GROUND
void Player::DoJump(s32 height) {
    MARKFUNCTION(0x800301D8);
    contactForce.y += height;
    flags &= ~TF_ON_GROUND;
    // PSX: sets field344=0, stateDispatch=28, field348=8
    field344 = 0;
    stateDispatch = SD_JUMP;
    field348 = 8;
    jumpReturnHeight = homePos.y;
}

// PSX: DoWallJump__6Player (PLAYER.CPP:3664, 0x80032C78)
void Player::DoWallJump() {
    MARKFUNCTION(0x80032C78);

    s32 angle = faceAngle + PSX_ANGLE_180;

    // PSX: adds small deltas to velocity.
    velocity.x += (s32)((-50LL * rmSin16((s16)angle)) >> 16);
    velocity.y += 120;
    velocity.z += (s32)((-50LL * rmSin16((s16)(angle + PSX_ANGLE_90))) >> 16);

    if (humanoidSound) {
        humanoidSound->WallJump();
    }
}

// PSX: FallingPhysics__6Player (PLAYER.CPP:3187, 0x80032368)
void Player::FallingPhysics() {
    MARKFUNCTION(0x80032368);

    // PSX checks bits 2(move), 3(jump), and 4(jump+dir) for in-air steering.
    u32 cb = (u32)commandBits;
    s32 hasInput = 0;
    if (((cb >> 2) & 1) || ((cb >> 4) & 1) || ((cb >> 3) & 1)) {
        hasInput = 1;
    }

    if (hasInput) {
        // PSX: FaceAngleY(this, faceAngle, 1)
        FaceAngleY(faceAngle, 1);
        // PSX: stack = {int32:0, int32:faceAngle, int32:0}
        // As SVector: {x=0, y=0, z=faceAngle, pad=0} - rotY in SVector.z slot
        SVector dir;
        dir.x = 0;
        dir.y = 0;
        dir.z = (s16)(faceAngle & 0xFFFF);
        dir.pad = 0;
        AddForce(150, &dir);
        gravity = 4096;
    }
    else {
        gravity = 2048;
    }
}

#if NEW_CHEATS
static constexpr u32 STUNTQUAKE_EFFECT_HASH = 0x0B0E21C4u;   // same visual as HUMANOID_LAND_IMPACT_EFFECT_HASH
static constexpr s32 STUNTQUAKE_RADIUS = 2500;
static constexpr s32 STUNTQUAKE_SHAKE_FRAMES = 0x0F;
static constexpr s32 STUNTQUAKE_DAMAGE = 10;
static constexpr s32 STUNTQUAKE_FIGHTING_POINTS = 100;
static constexpr s32 STUNTQUAKE_COLLISION_TAG_HIT_TYPE = static_cast<s32>(0x80000003u);
static constexpr s32 STUNTQUAKE_COLLISION_TAG_DAMAGE = static_cast<s32>(0x80000007u);
static constexpr s32 STUNTQUAKE_COLLISION_TAG_END = 0;

static void TriggerStuntquake(Player* player) {
    if (!player) {
        return;
    }

    Camera* camera = g_display ? g_display->GetCamera() : nullptr;
    if (camera) {
        camera->ShakeCamera(STUNTQUAKE_SHAKE_FRAMES);
    }

    GEffect_Create(STUNTQUAKE_EFFECT_HASH, &player->pos, nullptr, nullptr, 0, 0x19, 0);
    CSoundDirect::PlayTransient(0x28, &player->pos, 0, 0);
    CSoundDirect::PlayTransient(0x16, &player->pos, 0, 0);

    for (ccMinNode* node = g_ai ? g_ai->humanoidList.head : nullptr; node; node = node->next) {
        Humanoid* h = static_cast<Humanoid*>(node);
        if (!h || h == static_cast<Humanoid*>(player) || !h->health) {
            continue;
        }
        // Skip humanoids already in a collapse/stun reaction (AS_COLLAPSE_STUN..+3),
        // same range FindFoe excludes, so we don't interrupt an existing reaction.
        if ((u32)(h->actionState - (s32)AS_COLLAPSE_STUN) < 4u) {
            continue;
        }
        if (h->DistanceFromPoint(player->pos) > STUNTQUAKE_RADIUS) {
            continue;
        }

        h->HandleCollision(player, 1,
            STUNTQUAKE_COLLISION_TAG_HIT_TYPE, 0,
            STUNTQUAKE_COLLISION_TAG_DAMAGE, STUNTQUAKE_DAMAGE,
            STUNTQUAKE_COLLISION_TAG_END);
        h->SetActionState(AS_COLLAPSE_STUN, 0);

        if (g_hud) {
            g_hud->SetFoe(h);
        }
        if (g_scoreManager) {
            g_scoreManager->AddFightingPoints(STUNTQUAKE_FIGHTING_POINTS);
        }
    }
}
#endif

// PSX: CheckForLanding__6Player (PLAYER.CPP:4366, 0x80033C00)
// PSX: checks TF_ON_GROUND (set by collision system via Land() in HandleThingFloor)
void Player::CheckForLanding() {
    MARKFUNCTION(0x80033C00);
    if (!(flags & TF_ON_GROUND)) {
        return;
    }

#if NEW_CHEATS
    if (IsCheatEnabled(CheatOption::Stuntquake)) {
        TriggerStuntquake(this);
    }
#endif

    Model* m = model ? static_cast<Model*>(model) : nullptr;
    AnimStructure* anim = (m != nullptr) ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    s32 curAnim = anim ? anim->animEnum : 0;

    if (commandBits & CB_MOVE) {
        SetActionState(AS_RUN, 0);
        if (m) {
            s32 nextAnim = PLAYER_ANIM_RUN;
            if (curAnim == PLAYER_ANIM_ID_35) {
                nextAnim = PLAYER_ANIM_ID_37;
            }
            else if (curAnim == PLAYER_ANIM_ID_36) {
                nextAnim = PLAYER_ANIM_ID_38;
            }
            m->SetAnim(nextAnim, 0, 0, 0);
        }
    }
    else {
        SetActionState(AS_STAND, 0);
        if (m) {
            m->SetAnim(PLAYER_ANIM_HARD_FALL, 0, 0, 0);
        }
    }
    if (humanoidSound) {
        humanoidSound->Land((CSoundMaterial)field436);
    }
}

void Player::OnCheckpoint() {
    MARKFUNCTION(0x80033D0C);
    if (g_scoreManager) {
        g_scoreManager->HandleCheckpoint();
    }
    checkpoint.field0 = pos.x;
    checkpoint.field4 = pos.y;
    checkpoint.field8 = pos.z;
    checkpoint.field12 = orientation.x;
    checkpoint.field16 = orientation.y;
    checkpoint.field20 = orientation.z;
    if (g_blockManager) {
        checkpoint.field24 = g_blockManager->GetBlockNumber(pos);
    }
    checkpoint.field28 = 0;
    checkpoint.SetValidState(1);
}

void Player::SetLivesLeft(s32 lives) {
    MARKFUNCTION(0x80033D9C);
    if (lives >= 100) {
        lives = 99;
    }
    livesLeft = lives;
}

// PSX: GetWeaponPickupDialog__Fl (0x80030388)
static s32 GetWeaponPickupDialog(s32 weaponType) {
    MARKFUNCTION(0x80030388);

    if (weaponType == 308) {
        return 100;
    }
    if (weaponType == 325) {
        return 99;
    }

    return 0;
}

// PSX: GetWeaponFinalBlowDialog__Fl (0x80034338)
// Returns dialog ID for weapon final blow based on weapon thingType.
static s32 GetWeaponFinalBlowDialog(s32 weaponType) {
    MARKFUNCTION(0x80034338);
    switch (weaponType) {
        case 301: case 318: case 321: case 324:
            return 102;
        case 302:
            return 108;
        case 303: case 304: case 305: case 306: case 310: case 312:
        case 314: case 315: case 316: case 317: case 320: case 322: case 328:
            return 104;
        case 307:
            return 86;
        case 308:
            return 101;
        case 309:
            return 88;
        case 311:
            return 105;
        case 313:
            return 103;
        case 319:
            return 97;
        case 323:
            return 87;
        case 325:
            return 98;
        case 326:
            return 107;
        case 327:
            return 85;
        default:
            return 104;
    }
}

// PSX: SignalEnemyDead__6PlayerP8Humanoid (PLAYER.CPP:4828, 0x8003431C)
void Player::SignalEnemyDead(Humanoid* /*enemy*/) {
    MARKFUNCTION(0x8003431C);
    if (encounterState == 1) {
        encounterState = 2;
    }
}

// PSX: EnterCombatCombo__6Player (PLAYER.CPP:4967, 0x800343D4)
bool Player::EnterCombatCombo() {
    MARKFUNCTION(0x800343D4);
    return Humanoid::EnterCombatCombo() != 0;
}

// PSX: LoadCombatDialog__6Player (PLAYER.CPP:5000, 0x800343F4)
s32 Player::LoadCombatDialog() {
    MARKFUNCTION(0x800343F4);

    // PSX: a1[121] = +484 = field484 (FightingComboNode pointer)
    // PSX: if (!v2 || actionState == 37) return 0
    if (!field484 || actionState == (s32)AS_BACK_GRAB_LATCH) {
        return 0;
    }

    // PSX: v4 = *(field484+8), check *(u8*)(v4+19) == 1 (fighting type)
    // PSX: v6 = *(u8*)(*(v4+20)+1) - attack sub-type
    // These dereference FightingComboNode internals. Without fighting system data
    // loaded, field484 is 0 and we already returned above.
    // Full reversal of the node access requires FightingComboNode structure.

    if (!PlayerSingleEncounterCheak()) {
        return 0;
    }

    // PSX: check rightHandObj (weapon/held item) and sub-type in range 10-12
    Thing* weapon = rightHandObj;
    if (weapon) {
        s32 dialogID = GetWeaponFinalBlowDialog((s32)weapon->thingType);
        LoadDialog((u32)dialogID, 0x33);
        return 0;
    }

    // PSX: rmRangedRandom(2) to randomize between combat lines
    s32 rnd = (s32)rmRangedRandom(2);

    // PSX: check commandBits bit 8 and bit 9
    if ((commandBits >> 8) & 1) {
        if (rnd) {
            LoadDialog(26, 0x33);
            return 0;
        }
    }
    else if ((commandBits >> 9) & 1) {
        if (rnd) {
            LoadDialog(25, 0x33);
            return 0;
        }
    }

    LoadDialog(76, 0x33);
    return 0;
}

// PSX: PlayCombatKnockDownDialog__6Player15DamageTypesTags (PLAYER.CPP:5094, 0x80034510)
void Player::PlayCombatKnockDownDialog(s32 damageType) {
    MARKFUNCTION(0x80034510);
    switch (damageType) {
        case 2:
        case 3:
        case 5:
        {
            s32 sp = soundParam;
            if ((u32)(sp - 25) < 2 || sp == 76) {
                PlayDialog((u32)sp, 10);
            }
            break;
        }
        case 11:
        case 12:
        {
            Thing* weapon = rightHandObj;
            if (weapon) {
                s32 dialogID = GetWeaponFinalBlowDialog((s32)weapon->thingType);
                PlayDialog((u32)dialogID, 51);
            }
            break;
        }
        default:
            break;
    }
}

// PSX: HandleHitShock__6Player15DamageTypesTags (PLAYER.CPP:5155, 0x800345B8)
void Player::HandleHitShock(s32 damageType) {
    MARKFUNCTION(0x800345B8);
    switch (damageType) {
        case 1:
            Shock(SHOCK_1);
            break;
        case 2:
        case 3:
        case 5:
            Shock(SHOCK_3);
            break;
        case 4:
            Shock(SHOCK_2);
            break;
        default:
            break;
    }
}

// PSX: PlayCombatThrowDialog__6Player (PLAYER.HPP:496, 0x80034618)
void Player::PlayCombatThrowDialog() {
    MARKFUNCTION(0x80034618);
    PlayDialog(77, 10);
}

// PSX: PlayerSingleEncounterCheak__6Player (PLAYER.CPP:4660, 0x80034140)
// Returns 1 if there are fewer than 2 enemies nearby (single encounter).
// Returns 0 if 2+ enemies are within encounter range.
bool Player::PlayerSingleEncounterCheak() {
    MARKFUNCTION(0x80034140);

    s32 nearbyCount = 0;

    if (!encounterInitialized) {
        encounterInitialized = 1;
        encounterHumanoidArray = (void*)FightingCollision::GetHumanoidArray();
    }

    Humanoid** hArray = (Humanoid**)encounterHumanoidArray;
    for (s32 i = 0; i < FIGHTING_COLLISION_MAX; ++i) {
        Humanoid* h = hArray[i];
        if (h != (Humanoid*)this) {
            if (h) {
                if (h->actionState == (s32)AS_STAND_ANIM) {
                    ++nearbyCount;
                }
                else {
                    s32 dist = DistanceFromPointXZ(h->pos);
                    if (dist < encounterRange) {
                        ++nearbyCount;
                    }
                }
            }
        }
        if (nearbyCount >= 2) {
            return false;
        }
    }
    return true;
}

// PSX: LoadPlayerTauntResponse__6PlayerP8Humanoid (PLAYER.CPP:4712, 0x80034290)
void Player::LoadPlayerTauntResponse(Humanoid* target) {
    MARKFUNCTION(0x80034290);

    s32 dialogID;
    switch (target->thingType) {
        case 10:  // TT_GRONTAR
            dialogID = 66;
            break;
        case 12:  // TT_PAUL
            dialogID = 72;
            break;
        case 13:  // TT_OSCAR
            dialogID = 71;
            break;
        case 15:  // TT_DANTE
            dialogID = 70;
            break;
        case 17:  // TT_BUTCH
            dialogID = 67;
            break;
        default:
            dialogID = 73;
            break;
    }

    LoadDialog((u32)dialogID, 0x33);
}

// PSX: PlayPlayerTauntResponse__6Player (PLAYER.CPP:4786, 0x80034300)
void Player::PlayPlayerTauntResponse() {
    MARKFUNCTION(0x80034300);

    if (!soundHandle) {
        return;
    }
    if (!jcsValidateHandle(soundHandle)) {
        return;
    }

    u32 sp = (u32)soundParam;

    // PSX: valid dialog ranges [66,67] and [70,73]
    if (sp >= 66) {
        if (sp < 68) {
            PlayDialog(sp, 10);
            return;
        }
        if (sp < 74) {
            if (sp < 70) {
                return;
            }
            PlayDialog(sp, 10);
            return;
        }
    }
}

// PSX: _InactiveIdle__6Player (PLAYER.CPP:2331, 0x8003123C)
// Waits for the inactive idle animation callback. When triggered, plays
// currentAnimEnum with RUN_TO_LAST, then plays dialog. When animation
// completes or guard bit released, sets AS_STAND and calls _Stand() directly.
void Player::_InactiveIdle() {
    MARKFUNCTION(0x8003123C);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    bool animDone = false;

    // PSX: a1[187] = field748 (animCallbackData)
    if (animCallbackData) {
        // Animation callback triggered - play the stored animation
        animCallbackData = 0;
        m->ApplyAnimToModel(0, currentAnimEnum, ANIM_RUN_TO_LAST, 0, 0);
        PlayDialogBasedOnPriority(54, 54);
    }
    else if (animLoadState == 2 && anim) {
        // Check if animation enum matches and has completed a loop
        if (anim->animEnum == currentAnimEnum && anim->loopCount > 0) {
            animDone = true;
        }
    }

    // PSX: check commandBits bit 1 (guard)
    bool guardBit = ((commandBits >> 1) & 1) != 0;
    if (!guardBit || animDone) {
        // PSX: UnloadAnimation(0, 0, currentAnimEnum)
        if (g_characterManager) {
            g_characterManager->UnloadAnimationBatch(0, currentAnimEnum);
        }
        SetActionState(AS_STAND, 0);
        _Stand();
    }
}

// PSX: _Stand__6Player (PLAYER.CPP:2378, 0x80031350) - 1832 bytes, 117 blocks
// Priority-ordered command dispatch from commandBits.
// PSX: checks strafe, pickup/throw, combat, taunt, jump, run, idle anims.
// Nearly all paths fall through to LABEL_87 (standPostDispatch) except strafe
// and combat-success which return directly.
void Player::_Stand() {
    MARKFUNCTION(0x80031350);

    u32 cb = (u32)commandBits;

    // PSX: if NOT running (bit 2), clear stand-to-run counter (a1[154] = field616)
    if (((cb >> 2) & 1) == 0) {
        field616 = 0;
    }

    // PSX: bit 5 from stand enters dive-roll state (12).
    if ((cb >> 5) & 1) {
        FaceAngleY(faceAngle, 0);
        SetActionState(AS_DIVE_ROLL, 0);
        return;
    }

    // PSX command requests for grab-family input: bits 7,15,16,19.
    {
        s32 hasPickupBits = 0;
        if (((cb >> GA_GRAB) & 1) || ((cb >> GA_GRAB_FORWARD) & 1)
            || ((cb >> 16) & 1) || ((cb >> 19) & 1)) {
            hasPickupBits = 1;
        }
        if (hasPickupBits) {
            if (rightHandObj != 0 || leftHandObj != 0) {
                SetActionState(AS_THROW_PICKUP, 0);
            }
            else {
                if (CheckForPickup() == 0) {
                    SetActionState(AS_COMBAT_IDLE, 0);
                }
            }
            goto standPostDispatch;
        }
    }

    // PSX: bit 4 (guard/taunt) -> SetActionState(6) -> LABEL_87
    if ((cb >> 4) & 1) {
        SetActionState(AS_PAUSE, 0);
        goto standPostDispatch;
    }

    // PSX: bit 3 (jump) -> SetActionState(8) -> LABEL_87
    if ((cb >> 3) & 1) {
        SetActionState(AS_JUMP, 0);
        goto standPostDispatch;
    }

    // PSX: bit 2 (run) or bit 6 (strafe request) -> movement
    {
        s32 hasMove = 0;
        if (((cb >> 2) & 1) || ((cb >> 6) & 1)) {
            hasMove = 1;
        }
        if (hasMove) {
            // PSX: bit6 -> SetActionState(11) -> LABEL_87
            if ((cb >> 6) & 1) {
                SetActionState(AS_STRAFE, 0);
                goto standPostDispatch;
            }

            // PSX: angle diff computation (orientation.y - faceAngle, wrapped)
            s32 diff = orientation.y - faceAngle;
            if (diff > (s32)0xFFFF) {
                do { diff -= 0xFFFF; } while (diff > (s32)0xFFFF);
            }
            if (diff < 0) {
                do { diff += 0xFFFF; } while (diff < 0);
            }

            // PSX: if facing different direction, check turn-around vs immediate run
            if (faceAngle != orientation.y) {
                s32 absDiff = (diff >= 0) ? diff : -diff;
                // PSX: range [24577, 40960) -> turn around first
                if ((u32)(absDiff - 24577) < 0x3FFF) {
                    turnAroundFlag = 1;
                    turnTargetAngle = faceAngle;
                    goto standPostDispatch;
                }
                // PSX: not in turn range -> immediate run (param 0)
                SetActionState(AS_RUN, 0);
                goto standPostDispatch;
            }

            // PSX: faceAngle == orientation.y -> field616 ramp counter
            // Read counter BEFORE increment (PSX: v18 = a1[154]; a1[154] = v18+1; if v18<3)
            s32 counter = field616;
            field616 = counter + 1;
            if (counter < 3) {
                SVector dir = {};
                dir.z = (s16)(faceAngle & 0xFFFF);
                AddForce(PLAYER_RUN_FORCE, &dir);
            }
            else {
                SetActionState(AS_RUN, 3);
            }
            goto standPostDispatch;
        }
    }

    // PSX: combat bits 7-20 -> punch attack
    {
        s32 hasCombat = 0;
        if (((cb >> GA_GRAB) & 1) || ((cb >> GA_PUNCH) & 1) || ((cb >> GA_KICK) & 1) ||
            ((cb >> GA_BACK_PUNCH) & 1) || ((cb >> GA_BACK_KICK) & 1)
            || ((cb >> GA_HEAVY_PUNCH) & 1) || ((cb >> GA_HEAVY_KICK) & 1)
            || ((cb >> GA_SPECIAL_GRAB) & 1) || ((cb >> GA_GRAB_FORWARD) & 1)
            || ((cb >> 16) & 1)
            || ((cb >> GA_GRAB_HELD) & 1) || ((cb >> GA_GRAB_FWD_HELD) & 1)
            || ((cb >> 19) & 1)
            || ((cb >> 20) & 1)) {
            hasCombat = 1;
        }
        if (hasCombat) {
            SetActionState(AS_PUNCH_ATTACK, 0);
            // PSX: if SetActionState returned non-zero, return; else LABEL_87
            if (actionState == AS_PUNCH_ATTACK) {
                return;
            }
            goto standPostDispatch;
        }
    }

    // PSX: not on ground -> fall off ledge
    if (!(flags & TF_ON_GROUND)) {
        s32 floorHeight = (s32)0x80000001;
        if (model) {
            const Model* trackedModel = static_cast<const Model*>(model);
            if (trackedModel->field36) {
                const ModelFloorHeightState* floorState = GetModelFloorHeightState(trackedModel);
                floorHeight = floorState->current;
            }
        }
        if (floorHeight != (s32)0x80000001 && pos.y - floorHeight < 129) {
            homePos.y = floorHeight;
        }
        else {
            SetActionState(AS_FALL, 0);
        }
        goto standPostDispatch;
    }

    // PSX: slope check (flags bit 17)
    if ((flags >> 17) & 1) {
        SetActionState(AS_SLOPE_SLIDE, 0);
        goto standPostDispatch;
    }

    // PSX: on ground - check current animation for post-transition handling
    if (model) {
        Model* m = static_cast<Model*>(model);
        AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
        if (anim) {
            s32 curAnim = anim->animEnum;

            // PSX: flags bit 18 -> taunt idle (anim 47)
            if ((flags >> 18) & 1) {
                if (curAnim != PLAYER_ANIM_TAUNT_IDLE) {
                    m->SetAnim(PLAYER_ANIM_TAUNT_IDLE, 4, 0, 0);
                    if (LoadDialog(96, 51)) {
                        PlayDialog(96, 180);
                    }
                    playerFlags &= ~PF_COMBAT_READY;
                }
            }
            else if (curAnim == PLAYER_ANIM_FORWARD_ROLL) {
                // PSX: anim 27 - decelerate force accumulator (gp+392 -= gp+400)
                forceAccum -= FORCE_DECEL_RATE;
                if (forceAccum <= 0) {
                    forceAccum = 0;
                }
                else {
                    SVector dir = {};
                    dir.z = (s16)(faceAngle & 0xFFFF);
                    AddForce(forceAccum, &dir);
                }
            }
            else {
                // PSX: anim 40, 46 wait for completion; else check idle
                s32 loopTypeParam;
                if (curAnim == PLAYER_ANIM_HARD_FALL) {
                    if (anim->loopCount == 0) {
                        goto standPostDispatch;
                    }
                    loopTypeParam = 2;
                }
                else if (curAnim == PLAYER_ANIM_DIVE_ROLL_TURN) {
                    if (anim->loopCount <= 0) {
                        goto standPostDispatch;
                    }
                    loopTypeParam = 0;
                }
                else {
                    if (TestIdleAnimation()) {
                        goto standPostDispatch;
                    }
                    loopTypeParam = 0;
                }
                idleTimer = 0;
                playerFlags |= PF_COMBAT_READY;
                SetIdleAnimation(loopTypeParam, 1);
            }
        }
    }

standPostDispatch:
    // PSX: LABEL_87 - turn-around system
    if (turnAroundFlag) {
        // PSX: read timer BEFORE increment, compare threshold < old value
        s16 timer = turnAroundTimer;
        turnAroundTimer = timer + 1;
        if (turnDelayThreshold < timer) {
            if ((commandBits >> 2) & 1) {
                // Still pressing run -> transition to run
                SetActionState(AS_RUN, 0);
            }
            else {
                // Face stored angle and play turn anim
                FaceAngleY(turnTargetAngle, 1);
                if (model) {
                    s32 modelAnim = 0;
                    Model* m = static_cast<Model*>(model);
                    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                    if (anim) {
                        modelAnim = anim->animEnum;
                    }
                    if (modelAnim != PLAYER_ANIM_DIVE_ROLL_TURN) {
                        m->SetAnim(PLAYER_ANIM_DIVE_ROLL_TURN, 0, 0, 0);
                    }
                }
                turnAroundFlag = 0;
                turnAroundTimer = 0;
            }
        }
    }
    else {
        FaceAngleY(faceAngle, 1);
    }

    // PSX: idle timer (gp+420, threshold at gp+478)
    idleTimer++;
    if ((s16)idleAnimThreshold < idleTimer) {
        idleTimer = 0;
        // PSX: checks MEMORY[0xA8] level flag (not yet wired)
        if (!rightHandObj && !leftHandObj) {
            SetActionState(AS_WALL_JUMP_TAUNT, 0);
        }
    }
}

// PSX: _Flip__6Player (PLAYER.CPP:2683, 0x80031A78)
// Handles flip animations (forward flip, turning flip, flip variant).
// Applies directional force based on animEnum, checks landing.
void Player::_Flip() {
    MARKFUNCTION(0x80031A78);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    s32 animE = anim->animEnum;
    s32 force;

    // PSX: if animEnum == 295 (flip variant), maxFallDivisor=13, force=1500
    // Otherwise maxFallDivisor=15, force=1000
    if (animE == PLAYER_ANIM_FLIP_VARIANT) {
        maxFallDivisor = 13;
        force = 1500;
    }
    else {
        maxFallDivisor = 15;
        force = 1000;
    }

    if (animE == PLAYER_ANIM_FORWARD_FLIP) {
        // Forward flip: AddForce in facing direction at runSpeed
        // PSX: 3 s32 on stack {orientation.x, faceAngle, orientation.z},
        // reinterpreted as __int16* - SVector.z = lo16(faceAngle)
        SVector dir;
        dir.x = (s16)(orientation.x & 0xFFFF);
        dir.y = (s16)((orientation.x >> 16) & 0xFFFF);
        dir.z = (s16)(faceAngle & 0xFFFF);
        dir.pad = 0;
        AddForce(runSpeed, &dir);
    }
    else if (animE == PLAYER_ANIM_TURNING_FLIP) {
        // Turning flip: temporarily boost turnRate, face target angle
        u16 savedTurnRate = turnRate;
        turnRate = 5000;
        FaceAngleY(faceAngle, 1);
        turnRate = savedTurnRate;
        SVector dir;
        dir.x = (s16)(orientation.x & 0xFFFF);
        dir.y = (s16)((orientation.x >> 16) & 0xFFFF);
        dir.z = (s16)(faceAngle & 0xFFFF);
        dir.pad = 0;
        AddForce(runSpeed, &dir);
    }
    else {
        // Default flip: check CB_MOVE for directional control
        if (!(commandBits & CB_MOVE)) {
            goto handleLanding;
        }
        // Check angle difference for immediate vs gradual turn
        s32 diff = orientation.y - faceAngle;
        s32 absDiff = (diff >= 0) ? diff : -diff;
        s32 immediate = (absDiff < 16385) ? 1 : 0;
        FaceAngleY(faceAngle, immediate);
        SVector dir;
        dir.x = (s16)(orientation.x & 0xFFFF);
        dir.y = (s16)((orientation.x >> 16) & 0xFFFF);
        dir.z = (s16)(faceAngle & 0xFFFF);
        dir.pad = 0;
        AddForce(force, &dir);
    }

handleLanding:
    // PSX: vtable+204 = CheckForLanding
    CheckForLanding();

    // PSX: if still airborne, set flip gravity and test ledges.
    if (flags & TF_ON_GROUND) {
        gravity = 0x8000;
    }
    else {
        gravity = 4999;
        CheckForLedges();
    }
}

// PSX: _Jump__6Player (PLAYER.CPP:2830, 0x80031C68)
// PSX: wall jump check, combat transitions, jump phase tracking via field700/704/706/712,
// jump table system (standingJumpHold/Tap, runJumpHold/Tap), air control,
// HandleLand call, transition to fall when velocity negative AND height threshold met.
void Player::_Jump() {
    MARKFUNCTION(0x80031C68);

    s32 jumpHeld = 0;
    if (g_inputManager && behaviour) {
        Button* jumpButton = g_inputManager->GetButtonForBit((u16)behaviour->padPort, 6);
        if (jumpButton && jumpButton->duration) {
            jumpHeld = 1;
        }
    }
    Model* m = model ? static_cast<Model*>(model) : nullptr;
    AnimStructure* anim = (m != nullptr) ? static_cast<AnimStructure*>(m->animStructure) : nullptr;

    // PSX: check playerFlags bit 1 for wall jump eligibility
    // PSX: check commandBits bits 8,9,14 for combat air attack

    if ((playerFlags & 2) != 0) {
        u32 cb = (u32)commandBits;
        s32 wantsWallJump = 0;
        if (((cb >> 3) & 1) || ((cb >> 4) & 1)) {
            wantsWallJump = 1;
        }

        if (wantsWallJump && ((flags & TF_ON_GROUND) == 0)) {
            s32 floorHeight = (s32)0x80000001;
            if (m && m->field36) {
                const ModelFloorHeightState* floorState = GetModelFloorHeightState(m);
                floorHeight = floorState->current;
            }
            if (floorHeight == (s32)0x80000001 || (pos.y - floorHeight) > WALL_JUMP_MIN_HEIGHT_ABOVE_FLOOR) {
                s32 wallAngle = 0;
                LVector wallPos = {};
                s32 hasWall = CheckWallConstraint(
                    WALL_JUMP_MIN_WALL_HEIGHT,
                    WALL_JUMP_TRACE_DISTANCE,
                    WALL_JUMP_MIN_ANGLE,
                    wallAngle,
                    wallPos);
                if (hasWall) {
                    orientation.y = wallAngle + 0x8000;
                    homePos = wallPos;
                    SetActionState(AS_WALL_JUMP, 0);
                    return;
                }
            }
        }
    }

    {
        u32 cb = (u32)commandBits;
        s32 wantsAirAttack = 0;
        if (((cb >> 8) & 1) || ((cb >> 9) & 1) || ((cb >> 14) & 1)) {
            wantsAirAttack = 1;
        }

        if (wantsAirAttack) {
            commandBits = (s32)(((u32)commandBits | 0x4000u) & ~0x0100u & ~0x0200u);
            SetActionState(AS_PUNCH_ATTACK, 0);
            if (actionState == AS_PUNCH_ATTACK) {
                velocity.y = 0;
                if (m && m->drawable && ((m->drawable->displayFlag & 1) != 0)) {
                    m->Animate();
                }
            }
        }
    }

    m = model ? static_cast<Model*>(model) : nullptr;
    anim = (m != nullptr) ? static_cast<AnimStructure*>(m->animStructure) : nullptr;

    // PSX: vtable+204 = CheckForLanding
    CheckForLanding();

    // PSX: field700 jump phase state machine, gated by animation frame thresholds.
    if (field700 != 0) {
        if (field700 == 2 && (playerFlags & 1) == 0 && anim) {
            s16 frame = (s16)((u32)anim->currentFrame >> 16);
            if (frame >= 7) {
                if (m) {
                    m->SetAnim(PLAYER_ANIM_ID_35, 0, 0, 0);
                }
                anim->ForceFrame(frame);
                field700 = 3;
            }
        }
    }
    else {
        if ((playerFlags & 1) == 0) {
            if (field712 == s_runJumpHold) {
                if (jumpHeld) {
                    s16 frame = anim ? (s16)((u32)anim->currentFrame >> 16) : 0;
                    if (frame >= 6) {
                        s32 floorHeight = (s32)0x80000001;
                        if (m && m->field36) {
                            const ModelFloorHeightState* floorState = GetModelFloorHeightState(m);
                            floorHeight = floorState->current;
                        }
                        if (floorHeight == (s32)0x80000001 || (pos.y - floorHeight) >= 1024) {
                            field700 = 1;
                        }
                        else {
                            field700 = 2;
                        }
                    }
                }
                else {
                    field712 = s_runJumpTap;
                    field700 = 2;
                }
            }
        }
        else if (jumpHeld) {
            s16 frame = anim ? (s16)((u32)anim->currentFrame >> 16) : 0;
            if (frame >= 5) {
                field700 = 1;
                field704 = 1;
                field706 = 0;
                field712 = s_standingJumpHold;
            }
        }
        else {
            field700 = 2;
            field706 = 1;
            field704 = 0;
            field712 = s_standingJumpTap;
        }
    }

    // PSX: apply jump table parameters (LABEL_37 in decompile)
    if (field712) {
        FaceAngleY(faceAngle, 1);

        // maxFallDivisor from jump table
        // PSX: if holding button, reduce gravity by 4 for higher jump
        if (field704 && jumpHeld) {
            maxFallDivisor = field712[2] - 4;
        }
        else {
            if (field704) {
                field704 = 0;  // clear hold flag on release
            }
            maxFallDivisor = field712[2];
        }

        // PSX includes bits 2,3,4,6 in directional checks here.
        u32 cb = (u32)commandBits;
        s32 hasDir = 0;
        if (((cb >> 2) & 1) || ((cb >> 6) & 1) || ((cb >> 4) & 1) || ((cb >> 3) & 1)) {
            hasDir = 1;
        }

        // Gravity (XZ drag): full from table with input, half without
        if (hasDir) {
            gravity = field712[1];
        }
        else {
            gravity = field712[1] / 2;
        }

        // Air movement force with directional input
        if (hasDir) {
            FaceAngleY(faceAngle, 1);
            SVector dir = {};
            dir.z = (s16)(faceAngle & 0xFFFF);
            s32 forceToAdd = ((flags & TF_ON_GROUND) != 0) ? runSpeed : field712[0];
            AddForce(forceToAdd, &dir);

            // PSX: standing jump momentum preservation (field720/728 cosines)
            if (field712 == s_standingJumpTap || field712 == s_standingJumpHold) {
                s32 sinY = rmSin16(faceAngle);
                s32 cosY = rmSin16((s16)(faceAngle + 0x4000));
                s64 momentum = ((s64)field720 * (s64)sinY >> 16)
                    + ((s64)field728 * (s64)cosY >> 16);
                if ((s32)momentum > 58982) {
                    AddForce(6000, &dir);
                    field720 = 0;
                    field724 = 0;
                    field728 = 0;
                }
            }

            // PSX: vtable+228 = CheckForLedges (return value not checked)
            CheckForLedges();
        }
    }



    // PSX: fall transition: velocity.y <= 0 AND jumpReturnHeight - homePos.y >= 2561
    if (velocity.y <= 0) {
        if (jumpReturnHeight - homePos.y >= 2561) {
            SetActionState(AS_FALL, 3);
            field616 = 100;
        }
    }
}

// PSX: _Fall__6Player (PLAYER.CPP:3226, 0x80032444)
// PSX: FallingPhysics, hard-fall velocity check, CheckForLanding (vtable+204),
// TF_ON_GROUND check, CheckForLedges (vtable+228) if not on ground.
void Player::_Fall() {
    MARKFUNCTION(0x80032444);

    // PSX: air control + gravity setting
    FallingPhysics();

    // PSX: if fall speed exceeds threshold -> hard fall (AS 14)
    if (g_hardFallThreshold >= velocity.y) {
        SetActionState(AS_HARDFALL, 0);
        return;
    }

    // PSX: vtable+204 = CheckForLanding
    CheckForLanding();

    // PSX: if NOT on ground, check for ledges (vtable+228)
    u32 cb = (u32)commandBits;
    s32 hasDir = 0;
    if (((cb >> 2) & 1) || ((cb >> 6) & 1) || ((cb >> 4) & 1) || ((cb >> 3) & 1)) {
        hasDir = 1;
    }
    if (!(flags & TF_ON_GROUND) && hasDir) {
        CheckForLedges();
    }
}

// PSX: _HardFall__6Player (PLAYER.CPP:3343, 0x800324E8)
// Applies falling physics, checks for ground contact.
// On landing: transitions to Stand, then applies hard-fall landing animation.
void Player::_HardFall() {
    MARKFUNCTION(0x800324E8);

    FallingPhysics();

    // PSX: (flags >> 12) & 1 = TF_ON_GROUND
    if (flags & TF_ON_GROUND) {
        SetActionState(AS_STAND, 0);
        // PSX: model->SetAnim(40, 0, 0, 0)
        if (model) {
            Model* m = static_cast<Model*>(model);
            m->SetAnim(PLAYER_ANIM_HARD_FALL, 0, 0, 0);
        }
    }
}

// PSX: _HardLand__6Player (PLAYER.CPP:3365, 0x80032560)
// Waits for hard-land animation to complete (loopCount > 0).
// Transitions to GET_UP if alive, DEAD if health is 0.
void Player::_HardLand() {
    MARKFUNCTION(0x80032560);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    // PSX: check loopCount > 0 (animation completed)
    if (anim->loopCount > 0) {
        // PSX: if health > 0, GET_UP (68); else DEAD (72)
        if (health > 0) {
            SetActionState(AS_GET_UP, 0);
        }
        else {
            SetActionState(AS_DEAD, 0);
        }
    }
}

// PSX: _Run__6Player (PLAYER.CPP:3397, 0x800325CC) - 1148 bytes, 73 blocks
// PSX flow: pickup/strafe -> bit1 (return) -> slope/jump/combat (fall through)
// -> strafe (return) -> FaceAngleY -> ground check -> force accumulator.
// Most transitions fall through to the force accumulator at LABEL_58.
void Player::_Run() {
    MARKFUNCTION(0x800325CC);

    u32 cb = (u32)commandBits;

    // PSX: pickup/throw/strafe section (all fall through to LABEL_13)
    {
        s32 hasPickup = 0;
        if (((cb >> GA_GRAB) & 1) || ((cb >> GA_GRAB_FORWARD) & 1)
            || ((cb >> 16) & 1)) {
            hasPickup = 1;
        }
        if (hasPickup) {
            if (rightHandObj != 0 || leftHandObj != 0) {
                SetActionState(AS_THROW_PICKUP, 0);
                // PSX: falls through to LABEL_13
            }
            else {
                CheckForPickup();
            }
        }
        else if ((cb >> 6) & 1) {
            SetActionState(AS_STRAFE, 0);
            // PSX: falls through to LABEL_13
        }
    }

    // LABEL_13: guard release (bit1) without run (bit2) -> stand + roll -> return
    {
        s32 wantStop = 0;
        if (((cb >> 1) & 1) != 0) {
            wantStop = ((cb >> 2) & 1) == 0;
        }
        if (wantStop) {
            SetActionState(AS_STAND, 0);
            if (model) {
                Model* m = static_cast<Model*>(model);
                s32 rollAnim = PLAYER_ANIM_DIVE_ROLL_TURN;
                s32 rollParam = 0;
                if (faceAngle == orientation.y) {
                    rollAnim = PLAYER_ANIM_FORWARD_ROLL;
                    rollParam = 1;
                }
                m->SetAnim(rollAnim, rollParam, 0, 0);
            }
            return;
        }
    }

    // PSX: slope/jump/combat transitions (all fall through to LABEL_45)
    if ((flags >> 17) & 1) {
        // PSX: slope -> SetActionState(20) -> falls to LABEL_45
        SetActionState(AS_SLOPE_SLIDE, 0);
    }
    else {
        s32 hasJumpTaunt = 0;
        if (((cb >> 3) & 1) || ((cb >> 4) & 1)) {
            hasJumpTaunt = 1;
        }
        if (hasJumpTaunt) {
            // PSX: jump/taunt -> SetActionState(6) -> falls to LABEL_45
            SetActionState(AS_PAUSE, 0);
        }
        else {
            s32 hasCombat = 0;
            if (((cb >> GA_GRAB) & 1) || ((cb >> GA_PUNCH) & 1) || ((cb >> GA_KICK) & 1) ||
                ((cb >> GA_BACK_PUNCH) & 1) || ((cb >> GA_BACK_KICK) & 1)
                || ((cb >> GA_HEAVY_PUNCH) & 1) || ((cb >> GA_HEAVY_KICK) & 1)
                || ((cb >> GA_SPECIAL_GRAB) & 1) || ((cb >> GA_GRAB_FORWARD) & 1)
                || ((cb >> 16) & 1)
                || ((cb >> GA_GRAB_HELD) & 1) || ((cb >> GA_GRAB_FWD_HELD) & 1)
                || ((cb >> 19) & 1)
                || ((cb >> 20) & 1)) {
                hasCombat = 1;
            }
            if (hasCombat) {
                // PSX: combat -> SetActionState(32) -> falls to LABEL_45
                SetActionState(AS_PUNCH_ATTACK, 0);
            }
        }
    }

    // LABEL_45: dive roll (returns) or force path (falls through to LABEL_58)
    if ((cb >> 5) & 1) {
        LVector ledgeNormal = {};
        LVector ledgePos = {};
        if (CheckForLedges2(ledgeNormal, ledgePos, 200)) {
            s32 ledgeDelta = ledgePos.y - pos.y - 365;
            if ((u32)ledgeDelta < 0x4F) {
                if (!Obstacle::DetectObstacleAboveLedge(ledgeNormal, ledgePos)) {
                    SetActionState(AS_TABLE_ROLL, 0);
                    return;
                }
            }
        }

        SetActionState(AS_DIVE_ROLL, 0);
        FaceAngleY(faceAngle, 0);
        return;
    }

    // PSX: face toward input direction
    FaceAngleY(faceAngle, 1);

    // PSX: if NOT on ground, check for falling off ledge
    if (!(flags & TF_ON_GROUND)) {
        s32 floorHeight = (s32)0x80000001;
        if (model) {
            const Model* trackedModel = static_cast<const Model*>(model);
            if (trackedModel->field36) {
                const ModelFloorHeightState* floorState = GetModelFloorHeightState(trackedModel);
                floorHeight = floorState->current;
            }
        }
        if (floorHeight != (s32)0x80000001 && pos.y - floorHeight < 129) {
            pos.y = floorHeight;
        }
        else {
            if (actionState == AS_RUN) {
                SetActionState(AS_FALL, 3);
            }
        }
    }

    // LABEL_58: force accumulator (runs unconditionally on non-strafe path)
    // PSX: gp+392 += gp+396, capped at a1[53] (runSpeed)
    forceAccum += FORCE_RAMP_RATE;
    if ((u32)runSpeed < (u32)forceAccum) {
        forceAccum = runSpeed;
    }
    SVector dir = {};
    dir.z = (s16)(faceAngle & 0xFFFF);
    AddForce(forceAccum, &dir);
}

// PSX: _Push__6Player (PLAYER.CPP:3550, 0x80032A48)
// Handles push state: checks guard/jump/run bits for transitions.
// If running toward push target (angle within threshold), zeros velocity.
void Player::_Push() {
    MARKFUNCTION(0x80032A48);

    u32 cb = (u32)commandBits;

    // PSX: bit 1 (guard) -> Stand with param 3
    if ((cb >> 1) & 1) {
        SetActionState(AS_STAND, 3);
        return;
    }

    // PSX: bit 3 (jump) -> Pause
    if ((cb >> 3) & 1) {
        SetActionState(AS_PAUSE, 0);
        return;
    }

    // PSX: bit 2 (run) -> check angle for push direction
    if (!((cb >> 2) & 1)) {
        return;
    }

    // PSX: angle check between faceAngle and orientation.y
    s32 targetAngle = faceAngle;
    if (targetAngle == 0) {
        targetAngle = PSX_ANGLE_360;
    }
    else if (targetAngle > PSX_ANGLE_360) {
        targetAngle %= PSX_ANGLE_360;
    }

    s32 diff = targetAngle - orientation.y;
    if (diff < 0) {
        diff = -diff;
    }
    if (diff > PSX_ANGLE_180) {
        s32 correction = (diff > 0) ? -PSX_ANGLE_360 : PSX_ANGLE_360;
        diff = targetAngle - orientation.y + correction;
    }
    s32 absDiff = (diff >= 0) ? diff : -diff;

    // PSX: threshold 4552 - if angle difference too large, transition to strafe
    if (absDiff >= 4552) {
        SetActionState(AS_RUN, 0);
        return;
    }

    // Facing toward push target - zero velocity
    velocity = {};
}

// PSX: _PushObject__6Player (PLAYER.CPP:3604, 0x80032B80)
// Handles pushing an object. Checks guard/jump/taunt bits.
// If running, applies force in facing direction.
void Player::_PushObject() {
    MARKFUNCTION(0x80032B80);

    u32 cb = (u32)commandBits;

    // PSX: bit 1 (guard) -> Stand with param 3
    if ((cb >> 1) & 1) {
        SetActionState(AS_STAND, 3);
        return;
    }

    // PSX: bit 3 (jump) or bit 4 (taunt) -> state 6
    s32 hasJumpOrTaunt = 0;
    if (((cb >> 3) & 1) || ((cb >> 4) & 1)) {
        hasJumpOrTaunt = 1;
    }
    if (hasJumpOrTaunt) {
        SetActionState(AS_PAUSE, 0);
        return;
    }

    // PSX: bit 2 (run) -> apply force and face direction
    if (!((cb >> 2) & 1)) {
        return;
    }

    // PSX: checks field368 bit 2 for push confirmation
    if ((field368 >> 2) & 1) {
        SVector dir;
        dir.x = (s16)(orientation.x & 0xFFFF);
        dir.y = (s16)((orientation.x >> 16) & 0xFFFF);
        dir.z = (s16)(orientation.y & 0xFFFF);
        dir.pad = (s16)((orientation.y >> 16) & 0xFFFF);
        AddForce(forceAccum, &dir);
        FaceAngleY(faceAngle, 1);
        return;
    }

    SetActionState(AS_RUN, 0);
}

// PSX: _Teetering__6Player (PLAYER.CPP:3659, 0x80032C70)
// Empty function on PSX (COLLAPSED, 8 bytes)
void Player::_Teetering() {
    MARKFUNCTION(0x80032C70);
}

// PSX: _WallJump__6Player (PLAYER.CPP:3688, 0x80032D8C)
// Handles wall jump sequence: wall contact anim (32) -> launch anim (33) with force.
// Transitions to fall on launch completion, or stand on ground contact.
void Player::_WallJump() {
    MARKFUNCTION(0x80032D8C);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    s32 animE = anim->animEnum;

    if (animE == PLAYER_ANIM_WALL_JUMP_START) {
        maxFallDivisor = 0;
    }

    // PSX: anim 33 applies horizontal force in facing direction.
    s32 launchDone = 0;
    if (animE == PLAYER_ANIM_WALL_JUMP_LAUNCH) {
        SVector dir;
        dir.x = (s16)(orientation.x & 0xFFFF);
        dir.y = (s16)((orientation.x >> 16) & 0xFFFF);
        dir.z = (s16)(orientation.y & 0xFFFF);
        dir.pad = (s16)((orientation.y >> 16) & 0xFFFF);
        AddForce(4000, &dir);
    }

    // PSX: if anim 33 completed (loopCount > 0) -> transition to fall
    if (anim->animEnum == PLAYER_ANIM_WALL_JUMP_LAUNCH) {
        launchDone = (anim->loopCount != 0) ? 1 : 0;
    }

    if (launchDone) {
        SetActionState(AS_FALL, 0);
        SetDesiredMoveDirection(orientation.y);
        FaceAngleY(orientation.y, 0);
        return;
    }

    // PSX: CheckForLedges (vtable+228), then CheckForLanding (vtable+204)
    if (CheckForLedges()) {
        return;
    }

    CheckForLanding();
    if (flags & TF_ON_GROUND) {
        SetDesiredMoveDirection(orientation.y);
        FaceAngleY(orientation.y, 0);
    }
}

// PSX: _Collapse__6Player (PLAYER.CPP:3732, 0x80032EB0)
// Player-specific collapse: calls TestAndSetRisingAttack (vtable+260),
// increments counter, checks humanoidDataID threshold for GET_UP vs DEAD.
void Player::_Collapse() {
    MARKFUNCTION(0x80032EB0);

    // PSX: vtable+260 = TestAndSetRisingAttack
    TestAndSetRisingAttack();

    // PSX: increment field616 counter
    field616++;

    // PSX: check if counter exceeds humanoidDataID threshold
    s16 counter = (s16)field616;
    if ((s16)humanoidDataID < counter) {
        if (health > 0) {
            SetActionState(AS_GET_UP, 0);
        }
        else {
            field616 = 0;
            SetActionState(AS_DEAD, 0);
        }
    }
}

// PSX: _DoStand__6Player (PLAYER.CPP:3757, 0x80032F48)
// Transitions to Stand and zeros velocity vector.
void Player::_DoStand() {
    MARKFUNCTION(0x80032F48);
    SetActionState(AS_STAND, 0);
    velocity = {};
}
// PSX: _HorizontalPoleSwing__6Player (PLAYER.CPP:3802, 0x80032F8C)
// Pendulum swing physics on horizontal pole. Builds rotation matrix from
// orientation, applies angular velocity driven by gravity torque,
// transforms pendulum arm to get world position.
void Player::_HorizontalPoleSwing() {
    MARKFUNCTION(0x80032F8C);

    // PSX: pendulum arm offset (0, -660, 0)
    LVector pendulumArm = { 0, -660, 0 };

    // PSX: save orientation
    s32 angleX = orientation.x;
    s32 angleY = orientation.y;
    s32 angleZ = orientation.z;

    // PSX: save pos into stack vars (v30/v31/v32)
    LVector savedPos = pos;

    // PSX: pole anchor position at byte offset 296 = {collBboxMax.y, collBboxMax.z, field304}
    LVector poleAnchor = { collBboxMax.y, collBboxMax.z, field304 };

    // PSX: pendulum torque: (-660 * sin(angleX)) >> 16, then * 4000 * stateCounter
    s32 sinX = rmSin16(angleX);
    s64 torqueBase = (-660LL * sinX) >> 16;
    s64 torqueScaled = 4000 * (s64)stateCounter * torqueBase;

    // PSX: field424 += (2182 * rmDiv16i(torqueScaled >> 16, 2000)) >> 16
    s32 accel = rmDiv16i((s32)(torqueScaled >> 16), 2000);
    field424 = field424 + (s32)((2182LL * accel) >> 16);

    // PSX: update angleX with angular velocity, wrap to [0, 65535]
    s32 newAngleX = angleX + (s32)((2182LL * field424) >> 16);
    if (newAngleX < 0) {
        newAngleX += PSX_ANGLE_360;
    }
    newAngleX = newAngleX % PSX_ANGLE_360;

    // PSX: build rotation matrix from updated angles
    Mat4 rotMatrix;
    p3dBuildRotMatrixXYZ(newAngleX, angleY, angleZ, rotMatrix);

    // PSX: fill translation from pole anchor position
    p3dFillTransMatrix(poleAnchor, rotMatrix);

    // PSX: transform pendulum arm by matrix to get new position
    Vec3 armVec((f32)pendulumArm.x, (f32)pendulumArm.y, (f32)pendulumArm.z);
    Vec3 newPosVec = p3dVecTimesMatrix(armVec, rotMatrix);
    savedPos.x = (s32)newPosVec.x;
    savedPos.y = (s32)newPosVec.y;
    savedPos.z = (s32)newPosVec.z;

    // PSX: check animation midpoint for swing count
    if (model) {
        Model* m = static_cast<Model*>(model);
        AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
        if (anim) {
            s32 halfFrame = ((anim->endFrame >> 16) + (anim->endFrame >> 31)) >> 1;
            s32 currentFrame = (s16)((u32)anim->currentFrame >> 16);
            if (halfFrame < currentFrame) {
                field616++;
            }

            // PSX: switch animation based on angular velocity direction
            s32 targetAnim = (field424 >= 0) ? PLAYER_ANIM_POLE_SWING_FWD : PLAYER_ANIM_POLE_SWING_BACK;
            if (anim->animEnum != targetAnim) {
                m->SetAnim(targetAnim, 3, 0, 0);
                actionStateFlag = 1;
            }
        }
    }

    // PSX: check if swing passed through bottom (angle range check)
    s32 wantSound = 0;
    if (actionStateFlag) {
        if ((u32)(newAngleX - 5461) > 0xD8E3u) {
            actionStateFlag = 0;
            if (humanoidSound) {
                humanoidSound->PoleSwing();
            }
            wantSound = 0;
        }
    }

    // PSX: update orientation
    orientation.x = newAngleX;
    orientation.y = angleY;
    orientation.z = angleZ;

    // PSX: update pos and homePos from transformed position
    pos = savedPos;
    homePos = savedPos;

    // PSX: check dismount (jump bit 3 or taunt bit 4)
    maxFallDivisor = 0;
    u32 cb = (u32)commandBits;
    s32 wantDismount = 0;
    if (((cb >> 3) & 1) || ((cb >> 4) & 1)) {
        wantDismount = 1;
    }
    if (wantDismount) {
        wantSound = 1;
    }

    if (wantSound && field616 > 0) {
        // PSX: dismount from pole - matrix math for launch direction
        LVector launchOffset = { 0, 0, 0 };
        p3dFillTransMatrix(launchOffset, rotMatrix);

        SetActionState(AS_FLIP, 0);
        if (model) {
            Model* m = static_cast<Model*>(model);
            m->SetAnim(PLAYER_ANIM_FORWARD_FLIP, 0, 0, 0);
        }

        // PSX: launch direction depends on swing side
        if (newAngleX <= PSX_ANGLE_180) {
            // PSX: forward side - offset = (350*sin(angleY), 0, 350*cos(angleY))
            savedPos.x += (s32)((350LL * rmSin16(angleY)) >> 16);
            savedPos.z += (s32)((350LL * rmSin16((s16)(angleY + 0x4000))) >> 16);
            homePos = savedPos;
            launchOffset.z = -100;
            faceAngle = angleY + PSX_ANGLE_180;
        }
        else {
            // PSX: backward side - offset = (-350*sin(angleY), 0, -350*cos(angleY))
            savedPos.x += (s32)((-350LL * rmSin16(angleY)) >> 16);
            savedPos.z += (s32)((-350LL * rmSin16((s16)(angleY + 0x4000))) >> 16);
            homePos = savedPos;
            launchOffset.z = 100;
        }

        // PSX: transform launch offset by rotation matrix
        launchOffset.y = 0;
        Vec3 launchVec((f32)launchOffset.x, (f32)launchOffset.y, (f32)launchOffset.z);
        Vec3 launchWorld = p3dVecTimesMatrix(launchVec, rotMatrix);

        // PSX: abs(launch.y)
        s32 launchY = (s32)launchWorld.y;
        if (launchY < 0) {
            launchY = -launchY;
        }

        // PSX: set velocity from launch direction
        velocity.x = (s32)launchWorld.x;
        velocity.y = launchY;
        velocity.z = (s32)launchWorld.z;

        // PSX: set facing and reset swing state
        orientation.x = 0;
        orientation.y = faceAngle;
        orientation.z = angleZ;
        field424 = 0;
    }
}
// PSX: _LedgeLatch__6Player (PLAYER.CPP:4099, 0x8003352C)
// Holds player on a ledge. Zeros velocity/force, waits 4 frames,
// then checks for dismount (jump/taunt) or pull-up.
void Player::_LedgeLatch() {
    MARKFUNCTION(0x8003352C);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    // PSX: zero velocity and contactForce
    velocity = {};
    contactForce = {};

    // PSX: check animEnum == 31 (LEDGE_LATCH)
    if (anim->animEnum != PLAYER_ANIM_LEDGE_LATCH) {
        return;
    }

    // PSX: increment field616, check pre-increment value >= 4 (wait 5 calls)
    field616++;
    if (field616 <= 4) {
        return;
    }

    u32 cb368 = (u32)field368;

    // PSX: check field368 bit 3 - ledge grab confirmation from collision
    if ((cb368 >> 3) & 1) {
        // Move position back from ledge and drop
        // PSX: offset position by -300 * sin(orientation.y) in X/Z
        s32 sinY = rmSin16(orientation.y);
        s32 cosY = rmSin16((s16)(orientation.y + 0x4000));
        homePos.x += (s32)((-300LL * sinY) >> 16);
        homePos.y -= 850;
        homePos.z += (s32)((-300LL * cosY) >> 16);
        gravity = 0;
        return;
    }

    // PSX: check commandBits for directional input
    u32 cb = (u32)commandBits;
    s32 hasInput = 0;
    if (((cb >> 2) & 1) || ((cb >> 3) & 1) || ((cb >> 4) & 1)) {
        hasInput = 1;
    }

    if (hasInput) {
        // Check if input direction is facing the ledge (angle within threshold)
        s32 diff = orientation.y - faceAngle;
        if (diff > PSX_ANGLE_180) {
            diff -= PSX_ANGLE_360;
        }
        if (diff < -PSX_ANGLE_180) {
            diff += PSX_ANGLE_360;
        }
        s32 absDiff = (diff >= 0) ? diff : -diff;

        // PSX: threshold 9103 - if facing ledge, pull up
        s32 shouldPullUp = 0;
        if ((absDiff < 9103 || ((cb >> 3) & 1)) && !((u8)(field368 >> 7))) {
            shouldPullUp = 1;
        }

        if (shouldPullUp) {
            if (m->drawable && ((m->drawable->displayFlag & 1) != 0)) {
                m->Animate();
            }
            // PSX: transition to ledge pull-up
            SetActionState(AS_LEDGE_PULLUP, 0);
        }
        else {
            // PSX: LetGoOfLedge__8Humanoid - drop from ledge
            LetGoOfLedge();
        }
    }
}

// PSX: _LedgePullup__6Player (PLAYER.CPP:4194, 0x800337A8)
// Zeros velocity/force, waits for animation completion,
// then restores position with clamped Y.
void Player::_LedgePullup() {
    MARKFUNCTION(0x800337A8);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);

    // PSX: zero velocity and contactForce
    velocity = {};
    contactForce = {};

    if (!anim) {
        return;
    }

    // PSX: check loopCount > 0 (animation completed)
    if (anim->loopCount > 0) {
        _DoStand();

        if (collBboxMin.y < 0) {
            collBboxMin.y = 0;
        }
    }
}

// PSX: _Dead__6Player (PLAYER.CPP:4256, 0x80033858)
// Sets death flag and starts the standard Director death script once.
void Player::_Dead() {
    MARKFUNCTION(0x80033858);

    if (!field620) {
        field620 = 1;
        if (g_director) {
            g_director->SetCodeSnip(Director::GetDeathScript(), nullptr);
        }
    }
}

// PSX: _SlopeSlide__6Player (PLAYER.CPP:4274, 0x8003389C)
// Handles sliding on slopes. Determines slide direction from collision
// normal, projects velocity along slope surface, faces slide direction.
void Player::_SlopeSlide() {
    MARKFUNCTION(0x8003389C);

    u32 cb = (u32)commandBits;
    field616++;

    // PSX: check jump (bit 3) or taunt (bit 4) for dismount
    s32 wantDismount = 0;
    if (((cb >> 3) & 1) || ((cb >> 4) & 1)) {
        wantDismount = 1;
    }
    if (wantDismount && (u32)field616 >= 8) {
        SetActionState(AS_PAUSE, 0);
        return;
    }

    // PSX: check flags bit 16 (on slope surface)
    u32 fl = (u32)flags;
    if ((fl & 0x10000) == 0) {
        // Not on slope - fall with forward force
        SetActionState(AS_FALL, 0);
        SVector dir;
        dir.x = (s16)(orientation.x & 0xFFFF);
        dir.y = (s16)((orientation.x >> 16) & 0xFFFF);
        dir.z = (s16)(orientation.y & 0xFFFF);
        dir.pad = (s16)((orientation.y >> 16) & 0xFFFF);
        AddForce(5000, &dir);
        return;
    }

    // PSX: check flags bit 17 (slope physics active)
    if (((fl >> 17) & 1) == 0) {
        SetActionState(AS_STAND, 0);
        return;
    }

    // PSX: compute slide direction from collision normal
    // field148[6] = normalX at +172, field148[8] = normalZ at +180
    s32 normalX = field148[6];
    s32 normalZ = field148[8];

    if (normalX != 0 || normalZ != 0) {
        // PSX: determine face angle from dominant slope axis
        s32 slideAngle = 0;
        s32 projX, projZ;

        if (normalX != 0) {
            slideAngle = 0x4000;
            if (normalX > 0) {
                projX = PSX_ANGLE_360;
            }
            else {
                slideAngle = 0xC000;
                projX = -PSX_ANGLE_360;
            }
            projZ = 0;
        }
        else {
            projX = 0;
            if (normalZ > 0) {
                projZ = PSX_ANGLE_360;
            }
            else {
                slideAngle = PSX_ANGLE_180;
                projZ = -PSX_ANGLE_360;
            }
        }

        // PSX: compute velocity from facing direction
        s32 sinA = rmSin16(faceAngle);
        s32 cosA = rmSin16((s16)(faceAngle + 0x4000));
        s32 velX = (s32)(((s64)sinA * (u32)runSpeed) >> 16);
        s32 velZ = (s32)(((s64)cosA * (u32)runSpeed) >> 16);

        // PSX: project out slope-normal component
        // dot = projX * velX + projZ * velZ (in fixed-point)
        s32 dot = (s32)(((s64)projX * (s64)velX) >> 16) + (s32)(((s64)projZ * (s64)velZ) >> 16);

        // PSX: subtract normal component from velocity
        contactForce.x += velX - (s32)(((s64)dot * (s64)projX) >> 16);
        contactForce.y = contactForce.y; // PSX: no change to Y
        contactForce.z += velZ - (s32)(((s64)dot * (s64)projZ) >> 16);

        FaceAngleY(slideAngle, 0);
        SetDesiredMoveDirection(slideAngle);
    }
}
// PSX: _Straif__6Player (PLAYER.CPP:4606, 0x80033FF8)
// Player override of Humanoid::_Straif. Finds target if none,
// checks flag transitions, scales moveSpeed by strafe multiplier.
void Player::_Straif() {
    MARKFUNCTION(0x80033FF8);

    // PSX: if no target (field256 == 0), find one via FindFoe + SetHumanoidTarget
    if (field256 == 0) {
        Humanoid* foe = FindFoe(PLAYER_STRAFE_TARGET_RANGE, PLAYER_STRAFE_TARGET_HALF_ANGLE, 0);
        SetHumanoidTarget(foe);
    }

    // PSX: check flags bit 17 (slope/special surface)
    if ((flags >> 17) & 1) {
        SetActionState(AS_SLOPE_SLIDE, 0);
        return;
    }

    u32 cb = (u32)commandBits;

    // PSX: bit 4 (taunt) -> release target, pause
    if ((cb >> 4) & 1) {
        ReleaseTarget();
        SetActionState(AS_PAUSE, 0);
        return;
    }

    // PSX: bit 3 (jump) -> release target, jump
    if ((cb >> 3) & 1) {
        ReleaseTarget();
        SetActionState(AS_JUMP, 0);
        return;
    }

    // PSX: a1[52] = (gp+532 * a1[52]) >> 16
    moveSpeed = static_cast<s32>((static_cast<s64>(STRAFE_MOVE_SPEED) * static_cast<u32>(moveSpeed)) >> 16);

    // Delegate to base Humanoid strafe logic
    Humanoid::_Straif();
}

// PSX: _LadderDismount__6Player (PLAYER.CPP:4477, 0x80033DB8)
// PSX: return LadderDismount__8Humanoid(a1) - direct delegate
void Player::_LadderDismount() {
    MARKFUNCTION(0x80033DB8);
    Humanoid::_LadderDismount();
}

// PSX: _ClimbLadder__6Player (PLAYER.CPP:4488, 0x80033DD8)
// PSX: return ClimbLadder__8Humanoid(a1) - direct delegate
void Player::_ClimbLadder() {
    MARKFUNCTION(0x80033DD8);
    Humanoid::_ClimbLadder();
}

// PSX: _TableRoll__6Player (PLAYER.CPP:4509, 0x80033DF8)
// Handles rolling across a table surface. Applies small forward force
// during early frames, checks for end of surface, transitions to stand.
void Player::_TableRoll() {
    MARKFUNCTION(0x80033DF8);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    s16 frame = (s16)((u32)anim->currentFrame >> 16);

    if (anim->animEnum == PLAYER_ANIM_TABLE_ROLL) {
        if (frame < 5) {
            SVector dir = {};
            dir.x = (s16)(orientation.x & 0xFFFF);
            dir.z = (s16)(orientation.y & 0xFFFF);
            dir.pad = (s16)(orientation.z & 0xFFFF);
            AddForce(20, &dir);
            maxFallDivisor = 0;
        }

        if (TABLE_ROLL_HANGTIME_START < frame && frame < TABLE_ROLL_HANGTIME_END) {
            maxFallDivisor = 0;
        }

        if (frame == 13) {
            LVector testPos = pos;
            testPos.x += (s32)(((s64)rmSin16(faceAngle) * TABLE_LOOK_AHEAD_DISTANCE) >> 16);
            testPos.z += (s32)(((s64)rmSin16((s16)(faceAngle + 0x4000)) * TABLE_LOOK_AHEAD_DISTANCE) >> 16);

            s32 floorHeight = g_collisionSectors[0].GetWorldFloorHeight(testPos, 10);
            if (floorHeight != (s32)0x80000001) {
                s32 floorDiff = testPos.y - floorHeight;
                if (floorDiff < 0) {
                    floorDiff = -floorDiff;
                }
                if (floorDiff < 151) {
                    m->SetAnim(PLAYER_ANIM_TABLE_ROLL_END, 0, 0, 0);
                }
            }
        }
    }
    else {
        maxFallDivisor = 0;
    }

    if (anim->loopCount != 0) {
        flags2 &= ~0x70u;
        SetActionState(AS_STAND, 0);
        RestorePositionFromBip01();
    }
}

void Player::Debug_ApplyForcedAnimation() {
    if (!debugAnimOverrideActive || !model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    if (!anim) {
        return;
    }

    if (debugAnimOverridePaused) {
        anim->speed = 0;
        return;
    }

    if (anim->animEnum != debugAnimOverrideEnum ||
        anim->loopTypeField != debugAnimOverrideLoopType) {
        debugAnimOverrideApplying = true;
        m->ApplyAnimToModel((s32)debugModelCharType, debugAnimOverrideEnum, debugAnimOverrideLoopType, 0, 0);
        debugAnimOverrideApplying = false;

        anim = static_cast<AnimStructure*>(m->animStructure);
        if (!anim) {
            return;
        }
    }

    if (anim->speed == 0) {
        anim->speed = FIX16_ONE;
    }
}

bool Player::Debug_PlayAnimation(s32 animEnum, s32 loopType) {
    if (!model || animEnum < 0) {
        return false;
    }
    if (loopType < ANIM_LOOP || loopType > ANIM_STOP) {
        loopType = ANIM_LOOP;
    }
    if (!EnsurePlayerAnimationLoaded(animEnum, debugModelCharType)) {
        return false;
    }

    Model* m = static_cast<Model*>(model);

    debugAnimOverrideEnum = animEnum;
    debugAnimOverrideLoopType = loopType;
    debugAnimOverridePaused = false;
    debugAnimOverrideActive = true;

    debugAnimOverrideApplying = true;

    m->ApplyAnimToModel((s32)debugModelCharType, animEnum, loopType, 0, 0);
    debugAnimOverrideApplying = false;

    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (anim) {
        anim->speed = FIX16_ONE;
    }
    return true;
}

void Player::Debug_PauseAnimation() {
    if (!debugAnimOverrideActive || !model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    anim->speed = 0;
    debugAnimOverridePaused = true;
}

void Player::Debug_ResumeAnimation() {
    if (!debugAnimOverrideActive || !model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    anim->speed = FIX16_ONE;
    debugAnimOverridePaused = false;
}

void Player::Debug_StopAnimation() {
    debugAnimOverrideActive = false;
    debugAnimOverridePaused = false;
    debugAnimOverrideApplying = false;
    debugAnimOverrideEnum = -1;
    debugAnimOverrideLoopType = ANIM_LOOP;

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (anim && anim->speed == 0) {
        anim->speed = FIX16_ONE;
    }
}

bool Player::Debug_IsAnimationOverrideActive() const {
    return debugAnimOverrideActive;
}

bool Player::Debug_IsAnimationOverrideApplying() const {
    return debugAnimOverrideApplying;
}

bool Player::Debug_IsAnimationPaused() const {
    return debugAnimOverrideActive && debugAnimOverridePaused;
}

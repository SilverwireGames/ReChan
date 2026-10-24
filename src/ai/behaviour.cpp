#include "ai/behaviour.h"
#include "ai/activezn.h"
#include "ai/colfight.h"
#include "ai/cominter.h"
#include "ai/humanoid.h"
#include "ai/obstacle.h"
#include "ai/pickup.h"
#include "ai/player.h"
#include "ai/thing.h"
#include "gen/ai.h"
#include "gen/colsect.h"
#include "gen/fxp.h"
#include "gen/director.h"
#include "gen/game.h"
#include "gen/camera.h"
#include "gen/animstruct.h"
#include "gen/model.h"
#include "gen/path.h"
#include "gen/weffect.h"
#include "gen/psxmath_helpers.h"
#include "pc/debugui.h"
#include "extra/cheats.h"
#include "p3d/p3dmath.h"
#include "pc/log.h"

// PSX binary angle constants
static constexpr s32 ANGLE_FULL_ROTATION = 0xFFFF;
static constexpr s32 ANGLE_QUARTER_TURN = 0x4000;

// Direction classification thresholds (PSX angle ranges)
// angleDiff = ownerAngle - targetAngle, wrapped to [0, 0xFFFF]
// Behind:  angleDiff in [25487..40049)  ~140-220 degrees (input opposite to facing)
// Left:    angleDiff in [8193..24576)   ~45-135 degrees (input to character's left)
// Right:   angleDiff in [40961..57344)  ~225-315 degrees (input to character's right)
// Forward: angleDiff outside all ranges, near 0 degrees (input aligned with facing)
static constexpr u32 DIR_BEHIND_START = 25487;
static constexpr u32 DIR_BEHIND_RANGE = 0x38E2;
static constexpr u32 DIR_LEFT_START = 8193;
static constexpr u32 DIR_LEFT_RANGE = 0x3FFF;
static constexpr u32 DIR_RIGHT_START = 40961;
static constexpr u32 DIR_RIGHT_RANGE = 0x3FFF;

// Direction codes (bitmask values matching PSX commandList conditions)
static constexpr s32 DIR_FORWARD = 1;  // angleDiff near 0 - pushing same direction as facing
static constexpr s32 DIR_BACKWARD = 2;  // angleDiff near 180 - pushing opposite to facing
static constexpr s32 DIR_RIGHT = 4;  // angleDiff near 270 - pushing to character's right
static constexpr s32 DIR_LEFT = 8;  // angleDiff near 90 - pushing to character's left

// Fixed-point 127 << 16
static constexpr s32 STICK_MAX_FP = 0x7F0000;

// PSX gp+0x7FC / gp+0x800 / gp+0x814 threshold table entries (800DD148/4C/60).
static constexpr s32 PATH_BREAKOFF_DIST_THRESHOLD = 0x3E8;
static constexpr s32 PATH_NODE_REACHED_THRESHOLD = 0x12C;
static constexpr s32 REJOIN_ACTIVE_ZONE_PLAYER_DIST_THRESHOLD = 0x311;
static constexpr s32 NDMS_SLOWDOWN_DIST_THRESHOLD = 0x9C4;
static constexpr s32 NDMS_FAR_DIST_THRESHOLD = 0x6D6;
static constexpr s32 NDMS_MID_DIST_THRESHOLD = 0x4B0;

static constexpr s32 NDMS_HEIGHT_DELTA_THRESHOLD = 0x100;
static constexpr s32 NDMS_FLOOR_SAFE_DELTA_THRESHOLD = 0x101;

// PSX BEHAVEB globals (0x800DD5E0+).
static constexpr s32 BUTCH_FAR_DIST = 0x578;
static constexpr s32 BUTCH_ATTACK_DIST = 0x320;
static constexpr s32 GRONTAR_FAR_ATTACK_DIST = 0x708;
static constexpr s32 GRONTAR_CLOSE_ATTACK_DIST = 0x352;
static constexpr s32 WALL_CHECK_DISTANCE = 0xC0;
static constexpr s32 WALL_CHECK_DISTANCE_RADIUS = 0x46;
static constexpr s32 OSCAR_CLOSE_DIST = 0x311;
static constexpr s32 OSCAR_MID_DIST = 0x4B0;
static constexpr s32 OSCAR_ANGLE_WINDOW_START = 0x2000;
static constexpr s32 OSCAR_ANGLE_WINDOW_RANGE = 0xC38E;
static constexpr s32 OSCAR_HENCH_ANGLE_WINDOW_START = 0x1555;
static constexpr s32 OSCAR_HENCH_ANGLE_WINDOW_RANGE = 0xD8E3;
static constexpr s32 OSCAR_HENCH_SYNC_COUNTER_MAX = 15;
static constexpr s32 DANTE_PHASE1_HEALTH_SCALE = 0xB333;
static constexpr s32 DANTE_PHASE1_DIST_THRESHOLD = 0x3E8;
static constexpr s32 DANTE_PHASE2_HEALTH_SCALE = 0x6666;
static constexpr s32 DANTE_PHASE2_CLOSE_DIST = 0x400;
static constexpr s32 DANTE_PHASE3_CLOSE_DIST = 0x3E8;
static constexpr s32 DANTE_PHASE3_MID_DIST = 0x8CA;
static constexpr char DANTE_PHASE2_DESK_SUBZONE_NAME[] = "desk";
static constexpr s32 PAUL_CLOSE_ATTACK_DIST = 0x352;
static constexpr s32 PAUL_DANCE_TIME = 0xC8;
static constexpr s32 PAUL_RECOVER_TIME = 0x64;
static constexpr s32 PAUL_SPOTLIGHT_RADIUS = 0x190;
static constexpr char PAUL_SPOTLIGHT_NAME[] = "BossSpotLight";

// PSX BreakOffPathAndFight field-of-view checks.
static constexpr s32 BREAKOFF_FIELD_LEFT = 0x3555;
static constexpr s32 BREAKOFF_FIELD_RIGHT = 0x3555;

// PSX global at 0x800DD9B8 set by SetAIHandler for Oscar henchmen paths.
static Humanoid* OscarsHenchman = nullptr;
// PSX global at 0x800DD9B4 used by Oscar henchman combat decisions.
static Humanoid* bossOscar = nullptr;
// PSX global at 0x800DD9BC reserved for Paul boss wiring.
static Humanoid* bossPaul = nullptr;
// PSX gp+0xCC8 counter used by OscarHenchmanDMS.
static s32 OscarHenchmanSyncCounter = 0;

static bool IsPlayerAggressiveCombatState(s32 actionState) {
    if ((u32)(actionState - (s32)AS_COLLAPSE_STUN) < 4u) {
        return true;
    }
    return actionState == (s32)AS_SPIN_BACK;
}

static bool IsOscarSyncActionState(s32 actionState) {
    switch (actionState) {
        case AS_COMBAT_IDLE:
        case AS_BACK_GRAB_LATCH:
        case AS_BACK_GRAB:
        case AS_THROW_CHARACTER_RECEIVE:
        case AS_BACK_GRAB_RECEIVE_PRE_LATCH:
        case AS_BACK_GRAB_RECEIVE_LATCH:
        case AS_BACK_GRAB_RECEIVE:
            return true;
        default:
            return false;
    }
}

static bool IsDantePhase1PressureState(s32 actionState) {
    switch (actionState) {
        case AS_PAUSE:
        case AS_JUMP:
        case AS_FALL:
        case AS_FLIP:
        case AS_STATE_33:
        case AS_STATE_35:
            return true;
        default:
            return false;
    }
}

static WEffect* FindBossSpotLight() {
    return WEffect::Find(p3dHash(PAUL_SPOTLIGHT_NAME));
}

static s32 GetFaceAngleDataForwardAngle(const Behaviour* self) {
    return self->animConfigPtr ? self->animConfigPtr->runningSpeed : 0;
}

static s32 GetFaceAngleDataBackAngle(const Behaviour* self) {
    return self->animConfigPtr ? self->animConfigPtr->strafingSpeed : 0;
}

static bool ShouldAdvanceDantePhase(const Humanoid* humanoid, s32 scale) {
    const s64 scaledMaxHealth = ((s64)(u32)humanoid->maxHealth * (s64)scale) >> 16;
    return scaledMaxHealth >= (s64)(u32)humanoid->health;
}

// PSX: wallCheck(Humanoid*, long) (BEHAVEB.CPP:1274, 0x8001E05C)
static s32 wallCheck(Humanoid* humanoid, s32 angle) {
    MARKFUNCTION(0x8001E05C);

    if (!humanoid) {
        return 0;
    }

    s32 collisionRatio = 0;
    LVector wallNormal = {};
    LVector hitPoint = {};
    s32 verticalSpan = 0;
    s32 wallMaterial = 0;

    return humanoid->CheckWallCollision(
        (s16)angle,
        WALL_CHECK_DISTANCE,
        WALL_CHECK_DISTANCE_RADIUS,
        500,
        collisionRatio,
        wallNormal,
        hitPoint,
        verticalSpan,
        wallMaterial);
}

static void RestoreDeferredHandlerOrNDMS(Behaviour* self) {
    if (!self) {
        return;
    }

    if (self->nextHandlerDispatch != 0) {
        self->handlerThisOffset = self->nextHandlerThisOffset;
        self->handlerDispatch = self->nextHandlerDispatch;
        self->handler = self->nextHandler;
    }
    else {
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = Behaviour::NDMS;
    }

    self->nextHandlerThisOffset = 0;
    self->nextHandlerDispatch = 0;
    self->nextHandler = nullptr;
}

// PSX: AiFollowPath__9Behaviour fallback (BEHAVE.CPP:1261..1285, 0x80074CCC..0x80074EB4)
static void RestoreDeferredHandlerOrAiFollowPathFallback(Behaviour* self) {
    if (!self) {
        return;
    }

    if (self->nextHandlerDispatch != 0) {
        self->handlerThisOffset = self->nextHandlerThisOffset;
        self->handlerDispatch = self->nextHandlerDispatch;
        self->handler = self->nextHandler;
    }
    else {
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;

        switch (self->aiType) {
            case AITypes::TT_BUTCH:
                self->handler = Behaviour::ButchDMS;
                break;
            case AITypes::TT_GRONTAR:
                self->handler = Behaviour::GrontarDMS;
                break;
            case AITypes::TT_DANTE:
                self->handler = Behaviour::DanteDMS_Phase1;
                break;
            default:
                self->handler = Behaviour::NDMS;
                break;
        }
    }

    self->nextHandlerThisOffset = 0;
    self->nextHandlerDispatch = 0;
    self->nextHandler = nullptr;
}

// PSX: Jumping__9Behaviour fallback (BEHAVE.CPP:4076..4099, 0x80077540..0x8007760C)
static void RestoreDeferredHandlerOrJumpingFallback(Behaviour* self) {
    if (!self) {
        return;
    }

    if (self->nextHandlerDispatch != 0) {
        self->handlerThisOffset = self->nextHandlerThisOffset;
        self->handlerDispatch = self->nextHandlerDispatch;
        self->handler = self->nextHandler;
    }
    else {
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;

        switch (self->aiType) {
            case AITypes::TT_BUTCH:
                self->handler = Behaviour::ButchDMS;
                break;
            case AITypes::TT_GRONTAR:
                self->handler = Behaviour::GrontarDMS;
                break;
            case AITypes::TT_PAUL:
                self->handler = Behaviour::PaulDMS;
                break;
            case AITypes::TT_OSCAR:
                self->handler = Behaviour::OscarDMS;
                break;
            case AITypes::TT_DANTE:
                self->handler = Behaviour::DanteDMS_Phase1;
                break;
            default:
                self->handler = Behaviour::NDMS;
                break;
        }
    }

    self->nextHandlerThisOffset = 0;
    self->nextHandlerDispatch = 0;
    self->nextHandler = nullptr;
}

Behaviour::Behaviour(Humanoid* ownerHumanoid, u32 handlerType, s32 inAiParam) {
    MARKFUNCTION(0x8007458C);
    owner = ownerHumanoid;
    aiType = handlerType;
    currentPath = nullptr;
    field36 = 0;
    currentPathNodeIndex = 0;
    ndmsThinkCounter = 0;
    ndmsThinkDelay = 45;
    ndmsDecision = 5;
    aiParam = inAiParam;
    field60 = 0;
    navDecisionCounter = 50;
    navDecision = 2;
    comboScriptCursor = 0;
    comboScriptIndex = -1;
    worldNavCached = 0;
    worldNavCachedTicks = 0;
    worldNavBlocked = 0;
    worldNavDirection = 0;
    handlerThisOffset = 0;
    handlerDispatch = -1;
    handler = nullptr;
    ndmsRangeBand = 0;
    previousButtons = 0;
    buttonHoldCounter = 0;
    behaviourFlags = 0;
    padPort = 1;
    field276 = 0;
    SetAIHandler(handlerType);
}

void Behaviour::SetAIHandler(u32 handlerType) {
    MARKFUNCTION(0x800746DC);

    nextHandlerThisOffset = 0;
    nextHandlerDispatch = 0;
    nextHandler = nullptr;

    // PC: behave.txt may not exist yet when AI opens.
    if (g_ai && !g_ai->behaviourList.head) {
        g_ai->ParseBehaviourAttribScript();
    }

    const u32 behaviourHash = owner ? owner->behaviourNameHash : 0;
    if (g_ai && behaviourHash != 0) {
        animConfigPtr = static_cast<BehaviourAttrib*>(g_ai->behaviourList.FindNodeCRC(behaviourHash));
    }
    else {
        animConfigPtr = nullptr;
    }

    if (!animConfigPtr && g_ai) {
        animConfigPtr = static_cast<BehaviourAttrib*>(g_ai->behaviourList.FindNode("Default"));
    }

    handlerThisOffset = 0;
    handlerDispatch = -1;
    handler = Idle;

    if (owner && behaviourHash == p3dHash("OscarHenchmen")) {
        handlerThisOffset = 0;
        handlerDispatch = -1;
        handler = OscarHenchmanDMS;
        OscarsHenchman = owner;
        return;
    }

    switch (handlerType) {
        case AITypes::TT_PLAYER:
            padPort = 0;
            handler = PlayerUserControl;
            break;
        case AITypes::TT_GRONTAR:
            handler = GrontarDMS;
            break;
        case AITypes::TT_DANTE:
            handler = DanteDMS_Phase1;
            break;
        case AITypes::TT_OSCAR:
            bossOscar = owner;
            handler = OscarDMS;
            break;
        case AITypes::TT_BUTCH:
            handler = ButchDMS;
            break;
        case AITypes::TT_PAUL:
            bossPaul = owner;
            handler = PaulDMS;
            break;
        default:
            break;
    }
}

// PSX: CounterAttack__9Behaviour (BEHAVEB.CPP:1965, 0x8001EDB4)
s32 Behaviour::CounterAttack() {
    MARKFUNCTION(0x8001EDB4);

    Player* player = Player::s_player;
    s32 currentCounterTarget = player ? player->field484 : 0;
    s32 shouldCounter = 0;

    s32& lastCounterTarget = field80To176[11];
    s32& counterSlotCursor = field80To176[22];
    s32* counterTargets = &field80To176[12];
    s32* counterCounts = &field80To176[17];

    if (currentCounterTarget != 0 && player && (u32)(player->actionState - 0x20) < 5u) {
        if (lastCounterTarget != currentCounterTarget) {
            if (counterSlotCursor >= 5) {
                counterSlotCursor = 0;
            }

            const s32 slot = counterSlotCursor;
            if (counterTargets[slot] == currentCounterTarget) {
                counterCounts[slot] = counterCounts[slot] + 1;
            }
            else {
                counterTargets[slot] = currentCounterTarget;
                counterCounts[slot] = 0;

                for (s32 index = slot + 1; index < 5; index++) {
                    counterTargets[index] = 0;
                    counterCounts[index] = 0;
                }
            }

            counterSlotCursor = slot + 1;

            if (counterCounts[2] > 0 || counterCounts[1] >= 2 || counterCounts[0] >= 3) {
                shouldCounter = 1;
            }

            counterSlotCursor = 0;
        }
    }
    else {
        counterSlotCursor = 0;
    }

    lastCounterTarget = currentCounterTarget;
    return shouldCounter;
}

// PSX: _ButchDMS__9Behaviour (BEHAVEB.CPP:284, 0x8001CB20)
void Behaviour::ButchDMS(Behaviour* self) {
    MARKFUNCTION(0x8001CB20);

    if (!self || !self->owner) {
        return;
    }

    if (g_director && g_director->scriptState != 0) {
        return;
    }

    self->InActiveZone();

    Player* player = Player::s_player;
    if (!player) {
        return;
    }

    Humanoid* owner = self->owner;
    owner->SetTarget(player);

    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);
    if ((u32)(owner->actionState - 0x20) >= 5u) {
        self->comboScriptIndex = -1;
        self->comboScriptCursor = 0;
    }

    ActiveZone* ownerZone = owner->activeZone;
    self->currentPath = nullptr;
    if (ownerZone && self->BreakOffPathAndFight() == 0) {
        self->currentPath = ownerZone->FindFirstValidPath(owner);
    }

    if (self->currentPath) {
        self->InitPathAIState(self->currentPath);
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = ButchDMS;
        return;
    }

    const LVector ownerPos = owner->pos;
    const s32 ownerFacing = owner->orientation.y;

    if (g_ai->GetPickupWithinReach(owner) != nullptr) {
        owner->RequestAction(7);
        return;
    }

    if (IsPlayerAggressiveCombatState(player->actionState)) {
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = ButchDMS;

        self->navDecisionCounter = 0;
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = BackoffAndTaunt;
        return;
    }

    owner->FaceThingDesired(player);
    if (distanceToPlayer < BUTCH_ATTACK_DIST) {
        self->navDecisionCounter = 0;
        self->ComplexAttack();
        return;
    }

    self->navDecisionCounter++;
    if (self->navDecisionCounter >= 26) {
        self->navDecisionCounter = 0;

        if (BUTCH_FAR_DIST < distanceToPlayer) {
            const s32 randomAction = (s32)rmRangedRandom(100);
            if (randomAction < 30) {
                owner->RequestAction(30);
                return;
            }

            if ((u32)(randomAction - 30) < 30u) {
                s32 canCharge = 0;
                if (IsPointInFieldOf(player->pos, ownerPos, ownerFacing, 0xAAA, 0xAAA) != 0) {
                    s32 collisionRatio = 0;
                    LVector wallNormal = {};
                    LVector hitPoint = {};
                    s32 verticalSpan = 0;
                    s32 wallMaterial = 0;
                    const s32 wallHit = owner->CheckWallCollision(
                        (s16)ownerFacing,
                        distanceToPlayer,
                        0xFA,
                        0x100,
                        collisionRatio,
                        wallNormal,
                        hitPoint,
                        verticalSpan,
                        wallMaterial);
                    canCharge = (wallHit == 0) ? 1 : 0;
                }

                if (canCharge != 0) {
                    self->navDecisionCounter = 0;
                    self->handlerThisOffset = 0;
                    self->handlerDispatch = -1;
                    self->handler = ButchDMS_Charge;
                    return;
                }
            }
        }

        const s32 randomDecision = (s32)rmRangedRandom(100);
        if (randomDecision < 60) {
            const s32 randomDirection = (s32)rmRangedRandom(100);

            if (randomDirection < 15) {
                self->navDecision = 3;

                s32 collisionRatio = 0;
                LVector wallNormal = {};
                LVector hitPoint = {};
                s32 verticalSpan = 0;
                s32 wallMaterial = 0;
                if (owner->CheckWallCollision(
                    (s16)(ownerFacing + 0x4000),
                    0x100,
                    0xFA,
                    0x100,
                    collisionRatio,
                    wallNormal,
                    hitPoint,
                    verticalSpan,
                    wallMaterial) != 0) {
                    self->navDecision = 2;
                }
            }
            else if ((u32)(randomDirection - 15) < 15u) {
                self->navDecision = 2;

                s32 collisionRatio = 0;
                LVector wallNormal = {};
                LVector hitPoint = {};
                s32 verticalSpan = 0;
                s32 wallMaterial = 0;
                if (owner->CheckWallCollision(
                    (s16)(ownerFacing - 0x4000),
                    0x100,
                    0xFA,
                    0x100,
                    collisionRatio,
                    wallNormal,
                    hitPoint,
                    verticalSpan,
                    wallMaterial) != 0) {
                    self->navDecision = 3;
                }
            }
            else if ((u32)(randomDirection - 30) < 15u) {
                self->navDecision = 1;

                s32 collisionRatio = 0;
                LVector wallNormal = {};
                LVector hitPoint = {};
                s32 verticalSpan = 0;
                s32 wallMaterial = 0;
                if (owner->CheckWallCollision(
                    (s16)ownerFacing,
                    0x100,
                    0xFA,
                    0x100,
                    collisionRatio,
                    wallNormal,
                    hitPoint,
                    verticalSpan,
                    wallMaterial) != 0) {
                    if (wallMaterial == 0x180) {
                        owner->moveSpeed = (s16)self->animConfigPtr->runningSpeed;
                        owner->RequestAction(3);

                        self->nextHandlerThisOffset = 0;
                        self->nextHandlerDispatch = -1;
                        self->nextHandler = ButchDMS;

                        self->handlerThisOffset = 0;
                        self->handlerDispatch = -1;
                        self->handler = Jumping;
                        return;
                    }

                    self->navDecision = ((s32)rmRangedRandom(2) != 0) ? 2 : 3;
                }
            }
            else {
                self->navDecision = 4;
            }
        }
        else {
            self->navDecision = ((u32)(randomDecision - 60) < 20u) ? 5 : 6;
        }
    }

    const s32 navDecision = self->navDecision;
    if ((u32)navDecision >= 8u) {
        return;
    }

    switch (navDecision) {
        case 1:
            owner->moveSpeed = (s16)self->animConfigPtr->strafingSpeed;
            owner->RequestAction(6);
            return;

        case 2:
            owner->SetDesiredMoveDirection(owner->faceAngle - 0x4000);
            owner->moveSpeed = (s16)self->animConfigPtr->strafingSpeed;
            owner->RequestAction(6);
            return;

        case 3:
            owner->SetDesiredMoveDirection(owner->faceAngle + 0x4000);
            owner->moveSpeed = (s16)self->animConfigPtr->strafingSpeed;
            owner->RequestAction(6);
            return;

        case 4:
            owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
            owner->moveSpeed = (s16)self->animConfigPtr->strafingSpeed;
            owner->RequestAction(6);
            return;

        case 5:
            owner->RequestAction(1);
            return;

        case 6:
        {
            const s32 randomTaunt = (s32)rmRangedRandom(4);
            if (randomTaunt == 0) {
                owner->field316 = 0x5C;
            }
            else if (randomTaunt == 1) {
                owner->field316 = 0x5B;
            }
            else {
                owner->field316 = 0x5A;
            }

            owner->RequestAction(0x15);
            return;
        }

        default:
            return;
    }
}

// PSX: _ButchDMS_Charge__9Behaviour (BEHAVEB.CPP:630, 0x8001D178)
void Behaviour::ButchDMS_Charge(Behaviour* self) {
    MARKFUNCTION(0x8001D178);

    if (!self || !self->owner) {
        return;
    }

    Player* player = Player::s_player;
    if (!player) {
        return;
    }

    Humanoid* owner = self->owner;
    const LVector ownerPos = owner->pos;
    const LVector playerPos = player->pos;
    const s32 ownerFacing = owner->orientation.y;

    const s32 distanceToPlayer = owner->DistanceFromPoint(playerPos);
    owner->SetTarget(player);

    self->navDecisionCounter++;
    if (self->navDecisionCounter >= 51) {
        if (IsPointInFieldOf(playerPos, ownerPos, ownerFacing, 0xAAA, 0xAAA) == 0) {
            self->navDecisionCounter = 0;
            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = ButchDMS;
            return;
        }
    }

    s32 collisionRatio = 0;
    LVector wallNormal = {};
    LVector hitPoint = {};
    s32 verticalSpan = 0;
    s32 wallMaterial = 0;

    if (owner->CheckWallCollision(
        (s16)ownerFacing,
        0x100,
        0xFA,
        0x100,
        collisionRatio,
        wallNormal,
        hitPoint,
        verticalSpan,
        wallMaterial) != 0) {
        self->navDecisionCounter = 0;
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = ButchDMS;
        return;
    }

    if (distanceToPlayer < BUTCH_ATTACK_DIST) {
        owner->RequestAction(7);
        self->navDecisionCounter = 0;
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = ButchDMS;
        return;
    }

    const s32 runSpeed = (s16)self->animConfigPtr->runningSpeed;
    owner->moveSpeed = runSpeed << 1;
    owner->RequestAction(31);
}

// PSX: _GrontarDMS__9Behaviour (BEHAVEB.CPP:717, 0x8001D314)
void Behaviour::GrontarDMS(Behaviour* self) {
    MARKFUNCTION(0x8001D314);

    if (!self || !self->owner)
        return;

    Humanoid* owner = self->owner;
    Player* player = Player::s_player;
    if (!player) {
        return;
    }

    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);
    if ((u32)(owner->actionState - 0x20) >= 5u) {
        self->comboScriptIndex = -1;
        self->comboScriptCursor = 0;
    }

    owner->FaceThingDesired(player);

    s32& closeCombatState = self->field80To176[0];
    s32& comboStateA = self->field80To176[1];
    s32& comboStateB = self->field80To176[2];

    auto resetComboState = [&]() {
        closeCombatState = 0;
        comboStateA = 0;
        comboStateB = 0;
    };

    if (!self->InActiveZone()) {
        if (distanceToPlayer < GRONTAR_CLOSE_ATTACK_DIST
            && !IsPlayerAggressiveCombatState(player->actionState)) {
            const s32 playerInField = IsPointInFieldOf(player->pos, owner->pos, owner->orientation.y, 0x2AAA, 0x2AAA);
            const s32 randomAction = (s32)rmRangedRandom(100);
            if (playerInField != 0 && randomAction < 75) {
                owner->RequestAction(15);
            }
            else {
                self->ComplexAttack();
            }

            resetComboState();
            return;
        }

        LVector zoneCenter = {};
        owner->activeZone->GetActiveZoneCenterPoint(zoneCenter);
        // _Straif captures faceAngle as the movement heading, then turns
        // toward field256 while strafe-moving. Keep Jackie as that target and
        // use the zone center only for the captured movement heading.
        owner->SetTarget(player);
        owner->FacePointDesired(zoneCenter);
        owner->moveSpeed = (s16)self->animConfigPtr->strafingSpeed;
        owner->RequestAction(6);

        resetComboState();
        return;
    }

    if (IsPlayerAggressiveCombatState(player->actionState)) {
        owner->field316 = 0x5A;
        owner->RequestAction(0x15);
        return;
    }

    if (distanceToPlayer < GRONTAR_CLOSE_ATTACK_DIST) {
        const s32 playerInField = IsPointInFieldOf(player->pos, owner->pos, owner->orientation.y, 0x2AAA, 0x2AAA);
        const s32 randomAction = (s32)rmRangedRandom(100);
        if (playerInField != 0 && randomAction < 75) {
            owner->RequestAction(15);
        }
        else {
            self->ComplexAttack();
        }

        resetComboState();
        return;
    }

    if (distanceToPlayer < GRONTAR_FAR_ATTACK_DIST) {
        if (player->actionState == (s32)AS_DIVE_ROLL) {
            owner->RequestAction(7);
            resetComboState();
            return;
        }

        if (closeCombatState != 0) {
            if ((s32)rmRangedRandom(100) < 50) {
                owner->RequestAction(8);
                comboStateB = 1;
            }
            else {
                owner->RequestAction(9);
                comboStateA = 1;
            }

            closeCombatState = 0;
            return;
        }

        if (comboStateB != 0) {
            if (owner->field484 == 0) {
                owner->RequestAction(8);
                comboStateB = 0;
                comboStateA = 1;
            }
            return;
        }

        if (comboStateA != 0) {
            if (owner->field484 == 0) {
                owner->RequestAction(9);
                comboStateA = 0;
                comboStateB = 1;
            }
            return;
        }

        if ((s32)rmRangedRandom(100) < 2) {
            owner->field316 = 0x5A;
            owner->RequestAction(0x15);
            return;
        }

        owner->RequestAction(5);
        closeCombatState = 1;
        return;
    }

    if (player->actionState == (s32)AS_STAND) {
        if (owner->actionState != (s32)AS_TAUNT_ENTRY && (s32)rmRangedRandom(100) >= 99) {
            owner->field316 = 0x5A;
            owner->RequestAction(0x15);
            return;
        }

        owner->RequestAction(1);
        return;
    }

    if (player->actionState == (s32)AS_STRAFE || player->actionState == (s32)AS_RUN) {
        // PSX IsInMyDesiredFieldOf(player, owner, ...): check Grontar against
        // Jackie's desired facing, then leave the current action unchanged
        // when the 20% roll misses.
        if (IsPointInFieldOf(owner->pos, player->pos, player->faceAngle, 0x1555, 0x1555)) {
            if ((s32)rmRangedRandom(100) >= 20) {
                return;
            }

            owner->RequestAction(14);
            return;
        }
    }

    owner->RequestAction(1);
}

// PSX: _PaulDMS__9Behaviour (BEHAVEB.CPP:950, 0x8001D7CC)
void Behaviour::PaulDMS(Behaviour* self) {
    MARKFUNCTION(0x8001D7CC);

    if (!self || !self->owner) {
        return;
    }

    Humanoid* owner = self->owner;
    Humanoid* player = Player::s_player;
    if (!player) {
        return;
    }

    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);

    ActiveZone* ownerZone = owner->activeZone;
    if (!self->InActiveZone() || !ownerZone || !ownerZone->IsInActiveZone(player)) {
        return;
    }

    owner->SetTarget(player);

    if ((u32)(owner->actionState - 0x20) >= 5u) {
        self->comboScriptIndex = -1;
        self->comboScriptCursor = 0;
    }

    if (self->BreakOffPathAndFight() == 0) {
        self->currentPath = ownerZone->FindFirstValidPath(owner);
    }

    if (self->currentPath) {
        self->InitPathAIState(self->currentPath);
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = PaulDMS;
        return;
    }

    owner->FaceThingDesired(player);

    s32& spotRecoverTimer = self->field80To176[6]; // +0x68
    if (spotRecoverTimer != 0) {
        spotRecoverTimer -= 1;
    }

    const s32 strafeSpeed = GetFaceAngleDataBackAngle(self);

    s32 shouldDance = 0;
    WEffect* bossSpotLight = FindBossSpotLight();
    if (!bossSpotLight) {
        return;
    }

    LVector spotPos = bossSpotLight->pos;
    spotPos.y = owner->pos.y;

    if (owner->DistanceFromPoint(spotPos) < PAUL_SPOTLIGHT_RADIUS) {
        shouldDance = (distanceToPlayer > PAUL_CLOSE_ATTACK_DIST) ? 1 : 0;
    }

    s32& spotPathWasForcedOff = self->field80To176[3]; // +0x5C
    if (g_director && g_director->scriptState != 0) {
        bossSpotLight->EnablePath(0);
        spotPathWasForcedOff = 1;
        return;
    }

    if (spotPathWasForcedOff != 0) {
        bossSpotLight->EnablePath(1);
        spotPathWasForcedOff = 0;
    }

    s32& danceActive = self->field80To176[4]; // +0x60
    s32& danceTimer = self->field80To176[5]; // +0x64
    if (shouldDance != 0) {
        if (danceActive == 0 && spotRecoverTimer == 0) {
            bossSpotLight->EnablePath(0);
            danceActive = 1;
            danceTimer = PAUL_DANCE_TIME;
        }
    }

    if (danceActive != 0) {
        bool atAnimEnd = false;
        Model* model = owner->model ? static_cast<Model*>(owner->model) : nullptr;
        if (model && model->animStructure) {
            AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
            const s16 currentFrameHi = static_cast<s16>(static_cast<u32>(anim->currentFrame) >> 16);
            const s16 endFrameHi = static_cast<s16>(static_cast<u32>(anim->endFrame) >> 16);
            atAnimEnd = (currentFrameHi == endFrameHi);
        }

        if (atAnimEnd) {
            if ((s32)rmRangedRandom(100) < 50) {
                owner->field316 = 0x5B;
            }
            else {
                owner->field316 = 0x5A;
            }
            owner->RequestAction(1);
        }
        else {
            owner->RequestAction(0x15);
        }

        danceTimer -= 1;
        if (danceTimer > 0) {
            if (!IsPlayerAggressiveCombatState(owner->actionState)) {
                return;
            }
        }

        bossSpotLight->EnablePath(1);
        danceActive = 0;
        spotRecoverTimer = PAUL_RECOVER_TIME;
        return;
    }

    const s32 ownerInPlayerField = IsPointInFieldOf(owner->pos, player->pos, player->orientation.y, 0x1555, 0x1555) ? 1 : 0;
    const s32 playerPressureState = IsDantePhase1PressureState(player->actionState) ? 1 : 0;
    const s32 playerDiveRolling = (player->actionState == (s32)AS_DIVE_ROLL) ? 1 : 0;
    const s32 ownerInCombatState = ((u32)(owner->actionState - 0x20) < 5u) ? 1 : 0;

    s32& strafeDecision = self->field80To176[9]; // +0x74
    if (owner->actionState == (s32)AS_STRAFE) {
        if (owner->GetStraifPhase() != 0) {
            if (strafeDecision == 2) {
                owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
            }
            else if (strafeDecision == 3) {
                owner->SetDesiredMoveDirection(owner->faceAngle - 0x4000);
            }
            else if (strafeDecision == 1) {
                owner->SetDesiredMoveDirection(owner->faceAngle + 0x4000);
            }

            owner->SetTarget(player);
            owner->RequestAction(6);
            return;
        }

        owner->RequestAction(1);
        return;
    }

    if (IsPlayerAggressiveCombatState(player->actionState)) {
        owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
        owner->moveSpeed = strafeSpeed;
        owner->RequestAction(6);
        strafeDecision = 2;
        return;
    }

    if (distanceToPlayer > PAUL_CLOSE_ATTACK_DIST) {
        if (playerDiveRolling != 0 && ownerInPlayerField != 0) {
            owner->RequestAction(9);
            return;
        }

        if (playerPressureState != 0 && ownerInPlayerField != 0) {
            owner->RequestAction(8);
            return;
        }

        owner->moveSpeed = strafeSpeed;
        owner->RequestAction(2);
        return;
    }

    if (player->field484 != 0 && ownerInCombatState == 0) {
        if ((s32)rmRangedRandom(100) < 75) {
            if (!IsPlayerAggressiveCombatState(owner->actionState)) {
                s32 collisionRatio = 0;
                LVector wallNormal = {};
                LVector hitPoint = {};
                s32 verticalSpan = 0;
                s32 wallMaterial = 0;
                const s32 backBlocked = owner->CheckWallCollision(
                    (s16)(owner->faceAngle + 0x8000),
                    0x400,
                    0x96,
                    0x1F4,
                    collisionRatio,
                    wallNormal,
                    hitPoint,
                    verticalSpan,
                    wallMaterial);

                const s32 sideBlocked = owner->CheckWallCollision(
                    (s16)(owner->faceAngle + 0x4000),
                    0x200,
                    0x96,
                    0x1F4,
                    collisionRatio,
                    wallNormal,
                    hitPoint,
                    verticalSpan,
                    wallMaterial);

                const s32 randomChoice = (s32)rmRangedRandom(100);
                if (randomChoice < 50 && backBlocked == 0) {
                    owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
                    owner->moveSpeed = strafeSpeed;
                    strafeDecision = 2;
                }
                else if (randomChoice < 75 && sideBlocked == 0) {
                    owner->SetDesiredMoveDirection(owner->faceAngle + 0x4000);
                    owner->moveSpeed = (strafeSpeed * 3) / 2;
                    strafeDecision = 1;
                }
                else {
                    owner->SetDesiredMoveDirection(owner->faceAngle - 0x4000);
                    owner->moveSpeed = (strafeSpeed * 3) / 2;
                    strafeDecision = 3;
                }

                owner->SetTarget(player);
                owner->RequestAction(6);
                return;
            }
        }

        self->ComplexAttack();
        return;
    }

    if ((s32)rmRangedRandom(100) < 15) {
        owner->RequestAction(0x0E);
        return;
    }

    self->ComplexAttack();
}

// PSX: _OscarDMS__9Behaviour (BEHAVEB.CPP:1019, 0x8001E0B0)
void Behaviour::OscarDMS(Behaviour* self) {
    MARKFUNCTION(0x8001E0B0);

    if (!self || !self->owner) {
        return;
    }

    if (g_director && g_director->scriptState != 0) {
        return;
    }

    if (!self->InActiveZone()) {
        return;
    }

    Humanoid* player = Player::s_player;
    if (!player) {
        return;
    }

    Humanoid* owner = self->owner;
    owner->SetTarget(player);

    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);
    if ((u32)(owner->actionState - 0x20) >= 5u) {
        self->comboScriptIndex = -1;
        self->comboScriptCursor = 0;
    }

    if (IsPlayerAggressiveCombatState(player->actionState)) {
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = OscarDMS;

        self->navDecisionCounter = 0;
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = BackoffAndTaunt;
        return;
    }

    ActiveZone* ownerZone = owner->activeZone;
    if (ownerZone && self->BreakOffPathAndFight() == 0) {
        self->currentPath = ownerZone->FindFirstValidPath(owner);
    }

    if (self->currentPath) {
        self->InitPathAIState(self->currentPath);
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = OscarDMS;
        return;
    }

    s32 playerFacingFlag = 0;
    s32 targetingBossFlag = 0;
    s32 henchmanSupportFlag = 0;

    owner->FaceThingDesired(player);

    const s32 ownerFacing = owner->orientation.y;
    const s32 ownerToPlayerFacing = PsxClipAngle360(ownerFacing - player->orientation.y);
    // PSX (0x8001E294-0x8001E2B0): sltu against 0xC38E branches to the
    // circling path when v1 <= 0xC38E, so the flag is set on the > side, not
    // <=. This is a narrow ~85 deg window around 0 (nearly head-on), not the
    // wide ~275 deg middle range the inverted comparison previously matched
    // here, which made Oscar take the fast direct-chase branch almost always
    // instead of mostly circling/feinting at half speed.
    if ((u32)(ownerToPlayerFacing - OSCAR_ANGLE_WINDOW_START) > (u32)OSCAR_ANGLE_WINDOW_RANGE) {
        playerFacingFlag = 1;
    }

    const LVector ownerPos = owner->pos;
    if (playerFacingFlag == 0) {
        if (OscarsHenchman != nullptr) {
            const LVector henchmanPos = OscarsHenchman->pos;
            const s32 distanceToHenchman = owner->DistanceFromPoint(henchmanPos);
            const s32 sideFacing = ownerFacing + 0x4000;

            // PSX (0x8001E294-0x8001E330): both field-of-view probes use
            // ownerFacing, not sideFacing. sideFacing is only used by the
            // navDecision probe below.
            if (IsPointInFieldOf(henchmanPos, ownerPos, ownerFacing, 0x38E, 0x38E)
                && IsPointInFieldOf(player->pos, ownerPos, ownerFacing, 0x38E, 0x38E)
                && distanceToPlayer < distanceToHenchman) {
                henchmanSupportFlag = 1;
            }

            if (henchmanSupportFlag == 0) {
                self->navDecision = IsPointInFieldOf(henchmanPos, ownerPos, sideFacing, 0x4000, 0x4000) ? 2 : 3;
            }
        }
        else {
            self->navDecision = (ownerToPlayerFacing > 0x8000) ? 2 : 3;
        }
    }

    if (player->actionState == (s32)AS_BACK_GRAB_RECEIVE) {
        if (distanceToPlayer <= OSCAR_CLOSE_DIST) {
            self->ComplexAttack();
            return;
        }

        owner->moveSpeed = GetFaceAngleDataForwardAngle(self);

        s32 floorDelta = self->LookAheadFloorCheck(0, 0x200, 0x100);
        if (floorDelta < 0) {
            floorDelta = -floorDelta;
        }

        if ((u32)(floorDelta - 0x100) < 0x100u) {
            owner->RequestAction(3);

            self->nextHandlerThisOffset = 0;
            self->nextHandlerDispatch = -1;
            self->nextHandler = OscarDMS;

            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = Jumping;
            return;
        }

        owner->SetTarget(player);
        owner->RequestAction(6);
        return;
    }

    if (player->actionState == (s32)AS_STRAFE && player->field256 == (uintptr_t)owner) {
        targetingBossFlag = 1;
    }

    // PSX (0x8001E45C-0x8001E48C): bnez branches to the henchman/circling
    // path only when OscarsHenchman is non-null; falls through to this fast
    // chase path otherwise. So the OR condition is henchman == nullptr, not
    // != nullptr -- a solo Oscar (no henchman alive) goes straight to the
    // relentless chase, while a backed-up Oscar gets the circling/retreat
    // tree below.
    if (playerFacingFlag != 0 || targetingBossFlag != 0 || OscarsHenchman == nullptr) {
        if (distanceToPlayer <= OSCAR_CLOSE_DIST) {
            owner->RequestAction(8);
            return;
        }

        owner->moveSpeed = GetFaceAngleDataForwardAngle(self);

        s32 floorDelta = self->LookAheadFloorCheck(0, 0x200, 0x100);
        if (floorDelta < 0) {
            floorDelta = -floorDelta;
        }

        if ((u32)(floorDelta - 0x100) < 0x100u) {
            owner->RequestAction(3);
            owner->moveSpeed = GetFaceAngleDataForwardAngle(self);

            self->nextHandlerThisOffset = 0;
            self->nextHandlerDispatch = -1;
            self->nextHandler = OscarDMS;

            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = Jumping;
            return;
        }

        owner->SetTarget(player);
        owner->RequestAction(6);
        return;
    }

    if (distanceToPlayer <= OSCAR_CLOSE_DIST) {
        self->ComplexAttack();
        return;
    }

    if (distanceToPlayer <= OSCAR_MID_DIST) {
        owner->moveSpeed = GetFaceAngleDataBackAngle(self);
        owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
        owner->SetTarget(player);
        owner->RequestAction(6);
        return;
    }

    if (henchmanSupportFlag != 0) {
        owner->FaceThingDesired(player);
        owner->RequestAction(0x15);
        return;
    }

    s32 floorDelta = 0;
    if (self->navDecision == 2) {
        floorDelta = self->LookAheadFloorCheck(-0x4000, 0x96, 0x100);
        owner->SetDesiredMoveDirection(owner->faceAngle - 0x4000);
    }
    else {
        floorDelta = self->LookAheadFloorCheck(0x4000, 0x96, 0x100);
        owner->SetDesiredMoveDirection(owner->faceAngle + 0x4000);
    }

    s32 absFloorDelta = floorDelta;
    if (absFloorDelta < 0) {
        absFloorDelta = -absFloorDelta;
    }

    if ((u32)(absFloorDelta - 0x100) < 0x100u) {
        if (self->navDecision == 2) {
            owner->SetDesiredMoveDirection(ownerFacing - 0x4000);
        }
        else {
            owner->SetDesiredMoveDirection(ownerFacing + 0x4000);
        }

        owner->RequestAction(3);
        owner->moveSpeed = GetFaceAngleDataForwardAngle(self);

        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = OscarDMS;

        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = Jumping;

        s32 moveDirection = owner->faceAngle;
        self->NavigateWorld(moveDirection);
        return;
    }

    // PSX at 0x8001E6E8: the floor-range branch's delay slot loads 2 before
    // this navDecision comparison, so the selected side is live. The old
    // port always probed the + side, even when navDecision selected -.
    const s32 wallCheckSide = self->navDecision == 2 ? -0x4000 : 0x4000;
    const s32 wallCheckFarSide = self->navDecision == 2 ? -0x6000 : 0x6000;
    if (wallCheck(owner, ownerFacing + wallCheckSide) != 0
        || wallCheck(owner, ownerFacing + wallCheckFarSide) != 0) {
        owner->FaceThingDesired(player);
        owner->RequestAction(0x15);
        return;
    }

    owner->SetTarget(player);
    owner->moveSpeed = GetFaceAngleDataBackAngle(self);
    owner->RequestAction(6);

    s32 moveDirection = owner->faceAngle;
    self->NavigateWorld(moveDirection);
}

// PSX: _OscarHenchmanDMS__9Behaviour (BEHAVEB.CPP:1208, 0x8001E7DC)
void Behaviour::OscarHenchmanDMS(Behaviour* self) {
    MARKFUNCTION(0x8001E7DC);

    if (!self || !self->owner) {
        return;
    }

    if (g_director && g_director->scriptState != 0) {
        return;
    }

    if (!self->InActiveZone()) {
        return;
    }

    Humanoid* player = Player::s_player;
    if (!player) {
        return;
    }

    Humanoid* owner = self->owner;
    owner->SetTarget(player);

    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);
    if ((u32)(owner->actionState - 0x20) >= 5u) {
        self->comboScriptIndex = -1;
        self->comboScriptCursor = 0;
    }

    if (IsPlayerAggressiveCombatState(player->actionState)) {
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = OscarHenchmanDMS;

        self->navDecisionCounter = 0;
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = BackoffAndTaunt;
        return;
    }

    ActiveZone* ownerZone = owner->activeZone;
    if (ownerZone && self->BreakOffPathAndFight() == 0) {
        self->currentPath = ownerZone->FindFirstValidPath(owner);
    }

    if (self->currentPath) {
        self->InitPathAIState(self->currentPath);
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = OscarHenchmanDMS;
        return;
    }

    owner->FaceThingDesired(player);

    const s32 ownerFacing = owner->orientation.y;
    const s32 ownerToPlayerFacing = PsxClipAngle360(ownerFacing - player->orientation.y);
    const s32 forwardPressure = ((u32)(ownerToPlayerFacing - OSCAR_HENCH_ANGLE_WINDOW_START)
                                 <= (u32)OSCAR_HENCH_ANGLE_WINDOW_RANGE)
        ? 1
        : 0;

    if (forwardPressure == 0) {
        self->navDecision = (ownerToPlayerFacing > 0x8000) ? 2 : 3;
    }

    s32 syncState = 0;
    if (IsOscarSyncActionState(owner->actionState)) {
        syncState = IsOscarSyncActionState(player->actionState) ? 1 : 0;
    }

    if (syncState != 0) {
        OscarHenchmanSyncCounter = 0;
        return;
    }

    if (OscarHenchmanSyncCounter < OSCAR_HENCH_SYNC_COUNTER_MAX) {
        OscarHenchmanSyncCounter++;
        return;
    }

    if (forwardPressure != 0) {
        if (distanceToPlayer <= OSCAR_CLOSE_DIST) {
            if (bossOscar && bossOscar->actionState == (s32)AS_COMBAT_IDLE) {
                owner->RequestAction(1);
            }
            else {
                owner->RequestAction(7);
            }
            return;
        }

        owner->moveSpeed = GetFaceAngleDataForwardAngle(self);

        s32 floorDelta = self->LookAheadFloorCheck(0, 0x200, 0x100);
        if (floorDelta < 0) {
            floorDelta = -floorDelta;
        }

        if ((u32)(floorDelta - 0x100) < 0x100u) {
            owner->RequestAction(3);
            owner->moveSpeed = GetFaceAngleDataForwardAngle(self);

            self->nextHandlerThisOffset = 0;
            self->nextHandlerDispatch = -1;
            self->nextHandler = OscarHenchmanDMS;

            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = Jumping;
            return;
        }

        owner->SetTarget(player);
        owner->RequestAction(6);
        return;
    }

    if (distanceToPlayer <= OSCAR_CLOSE_DIST) {
        self->ComplexAttack();
        return;
    }

    if (distanceToPlayer <= OSCAR_MID_DIST) {
        owner->moveSpeed = GetFaceAngleDataBackAngle(self);
        owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
        owner->RequestAction(6);
        return;
    }

    s32 floorDelta = 0;
    if (self->navDecision == 2) {
        floorDelta = self->LookAheadFloorCheck(-0x4000, 0x96, 0x100);
        owner->SetDesiredMoveDirection(owner->faceAngle - 0x4000);
    }
    else {
        floorDelta = self->LookAheadFloorCheck(0x4000, 0x96, 0x100);
        owner->SetDesiredMoveDirection(owner->faceAngle + 0x4000);
    }

    s32 absFloorDelta = floorDelta;
    if (absFloorDelta < 0) {
        absFloorDelta = -absFloorDelta;
    }

    if ((u32)(absFloorDelta - 0x100) < 0x100u) {
        if (self->navDecision == 2) {
            owner->SetDesiredMoveDirection(ownerFacing - 0x4000);
        }
        else {
            owner->SetDesiredMoveDirection(ownerFacing + 0x4000);
        }

        owner->moveSpeed = GetFaceAngleDataForwardAngle(self);
        owner->RequestAction(3);

        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = OscarHenchmanDMS;

        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = Jumping;
        return;
    }

    // PSX at 0x8001ECEC: the floor-range branch's delay slot loads 2 before
    // this navDecision comparison, so the selected side is live. The old
    // port always probed the + side, even when navDecision selected -.
    const s32 wallCheckSide = self->navDecision == 2 ? -0x4000 : 0x4000;
    const s32 wallCheckFarSide = self->navDecision == 2 ? -0x6000 : 0x6000;
    if (wallCheck(owner, ownerFacing + wallCheckSide) != 0
        || wallCheck(owner, ownerFacing + wallCheckFarSide) != 0) {
        owner->FaceThingDesired(player);
        owner->RequestAction(0x15);
        return;
    }

    owner->SetTarget(player);
    owner->moveSpeed = GetFaceAngleDataBackAngle(self);
    owner->RequestAction(6);
}

// PSX: _DanteDMS_Phase1__9Behaviour (BEHAVEB.CPP:1967, 0x8001EEE0)
void Behaviour::DanteDMS_Phase1(Behaviour* self) {
    MARKFUNCTION(0x8001EEE0);

    if (!self || !self->owner) {
        return;
    }

    if (!self->InActiveZone()) {
        return;
    }

    Humanoid* player = Player::s_player;
    if (!player) {
        return;
    }

    Humanoid* owner = self->owner;
    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);

    if ((u32)(owner->actionState - 0x20) >= 5u) {
        self->comboScriptIndex = -1;
        self->comboScriptCursor = 0;
    }

    if (ShouldAdvanceDantePhase(owner, DANTE_PHASE1_HEALTH_SCALE)) {
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = DanteDMS_Phase2;
    }

    owner->FaceThingDesired(player);

    s32& damageCounter = self->field80To176[23];
    s32& lastKnownHealth = self->field80To176[24];
    const s32 currentHealth = (s32)(u32)owner->health;
    if (lastKnownHealth != 0 && currentHealth < lastKnownHealth) {
        damageCounter += (lastKnownHealth - currentHealth);
    }
    lastKnownHealth = currentHealth;

    s32 floorDelta = self->LookAheadFloorCheck(0, 0x200, 0x100);
    if ((u32)(floorDelta + 0x400) < 0xA01u) {
        s32 absFloorDelta = floorDelta;
        if (absFloorDelta < 0) {
            absFloorDelta = -absFloorDelta;
        }

        if (absFloorDelta >= 0x101) {
            owner->RequestAction(3);

            self->nextHandlerThisOffset = 0;
            self->nextHandlerDispatch = -1;
            self->nextHandler = DanteDMS_Phase1;

            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = Jumping;
        }
    }

    const s32 ownerInPlayerField = IsPointInFieldOf(owner->pos, player->pos, player->orientation.y, 0x1555, 0x1555) ? 1 : 0;
    const s32 pressureState = IsDantePhase1PressureState(player->actionState) ? 1 : 0;
    const s32 playerDiveRolling = (player->actionState == (s32)AS_DIVE_ROLL) ? 1 : 0;

    if (IsPlayerAggressiveCombatState(player->actionState)) {
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = DanteDMS_Phase1;

        self->navDecisionCounter = 0;
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = BackoffAndTaunt;
        return;
    }

    if (self->CounterAttack() != 0) {
        owner->RequestAction(0x14);
        damageCounter = 0;

        if ((u32)(owner->actionState - 0x2E) < 9u) {
            owner->LoadCombatDialog();
        }
        return;
    }

    if (distanceToPlayer > DANTE_PHASE1_DIST_THRESHOLD) {
        if (playerDiveRolling != 0 && ownerInPlayerField != 0) {
            owner->RequestAction(9);
            return;
        }

        if (pressureState != 0 && ownerInPlayerField != 0) {
            owner->RequestAction(8);
            return;
        }

        owner->moveSpeed = GetFaceAngleDataForwardAngle(self);
        owner->RequestAction(2);
        return;
    }

    if ((s32)rmRangedRandom(100) < 5) {
        owner->RequestAction(0x14);
        return;
    }

    self->ComplexAttack();
}

// PSX: _DanteDMS_Phase2__9Behaviour (BEHAVEB.CPP:2075, 0x8001F2B4)
void Behaviour::DanteDMS_Phase2(Behaviour* self) {
    MARKFUNCTION(0x8001F2B4);

    if (!self || !self->owner) {
        return;
    }

    Humanoid* owner = self->owner;
    ActiveZone* ownerZone = owner->activeZone;

    if (!self->InActiveZone()) {
        return;
    }

    Humanoid* player = Player::s_player;
    if (!player) {
        return;
    }

    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);
    if ((u32)(owner->actionState - 0x20) >= 5u) {
        self->comboScriptIndex = -1;
        self->comboScriptCursor = 0;
    }

    if (ShouldAdvanceDantePhase(owner, DANTE_PHASE2_HEALTH_SCALE)) {
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = DanteDMS_Phase3;
    }

    owner->FaceThingDesired(player);

    s32 floorDelta = self->LookAheadFloorCheck(0, 0x200, 0x100);
    if ((u32)(floorDelta + 0x400) < 0xA01u) {
        s32 absFloorDelta = floorDelta;
        if (absFloorDelta < 0) {
            absFloorDelta = -absFloorDelta;
        }

        if (absFloorDelta >= 0x101) {
            owner->RequestAction(3);

            self->nextHandlerThisOffset = 0;
            self->nextHandlerDispatch = -1;
            self->nextHandler = DanteDMS_Phase2;

            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = Jumping;
        }
    }

    SubZoneVolume* deskZone = static_cast<SubZoneVolume*>(ownerZone->subZoneList.FindNode(DANTE_PHASE2_DESK_SUBZONE_NAME));
    const s32 playerInDeskZone = deskZone->IsInSubZoneVolume(player) ? 1 : 0;

    if (playerInDeskZone != 0) {
        if (IsPlayerAggressiveCombatState(player->actionState)) {
            self->nextHandlerThisOffset = 0;
            self->nextHandlerDispatch = -1;
            self->nextHandler = DanteDMS_Phase2;

            self->navDecisionCounter = 0;
            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = BackoffAndTaunt;
            return;
        }

        if (distanceToPlayer < DANTE_PHASE2_CLOSE_DIST) {
            const s32 randomAction = (s32)rmRangedRandom(100);
            if (randomAction < 5) {
                owner->RequestAction(0x14);
                return;
            }

            if (randomAction < 50) {
                owner->RequestAction(0x0F);
                return;
            }

            self->ComplexAttack();
            return;
        }

        owner->SetTarget(player);
        owner->RequestAction(6);
        return;
    }

    if (IsPlayerAggressiveCombatState(player->actionState)) {
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = DanteDMS_Phase2;

        self->navDecisionCounter = 0;
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = BackoffAndTaunt;
        return;
    }

    if (distanceToPlayer < DANTE_PHASE2_CLOSE_DIST) {
        const s32 randomAction = (s32)rmRangedRandom(100);
        if (randomAction < 5) {
            owner->RequestAction(0x14);
            return;
        }

        if (randomAction < 30) {
            owner->RequestAction(0x0E);
            return;
        }

        self->ComplexAttack();
        return;
    }

    if (deskZone->IsInSubZoneVolume(owner)) {
        owner->RequestAction(0x1D);
        return;
    }

    LinearPath* path = ownerZone->FindFirstValidPath(owner);
    self->currentPath = path;
    self->InitPathAIState(path);

    self->nextHandlerThisOffset = 0;
    self->nextHandlerDispatch = -1;
    self->nextHandler = DanteDMS_Phase2;
}

// PSX: _DanteDMS_Phase3__9Behaviour (BEHAVEB.CPP:2184, 0x8001F678)
void Behaviour::DanteDMS_Phase3(Behaviour* self) {
    MARKFUNCTION(0x8001F678);

    if (!self || !self->owner) {
        return;
    }

    if (!self->InActiveZone()) {
        return;
    }

    Humanoid* player = Player::s_player;
    if (!player) {
        return;
    }

    Humanoid* owner = self->owner;
    owner->SetTarget(player);

    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);
    if ((u32)(owner->actionState - 0x20) >= 5u) {
        self->comboScriptIndex = -1;
        self->comboScriptCursor = 0;
    }

    owner->FaceThingDesired(player);

    s32 floorDelta = self->LookAheadFloorCheck(0, 0x200, 0x100);
    if ((u32)(floorDelta + 0x400) < 0xA01u) {
        s32 absFloorDelta = floorDelta;
        if (absFloorDelta < 0) {
            absFloorDelta = -absFloorDelta;
        }

        if (absFloorDelta >= 0x101) {
            owner->RequestAction(3);

            self->nextHandlerThisOffset = 0;
            self->nextHandlerDispatch = -1;
            self->nextHandler = DanteDMS_Phase3;

            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = Jumping;
        }
    }

    if (self->CounterAttack() != 0) {
        owner->RequestAction(0x14);
        self->field80To176[23] = 0;

        if ((u32)(owner->actionState - 0x2E) < 9u) {
            owner->LoadCombatDialog();
        }
        return;
    }

    s32 shouldBackAway = 0;
    if ((u32)(player->actionState - 0x20) < 5u) {
        shouldBackAway = ((u32)(owner->actionState - 0x20) < 5u) ? 0 : 1;
    }

    if (shouldBackAway != 0) {
        owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
        owner->moveSpeed = GetFaceAngleDataBackAngle(self);
        owner->RequestAction(6);
        return;
    }

    if (distanceToPlayer < DANTE_PHASE3_CLOSE_DIST) {
        if (IsPlayerAggressiveCombatState(player->actionState)) {
            self->nextHandlerThisOffset = 0;
            self->nextHandlerDispatch = -1;
            self->nextHandler = DanteDMS_Phase3;

            self->navDecisionCounter = 0;
            self->handlerThisOffset = 0;
            self->handlerDispatch = -1;
            self->handler = BackoffAndTaunt;
            return;
        }

        if ((s32)rmRangedRandom(100) < 20) {
            owner->RequestAction(7);
            return;
        }

        self->ComplexAttack();
        return;
    }

    if (distanceToPlayer < DANTE_PHASE3_MID_DIST) {
        if (IsPlayerAggressiveCombatState(player->actionState)) {
            owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
        }

        owner->moveSpeed = GetFaceAngleDataBackAngle(self);
        owner->RequestAction(6);
        return;
    }

    if (IsPlayerAggressiveCombatState(player->actionState)) {
        owner->RequestAction(1);
        return;
    }

    if (owner->actionState != (s32)AS_TARGET_MISSILE_ATTACK && (s32)rmRangedRandom(100) < 40) {
        owner->field316 = 0x5A;
        owner->RequestAction(0x15);
        return;
    }

    owner->RequestAction(1);
    u8* ownerBytes = reinterpret_cast<u8*>(owner);
    *reinterpret_cast<s32*>(ownerBytes + 0x108) = player->pos.x;
    *reinterpret_cast<s32*>(ownerBytes + 0x10C) = player->pos.y;
    *reinterpret_cast<s32*>(ownerBytes + 0x110) = player->pos.z;
    owner->RequestAction(0x1C);
}

bool Behaviour::InActiveZone() const {
    MARKFUNCTION(0x80074888);
    return owner && owner->IsInActiveZone();
}

void Behaviour::Process() {
    MARKFUNCTION(0x800748AC);

    if (handlerDispatch == 0) {
        return;
    }

    if (handlerDispatch <= 0) {
        if (handler) {
            u8* base = reinterpret_cast<u8*>(this);
            Behaviour* target = reinterpret_cast<Behaviour*>(base + handlerThisOffset);
            handler(target);
        }
        return;
    }
}

// PSX: BreakOffPathAndFight__9Behaviour (BEHAVE.CPP:1201, 0x80074BA0)
s32 Behaviour::BreakOffPathAndFight() {
    MARKFUNCTION(0x80074BA0);

    if (!owner) {
        return 0;
    }

    if ((u32)(owner->actionState - 25) < 3u) {
        return 0;
    }

    Player* player = Player::s_player;
    if (!player) {
        return 0;
    }

    if (owner->DistanceFromPointXZ(player->pos) >= PATH_BREAKOFF_DIST_THRESHOLD) {
        return 0;
    }

    return IsPointInFieldOf(player->pos, owner->pos, owner->orientation.y, BREAKOFF_FIELD_LEFT, BREAKOFF_FIELD_RIGHT)
        ? 1
        : 0;
}

// PSX: InitPathAIState__9BehaviourP10LinearPath (BEHAVE.CPP:4299, 0x800778C4)
void Behaviour::InitPathAIState(LinearPath* path) {
    MARKFUNCTION(0x800778C4);

    currentPath = path;
    currentPathNodeIndex = 0;
    field36 = 0;

    if (currentPath) {
        handlerThisOffset = 0;
        handlerDispatch = -1;
        handler = AiFollowPath;
    }
}

// PSX: AiFollowPath__9Behaviour (BEHAVE.CPP:1248, 0x80074C80)
void Behaviour::AiFollowPath(Behaviour* self) {
    MARKFUNCTION(0x80074C80);

    if (!self || !self->owner) {
        return;
    }

    self->field276 = 0;

    if (!self->InActiveZone()) {
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = GetBackIntoActiveZone;
        return;
    }

    if (self->BreakOffPathAndFight() != 0) {
        RestoreDeferredHandlerOrAiFollowPathFallback(self);
        return;
    }

    ActiveZone* activeZone = self->owner->activeZone;
    LinearPath* path = self->currentPath;
    if (!activeZone || !path || self->currentPathNodeIndex >= (u32)path->numPoints) {
        RestoreDeferredHandlerOrAiFollowPathFallback(self);
        return;
    }

    const u32 nodeIndex = self->currentPathNodeIndex;
    const u32 distToNode = (u32)self->owner->DistanceFromPointXZ(path->positions[nodeIndex]);

    if (distToNode < PATH_NODE_REACHED_THRESHOLD) {
        activeZone->DoActionsAtNode(path, (s32)nodeIndex, self->owner);

        self->currentPathNodeIndex = nodeIndex + 1;
        if (self->currentPathNodeIndex >= (u32)path->numPoints
            || activeZone->DoAICheck(path, (s32)self->currentPathNodeIndex, self->owner) == 0) {
            RestoreDeferredHandlerOrAiFollowPathFallback(self);
            return;
        }
    }
    else {
        if (activeZone->AllowBreakoffOfDestinationNode(path, (s32)nodeIndex)
            && activeZone->DoAICheck(path, (s32)nodeIndex, self->owner) == 0) {
            RestoreDeferredHandlerOrAiFollowPathFallback(self);
            return;
        }
    }

    if (activeZone->IsPathNodeTerminator(path, (s32)self->currentPathNodeIndex)) {
        if (self->currentPathNodeIndex != 0) {
            self->currentPathNodeIndex--;
        }
        return;
    }

    if (self->animConfigPtr) {
        self->owner->moveSpeed = (s16)self->animConfigPtr->runningSpeed;
    }

    self->owner->FacePointDesired(path->positions[self->currentPathNodeIndex]);
    self->owner->RequestAction(2);
}

// PSX: Idle__9Behaviour (BEHAVE.CPP:1877, 0x800754DC)
void Behaviour::Idle(Behaviour* self) {
    MARKFUNCTION(0x800754DC);

    if (!self || !self->owner) {
        return;
    }

    if (g_directorActive != 0) {
        return;
    }

    Humanoid* owner = self->owner;
    ActiveZone* ownerZone = owner->activeZone;
    if (!ownerZone) {
        return;
    }

    ActiveZone* activeZoneFromAttrib = nullptr;
    if (g_ai && owner->field388 != 0) {
        activeZoneFromAttrib = static_cast<ActiveZone*>(g_ai->activeZoneList.FindNodeCRC((u32)owner->field388, nullptr));
    }

    bool active = true;

    if (owner->field388 == 0) {
        if (owner->field392 == 0
            && owner->field396 == 0
            && owner->field400 == 0
            && owner->field404 == 0
            && owner->field412 == 0) {
            if (!ownerZone->IsInActiveZone(Player::s_player)) {
                return;
            }
        }
    }

    if (owner->field388 != 0) {
        active = (activeZoneFromAttrib != nullptr) ? activeZoneFromAttrib->IsInActiveZone(Player::s_player) : false;
    }

    if (owner->field392 != 0 || owner->field396 != 0 || owner->field400 != 0) {
        bool inSubZone = false;

        if (owner->field392 != 0) {
            SubZoneVolume* sub = static_cast<SubZoneVolume*>(ownerZone->subZoneList.FindNodeCRC((u32)owner->field392, nullptr));
            inSubZone = (sub != nullptr) ? sub->IsInSubZoneVolume(Player::s_player) : false;
        }

        if (owner->field396 != 0) {
            SubZoneVolume* sub = static_cast<SubZoneVolume*>(ownerZone->subZoneList.FindNodeCRC((u32)owner->field396, nullptr));
            inSubZone = inSubZone || ((sub != nullptr) ? sub->IsInSubZoneVolume(Player::s_player) : false);
        }

        if (owner->field400 != 0) {
            SubZoneVolume* sub = static_cast<SubZoneVolume*>(ownerZone->subZoneList.FindNodeCRC((u32)owner->field400, nullptr));
            inSubZone = inSubZone || ((sub != nullptr) ? sub->IsInSubZoneVolume(Player::s_player) : false);
        }

        active = active && inSubZone;
    }

    if (owner->field408 != -1) {
        if (ownerZone->IsInActiveZone(Player::s_player)) {
            const s32 memberCountMinusOne = (s32)ownerZone->memberCount - 1;
            if (owner->field408 >= memberCountMinusOne) {
                active = true;
            }
        }
    }

    if (!active) {
        return;
    }

    if (owner->field404 != 0) {
        SubZoneVolume* sub = static_cast<SubZoneVolume*>(ownerZone->subZoneList.FindNodeCRC((u32)owner->field404, nullptr));
        if (sub && sub->IsInSubZoneVolume(Player::s_player)) {
            return;
        }
    }

    if (owner->field412 != 0) {
        Model* model = static_cast<Model*>(owner->model);
        // PSX 0x80075750: only bit 4 (srl 4, then and with the 0/1 flag).
        const s32 modelGate = model ? (((s32)model->modelFlags >> 4) & 1) : 0;
        active = active && (modelGate != 0);
    }

    if (!active) {
        return;
    }

    if (self->BreakOffPathAndFight() == 0) {
        self->currentPath = ownerZone->FindFirstValidPath(owner);
    }

    if (self->currentPath) {
        self->nextHandlerThisOffset = 0;
        self->nextHandlerDispatch = -1;
        self->nextHandler = NDMS;
        self->InitPathAIState(self->currentPath);
    }
    else {
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = NDMS;
    }
}

// PSX: GetBackIntoActiveZone__9Behaviour (BEHAVE.CPP:4174, 0x800776FC)
void Behaviour::GetBackIntoActiveZone(Behaviour* self) {
    MARKFUNCTION(0x800776FC);

    if (!self || !self->owner) {
        return;
    }

    Player* player = Player::s_player;
    if (!player) {
        return;
    }

    if (self->owner->DistanceFromPoint(player->pos) <= REJOIN_ACTIVE_ZONE_PLAYER_DIST_THRESHOLD) {
        self->handlerThisOffset = 0;
        self->handlerDispatch = -1;
        self->handler = NDMS;
        return;
    }

    ActiveZone* activeZone = self->owner->activeZone;
    if (!activeZone) {
        return;
    }

    LVector center = {};
    activeZone->GetActiveZoneCenterPoint(center);

    if (self->InActiveZone()) {
        s32 testX = self->owner->pos.x;
        s32 testZ = self->owner->pos.z;

        if (testX < center.x) {
            testX -= 0x400;
        }
        else {
            testX += 0x400;
        }

        if (testZ < center.z) {
            testZ -= 0x400;
        }
        else {
            testZ += 0x400;
        }

        const s32 distFromCenterXZ = self->owner->DistanceFromPointXZ(center);
        if (activeZone->box.IsInside(testX, testZ) || distFromCenterXZ < 0x101) {
            if (self->owner->IsTargetInActiveZone()) {
                self->handlerThisOffset = 0;
                self->handlerDispatch = -1;
                self->handler = NDMS;
            }
            else {
                self->owner->FaceThingDesired(player);
                // PSX 0x80077854: writes owner+0x13C (the taunt animation), not the move speed.
                self->owner->field316 = 0x5A;
                self->owner->RequestAction(0x15);
            }
            return;
        }
    }

    self->owner->FacePointDesired(center);
    self->owner->moveSpeed = (s16)self->animConfigPtr->strafingSpeed;
    self->owner->SetTarget(player);
    self->owner->RequestAction(6);
}

// PSX: PlayerUserControl__9Behaviour (BEHAVE.CPP:1474, 0x80074F5C)
void Behaviour::PlayerUserControl(Behaviour* self) {
    MARKFUNCTION(0x80074F5C);
    if (!self || !self->owner || !g_game || !g_inputManager) {
        return;
    }

    Humanoid* owner = self->owner;

    if (!DebugUI::IsPlayerInputAllowed()) {
        owner->RequestAction(GA_GUARD_RELEASE);
        return;
    }

    u32 buttons = 0;
    if ((self->behaviourFlags & Behaviour::BF_INPUT_PROCESSING) != 0) {
        buttons = (u32)g_game->GetControlVal(self->padPort);
    }
    else {
        self->behaviourFlags |= Behaviour::BF_INPUT_PROCESSING;
    }

    const Control* control = nullptr;
    if ((u16)self->padPort < 2u) {
        control = &g_inputManager->controls[self->padPort];
    }

    s32 analogX = 0;
    s32 analogY = 0;
    s32 stickX = 0;
    s32 stickY = 0;
    s32 direction = 0;
    s32 ownerAngle = owner->orientation.y;
    s32 targetAngle = ownerAngle;

    owner->moveSpeed = 0;

    if (control && (control->padType == 's' || control->padType == 'S') && buttons != 0) {
        stickX = (s32)control->analogLX - 127;
        stickY = (s32)control->analogLY - 127;

        s32 absX = stickX;
        if (absX < 0) {
            absX = -absX;
        }
        if (absX >= 64) {
            analogX = stickX;
            if (stickX > 0) {
                buttons |= PsxPad::Right;
            }
            else {
                buttons |= PsxPad::Left;
            }
        }

        s32 absY = stickY;
        if (absY < 0) {
            absY = -absY;
        }
        if (absY >= 64) {
            analogY = stickY;
            if (stickY > 0) {
                buttons |= PsxPad::Down;
            }
            else {
                buttons |= PsxPad::Up;
            }
        }
    }
    else if ((buttons & 0xF000u) != 0) {
        if ((buttons & PsxPad::Left) != 0) {
            analogX = -32;
            stickX = -127;
        }
        else if ((buttons & PsxPad::Right) != 0) {
            analogX = 32;
            stickX = 127;
        }

        if ((buttons & PsxPad::Up) != 0) {
            analogY = -32;
            stickY = -127;
        }
        else if ((buttons & PsxPad::Down) != 0) {
            analogY = 32;
            stickY = 127;
        }
    }

    if (analogX != 0 || analogY != 0) {
        Camera& cam = g_game->GetCamera();
        s32 cameraAngle = cam.GetOrientY();

        s32 mirroredAnalogX = analogX;
#if NEW_CHEATS
        if (IsCheatEnabled(CheatOption::MirrorWorld)) {
            mirroredAnalogX = -analogX;
        }
#endif
        targetAngle = cameraAngle - rmATan216((f32)mirroredAnalogX, (f32)(-(s16)analogY)) + ANGLE_QUARTER_TURN;

        s32 angleDiff = ownerAngle - targetAngle;
        if (angleDiff > ANGLE_FULL_ROTATION) {
            s32 wrapped = angleDiff - ANGLE_FULL_ROTATION;
            while (wrapped > ANGLE_FULL_ROTATION) {
                wrapped -= ANGLE_FULL_ROTATION;
            }
            angleDiff = wrapped;
        }
        if (angleDiff < 0) {
            s32 wrapped = angleDiff + ANGLE_FULL_ROTATION;
            // Match PSX MIPS delay-slot flow exactly: the add executes once
            // even when the branch is not taken.
            while (true) {
                const bool wasNegative = wrapped < 0;
                wrapped += ANGLE_FULL_ROTATION;
                if (!wasNegative) {
                    break;
                }
            }
            angleDiff = wrapped - ANGLE_FULL_ROTATION;
        }

        if (angleDiff < 0) {
            angleDiff = -angleDiff;
        }

        s32 dirCode = DIR_BACKWARD;
        if ((u32)(angleDiff - DIR_BEHIND_START) >= DIR_BEHIND_RANGE) {
            dirCode = DIR_LEFT;
            if ((u32)(angleDiff - DIR_LEFT_START) >= DIR_LEFT_RANGE) {
                dirCode = DIR_RIGHT;
                if ((u32)(angleDiff - DIR_RIGHT_START) >= DIR_RIGHT_RANGE) {
                    dirCode = DIR_FORWARD;
                }
            }
        }
        direction = dirCode;

        s32 absX = stickX;
        if (absX < 0) {
            absX = -absX;
        }
        s32 absY = stickY;
        if (absY < 0) {
            absY = -absY;
        }
        s32 maxAxis = stickX;
        if (absY >= absX) {
            maxAxis = stickY;
        }
        if (maxAxis < 0) {
            maxAxis = -maxAxis;
        }

        s32 range = (s32)(((s64)(maxAxis << 16) * (s64)rmDiv16i(g_maxAttackRange << 16, STICK_MAX_FP)) >> 16) >> 16;
        if (range) {
            owner->moveSpeed = range;
        }
        else {
            owner->moveSpeed = 0;
        }
    }

    if (self->previousButtons == buttons) {
        self->buttonHoldCounter++;
    }
    else {
        self->buttonHoldCounter = 0;
    }
    self->previousButtons = buttons;

    s32 actionReq = FindActionRequest(self->actionRequestState, buttons & 0xFFFF0FFFu, direction, (u16)self->padPort);

    if ((u32)(actionReq - 2) < 3
        || actionReq == GA_STRAFE
        || actionReq == 19
        || actionReq == GA_GRAB
        || actionReq == GA_GRAB_FORWARD
        || actionReq == 16) {
        owner->SetDesiredMoveDirection(targetAngle);
    }

    owner->RequestAction((u32)actionReq);
}

// PSX: MoveToDestinationPoint__9BehaviourUl (BEHAVIOU.CPP:1736, 0x80075408)
// Returns 1 when the owner humanoid has reached destPoint within threshold distance.
// Otherwise faces the point and requests Run (far) or Walk (close) action.
s32 Behaviour::MoveToDestinationPoint(u32 threshold) {
    MARKFUNCTION(0x80075408);

    u32 dist = (u32)owner->DistanceFromPointXZ(destPoint);

    if (dist > threshold) {
        // Far from destination - run
        owner->FacePointDesired(destPoint);
        owner->RequestAction(2);  // GA_RUN
        // PSX: sets owner->moveSpeed from animConfigPtr speed data
        if (animConfigPtr) {
            s16 spd = (s16)animConfigPtr->runningSpeed;
            owner->moveSpeed = spd;
        }
        return 0;
    }

    if (dist > (threshold >> 1)) {
        // Close to destination - walk (half speed)
        owner->FacePointDesired(destPoint);
        owner->RequestAction(6);  // GA_WALK
        // PSX: sets owner->moveSpeed to half of speed
        if (animConfigPtr) {
            s16 spd = (s16)animConfigPtr->runningSpeed;
            owner->moveSpeed = (spd + ((u16)spd >> 15)) >> 1;
        }
        return 0;
    }

    // Arrived at destination
    return 1;
}

// PSX: NisControl__9Behaviour (BEHAVIOU.CPP:1774, 0x800753C4)
void Behaviour::NisControl(Behaviour* b) {
    MARKFUNCTION(0x800753C4);
    if (b->MoveToDestinationPoint(0x4B) != 0) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = PlayerUserControl;
    }
}

// PSX: ComplexAttack__9Behaviour (BEHAVE.CPP:1015, 0x80074914)
void Behaviour::ComplexAttack() {
    MARKFUNCTION(0x80074914);

    if (!owner || !animConfigPtr) {
        return;
    }

    if (comboScriptIndex == -1 && animConfigPtr->comboCount > 0) {
        if (owner->field484) {
            return;
        }

        s32 randomChance = (s32)rmRangedRandom(100) + 1;
        s32 selected = 0;
        while (selected < (s32)animConfigPtr->comboCount) {
            const s32 chance = (s32)animConfigPtr->comboChances[selected];
            if (chance >= randomChance) {
                break;
            }
            randomChance -= chance;
            selected++;
        }

        if (selected < (s32)animConfigPtr->comboCount) {
            comboScriptIndex = selected;
            comboScriptCursor = 0;
        }
    }

    if (comboScriptIndex != -1 && animConfigPtr->comboStrings) {
        const char* comboString = animConfigPtr->comboStrings[comboScriptIndex];
        if (!comboString) {
            comboScriptIndex = -1;
            comboScriptCursor = 0;
            return;
        }

        const char actionCode = comboString[comboScriptCursor];
        if (actionCode == 0) {
            comboScriptIndex = -1;
            comboScriptCursor = 0;
            return;
        }

        if (comboScriptCursor != 0) {
            if (owner->field488) {
                return;
            }
        }
        else if (owner->field484) {
            return;
        }

        comboScriptCursor++;

        switch (actionCode) {
            case 'C':
                owner->RequestAction(17);
                return;
            case 'F':
                owner->RequestAction(15);
                return;
            case 'K':
                owner->RequestAction(9);
                return;
            case 'L':
                owner->RequestAction(12);
                return;
            case 'P':
                owner->RequestAction(8);
                return;
            case 'T':
                owner->RequestAction(7);
                return;
            case 'W':
                owner->RequestAction(18);
                return;
            default:
                return;
        }
    }

    s32 randomChance = (s32)rmRangedRandom(100) + 1;
    char actionCode = 0;

    if (randomChance <= (s32)animConfigPtr->punchPerc) {
        actionCode = 'P';
    }
    else {
        randomChance -= (s32)animConfigPtr->punchPerc;
        if (randomChance <= (s32)animConfigPtr->kickPerc) {
            actionCode = 'K';
        }
        else {
            randomChance -= (s32)animConfigPtr->kickPerc;
            if (randomChance <= (s32)animConfigPtr->throwPerc) {
                actionCode = 'T';
            }
            else {
                owner->moveSpeed = (s32)(s16)animConfigPtr->strafingSpeed;
                owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
                // PSX 0x80074AEC: keeps the player as target while backing off.
                owner->SetTarget(Player::s_player);
                owner->RequestAction(6);
                return;
            }
        }
    }

    switch (actionCode) {
        case 'P':
            owner->RequestAction(8);
            return;
        case 'K':
            owner->RequestAction(9);
            return;
        case 'T':
            owner->RequestAction(7);
            return;
        default:
            return;
    }
}

// PSX: BackoffAndTaunt__9Behaviour (BEHAVE.CPP:2033, 0x800757EC)
void Behaviour::BackoffAndTaunt(Behaviour* b) {
    MARKFUNCTION(0x800757EC);

    if (!b || !b->owner || !b->animConfigPtr) {
        return;
    }

    b->owner->FaceThingDesired(Player::s_player);

    // PSX 0x80075820: this tests the thing type (lhu owner+0x18), not the action state.
    const s32 state = (s32)b->owner->thingType;
    const bool stateAllowsDistanceChecks =
        state == 10 || state == 12 || state == 13 || state == 15 || state == 17;

    if (!stateAllowsDistanceChecks) {
        if (b->navDecisionCounter < 15) {
            s32 floorDelta = b->LookAheadFloorCheck((s16)0x8000, 512, 256);
            if (floorDelta < 0) {
                floorDelta = -floorDelta;
            }
            if (floorDelta < NDMS_FLOOR_SAFE_DELTA_THRESHOLD) {
                b->navDecisionCounter++;
                b->owner->moveSpeed = (s32)(s16)b->animConfigPtr->strafingSpeed;
                b->owner->SetDesiredMoveDirection(b->owner->faceAngle + 0x8000);
                b->owner->SetTarget(Player::s_player);
                b->owner->RequestAction(6);
                return;
            }
        }

        b->navDecisionCounter = 0;
    }
    else {
        const s32 distToPlayer = b->owner->DistanceFromPoint(Player::s_player->pos);

        s32 floorDelta = b->LookAheadFloorCheck((s16)0x8000, 50, 256);
        if (floorDelta < 0) {
            floorDelta = -floorDelta;
        }

        const s32 wallBlocked = b->LookAheadWallCheck((s16)0x8000, 50, 256);
        if (distToPlayer < 1500 && floorDelta < NDMS_FLOOR_SAFE_DELTA_THRESHOLD && wallBlocked == 0) {
            b->owner->moveSpeed = (s32)(s16)b->animConfigPtr->strafingSpeed;
            b->owner->SetDesiredMoveDirection(b->owner->faceAngle + 0x8000);
            b->owner->SetTarget(Player::s_player);
            b->owner->RequestAction(6);
            return;
        }

        b->navDecisionCounter = 0;
    }

    if ((s32)rmRangedRandom(100) < 25 || b->owner->HasEnemyTauntDialog()) {
        const s32 tauntRoll = (s32)rmRangedRandom(4);
        if (tauntRoll == 1) {
            b->owner->field316 = 91;
        }
        else if (tauntRoll != 0) {
            b->owner->field316 = 90;
        }
        else {
            b->owner->field316 = 92;
        }

        b->owner->RequestAction(21);
    }

    RestoreDeferredHandlerOrNDMS(b);
}

// PSX: BackOutOfTheFight__9Behaviour (BEHAVE.CPP:2133, 0x80075A68)
void Behaviour::BackOutOfTheFight(Behaviour* b) {
    MARKFUNCTION(0x80075A68);

    if (!b || !b->owner || !b->animConfigPtr) {
        return;
    }

    b->owner->FaceThingDesired(Player::s_player);

    const s32 state = b->owner->actionState;
    const bool inRecoverRange = ((u32)(state - 69) < 4u) || state == 56;
    if (inRecoverRange) {
        RestoreDeferredHandlerOrNDMS(b);
        return;
    }

    if ((u32)(state - 46) < 9u) {
        return;
    }

    if (b->navDecisionCounter < 11) {
        s32 floorDelta = b->LookAheadFloorCheck((s16)0x8000, 512, 256);
        if (floorDelta < 0) {
            floorDelta = -floorDelta;
        }

        if (floorDelta < NDMS_FLOOR_SAFE_DELTA_THRESHOLD) {
            b->navDecisionCounter++;
            b->owner->moveSpeed = (s32)(s16)b->animConfigPtr->strafingSpeed;
            b->owner->SetDesiredMoveDirection(b->owner->faceAngle + 0x8000);
            b->owner->SetTarget(Player::s_player);
            b->owner->RequestAction(6);
            return;
        }
    }

    b->navDecisionCounter = 0;
    RestoreDeferredHandlerOrNDMS(b);
}

// PSX: NDMS__9Behaviour (BEHAVE.CPP:2214, 0x80075BE4)
void Behaviour::NDMS(Behaviour* b) {
    MARKFUNCTION(0x80075BE4);

    if (!b || !b->owner || !b->animConfigPtr) {
        return;
    }

    Humanoid* owner = b->owner;
    b->currentPath = nullptr;
    owner->FaceThingDesired(Player::s_player);

    s32 desiredMoveDir = owner->faceAngle;
    s32 randomCircling = 0;
    s32 randomAggression = 0;
    s32 randomDistancing = 0;

    ActiveZone* activeZone = owner->activeZone;
    if (!activeZone) {
        return;
    }

    Player* player = Player::s_player;
    if (!player) {
        return;
    }

    owner->SetTarget(player);

    if (!b->InActiveZone()) {
        if (activeZone->specialFlag) {
            b->handlerThisOffset = 0;
            b->handlerDispatch = -1;
            b->handler = DieWhenWeHitTheGround;
            return;
        }

        if (owner->DistanceFromPoint(player->pos) > NDMS_MID_DIST_THRESHOLD) {
            b->handlerThisOffset = 0;
            b->handlerDispatch = -1;
            b->handler = GetBackIntoActiveZone;
            return;
        }
    }

    if (activeZone && b->BreakOffPathAndFight() == 0) {
        b->currentPath = activeZone->FindFirstValidPath(owner);
    }

    if (b->currentPath) {
        b->InitPathAIState(b->currentPath);
        b->nextHandlerThisOffset = 0;
        b->nextHandlerDispatch = -1;
        b->nextHandler = NDMS;
        return;
    }

    const s32 playerState = player->actionState;
    if ((u32)(playerState - 69) < 4u || playerState == 56) {
        b->nextHandlerThisOffset = 0;
        b->nextHandlerDispatch = -1;
        b->nextHandler = NDMS;
        b->navDecisionCounter = 0;
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = BackoffAndTaunt;
        return;
    }

    const s32 actionState = owner->actionState;
    if ((u32)(actionState - 32) >= 5u) {
        b->comboScriptIndex = -1;
        b->comboScriptCursor = 0;
    }

    if ((u32)(actionState - (s32)AS_COLLAPSE_STUN) < 4u || actionState == (s32)AS_SPIN_BACK) {
        return;
    }

    Thing* pickup = g_ai ? g_ai->GetPickupWithinReach(owner) : nullptr;
    if (pickup) {
        Pickup* p = static_cast<Pickup*>(pickup);
        if (p->field332 != 0) {
            owner->RequestAction(7);
            return;
        }
    }

    b->ndmsThinkCounter++;
    // PSX 0x80075E68: rerolls once the counter reaches the delay (slt counter, delay).
    if (b->ndmsThinkCounter >= b->ndmsThinkDelay) {
        b->ndmsThinkCounter = 0;
        b->ndmsThinkDelay = (s32)b->animConfigPtr->minThinkFreq
            + (s32)rmRangedRandom((u32)((s32)b->animConfigPtr->maxThinkFreq - (s32)b->animConfigPtr->minThinkFreq));
        // PSX 0x80075EA0: a fourth roll whose result is discarded.
        rmRangedRandom(100);
        randomCircling = (s32)rmRangedRandom(100);
        randomDistancing = (s32)rmRangedRandom(100);
        randomAggression = (s32)rmRangedRandom(100);
    }

    s32 decision = b->ndmsDecision;
    const s32 distanceToPlayer = owner->DistanceFromPoint(player->pos);
    const s32 facingDelta = PsxClipAngle360(owner->orientation.y - player->orientation.y);
    const bool facingAway = (u32)(facingDelta - 0x2AAA) > 0xAAABu;

    if (distanceToPlayer > NDMS_FAR_DIST_THRESHOLD) {
        b->ndmsRangeBand = 0;

        s32 dy = owner->pos.y - player->pos.y;
        if (dy < 0) {
            dy = -dy;
        }

        // PSX: player well above/below but horizontally within attackingDist.
        if (dy > NDMS_HEIGHT_DELTA_THRESHOLD
            && owner->DistanceFromPointXZ(player->pos) <= REJOIN_ACTIVE_ZONE_PLAYER_DIST_THRESHOLD) {
            if (randomAggression - 20 < (s32)b->animConfigPtr->attackFreq) {
                decision = 6;
            }
            else {
                decision = owner->HasEnemyTauntDialog() ? 6 : 5;
            }
        }
        else if (randomCircling < (s32)b->animConfigPtr->aggression) {
            if (b->field60 != 0 && distanceToPlayer < NDMS_SLOWDOWN_DIST_THRESHOLD) {
                owner->moveSpeed = (s32)(s16)b->animConfigPtr->strafingSpeed;
                decision = 1;
            }
            else {
                b->field60 = 0;
                decision = 0;
                owner->moveSpeed = (s32)(s16)b->animConfigPtr->runningSpeed;
            }

            if (!owner->GetTicketIssuer()) {
                b->NavigateWorld(desiredMoveDir);
            }
        }
        else {
            if (randomCircling - 20 < (s32)b->animConfigPtr->aggression) {
                decision = 6;
            }
            else if (randomCircling - 20 < (s32)b->animConfigPtr->attackFreq) {
                decision = 6;
            }
            else {
                decision = owner->HasEnemyTauntDialog() ? 6 : 5;
            }
        }
    }
    else if (distanceToPlayer > NDMS_MID_DIST_THRESHOLD) {
        b->ndmsRangeBand = 1;
        b->field60 = 1;

        s32 dy = owner->pos.y - player->pos.y;
        if (dy < 0) {
            dy = -dy;
        }

        // PSX: player well above/below but horizontally within attackingDist.
        if (dy > NDMS_HEIGHT_DELTA_THRESHOLD
            && owner->DistanceFromPointXZ(player->pos) <= REJOIN_ACTIVE_ZONE_PLAYER_DIST_THRESHOLD) {
            if (randomAggression - 20 < (s32)b->animConfigPtr->attackFreq) {
                decision = 6;
            }
            else {
                decision = owner->HasEnemyTauntDialog() ? 6 : 5;
            }
        }
        else if (randomCircling < (s32)b->animConfigPtr->aggression) {
            owner->moveSpeed = (s32)(s16)b->animConfigPtr->strafingSpeed;

            if (owner->GetTicketIssuer()) {
                decision = 1;
            }
            else {
                b->NavigateWorld(desiredMoveDir);
                decision = b->NavigateEnemies(1);
            }
        }
        else {
            if (randomCircling - 20 < (s32)b->animConfigPtr->aggression) {
                decision = 6;
            }
            else if (randomCircling - 20 < (s32)b->animConfigPtr->attackFreq) {
                decision = 6;
            }
            else {
                decision = owner->HasEnemyTauntDialog() ? 6 : 5;
            }
        }
    }
    else if (distanceToPlayer > REJOIN_ACTIVE_ZONE_PLAYER_DIST_THRESHOLD) {
        // PSX 0x800761F8: between attackingDist and fringeDist.
        b->ndmsRangeBand = 2;
        b->field60 = 1;

        if (b->comboScriptIndex != -1) {
            b->ComplexAttack();
            return;
        }

        s32 dy = owner->pos.y - player->pos.y;
        if (dy < 0) {
            dy = -dy;
        }

        // PSX: player well above/below but horizontally within attackingDist.
        if (dy > NDMS_HEIGHT_DELTA_THRESHOLD
            && owner->DistanceFromPointXZ(player->pos) <= REJOIN_ACTIVE_ZONE_PLAYER_DIST_THRESHOLD) {
            if (randomAggression < (s32)b->animConfigPtr->attackFreq) {
                // PSX 0x80076288: slt player.y, owner.y
                if (player->pos.y < owner->pos.y) {
                    owner->RequestAction(9);
                }
                else {
                    owner->RequestAction(8);
                }
                return;
            }

            if (randomCircling - 20 < (s32)b->animConfigPtr->attackFreq) {
                decision = 6;
            }
            else {
                decision = owner->HasEnemyTauntDialog() ? 6 : 5;
            }
        }
        else if (randomCircling < (s32)b->animConfigPtr->aggression || facingAway) {
            owner->moveSpeed = (s32)(s16)b->animConfigPtr->strafingSpeed;

            if (owner->GetTicketIssuer()) {
                decision = 1;
            }
            else if (b->aiParam != 0 && activeZone) {
                if (activeZone->AllowedToMoveIn(owner) != 0) {
                    if (facingAway) {
                        decision = 1;
                    }
                    else {
                        decision = b->NavigateEnemies(2);
                    }
                }
                else {
                    decision = b->NavigateEnemies(3);
                }
            }
            else {
                decision = b->NavigateEnemies(2);
            }
        }
        else if (randomAggression >= (s32)b->animConfigPtr->attackFreq) {
            decision = 5;
        }
        else if (randomDistancing < (s32)b->animConfigPtr->distancing) {
            decision = 6;
        }
        else {
            decision = 7;
        }
    }
    else {
        // PSX 0x800763C4: within attackingDist.
        b->ndmsRangeBand = 3;
        b->field60 = 1;

        if (b->comboScriptIndex != -1) {
            b->ComplexAttack();
            return;
        }

        if ((u32)(actionState - 46) < 9u && (s32)b->animConfigPtr->aggression < randomCircling) {
            b->nextHandlerThisOffset = 0;
            b->nextHandlerDispatch = -1;
            b->nextHandler = NDMS;
            b->navDecisionCounter = 0;
            b->handlerThisOffset = 0;
            b->handlerDispatch = -1;
            b->handler = BackOutOfTheFight;
            return;
        }

        const bool zoneControlled = (b->aiParam != 0 && activeZone);
        if (zoneControlled && activeZone->AllowedToMoveIn(owner) == 0) {
            owner->moveSpeed = (s32)(s16)b->animConfigPtr->strafingSpeed;
            decision = 4;
        }
        else if (randomAggression < (s32)b->animConfigPtr->attackFreq) {
            decision = 7;
        }
        else if (facingAway && randomAggression - 70 < (s32)b->animConfigPtr->attackFreq) {
            decision = 7;
        }
        else if ((s32)b->animConfigPtr->aggression < randomCircling) {
            b->nextHandlerThisOffset = 0;
            b->nextHandlerDispatch = -1;
            b->nextHandler = NDMS;
            b->navDecisionCounter = 0;
            b->handlerThisOffset = 0;
            b->handlerDispatch = -1;
            b->handler = BackOutOfTheFight;
            return;
        }
        else {
            decision = 5;
        }
    }

    b->ndmsDecision = decision;

    if ((u32)b->ndmsDecision < 5u) {
        if (b->ndmsDecision == 0 || b->ndmsDecision == 1) {
            if (!owner->GetTicketIssuer()) {
                s32 floorDelta = b->LookAheadFloorCheck(0, 512, 256);
                s32 absFloorDelta = floorDelta;
                if (absFloorDelta < 0) {
                    absFloorDelta = -absFloorDelta;
                }

                if (absFloorDelta >= NDMS_FLOOR_SAFE_DELTA_THRESHOLD) {
                    if ((u32)(floorDelta + 0x400) < 0xA01u) {
                        owner->RequestAction(3);
                        b->nextHandlerThisOffset = 0;
                        b->nextHandlerDispatch = -1;
                        b->nextHandler = NDMS;
                        b->handlerThisOffset = 0;
                        b->handlerDispatch = -1;
                        b->handler = Jumping;
                        return;
                    }

                    s32 nextDecision = 6;
                    if ((s32)rmRangedRandom(1) == 0 && !owner->HasEnemyTauntDialog()) {
                        nextDecision = 5;
                    }
                    b->ndmsDecision = nextDecision;
                }
            }
        }
        else if (b->ndmsDecision == 2 || b->ndmsDecision == 3) {
            const s16 angleOffset = (b->ndmsDecision == 2) ? (s16)-0x4000 : (s16)0x4000;
            s32 floorDelta = b->LookAheadFloorCheck(angleOffset, 512, 256);
            if (floorDelta < 0) {
                floorDelta = -floorDelta;
            }

            if (floorDelta >= NDMS_FLOOR_SAFE_DELTA_THRESHOLD) {
                s32 nextDecision = 6;
                if ((s32)rmRangedRandom(1) == 0 && !owner->HasEnemyTauntDialog()) {
                    nextDecision = 5;
                }
                b->ndmsDecision = nextDecision;
            }
        }
        else if (b->ndmsDecision == 4) {
            s32 floorDelta = b->LookAheadFloorCheck((s16)0x8000, 512, 256);
            if (floorDelta < 0) {
                floorDelta = -floorDelta;
            }

            if (floorDelta >= NDMS_FLOOR_SAFE_DELTA_THRESHOLD) {
                // PSX 0x800766CC: 5 with a taunt line, else unchanged.
                if ((s32)rmRangedRandom(1) != 0) {
                    b->ndmsDecision = 6;
                }
                else if (owner->HasEnemyTauntDialog()) {
                    b->ndmsDecision = 5;
                }
            }
        }
    }

    owner->SetDesiredMoveDirection(desiredMoveDir);
    owner->SetTarget(player);

    switch (b->ndmsDecision) {
        case 0:
            owner->RequestAction(2);
            break;
        case 1:
            owner->RequestAction(6);
            break;
        case 2:
            owner->SetDesiredMoveDirection(owner->faceAngle - 0x4000);
            owner->RequestAction(6);
            break;
        case 3:
            owner->SetDesiredMoveDirection(owner->faceAngle + 0x4000);
            owner->RequestAction(6);
            break;
        case 4:
            owner->SetDesiredMoveDirection(owner->faceAngle + 0x8000);
            owner->RequestAction(6);
            break;
        case 6:
        {
            const s32 tauntRoll = (s32)rmRangedRandom(4);
            if (tauntRoll == 0) {
                owner->field316 = 92;
            }
            else if (tauntRoll == 1) {
                owner->field316 = 91;
            }
            else {
                owner->field316 = 90;
            }
            owner->RequestAction(21);
            break;
        }
        case 7:
            b->ComplexAttack();
            break;
        default:
            owner->RequestAction(1);
            break;
    }
}

// PSX: NavigateWorld__9BehaviourRl (BEHAVE.CPP:3317, 0x80076870)
s32 Behaviour::NavigateWorld(s32& moveDirection) {
    MARKFUNCTION(0x80076870);

    if (!owner) {
        return 0;
    }

    if (worldNavCached != 0) {
        moveDirection = worldNavDirection;
        if (worldNavCachedTicks > 0) {
            worldNavCachedTicks--;
        }
        else {
            worldNavCached = 0;
        }
        return moveDirection;
    }

    s32 collisionRatio = 0;
    LVector wallNormal = {};
    LVector hitPoint = {};
    s32 verticalSpan = 0;
    s32 wallMaterial = 0;

    if (worldNavBlocked != 0) {
        s32 hit = owner->CheckWallCollision(
            (s16)moveDirection,
            1024,
            150,
            500,
            collisionRatio,
            wallNormal,
            hitPoint,
            verticalSpan,
            wallMaterial);
        if (hit == 0 && owner->CheckDWOCollision((s16)moveDirection, 1024) == 0) {
            worldNavBlocked = 0;
            return moveDirection;
        }

        hit = owner->CheckWallCollision(
            (s16)worldNavDirection,
            512,
            150,
            500,
            collisionRatio,
            wallNormal,
            hitPoint,
            verticalSpan,
            wallMaterial);
        if (hit == 0 && owner->CheckDWOCollision((s16)worldNavDirection, 1024) == 0) {
            worldNavCached = 1;
            worldNavCachedTicks = 1;
            moveDirection = worldNavDirection;
            return moveDirection;
        }

        s32 wallAngle = 0;
        if (wallNormal.x != 0) {
            if (wallNormal.z == 0) {
                wallAngle = (wallNormal.x > 0) ? 0x4000 : 0xC000;
            }
            else {
                wallAngle = -(s32)rmATan216((f32)(-wallNormal.x), (f32)(-wallNormal.z)) + 0x4000;
                s32 absAngle = wallAngle;
                if (absAngle < 0) {
                    absAngle = -absAngle;
                }
                if (absAngle > 0x7FFF) {
                    if (wallAngle > 0) {
                        wallAngle -= 0x10000;
                    }
                    else {
                        wallAngle += 0x10000;
                    }
                }
                wallAngle += 0x8000;
            }
        }
        else {
            wallAngle = (wallNormal.z < 1) ? 0x8000 : 0;
        }

        const LVector playerPos = Player::s_player ? Player::s_player->pos : owner->pos;

        s32 turnDegrees = 0;
        if (wallNormal.x != 0) {
            turnDegrees = -15;
            if (wallNormal.x <= 0) {
                turnDegrees = 15;
                if (hitPoint.z < playerPos.z) {
                    turnDegrees = -15;
                }
            }
            else if (hitPoint.z < playerPos.z) {
                turnDegrees = 15;
            }
        }
        else {
            turnDegrees = 15;
            if (wallNormal.z > 0) {
                if (hitPoint.x < playerPos.x) {
                    turnDegrees = -15;
                }
            }
            else {
                turnDegrees = -15;
                if (hitPoint.x < playerPos.x) {
                    turnDegrees = 15;
                }
            }
        }

        if (turnDegrees <= 0) {
            if (owner->CheckWallCollision(
                (s16)(wallAngle + 0x4000),
                512,
                150,
                500,
                collisionRatio,
                wallNormal,
                hitPoint,
                verticalSpan,
                wallMaterial) != 0) {
                turnDegrees = 15;
            }
        }
        else {
            if (owner->CheckWallCollision(
                (s16)(wallAngle - 0x4000),
                512,
                150,
                500,
                collisionRatio,
                wallNormal,
                hitPoint,
                verticalSpan,
                wallMaterial) != 0) {
                turnDegrees = -15;
            }
        }

        worldNavDirection += (turnDegrees << 16) / 360;
        moveDirection = worldNavDirection;
        return moveDirection;
    }

    s32 hit = owner->CheckWallCollision(
        (s16)moveDirection,
        1024,
        150,
        500,
        collisionRatio,
        wallNormal,
        hitPoint,
        verticalSpan,
        wallMaterial);
    if (hit != 0 || owner->CheckDWOCollision((s16)moveDirection, 1024) != 0) {
        worldNavBlocked = 1;
    }
    else {
        worldNavCached = 1;
        worldNavCachedTicks = 1;
    }

    worldNavDirection = moveDirection;
    return moveDirection;
}

// PSX: NavigateEnemies__9Behaviouri (BEHAVE.CPP:3549, 0x80076C84)
s32 Behaviour::NavigateEnemies(s32 mode) {
    MARKFUNCTION(0x80076C84);

    if (!owner || !animConfigPtr) {
        return 1;
    }

    navDecisionCounter++;

    LVector ownerPos = owner->pos;
    const s32 sideFieldAngle = owner->orientation.y - 0x4000;
    s32 bias = 50;

    if (aiParam != 0) {
        ActiveZone* zone = owner->activeZone;
        if (zone) {
            for (s32 i = 0; i < 3; i++) {
                Humanoid* other = zone->members[i];
                if (!other || other == owner) {
                    continue;
                }

                if (IsPointInFieldOf(other->pos, ownerPos, sideFieldAngle, 0x4000, 0x4000)) {
                    bias += 30;
                }
                else {
                    bias -= 30;
                }
            }
        }
    }
    else {
        Humanoid** humans = FightingCollision::GetHumanoidArray();
        for (s32 i = 0; i < FIGHTING_COLLISION_MAX; i++) {
            Humanoid* other = humans[i];
            // PSX 0x80076DA8: the player is in this array too and is skipped.
            if (!other || other == Player::s_player || other == owner) {
                continue;
            }

            if (IsPointInFieldOf(other->pos, ownerPos, sideFieldAngle, 0x4000, 0x4000)) {
                bias += 30;
            }
            else {
                bias -= 30;
            }
        }
    }

    if (mode == 3) {
        if (navDecisionCounter >= 51) {
            navDecisionCounter = 0;
            const s32 roll = (s32)rmRangedRandom(100);
            if ((u32)(roll - 31) < 0x27u) {
                navDecision = ((u32)(roll - 36) < 0x1Du) ? 5 : 4;
            }
            else if (roll >= bias) {
                navDecision = 2;
            }
            else {
                navDecision = mode;
            }
        }
        return navDecision;
    }

    if (mode == 2) {
        if (navDecisionCounter < 51) {
            return navDecision;
        }

        navDecisionCounter = 0;
        if ((s32)rmRangedRandom(100) >= (s32)animConfigPtr->circling) {
            navDecision = ((s32)animConfigPtr->aggression < (s32)rmRangedRandom(100)) ? 5 : 1;
            return navDecision;
        }

        const s32 roll = (s32)rmRangedRandom(100);
        if ((u32)(roll - 31) < 0x27u) {
            navDecision = ((u32)(roll - 41) < 0x13u) ? 5 : 4;
        }
        else if (roll >= bias) {
            navDecision = 2;
        }
        else {
            navDecision = 3;
        }
        return navDecision;
    }

    if (mode != 1) {
        return 1;
    }

    if (navDecisionCounter < 51) {
        return navDecision;
    }

    navDecisionCounter = 0;
    if ((s32)rmRangedRandom(150) >= (s32)animConfigPtr->circling) {
        if ((s32)animConfigPtr->aggression >= (s32)rmRangedRandom(100)) {
            navDecision = mode;
        }
        else {
            navDecision = 5;
        }
        return navDecision;
    }

    const s32 roll = (s32)rmRangedRandom(100);
    if ((u32)(roll - 41) < 0x13u) {
        navDecision = 5;
    }
    else if (roll >= bias) {
        navDecision = 2;
    }
    else {
        navDecision = 3;
    }

    return navDecision;
}

// PSX: Jumping__9Behaviour (BEHAVE.CPP:4057, 0x800774BC)
void Behaviour::Jumping(Behaviour* b) {
    MARKFUNCTION(0x800774BC);

    if (!b || !b->owner) {
        return;
    }

    if ((b->owner->flags & TF_ON_GROUND) == 0) {
        b->owner->RequestAction(2);
        return;
    }

    if (b->field276 != 0) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = AiFollowPath;
        return;
    }

    RestoreDeferredHandlerOrJumpingFallback(b);
}

// PSX: DieWhenWeHitTheGround__9Behaviour (BEHAVE.CPP:4119, 0x8007762C)
void Behaviour::DieWhenWeHitTheGround(Behaviour* b) {
    MARKFUNCTION(0x8007762C);

    if (!b || !b->owner) {
        return;
    }

    const s32 floorHeight = g_collisionSectors[0].GetWorldFloorHeight(b->owner->pos, 0);
    if (floorHeight == (s32)0x80000001) {
        b->owner->health = 0;
    }
    else {
        if ((b->owner->flags & TF_ON_GROUND) == 0) {
            b->owner->RequestAction(2);
            return;
        }
        b->owner->health = 0;
    }

    if (b->owner->actionState != (s32)AS_DEAD) {
        b->owner->SetActionState(AS_DEAD, 0);
    }
}

// PSX: LookAheadFloorCheck__9Behaviourlll (BEHAVE.CPP:4325, 0x800778F8)
s32 Behaviour::LookAheadFloorCheck(s16 angleOffset, s32 distance, s32 radius) {
    MARKFUNCTION(0x800778F8);

    if (!owner) {
        return (s32)0x80000001;
    }

    LVector testPos = owner->pos;
    const s16 angle = (s16)(owner->orientation.y + angleOffset);
    testPos.x += (s32)(((s64)rmSin16(angle) * distance) >> 16);
    testPos.y += 1024;
    testPos.z += (s32)(((s64)rmSin16((s16)(angle + 0x4000)) * distance) >> 16);

    const s32 floorHeight = g_collisionSectors[0].GetWorldFloorHeight(testPos, radius);
    if (floorHeight != (s32)0x80000001) {
        return testPos.y - floorHeight - 1024;
    }
    return floorHeight;
}

// PSX: LookAheadWallCheck__9Behaviourlll (BEHAVE.CPP:4355, 0x80077A14)
s32 Behaviour::LookAheadWallCheck(s16 angleOffset, s32 distance, s32 radius) {
    MARKFUNCTION(0x80077A14);

    if (!owner) {
        return 0;
    }

    return owner->QuickCheckWallCollision((s16)(owner->orientation.y + angleOffset), distance, radius, 512);
}

// PSX: SubwayDodgeRight__9Behaviour (BEHAVE.CPP:3813, 0x80077004)
void Behaviour::SubwayDodgeRight(Behaviour* b) {
    MARKFUNCTION(0x80077004);

    if (!b || !b->owner) {
        return;
    }

    Obstacle* issuer = dynamic_cast<Obstacle*>(b->owner->GetTicketIssuer());
    if (!issuer) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = Behaviour::NDMS;
        return;
    }

    const s32 facingCheck = PsxClipAngle360(issuer->orientation.y + 0x8000 - b->owner->orientation.y);
    if ((u32)(facingCheck - 0x4000) <= 0x8000u) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = Behaviour::NDMS;
        return;
    }

    tagCollisionBox testBox = issuer->collBox;
    testBox.minX = (s16)(testBox.minX + 100);
    testBox.minY = (s16)(testBox.minY + 100);
    testBox.minZ = (s16)(testBox.minZ + 100);
    testBox.maxX = (s16)(testBox.maxX - 100);
    testBox.maxY = (s16)(testBox.maxY - 100);
    testBox.maxZ = (s16)(testBox.maxZ - 100);

    LVector delta = {};
    delta.x = b->owner->pos.x - issuer->pos.x;
    delta.y = b->owner->pos.y - issuer->pos.y;
    delta.z = b->owner->pos.z - issuer->pos.z;

    const s32 sinV = rmSin16(issuer->orientation.y);
    const s32 cosV = rmSin16(issuer->orientation.y + 0x4000);
    const s32 localX = (s32)(((s64)cosV * delta.x) >> 16) + (s32)((-(s64)sinV * delta.z) >> 16);
    const s32 localZ = (s32)(((s64)sinV * delta.x) >> 16) + (s32)(((s64)cosV * delta.z) >> 16);

    if (testBox.minX > testBox.maxX || testBox.minY > testBox.maxY || testBox.minZ > testBox.maxZ
        || localX < testBox.minX || localX > testBox.maxX
        || delta.y < testBox.minY || delta.y > testBox.maxY
        || localZ < testBox.minZ || localZ > testBox.maxZ) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = Behaviour::NDMS;
        return;
    }

    b->owner->FaceThingDesired(nullptr);
    b->owner->FaceAngleY(b->owner->faceAngle, 0);
    if (b->animConfigPtr) {
        b->owner->moveSpeed = (s16)b->animConfigPtr->runningSpeed;
    }
    b->owner->SetDesiredMoveDirection(b->owner->faceAngle + 0x4000);
    b->owner->SetTarget(nullptr);
    b->owner->RequestAction(6);
}

// PSX: SubwayDodgeLeft__9Behaviour (BEHAVE.CPP:3911, 0x80077200)
void Behaviour::SubwayDodgeLeft(Behaviour* b) {
    MARKFUNCTION(0x80077200);

    if (!b || !b->owner) {
        return;
    }

    Obstacle* issuer = dynamic_cast<Obstacle*>(b->owner->GetTicketIssuer());
    if (!issuer) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = Behaviour::NDMS;
        return;
    }

    const s32 facingCheck = PsxClipAngle360(issuer->orientation.y + 0x8000 - b->owner->orientation.y);
    if ((u32)(facingCheck - 0x4000) <= 0x8000u) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = Behaviour::NDMS;
        return;
    }

    tagCollisionBox testBox = issuer->collBox;
    testBox.minX = (s16)(testBox.minX + 100);
    testBox.minY = (s16)(testBox.minY + 100);
    testBox.minZ = (s16)(testBox.minZ + 100);
    testBox.maxX = (s16)(testBox.maxX - 100);
    testBox.maxY = (s16)(testBox.maxY - 100);
    testBox.maxZ = (s16)(testBox.maxZ - 100);

    LVector delta = {};
    delta.x = b->owner->pos.x - issuer->pos.x;
    delta.y = b->owner->pos.y - issuer->pos.y;
    delta.z = b->owner->pos.z - issuer->pos.z;

    const s32 sinV = rmSin16(issuer->orientation.y);
    const s32 cosV = rmSin16(issuer->orientation.y + 0x4000);
    const s32 localX = (s32)(((s64)cosV * delta.x) >> 16) + (s32)((-(s64)sinV * delta.z) >> 16);
    const s32 localZ = (s32)(((s64)sinV * delta.x) >> 16) + (s32)(((s64)cosV * delta.z) >> 16);

    if (testBox.minX > testBox.maxX || testBox.minY > testBox.maxY || testBox.minZ > testBox.maxZ
        || localX < testBox.minX || localX > testBox.maxX
        || delta.y < testBox.minY || delta.y > testBox.maxY
        || localZ < testBox.minZ || localZ > testBox.maxZ) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = Behaviour::NDMS;
        return;
    }

    b->owner->FaceThingDesired(nullptr);
    b->owner->FaceAngleY(b->owner->faceAngle, 0);
    if (b->animConfigPtr) {
        b->owner->moveSpeed = (s16)b->animConfigPtr->runningSpeed;
    }
    b->owner->SetDesiredMoveDirection(b->owner->faceAngle - 0x4000);
    b->owner->SetTarget(nullptr);
    b->owner->RequestAction(6);
}

// PSX: SubwayDodgeJump__9Behaviour (BEHAVE.CPP:4001, 0x800773FC)
void Behaviour::SubwayDodgeJump(Behaviour* b) {
    MARKFUNCTION(0x800773FC);

    if (!b || !b->owner) {
        return;
    }

    Thing* issuer = b->owner->GetTicketIssuer();
    if (!issuer) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = Behaviour::NDMS;
        return;
    }

    const s32 facingCheck = PsxClipAngle360(issuer->orientation.y + 0x8000 - b->owner->orientation.y);
    if ((u32)(facingCheck - 9102) <= 0xB8E3u) {
        b->handlerThisOffset = 0;
        b->handlerDispatch = -1;
        b->handler = Behaviour::NDMS;
        return;
    }

    if (b->animConfigPtr) {
        b->owner->moveSpeed = (s16)b->animConfigPtr->runningSpeed;
    }
    b->owner->RequestAction(3);

    b->nextHandlerThisOffset = 0;
    b->nextHandlerDispatch = -1;
    b->nextHandler = NDMS;

    b->handlerThisOffset = 0;
    b->handlerDispatch = -1;
    b->handler = Behaviour::Jumping;
}

// PSX: Dead__8Humanoid (HUMANOID.CPP:5732, 0x800691FC)
// Unconditional clear keyed on thingType, not pointer identity -- matches
// "bne thingType,0x17,skip / sw zero,OscarsHenchman" exactly.
void Behaviour::ClearOscarsHenchmanOnDeath(Humanoid* dying) {
    if (dying && dying->thingType == (s32)AITypes::TT_OSCAR_HENCHMAN) {
        OscarsHenchman = nullptr;
    }
}

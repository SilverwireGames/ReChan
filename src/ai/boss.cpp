#include "gen/common.h"
#include "ai/boss.h"
#include "ai/player.h"
#include "gen/ai.h"
#include "gen/animstruct.h"
#include "gen/camera.h"
#include "gen/colsect.h"
#include "gen/control.h"
#include "gen/database.h"
#include "gen/display.h"
#include "gen/geffect.h"
#include "gen/model.h"
#include "gen/path.h"
#include "gen/trail.h"
#include "p3d/hash.h"
#include "p3d/p3dmath.h"
#include "pc/log.h"
#include "snd/snddrct.h"

struct PsxFightingMoveRaw {
    u32 address;
    u32 firstWord;
    s32 turnDelta;
    u16 anim;
    u16 fightingPoints;
    u8 stylePointsFlag;
    s8 moveWindowStart;
    s8 moveWindowEnd;
    s8 moveDelta;
    s8 combatWindowStart;
    s8 combatWindowEnd;
    u8 weaponBreakOnEmpty;
    u8 fightingType;
    u32 data20;
    u32 data24;
    s16 throwVectorX;
    s16 throwVectorY;
    s16 throwVectorZ;
    s16 throwAttachX;
    s16 throwAttachY;
    s16 throwAttachZ;
    u16 throwTargetAnim;
    s8 throwImpactFrame;
    s8 throwAttachFrame;
    s8 throwReleaseFrame;
    s8 throwScoreFrame;
};

struct FightingComboNode {
    u32 psxAddress;
    u8 requestedCommand;
    s8 minFrame;
    s8 maxFrame;
    u8 field03;
    u8 field04;
    s8 field05;
    s8 field06;
    s8 field07;
    const PsxFightingMoveRaw* moveData;
    FightingComboNode* child;
    FightingComboNode* sibling;
};

const FightingComboNode* ResolveFightingNodeAddressConst(u32 nodeAddress);

static constexpr u32 BUTCH_STOMP_EFFECT_HASH = 0x0B0E21C4u;
static constexpr u32 AS_BUTCH_THROW_POT = 76u;
static constexpr u32 BOSS_RISING_ATTACK_REMAP_BIT = 0x00800000u;
static constexpr u32 BOSS_RISING_ATTACK_CLEAR_MASK =
    ~( (1u << GA_PUNCH)
    | (1u << GA_KICK)
    | (1u << GA_GRAB)
    | (1u << GA_GRAB_FORWARD)
    | (1u << GA_GRAB_HELD)
    | (1u << GA_GRAB_FWD_HELD));

static constexpr u32 GRONTAR_DIVE_ROLL_KICK_ROOT_ADDRESS = 0x80020BF8u;
static constexpr s32 GRONTAR_DIVE_ROLL_FIGHT_DISTANCE = 0x2EE;
static constexpr s32 GRONTAR_DIVE_ROLL_FIGHT_HALF_ANGLE = 0x2AAA;

// PSX gp globals:
// 0x800DD5B0..0x800DD5C4
static constexpr s32 GRONTAR_HIT_THROW_BREAKOUT = 0x0A;
static constexpr u32 GrontarThrowDistance = 0x640u;
static constexpr s32 GrontarThrowHalfAngle = 0x0C16;
static constexpr s32 GRONTAR_TARGET_TRACK_HORIZONTAL_SWING = 2;
static constexpr s32 GRONTAR_TARGET_TRACK_OVERHEAD_SWING = 0;
static constexpr s32 GRONTAR_TARGET_TRACK_DEFAULT = 7;
static constexpr s32 DANTE_MISSILE_GROUP_SIZE = 5;
static s32 s_danteMissileAttackFrameThreshold = 0x14;
static constexpr s32 DANTE_MISSILE_DAMAGE_RADIUS = 0x320;
static constexpr u16 DANTE_MISSILE_DAMAGE = 0x14;
static constexpr s32 DANTE_RECOVERY_TIME = 0x32;
static s32 s_danteTargetMissileFrameThreshold = 0x19;
static constexpr s32 DANTE_MISSILE_TRAIL_STEPS = 3;
static constexpr u32 DANTE_MISSILE_TRAIL_COLOR = 0x50FAu;
static constexpr s32 DANTE_MISSILE_TRAIL_START_OFFSET = 0x1E;
static constexpr s32 DANTE_MISSILE_TRAIL_END_OFFSET = 0x96;
static constexpr s32 DANTE_TARGET_MISSILE_TRAIL_START_OFFSET = 0x32;
static constexpr s32 DANTE_TARGET_MISSILE_TRAIL_END_OFFSET = 0xB4;
static constexpr u32 DANTE_MISSILE_EFFECT_HASH = 0x0D513C22u;
static constexpr u32 DANTE_TARGET_MISSILE_EFFECT_HASH = 0x0AD513C5u;
static constexpr const char* DANTE_PATH_PATTERN1_NAME = "Pattern1";
static constexpr const char* DANTE_PATH_PATTERN2_NAME = "Pattern2";
static constexpr const char* DANTE_MISSILE_LAUNCHER_NAME = "AI_Missile_Launcher";
static constexpr s32 DANTE_KNOCKDOWN_DIALOG_PRIORITY = 10;
static constexpr s32 DANTE_TARGETING_FRAME = 0x14;

// PSX: OscarStraif table at 0x800D9230.
static s32 s_oscarStraif[] = {
    22, 0,
    49, 0,
    96, 0,
    50, 0,
    97, 0,
};

static constexpr s32 OSCAR_STRAIF_PHASE_GUARD_ANIM = 49;

static Grontar* s_bossGrontar = nullptr;
static Dante* s_bossDante = nullptr;
static Butch* s_bossButch = nullptr;
static Oscar* s_bossOscar = nullptr;
static Paul* s_bossPaul = nullptr;

static LVector CrossFixed16(const LVector& lhs, const LVector& rhs) {
    LVector out = {};
    out.x = static_cast<s32>(((static_cast<s64>(lhs.y) * rhs.z) - (static_cast<s64>(lhs.z) * rhs.y)) >> 16);
    out.y = static_cast<s32>(((static_cast<s64>(lhs.z) * rhs.x) - (static_cast<s64>(lhs.x) * rhs.z)) >> 16);
    out.z = static_cast<s32>(((static_cast<s64>(lhs.x) * rhs.y) - (static_cast<s64>(lhs.y) * rhs.x)) >> 16);
    return out;
}

static bool IsDanteMissileImmuneActionState(s32 actionState) {
    switch (actionState) {
    case 6:
    case 8:
    case 13:
    case 16:
    case 33:
    case 35:
        return true;
    default:
        return false;
    }
}

// PSX: __4BossPC10tagLVectorUs (BOSS.CPP:148)
Boss::Boss(const LVector* initialPos, u16 type)
    : Humanoid(initialPos, type) {
    MARKFUNCTION(0x8001A758);
    SetHumanoidTarget(nullptr);
}

// PSX: _._4Boss (BOSS.CPP:155)
Boss::~Boss() {
    MARKFUNCTION(0x8001A79C);
}

// PSX: CreateModel__4BossPCc (BOSS.CPP:158)
void Boss::CreateModel(const char* name) {
    MARKFUNCTION(0x8001A7C4);

    Model* m = static_cast<Model*>(model);
    if (!m) {
        HumanoidModel* hm = new HumanoidModel();
        model = hm;
        hm->backPtr = this;
        m = hm;
    }

    behaviour = new Behaviour(this, thingType, 0);

    Thing::CreateModel(name);

    m = static_cast<Model*>(model);
    if (m) {
        m->ApplyAnimToModel(thingType, 0, 2, 0, 0);
        static_cast<SModel*>(m)->SetupModelCallbacks();
    }

    if (actionState != (s32)AS_NIS_MODE) {
        SetActionState(AS_STAND, 0);
    }

    orientation = spawnOrientation;
    faceAngle = spawnOrientation.y;
    CreateSound();
}

// PSX: SetActionState__4BossUll (BOSS.CPP:210)
void Boss::SetActionState(u32 state, s32 param) {
    MARKFUNCTION(0x8001A8EC);
    Humanoid::SetActionState(state, param);
}

// PSX: _Collapse__4Boss (BOSS.CPP:238)
void Boss::_Collapse() {
    MARKFUNCTION(0x8001A90C);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
    if (frame >= 0x15 && combatFlag == 0) {
        combatFlag = 1;
        CSoundDirect::PlayTransient(0x2B, &pos, 0, 0);
    }

    if (anim->loopCount == 0) {
        return;
    }

    if ((flags & TF_ON_GROUND) == 0) {
        return;
    }

    if (health == 0) {
        SetActionState(AS_DEAD, 0);
        return;
    }

    ++stateTimer;
    if ((s16)humanoidDataID >= (s16)stateTimer) {
        return;
    }

    if ((m->modelFlags & 0x10u) != 0 && Player::s_player) {
        Player::s_player->SignalEnemyGetUp();
    }

    u32 requested = static_cast<u32>(commandBits);
    const bool wantsRisingAttack =
        ((requested >> GA_PUNCH) & 1u)
        || ((requested >> GA_KICK) & 1u)
        || ((requested >> GA_GRAB_FORWARD) & 1u)
        || ((requested >> GA_GRAB) & 1u)
        || ((requested >> GA_GRAB_HELD) & 1u)
        || ((requested >> GA_GRAB_FWD_HELD) & 1u);

    if (wantsRisingAttack) {
        requested = (requested & BOSS_RISING_ATTACK_CLEAR_MASK) | BOSS_RISING_ATTACK_REMAP_BIT;
        commandBits = static_cast<s32>(requested);
        field488 = FindSiblingWithRequestedCommand(
            static_cast<const FightingComboNode*>(defaultFightingSystem),
            requested);
    }

    SetActionState(AS_GET_UP, 0);
}

// PSX: _CrouchUp__4Boss (BOSS.CPP:304)
void Boss::_CrouchUp() {
    MARKFUNCTION(0x8001AAF4);

    flags |= TF_BIT1;

    if (((static_cast<u32>(commandBits) >> GA_GRAB_FORWARD) & 1u) != 0) {
        if (model) {
            Model* m = static_cast<Model*>(model);
            AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
            if (anim) {
                const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
                if (frame < 0x1C) {
                    SetActionState(AS_COMBAT_IDLE, 0);
                }
            }
        }
    }

    Humanoid::_CrouchUp();
}

// PSX: TestAndSetBackGrab__4Boss (BOSS.CPP:325)
s32 Boss::TestAndSetBackGrab() {
    MARKFUNCTION(0x8001AB68);
    return 0;
}

// PSX: __5ButchPC10tagLVector (BOSS.CPP:335)
Butch::Butch(const LVector* initialPos)
    : Boss(initialPos, AITypes::TT_BUTCH) {
    MARKFUNCTION(0x8001AB70);

    SetActionState(AS_INACTIVE_IDLE, 0);
    moveSpeed = 3000;
    comboCount = 1;
    field268 = 0;

    s_bossButch = this;
}

// PSX: _._5Butch (BOSS.CPP:350)
Butch::~Butch() {
    MARKFUNCTION(0x8001ABD0);
    s_bossButch = nullptr;
}

// PSX: SetActionState__5ButchUll (BOSS.CPP:355)
void Butch::SetActionState(u32 state, s32 param) {
    MARKFUNCTION(0x8001AC00);

    Boss::SetActionState(state, param);

    switch (state) {
    case AS_HIT_EXPLOSION:
        field344 = 0;
        stateDispatch = SD_BUTCH_STOMP;
        field348 = 8;
        if (model) {
            Model* m = static_cast<Model*>(model);
            m->SetAnim(0x42, param, 0, 0);
        }
        field268 = 0;
        actionState = static_cast<s32>(state);
        stateTimer = 0;
        break;

    case AS_HIT_ENVIRONMENT:
        field344 = 0;
        stateDispatch = SD_BUTCH_CHARGE;
        field348 = 8;
        if (model) {
            Model* m = static_cast<Model*>(model);
            m->SetAnim(0x62, param, 0, 0);
            AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
            if (anim) {
                anim->SetLoopType(ANIM_LOOP, 1);
            }
        }
        actionState = static_cast<s32>(state);
        stateTimer = 0;
        break;

    case AS_BUTCH_THROW_POT:
        field344 = 0;
        stateDispatch = SD_BUTCH_THROW_POT;
        field348 = 8;
        LOG("[ChefPot] Butch::SetActionState AS_BUTCH_THROW_POT param=%d cmd=0x%08X right=%p left=%p",
            param,
            static_cast<u32>(commandBits),
            rightHandObj,
            leftHandObj);
        actionState = static_cast<s32>(state);
        stateTimer = 0;
        break;

    default:
        break;
    }
}

// PSX: _Stomp__5Butch (BOSS.CPP:419)
void Butch::_Stomp() {
    MARKFUNCTION(0x8001AD30);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    if (anim->loopCount != 0) {
        field268 = 0;
        SetActionState(AS_STAND, 0);
        return;
    }

    const LVector stompPos = pos;
    Player* player = Player::s_player;
    const LVector playerPos = player ? player->pos : LVector{};

    field268 += 1;
    if (field268 < 0x2B) {
        flags |= TF_BIT1;
    }

    if (field268 != 0x2A || !player) {
        return;
    }

    Camera* camera = (g_display) ? g_display->GetCamera() : nullptr;
    if (camera) {
        camera->ShakeCamera(0x0F);
    }

    GEffect_Create(BUTCH_STOMP_EFFECT_HASH, &stompPos, nullptr, nullptr, 0, 0x19, 0);
    CSoundDirect::PlayTransient(0x28, &pos, 0, 0);
    CSoundDirect::PlayTransient(0x16, &pos, 0, 0);

    const s32 floorHeight = g_collisionSectors[0].GetWorldFloorHeight(playerPos, 1);
    if (playerPos.y - floorHeight < 0x81) {
        player->SetActionState(AS_COLLAPSE_STUN, 0);
        player->SubtractHitPoints(0x1C);
        Shock(SHOCK_16);
    }
}

// PSX: _Charge__5Butch (BOSS.CPP:491)
void Butch::_Charge() {
    MARKFUNCTION(0x8001AEF0);

    FaceAngleY(faceAngle, 1);

    const u32 cb = static_cast<u32>(commandBits);
    if (((cb >> GA_PUNCH) & 1u) != 0 || ((cb >> GA_KICK) & 1u) != 0) {
        SetActionState(AS_COMBAT_IDLE, 0);
        return;
    }

    const bool wantsPickupAction =
        (((cb >> GA_GRAB_HELD) & 1u) != 0)
        || (((cb >> GA_GRAB) & 1u) != 0)
        || (((cb >> GA_GRAB_FORWARD) & 1u) != 0);

    if (wantsPickupAction) {
        LOG("[ChefPot] _Charge pickupAction cmd=0x%08X right=%p left=%p",
            cb,
            rightHandObj,
            leftHandObj);

        if (rightHandObj != nullptr || leftHandObj != nullptr) {
            SetActionState(AS_THROW_PICKUP, 0);
        }
        else {
            SetActionState(AS_COMBAT_IDLE, 0);
        }
        return;
    }

    if (((cb >> GA_GUARD_RELEASE) & 1u) != 0) {
        SetActionState(AS_STAND, 0);
        return;
    }

    if (((cb >> GA_AI_DIVE_ROLL) & 1u) != 0) {
        SetActionState(AS_STRAFE, 0);
        return;
    }

    if (((cb >> 30) & 1u) != 0) {
        SetActionState(AS_HIT_EXPLOSION, 0);
        return;
    }

    if (static_cast<s32>(cb) < 0) {
        AddForce(moveSpeed, reinterpret_cast<const SVector*>(&orientation));
    }
}

// PSX: _ThrowPot__5Butch (BOSS.CPP:542)
void Butch::_ThrowPot() {
    MARKFUNCTION(0x8001B058);

    FaceAngleY(faceAngle, 0);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    if (anim->loopCount != 0) {
        if (rightHandObj) {
            LOG("[ChefPot] _ThrowPot looped owner=%p frame=%d held=%p name='%s' type=%u",
                this,
                static_cast<s16>((u32)anim->currentFrame >> 16),
                rightHandObj,
                rightHandObj->GetName(),
                rightHandObj->thingType);
        }

        SetActionState(AS_STAND, 0);
    }
}

// PSX: __7GrontarPC10tagLVector (BOSS.CPP:582)
Grontar::Grontar(const LVector* initialPos)
    : Boss(initialPos, AITypes::TT_GRONTAR) {
    MARKFUNCTION(0x8001B0BC);

    SetActionState(AS_INACTIVE_IDLE, 0);
    moveSpeed = 3000;
    comboCount = 1;

    s_bossGrontar = this;
}

// PSX: _._7Grontar (BOSS.CPP:594)
Grontar::~Grontar() {
    MARKFUNCTION(0x8001B118);
    s_bossGrontar = nullptr;
}

// PSX: SetActionState__7GrontarUll (BOSS.CPP:599)
void Grontar::SetActionState(u32 state, s32 param) {
    MARKFUNCTION(0x8001B148);

    Boss::SetActionState(state, param);

    if (state != AS_STRAFE) {
        return;
    }

    field344 = 0;
    // AI request 5 starts Grontar's combo selector; request 6 remains ordinary strafe.
    stateDispatch = HasAction(GA_DIVE_ROLL) ? SD_GRONTAR_COMBO : SD_STRAFE;
    field348 = 8;
    field488 = 0;

    if (model) {
        Model* m = static_cast<Model*>(model);
        // force=1: re-entering AS_STRAFE while anim 24 is already showing (held
        // at its last frame from a prior ANIM_RUN_TO_LAST cycle) must not skip
        // the reset, or AnimStructure::loopCount stays stuck nonzero and
        // Humanoid::_Taunt() dispatches off stale state instead of replaying it.
        m->SetAnim(24, param, 1, 0);

        if (m->drawableType == 2 && m->drawable) {
            DrawableSTree* drawable = static_cast<DrawableSTree*>(m->drawable);
            if ((drawable->mirrorFlags & 1u) != 0) {
                static_cast<SModel*>(m)->MirrorTree();
            }
        }
    }

    actionState = static_cast<s32>(state);
    stateTimer = 0;
}

void Grontar::ProcessAction() {
    if (stateDispatch == SD_GRONTAR_COMBO) {
        // Keep this route active while _DiveRoll waits for the following 8/9 request.
        _DiveRoll();
        return;
    }

    Humanoid::ProcessAction();
}

// PSX: _Stand__7Grontar (BOSS.CPP:652)
void Grontar::_Stand() {
    MARKFUNCTION(0x8001B20C);

    if (((static_cast<u32>(commandBits) >> 5) & 1u) != 0) {
        FaceAngleY(faceAngle, 0);
        SetActionState(AS_STRAFE, 0);
    }

    Humanoid::_Stand();
}

// PSX: _Run__7Grontar (BOSS.CPP:669)
void Grontar::_Run() {
    MARKFUNCTION(0x8001B274);

    if (((static_cast<u32>(commandBits) >> 5) & 1u) != 0) {
        FaceAngleY(faceAngle, 0);
        SetActionState(AS_STRAFE, 0);
    }

    Humanoid::_Run();
}

// PSX: _Taunt__7Grontar (BOSS.CPP:686)
void Grontar::_Taunt() {
    MARKFUNCTION(0x8001B2DC);

    if (((static_cast<u32>(commandBits) >> 5) & 1u) != 0) {
        FaceAngleY(faceAngle, 0);
        SetActionState(AS_STRAFE, 0);
    }

    Humanoid::_Taunt();
}

// PSX: _Straif__7Grontar (BOSS.CPP:703)
void Grontar::_Straif() {
    MARKFUNCTION(0x8001B344);

    if (((static_cast<u32>(commandBits) >> 5) & 1u) != 0) {
        FaceAngleY(faceAngle, 0);
        SetActionState(AS_STRAFE, 0);
    }

    Humanoid::_Straif();
}

// PSX: _DiveRoll__7Grontar (BOSS.CPP:720)
void Grontar::_DiveRoll() {
    MARKFUNCTION(0x8001B3AC);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);

    if (anim->loopCount != 0) {
        SetActionState(AS_STAND, 0);
        return;
    }

    if (field488 == 0) {
        const FightingComboNode* root =
            ResolveFightingNodeAddressConst(GRONTAR_DIVE_ROLL_KICK_ROOT_ADDRESS);
        field488 = FindSiblingWithRequestedCommand(root, static_cast<u32>(commandBits));
        return;
    }

    const FightingComboNode* nextNode =
        ResolveFightingNodeAddressConst(static_cast<u32>(field488));
    if (!nextNode) {
        field488 = 0;
        return;
    }

    if (frame < nextNode->field05) {
        return;
    }

    SetHumanoidTarget(FindFoe(GRONTAR_DIVE_ROLL_FIGHT_DISTANCE, GRONTAR_DIVE_ROLL_FIGHT_HALF_ANGLE, 0));
    SetCurrentFightingNode();
}

// PSX: _GotHitHigh__7Grontar (BOSS.CPP:781)
void Grontar::_GotHitHigh() {
    MARKFUNCTION(0x8001B47C);

    Humanoid::_GotHitHigh();

    const s32 randomRoll = static_cast<s32>(rmRangedRandom(100));
    if (((static_cast<u32>(commandBits) >> 15) & 1u) != 0
        && randomRoll < GRONTAR_HIT_THROW_BREAKOUT) {
        SetActionState(AS_COMBAT_IDLE, 0);
    }
}

// PSX: _GotHitMed__7Grontar (BOSS.CPP:800)
void Grontar::_GotHitMed() {
    MARKFUNCTION(0x8001B4F0);

    Humanoid::_GotHitMed();

    const s32 randomRoll = static_cast<s32>(rmRangedRandom(100));
    if (((static_cast<u32>(commandBits) >> 15) & 1u) != 0
        && randomRoll < GRONTAR_HIT_THROW_BREAKOUT) {
        SetActionState(AS_COMBAT_IDLE, 0);
    }
}

// PSX: FindFoe__7GrontarUlli (BOSS.CPP:819)
Humanoid* Grontar::FindFoe(u32 range, s32 param, s32 immediate) {
    MARKFUNCTION(0x8001B564);

    u32 searchRange = range;
    s32 halfAngle = param;

    if (((static_cast<u32>(commandBits) >> 7) & 1u) != 0) {
        searchRange = GrontarThrowDistance;
        halfAngle = GrontarThrowHalfAngle;
    }

    return Humanoid::FindFoe(searchRange, halfAngle, immediate);
}

// PSX: GetTargetingFrame__7GrontarRC18StrikeFightingMove (BOSS.CPP:831)
s32 Grontar::GetTargetingFrame(const PsxFightingMoveRaw* move) const {
    MARKFUNCTION(0x8001B5A0);

    if (!move) {
        return GRONTAR_TARGET_TRACK_DEFAULT;
    }

    switch (move->anim) {
    case 25:
    case 26:
        return GRONTAR_TARGET_TRACK_HORIZONTAL_SWING;
    case 63:
        return GRONTAR_TARGET_TRACK_OVERHEAD_SWING;
    case 54:
    case 58:
        return 0;
    default:
        return GRONTAR_TARGET_TRACK_DEFAULT;
    }
}

// PSX: __5DantePC10tagLVector (BOSS.CPP:870)
Dante::Dante(const LVector* initialPos)
    : Boss(initialPos, AITypes::TT_DANTE) {
    MARKFUNCTION(0x8001B60C);

    SetActionState(AS_INACTIVE_IDLE, 0);
    moveSpeed = 3000;
    comboCount = 1;

    missilePaths = nullptr;
    missilePathCount = 0;

    missileTrail0 = new Trails(2);
    missileTrail1 = new Trails(2);
    missileTrail2 = new Trails(2);
    missileTrail3 = new Trails(2);
    missileTrail4 = new Trails(2);
    missileTrail5 = new Trails(0x20);

    field290 = 0;
    field294 = 0;
    missileFrameIndex = 0;
    missilePatternIndex = 0;
    missileRecoveryCounter = 0;
    targetMissilePrimed = 0;

    s_bossDante = this;
}

// PSX: _._5Dante (BOSS.CPP:905)
Dante::~Dante() {
    MARKFUNCTION(0x8001B718);

    s_bossDante = nullptr;

    if (missilePaths != nullptr) {
        for (s32 i = 0; i < missilePathCount; i++) {
            delete missilePaths[i];
            missilePaths[i] = nullptr;
        }

        delete[] missilePaths;
        missilePaths = nullptr;
    }

    delete missileTrail0;
    missileTrail0 = nullptr;

    delete missileTrail1;
    missileTrail1 = nullptr;

    delete missileTrail2;
    missileTrail2 = nullptr;

    delete missileTrail3;
    missileTrail3 = nullptr;

    delete missileTrail4;
    missileTrail4 = nullptr;

    delete missileTrail5;
    missileTrail5 = nullptr;
}

// PSX: AnalyzeMesh__5DanteP6DBRoot (BOSS.CPP:954)
void Dante::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8001B8D0);

    Humanoid::AnalyzeMesh(root);

    if (!root || !g_database) {
        return;
    }

    root->FindAttrib(4);

    missilePaths = new LinearPath*[8];

    DBPath* path = g_database->FindPath(DANTE_PATH_PATTERN1_NAME);
    if (path != nullptr) {
        LinearPath* linearPath = new LinearPath();
        linearPath->Init(path);
        missilePaths[missilePathCount] = linearPath;
        missilePathCount += 1;
    }

    path = g_database->FindPath(DANTE_PATH_PATTERN2_NAME);
    if (path != nullptr) {
        LinearPath* linearPath = new LinearPath();
        linearPath->Init(path);
        missilePaths[missilePathCount] = linearPath;
        missilePathCount += 1;
    }
}

// PSX: Think__5Dante (BOSS.CPP:1008)
void Dante::Think() {
    MARKFUNCTION(0x8001BA80);
    Humanoid::Think();
}

// PSX: SetActionState__5DanteUll (BOSS.CPP:1013)
void Dante::SetActionState(u32 state, s32 param) {
    MARKFUNCTION(0x8001BAA0);

    Boss::SetActionState(state, param);

    switch (state) {
    case AS_MISSILE_PREPARE:
        field344 = 0;
        stateDispatch = SD_DANTE_MISSILE_PREPARE;
        field348 = 8;
        if (model) {
            Model* m = static_cast<Model*>(model);
            m->SetAnim(0x74, param, 0, 0);
        }
        actionState = static_cast<s32>(state);
        break;

    case AS_MISSILE_ATTACK:
        field344 = 0;
        stateDispatch = SD_DANTE_MISSILE_ATTACK;
        field348 = 8;
        if (model) {
            Model* m = static_cast<Model*>(model);
            m->SetAnim(0x5C, param, 0, 0);
        }
        actionState = static_cast<s32>(state);
        break;

    case AS_TARGET_MISSILE_ATTACK:
        field344 = 0;
        stateDispatch = SD_DANTE_TARGET_MISSILE_ATTACK;
        field348 = 8;
        if (model) {
            Model* m = static_cast<Model*>(model);
            m->SetAnim(0x5C, param, 0, 0);
        }
        missileRecoveryCounter = 0;
        actionState = static_cast<s32>(state);
        break;

    default:
        break;
    }
}

// PSX: _Stand__5Dante (BOSS.CPP:1062)
void Dante::_Stand() {
    MARKFUNCTION(0x8001BBA4);

    const u32 cb = static_cast<u32>(commandBits);
    if (((cb >> 29) & 1u) != 0) {
        SetActionState(AS_MISSILE_PREPARE, 0);
    }
    else if (((cb >> 28) & 1u) != 0) {
        SetActionState(AS_TARGET_MISSILE_ATTACK, 0);
    }

    Humanoid::_Stand();
}

// PSX: _Taunt__5Dante (BOSS.CPP:1081)
void Dante::_Taunt() {
    MARKFUNCTION(0x8001BC18);

    if (((static_cast<u32>(commandBits) >> 29) & 1u) != 0) {
        SetActionState(AS_MISSILE_PREPARE, 0);
    }

    Humanoid::_Taunt();
}

// PSX: _GotHitHigh__5Dante (BOSS.CPP:1095)
void Dante::_GotHitHigh() {
    MARKFUNCTION(0x8001BC70);

    Humanoid::_GotHitHigh();

    if (((static_cast<u32>(commandBits) >> 15) & 1u) != 0) {
        SetActionState(AS_COMBAT_IDLE, 0);
    }
}

// PSX: _GotHitMed__5Dante (BOSS.CPP:1108)
void Dante::_GotHitMed() {
    MARKFUNCTION(0x8001BCC4);

    Humanoid::_GotHitMed();

    if (((static_cast<u32>(commandBits) >> 15) & 1u) != 0) {
        SetActionState(AS_COMBAT_IDLE, 0);
    }
}

// PSX: _GotHitFreeForm__5Dante (BOSS.CPP:1121)
void Dante::_GotHitFreeForm() {
    MARKFUNCTION(0x8001BD18);
    Humanoid::GotHitFreeForm();
}

// PSX: _ThrowFreeFall__5Dante (BOSS.CPP:1127)
s32 Dante::_ThrowFreeFall() {
    MARKFUNCTION(0x8001BD38);
    return Humanoid::ThrowFreeFall();
}

// PSX: _MissilePrepare__5Dante (BOSS.CPP:1132)
void Dante::_MissilePrepare() {
    MARKFUNCTION(0x8001BD58);

    FaceAngleY(faceAngle, 0);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    if (anim->loopCount != 0) {
        missilePatternIndex = (static_cast<s32>(rmRangedRandom(100)) < 0x32) ? 0 : 1;
        missileFrameIndex = 0;
        SetActionState(AS_MISSILE_ATTACK, 0);
    }
}

// PSX: _MissileAttack__5Dante (BOSS.CPP:1113)
void Dante::_MissileAttack() {
    MARKFUNCTION(0x8001BDE4);

    FaceAngleY(faceAngle, 0);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    const s32 endFrame = static_cast<s16>((u32)anim->endFrame >> 16);
    if (endFrame < s_danteMissileAttackFrameThreshold) {
        s_danteMissileAttackFrameThreshold = endFrame;
    }

    const s32 currentFrame = static_cast<s16>((u32)anim->currentFrame >> 16);
    if (currentFrame < s_danteMissileAttackFrameThreshold) {
        return;
    }

    CSoundDirect::PlayTransient(0xD5, nullptr, 3, 0);
    CSoundDirect::PlayTransient(0xD6, nullptr, 7, 0);

    ASSERT(missilePaths != nullptr);
    LinearPath* selectedPath = missilePaths[missilePatternIndex];
    ASSERT(selectedPath != nullptr);
    ASSERT(selectedPath->positions != nullptr);

    bool shouldHitPlayer = false;

    for (s32 groupIndex = 0; groupIndex < DANTE_MISSILE_GROUP_SIZE; groupIndex++) {
        if (missileFrameIndex >= selectedPath->numPoints) {
            break;
        }

        const LVector pathPos = selectedPath->positions[missileFrameIndex];

        const u32 launcherHash = p3dHash(DANTE_MISSILE_LAUNCHER_NAME);
        WorldPointNode* launcherPoint = WorldPoints_GetNISPoint(launcherHash);
        ASSERT(launcherPoint != nullptr);
        const LVector launcherPos = launcherPoint->pos;
        const LVector up = { 0, 1, 0 };

        LVector cross = CrossFixed16(launcherPos, up);
        rmV3Normalize(&cross, &cross);

        LVector launcherOffsetPos = {
            launcherPos.x + fixmul16(cross.x, DANTE_MISSILE_TRAIL_START_OFFSET),
            launcherPos.y + fixmul16(cross.y, DANTE_MISSILE_TRAIL_START_OFFSET),
            launcherPos.z + fixmul16(cross.z, DANTE_MISSILE_TRAIL_START_OFFSET),
        };

        LVector pathOffsetPos = {
            pathPos.x + fixmul16(cross.x, DANTE_MISSILE_TRAIL_END_OFFSET),
            pathPos.y + fixmul16(cross.y, DANTE_MISSILE_TRAIL_END_OFFSET),
            pathPos.z + fixmul16(cross.z, DANTE_MISSILE_TRAIL_END_OFFSET),
        };

        switch (groupIndex) {
        case 0:
            if (missileTrail0) {
                missileTrail0->Add(&launcherPos, &launcherPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
                missileTrail0->Add(&pathPos, &pathOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
            }
            break;
        case 1:
            if (missileTrail1) {
                missileTrail1->Add(&launcherPos, &launcherOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
                missileTrail1->Add(&pathPos, &pathOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
            }
            break;
        case 2:
            if (missileTrail2) {
                missileTrail2->Add(&launcherPos, &launcherOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
                missileTrail2->Add(&pathPos, &pathOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
            }
            break;
        case 3:
            if (missileTrail3) {
                missileTrail3->Add(&launcherPos, &launcherOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
                missileTrail3->Add(&pathPos, &pathOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
            }
            break;
        case 4:
            if (missileTrail4) {
                missileTrail4->Add(&launcherPos, &launcherOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
                missileTrail4->Add(&pathPos, &pathOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
            }
            break;
        default:
            break;
        }

        GEffect_Create(DANTE_MISSILE_EFFECT_HASH, &pathPos, nullptr, nullptr, 0, 2, 0);

        Player* player = Player::s_player;
        if (player != nullptr) {
            const s32 distance = player->DistanceFromPoint(pathPos);
            if (!IsDanteMissileImmuneActionState(player->actionState)
                && distance < DANTE_MISSILE_DAMAGE_RADIUS) {
                shouldHitPlayer = true;
            }
        }

        missileFrameIndex += 1;
    }

    Player* player = Player::s_player;
    if (shouldHitPlayer && player != nullptr) {
        player->SetActionState(AS_COLLAPSE_STUN, 0);
        player->SubtractHitPoints(DANTE_MISSILE_DAMAGE);
        Shock(SHOCK_17);
    }

    anim->currentFrame = 0;

    if (missileFrameIndex >= selectedPath->numPoints) {
        missileFrameIndex = 0;
        SetActionState(AS_STAND, 0);
    }
}

// PSX: _TargetMissileAttack__5Dante (BOSS.CPP:1232)
void Dante::_TargetMissileAttack() {
    MARKFUNCTION(0x8001C358);

    FaceAngleY(faceAngle, 0);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    if (missileRecoveryCounter != 0) {
        if (missileRecoveryCounter < DANTE_RECOVERY_TIME) {
            missileRecoveryCounter += 1;
            return;
        }

        SetActionState(AS_STAND, 0);
        missileRecoveryCounter = 0;
        return;
    }

    if (targetMissilePrimed == 0) {
        Player* player = Player::s_player;
        if (player) {
            targetMissilePos = player->pos;
        }
        targetMissilePrimed = 1;
    }

    const s32 endFrame = static_cast<s16>((u32)anim->endFrame >> 16);
    if (endFrame < s_danteTargetMissileFrameThreshold) {
        s_danteTargetMissileFrameThreshold = endFrame;
    }

    const s32 currentFrame = static_cast<s16>((u32)anim->currentFrame >> 16);
    if (currentFrame < s_danteTargetMissileFrameThreshold) {
        return;
    }

    CSoundDirect::PlayTransient(0xD5, nullptr, 3, 0);
    CSoundDirect::PlayTransient(0xD6, nullptr, 7, 0);

    const u32 launcherHash = p3dHash(DANTE_MISSILE_LAUNCHER_NAME);
    WorldPointNode* launcherPoint = WorldPoints_GetNISPoint(launcherHash);
    ASSERT(launcherPoint != nullptr);
    const LVector launcherPos = launcherPoint->pos;
    const LVector up = { 0, 1, 0 };

    LVector cross = CrossFixed16(launcherPos, up);
    rmV3Normalize(&cross, &cross);

    LVector launcherOffsetPos = {
        launcherPos.x + fixmul16(cross.x, DANTE_TARGET_MISSILE_TRAIL_START_OFFSET),
        launcherPos.y + fixmul16(cross.y, DANTE_TARGET_MISSILE_TRAIL_START_OFFSET),
        launcherPos.z + fixmul16(cross.z, DANTE_TARGET_MISSILE_TRAIL_START_OFFSET),
    };

    LVector targetOffsetPos = {
        targetMissilePos.x + fixmul16(cross.x, DANTE_TARGET_MISSILE_TRAIL_END_OFFSET),
        targetMissilePos.y + fixmul16(cross.y, DANTE_TARGET_MISSILE_TRAIL_END_OFFSET),
        targetMissilePos.z + fixmul16(cross.z, DANTE_TARGET_MISSILE_TRAIL_END_OFFSET),
    };

    if (missileTrail0) {
        missileTrail0->Add(&launcherPos, &launcherOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
        missileTrail0->Add(&targetMissilePos, &targetOffsetPos, DANTE_MISSILE_TRAIL_COLOR, DANTE_MISSILE_TRAIL_STEPS, nullptr);
    }

    GEffect_Create(DANTE_TARGET_MISSILE_EFFECT_HASH, &targetMissilePos, nullptr, nullptr, 0, 2, 0);

    Player* player = Player::s_player;
    if (player && player->DistanceFromPoint(targetMissilePos) < DANTE_MISSILE_DAMAGE_RADIUS) {
        player->SetActionState(AS_COLLAPSE_STUN, 0);
        player->SubtractHitPoints(DANTE_MISSILE_DAMAGE);
    }

    targetMissilePrimed = 0;
    missileRecoveryCounter = 1;

    m->SetAnim(0x5B, 5, 0, 0);
}

// PSX: LoadCombatDialog__5Dante (BOSS.CPP:1307)
s32 Dante::LoadCombatDialog() {
    MARKFUNCTION(0x8001C70C);

    Humanoid::LoadCombatDialog();
    return 1;
}

// PSX: PlayCombatKnockDownDialog__5Dante15DamageTypesTags (BOSS.CPP:1312)
void Dante::PlayCombatKnockDownDialog(s32 damageType) {
    MARKFUNCTION(0x8001C72C);

    switch (damageType) {
    case 2:
    case 3:
    case 5:
    case 11:
    case 12:
        PlayDialog(static_cast<u32>(soundParam), DANTE_KNOCKDOWN_DIALOG_PRIORITY);
        break;
    default:
        break;
    }
}

// PSX: GetTargetingFrame__5DanteRC18StrikeFightingMove (BOSS.CPP:1554)
s32 Dante::GetTargetingFrame(const PsxFightingMoveRaw* /*move*/) const {
    MARKFUNCTION(0x8001CA10);
    return DANTE_TARGETING_FRAME;
}

// PSX: PlayCombatThrowDialog__5Dante (BOSS.CPP:1559)
void Dante::PlayCombatThrowDialog() {
    MARKFUNCTION(0x8001CA18);
    PlayDialog(static_cast<u32>(soundParam), DANTE_KNOCKDOWN_DIALOG_PRIORITY);
}

// PSX: __4PaulPC10tagLVector (BOSS.CPP:1746)
Paul::Paul(const LVector* initialPos)
    : Boss(initialPos, AITypes::TT_PAUL) {
    MARKFUNCTION(0x8001C77C);

    SetActionState(AS_INACTIVE_IDLE, 0);
    moveSpeed = 3000;
    comboCount = 1;

    s_bossPaul = this;
}

// PSX: _._4Paul (BOSS.CPP:1758)
Paul::~Paul() {
    MARKFUNCTION(0x8001C7D8);
    s_bossPaul = nullptr;
}

// PSX: _Straif__4Paul (BOSS.CPP:1763)
void Paul::_Straif() {
    MARKFUNCTION(0x8001C808);

    flags |= TF_BIT1;
    Humanoid::_Straif();
}

// PSX: _GotHitHigh__4Paul (BOSS.CPP:1774)
void Paul::_GotHitHigh() {
    MARKFUNCTION(0x8001C834);

    Humanoid::_GotHitHigh();

    if (((static_cast<u32>(commandBits) >> 6) & 1u) != 0) {
        SetActionState(AS_STRAFE, 0);
    }
}

// PSX: _GotHitMed__4Paul (BOSS.CPP:1789)
void Paul::_GotHitMed() {
    MARKFUNCTION(0x8001C888);

    Humanoid::_GotHitMed();

    if (((static_cast<u32>(commandBits) >> 6) & 1u) != 0) {
        SetActionState(AS_STRAFE, 0);
    }
}

// PSX: SetActionState__4PaulUll (BOSS.CPP:1804)
void Paul::SetActionState(u32 state, s32 param) {
    MARKFUNCTION(0x8001C8DC);
    Boss::SetActionState(state, param);
}

// PSX: __5OscarPC10tagLVector (BOSS.CPP:1810)
Oscar::Oscar(const LVector* initialPos)
    : Boss(initialPos, AITypes::TT_OSCAR) {
    MARKFUNCTION(0x8001C8FC);

    SetActionState(AS_INACTIVE_IDLE, 0);
    moveSpeed = 3000;
    faceAngleData = s_oscarStraif;
    comboCount = 1;

    s_bossOscar = this;
}

// PSX: _._5Oscar (BOSS.CPP:1825)
Oscar::~Oscar() {
    MARKFUNCTION(0x8001C964);
    s_bossOscar = nullptr;
}

// PSX: SetActionState__5OscarUll (BOSS.CPP:1830)
void Oscar::SetActionState(u32 state, s32 param) {
    MARKFUNCTION(0x8001C994);
    Boss::SetActionState(state, param);
}

// PSX: _Straif__5Oscar (BOSS.CPP:1835)
void Oscar::_Straif() {
    MARKFUNCTION(0x8001C9B4);

    if (model) {
        Model* m = static_cast<Model*>(model);
        AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
        if (anim && anim->animEnum == OSCAR_STRAIF_PHASE_GUARD_ANIM) {
            if (GetStraifPhase() == 0) {
                return;
            }
        }
    }

    Humanoid::_Straif();
}

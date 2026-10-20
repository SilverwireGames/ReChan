#include "ai/humanoid.h"
#include "ai/activezn.h"
#include "ai/humndata.h"
#include "ai/obstacle.h"
#include "ai/platform.h"
#include "ai/pickup.h"
#include "ai/player.h"
#include "ai/table.h"
#include "ai/colfight.h"
#include "gen/common.h"
#include "gen/database.h"
#include "gen/model.h"
#include "gen/ai.h"
#include "gen/animmat.h"
#include "gen/animstruct.h"
#include "gen/director.h"
#include "gen/game.h"
#include "gen/control.h"
#include "gen/colsect.h"
#include "gen/fxp.h"
#include "gen/scoremgr.h"
#include "gen/time.h"
#include "gen/world.h"
#include "gen/geffect.h"
#include "gen/trail.h"
#include "gen/psxmath_helpers.h"
#include "fe/hud.h"
#include "snd/rsevent.h"
#include "snd/hmndsnd.h"
#include "snd/sndfact.h"
#include "p3d/hash.h"
#include "p3d/p3dmath.h"
#include "p3d/skeleton.h"
#include "pc/debugui.h"
#include "pc/log.h"
#if NEW_CHEATS
#include "extra/cheats.h"
#endif

#include "extra/shadowcsm.h"

static constexpr s32 HUMANOID_ANIM_RUN = 2;
static constexpr s32 HUMANOID_ANIM_DIVE_ROLL = 90;
static constexpr s32 HUMANOID_ANIM_JUMP = 39;
static constexpr s32 HUMANOID_ANIM_RUN_LAND = 37;
static constexpr s32 HUMANOID_ANIM_HARD_LAND = 40;
static constexpr s32 HUMANOID_ANIM_FLIP_LAND = 295;
static constexpr s16 HUMANOID_JUMP_KICK_ENTRY_FRAME = 2;
static constexpr s32 HUMANOID_ANIM_LEDGE_PULLUP = 30;
static constexpr s32 HUMANOID_ANIM_LEDGE_LATCH = 31;
static constexpr s16 DIVE_ROLL_FORCE_END_FRAME = 14;
static constexpr s32 DIVE_ROLL_FORCE = 0xDAC;
static constexpr s16 DIVE_ROLL_JUMP_PAUSE_FRAME = 0xB;
static constexpr s16 DIVE_ROLL_RUN_STRAFE_FRAME = 0xD;
static constexpr s32 HEAD_RISE = 0xA5;
static constexpr s32 HUMANOID_LEDGE_LATCH_MIN_Y_RISE = 0x40;
static constexpr s32 HUMANOID_DIVE_ROLL_Y_RISE = 0x180;
static constexpr s32 LEDGE_TRACE_DISTANCE = 384;
static constexpr s32 LEDGE_TRACE_MIN_Y = 500;
static constexpr s32 LEDGE_TRACE_MAX_Y = 750;
static constexpr s32 LEDGE_TRACE_CLEARANCE = 1024;
static constexpr s32 LEDGE_FLOOR_MIN_HEIGHT = 1022;
static constexpr s32 ATTACKER_WEAPON_RADIUS = 0x40;
static constexpr s32 TARGET_TRACK_MAX_FRAME = 5;
static constexpr s32 DIVEROLLKICK_TARGET_TRACK_MAX_FRAME = 3;
static constexpr s32 DIVEROLLPUNCH_TARGET_TRACK_MAX_FRAME = 4;
static constexpr s32 CHARGEPUNCH_TARGET_TRACK_MAX_FRAME = 10;
static constexpr u32 RISING_ATTACK_REMAP_BIT = 0x00800000u;
static constexpr u32 RISING_ATTACK_CLEAR_MASK =
    ~( (1u << GA_PUNCH)
    | (1u << GA_KICK)
    | (1u << GA_GRAB)
    | (1u << GA_GRAB_FORWARD)
    | (1u << GA_GRAB_HELD)
    | (1u << GA_GRAB_FWD_HELD));
static constexpr s32 WALL_KICK_TRACE_DISTANCE = 256;
static constexpr s32 WALL_KICK_COLLISION_RADIUS = 16;
static constexpr s32 WALL_KICK_COLLISION_HEIGHT = 500;
static constexpr u32 WALL_KICK_MIN_WALL_HEIGHT = 500;
static constexpr s16 WALL_KICK_ANGLE_THRESHOLD_DEGREES = 160;
static constexpr u32 FIGHT_DISTANCE = 775;
static constexpr u32 DIVE_ROLL_FIGHT_DISTANCE = 750;
static constexpr s32 FIGHT_HALF_ANGLE = 10922;
static constexpr u32 THROW_LATCH_DISTANCE = 775;
static constexpr s32 THROW_LATCH_HALF_ANGLE = 7281;
static constexpr s32 THROW_LATCH_VERTICAL_DELTA_MAX = 200;
static constexpr u16 GRAB_STRENGTH = 60;
static constexpr s32 GRAB_HEIGHT = 384;
static constexpr s16 BACK_GRAB_RELEASE_SPEED_FRAME = 13;
static constexpr s32 BACK_GRAB_RECOVERY_START_FRAME = 7;
static constexpr s32 BACK_GRAB_RECEIVE_PRE_LATCH_FRAMES = 31;
static constexpr s32 BACK_GRAB_ATTACH_Z = 0x1C2;
// PSX gp+0x738 (0x800DD084), passed as Pickup::Release forceMag in _Throw directional release.
// Symbol dump labels this as THROW_PROJECTILE_VELOCITY with default value 0x59D8.
static s32 s_throwPickupReleaseForce = 0x59D8;
// PSX gp+0x73C (0x800DD088), release-frame threshold for _TableThrow.
static s32 s_tableThrowReleaseFrame = 0xD;
static u32 s_humanoidSuitTraceBudget = 64;
static u32 s_nonSTreeHumanoidModelBudget = 16;
static constexpr s32 DROP_PICKUP_DAMAGE_THRESHOLD = 14;
static constexpr s32 BACK_GRAB_MIN_RELATIVE_ANGLE = 5461;
static constexpr u32 BACK_GRAB_RELATIVE_ANGLE_RANGE = 0xD8E3;
static constexpr u16 HUMANOID_STUN_DURATION = 66;
static constexpr s32 HUMANOID_ANIM_DEAD = 17;
static constexpr s32 HUMANOID_ANIM_CRUSHED = 84;
static constexpr u32 HUMANOID_STUN_EFFECT_HASH = 0x004CC954u;
static constexpr u32 HUMANOID_LAND_IMPACT_EFFECT_HASH = 0x0B0E21C4u;
static constexpr u32 HUMANOID_STRIKE_IMPACT_EFFECT_HASH = 0x03E24164u;
// PSX 0x800DD0E0..0x800DD0E2: SPL_PLAYER_TRAIL_{R,G,B}
static u8 s_splPlayerTrailR = 0x7Fu;
static u8 s_splPlayerTrailG = 0x7Fu;
static u8 s_splPlayerTrailB = 0x7Fu;
// PSX 0x800DD0E4: SPL_PLAYER_TRAIL_FRAMES
static s32 s_splPlayerTrailFrames = 0x10;
static constexpr u32 PLAYER_DIVE_ROLL_KICK_ROOT_ADDRESS = 0x800CEE44u;
static constexpr u32 PLAYER_BACK_GRAB_KICK_ROOT_ADDRESS = 0x800CEE6Cu;
// PSX gp+1948, data block default at 0x800DD0E8.
static s32 freeFormFightingMode = 0;

static constexpr u32 NON_BOSS_TAUNT_COOLDOWN_FRAMES = 90; // ~3s at 30fps; approximation

#if HIGH_FPS_PLAY_PRESENTATION
static constexpr s32 MAX_HUMANOID_RENDER_SMOOTH_STATES = 256;

// Draw() lerps render position/orientation/anim-frame between the previous
// and current logic tick's values. Any code that writes pos/orientation
// directly instead of through normal velocity integration (ledge latch,
// Teleport, root-motion realignment, etc.) must call
// Humanoid::ResetRenderInterpolation() right after, or the lerp will slide
// through the discontinuity and the model will visibly snap through empty
// space for a frame or two.
struct HumanoidRenderSmoothState {
    Humanoid* owner = nullptr;
    bool initialized = false;
    LVector prevPos = {};
    LVector curPos = {};
    LVector prevOrient = {};
    LVector curOrient = {};
    bool animInitialized = false;
    s32 animEnum = -1;
    s32 animLoopType = ANIM_LOOP;
    s32 prevAnimFrame = 0;
    s32 curAnimFrame = 0;
};

static HumanoidRenderSmoothState s_humanoidRenderSmoothStates[MAX_HUMANOID_RENDER_SMOOTH_STATES] = {};

static HumanoidRenderSmoothState* FindHumanoidRenderSmoothState(Humanoid* humanoid) {
    if (!humanoid) {
        return nullptr;
    }

    HumanoidRenderSmoothState* freeState = nullptr;
    for (s32 i = 0; i < MAX_HUMANOID_RENDER_SMOOTH_STATES; i++) {
        HumanoidRenderSmoothState* state = &s_humanoidRenderSmoothStates[i];
        if (state->owner == humanoid) {
            return state;
        }
        if (!freeState && state->owner == nullptr) {
            freeState = state;
        }
    }

    if (!freeState) {
        return nullptr;
    }

    freeState->owner = humanoid;
    freeState->initialized = false;
    freeState->prevPos = {};
    freeState->curPos = {};
    freeState->prevOrient = {};
    freeState->curOrient = {};
    freeState->animInitialized = false;
    freeState->animEnum = -1;
    freeState->prevAnimFrame = 0;
    freeState->curAnimFrame = 0;
    return freeState;
}

static void ClearHumanoidRenderSmoothState(Humanoid* humanoid) {
    if (!humanoid) {
        return;
    }

    for (s32 i = 0; i < MAX_HUMANOID_RENDER_SMOOTH_STATES; i++) {
        HumanoidRenderSmoothState* state = &s_humanoidRenderSmoothStates[i];
        if (state->owner == humanoid) {
            state->owner = nullptr;
            state->initialized = false;
            state->prevPos = {};
            state->curPos = {};
            state->prevOrient = {};
            state->curOrient = {};
            state->animInitialized = false;
            state->animEnum = -1;
            state->prevAnimFrame = 0;
            state->curAnimFrame = 0;
            return;
        }
    }
}

static s32 WrapLoopFrameReal(s32 frameReal, s32 endFrame) {
    const s32 range = endFrame + FIX16_ONE;
    if (range <= 0) {
        return frameReal;
    }

    while (frameReal < 0) {
        frameReal += range;
    }
    while (frameReal > endFrame) {
        frameReal -= range;
    }
    if (frameReal < 0) {
        frameReal = endFrame;
    }

    return frameReal;
}

static s32 BuildInterpolatedAnimFrameReal(const AnimStructure* anim, s32 prevFrame, s32 curFrame,
    f32 alpha) {
    if (!anim) {
        return curFrame;
    }

    s32 frameStep = curFrame - prevFrame;

    const bool isLoopingAnim = anim->loopTypeField == ANIM_LOOP
        || anim->loopTypeField == ANIM_LOOP_REVERSE
        || anim->loopTypeField == ANIM_BLEND;

    if (isLoopingAnim) {
        const s32 range = anim->endFrame + FIX16_ONE;
        if (range > 0) {
            const s32 halfRange = range >> 1;
            if (frameStep > halfRange) {
                frameStep -= range;
            }
            else if (frameStep < -halfRange) {
                frameStep += range;
            }
        }
    }

    s32 frameReal = prevFrame + (s32)((f32)frameStep * alpha);

    if (isLoopingAnim) {
        frameReal = WrapLoopFrameReal(frameReal, anim->endFrame);
    }
    else {
        if (frameReal < 0) {
            frameReal = 0;
        }
        if (frameReal > anim->endFrame) {
            frameReal = anim->endFrame;
        }
    }

    return frameReal;
}

static void UpdateHumanoidRenderAnimPose(HumanoidModel* model, HumanoidRenderSmoothState* smoothState,
    f32 alpha) {
    if (!model || !smoothState || !smoothState->animInitialized) {
        return;
    }

    AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
    if (!anim || !anim->flip || !anim->animation) {
        return;
    }

    if (alpha < 0.0f) {
        alpha = 0.0f;
    }

    if (alpha > 1.0f) {
        alpha = 1.0f;
    }

    if (anim->sequence) {
        s32 prevLocalFrame = 0;
        s32 curLocalFrame = 0;
        TransformAnim* prevPart = anim->sequence->ResolvePart(smoothState->prevAnimFrame >> 16, prevLocalFrame);
        TransformAnim* curPart = anim->sequence->ResolvePart(smoothState->curAnimFrame >> 16, curLocalFrame);

        TransformAnim* part = curPart ? curPart : prevPart;
        if (!part) {
            return;
        }

        const s32 curFrameReal = curLocalFrame << 16 | (smoothState->curAnimFrame & 0xFFFF);

        s32 frameReal;
        if (prevPart == curPart) {
            const s32 prevFrameReal = prevLocalFrame << 16 | (smoothState->prevAnimFrame & 0xFFFF);
            frameReal = prevFrameReal + (s32)((f32)(curFrameReal - prevFrameReal) * alpha);
        }
        else {
            frameReal = curFrameReal;
        }

        const s32 maxFrameReal = (part->numFrames > 0) ? ((part->numFrames - 1) << 16) : 0;
        if (frameReal < 0) {
            frameReal = 0;
        }
        if (frameReal > maxFrameReal) {
            frameReal = maxFrameReal;
        }

        if (part != anim->flip->anim) {
            anim->flip->anim = part;
            anim->flip->dirty = 1;
        }
        anim->flip->SetFrameReal(frameReal);
        anim->flip->UpdateJoints();
        return;
    }

    const s32 frameReal = BuildInterpolatedAnimFrameReal(
        anim,
        smoothState->prevAnimFrame,
        smoothState->curAnimFrame,
        alpha);

    anim->ApplyFlipPose(frameReal);
}

struct HumanoidRenderAnimPoseBackup {
    AnimStructure* animStructure = nullptr;
    TransformFlip* flip = nullptr;
    TransformAnim* anim = nullptr;
    s32 frameReal = 0;
    bool valid = false;
};

static HumanoidRenderAnimPoseBackup ApplyHumanoidRenderAnimPose(
    HumanoidModel* model,
    HumanoidRenderSmoothState* smoothState,
    f32 alpha) {
    HumanoidRenderAnimPoseBackup backup = {};
    if (!model || !smoothState || !smoothState->animInitialized) {
        return backup;
    }

    AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
    if (!anim || !anim->flip || !anim->animation) {
        return backup;
    }

    backup.animStructure = anim;
    backup.flip = anim->flip;
    backup.anim = anim->flip->anim;
    // Blend2's flip holds the destination sample, not the blend clock.
    // Restore using the clock so the saved pose gets its original blend weight.
    backup.frameReal = anim->mode == 0 && anim->loopTypeField == ANIM_BLEND2
        ? anim->currentFrame : anim->flip->frameReal;
    backup.valid = true;

    UpdateHumanoidRenderAnimPose(model, smoothState, alpha);
    return backup;
}

static void RestoreHumanoidRenderAnimPose(const HumanoidRenderAnimPoseBackup& backup) {
    if (!backup.valid || !backup.flip) {
        return;
    }

    if (backup.flip->anim != backup.anim) {
        backup.flip->anim = backup.anim;
        backup.flip->dirty = 1;
    }

    if (backup.animStructure && !backup.animStructure->sequence && backup.animStructure->flip == backup.flip) {
        backup.animStructure->ApplyFlipPose(backup.frameReal);
        return;
    }

    backup.flip->SetFrameReal(backup.frameReal);
    backup.flip->UpdateJoints();
}

void Humanoid::ResetRenderInterpolation() {
    ClearHumanoidRenderSmoothState(this);
}

void Humanoid::AdvanceRenderInterpolationTick() {
    HumanoidRenderSmoothState* smoothState = FindHumanoidRenderSmoothState(this);
    if (!smoothState) {
        return;
    }

    if (!smoothState->initialized) {
        smoothState->prevPos = pos;
        smoothState->curPos = pos;
        smoothState->prevOrient = orientation;
        smoothState->curOrient = orientation;
        smoothState->initialized = true;
    }
    else {
        smoothState->prevPos = smoothState->curPos;
        smoothState->curPos = pos;
        smoothState->prevOrient = smoothState->curOrient;
        smoothState->curOrient = orientation;
    }

    HumanoidModel* hm = model ? static_cast<HumanoidModel*>(model) : nullptr;
    AnimStructure* anim = hm ? static_cast<AnimStructure*>(hm->animStructure) : nullptr;
    if (!anim) {
        smoothState->animInitialized = false;
        return;
    }

    const s32 animEnum = anim->animEnum;
    const s32 currentFrame = anim->currentFrame;

    if (!smoothState->animInitialized || smoothState->animEnum != animEnum) {
        smoothState->animInitialized = true;
        smoothState->animEnum = animEnum;
        smoothState->prevAnimFrame = currentFrame;
        smoothState->curAnimFrame = currentFrame;
    }
    else {
        // Blend2's clock is unrelated to the destination clip's frame number.
        // Its last pose was field56, so interpolate from that pose when playback
        // resumes, rather than rewinding through destination frames 5..1.
        const bool finishedBlend = smoothState->animLoopType == ANIM_BLEND2
            && anim->loopTypeField != ANIM_BLEND2;
        smoothState->prevAnimFrame = finishedBlend ? anim->field56 : smoothState->curAnimFrame;
        smoothState->curAnimFrame = currentFrame;
    }
    smoothState->animLoopType = anim->loopTypeField;
}
#else
void Humanoid::ResetRenderInterpolation() {}
void Humanoid::AdvanceRenderInterpolationTick() {}
#endif

// PSX gp+1764 (0x800DD030): gravityReduction
static s32 s_gravityReduction = 3;
// PSX gp+1860 (0x800DD094): flying-back gravity scale
static s32 s_flyingBackGravityScale = 1;
// PSX gp+1958 (0x800DD0F2): zeroGHangTime
static s32 s_zeroGHangTime = 5;

static s32 GetFlyingBackFallDivisor() {
    if (s_gravityReduction == 0) {
        return 18;
    }
    return 18 / s_gravityReduction;
}

static bool IsActiveZoneStillRegistered(const ActiveZone* zone) {
    if (!zone || !g_ai) {
        return false;
    }

    for (ccMinNode* node = g_ai->activeZoneList.head; node; node = node->next) {
        if (node == static_cast<const ccMinNode*>(zone)) {
            return true;
        }
    }

    return false;
}

static s32 CallNextActionCallback(uintptr_t adjustedThis) {
    Humanoid* humanoid = reinterpret_cast<Humanoid*>(adjustedThis);
    if (!humanoid) {
        return 0;
    }

    const s32 nextState = humanoid->walkCycleFlag;
    if (nextState == 0) {
        return 0;
    }

    humanoid->field466 = 0;
    humanoid->walkCycleFlag = 0;
    humanoid->SetActionState(static_cast<u32>(nextState), 0);
    return nextState;
}

// PSX: StitchIdleAnimation__8Humanoid (HUMANOID.CPP:2741, 0x800655B4)
static s32 StitchIdleAnimationCallback(uintptr_t adjustedThis) {
    Humanoid* humanoid = reinterpret_cast<Humanoid*>(adjustedThis);
    if (!humanoid) {
        return 1;
    }

    if (humanoid->actionState != AS_STAND) {
        return 1;
    }

    HumanoidModel* model = humanoid->model ? static_cast<HumanoidModel*>(humanoid->model) : nullptr;
    if (!model) {
        return 1;
    }

    model->SetAnim(model->field120, 0, 0, 0);

    AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
    if (!anim) {
        return 1;
    }

    anim->SetLoopType(ANIM_LOOP, 1);
    return 1;
}

static void SetCallNextActionCallback(Model* model) {
    if (!model) {
        return;
    }

    AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
    if (!anim) {
        return;
    }

    anim->humanoidCB.offsetLo = 0;
    anim->humanoidCB.offsetHi = -1;
    anim->humanoidCB.funcPtr = reinterpret_cast<void*>(CallNextActionCallback);
}

static s32 GetWeaponTransitionIdle(const Pickup* pickup);
static void SetTransitionIdleAnim(HumanoidModel* model, s32 transitionAnim, s32 targetIdleAnim);

static constexpr s32 COLLISION_TAG_IMPACT_REGION = static_cast<s32>(0x80000002u);
static constexpr s32 COLLISION_TAG_HIT_TYPE = static_cast<s32>(0x80000003u);
static constexpr s32 COLLISION_TAG_FORCE = static_cast<s32>(0x80000005u);
static constexpr s32 COLLISION_TAG_IMPULSE = static_cast<s32>(0x80000006u);
static constexpr s32 COLLISION_TAG_DAMAGE = static_cast<s32>(0x80000007u);
static constexpr s32 COLLISION_TAG_END = 0;

// PSX: humanoidStraif animation table at 0x800D9208
// [idle,loop], [forward,loop], [back-side,loop], [backward,loop], [side,loop]
static s32 s_humanoidStraif[] = {
    22, 0,
    49, 0,
    49, 1,
    50, 0,
    50, 1,
};

static s32 GetRelativeAngle(s32 sourceAngle, s32 targetAngle) {
    s32 delta = sourceAngle - targetAngle;

    while (delta > 0xFFFF) {
        delta -= 0xFFFF;
    }
    while (delta < 0) {
        delta += 0xFFFF;
    }

    return delta;
}

static s32 WrapFacingTurnDelta(s32 delta) {
    while (delta > 0x8000) {
        delta -= 0xFFFF;
    }
    while (delta < -32768) {
        delta += 0xFFFF;
    }
    return delta;
}

struct ThrowMoveData {
    u32 address = 0;
    LVector releaseVector = {};
    LVector attachVector = {};
    u8 damage = 0;
    s8 shockFrame = 127;
};

// PSX throw rows store full 32-bit release/attach vectors that are not preserved
// by the generic exported fighting-move aggregate. Use exact per-row truth here.
// Keep this table sorted by address; LookupThrowMoveData uses binary search.
static constexpr ThrowMoveData kThrowMoveData[] = {
    { 0x80021BE4u, { 0, 0, 14000 }, { 20, 0, 700 }, 30, 32 },
    { 0x80021C20u, { 0, 4000, 20000 }, { 100, 0, 700 }, 30, 38 },
    { 0x80021C5Cu, { 0, 0, 14000 }, { 20, 0, 700 }, 30, 44 },
    { 0x80021C98u, { 0, 2000, -30000 }, { 20, 0, 525 }, 30, 33 },
    { 0x80021CD4u, { 0, 0, 0 }, { 0, 100, 600 }, 40, 127 },
    { 0x80021D10u, { 0, 6500, 30000 }, { 0, 0, 600 }, 40, 45 },
    { 0x80021D4Cu, { 0, 2000, 60000 }, { 20, 0, 1000 }, 45, 29 },
    { 0x80021D88u, { -60000, 2000, 4100 }, { 0, 0, 1000 }, 45, 34 },
    { 0x80021DC4u, { 0, 9500, 40000 }, { 20, 0, 700 }, 0, 44 },
    { 0x80021E00u, { 0, 6000, 8000 }, { 20, 0, 700 }, 45, 44 },
    { 0x80021E3Cu, { 0, 0, 14000 }, { 20, 0, 525 }, 45, 33 },
    { 0x80021E78u, { 0, 0, 60000 }, { 20, 0, 875 }, 45, 35 },
    { 0x80021EB4u, { 0, 0, 14000 }, { 20, 0, 875 }, 45, 32 },
    { 0x80021EF0u, { 0, 0, -13000 }, { 20, 0, 500 }, 45, 47 },
    { 0x80021F2Cu, { 0, 0, 0 }, { 20, 0, 550 }, 45, 127 },
    { 0x80021F68u, { 0, 0, 0 }, { 20, 0, 550 }, 45, 127 },
    { 0x800D15E4u, { 0, 2000, 10000 }, { 125, 0, 400 }, 45, 45 },
    { 0x800D1620u, { 0, 4000, -6000 }, { 125, 0, 520 }, 50, 47 },
    { 0x800D165Cu, { 0, 0, 8000 }, { 20, 0, 684 }, 45, 23 },
    { 0x800D1698u, { 0, 3500, -40000 }, { 20, 0, 400 }, 45, 22 },
    { 0x800D1710u, { 0, 0, 0 }, { 0, 0, 300 }, 45, 8 },
    { 0x800D174Cu, { 0, 9500, 40000 }, { 20, 0, 700 }, 0, 44 },
    { 0x800D1788u, { 0, 0, 14000 }, { 20, 0, 650 }, 45, 48 },
    { 0x800D17C4u, { -5536, 5000, 2000 }, { 20, 0, 500 }, 45, 38 },
    { 0x800D1800u, { 0, 0, 14000 }, { 20, 0, 875 }, 35, 127 },
    { 0x800D183Cu, { 0, 0, 14000 }, { 20, 0, 400 }, 35, 127 },
    { 0x800D1878u, { 0, 0, 14000 }, { 20, 0, 875 }, 35, 127 },
    { 0x800D18B4u, { 0, 3000, 30000 }, { 20, 0, 875 }, 35, 29 },
    { 0x800D18F0u, { 40000, 5000, 5000 }, { 20, 0, 875 }, 35, 35 },
    { 0x800D192Cu, { 40000, 5000, 5000 }, { 20, 0, 875 }, 35, 35 },
    { 0x800D1968u, { 40000, 5000, 5000 }, { 20, 0, 875 }, 35, 35 },
    { 0x800D19A4u, { 0, 0, 14000 }, { 20, 0, 875 }, 35, 127 },
    { 0x800D19E0u, { 0, 0, 14000 }, { 20, 0, 875 }, 35, 127 },
    { 0x800D1A1Cu, { 0, 0, 14000 }, { 20, 0, 875 }, 35, 127 },
    { 0x800D1A58u, { 0, 0, 14000 }, { 20, 0, 675 }, 35, 127 },
    { 0x800D1A94u, { 0, 0, 14000 }, { 20, 0, 600 }, 35, 127 },
    { 0x800D1AD0u, { 0, 9500, 40000 }, { 20, 0, 700 }, 0, 44 },
    { 0x800D1B0Cu, { 0, 0, 14000 }, { 20, 0, 650 }, 35, 127 },
    { 0x800D1B48u, { -5536, 5000, 2000 }, { 20, 0, 500 }, 35, 127 },
};

static const ThrowMoveData* LookupThrowMoveData(u32 address) {
    s32 low = 0;
    s32 high = static_cast<s32>(sizeof(kThrowMoveData) / sizeof(kThrowMoveData[0])) - 1;

    while (low <= high) {
        const s32 mid = low + ((high - low) / 2);
        const ThrowMoveData& entry = kThrowMoveData[mid];
        if (entry.address == address) {
            return &entry;
        }

        if (entry.address < address) {
            low = mid + 1;
        }
        else {
            high = mid - 1;
        }
    }

    return nullptr;
}

static u8 LookupThrowDamageByte(u32 address) {
    const ThrowMoveData* data = LookupThrowMoveData(address);
    return data ? data->damage : 0;
}

static s8 LookupThrowShockFrame(u32 address) {
    const ThrowMoveData* data = LookupThrowMoveData(address);
    return data ? data->shockFrame : 127;
}

// PSX: CalculateFallDamage__Fl (PLAYER.CPP:3161, 0x80032348)
static s32 CalculateFallDamage(s32 heightUnits) {
    MARKFUNCTION(0x80032348);

    if (heightUnits >= 6) {
        return (5 * heightUnits) - 5;
    }

    return 0;
}

struct PsxFightingMoveRaw;

struct FightingComboNode {
    u32 psxAddress = 0;
    u8 requestedCommand = 0;
    s8 minFrame = 0;
    s8 maxFrame = 0;
    u8 field03 = 0;
    u8 field04 = 0;
    s8 field05 = 0;
    s8 field06 = 0;
    s8 field07 = 0;
    const PsxFightingMoveRaw* moveData = nullptr;
    FightingComboNode* child = nullptr;
    FightingComboNode* sibling = nullptr;
};

struct FightingSystemHashEntry {
    u32 hash = 0;
    u32 rootAddress = 0;
};

struct TypeFightingSystemEntry {
    u16 type = 0;
    u32 hash = 0;
};

struct PsxFightingNodeRaw {
    u32 address = 0;
    u32 packedCommand = 0;
    u32 field04 = 0;
    u32 moveData = 0;
    u32 childAddress = 0;
    u32 siblingAddress = 0;
};

struct PsxFightingMoveRaw {
    u32 address = 0;
    u32 firstWord = 0;
    s32 turnDelta = 0;
    u16 anim = 0;
    u16 fightingPoints = 0;
    u8 stylePointsFlag = 0;
    s8 moveWindowStart = 0;
    s8 moveWindowEnd = 0;
    s8 moveDelta = 0;
    s8 combatWindowStart = 0;
    s8 combatWindowEnd = 0;
    u8 weaponBreakOnEmpty = 0;
    u8 fightingType = 0;
    u32 data20 = 0;
    u32 data24 = 0;
    s16 throwVectorX = 0;
    s16 throwVectorY = 0;
    s16 throwVectorZ = 0;
    s16 throwAttachX = 0;
    s16 throwAttachY = 0;
    s16 throwAttachZ = 0;
    u16 throwTargetAnim = 0;
    s8 throwImpactFrame = 0;
    s8 throwAttachFrame = 0;
    s8 throwReleaseFrame = 0;
    s8 throwScoreFrame = 0;
};

struct PsxFightingJointRaw {
    u32 address = 0;
    u32 word0 = 0;
    u32 word1 = 0;
    u32 word2 = 0;
    u32 word3 = 0;
    u32 word4 = 0;
    u32 word5 = 0;

    u8 JointIndex() const { return static_cast<u8>(word0 & 0xFFu); }
    u8 SoundEvent() const { return static_cast<u8>((word0 >> 8) & 0xFFu); }
    u8 Damage() const { return static_cast<u8>((word0 >> 16) & 0xFFu); }
    s8 HitForce() const { return static_cast<s8>((word0 >> 24) & 0xFFu); }
    // PSX: RC9_RMVECT16 is a 3x s32 vector at +4,+8,+12.
    s32 ForceX() const { return static_cast<s32>(word1); }
    s32 ForceY() const { return static_cast<s32>(word2); }
    s32 ForceZ() const { return static_cast<s32>(word3); }
    s8 AttackStartFrame() const { return static_cast<s8>(word4 & 0xFFu); }
    s8 AttackEndFrame() const { return static_cast<s8>((word4 >> 8) & 0xFFu); }
    s8 SoundFrame() const { return static_cast<s8>((word4 >> 16) & 0xFFu); }
    u8 TrailFlags() const { return static_cast<u8>((word4 >> 24) & 0xFFu); }
};

#include "ai/fightani_data.inl"
#include "ai/fightmove_data.inl"
#include "ai/fightjoint_data.inl"

static FightingComboNode
    s_fightingNodeCache[sizeof(kPsxFightingNodeTable) / sizeof(kPsxFightingNodeTable[0])];
static bool s_fightingNodeCacheInitialized = false;

static s32 FindPsxFightingNodeIndex(u32 address) {
    const s32 nodeCount = static_cast<s32>(
        sizeof(kPsxFightingNodeTable) / sizeof(kPsxFightingNodeTable[0]));

    s32 low = 0;
    s32 high = nodeCount - 1;
    while (low <= high) {
        const s32 mid = low + ((high - low) / 2);
        const u32 midAddress = kPsxFightingNodeTable[mid].address;
        if (midAddress == address) {
            return mid;
        }
        if (midAddress < address) {
            low = mid + 1;
        }
        else {
            high = mid - 1;
        }
    }

    return -1;
}

static s32 FindPsxFightingMoveIndex(u32 address) {
    const s32 moveCount = static_cast<s32>(
        sizeof(kPsxFightingMoveTable) / sizeof(kPsxFightingMoveTable[0]));

    s32 low = 0;
    s32 high = moveCount - 1;
    while (low <= high) {
        const s32 mid = low + ((high - low) / 2);
        const u32 midAddress = kPsxFightingMoveTable[mid].address;
        if (midAddress == address) {
            return mid;
        }
        if (midAddress < address) {
            low = mid + 1;
        }
        else {
            high = mid - 1;
        }
    }

    return -1;
}

static const PsxFightingMoveRaw* ResolveFightingMoveAddress(u32 address) {
    if (!address) {
        return nullptr;
    }

    const s32 moveIndex = FindPsxFightingMoveIndex(address);
    if (moveIndex < 0) {
        return nullptr;
    }

    return &kPsxFightingMoveTable[moveIndex];
}

static s32 FindPsxFightingJointIndex(u32 address) {
    const s32 jointCount = static_cast<s32>(
        sizeof(kPsxFightingJointTable) / sizeof(kPsxFightingJointTable[0]));

    s32 low = 0;
    s32 high = jointCount - 1;
    while (low <= high) {
        const s32 mid = low + ((high - low) / 2);
        const u32 midAddress = kPsxFightingJointTable[mid].address;
        if (midAddress == address) {
            return mid;
        }
        if (midAddress < address) {
            low = mid + 1;
        }
        else {
            high = mid - 1;
        }
    }

    return -1;
}

static const PsxFightingJointRaw* ResolveFightingJointAddress(u32 address) {
    if (!address) {
        return nullptr;
    }

    const s32 jointIndex = FindPsxFightingJointIndex(address);
    if (jointIndex < 0) {
        return nullptr;
    }

    return &kPsxFightingJointTable[jointIndex];
}

static void InitFightingNodeCache() {
    if (s_fightingNodeCacheInitialized) {
        return;
    }

    const s32 nodeCount = static_cast<s32>(
        sizeof(kPsxFightingNodeTable) / sizeof(kPsxFightingNodeTable[0]));

    for (s32 i = 0; i < nodeCount; i++) {
        const PsxFightingNodeRaw& raw = kPsxFightingNodeTable[i];
        FightingComboNode& node = s_fightingNodeCache[i];

        node.psxAddress = raw.address;
        node.requestedCommand = static_cast<u8>(raw.packedCommand & 0xFF);
        node.minFrame = static_cast<s8>((raw.packedCommand >> 8) & 0xFF);
        node.maxFrame = static_cast<s8>((raw.packedCommand >> 16) & 0xFF);
        node.field03 = static_cast<u8>((raw.packedCommand >> 24) & 0xFF);
        node.field04 = static_cast<u8>(raw.field04 & 0xFF);
        node.field05 = static_cast<s8>((raw.field04 >> 8) & 0xFF);
        node.field06 = static_cast<s8>((raw.field04 >> 16) & 0xFF);
        node.field07 = static_cast<s8>((raw.field04 >> 24) & 0xFF);
        node.moveData = ResolveFightingMoveAddress(raw.moveData);
        node.child = nullptr;
        node.sibling = nullptr;
    }

    for (s32 i = 0; i < nodeCount; i++) {
        const PsxFightingNodeRaw& raw = kPsxFightingNodeTable[i];
        FightingComboNode& node = s_fightingNodeCache[i];

        if (raw.childAddress) {
            const s32 childIndex = FindPsxFightingNodeIndex(raw.childAddress);
            if (childIndex >= 0) {
                node.child = &s_fightingNodeCache[childIndex];
            }
        }

        if (raw.siblingAddress) {
            const s32 siblingIndex = FindPsxFightingNodeIndex(raw.siblingAddress);
            if (siblingIndex >= 0) {
                node.sibling = &s_fightingNodeCache[siblingIndex];
            }
        }
    }

    s_fightingNodeCacheInitialized = true;
}

static FightingComboNode* ResolveFightingRootAddress(u32 rootAddress) {
    InitFightingNodeCache();

    const s32 rootIndex = FindPsxFightingNodeIndex(rootAddress);
    if (rootIndex < 0) {
        return nullptr;
    }

    return &s_fightingNodeCache[rootIndex];
}

const FightingComboNode* ResolveFightingNodeAddressConst(u32 nodeAddress) {
    return ResolveFightingRootAddress(nodeAddress);
}

static FightingComboNode* GetFallbackFightingRoot() {
    return ResolveFightingRootAddress(kPlayerPunchRootAddress);
}

// PSX: FindFightingSystem__FUl (FIGHTANI.CPP:213)
static FightingComboNode* FindFightingSystem(u32 hash) {
    MARKFUNCTION(0x8007DBE0);

    const s32 tableCount = static_cast<s32>(
        sizeof(kFightingSystemTable) / sizeof(kFightingSystemTable[0]));
    for (s32 i = 0; i < tableCount; i++) {
        if (kFightingSystemTable[i].hash == hash) {
            FightingComboNode* root =
                ResolveFightingRootAddress(kFightingSystemTable[i].rootAddress);
            return root ? root : GetFallbackFightingRoot();
        }
    }

    return GetFallbackFightingRoot();
}

// PSX: FindBossFightingSystem__FUl (FIGHTANI.CPP:233)
static FightingComboNode* FindBossFightingSystem(u32 hash) {
    MARKFUNCTION(0x8007DC34);

    const s32 tableCount = static_cast<s32>(
        sizeof(kBossFightingSystemTable) / sizeof(kBossFightingSystemTable[0]));
    for (s32 i = 0; i < tableCount; i++) {
        if (kBossFightingSystemTable[i].hash == hash) {
            FightingComboNode* root =
                ResolveFightingRootAddress(kBossFightingSystemTable[i].rootAddress);
            return root ? root : GetFallbackFightingRoot();
        }
    }

    return GetFallbackFightingRoot();
}

static bool IsBossFightingType(u16 type) {
    switch (type) {
    case 10:
    case 12:
    case 13:
    case 15:
    case 17:
    case 23:
        return true;
    default:
        return false;
    }
}

// PSX: FindTypeFightingSystem__FUsP18TypeFightingSystemUl (FIGHTANI.CPP:269)
static FightingComboNode* FindTypeFightingSystem(
    u16 type,
    const TypeFightingSystemEntry* table,
    u32 tableSize) {
    MARKFUNCTION(0x8007DC8C);

    if (!tableSize) {
        return nullptr;
    }

    for (u32 i = 0; i < tableSize; i++) {
        if (table[i].type == type) {
            if (IsBossFightingType(type)) {
                return FindBossFightingSystem(table[i].hash);
            }
            return FindFightingSystem(table[i].hash);
        }
    }

    return nullptr;
}

// PSX: GetFightingSystem__FUs (FIGHTANI.CPP:316)
static FightingComboNode* GetFightingSystem(u16 type) {
    MARKFUNCTION(0x8007DD20);

    FightingComboNode* root = FindTypeFightingSystem(
        type,
        kCharacterTypeFightingSystemTable,
        static_cast<u32>(
            sizeof(kCharacterTypeFightingSystemTable)
            / sizeof(kCharacterTypeFightingSystemTable[0])));

    if (!root) {
        root = GetFallbackFightingRoot();
    }

    return root;
}

// PSX: FindSiblingWithRequestedCommand__8HumanoidPC17FightingComboNodel (HUMANOID.CPP:7713)
s32 Humanoid::FindSiblingWithRequestedCommand(const FightingComboNode* root, u32 requestedBits) {
    MARKFUNCTION(0x8006B5A8);

    const FightingComboNode* node = root;
    while (node) {
        if (node->requestedCommand < 32
            && ((requestedBits >> node->requestedCommand) & 1u) != 0) {
            return static_cast<s32>(node->psxAddress);
        }
        node = node->sibling;
    }
    return 0;
}

// PSX: FindSiblingWithRequestedCommand__8HumanoidPC17FightingComboNodell (HUMANOID.CPP:7741)
s32 Humanoid::FindSiblingWithRequestedCommand(
    const FightingComboNode* root, u32 requestedBits, s32 frame) {
    MARKFUNCTION(0x8006B5EC);

    const FightingComboNode* node = root;
    while (node) {
        if (node->requestedCommand < 32
            && ((requestedBits >> node->requestedCommand) & 1u) != 0
            && frame >= node->minFrame
            && node->maxFrame >= frame) {
            return static_cast<s32>(node->psxAddress);
        }
        node = node->sibling;
    }
    return 0;
}

// PSX: FindChildWithRequestedCommand__8HumanoidPC17FightingComboNodel (HUMANOID.CPP:7776)
s32 Humanoid::FindChildWithRequestedCommand(const FightingComboNode* root, u32 requestedBits) {
    MARKFUNCTION(0x8006B658);

    if (!root) {
        return 0;
    }

    return FindSiblingWithRequestedCommand(root->child, requestedBits);
}

// PSX: FindChildWithRequestedCommand__8HumanoidPC17FightingComboNodell (HUMANOID.CPP:7797)
s32 Humanoid::FindChildWithRequestedCommand(
    const FightingComboNode* root, u32 requestedBits, s32 frame) {
    MARKFUNCTION(0x8006B67C);

    if (!root) {
        return 0;
    }

    return FindSiblingWithRequestedCommand(root->child, requestedBits, frame);
}

// PSX reads the root joint via tSTree::GetJoint(0) (vtable+0x14 @0x80085A10),
// which resolves through the tree's joint-order map, not by joint name.
static const STreeJoint* GetBip01Joint(const STreeData* skeleton) {
    if (!skeleton) {
        return nullptr;
    }
    return skeleton->GetJoint(0);
}

// PSX: HandleAnimationControl__8Humanoid (HUMANOID.CPP:1590)
s32 Humanoid::HandleAnimationControl() {
    MARKFUNCTION(0x80064194);

    if ((flags2 & TF2_NIS_ENTER) == 0) {
        return 0;
    }

    if (!model) {
        return 0;
    }

    Model* m = static_cast<Model*>(model);
    if (!m->drawable) {
        return 0;
    }

    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return 0;
    }

    OriginalSTree* source = GetActiveSTree(m->drawable);
    STreeData* skeleton = source ? source->skeleton : nullptr;
    const STreeJoint* joint = GetBip01Joint(skeleton);
    if (!joint) {
        return 0;
    }

    LVector nextHome = homePos;
    LVector localOffset = {};
    localOffset.x = joint->translationX;
    localOffset.y = joint->translationY;
    localOffset.z = joint->translationZ;

    LVector worldOffset = {};
    GetObjectToWorldSpaceVector(localOffset, worldOffset);

    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
    const s32 loopCount = anim->loopCount;
    if (loopCount > 0 && frame == 0) {
        field516 = worldOffset.x;
        field520 = worldOffset.y;
        field524 = worldOffset.z;
    }

    if (((flags2 >> 5) & 1) == 0) {
        nextHome.y += (s32)worldOffset.y - field520;
    }

    if (((flags2 >> 6) & 1) == 0) {
        nextHome.x += (s32)worldOffset.x - field516;
        nextHome.z += (s32)worldOffset.z - field524;
    }

    field516 = worldOffset.x;
    field520 = worldOffset.y;
    field524 = worldOffset.z;

    if (loopCount == 0 && frame == 0) {
        if (actionState == AS_THROW_CHARACTER_RECEIVE) {
            pos.y = nextHome.y;
            ResetRenderInterpolation();
        }
    }

    homePos = nextHome;
    return 1;
}

// PSX: __8HumanoidPC10tagLVectorUs (HUMANOID.CPP:350)
Humanoid::Humanoid(const LVector* initialPos, u16 type)
    : DynamicThing(initialPos, type) {
    MARKFUNCTION(0x80062A34);

    attackJointIndex = -1;
    prevAttackJointIndex = -1;
    collBboxMin = { 175, 0, 768 };
    collBboxMax = { 175, 0, 768 };
    humanoidSound = nullptr;
    combatFlag = 0;
    turnRate = 2730;
    field344 = 0;
    stateDispatch = SD_STAND;
    field348 = 8;
    distantTargetRange = 16000;
    stateCounter = 100; // PSX: this+52 = 100 for humanoids
    field424 = 0;
    field428 = 0;
    field432 = 0;
    field466 = 0;
    field468 = 0;
    comboCount = 1;
    animControl = 0;
    field528 = 0;
    moveSpeed = 3000; // PSX: set in constructor
    spawnCount = 1;
    field408 = -1;
    field484 = 0;
    field488 = 0;
    field364 = 0;
    field256 = 0;
    field260 = 0;
    rightHandObj = nullptr;
    leftHandObj = nullptr;
    field496 = 0;
    activeZone = nullptr;
    field384 = 0;
    field388 = 0;
    field392 = 0;
    field396 = 0;
    field400 = 0;
    field404 = 0;
    field412 = 0;
    field416 = 0;
    field316 = 0;
    field320 = 0;
    field324 = 0;
    field452 = 0;
    behaviourNameHash = 0;
    field436 = 0;
    characterSubType = 0;
    faceAngleData = s_humanoidStraif;
    soundHandle = 0;
    soundParam = 0;
    punchDir = 0;
    kickDir = 0;
    comboDir = 0;
    // PSX: health/maxHealth set to 100 for humanoids
    health = 100;
    maxHealth = 100;

    const HumanoidDataEntry* dataEntry = GetHumanoidData(type);
    if (dataEntry) {
        humanoidData = dataEntry->data;
        humanoidDataID = dataEntry->dataID;
    }
    else {
        humanoidData = nullptr;
        humanoidDataID = 20;
    }

    fightingSystem = static_cast<void*>(GetFightingSystem(type));
    defaultFightingSystem = fightingSystem;

}

// PSX: _._8Humanoid (HUMANOID.CPP:490)
Humanoid::~Humanoid() {
    MARKFUNCTION(0x80062C58);
#if HIGH_FPS_PLAY_PRESENTATION
    ClearHumanoidRenderSmoothState(this);
#endif
    // PSX: KillDialog, DeleteModel, DeleteRightHandObj, DeleteLeftHandObj, etc.
    KillDialog(0, 0, 512);
    pcDestroying = true;
    UnregisterEncounterPopulation();
    DeleteModel();
    DeleteRightHandObj();
    DeleteLeftHandObj();
    FightingCollision::RemoveHumanoid(this);
    if (humanoidSound) {
        humanoidSound->Release();
        humanoidSound = nullptr;
    }
    if (trails) {
        delete static_cast<Trails*>(trails);
        trails = nullptr;
    }
    delete behaviour;
    behaviour = nullptr;
    fightingSystem = nullptr;
    defaultFightingSystem = nullptr;
    humanoidData = nullptr;
}

// PSX: Think__8Humanoid (HUMANOID.CPP:1133)
void Humanoid::Think() {
    MARKFUNCTION(0x80063808);

    const bool directorInputLocked =
        g_director && g_director->scriptState != 0 && g_director->enableInput == 0;
    const bool isPlayerHumanoid = (this == static_cast<Humanoid*>(Player::s_player));
    const bool isDirectorControlledNis =
        actionState == static_cast<s32>(AS_NIS_MODE) || (flags2 & TF2_NIS_ENTER) != 0;

    // PSX cutscene scripts can continue after g_directorActive drops; freeze
    // non-player AI while Director still owns the scene unless the actor is in
    // explicit NIS control from the script itself.
    if (directorInputLocked && !isPlayerHumanoid && !isDirectorControlledNis) {
        if (model) {
            HumanoidModel* hm = static_cast<HumanoidModel*>(model);
            hm->attackHandRadius = 72;
            hm->attackFootRadius = 100;
        }

        runSpeed = 0;
        flags &= ~TF_BIT1;
        flags2 &= ~TF2_BIT3;
        commandBits = 0;

        ProcessAction();
        Move();

        field368 = 0;
        thinkCounter++;
        return;
    }

    // PSX step 1: CHumanoidSound::Think
    if (humanoidSound) {
        humanoidSound->Think();
    }
    // PSX step 2-3 (0x80063834-0x80063934): boss types (Grontar/Paul/Oscar/
    // Dante/Butch) call LoadEnemyTaunts unconditionally every Think tick;
    // other types only roll for it while not AS_STAND_ANIM. PSX also gates
    // non-boss rolls on a shared last-taunt-frame cooldown (gp+0x6E8/+0x6F8)
    //
    // PC: skip this while the actor is under explicit director/NIS control
    if (!isDirectorControlledNis) {
        bool isBossTypeForTaunt = false;
        switch (thingType) {
            case AITypes::TT_GRONTAR:
            case AITypes::TT_PAUL:
            case AITypes::TT_OSCAR:
            case AITypes::TT_DANTE:
            case AITypes::TT_BUTCH:
                isBossTypeForTaunt = true;
                break;
        }

        if (isBossTypeForTaunt) {
            LoadEnemyTaunts();
        }
        else {
            static u32 s_lastNonBossTauntFrame = 0;
            const u32 currentFrame = g_time ? g_time->GetFrameCounter() : 0;
            if (actionState != (s32)AS_STAND_ANIM
                && (s32)rmRangedRandom(100) < 20
                && (currentFrame - s_lastNonBossTauntFrame) >= NON_BOSS_TAUNT_COOLDOWN_FRAMES) {
                s_lastNonBossTauntFrame = currentFrame;
                LoadEnemyTaunts();
            }
        }
    }

    // PSX step 4 (0x800638CC-0x80063944): if a taunt dialog request is
    // pending (flags2 bit7) and the prior dialog handle is gone/invalid/done
    // playing, clear the pending state and load the player's response line.
    if (flags2 & TF2_TAUNT_PENDING) {
        const bool needsResponse = !soundHandle
            || !jcsValidateHandle(soundHandle)
            || !jcsIsPlaying(soundHandle);

        if (needsResponse) {
            soundHandle = 0;
            soundParam = 0;
            flags2 &= ~TF2_TAUNT_PENDING;
            if (Player::s_player) {
                Player::s_player->LoadPlayerTauntResponse(this);
            }
        }
    }

    // PSX step 6: clear flag bits
    if (model) {
        HumanoidModel* hm = static_cast<HumanoidModel*>(model);
        hm->attackHandRadius = 72;
        hm->attackFootRadius = 100;
    }

    flags &= ~TF_BIT1;
    flags2 &= ~TF2_BIT3;

    // PSX step 7: process AI behaviour
    ProcessControl();

    // PSX step 8: delta time computation (fixed-point 16.16 multiply)
    // result = (moveSpeed * deltaTime) >> 16
    s64 dt = (s64)moveSpeed * (s64)deltaTime;
    s32 scaledRange = (s32)((u64)dt >> 16);
    runSpeed = scaledRange; // PSX writes this to +212 each frame
    deltaTime = FIX16_ONE; // reset to 1.0

    // PSX step 9: face player if not player and not in certain states
    // (requires FightingCollision system, simplified for now)

    ProcessAction();

    Move();

    field368 = 0;
    thinkCounter++;
}

// PSX: Draw__8Humanoid (HUMANOID.CPP:1280)
// PSX: swaps animation matrices, sets pos/orient on model, Show(0),
// then updates collision bbox from skeleton joints (debug draw skipped).
void Humanoid::Draw() {
    MARKFUNCTION(0x80063A88);

    LVector drawPos = pos;
    LVector drawOrient = orientation;

#if HIGH_FPS_PLAY_PRESENTATION
    const bool humanoidInPlay =
        (g_time && g_game && g_game->GetState() == GameState::Play);
    f32 presentationAlpha = humanoidInPlay ? g_time->GetPlayPresentationAlpha() : 0.0f;
    HumanoidRenderSmoothState* smoothState = nullptr;

    if (humanoidInPlay) {
        smoothState = FindHumanoidRenderSmoothState(this);

        if (smoothState && smoothState->initialized) {
            if (presentationAlpha < 0.0f) {
                presentationAlpha = 0.0f;
            }
            else if (presentationAlpha > 1.0f) {
                presentationAlpha = 1.0f;
            }

            s32 stepDx = smoothState->curPos.x - smoothState->prevPos.x;
            s32 stepDy = smoothState->curPos.y - smoothState->prevPos.y;
            s32 stepDz = smoothState->curPos.z - smoothState->prevPos.z;

            drawPos.x = smoothState->prevPos.x + (s32)((f32)stepDx * presentationAlpha);
            drawPos.y = smoothState->prevPos.y + (s32)((f32)stepDy * presentationAlpha);
            drawPos.z = smoothState->prevPos.z + (s32)((f32)stepDz * presentationAlpha);

            const s32 stepRX = PsxAngleDelta16(smoothState->curOrient.x, smoothState->prevOrient.x);
            const s32 stepRY = PsxAngleDelta16(smoothState->curOrient.y, smoothState->prevOrient.y);
            const s32 stepRZ = PsxAngleDelta16(smoothState->curOrient.z, smoothState->prevOrient.z);

            drawOrient.x = (u16)(smoothState->prevOrient.x + (s32)((f32)stepRX * presentationAlpha));
            drawOrient.y = (u16)(smoothState->prevOrient.y + (s32)((f32)stepRY * presentationAlpha));
            drawOrient.z = (u16)(smoothState->prevOrient.z + (s32)((f32)stepRZ * presentationAlpha));
        }
    }
    else {
        ClearHumanoidRenderSmoothState(this);
    }
#endif

#if MODERN_GRAPHICS
    if (ShadowCSM::IsCasterPrepass()) {
        if (model) {
            HumanoidModel* hm = static_cast<HumanoidModel*>(model);
            Model* m = static_cast<Model*>(model);
            m->posX = drawPos.x;
            m->posY = drawPos.y;
            m->posZ = drawPos.z;
            m->rotX = (u16)(drawOrient.x & 0xFFFF);
            m->rotY = (u16)(drawOrient.y & 0xFFFF);
            m->rotZ = (u16)(drawOrient.z & 0xFFFF);
#if HIGH_FPS_PLAY_PRESENTATION
            HumanoidRenderAnimPoseBackup renderAnimBackup = {};
            if (humanoidInPlay && smoothState && hm->animMatrices) {
                const s32 savedCapture = hm->animMatrices->CaptureEnabled();
                hm->animMatrices->SetCaptureEnabled(0);
                renderAnimBackup = ApplyHumanoidRenderAnimPose(hm, smoothState, presentationAlpha);
                m->Show(0);
                RestoreHumanoidRenderAnimPose(renderAnimBackup);
                hm->animMatrices->SetCaptureEnabled(savedCapture);
                return;
            }
#endif
            m->Show(0);
        }
        return;
    }
#endif

    if (model) {
        HumanoidModel* hm = static_cast<HumanoidModel*>(model);
#if HIGH_FPS_PLAY_PRESENTATION
        s32 savedCapture = 1;
        // High-FPS path: the authoritative attack-joint capture + buffer swap is
        // driven once per logic tick by CaptureHumanoidAttackJointsLoop, NOT here.
        // Draw() runs at render rate on an interpolated pose, so capture is
        // disabled during the in-play Show() to keep render-only/interpolated
        // joint positions out of the gameplay hit-detection buffers.
        if (hm->animMatrices) {
            savedCapture = hm->animMatrices->CaptureEnabled();
            hm->animMatrices->SetCaptureEnabled(humanoidInPlay ? 0 : 1);
        }
#else
        if (hm->animMatrices) {
            hm->animMatrices->Swap();
        }
#endif
        // PSX: copy pos/orientation to model, then Show(0)
        Model* m = static_cast<Model*>(model);
        m->posX = drawPos.x;
        m->posY = drawPos.y;
        m->posZ = drawPos.z;
        m->rotX = (u16)(drawOrient.x & 0xFFFF);
        m->rotY = (u16)(drawOrient.y & 0xFFFF);
        m->rotZ = (u16)(drawOrient.z & 0xFFFF);

    #if HIGH_FPS_PLAY_PRESENTATION
        HumanoidRenderAnimPoseBackup renderAnimBackup = {};
        if (humanoidInPlay) {
            renderAnimBackup = ApplyHumanoidRenderAnimPose(hm, smoothState, presentationAlpha);
        }
    #endif
        m->Show(0);

#if HIGH_FPS_PLAY_PRESENTATION
        RestoreHumanoidRenderAnimPose(renderAnimBackup);
        if (hm->animMatrices) {
            hm->animMatrices->SetCaptureEnabled(savedCapture);
        }
#endif

        attackJointIndex = -1;

        if (rightHandObj) {
            rightHandObj->UpdatePosition();
            rightHandObj->Draw();
        }
        if (leftHandObj) {
            leftHandObj->UpdatePosition();
            leftHandObj->Draw();
        }

        UpdateCollisionBoundsFromJoints();

        return;
    }
    // No model: fallback to debug wireframe
    Thing::Draw();
}

// PSX: tail of Draw__8Humanoid.
void Humanoid::UpdateCollisionBoundsFromJoints() {
    HumanoidModel* hm = static_cast<HumanoidModel*>(model);
    if (!hm || !hm->animMatrices || !hm->animMatrices->Copy()) {
        return;
    }

    collBboxMin.x = 175;
    collBboxMin.y = 0;
    collBboxMax.x = 175;

    const s32* rootMatrix = hm->animMatrices->GetMatrix(0);
    if (rootMatrix) {
        collBboxMin.z = rootMatrix[6] - pos.y + HEAD_RISE;
    }
    else {
        collBboxMin.z = HEAD_RISE;
    }

    const s32 state = actionState;
    if ((u32)(state - 57) < 2u || state == 53 || state == AS_THROW_FREE_FALL) {
        s32 maxRadius = 0;
        for (u32 i = 0; i < 5; i++) {
            const s32* matrix = hm->animMatrices->GetMatrix(i);
            if (!matrix) {
                continue;
            }
            const s32 radius = (s32)rmMag2((f32)(matrix[5] - pos.x), (f32)(matrix[7] - pos.z));
            if (maxRadius < radius) {
                maxRadius = radius;
            }
        }
        collBboxMax.x = maxRadius;
    }

    if (state == AS_DIVE_ROLL) {
        if (thingType != 10) {
            collBboxMin.z = HUMANOID_DIVE_ROLL_Y_RISE;
        }
    }
    else if (state == AS_LEDGE_LATCH) {
        if (collBboxMin.z < HUMANOID_LEDGE_LATCH_MIN_Y_RISE) {
            collBboxMin.z = HUMANOID_LEDGE_LATCH_MIN_Y_RISE;
        }
    }
}

// PSX: Reset__8Humanoid (HUMANOID.CPP:513)
void Humanoid::Reset() {
    MARKFUNCTION(0x80062DC0);
    DynamicThing::Reset();

    // PSX: if this != global player, clear target
    if (this != (Thing*)Player::s_player) {
        SetTarget(nullptr);
    }

    turnRate = 2730;
    stateTimer = 0;
    thinkCounter = 0;

    // PSX: flags |= 4 (needs activation), flags &= ~0x100
    flags |= TF_NEEDS_ACTIVATION;
    flags &= ~TF_BIT8;
    RegisterEncounterPopulation();
}

// PSX: Activate__8Humanoid (HUMANOID.CPP:760)
void Humanoid::Activate() {
    MARKFUNCTION(0x80063210);
    // PSX: save prior activated state, then call base
    bool wasActivated = (flags & TF_ACTIVATED) != 0;
    Thing::Activate();
    // PSX: if newly activated (wasn't before, is now), insert into FightingCollision
    bool isActivated = (flags & TF_ACTIVATED) != 0;
    if (!wasActivated && isActivated) {
        FightingCollision::InsertHumanoid(this);
    }
}

// PSX: Deactivate__8Humanoid (HUMANOID.CPP:776)
void Humanoid::Deactivate() {
    MARKFUNCTION(0x80063270);
    Thing::Deactivate();
    if ((flags & TF_ACTIVATED) == 0) {
        FightingCollision::RemoveHumanoid(this);
    }
}

// PSX: Move__8Humanoid (HUMANOID.CPP:1544)
void Humanoid::Move() {
    MARKFUNCTION(0x80064100);
    DynamicThing::Move();
    HandleAnimationControl();

    World* world = g_game ? g_game->GetWorld() : nullptr;
    if (world) {
        world->CheckThingSwitches(this);
    }
}

// PSX: RestorePositionFromBip01__8Humanoid (HUMANOID.CPP:1681)
s32 Humanoid::RestorePositionFromBip01() {
    MARKFUNCTION(0x800643B8);

    if (!model) {
        return 0;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim || !anim->flip) {
        return 0;
    }

    anim->flip->SetFrame(0);
    anim->flip->UpdateJoints();

    if (!m->drawable) {
        return 0;
    }

    OriginalSTree* source = GetActiveSTree(m->drawable);
    STreeData* skeleton = source ? source->skeleton : nullptr;
    const STreeJoint* joint = GetBip01Joint(skeleton);
    if (!joint) {
        return 0;
    }

    LVector local = {};
    local.x = joint->translationX;
    local.y = joint->translationY;
    local.z = joint->translationZ;

    LVector worldOffset = {};
    GetObjectToWorldSpaceVector(local, worldOffset);

    s32 nextX = homePos.x;
    s32 nextY = homePos.y - worldOffset.y;
    s32 nextZ = homePos.z;
    if (((flags2 >> 6) & 1) == 0) {
        nextX -= worldOffset.x;
        nextZ -= worldOffset.z;
    }

    pos.x = nextX;
    pos.y = nextY;
    pos.z = nextZ;
    homePos.x = nextX;
    homePos.y = nextY;
    homePos.z = nextZ;
    ResetRenderInterpolation();

    if (((flags2 >> 6) & 1) != 0) {
        return nextY;
    }
    return nextX;
}

// PSX: CheckForLedges2__8HumanoidR9_RMVECT16R10tagLVectorl (HUMANOID.CPP:6730)
bool Humanoid::CheckForLedges2(LVector& outNormal, LVector& outCorrectionPos, s32 clearance) {
    MARKFUNCTION(0x8006A3B0);

    LVector startPos = pos;
    LVector endPos = startPos;
    endPos.x += (s32)(((s64)384 * rmSin16(orientation.y)) >> 16);
    endPos.z += (s32)(((s64)384 * rmSin16((s16)(orientation.y + 0x4000))) >> 16);

    u16 outMaterial = 0;
    return CollisionSector::LedgePrototype(
        startPos,
        endPos,
        startPos.y + 100,
        startPos.y + 600,
        outNormal,
        outCorrectionPos,
        outMaterial,
        clearance);
}

// PSX: PrepareLedgeLatch__8HumanoidPC10tagLVectorPC9_RMVECT16 (HUMANOID.CPP:6581)
void Humanoid::PrepareLedgeLatch(const LVector& correctionPos, const LVector& normal) {
    MARKFUNCTION(0x80069FEC);

    velocity = {};
    contactForce = {};
    DropPickup(1, 1);

    s32 facingAngle = 0;
    if (normal.x != 0) {
        if (normal.z == 0) {
            facingAngle = (normal.x > 0) ? 0xC000 : 0x4000;
        }
        else {
            facingAngle = (0x4000 - (s32)rmATan216((f32)(-normal.x), (f32)(-normal.z))) & 0xFFFF;
        }
    }
    else {
        facingAngle = (normal.z > 0) ? 0x8000 : 0;
    }

    orientation.y = facingAngle;

    s32 offsetX = (s16)((s64)rmSin16((s16)facingAngle) >> 9);
    s32 offsetZ = (s16)((s64)rmSin16((s16)(facingAngle + 0x4000)) >> 9);

    pos.x = correctionPos.x + offsetX;
    pos.y = correctionPos.y;
    pos.z = correctionPos.z + offsetZ;
    homePos = pos;
    ResetRenderInterpolation();
}

// PSX: CheckForLedges__8Humanoid (HUMANOID.CPP:6644)
bool Humanoid::CheckForLedges() {
    MARKFUNCTION(0x8006A1D8);

    if (velocity.y > 0) {
        return false;
    }

    LVector startPos = pos;
    LVector endPos = startPos;
    endPos.x += (s32)(((s64)LEDGE_TRACE_DISTANCE * rmSin16(orientation.y)) >> 16);
    endPos.z += (s32)(((s64)LEDGE_TRACE_DISTANCE * rmSin16((s16)(orientation.y + 0x4000))) >> 16);

    LVector ledgeNormal = {};
    LVector ledgePos = {};
    u16 ledgeMaterial = 0;
    if (!CollisionSector::LedgePrototype(
            startPos,
            endPos,
            startPos.y + LEDGE_TRACE_MIN_Y,
            startPos.y + LEDGE_TRACE_MAX_Y,
            ledgeNormal,
            ledgePos,
            ledgeMaterial,
            LEDGE_TRACE_CLEARANCE)) {
        return false;
    }

    if (Obstacle::DetectObstacleAboveLedge(ledgeNormal, ledgePos)) {
        return false;
    }

    s32 floorDelta = 0x2000;
    Model* trackedModel = model ? static_cast<Model*>(model) : nullptr;
    if (trackedModel && trackedModel->field36) {
        const ModelFloorHeightState* floorState = GetModelFloorHeightState(trackedModel);
        if (floorState->current != (s32)0x80000001) {
            floorDelta = ledgePos.y - floorState->current;
        }
    }

    if (floorDelta < LEDGE_FLOOR_MIN_HEIGHT) {
        return false;
    }

    SetActionState(AS_LEDGE_LATCH, 0);
    if (humanoidSound) {
        humanoidSound->Grab((CSoundMaterial)ledgeMaterial);
    }

    Land();
    PrepareLedgeLatch(ledgePos, ledgeNormal);
    return true;
}

// PSX: CheckForPickup__8Humanoid (HUMANOID.CPP:6081, 0x800697C4)
s32 Humanoid::CheckForPickup() {
    MARKFUNCTION(0x800697C4);
    // PSX: only tries to acquire when right hand is free.
    if (rightHandObj) {
        return 0;
    }

    if (!g_ai) {
        return 0;
    }

    Pickup* pickup = static_cast<Pickup*>(g_ai->GetPickupWithinReach(this));
    if (!pickup) {
        return 0;
    }

    // PSX: bail out if the pickup animation for this model is not resident.
    if (!model) {
        return 0;
    }
    HumanoidModel* hm = static_cast<HumanoidModel*>(model);
    if (!hm->IsAnimationLoaded(static_cast<s32>(pickup->GetPickupMove()))) {
        return 0;
    }

    // PSX removes from inactivePickupList, stores owner pointer in pickup, clears combat flag,
    // plays GrabWeapon SFX, then enters AS_PICKUP.
    ccMinNode* removed = g_ai->inactivePickupList.RemNode(static_cast<ccMinNode*>(pickup));
    if (!removed) {
        return 0;
    }

    Pickup* heldPickup = static_cast<Pickup*>(removed);
    heldPickup->attachedOwner = this;
    rightHandObj = heldPickup;
    combatFlag = 0;

    if (thingType == AITypes::TT_BUTCH) {
        LOG("[ChefPot] CheckForPickup success owner=%p pickup=%p name='%s' type=%u hash=0x%08X",
            this,
            heldPickup,
            heldPickup->GetName(),
            heldPickup->thingType,
            heldPickup->GetNameCRC());
    }

    if (humanoidSound) {
        humanoidSound->GrabWeapon();
    }

    SetActionState(AS_PICKUP, 0);
    return 1;
}

// PSX: CheckforPickup__8HumanoidUl (HUMANOID.CPP:6138, 0x80069894)
s32 Humanoid::CheckforPickup(u32 pickupHash) {
    MARKFUNCTION(0x80069894);

    if (rightHandObj) {
        return 0;
    }

    if (!g_ai) {
        return 0;
    }

    ccList* sourceList = &g_ai->inactivePickupList;
    bool fromInactiveList = true;
    ccNode* pickupNode = sourceList->FindNodeCRC(pickupHash, nullptr);
    if (!pickupNode) {
        sourceList = &g_ai->pickupList;
        fromInactiveList = false;
        pickupNode = sourceList->FindNodeCRC(pickupHash, nullptr);
        if (!pickupNode) {
            if (thingType == AITypes::TT_BUTCH) {
                LOG("[ChefPot] CheckforPickup fail owner=%p hash=0x%08X (not found)", this, pickupHash);
            }
            return 0;
        }
    }

    Pickup* pickup = static_cast<Pickup*>(sourceList->RemNode(static_cast<ccMinNode*>(pickupNode)));
    if (!pickup) {
        if (thingType == AITypes::TT_BUTCH) {
            LOG("[ChefPot] CheckforPickup fail owner=%p hash=0x%08X (remove failed)", this, pickupHash);
        }
        return 0;
    }

    if (thingType == AITypes::TT_BUTCH) {
        LOG("[ChefPot] CheckforPickup success owner=%p hash=0x%08X source=%s pickup=%p name='%s' type=%u",
            this,
            pickupHash,
            fromInactiveList ? "inactive" : "active",
            pickup,
            pickup->GetName(),
            pickup->thingType);
    }

    pickup->CreateModel(nullptr);
    pickup->attachedOwner = this;
    SetRightHandObj(pickup);
    return 1;
}

// PSX: SetRightHandObj__8HumanoidP6Pickup (HUMANOID.CPP:6190, 0x8006D0CC)
s32 Humanoid::SetRightHandObj(Pickup* pickup) {
    MARKFUNCTION(0x8006D0CC);

    rightHandObj = pickup;
    flags2 |= 1u;
    if (!pickup) {
        return 0;
    }

    return pickup->SetupPickup(this, 2);
}

// PSX: CreateModel__8HumanoidPCc (HUMANOID.CPP:795, 0x80063248)
// PSX: creates HumanoidModel if not exists, creates Behaviour, then calls Thing::CreateModel
void Humanoid::CreateModel(const char* name) {
    MARKFUNCTION(0x800632B4);

    // PSX: if model == null, create HumanoidModel(136)
    if (!model) {
        HumanoidModel* hm = new HumanoidModel();
        model = hm;
        hm->backPtr = this;
    }

    // PC: display-only models get gameplay state on promotion.
    if (!pcVisualModelOnly) {
        CreateGameplayState();
    }

    // PSX: calls Thing::CreateModel which does the LevelManager lookup
    Thing::CreateModel(name);

    // PSX: ApplyAnimToModel(thingType, 0, 2, 0, 0) then InitSemiTransMode
    Model* m = static_cast<Model*>(model);
    SModel* sm = dynamic_cast<SModel*>(m);
    if (m && !sm && s_nonSTreeHumanoidModelBudget > 0) {
        LOG("[Humanoid::CreateModel] Non-STree model attached to humanoid thingType=%u name=%s drawableType=%d",
            thingType,
            GetName() ? GetName() : "null",
            m->drawableType);
        s_nonSTreeHumanoidModelBudget--;
    }

    if (sm) {
        m->ApplyAnimToModel(thingType, 0, 2, 0, 0);
        sm->SetupModelCallbacks();
        m->SetAnim(22, 0, 1, 0);
        sm->InitSemiTransMode();
    }

    // PSX CreateModel transitions into field364 state when set, else AS_STAND_ANIM.
    if (field364 != 0) {
        SetActionState((u32)field364, 0);
    }
    else {
        SetActionState(AS_STAND_ANIM, 0);
    }

    // PSX CreateModel restores the cached spawn-facing transform after the
    // initial animation/state setup.
    orientation = spawnOrientation;
    faceAngle = spawnOrientation.y;

    if (sm) {
        sm->scale = GetCharSubTypeScale(characterSubType);

        if (m->drawableType == 2 && m->drawable) {
            DrawableSTree* streeDrawable = static_cast<DrawableSTree*>(m->drawable);
            const s16 suitIndex = static_cast<s16>(GetCharSubTypeClut(characterSubType));
            if (s_humanoidSuitTraceBudget > 0) {
                LOG("[HumanoidSuit] thingType=%u name=%s charSubType=%d suit=%d drawableType=%d",
                    thingType,
                    GetName() ? GetName() : "null",
                    characterSubType,
                    static_cast<s32>(suitIndex),
                    m->drawableType);
                s_humanoidSuitTraceBudget--;
            }
            streeDrawable->ChangeSuit(suitIndex);
        }
    }

    // PSX: vtable+212 call -> CreateSound
    CreateSound();
}

// PC: Bookkeeping for authored and generated encounter members.
void Humanoid::RegisterEncounterPopulation() {
    if (thingType == AITypes::TT_PLAYER || field452 == 0 || !activeZone ||
        !IsActiveZoneStillRegistered(activeZone)) {
        return;
    }

    if (!encounterPopulationRegistered) {
        activeZone->RegisterEncounterHumanoid(this);
        encounterPopulationRegistered = true;
        encounterPopulationResolved = false;
    }
    else if (encounterPopulationResolved) {
        activeZone->RegisterEncounterHumanoid(this);
        encounterPopulationResolved = false;
    }
}

void Humanoid::ResolveEncounterPopulation() {
    if (!encounterPopulationRegistered || encounterPopulationResolved) {
        return;
    }

    if (activeZone && IsActiveZoneStillRegistered(activeZone)) {
        activeZone->ResolveEncounterHumanoid(this);
    }
    encounterPopulationResolved = true;
}

void Humanoid::UnregisterEncounterPopulation() {
    if (!encounterPopulationRegistered) {
        return;
    }

    ResolveEncounterPopulation();
    encounterPopulationRegistered = false;
}

// PSX: the zone-member and Behaviour part of CreateModel__8HumanoidPCc (0x800632B4).
void Humanoid::CreateGameplayState() {
    if (field452 != 0 && activeZone && IsActiveZoneStillRegistered(activeZone)) {
        activeZone->AddHumanoidToOverlordMembers(this);
    }

    if (!behaviour) {
        behaviour = new Behaviour(this, thingType, field452);
        if (field444 != 0) {
            behaviour->handlerThisOffset = 0;
            behaviour->handlerDispatch = -1;
            behaviour->handler = (spawnCount != 0) ? Behaviour::Idle : Behaviour::NDMS;
        }
        field444 = 1;
    }
}

// PSX: the zone-member and Behaviour part of DeleteModel__8Humanoid (0x80063514).
void Humanoid::DestroyGameplayState() {
    // Host safety: AI teardown can clear activeZoneList before humanoids are destroyed.
    if (field452 != 0 && activeZone && IsActiveZoneStillRegistered(activeZone)) {
        activeZone->RemoveHumanoidFromOverlordMembers(this);
    }

    // PC: the player's Behaviour is never rebuilt.
    if (behaviour && this != Player::s_player) {
        spawnCount = (behaviour->handlerDispatch == -1 && behaviour->handler == Behaviour::Idle) ? 1 : 0;
        delete behaviour;
        behaviour = nullptr;
    }
}

void Humanoid::OnVisualModelPromoted() {
    CreateGameplayState();
}

void Humanoid::OnVisualModelDemoted() {
    DestroyGameplayState();
    flags2 &= ~8u;

    Model* m = static_cast<Model*>(model);
    if (m) {
        m->SetAnim(22, 0, 1, 0);
    }
    SetActionState((field364 != 0) ? (u32)field364 : (u32)AS_STAND_ANIM, 0);
    orientation = spawnOrientation;
    faceAngle = spawnOrientation.y;
}

// PSX: DeleteModel__8Humanoid (HUMANOID.CPP:910)
void Humanoid::DeleteModel() {
    MARKFUNCTION(0x80063514);
    const bool wasVisualOnly = pcVisualModelOnly;
    Thing::DeleteModel();

    if (!wasVisualOnly) {
        DestroyGameplayState();
    }

    // PSX: SetActionState(0, 0), then clear flags2 bit 3.
    if (!pcDestroying && this != Player::s_player) {
        SetActionState(AS_INACTIVE_IDLE, 0);
        flags2 &= ~8u;
    }

    ReleaseSound();
}

// PSX: DeleteRightHandObj__8Humanoid (HUMANOID.CPP:6202, 0x8006D070)
void Humanoid::DeleteRightHandObj() {
    if (rightHandObj) {
        delete rightHandObj;
        rightHandObj = nullptr;
        flags2 &= ~1;
    }
}

// PSX: DeleteLeftHandObj__8Humanoid (HUMANOID.CPP:6225, 0x8006D014)
void Humanoid::DeleteLeftHandObj() {
    if (leftHandObj) {
        delete leftHandObj;
        leftHandObj = nullptr;
        flags2 &= ~2;
    }
}

// PSX: DropPickup__8Humanoidii (HUMANOID.CPP:7819, 0x8006B6A0)
void Humanoid::DropPickup(s32 dropRight, s32 dropLeft) {
    MARKFUNCTION(0x8006B6A0);
    if (dropRight) {
        if (rightHandObj) {
            Pickup* pickup = static_cast<Pickup*>(rightHandObj);
            if (pickup->weaponField == 0) {
                ccList* pickupList = g_ai ? &g_ai->pickupList : nullptr;
                pickup->Release(this, pickupList, nullptr, 0);
                rightHandObj = nullptr;
                flags2 &= ~1u;
            }
        }
    }
    if (dropLeft) {
        if (leftHandObj) {
            Pickup* pickup = static_cast<Pickup*>(leftHandObj);
            if (pickup->weaponField == 0) {
                ccList* pickupList = g_ai ? &g_ai->pickupList : nullptr;
                pickup->Release(this, pickupList, nullptr, 0);
                leftHandObj = nullptr;
                flags2 &= ~2u;
            }
        }
    }
}

// PSX: CreateSound__8Humanoid (HUMANOID.CPP:888, 0x800634C4)
void Humanoid::CreateSound() {
    MARKFUNCTION(0x800634C4);
    if (humanoidSound) {
        return;
    }
    CSound* tmp = nullptr;
    s32 result = CSoundFactory::CreateObject(10060, &tmp, thingType);
    LOG("[Humanoid] CreateSound type=%u result=%d ptr=%p", thingType, result, tmp);
    if (result >= 0) {
        humanoidSound = static_cast<CHumanoidSound*>(tmp);
        humanoidSound->Initialize(&pos, this);
    }
}

// PSX: ReleaseSound__8Humanoid (HUMANOID.CPP:952, 0x80063614)
void Humanoid::ReleaseSound() {
    MARKFUNCTION(0x80063614);
    if (humanoidSound) {
        humanoidSound->Release();
        humanoidSound = nullptr;
    }
}

// PSX: HandleCollisionReactionStates__8Humanoidll (HUMANOID.CPP:1734)
void Humanoid::HandleCollisionReactionStates(s32 hitType, s32 impactRegion) {
    MARKFUNCTION(0x80064528);

    switch (actionState) {
        case AS_PAUSE:
        case AS_JUMP:
        case AS_FALL:
        case AS_FLIP:
        case 33:
        case 35:
            SetActionState((hitType == 18) ? 46 : AS_FLYING_BACK, 0);
            DropPickup(1, 1);
            break;

        case AS_BACK_GRAB_LATCH:
        case AS_BACK_GRAB:
            contactForce = {};
            break;

        case AS_GOT_HIT_FREEFORM:
        case AS_FLYING_BACK:
        case AS_FLYING_BACK_LAND:
        case AS_THROW_FREE_FALL:
            SetActionState(AS_GOT_HIT_FREEFORM, 0);
            break;

        case AS_SPIN_BACK:
        case AS_THROW_CHARACTER_RECEIVE:
            return;

        case AS_BACK_GRAB_RECEIVE_PRE_LATCH:
        case AS_BACK_GRAB_RECEIVE_LATCH:
        case AS_BACK_GRAB_RECEIVE:
            contactForce = {};
            SetActionState(((impactRegion >> 4) & 1) ? AS_STUN_REACT_PUNCH : AS_STUN_REACT_KICK, 0);
            break;

        default:
            switch (hitType) {
                case 3:
                case 12:
                case 17:
                    maxFallDivisor = GetFlyingBackFallDivisor();
                    SetActionState(AS_FLYING_BACK, 0);
                    DropPickup(1, 1);
                    break;

                case 5:
                case 11:
                case 14:
                case 15:
                    SetActionState(AS_SPIN_BACK, 0);
                    DropPickup(1, 1);
                    break;

                case 8:
                case 13:
                    SetActionState(AS_STUNNED, 0);
                    DropPickup(1, 1);
                    break;

                default:
                    if (hitType == 2 && !humanoidData) {
                        maxFallDivisor = GetFlyingBackFallDivisor();
                        SetActionState(AS_FLYING_BACK, 0);
                        DropPickup(1, 1);
                        break;
                    }

                    if ((impactRegion & 1) == 0) {
                        SetActionState(((impactRegion >> 4) & 1) ? AS_HIT_REACT_COMBO_A : AS_HIT_REACT_COMBO_B, 0);
                    }
                    else if ((impactRegion >> 4) & 1) {
                        SetActionState(AS_HIT_REACT_PUNCH_A, 0);
                    }
                    else if (((impactRegion >> 5) & 1) || ((impactRegion >> 6) & 1)) {
                        SetActionState(AS_HIT_REACT_KICK_A, 0);
                    }
                    break;
            }
            break;
    }
}

#if NEW_CHEATS
static constexpr s32 ONE_PUNCH_LAUNCH_FORCE = 10000;
static constexpr s32 ONE_PUNCH_LAUNCH_LIFT = 6000;
#endif

// PSX: HandleCollision__8HumanoidP5Thingle (HUMANOID.CPP:1997)
void Humanoid::HandleCollision(Thing* other, s32 damage, ...) {
    MARKFUNCTION(0x80064808);
    if (!other) {
        return;
    }

    s32 impactRegion = 17;
    s32 hitType = 1;
    const LVector* impulse = nullptr;
    s32 forceMagnitude = 0;
    s32 hitPoints = 0;

    va_list args;
    va_start(args, damage);
    while (true) {
        const s32 tag = va_arg(args, s32);
        if (tag == COLLISION_TAG_END) {
            break;
        }
        switch (static_cast<u32>(tag)) {
            case static_cast<u32>(COLLISION_TAG_IMPACT_REGION):
                impactRegion = va_arg(args, s32);
                break;
            case static_cast<u32>(COLLISION_TAG_HIT_TYPE):
                hitType = va_arg(args, s32);
                break;
            case static_cast<u32>(COLLISION_TAG_IMPULSE):
                impulse = va_arg(args, const LVector*);
                break;
            case static_cast<u32>(COLLISION_TAG_FORCE):
                forceMagnitude = va_arg(args, s32);
                break;
            case static_cast<u32>(COLLISION_TAG_DAMAGE):
                hitPoints = va_arg(args, s32);
                break;
            default:
                (void)va_arg(args, s32);
                break;
        }
    }
    va_end(args);

    if (hitPoints == 0) {
        hitPoints = damage;
    }

    const bool otherIsHumanoid = other->thingType < static_cast<u16>(AITypes::TT_HUMANOID_LAST + 1);
    if (actionState == AS_COUNTER_ATTACK_LATCH && otherIsHumanoid && (other->flags & TF_BIT1) == 0) {
        SetHumanoidTarget(static_cast<Humanoid*>(other));
        SetActionState(AS_COUNTER_ATTACK, 0);
        return;
    }

    if ((flags & TF_BIT1) != 0 && otherIsHumanoid) {
        return;
    }

    const s32 oldState = actionState;

    ReleaseTarget();
    KillDialog(1, 0, 55);
    field484 = 0;
    field488 = 0;

    if (oldState == AS_JUMP || oldState == AS_PAUSE) {
        maxFallDivisor = 18;
        velocity = {};
        contactForce = {};
    }

    if (oldState == AS_STUNNED && animControl != 0) {
        animControl = 0;
    }

    if (oldState == AS_PICKUP && (flags2 & 1) == 0) {
        DropPickup(1, 1);
    }

    if (forceMagnitude != 0) {
        SVector forceDir = {};
        forceDir.x = static_cast<s16>(orientation.x);
        forceDir.y = 0;
        forceDir.z = static_cast<s16>(orientation.y);
        forceDir.pad = 0;
        AddForce(-forceMagnitude, &forceDir);
    }

    if (impulse) {
        contactForce.x += impulse->x;
        contactForce.y += impulse->y;
        contactForce.z += impulse->z;
    }

    HandleCollisionReactionStates(hitType, impactRegion);
    HandleCollisionSound(hitType);

    if (hitPoints != 0) {
        s32 appliedDamage = hitPoints;
        if (oldState == 53) {
            appliedDamage = static_cast<s32>((39321LL * static_cast<s64>(appliedDamage)) >> 16);
        }

#if NEW_CHEATS
        bool onePunchKill = false;
#endif

        if (this != (Humanoid*)Player::s_player) {
#if NEW_CHEATS
            const bool playerMelee = other == static_cast<Thing*>(Player::s_player)
                && hitType >= 1 && hitType <= 5;
            if (playerMelee && IsCheatEnabled(CheatOption::OnePunchMan)) {
                appliedDamage = static_cast<s32>(health);
                onePunchKill = appliedDamage > 0;
            }
#endif
            bool setFoe = other->thingType >= 0x191u && other->thingType < 0x1D9u;
            if (!setFoe && other->thingType == AITypes::TT_PLATFORM) {
                setFoe = ((other->flags2 >> 13) & 1) != 0;
            }

            if (setFoe && g_hud) {
                g_hud->SetFoe(this);
            }
        }

        if (appliedDamage > 0) {
            u32 clampedDamage = static_cast<u32>(appliedDamage);
            if (clampedDamage > 0xFFFFu) {
                clampedDamage = 0xFFFFu;
            }
            SubtractHitPoints(static_cast<u16>(clampedDamage));
            HandleHitShock(hitType);

            if (appliedDamage > DROP_PICKUP_DAMAGE_THRESHOLD && rightHandObj) {
                Pickup* heldPickup = static_cast<Pickup*>(rightHandObj);
                heldPickup->field308 = (this == static_cast<Humanoid*>(Player::s_player)) ? 1 : 0;
                DropPickup(1, 1);
            }

#if NEW_CHEATS
            if (onePunchKill) {
                Effects* burst =
                    GEffect_Create(HUMANOID_LAND_IMPACT_EFFECT_HASH, &pos, nullptr, nullptr, 0, 0x19, 0);
                if (!burst) {
                    LOG(
                        "[EffectsParity] OnePunchMan effect create miss hash=%08X\n",
                        HUMANOID_LAND_IMPACT_EFFECT_HASH);
                }

                // Send the victim flying away along its own facing, same
                // AS_FLYING_BACK reaction a heavy environmental hit would cause.
                SVector launchDir = {};
                launchDir.z = static_cast<s16>(orientation.y);
                AddForce(-ONE_PUNCH_LAUNCH_FORCE, &launchDir);
                contactForce.y += ONE_PUNCH_LAUNCH_LIFT;

                maxFallDivisor = GetFlyingBackFallDivisor();
                SetActionState(AS_FLYING_BACK, 0);
            }
#endif
        }
    }
}

// PSX: HandleCollisionSound__8Humanoidl (HUMANOID.CPP:1978, 0x8006475C)
void Humanoid::HandleCollisionSound(s32 hitType) {
    MARKFUNCTION(0x8006475C);
    if (!humanoidSound) {
        return;
    }
    switch (hitType) {
        case 1:
        case 8:
            humanoidSound->PunchHit();
            break;
        case 2:
        case 3:
            humanoidSound->SuperPunch();
            break;
        case 4:
            humanoidSound->KickHit();
            break;
        case 5:
            humanoidSound->SuperKick();
            break;
        case 18:
            humanoidSound->HitByFireBlast();
            break;
    }
}

// PSX: AnalyzeMesh__8HumanoidP6DBRoot (HUMANOID.CPP:535)
void Humanoid::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x80062E54);
    Thing::AnalyzeMesh(root);

    if (!root) {
        return;
    }

    spawnPos = root->pos;
    spawnOrientation.x = root->field40;
    spawnOrientation.y = root->field44;
    spawnOrientation.z = root->field48;

    behaviourNameHash = 0;
    const char* activeZoneAttribName = nullptr;

    for (u32 index = 0; index < root->attribCount; index++) {
        const DBAttrib* attrib = root->GetAttribByIndex(index);
        if (!attrib) {
            continue;
        }

        switch (attrib->id) {
            case 0x0C:
            {
                const char* subtypeName = attrib->GetAttribString();
                const u32 subtypeHash = p3dHash(subtypeName);
                characterSubType = GetCharSubTypeEnumFromHashID((s32)subtypeHash);
                if (s_humanoidSuitTraceBudget > 0) {
                    LOG("[HumanoidSubtype] thingType=%u name=%s subtypeStr=%s hash=0x%08X enum=%d",
                        thingType,
                        root->GetName() ? root->GetName() : "null",
                        subtypeName ? subtypeName : "null",
                        subtypeHash,
                        characterSubType);
                    s_humanoidSuitTraceBudget--;
                }
                break;
            }
            case 0x0D:
            {
                const u16 hitPoints = (u16)attrib->value;
                maxHealth = hitPoints;
                health = hitPoints;
                break;
            }
            case 0x0E:
                activeZoneAttribName = attrib->GetAttribString();
                activeZone = g_ai
                    ? static_cast<ActiveZone*>(g_ai->activeZoneList.FindNodeCRC(p3dHash(attrib->GetAttribString())))
                    : nullptr;
                break;
            case 0x10:
                field452 = (s32)attrib->value;
                break;
            case 0x11:
                behaviourNameHash = p3dHash(attrib->GetAttribString());
                break;
            case 0x1C:
            {
                const char* pickupName = attrib->GetAttribString();
                const u32 pickupHash = p3dHash(pickupName);
                const s32 pickupBound = CheckforPickup(pickupHash);
                if (thingType == AITypes::TT_BUTCH) {
                    LOG("[ChefPot] AnalyzeMesh attrib1C name='%s' hash=0x%08X bound=%d rightHand=%p flags2=0x%08X",
                        pickupName ? pickupName : "",
                        pickupHash,
                        pickupBound,
                        rightHandObj,
                        flags2);
                }
                break;
            }
            case 0x1D:
                if (p3dHash(attrib->GetAttribString()) == p3dHash("AS_NISMode")) {
                    field364 = AS_NIS_MODE;
                    SetActionState(AS_NIS_MODE, 0);
                }
                break;
            case 0x1F:
                field384 = GetPreActiveIdle((s32)p3dHash(attrib->GetAttribString()));
                break;
            case 0x20:
                field388 = (s32)p3dHash(attrib->GetAttribString());
                break;
            case 0x21:
                field392 = (s32)p3dHash(attrib->GetAttribString());
                break;
            case 0x22:
                field396 = (s32)p3dHash(attrib->GetAttribString());
                break;
            case 0x23:
                field400 = (s32)p3dHash(attrib->GetAttribString());
                break;
            case 0x24:
                field404 = (s32)p3dHash(attrib->GetAttribString());
                break;
            case 0x25:
                field408 = (s32)attrib->value;
                break;
            case 0x28:
                field412 = 1;
                break;
            case 0x29:
                field416 = 1;
                break;
            default:
                break;
        }
    }

    const s16 subTypeHitPoints = GetCharSubTypeHitPoints(characterSubType);
    if (subTypeHitPoints != 0) {
        maxHealth = (u16)subTypeHitPoints;
        health = (u16)subTypeHitPoints;
    }

    const u32 subTypeBehaviourHash = GetBehaviourNameHash(characterSubType);
    if (subTypeBehaviourHash != 0) {
        behaviourNameHash = subTypeBehaviourHash;
    }

    RegisterEncounterPopulation();

    if (thingType == 2) {
        s32 activeZonePathCount = 0;
        s32 activeZoneSubZoneCount = 0;
        if (activeZone) {
            for (ccMinNode* node = activeZone->pathList.head; node; node = node->next) {
                activeZonePathCount++;
            }
            for (ccMinNode* node = activeZone->subZoneList.head; node; node = node->next) {
                activeZoneSubZoneCount++;
            }
        }

        LOG("[Humanoid::AnalyzeMesh] type=%u name=%s activeZoneAttr=%s activeZone=%s paths=%d subZones=%d behaviour=0x%08X preActiveIdle=%d",
            thingType,
            root->GetName() ? root->GetName() : "null",
            activeZoneAttribName ? activeZoneAttribName : "null",
            activeZone && activeZone->GetName() ? activeZone->GetName() : "null",
            activeZonePathCount,
            activeZoneSubZoneCount,
            behaviourNameHash,
            field384);
    }
}


void Humanoid::Teleport(const LVector& newPos) {
    u16 bn = g_blockManager->GetBlockNumberAllBlocks(newPos);
    if (bn != BLOCK_UNASSIGNED) {
        blockNum = bn;
    }

    pos = newPos;
    homePos = newPos;
    velocity = {};
    contactForce = {};
    UpdatePosition();
    g_blockManager->DemandLoading();
    g_game->GetWorld()->CheckThingSwitches(this);
    ResetRenderInterpolation();
}

// PSX: SubtractHitPoints__8HumanoidUs (HUMANOID.CPP:9388, 0x8006CEB4)
s32 Humanoid::SubtractHitPoints(u16 hitPoints) {
    MARKFUNCTION(0x8006CEB4);

    const u16 healthBefore = health;

#if NEW_CHEATS
    if (this == static_cast<Humanoid*>(Player::s_player)
        && IsCheatEnabled(CheatOption::GodMode)) {
        health = maxHealth;
        if (g_hud) g_hud->UpdateFoe(this);
        return health;
    }
#endif

    if (DebugUI::IsHumanoidDamageDisabled()) {
        if (g_hud) {
            g_hud->UpdateFoe(this);
        }
        return health;
    }

    if (hitPoints >= health) {
        health = 0;
    }
    else {
        health = static_cast<u16>(health - hitPoints);
    }

    if (g_hud) {
        g_hud->UpdateFoe(this);
    }

    return health;
}

// PSX: SetActionState__8HumanoidUll (HUMANOID.CPP:2792)
// PSX: 5580 bytes, 74-case switch. Each case sets up animation, flags, and the
// method thunk (stateDispatch) that ProcessAction uses to call the state handler.
// On PC, we set stateDispatch to the vtable index corresponding to the handler.
void Humanoid::SetActionState(u32 state, s32 param) {
    MARKFUNCTION(0x80065680);

    s32 prevState = actionState;
    (void)prevState;

    // PSX preamble: clear combatFlag, set flags bit 11, end sounds
    combatFlag = 0;
    flags |= TF_DYNAMIC;
    if (humanoidSound) {
        humanoidSound->EndAllSounds();
    }

    if (state >= AS_COUNT) return;

    bool handled = true;

    // Map state number to handler dispatch index
    // PSX uses a 74-entry jump table; here we map the known cases.
    switch (state) {
        case AS_INACTIVE_IDLE:
            // PSX case 0: clear dispatch tuple and play anim 1 when model exists.
            field344 = 0;
            stateDispatch = SD_NONE;
            field348 = 0;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(1, param, 0, 0);
            }
            break;
        case AS_STAND:
            // PSX case 1: idle setup uses SetIdleAnimation and clears movement bits.
            field344 = 0;
            stateDispatch = SD_STAND;
            field348 = 8;
            SetIdleAnimation(param, 1);
            flags2 &= ~0x70u;
            break;
        case AS_STAND_ANIM:
            // PSX case 2: pre-active stand anim (field384) else idle 22.
            field344 = 0;
            stateDispatch = SD_STAND;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                const s32 standAnim = (field384 != 0) ? field384 : 22;
                m->SetAnim(standAnim, param, 0, 0);
                AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                if (anim) {
                    anim->SetLoopType(ANIM_LOOP, 1);
                }
            }
            flags2 &= ~0x70u;
            break;
        case AS_TAUNT_ENTRY:
        {
            // PSX Humanoid case 4: taunt entry (dispatch slot 23), not dive-roll.
            field344 = 0;
            stateDispatch = SD_DIVE_ROLL;
            field348 = 8;

            if (PlayDialogBasedOnPriority(0, 55) != 0) {
                flags2 |= 0x80u;
            }

            if (model) {
                Model* m = static_cast<Model*>(model);
                s32 animEnum = (field316 != 0) ? field316 : HUMANOID_ANIM_DIVE_ROLL;
                m->SetAnim(animEnum, param, 0, 0);
                AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                if (anim) {
                    anim->SetLoopType(ANIM_LOOP, 1);
                }
            }

            flags2 &= ~0x70u;
            break;
        }
        case AS_TAUNT_PAUSE:
            // PSX case 5: only reinitialize when transitioning into the state.
            if (prevState != static_cast<s32>(AS_TAUNT_PAUSE)) {
                field344 = 0;
                stateDispatch = SD_PAUSE;
                field348 = 8;

                if (model) {
                    Model* m = static_cast<Model*>(model);
                    const s32 holdAnim = (field316 != 0) ? field316 : 22;
                    m->SetAnim(holdAnim, param, 0, 0);
                    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                    if (anim) {
                        anim->SetLoopType(ANIM_LOOP, 1);
                    }
                }

                flags2 &= ~0x70u;
            }
            break;
        case AS_PAUSE:             
            stateDispatch = SD_PAUSE; 
            break;
        case AS_JUMP:
            // PSX case 8: jump setup.
            field344 = 0;
            stateDispatch = SD_JUMP;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(HUMANOID_ANIM_JUMP, 0, 0, 0);
            }
            DoJump();
            break;
        case AS_WALL_JUMP:         
            stateDispatch = SD_WALLJUMP; 
            break;
        case AS_RUN:
            field344 = 0;
            stateDispatch = SD_RUN;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(HUMANOID_ANIM_RUN, param, 0, 0);
            }
            flags2 &= ~0x70u;
            break;
        case AS_STRAFE:
            field344 = 0;
            stateDispatch = SD_STRAFE;
            field348 = 8;
            break;
        case AS_DIVE_ROLL:
            field344 = 0;
            stateDispatch = SD_DIVE_ROLL_CHAIN;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(24, param, 1, 0);
                SetCallNextActionCallback(m);
            }
            field488 = 0;
            break;
        case AS_PUSH:
            // PSX case 22: direct callback TableThrow__8Humanoid.
            if (this == static_cast<Humanoid*>(Player::s_player)) {
                LoadDialog(83, 0x33);
            }
            flags2 |= 0x0001u;
            field344 = 0;
            stateDispatch = SD_PUSH;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(0x58, param, 0, 0);
            }
            break;
        case AS_LEDGE_LATCH:
            field344 = 0;
            stateDispatch = SD_LEDGE_LATCH;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(HUMANOID_ANIM_LEDGE_LATCH, 0, 0, 0);
            }
            break;
        case AS_LEDGE_PULLUP:
            flags2 &= ~0x70;
            field344 = 0;
            stateDispatch = SD_LEDGE_PULLUP;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(HUMANOID_ANIM_LEDGE_PULLUP, 0, 0, 0);
            }
            break;
        case AS_LADDER_CLIMB_DOWN:
        {
            // PSX case 25 (loc_800665C4): top latch entry, anim 0x122.
            const bool clearLatchBits = ((flags2 & 0x10) == 0) || ((flags2 & 0x20) != 0 && (flags2 & 0x40) != 0);
            if (clearLatchBits) {
                flags2 = (flags2 | 0x10) & ~0x60;
                field516 = 0;
                field520 = 0;
                field524 = 0;
            }
            stateDispatch = SD_LADDER_LATCH_TOP;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(0x122, 0, 0, 0);
            }
            break;
        }
        case AS_LADDER_CLIMB_UP:
            // PSX case 26 (loc_80066644): latch-on state, anim 0x123.
            stateDispatch = SD_LADDER_LATCH;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(0x123, 0, 0, 0);
            }
            break;
        case AS_LADDER_CLIMBING:
        {
            // PSX case 27 (loc_80066668): active climb.
            stateDispatch = SD_CLIMB_LADDER;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(0x123, 0, 0, 0);
            }
            flags &= ~TF_DYNAMIC;
            velocity = {};
            contactForce = {};
            DropPickup(1, 1);
            break;
        }
        case AS_LADDER_DISMOUNT:
            // PSX case 28 (loc_800666E8): dismount, anim 0x126.
            stateDispatch = SD_LADDER_DISMOUNT;
            flags2 &= ~0x70;
            flags |= TF_DYNAMIC;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(0x126, 0, 0, 0);
            }
            break;
        case AS_HOTFOOT:
            // PSX case 30: player-only hotfoot VO, anim 43, dispatch slot 34 (_Hotfoot).
            if (thingType == AITypes::TT_PLAYER) {
                LoadDialog(0x5B, 0x3C);
                PlayDialog(0x5B, 0x1E);
            }
            field344 = 0;
            stateDispatch = SD_COLLAPSE;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(43, param, 0, 0);
            }
            break;
        case AS_SLOPE_SLIDE:       
            stateDispatch = SD_SLOPE_SLIDE; 
            break;
        case AS_PUNCH_ATTACK:
        case AS_KICK_ATTACK:
            if (rightHandObj || leftHandObj) {
                const Pickup* heldPickup = rightHandObj
                    ? static_cast<const Pickup*>(rightHandObj)
                    : static_cast<const Pickup*>(leftHandObj);
                LOG(
                    "[PickupCombat] set state thingType=%u pickupType=%u nextState=%d cmd=0x%08X fromState=%d",
                    static_cast<u32>(thingType),
                    heldPickup ? static_cast<u32>(heldPickup->thingType) : 0u,
                    static_cast<s32>(state),
                    static_cast<u32>(commandBits),
                    actionState);
            }
            if (!EnterCombatCombo()) {
                return;
            }
            break;
        case AS_COMBAT_IDLE:
            if (rightHandObj || leftHandObj) {
                const Pickup* heldPickup = rightHandObj
                    ? static_cast<const Pickup*>(rightHandObj)
                    : static_cast<const Pickup*>(leftHandObj);
                LOG(
                    "[PickupCombat] set state thingType=%u pickupType=%u nextState=%d cmd=0x%08X fromState=%d",
                    static_cast<u32>(thingType),
                    heldPickup ? static_cast<u32>(heldPickup->thingType) : 0u,
                    static_cast<s32>(state),
                    static_cast<u32>(commandBits),
                    actionState);
            }
            if (!EnterCombatCombo()) {
                return;
            }
            velocity = {};
            contactForce = {};
            break;
        case AS_BACK_GRAB_LATCH:
            field344 = 0;
            stateDispatch = SD_BACK_GRAB_LATCH;
            field348 = 8;
            break;
        case AS_BACK_GRAB:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(79, 0, 0, 0);
                AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                if (anim) {
                    anim->SetLoopType(ANIM_LOOP, 1);
                }
            }
            field344 = 0;
            stateDispatch = SD_BACK_GRAB;
            field348 = 8;
            break;
        case AS_BACK_GRAB_RELEASE:
            ReleaseTarget();
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(9, 2, 0, 0);
            }
            field344 = 0;
            stateDispatch = SD_BACK_GRAB_RELEASE;
            field348 = 8;
            break;
        case AS_COUNTER_ATTACK_PRE_LATCH:
        {
            const s32 nextField484 = field488;
            field488 = 0;
            field484 = nextField484;

            const FightingComboNode* currentNode =
                ResolveFightingNodeAddressConst(static_cast<u32>(field484));
            const PsxFightingMoveRaw* move =
                (currentNode && currentNode->moveData) ? currentNode->moveData : nullptr;

            if (model) {
                Model* m = static_cast<Model*>(model);
                if (move) {
                    m->SetAnim(static_cast<s32>(move->anim), currentNode->field07, 1, currentNode->field06);

                    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                    if (anim) {
                        anim->speed = static_cast<s32>(move->firstWord);
                    }
                }
            }
            field344 = 0;
            stateDispatch = SD_COUNTER_ATTACK_PRE_LATCH;
            field348 = 8;
            break;
        }
        case AS_COUNTER_ATTACK_LATCH:
            field344 = 0;
            stateDispatch = SD_COUNTER_ATTACK_LATCH;
            field348 = 8;
            break;
        case AS_COUNTER_ATTACK:
            contactForce = {};
            field344 = 0;
            stateDispatch = SD_COUNTER_ATTACK;
            field348 = 8;
            break;
        case AS_COUNTER_ATTACK_RECOVERY:
            field344 = 0;
            stateDispatch = SD_COUNTER_ATTACK_RECOVERY;
            field348 = 8;
            break;
        case AS_PICKUP:
        {
            // PSX case 44 uses direct callback Pickup__8Humanoid.
            stateDispatch = SD_NONE;
            s32 pickupAnim = 44;
            if (rightHandObj) {
                Pickup* pickup = static_cast<Pickup*>(rightHandObj);
                pickup->SetPickupMove(pos.y + GRAB_HEIGHT);
                pickupAnim = static_cast<s32>(pickup->GetPickupMove());
            }
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(pickupAnim, 0, 0, 0);
            }
            break;
        }
        case AS_THROW_PICKUP:
            stateDispatch = SD_THROW;
            if (model && rightHandObj) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(static_cast<s32>(static_cast<Pickup*>(rightHandObj)->GetThrowMove()), param, 0, 0);
            }
            break;
        case 46:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(static_cast<s32>(punchDir) + 9, param, 0, 0);
                SetCallNextActionCallback(m);
            }
            punchDir = static_cast<u16>((punchDir + 1) % 3);
            walkCycleFlag = 1;
            field344 = 0;
            // PSX: stateDispatch=36 resolves to _GotHitHigh via vtable slot.
            stateDispatch = SD_GOT_HIT_HIGH;
            field348 = 8;
            orientation.x = 0;
            flags2 &= ~0x70u;
            break;
        case 47:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(static_cast<s32>(kickDir) + 10, param, 0, 0);
                SetCallNextActionCallback(m);
            }
            kickDir = static_cast<u16>((kickDir + 1) % 3);
            walkCycleFlag = 1;
            field344 = 0;
            // PSX: stateDispatch=37 resolves to _GotHitMed via vtable slot.
            stateDispatch = SD_GOT_HIT_MED;
            field348 = 8;
            orientation.x = 0;
            flags2 &= ~0x70u;
            break;
        case 49:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(static_cast<s32>(comboDir) + 11, param, 0, 0);
                SetCallNextActionCallback(m);
            }
            comboDir = static_cast<u16>((comboDir + 1) % 2);
            walkCycleFlag = 1;
            field344 = 0;
            // PSX: stateDispatch=36 resolves to _GotHitHigh via vtable slot.
            stateDispatch = SD_GOT_HIT_HIGH;
            field348 = 8;
            orientation.x = 0;
            flags2 &= ~0x70u;
            break;
        case 50:
            if (model) {
                Model* m = static_cast<Model*>(model);
                // PSX case 50 selects anim 12 and arms CallNextAction.
                m->SetAnim(12, param, 0, 0);
                SetCallNextActionCallback(m);
            }
            walkCycleFlag = 1;
            field344 = 0;
            // PSX: stateDispatch=37 resolves to _GotHitMed via vtable slot.
            stateDispatch = SD_GOT_HIT_MED;
            field348 = 8;
            orientation.x = 0;
            flags2 &= ~0x70u;
            break;
        case 51:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(static_cast<s32>(punchDir) + 9, param, 0, 0);
            }
            punchDir = static_cast<u16>((punchDir + 1) % 3);
            field344 = 0;
            stateDispatch = SD_STUNNED;
            field348 = 8;
            orientation.x = 0;
            flags2 &= ~0x70u;
            break;
        case 52:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(static_cast<s32>(kickDir) + 10, param, 0, 0);
            }
            kickDir = static_cast<u16>((kickDir + 1) % 3);
            field344 = 0;
            stateDispatch = SD_STUNNED;
            field348 = 8;
            orientation.x = 0;
            flags2 &= ~0x70u;
            break;
        case AS_GOT_HIT_FREEFORM:
            stateTimer = 0;
            field344 = 0;
            // PSX case 53 uses slot 40 in ProcessAction dispatch.
            stateDispatch = SD_GOT_HIT_FREEFORM;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(13, param, 1, 0);
            }
            velocity.y = 0;
            flags &= ~TF_ON_GROUND;
            if ((flags2 & 0x10u) == 0 || ((flags2 & 0x20u) != 0 && (flags2 & 0x40u) != 0)) {
                flags2 = (flags2 & ~0x70u) | 0x10u;
                field516 = 0;
                field520 = 0;
                field524 = 0;
            }
            break;
        case AS_SPIN_BACK:
            // PSX: when Jackie enters state 56, preload his "hurt" line
            // (dialog 28, priority 0x35) for playback during get-up.
            if (this == static_cast<Humanoid*>(Player::s_player)) {
                LoadDialog(0x1C, 0x35);
            }
            combatFlag = 0;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(3, param, 0, 0);
            }
            field344 = 0;
            stateDispatch = SD_SPIN_BACK;
            field348 = 8;
            if ((flags2 & 0x10u) == 0) {
                flags2 |= 0x30u;
                field516 = 0;
                field520 = 0;
                field524 = 0;
            }
            break;
        case AS_FLYING_BACK:
            // PSX: state 57 preloads the same player-only "hurt" line.
            if (this == static_cast<Humanoid*>(Player::s_player)) {
                LoadDialog(0x1C, 0x35);
            }
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(14, param, 1, 0);
            }
            stateTimer = 0;
            field344 = 0;
            stateDispatch = SD_FLYING_BACK;
            field348 = 8;
            flags &= ~TF_ON_GROUND;
            if ((flags2 & 0x10u) == 0 || ((flags2 & 0x20u) != 0 && (flags2 & 0x40u) != 0)) {
                flags2 = (flags2 & ~0x70u) | 0x10u;
                field516 = 0;
                field520 = 0;
                field524 = 0;
            }
            break;
        case AS_FLYING_BACK_LAND:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(15, param, 1, 0);
            }
            field344 = 0;
            stateDispatch = SD_FLOATING;
            field348 = 8;
            flags &= ~TF_ON_GROUND;
            if ((flags2 & 0x10u) == 0 || ((flags2 & 0x20u) != 0 && (flags2 & 0x40u) != 0)) {
                flags2 = (flags2 & ~0x70u) | 0x10u;
                field516 = 0;
                field520 = 0;
                field524 = 0;
            }
            break;
        case AS_STUNNED:
        {
            field468 = HUMANOID_STUN_DURATION;
            animControl = 0;
            LOG("[EffectsParity] Enter AS_STUNNED thingType=%u\n", static_cast<u32>(thingType));
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(4, param, 0, 0);

                HumanoidModel* hm = static_cast<HumanoidModel*>(m);
                AnimationMatrices* animMatrices = hm ? hm->animMatrices : nullptr;
                if (animMatrices) {
                    LVector effectPos = {};
                    const LVector* effectPosRef = nullptr;
                    u32 createFlags = 0x80000000u;

                    const s32* rootMatrix = animMatrices->GetMatrix(0u);
                    if (rootMatrix) {
                        // PSX passes matrix translation pointer (+0x14) directly when follow-pos is enabled.
                        effectPosRef = reinterpret_cast<const LVector*>(rootMatrix + 5);
                        createFlags |= 1u;
                    }
                    else {
                        LVector throwaway = {};
                        animMatrices->GetAttack(0u, throwaway, effectPos);
                        effectPosRef = &effectPos;
                    }

                    Effects* effect = GEffect_Create(
                        HUMANOID_STUN_EFFECT_HASH,
                        effectPosRef,
                        nullptr,
                        nullptr,
                        1,
                        0,
                        createFlags);
                    if (effect) {
                        // Keep PSX-style nonzero token semantics in the +224 s32 slot.
                        animControl = 1;
                    }
                    else {
                        animControl = 0;
                        LOG(
                            "[EffectsParity] AS_STUNNED effect create miss hash=%08X\n",
                            HUMANOID_STUN_EFFECT_HASH);
                    }
                }
            }
            field344 = 0;
            field348 = 8;
            stateDispatch = SD_STUNNED;
            flags2 &= ~0x70u;
            if (humanoidSound) {
                humanoidSound->BeginStun();
            }
            break;
        }
        case AS_THROW_CHARACTER_RECEIVE:
            combatFlag = 0;
            field344 = 0;
            stateDispatch = SD_THROW_CHARACTER_RECEIVE;
            field348 = 8;
            flags2 = (flags2 & ~0x70u) | 0x10u;
            field516 = 0;
            field520 = 0;
            field524 = 0;
            velocity = {};
            contactForce = {};
            field484 = 0;
            field488 = 0;
            break;
        case AS_BACK_GRAB_RECEIVE_PRE_LATCH:
            field344 = 0;
            stateDispatch = SD_BACK_GRAB_RECEIVE_PRE_LATCH;
            field348 = 8;
            break;
        case AS_BACK_GRAB_RECEIVE_LATCH:
            field344 = 0;
            stateDispatch = SD_BACK_GRAB_RECEIVE_LATCH;
            field348 = 8;
            break;
        case AS_BACK_GRAB_RECEIVE:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(80, 0, 0, 0);
                AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                if (anim) {
                    anim->SetLoopType(ANIM_LOOP, 1);
                }
            }
            field344 = 0;
            stateDispatch = SD_BACK_GRAB_RECEIVE;
            field348 = 8;
            break;
        case AS_THROW_FREE_FALL:
            combatFlag = 0;
            field344 = 0;
            stateDispatch = SD_THROW_FREE_FALL;
            field348 = 8;
            flags2 &= ~0x70u;
            break;
        case AS_GET_UP:
            flags2 &= ~0x70u;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(28, param, 0, 0);
            }
            field344 = 0;
            stateDispatch = SD_GET_UP;
            field348 = 8;
            break;
        case AS_COLLAPSE_STUN:
            DropPickup(1, 1);
            combatFlag = 0;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(17, param, 0, 0);
            }
            field344 = 0;
            stateDispatch = SD_COLLAPSE_RECOVER;
            field348 = 8;
            flags2 &= ~0x70u;
            break;
        case AS_FLYING_BACK_CHECK:
            field344 = 0;
            stateDispatch = SD_COLLAPSE_RECOVER;
            field348 = 8;
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(16, param, 1, 0);
            }
            break;
        case AS_SPIN_BACK_RECOVER:
            field344 = 0;
            stateDispatch = SD_COLLAPSE_RECOVER;
            field348 = 8;
            break;
        case AS_DEAD:
        {
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
                m->SetAnim(HUMANOID_ANIM_DEAD, param, 0, 0);
            }

            if (model) {
                Model* m = static_cast<Model*>(model);
                AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
                if (anim) {
                    anim->humanoidCB = {};
                }
            }

            field528 = 0;
            thinkCounter = 0;
            flags |= 0x0080;
            field344 = 0;
            stateDispatch = SD_DEAD;
            field348 = 8;
            break;
        }
        case AS_GOT_HIT_CRUSHER:
            if (model) {
                Model* m = static_cast<Model*>(model);
                m->SetAnim(HUMANOID_ANIM_CRUSHED, param, 0, 0);
            }
            health = 0;
            field344 = 0;
            stateDispatch = SD_GOT_HIT_CRUSHER;
            field348 = 8;
            break;
        case AS_NIS_MODE:
            field344 = 0;
            stateDispatch = SD_NIS_MODE;
            field348 = 8;
            break;
        case AS_HIT_EXPLOSION:     stateDispatch = SD_GOT_HIT_HIGH; break;
        case AS_HIT_ENVIRONMENT:   stateDispatch = SD_GOT_HIT_HIGH; break;
        default:
            handled = false;
            break;
    }

    if (!handled) {
        return;
    }

    actionState = (s32)state;
    stateTimer = 0;
    (void)param;
}

// PSX: ProcessAction__8Humanoid (HUMANOID.CPP:2659)
// PSX uses a method thunk at fields +344/+346/+348 to dispatch to the current
// state handler. On PC, we dispatch via stateDispatch (the vtable index).
void Humanoid::ProcessAction() {
    MARKFUNCTION(0x8006538C);

    // PSX case 44 uses a direct callback (field346=-1 + Pickup__8Humanoid).
    // Host preserves that behavior via action-state direct routing.
    if (actionState == AS_PICKUP) {
        _Pickup();
        return;
    }

    if (stateDispatch == SD_NONE) return;

    switch (stateDispatch) {
        case SD_STAND:        _Stand(); break;
        case SD_RUN:          _Run(); break;
        case SD_JUMP:         _Jump(); break;
        case SD_FALL:         _Fall(); break;
        case SD_DIVE_ROLL_CHAIN:
            // PSX slot 27 on Humanoid resolves to strafe-chain; Player handles slot 27 in Player::ProcessAction.
            _Straif();
            break;
        case SD_DIVE_ROLL:
            // PSX slot 23 on Humanoid resolves to taunt; Player handles slot 23 in Player::ProcessAction.
            _Taunt();
            break;
        case SD_STRAFE:       _Straif(); break;
        case SD_PAUSE:        _Pause(); break;
        case SD_GOT_HIT_HIGH: _GotHitHigh(); break;
        case SD_GOT_HIT_MED:  _GotHitMed(); break;
        case SD_GOT_HIT_LOW:  _GotHitLow(); break;
        case SD_COLLAPSE:     _Hotfoot(); break;
        case SD_COLLAPSE_RECOVER: _Collapse(); break;
        case SD_DEAD:         _Dead(); break;
        case SD_KILLED:       Killed(); break;
        case SD_SPIN_BACK:    _SpinBack(); break;
        case SD_FLYING_BACK:  _FlyingBack(); break;
        case SD_FLOATING:     _Floating(); break;
        case SD_STUNNED:      _Stunned(); break;
        case SD_PUSH:         _TableThrow(); break;
        case SD_GET_UP:       _CrouchUp(); break;
        case SD_THROW:        _Throw(); break;
        case SD_FIGHTING_COMBO: ProcessFightingComboNode(); break;
        case SD_BACK_GRAB_LATCH: BackGrabCharacterLatch(); break;
        case SD_BACK_GRAB: BackGrabCharacter(); break;
        case SD_BACK_GRAB_RELEASE: BackGrabCharacterRelease(); break;
        case SD_BACK_GRAB_RECEIVE_PRE_LATCH: BackGrabCharacterReceivePreLatch(); break;
        case SD_BACK_GRAB_RECEIVE_LATCH: BackGrabCharacterReceiveLatch(); break;
        case SD_BACK_GRAB_RECEIVE: BackGrabCharacterReceive(); break;
        case SD_COUNTER_ATTACK_PRE_LATCH: CounterAttackPreLatch(); break;
        case SD_COUNTER_ATTACK_LATCH: CounterAttackLatch(); break;
        case SD_COUNTER_ATTACK: CounterAttack(); break;
        case SD_COUNTER_ATTACK_RECOVERY: CounterAttackRecovery(); break;
        case SD_THROW_CHARACTER_RECEIVE: ThrowCharacterReceive(); break;
        case SD_THROW_FREE_FALL: ThrowFreeFall(); break;
        case SD_GOT_HIT_FREEFORM: GotHitFreeForm(); break;
        case SD_GOT_HIT_CRUSHER: GotHitCrusher(); break;
        case SD_DANTE_MISSILE_PREPARE:        _MissilePrepare(); break;
        case SD_DANTE_MISSILE_ATTACK:         _MissileAttack(); break;
        case SD_DANTE_TARGET_MISSILE_ATTACK:  _TargetMissileAttack(); break;
        case SD_LEDGE_LATCH:  _LedgeLatch(); break;
        case SD_LEDGE_PULLUP: _LedgePullup(); break;
        case SD_LADDER_LATCH_TOP: _LadderLatchTop(); break;
        case SD_LADDER_LATCH: _LadderLatch(); break;
        case SD_CLIMB_LADDER: _ClimbLadder(); break;
        case SD_LADDER_DISMOUNT: _LadderDismount(); break;
        case SD_NIS_MODE:     _NISMode(); break;
        case SD_BUTCH_STOMP:      _Stomp(); break;
        case SD_BUTCH_CHARGE:     _Charge(); break;
        case SD_BUTCH_THROW_POT:  _ThrowPot(); break;
        default: break;
    }
}

// PSX: SetTauntAnim__8Humanoidl (HUMANOID.CPP:2666)
void Humanoid::SetTauntAnim(s32 index) {
    MARKFUNCTION(0x800653F4);
    field316 = GetTauntAnim(index);
}

// PSX: ProcessControl__8Humanoid (HUMANOID.CPP:961)
void Humanoid::ProcessControl() {
    MARKFUNCTION(0x80063660);
    commandBits = 0;
    if (behaviour) {
        behaviour->Process();
    }
}

// PSX: Kill__8Humanoid (HUMANOID.CPP:9405)
void Humanoid::Kill() {
    MARKFUNCTION(0x8006CF00);

    if (field260 != 0) {
        field344 = 0;
        stateDispatch = SD_KILLED;
        field348 = 8;
        actionState = AS_DEAD;
        flags |= TF_BIT5;
        return;
    }

    flags |= TF_DEAD;
    ResolveEncounterPopulation();
}

// PSX: Killed__8Humanoid (HUMANOID.CPP:9440)
void Humanoid::Killed() {
    MARKFUNCTION(0x8006CF50);

    if (field260 != 0) {
        actionState = AS_DEAD;
        return;
    }

    flags |= TF_DEAD;
    ResolveEncounterPopulation();
}

// PSX: GetStraifPhase__8Humanoid (HUMANOID.CPP:9476, 0x8006CF7C)
s32 Humanoid::GetStraifPhase() {
    MARKFUNCTION(0x8006CF7C);

    if (actionState != AS_STRAFE) {
        return 0;
    }

    if (!model) {
        return 0;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    if (!anim) {
        return 0;
    }

    // PSX mapping from AnimStructure offsets used by GetStraifPhase:
    // +0x44 endFrame, +0x3E currentFrame high halfword, +0x2C loopTypeField, +0x54 loopCount.
    const s32 animWord44 = anim->endFrame;
    const s16 animHalf3E = static_cast<s16>(static_cast<u32>(anim->currentFrame) >> 16);
    const s32 animWord2C = anim->loopTypeField;
    const s32 animWord54 = anim->loopCount;

    const s32 animTop16 = animWord44 >> 16;
    const s32 animTop15Signed = animWord44 >> 17;

    if ((s32)animHalf3E == animTop15Signed) {
        return 2;
    }

    if (animWord2C == 0) {
        return (((s32)animHalf3E ^ animTop16) != 0) ? 1 : 0;
    }

    if (animWord2C == 1) {
        // PSX: keep reverse strafe active until it returns to frame 0;
        // only then use loopCount to decide whether another reverse pass runs.
        if (animHalf3E != 0) {
            return 1;
        }
        return (animWord54 < 2) ? 1 : 0;
    }

    return 0;
}

void Humanoid::RequestAction(u32 actionID) {
    MARKFUNCTION(0x8006CFFC);
    commandBits |= (1 << actionID);
}

// PSX: FaceThing__8HumanoidP5Thingi (HUMANOID.CPP:2252)
void Humanoid::FaceThing(Thing* target, s32 immediate) {
    MARKFUNCTION(0x80064B98);
    if (!target) return;
    LVector point = target->pos;
    FacePoint(point, immediate);
}

// PSX: FacePoint__8HumanoidRC10tagLVectori (HUMANOID.CPP:2260)
// Computes the angle from this->pos to point, then either snaps or gradually
// turns orientation.y towards it, limited by turnRate.
void Humanoid::FacePoint(const LVector& point, s32 immediate) {
    MARKFUNCTION(0x80064BD0);

    s32 dx = point.x - pos.x;
    s32 dz = point.z - pos.z;

    s32 targetAngle = 0;
    u32 quadrant = (u32)dx >> 31;
    if (dz < 0) {
        quadrant += 2;
    }

    if (quadrant == 1) {
        targetAngle = (s32)rmATan216((f32)-dx, (f32)dz) + 0xC000;
    }
    else if (quadrant >= 2) {
        if (quadrant < 4) {
            targetAngle = (s32)rmATan216((f32)-dz, (f32)-dx) + 0x8000;
        }
    }
    else {
        targetAngle = (s32)rmATan216((f32)-dx, (f32)dz) - 0x4000;
    }

    if (immediate == 0) {
        // Snap directly to target angle
        orientation.y = targetAngle;
        return;
    }

    // PSX FacePoint wraps with +/-0xFFFF before applying turn step.
    const s32 currentY = orientation.y;
    const s32 diff = WrapFacingTurnDelta(targetAngle - currentY);

    const s32 step = (s32)(s16)turnRate;
    if (diff >= 0) {
        if (diff >= step) {
            orientation.y = currentY + step;
        }
        else {
            orientation.y = targetAngle;
        }
    }
    else {
        if (-diff < step) {
            orientation.y = targetAngle;
        }
        else {
            orientation.y = currentY - step;
        }
    }
}

// PSX: FaceThingDesired__8HumanoidP5Thing (HUMANOID.CPP:2333)
bool Humanoid::FaceThingDesired(Thing* target) {
    MARKFUNCTION(0x80064D7C);
    if (!target) {
        return orientation.y == faceAngle;
    }
    return FacePointDesired(target->pos);
}

// PSX: FacePointDesired__8HumanoidRC10tagLVector (HUMANOID.CPP:2352)
// Computes desired facing angle only (faceAngle), without changing orientation.
bool Humanoid::FacePointDesired(const LVector& point) {
    MARKFUNCTION(0x80064DB4);

    s32 dx = point.x - pos.x;
    s32 dz = point.z - pos.z;

    s32 desired = 0;
    u32 quadrant = (u32)dx >> 31;
    if (dz < 0) {
        quadrant += 2;
    }

    if (quadrant == 1) {
        desired = (s32)rmATan216((f32)-dx, (f32)dz) + 0xC000;
    }
    else if (quadrant >= 2) {
        if (quadrant < 4) {
            desired = (s32)rmATan216((f32)-dz, (f32)-dx) + 0x8000;
        }
    }
    else {
        desired = (s32)rmATan216((f32)-dx, (f32)dz) - 0x4000;
    }

    faceAngle = desired;
    return orientation.y == faceAngle;
}

// PSX: SetIdleAnimation__8Humanoidli (HUMANOID.CPP:2717, 0x800654C4)
// Plays idle animation via model->SetAnim. If no weapon, plays anim 22.
// If weapon, plays weapon idle with optional transition.
void Humanoid::SetIdleAnimation(s32 loopType, s32 doTransition) {
    MARKFUNCTION(0x800654C4);

    if (!model) {
        return;
    }

    HumanoidModel* humanoidModel = static_cast<HumanoidModel*>(model);

    // PSX: checks rightHandObj (pickup pointer) for weapon idle
    if (!rightHandObj) {
        // No weapon: play standard idle (anim 22) via model->SetAnim
        humanoidModel->SetAnim(22, loopType, 0, 0);
        return;
    }

    Pickup* pickup = static_cast<Pickup*>(rightHandObj);
    const s32 weaponIdleAnim = pickup->idleAnim;
    const s32 transitionAnim = GetWeaponTransitionIdle(pickup);
    if (transitionAnim != 0 && doTransition != 0) {
        SetTransitionIdleAnim(humanoidModel, transitionAnim, weaponIdleAnim);
        return;
    }

    humanoidModel->SetAnim(weaponIdleAnim, loopType, 0, 0);

    AnimStructure* anim = static_cast<AnimStructure*>(humanoidModel->animStructure);
    if (anim) {
        anim->SetLoopType(ANIM_LOOP, 1);
    }
}

// PSX: TestIdleAnimation__8Humanoid (HUMANOID.CPP:2763, 0x80065618)
// Returns true if currently playing the idle animation (22 or weapon idle).
bool Humanoid::TestIdleAnimation() {
    MARKFUNCTION(0x80065618);

    if (!model) {
        return false;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return false;
    }

    const s32 curAnim = anim->animEnum;

    if (rightHandObj) {
        return curAnim == static_cast<Pickup*>(rightHandObj)->idleAnim;
    }

    if (leftHandObj) {
        return curAnim == static_cast<Pickup*>(leftHandObj)->idleAnim;
    }

    return curAnim == 22;
}

// PSX: FindFoe__8HumanoidUlli (HUMANOID.CPP:2446)
// Searches nearby humanoids within range for a combat target.
// Searches nearby humanoids within range for a combat target.
Humanoid* Humanoid::FindFoe(u32 range, s32 param, s32 immediate) {
    MARKFUNCTION(0x80064F94);

    Humanoid* best = nullptr;
    u32 bestDist = 0xFFFFFFFFu;

    for (ccMinNode* node = g_ai ? g_ai->humanoidList.head : nullptr; node; node = node->next) {
        Humanoid* h = static_cast<Humanoid*>(node);
        if (!h || h == this) {
            continue;
        }

        const s32 targetState = h->actionState;
        if ((u32)(targetState - 69) < 4u || targetState == 56) {
            continue;
        }
        if (targetState == AS_COMBAT_IDLE || targetState == AS_THROW_CHARACTER_RECEIVE
            || targetState == AS_BACK_GRAB_LATCH || targetState == AS_BACK_GRAB
            || targetState == AS_BACK_GRAB_RECEIVE_PRE_LATCH
            || targetState == AS_BACK_GRAB_RECEIVE_LATCH
            || targetState == AS_BACK_GRAB_RECEIVE) {
            continue;
        }
        if (!h->health) {
            continue;
        }

        const u32 dist = static_cast<u32>(DistanceFromPoint(h->pos));
        if (dist >= range || dist >= bestDist) {
            continue;
        }

        const s32 fieldAngle = immediate ? faceAngle : orientation.y;
        if (IsPointInFieldOf(h->pos, pos, fieldAngle, param, param)) {
            best = h;
            bestDist = dist;
        }
    }

    return best;
}

// PSX: SetTarget__8HumanoidP8Humanoid (HUMANOID.CPP:2502)
void Humanoid::SetTarget(Humanoid* target) {
    MARKFUNCTION(0x8006511C);
    if (target == (Humanoid*)this) {
        return;
    }

    const s32 currentState = actionState;
    if (currentState == AS_COMBAT_IDLE || currentState == AS_THROW_CHARACTER_RECEIVE
        || currentState == AS_BACK_GRAB_LATCH || currentState == AS_BACK_GRAB
        || currentState == AS_BACK_GRAB_RECEIVE_PRE_LATCH
        || currentState == AS_BACK_GRAB_RECEIVE_LATCH
        || currentState == AS_BACK_GRAB_RECEIVE
        || currentState == 51 || currentState == 52) {
        return;
    }

    SetHumanoidTarget(target);
}

// PSX: SetHumanoidTarget__8HumanoidP8Humanoid (HUMANOID.CPP:2535, 0x800651C0)
// Releases old target, sets new one with refcount.
void Humanoid::SetHumanoidTarget(Humanoid* target) {
    ReleaseTarget();
    if (target) {
        target->field260++;
    }
    field256 = reinterpret_cast<uintptr_t>(target);
}

// PSX: ReleaseTarget__8Humanoid (HUMANOID.CPP:2553)
void Humanoid::ReleaseTarget() {
    MARKFUNCTION(0x80065200);
    uintptr_t targetAddr = field256;
    if (targetAddr) {
        Humanoid* t = reinterpret_cast<Humanoid*>(targetAddr);
        if (t->field260 > 0) {
            t->field260--;
        }
        field256 = 0;
    }
}

// PSX: IsInActiveZone__8Humanoid (HUMANOID.CPP:2612, 0x80065230)
bool Humanoid::IsInActiveZone() const {
    MARKFUNCTION(0x80065230);

    return activeZone != nullptr
        && activeZone->box.IsValid()
        && activeZone->box.IsInside(pos);
}

// PSX: IsTargetInActiveZone__8Humanoid (HUMANOID.CPP:2630, 0x80065290)
bool Humanoid::IsTargetInActiveZone() const {
    MARKFUNCTION(0x80065290);

    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    return target != nullptr
        && activeZone != nullptr
        && activeZone->box.IsValid()
        && activeZone->box.IsInside(target->pos);
}

// PSX: QuickCheckWallCollision__8Humanoidllll (HUMANOID.CPP:8789, 0x8006C59C)
s32 Humanoid::QuickCheckWallCollision(s16 angle, s32 distance, s32 radius, s32 height) {
    MARKFUNCTION(0x8006C59C);

    s32 collisionRatio = 0;
    LVector wallNormal = {};
    LVector hitPoint = {};
    s32 verticalSpan = 0;
    s32 wallMaterial = 0;
    return CheckWallCollision(
        angle,
        distance,
        radius,
        height,
        collisionRatio,
        wallNormal,
        hitPoint,
        verticalSpan,
        wallMaterial);
}

// PSX: CheckWallCollision__8HumanoidllllRlR9_RMVECT16R10tagLVectorT5T5 (HUMANOID.CPP:8815, 0x8006C5E8)
s32 Humanoid::CheckWallCollision(
    s16 angle,
    s32 distance,
    s32 radius,
    s32 height,
    s32& outCollisionRatio,
    LVector& outWallNormal,
    LVector& outHitPoint,
    s32& outVerticalSpan,
    s32& outWallMaterial) {
    MARKFUNCTION(0x8006C5E8);

    LVector startPos = pos;
    LVector endPos = startPos;
    endPos.x = startPos.x + (s32)(((s64)rmSin16(angle) * distance) >> 16);
    endPos.z = startPos.z + (s32)(((s64)rmSin16((s16)(angle + 0x4000)) * distance) >> 16);

    LVector searchMin = {};
    LVector searchMax = {};

    if (startPos.x >= endPos.x) {
        searchMin.x = endPos.x - radius;
        searchMax.x = startPos.x + radius;
    }
    else {
        searchMin.x = startPos.x - radius;
        searchMax.x = endPos.x + radius;
    }

    if (startPos.y >= endPos.y) {
        searchMin.y = endPos.y;
        searchMax.y = startPos.y + height;
    }
    else {
        searchMin.y = startPos.y;
        searchMax.y = endPos.y + height;
    }

    if (startPos.z >= endPos.z) {
        searchMin.z = endPos.z - radius;
        searchMax.z = startPos.z + radius;
    }
    else {
        searchMin.z = startPos.z - radius;
        searchMax.z = endPos.z + radius;
    }

    Wall* wallArray[64] = {};
    s32 wallCount = CollisionSector::FillWorldWallArray(searchMin, searchMax, wallArray, 64);
    if (wallCount > 64) {
        wallCount = 64;
    }

    s32 result = CollisionSector::CheckArrayWallCollision(
        wallArray,
        wallCount,
        startPos,
        endPos,
        radius,
        0,
        height,
        1);

    outCollisionRatio = g_wallCollisionInfo.collisionRatio;
    outWallNormal = g_wallCollisionInfo.wallNormal;
    outHitPoint = g_wallCollisionInfo.hitPoint;
    outVerticalSpan = g_wallCollisionInfo.wallVerticalMax - g_wallCollisionInfo.wallVerticalMin;
    outWallMaterial = g_wallCollisionInfo.wallMaterial;
    return result;
}

// PSX: CheckDWOCollision__8Humanoidll (HUMANOID.CPP:8852, 0x8006C750)
s32 Humanoid::CheckDWOCollision(s16 angle, s32 distance) {
    MARKFUNCTION(0x8006C750);

    const s32 aheadX = pos.x + (s32)(((s64)rmSin16(angle) * distance) >> 16);
    const s32 aheadZ = pos.z + (s32)(((s64)rmSin16((s16)(angle + 0x4000)) * distance) >> 16);

    if (!g_ai) {
        return 0;
    }

    for (ccMinNode* node = g_ai->moveList.head; node; node = node->next) {
        Obstacle* obstacle = dynamic_cast<Obstacle*>(static_cast<Thing*>(node));
        if (!obstacle) {
            continue;
        }

        if ((obstacle->flags & TF_MODEL_CREATED) == 0) {
            continue;
        }

        if (!obstacle->GetPhysical()) {
            continue;
        }

        const s32 deltaX = aheadX - obstacle->pos.x;
        const s32 deltaZ = aheadZ - obstacle->pos.z;

        const s32 sinV = rmSin16((s16)(-obstacle->orientation.y));
        const s32 cosV = rmSin16((s16)(0x4000 - obstacle->orientation.y));

        const s32 localX = (s32)(((s64)cosV * deltaX) >> 16) + (s32)(((s64)sinV * deltaZ) >> 16);
        const s32 localZ = (s32)((-(s64)sinV * deltaX) >> 16) + (s32)(((s64)cosV * deltaZ) >> 16);

        if (localX < (s32)obstacle->collBox.minX - 128) {
            continue;
        }
        if (localX > (s32)obstacle->collBox.maxX + 128) {
            continue;
        }
        if (localZ < (s32)obstacle->collBox.minZ - 128) {
            continue;
        }
        if (localZ > (s32)obstacle->collBox.maxZ + 128) {
            continue;
        }

        return 1;
    }

    return 0;
}

// PSX: CheckWallConstraint__8HumanoidUlUllRlR10tagLVector (HUMANOID.CPP:8937, 0x8006C9B8)
s32 Humanoid::CheckWallConstraint(
    u32 minWallHeight,
    s32 distance,
    s32 minAngle,
    s32& outWallAngle,
    LVector& outHitPoint) {
    MARKFUNCTION(0x8006C9B8);

    s32 collisionRatio = 0;
    LVector wallNormal = {};
    s32 wallVerticalSpan = 0;
    s32 wallMaterial = 0;
    if (!CheckWallCollision(
            static_cast<s16>(orientation.y),
            distance,
            WALL_KICK_COLLISION_RADIUS,
            WALL_KICK_COLLISION_HEIGHT,
            collisionRatio,
            wallNormal,
            outHitPoint,
            wallVerticalSpan,
            wallMaterial)) {
        return 0;
    }

    if (minWallHeight >= static_cast<u32>(wallVerticalSpan)) {
        return 0;
    }

    s32 wallAngle = 0;
    if (wallNormal.x != 0) {
        if (wallNormal.z != 0) {
            s32 a = 0x4000 - static_cast<s32>(rmATan216((f32)(-wallNormal.x), (f32)(-wallNormal.z)));
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

    s32 diff = faceAngle - wallAngle;
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

    if (((wrapped < 0) ? -wrapped : wrapped) < minAngle) {
        return 0;
    }

    outWallAngle = wallAngle;
    return 1;
}

// PSX: HasEnemyTauntDialog__8Humanoid (HUMANOID.CPP:9350, 0x8006CE5C)
bool Humanoid::HasEnemyTauntDialog() {
    MARKFUNCTION(0x8006CE5C);

    if (!soundHandle) {
        return false;
    }

    if (!jcsValidateHandle(soundHandle)) {
        return false;
    }

    return soundParam < 10;
}

// PSX: FaceAngleY__8Humanoidli (HUMANOID.CPP:2402)
// Turns orientation.y toward the given angle, limited by turnRate.
// If immediate == 0: snap directly. Otherwise: gradual turn.
void Humanoid::FaceAngleY(s32 angle, s32 immediate) {
    MARKFUNCTION(0x80064EB0);

    if (immediate == 0) {
        orientation.y = angle;
        return;
    }

    const s32 currentY = orientation.y;
    const s32 diff = WrapFacingTurnDelta(angle - currentY);

    const s32 step = (s32)(s16)turnRate;
    if (diff >= 0) {
        if (diff >= step) {
            orientation.y = currentY + step;
        }
        else {
            orientation.y = angle;
        }
    }
    else {
        if (-diff < step) {
            orientation.y = angle;
        }
        else {
            orientation.y = currentY - step;
        }
    }
}

// PSX: ReturnMostSignificant32BitNumber__FUl (HUMANOID.CPP:3826)
// Returns the 1-based index of the highest set bit, or 0 if input is 0.
// PSX uses binary search: test top 16 bits, then 8, then 4, etc.
static s32 ReturnMostSignificant32BitNumber(u32 value) {
    MARKFUNCTION(0x80066C4C);
    if (value == 0) return 0;
    s32 result = 0;
    s32 shift = 16;
    while (shift > 0) {
        u32 upper = value >> shift;
        if (upper != 0) {
            result += shift;
            value = upper;
        }
        shift >>= 1;
    }
    return result;
}

// PSX: _Stand__8Humanoid (HUMANOID.CPP:3859)
// Dispatches input commands from commandBits via highest-bit priority.
void Humanoid::_Stand() {
    MARKFUNCTION(0x80066CA0);

    s32 cmd = ReturnMostSignificant32BitNumber((u32)commandBits);
    flags2 |= 0x0008; // ground sticking

    if (cmd < 1 || cmd > 31) return;

    switch (cmd) {
        case 1:
            FaceAngleY(faceAngle, 0);
            return;

        case 2:
            SetActionState(AS_RUN, ANIM_BLEND);
            return;

        case 3:
            SetActionState(AS_JUMP, 0);
            return;

        case 5:
            FaceAngleY(faceAngle, 0);
            SetActionState(AS_STRAFE, 0);
            return;

        case 6:
            SetActionState(AS_STRAFE, 0);
            return;

        case GA_GRAB:
        case GA_GRAB_FORWARD:
        case 16:
        case GA_GRAB_HELD:
            if (rightHandObj != 0 || leftHandObj != 0) {
                const s32 weaponField = rightHandObj ? static_cast<Pickup*>(rightHandObj)->weaponField : 0;
                if (weaponField != 0) {
                    SetActionState(AS_COMBAT_IDLE, 0);
                }
                else {
                    SetActionState(AS_THROW_PICKUP, 0);
                }
            }
            else {
                if (CheckForPickup() == 1) {
                    return;
                }
                SetActionState(AS_COMBAT_IDLE, 0);
            }
            return;

        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 20:
            SetActionState(AS_PUNCH_ATTACK, 0);
            return;

        case GA_GRAB_FWD_HELD:
        case 19:
            SetActionState(AS_COMBAT_IDLE, 0);
            return;

        // PSX (0x80066D10): SetActionState(this, 4, 0) -- vtable slot 0xE8 is
        // SetActionState, and 4 is AS_TAUNT_ENTRY, not AS_STRAFE.
        case 21:
            SetActionState(AS_TAUNT_ENTRY, 0);
            return;

        case 30:
            SetActionState(AS_HIT_EXPLOSION, 0);
            return;

        case 31:
            SetActionState(AS_HIT_ENVIRONMENT, 0);
            return;

        default:
            return;
    }
}

// PSX: _DiveRoll__8Humanoid (HUMANOID.CPP:3977)
// Frame-based dive roll: force on early frames, then command-gated exits.
void Humanoid::_DiveRoll() {
    MARKFUNCTION(0x80066E3C);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    // PSX reads currentFrame high word (+62), not low word.
    s16 frame = (s16)((u32)anim->currentFrame >> 16);
    u32 cb = (u32)commandBits;

    if (anim->loopCount > 0) {
        if ((cb >> 3) & 1) {
            SetActionState(AS_JUMP, 0);
            return;
        }
        if ((cb >> 4) & 1) {
            SetActionState(AS_PAUSE, 0);
            return;
        }
        if ((cb >> 5) & 1) {
            SetActionState(AS_DIVE_ROLL, 0);
            return;
        }
        if ((cb >> 2) & 1) {
            SetActionState(AS_RUN, 0);
            m->SetAnim(HUMANOID_ANIM_RUN, 0, 0, 0);
            return;
        }
        SetActionState(AS_STAND, 0);
        return;
    }

    if (frame < DIVE_ROLL_FORCE_END_FRAME) {
        SVector dir;
        dir.x = (s16)orientation.x;
        dir.y = 0;
        dir.z = (s16)orientation.y;
        dir.pad = 0;
        AddForce(DIVE_ROLL_FORCE, &dir);
    }

    if (frame >= DIVE_ROLL_JUMP_PAUSE_FRAME) {
        if ((cb >> 3) & 1) {
            SetActionState(AS_JUMP, 0);
        }
        else if ((cb >> 4) & 1) {
            SetActionState(AS_PAUSE, 0);
        }
    }

    if (frame >= DIVE_ROLL_RUN_STRAFE_FRAME) {
        if ((cb >> 2) & 1) {
            SetActionState(AS_RUN, 0);
            m->SetAnim(HUMANOID_ANIM_RUN, 0, 0, 0);
        }
        else if ((cb >> 5) & 1) {
            SetActionState(AS_DIVE_ROLL, 0);
        }
    }

    if (field488 != 0) {
        const FightingComboNode* nextNode =
            ResolveFightingNodeAddressConst(static_cast<u32>(field488));
        if (!nextNode) {
            field488 = 0;
            return;
        }

        if (frame < nextNode->field05) {
            return;
        }

        SetHumanoidTarget(FindFoe(DIVE_ROLL_FIGHT_DISTANCE, FIGHT_HALF_ANGLE, 0));
        SetCurrentFightingNode();
        return;
    }

    const FightingComboNode* diveRollKickRoot =
        ResolveFightingNodeAddressConst(PLAYER_DIVE_ROLL_KICK_ROOT_ADDRESS);
    field488 = FindSiblingWithRequestedCommand(diveRollKickRoot, static_cast<u32>(commandBits));
}

// PSX: _Taunt__8Humanoid (HUMANOID.CPP:4069)
// Wait for animation to complete, then dispatch commands.
void Humanoid::_Taunt() {
    MARKFUNCTION(0x8006710C);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    // PSX: wait for animation to complete (loopCount > 0)
    if (anim->loopCount == 0) {
        return;
    }

    s32 cmd = ReturnMostSignificant32BitNumber((u32)commandBits);
    if (cmd < 2 || cmd > 31) {
        SetActionState(AS_STAND, 0);
        return;
    }

    switch (cmd) {
        case 2:
            SetActionState(AS_RUN, ANIM_BLEND);
            return;
        case 6:
            SetActionState(AS_STRAFE, 0);
            return;
        case GA_GRAB:
        case GA_GRAB_FORWARD:
        case 16:
        case GA_GRAB_HELD:
            if (rightHandObj != 0 || leftHandObj != 0) {
                const s32 weaponField = rightHandObj ? static_cast<Pickup*>(rightHandObj)->weaponField : 0;
                if (weaponField != 0) {
                    SetActionState(AS_COMBAT_IDLE, 0);
                }
                else {
                    SetActionState(AS_THROW_PICKUP, 0);
                }
            }
            else {
                if (CheckForPickup() == 1) {
                    return;
                }
                SetActionState(AS_COMBAT_IDLE, 0);
            }
            return;
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 20:
            SetActionState(AS_PUNCH_ATTACK, 0);
            return;
        case GA_GRAB_FWD_HELD:
        case 19:
            SetActionState(AS_COMBAT_IDLE, 0);
            return;
        case 21:
            FaceAngleY(faceAngle, 0);
            return;
        case 30:
            SetActionState(AS_HIT_EXPLOSION, 0);
            return;
        case 31:
            SetActionState(AS_HIT_ENVIRONMENT, 0);
            return;
        default:
            SetActionState(AS_STAND, 0);
            return;
    }
}

// PSX: Butch-only vtable slots; base Humanoid never reaches these (no-op).
void Humanoid::_Stomp() {
}

void Humanoid::_Charge() {
}

void Humanoid::_ThrowPot() {
}

// PSX: Dante-only vtable slots; base Humanoid never reaches these (no-op).
void Humanoid::_MissilePrepare() {
}

void Humanoid::_MissileAttack() {
}

void Humanoid::_TargetMissileAttack() {
}

// PSX: _Pause__8Humanoid (HUMANOID.CPP:4153)
// Simple counter decrement, then return to stand.
void Humanoid::_Pause() {
    MARKFUNCTION(0x80067288);

    FaceAngleY(faceAngle, 0);

    if (field324 != 0) {
        field324--;
    }
    else {
        SetActionState(AS_STAND, 0);
    }
}

// PSX: _Run__8Humanoid (HUMANOID.CPP:4172)
// Extensive bit dispatch for attack/move transitions.
void Humanoid::_Run() {
    MARKFUNCTION(0x800672EC);

    flags2 |= 0x0008; // ground sticking
    s32 sd = commandBits;

    // Strafe request (bit 6)
    if (sd & 0x0040) {
        SetActionState(AS_STRAFE, 0);
        return;
    }

    // Attack punch group (bits 8,10,12,20)
    if ((sd >> 8) & 1 || (sd >> 10) & 1 || (sd >> 12) & 1 || (sd >> 20) & 1) {
        SetActionState(AS_PUNCH_ATTACK, 0);
        return;
    }

    // Attack kick group (bits 9,11,13,14)
    if ((sd >> 9) & 1 || (sd >> 11) & 1 || (sd >> 13) & 1 || (sd >> 14) & 1) {
        SetActionState(AS_KICK_ATTACK, 0);
        return;
    }

    // Multi-hit combat (bits 7,15,16,17,18,19) -> pickup/throw or combat idle
    if (((sd >> GA_GRAB) & 1) || ((sd >> GA_GRAB_FORWARD) & 1)
        || ((sd >> 16) & 1)
        || ((sd >> GA_GRAB_HELD) & 1) || ((sd >> GA_GRAB_FWD_HELD) & 1)
        || ((sd >> 19) & 1)) {
        if (rightHandObj != 0 || leftHandObj != 0) {
            if (rightHandObj && static_cast<Pickup*>(rightHandObj)->weaponField != 0) {
                SetActionState(AS_COMBAT_IDLE, 0);
            }
            else {
                SetActionState(AS_THROW_PICKUP, 0);
            }
        }
        else {
            if (CheckForPickup() == 1) {
                return;
            }
            SetActionState(AS_COMBAT_IDLE, 0);
        }
        return;
    }

    // Taunt (bit 4) -> pause
    if (sd & 0x0010) {
        SetActionState(AS_PAUSE, 0);
        return;
    }

    // Punch (bit 3) -> jump
    if (sd & 0x0008) {
        SetActionState(AS_JUMP, 0);
        return;
    }

    // PSX (0x800674DC): vtable SetActionState(this, 4, ...) -- 4 is
    // AS_TAUNT_ENTRY, not AS_STRAFE.
    if (sd & 0x200000) {
        SetActionState(AS_TAUNT_ENTRY, 0);
        return;
    }

    // Guard (bit 1) -> stand
    if (sd & 0x0002) {
        SetActionState(AS_STAND, 0);
        return;
    }

    // Kick (bit 2) -> face + run forward
    if (sd & 0x0004) {
        FaceAngleY(faceAngle, 0);
        SVector dir;
        dir.x = (s16)orientation.x;
        dir.y = 0;
        dir.z = (s16)orientation.y;
        dir.pad = 0;
        AddForce(runSpeed, &dir);
        return;
    }

    // Explosion (bit 31)
    if (sd < 0) {
        SetActionState(AS_HIT_ENVIRONMENT, 0);
        return;
    }

    // Env hit (bit 30)
    if ((sd >> 30) & 1) {
        SetActionState(AS_HIT_EXPLOSION, 0);
        return;
    }

    // Strafe (bit 5) -> face and strafe
    if (sd & 0x0020) {
        FaceAngleY(faceAngle, 0);
        SetActionState(AS_STRAFE, 0);
    }
}

// PSX: _Straif__8Humanoid (HUMANOID.CPP:4307)
void Humanoid::_Straif() {
    MARKFUNCTION(0x80067610);

    // PSX: capture orientation.y and faceAngle at function entry.
    // FaceThingDesired/FaceAngleY modify these fields later, but the captured
    // values are used for movement direction and animation selection.
    s32 savedOrientY = orientation.y;
    s32 savedFaceAngle = faceAngle;

    // PSX: SVector direction = {0, 0, savedFaceAngle, 0} for AddForce
    SVector dir;
    dir.x = 0;
    dir.y = 0;
    dir.z = (s16)(savedFaceAngle & 0xFFFF);
    dir.pad = (s16)((u32)savedFaceAngle >> 16);

    // PSX: flags bit 17 -> slope slide
    if ((flags >> 17) & 1) {
        SetActionState(AS_SLOPE_SLIDE, 0);
        return;
    }

    s32 sd = commandBits;

    // Attack punch group (bits 8,10,12,20)
    if ((sd >> 8) & 1 || (sd >> 10) & 1 || (sd >> 12) & 1 || (sd >> 20) & 1) {
        // PSX: remap back-punch (bit 10) to punch (bit 8)
        if ((sd >> 10) & 1) {
            commandBits = (sd & 0xFFFFFAFF) | 0x100;
        }
        faceAngle = savedOrientY;
        ReleaseTarget();
        SetActionState(AS_PUNCH_ATTACK, 0);
        return;
    }

    // Attack kick group (bits 9,11,13,14)
    if ((sd >> 9) & 1 || (sd >> 11) & 1 || (sd >> 13) & 1 || (sd >> 14) & 1) {
        // PSX: remap back-kick (bit 11) to kick (bit 9)
        if ((sd >> 11) & 1) {
            commandBits = (sd & 0xFFFFF5FF) | 0x200;
        }
        faceAngle = savedOrientY;
        ReleaseTarget();
        SetActionState(AS_KICK_ATTACK, 0);
        return;
    }

    // Grab/combat (bits 7,15,16,17,18,19)
    if (((sd >> GA_GRAB) & 1) || ((sd >> GA_GRAB_FORWARD) & 1)
        || ((sd >> 16) & 1)
        || ((sd >> GA_GRAB_HELD) & 1) || ((sd >> GA_GRAB_FWD_HELD) & 1)
        || ((sd >> 19) & 1)) {
        if (rightHandObj || leftHandObj) {
            Pickup* heldPickup = static_cast<Pickup*>(rightHandObj);
            if (!heldPickup->weaponField) {
                ReleaseTarget();
                SetActionState(AS_THROW_PICKUP, 0);
                return;
            }
        }
        else if (CheckForPickup() == 1) {
            ReleaseTarget();
            return;
        }
        ReleaseTarget();
        SetActionState(AS_COMBAT_IDLE, 0);
        return;
    }

    // Explosion (bit 31)
    if (sd < 0) {
        ReleaseTarget();
        SetActionState(AS_HIT_ENVIRONMENT, 0);
        return;
    }

    // Env hit (bit 30)
    if ((sd >> 30) & 1) {
        ReleaseTarget();
        SetActionState(AS_HIT_EXPLOSION, 0);
        return;
    }

    // Guard release (bit 1)
    if (sd & 0x0002) {
        faceAngle = savedOrientY;
        ReleaseTarget();
        SetActionState(AS_STAND, 0);
        SetIdleAnimation(0, 0);
        return;
    }

    // PSX (0x80067950): vtable SetActionState(this, 4, ...) -- 4 is
    // AS_TAUNT_ENTRY, not AS_STRAFE.
    if (sd & 0x200000) {
        ReleaseTarget();
        SetActionState(AS_TAUNT_ENTRY, 0);
        return;
    }

    // Taunt (bit 4) -> pause
    if (sd & 0x0010) {
        ReleaseTarget();
        SetActionState(AS_PAUSE, 0);
        return;
    }

    // Punch (bit 3) -> jump
    if (sd & 0x0008) {
        ReleaseTarget();
        SetActionState(AS_JUMP, 0);
        return;
    }

    // Kick (bit 2) -> face + run
    if (sd & 0x0004) {
        faceAngle = savedOrientY;
        ReleaseTarget();
        SetActionState(AS_RUN, 0);
        return;
    }

    // Strafe (bit 5) -> face + strafe
    if (sd & 0x0020) {
        FaceAngleY(faceAngle, 0);
        ReleaseTarget();
        SetActionState(AS_STRAFE, 0);
        return;
    }

    // PSX: resolve faceAngleData - use rightHandObj's if available (weapon strafe anims)
    s32* animArray = nullptr;
    if (rightHandObj) {
        Pickup* heldPickup = static_cast<Pickup*>(rightHandObj);
        animArray = const_cast<s32*>(heldPickup->moveStruct);
    }
    if (!animArray) {
        animArray = (s32*)faceAngleData;
    }
    if (!animArray) {
        animArray = s_humanoidStraif;
    }

    // PSX: face target if present
    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    if (target) {
        FaceThingDesired(target);
        FaceAngleY(faceAngle, 1);
        if (target->actionState == AS_DEAD) {
            ReleaseTarget();
        }
    }

    // PSX: model and animStructure (loaded unconditionally on PSX)
    Model* m = static_cast<Model*>(model);
    AnimStructure* animStruct = m ? (AnimStructure*)m->animStructure : nullptr;

    if (moveSpeed != 0) {
        // PSX: movement force uses captured faceAngle direction
        AddForce(moveSpeed, &dir);

        // PSX: ClipAngle360 wraps by +/-0xFFFF (not a 0x10000 bitmask).
        s32 angleDiff = GetRelativeAngle(savedOrientY, savedFaceAngle);

        // PSX: select strafe animation based on clipped angle ranges
        s32 animIndex;
        s32 loopType;

        if (angleDiff >= 24577 && angleDiff <= 40959) {
            // 135-225 degrees: back-side strafe
            animIndex = animArray[4];
            loopType = animArray[5];
        }
        else if (angleDiff >= 8193 && angleDiff < 24576) {
            // 45-135 degrees: side strafe
            animIndex = animArray[8];
            loopType = animArray[9];
        }
        else if (angleDiff > 40960 && angleDiff <= 57343) {
            // 225-315 degrees: backward strafe
            animIndex = animArray[6];
            loopType = animArray[7];
        }
        else {
            // 0-45 degrees or 315-360 degrees: forward strafe
            animIndex = animArray[2];
            loopType = animArray[3];
        }

        if (m) {
            m->SetAnim(animIndex, 0, 0, 0);
        }
        if (animStruct) {
            animStruct->SetLoopType(loopType, 0);
        }
    }
    else {
        // PSX: no movement - play idle when strafe anim loops back to start
        if (animStruct) {
            // PSX: *(__int16*)(animStruct + 62) == *(__int16*)(animStruct + 66)
            // = currentFrame.hi == startFrame.hi (animation looped back to start)
            bool looped = (s16)((u32)animStruct->currentFrame >> 16) == (s16)((u32)animStruct->startFrame >> 16);
            if (animStruct->animEnum != animArray[0] && looped) {
                if (m) {
                    m->SetAnim(animArray[0], 3, 0, 0);
                }
            }
        }
    }
}

// PSX: _Jump__8Humanoid (HUMANOID.CPP:4569)
// Apply forces, check air attack, then test landing/ledge transitions.
void Humanoid::_Jump() {
    MARKFUNCTION(0x80067DBC);

    flags2 |= 0x0008; // ground sticking
    Model* m = model ? static_cast<Model*>(model) : nullptr;
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;

    // If kick bit set (bit 2), apply directional jump
    if (commandBits & 0x0004) {
        gravity = FIX16_HALF;
        FaceAngleY(faceAngle, 1);
        SVector dir;
        dir.x = (s16)orientation.x;
        dir.y = 0;
        dir.z = (s16)orientation.y;
        dir.pad = 0;
        AddForce(runSpeed, &dir);
        AddForce(runSpeed >> 2, &dir);
    }

    const s16 frame = anim ? static_cast<s16>((u32)anim->currentFrame >> 16) : 0;

    // PSX gates jump kick/punch entry while currentFrame.hi <= jumpKickEntryFrame (2).
    if (frame <= HUMANOID_JUMP_KICK_ENTRY_FRAME
        && (((commandBits >> 8) & 1) || ((commandBits >> 9) & 1) || ((commandBits >> 14) & 1))) {
        commandBits = (commandBits | 0x4000) & ~0x0100 & ~0x0200;
        SetActionState(AS_PUNCH_ATTACK, 0);
        return;
    }

    CheckForLanding();
    CheckForLedges();
}

// PSX: CheckForLanding__8Humanoid (HUMANOID.CPP:6021, 0x80069688)
s32 Humanoid::CheckForLanding() {
    MARKFUNCTION(0x80069688);

    if ((flags & TF_ON_GROUND) == 0) {
        return 0;
    }

    Model* m = model ? static_cast<Model*>(model) : nullptr;
    const s32 previousState = actionState;

    if ((commandBits & 0x0004) != 0) {
        SetActionState(AS_RUN, 0);
        if (humanoidSound) {
            humanoidSound->Land((CSoundMaterial)field436);
        }
        if (m) {
            m->SetAnim(HUMANOID_ANIM_RUN_LAND, 0, 0, 0);
        }
        return 1;
    }

    SetActionState(AS_STAND, 0);
    if (m) {
        m->SetAnim((previousState == AS_FLIP) ? HUMANOID_ANIM_FLIP_LAND : HUMANOID_ANIM_HARD_LAND, 0, 0, 0);
    }
    if (humanoidSound) {
        humanoidSound->Land((CSoundMaterial)field436);
    }

    return 1;
}

// PSX: _Fall__8Humanoid (HUMANOID.CPP:4620)
// Empty function on PSX (8 bytes, just jr $ra + nop)
void Humanoid::_Fall() {
    MARKFUNCTION(0x80067F2C);
}

// PSX: DoJump__8Humanoid (HUMANOID.CPP:6267, 0x80069968)
void Humanoid::DoJump() {
    MARKFUNCTION(0x80069968);

    contactForce.y += distantTargetRange;
    flags &= ~TF_ON_GROUND;
}

// PSX: HandleLand__8Humanoidl (HUMANOID.CPP:6311, 0x80069AB4)
void Humanoid::HandleLand(s32 height) {
    MARKFUNCTION(0x80069AB4);

    if (height >= groundStandHeight) {
        return;
    }

    const s32 fallDamage = CalculateFallDamage((groundStandHeight - height) / 512);

    if (actionState != AS_NIS_MODE) {
        SubtractHitPoints(static_cast<u16>(fallDamage));
    }

    if (fallDamage > 0 && this == (Humanoid*)Player::s_player) {
        Shock(ShockEnum::SHOCK_8);
    }

    if (fallDamage > 0) {
        LOG("[EffectsParity] HandleLand fallDamage=%d thingType=%u\n", fallDamage, static_cast<u32>(thingType));
        Effects* landEffect =
            GEffect_Create(HUMANOID_LAND_IMPACT_EFFECT_HASH, &pos, nullptr, nullptr, 0, 0x19, 0);
        if (!landEffect) {
            LOG(
                "[EffectsParity] HandleLand effect create miss hash=%08X fallDamage=%d\n",
                HUMANOID_LAND_IMPACT_EFFECT_HASH,
                fallDamage);
        }
    }

    if (health == 0) {
        SetActionState(AS_DEAD, 0);
    }
}

// PSX: _DoStand__8Humanoid (HUMANOID.CPP:6274, 0x80069A04)
// Callback target used by AnimStructure::ProcessHumanoidCB selector 61.
void Humanoid::_DoStand() {
    MARKFUNCTION(0x80069A04);
    SetActionState(AS_STAND, 0);
}

// PSX: _DoRun__8Humanoid (HUMANOID.CPP:6280, 0x80069A34)
// Callback target used by AnimStructure::ProcessHumanoidCB selector 62.
void Humanoid::_DoRun() {
    MARKFUNCTION(0x80069A34);
    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    m->SetAnim(HUMANOID_ANIM_RUN, ANIM_BLEND, 0, 0);
}

// PSX: _Pickup__8Humanoid (HUMANOID.CPP:4959)
// Grab item at animation frame threshold.
void Humanoid::_Pickup() {
    MARKFUNCTION(0x80068508);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
    if (rightHandObj != 0 && frame >= static_cast<s16>(static_cast<Pickup*>(rightHandObj)->GetPickupMoveGrabFrame())) {
        flags2 |= 0x0001;
        static_cast<Pickup*>(rightHandObj)->SetupPickup(this, 2);
    }

    if (anim->loopCount > 0) {
        SetActionState(AS_STAND, 0);
    }
}

// PSX: Hotfoot__8Humanoid (HUMANOID.CPP:4644, 0x80067F54)
void Humanoid::_Hotfoot() {
    MARKFUNCTION(0x80067F54);

    FaceAngleY(faceAngle, 1);

    SVector dir = {};
    dir.x = (s16)orientation.x;
    dir.y = 0;
    dir.z = (s16)orientation.y;
    dir.pad = 0;

    if (((u32)commandBits >> 2) & 1u) {
        AddForce(runSpeed, &dir);
    }

    if (((u32)commandBits >> 6) & 1u) {
        AddForce(runSpeed, &dir);
    }

    if (((u32)commandBits >> 4) & 1u) {
        SetActionState(AS_PAUSE, 0);
    }

    if (((u32)commandBits >> 3) & 1u) {
        SetActionState(AS_JUMP, 0);
    }

    if ((((u32)field368 >> 3) & 1u) == 0) {
        SetActionState(AS_RUN, 0);
    }

    if (model) {
        Model* m = static_cast<Model*>(model);
        AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
        if (anim && anim->loopCount > 0) {
            SetActionState(AS_STAND, 0);
        }
    }

    if (health == 0) {
        SetActionState(AS_DEAD, 0);
    }
}

// PSX: _LadderLatchTop__8Humanoid (HUMANOID.CPP:6362, 0x80069B94)
void Humanoid::_LadderLatchTop() {
    MARKFUNCTION(0x80069B94);

    velocity = {};
    contactForce = {};
    maxFallDivisor = 0;

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (anim && anim->loopCount > 0) {
        flags2 &= ~0x70;
        SetActionState(AS_LADDER_CLIMBING, 0);
        RestorePositionFromBip01();
    }
}

// PSX: _LadderLatch__8Humanoid (HUMANOID.CPP:6393, 0x80069C2C)
void Humanoid::_LadderLatch() {
    MARKFUNCTION(0x80069C2C);

    const u32 f368 = static_cast<u32>(field368);
    velocity = {};
    contactForce = {};
    maxFallDivisor = 0;

    if (((f368 >> 1) & 1u) == 0) {
        SetActionState(AS_LADDER_DISMOUNT, 0);
    }

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (anim && anim->loopCount > 0) {
        SetActionState(AS_LADDER_CLIMBING, 0);
    }
}

// PSX: LadderDismount__8Humanoid (HUMANOID.CPP:6426, 0x80069CC8)
void Humanoid::_LadderDismount() {
    MARKFUNCTION(0x80069CC8);
    _Jump();
}

// PSX: ClimbLadder__8Humanoid (HUMANOID.CPP:6448, 0x80069CF8)
void Humanoid::_ClimbLadder() {
    MARKFUNCTION(0x80069CF8);

    s32 climbUp = 0;
    s32 slideDown = 0;

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);

    // PSX: check commandBits bit 2 (directional input active)
    if ((commandBits >> 2) & 1) {
        // wrap (orientation.y - faceAngle) into 0..0xFFFF range
        s32 angleDiff = orientation.y - faceAngle;
        while (angleDiff > 0xFFFF) {
            angleDiff -= 0xFFFF;
        }
        while (angleDiff < 0) {
            angleDiff += 0xFFFF;
        }

        climbUp = (u32)(angleDiff - 0x2000) > 0xC000;
        slideDown = (u32)(angleDiff - 0x6001) < 0x3FFF;
    }

    // clear velocity, contactForce, maxFallDivisor
    velocity = {};
    contactForce = {};
    maxFallDivisor = 0;

    // PSX: NIS override check - if player and director is running NIS ladder script,
    // force climbing behavior (skip dismount check)
    s32 nisOverride = 0;
    if (this == (Humanoid*)Player::s_player) {
        if (g_director && g_director->scriptState != 0) {
            if (g_director->codeSnipPtr == Director::GetNISLadder1Script()) {
                nisOverride = 1;
            }
        }
    }

    if (!nisOverride) {
        // dismount check: if not holding ladder input, jump off
        s32 shouldDismount = 0;
        if (!(field368 & 2) || (commandBits & 8) || (commandBits & 16)) {
            shouldDismount = 1;
        }
        if (shouldDismount) {
            SetActionState(AS_LADDER_DISMOUNT, 0);
            return;
        }
    }

    if (climbUp) {
        // end any slide sound
        if (humanoidSound) {
            humanoidSound->EndSlideDownLadder();
        }

        // switch to climb-up anim (292) if not already playing
        if (anim->animEnum != 292) {
            m->SetAnim(292, 0, 0, 0);
            anim->SetLoopType(ANIM_BLEND, 1);
            flags2 = (flags2 & ~0x70) | 0x50;
            field516 = 0;
            field520 = 0;
            field524 = 0;
        }

        anim->IncFrame();

        // play footstep at frames 10 and 2
        s16 frame = (s16)((u32)anim->currentFrame >> 16);
        if (frame == 10 || frame == 2) {
            if (humanoidSound) {
                humanoidSound->Footstep(CSoundMaterial(2));
            }
        }

    }
    else if (slideDown) {
        // begin slide sound
        if (humanoidSound) {
            humanoidSound->BeginSlideDownLadder();
        }

        // switch to slide-down anim (293) if not already playing
        if (anim->animEnum != 293) {
            m->SetAnim(293, 0, 0, 0);
            RestorePositionFromBip01();
            // clear bits 4,5,6
            flags2 &= ~0x70;
        }

        // slide down: decrease homePos.y
        homePos.y -= 64;
    }
}

// PSX: _LedgeLatch__8Humanoid (HUMANOID.CPP:6753, 0x8006A4C4)
void Humanoid::_LedgeLatch() {
    MARKFUNCTION(0x8006A4C4);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    s32 shouldPullUp = 0;
    if (anim && anim->loopCount > 0) {
        shouldPullUp = 1;
    }
    else if ((commandBits & (1 << GA_MOVE)) != 0) {
        shouldPullUp = 1;
    }

    if (shouldPullUp) {
        SetActionState(AS_LEDGE_PULLUP, 0);
    }
}

// PSX: _LedgePullup__8Humanoid (HUMANOID.CPP:6764, 0x8006A538)
void Humanoid::_LedgePullup() {
    MARKFUNCTION(0x8006A538);

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    if (anim && anim->loopCount > 0) {
        SetActionState(AS_STAND, 0);
    }

    if (collBboxMin.y < 0) {
        collBboxMin.y = 0;
    }
}

// PSX: TestAndSetRisingAttack__8Humanoid (HUMANOID.CPP:5438, 0x80068D38)
s32 Humanoid::TestAndSetRisingAttack() {
    MARKFUNCTION(0x80068D38);
    s32 result = field488;
    if (result == 0) {
        const u32 requested = static_cast<u32>(commandBits);
        const bool wantsRisingAttack =
            ((requested >> GA_PUNCH) & 1u) != 0
            || ((requested >> GA_KICK) & 1u) != 0
            || ((requested >> GA_GRAB) & 1u) != 0
            || ((requested >> GA_GRAB_FORWARD) & 1u) != 0
            || ((requested >> GA_GRAB_HELD) & 1u) != 0
            || ((requested >> GA_GRAB_FWD_HELD) & 1u) != 0;

        if (wantsRisingAttack) {
            const u32 remapped = (requested & RISING_ATTACK_CLEAR_MASK) | RISING_ATTACK_REMAP_BIT;
            commandBits = static_cast<s32>(remapped);
            result = FindSiblingWithRequestedCommand(
                static_cast<const FightingComboNode*>(defaultFightingSystem), remapped);
            field488 = result;
        }
    }

    return result;
}

// PSX: BackGrabCharacterLatch__8Humanoid (FIGHTANI.CPP:518, 0x8007DF28)
s32 Humanoid::BackGrabCharacterLatch() {
    MARKFUNCTION(0x8007DF28);

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    const s32 frame = anim ? static_cast<s16>((u32)anim->currentFrame >> 16) : 0;

    flags |= TF_BIT1;

    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    if (target) {
        target->SetHumanoidTarget(this);
    }

    if (frame == 1 && target) {
        if (target->model) {
            Model* targetModel = static_cast<Model*>(target->model);
            targetModel->SetAnim(78, 0, 0, 0);
        }
        target->SetActionState(AS_BACK_GRAB_RECEIVE_LATCH, 0);
    }

    const s32 loopCount = anim ? anim->loopCount : 0;
    if (loopCount > 0) {
        SetActionState(AS_BACK_GRAB, 0);
    }

    return loopCount;
}

// PSX: BackGrabCharacter__8Humanoid (HUMANOID.CPP:4777, 0x8006826C)
s32 Humanoid::BackGrabCharacter() {
    MARKFUNCTION(0x8006826C);

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    const s32 frame = anim ? static_cast<s16>((u32)anim->currentFrame >> 16) : 0;

    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    if (target) {
        target->SetHumanoidTarget(this);
    }
    else {
        SetActionState(AS_STAND, 0);
    }

    flags |= TF_BIT1;
    --field508;

    if (field488 != 0) {
        const FightingComboNode* nextNode =
            ResolveFightingNodeAddressConst(static_cast<u32>(field488));
        if (!nextNode) {
            field488 = 0;
            return 0;
        }
        if (frame >= nextNode->field05) {
            return SetCurrentFightingNode();
        }
        return (frame < nextNode->field05) ? 1 : 0;
    }

    const FightingComboNode* root =
        ResolveFightingNodeAddressConst(PLAYER_BACK_GRAB_KICK_ROOT_ADDRESS);
    field488 = FindSiblingWithRequestedCommand(root, static_cast<u32>(commandBits));
    return field488;
}

// PSX: BackGrabCharacterRelease__8Humanoid (HUMANOID.CPP:4828, 0x80068338)
s32 Humanoid::BackGrabCharacterRelease() {
    MARKFUNCTION(0x80068338);

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    if (anim) {
        const s32 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
        if (frame == BACK_GRAB_RELEASE_SPEED_FRAME) {
            anim->speed = rmDiv16i(anim->endFrame, BACK_GRAB_RECOVERY_START_FRAME << 16);
        }
    }

    const s32 loopCount = anim ? anim->loopCount : 0;
    if (loopCount > 0) {
        SetActionState(AS_STAND, 0);
    }

    return loopCount;
}

// PSX: BackGrabCharacterReceivePreLatch__8Humanoid (HUMANOID.CPP:4872, 0x800683C4)
s32 Humanoid::BackGrabCharacterReceivePreLatch() {
    MARKFUNCTION(0x800683C4);

    const u32 timer = static_cast<u32>(++stateTimer);
    if (timer >= static_cast<u32>(BACK_GRAB_RECEIVE_PRE_LATCH_FRAMES)) {
        SetActionState(AS_STAND, 0);
        return 0;
    }

    return 1;
}

// PSX: BackGrabCharacterReceiveLatch__8Humanoid (HUMANOID.CPP:4894, 0x80068410)
s32 Humanoid::BackGrabCharacterReceiveLatch() {
    MARKFUNCTION(0x80068410);

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    const s32 loopCount = anim ? anim->loopCount : 0;
    if (loopCount > 0) {
        SetActionState(AS_BACK_GRAB_RECEIVE, 0);
    }

    return loopCount;
}

// PSX: BackGrabCharacterReceive__8Humanoid (HUMANOID.CPP:4915, 0x80068460)
s32 Humanoid::BackGrabCharacterReceive() {
    MARKFUNCTION(0x80068460);

    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    if (!target) {
        SetActionState(AS_STAND, 0);
        return 0;
    }

    if (static_cast<s16>(target->field508) < 0) {
        SetActionState(AS_STAND, 0);
        target->SetActionState(AS_BACK_GRAB_RELEASE, 0);
        return 0;
    }

    const s32 result = static_cast<s32>(target->field508) - 1;
    if (((flags >> 1) & 1u) == 0) {
        target->field508 = static_cast<u16>(result);
    }

    return result;
}

// PSX: TestAndSetBackGrab__8Humanoid (FIGHTANI.CPP:565, 0x8007E014)
s32 Humanoid::TestAndSetBackGrab() {
    MARKFUNCTION(0x8007E014);

    const u32 requested = static_cast<u32>(commandBits);
    const u32 backGrabMask =
        (1u << GA_GRAB) | (1u << GA_GRAB_FORWARD) | (1u << GA_GRAB_HELD)
        | (1u << GA_GRAB_FWD_HELD) | (1u << GA_AI_DIVE_ROLL);
    if ((requested & backGrabMask) == 0) {
        return 0;
    }

    Humanoid* target = FightTargetAndThrowLatch(2);
    if (!target || target->humanoidData != nullptr) {
        return 0;
    }

    if ((u32)(GetRelativeAngle(orientation.y, target->orientation.y) - BACK_GRAB_MIN_RELATIVE_ANGLE)
        <= BACK_GRAB_RELATIVE_ANGLE_RANGE) {
        return 0;
    }

    SetHumanoidTarget(target);
    target = reinterpret_cast<Humanoid*>(field256);
    if (!target) {
        return 0;
    }
    target->SetHumanoidTarget(this);

    field508 = GRAB_STRENGTH;

    if (model) {
        Model* m2 = static_cast<Model*>(model);
        m2->SetAnim(77, 0, 0, 0);
    }

    SetActionState(AS_BACK_GRAB_LATCH, 0);
    target->SetActionState(AS_BACK_GRAB_RECEIVE_PRE_LATCH, 0);

    LVector local = { 0, 0, BACK_GRAB_ATTACH_Z };
    LVector world = {};
    GetObjectToWorldSpaceVector(local, world);
    target->homePos.x = pos.x + world.x;
    target->homePos.y = pos.y + world.y;
    target->homePos.z = pos.z + world.z;
    target->orientation.y = orientation.y;
    return 1;
}

// PSX: GotHitFreeForm__8Humanoid (HUMANOID.CPP:8682, 0x8006C3EC)
// AS_GOT_HIT_FREEFORM hang-time handler: hold fall divisor at 0, then resume
// flying-back handling once zeroGHangTime has elapsed.
void Humanoid::GotHitFreeForm() {
    MARKFUNCTION(0x8006C3EC);

    stateTimer += 1;
    if (stateTimer > s_zeroGHangTime) {
        _FlyingBack();
        return;
    }

    maxFallDivisor = 0;
}

void Humanoid::GotHitCrusher() {
    MARKFUNCTION(0x80068A14);

    // PSX: waits for the crush animation (HUMANOID_ANIM_CRUSHED) set in
    // SetActionState to finish - loopCount only goes nonzero once a
    // non-looping anim has played through - before actually dying.
    if (!model) {
        return;
    }
    AnimStructure* anim = static_cast<AnimStructure*>(static_cast<Model*>(model)->animStructure);
    if (anim && anim->loopCount != 0) {
        SetActionState(AS_DEAD, 0);
    }
}

// PSX: LetGoOfLedge__8Humanoid (HUMANOID.CPP:8735, 0x8006C478)
// Releases from ledge: repositions away from ledge face and transitions to fall.
s32 Humanoid::LetGoOfLedge() {
    MARKFUNCTION(0x8006C478);

    // PSX: check actionState == 23 and animEnum == 31 (LEDGE_LATCH)
    s32 ok = 0;
    if (actionState == (s32)AS_LEDGE_LATCH) {
        if (model) {
            Model* m = static_cast<Model*>(model);
            AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
            if (anim && anim->animEnum == 31) {
                ok = 1;
            }
        }
    }

    if (ok) {
        SetActionState(AS_FALL, 0);

        // PSX: reposition away from ledge face by -300 units along orientation
        s32 hx = homePos.x;
        s32 hy = homePos.y;
        s32 hz = homePos.z;
        s32 sinY = rmSin16(orientation.y);
        s32 cosY = rmSin16((s16)(orientation.y + 0x4000));
        s32 newX = hx + (s32)((-300LL * sinY) >> 16);
        s32 newY = hy - 850;
        s32 newZ = hz + (s32)((-300LL * cosY) >> 16);
        pos.x = newX;
        pos.y = newY;
        pos.z = newZ;
        homePos.x = newX;
        homePos.y = newY;
        homePos.z = newZ;
        ResetRenderInterpolation();
    }

    return ok;
}

// PSX: _NISMode__8Humanoid (HUMANOID.CPP:8770, 0x8006C564)
void Humanoid::_NISMode() {
    MARKFUNCTION(0x8006C564);

    flags &= ~TF_DYNAMIC;
    velocity = {};
    contactForce = {};
    maxFallDivisor = 0;
    groundStandHeight = homePos.y;

    if (model) {
        HumanoidModel* humanoidModel = static_cast<HumanoidModel*>(model);
        humanoidModel->field116 = 0;
    }
}

// PSX: _TableThrow__8Humanoid (HUMANOID.CPP:5061, 0x80068718)
// Faces target during startup frames, releases table/chair at frame threshold,
// then returns to stand when the throw animation loops.
void Humanoid::_TableThrow() {
    MARKFUNCTION(0x80068718);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    s16 frame = (s16)((u32)anim->currentFrame >> 16);

    // PSX: face target during first 6 frames.
    if (frame < 6 && field256 != 0) {
        Thing* target = reinterpret_cast<Thing*>(field256);
        FaceThing(target, 1);
    }

    if (frame >= static_cast<s16>(s_tableThrowReleaseFrame) && (flags2 & 0x0001u) != 0) {
        if (field496) {
            LVector throwOrientation = {};
            throwOrientation.y = orientation.y;

            switch (field496->thingType) {
                case AITypes::TT_TABLE:
                    static_cast<Table*>(field496)->Throw(60, 140, throwOrientation, pos);
                    break;
                case AITypes::TT_CHAIR:
                    static_cast<Chair*>(field496)->Throw(60, 140, throwOrientation, pos);
                    break;
                default:
                    break;
            }
            field496 = nullptr;
        }

        if (this == static_cast<Humanoid*>(Player::s_player)) {
            PlayDialog(83, 10);
        }
    }

    if (anim->loopCount > 0) {
        SetActionState(AS_STAND, 0);
    }
}

// PSX: _Throw__8Humanoid (HUMANOID.CPP:4998, 0x800685A8)
// Face target during early frames, release thrown object at animation
// frame threshold, transition to stand when animation completes.
void Humanoid::_Throw() {
    MARKFUNCTION(0x800685A8);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    s16 frame = (s16)((u32)anim->currentFrame >> 16);

    // PSX: face target during first 6 frames
    if (frame < 6 && field256 != 0) {
        Thing* target = reinterpret_cast<Thing*>(field256);
        FaceThing(target, 1);
    }

    Pickup* pickup = static_cast<Pickup*>(rightHandObj);
    const s16 throwFrame = pickup ? static_cast<s16>(pickup->GetThrowMoveThrowFrame()) : 0;
    if (pickup != nullptr && frame >= throwFrame) {
        ccList* pickupList = g_ai ? &g_ai->pickupList : nullptr;

        if ((flags2 & 0x0001) != 0) {
            if (this == static_cast<Humanoid*>(Player::s_player)) {
                PlayDialog(84, 10);
            }

            SVector throwDir = {};
            throwDir.x = static_cast<s16>(orientation.x);
            throwDir.y = 0;
            throwDir.z = static_cast<s16>(orientation.y);
            throwDir.pad = 0;

            pickup->field308 = 1;
            pickup->Release(
                this,
                pickupList,
                &throwDir,
                s_throwPickupReleaseForce);
            flags2 &= ~0x0001u;
        }
        else {
            pickup->field308 = 1;
            pickup->Release(this, pickupList, nullptr, 0);
        }

        rightHandObj = nullptr;
    }

    // PSX: if animation completed (loopCount > 0)
    if (anim->loopCount > 0) {
        ReleaseTarget();
        SetActionState(AS_STAND, 0);
    }
}

// PSX: _GotHitHigh__8Humanoid (HUMANOID.CPP:5114, 0x8006882C)
// First frame: force animation to specific global frame. Adjusts speed
// based on field466 knockback. Sets death state if health gone.
void Humanoid::_GotHitHigh() {
    MARKFUNCTION(0x8006882C);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    // PSX: on first frame (walkCycleFlag == 46), force specific global frame
    if (walkCycleFlag == 46) {
        // PSX: ForceFrame(gp+1856) - global value, use 0 as default
        anim->ForceFrame(0);
        walkCycleFlag = 1;
    }

    // PSX: if field466 (knockback speed) nonzero, adjust animation speed
    if (field466 != 0) {
        // PSX: speed = rmDiv16i(endFrame, field466 << 16)
        if (anim->endFrame != 0) {
            anim->speed = rmDiv16i(anim->endFrame, (s32)field466 << 16);
        }
    }

    // PSX: if health == 0, set walkCycleFlag to AS_DEAD (72)
    if (health <= 0) {
        walkCycleFlag = (s32)AS_DEAD;
    }
}

// PSX: _GotHitMed__8Humanoid (HUMANOID.CPP:5161, 0x800688B4)
// Adjusts animation speed from knockback. Sets death state if HP gone.
void Humanoid::_GotHitMed() {
    MARKFUNCTION(0x800688B4);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    // PSX: if field466 nonzero, adjust animation speed
    if (field466 != 0) {
        if (anim->endFrame != 0) {
            anim->speed = rmDiv16i(anim->endFrame, (s32)field466 << 16);
        }
    }

    // PSX: if health == 0, set walkCycleFlag to AS_DEAD
    if (health <= 0) {
        walkCycleFlag = (s32)AS_DEAD;
    }
}

// PSX: _GotHitLow__8Humanoid (HUMANOID.CPP:5232, 0x800689B4)
// Identical logic to _GotHitMed: speed adjust + death check.
void Humanoid::_GotHitLow() {
    MARKFUNCTION(0x800689B4);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    if (field466 != 0) {
        if (anim->endFrame != 0) {
            anim->speed = rmDiv16i(anim->endFrame, (s32)field466 << 16);
        }
    }

    if (health <= 0) {
        walkCycleFlag = (s32)AS_DEAD;
    }
}

// PSX: _Stunned__8Humanoid (HUMANOID.CPP:5333, 0x80068AB4)
// Countdown stun timer (field468). On expire, clean up animControl
// and return to stand. On health depletion, go dead.
void Humanoid::_Stunned() {
    MARKFUNCTION(0x80068AB4);

    if ((s16)field468 > 0) {
        // PSX: decrement stun timer by rate (field468 - comboCount)
        field468 = (u16)((u16)field468 - comboCount);
    }
    else {
        // Stun expired
        field468 = 0;

        // PSX: if animControl target exists, signal and clear
        if (animControl != 0) {
            // PSX: *(animControl + 108) = 1 — signal stun target complete
            animControl = 0;
        }

        SetActionState(AS_STAND, 0);
    }

    // PSX: death check (health == 0)
    if (health <= 0) {
        if (animControl != 0) {
            animControl = 0;
        }
        SetActionState(AS_DEAD, 0);
    }
}

// PSX: _SpinBack__8Humanoid (HUMANOID.CPP:5373, 0x80068B78)
// Wait for spin-back animation to complete (loopCount > 0),
// then transition to recovery state.
void Humanoid::_SpinBack() {
    MARKFUNCTION(0x80068B78);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    // PSX: if loopCount > 0, transition to spin-back recovery
    if (anim->loopCount > 0) {
        SetActionState(AS_SPIN_BACK_RECOVER, 0);
    }
}

// PSX: _FlyingBack__8Humanoid (HUMANOID.CPP:5397, 0x80068BC8)
// Scale gravity by global knockback factor, check animation complete
// for landing transition, check ground for ground-check transition.
void Humanoid::_FlyingBack() {
    MARKFUNCTION(0x80068BC8);

    // PSX: gravity *= gp+1860
    // PSX: maxFallDivisor = 18 / gp+1764
    gravity *= s_flyingBackGravityScale;
    maxFallDivisor = GetFlyingBackFallDivisor();

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    // PSX: if animation complete (loopCount > 0), transition to landing
    if (anim->loopCount > 0) {
        SetActionState(AS_FLYING_BACK_LAND, 0);
    }

    // PSX: if on ground (flags bit 12), transition to ground check
    if (flags & TF_ON_GROUND) {
        SetActionState(AS_FLYING_BACK_CHECK, 0);
    }
}

// PSX: _Floating__8Humanoid (HUMANOID.CPP:5425, 0x80068C9C)
// Continues the post-knockback fall until ground contact transitions to collapse.
void Humanoid::_Floating() {
    MARKFUNCTION(0x80068C9C);

    gravity *= s_flyingBackGravityScale;
    maxFallDivisor = GetFlyingBackFallDivisor();

    if (flags & TF_ON_GROUND) {
        SetActionState(AS_FLYING_BACK_CHECK, 0);
    }
}

// PSX: _Collapse__8Humanoid (HUMANOID.CPP:5476, 0x80068DD4)
// Play collapse groan dialog, call ProcessControl, check animation
// complete + on-ground for get-up/death transition.
void Humanoid::_Collapse() {
    MARKFUNCTION(0x80068DD4);

    if (!model) {
        return;
    }
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    LoadDialog(1, 50);

    // PSX: vtable+260 = TestAndSetRisingAttack
    TestAndSetRisingAttack();

    // PSX: check loopCount > 0 AND on-ground
    if (anim->loopCount <= 0) {
        return;
    }
    if (!(flags & TF_ON_GROUND)) {
        return;
    }

    // PSX: if health == 0, die
    if (health <= 0) {
        SetActionState(AS_DEAD, 0);
        return;
    }

    // PSX: check stateTimer against humanoidDataID threshold
    if ((s16)humanoidDataID < (s16)stateTimer) {
        // PSX: if not this player AND model has bit 4 flag, signal get-up
        if (this != (Humanoid*)Player::s_player) {
            // PSX: check model->modelFlags bit 4
            if (m->modelFlags & 0x10) {
                Player::s_player->SignalEnemyGetUp();
            }
        }
        SetActionState(AS_GET_UP, 0);
    }
    else {
        stateTimer++;
    }
}

// PSX: _Dead__8Humanoid (HUMANOID.CPP:5723, 0x800691DC)
// Complex death handler: type-specific checks, signal player,
// toggle flags, cleanup, remove from fighting system.
void Humanoid::_Dead() {
    MARKFUNCTION(0x800691DC);

    // PSX (0x800691FC): clear the OscarsHenchman global when an Oscar
    // henchman dies, so a solo Oscar drops the circling/backoff tree and
    // switches to his relentless chase.
    Behaviour::ClearOscarsHenchmanOnDeath(this);

    // PSX: type check for respawn eligibility
    // Types 10, 12, 13, 15, 17 are boss types that don't signal player
    bool isBossType = false;
    switch (thingType) {
        case AITypes::TT_GRONTAR:
        case AITypes::TT_PAUL:
        case AITypes::TT_OSCAR:
        case AITypes::TT_DANTE:
        case AITypes::TT_BUTCH:
            isBossType = true;
            break;
    }

#if NEW_CHEATS
    // Saints Row-style "Heaven Bound": let the death animation settle for one
    // second, turn the limp corpse pose upright, then ascend slowly before
    // resuming the original cleanup path below. Boss scripts remain untouched.
    static constexpr s32 HEAVEN_BOUND_DELAY_FRAMES = 15;
    static constexpr s32 HEAVEN_BOUND_ASCEND_FRAMES = 90;
    static constexpr s32 HEAVEN_BOUND_ROTATE_FRAMES = 15;
    static constexpr s32 HEAVEN_BOUND_ASCEND_SPEED = 44;
    if (!isBossType && IsCheatEnabled(CheatOption::HeavenBound)
        && thinkCounter < HEAVEN_BOUND_DELAY_FRAMES + HEAVEN_BOUND_ASCEND_FRAMES) {
        // The ascent can carry a corpse outside the level's spatial blocks.
        // Keep it active until the effect falls through to normal death cleanup;
        // otherwise UpdatePosition deactivates it and its timer freezes in midair.
        flags |= TF_BIT5;

        if (thinkCounter <= 1) {
            FightingCollision::RemoveHumanoid(this);
            ReleaseTarget();
            if (Player::s_player) Player::s_player->SignalEnemyDead(this);
        }

        flags |= 0x0080;
        if (thinkCounter >= HEAVEN_BOUND_DELAY_FRAMES) {
            const s32 ascendFrame = thinkCounter - HEAVEN_BOUND_DELAY_FRAMES;
            if (ascendFrame < HEAVEN_BOUND_ROTATE_FRAMES) {
                // No ragdoll solver exists, so ease the finished horizontal
                // death pose upright over half a second while retaining its
                // limp animation pose. Include the integer remainder on the
                // final frame so the total rotation is exactly 90 degrees.
                const s32 baseStep = 0x4000 / HEAVEN_BOUND_ROTATE_FRAMES;
                orientation.x += (ascendFrame + 1 == HEAVEN_BOUND_ROTATE_FRAMES)
                    ? (0x4000 - baseStep * (HEAVEN_BOUND_ROTATE_FRAMES - 1))
                    : baseStep;
            }
            flags &= ~TF_ON_GROUND;
            maxFallDivisor = 0;
            velocity.x = 0;
            velocity.y = HEAVEN_BOUND_ASCEND_SPEED;
            velocity.z = 0;
            force = {};
            contactForce = {};
            orientation.y += 96;
        }
        KillDialog(0, 0, 512);
        return;
    }
#endif

    if (!isBossType) {
        // PSX: SignalEnemyDead(thePlayer, this)
        if (Player::s_player) {
            Player::s_player->SignalEnemyDead(this);
        }

        // PSX: toggle flags bit 8 based on thinkCounter state
        if ((thinkCounter & 0x03) == 2) {
            if (flags & TF_BIT8) {
                flags &= ~TF_BIT8;
            }
            else {
                flags |= TF_BIT8;
            }
        }

        bool stillAnimating = false;
        if (model) {
            Model* m = static_cast<Model*>(model);
            stillAnimating = (m->modelFlags & 0x10) != 0;
        }

        if (!stillAnimating || thinkCounter >= 41) {
            FightingCollision::RemoveHumanoid(this);
            ReleaseTarget();
            flags &= ~0x0080; // clear bit 7

            if (field260 != 0) {
                // PSX: set model flag for fade-out
                if (model) {
                    Model* m = static_cast<Model*>(model);
                    m->modelFlags |= 0x20;
                }
            }
            else {
                // PSX: call Kill virtual to deactivate
                Kill();
            }
        }
    }

    KillDialog(0, 0, 512);
}

// PSX: _CrouchUp__8Humanoid (HUMANOID.CPP:5822, 0x8006934C)
void Humanoid::_CrouchUp() {
    MARKFUNCTION(0x8006934C);

    PlayDialog(1, 30);

    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        return;
    }

    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
    const s16 numFrames = (anim->animation) ? anim->animation->numFrames : 0;
    if (frame < static_cast<s16>(numFrames - TARGET_TRACK_MAX_FRAME)) {
        flags |= TF_BIT1;
    }

    if (anim->loopCount != 0) {
        SetActionState(AS_STAND, 0);
        return;
    }

    if (field488 != 0) {
        SetHumanoidTarget(FindFoe(DIVE_ROLL_FIGHT_DISTANCE, FIGHT_HALF_ANGLE, 0));
        SetCurrentFightingNode();
    }
}

// PSX: LoadDialog__8HumanoidUll (HUMANOID.CPP, 0x8006CB54)
s32 Humanoid::LoadDialog(u32 dialogID, s32 priority) {
    MARKFUNCTION(0x8006CB54);
    s32 handle = rsEvent(RS_LOAD_DIALOG, (s32)thingType, (s32)dialogID, priority);
    if (handle) {
        soundHandle = handle;
        soundParam = (s32)dialogID;
    }
    return 1;
}

// PSX: LoadEnemyTaunts__8Humanoid (HUMANOID.CPP:991, 0x80063690)
void Humanoid::LoadEnemyTaunts() {
    MARKFUNCTION(0x80063690);

    Player* player = Player::s_player;
    if (!player) {
        return;
    }

    const s32 playerHealthRatio = rmDiv16i((s32)((u32)player->health << 16), (s32)((u32)player->maxHealth << 16));
    const s32 selfHealthRatio = rmDiv16i((s32)((u32)health << 16), (s32)((u32)maxHealth << 16));

    if (player->health == 0 || player->actionState == (s32)AS_DEAD) {
        LoadDialog(9, 0xFF);
        return;
    }

    // PSX (0x800637D0/0x800637E8): every path below loads at priority 0x31
    // (49), not 0xFF -- only the player-dead gloat above uses 0xFF. 0x31 is
    // what falls inside PlayDialogBasedOnPriority's [0,55] window that
    // AS_TAUNT_ENTRY uses to actually start playback; loading at 0xFF here
    // meant the dialog was queued but never played.
    if (selfHealthRatio < playerHealthRatio) {
        // PSX: losing -- self is worse off than the player.
        if (player->encounterState == 2) {
            if (!LoadDialog(3, 0x31)) {
                LoadDialog(0, 0x31);
            }
            return;
        }
        LoadDialog(4, 0x31);
        return;
    }

    if (playerHealthRatio < 0x4CCC) {
        // PSX: winning big -- player is below ~30% health.
        bool isBossType = false;
        switch (thingType) {
            case AITypes::TT_GRONTAR:
            case AITypes::TT_PAUL:
            case AITypes::TT_OSCAR:
            case AITypes::TT_DANTE:
            case AITypes::TT_BUTCH:
                isBossType = true;
                break;
        }
        LoadDialog(isBossType ? 9 : 5, 0x31);
        return;
    }

    LoadDialog(player->encounterState == 2 ? 2 : 4, 0x31);
}

// PSX: PlayDialog__8HumanoidUlUl (HUMANOID.CPP, 0x8006CBA0)
s32 Humanoid::PlayDialog(u32 dialogID, s32 priority) {
    MARKFUNCTION(0x8006CBA0);
    if (soundParam != (s32)dialogID) {
        return 0;
    }
    if (soundHandle && jcsValidateHandle(soundHandle)) {
        SetPendingDialogPlayPosition(&pos);
        if (rsEvent(RS_PLAY_DIALOG, soundHandle, (s32)(intptr_t)&pos, priority) != 0) {
            return 1;
        }
        rsEvent(RS_KILL_DIALOG, soundHandle, 0, 0);
    }
    soundHandle = 0;
    soundParam = 0;
    return 1;
}

// PSX: PlayDialogBasedOnPriority__8Humanoidll (HUMANOID.CPP, 0x8006CC38)
s32 Humanoid::PlayDialogBasedOnPriority(s32 minPriority, s32 maxPriority) {
    MARKFUNCTION(0x8006CC38);

    if (!soundHandle) {
        soundHandle = 0;
        soundParam = 0;
        return 0;
    }

    if (!jcsValidateHandle(soundHandle)) {
        soundHandle = 0;
        soundParam = 0;
        return 0;
    }

    s32 dialogPriority = jcsQueryDialogPriority(soundHandle);
    if (dialogPriority >= minPriority) {
        if (maxPriority >= dialogPriority) {
            SetPendingDialogPlayPosition(&pos);
            if (rsEvent(RS_PLAY_DIALOG, soundHandle, (s32)(intptr_t)&pos, 30) != 0) {
                return 1;
            }
            rsEvent(RS_KILL_DIALOG, soundHandle, 0, 0);
            soundHandle = 0;
            soundParam = 0;
            return 0;
        }
    }

    return 0;
}

// PSX: KillDialog__8Humanoidill (HUMANOID.CPP, 0x8006CCF8)
s32 Humanoid::KillDialog(s32 force, s32 minPriority, s32 maxPriority) {
    MARKFUNCTION(0x8006CCF8);

    if (!soundHandle) {
        soundHandle = 0;
        soundParam = 0;
        return 1;
    }

    if (!jcsValidateHandle(soundHandle)) {
        soundHandle = 0;
        soundParam = 0;
        return 1;
    }

    s32 dialogPriority = jcsQueryDialogPriority(soundHandle);
    if (dialogPriority >= minPriority && maxPriority >= dialogPriority) {
        if (!jcsIsPlaying(soundHandle) || force) {
            rsEvent(RS_KILL_DIALOG, soundHandle, 0, 0);
            soundHandle = 0;
            soundParam = 0;
            return 1;
        }
    }

    return 0;
}

static bool IsThrowLatchDisallowedState(u32 state) {
    switch (state) {
    case AS_FALL:
    case AS_HARDFALL:
    case AS_COUNTER_ATTACK_PRE_LATCH:
    case AS_COUNTER_ATTACK_LATCH:
    case AS_COUNTER_ATTACK:
    case 51:
    case 52:
    case AS_GOT_HIT_FREEFORM:
    case AS_FLYING_BACK:
    case AS_FLYING_BACK_LAND:
    case AS_BACK_GRAB_RECEIVE_PRE_LATCH:
    case AS_BACK_GRAB_RECEIVE_LATCH:
    case AS_BACK_GRAB_RECEIVE:
    case AS_THROW_FREE_FALL:
        return true;
    default:
        return false;
    }
}

// PSX: ReSyncOrientation__8HumanoidRC12FightingMove (HUMANOID.CPP:8311)
s32 Humanoid::ReSyncOrientation(const PsxFightingMoveRaw* move) {
    MARKFUNCTION(0x8006BF04);

    if (!move) {
        faceAngle = orientation.y;
        return orientation.y;
    }

    const u16 anim = move->anim;
    const bool rotateHalfTurn = (anim == 107 || anim == 73
        || (anim == 120 && this == static_cast<Humanoid*>(Player::s_player)));
    if (!rotateHalfTurn) {
        faceAngle = orientation.y;
        return orientation.y;
    }

    const s32 currentOrientY = orientation.y;
    const s32 rotated = currentOrientY + 0x8000;
    s32 absRotated = rotated;
    if (absRotated < 0) {
        absRotated = -absRotated;
    }

    faceAngle = rotated;
    if (absRotated > 0x7FFF) {
        if (rotated > 0) {
            faceAngle = currentOrientY - 0x7FFF;
        }
        else {
            faceAngle = rotated + 0xFFFF;
        }
    }

    orientation.y = faceAngle;
    return faceAngle;
}

// PSX: GetWeaponTransitionIdle__FP6Pickup (HUMANOID.CPP:2671, 0x80065420)
static s32 GetWeaponTransitionIdle(const Pickup* pickup) {
    MARKFUNCTION(0x80065420);

    if (!pickup) {
        return 0;
    }

    const s32 idleAnim = pickup->idleAnim;

    switch (idleAnim) {
    case 189:
        return 190;
    case 206:
        return 207;
    case 220:
        return 221;
    case 231:
        return 237;
    case 244:
        return 247;
    case 254:
        return 257;
    default:
        return 0;
    }
}

// PSX: SetTransitionAnim__13HumanoidModelll (MHUMAN.CPP:224, 0x8006E46C)
static void SetTransitionIdleAnim(HumanoidModel* model, s32 transitionAnim, s32 targetIdleAnim) {
    if (!model) {
        return;
    }

    model->SetAnim(transitionAnim, 0, 0, 0);

    AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
    if (anim) {
        anim->humanoidCB.offsetLo = 0;
        anim->humanoidCB.offsetHi = -1;
        anim->humanoidCB.funcPtr = reinterpret_cast<void*>(StitchIdleAnimationCallback);
    }

    model->field120 = targetIdleAnim;
}

// PSX: FightTargetAndThrowLatch__8Humanoid12FightingType (HUMANOID.CPP:7856)
Humanoid* Humanoid::FightTargetAndThrowLatch(u8 fightingType) {
    MARKFUNCTION(0x8006B778);

    if (fightingType != 2) {
        return FindFoe(THROW_LATCH_DISTANCE, FIGHT_HALF_ANGLE, 1);
    }

    Humanoid* target = FindFoe(THROW_LATCH_DISTANCE, THROW_LATCH_HALF_ANGLE, 0);
    if (!target) {
        return nullptr;
    }

    s32 dy = pos.y - target->pos.y;
    if (dy < 0) {
        dy = -dy;
    }
    if (dy > THROW_LATCH_VERTICAL_DELTA_MAX) {
        return nullptr;
    }

    const u32 targetState = static_cast<u32>(target->actionState);
    if (IsThrowLatchDisallowedState(targetState)
        || ((target->flags >> 1) & 1u) != 0) {
        return nullptr;
    }

    return target;
}

// PSX: SetCurrentFightingNode__8Humanoid (HUMANOID.CPP:8049)
s32 Humanoid::SetCurrentFightingNode() {
    MARKFUNCTION(0x8006BA30);

    const FightingComboNode* nextNode = ResolveFightingNodeAddressConst(static_cast<u32>(field488));
    if (!nextNode || !nextNode->moveData || !model) {
        SetActionState(AS_STAND, 0);
        return 0;
    }

    FightingCollision::ClearAttack(this);
    field472 = 0;
    field428 = 0;
    field484 = static_cast<s32>(nextNode->psxAddress);
    field488 = 0;

    const PsxFightingMoveRaw* move = nextNode->moveData;
    HumanoidModel* hm = static_cast<HumanoidModel*>(model);
    if (!hm->IsAnimationLoaded(static_cast<s32>(move->anim))) {
        if (rightHandObj || leftHandObj) {
            const Pickup* heldPickup = rightHandObj
                ? static_cast<const Pickup*>(rightHandObj)
                : static_cast<const Pickup*>(leftHandObj);
            LOG(
                "[PickupCombat] missing anim thingType=%u pickupType=%u cmd=0x%08X node=0x%08X anim=%d state=%d",
                static_cast<u32>(thingType),
                heldPickup ? static_cast<u32>(heldPickup->thingType) : 0u,
                static_cast<u32>(commandBits),
                static_cast<u32>(nextNode->psxAddress),
                static_cast<s32>(move->anim),
                actionState);
        }
        SetActionState(AS_STAND, 0);
        return 0;
    }

    Model* m = static_cast<Model*>(model);

    if (m->drawableType == 2 && m->drawable) {
        DrawableSTree* drawable = static_cast<DrawableSTree*>(m->drawable);
        if ((drawable->mirrorFlags & 1u) != 0) {
            static_cast<SModel*>(m)->MirrorTree();
        }
    }

    combatFlag = 0;
    m->SetAnim(static_cast<s32>(move->anim), nextNode->field07, 1, nextNode->field06);

    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    if (!anim) {
        SetActionState(AS_STAND, 0);
        return 0;
    }

    anim->speed = static_cast<s32>(move->firstWord);

    s32 nextFaceAngle = orientation.y + move->turnDelta;
    s32 absAngle = nextFaceAngle;
    if (absAngle < 0) {
        absAngle = -absAngle;
    }
    faceAngle = nextFaceAngle;
    if (absAngle > 0x7FFF) {
        faceAngle += (nextFaceAngle > 0) ? -65535 : 0xFFFF;
    }
    orientation.y = faceAngle;

    if (anim && anim->flip) {
        anim->flip->SetFrame(0);
        anim->flip->UpdateJoints();
    }

    flags2 &= ~0x70u;
    if (nextNode->field04) {
        flags2 |= 0x30;
        field516 = 0;
        field520 = 0;
        field524 = 0;
    }

    field344 = 0;
    stateDispatch = SD_FIGHTING_COMBO;
    field348 = 8;
    return 1;
}

// PSX: ProcessFightingMove__8HumanoidRC12FightingMovel (HUMANOID.CPP:6975)
s32 Humanoid::ProcessFightingMove(const PsxFightingMoveRaw* move, s32 frame) {
    MARKFUNCTION(0x8006A6D4);

    if (!move) {
        return 0;
    }

    if (move->fightingType != 2) {
        return ProcessGenericFightingMove(move, frame);
    }

    actionState = AS_COMBAT_IDLE;
    return ProcessBodyThrow(move, frame);
}

// PSX: ProcessSoundEvent__8Humanoidll (HUMANOID.CPP:6920)
s32 Humanoid::ProcessSoundEvent(s32 eventType) {
    MARKFUNCTION(0x8006A650);

    if (!humanoidSound) {
        return 0;
    }

    switch (eventType) {
    case 1:
    case 2:
    case 3:
        return humanoidSound->PunchMiss();
    case 4:
    case 5:
        return humanoidSound->KickMiss();
    case 10:
    case 11:
    case 12:
    case 13:
        return humanoidSound->WeaponMiss();
    default:
        return 0;
    }
}

// PSX: GetImpactRegion__8HumanoidRC10tagLVector (HUMANOID.CPP:4704)
s32 Humanoid::GetImpactRegion(const LVector& point) {
    MARKFUNCTION(0x800680B8);

    const s32 originX = pos.x;
    const s32 originY = pos.y;
    const s32 originZ = pos.z;

    const s32 sinY90 = rmSin16(static_cast<s16>(orientation.y) + 0x4000);
    const s32 sinY = rmSin16(static_cast<s16>(orientation.y));

    const s32 dy = point.y - originY;

    s32 region = 1;
    if (static_cast<s32>(((static_cast<s64>(point.z - originZ) * sinY) >> 16)
            + ((static_cast<s64>(point.x - originX) * sinY90) >> 16))
        <= 0) {
        region = 2;
    }

    s32 regionFlags;
    if (static_cast<s32>(((static_cast<s64>(originX - point.x) * sinY) >> 16)
            + ((static_cast<s64>(point.z - originZ) * sinY90) >> 16))
        <= 0) {
        regionFlags = region | 8;
    }
    else {
        regionFlags = region | 4;
    }

    HumanoidModel* hm = static_cast<HumanoidModel*>(model);
    if (hm) {
        if (dy < hm->field108) {
            return regionFlags | 0x40;
        }
        if (dy >= hm->field112) {
            return regionFlags | 0x10;
        }
    }

    return regionFlags | 0x20;
}

static s32 GetTrailMatrixTypeFromStrikeJoint(u8 jointIndex) {
    switch (jointIndex) {
    case 1:
        return 6;
    case 2:
        return 7;
    case 3:
        return 8;
    case 4:
        return 9;
    default:
        return -1;
    }
}

// PSX: DisableTrailCallbacks__8Humanoid (HUMANOID.CPP:8284, 0x8006BEB4)
static void DisableTrailCallbacks(Humanoid* humanoid) {
    if (!humanoid || humanoid->prevAttackJointIndex == -1 || !humanoid->model) {
        return;
    }

    HumanoidModel* hm = static_cast<HumanoidModel*>(humanoid->model);
    AnimationMatrices* animMatrices = hm ? hm->animMatrices : nullptr;
    if (animMatrices) {
        animMatrices->SetExtraCallbacks(humanoid->prevAttackJointIndex, 0);
    }
    humanoid->prevAttackJointIndex = -1;
}

// PSX: DoTrailCallbacks__8HumanoidRC13FightingJoint (HUMANOID.CPP:8148, 0x8006BC40)
static void DoTrailCallbacks(Humanoid* humanoid, const PsxFightingJointRaw* joint) {
    if (!humanoid || !joint || !humanoid->model) {
        return;
    }

    HumanoidModel* hm = static_cast<HumanoidModel*>(humanoid->model);
    AnimationMatrices* animMatrices = hm ? hm->animMatrices : nullptr;
    Trails* trailEffect = static_cast<Trails*>(humanoid->trails);
    if (!animMatrices || !trailEffect) {
        return;
    }

    if (humanoid->prevAttackJointIndex == -1) {
        humanoid->prevAttackJointIndex = GetTrailMatrixTypeFromStrikeJoint(joint->JointIndex());
        if (humanoid->prevAttackJointIndex != -1) {
            animMatrices->SetExtraCallbacks(humanoid->prevAttackJointIndex, 1);
        }
        return;
    }

    if (((animMatrices->extraMask >> humanoid->prevAttackJointIndex) & 1) == 0) {
        return;
    }

    LVector trailStart = {};
    LVector trailEnd = {};
    animMatrices->GetAttack(static_cast<u32>(joint->JointIndex()), trailStart, trailStart);

    if (joint->JointIndex() == static_cast<u8>(humanoid->prevAttackJointIndex)) {
        trailEnd = trailStart;
        trailEnd.y += 100;
    }
    else {
        animMatrices->GetAttack(static_cast<u32>(humanoid->prevAttackJointIndex), trailEnd, trailEnd);
    }

    s32 trailR = static_cast<s32>(s_splPlayerTrailR);
    s32 trailG = static_cast<s32>(s_splPlayerTrailG);
    s32 trailB = static_cast<s32>(s_splPlayerTrailB);
    if (joint->TrailFlags() == 2) {
        trailR = 100;
        trailG = 100;
        trailB = 220;
    }

    const u32 trailColor = static_cast<u32>(trailR & 0xFF)
        | (static_cast<u32>(trailG & 0xFF) << 8)
        | (static_cast<u32>(trailB & 0xFF) << 16);

    const Thing* ticketIssuer = humanoid->GetTicketIssuer();
    const Platform* movingPlatform = (ticketIssuer && ticketIssuer->thingType == AITypes::TT_PLATFORM)
        ? static_cast<const Platform*>(ticketIssuer)
        : nullptr;

    const LVector* posRef = nullptr;
    if (movingPlatform && rmMag3ff(
            movingPlatform->velocity.x,
            movingPlatform->velocity.y,
            movingPlatform->velocity.z)
            > 0) {
        posRef = &humanoid->pos;
    }
    else if (rmMag3ff(humanoid->force.x, humanoid->force.y, humanoid->force.z) > 0) {
        posRef = &humanoid->pos;
    }

    if (posRef) {
        trailEffect->SetCurrentPos(&humanoid->pos);
    }

    trailEffect->Add(
        &trailStart,
        &trailEnd,
        trailColor,
        s_splPlayerTrailFrames,
        posRef);
}

// PSX: ProcessFightingMoveStrikeJoint__8HumanoidRC13FightingJointlllii (HUMANOID.CPP:7017)
s32 Humanoid::ProcessFightingMoveStrikeJoint(
    const PsxFightingJointRaw* joint,
    s32 frame,
    s32 attackType,
    s32 fightingPoints,
    s32 stylePointsFlag,
    s32 weaponBreakOnEmpty) {
    MARKFUNCTION(0x8006A714);

    Humanoid* const player = static_cast<Humanoid*>(Player::s_player);
    const bool isPlayerAttacker = (this == player);

    if (!joint) {
        if (isPlayerAttacker) {
            DisableTrailCallbacks(this);
        }
        return 0;
    }

    if (frame < joint->AttackStartFrame() || joint->AttackEndFrame() < frame) {
        if (isPlayerAttacker) {
            DisableTrailCallbacks(this);
        }
        return 0;
    }

    attackJointIndex = static_cast<s32>(joint->JointIndex());

    HumanoidModel* hm = static_cast<HumanoidModel*>(model);
    AnimationMatrices* animMatrices = hm ? hm->animMatrices : nullptr;
    if (!animMatrices) {
        return 0;
    }

    FightingCollisionAttackType attack = {};
    attack.attackType = attackType;
    animMatrices->GetAttack(static_cast<u32>(joint->JointIndex()), attack.startA, attack.endA);

    switch (joint->JointIndex()) {
    case 1:
    case 2:
        attack.radiusA = hm->attackHandRadius;
        break;
    case 3:
    case 4:
        attack.radiusA = hm->attackFootRadius;
        break;
    case 5:
        attack.radiusA = ATTACKER_WEAPON_RADIUS;
        break;
    default:
        attack.radiusA = 0;
        break;
    }

    const s32 soundEvent = static_cast<s32>(joint->SoundEvent());
    const bool weaponJoint = rightHandObj != nullptr && static_cast<u32>(soundEvent - 10) < 3u;
    if (weaponJoint) {
        const Pickup* heldPickup = static_cast<const Pickup*>(rightHandObj);
        attack.hasSecondary = 1;
        animMatrices->GetWeaponAttack(
            static_cast<u32>(joint->JointIndex()),
            heldPickup->collisionPoints[0],
            attack.startB,
            attack.endB);
        attack.radiusB = ATTACKER_WEAPON_RADIUS;
    }

    Thing* ticketIssuer = GetTicketIssuer();
    const Obstacle* issuerObstacle = ticketIssuer ? dynamic_cast<Obstacle*>(ticketIssuer) : nullptr;
    const LVector* issuerDelta = issuerObstacle ? issuerObstacle->GetDeltaVelocity() : nullptr;
    if (issuerDelta) {
        attack.startA.x += 2 * issuerDelta->x;
        attack.startA.y += 2 * issuerDelta->y;
        attack.startA.z += 2 * issuerDelta->z;
        attack.endA.x += 2 * issuerDelta->x;
        attack.endA.y += 2 * issuerDelta->y;
        attack.endA.z += 2 * issuerDelta->z;
        attack.startB.x += 2 * issuerDelta->x;
        attack.startB.y += 2 * issuerDelta->y;
        attack.startB.z += 2 * issuerDelta->z;
        attack.endB.x += 2 * issuerDelta->x;
        attack.endB.y += 2 * issuerDelta->y;
        attack.endB.z += 2 * issuerDelta->z;
    }

    Humanoid* victims[4] = {};
    s32 victimCount = FightingCollision::CheckAttack(victims, 4, this, &attack);
    if (victimCount > 4) {
        victimCount = 4;
    }

    s32 didHit = 0;
    for (s32 i = 0; i < victimCount; i++) {
        Humanoid* victim = victims[i];
        if (!victim) {
            continue;
        }

        const s32 victimState = victim->actionState;
        const bool blockedState = (static_cast<u32>(victimState - 69) < 4u) || victimState == 56;
        if (blockedState || ((victim->flags >> 1) & 1u) != 0) {
            continue;
        }

        didHit = 1;
        if (field472 == 0) {
            field472 = 1;
        }

        if (weaponJoint) {
            Shock(ShockEnum::SHOCK_14);
        }

        PlayCombatKnockDownDialog(soundEvent);
        ReleaseTarget();

        LVector localForce = { joint->ForceX(), joint->ForceY(), joint->ForceZ() };
        LVector worldForce = {};
        GetObjectToWorldSpaceVector(localForce, worldForce);

        const s32 impactRegion = victim->GetImpactRegion(attack.endA);

        if (soundEvent == 2 || soundEvent == 3 || soundEvent == 12) {
            Effects* strikeEffect = GEffect_Create(
                HUMANOID_STRIKE_IMPACT_EFFECT_HASH,
                &attack.endA,
                nullptr,
                nullptr,
                0,
                0,
                0x80000000u);
            if (!strikeEffect) {
                LOG(
                    "[EffectsParity] Strike effect create miss hash=%08X soundEvent=%d\n",
                    HUMANOID_STRIKE_IMPACT_EFFECT_HASH,
                    soundEvent);
            }
        }

        victim->HandleCollision(
            this,
            1,
            COLLISION_TAG_HIT_TYPE,
            soundEvent,
            COLLISION_TAG_IMPACT_REGION,
            impactRegion,
            COLLISION_TAG_IMPULSE,
            &worldForce,
            COLLISION_TAG_DAMAGE,
            static_cast<s32>(joint->Damage()),
            COLLISION_TAG_END);

        if (weaponJoint && soundEvent >= 10 && humanoidSound) {
            humanoidSound->WeaponHit();
        }

        victim->field466 = static_cast<s16>(static_cast<s8>(joint->HitForce()));

        if (isPlayerAttacker) {
            if (g_hud) {
                g_hud->SetFoe(victim);
            }
            if (g_scoreManager) {
                g_scoreManager->AddFightingPoints(fightingPoints);
                if (stylePointsFlag) {
                    g_scoreManager->AddStylePoints(fightingPoints);
                }
            }
        }
    }

    if (weaponJoint && rightHandObj && victimCount > 0) {
        const u16 oldDurability = rightHandObj->health;
        rightHandObj->health = (victimCount >= static_cast<s32>(oldDurability))
            ? 0
            : static_cast<u16>(oldDurability - victimCount);

        Thing* brokenWeapon = rightHandObj;
        if (brokenWeapon->health == 0 && weaponBreakOnEmpty != 0) {
            DropPickup(1, 1);
            if (rightHandObj == brokenWeapon) {
                rightHandObj = nullptr;
                flags2 &= ~1u;
            }
            if (leftHandObj == brokenWeapon) {
                leftHandObj = nullptr;
                flags2 &= ~2u;
            }

            brokenWeapon->Kill();
            field472 = 0;
        }
    }

    if (victimCount > 0) {
        HandleHitShock(soundEvent);
    }

    Obstacle* obstacles[4] = {};
    s32 obstacleCount = g_ai ? g_ai->CheckObstacleAttack(obstacles, 4, this, &attack) : 0;
    if (obstacleCount > 4) {
        obstacleCount = 4;
    }

    ticketIssuer = GetTicketIssuer();
    for (s32 i = 0; i < obstacleCount; i++) {
        Obstacle* obstacle = obstacles[i];
        if (!obstacle) {
            continue;
        }

        if (!rightHandObj || ticketIssuer != static_cast<Thing*>(obstacle)) {
            obstacle->HandleAttack(
                this,
                soundEvent,
                static_cast<s32>(joint->ForceZ()),
                static_cast<s32>(joint->Damage()));
        }
    }

    if (isPlayerAttacker && joint->TrailFlags() != 0) {
        DoTrailCallbacks(this, joint);
    }

    return didHit;
}

// PSX: GetTargetingFrame__8HumanoidRC18StrikeFightingMove (HUMANOID.CPP:7350)
s32 Humanoid::GetTargetingFrame(const PsxFightingMoveRaw* move) const {
    MARKFUNCTION(0x8006ADC8);

    (void)CHARGEPUNCH_TARGET_TRACK_MAX_FRAME;
    if (!move) {
        return TARGET_TRACK_MAX_FRAME;
    }

    if (move->anim == 25) {
        return DIVEROLLKICK_TARGET_TRACK_MAX_FRAME;
    }
    if (move->anim == 26) {
        return DIVEROLLPUNCH_TARGET_TRACK_MAX_FRAME;
    }
    return TARGET_TRACK_MAX_FRAME;
}

// PSX: ProcessGenericFightingMove__8HumanoidRC18StrikeFightingMovel (HUMANOID.CPP:7404)
s32 Humanoid::ProcessGenericFightingMove(const PsxFightingMoveRaw* move, s32 frame) {
    MARKFUNCTION(0x8006AE0C);

    if (!move) {
        return 0;
    }

    if (frame < GetTargetingFrame(move)) {
        Humanoid* target = reinterpret_cast<Humanoid*>(field256);
        if (target) {
            FaceThing(target, 1);
        }
    }

    if (frame >= move->moveWindowStart && move->moveWindowEnd >= frame) {
        const s32 moveAmount = static_cast<s32>(static_cast<u8>(move->moveDelta));
        homePos.x += static_cast<s32>(
            (static_cast<s64>(rmSin16(orientation.y)) * static_cast<s64>(moveAmount)) >> 16);
        homePos.z += static_cast<s32>((static_cast<s64>(
                                      rmSin16(static_cast<s16>(orientation.y) + 0x4000))
                                  * static_cast<s64>(moveAmount))
                                 >> 16);
    }

    if (frame >= move->combatWindowStart && move->combatWindowEnd >= frame) {
        flags |= 2u;
    }

    const PsxFightingJointRaw* primaryJoint = ResolveFightingJointAddress(move->data20);
    ProcessFightingMoveStrikeJoint(
        primaryJoint,
        frame,
        0,
        static_cast<s32>(move->fightingPoints),
        static_cast<s32>(move->stylePointsFlag),
        static_cast<s32>(move->weaponBreakOnEmpty));
    if (primaryJoint && primaryJoint->SoundFrame() == frame) {
        ProcessSoundEvent(static_cast<s32>(primaryJoint->SoundEvent()));
    }

    if (move->data24) {
        const PsxFightingJointRaw* secondaryJoint = ResolveFightingJointAddress(move->data24);
        ProcessFightingMoveStrikeJoint(
            secondaryJoint,
            frame,
            1,
            static_cast<s32>(move->fightingPoints),
            static_cast<s32>(move->stylePointsFlag),
            static_cast<s32>(move->weaponBreakOnEmpty));
        if (secondaryJoint && secondaryJoint->SoundFrame() == frame) {
            ProcessSoundEvent(static_cast<s32>(secondaryJoint->SoundEvent()));
        }
    }

    return 0;
}

// PSX: ProcessBodyThrow__8HumanoidRC17ThrowFightingMovel (HUMANOID.CPP:7504)
s32 Humanoid::ProcessBodyThrow(const PsxFightingMoveRaw* move, s32 frame) {
    MARKFUNCTION(0x8006B0A0);

    if (!move) {
        return 0;
    }

    maxFallDivisor = 0;

    Player* const player = Player::s_player;
    const bool isPlayerAttacker = (this == static_cast<Humanoid*>(player));
    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    const ThrowMoveData* throwData = LookupThrowMoveData(move->address);
    if (frame > 0 && move->throwAttachFrame >= frame && target) {
        FaceThing(target, 0);
    }

    if (frame == move->throwAttachFrame) {
        if (target) {
            LVector local = throwData
                ? throwData->attachVector
                : LVector{ static_cast<s32>(move->throwAttachX),
                           static_cast<s32>(move->throwAttachY),
                           static_cast<s32>(move->throwAttachZ) };
            LVector world = {};
            GetObjectToWorldSpaceVector(local, world);

            target->homePos.x = pos.x + world.x;
            target->homePos.y = pos.y + world.y;
            target->homePos.z = pos.z + world.z;

            if (target->model) {
                Model* targetModel = static_cast<Model*>(target->model);
                targetModel->SetAnim(static_cast<s32>(move->throwTargetAnim), 0, 0, 0);
            }

            target->SetActionState(AS_THROW_CHARACTER_RECEIVE, 0);
            target->FaceThing(this, 0);
            target->DropPickup(1, 1);
            target->SetHumanoidTarget(this);
            target->field528 = static_cast<s32>(move->address);
        }

        if (isPlayerAttacker) {
            if (target && g_hud) {
                g_hud->SetFoe(target);
            }
            if (player && player->PlayerSingleEncounterCheak()) {
                player->LoadDialog(0x4D, 0x33);
            }
        }
    }

    if (frame == move->throwReleaseFrame && target) {
        FightingCollision::Set(target, this);

        LVector local = throwData
            ? throwData->releaseVector
            : LVector{ static_cast<s32>(move->throwVectorX),
                       static_cast<s32>(move->throwVectorY),
                       static_cast<s32>(move->throwVectorZ) };
        LVector world = {};
        GetObjectToWorldSpaceVector(local, world);

        target->contactForce.x += world.x;
        target->contactForce.y += world.y;
        target->contactForce.z += world.z;
        target->SetActionState(AS_THROW_FREE_FALL, 0);
        target->field528 = static_cast<s32>(move->address);
        target->ReleaseTarget();
        ReleaseTarget();
    }

    if (frame == move->throwImpactFrame) {
        if (isPlayerAttacker && player) {
            player->PlayCombatThrowDialog();
        }

        if (rightHandObj) {
            const u16 oldDurability = rightHandObj->health;
            rightHandObj->health = (oldDurability <= 1)
                ? 0
                : static_cast<u16>(oldDurability - 1);

            Thing* brokenWeapon = rightHandObj;
            if (brokenWeapon->health == 0 && move->weaponBreakOnEmpty != 0) {
                DropPickup(1, 1);

                if (rightHandObj == brokenWeapon) {
                    rightHandObj = nullptr;
                    flags2 &= ~1u;
                }
                if (leftHandObj == brokenWeapon) {
                    leftHandObj = nullptr;
                    flags2 &= ~2u;
                }

                brokenWeapon->Kill();
                field472 = 0;
            }
        }
    }

    if (frame == move->throwScoreFrame && isPlayerAttacker) {
        if (target && g_hud) {
            g_hud->SetFoe(target);
        }
        if (g_scoreManager) {
            const s32 fightingPoints = static_cast<s32>(move->fightingPoints);
            g_scoreManager->AddFightingPoints(fightingPoints);
            if (move->stylePointsFlag) {
                g_scoreManager->AddStylePoints(fightingPoints);
            }
        }
    }

    if (frame >= move->combatWindowStart && move->combatWindowEnd >= frame) {
        flags |= 2u;
    }

    if (frame >= move->moveWindowStart && move->moveWindowEnd >= frame) {
        const s32 moveAmount = static_cast<s32>(static_cast<u8>(move->moveDelta));
        homePos.x += static_cast<s32>(
            (static_cast<s64>(rmSin16(orientation.y)) * static_cast<s64>(moveAmount)) >> 16);
        homePos.z += static_cast<s32>((static_cast<s64>(
                                      rmSin16(static_cast<s16>(orientation.y) + 0x4000))
                                  * static_cast<s64>(moveAmount))
                                 >> 16);
    }

    return 1;
}

// PSX: BodyThrowAttack__8Humanoidl (HUMANOID.CPP:5666)
s32 Humanoid::BodyThrowAttack(s32 radius) {
    MARKFUNCTION(0x8006909C);

    HumanoidModel* hm = static_cast<HumanoidModel*>(model);
    AnimationMatrices* animMatrices = hm ? hm->animMatrices : nullptr;
    if (!animMatrices) {
        return 0;
    }

    FightingCollisionAttackType attack = {};
    attack.attackType = 0;
    animMatrices->GetAttack(5u, attack.startA, attack.endA);
    attack.radiusA = radius;
    attack.hasSecondary = 0;

    Humanoid* victims[4] = {};
    s32 victimCount = FightingCollision::CheckAttack(victims, 4, this, &attack);
    if (victimCount > 4) {
        victimCount = 4;
    }

    Humanoid* latchedTarget = reinterpret_cast<Humanoid*>(field256);
    for (s32 i = 0; i < victimCount; i++) {
        Humanoid* victim = victims[i];
        if (!victim || victim == latchedTarget) {
            continue;
        }

        const s32 impactRegion = victim->GetImpactRegion(attack.endA);

        victim->HandleCollision(
            this,
            1,
            COLLISION_TAG_HIT_TYPE,
            3,
            COLLISION_TAG_IMPACT_REGION,
            impactRegion,
            COLLISION_TAG_DAMAGE,
            10,
            COLLISION_TAG_END);

        if (g_scoreManager) {
            g_scoreManager->AddFightingPoints(200);
            g_scoreManager->AddStylePoints(100);
        }
    }

    return victimCount > 0;
}

// PSX: ThrowCharacterReceive__8Humanoid (HUMANOID.CPP:5541)
s32 Humanoid::ThrowCharacterReceive() {
    MARKFUNCTION(0x80068EF8);

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    if (!anim) {
        return 0;
    }

    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);

    flags |= TF_ON_GROUND;
    maxFallDivisor = 0;
    BodyThrowAttack(192);

    const PsxFightingMoveRaw* move = ResolveFightingMoveAddress(static_cast<u32>(field528));
    if (move) {
        const u8 throwDamage = LookupThrowDamageByte(move->address);
        if (move->throwScoreFrame == frame) {
            SubtractHitPoints(throwDamage);
        }

        if (LookupThrowShockFrame(move->address) == frame) {
            Shock(ShockEnum::SHOCK_6);
        }
    }

    if (anim->loopCount > 0) {
        if (move && move->throwScoreFrame == 127) {
            const u8 throwDamage = LookupThrowDamageByte(move->address);
            SubtractHitPoints(throwDamage);
        }
        SetActionState(AS_SPIN_BACK_RECOVER, 0);
        return 1;
    }

    return 0;
}

// PSX: ThrowFreeFall__8Humanoid (HUMANOID.CPP:5622)
s32 Humanoid::ThrowFreeFall() {
    MARKFUNCTION(0x80068FFC);

    if (((flags >> 12) & 1u) != 0 && velocity.y == 0) {
        const PsxFightingMoveRaw* move = ResolveFightingMoveAddress(static_cast<u32>(field528));
        if (move) {
            const u8 throwDamage = LookupThrowDamageByte(move->address);
            SubtractHitPoints(throwDamage);
        }

        SetActionState(AS_FLYING_BACK_CHECK, 0);
        return 1;
    }

    gravity = 0;
    return BodyThrowAttack(384);
}

// PSX: ProcessFightingComboNode__8Humanoid (HUMANOID.CPP:8363)
s32 Humanoid::ProcessFightingComboNode() {
    MARKFUNCTION(0x8006BFE0);

    if (!model) {
        return 0;
    }

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = static_cast<AnimStructure*>(m->animStructure);
    const FightingComboNode* currentNode =
        ResolveFightingNodeAddressConst(static_cast<u32>(field484));
    if (!anim || !currentNode || !currentNode->moveData) {
        SetActionState(AS_STAND, 0);
        return 0;
    }

    const PsxFightingMoveRaw* currentMove = currentNode->moveData;
    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
    ProcessFightingMove(currentMove, frame);

    if (field488 == 0) {
        field488 = FindChildWithRequestedCommand(currentNode, static_cast<u32>(commandBits), frame);
        if (field488 == 0) {
            if (anim->loopCount > 0) {
                if (this == static_cast<Humanoid*>(Player::s_player)) {
                    KillDialog(0, 0, 512);
                }
                ReSyncOrientation(currentMove);
                field484 = 0;
                ReleaseTarget();
                SetActionState(AS_STAND, 0);
                SetIdleAnimation(0, 0);
            }
            return 0;
        }
    }

    const FightingComboNode* nextNode =
        ResolveFightingNodeAddressConst(static_cast<u32>(field488));
    if (!nextNode || !nextNode->moveData) {
        field488 = 0;
        return 0;
    }

    if (frame < nextNode->field05) {
        return 0;
    }

    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    const u8 nextType = nextNode->moveData->fightingType;
    if (!target || target->actionState != AS_THROW_CHARACTER_RECEIVE) {
        target = FightTargetAndThrowLatch(nextType);
    }

    bool allowThrow = target != nullptr && nextType == 2;
    if (allowThrow && rightHandObj && thingType != 0) {
        allowThrow = false;
    }

    bool allowNonThrow = false;
    if (nextType != 2) {
        if (freeFormFightingMode != 0 || !nextNode->field03 || field472 != 0) {
            allowNonThrow = true;
        }
        else if (target) {
            const u32 targetState = static_cast<u32>(target->actionState);
            // PSX accepts 53, 57, 58, and 63 in this continuation gate.
            allowNonThrow = (targetState == 53 || targetState == 57
                || targetState == static_cast<u32>(AS_FLYING_BACK_LAND)
                || targetState == static_cast<u32>(AS_THROW_FREE_FALL));
        }
    }

    bool allowCurrentMove = true;
    if (currentMove->fightingType == 1 && !rightHandObj) {
        const PsxFightingJointRaw* currentJoint = ResolveFightingJointAddress(currentMove->data20);
        allowCurrentMove = !currentJoint || currentJoint->SoundEvent() < 10
            || currentJoint->SoundEvent() >= 13;
    }

    if ((allowCurrentMove && allowNonThrow) || allowThrow) {
        SetHumanoidTarget(target);
        return SetCurrentFightingNode();
    }

    if (anim->loopCount > 0) {
        if (this == static_cast<Humanoid*>(Player::s_player)) {
            KillDialog(0, 0, 512);
        }
        ReSyncOrientation(currentMove);
        field484 = 0;
        field488 = 0;
        ReleaseTarget();
        SetActionState(AS_STAND, 0);
        SetIdleAnimation(0, 0);
    }

    return 0;
}

// PSX: TestAndSetWeaponKungFU__8Humanoid (HUMANOID.CPP:8569)
s32 Humanoid::TestAndSetWeaponKungFU() {
    MARKFUNCTION(0x8006C2C8);

    u32 weaponSystemAddress = rightHandObj ? static_cast<Pickup*>(rightHandObj)->fightingSystemRoot : 0;
    if (!weaponSystemAddress) {
        weaponSystemAddress = leftHandObj ? static_cast<Pickup*>(leftHandObj)->fightingSystemRoot : 0;
        if (!weaponSystemAddress) {
            return 0;
        }
    }

    FightingComboNode* weaponRoot = ResolveFightingRootAddress(weaponSystemAddress);
    if (!weaponRoot) {
        return 0;
    }

    defaultFightingSystem = weaponRoot;
    return 1;
}

// PSX: TestWallContextFightingRequestRemap__8Humanoid (HUMANOID.CPP:8607)
s32 Humanoid::TestWallContextFightingRequestRemap() {
    MARKFUNCTION(0x8006C31C);

    const s32 angleThreshold =
        (static_cast<s32>(WALL_KICK_ANGLE_THRESHOLD_DEGREES) << 16) / 360;
    s32 wallAngle = 0;
    LVector hitPoint = {};
    if (!CheckWallConstraint(
            WALL_KICK_MIN_WALL_HEIGHT,
            WALL_KICK_TRACE_DISTANCE,
            angleThreshold,
            wallAngle,
            hitPoint)) {
        return 0;
    }

    if (runSpeed != 0 && HasKick()) {
        commandBits = 0;
        RequestAction(22);
        homePos = hitPoint;
        return 1;
    }

    return 0;
}

// PSX: EnterCombatCombo__8Humanoid (HUMANOID.CPP:7968)
s32 Humanoid::EnterCombatCombo() {
    MARKFUNCTION(0x8006B8C8);

    const Pickup* heldPickup = rightHandObj
        ? static_cast<const Pickup*>(rightHandObj)
        : static_cast<const Pickup*>(leftHandObj);

    if (!TestAndSetWeaponKungFU()) {
        defaultFightingSystem = fightingSystem;
    }

    TestWallContextFightingRequestRemap();

    if (TestAndSetBackGrab() == 1) {
        return 1;
    }

    field488 = FindSiblingWithRequestedCommand(
        static_cast<const FightingComboNode*>(defaultFightingSystem),
        static_cast<u32>(commandBits));

    if (field488 == 0) {
        if (heldPickup) {
            LOG(
                "[PickupCombat] no node thingType=%u pickupType=%u cmd=0x%08X root=0x%08X state=%d",
                static_cast<u32>(thingType),
                static_cast<u32>(heldPickup->thingType),
                static_cast<u32>(commandBits),
                static_cast<u32>(reinterpret_cast<uintptr_t>(defaultFightingSystem)),
                actionState);
        }
        return 0;
    }

    const FightingComboNode* requestedNode =
        ResolveFightingNodeAddressConst(static_cast<u32>(field488));
    if (!requestedNode || !requestedNode->moveData) {
        if (heldPickup) {
            LOG(
                "[PickupCombat] bad node thingType=%u pickupType=%u cmd=0x%08X node=0x%08X state=%d",
                static_cast<u32>(thingType),
                static_cast<u32>(heldPickup->thingType),
                static_cast<u32>(commandBits),
                static_cast<u32>(field488),
                actionState);
        }
        field488 = 0;
        return 0;
    }

    if (requestedNode->requestedCommand == 20) {
        SetHumanoidTarget(FindFoe(FIGHT_DISTANCE, FIGHT_HALF_ANGLE, 1));
        SetActionState(AS_COUNTER_ATTACK_PRE_LATCH, 0);
        return 0;
    }

    Humanoid* target = FightTargetAndThrowLatch(requestedNode->moveData->fightingType);
    if (requestedNode->moveData->fightingType == 2 && !target) {
        if (heldPickup) {
            LOG(
                "[PickupCombat] throw latch miss thingType=%u pickupType=%u cmd=0x%08X node=0x%08X anim=%d state=%d",
                static_cast<u32>(thingType),
                static_cast<u32>(heldPickup->thingType),
                static_cast<u32>(commandBits),
                static_cast<u32>(requestedNode->psxAddress),
                static_cast<s32>(requestedNode->moveData->anim),
                actionState);
        }
        field488 = 0;
        return 0;
    }

    SetHumanoidTarget(target);
    LoadCombatDialog();
    SetCurrentFightingNode();
    return 1;
}

// PSX: _CounterAttack__8Humanoid (HUMANOID.CPP:5861)
s32 Humanoid::CounterAttack() {
    MARKFUNCTION(0x80069420);

    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    if (!target) {
        SetActionState(AS_STAND, 0);
        return 0;
    }

    u32 requestedBits = 0x1000000u;
    const FightingComboNode* targetNode =
        ResolveFightingNodeAddressConst(static_cast<u32>(target->field484));
    if (targetNode && targetNode->moveData) {
        const PsxFightingJointRaw* targetJoint =
            ResolveFightingJointAddress(targetNode->moveData->data20);
        if (targetJoint) {
            const u8 jointIndex = targetJoint->JointIndex();
            if (jointIndex == 2) {
                requestedBits = 0x2000000u;
            }
            else if (jointIndex == 3) {
                requestedBits = 0x4000000u;
            }
            else if (jointIndex == 4) {
                requestedBits = 0x8000000u;
            }
        }
    }

    const FightingComboNode* currentNode =
        ResolveFightingNodeAddressConst(static_cast<u32>(field484));
    field488 = FindChildWithRequestedCommand(currentNode, requestedBits);
    if (field488 != 0) {
        return SetCurrentFightingNode();
    }

    SetActionState(AS_COUNTER_ATTACK_RECOVERY, 0);
    return 0;
}

// PSX: _CounterAttackPreLatch__8Humanoid (HUMANOID.CPP:5928)
s32 Humanoid::CounterAttackPreLatch() {
    MARKFUNCTION(0x80069518);

    const FightingComboNode* currentNode =
        ResolveFightingNodeAddressConst(static_cast<u32>(field484));
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    if (!currentNode || !currentNode->moveData || !anim) {
        return 0;
    }

    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
    if (frame >= currentNode->moveData->combatWindowStart) {
        SetActionState(AS_COUNTER_ATTACK_LATCH, 0);
        return 1;
    }

    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    if (target) {
        FaceThingDesired(target);
        FaceAngleY(faceAngle, 0);
    }
    return 0;
}

// PSX: _CounterAttackLatch__8Humanoid (HUMANOID.CPP:5963)
s32 Humanoid::CounterAttackLatch() {
    MARKFUNCTION(0x800695A8);

    const FightingComboNode* currentNode =
        ResolveFightingNodeAddressConst(static_cast<u32>(field484));
    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    if (!currentNode || !currentNode->moveData || !anim) {
        return 0;
    }

    const s16 frame = static_cast<s16>((u32)anim->currentFrame >> 16);
    if (frame > currentNode->moveData->combatWindowEnd) {
        SetActionState(AS_COUNTER_ATTACK_RECOVERY, 0);
        return 1;
    }

    Humanoid* target = reinterpret_cast<Humanoid*>(field256);
    if (target) {
        FaceThingDesired(target);
        FaceAngleY(faceAngle, 0);
    }
    return 0;
}

// PSX: _CounterAttackRecovery__8Humanoid (HUMANOID.CPP:6005)
s32 Humanoid::CounterAttackRecovery() {
    MARKFUNCTION(0x80069638);

    Model* m = static_cast<Model*>(model);
    AnimStructure* anim = m ? static_cast<AnimStructure*>(m->animStructure) : nullptr;
    if (!anim) {
        return 0;
    }

    if (anim->loopCount > 0) {
        SetActionState(AS_STAND, 0);
        return 1;
    }

    return 0;
}

// PSX: PlayCombatThrowDialog__8Humanoid (HUMANOID.HPP:1388)
void Humanoid::PlayCombatThrowDialog() {
    MARKFUNCTION(0x8006D104);
}

// PSX: PlayCombatKnockDownDialog__8Humanoid15DamageTypesTags (HUMANOID.HPP:1387)
void Humanoid::PlayCombatKnockDownDialog(s32 /*damageType*/) {
    MARKFUNCTION(0x8006D10C);
}

// PSX: LoadCombatDialog__8Humanoid (HUMANOID.HPP:1386)
s32 Humanoid::LoadCombatDialog() {
    MARKFUNCTION(0x8006D114);
    return 0;
}

// PSX: HandleHitShock__8Humanoid15DamageTypesTags (HUMANOID.HPP:1052)
void Humanoid::HandleHitShock(s32 /*damageType*/) {
    MARKFUNCTION(0x8006D11C);
}

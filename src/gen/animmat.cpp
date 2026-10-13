#include "gen/common.h"
#include "gen/animmat.h"
#include "gen/model.h"
#include "gen/psxmath_helpers.h"
#include "gen/time.h"
#include "ai/humanoid.h"
#include "extra/cheats.h"
#include "p3d/context.h"
#include "p3d/skeleton.h"
#include "p3d/matrix.h"
#include "p3d/hash.h"
#include <cmath>
#include <cstring>

static void BuildPsxIdentityMatrix(s32* matrixData) {
    std::memset(matrixData, 0, sizeof(s32) * 8);

    // PSX MATRIX rotation is Q12 fixed-point shorts at [0..8].
    s16* rot = reinterpret_cast<s16*>(matrixData);
    rot[0] = 0x1000;
    rot[4] = 0x1000;
    rot[8] = 0x1000;
}

static s16 FloatToPsxMatrixElem(f32 value) {
    s32 fixed = (s32)std::lround(value * 4096.0f);
    if (fixed < -32768) {
        fixed = -32768;
    }
    else if (fixed > 32767) {
        fixed = 32767;
    }
    return static_cast<s16>(fixed);
}

static void WritePsxMatrix(s32* matrixData, const Mat4& jointMatrix) {
    std::memset(matrixData, 0, sizeof(s32) * 8);

    s16* rot = reinterpret_cast<s16*>(matrixData);
    rot[0] = FloatToPsxMatrixElem(jointMatrix.m[0]);
    rot[1] = FloatToPsxMatrixElem(jointMatrix.m[1]);
    rot[2] = FloatToPsxMatrixElem(jointMatrix.m[2]);
    rot[3] = FloatToPsxMatrixElem(jointMatrix.m[4]);
    rot[4] = FloatToPsxMatrixElem(jointMatrix.m[5]);
    rot[5] = FloatToPsxMatrixElem(jointMatrix.m[6]);
    rot[6] = FloatToPsxMatrixElem(jointMatrix.m[8]);
    rot[7] = FloatToPsxMatrixElem(jointMatrix.m[9]);
    rot[8] = FloatToPsxMatrixElem(jointMatrix.m[10]);

    matrixData[5] = (s32)jointMatrix.GetTransX();
    matrixData[6] = (s32)jointMatrix.GetTransY();
    matrixData[7] = (s32)jointMatrix.GetTransZ();
}

// PSX: _17AnimationMatrices (ANIMMAT.CPP:550, 0x80078880)
AnimationMatrices::AnimationMatrices() {
    MARKFUNCTION(0x80078880);

    humanoid = nullptr;
    copied = 0;
    extraMask = 0;

    current = &matricesA[0][0];
    previous = &matricesB[0][0];

    for (u32 i = 0; i < 10; i++) {
        BuildPsxIdentityMatrix(matricesA[i]);
        BuildPsxIdentityMatrix(matricesB[i]);
    }

    for (s32 i = 0; i < AM_NUM_SLOTS; i++) {
        boneJointIndex[i] = -1;
    }
    bonesCached = false;
    captureEnabled = true;
}

// PSX: SetHumanoid__17AnimationMatricesP8Humanoid (ANIMMAT.CPP:577, 0x80078908)
void AnimationMatrices::SetHumanoid(void* owner) {
    MARKFUNCTION(0x80078908);
    humanoid = owner;
}

// PSX: GetHumanoid__17AnimationMatrices (ANIMMAT.CPP:587, 0x80078910)
void* AnimationMatrices::GetHumanoid() const {
    MARKFUNCTION(0x80078910);
    return humanoid;
}

// PSX: Copy__C17AnimationMatrices (ANIMMAT.CPP:597, 0x8007891C)
s32 AnimationMatrices::Copy() const {
    MARKFUNCTION(0x8007891C);
    return copied;
}

// PSX: Swap__17AnimationMatrices (ANIMMAT.CPP:831, 0x80078E80)
s32 AnimationMatrices::Swap() {
    MARKFUNCTION(0x80078E80);
    s32* oldCurrent = current;
    copied = 0;
    current = previous;
    previous = oldCurrent;
    return static_cast<s32>(reinterpret_cast<uintptr_t>(current) & 0xFFFFFFFFu);
}

// PSX: GetMatrix__C17AnimationMatricesUl (ANIMMAT.CPP:849, 0x80078E98)
s32* AnimationMatrices::GetMatrix(u32 joint) {
    MARKFUNCTION(0x80078E98);
    if (joint >= 10 || !current) {
        return nullptr;
    }
    return current + (joint * 8);
}

const s32* AnimationMatrices::GetMatrix(u32 joint) const {
    MARKFUNCTION(0x80078E98);
    if (joint >= 10 || !current) {
        return nullptr;
    }
    return current + (joint * 8);
}

s32* AnimationMatrices::GetMatrix(AnimationMatrices* am, u32 joint) {
    if (!am) {
        return nullptr;
    }
    return am->GetMatrix(joint);
}

const s32* AnimationMatrices::GetMatrix(const AnimationMatrices* am, u32 joint) {
    if (!am) {
        return nullptr;
    }
    return am->GetMatrix(joint);
}

// PSX: GetAttack__C17AnimationMatricesUlR10tagLVectorT2 (ANIMMAT.CPP:878, 0x80078EB8)
s32 AnimationMatrices::GetAttack(u32 joint, LVector& outPrev, LVector& outCur) const {
    MARKFUNCTION(0x80078EB8);
    if (joint >= 10) {
        return 0;
    }
    s32 offset = joint * 8;
    outPrev.x = previous[offset + 5];
    outPrev.y = previous[offset + 6];
    outPrev.z = previous[offset + 7];
    outCur.x = current[offset + 5];
    outCur.y = current[offset + 6];
    outCur.z = current[offset + 7];
    return 1;
}

s32 AnimationMatrices::GetWeaponAttack(u32 joint, const LVector& localOffset, LVector& outPrev, LVector& outCur) const {
    MARKFUNCTION(0x80078F64);
    if (joint >= 10) {
        return 0;
    }

    const s32 offset = joint * 8;

    const s32* prevMatrix = previous + offset;
    const s16* prevRot = reinterpret_cast<const s16*>(prevMatrix);
    outPrev.x = prevMatrix[5]
        + (s32)((((s64)localOffset.x * prevRot[0])
            + ((s64)localOffset.y * prevRot[1])
            + ((s64)localOffset.z * prevRot[2])) >> 12);
    outPrev.y = prevMatrix[6]
        + (s32)((((s64)localOffset.x * prevRot[3])
            + ((s64)localOffset.y * prevRot[4])
            + ((s64)localOffset.z * prevRot[5])) >> 12);
    outPrev.z = prevMatrix[7]
        + (s32)((((s64)localOffset.x * prevRot[6])
            + ((s64)localOffset.y * prevRot[7])
            + ((s64)localOffset.z * prevRot[8])) >> 12);

    const s32* curMatrix = current + offset;
    const s16* curRot = reinterpret_cast<const s16*>(curMatrix);
    outCur.x = curMatrix[5]
        + (s32)((((s64)localOffset.x * curRot[0])
            + ((s64)localOffset.y * curRot[1])
            + ((s64)localOffset.z * curRot[2])) >> 12);
    outCur.y = curMatrix[6]
        + (s32)((((s64)localOffset.x * curRot[3])
            + ((s64)localOffset.y * curRot[4])
            + ((s64)localOffset.z * curRot[5])) >> 12);
    outCur.z = curMatrix[7]
        + (s32)((((s64)localOffset.x * curRot[6])
            + ((s64)localOffset.y * curRot[7])
            + ((s64)localOffset.z * curRot[8])) >> 12);

    return 1;
}

s32 AnimationMatrices::GetMatrixPair(u32 joint, s32 outPrev[8], s32 outCur[8]) const {
    if (joint >= 10 || !previous || !current) {
        return 0;
    }

    const s32 offset = joint * 8;
    std::memcpy(outPrev, previous + offset, sizeof(s32) * 8);
    std::memcpy(outCur, current + offset, sizeof(s32) * 8);
    return 1;
}

void AnimationMatrices::SetCaptureEnabled(s32 enable) {
    captureEnabled = (enable != 0);
}

s32 AnimationMatrices::CaptureEnabled() const {
    return captureEnabled ? 1 : 0;
}

// PSX bone name table: 10 tracked joints matching SetupModelCallbacks order.
// Slot 0: Head, 1: L Hand, 2: R Hand, 3: L Foot, 4: R Foot,
// 5: Pelvis, 6: L UpperArm, 7: R UpperArm, 8: L Thigh, 9: R Thigh
static const char* const AM_JointNames[AM_NUM_SLOTS] = {
    "Bip01 Head",
    "Bip01 L Hand",
    "Bip01 R Hand",
    "Bip01 L Foot",
    "Bip01 R Foot",
    "Bip01 Pelvis",
    "Bip01 L UpperArm",
    "Bip01 R UpperArm",
    "Bip01 L Thigh",
    "Bip01 R Thigh",
};

static void BuildModelWorldMatrix(const Model* model, Mat4& worldMatrix) {
    worldMatrix = Mat4();
    if (!model) {
        return;
    }

    p3dBuildRotMatrixZYX(model->rotX, model->rotY, model->rotZ, worldMatrix);

    const SModel* skeletalModel = static_cast<const SModel*>(model);
    worldMatrix.ScaleRotation(FIX16_TO_FLOAT(skeletalModel->scale));
    worldMatrix.SetTranslation((f32)model->posX, (f32)model->posY, (f32)model->posZ);
}

// Builds the world matrix from the Humanoid's own authoritative pos/orientation
// rather than the model's render-only posX/Y/Z/rotX/Y/Z. Those model fields are
// only written once per render frame from Humanoid::Draw() (and, under
// HIGH_FPS_PLAY_PRESENTATION, hold an interpolated/render-only pose) - using
// them to build the attack-joint capture matrix made the captured matrix go
// stale or freeze whenever multiple logic ticks ran between two Draw() calls.
static void BuildHumanoidWorldMatrix(const Humanoid* owner, Mat4& worldMatrix) {
    worldMatrix = Mat4();
    if (!owner) {
        return;
    }

    p3dBuildRotMatrixZYX(owner->orientation.x, owner->orientation.y, owner->orientation.z, worldMatrix);

    const SModel* skeletalModel = owner->model ? static_cast<const SModel*>(static_cast<const Model*>(owner->model)) : nullptr;
    if (skeletalModel) {
        worldMatrix.ScaleRotation(FIX16_TO_FLOAT(skeletalModel->scale));
    }
    worldMatrix.SetTranslation((f32)owner->pos.x, (f32)owner->pos.y, (f32)owner->pos.z);
}


static void BuildHeadTrackOverrideMatrix(const LVector& col0, const LVector& col1, const LVector& col2,
    const STreeJoint* joint, Mat4& overrideMatrix) {
    overrideMatrix = Mat4();
    overrideMatrix.m[0] = FIX16_TO_FLOAT(col0.x);
    overrideMatrix.m[1] = FIX16_TO_FLOAT(col0.y);
    overrideMatrix.m[2] = FIX16_TO_FLOAT(col0.z);
    overrideMatrix.m[4] = FIX16_TO_FLOAT(col1.x);
    overrideMatrix.m[5] = FIX16_TO_FLOAT(col1.y);
    overrideMatrix.m[6] = FIX16_TO_FLOAT(col1.z);
    overrideMatrix.m[8] = FIX16_TO_FLOAT(col2.x);
    overrideMatrix.m[9] = FIX16_TO_FLOAT(col2.y);
    overrideMatrix.m[10] = FIX16_TO_FLOAT(col2.z);
    overrideMatrix.SetTranslation((f32)joint->translationX, (f32)joint->translationY, (f32)joint->translationZ);
}

static void PsxVecTimesRotMatrixTrunc(const LVector& in, const Mat4& matrix, LVector& out) {
    out.x = PsxTruncToS32((f32)in.x * matrix.m[0] + (f32)in.y * matrix.m[4] + (f32)in.z * matrix.m[8]);
    out.y = PsxTruncToS32((f32)in.x * matrix.m[1] + (f32)in.y * matrix.m[5] + (f32)in.z * matrix.m[9]);
    out.z = PsxTruncToS32((f32)in.x * matrix.m[2] + (f32)in.y * matrix.m[6] + (f32)in.z * matrix.m[10]);
}

static void HeadTrack(Humanoid* owner, STreeJoint* joint, const Mat4& currentMatrix) {
    if (!owner || !joint) {
        return;
    }

    Thing* targetThing = reinterpret_cast<Thing*>((intptr_t)owner->field256);
    if ((owner->flags2 & TF2_BIT3) == 0 || !targetThing) {
        joint->flags = (joint->flags & ~(STF_TRANSFORM | STF_MULT_MATRIX)) | STF_TRANSFORM;
        joint->useOverrideMatrix = false;
        return;
    }

    joint->flags = (joint->flags & ~(STF_TRANSFORM | STF_MULT_MATRIX)) | STF_MULT_MATRIX;

    LVector targetPos = targetThing->pos;
    if (targetThing->thingType < 0x1D && targetThing->model) {
        HumanoidModel* targetModel = static_cast<HumanoidModel*>(static_cast<Model*>(targetThing->model));
        if (targetModel->animMatrices) {
            const s32* targetMatrix = targetModel->animMatrices->GetMatrix(0);
            if (targetMatrix) {
                targetPos = { targetMatrix[5], targetMatrix[6], targetMatrix[7] };
            }
        }
    }

    HumanoidModel* model = owner->model
        ? static_cast<HumanoidModel*>(static_cast<Model*>(owner->model))
        : nullptr;
    if (!model) {
        joint->useOverrideMatrix = false;
        return;
    }

    Mat4 modelWorld;
    BuildModelWorldMatrix(model, modelWorld);

    Mat4 worldCurrent = modelWorld * currentMatrix;

    Mat4 inverseCurrent;
    PsxInverseOrthMatrix(worldCurrent, inverseCurrent);

    LVector localTarget = {};
    PsxVecTimesMatrixTrunc(targetPos, inverseCurrent, localTarget);
    localTarget.x -= joint->translationX;
    localTarget.y -= joint->translationY;
    localTarget.z -= joint->translationZ;

    LVector sphericalTarget = {};
    PsxCartesianToSpherical(localTarget, sphericalTarget);

    LVector sphericalCurrent = {};
    PsxCartesianToSpherical(model->headTrackDir, sphericalCurrent);
    (void)sphericalCurrent;

    sphericalTarget.x = FIX16_ONE;
    if (sphericalTarget.y >= 0x4000) {
        sphericalTarget.y = (sphericalTarget.y <= 0x8000) ? 0x4000 : 0xC000;
    }
    else if (sphericalTarget.y < -16383) {
        sphericalTarget.y = (sphericalTarget.y < -32767) ? -49152 : -16384;
    }

    PsxSphericalToCartesian(sphericalTarget, model->headTrackDir);

    LVector heading = {};
    rmV3Normalize(&heading, &model->headTrackDir);

    LVector worldUpPoint = { 0, FIX16_ONE, 0 };
    LVector localUp = {};
    PsxVecTimesRotMatrixTrunc(worldUpPoint, inverseCurrent, localUp);

    LVector col1 = {};
    PsxV3Cross16(&col1, &heading, &localUp);
    if (rmMag3((f32)col1.x, (f32)col1.y, (f32)col1.z) < 0x100) {
        const LVector fallbackUp = (PsxAbsS32(heading.y) < 0x7000)
            ? LVector{ 0, FIX16_ONE, 0 }
            : LVector{ FIX16_ONE, 0, 0 };
        PsxV3Cross16(&col1, &heading, &fallbackUp);
    }
    rmV3Normalize(&col1, &col1);

    LVector col0 = {};
    PsxV3Cross16(&col0, &col1, &heading);
    rmV3Normalize(&col0, &col0);

    BuildHeadTrackOverrideMatrix(col0, col1, heading, joint, joint->overrideMatrix);
    joint->useOverrideMatrix = true;
}

#if NEW_CHEATS
static void BobbleHeadUpdate(Humanoid* owner, STreeJoint* joint) {
    if (!owner || !joint) {
        return;
    }
    HumanoidModel* model = owner->model ? static_cast<HumanoidModel*>(owner->model) : nullptr;
    if (!model) {
        return;
    }

    const u32 currentTick = g_time ? g_time->GetFrameCounter() : 0;
    if (model->bobbleTick != currentTick) {
        model->bobbleTick = currentTick;

        const s32 px = owner->pos.x;
        const s32 pz = owner->pos.z;

        if (!model->bobblePrevValid) {
            model->bobblePrevValid = true;
            model->bobblePrevX = px;
            model->bobblePrevZ = pz;
        }

        s32 dx = px - model->bobblePrevX;
        s32 dz = pz - model->bobblePrevZ;
        model->bobblePrevX = px;
        model->bobblePrevZ = pz;

        // Ignore teleports / respawns so the head doesn't launch on a warp.
        constexpr s32 kTeleportThreshold = 4000;
        if (dx > kTeleportThreshold || dx < -kTeleportThreshold
            || dz > kTeleportThreshold || dz < -kTeleportThreshold) {
            dx = 0;
            dz = 0;
            model->bobbleKick = 0.0f;
        }

        const f32 fdx = (f32)dx;
        const f32 fdz = (f32)dz;
        const f32 speed = std::sqrt(fdx * fdx + fdz * fdz);

        const f32 adx = (f32)(dx - model->bobblePrevDX);
        const f32 adz = (f32)(dz - model->bobblePrevDZ);
        const f32 accel = std::sqrt(adx * adx + adz * adz);
        model->bobblePrevDX = dx;
        model->bobblePrevDZ = dz;

        // Tunable feel. Amplitudes are in binary-angle units (0x10000 = 360 deg,
        // ~182 per degree). World movement is in the thousands per running frame.
        constexpr f32 kSpeedToAmp = 11.0f;   // continuous jiggle vs. speed
        constexpr f32 kMaxContAmp = 1400.0f; // ~7.7 deg steady wobble
        constexpr f32 kAccelToKick = 8.0f;   // extra wobble on accel spikes
        constexpr f32 kKickDecay = 0.85f;
        constexpr f32 kMaxKick = 3400.0f;
        constexpr f32 kMaxTotal = 4600.0f;   // ~25 deg hard cap
        constexpr f32 kPhaseStep = 1.0f;
        constexpr f32 kTwoPi = 6.2831853f;

        f32 targetAmp = speed * kSpeedToAmp;
        if (targetAmp > kMaxContAmp) {
            targetAmp = kMaxContAmp;
        }
        model->bobbleAmp += (targetAmp - model->bobbleAmp) * 0.2f;

        model->bobbleKick = model->bobbleKick * kKickDecay + accel * kAccelToKick;
        if (model->bobbleKick > kMaxKick) {
            model->bobbleKick = kMaxKick;
        }

        model->bobblePhase += kPhaseStep;
        if (model->bobblePhase > kTwoPi) {
            model->bobblePhase -= kTwoPi;
        }

        f32 total = model->bobbleAmp + model->bobbleKick;
        if (total > kMaxTotal) {
            total = kMaxTotal;
        }

        model->bobbleAngleX = std::sin(model->bobblePhase) * total;
        model->bobbleAngleZ = std::sin(model->bobblePhase * 0.8f + 1.7f) * total * 0.7f;
    }

    // Don't stack the offset on a joint that was not re-evaluated.
    if (model->bobbleApplied
        && joint->rotationX == model->bobbleAppliedRotX
        && joint->rotationZ == model->bobbleAppliedRotZ) {
        joint->rotationX = model->bobbleBaseRotX;
        joint->rotationZ = model->bobbleBaseRotZ;
    }
    model->bobbleBaseRotX = joint->rotationX;
    model->bobbleBaseRotZ = joint->rotationZ;

    joint->rotationX = (s16)(joint->rotationX + (s16)model->bobbleAngleX);
    joint->rotationZ = (s16)(joint->rotationZ + (s16)model->bobbleAngleZ);

    model->bobbleAppliedRotX = joint->rotationX;
    model->bobbleAppliedRotZ = joint->rotationZ;
    model->bobbleApplied = true;

    if (owner->thingType == AITypes::TT_PLAYER)
        joint->renderScale = 2.2f;
    else
        joint->renderScale = 2.8f;
}
#endif

static s32 AM_HeadCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->CopyMatrix(0, currentMatrix);
        Humanoid* owner = static_cast<Humanoid*>(animMatrices->GetHumanoid());
        HeadTrack(owner, joint, currentMatrix);
#if NEW_CHEATS
        if (IsCheatEnabled(CheatOption::BobbleHead)) {
            BobbleHeadUpdate(owner, joint);
        }
#endif
    }
    return 1;
}

static s32 AM_LHandCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->CopyMatrix(1, currentMatrix);
    }
    return 1;
}

static s32 AM_RHandCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->CopyMatrix(2, currentMatrix);
    }
    return 1;
}

static s32 AM_LFootCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->CopyMatrix(3, currentMatrix);
    }
    return 1;
}

static s32 AM_RFootCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->CopyMatrix(4, currentMatrix);
    }
    return 1;
}

static s32 AM_PelvisCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->CopyMatrix(5, currentMatrix);
    }
    return 1;
}

static s32 AM_LUpperArmCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->extraMask |= 0x40;
        animMatrices->CopyMatrix(6, currentMatrix);
    }
    return 1;
}

static s32 AM_RUpperArmCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->extraMask |= 0x80;
        animMatrices->CopyMatrix(7, currentMatrix);
    }
    return 1;
}

static s32 AM_LThighCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->extraMask |= 0x100;
        animMatrices->CopyMatrix(8, currentMatrix);
    }
    return 1;
}

static s32 AM_RThighCallback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& currentMatrix) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    if (animMatrices) {
        animMatrices->extraMask |= 0x200;
        animMatrices->CopyMatrix(9, currentMatrix);
    }
    return 1;
}

static s32 AM_Bip01Callback(STreeJoint* joint, u32 /*jointIndex*/, const Mat4& /*currentMatrix*/) {
    AnimationMatrices* animMatrices = static_cast<AnimationMatrices*>(joint ? joint->callbackData : nullptr);
    Humanoid* owner = animMatrices ? static_cast<Humanoid*>(animMatrices->GetHumanoid()) : nullptr;
    HumanoidModel* model = owner && owner->model
        ? static_cast<HumanoidModel*>(owner->model)
        : nullptr;
    if (!joint || !owner || !model) {
        return 1;
    }

    if ((owner->flags2 & TF2_NIS_ENTER) != 0) {
        if ((owner->flags2 & TF2_NIS_FROZEN) == 0) {
#if HIGH_FPS_PLAY_PRESENTATION
            const u32 currentTick = g_time ? g_time->GetFrameCounter() : 0;
            if (model->nisJointTranslationYTick != currentTick) {
                model->nisJointTranslationYTick = currentTick;
                joint->translationY = model->field116;
                model->field116 = 100;
                model->nisJointTranslationYRender = joint->translationY;
            }
            else {
                joint->translationY = model->nisJointTranslationYRender;
            }
#else
            joint->translationY = model->field116;
            model->field116 = 100;
#endif
        }
        if (((owner->flags2 >> 6) & 1) == 0) {
            joint->translationX = 0;
            joint->translationZ = 0;
        }
    }

    return 1;
}

void AnimationMatrices::SetupCallbacks(Model* model) {
    if (!model || !model->drawable) {
        return;
    }

    OriginalSTree* active = GetActiveSTree(model->drawable);
    STreeData* skeleton = active ? active->skeleton : nullptr;
    if (!skeleton || !skeleton->joints || skeleton->numJoints == 0) {
        return;
    }

    const DrawableSTree* drawable = (model->drawableType == 2)
        ? static_cast<DrawableSTree*>(model->drawable)
        : nullptr;
    const bool mirrored = drawable && ((drawable->mirrorFlags & 1) != 0);

    const STreeJointCallback leftHandCallback = mirrored ? AM_RHandCallback : AM_LHandCallback;
    const STreeJointCallback rightHandCallback = mirrored ? AM_LHandCallback : AM_RHandCallback;
    const STreeJointCallback leftFootCallback = mirrored ? AM_RFootCallback : AM_LFootCallback;
    const STreeJointCallback rightFootCallback = mirrored ? AM_LFootCallback : AM_RFootCallback;

    struct CallbackBinding {
        const char* name;
        STreeJointCallback callback;
        bool isPreCallback;
    };

    // Not static: the hand/foot entries depend on this model's mirror flag.
    const CallbackBinding bindings[] = {
        { "Bip01 Head", AM_HeadCallback, true },
        { "Bip01 L Hand", leftHandCallback, false },
        { "Bip01 R Hand", rightHandCallback, false },
        { "Bip01 L Foot", leftFootCallback, false },
        { "Bip01 R Foot", rightFootCallback, false },
        { "Bip01 Pelvis", AM_PelvisCallback, false },
        { "Bip01", AM_Bip01Callback, true },
    };

    for (const CallbackBinding& binding : bindings) {
        const u32 nameHash = p3dHash(binding.name);
        for (u32 jointIndex = 0; jointIndex < skeleton->numJoints; jointIndex++) {
            STreeJoint& joint = skeleton->joints[jointIndex];
            if (joint.nameUID != nameHash) {
                continue;
            }

            joint.callbackData = this;
            if (binding.isPreCallback) {
                joint.preCallback = binding.callback;
                joint.flags |= STF_PRE_CALLBACK_MASK;
            }
            else {
                joint.postCallback = binding.callback;
                joint.flags |= STF_POST_CALLBACK_MASK;
            }
            break;
        }
    }
}

void AnimationMatrices::SetupExtraCallbacks(Model* model) {
    MARKFUNCTION(0x80078BD0);

    if (!model || !model->drawable) {
        return;
    }

    OriginalSTree* active = GetActiveSTree(model->drawable);
    STreeData* skeleton = active ? active->skeleton : nullptr;
    if (!skeleton || !skeleton->joints || skeleton->numJoints == 0) {
        return;
    }

    static const STreeJointCallback callbacks[] = {
        AM_LUpperArmCallback,
        AM_RUpperArmCallback,
        AM_LThighCallback,
        AM_RThighCallback,
    };

    for (u32 slot = 6; slot < 10; slot++) {
        const u32 nameHash = p3dHash(AM_JointNames[slot]);
        for (u32 jointIndex = 0; jointIndex < skeleton->numJoints; jointIndex++) {
            STreeJoint& joint = skeleton->joints[jointIndex];
            if (joint.nameUID != nameHash) {
                continue;
            }

            joint.callbackData = this;
            joint.postCallback = callbacks[slot - 6];
            break;
        }
    }
}

s32 AnimationMatrices::SetExtraCallbacks(s32 matrixType, s32 enable) {
    MARKFUNCTION(0x80078CAC);

    Humanoid* owner = static_cast<Humanoid*>(GetHumanoid());
    HumanoidModel* model = owner && owner->model
        ? static_cast<HumanoidModel*>(owner->model)
        : nullptr;
    if (!model || !model->drawable) {
        return 0;
    }

    OriginalSTree* active = GetActiveSTree(model->drawable);
    STreeData* skeleton = active ? active->skeleton : nullptr;
    if (!skeleton || !skeleton->joints || skeleton->numJoints == 0) {
        return 0;
    }

    if (matrixType == -1) {
        for (u32 slot = 6; slot < 10; slot++) {
            const u32 nameHash = p3dHash(AM_JointNames[slot]);
            for (u32 jointIndex = 0; jointIndex < skeleton->numJoints; jointIndex++) {
                STreeJoint& joint = skeleton->joints[jointIndex];
                if (joint.nameUID == nameHash) {
                    joint.flags &= ~STF_POST_CALLBACK_MASK;
                    break;
                }
            }
        }
        return 1;
    }

    if (matrixType < 0 || matrixType >= AM_NUM_SLOTS) {
        return 0;
    }

    const u32 nameHash = p3dHash(AM_JointNames[matrixType]);
    for (u32 jointIndex = 0; jointIndex < skeleton->numJoints; jointIndex++) {
        STreeJoint& joint = skeleton->joints[jointIndex];
        if (joint.nameUID != nameHash) {
            continue;
        }

        if (enable) {
            joint.flags |= STF_POST_CALLBACK_MASK;
        }
        else {
            joint.flags &= ~STF_POST_CALLBACK_MASK;
            extraMask &= ~(1 << matrixType);
        }
        return 1;
    }

    return 0;
}

s32 AnimationMatrices::CopyMatrix(u32 joint, const Mat4& jointMatrix) {
    MARKFUNCTION(0x80078E0C);
    s32* matrixData = GetMatrix(joint);
    if (!matrixData) {
        return 0;
    }

    if (!captureEnabled) {
        return 1;
    }

    // This capture feeds attack-joint hit detection, which must reflect the
    // humanoid's authoritative simulation pos/orientation - never the render
    // context's currently-active world matrix (which reflects whatever was
    // last drawn, not necessarily this humanoid, and isn't updated at all
    // between repeated logic-tick captures within the same render frame) and
    // never the model's render-only posX/Y/Z (which only updates once per
    // Draw() call and can be an interpolated render-only pose).
    Humanoid* owner = static_cast<Humanoid*>(GetHumanoid());
    if (owner) {
        Mat4 worldMatrix;
        BuildHumanoidWorldMatrix(owner, worldMatrix);
        WritePsxMatrix(matrixData, worldMatrix * jointMatrix);
    }
    else {
        WritePsxMatrix(matrixData, jointMatrix);
    }

    copied = 1;
    return 1;
}

void AnimationMatrices::CacheBoneIndices(const STreeData* skeleton) {
    if (!skeleton || !skeleton->joints || skeleton->numJoints == 0) {
        return;
    }

    for (s32 slot = 0; slot < AM_NUM_SLOTS; slot++) {
        u32 nameHash = p3dHash(AM_JointNames[slot]);
        boneJointIndex[slot] = -2; // not found
        for (u32 j = 0; j < skeleton->numJoints; j++) {
            if (skeleton->joints[j].nameUID == nameHash) {
                boneJointIndex[slot] = (s32)j;
                break;
            }
        }
    }
    bonesCached = true;
}

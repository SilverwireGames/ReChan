#include "gen/animstruct.h"
#include "gen/paramanim.h"
#include "gen/animmgr.h"
#include "gen/model.h"
#include "gen/levelmgr.h"
#include "gen/time.h"
#include "gen/charmgr.h"
#include "ai/humanoid.h"
#include "p3d/p3dmath.h"
#include "pc/log.h"

static void BlendTreePose(s32 weight, const BlendPoseState* blendPose, STreeData* tree) {
    if (!blendPose || !blendPose->joints || !tree || !tree->joints) {
        return;
    }

    u32 jointCount = tree->numJoints;
    if (blendPose->jointCount < jointCount) {
        jointCount = blendPose->jointCount;
    }

    for (u32 jointIndex = 0; jointIndex < jointCount; jointIndex++) {
        STreeJoint& joint = tree->joints[jointIndex];
        const BlendJointPose& pose = blendPose->joints[jointIndex];

        joint.translationX = pose.translationX + (s32)(((s64)weight * (s64)(joint.translationX - pose.translationX)) >> 16);
        joint.translationY = pose.translationY + (s32)(((s64)weight * (s64)(joint.translationY - pose.translationY)) >> 16);
        joint.translationZ = pose.translationZ + (s32)(((s64)weight * (s64)(joint.translationZ - pose.translationZ)) >> 16);
        joint.rotationX = (s16)(pose.rotationX + (s32)(((s64)weight * (s64)(s32)(s16)(joint.rotationX - pose.rotationX)) >> 16));
        joint.rotationY = (s16)(pose.rotationY + (s32)(((s64)weight * (s64)(s32)(s16)(joint.rotationY - pose.rotationY)) >> 16));
        joint.rotationZ = (s16)(pose.rotationZ + (s32)(((s64)weight * (s64)(s32)(s16)(joint.rotationZ - pose.rotationZ)) >> 16));
    }
}

static STreeData* ResolveTransformAnimPuppetTree(const TransformAnim* animation) {
    if (!animation || !g_levelManager) {
        return nullptr;
    }

    auto ResolveSkeletonFromOriginal = [](OriginalBasic* original) -> STreeData* {
        if (!original) {
            return nullptr;
        }

        if (original->GetType() == 1) {
            OriginalSTree* stree = static_cast<OriginalSTree*>(original);
            return stree->skeleton;
        }

        if (original->GetType() != 2) {
            return nullptr;
        }

        // ETree carries its own joint list in the PSX path.
        OriginalETree* etree = static_cast<OriginalETree*>(original);
        if (etree->skeleton) {
            return etree->skeleton;
        }

        // Legacy fallback for older assets that only expose part joint hashes.
        for (u32 i = 0; i < etree->geoPartCount; i++) {
            u32 jointHash = etree->geoPartJointHashes ? etree->geoPartJointHashes[i] : 0;
            if (!jointHash) {
                continue;
            }

            OriginalBasic* streeBasic = g_levelManager->FindSTree((s32)jointHash);
            if (streeBasic && streeBasic->GetType() == 1) {
                OriginalSTree* stree = static_cast<OriginalSTree*>(streeBasic);
                return stree->skeleton;
            }
        }

        return nullptr;
    };

    OriginalBasic* original = nullptr;
    const s32 targetHash = static_cast<s32>(animation->targetNameUID);
    switch (animation->targetType) {
        case 0:
            original = g_levelManager->FindSTree(targetHash);
            if (!original) {
                original = g_levelManager->FindETree(targetHash);
            }
            break;
        case 1:
            original = g_levelManager->FindSTree(targetHash);
            break;
        case 2:
            original = g_levelManager->FindETree(targetHash);
            break;
        default:
            break;
    }

    return ResolveSkeletonFromOriginal(original);
}

// PSX: _13AnimStructurelP10tAnimationlP5ModelP13DrawableBasic (0x80070740)
// mode: 0=normal, 1=reverse, 2=runToLast, 3=camera
// animation: TransformAnim* (parsed from raw binary)
// loopType: initial loop type
// model: Model* (nullptr for camera anims)
// drawableBasic: DrawableBasic* (nullptr for camera anims)
AnimStructure::AnimStructure(s32 m, void* anim, s32 lt, Model* mdl, void* drawableBasic) {
    MARKFUNCTION(0x80070740);

    mode = m;
    model = mdl;
    flip = nullptr;
    blendPose = nullptr;
    speed = FIX16_ONE;
    loopCount = 0;
    sequence = nullptr;

    rawAnimation = anim;
    if (IsCharSequenceAnim(rawAnimation)) {
        sequence = static_cast<CharSequenceAnim*>(rawAnimation);
        animation = sequence->parts ? sequence->parts[0] : nullptr;
    }
    else {
        animation = static_cast<TransformAnim*>(rawAnimation);
    }

    // PSX: mode 0 creates tTransformFlip2 + tTreeFlip, attaches to tree + animation
    if (mode == 0 && animation && mdl) {
        // Get skeleton from model's drawable
        STreeData* skeleton = nullptr;
        DrawableBasic* drawable = drawableBasic
            ? static_cast<DrawableBasic*>(drawableBasic)
            : mdl->drawable;
        OriginalSTree* active = GetActiveSTree(drawable);
        if (active) {
            skeleton = active->skeleton;
        }
        if (skeleton) {
            flip = new TransformFlip();
            flip->Attach(skeleton, animation);

            // Set endFrame from animation's numFrames
            if (animation->numFrames > 0) {
                endFrame = (animation->numFrames - 1) << 16;
            }

            flip->Reset();
        }
    }
    else if ((mode == 1 || mode == 2) && animation) {
        // PSX mode 1/2 path calls tAnimation::MakePuppet and always yields a flip object.
        // For tTransformAnim, target resolution may fail, but SetFrame still operates on a valid puppet.
        STreeData* skeleton = ResolveTransformAnimPuppetTree(animation);
        flip = new TransformFlip();
        flip->Attach(skeleton, animation);
    }

    ResetCountsToAnim();
    SetLoopType(lt, 1);
}

// PSX: __13AnimStructure (0x80070AB8)
AnimStructure::~AnimStructure() {
    MARKFUNCTION(0x80070AB8);

    delete flip;
    flip = nullptr;
    animation = nullptr;
    delete blendPose;
    blendPose = nullptr;
}

// PSX: ResetCountsToAnim__13AnimStructure (0x80070D30)
void AnimStructure::ResetCountsToAnim() {
    MARKFUNCTION(0x80070D30);

    const s32 totalFrames = sequence ? sequence->numFrames
                          : (animation ? animation->numFrames : 0);
    if (totalFrames <= 0) {
        return;
    }

    // PSX: endFrame = (GetNumFrames(animation) - 1) << 16
    if (totalFrames > 0) {
        endFrame = (totalFrames - 1) << 16;
    }
    startFrame = 0;
    currentFrame = 0;
    prevFrame = 0;
    loopCount = 0;

    s32 tick = g_time ? (s32)g_time->frameCounter : 0;
    currentTick = tick;
    prevTick = tick - 1;
}

// PSX: ForceFrame__13AnimStructurel (0x80070DB8)
void AnimStructure::ForceFrame(s32 frame) {
    MARKFUNCTION(0x80070DB8);
    currentFrame = frame << 16;
    if (flip) {
        flip->SetFrame(frame);
        flip->UpdateJoints();
    }
}

// PSX: SetLoopType__13AnimStructureli (0x80070C20)
void AnimStructure::SetLoopType(s32 type, s32 resetCounts) {
    MARKFUNCTION(0x80070C20);

    switch (type) {
        case 0: // Loop
            handlerOffset = 0;
            handlerIndex = 7;
            handlerTable = 8;
            break;
        case 1: // LoopReverse
            handlerOffset = 0;
            handlerIndex = 8;
            handlerTable = 8;
            break;
        case 2: // RunToLast
            handlerOffset = 0;
            handlerIndex = 11;
            handlerTable = 8;
            break;
        case 3: // HoldFirst
            handlerOffset = 0;
            handlerIndex = 9;
            handlerTable = 8;
            break;
        case 4: // HoldLast
            handlerOffset = 0;
            handlerIndex = 10;
            handlerTable = 8;
            break;
        case 5: // Blend
            handlerOffset = 0;
            handlerIndex = 14;
            handlerTable = 8;
            break;
        case 6: // DecFrame
            handlerOffset = 0;
            handlerIndex = 15;
            handlerTable = 8;
            break;
        case 7: // Blend2
            handlerOffset = 0;
            handlerIndex = 17;
            handlerTable = 8;
            resetCounts = 0;
            break;
        case 8: // Stop
            handlerOffset = 0;
            handlerIndex = 0;
            handlerTable = 0;
            handlerPad = 0;
            break;
        default:
            break;
    }

    loopTypeField = type;
    if (resetCounts) {
        ResetCountsToAnim();
    }
}

// PSX: ExecuteHandler__13AnimStructurei (0x80070E1C)
void AnimStructure::ExecuteHandler(s32 doFlip) {
    MARKFUNCTION(0x80070E1C);

    // PSX: currentTick = MEMORY[0x1C]
    s32 tick = g_time ? (s32)g_time->frameCounter : 0;
    currentTick = tick;

    // Compute frame delta based on speed
    s32 elapsed;
    if (speed == FIX16_ONE) {
        elapsed = (tick - prevTick) << 16;
    }
    else {
        elapsed = (s32)((((s64)(tick - prevTick)) << 16) * (s64)speed >> 16);
    }
    // Advance frame based on loop type
    if (loopTypeField == ANIM_RUN_TO_LAST && loopCount != 0 && currentFrame >= endFrame) {
        currentFrame = endFrame;
    }
    else if (loopTypeField == 1) {
        // Reverse: subtract
        currentFrame = currentFrame - elapsed;
    }
    else if (loopTypeField != 5) {
        // Normal/other: add
        currentFrame = currentFrame + elapsed;
    }
    // mode 5 (blend) = no frame advance

    // Run the boundary handler (loop, hold, etc).
    // PSX: when model != null AND handlerIndex > 0, dispatches through
    // Model vtable: model->vtable[handlerIndex-1](model + handlerOffset, this).
    // When model == null (e.g. camera anims), falls back to built-in switch.
    if (handlerIndex != 0) {
        if (model && handlerIndex > 0) {
            // Dispatch through Model virtual handler methods (PSX vtable path)
            switch (loopTypeField) {
                case 0: model->HandleLoop(this); break;
                case 1: model->HandleLoopReverse(this); break;
                case 2: model->HandleRunToLast(this); break;
                case 3: model->HandleHoldFirst(this); break;
                case 4: model->HandleHoldLast(this); break;
                case 5: model->HandleIncFrame(this); break;
                case 6: model->HandleDecFrame(this); break;
                case 7: model->HandleRunToLastBlend(this); break;
                default: break;
            }
        }
        else {
            // Fallback: no model (camera anims, etc.) - direct AnimStructure handlers
            switch (loopTypeField) {
                case 0: Loop(); break;
                case 1: LoopReverse(); break;
                case 2: RunToLast(); break;
                case 3: HoldFirst(); break;
                case 4: HoldLast(); break;
                case 6: DecFrame(); break;
                default: break;
            }
        }
    }

    // PSX: if doFlip, updates flipbook state.
    if (doFlip && flip) {
        if (currentFrame < 0) {
            currentFrame = 0;
        }

        // For multi-part (sequence) animations, resolve which TransformAnim part
        // owns the current frame and re-attach the flip to it with a local frame.
        if (sequence) {
            s32 localFrame = 0;
            TransformAnim* part = sequence->ResolvePart(currentFrame >> 16, localFrame);
            if (part && part != flip->anim) {
                flip->anim = part;
                flip->dirty = 1;
            }
            if (part) {
                const s32 localFrameReal = localFrame << 16 | (currentFrame & 0xFFFF);
                flip->SetFrameReal(localFrameReal);
                flip->UpdateJoints();
                prevFrame = currentFrame;
                prevTick = currentTick;
                return;
            }
        }

        ApplyFlipPose(currentFrame);
    }

    // Store state
    prevFrame = currentFrame;
    prevTick = currentTick;
}

// PC: pose-apply tail of Update, shared with the render path.
void AnimStructure::ApplyFlipPose(s32 frameReal) {
    if (!flip) {
        return;
    }

    flip->SetFrameReal(frameReal);
    flip->UpdateJoints();

    if (mode == 0 && loopTypeField == ANIM_BLEND2) {
        const s32 blendDuration = endFrame + FIX16_ONE;
        s32 blendWeight = FIX16_ONE + 1;
        if (blendDuration != 0) {
            blendWeight = rmDiv16i(frameReal + FIX16_ONE, blendDuration);
        }

        if (blendWeight <= FIX16_ONE) {
            BlendTreePose(blendWeight, blendPose, flip->tree);
        }
    }
}

// PSX: Loop__13AnimStructure (0x8007119C)
void AnimStructure::Loop() {
    if (currentFrame > endFrame) {
        loopCount++;
        s32 denom = endFrame + FIX16_ONE;
        if (denom != 0) {
            currentFrame = currentFrame % denom;
        }
    }
}

// PSX: LoopReverse__13AnimStructure (0x80071200)
void AnimStructure::LoopReverse() {
    if (currentFrame < 0) {
        currentFrame = endFrame - FIX16_ONE;
        loopCount++;
    }
}

// PSX: HoldFirst__13AnimStructure (0x80071234)
void AnimStructure::HoldFirst() {
    if (startFrame < currentFrame) {
        currentFrame = startFrame;
        loopCount++;
        ProcessHumanoidCB();
    }
}

// PSX: HoldLast__13AnimStructure (0x80071278)
void AnimStructure::HoldLast() {
    if (currentFrame > endFrame) {
        currentFrame = endFrame;
        loopCount++;
        ProcessHumanoidCB();
    }
}

// PSX: RunToLast__13AnimStructure (0x800712BC)
void AnimStructure::RunToLast() {
    if (currentFrame > endFrame) {
        currentFrame = endFrame;
        if (loopCount == 0) {
            loopCount = 1;
            ProcessHumanoidCB();
        }
    }
}

// PSX: IncFrame__13AnimStructure
void AnimStructure::IncFrame() {
    if (endFrame >= currentFrame) {
        currentFrame = prevFrame + FIX16_ONE;
    }
    else {
        s32 denom = endFrame + FIX16_ONE;
        loopCount++;
        if (denom != 0) {
            currentFrame = currentFrame % denom;
        }
        ProcessHumanoidCB();
    }

    if (flip) {
        flip->SetFrameReal(currentFrame);
        flip->UpdateJoints();
    }
}

// PSX: DecFrame__13AnimStructure
void AnimStructure::DecFrame() {
    currentFrame = prevFrame - FIX16_ONE;
    if (currentFrame < 0) {
        currentFrame = endFrame;
        ProcessHumanoidCB();
    }
}

// PSX: RunToLastBlend__13AnimStructure
void AnimStructure::RunToLastBlend() {
    MARKFUNCTION(0x80071410);

    if (currentFrame > endFrame) {
        AnimHumanoidCB savedCB = humanoidCB;
        const s32 startFrame = field56;
        s32 loopType = ANIM_RUN_TO_LAST;
        if (blendPose) {
            loopType = blendPose->loopType;
        }

        SModel* sModel = dynamic_cast<SModel*>(model);
        if (!sModel || !animation) {
            return;
        }

        sModel->ApplyAnimToModelBasic(rawAnimation ? rawAnimation : animation);
        if (flip) {
            flip->Reset();
        }

        SetLoopType(loopType, 1);
        humanoidCB = savedCB;
        currentFrame = startFrame + FIX16_ONE;
    }
}

// PSX: ProcessHumanoidCB__13AnimStructure (0x80071108)
void AnimStructure::ProcessHumanoidCB() {
    s16 selector = humanoidCB.offsetHi;
    if (selector == 0) {
        return;
    }

    AnimHumanoidCB cb = humanoidCB;
    humanoidCB = {};

    if (!model || !model->backPtr) {
        return;
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(model->backPtr);
    uintptr_t adjusted = base + (s16)cb.offsetLo;

    // PSX: selector <= 0 means direct callback function at +100.
    if (cb.offsetHi <= 0) {
        uintptr_t fnValue = reinterpret_cast<uintptr_t>(cb.funcPtr);
        if (fnValue > 0xFFFF) {
            using CallbackFn = s32(*)(uintptr_t);
            CallbackFn fn = reinterpret_cast<CallbackFn>(cb.funcPtr);
            fn(adjusted);
        }
        return;
    }

    // PSX selector dispatch uses vtable slots via selector:
    // 60 -> vtable +236 (DoJump), 61 -> vtable +240 (_DoStand), 62 -> vtable +244 (_DoRun).
    Humanoid* humanoid = dynamic_cast<Humanoid*>(model->backPtr);
    if (!humanoid) {
        return;
    }

    switch (cb.offsetHi) {
        case 60:
            humanoid->DoJump();
            return;
        case 61:
            humanoid->_DoStand();
            return;
        case 62:
            humanoid->_DoRun();
            return;
        default:
            return;
    }
}

// PSX: ReAttachTree__13AnimStructurell (0x80070B6C)
void AnimStructure::ReAttachTree(s32 type, s32 animEnum) {
    MARKFUNCTION(0x80070B6C);

    // Keep flip->tree pointed at the model's current skeleton
    if (flip && model) {
        OriginalSTree* active = GetActiveSTree(model->drawable);
        if (active && active->skeleton && flip->tree != active->skeleton) {
            flip->tree = active->skeleton;
            flip->dirty = 1;
        }
    }

    if (!g_characterManager) {
        return;
    }
    void* raw = g_characterManager->GetAnimation((u32)type, animEnum);
    if (!raw) {
        return;
    }

    if (IsCameraParamAnim(raw) || IsCharSequenceAnim(raw)) {
        return;
    }

    rawAnimation = raw;
    sequence = nullptr;
    animation = static_cast<TransformAnim*>(raw);


    if (flip && model) {
        OriginalSTree* active = GetActiveSTree(model->drawable);
        if (active && active->skeleton) {
            flip->Attach(active->skeleton, animation);
        }
    }

    // PSX calls flip->Update() here
}

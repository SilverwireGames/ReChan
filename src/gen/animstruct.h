#pragma once
#include "core.h"
#include "p3d/p3dmath.h"
#include "p3d/flip.h"
#include "gen/cclist.h"

class Model;

// Loop type constants (PSX SetLoopType parameter)
enum AnimLoopType : s32 {
    ANIM_LOOP = 0,
    ANIM_LOOP_REVERSE = 1,
    ANIM_RUN_TO_LAST = 2,
    ANIM_HOLD_FIRST = 3,
    ANIM_HOLD_LAST = 4,
    ANIM_BLEND = 5,  // mode 5 = no frame advance in ExecuteHandler
    ANIM_DEC_FRAME = 6,
    ANIM_BLEND2 = 7,
    ANIM_STOP = 8,
};

// Humanoid callback info (PSX +96..+103)
struct AnimHumanoidCB {
    s16 offsetLo = 0;    // +96 low
    s16 offsetHi = 0;    // +96 high (HIWORD)
    void* funcPtr = nullptr; // +100
};

struct BlendJointPose {
    s32 translationX = 0;
    s32 translationY = 0;
    s32 translationZ = 0;
    s16 rotationX = 0;
    s16 rotationY = 0;
    s16 rotationZ = 0;
};

struct BlendPoseState {
    BlendJointPose* joints = nullptr;
    u32 jointCount = 0;
    s32 loopType = ANIM_RUN_TO_LAST;

    ~BlendPoseState() { delete[] joints; }
};

// AnimStructure (104 bytes on PSX)
// PSX layout:
//   [0-5]  ccNode (24 bytes)
//   [6]  +24: void* animation (tAnimation*)
//   [7]  +28: void* flip (tParamFlip*/tTreeFlip*/tSequenceFlip*)
//   [8]  +32: Model* model
//   [9]  +36: s32 mode (0=normal, 1=reverse, 2=runToLast, 3=camera)
//   [10] +40: (reserved)
//   [11] +44: s32 loopType
//   [12] +48: s32 speed (16.16, 0x10000 = 1.0)
//   [13] +52: BlendPoseState* blendPose (for mode 7 blend)
//   [14] +56: blend resume frame (16.16)
//   [15] +60: s32 currentFrame (16.16)
//   [16] +64: s32 startFrame
//   [17] +68: s32 endFrame
//   [18] +72: s32 prevFrame
//   [19] +76: s32 prevTick
//   [20] +80: s32 currentTick
//   [21] +84: s32 loopCount
//   [22] +88: s16 handlerOffset, s16 handlerIndex (+90)
//   [23] +92: s16 handlerTable (+92), s16 unused (+94)
//   [24] +96: s16 cbOffsetLo, s16 cbValid (+98)
//   [25] +100: void* cbFuncPtr
class AnimStructure : public ccNode {
public:
    AnimStructure(s32 mode, void* animation, s32 loopType, Model* model = nullptr, void* drawableBasic = nullptr);
    ~AnimStructure() override;
    void ExecuteHandler(s32 doFlip);
    void SetLoopType(s32 type, s32 resetCounts);
    void ApplyFlipPose(s32 frameReal);
    void ResetCountsToAnim();
    void ForceFrame(s32 frame);
    void ReAttachTree(s32 type, s32 animEnum);

    // Accessors
    TransformFlip* GetFlip() const { return flip; }
    TransformAnim* GetAnimation() const { return animation; }
    s32 GetCurrentFrame() const { return currentFrame; }
    s32 GetLoopCount() const { return loopCount; }

    // +24
    TransformAnim* animation = nullptr;
    void* rawAnimation = nullptr;
    // Non-null when the animation resource contains multiple concatenated blocks.
    CharSequenceAnim* sequence = nullptr;
    // +28
    TransformFlip* flip = nullptr;
    // +32
    Model* model = nullptr;
    // +36
    s32 mode = 0;
    // +40: current animation enum being played (set by ApplyAnimToModel)
    s32 animEnum = 0;
    // +44
    s32 loopTypeField = 0;
    // +48
    s32 speed = FIX16_ONE;
    // +52
    BlendPoseState* blendPose = nullptr;
    // +56
    s32 field56 = 0;
    // +60
    s32 currentFrame = 0;
    // +64
    s32 startFrame = 0;
    // +68
    s32 endFrame = 0;
    // +72
    s32 prevFrame = 0;
    // +76
    s32 prevTick = 0;
    // +80
    s32 currentTick = 0;
    // +84
    s32 loopCount = 0;
    // +88
    s16 handlerOffset = 0;
    s16 handlerIndex = 0;
    // +92
    s16 handlerTable = 0;
    s16 handlerPad = 0;
    // +96..+103
    AnimHumanoidCB humanoidCB = {};

    // PSX: these are called from Model virtual handler trampolines.
    // Must be accessible to Model-level handlers.
    void Loop();
    void LoopReverse();
    void HoldFirst();
    void HoldLast();
    void RunToLast();
    void IncFrame();
    void DecFrame();
    void RunToLastBlend();
    void ProcessHumanoidCB();
};

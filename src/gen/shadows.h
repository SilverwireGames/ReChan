#pragma once

#include "core.h"
#include "p3d/p3dmath.h"
#if !defined(RC_PLATFORM_PSP)
#include <functional>
#endif

class Model;

enum ModelShadowType : u32 {
    MODEL_SHADOW_NONE = 0,
    MODEL_SHADOW_SIMPLE = 1,
    MODEL_SHADOW_TREE = 2,
};

struct ModelFloorHeightState {
    // Field names are legacy, but PSX logic treats slot 0 as previous floor
    // and slot 1 as current floor (see Thing::ClearFloorHeight/SetFloorHeight).
    s32 current = (s32)0x80000001;
    s32 previous = (s32)0x80000001;
    s32 shadowMinX = 0;
    s32 shadowMinY = 0;
    s32 shadowMaxX = 0;
    s32 shadowMaxZ = 0;
    u32 shadowType = MODEL_SHADOW_NONE;
};

// Immediate-render adaptation for PSX OT-style shadow composition.
// World draw starts a queue each block pass, model Show paths enqueue shadow fans,
// and the queue flushes after block geometry so subtract shadows are not overdrawn.
void BeginModelShadowQueue();
void FlushModelShadowQueue();
void EndModelShadowQueue();

#if defined(RC_PLATFORM_PSP)
using LateEntityDrawCallback = void (*)(void* context);
void QueueLateEntityDraw(LateEntityDrawCallback callback, void* context);
#else
void QueueLateEntityDraw(std::function<void()> drawFn);
#endif
void FlushLateEntityDrawQueue();

// PSX: ShadowShow (SHADOW.CPP:192, 0x800AED94)
void ShadowShow(const LVector& center, LVector* points, s32 pointCount);

class Shadow {
public:
    explicit Shadow(Model* modelPtr, u32 shadowType);
    virtual ~Shadow();

    virtual s32 Place(LVector& center, LVector* points) = 0;
    virtual void Show(void* view) = 0;

    ModelFloorHeightState* GetFloorHeightState() { return &floorHeightState; }
    const ModelFloorHeightState* GetFloorHeightState() const { return &floorHeightState; }

protected:
    Model* model = nullptr;
    ModelFloorHeightState floorHeightState = {};
};

Shadow* GetModelShadow(Model* model);
const Shadow* GetModelShadow(const Model* model);
ModelFloorHeightState* GetModelFloorHeightState(Model* model);
const ModelFloorHeightState* GetModelFloorHeightState(const Model* model);

class TreeShadow : public Shadow {
public:
    explicit TreeShadow(Model* modelPtr);
    ~TreeShadow() override;

    s32 Place(LVector& center, LVector* points) override;
    void Show(void* view) override;
};

class SimpleShadow : public Shadow {
public:
    explicit SimpleShadow(Model* modelPtr);
    ~SimpleShadow() override;

    s32 Place(LVector& center, LVector* points) override;
    void Show(void* view) override;
};

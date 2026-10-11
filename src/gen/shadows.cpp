#include "gen/shadows.h"
#include "gen/animmat.h"
#include "gen/model.h"
#include "p3d/context.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include <vector>

#include "extra/shadowcsm.h"

static constexpr s32 COLLISION_SECTOR_MIN_HEIGHT = (s32)0x80000001;
// PSX globals at gp+0xB74/gp+0xB78 (0x800DD4C0/0x800DD4C4).
static constexpr s32 TREE_SHADOW_SCALE_BASE = 0x2CB4E;
static constexpr s32 TREE_SHADOW_SCALE_HEIGHT_SLOPE = 0x19;

static constexpr s32 SHADOW_TABLE[] = {
    60, 0, -55,
    70, 0, 0,
    60, 0, 55,
    -60, 0, 55,
    -70, 0, 0,
    -60, 0, -55,
};

struct ShadowRenderVertex {
    f32 x;
    f32 y;
    f32 z;
    f32 r;
    f32 g;
    f32 b;
    f32 u;
    f32 v;
    f32 tpage;
    f32 cba;
};

struct QueuedShadowFan {
    LVector center = {};
    LVector points[6] = {};
    s32 pointCount = 0;
};

static bool s_modelShadowQueueActive = false;
static std::vector<QueuedShadowFan> s_modelShadowQueue;

#if defined(RC_PLATFORM_PSP)
static pddiPrimBuffer* GetPspShadowBuffer(s32 pointCount) {
    static pddiPrimBuffer* buffers[2] = {};
    const s32 bufferIndex = (pointCount == 4) ? 0 : 1;
    if (!buffers[bufferIndex] && p3d::device) {
        const u32 format = PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO;
        buffers[bufferIndex] = p3d::device->NewPrimBuffer(
            pddiPrimBufferDesc(PDDI_PRIM_TRIANGLES,
                               format,
                               static_cast<u32>(pointCount + 1),
                               static_cast<u32>(pointCount * 3)));
    }
    return buffers[bufferIndex];
}
#endif

static bool IsValidShadowFloorHeight(s32 floorHeight) {
    return floorHeight > COLLISION_SECTOR_MIN_HEIGHT;
}

static bool ResolveShadowFloorHeight(const ModelFloorHeightState* shadowState, s32* outFloorHeight) {
    if (!shadowState || !outFloorHeight) {
        return false;
    }

    // Slot usage from Thing::SetFloorHeight/ClearFloorHeight:
    // prefer active frame floor, then preserve last valid floor as fallback.
    s32 floorHeight = shadowState->previous;
    if (!IsValidShadowFloorHeight(floorHeight)) {
        floorHeight = shadowState->current;
    }

    if (!IsValidShadowFloorHeight(floorHeight)) {
        return false;
    }

    *outFloorHeight = floorHeight;
    return true;
}

static void SubmitShadowFan(const LVector& center, LVector* points, s32 pointCount) {
    if (!points || pointCount <= 0 || pointCount > 6) {
        return;
    }

    if (!s_modelShadowQueueActive) {
        ShadowShow(center, points, pointCount);
        return;
    }

    QueuedShadowFan queued = {};
    queued.center = center;
    queued.pointCount = pointCount;
    for (s32 i = 0; i < pointCount; i++) {
        queued.points[i] = points[i];
    }

    s_modelShadowQueue.push_back(queued);
}

#if defined(RC_PLATFORM_PSP)
static void FillShadowFanVertex(ShadowRenderVertex& vertex, const LVector& position, f32 shade) {
    vertex.x = static_cast<f32>(position.x);
    vertex.y = static_cast<f32>(position.y);
    vertex.z = static_cast<f32>(position.z);
    vertex.r = shade;
    vertex.g = shade;
    vertex.b = shade;
    vertex.u = 0.0f;
    vertex.v = 0.0f;
    vertex.tpage = -1.0f;  // no texture page
    vertex.cba = 0.0f;
}

// One buffer per flush of a frame: a buffer the GE may still read this frame
// is never rewritten (the pool restarts at BeginModelShadowQueue).
static std::vector<pddiPrimBuffer*> s_pspShadowBatchBuffers;
static u32 s_pspShadowBatchNext = 0;

// All queued fans as one draw. ShadowShow draws each fan alone with the same
// state (identity world, subtractive blend without depth writes, the same
// polygon offset); a saturating subtraction does not depend on how the fans
// are split into draws, and the triangles keep their order.
static bool DrawShadowFanBatch() {
    if (!p3d::device || !p3d::context) return false;
#if MODERN_GRAPHICS
    if (ShadowCSM::GetQuality() != SHADOW_QUALITY_OFF) return true;
#endif
    static std::vector<ShadowRenderVertex> verts;
    static std::vector<u16> indices;
    verts.clear();
    indices.clear();
    for (const QueuedShadowFan& fan : s_modelShadowQueue) {
        if (fan.pointCount <= 0 || fan.pointCount > 6) continue;
        const u16 base = static_cast<u16>(verts.size());
        ShadowRenderVertex vertex = {};
        FillShadowFanVertex(vertex, fan.center, 40.0f / 128.0f);
        verts.push_back(vertex);
        for (s32 i = 0; i < fan.pointCount; i++) {
            FillShadowFanVertex(vertex, fan.points[i], 1.0f / 128.0f);
            verts.push_back(vertex);
            indices.push_back(base);
            indices.push_back(static_cast<u16>(base + i + 1));
            indices.push_back(static_cast<u16>(base + ((i + 1) % fan.pointCount) + 1));
        }
    }
    if (verts.empty()) return true;
    if (s_pspShadowBatchNext >= s_pspShadowBatchBuffers.size()) {
        pddiPrimBuffer* created = p3d::device->NewPrimBuffer(pddiPrimBufferDesc(PDDI_PRIM_TRIANGLES,
            PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO, 7u, 18u));
        if (!created) return false;
        s_pspShadowBatchBuffers.push_back(created);
    }
    pddiPrimBuffer* buffer = s_pspShadowBatchBuffers[s_pspShadowBatchNext++];
    buffer->SetVertexData(verts.data(), static_cast<u32>(verts.size()));
    buffer->SetIndices(indices.data(), static_cast<u32>(indices.size()));

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    Mat4 identity;
    p3d::context->SetWorldMatrix(identity);
    p3d::context->EnableZBuffer(true);
    p3d::context->SetCullMode(PDDI_CULL_NONE);
    p3d::context->SetBlendMode(PDDI_BLEND_SUBTRACT);
    p3d::context->SetPolygonOffset(true, 16.0f, 32.0f);
    p3d::context->DrawPrimBuffer(buffer);
    p3d::context->SetPolygonOffset(false);
    p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    p3d::context->SetWorldMatrix(savedWorld);
    return true;
}
#endif

static void FlushShadowFanQueue() {
#if defined(RC_PLATFORM_PSP) && !defined(PSP_SHADOW_FAN_DRAWS)
    if (s_modelShadowQueue.size() > 1u && DrawShadowFanBatch()) {
        s_modelShadowQueue.clear();
        return;
    }
#endif
    for (QueuedShadowFan& queued : s_modelShadowQueue) {
        ShadowShow(queued.center, queued.points, queued.pointCount);
    }

    s_modelShadowQueue.clear();
}

Shadow* GetModelShadow(Model* model) {
    if (!model || !model->field36) {
        return nullptr;
    }

    return static_cast<Shadow*>(model->field36);
}

const Shadow* GetModelShadow(const Model* model) {
    if (!model || !model->field36) {
        return nullptr;
    }

    return static_cast<const Shadow*>(model->field36);
}

ModelFloorHeightState* GetModelFloorHeightState(Model* model) {
    Shadow* shadow = GetModelShadow(model);
    if (!shadow) {
        return nullptr;
    }

    return shadow->GetFloorHeightState();
}

const ModelFloorHeightState* GetModelFloorHeightState(const Model* model) {
    const Shadow* shadow = GetModelShadow(model);
    if (!shadow) {
        return nullptr;
    }

    return shadow->GetFloorHeightState();
}

Shadow::Shadow(Model* modelPtr, u32 shadowType) {
    MARKFUNCTION(0x800AED38);
    model = modelPtr;
    floorHeightState.shadowType = shadowType;
}

Shadow::~Shadow() {
    MARKFUNCTION(0x800AED60);
}

// PSX: ShadowShow (SHADOW.CPP:192, 0x800AED94)
void ShadowShow(const LVector& center, LVector* points, s32 pointCount) {
    MARKFUNCTION(0x800AED94);

    if (!points || pointCount <= 0 || pointCount > 6 || !p3d::device || !p3d::context) {
        return;
    }

#if MODERN_GRAPHICS
    // CSM (Low/Medium/High) replaces the blob shadow with a real shadow map
    // cast onto world geometry; only Off still draws the PSX-style fan.
    if (ShadowCSM::GetQuality() != SHADOW_QUALITY_OFF) {
        return;
    }
#endif

    ShadowRenderVertex verts[7] = {};
    u16 indices[18] = {};

    static constexpr f32 kCenterShade = 40.0f / 128.0f;
    static constexpr f32 kEdgeShade = 1.0f / 128.0f;
    static constexpr f32 kShadowNoTexPage = -1.0f;

    verts[0].x = static_cast<f32>(center.x);
    verts[0].y = static_cast<f32>(center.y);
    verts[0].z = static_cast<f32>(center.z);
    verts[0].r = kCenterShade;
    verts[0].g = kCenterShade;
    verts[0].b = kCenterShade;
    verts[0].u = 0.0f;
    verts[0].v = 0.0f;
    verts[0].tpage = kShadowNoTexPage;
    verts[0].cba = 0.0f;

    for (s32 i = 0; i < pointCount; i++) {
        verts[i + 1].x = static_cast<f32>(points[i].x);
        verts[i + 1].y = static_cast<f32>(points[i].y);
        verts[i + 1].z = static_cast<f32>(points[i].z);
        verts[i + 1].r = kEdgeShade;
        verts[i + 1].g = kEdgeShade;
        verts[i + 1].b = kEdgeShade;
        verts[i + 1].u = 0.0f;
        verts[i + 1].v = 0.0f;
        verts[i + 1].tpage = kShadowNoTexPage;
        verts[i + 1].cba = 0.0f;

        indices[(i * 3) + 0] = 0;
        indices[(i * 3) + 1] = (u16)(i + 1);
        indices[(i * 3) + 2] = (u16)(((i + 1) % pointCount) + 1);
    }

    const u32 vertCount = (u32)(pointCount + 1);
    const u32 indexCount = (u32)(pointCount * 3);
    pddiPrimBufferDesc desc(PDDI_PRIM_TRIANGLES,
                            PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO,
                            vertCount,
                            indexCount);
#if defined(RC_PLATFORM_PSP)
    pddiPrimBuffer* buffer = GetPspShadowBuffer(pointCount);
#else
    pddiPrimBuffer* buffer = p3d::device->NewPrimBuffer(desc);
#endif
    if (!buffer) {
        return;
    }

    buffer->SetVertexData(verts, vertCount);
    buffer->SetIndices(indices, indexCount);

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    Mat4 identity;

    p3d::context->SetWorldMatrix(identity);
    p3d::context->EnableZBuffer(true);
    p3d::context->SetCullMode(PDDI_CULL_NONE);
    p3d::context->SetBlendMode(PDDI_BLEND_SUBTRACT);
    p3d::context->SetPolygonOffset(true, 16.0f, 32.0f);
    p3d::context->DrawPrimBuffer(buffer);
    p3d::context->SetPolygonOffset(false);
    p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    p3d::context->SetWorldMatrix(savedWorld);

#if !defined(RC_PLATFORM_PSP)
    buffer->Release();
#endif
}

// PSX: TreeShadow::TreeShadow (SHADOW.CPP:249, 0x800AEFE0)
TreeShadow::TreeShadow(Model* modelPtr)
    : Shadow(modelPtr, MODEL_SHADOW_TREE) {
    MARKFUNCTION(0x800AEFE0);
}

TreeShadow::~TreeShadow() {
    MARKFUNCTION(0x800AF014);
}

// PSX: TreeShadow::Place (SHADOW.CPP:273, 0x800AF03C)
s32 TreeShadow::Place(LVector& center, LVector* points) {
    MARKFUNCTION(0x800AF03C);

    if (!model || !points) {
        return 0;
    }

    ModelFloorHeightState* shadowState = GetFloorHeightState();
    if (!shadowState) {
        return 0;
    }

    s32 floorHeight = 0;
    if (!ResolveShadowFloorHeight(shadowState, &floorHeight)) {
        return 0;
    }

    const s32 drawFloorHeight = floorHeight;

    HumanoidModel* humanoidModel = static_cast<HumanoidModel*>(model);
    const s32 heightAboveFloor = humanoidModel->posY - floorHeight;
    const s32 shadowScale = TREE_SHADOW_SCALE_BASE - (TREE_SHADOW_SCALE_HEIGHT_SLOPE * heightAboveFloor);
    if (shadowScale < 0) {
        return 0;
    }

    center.x = humanoidModel->posX;
    center.y = drawFloorHeight;
    center.z = humanoidModel->posZ;

    if (humanoidModel->animMatrices && humanoidModel->animMatrices->Copy()) {
        const s32* pelvisMatrix = humanoidModel->animMatrices->GetMatrix(5);
        if (pelvisMatrix) {
            center.x = pelvisMatrix[5];
            center.z = pelvisMatrix[7];
        }
    }

    Mat4 rotMatrix;
    p3dBuildRotMatrixXYZ(humanoidModel->rotX, humanoidModel->rotY, humanoidModel->rotZ, rotMatrix);

    for (s32 i = 0; i < 6; i++) {
        const s32 tableX = SHADOW_TABLE[(i * 3) + 0];
        const s32 tableZ = SHADOW_TABLE[(i * 3) + 2];

        const s32 localX = (s32)(((s64)tableX * (s64)shadowScale) >> 16);
        const s32 localZ = (s32)(((s64)tableZ * (s64)shadowScale) >> 16);

        Vec3 local((f32)localX, 0.0f, (f32)localZ);
        Vec3 rotated = p3dVecTimesRotMatrix(local, rotMatrix);

        points[i].x = center.x + (s32)rotated.x;
        points[i].y = drawFloorHeight;
        points[i].z = center.z + (s32)rotated.z;
    }

    return 1;
}

// PSX: TreeShadow::Show (SHADOW.CPP:342, 0x800AF214)
void TreeShadow::Show(void* /*view*/) {
    MARKFUNCTION(0x800AF214);

    LVector center = {};
    LVector points[6] = {};
    if (Place(center, points) != 0) {
        SubmitShadowFan(center, points, 6);
    }
}

// PSX: SimpleShadow::SimpleShadow (SHADOW.CPP:357, 0x800AF258)
SimpleShadow::SimpleShadow(Model* modelPtr)
    : Shadow(modelPtr, MODEL_SHADOW_SIMPLE) {
    MARKFUNCTION(0x800AF258);
}

SimpleShadow::~SimpleShadow() {
    MARKFUNCTION(0x800AF2AC);
}

// PSX: SimpleShadow::Place (SHADOW.CPP:385, 0x800AF2D4)
s32 SimpleShadow::Place(LVector& center, LVector* points) {
    MARKFUNCTION(0x800AF2D4);

    if (!model || !points) {
        return 0;
    }

    ModelFloorHeightState* shadowState = GetFloorHeightState();
    if (!shadowState) {
        return 0;
    }

    s32 floorHeight = 0;
    if (!ResolveShadowFloorHeight(shadowState, &floorHeight)) {
        return 0;
    }

    const s32 drawFloorHeight = floorHeight;

    center.x = model->posX;
    center.y = drawFloorHeight;
    center.z = model->posZ;

    Mat4 rotMatrix;
    p3dBuildRotMatrixXYZ(model->rotX, model->rotY, model->rotZ, rotMatrix);

    auto writePoint = [&](s32 localX, s32 localZ, LVector* outPoint) {
        Vec3 local((f32)localX, 0.0f, (f32)localZ);
        Vec3 rotated = p3dVecTimesRotMatrix(local, rotMatrix);

        outPoint->x = center.x + (s32)rotated.x;
        outPoint->y = drawFloorHeight;
        outPoint->z = center.z + (s32)rotated.z;
    };

    writePoint(shadowState->shadowMaxX, shadowState->shadowMinY, &points[0]);
    writePoint(shadowState->shadowMaxX, shadowState->shadowMaxZ, &points[1]);
    writePoint(shadowState->shadowMinX, shadowState->shadowMaxZ, &points[2]);
    writePoint(shadowState->shadowMinX, shadowState->shadowMinY, &points[3]);

    return 1;
}

// PSX: SimpleShadow::Show (SHADOW.CPP:457, 0x800AF4A4)
void SimpleShadow::Show(void* /*view*/) {
    MARKFUNCTION(0x800AF4A4);

    LVector center = {};
    LVector points[4] = {};
    if (Place(center, points) == 0) {
        return;
    }

    // PSX SimpleShadow::Show averages the quad corners to build fan center.
    LVector fanCenter = {};
    fanCenter.x = (points[0].x + points[1].x + points[2].x + points[3].x) / 4;
    fanCenter.y = (points[0].y + points[1].y + points[2].y + points[3].y) / 4;
    fanCenter.z = (points[0].z + points[1].z + points[2].z + points[3].z) / 4;
    SubmitShadowFan(fanCenter, points, 4);
}

void BeginModelShadowQueue() {
    s_modelShadowQueueActive = true;
#if defined(RC_PLATFORM_PSP)
    s_pspShadowBatchNext = 0;
#endif
#if defined(RC_PLATFORM_PSP)
    if (s_modelShadowQueue.capacity() < 128u) {
        s_modelShadowQueue.reserve(128u);
    }
#endif
    s_modelShadowQueue.clear();
}

void FlushModelShadowQueue() {
    if (!s_modelShadowQueueActive) {
        return;
    }

    FlushShadowFanQueue();
}

void EndModelShadowQueue() {
    if (s_modelShadowQueueActive) {
        FlushShadowFanQueue();
    }

    s_modelShadowQueueActive = false;
    s_modelShadowQueue.clear();
}

#if defined(RC_PLATFORM_PSP)
struct LateEntityDraw {
    LateEntityDrawCallback callback = nullptr;
    void* context = nullptr;
};

static std::vector<LateEntityDraw> s_lateEntityDrawQueue;
static std::vector<LateEntityDraw> s_lateEntityDrawPending;

void QueueLateEntityDraw(LateEntityDrawCallback callback, void* context) {
    if (!callback) {
        return;
    }
    if (s_lateEntityDrawQueue.capacity() < 64u) {
        s_lateEntityDrawQueue.reserve(64u);
    }
    s_lateEntityDrawQueue.push_back({ callback, context });
}
#else
static std::vector<std::function<void()>> s_lateEntityDrawQueue;
static std::vector<std::function<void()>> s_lateEntityDrawPending;

void QueueLateEntityDraw(std::function<void()> drawFn) {
    s_lateEntityDrawQueue.push_back(std::move(drawFn));
}
#endif

void FlushLateEntityDrawQueue() {
    if (s_lateEntityDrawQueue.empty()) {
        return;
    }

#if defined(RC_PLATFORM_PSP)
    if (s_lateEntityDrawPending.capacity() < s_lateEntityDrawQueue.size()) {
        s_lateEntityDrawPending.reserve(s_lateEntityDrawQueue.size());
    }
    s_lateEntityDrawPending.clear();
    s_lateEntityDrawPending.swap(s_lateEntityDrawQueue);
    std::vector<LateEntityDraw>& pending = s_lateEntityDrawPending;
#else
    std::vector<std::function<void()>> pending = std::move(s_lateEntityDrawQueue);
    s_lateEntityDrawQueue.clear();
#endif
    for (auto& drawFn : pending) {
#if defined(RC_PLATFORM_PSP)
        drawFn.callback(drawFn.context);
#else
        if (drawFn) {
            drawFn();
        }
#endif
    }
#if defined(RC_PLATFORM_PSP)
    pending.clear();
#endif
}

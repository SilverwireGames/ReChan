#include "common.h"
#include "gen/trail.h"

#include "gen/blockmgr.h"
#include "gen/colsect.h"
#include "gen/model.h"

#include "p3d/context.h"
#include "p3d/p3dmath.h"

static ccMinList g_trailsPoolList;
static ccMinNode* g_trailsPoolInsertAfter = nullptr;

#if defined(RC_PLATFORM_PSP)
static pddiPrimBuffer* GetPspTrailQuadBuffer() {
    static pddiPrimBuffer* buffer = nullptr;
    if (!buffer && p3d::device) {
        buffer = p3d::device->NewPrimBuffer(
            pddiPrimBufferDesc(PDDI_PRIM_TRIANGLES,
                               PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO,
                               4u,
                               6u));
    }
    return buffer;
}
#endif

static constexpr s32 SPL_TRAIL_RATIO = 0x4E44;
static constexpr s32 TRAIL_ZSORT_SCRATCH_BYTES = 136;

static f32 DecodeTrailColorChannel(u32 color, s32 shift) {
    return static_cast<f32>((color >> shift) & 0xFFu) * (1.0f / 255.0f);
}

struct TrailRenderVertex {
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

struct TrailTexturedRenderVertex {
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

struct TrailSegmentOrder {
    TrailInfo* first = nullptr;
    f32 depth = 0.0f;
};

struct TrailTextureInfo {
    bool valid = false;
    f32 u0 = 0.0f;
    f32 v0 = 0.0f;
    f32 u1 = 0.0f;
    f32 v1 = 0.0f;
    f32 u2 = 0.0f;
    f32 v2 = 0.0f;
    f32 u3 = 0.0f;
    f32 v3 = 0.0f;
    f32 tpage = -1.0f;
    f32 cba = 0.0f;
};

static bool BuildTrailTextureInfo(const OriginalGeo* geo, TrailTextureInfo& out) {
    if (!geo || !geo->dynamicPrimStart || !geo->dynamicPrimVertCount || !geo->dynamicVerts || geo->dynamicPrimCount == 0) {
        return false;
    }

    u32 primIndex = 0;
    u32 start = 0;
    u32 count = 0;
    bool found = false;

    for (u32 i = 0; i < geo->dynamicPrimCount; i++) {
        const u32 localStart = geo->dynamicPrimStart[i];
        const u32 localCount = static_cast<u32>(geo->dynamicPrimVertCount[i]);
        if (localCount >= 4 && localStart + localCount <= geo->dynamicVertCount) {
            primIndex = i;
            start = localStart;
            count = localCount;
            found = true;
            break;
        }
    }

    if (!found || count < 4) {
        return false;
    }

    const GeoRenderVertex& baseVertex = geo->dynamicVerts[start];
    out.tpage = baseVertex.tpage;
    out.cba = baseVertex.cba;

    if (geo->dynamicPrimUVWords) {
        const u32 uvBase = primIndex * 4u;
        const u16 packed0 = geo->dynamicPrimUVWords[uvBase + 0u];
        const u16 packed1 = geo->dynamicPrimUVWords[uvBase + 1u];
        const u16 packed2 = geo->dynamicPrimUVWords[uvBase + 2u];
        const u16 packed3 = geo->dynamicPrimUVWords[uvBase + 3u];

        out.u0 = static_cast<f32>(packed0 & 0xFFu);
        out.v0 = static_cast<f32>((packed0 >> 8) & 0xFFu);
        out.u1 = static_cast<f32>(packed1 & 0xFFu);
        out.v1 = static_cast<f32>((packed1 >> 8) & 0xFFu);
        out.u2 = static_cast<f32>(packed2 & 0xFFu);
        out.v2 = static_cast<f32>((packed2 >> 8) & 0xFFu);
        out.u3 = static_cast<f32>(packed3 & 0xFFu);
        out.v3 = static_cast<f32>((packed3 >> 8) & 0xFFu);
    }
    else {
        const GeoRenderVertex& v0 = geo->dynamicVerts[start + 0u];
        const GeoRenderVertex& v1 = geo->dynamicVerts[start + 1u];
        const GeoRenderVertex& v2 = geo->dynamicVerts[start + 2u];
        const GeoRenderVertex& v3 = geo->dynamicVerts[start + 3u];

        out.u0 = v0.u;
        out.v0 = v0.v;
        out.u1 = v1.u;
        out.v1 = v1.v;
        out.u2 = v2.u;
        out.v2 = v2.v;
        out.u3 = v3.u;
        out.v3 = v3.v;
    }

    out.valid = true;
    return true;
}

static const TrailTextureInfo* SelectTrailTextureInfo(const TrailTextureInfo* first,
                                                      const TrailTextureInfo* second,
                                                      const TrailTextureInfo* mid,
                                                      const TrailTextureInfo* last,
                                                      s32 segmentIndex,
                                                      s32 segmentCount) {
    if (segmentIndex <= 0) {
        return first;
    }
    if (segmentIndex == segmentCount - 1) {
        return last;
    }
    if (segmentIndex == 1) {
        return second;
    }
    return mid;
}

TrailInfo::TrailInfo() {
    MARKFUNCTION(0x8007A624);
}

// PSX: SetupDecrements__9TrailInfoi (TRAIL.CPP:86)
void TrailInfo::SetupDecrements(s32 steps) {
    MARKFUNCTION(0x800791B8);

    ASSERT(steps != 0);

    const s32 midX = (x1 + x2) >> 1;
    const s32 midY = (y1 + y2) >> 1;
    const s32 midZ = (z1 + z2) >> 1;

    dx = (midX - x1) / steps;
    dy = (midY - y1) / steps;
    dz = (midZ - z1) / steps;

    r = static_cast<s16>((static_cast<u8>(color) << 8));
    g = static_cast<s16>((static_cast<u8>(color >> 8) << 8));
    b = static_cast<s16>((static_cast<u8>(color >> 16) << 8));

    dr = static_cast<s16>(r / steps);
    dg = static_cast<s16>(g / steps);
    db = static_cast<s16>(b / steps);

    life = static_cast<u16>(steps);
}

// PSX: SetVelocity__9TrailInfoP10tagLVector (TRAIL.CPP:127)
void TrailInfo::SetVelocity(const LVector* vel) {
    MARKFUNCTION(0x800793C4);

    if (vel) {
        velocity = *vel;
    }
    else {
        velocity = {};
    }
}

// PSX: Update__9TrailInfo (TRAIL.CPP:153)
bool TrailInfo::Update() {
    MARKFUNCTION(0x80079400);

    x1 += dx;
    y1 += dy;
    z1 += dz;

    x2 -= dx;
    y2 -= dy;
    z2 -= dz;

    TrailInfo* prevTrail = static_cast<TrailInfo*>(prev);
    if (prevTrail) {
        x1 += static_cast<s32>((static_cast<s64>(SPL_TRAIL_RATIO)
            * static_cast<s64>(prevTrail->x1 - x1)) >> 16);
        y1 += static_cast<s32>((static_cast<s64>(SPL_TRAIL_RATIO)
            * static_cast<s64>(prevTrail->y1 - y1)) >> 16);
        z1 += static_cast<s32>((static_cast<s64>(SPL_TRAIL_RATIO)
            * static_cast<s64>(prevTrail->z1 - z1)) >> 16);

        x2 += static_cast<s32>((static_cast<s64>(SPL_TRAIL_RATIO)
            * static_cast<s64>(prevTrail->x2 - x2)) >> 16);
        y2 += static_cast<s32>((static_cast<s64>(SPL_TRAIL_RATIO)
            * static_cast<s64>(prevTrail->y2 - y2)) >> 16);
        z2 += static_cast<s32>((static_cast<s64>(SPL_TRAIL_RATIO)
            * static_cast<s64>(prevTrail->z2 - z2)) >> 16);
    }

    r = static_cast<s16>(r - dr);
    g = static_cast<s16>(g - dg);
    b = static_cast<s16>(b - db);

    const u16 oldLife = life;
    life = static_cast<u16>(oldLife - 1);

    color = static_cast<u32>(static_cast<u8>(static_cast<u16>(r) >> 8))
        | static_cast<u32>(static_cast<u16>(g) & 0xFF00)
        | (static_cast<u32>(static_cast<u8>(static_cast<u16>(b) >> 8)) << 16);

    return oldLife > 0;
}

// PSX: _6Trailsi (TRAIL.CPP:220)
Trails::Trails(s32 maxTrails)
    : poolCount(maxTrails) {
    MARKFUNCTION(0x80079614);

    trailInfoPool = (poolCount > 0) ? new TrailInfo[poolCount] : nullptr;
    for (s32 i = 0; i < poolCount; i++) {
        trailInfoPool[i].trailId = static_cast<u16>(i);
        freeList.AddNode(freeList.tail, &trailInfoPool[i]);
    }

    mode = 0;
    activeInEffects = 0;
    currentPos = nullptr;

    if (poolCount > 0) {
        zSortScratch = new u8[TRAIL_ZSORT_SCRATCH_BYTES * poolCount];
        zSortScratchEnd = zSortScratch + (TRAIL_ZSORT_SCRATCH_BYTES * poolCount);
    }

    g_trailsPoolList.AddNode(g_trailsPoolInsertAfter, this);
}

// PSX: __6Trails (TRAIL.CPP:265)
Trails::~Trails() {
    MARKFUNCTION(0x80079778);

    if (activeInEffects) {
        Effects_RemoveEffect(this);
    }
    else {
        g_trailsPoolList.RemNode(this);
    }

    delete[] trailInfoPool;
    trailInfoPool = nullptr;

    delete[] zSortScratch;
    zSortScratch = nullptr;
    zSortScratchEnd = nullptr;

    mode = 0;
    activeInEffects = 0;
    currentPos = nullptr;

    // Keep list destructors from deleting pooled array elements.
    activeList.head = nullptr;
    activeList.tail = nullptr;
    freeList.head = nullptr;
    freeList.tail = nullptr;
}

// PSX: Add__6TrailsP10tagLVectorT1UliT1 (TRAIL.CPP:297)
TrailInfo* Trails::Add(const LVector* start, const LVector* end, u32 inColor, s32 steps, const LVector* vel) {
    MARKFUNCTION(0x80079894);

    if (!start || !end) {
        return nullptr;
    }

    const s32 block = CollisionSector::GetBlockNumber(*start);

    if (block != -1) {
        blockNum = block;
    }

    LVector startPos = *start;
    LVector endPos = *end;

    if (vel) {
        startPos -= *vel;
        endPos -= *vel;
    }

    if (!activeInEffects) {
        activeInEffects = 1;
        g_trailsPoolList.RemNode(this);
        Effects_AddEffect(this, 0);
    }

    TrailInfo* trail = nullptr;

    if (freeList.head) {
        trail = static_cast<TrailInfo*>(freeList.RemHead());
    }
    else {
        trail = FindDoneTrail(1);
        if (!trail) {
            return nullptr;
        }
        activeList.RemNode(trail);
    }

    trail->x1 = startPos.x;
    trail->y1 = startPos.y;
    trail->z1 = startPos.z;

    trail->x2 = endPos.x;
    trail->y2 = endPos.y;
    trail->z2 = endPos.z;

    trail->color = inColor;

    trail->x1 <<= 8;
    trail->y1 <<= 8;
    trail->z1 <<= 8;

    trail->x2 <<= 8;
    trail->y2 <<= 8;
    trail->z2 <<= 8;

    trail->SetupDecrements(steps);
    trail->SetVelocity(vel);

    mode = 2;

    activeList.AddNode(nullptr, trail);
    return trail;
}

// PSX: PutBackEffect__6Trails (TRAIL.CPP:479)
s32 Trails::PutBackEffect() {
    MARKFUNCTION(0x80079AB4);

    g_trailsPoolList.AddNode(g_trailsPoolInsertAfter, this);
    activeInEffects = 0;
    Flush();
    return 0;
}

// PSX: Flush__6Trails (TRAIL.CPP:499)
void Trails::Flush() {
    MARKFUNCTION(0x80079AF4);

    mode = 0;

    while (activeList.head) {
        ccMinNode* node = activeList.RemHead();
        freeList.AddNode(freeList.tail, node);
    }
}

// PSX: FindDoneTrail__6Trailsi (TRAIL.CPP:517)
TrailInfo* Trails::FindDoneTrail(s32 threshold) {
    MARKFUNCTION(0x80079B54);

    TrailInfo* trail = static_cast<TrailInfo*>(activeList.tail);
    while (trail && threshold < static_cast<s16>(trail->life)) {
        trail = static_cast<TrailInfo*>(trail->prev);
    }

    return trail;
}

// PSX: Update__6Trails (TRAIL.CPP:548)
s32 Trails::Update() {
    MARKFUNCTION(0x80079B88);

    if (mode != 0) {
        TrailInfo* trail = static_cast<TrailInfo*>(activeList.head);

        if (trail) {
            while (trail) {
                TrailInfo* next = static_cast<TrailInfo*>(trail->next);

                if (!trail->Update()) {
                    activeList.RemNode(trail);
                    freeList.AddNode(freeList.tail, trail);
                }

                trail = next;
            }
        }
        else {
            Effects_RemoveEffect(this);
            g_trailsPoolList.AddNode(g_trailsPoolInsertAfter, this);
            activeInEffects = 0;
            currentPos = nullptr;
        }
    }

    return mode;
}

// PSX: SetCurrentPos__6TrailsP10tagLVector (TRAIL.CPP:586)
void Trails::SetCurrentPos(LVector* pos) {
    MARKFUNCTION(0x80079C44);
    currentPos = pos;
}

// PSX: Display__6Trailsi (TRAIL.CPP:599)
void Trails::Display(s32 inBlockNum) {
    MARKFUNCTION(0x80079C4C);

    if (mode == 0 || !g_blockManager) {
        return;
    }

    if (inBlockNum < 0) {
        return;
    }

    Block* block = nullptr;
    const u32 totalBlocks = g_blockManager->GetNumBlocks();
    for (u32 i = 0; i < totalBlocks; i++) {
        Block* candidate = g_blockManager->GetBlock(i);
        if (candidate && candidate->blockNum == static_cast<u16>(inBlockNum)) {
            block = candidate;
            break;
        }
    }

    if (!block) {
        return;
    }

    const s32 numTrails = activeList.GetNumElements();
    if (mode == 2 && numTrails >= 2) {
        ChanZSortDisplayNonTexture(numTrails);
    }
    else if (mode == 3 && numTrails >= 3) {
        ChanZSortDisplayTexture(numTrails);
    }
}

// PSX: ChanZSortDisplayNonTexture__6Trailsi (TRAIL.CPP:629)
void Trails::ChanZSortDisplayNonTexture(s32 count) {
    MARKFUNCTION(0x80079D1C);

    if (!p3d::context || !p3d::device || count < 2) {
        return;
    }

    TrailInfo* headTrail = static_cast<TrailInfo*>(activeList.head);
    if (!headTrail || !headTrail->next) {
        return;
    }

    if (!zSortScratch || !zSortScratchEnd) {
        return;
    }

    const s32 maxSortedSegments = static_cast<s32>(
        (zSortScratchEnd - zSortScratch) / static_cast<s32>(sizeof(TrailSegmentOrder)));
    if (maxSortedSegments <= 0) {
        return;
    }

    const bool useCurrentPos = (currentPos != nullptr);

    LVector worldOrigin = {};
    s32 baseX = 0;
    s32 baseY = 0;
    s32 baseZ = 0;
    if (useCurrentPos) {
        // PSX path with currentPos set uses zero local origin and relies on world translate.
        worldOrigin = *currentPos;
    }
    else {
        // PSX path without currentPos uses first trail point as both local origin and world translate.
        baseX = headTrail->x1;
        baseY = headTrail->y1;
        baseZ = headTrail->z1;
        worldOrigin.x = baseX >> 8;
        worldOrigin.y = baseY >> 8;
        worldOrigin.z = baseZ >> 8;
    }

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    Mat4 trailWorld = savedWorld;
    p3dFillTransMatrix(worldOrigin, trailWorld);

    p3d::context->SetWorldMatrix(trailWorld);
    p3d::context->SetTexInfoOverride(false, 0);
    p3d::context->SetCullMode(PDDI_CULL_NONE);
    // PSX trail non-textured path uses draw mode tpage 0x60 -> semitrans mode 3 (quarter add).
    p3d::context->SetBlendMode(PDDI_BLEND_PSX_QUARTER);

    const Mat4& viewMatrix = p3d::context->GetViewMatrix();

    static const u16 kQuadIndices[6] = { 0, 1, 2, 2, 1, 3 };
    const u32 format = PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO;

    auto toTrailCoord = [](s32 coord, s32 base) -> f32 {
        return static_cast<f32>(static_cast<s16>((coord - base) >> 8));
    };

    auto calcDepth = [&](f32 x, f32 y, f32 z) -> f32 {
        f32 worldX = 0.0f;
        f32 worldY = 0.0f;
        f32 worldZ = 0.0f;
        Mat4TransformPoint(trailWorld, x, y, z, worldX, worldY, worldZ);

        f32 viewX = 0.0f;
        f32 viewY = 0.0f;
        f32 viewZ = 0.0f;
        Mat4TransformPoint(viewMatrix, worldX, worldY, worldZ, viewX, viewY, viewZ);
        return -viewZ;
    };

    TrailSegmentOrder* sorted = reinterpret_cast<TrailSegmentOrder*>(zSortScratch);
    s32 sortedCount = 0;

    for (TrailInfo* first = headTrail; first && first->next && sortedCount < maxSortedSegments;
         first = static_cast<TrailInfo*>(first->next)) {
        TrailInfo* second = static_cast<TrailInfo*>(first->next);

        const f32 firstX1 = toTrailCoord(first->x1, baseX);
        const f32 firstY1 = toTrailCoord(first->y1, baseY);
        const f32 firstZ1 = toTrailCoord(first->z1, baseZ);
        const f32 firstX2 = toTrailCoord(first->x2, baseX);
        const f32 firstY2 = toTrailCoord(first->y2, baseY);
        const f32 firstZ2 = toTrailCoord(first->z2, baseZ);

        const f32 secondX1 = toTrailCoord(second->x1, baseX);
        const f32 secondY1 = toTrailCoord(second->y1, baseY);
        const f32 secondZ1 = toTrailCoord(second->z1, baseZ);
        const f32 secondX2 = toTrailCoord(second->x2, baseX);
        const f32 secondY2 = toTrailCoord(second->y2, baseY);
        const f32 secondZ2 = toTrailCoord(second->z2, baseZ);

        const f32 depth = (calcDepth(firstX1, firstY1, firstZ1)
            + calcDepth(firstX2, firstY2, firstZ2)
            + calcDepth(secondX1, secondY1, secondZ1)
            + calcDepth(secondX2, secondY2, secondZ2))
            * 0.25f;

        sorted[sortedCount].first = first;
        sorted[sortedCount].depth = depth;
        sortedCount++;
    }

    for (s32 i = 1; i < sortedCount; i++) {
        const TrailSegmentOrder key = sorted[i];
        s32 j = i;
        while (j > 0 && sorted[j - 1].depth < key.depth) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }

    for (s32 i = 0; i < sortedCount; i++) {
        TrailInfo* first = sorted[i].first;
        if (!first || !first->next) {
            continue;
        }
        TrailInfo* second = static_cast<TrailInfo*>(first->next);

        const f32 firstX1 = toTrailCoord(first->x1, baseX);
        const f32 firstY1 = toTrailCoord(first->y1, baseY);
        const f32 firstZ1 = toTrailCoord(first->z1, baseZ);
        const f32 firstX2 = toTrailCoord(first->x2, baseX);
        const f32 firstY2 = toTrailCoord(first->y2, baseY);
        const f32 firstZ2 = toTrailCoord(first->z2, baseZ);

        const f32 secondX1 = toTrailCoord(second->x1, baseX);
        const f32 secondY1 = toTrailCoord(second->y1, baseY);
        const f32 secondZ1 = toTrailCoord(second->z1, baseZ);
        const f32 secondX2 = toTrailCoord(second->x2, baseX);
        const f32 secondY2 = toTrailCoord(second->y2, baseY);
        const f32 secondZ2 = toTrailCoord(second->z2, baseZ);

        const f32 firstR = DecodeTrailColorChannel(first->color, 0);
        const f32 firstG = DecodeTrailColorChannel(first->color, 8);
        const f32 firstB = DecodeTrailColorChannel(first->color, 16);

        const f32 secondR = DecodeTrailColorChannel(second->color, 0);
        const f32 secondG = DecodeTrailColorChannel(second->color, 8);
        const f32 secondB = DecodeTrailColorChannel(second->color, 16);

        TrailRenderVertex quadVerts[4] = {
            { firstX1, firstY1, firstZ1, firstR, firstG, firstB, 0.0f, 0.0f, -1.0f, 0.0f },
            { firstX2, firstY2, firstZ2, firstR, firstG, firstB, 0.0f, 0.0f, -1.0f, 0.0f },
            { secondX1, secondY1, secondZ1, secondR, secondG, secondB, 0.0f, 0.0f, -1.0f, 0.0f },
            { secondX2, secondY2, secondZ2, secondR, secondG, secondB, 0.0f, 0.0f, -1.0f, 0.0f },
        };

        pddiPrimBufferDesc desc(PDDI_PRIM_TRIANGLES, format, 4u, 6u);
#if defined(RC_PLATFORM_PSP)
        pddiPrimBuffer* buffer = GetPspTrailQuadBuffer();
#else
        pddiPrimBuffer* buffer = p3d::device->NewPrimBuffer(desc);
#endif
        if (!buffer) {
            continue;
        }

        buffer->SetVertexData(quadVerts, 4u);
        buffer->SetIndices(kQuadIndices, 6u);
        p3d::context->DrawPrimBuffer(buffer);
#if !defined(RC_PLATFORM_PSP)
        buffer->Release();
#endif
    }

    p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    p3d::context->SetWorldMatrix(savedWorld);
}

// PSX: ChanZSortDisplayTexture__6Trailsi (TRAIL.CPP:826)
void Trails::ChanZSortDisplayTexture(s32 count) {
    MARKFUNCTION(0x8007A134);

    if (!p3d::context || !p3d::device || count < 3) {
        return;
    }

    TrailInfo* headTrail = static_cast<TrailInfo*>(activeList.head);
    if (!headTrail || !headTrail->next) {
        return;
    }

    TrailTextureInfo texFirst = {};
    TrailTextureInfo texSecond = {};
    TrailTextureInfo texMid = {};
    TrailTextureInfo texLast = {};

    const bool firstValid = BuildTrailTextureInfo(textureGeoFirst, texFirst);
    const bool secondValid = BuildTrailTextureInfo(textureGeoSecond, texSecond);
    const bool midValid = BuildTrailTextureInfo(textureGeoMid, texMid);
    const bool lastValid = BuildTrailTextureInfo(textureGeoLast, texLast);

    if (!firstValid || !secondValid || !midValid || !lastValid) {
        return;
    }

    const s32 baseX = headTrail->x1;
    const s32 baseY = headTrail->y1;
    const s32 baseZ = headTrail->z1;

    LVector worldOrigin = {};
    worldOrigin.x = baseX >> 8;
    worldOrigin.y = baseY >> 8;
    worldOrigin.z = baseZ >> 8;

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    Mat4 trailWorld = savedWorld;
    p3dFillTransMatrix(worldOrigin, trailWorld);

    p3d::context->SetWorldMatrix(trailWorld);
    p3d::context->SetTexInfoOverride(false, 0);
    p3d::context->SetCullMode(PDDI_CULL_NONE);

    static const u16 kQuadIndices[6] = { 0, 1, 2, 2, 1, 3 };
    const u32 format = PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO;

    auto toTrailCoord = [](s32 coord, s32 base) -> f32 {
        return static_cast<f32>(static_cast<s16>((coord - base) >> 8));
    };

    s32 segmentIndex = 0;
    for (TrailInfo* first = headTrail; first && first->next; first = static_cast<TrailInfo*>(first->next), segmentIndex++) {
        TrailInfo* second = static_cast<TrailInfo*>(first->next);
        const TrailTextureInfo* tex = SelectTrailTextureInfo(&texFirst, &texSecond, &texMid, &texLast, segmentIndex, count - 1);
        if (!tex || !tex->valid) {
            continue;
        }

        const f32 firstX1 = toTrailCoord(first->x1, baseX);
        const f32 firstY1 = toTrailCoord(first->y1, baseY);
        const f32 firstZ1 = toTrailCoord(first->z1, baseZ);
        const f32 firstX2 = toTrailCoord(first->x2, baseX);
        const f32 firstY2 = toTrailCoord(first->y2, baseY);
        const f32 firstZ2 = toTrailCoord(first->z2, baseZ);

        const f32 secondX1 = toTrailCoord(second->x1, baseX);
        const f32 secondY1 = toTrailCoord(second->y1, baseY);
        const f32 secondZ1 = toTrailCoord(second->z1, baseZ);
        const f32 secondX2 = toTrailCoord(second->x2, baseX);
        const f32 secondY2 = toTrailCoord(second->y2, baseY);
        const f32 secondZ2 = toTrailCoord(second->z2, baseZ);

        const f32 c0r = DecodeTrailColorChannel(first->color, 0);
        const f32 c0g = DecodeTrailColorChannel(first->color, 8);
        const f32 c0b = DecodeTrailColorChannel(first->color, 16);
        const f32 c1r = DecodeTrailColorChannel(second->color, 0);
        const f32 c1g = DecodeTrailColorChannel(second->color, 8);
        const f32 c1b = DecodeTrailColorChannel(second->color, 16);

        TrailTexturedRenderVertex quadVerts[4] = {
            { firstX1, firstY1, firstZ1, c0r, c0g, c0b, tex->u0, tex->v0, tex->tpage, tex->cba },
            { firstX2, firstY2, firstZ2, c0r, c0g, c0b, tex->u1, tex->v1, tex->tpage, tex->cba },
            { secondX1, secondY1, secondZ1, c1r, c1g, c1b, tex->u2, tex->v2, tex->tpage, tex->cba },
            { secondX2, secondY2, secondZ2, c1r, c1g, c1b, tex->u3, tex->v3, tex->tpage, tex->cba },
        };

        pddiBlendMode blendMode = PDDI_BLEND_ALPHA;
        if (tex->tpage >= 0.0f && tex->tpage <= 65535.0f) {
            const u8 semiTransMode = static_cast<u8>((static_cast<u16>(tex->tpage) >> 5) & 3u);
            switch (semiTransMode) {
                case 1: blendMode = PDDI_BLEND_ADD; break;
                case 2: blendMode = PDDI_BLEND_SUBTRACT; break;
                case 3: blendMode = PDDI_BLEND_PSX_QUARTER; break;
                default: blendMode = PDDI_BLEND_ALPHA; break;
            }
        }
        p3d::context->SetBlendMode(blendMode);

        pddiPrimBufferDesc desc(PDDI_PRIM_TRIANGLES, format, 4u, 6u);
#if defined(RC_PLATFORM_PSP)
        pddiPrimBuffer* buffer = GetPspTrailQuadBuffer();
#else
        pddiPrimBuffer* buffer = p3d::device->NewPrimBuffer(desc);
#endif
        if (!buffer) {
            continue;
        }

        buffer->SetVertexData(quadVerts, 4u);
        buffer->SetIndices(kQuadIndices, 6u);
        p3d::context->DrawPrimBuffer(buffer);
#if !defined(RC_PLATFORM_PSP)
        buffer->Release();
#endif
    }

    p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    p3d::context->SetWorldMatrix(savedWorld);
}

// PSX: Create__6Trails (TRAIL.HPP:103)
s32 Trails::Create() {
    MARKFUNCTION(0x8007A61C);
    return 1;
}

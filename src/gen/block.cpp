#include "gen/block.h"
#include "gen/database.h"
#include "gen/geometry.h"
#include "gen/colsect.h"
#include "gen/game.h"
#include "gen/uvdata.h"
#include "gen/world.h"
#include "p3d/byteread.h"
#include "p3d/context.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "pc/log.h"
#include "psp/psp_perf.h"

u32 splat_fog_near = 0;
u32 splat_fog_far = 0;
u32 splat_fog_color = 0;
u32 splat_dynamic_light = 0;

static u32 s_lastLoggedSplatFogNear = 0xFFFFFFFFu;
static u32 s_lastLoggedSplatFogFar = 0xFFFFFFFFu;
static u32 s_lastLoggedSplatFogColor = 0xFFFFFFFFu;
static u32 s_lastLoggedSplatDynamicLight = 0xFFFFFFFFu;
static u32 s_seenGMFogCall = 0;

Block::Block() {
    MARKFUNCTION(0x80052B64); // __5Block
    memset(this, 0, sizeof(Block));
}

Block::~Block() {
    Destroy();
}

void Block::Destroy() {
    if (primGeom) {
        delete primGeom;
        primGeom = nullptr;
    }

    if (collision) {
        collision->Unload();
        collision = nullptr;
    }

    if (shadowCasterBuffer) {
        shadowCasterBuffer->Release();
        shadowCasterBuffer = nullptr;
    }
}

// Init__5BlockPC8DBVolume (BLOCK.CPP:288)
void Block::Init(const DBVolume* vol) {
    MARKFUNCTION(0x80052BB0);

    // Copy position from DBVolume
    posX = vol->pos.x;
    posY = vol->pos.y;
    posZ = vol->pos.z;

    // Set dimensions from bbox corners
    SetDimension(&vol->bboxMin, &vol->bboxMax);

    // Default LOD values
    lodNearX = 5; lodFarX = 1;
    lodNearY = 5; lodFarY = 1;
    lodNearZ = 5; lodFarZ = 1;
    lodExtra0 = 0;
    lodExtra1 = 0;
    unk38 = 0;
    unk40 = 0;
    fogMinDist = 0;
    fogMaxDist = 0;
    fogColor = 0;
    unk56 = 0;
    unk60 = 0;

    // Attrib 15: block number
    {
        const DBAttrib* a = vol->FindAttrib(15);
        if (a) blockNum = static_cast<u16>(a->value);
    }

    // Attrib 10: LOD near (all axes)
    {
        const DBAttrib* a = vol->FindAttrib(10);
        if (a && a->value != 0) {
            lodNearX = static_cast<u8>(a->value);
            lodNearY = static_cast<u8>(a->value);
            lodNearZ = static_cast<u8>(a->value);
        }
    }

    // Attrib 11: LOD far (all axes)
    {
        const DBAttrib* a = vol->FindAttrib(11);
        if (a && a->value != 0) {
            lodFarX = static_cast<u8>(a->value);
            lodFarY = static_cast<u8>(a->value);
            lodFarZ = static_cast<u8>(a->value);
        }
    }

    // Attrib 12: LOD extra 0
    {
        const DBAttrib* a = vol->FindAttrib(12);
        if (a && a->value != 0) {
            lodExtra0 = static_cast<u8>(a->value);
        }
    }

    // Attrib 13: LOD extra 1
    {
        const DBAttrib* a = vol->FindAttrib(13);
        if (a && a->value != 0) {
            lodExtra1 = static_cast<u8>(a->value);
        }
    }

    // Attrib 16: unk38
    {
        const DBAttrib* a = vol->FindAttrib(16);
        if (a) unk38 = static_cast<u16>(a->value);
    }

    // Attrib 17: unk40
    {
        const DBAttrib* a = vol->FindAttrib(17);
        if (a) unk40 = static_cast<u16>(a->value);
    }

    // Attrib 20: fog min distance
    {
        const DBAttrib* a = vol->FindAttrib(20);
        if (a) fogMinDist = static_cast<u16>(a->value);
    }

    // Attrib 21: fog max distance
    {
        const DBAttrib* a = vol->FindAttrib(21);
        if (a) fogMaxDist = static_cast<u16>(a->value);
    }

    // Attrib 22: fog color
    {
        const DBAttrib* a = vol->FindAttrib(22);
        if (a) fogColor = a->value;
    }

    // Attrib 23: unk56
    {
        const DBAttrib* a = vol->FindAttrib(23);
        if (a) unk56 = static_cast<u16>(a->value);
    }

    // Attrib 24: unk60
    {
        const DBAttrib* a = vol->FindAttrib(24);
        if (a) unk60 = a->value;
    }

    if (fogMinDist != 0 || fogMaxDist != 0 || fogColor != 0) {
        LOG("[Block] Fog attrs block=%u near=%u far=%u col=0x%06X",
            blockNum,
            fogMinDist,
            fogMaxDist,
            fogColor & 0xFFFFFFu);
    }

    // Attrib 25: override lodNearY, lodNearZ
    {
        const DBAttrib* a = vol->FindAttrib(25);
        if (a) {
            lodNearY = static_cast<u8>(a->value);
            lodNearZ = static_cast<u8>(a->value);
        }
    }

    // Attrib 26: override lodFarY, lodFarZ
    {
        const DBAttrib* a = vol->FindAttrib(26);
        if (a) {
            lodFarY = static_cast<u8>(a->value);
            lodFarZ = static_cast<u8>(a->value);
        }
    }

    // Attrib 27: override lodNearZ only
    {
        const DBAttrib* a = vol->FindAttrib(27);
        if (a) lodNearZ = static_cast<u8>(a->value);
    }

    // Attrib 28: override lodFarZ only
    {
        const DBAttrib* a = vol->FindAttrib(28);
        if (a) lodFarZ = static_cast<u8>(a->value);
    }

    // Attrib 99: script info (string attrib)
    {
        const DBAttrib* a = vol->FindAttrib(99);
        if (a) {
            if (a->strValue) {
                hasScript = 1;
                scriptId = static_cast<u16>(static_cast<s8>(vol->scriptAxis));
            }
            else {
                hasScript = 0;
                scriptId = 0;
            }
        }
    }
}
// SetDimension__5BlockRC10tagLVectorT1 (BLOCK.CPP:442)
void Block::SetDimension(const LVector* a, const LVector* b) {
    MARKFUNCTION(0x80052EA0);

    s32 dx = a->x - b->x;
    s32 dy = a->y - b->y;
    s32 dz = a->z - b->z;

    dimX = (dx >= 0) ? dx : -dx;
    dimY = (dy >= 0) ? dy : -dy;
    dimZ = (dz >= 0) ? dz : -dz;

    // Negative half-extents
    halfExtNegX = -(dimX / 2);
    halfExtNegY = -(dimY / 2);
    halfExtNegZ = -(dimZ / 2);

    // Positive half-extents
    halfExtPosX = dimX / 2;
    halfExtPosY = dimY / 2;
    halfExtPosZ = dimZ / 2;
}

// Parse__5BlockUlPc (BLOCK.CPP:482)
// PSX: a1+64 = data, a1+0 = parsed, a1+72 = collision, a1+68 = primGeom
void Block::Parse(u32 size, const u8* blkData) {
    MARKFUNCTION(0x80052F80);

    const u32* d = (const u32*)blkData;

    // PSX: *(a1+64) = a3
    data = blkData;

    // PSX: *(a1+0) = (*(data+16) != 0) ? 1 : 0
    parsed = (d[4] != 0) ? 1 : 0;

    // PSX: collision = AsynchLoad(blockNum, data + *(data+20))
    // AsynchLoad scans g_collisionSectors[12] for empty slot (status == -1),
    // sets status = blockNum, calls Load with the collision data pointer.
    u32 colOffset = d[5]; // *(data+20)
    const u32* colData = (const u32*)(blkData + colOffset);

    CollisionSector* slot = nullptr;
    for (int i = 0; i < MAX_COLLISION_SECTORS; i++) {
        if (g_collisionSectors[i].status == -1) {
            slot = &g_collisionSectors[i];
            break;
        }
    }
    if (slot) {
        slot->status = blockNum;
        slot->Load((u32*)colData);
        collision = slot;
    }
    else {
        LOG("[Block] WARNING: No free collision sector for block %u (all 12 slots used)", blockNum);
    }

    LOG("[Block] Parse block %u: size=%u parsed=%u colOffset=%u walls=%u floors=%u bounds=(%d,%d,%d)-(%d,%d,%d)",
        blockNum, size, parsed, colOffset,
        collision ? collision->wallCount : 0,
        collision ? collision->floorCount : 0,
        collision ? collision->boundsMin.x : 0, collision ? collision->boundsMin.y : 0, collision ? collision->boundsMin.z : 0,
        collision ? collision->boundsMax.x : 0, collision ? collision->boundsMax.y : 0, collision ? collision->boundsMax.z : 0);

    // PSX: *(a1+68) = LoadPrim(data + 24)
    LoadPrim(blkData + 24, size - 24);
}

// Unload__5Block (BLOCK.CPP:514)
// PSX: unloads collision sector, clears data and collision pointers
void Block::Unload() {
    MARKFUNCTION(0x80052FF0);

    // PSX: Unload__15CollisionSector(collision)
    // Collision sectors are in the global array - just unload, don't delete
    if (collision) {
        collision->Unload();
    }
    collision = nullptr;
    data = nullptr;
}

// PointInBlock__C5BlockRC10tagLVector (BLOCK.CPP:524)
// PSX: reads bounds directly from collision sector (+4..+24)
bool Block::PointInBlock(const LVector* pt) const {
    MARKFUNCTION(0x80053024);

    if (!collision) {
        return false;
    }

    // PSX: v2 = *(a1+72) = collision sector ptr
    // v2[1..3] = boundsMin, v2[4..6] = boundsMax
    if (pt->x < collision->boundsMin.x) return false;
    if (pt->x > collision->boundsMax.x) return false;
    if (pt->y < collision->boundsMin.y) return false;
    if (pt->y > collision->boundsMax.y) return false;
    if (pt->z < collision->boundsMin.z) return false;
    if (pt->z > collision->boundsMax.z) return false;
    return true;
}

// GetNextBlockNumber__C5Block (BLOCK.CPP:531)
// PSX: reads unk40 as s16; if > 0 return unk40-1, else return blockNum+1
u32 Block::GetNextBlockNumber() const {
    MARKFUNCTION(0x800530B0);

    s16 val = static_cast<s16>(unk40);
    if (val > 0) return static_cast<u32>(val - 1);
    return static_cast<u32>(blockNum + 1);
}

// GetPrevBlockNumber__C5Block (BLOCK.CPP:549)
// PSX: reads unk40 as s16; if < 0 return ~unk40, else return blockNum-1
u32 Block::GetPrevBlockNumber() const {
    MARKFUNCTION(0x800530D4);

    s16 val = static_cast<s16>(unk40);
    if (val < 0) return static_cast<u32>(~val);
    return static_cast<u32>(blockNum - 1);
}

// Draw__5BlockRC10tagLVector (BLOCK.CPP:600)
// PSX: calls SyncCamView, TransMatrix with drawPos, iterates texPageFrames,
//      updates UVPrimData/CBVPrimData, then RP_ZCullGClip or RP_ZCullGMFog.
// PC: set world matrix from drawPos, draw the prim buffer.
bool Block::Draw(const LVector* drawPos) {
    MARKFUNCTION(0x800530F8);

    if (!primGeom) return false;

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();

    // TransMatrix: set world translation from the passed position
    // (DrawEverythingHandler passes block pos modified by OffsetToPreventSeams)
    // PSX: gte_SetTransMatrix with drawPos directly, no axis flips
    Mat4 world;
    world.SetTranslation(static_cast<f32>(drawPos->x),
                         static_cast<f32>(drawPos->y),
                         static_cast<f32>(drawPos->z));
    p3d::context->SetWorldMatrix(world);

    // Keep block rendering deterministic even if previous effect/model draws
    // changed context state and did not restore it.
    if (g_game && g_game->GetWorld()) {
        p3d::context->SetVRAMHandle(g_game->GetWorld()->GetVRAMHandle());
    }
    p3d::context->SetTexInfoOverride(false, 0);
    p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    p3d::context->SetCullMode(PDDI_CULL_NONE);

    PspPerf::Trace("block draw: uv begin");
    UpdateUVPrimData(blockNum, primGeom);
    UpdateCBVPrimData(blockNum, primGeom);
    PspPerf::Trace("block draw: uv done");

    const u16 blockFogNear = fogMinDist;
    const u16 blockFogFar = fogMaxDist;
    const u32 blockFogColor = fogColor;

    const u16 globalFogNear = static_cast<u16>(splat_fog_near);
    const u16 globalFogFar = static_cast<u16>(splat_fog_far);
    const u32 globalFogColor = splat_fog_color;

    if (s_lastLoggedSplatFogNear != splat_fog_near ||
        s_lastLoggedSplatFogFar != splat_fog_far ||
        s_lastLoggedSplatFogColor != splat_fog_color ||
        s_lastLoggedSplatDynamicLight != splat_dynamic_light) {
        LOG("[Block] splat globals changed: near=%u far=%u col=0x%06X dyn=0x%08X",
            splat_fog_near,
            splat_fog_far,
            splat_fog_color & 0xFFFFFFu,
            splat_dynamic_light);
        s_lastLoggedSplatFogNear = splat_fog_near;
        s_lastLoggedSplatFogFar = splat_fog_far;
        s_lastLoggedSplatFogColor = splat_fog_color;
        s_lastLoggedSplatDynamicLight = splat_dynamic_light;
    }

    u16 fogFar = 0;
    if (blockFogFar != 0) {
        fogFar = (globalFogFar != 0) ? globalFogFar : blockFogFar;
    }
    else if (globalFogFar != 0) {
        fogFar = globalFogFar;
    }
    else {
        PspPerf::Trace("block draw: clip begin");
        RP_ZCullGClip(primGeom, drawPos);
        PspPerf::Trace("block draw: clip done");
        p3d::context->SetWorldMatrix(savedWorld);
        return true;
    }

    const u16 fogNear = (globalFogNear != 0) ? globalFogNear : blockFogNear;
    const u32 fogCol = (globalFogColor != 0) ? globalFogColor : blockFogColor;
    if (!s_seenGMFogCall) {
        LOG("[Block] First GMFog draw: block=%u near=%u far=%u col=0x%06X gNear=%u gFar=%u gCol=0x%06X dyn=0x%08X",
            blockNum,
            fogNear,
            fogFar,
            fogCol & 0xFFFFFFu,
            globalFogNear,
            globalFogFar,
            globalFogColor & 0xFFFFFFu,
            splat_dynamic_light);
        s_seenGMFogCall = 1;
    }
    PspPerf::Trace("block draw: fog begin");
    const int drew = RP_ZCullGMFog(primGeom, drawPos, fogNear, fogFar, fogCol);
    PspPerf::Trace("block draw: fog done");
    if (!drew) {
        LOG("[Block] GMFog draw dropped: block=%u primGeom=%p near=%u far=%u col=0x%06X gNear=%u gFar=%u gCol=0x%06X",
            blockNum,
            primGeom,
            fogNear,
            fogFar,
            fogCol & 0xFFFFFFu,
            globalFogNear,
            globalFogFar,
            globalFogColor & 0xFFFFFFu);
    }

    p3d::context->SetWorldMatrix(savedWorld);

    return true;
}

// LoadPrim__5BlockPv (BLOCK.CPP:668)
// PSX: constructs tEntity on prim data, patches vtable, fixes up 12
//      word-offset pointers to absolute (<<2 + base), patches OT linked list.
// PC: parses GPU packet data to build pddiPrimBuffer.
void Block::LoadPrim(const u8* primData, u32 primSize) {
    MARKFUNCTION(0x8005328C);

    if (primGeom) {
        delete primGeom;
        primGeom = nullptr;
    }
    if (shadowCasterBuffer) {
        shadowCasterBuffer->Release();
        shadowCasterBuffer = nullptr;
    }

    if (!primData || primSize < 108) {
        return;
    }

    tPrimGeom* geom = new tPrimGeom();
    geom->ownedRawData = new u8[primSize];
    geom->ownedRawSize = primSize;
    memcpy(geom->ownedRawData, primData, primSize);

    const u8* raw = geom->ownedRawData;

    u32 vertListOff = p3dReadU32LE(raw + 0x10) << 2;
    u32 primListOff = p3dReadU32LE(raw + 0x40) << 2;
    u32 primListAltOff = p3dReadU32LE(raw + 0x44) << 2;
    u32 primData48Off = p3dReadU32LE(raw + 0x48) << 2;
    u32 primData4COff = p3dReadU32LE(raw + 0x4C) << 2;
    u32 gmFogWriteOff = p3dReadU32LE(raw + 0x50) << 2;
    u32 polyDataOff = p3dReadU32LE(raw + 0x54) << 2;
    u32 gmFogColourOff = p3dReadU32LE(raw + 0x58) << 2;
    u32 primData5COff = p3dReadU32LE(raw + 0x5C) << 2;
    u32 loopVertOff = p3dReadU32LE(raw + 0x60) << 2;
    u32 loopPrimOff = p3dReadU32LE(raw + 0x68) << 2;

    geom->SetVertexList((vertListOff < primSize) ? (raw + vertListOff) : nullptr);
    geom->numVerts = p3dReadU16LE(raw + 0x14);
    geom->numPolys = p3dReadU16LE(raw + 0x16);

    geom->bboxMinX = p3dReadS32LE(raw + 0x18);
    geom->bboxMinY = p3dReadS32LE(raw + 0x1C);
    geom->bboxMinZ = p3dReadS32LE(raw + 0x20);
    geom->bboxMaxX = p3dReadS32LE(raw + 0x24);
    geom->bboxMaxY = p3dReadS32LE(raw + 0x28);
    geom->bboxMaxZ = p3dReadS32LE(raw + 0x2C);
    geom->sphereX = p3dReadS32LE(raw + 0x30);
    geom->sphereY = p3dReadS32LE(raw + 0x34);
    geom->sphereZ = p3dReadS32LE(raw + 0x38);
    geom->sphereRadius = p3dReadS32LE(raw + 0x3C);

    geom->primList = (primListOff < primSize) ? (raw + primListOff) : nullptr;
    geom->primListAlt = (primListAltOff < primSize) ? (raw + primListAltOff) : nullptr;
    geom->primData48 = (primData48Off < primSize) ? (raw + primData48Off) : nullptr;
    geom->primData4C = (primData4COff < primSize) ? (raw + primData4COff) : nullptr;
    geom->gmFogWriteList = (gmFogWriteOff < primSize) ? (raw + gmFogWriteOff) : nullptr;
    geom->polyData = (polyDataOff < primSize) ? (raw + polyDataOff) : nullptr;
    geom->gmFogColourList = (gmFogColourOff < primSize) ? (raw + gmFogColourOff) : nullptr;
    geom->primData5C = (primData5COff < primSize) ? (raw + primData5COff) : nullptr;
    geom->loopVertData = (loopVertOff < primSize) ? (raw + loopVertOff) : nullptr;
    geom->geoType = p3dReadU16LE(raw + 0x64);
    geom->numLoops = p3dReadU16LE(raw + 0x66);
    geom->loopPrimData = (loopPrimOff < primSize) ? (raw + loopPrimOff) : nullptr;

    if (!geom->GetVertexList() || !geom->primList || !geom->polyData || !geom->loopVertData || !geom->loopPrimData) {
        delete geom;
        return;
    }

    primGeom = geom;
}

#include "gen/colsect.h"
#include "pc/log.h"
#include "gen/blockmgr.h"
#include "gen/colwall.h"
#include "gen/colfloor.h"
#include "p3d/p3dmath.h"
#include <new>

// PSX: 12 collision sectors (demand-loaded). PC: 128 to cover all blocks at once.
CollisionSector g_collisionSectors[MAX_COLLISION_SECTORS];

// ---------------------------------------------------------------------------
// Query acceleration (pure cost, identical results).
//
// The world queries below used to visit all 128 sector slots and, inside each
// overlapping sector, read the XZ box of every wall (56-byte stride) and floor
// (80-byte stride).  Per humanoid and step that touched ~90 cache lines of
// sector headers plus one line per wall/floor.  Load() now records, per slot,
// a compact copy of the four box words of each wall/floor and an ordered list of loaded slots with their bounds.
// The boxes are stored as s16 (8 bytes, eight per cache line, ~8 B of RAM per
// wall/floor); a sector with any box word outside s16 keeps the strided path,
// so the widened comparison is always exact.
//
// Equivalence:
//  * Slots that never went through Load() (or were Zero()ed since) hold the
//    Zero() state: no walls/floors and inverted bounds, so they never add an
//    entry nor contain a point.  Skipping them changes nothing.
//  * The active list is rebuilt in slot-index order, so sectors, walls and
//    floors are still visited in exactly the original order (this matters for
//    the 64-entry truncation and for the returned counts).
//  * Sector bounds, wall and floor boxes are only written by Load()/Zero(),
//    which refresh the copies; the collision records themselves are immutable
//    after Load (PSP: Block-owned copy, PC: const archive data).
//  * status is still checked at query time, after the (pure) bounds test.
//  * If the compact allocation fails the original strided reads are used.
// ---------------------------------------------------------------------------
namespace {

struct CollisionBox {
    s16 w0; // wall: xBound1 | floor: xMin (+64)
    s16 w1; // wall: zBound1 | floor: zMin (+68)
    s16 w2; // wall: xBound2 | floor: xMax (+72)
    s16 w3; // wall: zBound2 | floor: zMax (+76)
};
static_assert(sizeof(CollisionBox) == 8, "compact collision box must stay 8 bytes");

bool FitsS16(s32 value) {
    return value >= -32768 && value <= 32767;
}

struct ActiveSector {
    s32 minX, maxX, minY, maxY, minZ, maxZ;
    CollisionSector* sector;
    const CollisionBox* wallBoxes;  // nullptr: read the Wall records
    const CollisionBox* floorBoxes; // nullptr: read the 80-byte floor records
};

bool s_sectorLoaded[MAX_COLLISION_SECTORS];
CollisionBox* s_sectorBoxes[MAX_COLLISION_SECTORS];
bool s_sectorBoxesValid[MAX_COLLISION_SECTORS];
ActiveSector s_activeSectors[MAX_COLLISION_SECTORS];
s32 s_activeSectorCount = 0;

constexpr u32 kFloorRecordStride = 80;
constexpr u32 kFloorBoxOffset = 64;

s32 SectorSlot(const CollisionSector* sector) {
    const CollisionSector* base = &g_collisionSectors[0];
    if (sector < base || sector >= base + MAX_COLLISION_SECTORS) {
        return -1;
    }
    return static_cast<s32>(sector - base);
}

void RebuildActiveSectors() {
    s32 count = 0;
    for (s32 slot = 0; slot < MAX_COLLISION_SECTORS; slot++) {
        if (!s_sectorLoaded[slot]) {
            continue;
        }
        CollisionSector* sector = &g_collisionSectors[slot];
        ActiveSector& entry = s_activeSectors[count++];
        entry.minX = sector->boundsMin.x;
        entry.maxX = sector->boundsMax.x;
        entry.minY = sector->boundsMin.y;
        entry.maxY = sector->boundsMax.y;
        entry.minZ = sector->boundsMin.z;
        entry.maxZ = sector->boundsMax.z;
        entry.sector = sector;
        const CollisionBox* boxes = s_sectorBoxesValid[slot] ? s_sectorBoxes[slot] : nullptr;
        // A valid table with no entries is still "compact" (zero iterations).
        static const CollisionBox kEmpty = {};
        if (s_sectorBoxesValid[slot] && !boxes) {
            boxes = &kEmpty;
        }
        entry.wallBoxes = boxes;
        entry.floorBoxes = boxes ? boxes + sector->wallCount : nullptr;
    }
    s_activeSectorCount = count;
}

void ReleaseSectorBoxes(s32 slot) {
    delete[] s_sectorBoxes[slot];
    s_sectorBoxes[slot] = nullptr;
    s_sectorBoxesValid[slot] = false;
}

void BuildSectorBoxes(s32 slot, const CollisionSector* sector) {
    ReleaseSectorBoxes(slot);
    const u32 total = sector->wallCount + sector->floorCount;
    if (total < sector->wallCount) {
        return; // overflow: keep strided reads
    }
    if (total == 0) {
        s_sectorBoxesValid[slot] = true;
        return;
    }
    const u8* floorBase = reinterpret_cast<const u8*>(sector->floors);
    for (u32 i = 0; i < sector->wallCount; i++) {
        const Wall& wall = sector->walls[i];
        if (!FitsS16(wall.xBound1) || !FitsS16(wall.zBound1)
            || !FitsS16(wall.xBound2) || !FitsS16(wall.zBound2)) {
            return; // keep strided reads for this sector
        }
    }
    for (u32 i = 0; i < sector->floorCount; i++) {
        const s32* floorBounds = reinterpret_cast<const s32*>(
            floorBase + i * kFloorRecordStride + kFloorBoxOffset);
        if (!FitsS16(floorBounds[0]) || !FitsS16(floorBounds[1])
            || !FitsS16(floorBounds[2]) || !FitsS16(floorBounds[3])) {
            return;
        }
    }

    CollisionBox* boxes = new (std::nothrow) CollisionBox[total];
    if (!boxes) {
        return;
    }
    for (u32 i = 0; i < sector->wallCount; i++) {
        const Wall& wall = sector->walls[i];
        boxes[i].w0 = static_cast<s16>(wall.xBound1);
        boxes[i].w1 = static_cast<s16>(wall.zBound1);
        boxes[i].w2 = static_cast<s16>(wall.xBound2);
        boxes[i].w3 = static_cast<s16>(wall.zBound2);
    }
    for (u32 i = 0; i < sector->floorCount; i++) {
        const s32* floorBounds = reinterpret_cast<const s32*>(
            floorBase + i * kFloorRecordStride + kFloorBoxOffset);
        CollisionBox& box = boxes[sector->wallCount + i];
        box.w0 = static_cast<s16>(floorBounds[0]);
        box.w1 = static_cast<s16>(floorBounds[1]);
        box.w2 = static_cast<s16>(floorBounds[2]);
        box.w3 = static_cast<s16>(floorBounds[3]);
    }
    s_sectorBoxes[slot] = boxes;
    s_sectorBoxesValid[slot] = true;
}

} // namespace

static s32 FloorQueryImpl(
    Floor** floorArray, s32 count, const LVector* pos, s32 radius, bool details,
    s32* outCeilingH, LVector* outFloorNormal, LVector* outCeilingNormal,
    s32* outHasRailing, LVector* outRailCorrection);

// Scratch buffer for floor pointer collection (PSX: 0x800DFD58, capacity 64)
static Floor* g_floorPtrScratch[64];

// Scratch buffer for wall pointer collection used by ledge prototype checks
static Wall* g_wallPtrScratch[64];

// PSX: temporary world-wall query pointer array (0x800DFCB8).
static Wall* g_worldWallQueryScratch[64];

// PSX: __15CollisionSector (COLSECT.CPP:415) 0x80042278
CollisionSector::CollisionSector() {
    MARKFUNCTION(0x80042278);
    Zero();
}

// PSX: Zero__15CollisionSector (COLSECT.CPP:423) 0x80040C94
void CollisionSector::Zero() {
    MARKFUNCTION(0x80040C94);
    status = -1;
    boundsMin.x = 32767;
    boundsMin.y = 32767;
    boundsMin.z = 32767;
    boundsMax.x = -32767;
    boundsMax.y = -32767;
    boundsMax.z = -32767;
    wallCount = 0;
    walls = nullptr;
    floorCount = 0;
    floors = nullptr;

    const s32 slot = SectorSlot(this);
    if (slot >= 0 && (s_sectorLoaded[slot] || s_sectorBoxes[slot])) {
        ReleaseSectorBoxes(slot);
        s_sectorLoaded[slot] = false;
        RebuildActiveSectors();
    }
}

// PSX: Load__15CollisionSectorPUl (COLSECT.CPP:1601) 0x800422C0
// Data layout: [12]=minX, [16]=minY, [20]=minZ, [24]=maxX, [28]=maxY, [32]=maxZ,
//              [36]=numWalls, [40]=numFloors, [48]=wallData[], then floorData[]
void CollisionSector::Load(u32* data) {
    MARKFUNCTION(0x800422C0);
    boundsMin.x = (s32)data[3];   // data+12
    boundsMin.y = (s32)data[4];   // data+16
    boundsMin.z = (s32)data[5];   // data+20
    boundsMax.x = (s32)data[6];   // data+24
    boundsMax.y = (s32)data[7];   // data+28
    boundsMax.z = (s32)data[8];   // data+32
    wallCount = data[9];           // data+36
    floorCount = data[10];         // data+40
    walls = (Wall*)&data[12];      // data+48 (inline wall data)
    // Floor data follows walls: each wall is 56 bytes = 14 u32s
    floors = (Floor*)((u8*)walls + wallCount * 56);

    const s32 slot = SectorSlot(this);
    if (slot >= 0) {
        BuildSectorBoxes(slot, this);
        s_sectorLoaded[slot] = true;
        RebuildActiveSectors();
    }
}

// PSX: Unload__15CollisionSector (COLSECT.CPP:445) 0x800422A0
void CollisionSector::Unload() {
    MARKFUNCTION(0x800422A0);
    Zero();
}

// PSX: GetBlockNumber__15CollisionSectorRC10tagLVector (COLSECT.CPP:511) 0x80040D8C
s32 CollisionSector::GetBlockNumber(const LVector& pos) {
    MARKFUNCTION(0x80040D8C);

    s32 blockNum = -1;

    // Same slot order as the 0..127 scan; the (pure) containment test now runs
    // before the IsValidBlockNumber list walk, which only matters for slots
    // that contain pos.
    for (s32 activeIdx = 0; activeIdx < s_activeSectorCount; activeIdx++) {
        const ActiveSector& entry = s_activeSectors[activeIdx];
        if (!(pos.x >= entry.minX && entry.maxX >= pos.x)) continue;
        if (!(pos.y >= entry.minY && entry.maxY >= pos.y)) continue;
        if (!(pos.z >= entry.minZ && entry.maxZ >= pos.z)) continue;

        const CollisionSector* sector = entry.sector;
        if (sector->status == -1) {
            continue;
        }

        const u32 sectorBlockNum = static_cast<u32>(sector->status);
        if (g_blockManager && !g_blockManager->IsValidBlockNumber(sectorBlockNum)) {
            continue;
        }

        blockNum = sector->status;
    }

    return blockNum;
}

// PSX: GetWorldFloorHeight__15CollisionSectorRC10tagLVectorl (COLSECT.CPP:1105) 0x800417B8
// Simple wrapper that discards ceiling/normal outputs
s32 CollisionSector::GetWorldFloorHeight(const LVector& pos, s32 radius) {
    MARKFUNCTION(0x800417B8);

    // Same search box and floor scan as GetWorldFloorAndCeilingHeight; the
    // discarded normal/ceiling/railing outputs are simply not computed.
    LVector searchMin = {};
    searchMin.x = pos.x - radius;
    searchMin.y = pos.y;
    searchMin.z = pos.z - radius;
    LVector searchMax = {};
    searchMax.x = pos.x + radius;
    searchMax.y = pos.y;
    searchMax.z = pos.z + radius;

    s32 count = FillWorldFloorArray(searchMin, searchMax, g_floorPtrScratch, 64);
    if (count > 64) count = 64;

    return FloorQueryImpl(g_floorPtrScratch, count, &pos, radius, false,
                          nullptr, nullptr, nullptr, nullptr, nullptr);
}

// PSX: (COLSECT.CPP:1142) 0x800417F0
// Query floor/ceiling height at a world position using all loaded collision sectors
void CollisionSector::GetWorldFloorAndCeilingHeight(
    s32& outFloorH, s32& outCeilingH,
    LVector& outFloorNormal, LVector& outCeilingNormal,
    const LVector& pos, s32 radius) {
    MARKFUNCTION(0x800417F0);

    // Build search box: pos ± radius
    LVector searchMin;
    memset(&searchMin, 0, sizeof(LVector));
    searchMin.x = pos.x - radius;
    searchMin.y = pos.y;
    searchMin.z = pos.z - radius;

    LVector searchMax;
    memset(&searchMax, 0, sizeof(LVector));
    searchMax.x = pos.x + radius;
    searchMax.y = pos.y;
    searchMax.z = pos.z + radius;

    // Collect overlapping floors into scratch array
    s32 count = FillWorldFloorArray(searchMin, searchMax, g_floorPtrScratch, 64);
    if (count > 64) count = 64;

    s32 localHasRailing;
    LVector localRailCorrection;
    GetArrayFloorAndCeilingHeight(g_floorPtrScratch, count,
                                  outFloorH, outCeilingH, outFloorNormal, outCeilingNormal,
                                  &localHasRailing, &localRailCorrection, &pos, radius);
}

// PSX: (COLSECT.CPP:1207) 0x80041980
// Collect Floor pointers overlapping with the search AABB from all sectors
s32 CollisionSector::FillWorldFloorArray(
    const LVector& searchMin, const LVector& searchMax,
    Floor** outArray, s32 maxCount) {
    MARKFUNCTION(0x80041980);

    s32 totalAdded = 0;
    const s32 sMinX = searchMin.x;
    const s32 sMinZ = searchMin.z;
    const s32 sMaxX = searchMax.x;
    const s32 sMaxZ = searchMax.z;

    for (s32 activeIdx = 0; activeIdx < s_activeSectorCount; activeIdx++) {
        const ActiveSector& entry = s_activeSectors[activeIdx];

        // AABB overlap test between search box and sector bounds
        if (sMaxX < entry.minX) continue;
        if (entry.maxX < sMinX) continue;
        if (sMaxZ < entry.minZ) continue;
        if (entry.maxZ < sMinZ) continue;

        CollisionSector* sector = entry.sector;
        if (sector->status == -1) continue;

        if (const CollisionBox* boxes = entry.floorBoxes) {
            u8* floorBase = (u8*)sector->floors;
            const u32 floorCount = sector->floorCount;
            for (u32 i = 0; i < floorCount; i++) {
                const CollisionBox& box = boxes[i];
                if (sMaxX < box.w0) continue;
                if (box.w2 < sMinX) continue;
                if (sMaxZ < box.w1) continue;
                if (box.w3 < sMinZ) continue;

                if (totalAdded < maxCount) {
                    outArray[totalAdded] = (Floor*)(floorBase + i * kFloorRecordStride);
                }
                totalAdded++;
            }
            continue;
        }

        // Fallback (compact table unavailable): iterate floors in this sector
        // PSX: floor entries are 80 bytes (64B Floor + 16B AABB) in the data stream
        // but our Floor struct is 64B; the AABB bounds are at offsets +64..+76 past each entry
        u8* floorBase = (u8*)sector->floors;
        for (u32 i = 0; i < sector->floorCount; i++) {
            Floor* floor = (Floor*)(floorBase + i * 80);
            // Per-floor AABB at offsets +64,+68,+72,+76 of the 80-byte entry
            s32* floorBounds = (s32*)(floorBase + i * 80 + 64);
            s32 fXMin = floorBounds[0]; // +64
            s32 fZMin = floorBounds[1]; // +68
            s32 fXMax = floorBounds[2]; // +72
            s32 fZMax = floorBounds[3]; // +76

            if (searchMax.x < fXMin) continue;
            if (fXMax < searchMin.x) continue;
            if (searchMax.z < fZMin) continue;
            if (fZMax < searchMin.z) continue;

            if (totalAdded < maxCount) {
                outArray[totalAdded] = floor;
            }
            totalAdded++;
        }
    }

    return totalAdded;
}

// Shared body of GetArrayFloorAndCeilingHeight.  With details == false only
// the floor height is produced (normal, ceiling-normal and railing outputs are
// not computed); the returned floor height is the same in both modes because
// passes 1-2 are identical and pass 3 never changes v12/v17.
static s32 FloorQueryImpl(
    Floor** floorArray, s32 count, const LVector* pos, s32 radius, bool details,
    s32* outCeilingH, LVector* outFloorNormal, LVector* outCeilingNormal,
    s32* outHasRailing, LVector* outRailCorrection) {

    // PSX: v12 = lowest walkable ceiling above test pos
    //      v13 = lowest secondary ceiling above test pos
    s32 v12 = 0x7FFFFFFF;
    s32 v13 = 0x7FFFFFFF;

    // Pure-cost restructuring (identical outputs):
    //  * GetFloorHeight/CheckFloorBounds are pure, so each is evaluated at
    //    most once per floor here (heights from pass 1 are reused, bounds are
    //    memoised) and the cheap height/flag tests run before the bounds test
    //    whenever the bounds result could not change the outcome.
    //  * Pass 2 used to normalise the floor normal (integer sqrt + division)
    //    on every improvement; only the last improvement is observable, so it
    //    is normalised once after the pass.  Pass 3 likewise keeps only the
    //    last railing correction.
    // Callers clamp count to 64; larger arrays just bypass the memo.
    constexpr s32 kMemo = 64;
    s32 heights[kMemo];
    s8 boundsMemo[kMemo];
    const LVector& p = *pos;
    auto heightAt = [&](s32 i) -> s32 {
        return (i < kMemo) ? heights[i] : floorArray[i]->GetFloorHeight(p);
    };
    auto inBounds = [&](s32 i) -> bool {
        if (i >= kMemo) {
            return floorArray[i]->CheckFloorBounds(p, radius) != 0;
        }
        if (boundsMemo[i] < 0) {
            boundsMemo[i] = floorArray[i]->CheckFloorBounds(p, radius) ? 1 : 0;
        }
        return boundsMemo[i] != 0;
    };

    // Pass 1: find lowest surfaces ABOVE the test position
    // PSX condition: a9[1] < h  (pos->y < floorHeight)
    // Original: if (pos.y < h && bounds) { if (f&1 && h<v12) v12=h;
    //                                       else if (f&2 && h<v13) v13=h; }
    const s32 posY = p.y;
    for (s32 i = 0; i < count; i++) {
        Floor* floor = floorArray[i];
        s32 h = floor->GetFloorHeight(p);
        if (i < kMemo) {
            heights[i] = h;
            boundsMemo[i] = -1;
        }

        if (!(posY < h)) continue;
        const u16 flags = floor->flags;
        if ((flags & 1) && h < v12) {
            if (inBounds(i)) v12 = h;
        }
        else if (((flags >> 1) & 1) && h < v13) {
            if (inBounds(i)) v13 = h;
        }
    }

    if (v13 < v12) {
        v12 = v13;
    }

    // Pass 2: find highest walkable floor BELOW the ceiling (v12)
    s32 v17 = (s32)0x80000001;
    const Floor* bestFloor = nullptr;
    for (s32 i = 0; i < count; i++) {
        const Floor* floor = floorArray[i];

        const u16 flags = floor->flags;
        bool walkable = (flags & 1) != 0;
        bool noRailing = ((flags >> 2) & 1) == 0;
        if (!(walkable && noRailing)) continue;

        s32 h = heightAt(i);
        if (!(h < v12 && v17 < h)) continue;
        if (!inBounds(i)) continue;

        v17 = h;
        bestFloor = floor;
    }
    if (!details) {
        return (v17 <= (s32)0x80000001) ? (s32)0x80000001 : v17;
    }
    if (bestFloor) {
        bestFloor->GetFloorNormal(*outFloorNormal);
        outCeilingNormal->x = (s32)(u16)bestFloor->field0C;
    }

    // Pass 3: check railing floors for correction
    *outHasRailing = 0;
    const Floor* railingFloor = nullptr;
    for (s32 i = 0; i < count; i++) {
        const Floor* floor = floorArray[i];

        const u16 flags = floor->flags;
        bool walkable = (flags & 1) != 0;
        bool hasRailing = ((flags >> 2) & 1) != 0;
        if (!(walkable && hasRailing)) continue;

        s32 h = heightAt(i);
        if (!(h < v12 && v17 < h)) continue;
        if (!inBounds(i)) continue;

        railingFloor = floor;
    }
    if (railingFloor) {
        *outHasRailing = 1;
        railingFloor->GetRailingCorrection(*outRailCorrection, p);
    }

    // PSX output: a3 = v17 (floor to stand on), a4 = v13 (secondary ceiling)
    *outCeilingH = (v13 == 0x7FFFFFFF) ? 0x7FFFFFFF : v13;
    return (v17 <= (s32)0x80000001) ? (s32)0x80000001 : v17;
}

// PSX: (COLSECT.CPP:1271) 0x80041ADC
// Scan collected floors for best floor height, ceiling height, and railing corrections
void CollisionSector::GetArrayFloorAndCeilingHeight(
    Floor** floorArray, s32 count,
    s32& outFloorH, s32& outCeilingH,
    LVector& outFloorNormal, LVector& outCeilingNormal,
    s32* outHasRailing, LVector* outRailCorrection,
    const LVector* pos, s32 radius) {
    MARKFUNCTION(0x80041ADC);
    outFloorH = FloorQueryImpl(floorArray, count, pos, radius, true,
        &outCeilingH, &outFloorNormal, &outCeilingNormal, outHasRailing, outRailCorrection);
}

// PSX: LedgePrototype__15CollisionSectorRC10tagLVectorT1llR9_RMVECT16R10tagLVectorRll (COLSECT.CPP:1403)
bool CollisionSector::LedgePrototype(
    const LVector& startPos, const LVector& endPos,
    s32 minHeight, s32 maxHeight,
    LVector& outNormal, LVector& outCorrectionPos,
    u16& outMaterial, s32 clearance) {
    MARKFUNCTION(0x80041DC4);

    LVector searchMin = {};
    LVector searchMax = {};
    searchMin.x = (startPos.x < endPos.x) ? startPos.x : endPos.x;
    searchMin.y = (startPos.y < endPos.y) ? startPos.y : endPos.y;
    searchMin.z = (startPos.z < endPos.z) ? startPos.z : endPos.z;
    searchMax.x = (startPos.x > endPos.x) ? startPos.x : endPos.x;
    searchMax.y = (startPos.y > endPos.y) ? startPos.y : endPos.y;
    searchMax.z = (startPos.z > endPos.z) ? startPos.z : endPos.z;

    s32 wallCount = FillWorldWallArray(searchMin, searchMax, g_wallPtrScratch, 64);
    if (wallCount > 64) {
        wallCount = 64;
    }

    s32 floorCount = FillWorldFloorArray(searchMin, searchMax, g_floorPtrScratch, 64);
    if (floorCount > 64) {
        floorCount = 64;
    }

    for (s32 floorIdx = 0; floorIdx < floorCount; floorIdx++) {
        Floor* floor = g_floorPtrScratch[floorIdx];
        if (!floor->LedgePrototype(startPos, endPos, minHeight, maxHeight, outNormal, outCorrectionPos)) {
            continue;
        }

        outMaterial = (u16)floor->field0C;

        LVector floorNormal = {};
        floor->GetFloorNormal(floorNormal);
        if (floorNormal.y <= 46347) {
            s32 dot =
                fixmul16(outNormal.x, floorNormal.x) +
                fixmul16(outNormal.z, floorNormal.z);
            if (dot >= 0) {
                continue;
            }
        }

        LVector probePos = {};
        probePos.x = outCorrectionPos.x + (s16)((s64)outNormal.x >> 9);
        probePos.y = outCorrectionPos.y;
        probePos.z = outCorrectionPos.z + (s16)((s64)outNormal.z >> 9);

        s32 floorH = 0;
        s32 ceilingH = 0;
        LVector floorNorm = {};
        LVector ceilingNorm = {};
        s32 hasRailing = 0;
        LVector railCorrection = {};
        GetArrayFloorAndCeilingHeight(
            g_floorPtrScratch,
            floorCount,
            floorH,
            ceilingH,
            floorNorm,
            ceilingNorm,
            &hasRailing,
            &railCorrection,
            &probePos,
            0);

        if (outCorrectionPos.y < floorH + clearance) {
            continue;
        }

        s32 normalStepX = (s16)((s64)outNormal.x >> 10);
        s32 normalStepZ = (s16)((s64)outNormal.z >> 10);

        LVector wallStart = {};
        wallStart.x = outCorrectionPos.x + normalStepX;
        wallStart.y = outCorrectionPos.y + 64;
        wallStart.z = outCorrectionPos.z + normalStepZ;

        LVector wallEnd = {};
        wallEnd.x = outCorrectionPos.x - normalStepX;
        wallEnd.y = outCorrectionPos.y + 64;
        wallEnd.z = outCorrectionPos.z - normalStepZ;

        bool blocked = false;
        for (s32 wallIdx = 0; wallIdx < wallCount; wallIdx++) {
            s32 outFrac = 0;
            LVector outWallNormal = {};
            LVector outHitPoint = {};
            if (g_wallPtrScratch[wallIdx]->CheckWallCollision(
                wallStart,
                wallEnd,
                0,
                0,
                0,
                0,
                outFrac,
                outWallNormal,
                outHitPoint)) {
                blocked = true;
                break;
            }
        }

        if (blocked) {
            continue;
        }

        return true;
    }

    return false;
}

// Wall collision result globals
WallCollisionInfo g_wallCollisionInfo;  // PSX: 0x800E00C8
Wall* g_colHitWall = nullptr;           // PSX: gp+1160

// PSX: CheckWorldWallCollision__15CollisionSectorRC10tagLVectorT1lllRlR9_RMVECT16R10tagLVectorRl
// (COLSECT.CPP:657) 0x80041038
s32 CollisionSector::CheckWorldWallCollision(
    const LVector& oldPos, const LVector& newPos,
    s32 radius, s32 height, s32 arg5,
    s32& outRatio, LVector& outNormal, LVector& outHitPoint, s32& outWallHorizontal) {
    MARKFUNCTION(0x80041038);

    LVector searchMin = {};
    LVector searchMax = {};
    searchMin.x = (oldPos.x < newPos.x) ? oldPos.x : newPos.x;
    searchMin.y = (oldPos.y < newPos.y) ? oldPos.y : newPos.y;
    searchMin.z = (oldPos.z < newPos.z) ? oldPos.z : newPos.z;
    searchMax.x = (oldPos.x > newPos.x) ? oldPos.x : newPos.x;
    searchMax.y = (oldPos.y > newPos.y) ? oldPos.y : newPos.y;
    searchMax.z = (oldPos.z > newPos.z) ? oldPos.z : newPos.z;

    searchMin.x -= radius;
    searchMin.z -= radius;
    searchMax.x += radius;
    searchMax.z += radius;
    searchMin.y += height;
    searchMax.y += arg5;

    s32 wallCount = FillWorldWallArray(searchMin, searchMax, g_worldWallQueryScratch, 64);
    if (wallCount > 64) {
        wallCount = 64;
    }

    const s32 hit = CheckArrayWallCollision(
        g_worldWallQueryScratch,
        wallCount,
        oldPos,
        newPos,
        radius,
        height,
        arg5,
        0);

    outRatio = g_wallCollisionInfo.collisionRatio;
    outNormal = g_wallCollisionInfo.wallNormal;
    outHitPoint = g_wallCollisionInfo.hitPoint;
    outWallHorizontal = g_wallCollisionInfo.wallHorizontal;
    return hit;
}

// PSX: FillWorldWallArray__15CollisionSectorRC10tagLVectorT1PPC4Walli (COLSECT.CPP:863) 0x800411F8
// Collect Wall pointers overlapping with the search AABB from all sectors
s32 CollisionSector::FillWorldWallArray(
    const LVector& searchMin, const LVector& searchMax,
    Wall** outArray, s32 maxCount) {
    MARKFUNCTION(0x800411F8);

    s32 totalAdded = 0;
    const s32 sMinX = searchMin.x;
    const s32 sMinZ = searchMin.z;
    const s32 sMaxX = searchMax.x;
    const s32 sMaxZ = searchMax.z;

    for (s32 activeIdx = 0; activeIdx < s_activeSectorCount; activeIdx++) {
        const ActiveSector& entry = s_activeSectors[activeIdx];

        // 6-axis AABB overlap test (sector bounds vs search box)
        if (sMaxX < entry.minX) continue;
        if (entry.maxX < sMinX) continue;
        if (searchMax.y < entry.minY) continue;
        if (entry.maxY < searchMin.y) continue;
        if (sMaxZ < entry.minZ) continue;
        if (entry.maxZ < sMinZ) continue;

        CollisionSector* sector = entry.sector;
        if (sector->status == -1) continue;

        if (const CollisionBox* boxes = entry.wallBoxes) {
            Wall* walls = sector->walls;
            const u32 wallCount = sector->wallCount;
            for (u32 i = 0; i < wallCount; i++) {
                const CollisionBox& box = boxes[i];
                if (!(sMinX < box.w2)) continue;
                if (!(sMinZ < box.w3)) continue;
                if (!(box.w0 < sMaxX)) continue;
                if (!(box.w1 < sMaxZ)) continue;

                if (totalAdded < maxCount) {
                    outArray[totalAdded] = &walls[i];
                }
                totalAdded++;
            }
            continue;
        }

        // Fallback (compact table unavailable): walls, stride 56 = sizeof(Wall)
        for (u32 i = 0; i < sector->wallCount; i++) {
            Wall* wall = &sector->walls[i];

            // Per-wall AABB check using bound fields
            if (!(searchMin.x < wall->xBound2)) continue;
            if (!(searchMin.z < wall->zBound2)) continue;
            if (!(wall->xBound1 < searchMax.x)) continue;
            if (!(wall->zBound1 < searchMax.z)) continue;

            if (totalAdded < maxCount) {
                outArray[totalAdded] = wall;
            }
            totalAdded++;
        }
    }

    return totalAdded;
}

// PSX: CheckArrayWallCollision__15CollisionSectorPPC4WalliRC10tagLVectorT3llli (COLSECT.CPP:931) 0x80041384
// Find best (smallest fraction) wall collision from the wall array
s32 CollisionSector::CheckArrayWallCollision(
    Wall** walls, s32 count,
    const LVector& oldPos, const LVector& newPos,
    s32 radius, s32 height, s32 arg5, int checkHeight) {
    MARKFUNCTION(0x80041384);

    s32 found = 0;
    g_colHitWall = nullptr;
    g_wallCollisionInfo.collisionRatio = 0x00020000; // 2.0 in 16.16 (sentinel)

    for (s32 i = 0; i < count; i++) {
        Wall* wall = walls[i];

        // Skip curb walls when checking height
        if (checkHeight != 0) {
            if (wall->IsCurb()) continue;
        }

        s32 outFrac;
        LVector outNormal;
        LVector outHitPoint;

        s32 hit = wall->CheckWallCollision(
            oldPos, newPos,
            radius, height, arg5, checkHeight,
            outFrac, outNormal, outHitPoint);

        if (!hit) continue;
        if (!(outFrac < g_wallCollisionInfo.collisionRatio)) continue;

        // New best collision
        g_colHitWall = wall;
        g_wallCollisionInfo.collisionRatio = outFrac;
        g_wallCollisionInfo.wallNormal = outNormal;
        g_wallCollisionInfo.hitPoint = outHitPoint;

        // Determine coordinate for slope evaluation
        s32 coord;
        if ((wall->flags & 0xFFFF0000) == 0xFFFF0000) {
            coord = outHitPoint.x;
        }
        else {
            coord = outHitPoint.z;
        }

        g_wallCollisionInfo.wallHorizontal = wall->CheckWallBounds(
            outHitPoint, radius, height, arg5, checkHeight);
        g_wallCollisionInfo.wallVerticalMin =
            fixmul16(wall->topSlope, coord) + wall->topIntercept;
        g_wallCollisionInfo.wallVerticalMax =
            fixmul16(wall->bottomSlope, coord) + wall->bottomIntercept;
        g_wallCollisionInfo.wallMaterial = (wall->flags & 0xFFFF);

        found = 1;
    }

    return found;
}

// Helper: fixed-point dot product of two LVectors
static s32 LVectorDot(const LVector* a, const LVector* b) {
    return fixmul16(a->x, b->x) + fixmul16(a->y, b->y) + fixmul16(a->z, b->z);
}

// PSX: CheckArrayWallIntersection__15CollisionSectorPPC4WalliR10tagLVectorRC10tagLVectorllli (COLSECT.CPP:1011) 0x800415C0
// Iteratively push position out of all intersecting walls
s32 CollisionSector::CheckArrayWallIntersection(
    Wall** walls, s32 count,
    LVector& hitPos, const LVector& moveDir,
    s32 radius, s32 height, s32 arg5, int checkHeight) {
    MARKFUNCTION(0x800415C0);

    s32 foundAny = 0;
    s32 iterCount = 0;
    LVector currentPos = moveDir;

    do {
        s32 hitCount = 0;
        s32 anyNegDot = 0;
        LVector hitInfoArray[4]; // {normalX, 0, normalZ} entries for dot product checks

        for (s32 wallIdx = 0; wallIdx < count; wallIdx++) {
            Wall* wall = walls[wallIdx];

            if (checkHeight != 0) {
                if (wall->IsCurb()) continue;
            }

            LVector hitResult;
            s32 hit = wall->CheckWallIntersection(
                hitResult, currentPos,
                radius, height, arg5, checkHeight);

            if (!hit) continue;

            currentPos = hitResult;
            foundAny = 1;

            if (hitCount >= 4) continue;

            // Record wall normal for dot product checks
            hitInfoArray[hitCount].x = wall->normalX;
            hitInfoArray[hitCount].y = 0;
            hitInfoArray[hitCount].z = wall->normalZ;

            // Check for opposing normals
            for (s32 j = 0; j < hitCount; j++) {
                s32 dot = LVectorDot(&hitInfoArray[hitCount], &hitInfoArray[j]);
                if (dot < 0) {
                    anyNegDot = 1;
                }
            }

            hitCount++;
        }

        if (anyNegDot == 0) break;
        iterCount++;
    } while (iterCount < 16);

    if (foundAny) {
        hitPos = currentPos;
    }

    return foundAny;
}

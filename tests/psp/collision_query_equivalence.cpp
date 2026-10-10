// Differential test: the accelerated CollisionSector world queries (active
// sector list, compact 8-byte (s16) wall/floor boxes with strided fallback, memoised floor passes and the
// height-only GetWorldFloorHeight) must return exactly what the original
// 128-slot / strided implementation returned: same counts, same pointers in
// the same order, same heights, normals, ceilings and railing corrections.
#include "gen/blockmgr.h"
#include "gen/colfloor.h"
#include "gen/colsect.h"
#include "gen/colwall.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

BlockManager* g_blockManager = nullptr;
bool BlockManager::IsValidBlockNumber(u32) const { return true; }

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "Collision query regression: %s\n", message); std::abort(); }
}

// ---- Reference implementation (verbatim logic of the pre-optimisation code) ----
static s32 RefGetBlockNumber(const LVector& pos) {
    s32 blockNum = -1;
    for (s32 i = 0; i < MAX_COLLISION_SECTORS; i++) {
        const CollisionSector* s = &g_collisionSectors[i];
        if (s->status == -1) continue;
        bool inside = pos.x >= s->boundsMin.x && s->boundsMax.x >= pos.x &&
            pos.y >= s->boundsMin.y && s->boundsMax.y >= pos.y &&
            pos.z >= s->boundsMin.z && s->boundsMax.z >= pos.z;
        if (inside) blockNum = s->status;
    }
    return blockNum;
}

static s32 RefFillFloors(const LVector& mn, const LVector& mx, Floor** out, s32 maxCount) {
    s32 total = 0;
    for (s32 si = 0; si < MAX_COLLISION_SECTORS; si++) {
        CollisionSector* s = &g_collisionSectors[si];
        if (s->status == -1) continue;
        if (mx.x < s->boundsMin.x || s->boundsMax.x < mn.x) continue;
        if (mx.z < s->boundsMin.z || s->boundsMax.z < mn.z) continue;
        u8* base = (u8*)s->floors;
        for (u32 i = 0; i < s->floorCount; i++) {
            s32* b = (s32*)(base + i * 80 + 64);
            if (mx.x < b[0] || b[2] < mn.x || mx.z < b[1] || b[3] < mn.z) continue;
            if (total < maxCount) out[total] = (Floor*)(base + i * 80);
            total++;
        }
    }
    return total;
}

static s32 RefFillWalls(const LVector& mn, const LVector& mx, Wall** out, s32 maxCount) {
    s32 total = 0;
    for (s32 si = 0; si < MAX_COLLISION_SECTORS; si++) {
        CollisionSector* s = &g_collisionSectors[si];
        if (s->status == -1) continue;
        if (mx.x < s->boundsMin.x || s->boundsMax.x < mn.x) continue;
        if (mx.y < s->boundsMin.y || s->boundsMax.y < mn.y) continue;
        if (mx.z < s->boundsMin.z || s->boundsMax.z < mn.z) continue;
        for (u32 i = 0; i < s->wallCount; i++) {
            Wall* w = &s->walls[i];
            if (!(mn.x < w->xBound2) || !(mn.z < w->zBound2)) continue;
            if (!(w->xBound1 < mx.x) || !(w->zBound1 < mx.z)) continue;
            if (total < maxCount) out[total] = w;
            total++;
        }
    }
    return total;
}

static void RefFloorAndCeiling(Floor** arr, s32 count, s32& outFloorH, s32& outCeilH,
    LVector& outFloorN, LVector& outCeilN, s32* outRail, LVector* outRailCorr,
    const LVector* pos, s32 radius) {
    s32 v12 = 0x7FFFFFFF, v13 = 0x7FFFFFFF;
    for (s32 i = 0; i < count; i++) {
        Floor* f = arr[i];
        s32 h = f->GetFloorHeight(*pos);
        if (pos->y < h && f->CheckFloorBounds(*pos, radius)) {
            if ((f->flags & 1) && h < v12) v12 = h;
            else if (((f->flags >> 1) & 1) && h < v13) v13 = h;
        }
    }
    if (v13 < v12) v12 = v13;
    s32 v17 = (s32)0x80000001;
    for (s32 i = 0; i < count; i++) {
        Floor* f = arr[i];
        if (!((f->flags & 1) && !((f->flags >> 2) & 1))) continue;
        if (!f->CheckFloorBounds(*pos, radius)) continue;
        s32 h = f->GetFloorHeight(*pos);
        if (h < v12 && v17 < h) {
            v17 = h;
            f->GetFloorNormal(outFloorN);
            outCeilN.x = (s32)(u16)f->field0C;
        }
    }
    *outRail = 0;
    for (s32 i = 0; i < count; i++) {
        Floor* f = arr[i];
        if (!((f->flags & 1) && ((f->flags >> 2) & 1))) continue;
        if (!f->CheckFloorBounds(*pos, radius)) continue;
        s32 h = f->GetFloorHeight(*pos);
        if (h < v12 && v17 < h) {
            *outRail = 1;
            f->GetRailingCorrection(*outRailCorr, *pos);
        }
    }
    outFloorH = (v17 <= (s32)0x80000001) ? (s32)0x80000001 : v17;
    outCeilH = (v13 == 0x7FFFFFFF) ? 0x7FFFFFFF : v13;
}

// ---- Synthetic sector data ----
static u32 g_rng = 0x1234567u;
static s32 rnd(s32 lo, s32 hi) { // inclusive
    g_rng = g_rng * 1103515245u + 12345u;
    return lo + (s32)((g_rng >> 8) % (u32)(hi - lo + 1));
}

static std::vector<u32> MakeSector(s32 ox, s32 oz, u32 walls, u32 floors) {
    std::vector<u32> d(12 + walls * 14 + floors * 20, 0);
    d[3] = (u32)ox; d[4] = (u32)rnd(-3000, -500); d[5] = (u32)oz;
    d[6] = (u32)(ox + 4000); d[7] = (u32)rnd(500, 3000); d[8] = (u32)(oz + 4000);
    d[9] = walls; d[10] = floors;
    for (u32 i = 0; i < walls; i++) {
        u32* w = &d[12 + i * 14];
        for (int k = 0; k < 10; k++) w[k] = (u32)rnd(-70000, 70000);
        s32 x1 = ox + rnd(-500, 4000), z1 = oz + rnd(-500, 4000);
        w[10] = (u32)x1; w[11] = (u32)z1;
        w[12] = (u32)(x1 + rnd(0, 1500)); w[13] = (u32)(z1 + rnd(0, 1500));
    }
    for (u32 i = 0; i < floors; i++) {
        u32* f = &d[12 + walls * 14 + i * 20];
        s32 x0 = ox + rnd(-200, 3500), z0 = oz + rnd(-200, 3500);
        s32 x1 = x0 + rnd(50, 1500), z1 = z0 + rnd(50, 1500);
        f[0] = (u32)rnd(-30000, 30000);              // normalX (slope)
        f[1] = (u32)rnd(-30000, 30000);              // normalZ
        f[2] = (u32)rnd(-800, 800);                  // heightC
        f[3] = ((u32)rnd(0, 7) << 16) | (u32)rnd(0, 0xFFFF); // flags | field0C
        // Rectangle as 4 half-planes: x - x0 >= 0, x1 - x >= 0, z - z0, z1 - z.
        f[4] = 65536; f[5] = 0; f[6] = (u32)-x0;
        f[7] = (u32)-65536; f[8] = 0; f[9] = (u32)x1;
        f[10] = 0; f[11] = 65536; f[12] = (u32)-z0;
        f[13] = 0; f[14] = (u32)-65536; f[15] = (u32)z1;
        f[16] = (u32)x0; f[17] = (u32)z0; f[18] = (u32)x1; f[19] = (u32)z1;
    }
    return d;
}

static std::vector<std::vector<u32>> g_data(MAX_COLLISION_SECTORS);

static void LoadSlot(s32 slot, s32 blockNum, bool outsideS16 = false) {
    s32 ox = rnd(-6, 6) * 3000, oz = rnd(-6, 6) * 3000;
    if (outsideS16) ox = 30000; // boxes reach > 32767: strided fallback path
    g_data[slot] = MakeSector(ox, oz, (u32)rnd(0, 40), (u32)rnd(0, 30));
    g_collisionSectors[slot].status = blockNum;
    g_collisionSectors[slot].Load(g_data[slot].data());
}

static bool SameVec(const LVector& a, const LVector& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

static long g_checks = 0;
static long g_wallHits = 0, g_floorHits = 0, g_truncated = 0, g_validFloors = 0, g_railings = 0, g_ceilings = 0;

static void CompareQueries(int iterations) {
    for (int it = 0; it < iterations; it++) {
        LVector c = { rnd(-22000, 36000), rnd(-2500, 2500), rnd(-22000, 22000) };
        s32 ext = rnd(0, 3) == 0 ? rnd(0, 20000) : rnd(0, 1500);
        LVector mn = { c.x - ext, c.y - rnd(0, 800), c.z - ext };
        LVector mx = { c.x + ext, c.y + rnd(0, 800), c.z + ext };
        s32 cap = rnd(0, 3) == 0 ? rnd(0, 8) : 64;

        Wall* wa[64]; Wall* wb[64];
        std::memset(wa, 0, sizeof(wa)); std::memset(wb, 0, sizeof(wb));
        s32 nwa = RefFillWalls(mn, mx, wa, cap);
        s32 nwb = CollisionSector::FillWorldWallArray(mn, mx, wb, cap);
        require(nwa == nwb, "wall count");
        require(std::memcmp(wa, wb, sizeof(wa)) == 0, "wall array/order");
        g_wallHits += nwa > 0;
        g_truncated += nwa > cap;

        Floor* fa[64]; Floor* fb[64];
        std::memset(fa, 0, sizeof(fa)); std::memset(fb, 0, sizeof(fb));
        s32 nfa = RefFillFloors(mn, mx, fa, cap);
        s32 nfb = CollisionSector::FillWorldFloorArray(mn, mx, fb, cap);
        require(nfa == nfb, "floor count");
        require(std::memcmp(fa, fb, sizeof(fa)) == 0, "floor array/order");
        g_floorHits += nfa > 0;
        g_truncated += nfa > cap;

        // Floor/ceiling evaluation over a full-capacity array.
        nfa = RefFillFloors(mn, mx, fa, 64);
        if (nfa > 64) nfa = 64;
        for (int q = 0; q < 4; q++) {
            LVector p = { rnd(mn.x, mx.x), rnd(-1500, 1500), rnd(mn.z, mx.z) };
            s32 radius = rnd(0, 3) == 0 ? 0 : rnd(0, 600);
            s32 fhA = 11, chA = 12, rA = 13, fhB = 11, chB = 12, rB = 13;
            LVector fnA = { 1, 2, 3 }, cnA = { 4, 5, 6 }, rcA = { 7, 8, 9 };
            LVector fnB = fnA, cnB = cnA, rcB = rcA;
            RefFloorAndCeiling(fa, nfa, fhA, chA, fnA, cnA, &rA, &rcA, &p, radius);
            CollisionSector::GetArrayFloorAndCeilingHeight(fa, nfa, fhB, chB, fnB, cnB, &rB, &rcB, &p, radius);
            require(fhA == fhB && chA == chB, "floor/ceiling height");
            require(SameVec(fnA, fnB) && SameVec(cnA, cnB), "floor/ceiling normal");
            require(rA == rB && SameVec(rcA, rcB), "railing correction");
            g_validFloors += fhA != (s32)0x80000001;
            g_railings += rA;
            g_ceilings += chA != 0x7FFFFFFF;

            // World wrappers.
            LVector smn = { p.x - radius, p.y, p.z - radius };
            LVector smx = { p.x + radius, p.y, p.z + radius };
            Floor* wfa[64];
            s32 wn = RefFillFloors(smn, smx, wfa, 64);
            if (wn > 64) wn = 64;
            s32 wfh = 0, wch = 0, wr = 0;
            LVector wfn = {}, wcn = {}, wrc = {};
            RefFloorAndCeiling(wfa, wn, wfh, wch, wfn, wcn, &wr, &wrc, &p, radius);
            require(g_collisionSectors[0].GetWorldFloorHeight(p, radius) == wfh, "GetWorldFloorHeight");
            s32 gfh = 0, gch = 0;
            LVector gfn = {}, gcn = {};
            CollisionSector::GetWorldFloorAndCeilingHeight(gfh, gch, gfn, gcn, p, radius);
            require(gfh == wfh && gch == wch && SameVec(gfn, wfn) && SameVec(gcn, wcn),
                "GetWorldFloorAndCeilingHeight");

            require(RefGetBlockNumber(p) == CollisionSector::GetBlockNumber(p), "GetBlockNumber");
            g_checks += 4;
        }
        g_checks += 4;
    }
}

int main() {
    for (const auto& s : g_collisionSectors) require(s.status == -1, "initial state");

    // Sparse, out-of-order loads leaving gaps between slots.
    const s32 order[] = { 5, 0, 77, 3, 127, 40, 41, 9, 64, 100, 2, 1 };
    s32 block = 0;
    for (s32 slot : order) LoadSlot(slot, block++);
    LoadSlot(64, block++, true);
    CompareQueries(3000);

    // Unload some, reload different data into others (including re-Load
    // without Unload) and fill the gaps.
    g_collisionSectors[77].Unload();
    g_collisionSectors[0].Unload();
    LoadSlot(41, 200);
    LoadSlot(77, 201);
    LoadSlot(50, 202);
    CompareQueries(3000);

    // A slot whose status is set but never loaded, and a loaded slot whose
    // status is cleared behind the sector's back.
    g_collisionSectors[90].status = 999;
    g_collisionSectors[40].status = -1;
    CompareQueries(3000);
    g_collisionSectors[90].Unload();
    g_collisionSectors[40].Unload();

    // Fill every slot (capacity + truncation stress), then drain.
    for (s32 slot = 0; slot < MAX_COLLISION_SECTORS; slot++) LoadSlot(slot, 300 + slot);
    CompareQueries(1500);
    for (s32 slot = 0; slot < MAX_COLLISION_SECTORS; slot += 2) g_collisionSectors[slot].Unload();
    CompareQueries(1500);
    for (auto& s : g_collisionSectors) s.Unload();
    CompareQueries(200);
    for (const auto& s : g_collisionSectors) require(s.status == -1, "sector leak");

    std::printf("PASS: collision query equivalence (%ld comparisons; non-empty wall/floor fills %ld/%ld,"
        " truncated %ld, floors found %ld, ceilings %ld, railings %ld)\n",
        g_checks, g_wallHits, g_floorHits, g_truncated, g_validFloors, g_ceilings, g_railings);
}

// Production Block/CollisionSector/Floor code with rendering excluded.
#include "gen/block.h"
#include "gen/colsect.h"
#include "gen/geometry.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Geometry ownership is irrelevant to these collision queries. Keep the
// rendering dependency small while Block::Parse/LoadPrim run unchanged.
const u8* tGeometry::GetVertexList() const { return vertexList; }
void tGeometry::SetVertexList(const u8* value) { vertexList = value; }
tPrimGeom::~tPrimGeom() { delete[] ownedRawData; }

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "Collision regression: %s\n", message); std::abort(); }
}

static void makeBlock(std::vector<u32>& scratch, s32 x, s32 height) {
    constexpr u32 collisionOffset = 24 + 108;
    scratch.assign((collisionOffset + 48 + 80) / 4, 0);
    scratch[5] = collisionOffset;
    auto* col = scratch.data() + collisionOffset / 4;
    col[3] = x; col[4] = -1000; col[5] = -100;
    col[6] = x + 100; col[7] = 1000; col[8] = 100;
    col[10] = 1;
    auto* floor = col + 12;
    floor[2] = height;
    floor[3] = 1u << 16; // walkable flag in upper halfword
    floor[4] = 65536; floor[6] = -x;
    floor[7] = -65536; floor[9] = x + 100;
    floor[11] = 65536; floor[12] = 100;
    floor[14] = -65536; floor[15] = 100;
    floor[16] = x; floor[17] = -100; floor[18] = x + 100; floor[19] = 100;
}

static s32 floorAt(s32 x) {
    LVector position{}, floorNormal{}, ceilingNormal{};
    position.x = x; position.y = 500;
    s32 floorHeight = 0, ceilingHeight = 0;
    CollisionSector::GetWorldFloorAndCeilingHeight(floorHeight, ceilingHeight,
        floorNormal, ceilingNormal, position, 0);
    return floorHeight;
}

int main() {
    Block a, b;
    a.blockNum = 1; b.blockNum = 2;
    std::vector<u32> scratch;
    makeBlock(scratch, 0, 100);
    a.Parse(scratch.size() * 4, reinterpret_cast<const u8*>(scratch.data()));
    require(floorAt(50) == 100, "initial floor");
    makeBlock(scratch, 1000, 200); // overwrite the SAME streaming buffer
    b.Parse(scratch.size() * 4, reinterpret_cast<const u8*>(scratch.data()));
    scratch.clear(); scratch.shrink_to_fit(); // then release it entirely
    require(floorAt(50) == 100, "first block lost floor after scratch reuse/free");
    require(floorAt(1050) == 200, "second block lost floor after scratch free");
    for (int i = 0; i < 32; ++i) {
        makeBlock(scratch, 0, 150 + i);
        a.Parse(scratch.size() * 4, reinterpret_cast<const u8*>(scratch.data()));
        std::fill(scratch.begin(), scratch.end(), 0);
        require(floorAt(50) == 150 + i, "reparse kept stale floor");
        require(floorAt(1050) == 200, "reparse damaged neighbouring block");
    }
    a.Unload();
    require(floorAt(50) == static_cast<s32>(0x80000001), "unloaded sector still queried");
    b.Destroy(); b.Destroy();
    require(floorAt(1050) == static_cast<s32>(0x80000001), "destroyed sector still queried");
    for (const auto& sector : g_collisionSectors) require(sector.status == -1, "sector leak");
    std::puts("PASS: independent floors after scratch reuse/free, 32 reparses, unload/destroy");
}

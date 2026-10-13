#pragma once
#include "core.h"
#include "gen/manager.h"
#include "gen/block.h"
#include "gen/blockmgr.h"
#include "p3d/stream.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include <string>

class hdMenuItem;
class Thing;
class WDBSwitch;

// PSX player load-group hashes are checkpoint-backed state restored during
// AI::Populate; World later syncs those stored hashes back onto the player slot.
void SeedPlayerLoadGroups(u32 primaryHash, u32 secondaryHash);

// PSX globals used by front-end destination selection return flow.
extern LVector g_destSelectReturnPos;
extern bool g_destSelectReturnPosValid;
extern u8 g_arrowInside;

// PSX VRAM simulation (1024x512 16-bit words), heap-allocated
struct PsxVRAM {
    u16* data; // [y * 1024 + x], 1024x512

    PsxVRAM() : data(new u16[1024 * 512]()) {}
    ~PsxVRAM() { delete[] data; }
    PsxVRAM(const PsxVRAM&) = delete;
    PsxVRAM& operator=(const PsxVRAM&) = delete;

    u16  Get(int x, int y) const { return data[y * 1024 + x]; }
    void Set(int x, int y, u16 v) { data[y * 1024 + x] = v; }

    void Clear() {
        memset(data, 0, 1024 * 512 * sizeof(u16));
        MarkDirty(0, 0, 1024, 512);
    }

    void Upload(s16 x, s16 y, s16 w, s16 h, const u8* raw) {
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                int vx = x + col, vy = y + row;
                if (vx >= 0 && vx < 1024 && vy >= 0 && vy < 512) {
                    int idx = (row * w + col) * 2;
                    Set(vx, vy, static_cast<u16>(raw[idx] | (raw[idx + 1] << 8)));
                }
            }
        }
        MarkDirty(x, y, w, h);
    }

    bool ConsumeDirtyRect(s16& outX, s16& outY, s16& outW, s16& outH) {
        if (!dirty) {
            return false;
        }

        outX = dirtyMinX;
        outY = dirtyMinY;
        outW = static_cast<s16>(dirtyMaxX - dirtyMinX);
        outH = static_cast<s16>(dirtyMaxY - dirtyMinY);
        dirty = false;
        dirtyMinX = 1024;
        dirtyMinY = 512;
        dirtyMaxX = 0;
        dirtyMaxY = 0;
        return outW > 0 && outH > 0;
    }

    bool IsDirty() const { return dirty; }

    void ClearDirty() {
        dirty = false;
        dirtyMinX = 1024;
        dirtyMinY = 512;
        dirtyMaxX = 0;
        dirtyMaxY = 0;
    }

    // Decode a 256x256 texture page to RGBA8 (out must be 256*256*4 bytes)
    void DecodePage(u16 tpage, u16 cba, u8* rgbaOut) const;

private:
    void MarkDirty(s16 x, s16 y, s16 w, s16 h) {
        if (w <= 0 || h <= 0) {
            return;
        }

        const s32 minX = std::max<s32>(0, x);
        const s32 minY = std::max<s32>(0, y);
        const s32 maxX = std::min<s32>(1024, static_cast<s32>(x) + w);
        const s32 maxY = std::min<s32>(512, static_cast<s32>(y) + h);
        if (minX >= maxX || minY >= maxY) {
            return;
        }

        if (!dirty) {
            dirtyMinX = static_cast<s16>(minX);
            dirtyMinY = static_cast<s16>(minY);
            dirtyMaxX = static_cast<s16>(maxX);
            dirtyMaxY = static_cast<s16>(maxY);
            dirty = true;
            return;
        }

        dirtyMinX = static_cast<s16>(std::min<s32>(dirtyMinX, minX));
        dirtyMinY = static_cast<s16>(std::min<s32>(dirtyMinY, minY));
        dirtyMaxX = static_cast<s16>(std::max<s32>(dirtyMaxX, maxX));
        dirtyMaxY = static_cast<s16>(std::max<s32>(dirtyMaxY, maxY));
    }

    bool dirty = true;
    s16 dirtyMinX = 0;
    s16 dirtyMinY = 0;
    s16 dirtyMaxX = 1024;
    s16 dirtyMaxY = 512;
};

class World : public Manager {
public:
    World();
    ~World() override;

    bool Load(const std::string& lcfPath);
    bool LoadLevelIndex(u32 levelIndex);
    void LoadPetal(u32 petalIndex);
    void LoadLevelNames();
    void LoadPermanent();
    void ProcessSwitches();
    void Render(const LVector* playerPos);
    void Unload();
    void UnloadLevelPart2();
    void UnloadPermanent();
    void UnloadPetal();
    void ResetLevel();
    void CheckThingSwitches(Thing* thing);
    WDBSwitch* FindPrimarySwitchByCRC(u32 crc);
    void ProcessPendingSwitchActions();
    void RequestPlayerResetOnLoad() { pendingPlayerReset = true; }
    bool ConsumePlayerResetOnLoad() {
        const bool shouldReset = pendingPlayerReset;
        pendingPlayerReset = false;
        return shouldReset;
    }

    u32 GetCurrentLevelIndex() const { return currentLevelIndex; }
    u32 GetTargetLevelIndex() const { return targetLevelIndex; }
    u32 GetCurrentPetalIndex() const { return currentPetalIndex; }
    u32 GetTargetPetalIndex() const { return targetPetalIndex; }

    void SetTargetLevelIndex(u32 levelIndex) { targetLevelIndex = levelIndex; }
    void SetTargetPetalIndex(u32 petalIndex) { targetPetalIndex = petalIndex; }
    void SetTargetLevelPetal(u32 levelIndex, u32 petalIndex) {
        targetLevelIndex = levelIndex;
        targetPetalIndex = petalIndex;
    }

    u32 GetBlockCount() const { return g_blockManager ? g_blockManager->GetNumBlocks() : 0; }
    BlockManager* GetBlockManager() { return g_blockManager; }
    const LVector& GetLevelMin() const { return levelMin; }
    const LVector& GetLevelMax() const { return levelMax; }

    // Upload raw PSX texture data to VRAM and refresh the GL texture
    void UploadToVRAM(s16 x, s16 y, s16 w, s16 h, const u8* raw);
    void RefreshVRAMTexture();
    // Palette cycling and RAM-texture flipbooks write CPU VRAM many times per
    // frame. The writes are coalesced into one upload at the top of
    // World::Render, bounded to the rectangle they actually touched: a palette
    // transfer dirties 256x1 of a 1024x512 surface.
    void MarkVRAMDirtyRect(s32 x, s32 y, s32 w, s32 h);
    void FlushDirtyVRAM();
    u32 GetVRAMHandle() const { return vramHandle; }
    const PsxVRAM& GetVRAM() const { return vram; }

    // PSX: PackLevelName (WORLD.CPP:757, 0x8004521C)
    static u32 PackLevelName(u32 levelIndex, u32 petalIndex) {
        return (levelIndex << 16) | (petalIndex + 1);
    }

    // PSX: UnpackLevelName (WORLD.CPP:763, 0x8004522C)
    static void UnpackLevelName(u32 packed, u32& outLevel, u32& outPetal) {
        outLevel = packed >> 16;
        outPetal = (packed & 0xFFFF) - 1;
    }

    // PSX: LevelMenuExecute (WORLD.CPP:868, 0x80045634)
    static s32 LevelMenuExecute(hdMenuItem* item);

    // Converts a level ID (e.g. 7=hub) to its index in the level list.
    s32 LevelIDToIndex(s32 levelID) const {
        for (s32 i = 0; i < levelCount; i++) {
            if (levelList && levelList[i * 2] == levelID)
                return i;
        }
        return 0;
    }

    // Returns the number of petals for the current level.
    s32 GetCurLevelPetals() const {
        if (levelList && currentLevelIndex < (u32)levelCount)
            return levelList[currentLevelIndex * 2 + 1];
        return 1;
    }

    // Returns the level ID for the current level index.
    s32 GetCurLevelID() const {
        if (levelList && currentLevelIndex < (u32)levelCount)
            return levelList[currentLevelIndex * 2];
        return 0;
    }

    bool IsCurrentLevelHub() const {
        return GetCurLevelID() == 7;
    }

    // Returns the level ID for an arbitrary level index.
    s32 GetLevelIDFromIndex(u32 levelIndex) const {
        if (levelList && levelIndex < (u32)levelCount)
            return levelList[levelIndex * 2];
        return 0;
    }

    // Returns the petal count for an arbitrary level index.
    s32 GetLevelPetalCountFromIndex(u32 levelIndex) const {
        if (levelList && levelIndex < (u32)levelCount)
            return levelList[levelIndex * 2 + 1];
        return 1;
    }

    // Returns the level name for an arbitrary level index.
    const char* GetLevelNameFromIndex(u32 levelIndex) const {
        if (levelNames && levelIndex < (u32)levelCount)
            return levelNames[levelIndex];
        return nullptr;
    }

    // Returns the petal/destination name for a given level index and petal index.
    const char* GetPetalNameFromIndex(u32 levelIndex, u32 petalIndex) const {
        if (!petalNames || levelIndex >= (u32)levelCount)
            return nullptr;
        if (!petalNames[levelIndex])
            return nullptr;

        const u32 petalCount = (u32)GetLevelPetalCountFromIndex(levelIndex);
        if (petalIndex >= petalCount)
            return nullptr;

        return petalNames[levelIndex][petalIndex];
    }

private:
    PsxVRAM vram;
    u32 vramHandle = 0;
    // Inclusive min, exclusive max. Empty when minX >= maxX.
    s32 vramDirtyMinX = 0;
    s32 vramDirtyMinY = 0;
    s32 vramDirtyMaxX = 0;
    s32 vramDirtyMaxY = 0;
    std::vector<u8> streamData; // LCF file data (kept alive for block pointers)
#if defined(RC_PLATFORM_PSP)
    // PSP streams stay on the Memory Stick; only the parsed directory and
    // bounded entry scratch buffers remain resident in user RAM.
    std::string streamPath;
    std::vector<tStreamEntry> streamEntries;
    std::vector<u8> pspStreamScratch;
    std::vector<u8> pspPermScratch;
    std::vector<u8> pspP3dScratch;
#endif
    LVector levelMin = {}, levelMax = {};

    // Level table data (from RTARGET/GAME_LN.TXT via LoadLevelNames)
    // PSX offsets: +0x24 through +0x38
    s32* levelList = nullptr;        // [levelCount * 2]: pairs of {levelID, petalCount}
    char** levelNames = nullptr;     // [levelCount + 1]: level name strings
    char*** petalNames = nullptr;    // [levelCount + 1]: per-level petal name arrays
    u8** petalSoundIDs = nullptr;    // [levelCount]: per-level sound byte arrays
    s32* highestPetal = nullptr;     // [levelCount]: highest petal index per level
    s32 levelCount = 0;

    // PSX world progression fields (offsets +0x3C..+0x4C in original layout)
    u32 currentLevelIndex = 0xFFFFFFFF;
    u32 targetLevelIndex = 6;
    u32 currentPetalIndex = 0;
    u32 targetPetalIndex = 0;
    u32 previousLevelIndex = 0xFFFFFFFF;
    bool pendingPlayerReset = false;
    ccList switchLists[4];

    void LoadTPGTextures(const u8* lcfData, u32 lcfSize);
    void LoadTPGTexturePayload(const u8* payload, u32 payloadSize);
#if defined(RC_PLATFORM_PSP)
    bool LoadPspStream(const std::string& lcfPath);
    void LoadPspPetal(u32 petalIndex);
#endif
    void PurgeSwitches();
    void CheckSwitchList(ccList& list, Thing* thing);

    // DrawEverythingHandler (GAME.CPP:2211) - sorting + rendering pipeline
    void DrawEverythingHandler(const LVector* playerPos);  // 0x8002A98C

    // computeBlockToPointDistances (GAME.CPP:1976) - 8-corner bbox distance + frustum cull
    void computeBlockToPointDistances(const Block* block, const LVector* playerPos,
                                      s32* outDistSq, s32* outZDepth);  // 0x8002A238

    // OffsetToPreventSeams (GAME.CPP:2482) - shifts block pos toward player
    void OffsetToPreventSeams(LVector& pos, const LVector& playerPos);  // 0x8002AF88
};

// PSX: DeletePlayerBlendAndAnimData (WORLD.CPP:2059, 0x80047014)
s32 DeletePlayerBlendAndAnimData();

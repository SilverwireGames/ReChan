#include "gen/uvdata.h"
#include "gen/geometry.h"
#include "p3d/byteread.h"
#include "pc/log.h"
#include "p3d/p3dmath.h"
#include <cstring>

UVPrimData g_UVPrimDataArray[UV_PRIM_MAX];
u32 g_UVPrimDataCount = 0;
CBVPrimData g_CBVPrimDataArray[CBV_PRIM_MAX];
u32 g_CBVPrimDataCount = 0;

static void LogInvalidPrimWindow(const char* tag, int blockNum, const tPrimGeom* geom, u32 primListBaseOff) {
    LOG("[%s] Invalid primList window: block=%d primListBaseOff=%u ownedRawSize=%u geoType=%u numLoops=%u primList=%p raw=%p",
        tag,
        blockNum,
        primListBaseOff,
        geom ? geom->ownedRawSize : 0,
        geom ? geom->geoType : 0,
        geom ? geom->numLoops : 0,
        geom ? geom->primList : nullptr,
        geom ? geom->ownedRawData : nullptr);
}

static void LogInvalidPrimOffset(const char* tag, int blockNum, u32 hash, u32 entryIdx, u32 wordIdx,
                                 u32 primOffset, u32 primListBytes, const tPrimGeom* geom) {
    LOG("[%s] Invalid prim offset: block=%d hash=0x%08X entry=%u word=%u primOffset=%u primListBytes=%u ownedRawSize=%u geoType=%u numLoops=%u primList=%p raw=%p",
        tag,
        blockNum,
        hash,
        entryIdx,
        wordIdx,
        primOffset,
        primListBytes,
        geom ? geom->ownedRawSize : 0,
        geom ? geom->geoType : 0,
        geom ? geom->numLoops : 0,
        geom ? geom->primList : nullptr,
        geom ? geom->ownedRawData : nullptr);
}

static void LogUVPrimEntrySample(const UVPrimData& data, u32 permOffset) {
    if (!data.entryRaw || data.numEntries == 0) {
        return;
    }

    const u8* entry = data.entryRaw;
    LOG("[UVPrimData] Sample 0x8C21: hash=0x%08X block=%u permOffset=%u offsets=[%u,%u,%u,%u] baseUV=[0x%04X,0x%04X,0x%04X,0x%04X]",
        data.hash,
        data.blockNum,
        permOffset,
        p3dReadU32LE(entry + 0),
        p3dReadU32LE(entry + 4),
        p3dReadU32LE(entry + 8),
        p3dReadU32LE(entry + 12),
        p3dReadU16LE(entry + 16),
        p3dReadU16LE(entry + 20),
        p3dReadU16LE(entry + 24),
        p3dReadU16LE(entry + 28));
}

// FindUVPrimInfo__10UVPrimDataUl (UVDATA.CPP:129)
UVPrimData* FindUVPrimInfo(u32 hash) {
    MARKFUNCTION(0x8009842C);

    for (u32 index = 0; index < g_UVPrimDataCount; index++) {
        UVPrimData& data = g_UVPrimDataArray[index];
        if (data.hash == hash) {
            return &data;
        }
    }

    return nullptr;
}

// PSX: Init__10UVPrimDataiiii (UVDATA.CPP:251)
void UVPrimData::Init(s32 newUMask, s32 newVMask, s32 newUStep, s32 newVStep) {
    MARKFUNCTION(0x800985D8);

    uMask = (u32)newUMask;
    vMask = (u32)newVMask;
    uStep = (u16)newUStep;
    uAccum = 0;
    vAccum = 0;
    valid = 1;
    // PSX stores the negated 4th argument into vStep.
    vStep = (u16)(-newVStep);
}

bool UpdateUVPrimData(int blockNum, tPrimGeom* geom) {
    MARKFUNCTION(0x800984E4);

    if (!geom || !geom->ownedRawData || !geom->primList) {
        return false;
    }

    const u32 primListBaseOff = static_cast<u32>(geom->primList - geom->ownedRawData);
    if (primListBaseOff >= geom->ownedRawSize) {
        LogInvalidPrimWindow("UVPrimData", blockNum, geom, primListBaseOff);
        ASSERT(false);
        return false;
    }

    const u32 primListBytes = geom->ownedRawSize - primListBaseOff;
    bool updated = false;
    u8* primList = const_cast<u8*>(geom->primList);

    for (u32 dataIdx = 0; dataIdx < g_UVPrimDataCount; dataIdx++) {
        const UVPrimData& uvData = g_UVPrimDataArray[dataIdx];
        if (!uvData.valid || uvData.blockNum != static_cast<u32>(blockNum) || !uvData.entryRaw) {
            continue;
        }

        for (u32 entryIdx = 0; entryIdx < uvData.numEntries; entryIdx++) {
            const u8* entry = uvData.entryRaw + entryIdx * 32;
            for (u32 wordIdx = 0; wordIdx < 4; wordIdx++) {
                const u32 primOffset = p3dReadU32LE(entry + wordIdx * 4);
                if (primOffset + 2 > primListBytes) {
                    LogInvalidPrimOffset("UVPrimData", blockNum, uvData.hash, entryIdx, wordIdx, primOffset, primListBytes, geom);
                    ASSERT(false);
                    continue;
                }
                const u16 uvWord = static_cast<u16>(p3dReadU16LE(entry + 16 + wordIdx * 4) + uvData.uvCombined);
                const u8 nextU = static_cast<u8>(uvWord & 0xFF);
                const u8 nextV = static_cast<u8>((uvWord >> 8) & 0xFF);
                if (primList[primOffset + 0] != nextU) {
                    primList[primOffset + 0] = nextU;
                    updated = true;
                }
                if (primList[primOffset + 1] != nextV) {
                    primList[primOffset + 1] = nextV;
                    updated = true;
                }
            }
        }
    }

    return updated;
}

CBVPrimData::CBVPrimData()
    : valid(0), hash(0), blockNum(0), numEntries(0), entryRaw(nullptr), mode(0),
      randomFrames(0), frameCount(0), frameCountMax(0), frameCountMin(0), currentIndex(0),
      fixedColour(0), colourInfo(nullptr), restartDelay(0), holdDelay(0),
      restartDelayCurrent(0), holdDelayCurrent(0), direction(0) {
    MARKFUNCTION(0x8009890C);
}

int ColourInfo::Init(u32 startColour, u32 endColour, int steps) {
    MARKFUNCTION(0x80098F00);

    ASSERT(steps != 0);

    startR = static_cast<u16>((startColour & 0xFF) << 8);
    startG = static_cast<u16>(((startColour >> 8) & 0xFF) << 8);
    startB = static_cast<u16>(((startColour >> 16) & 0xFF) << 8);
    endR = static_cast<u16>((endColour & 0xFF) << 8);
    endG = static_cast<u16>(((endColour >> 8) & 0xFF) << 8);
    endB = static_cast<u16>(((endColour >> 16) & 0xFF) << 8);

    stepR = static_cast<u16>((static_cast<s32>(endR) - static_cast<s32>(startR)) / steps);
    stepG = static_cast<u16>((static_cast<s32>(endG) - static_cast<s32>(startG)) / steps);
    stepB = static_cast<u16>((static_cast<s32>(endB) - static_cast<s32>(startB)) / steps);

    currentR = startR;
    currentG = startG;
    currentB = startB;
    reverse = 0;
    return startR;
}

int ColourInfo::Reset(int useEndColour) {
    MARKFUNCTION(0x80099044);

    reverse = static_cast<u32>(useEndColour);
    if (useEndColour) {
        currentR = endR;
        currentG = endG;
        currentB = endB;
    }
    else {
        currentR = startR;
        currentG = startG;
        currentB = startB;
    }
    return currentR;
}

int ColourInfo::Update() {
    MARKFUNCTION(0x80099080);

    const s16 deltaR = static_cast<s16>(stepR);
    const s16 deltaG = static_cast<s16>(stepG);
    const s16 deltaB = static_cast<s16>(stepB);

    if (reverse) {
        currentR = static_cast<u16>(currentR - deltaR);
        currentG = static_cast<u16>(currentG - deltaG);
        currentB = static_cast<u16>(currentB - deltaB);
    }
    else {
        currentR = static_cast<u16>(currentR + deltaR);
        currentG = static_cast<u16>(currentG + deltaG);
        currentB = static_cast<u16>(currentB + deltaB);
    }

    return currentR;
}

u32 ColourInfo::GetColour() const {
    MARKFUNCTION(0x800990F4);
    return ((currentR >> 8) & 0xFF) | (currentG & 0xFF00) | ((static_cast<u32>(currentB >> 8) & 0xFF) << 16);
}

int CBVPrimData::Init(int newFrameCount, int newFrameCountMin, u32 newFixedColour,
                      int newRestartDelay, int newHoldDelay, int newMode) {
    MARKFUNCTION(0x80098960);

    frameCount = newFrameCount;
    frameCountMax = newFrameCount;
    randomFrames = 0;
    if (newFrameCountMin) {
        randomFrames = 1;
        frameCountMin = newFrameCountMin;
    }

    restartDelay = newRestartDelay;
    restartDelayCurrent = newRestartDelay;
    holdDelay = newHoldDelay;
    holdDelayCurrent = newHoldDelay;
    fixedColour = newFixedColour;

    if (newMode <= 0) {
        currentIndex = newFrameCount - 1;
        direction = -1;
        mode = 2;
        if (newMode == -2) {
            mode = 4;
        }
    }
    else {
        direction = 1;
        mode = 1;
        currentIndex = 0;
        if (newMode == 2) {
            mode = 3;
        }
    }

    if (!colourInfo) {
        colourInfo = new ColourInfo*[numEntries];
        for (u32 entryIdx = 0; entryIdx < numEntries; entryIdx++) {
            colourInfo[entryIdx] = nullptr;
        }
    }

    for (u32 entryIdx = 0; entryIdx < numEntries; entryIdx++) {
        if (!colourInfo[entryIdx]) {
            colourInfo[entryIdx] = new ColourInfo();
        }

        const u32 entryColour = p3dReadU32LE(entryRaw + entryIdx * 8 + 4);
        if (currentIndex != 0) {
            colourInfo[entryIdx]->Init(newFixedColour, entryColour, newFrameCount);
        }
        else {
            colourInfo[entryIdx]->Init(entryColour, newFixedColour, newFrameCount);
        }
    }

    valid = 1;
    return 1;
}

void CBVPrimData::Release() {
    MARKFUNCTION(0x80098B2C);
    FreeColourInfo();
}

void CBVPrimData::FreeColourInfo() {
    MARKFUNCTION(0x80098B4C);

    if (!colourInfo) {
        return;
    }

    for (u32 entryIdx = 0; entryIdx < numEntries; entryIdx++) {
        delete colourInfo[entryIdx];
        colourInfo[entryIdx] = nullptr;
    }

    delete[] colourInfo;
    colourInfo = nullptr;
}

int CBVPrimData::Update() {
    MARKFUNCTION(0x80098BE0);

    if (!colourInfo) {
        return 0;
    }

    const s32 oldRestartDelayCurrent = restartDelayCurrent;
    restartDelayCurrent = oldRestartDelayCurrent - 1;
    if (oldRestartDelayCurrent > 0) {
        return restartDelayCurrent;
    }

    s32 resetColours = 0;
    s32 resetToEnd = 0;
    s32 updateColours = 1;

    currentIndex += direction;
    if (mode == 2) {
        if (currentIndex < 0) {
            const s32 oldHoldDelayCurrent = holdDelayCurrent;
            holdDelayCurrent = oldHoldDelayCurrent - 1;
            if (oldHoldDelayCurrent <= 0) {
                resetColours = 1;
                currentIndex = frameCount - 1;
            }
            else {
                updateColours = 0;
            }
        }
    }
    else if (mode == 1) {
        if (currentIndex > frameCount - 1) {
            const s32 oldHoldDelayCurrent = holdDelayCurrent;
            holdDelayCurrent = oldHoldDelayCurrent - 1;
            if (oldHoldDelayCurrent <= 0) {
                currentIndex = 0;
                resetColours = 1;
            }
            else {
                updateColours = 0;
            }
        }
    }
    else if (mode == 3) {
        if (currentIndex < 0 || currentIndex > frameCount - 1) {
            const s32 oldHoldDelayCurrent = holdDelayCurrent;
            holdDelayCurrent = oldHoldDelayCurrent - 1;
            if (oldHoldDelayCurrent <= 0) {
                resetColours = 1;
                resetToEnd = (direction > 0) ? 1 : 0;
                direction = -direction;
            }
            else {
                updateColours = 0;
            }
        }
    }
    else if (mode == 4) {
        if (currentIndex < 0 || currentIndex > frameCount - 1) {
            const s32 oldHoldDelayCurrent = holdDelayCurrent;
            holdDelayCurrent = oldHoldDelayCurrent - 1;
            if (oldHoldDelayCurrent <= 0) {
                resetColours = 1;
                resetToEnd = (direction < 0) ? 1 : 0;
                direction = -direction;
            }
            else {
                updateColours = 0;
            }
        }
    }

    if (resetColours) {
        restartDelayCurrent = restartDelay;
        holdDelayCurrent = holdDelay;

        if (randomFrames) {
            frameCount = frameCountMin + static_cast<s32>(rmRangedRandom(static_cast<u32>(frameCountMax - frameCountMin + 1)));
            for (u32 entryIdx = 0; entryIdx < numEntries; entryIdx++) {
                const u32 entryColour = p3dReadU32LE(entryRaw + entryIdx * 8 + 4);
                if (mode == 3 || mode == 1) {
                    colourInfo[entryIdx]->Init(fixedColour, entryColour, frameCount);
                }
                else {
                    colourInfo[entryIdx]->Init(entryColour, fixedColour, frameCount);
                }
                colourInfo[entryIdx]->Reset(resetToEnd);
            }
        }
        else {
            for (u32 entryIdx = 0; entryIdx < numEntries; entryIdx++) {
                colourInfo[entryIdx]->Reset(resetToEnd);
            }
        }
    }
    else if (updateColours) {
        for (u32 entryIdx = 0; entryIdx < numEntries; entryIdx++) {
            colourInfo[entryIdx]->Update();
        }
    }

    return 1;
}

// FindCBVPrimInfo__11CBVPrimDataUl (UVDATA.CPP:387)
CBVPrimData* FindCBVPrimInfo(u32 hash) {
    MARKFUNCTION(0x80098744);

    for (u32 index = 0; index < g_CBVPrimDataCount; index++) {
        CBVPrimData& data = g_CBVPrimDataArray[index];
        if (data.hash == hash) {
            return &data;
        }
    }

    return nullptr;
}

bool UpdateCBVPrimData(int blockNum, tPrimGeom* geom) {
    MARKFUNCTION(0x800987FC);

    if (!geom || !geom->ownedRawData || !geom->primList) {
        return false;
    }

    const u32 primListBaseOff = static_cast<u32>(geom->primList - geom->ownedRawData);
    if (primListBaseOff >= geom->ownedRawSize) {
        LogInvalidPrimWindow("CBVPrimData", blockNum, geom, primListBaseOff);
        ASSERT(false);
        return false;
    }

    const u32 primListBytes = geom->ownedRawSize - primListBaseOff;
    bool updated = false;
    u8* primList = const_cast<u8*>(geom->primList);

    for (u32 dataIdx = 0; dataIdx < g_CBVPrimDataCount; dataIdx++) {
        CBVPrimData& cbvData = g_CBVPrimDataArray[dataIdx];
        if (!cbvData.valid || cbvData.blockNum != static_cast<u32>(blockNum) || !cbvData.colourInfo) {
            continue;
        }

        for (u32 entryIdx = 0; entryIdx < cbvData.numEntries; entryIdx++) {
            const u32 primOffset = p3dReadU32LE(cbvData.entryRaw + entryIdx * 8);
            if (primOffset + 4 > primListBytes) {
                LogInvalidPrimOffset("CBVPrimData", blockNum, cbvData.hash, entryIdx, 0, primOffset, primListBytes, geom);
                ASSERT(false);
                continue;
            }
            u32* primWord = reinterpret_cast<u32*>(primList + primOffset);
            const u32 nextWord = (*primWord & 0xFF000000u)
                | cbvData.colourInfo[entryIdx]->GetColour();
            if (*primWord != nextWord) {
                *primWord = nextWord;
                updated = true;
            }
        }
    }

    return updated;
}

// PSX: Load__10UVPrimDataR10tReadChunkPPv (UVDATA.CPP:81, 0x80098334)
void LoadUVPrimData(u16 chunkId, const u8* body, u32 bodySize,
                    const u8* permBase, u32& permCursor, u32 permSize)
{
    MARKFUNCTION(0x80098334);

    if (chunkId == 0x8C20) {
        if (g_UVPrimDataCount >= UV_PRIM_MAX) return;
        if (bodySize < 12) return;

        UVPrimData& d = g_UVPrimDataArray[g_UVPrimDataCount];
        memset(&d, 0, sizeof(d));
        d.hash = p3dReadU32LE(body + 0);
        d.blockNum = p3dReadU32LE(body + 4);
        d.numEntries = p3dReadU32LE(body + 8);
        // PSX: UVPrimData::Init / FindUVPrimInfo supply masks + steps. Some builds
        // embed them after the first 12 bytes in the 0x8C20 chunk body.
        if (bodySize >= 24) {
            d.uMask = p3dReadU32LE(body + 12);
            d.vMask = p3dReadU32LE(body + 16);
        }
        if (bodySize >= 28) {
            d.uStep = static_cast<u16>(p3dReadU16LE(body + 20));
            d.vStep = static_cast<u16>(p3dReadU16LE(body + 22));
        }
        g_UVPrimDataCount++;

        LOG("[UVPrimData] Load 0x8C20: idx=%u hash=0x%08X blockNum=%u numEntries=%u "
            "uMask=0x%X vMask=0x%X uStep=%u vStep=%u",
            g_UVPrimDataCount - 1, d.hash, d.blockNum, d.numEntries,
            d.uMask, d.vMask, d.uStep, d.vStep);
    }
    else if (chunkId == 0x8C21) {
        if (g_UVPrimDataCount == 0) return;
        UVPrimData& d = g_UVPrimDataArray[g_UVPrimDataCount - 1];
        const u32 entryPermOffset = permCursor;

        if (permCursor + d.numEntries * 32 > permSize) {
            LOG("[UVPrimData] 0x8C21 perm overflow: need %u, have %u",
                permCursor + d.numEntries * 32, permSize);
            return;
        }

        d.entryRaw = permBase + permCursor;
        permCursor += d.numEntries * 32;
        d.valid = 1;

        // PSX: UVPrimData::FindUVPrimInfo(hash) + Init(...) supply masks/steps when
        // not present in the 0x8C20 chunk body — table lives in the executable on PSX.
        if (d.uMask == 0 && d.vMask == 0 && d.uStep == 0 && d.vStep == 0) {
            LOG("[UVPrimData] WARN: hash=0x%08X has no masks/steps (need FindUVPrimInfo from ROM)",
                d.hash);
        }

        if (d.hash == 0x0C0DF312 || d.hash == 0x0C3DF312) {
            LogUVPrimEntrySample(d, entryPermOffset);
        }

        LOG("[UVPrimData] Load 0x8C21: entries=%u entryRaw=%p valid=1", d.numEntries, d.entryRaw);
    }
}

// PSX: Load__11CBVPrimDataR10tReadChunkPPv (UVDATA.CPP:339, 0x8009864C)
void LoadCBVPrimData(u16 chunkId, const u8* body, u32 bodySize,
                     const u8* permBase, u32& permCursor, u32 permSize) {
    MARKFUNCTION(0x8009864C);

    if (chunkId == 0x8C30) {
        if (g_CBVPrimDataCount >= CBV_PRIM_MAX) return;
        if (bodySize < 12) return;

        CBVPrimData& d = g_CBVPrimDataArray[g_CBVPrimDataCount];
        d.Release();
        d = CBVPrimData();
        d.hash = p3dReadU32LE(body + 0);
        d.blockNum = p3dReadU32LE(body + 4);
        d.numEntries = p3dReadU32LE(body + 8);
        g_CBVPrimDataCount++;

        LOG("[CBVPrimData] Load 0x8C30: idx=%u hash=0x%08X blockNum=%u numEntries=%u",
            g_CBVPrimDataCount - 1, d.hash, d.blockNum, d.numEntries);
    }
    else if (chunkId == 0x8C31) {
        if (g_CBVPrimDataCount == 0) return;
        CBVPrimData& d = g_CBVPrimDataArray[g_CBVPrimDataCount - 1];

        if (permCursor + d.numEntries * 8 > permSize) {
            LOG("[CBVPrimData] 0x8C31 perm overflow");
            return;
        }

        d.entryRaw = permBase + permCursor;
        permCursor += d.numEntries * 8;
        d.valid = 1;

        LOG("[CBVPrimData] Load 0x8C31: entries=%u valid=1", d.numEntries);
    }
}

// PSX: Unload__10UVPrimData (UVDATA.CPP:154, 0x80098478)
void UnloadUVPrimData() {
    MARKFUNCTION(0x80098478);
    for (u32 i = 0; i < g_UVPrimDataCount; i++) {
        memset(&g_UVPrimDataArray[i], 0, sizeof(UVPrimData));
    }
    g_UVPrimDataCount = 0;
}

// PSX: Unload__11CBVPrimData (UVDATA.CPP:412, 0x80098790)
void UnloadCBVPrimData() {
    MARKFUNCTION(0x80098790);
    for (u32 i = 0; i < g_CBVPrimDataCount; i++) {
        g_CBVPrimDataArray[i].Release();
        g_CBVPrimDataArray[i] = CBVPrimData();
    }
    g_CBVPrimDataCount = 0;
}

// Tick all UV accumulators once per frame
void TickAllUVPrimData() {
    for (u32 i = 0; i < g_UVPrimDataCount; i++) {
        if (g_UVPrimDataArray[i].valid) {
            g_UVPrimDataArray[i].Tick();
        }
    }
}

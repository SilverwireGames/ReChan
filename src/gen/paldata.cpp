#include <cstring>
#include "common.h"

#include "gen/paldata.h"

#include "gen/weffect.h"

#include "p3d/texture.h"
#include "pc/log.h"

static PaletteData* g_paletteDataArray[4] = {};
static s32 g_paletteDataCount = 0;

static u16 ReadU16Pal(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

static u32 ReadU32Pal(const u8* p) {
    return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static bool IsValidPalettePointer(const void* pointer) {
    if (!pointer) {
        return false;
    }

#if defined(RC_PLATFORM_PSP)
    // Palette streams and the clone upload buffer must live in user RAM. A
    // stale stream from the previous level can otherwise look non-null and
    // make the MIPS core read a KSEG/garbage address during RLE expansion.
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    return (address & 1u) == 0u
        && address >= 0x08000000u
        && address < 0x0C000000u;
#else
    return true;
#endif
}

PaletteData::PaletteData() {
    MARKFUNCTION(0x8009CE34);
}

PaletteData::~PaletteData() {
    MARKFUNCTION(0x8009CE44);

    if (writeCursorEnd) {
        u16* base = writeCursorEnd - 256;
        delete[] base;
    }
#if defined(RC_PLATFORM_PSP)
    delete[] ownedStream;
    ownedStream = nullptr;
    ownedStreamSize = 0;
#endif

    writeCursor = nullptr;
    writeCursorEnd = nullptr;
}

bool LoadPaletteData(u16 chunkId, const u8* body, u32 bodySize,
                     const u8* permBase, u32& permCursor, u32 permSize) {
    MARKFUNCTION(0x8009CEA4);

    if (g_paletteDataCount >= 4) {
        return true;
    }

    if (chunkId == 0x8C00) {
        if (!body || bodySize < 10) {
            LOG("[PaletteData] 0x8C00 short chunk: bodySize=%u", bodySize);
            return false;
        }

        PaletteData* entry = new PaletteData();
        if (!entry) {
            return false;
        }

        entry->hash = ReadU32Pal(body + 0);
        entry->frameCount = static_cast<u32>(ReadU16Pal(body + 4));
        entry->permOffset = ReadU32Pal(body + 6);
        entry->frameOffset = 0;
        entry->frameIndex = 0;
        entry->writeCursor = nullptr;
        entry->frameStreamSize = entry->permOffset;
        entry->paletteLutCount = 0;

        g_paletteDataArray[g_paletteDataCount++] = entry;
        return true;
    }

    if (chunkId == 0x8C01) {
        if (g_paletteDataCount <= 0) {
            return true;
        }

        PaletteData* entry = g_paletteDataArray[g_paletteDataCount - 1];
        if (!entry || !permBase) {
            return false;
        }

        if (permCursor > permSize || entry->permOffset > permSize || permCursor + entry->permOffset > permSize) {
            LOG("[PaletteData] 0x8C01 perm overflow: cursor=%u permOffset=%u permSize=%u",
                permCursor,
                entry->permOffset,
                permSize);
            return false;
        }

        entry->frameStream = permBase + permCursor;

        u32 lutCursor = permCursor + entry->permOffset;
        if (lutCursor + 2 > permSize) {
            LOG("[PaletteData] 0x8C01 short LUT length read at %u (size=%u)", lutCursor, permSize);
            return false;
        }

        const u16 lutCount = ReadU16Pal(permBase + lutCursor);
        lutCursor += 2;

        const u32 lutBytes = static_cast<u32>(lutCount) * 2u;
        if (lutCursor + lutBytes > permSize) {
            LOG("[PaletteData] 0x8C01 LUT overflow: cursor=%u bytes=%u permSize=%u",
                lutCursor,
                lutBytes,
                permSize);
            return false;
        }

        entry->paletteLut = reinterpret_cast<const u16*>(permBase + lutCursor);
        entry->paletteLutCount = lutCount;
#if defined(RC_PLATFORM_PSP)
        {
            // Own the frame stream + LUT: World::pspPermScratch is freed after the
            // level loads, and Update() would otherwise read recycled heap memory.
            const u32 ownedSize = entry->permOffset + 2u + lutBytes;
            u8* owned = new u8[ownedSize];
            std::memcpy(owned, permBase + permCursor, ownedSize);
            delete[] entry->ownedStream;
            entry->ownedStream = owned;
            entry->ownedStreamSize = ownedSize;
            entry->frameStream = owned;
            entry->paletteLut = reinterpret_cast<const u16*>(owned + entry->permOffset + 2u);
        }
#endif
        lutCursor += lutBytes;

        const u8 lowPermOffset = static_cast<u8>(entry->permOffset);
        const u8 lowLutBytes = static_cast<u8>(lutBytes);
        const u32 alignPad = static_cast<u32>((lowPermOffset + lowLutBytes + 2u) & 3u);

        if (lutCursor + alignPad > permSize) {
            LOG("[PaletteData] 0x8C01 align overflow: cursor=%u pad=%u permSize=%u",
                lutCursor,
                alignPad,
                permSize);
            return false;
        }

        permCursor = lutCursor + alignPad;
        return true;
    }

    return false;
}

PaletteData* FindPaletteInfo(u32 hash) {
    MARKFUNCTION(0x8009CFEC);

    for (s32 i = 0; i < g_paletteDataCount; i++) {
        PaletteData* entry = g_paletteDataArray[i];
        if (entry && entry->hash == hash) {
            return entry;
        }
    }

    return nullptr;
}

PaletteData* PaletteData::Clone() const {
    MARKFUNCTION(0x8009D038);

    PaletteData* clone = new PaletteData();
    if (!clone) {
        return nullptr;
    }

    u16* buffer = new u16[256];
    if (!buffer) {
        delete clone;
        return nullptr;
    }

    clone->hash = hash;
    clone->frameCount = frameCount;
    clone->permOffset = permOffset;
    clone->frameStream = frameStream;
    clone->frameStreamSize = frameStreamSize;
    clone->paletteLutCount = paletteLutCount;

    clone->frameIndex = 0;
    clone->frameOffset = 0;

    clone->writeCursor = buffer;
    clone->writeCursorEnd = buffer + 256;

    clone->clut = 0x4000;
    clone->setupFlags = 0;
    clone->paletteLut = paletteLut;
#if defined(RC_PLATFORM_PSP)
    if (ownedStream && ownedStreamSize > 0) {
        u8* owned = new u8[ownedStreamSize];
        std::memcpy(owned, ownedStream, ownedStreamSize);
        clone->ownedStream = owned;
        clone->ownedStreamSize = ownedStreamSize;
        clone->frameStream = owned;
        clone->paletteLut = reinterpret_cast<const u16*>(owned + (reinterpret_cast<const u8*>(paletteLut) - ownedStream));
    }
#endif

    return clone;
}

u32 PaletteData::SetupClut(ComEffect* effect, s32 mode, u32 flags) {
    MARKFUNCTION(0x8009D0F4);

    const u32 newClut = effect ? effect->GetClut(mode) : 0x4000;
    clut = newClut;
    setupFlags = flags;
    return newClut;
}

void UnloadPaletteData() {
    MARKFUNCTION(0x8009D134);

    for (s32 i = 0; i < g_paletteDataCount; i++) {
        if (g_paletteDataArray[i]) {
            delete g_paletteDataArray[i];
            g_paletteDataArray[i] = nullptr;
        }
    }

    g_paletteDataCount = 0;
}

s32 PaletteData::InitPalette() {
    MARKFUNCTION(0x8009D1A0);

    frameIndex = 0;
    frameOffset = 0;

    if (!IsValidPalettePointer(writeCursorEnd)) {
        writeCursor = nullptr;
        return 0;
    }

    writeCursor = writeCursorEnd - 256;
    return 1;
}

s32 PaletteData::NextFrame() {
    MARKFUNCTION(0x8009D1B8);

    if (frameCount == 0) {
        return 1;
    }

    if (!IsValidPalettePointer(frameStream)
        || frameStreamSize < 4
        || frameOffset > frameStreamSize - 4) {
        return 1;
    }

    // Frame stream is consumed by Update() first, then advanced here.
    // Wrap immediately after the last valid frame to avoid one invalid read tick.
    if ((frameIndex + 1) >= frameCount) {
        return 1;
    }

    const u32 currentOffset = frameOffset;
    const u16 frameBytes = ReadU16Pal(frameStream + currentOffset + 2);
    if (static_cast<u32>(frameBytes) > frameStreamSize - currentOffset - 4u) {
        return 1;
    }
    frameIndex += 1;
    frameOffset = currentOffset + 4u + static_cast<u32>(frameBytes);

    return 0;
}

s32 PaletteData::Update() {
    MARKFUNCTION(0x8009D208);

    if (!IsValidPalettePointer(writeCursor)
        || !IsValidPalettePointer(writeCursorEnd)
        || !IsValidPalettePointer(frameStream)
        || !IsValidPalettePointer(paletteLut)
        || frameStreamSize < 4
        || frameOffset > frameStreamSize - 4
        || paletteLutCount == 0
        || paletteLutCount > 256) {
        return 0;
    }

    // RLE runs are indexed from palette start every frame.
    writeCursor = writeCursorEnd - 256;

    const u8* frame = frameStream + frameOffset;
    const u32 framePayloadSize = static_cast<u32>(ReadU16Pal(frame + 2));
    if (framePayloadSize > frameStreamSize - frameOffset - 4u) {
        writeCursor = nullptr;
        return 0;
    }

    const u16 runCount = ReadU16Pal(frame + 0);
    const u16 alphaMask = (setupFlags == 1u) ? 0x8000u : 0u;

    const u8* runData = frame + 4;
    u32 runOffset = 0;
    for (u32 run = 0; run < runCount; run++) {
        if (runOffset + 2u > framePayloadSize) {
            writeCursor = nullptr;
            return 0;
        }

        const u8 skip = *runData++;
        const u8 spanByte = *runData++;
        runOffset += 2u;

        if (static_cast<u32>(skip) > 256u
            || static_cast<u32>(skip) + (spanByte ? static_cast<u32>(spanByte) : 256u) > 256u
            || runOffset + (spanByte ? static_cast<u32>(spanByte) : 256u) > framePayloadSize) {
            writeCursor = nullptr;
            return 0;
        }

        // The check above bounds one run; also bound the running total so a
        // sequence of runs cannot advance writeCursor past its 256-entry buffer.
        const u32 runSpan = spanByte ? static_cast<u32>(spanByte) : 256u;
        if (writeCursor > writeCursorEnd
            || static_cast<u32>(writeCursorEnd - writeCursor) < static_cast<u32>(skip) + runSpan) {
            writeCursor = nullptr;
            return 0;
        }

        writeCursor += skip;

        u32 span = 256;
        if (spanByte != 0) {
            span = spanByte;
        }

        for (u32 i = 0; i < span; i++) {
            const u8 paletteIndex = *runData++;
            if (paletteIndex >= paletteLutCount) {
                writeCursor = nullptr;
                return 0;
            }
            *writeCursor = static_cast<u16>(paletteLut[paletteIndex] | alphaMask);
            writeCursor += 1;
        }
        runOffset += span;
    }

    writeCursor = writeCursorEnd;
    return writeCursorEnd ? 1 : 0;
}

s32 PaletteData::TransferVram() {
    MARKFUNCTION(0x8009D2E0);

    if (!writeCursorEnd || !p3d::rawTextureUploader) {
        return 0;
    }

    const s16 rectX = static_cast<s16>(16 * (clut & 0x3Fu));
    const s16 rectY = static_cast<s16>(clut >> 6);
    const u8* src = reinterpret_cast<const u8*>(writeCursorEnd - 256);

    p3d::rawTextureUploader(rectX, rectY, 256, 1, src);
    return 1;
}

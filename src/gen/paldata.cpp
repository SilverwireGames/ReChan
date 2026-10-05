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

PaletteData::PaletteData() {
    MARKFUNCTION(0x8009CE34);
}

PaletteData::~PaletteData() {
    MARKFUNCTION(0x8009CE44);

    if (writeCursorEnd) {
        u16* base = writeCursorEnd - 256;
        delete[] base;
    }

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

    clone->frameIndex = 0;
    clone->frameOffset = 0;

    clone->writeCursor = buffer;
    clone->writeCursorEnd = buffer + 256;

    clone->clut = 0x4000;
    clone->setupFlags = 0;
    clone->paletteLut = paletteLut;

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

    if (!writeCursorEnd) {
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

    if (!frameStream) {
        return 1;
    }

    // PSX (8009D1B8): slt frameIndex, frameCount -- it advances while the index
    // is below frameCount, so indices 0..frameCount inclusive get played.
    // Wrapping a tick earlier silently drops the last frame of every cycle.
    if (frameIndex >= frameCount) {
        return 1;
    }

    const u32 currentOffset = frameOffset;
    const u16 frameBytes = ReadU16Pal(frameStream + currentOffset + 2);
    frameIndex += 1;
    frameOffset = currentOffset + 4u + static_cast<u32>(frameBytes);

    return 0;
}

s32 PaletteData::Update() {
    MARKFUNCTION(0x8009D208);

    if (!writeCursor || !frameStream || !paletteLut) {
        return 0;
    }

    // Run positions are absolute within the 256-entry palette.
    //
    // PSX (8009D208) leaves the cursor at writeCursorEnd and sign-extends the
    // skip byte (sll 24 / sra 23), so a byte b of 128..255 seeks to offset b
    // (256 + (s8)b). That encoding only reaches the upper half: b < 128 would
    // land at 256 + b, past the palette. Rebasing to the palette start and
    // treating the byte as unsigned yields offset b for every b in 0..255 --
    // identical to PSX wherever its form is valid, and correct where it is not.
    writeCursor = writeCursorEnd - 256;

    const u8* frame = frameStream + frameOffset;
    const u16 runCount = ReadU16Pal(frame + 0);
    const u16 alphaMask = (setupFlags == 1u) ? 0x8000u : 0u;

    u16* const paletteStart = writeCursorEnd - 256;
    const u8* runData = frame + 4;
    for (u32 run = 0; run < runCount; run++) {
        // Unsigned: paired with the rebase above, see the note there.
        const u8 skip = *runData++;
        const u8 spanByte = *runData++;

        writeCursor += skip;

        u32 span = 256;
        if (spanByte != 0) {
            span = spanByte;
        }

        for (u32 i = 0; i < span; i++) {
            const u8 paletteIndex = *runData++;
            // The cursor is data-driven; keep writes inside the 256-entry
            // buffer so malformed data corrupts colours rather than the heap.
            // runData still advances so the stream stays in step.
            if (writeCursor >= paletteStart && writeCursor < writeCursorEnd) {
                *writeCursor = static_cast<u16>(paletteLut[paletteIndex] | alphaMask);
            }
            writeCursor += 1;
        }
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

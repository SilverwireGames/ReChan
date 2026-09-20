#pragma once

#include "core.h"

class ComEffect;

// PSX: PaletteData (PALDATA.CPP, 44 bytes)
// Stores frame stream metadata and a 256-word upload buffer for CLUT animation.
class PaletteData {
public:
    PaletteData();
    ~PaletteData();

    PaletteData* Clone() const;
    u32 SetupClut(ComEffect* effect, s32 mode, u32 flags);

    s32 InitPalette();
    s32 NextFrame();
    s32 Update();
    s32 TransferVram();

    // PSX object layout is 11 dwords (44 bytes on PSX).
    const u8* frameStream = nullptr;
    u16* writeCursor = nullptr;
    u16* writeCursorEnd = nullptr;

    u32 permOffset = 0;
    u32 hash = 0;
    u32 clut = 0x4000;
    u32 frameCount = 0;
    u32 frameOffset = 0;
    u32 frameIndex = 0;
    u32 setupFlags = 0;

    const u16* paletteLut = nullptr;
    // 0x8C01 places the LUT at permOffset bytes after frameStream. Keeping
    // the parsed limits here lets the PSP-side RLE decoder reject malformed
    // animation data before it reads past the permanent buffer.
    u32 frameStreamSize = 0;
    u32 paletteLutCount = 0;
};

// PSX: Load__11PaletteDataR10tReadChunkPPv (PALDATA.CPP:117, 0x8009CEA4)
bool LoadPaletteData(u16 chunkId, const u8* body, u32 bodySize,
                     const u8* permBase, u32& permCursor, u32 permSize);

// PSX: FindPaletteInfo__11PaletteDataUl (PALDATA.CPP:186, 0x8009CFEC)
PaletteData* FindPaletteInfo(u32 hash);

// PSX: Unload__11PaletteData (PALDATA.CPP:270, 0x8009D134)
void UnloadPaletteData();

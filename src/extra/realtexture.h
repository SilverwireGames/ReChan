#pragma once
#include "core.h"
#include "gen/config.h"
#include <unordered_map>

#if REAL_TEXTURE_RENDERING

class pddiTexture;

// One real-texture entry: a real GPU texture plus the sub-rect (in the same
// raw page-pixel UV space the legacy VRAM path uses) it corresponds to.
struct RealTextureEntry {
    pddiTexture* texture = nullptr;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float sizeX = 1.0f;
    float sizeY = 1.0f;
};

// One contiguous range of a mesh's index buffer that all shares the same
// (tpage, cba) material, used to sub-range-draw it with a bound real texture.
struct RealTextureGroup {
    u32 startIndex = 0;
    u32 indexCount = 0;
    u16 tpage = 0;
    u16 cba = 0;
};

// Maps a PSX (tpage, cba) pair -- the same key every mesh vertex already
// carries -- to a real, full-resolution 2D texture. Populated whenever a
// named texture chunk is parsed (character or world geometry), independent
// of whether a ModLoader override exists for it: modded entries come from
// the override PNG at native resolution, vanilla entries from decoding the
// original VRAM/CLUT data at its native (PSX) resolution.
class RealTextureRegistry {
public:
    static RealTextureRegistry& Instance();

    // rgba is width*height*4 bytes (RGBA8). Replaces any existing entry for
    // the same (tpage, cba) pair, releasing its texture.
    void Register(u16 tpage, u16 cba, const u8* rgba, int width, int height,
                  float offsetX, float offsetY, float sizeX, float sizeY);

    [[nodiscard]] const RealTextureEntry* Find(u16 tpage, u16 cba) const;

    // Releases all owned textures. Call before re-registering a character's
    // or level's textures from scratch (reload/unload).
    void Clear();

    RealTextureRegistry(const RealTextureRegistry&) = delete;
    RealTextureRegistry& operator=(const RealTextureRegistry&) = delete;

private:
    RealTextureRegistry() = default;
    ~RealTextureRegistry();

    std::unordered_map<u32, RealTextureEntry> m_entries; // key = tpage<<16 | cba
};

#endif // REAL_TEXTURE_RENDERING

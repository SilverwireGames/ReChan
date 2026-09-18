#include "extra/realtexture.h"

#if REAL_TEXTURE_RENDERING

#include "p3d/context.h"
#include "pddi/pddidev.h"
#include "pddi/pdditex.h"
#include "pc/log.h"

RealTextureRegistry& RealTextureRegistry::Instance() {
    static RealTextureRegistry instance;
    return instance;
}

RealTextureRegistry::~RealTextureRegistry() {
    Clear();
}

static u32 MakeKey(u16 tpage, u16 cba) {
    return (static_cast<u32>(tpage) << 16) | cba;
}

void RealTextureRegistry::Register(u16 tpage, u16 cba, const u8* rgba, int width, int height,
                                   float offsetX, float offsetY, float sizeX, float sizeY) {
    if (!rgba || width <= 0 || height <= 0 || !p3d::device) return;

    pddiTexture* texture = p3d::device->NewTexture();
    texture->SetData(width, height, 32, 8, rgba);
    texture->SetFilterMode(PDDI_FILTER_NONE);

    const u32 key = MakeKey(tpage, cba);
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.texture) {
        it->second.texture->Release();
    }

    m_entries[key] = RealTextureEntry{ texture, offsetX, offsetY, sizeX, sizeY };
}

const RealTextureEntry* RealTextureRegistry::Find(u16 tpage, u16 cba) const {
    auto it = m_entries.find(MakeKey(tpage, cba));
    return it != m_entries.end() ? &it->second : nullptr;
}

void RealTextureRegistry::Clear() {
    for (auto& [key, entry] : m_entries) {
        if (entry.texture) {
            entry.texture->Release();
        }
    }
    m_entries.clear();
}

#endif // REAL_TEXTURE_RENDERING

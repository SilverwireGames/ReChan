#pragma once

#include "core.h"
#include "gen/utf8text.h"
#include "pc/textmgr.h"

struct TextBackendFontDesc {
    TextFontHandle handle = 0;
    const char* name = nullptr;
    const u8* fileData = nullptr;
    s32 fileSize = 0;
    s32 pixelHeight = 0;
};

class ITextBackend {
public:
    virtual ~ITextBackend() = default;

    virtual bool Init() = 0;
    virtual void Shutdown() = 0;
    virtual bool IsReady() const = 0;

    virtual bool RegisterFont(const TextBackendFontDesc& desc) = 0;
    virtual void UnregisterFont(TextFontHandle handle) = 0;
    // Adds an exact-size rasterisation of a registered font. Text whose
    // on-screen size is near `pixelHeight` draws from it 1:1 instead of
    // minifying the registration atlas. With `bakedOutline` the strike serves
    // only black-outlined text and carries the outline in its texels.
    virtual bool AddStrike(TextFontHandle handle, s32 pixelHeight, bool bakedOutline) {
        (void)handle;
        (void)pixelHeight;
        (void)bakedOutline;
        return false;
    }

    virtual TextBounds Measure(Utf8TextView text, const TextRenderState& state) const = 0;
    virtual s32 CountWrappedLines(Utf8TextView text, const TextRenderState& state) const = 0;
    virtual void Draw(Utf8TextView text, f32 x, f32 y, const TextRenderState& state) const = 0;
};

ITextBackend* CreateDefaultTextBackend();
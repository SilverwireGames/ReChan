// texture.cpp — tTexture + tTextureLoader implementation
//
// PSX 0x6008 format: PString name, RECT16 vram, u32 type, raw data.
// Textures come in CLUT+pixel pairs within a 0xFF04 TexturePage.
// 4bpp: w = rw*4, 8bpp: w = rw*2.
//
#include "p3d/texture.h"
#include "p3d/context.h"
#include "p3d/chunkfile.h"
#include "p3d/inventory.h"
#include "p3d/fileio.h"
#include "pddi/pddidev.h"
#include "pddi/pdditex.h"
#if defined(RC_PLATFORM_PSP)
// Only the PSP paths log; the desktop libp3d build has no src/ include path.
#include "pc/log.h"
#include "pddi/psp/psprender.h"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb/stb_image.h"

#include <string>
#include <algorithm>
#include <cstring>

namespace p3d {
    RawTextureUploadCallback rawTextureUploader = nullptr;
}

#if defined(RC_PLATFORM_PSP)
namespace {

#pragma pack(push, 1)
struct PspNativeTextureHeader {
    char magic[4];
    u16 width;
    u16 height;
    u16 format;
};
#pragma pack(pop)

static_assert(sizeof(PspNativeTextureHeader) == 10, "unexpected PSTX header padding");

static std::string PspNativeTexturePath(const char* path) {
    std::string result = path ? path : "";
    const std::size_t extension = result.rfind(".png");
    if (extension != std::string::npos && extension + 4 == result.size()) {
        result.replace(extension, 4, ".ptex");
    }
    return result;
}

static tTexture* LoadPspNativeTexture(const char* path, s32* outWidth, s32* outHeight) {
    const std::string nativePath = PspNativeTexturePath(path);
    const auto file = p3d::io::ReadFile(nativePath);
    if (!file || file->size() < sizeof(PspNativeTextureHeader)) {
#if !defined(NDEBUG)
        LOG("[PspTexture] native miss path=%s resolved=%s", nativePath.c_str(),
            p3d::io::ResolvePath(nativePath).c_str());
#endif
        return nullptr;
    }

    PspNativeTextureHeader header{};
    std::memcpy(&header, file->data(), sizeof(header));
    if (std::memcmp(header.magic, "PSTX", 4) != 0
        || header.format != 1
        || header.width == 0 || header.height == 0
        || header.width > 512 || header.height > 512) {
#if !defined(NDEBUG)
        LOG("[PspTexture] native invalid path=%s bytes=%lu magic=%c%c%c%c size=%ux%u format=%u",
            nativePath.c_str(), static_cast<unsigned long>(file->size()),
            header.magic[0], header.magic[1], header.magic[2], header.magic[3],
            static_cast<unsigned>(header.width), static_cast<unsigned>(header.height),
            static_cast<unsigned>(header.format));
#endif
        return nullptr;
    }

    const std::size_t pixelCount = static_cast<std::size_t>(header.width) * header.height;
    if (file->size() != sizeof(header) + pixelCount * sizeof(u16)) {
#if !defined(NDEBUG)
        LOG("[PspTexture] native size mismatch path=%s bytes=%lu expected=%lu",
            nativePath.c_str(), static_cast<unsigned long>(file->size()),
            static_cast<unsigned long>(sizeof(header) + pixelCount * sizeof(u16)));
#endif
        return nullptr;
    }

    tTexture* texture = new tTexture();
    if (!texture->Create(header.width, header.height, 16, 4, nullptr)) {
#if !defined(NDEBUG)
        LOG("[PspTexture] native create failed path=%s size=%ux%u", nativePath.c_str(),
            static_cast<unsigned>(header.width), static_cast<unsigned>(header.height));
#endif
        texture->Release();
        return nullptr;
    }

    auto* nativeTexture = static_cast<pspTexture*>(texture->GetTexture());
    nativeTexture->SetNative4444Data(header.width, header.height,
        reinterpret_cast<const u16*>(file->data() + sizeof(header)));
    if (!nativeTexture->IsValid()) {
#if !defined(NDEBUG)
        LOG("[PspTexture] native upload failed path=%s", nativePath.c_str());
#endif
        texture->Release();
        return nullptr;
    }

#if !defined(NDEBUG)
    LOG("[PspTexture] native loaded path=%s size=%ux%u", nativePath.c_str(),
        static_cast<unsigned>(header.width), static_cast<unsigned>(header.height));
#endif

    if (outWidth) *outWidth = header.width;
    if (outHeight) *outHeight = header.height;
    return texture;
}

// PNG/JPEG frontend art is authored for the desktop at 1920x1080.  Keeping
// those dimensions until pspTexture::SetData would briefly require an 8 MiB
// 16-bit texture plus the decoder's RGBA buffer for every image.  The PSP has
// no use for that source resolution: its native framebuffer is 480x272 and
// the GE's largest practical texture for this title is 512x512.
static void GetPspTextureDimensions(int sourceWidth, int sourceHeight,
                                     int& targetWidth, int& targetHeight) {
    targetWidth = sourceWidth;
    targetHeight = sourceHeight;

    if (sourceWidth <= 512 && sourceHeight <= 512) {
        return;
    }

    if (sourceWidth >= 960 && sourceHeight >= 540
        && sourceWidth >= sourceHeight * 3 / 2) {
        targetWidth = 480;
        targetHeight = 272;
        return;
    }

    const float scale = std::min(512.0f / static_cast<float>(sourceWidth),
                                 512.0f / static_cast<float>(sourceHeight));
    targetWidth = std::max(1, static_cast<int>(sourceWidth * scale + 0.5f));
    targetHeight = std::max(1, static_cast<int>(sourceHeight * scale + 0.5f));
}

} // namespace
#endif

// tTexture

tTexture::tTexture() = default;

tTexture* tTexture::LoadFromImagePath(const char* path, s32* outWidth, s32* outHeight) {
    if (!path || !path[0]) {
        return nullptr;
    }

#if defined(RC_PLATFORM_PSP)
    if (tTexture* native = LoadPspNativeTexture(path, outWidth, outHeight)) {
        return native;
    }
#endif

    int width = 0;
    int height = 0;
    int channels = 0;
    const std::string resolvedPath = p3d::io::ResolvePath(path);
    unsigned char* rgbaBytes = stbi_load(resolvedPath.c_str(), &width, &height, &channels, 4);
    if (!rgbaBytes || width <= 0 || height <= 0) {
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        LOG("[PspTexture] stbi_load failed path=%s reason=%s", resolvedPath.c_str(),
            stbi_failure_reason() ? stbi_failure_reason() : "unknown");
#endif
        return nullptr;
    }

    int targetWidth = width;
    int targetHeight = height;
#if defined(RC_PLATFORM_PSP)
    GetPspTextureDimensions(width, height, targetWidth, targetHeight);
#endif

    u32* packedPixels = new u32[(size_t)targetWidth * (size_t)targetHeight];
    for (s32 y = 0; y < targetHeight; ++y) {
        const s32 sourceY = std::min<s32>(static_cast<s32>(height - 1),
            static_cast<s32>((static_cast<int64_t>(y) * height) / targetHeight));
        for (s32 x = 0; x < targetWidth; ++x) {
            const s32 sourceX = std::min<s32>(static_cast<s32>(width - 1),
                static_cast<s32>((static_cast<int64_t>(x) * width) / targetWidth));
            const size_t sourceIndex = (static_cast<size_t>(sourceY) * width + sourceX) * 4;
            const u8 r = rgbaBytes[sourceIndex + 0];
            const u8 g = rgbaBytes[sourceIndex + 1];
            const u8 b = rgbaBytes[sourceIndex + 2];
            const u8 a = rgbaBytes[sourceIndex + 3];
            packedPixels[static_cast<size_t>(y) * targetWidth + x] =
                ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
        }
    }

    stbi_image_free(rgbaBytes);

    tTexture* tex = new tTexture();
    if (!tex->Create(targetWidth, targetHeight, 32, 8, packedPixels)) {
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        LOG("[PspTexture] create failed path=%s source=%dx%d target=%dx%d",
            resolvedPath.c_str(), width, height, targetWidth, targetHeight);
#endif
        delete[] packedPixels;
        tex->Release();
        return nullptr;
    }

    delete[] packedPixels;

    if (outWidth) {
        *outWidth = width;
    }
    if (outHeight) {
        *outHeight = height;
    }

    return tex;
}

tTexture::~tTexture() {
    if (texture) {
        texture->Release();
        texture = nullptr;
    }
    delete[] textureData;
    textureData = nullptr;
    textureDataSize = 0;
}

bool tTexture::Create(int width, int height, int bpp, int alphaDepth,
                      const void* rgba) {
    if (!p3d::device) return false;
    if (texture) {
        texture->Release();
        texture = nullptr;
    }
    texture = p3d::device->NewTexture();
    texture->SetData(width, height, bpp, alphaDepth, rgba);
#if defined(RC_PLATFORM_PSP)
    if (!static_cast<pspTexture*>(texture)->IsValid()) {
        texture->Release();
        texture = nullptr;
        return false;
    }
#endif
    return true;
}

int tTexture::GetWidth() const { return texture ? texture->GetWidth() : 0; }
int tTexture::GetHeight() const { return texture ? texture->GetHeight() : 0; }

void tTexture::SetTextureData(const u8* data, u32 size) {
    delete[] textureData;
    textureData = nullptr;
    textureDataSize = 0;

    if (!data || size == 0) {
        return;
    }

    textureData = new u8[size];
    std::memcpy(textureData, data, size);
    textureDataSize = size;
}

void tTexture::SetTextureRect(s16 x, s16 y, s16 w, s16 h) {
    textureRect.x = x;
    textureRect.y = y;
    textureRect.w = w;
    textureRect.h = h;
}

// 0x6008 sub-chunk parser

struct TexChunkHeader {
    std::string name;
    s16 rx, ry, rw, rh;   // VRAM rect
    u32 type;
    const u8* rawData;
    u32 rawSize;
    bool isClut;
};

static bool ParseTexChunkHeader(const u8* data, u32 size, TexChunkHeader& out) {
    if (!data || size < 1) return false;

    u32 pos = 0;
    u8 nameLen = data[pos++];
    if (pos + nameLen + 12 > size) return false;

    out.name.assign(reinterpret_cast<const char*>(data + pos), nameLen);
    pos += nameLen;

    // Trim trailing whitespace/nulls
    while (!out.name.empty() && (out.name.back() == ' ' || out.name.back() == '\0'))
        out.name.pop_back();

    // RECT16: x, y, w, h (s16 LE)
    auto readS16 = [&]() -> s16 {
        s16 v = static_cast<s16>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return v;
    };
    out.rx = readS16();
    out.ry = readS16();
    out.rw = readS16();
    out.rh = readS16();

    // u32 type
    out.type = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24);
    pos += 4;

    out.rawData = data + pos;
    out.rawSize = size - pos;
    out.isClut = out.name.find("CLUT") != std::string::npos;

    return true;
}

// tTextureLoader

void tTextureLoader::LoadChunk(tChunkFile* file, tInventory* store) {
    if (!store) {
        return;
    }

    const u32 len = file->GetCurrentDataLength();
    std::string payload(len, '\0');
    file->GetData(payload.data(), len);

    TexChunkHeader hdr;
    if (!ParseTexChunkHeader(reinterpret_cast<const u8*>(payload.data()), len, hdr)) {
        return;
    }

    auto* tex = new tTexture();
    tex->SetName(hdr.name.c_str());
    tex->SetTextureRect(hdr.rx, hdr.ry, hdr.rw, hdr.rh);
    tex->SetTextureType(hdr.type);
    tex->SetTextureData(hdr.rawData, hdr.rawSize);
    store->Store(tex);
    tex->Release();
}

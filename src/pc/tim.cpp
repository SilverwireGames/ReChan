#include "gen/common.h"
#include "gen/psxcolor_helpers.h"
#include "pc/tim.h"
#include "gen/config.h"
#include "p3d/texture.h"
#include "p3d/fileio.h"
#include "p3d/shader.h"
#include "p3d/matrix.h"
#include "p3d/context.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "pddi/pddishad.h"
#include "pddi/pdditex.h"
#include "xclib/xcfile.h"
#include <vector>
#include <algorithm>
#include <cmath>
#if MOD_LOADER
#include "extra/modloader.h"
#include "p3d/hash.h"
#include <filesystem>
#include "vendor/stb/stb_image.h"
#endif

// TIM file structures
struct TimHeader {
    u32 magic;  // 0x10
    u32 flags;  // bits 0-2: depth (0=4bit,1=8bit,2=15bit,3=24bit), bit 3: has CLUT
};

struct TimSection {
    u32 size;   // total section size in bytes (including this field)
    u16 x, y;   // VRAM destination
    u16 w, h;   // dimensions (in 16-bit units for image data)
};

TimImage* Tim::LoadFromFile(const char* path) {
#if MOD_LOADER
    {
        std::string stem = std::filesystem::path(path).stem().string();
        for (char& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        u32 crc = p3dHash(stem.c_str());
        if (ModLoader::Instance().HasTexture(crc)) {
            const std::string* pngPath = ModLoader::Instance().GetTexturePath(crc);
            if (pngPath) {
                int w, h, ch;
                const std::string resolvedPngPath = p3d::io::ResolvePath(*pngPath);
                unsigned char* px = stbi_load(resolvedPngPath.c_str(), &w, &h, &ch, 4);
                if (px) {
                    TimImage* img = new TimImage();
                    img->width = w;
                    img->height = h;
                    img->rgba = new u32[w * h];
                    memcpy(img->rgba, px, static_cast<size_t>(w * h) * 4);
                    stbi_image_free(px);
                    LOG("[ModLoader] Texture override: %s -> %s", path, pngPath->c_str());
                    return img;
                }
            }
        }
    }
#endif
    u8* data = nullptr;
    u32 fileSize = 0;
    if (!xcReadFileLow(path, &data, &fileSize)) {
        LOG("[Tim] Failed to open: %s", path);
        return nullptr;
    }

    TimImage* img = Tim::LoadFromMemory(data, fileSize);
    delete[] data;

    if (img) {
        LOG("[Tim] Loaded %s: %dx%d", path, img->width, img->height);
    }
    else {
        LOG("[Tim] Failed to decode: %s", path);
    }
    return img;
}

TimImage* Tim::LoadFromMemory(const u8* data, u32 fileSize) {
    if (!data || fileSize < 8)
        return nullptr;

    const TimHeader* hdr = reinterpret_cast<const TimHeader*>(data);
    if (hdr->magic != 0x10)
        return nullptr;

    u32 depth = hdr->flags & 0x07;
    bool hasClut = (hdr->flags & 0x08) != 0;
    u32 pos = 8;

    // Parse optional CLUT
    u16* clut = nullptr;
    s32 clutColors = 0;
    if (hasClut) {
        if (pos + 12 > (u32)fileSize) { return nullptr; }
        const TimSection* cs = reinterpret_cast<const TimSection*>(data + pos);
        clutColors = cs->w * cs->h;
        clut = new u16[clutColors];
        memcpy(clut, data + pos + 12, clutColors * 2);
        pos += cs->size;
    }

    // Parse image section
    if (pos + 12 > (u32)fileSize) {
        delete[] clut;
        return nullptr;
    }
    const TimSection* is = reinterpret_cast<const TimSection*>(data + pos);
    u16 iw = is->w; // in 16-bit halfword units
    u16 ih = is->h;
    const u8* imgData = data + pos + 12;

    // Calculate pixel dimensions
    s32 pixW, pixH;
    pixH = ih;
    switch (depth) {
        case 0: pixW = iw * 4; break;  // 4bpp: 4 pixels per halfword
        case 1: pixW = iw * 2; break;  // 8bpp: 2 pixels per halfword
        case 2: pixW = iw;     break;  // 16bpp: 1 pixel per halfword
        case 3: pixW = (iw * 2) / 3; break; // 24bpp
        default:
            delete[] clut;
            return nullptr;
    }

    TimImage* img = new TimImage();
    img->width = pixW;
    img->height = pixH;
    img->rgba = new u32[pixW * pixH];
    memset(img->rgba, 0, pixW * pixH * 4);

    // Decode pixels
    switch (depth) {
        case 0:
        { // 4bpp indexed
            if (!clut) break;
            for (s32 y = 0; y < pixH; y++) {
                const u8* row = imgData + y * iw * 2;
                for (s32 x = 0; x < pixW; x++) {
                    u8 byte = row[x / 2];
                    u8 idx = (x & 1) ? (byte >> 4) : (byte & 0x0F);
                    if (idx < clutColors)
                        img->rgba[y * pixW + x] = PsxAbgr1555ToRgba8888(clut[idx]);
                }
            }
            break;
        }
        case 1:
        { // 8bpp indexed
            if (!clut) break;
            for (s32 y = 0; y < pixH; y++) {
                const u8* row = imgData + y * iw * 2;
                for (s32 x = 0; x < pixW; x++) {
                    u8 idx = row[x];
                    if (idx < clutColors)
                        img->rgba[y * pixW + x] = PsxAbgr1555ToRgba8888(clut[idx]);
                }
            }
            break;
        }
        case 2:
        { // 16bpp direct color
            for (s32 y = 0; y < pixH; y++) {
                const u16* row = reinterpret_cast<const u16*>(imgData + y * iw * 2);
                for (s32 x = 0; x < pixW; x++) {
                    img->rgba[y * pixW + x] = PsxAbgr1555ToRgba8888(row[x]);
                }
            }
            break;
        }
        case 3:
        { // 24bpp direct color
            for (s32 y = 0; y < pixH; y++) {
                const u8* row = imgData + y * iw * 2;
                for (s32 x = 0; x < pixW; x++) {
                    s32 off = x * 3;
                    u8 r = row[off + 0];
                    u8 g = row[off + 1];
                    u8 b = row[off + 2];
                    img->rgba[y * pixW + x] = (255u << 24) | (b << 16) | (g << 8) | r;
                }
            }
            break;
        }
    }

    delete[] clut;
    return img;
}

tTexture* Tim::CreateTexture(const TimImage* img) {
    if (!img || !img->rgba) return nullptr;
    tTexture* tex = new tTexture();
    tex->Create(img->width, img->height, 32, 8, img->rgba);
    return tex;
}

// ScreenDraw

static pddiBaseShader* s_screenShader = nullptr;

static void EnsureShader() {
    if (s_screenShader)
        return;

    s_screenShader = p3d::device->NewShader("simple");
}

// Overlay batching
static constexpr s32 kMinCircleSegments = 3;
static constexpr s32 kMaxCircleSegments = 64;
static constexpr f32 kTwoPi = 6.28318530718f;

static s32 s_overlayBatchDepth = 0;
static Mat4 s_overlayBatchPrevProj;
#if defined(RC_PLATFORM_PSP)
static constexpr size_t kPspOverlayVertexReserve = 8192;
static std::vector<pddiBatchVertex> s_pendingPrimitiveVerts = [] {
    std::vector<pddiBatchVertex> vertices;
    // Allocate the steady-state overlay arena before the first frame.  If a
    // scene submits more than this bound, QueueBatchedVertex flushes instead
    // of allowing vector growth inside the GU frame.
    vertices.reserve(kPspOverlayVertexReserve);
    return vertices;
}();
#else
static std::vector<pddiBatchVertex> s_pendingPrimitiveVerts;
#endif
static tTexture* s_pendingPrimitiveTex = nullptr;
static bool s_pendingPrimitiveActive = false;

static void FlushPendingPrimitiveBatch();

static s32 ClampCircleSegments(s32 segments) {
    if (segments < kMinCircleSegments)
        return kMinCircleSegments;

    if (segments > kMaxCircleSegments)
        return kMaxCircleSegments;

    return segments;
}

static void QueueBatchedVertex(f32 x, f32 y,
                               f32 u, f32 v,
                               u8 r, u8 g, u8 b, u8 a) {
#if defined(RC_PLATFORM_PSP)
    if (s_pendingPrimitiveVerts.size() >= kPspOverlayVertexReserve) {
        FlushPendingPrimitiveBatch();
    }
#endif
    s_pendingPrimitiveVerts.push_back({ x, y, u, v, r, g, b, a });
}

static void FlushPendingPrimitiveBatch() {
    if (s_pendingPrimitiveVerts.empty()) {
        s_pendingPrimitiveActive = false;
        return;
    }

    p3d::context->DrawQuadBatch(
        s_pendingPrimitiveTex ? s_pendingPrimitiveTex->GetTexture() : nullptr,
        PDDI_BLEND_ALPHA,
        s_pendingPrimitiveVerts.data(),
        (s32)s_pendingPrimitiveVerts.size());

    s_pendingPrimitiveVerts.clear();
    s_pendingPrimitiveActive = false;
}

static void EnsurePrimitiveBatchTexture(tTexture* tex) {
    if (s_pendingPrimitiveActive && s_pendingPrimitiveTex != tex) {
        FlushPendingPrimitiveBatch();
    }

    s_pendingPrimitiveTex = tex;
    s_pendingPrimitiveActive = true;
}

static void QueueBatchedQuad(tTexture* tex, f32 x, f32 y, f32 w, f32 h,
                             f32 u0, f32 v0, f32 u1, f32 v1,
                             u8 r, u8 g, u8 b, u8 a) {
    if (w == 0.0f || h == 0.0f || a == 0)
        return;

    EnsurePrimitiveBatchTexture(tex);

    const f32 yTop = y;
    const f32 yBottom = y + h;

    QueueBatchedVertex(x, yBottom, u0, v1, r, g, b, a);
    QueueBatchedVertex(x + w, yBottom, u1, v1, r, g, b, a);
    QueueBatchedVertex(x + w, yTop, u1, v0, r, g, b, a);

    QueueBatchedVertex(x, yBottom, u0, v1, r, g, b, a);
    QueueBatchedVertex(x + w, yTop, u1, v0, r, g, b, a);
    QueueBatchedVertex(x, yTop, u0, v0, r, g, b, a);
}

static void QueueBatchedFilledCircle(tTexture* tex,
                                     f32 centerX, f32 centerY,
                                     f32 radiusX, f32 radiusY,
                                     f32 u0, f32 v0, f32 u1, f32 v1,
                                     s32 segments,
                                     u8 r, u8 g, u8 b, u8 a) {
    if (radiusX <= 0.0f || radiusY <= 0.0f || a == 0)
        return;

    segments = ClampCircleSegments(segments);
    EnsurePrimitiveBatchTexture(tex);

    const f32 uvCenterX = (u0 + u1) * 0.5f;
    const f32 uvCenterY = (v0 + v1) * 0.5f;
    const f32 uvRadiusX = (u1 - u0) * 0.5f;
    const f32 uvRadiusY = (v1 - v0) * 0.5f;

    for (s32 i = 0; i < segments; ++i) {
        const f32 a0 = ((f32)i / (f32)segments) * kTwoPi;
        const f32 a1 = ((f32)(i + 1) / (f32)segments) * kTwoPi;

        const f32 c0 = std::cos(a0);
        const f32 s0 = std::sin(a0);
        const f32 c1 = std::cos(a1);
        const f32 s1 = std::sin(a1);

        const f32 x0 = centerX;
        const f32 y0 = centerY;

        const f32 x1p = centerX + c0 * radiusX;
        const f32 y1p = centerY + s0 * radiusY;

        const f32 x2p = centerX + c1 * radiusX;
        const f32 y2p = centerY + s1 * radiusY;

        const f32 tu0 = uvCenterX;
        const f32 tv0 = uvCenterY;

        const f32 tu1 = uvCenterX + c0 * uvRadiusX;
        const f32 tv1 = uvCenterY + s0 * uvRadiusY;

        const f32 tu2 = uvCenterX + c1 * uvRadiusX;
        const f32 tv2 = uvCenterY + s1 * uvRadiusY;

        QueueBatchedVertex(x0, y0, tu0, tv0, r, g, b, a);
        QueueBatchedVertex(x1p, y1p, tu1, tv1, r, g, b, a);
        QueueBatchedVertex(x2p, y2p, tu2, tv2, r, g, b, a);
    }
}

static void QueueBatchedCircle(tTexture* tex,
                               f32 centerX, f32 centerY,
                               f32 radiusX, f32 radiusY,
                               f32 thickness,
                               f32 u0, f32 v0, f32 u1, f32 v1,
                               s32 segments,
                               u8 r, u8 g, u8 b, u8 a) {
    if (radiusX <= 0.0f || radiusY <= 0.0f || thickness <= 0.0f || a == 0)
        return;

    segments = ClampCircleSegments(segments);
    EnsurePrimitiveBatchTexture(tex);

    const f32 innerRadiusX = std::max(0.0f, radiusX - thickness);
    const f32 innerRadiusY = std::max(0.0f, radiusY - thickness);

    const f32 uvCenterX = (u0 + u1) * 0.5f;
    const f32 uvCenterY = (v0 + v1) * 0.5f;
    const f32 uvRadiusX = (u1 - u0) * 0.5f;
    const f32 uvRadiusY = (v1 - v0) * 0.5f;

    const f32 innerUvScaleX = radiusX > 0.0001f ? (innerRadiusX / radiusX) : 0.0f;
    const f32 innerUvScaleY = radiusY > 0.0001f ? (innerRadiusY / radiusY) : 0.0f;

    for (s32 i = 0; i < segments; ++i) {
        const f32 a0 = ((f32)i / (f32)segments) * kTwoPi;
        const f32 a1 = ((f32)(i + 1) / (f32)segments) * kTwoPi;

        const f32 c0 = std::cos(a0);
        const f32 s0 = std::sin(a0);
        const f32 c1 = std::cos(a1);
        const f32 s1 = std::sin(a1);

        const f32 outerX0 = centerX + c0 * radiusX;
        const f32 outerY0 = centerY + s0 * radiusY;
        const f32 outerX1 = centerX + c1 * radiusX;
        const f32 outerY1 = centerY + s1 * radiusY;

        const f32 innerX0 = centerX + c0 * innerRadiusX;
        const f32 innerY0 = centerY + s0 * innerRadiusY;
        const f32 innerX1 = centerX + c1 * innerRadiusX;
        const f32 innerY1 = centerY + s1 * innerRadiusY;

        const f32 outerU0 = uvCenterX + c0 * uvRadiusX;
        const f32 outerV0 = uvCenterY + s0 * uvRadiusY;
        const f32 outerU1 = uvCenterX + c1 * uvRadiusX;
        const f32 outerV1 = uvCenterY + s1 * uvRadiusY;

        const f32 innerU0 = uvCenterX + c0 * uvRadiusX * innerUvScaleX;
        const f32 innerV0 = uvCenterY + s0 * uvRadiusY * innerUvScaleY;
        const f32 innerU1 = uvCenterX + c1 * uvRadiusX * innerUvScaleX;
        const f32 innerV1 = uvCenterY + s1 * uvRadiusY * innerUvScaleY;

        // Triangle 1
        QueueBatchedVertex(outerX0, outerY0, outerU0, outerV0, r, g, b, a);
        QueueBatchedVertex(outerX1, outerY1, outerU1, outerV1, r, g, b, a);
        QueueBatchedVertex(innerX1, innerY1, innerU1, innerV1, r, g, b, a);

        // Triangle 2
        QueueBatchedVertex(outerX0, outerY0, outerU0, outerV0, r, g, b, a);
        QueueBatchedVertex(innerX1, innerY1, innerU1, innerV1, r, g, b, a);
        QueueBatchedVertex(innerX0, innerY0, innerU0, innerV0, r, g, b, a);
    }
}

// Internal: begin 2D overlay rendering (saves projection, sets ortho).
// canvasW/canvasH default to the live screen size; pass the actual render
// target's size when drawing off-screen so the projection matches the
// buffer being rendered to instead of the main window.
static Mat4 BeginOverlay(f32 canvasW = 0.0f, f32 canvasH = 0.0f) {
    EnsureShader();

    // Inside an active batch the overlay state is already set; don't touch it.
    if (s_overlayBatchDepth > 0) {
        return s_overlayBatchPrevProj;
    }

    Mat4 prev = p3d::context->GetProjectionMatrix();

    const f32 w = canvasW > 0.0f ? canvasW : SCREEN_WIDTH;
    const f32 h = canvasH > 0.0f ? canvasH : SCREEN_HEIGHT;
    p3d::context->SetProjectionMatrix(Ortho(0.0f, w, h, 0.0f, -1.0f, 1.0f));
    p3d::context->EnableZBuffer(false);
    p3d::context->SetCullMode(PDDI_CULL_NONE);
    p3d::context->SetMultisampleEnabled(false);

    return prev;
}

// Internal: end 2D overlay rendering (restores previous state).
static void EndOverlay(const Mat4& prev) {
    // Inside an active batch the state is restored once by EndBatch, not per quad.
    if (s_overlayBatchDepth > 0) {
        return;
    }
    p3d::context->SetProjectionMatrix(prev);
    p3d::context->EnableZBuffer(true);
    p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    p3d::context->SetMultisampleEnabled(true);
}

void ScreenDraw::BeginBatch(f32 canvasW, f32 canvasH) {
    EnsureShader();
    if (s_overlayBatchDepth++ == 0) {
        s_overlayBatchPrevProj = p3d::context->GetProjectionMatrix();
        const f32 w = canvasW > 0.0f ? canvasW : SCREEN_WIDTH;
        const f32 h = canvasH > 0.0f ? canvasH : SCREEN_HEIGHT;
        p3d::context->SetProjectionMatrix(Ortho(0.0f, w, h, 0.0f, -1.0f, 1.0f));
        p3d::context->EnableZBuffer(false);
        p3d::context->SetCullMode(PDDI_CULL_NONE);
        p3d::context->SetMultisampleEnabled(false);
    }
}

void ScreenDraw::EndBatch() {
    if (s_overlayBatchDepth <= 0) {
        return;
    }
    if (--s_overlayBatchDepth == 0) {
        FlushPendingPrimitiveBatch();
        p3d::context->SetProjectionMatrix(s_overlayBatchPrevProj);
        p3d::context->EnableZBuffer(true);
        p3d::context->SetBlendMode(PDDI_BLEND_NONE);
        p3d::context->SetMultisampleEnabled(true);
    }
}

void ScreenDraw::SetScissor(s32 x, s32 y, s32 w, s32 h) {
    // Scissor changes rasterization state, so pending triangles must be drawn
    // before changing it. This keeps ordering correct inside a ScreenDraw batch.
    if (s_overlayBatchDepth > 0) {
        FlushPendingPrimitiveBatch();
    }

    if (p3d::context) {
        p3d::context->SetScissor(x, y, w, h);
    }
}

void ScreenDraw::DrawFullscreen(tTexture* tex) {
    if (s_overlayBatchDepth > 0) {
        FlushPendingPrimitiveBatch();
    }

    Mat4 prev = BeginOverlay();
    p3d::context->SetBlendMode(PDDI_BLEND_NONE);

    s_screenShader->SetTexture(0, tex ? tex->GetTexture() : nullptr);
    s_screenShader->SetColour(0, pddiColour(255, 255, 255, 255));
    p3d::context->DrawQuad(s_screenShader, SCALE_AND_CENTER_X(0.0f), 0.0f, SCREEN_SCALE_X(DEFAULT_SCREEN_WIDTH), SCREEN_HEIGHT, 0.0f, 0.0f, 1.0f, 1.0f);

    EndOverlay(prev);
}

void ScreenDraw::DrawFullscreenAdvanced(tTexture* tex,
                                       f32 sourceAspect,
                                       f32 desiredAspect,
                                       bool fillScreen) {
    if (s_overlayBatchDepth > 0) {
        FlushPendingPrimitiveBatch();
    }

    if (!tex || sourceAspect <= 0.0f || desiredAspect <= 0.0f) {
        return;
    }

    Mat4 prev = BeginOverlay();
    p3d::context->SetBlendMode(PDDI_BLEND_NONE);

    f32 u0 = 0.0f;
    f32 v0 = 0.0f;
    f32 u1 = 1.0f;
    f32 v1 = 1.0f;

    // First crop the SOURCE image to the desired aspect ratio.
    //
    // Example:
    // sourceAspect  = 4.0f / 3.0f
    // desiredAspect = 16.0f / 9.0f
    //
    // This removes the top/bottom black bars from a 4:3 frame
    // that contains letterboxed 16:9 video.
    if (sourceAspect < desiredAspect) {
        // Source is taller/narrower than desired.
        // Crop vertically.
        const f32 visibleHeight = sourceAspect / desiredAspect;
        const f32 cropY = (1.0f - visibleHeight) * 0.5f;

        v0 = cropY;
        v1 = 1.0f - cropY;
    }
    else if (sourceAspect > desiredAspect) {
        // Source is wider than desired.
        // Crop horizontally.
        const f32 visibleWidth = desiredAspect / sourceAspect;
        const f32 cropX = (1.0f - visibleWidth) * 0.5f;

        u0 = cropX;
        u1 = 1.0f - cropX;
    }

    f32 x = SCALE_AND_CENTER_X(0.0f);
    f32 y = 0.0f;
    f32 w = SCREEN_SCALE_X(DEFAULT_SCREEN_WIDTH);
    f32 h = SCREEN_HEIGHT;

    if (fillScreen) {
        // Scale the desired-aspect image to fill the screen.
        // If the screen aspect differs, this crops outside the screen.
        const f32 screenAspect = w / h;

        if (screenAspect > desiredAspect) {
            // Screen is wider than desired.
            // Match width, extend height beyond screen.
            const f32 newH = w / desiredAspect;
            y = (h - newH) * 0.5f;
            h = newH;
        }
        else if (screenAspect < desiredAspect) {
            // Screen is narrower than desired.
            // Match height, extend width beyond screen.
            const f32 newW = h * desiredAspect;
            x = x + (w - newW) * 0.5f;
            w = newW;
        }
    }
    else {
        // Fit inside screen, preserving aspect ratio.
        // This may create black bars.
        const f32 screenAspect = w / h;

        if (screenAspect > desiredAspect) {
            // Screen is wider.
            // Match height, reduce width.
            const f32 newW = h * desiredAspect;
            x = x + (w - newW) * 0.5f;
            w = newW;
        }
        else if (screenAspect < desiredAspect) {
            // Screen is taller/narrower.
            // Match width, reduce height.
            const f32 newH = w / desiredAspect;
            y = (h - newH) * 0.5f;
            h = newH;
        }
    }

    s_screenShader->SetTexture(0, tex->GetTexture());
    s_screenShader->SetColour(0, pddiColour(255, 255, 255, 255));

    p3d::context->DrawQuad(
        s_screenShader,
        x,
        y,
        w,
        h,
        u0,
        v0,
        u1,
        v1
    );

    EndOverlay(prev);
}

void ScreenDraw::DrawQuad(tTexture* tex, f32 x, f32 y, f32 w, f32 h,
                          f32 u0, f32 v0, f32 u1, f32 v1,
                          u8 r, u8 g, u8 b, u8 a) {
    if (s_overlayBatchDepth > 0) {
        EnsureShader();
        QueueBatchedQuad(tex, x, y, w, h, u0, v0, u1, v1, r, g, b, a);
        return;
    }

    Mat4 prev = BeginOverlay();
    p3d::context->SetBlendMode(PDDI_BLEND_ALPHA);

    QueueBatchedQuad(tex, x, y, w, h, u0, v0, u1, v1, r, g, b, a);
    FlushPendingPrimitiveBatch();

    EndOverlay(prev);
}

void ScreenDraw::DrawShaderQuad(pddiBaseShader* shader, f32 x, f32 y, f32 w, f32 h,
                                f32 u0, f32 v0, f32 u1, f32 v1,
                                pddiBlendMode blendMode,
                                f32 canvasW, f32 canvasH) {
    if (!shader) {
        return;
    }

    if (s_overlayBatchDepth > 0) {
        FlushPendingPrimitiveBatch();
    }

    Mat4 prev = BeginOverlay(canvasW, canvasH);
    p3d::context->SetBlendMode(blendMode);
    p3d::context->DrawQuad(shader, x, y, w, h, u0, v0, u1, v1);
    EndOverlay(prev);
}

void ScreenDraw::DrawColoredQuad(u8 r, u8 g, u8 b, u8 a) {
    DrawColoredRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, r, g, b, a);
}

void ScreenDraw::DrawCircle(tTexture* tex,
                            f32 centerX, f32 centerY,
                            f32 radiusX, f32 radiusY,
                            f32 thickness,
                            f32 u0, f32 v0, f32 u1, f32 v1,
                            s32 segments, u8 r, u8 g, u8 b, u8 a) {
    if (s_overlayBatchDepth > 0) {
        EnsureShader();
        QueueBatchedCircle(tex,
                           centerX, centerY,
                           radiusX, radiusY,
                           thickness,
                           u0, v0, u1, v1,
                           segments,
                           r, g, b, a);
        return;
    }

    Mat4 prev = BeginOverlay();
    p3d::context->SetBlendMode(PDDI_BLEND_ALPHA);

    QueueBatchedCircle(tex,
                       centerX, centerY,
                       radiusX, radiusY,
                       thickness,
                       u0, v0, u1, v1,
                       segments,
                       r, g, b, a);
    FlushPendingPrimitiveBatch();

    EndOverlay(prev);
}

void ScreenDraw::DrawCircle(f32 centerX, f32 centerY,
                            f32 radiusX, f32 radiusY,
                            f32 thickness,
                            f32 u0, f32 v0, f32 u1, f32 v1,
                            s32 segments, u8 r, u8 g, u8 b, u8 a) {
    DrawCircle(nullptr,
               centerX, centerY,
               radiusX, radiusY,
               thickness,
               u0, v0, u1, v1,
               segments,
               r, g, b, a);
}

void ScreenDraw::DrawFilledCircle(tTexture* tex,
                                  f32 centerX, f32 centerY,
                                  f32 radiusX, f32 radiusY,
                                  f32 u0, f32 v0, f32 u1, f32 v1,
                                  s32 segments, u8 r, u8 g, u8 b, u8 a) {
    if (s_overlayBatchDepth > 0) {
        EnsureShader();
        QueueBatchedFilledCircle(tex,
                                 centerX, centerY,
                                 radiusX, radiusY,
                                 u0, v0, u1, v1,
                                 segments,
                                 r, g, b, a);
        return;
    }

    Mat4 prev = BeginOverlay();
    p3d::context->SetBlendMode(PDDI_BLEND_ALPHA);

    QueueBatchedFilledCircle(tex,
                             centerX, centerY,
                             radiusX, radiusY,
                             u0, v0, u1, v1,
                             segments,
                             r, g, b, a);
    FlushPendingPrimitiveBatch();

    EndOverlay(prev);
}

void ScreenDraw::DrawFilledCircle(f32 centerX, f32 centerY,
                                  f32 radiusX, f32 radiusY,
                                  f32 u0, f32 v0, f32 u1, f32 v1,
                                  s32 segments, u8 r, u8 g, u8 b, u8 a) {
    DrawFilledCircle(nullptr,
                     centerX, centerY,
                     radiusX, radiusY,
                     u0, v0, u1, v1,
                     segments,
                     r, g, b, a);
}

void ScreenDraw::DrawColoredRect(f32 x, f32 y, f32 w, f32 h,
                                 u8 r, u8 g, u8 b, u8 a) {
    if (s_overlayBatchDepth > 0) {
        EnsureShader();
        QueueBatchedQuad(nullptr, x, y, w, h,
                         0.0f, 0.0f, 1.0f, 1.0f,
                         r, g, b, a);
        return;
    }

    Mat4 prev = BeginOverlay();
    p3d::context->SetBlendMode(PDDI_BLEND_ALPHA);

    QueueBatchedQuad(nullptr, x, y, w, h,
                     0.0f, 0.0f, 1.0f, 1.0f,
                     r, g, b, a);
    FlushPendingPrimitiveBatch();

    EndOverlay(prev);
}

void ScreenDraw::DrawGouraudQuad(f32 x0, f32 y0, u8 r0, u8 g0, u8 b0, u8 a0,
                                 f32 x1, f32 y1, u8 r1, u8 g1, u8 b1, u8 a1,
                                 f32 x2, f32 y2, u8 r2, u8 g2, u8 b2, u8 a2,
                                 f32 x3, f32 y3, u8 r3, u8 g3, u8 b3, u8 a3) {
#if defined(RC_PLATFORM_PSP)
    if (s_overlayBatchDepth > 0) {
        // The batch's untextured triangles: the same vertices in the order
        // the immediate path builds (0 1 2, 1 3 2) and the same colours (a
        // byte survives the /255 float round trip below), one draw instead of
        // one per quad.
        EnsureShader();
        EnsurePrimitiveBatchTexture(nullptr);
        QueueBatchedVertex(x0, y0, 0.0f, 0.0f, r0, g0, b0, a0);
        QueueBatchedVertex(x1, y1, 0.0f, 0.0f, r1, g1, b1, a1);
        QueueBatchedVertex(x2, y2, 0.0f, 0.0f, r2, g2, b2, a2);
        QueueBatchedVertex(x1, y1, 0.0f, 0.0f, r1, g1, b1, a1);
        QueueBatchedVertex(x3, y3, 0.0f, 0.0f, r3, g3, b3, a3);
        QueueBatchedVertex(x2, y2, 0.0f, 0.0f, r2, g2, b2, a2);
        return;
    }
#endif
    if (s_overlayBatchDepth > 0) {
        FlushPendingPrimitiveBatch();
    }

    Mat4 prev = BeginOverlay();
    p3d::context->SetBlendMode(PDDI_BLEND_ALPHA);

    p3d::context->DrawGouraudQuad(
        x0, y0, r0 / 255.0f, g0 / 255.0f, b0 / 255.0f, a0 / 255.0f,
        x1, y1, r1 / 255.0f, g1 / 255.0f, b1 / 255.0f, a1 / 255.0f,
        x2, y2, r2 / 255.0f, g2 / 255.0f, b2 / 255.0f, a2 / 255.0f,
        x3, y3, r3 / 255.0f, g3 / 255.0f, b3 / 255.0f, a3 / 255.0f);

    EndOverlay(prev);
}

void ScreenDraw::Shutdown() {
    if (s_screenShader) {
        s_screenShader->Release();
        s_screenShader = nullptr;
    }
}

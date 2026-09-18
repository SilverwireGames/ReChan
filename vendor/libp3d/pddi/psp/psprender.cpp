#include "pddi/psp/psprender.h"
#include "psp/psp_perf.h"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 272;
constexpr int kBufferWidth = 512;
constexpr int kVramWidth = 1024;
constexpr int kVramHeight = 512;
constexpr int kPageSize = 256;
// Keep the command list bounded to 1 MiB.  The native 480x272 frame fits in
// this list; allocating another MiB here costs more than it saves on the PSP's
// small user heap and makes streamed gameplay assets compete with rendering.
constexpr int kMaxDisplayListWords = 262144;

alignas(16) unsigned int g_displayList[kMaxDisplayListWords];
pspContext* g_activeContext = nullptr;
pspShader* g_activeShader = nullptr;

static int NextPowerOfTwo(int value) {
    int result = 1;
    while (result < value && result < 1024) result <<= 1;
    return result;
}

static u8 ToByte(float value) {
    if (value <= 1.0f) value *= 255.0f;
    return static_cast<u8>(std::clamp(value, 0.0f, 255.0f));
}

static u32 MakeColor(float r, float g, float b, float a = 1.0f) {
    return static_cast<u32>(ToByte(r))
        | (static_cast<u32>(ToByte(g)) << 8)
        | (static_cast<u32>(ToByte(b)) << 16)
        | (static_cast<u32>(ToByte(a)) << 24);
}

static u32 MakeColor(pddiColour c) {
    return static_cast<u32>(c.r)
        | (static_cast<u32>(c.g) << 8)
        | (static_cast<u32>(c.b) << 16)
        | (static_cast<u32>(c.a) << 24);
}

// PSX texture pages use bit 15 as STP/semi-transparency metadata, while the
// PSP GU 5551 format interprets bit 15 as the texel alpha bit.  The RGB
// payload is already 5:5:5, so the native conversion only needs to normalize
// alpha.  A zero RGB texel remains transparent; every visible texel becomes
// opaque and primitive blend state still controls PSX semi-transparency.
static u16 ConvertPsxTexelToGu5551(u16 source) {
    const u16 rgb = static_cast<u16>(source & 0x7FFFu);
    const bool visible = rgb != 0 || (source & 0x8000u) != 0;
    return visible ? static_cast<u16>(rgb | 0x8000u) : 0u;
}

static void LoadGumMatrix(int mode, const Mat4& source) {
    ScePspFMatrix4 matrix;
    std::memcpy(&matrix, source.Data(), sizeof(matrix));
    sceGumMatrixMode(mode);
    sceGumLoadMatrix(&matrix);
}

static void SetBlend(pddiBlendMode mode) {
    if (mode == PDDI_BLEND_NONE) {
        sceGuDisable(GU_BLEND);
        return;
    }

    sceGuEnable(GU_BLEND);
    switch (mode) {
        case PDDI_BLEND_ADD:
            // PSP GU has no GU_ONE factor; GU_FIX with all RGB bits set is
            // the native equivalent for a constant-one destination factor.
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0x00FFFFFF);
            break;
        case PDDI_BLEND_SUBTRACT:
            sceGuBlendFunc(GU_SUBTRACT, GU_SRC_ALPHA, GU_FIX, 0, 0x00FFFFFF);
            break;
        case PDDI_BLEND_PSX_QUARTER:
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x40404040, 0x40404040);
            break;
        case PDDI_BLEND_ALPHA:
        default:
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            break;
    }
}

static void ConfigureTexture(pspTexture* texture, bool pageTexture) {
    if (!texture || !texture->GetPixels()) {
        sceGuDisable(GU_TEXTURE_2D);
        return;
    }

    texture->Bind(0);
    sceGuEnable(GU_TEXTURE_2D);
    if (pageTexture) {
        // PDDI UVs for PSX materials are page pixels in the 0..256 range.
        sceGuTexScale(1.0f / 256.0f, 1.0f / 256.0f);
    }
    else {
        sceGuTexScale(1.0f, 1.0f);
    }
}

} // namespace

pspPrimBuffer::pspPrimBuffer(const pddiPrimBufferDesc& desc)
    : primType(desc.primType), vertexFormat(desc.vertexFormat) {
    vertices.resize(desc.vertexCount);
    tpages.resize(desc.vertexCount, -1.0f);
    cbas.resize(desc.vertexCount, 0.0f);
    indices.resize(desc.indexCount);
}

void pspPrimBuffer::SetVertexData(const void* data, u32 count) {
    if (!data) return;
    vertices.resize(count);
    tpages.resize(count, -1.0f);
    cbas.resize(count, 0.0f);

    const bool hasPosition = (vertexFormat & PDDI_V_POSITION) != 0;
    const bool hasColour = (vertexFormat & PDDI_V_COLOUR) != 0;
    const bool hasUv = (vertexFormat & PDDI_V_UV) != 0;
    const bool hasTexInfo = (vertexFormat & PDDI_V_TEXINFO) != 0;
    const std::size_t stride = (hasPosition ? 3u : 0u)
        + (hasColour ? 3u : 0u)
        + (hasUv ? 2u : 0u)
        + (hasTexInfo ? 2u : 0u);
    const float* source = static_cast<const float*>(data);
    for (u32 i = 0; i < count; ++i) {
        const float* input = source + static_cast<std::size_t>(i) * stride;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float r = 1.0f, g = 1.0f, b = 1.0f;
        float u = 0.0f, v = 0.0f, tpage = -1.0f, cba = 0.0f;
        if (hasPosition) {
            x = input[0]; y = input[1]; z = input[2]; input += 3;
        }
        if (hasColour) {
            r = input[0]; g = input[1]; b = input[2]; input += 3;
        }
        if (hasUv) {
            u = input[0]; v = input[1]; input += 2;
        }
        if (hasTexInfo) {
            tpage = input[0]; cba = input[1];
        }
        vertices[i] = {u, v, MakeColor(r, g, b), x, y, z};
        tpages[i] = tpage;
        cbas[i] = cba;
    }
}

void pspPrimBuffer::SetIndices(const u16* source, u32 count) {
    if (!source) return;
    // Keep dynamic PSP buffers allocation-free after their first warm-up.
    // assign() is correct, but its growth policy is implementation-defined
    // and can introduce avoidable heap churn when a sorted batch changes size.
    if (indices.capacity() < count) indices.reserve(count);
    indices.resize(count);
    std::copy(source, source + count, indices.begin());
}

pspTexture::~pspTexture() {
    if (pixels) {
        std::free(pixels);
        pixels = nullptr;
    }
}

void pspTexture::SetData(int w, int h, int bppIn, int alphaDepthIn, const void* rgba) {
    width = std::max(w, 1);
    height = std::max(h, 1);
    bpp = bppIn;
    alphaDepth = alphaDepthIn;
    storageWidth = NextPowerOfTwo(width);
    storageHeight = NextPowerOfTwo(height);

    const size_t bytes = static_cast<size_t>(storageWidth) * static_cast<size_t>(storageHeight) * sizeof(u16);
    if (!pixels) pixels = static_cast<u16*>(memalign(16, bytes));
    else {
        void* resized = std::realloc(pixels, bytes);
        if (resized) pixels = static_cast<u16*>(resized);
    }
    if (!pixels) return;

    std::memset(pixels, 0, bytes);
    if (rgba) {
        const auto* source = static_cast<const u32*>(rgba);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const u32 color = source[y * width + x];
                const u16 r = static_cast<u16>((color & 0xffu) >> 4);
                const u16 g = static_cast<u16>(((color >> 8) & 0xffu) >> 4);
                const u16 b = static_cast<u16>(((color >> 16) & 0xffu) >> 4);
                const u16 a = static_cast<u16>(((color >> 24) & 0xffu) >> 4);
                pixels[y * storageWidth + x] = static_cast<u16>(r | (g << 4) | (b << 8) | (a << 12));
            }
        }
    }
    dirty = true;
}

void pspTexture::SetNative4444Data(int w, int h, const u16* source) {
    width = std::max(w, 1);
    height = std::max(h, 1);
    bpp = 16;
    alphaDepth = 4;
    storageWidth = NextPowerOfTwo(width);
    storageHeight = NextPowerOfTwo(height);

    const size_t bytes = static_cast<size_t>(storageWidth) * static_cast<size_t>(storageHeight) * sizeof(u16);
    if (!pixels) pixels = static_cast<u16*>(memalign(16, bytes));
    else {
        void* resized = std::realloc(pixels, bytes);
        if (resized) pixels = static_cast<u16*>(resized);
    }
    if (!pixels) return;

    std::memset(pixels, 0, bytes);
    if (source) {
        for (int y = 0; y < height; ++y) {
            std::memcpy(pixels + static_cast<size_t>(y) * storageWidth,
                        source + static_cast<size_t>(y) * width,
                        static_cast<size_t>(width) * sizeof(u16));
        }
    }
    dirty = true;
}

void pspTexture::Bind(int) {
    if (!pixels) {
        sceGuDisable(GU_TEXTURE_2D);
        return;
    }
    if (dirty) {
        sceKernelDcacheWritebackRange(pixels,
            static_cast<unsigned int>(storageWidth * storageHeight * sizeof(u16)));
        dirty = false;
    }
    sceGuTexMode(GU_PSM_4444, 0, 0, 0);
    sceGuTexImage(0, storageWidth, storageHeight, storageWidth, pixels);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(filterMode == PDDI_FILTER_NONE ? GU_NEAREST : GU_LINEAR,
                   filterMode == PDDI_FILTER_NONE ? GU_NEAREST : GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

void pspShader::SetTexture(u32 param, pddiTexture* tex) {
    if (param < textures.size()) textures[param] = tex;
}

void pspShader::PreRender() {
    g_activeShader = this;
    if (g_activeContext) g_activeContext->ApplyShader(this);
}

pspDisplay::~pspDisplay() {
    sceGuTerm();
}

bool pspDisplay::InitDisplay(const pddiDisplayInit&) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    sceGuInit();
    sceGuStart(GU_DIRECT, g_displayList);
    sceGuDrawBuffer(GU_PSM_8888, reinterpret_cast<void*>(0), kBufferWidth);
    sceGuDispBuffer(kWidth, kHeight, reinterpret_cast<void*>(kBufferWidth * kHeight * 4), kBufferWidth);
    sceGuDepthBuffer(reinterpret_cast<void*>(kBufferWidth * kHeight * 8), kBufferWidth);
    sceGuOffset(2048 - kWidth / 2, 2048 - kHeight / 2);
    sceGuViewport(2048, 2048, kWidth, kHeight);
    sceGuDepthRange(0xffff, 0);
    sceGuScissor(0, 0, kWidth, kHeight);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuDepthMask(GU_FALSE);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuClearColor(0xff000000);
    sceGuClearDepth(0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    return true;
}

void pspDisplay::SwapBuffers() {
    if (!g_activeContext) return;
    g_activeContext->EndFrame();
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuSwapBuffers();
    sceDisplayWaitVblankStart();
}

void pspDisplay::GetVideoMode(int index, pddiVideoMode& mode) {
    if (index != 0) {
        mode = {};
        return;
    }
    mode.width = kWidth;
    mode.height = kHeight;
    mode.refreshRate = 60;
}

void pspDisplay::AddOverlayCallback(OverlayCallback cb) {
    if (cb) overlayCallbacks.push_back(cb);
}

void pspDisplay::RenderOverlay() {
    for (OverlayCallback callback : overlayCallbacks) {
        if (callback) callback();
    }
}

void pspGamepad::Poll() {
    SceCtrlData data{};
    if (sceCtrlReadBufferPositive(&data, 1) < 0) {
        connected = false;
        buttons = 0;
        axes.fill(0.0f);
        return;
    }
    connected = true;
    buttons = data.Buttons;
    axes[GamepadAxis::LeftX] = (static_cast<float>(data.Lx) - 128.0f) / 127.0f;
    axes[GamepadAxis::LeftY] = (static_cast<float>(data.Ly) - 128.0f) / 127.0f;
    if (std::abs(axes[GamepadAxis::LeftX]) < 0.12f) axes[GamepadAxis::LeftX] = 0.0f;
    if (std::abs(axes[GamepadAxis::LeftY]) < 0.12f) axes[GamepadAxis::LeftY] = 0.0f;

    if ((buttons & PSP_CTRL_START) && (buttons & PSP_CTRL_SELECT) && display) {
        display->RequestClose();
    }
}

bool pspGamepad::IsButtonDown(int button) const {
    static constexpr u32 masks[GamepadButton::COUNT] = {
        PSP_CTRL_CROSS, PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE, PSP_CTRL_TRIANGLE,
        PSP_CTRL_LTRIGGER, PSP_CTRL_RTRIGGER, PSP_CTRL_SELECT, PSP_CTRL_START,
        0, PSP_CTRL_L3, PSP_CTRL_R3, PSP_CTRL_UP, PSP_CTRL_RIGHT,
        PSP_CTRL_DOWN, PSP_CTRL_LEFT,
    };
    return button >= 0 && button < GamepadButton::COUNT && masks[button] != 0
        && (buttons & masks[button]) != 0;
}

float pspGamepad::GetAxis(int axis) const {
    if (axis < 0 || axis >= static_cast<int>(axes.size())) return 0.0f;
    return axes[axis];
}

pspContext::pspContext(pspDisplay* display) : display(display) {
    // Page decoding is a load-time operation. Reserve every cache slot here
    // so the first texture bind during gameplay cannot call memalign from the
    // render path and introduce a frame-time spike.
    for (auto& cache : pageCache) {
        cache.pixels = static_cast<u16*>(memalign(16, kPageSize * kPageSize * sizeof(u16)));
    }
}

pspContext::~pspContext() {
    for (auto& image : vramImages) {
        if (image.pixels) std::free(image.pixels);
        image = {};
    }
    for (auto& cache : pageCache) {
        if (cache.pixels) std::free(cache.pixels);
        cache = {};
    }
    if (g_activeContext == this) g_activeContext = nullptr;
}

void pspContext::BeginFrame() {
    g_activeContext = this;
    g_activeShader = nullptr;
    PspPerf::BeginFrame();
    sceGuStart(GU_DIRECT, g_displayList);
    Setup3DState();
}

void pspContext::EndFrame() {
    // The display owns the final sceGuFinish/swap sequence.  Keeping the list
    // open here lets the existing overlay callback draw into the same frame.
}

void pspContext::Setup3DState() {
    sceGuDisable(GU_TEXTURE_2D);
    SetBlend(blendMode);
    if (zBufferEnabled) sceGuEnable(GU_DEPTH_TEST);
    else sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuDepthMask(zBufferEnabled ? GU_FALSE : GU_TRUE);
    switch (cullMode == PDDI_CULL_NONE ? PDDI_CULL_NONE
                                       : (worldMirror ? PDDI_CULL_INVERTED : cullMode)) {
        case PDDI_CULL_NONE: sceGuDisable(GU_CULL_FACE); break;
        case PDDI_CULL_INVERTED: sceGuEnable(GU_CULL_FACE); sceGuFrontFace(GU_CCW); break;
        case PDDI_CULL_NORMAL: default: sceGuEnable(GU_CULL_FACE); sceGuFrontFace(GU_CW); break;
    }
}

void pspContext::Setup2DState() {
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDepthMask(GU_TRUE);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumOrtho(0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, -1.0f, 1.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
}

void pspContext::Clear(int flags) {
    const u32 color = MakeColor(clearColour);
    sceGuClearColor(color);
    sceGuClearDepth(0);
    int clearFlags = 0;
    if (flags & PDDI_BUFFER_COLOUR) clearFlags |= GU_COLOR_BUFFER_BIT;
    if (flags & PDDI_BUFFER_DEPTH) clearFlags |= GU_DEPTH_BUFFER_BIT;
    if (clearFlags) sceGuClear(clearFlags);
}

void pspContext::SetCullMode(pddiCullMode mode) {
    cullMode = mode;
    if (mode == PDDI_CULL_NONE) sceGuDisable(GU_CULL_FACE);
    else {
        sceGuEnable(GU_CULL_FACE);
        sceGuFrontFace(mode == PDDI_CULL_INVERTED ? GU_CCW : GU_CW);
    }
}

void pspContext::EnableZBuffer(bool enable) {
    zBufferEnabled = enable;
    if (enable) sceGuEnable(GU_DEPTH_TEST);
    else sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(enable ? GU_FALSE : GU_TRUE);
}

void pspContext::SetBlendMode(pddiBlendMode mode) {
    blendMode = mode;
    SetBlend(mode);
}

void pspContext::SetPolygonOffset(bool enable, f32 factor, f32 units) {
    // The GE has no desktop-style polygon offset state.  Keep the method in
    // the PDDI contract and ignore it; PSP's reversed depth test remains
    // deterministic without changing the global viewport offset.
    (void)enable;
    (void)factor;
    (void)units;
}

void pspContext::SetScissor(int x, int y, int w, int h) {
    const int right = std::clamp(x + w, 0, kWidth);
    const int bottom = std::clamp(y + h, 0, kHeight);
    sceGuScissor(std::clamp(x, 0, kWidth), std::clamp(y, 0, kHeight), right, bottom);
}

void pspContext::ApplyShader(const pspShader* shader) {
    shaderTexture = shader ? shader->GetTexture(0) : nullptr;
    if (shader) SetBlendMode(PDDI_BLEND_ALPHA);
}

void pspContext::DrawQuad(pddiBaseShader* shader, float x, float y, float w, float h,
                          float u0, float v0, float u1, float v1) {
    pspShader* psp = static_cast<pspShader*>(shader);
    Setup2DState();
    if (psp) psp->PreRender();
    PspVertex* vertices = static_cast<PspVertex*>(sceGuGetMemory(sizeof(PspVertex) * 6));
    if (!vertices) return;
    const u32 color = psp ? MakeColor(psp->GetDiffuse()) : 0xffffffffu;
    vertices[0] = {u0, v1, color, x, y + h, 0.0f};
    vertices[1] = {u1, v1, color, x + w, y + h, 0.0f};
    vertices[2] = {u1, v0, color, x + w, y, 0.0f};
    vertices[3] = vertices[0];
    vertices[4] = vertices[2];
    vertices[5] = {u0, v0, color, x, y, 0.0f};
    Draw2D(vertices, 6, psp ? psp->GetTexture() : shaderTexture);
}

void pspContext::DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                               const pddiBatchVertex* source, s32 vertCount) {
    if (!source || vertCount <= 0) return;
    Setup2DState();
    SetBlend(blend);
    PspVertex* vertices = static_cast<PspVertex*>(sceGuGetMemory(sizeof(PspVertex) * vertCount));
    if (!vertices) return;
    for (s32 i = 0; i < vertCount; ++i) {
        vertices[i] = { source[i].u, source[i].v,
                        MakeColor(pddiColour(source[i].r, source[i].g, source[i].b, source[i].a)),
                        source[i].x, source[i].y, 0.0f };
    }
    Draw2D(vertices, vertCount, tex);
}

void pspContext::Draw2D(const PspVertex* vertices, int count, pddiTexture* texture) {
    if (texture) ConfigureTexture(static_cast<pspTexture*>(texture), false);
    else sceGuDisable(GU_TEXTURE_2D);
    sceGumDrawArray(GU_TRIANGLES,
                   GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   count, nullptr, vertices);
    PspPerf::RecordDrawSubmission(static_cast<u32>(count));
}

void pspContext::DrawFilledCircle(pddiBaseShader* shader, float cx, float cy,
                                  float rx, float ry, float u0, float v0, float u1, float v1,
                                  int segments) {
    segments = std::clamp(segments, 3, 48);
    pspShader* psp = static_cast<pspShader*>(shader);
    if (psp) psp->PreRender();
    Setup2DState();
    PspVertex* vertices = static_cast<PspVertex*>(sceGuGetMemory(sizeof(PspVertex) * segments * 3));
    if (!vertices) return;
    const u32 color = psp ? MakeColor(psp->GetDiffuse()) : 0xffffffffu;
    constexpr float pi2 = 6.28318530718f;
    for (int i = 0; i < segments; ++i) {
        const float a0 = pi2 * static_cast<float>(i) / segments;
        const float a1 = pi2 * static_cast<float>(i + 1) / segments;
        const float uvx = (u0 + u1) * 0.5f;
        const float uvy = (v0 + v1) * 0.5f;
        const int o = i * 3;
        vertices[o + 0] = {uvx, uvy, color, cx, cy, 0.0f};
        vertices[o + 1] = {uvx + std::cos(a0) * (u1 - u0) * 0.5f,
                           uvy + std::sin(a0) * (v1 - v0) * 0.5f, color,
                           cx + std::cos(a0) * rx, cy + std::sin(a0) * ry, 0.0f};
        vertices[o + 2] = {uvx + std::cos(a1) * (u1 - u0) * 0.5f,
                           uvy + std::sin(a1) * (v1 - v0) * 0.5f, color,
                           cx + std::cos(a1) * rx, cy + std::sin(a1) * ry, 0.0f};
    }
    Draw2D(vertices, segments * 3, psp ? psp->GetTexture() : shaderTexture);
}

void pspContext::DrawCircle(pddiBaseShader* shader, float cx, float cy,
                            float rx, float ry, float thickness,
                            float u0, float v0, float u1, float v1, int segments) {
    if (thickness <= 0.0f) return;
    segments = std::clamp(segments, 3, 48);
    pspShader* psp = static_cast<pspShader*>(shader);
    if (psp) psp->PreRender();
    Setup2DState();
    PspVertex* vertices = static_cast<PspVertex*>(sceGuGetMemory(sizeof(PspVertex) * segments * 6));
    if (!vertices) return;
    const u32 color = psp ? MakeColor(psp->GetDiffuse()) : 0xffffffffu;
    constexpr float pi2 = 6.28318530718f;
    const float irx = std::max(0.0f, rx - thickness);
    const float iry = std::max(0.0f, ry - thickness);
    for (int i = 0; i < segments; ++i) {
        const float a0 = pi2 * static_cast<float>(i) / segments;
        const float a1 = pi2 * static_cast<float>(i + 1) / segments;
        const int o = i * 6;
        const float uvx = (u0 + u1) * 0.5f;
        const float uvy = (v0 + v1) * 0.5f;
        auto point = [&](float a, float px, float py, int n) {
            vertices[o + n] = {uvx + std::cos(a) * (u1 - u0) * 0.5f,
                               uvy + std::sin(a) * (v1 - v0) * 0.5f, color,
                               cx + std::cos(a) * px, cy + std::sin(a) * py, 0.0f};
        };
        point(a0, rx, ry, 0); point(a0, irx, iry, 1); point(a1, rx, ry, 2);
        point(a1, rx, ry, 3); point(a0, irx, iry, 4); point(a1, irx, iry, 5);
    }
    Draw2D(vertices, segments * 6, psp ? psp->GetTexture() : shaderTexture);
}

void pspContext::DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                                 float x1, float y1, float r1, float g1, float b1, float a1,
                                 float x2, float y2, float r2, float g2, float b2, float a2,
                                 float x3, float y3, float r3, float g3, float b3, float a3) {
    Setup2DState();
    sceGuDisable(GU_TEXTURE_2D);
    PspVertex* vertices = static_cast<PspVertex*>(sceGuGetMemory(sizeof(PspVertex) * 6));
    if (!vertices) return;
    vertices[0] = {0, 0, MakeColor(r0, g0, b0, a0), x0, y0, 0};
    vertices[1] = {0, 0, MakeColor(r1, g1, b1, a1), x1, y1, 0};
    vertices[2] = {0, 0, MakeColor(r2, g2, b2, a2), x2, y2, 0};
    vertices[3] = vertices[0]; vertices[4] = vertices[2];
    vertices[5] = {0, 0, MakeColor(r3, g3, b3, a3), x3, y3, 0};
    Draw2D(vertices, 6, nullptr);
}

pspContext::VramImage* pspContext::GetVram(u32 handle) {
    if (handle == 0 || handle > vramImages.size()) return nullptr;
    VramImage& image = vramImages[handle - 1];
    return image.pixels ? &image : nullptr;
}

u32 pspContext::CreateVRAMTexture(int w, int h, const u16* data) {
    for (u32 i = 0; i < vramImages.size(); ++i) {
        VramImage& image = vramImages[i];
        if (image.pixels) continue;
        image.width = w;
        image.height = h;
        image.pixels = static_cast<u16*>(memalign(16, static_cast<size_t>(w) * h * sizeof(u16)));
        if (!image.pixels) return 0;
        if (data) std::memcpy(image.pixels, data, static_cast<size_t>(w) * h * sizeof(u16));
        sceKernelDcacheWritebackRange(image.pixels, static_cast<unsigned int>(w * h * sizeof(u16)));
        return i + 1;
    }
    return 0;
}

void pspContext::DestroyVRAMTexture(u32 handle) {
    VramImage* image = GetVram(handle);
    if (!image) return;
    std::free(image->pixels);
    *image = {};
    for (auto& cache : pageCache) {
        if (cache.handle == handle) cache.handle = 0;
    }
}

void pspContext::UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) {
    VramImage* image = GetVram(handle);
    if (!image || !data || image->width != w || image->height != h) return;
    std::memcpy(image->pixels, data, static_cast<size_t>(w) * h * sizeof(u16));
    sceKernelDcacheWritebackRange(image->pixels, static_cast<unsigned int>(w * h * sizeof(u16)));
    for (auto& cache : pageCache) cache.handle = 0;
}

u16* pspContext::GetDecodedPage(u32 handle, u16 tpage, u16 cba) {
    VramImage* image = GetVram(handle);
    if (!image || image->width < kVramWidth || image->height < kVramHeight) return nullptr;
    for (auto& cache : pageCache) {
        if (cache.handle == handle && cache.tpage == tpage && cache.cba == cba) {
            cache.age = cacheAge++;
            return cache.pixels;
        }
    }

    PageCache* target = &pageCache[0];
    for (auto& cache : pageCache) {
        if (cache.handle == 0 || cache.age < target->age) target = &cache;
    }
    if (!target->pixels) return nullptr;

    PspPerf::RecordPageCacheMiss();

    const u32 tx = tpage & 0x0fu;
    const u32 ty = (tpage >> 4) & 1u;
    const u32 depth = (tpage >> 7) & 3u;
    const u32 pageX = tx * 64u;
    const u32 pageY = ty * 256u;
    const u32 clutX = (cba & 0x3fu) * 16u;
    const u32 clutY = (cba >> 6) & 0x1ffu;
    const u32 sourceY = pageY % static_cast<u32>(image->height);
    const u32 paletteY = clutY % static_cast<u32>(image->height);
    u16* const destination = target->pixels;

    // Decode the CLUT once per page instead of performing a modulo and a VRAM
    // lookup for every output pixel.  The table lives on the stack and is only
    // 512 bytes, so a page miss remains allocation-free on the PSP.
    if (depth == 0 || depth == 1) {
        const u32 paletteSize = depth == 0 ? 16u : 256u;
        u16 palette[256] = {};
        const u16* paletteSource = image->pixels + paletteY * image->width;
        for (u32 i = 0; i < paletteSize; ++i) {
            palette[i] = ConvertPsxTexelToGu5551(
                paletteSource[(clutX + i) % static_cast<u32>(image->width)]);
        }

        for (u32 y = 0; y < kPageSize; ++y) {
            const u16* sourceRow = image->pixels
                + ((sourceY + y) % static_cast<u32>(image->height)) * image->width;
            u16* destinationRow = destination + y * kPageSize;
            const bool packedRowInRange = pageX + (depth == 0 ? 64u : 128u)
                <= static_cast<u32>(image->width);
            if (depth == 0) {
                for (u32 x = 0; x < kPageSize; ++x) {
                    const u32 sourceX = pageX + (x >> 2);
                    const u16 word = sourceRow[packedRowInRange
                        ? sourceX : sourceX % static_cast<u32>(image->width)];
                    destinationRow[x] = palette[(word >> ((x & 3u) * 4u)) & 0x0fu];
                }
            }
            else {
                for (u32 x = 0; x < kPageSize; ++x) {
                    const u32 sourceX = pageX + (x >> 1);
                    const u16 word = sourceRow[packedRowInRange
                        ? sourceX : sourceX % static_cast<u32>(image->width)];
                    const u32 index = (x & 1u) ? (word >> 8) : (word & 0xffu);
                    destinationRow[x] = palette[index];
                }
            }
        }
    }
    else {
        // 16-bit pages keep the PSX 5:5:5 payload but still need their STP
        // bit normalized to GU alpha.  Convert in place while retaining the
        // wrapped fallback for an unusual page origin near the right edge of
        // the 1024-word VRAM.
        for (u32 y = 0; y < kPageSize; ++y) {
            const u16* sourceRow = image->pixels
                + ((sourceY + y) % static_cast<u32>(image->height)) * image->width;
            u16* destinationRow = destination + y * kPageSize;
            if (pageX + kPageSize <= static_cast<u32>(image->width)) {
                for (u32 x = 0; x < kPageSize; ++x) {
                    destinationRow[x] = ConvertPsxTexelToGu5551(sourceRow[pageX + x]);
                }
            }
            else {
                for (u32 x = 0; x < kPageSize; ++x) {
                    destinationRow[x] = ConvertPsxTexelToGu5551(
                        sourceRow[(pageX + x) % static_cast<u32>(image->width)]);
                }
            }
        }
    }
    target->handle = handle;
    target->tpage = tpage;
    target->cba = cba;
    target->age = cacheAge++;
    sceKernelDcacheWritebackRange(target->pixels, kPageSize * kPageSize * sizeof(u16));
    return target->pixels;
}

void pspContext::BindPage(float tpageFloat, float cbaFloat) {
    const u16 tpage = static_cast<u16>(std::max(tpageFloat, 0.0f));
    const u16 cba = static_cast<u16>(std::max(cbaFloat, 0.0f));
    u16* page = GetDecodedPage(vramHandle, tpage, cba);
    if (!page) {
        sceGuDisable(GU_TEXTURE_2D);
        return;
    }
    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    sceGuTexImage(0, kPageSize, kPageSize, kPageSize, page);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexScale(1.0f / 256.0f, 1.0f / 256.0f);
    sceGuEnable(GU_TEXTURE_2D);
}

void pspContext::DrawPrimBuffer(pddiPrimBuffer* base, u32 indexOffset, u32 indexCount) {
    // This device only manufactures pspPrimBuffer instances, and every
    // buffer reaching this context comes from that same pspDevice. Avoiding
    // RTTI here removes a type walk from every submitted draw call on the
    // 333 MHz CPU while preserving the backend's ownership invariant.
    auto* buffer = static_cast<pspPrimBuffer*>(base);
    if (!buffer || buffer->GetIndexCount() == 0) return;

    Setup3DState();
    LoadGumMatrix(GU_PROJECTION, projection);
    LoadGumMatrix(GU_VIEW, view);
    LoadGumMatrix(GU_MODEL, world);
    sceGumUpdateMatrix();

    const u32 total = buffer->GetIndexCount();
    u32 start = std::min(indexOffset, total);
    u32 end = indexCount ? std::min(start + indexCount, total) : total;
    if (end <= start) return;

    const PspVertex* vertices = buffer->GetVertices();
    const u16* indices = buffer->GetIndices();
    const bool isTriangles = buffer->GetPrimType() == PDDI_PRIM_TRIANGLES;
    while (start < end) {
        u32 runEnd = end;
        u32 materialKey = 0;
        if (isTriangles && vramHandle && start + 2 < end) {
            const u16 firstIndex = indices[start];
            const float firstTpage = texInfoOverride ? static_cast<float>(texInfoWord >> 16)
                                                     : buffer->GetTPage(firstIndex);
            const float firstCba = texInfoOverride ? static_cast<float>(texInfoWord & 0xffffu)
                                                   : buffer->GetCba(firstIndex);
            if (firstTpage >= 0.0f) {
                materialKey = (static_cast<u32>(static_cast<u16>(firstTpage)) << 16)
                    | static_cast<u16>(firstCba);
                for (u32 cursor = start + 3; cursor + 2 < end; cursor += 3) {
                    const u16 triIndex = indices[cursor];
                    const float tpage = texInfoOverride ? firstTpage : buffer->GetTPage(triIndex);
                    const float cba = texInfoOverride ? firstCba : buffer->GetCba(triIndex);
                    const u32 key = (static_cast<u32>(static_cast<u16>(tpage)) << 16)
                        | static_cast<u16>(cba);
                    if (key != materialKey) {
                        runEnd = cursor;
                        break;
                    }
                }
                BindPage(firstTpage, firstCba);
            }
            else {
                sceGuDisable(GU_TEXTURE_2D);
            }
        }
        else if (currentTexture && realTextureMode) {
            ConfigureTexture(static_cast<pspTexture*>(currentTexture), false);
        }
        else {
            sceGuDisable(GU_TEXTURE_2D);
        }

        int primitive = GU_TRIANGLES;
        switch (buffer->GetPrimType()) {
            case PDDI_PRIM_TRISTRIP: primitive = GU_TRIANGLE_STRIP; break;
            case PDDI_PRIM_LINES: primitive = GU_LINES; break;
            case PDDI_PRIM_LINESTRIP: primitive = GU_LINE_STRIP; break;
            case PDDI_PRIM_POINTS: primitive = GU_POINTS; break;
            case PDDI_PRIM_TRIANGLES: default: primitive = GU_TRIANGLES; break;
        }
        u16 maxIndex = 0;
        bool indicesValid = true;
        for (u32 cursor = start; cursor < runEnd; ++cursor) {
            const u16 index = indices[cursor];
            maxIndex = std::max(maxIndex, index);
            if (index >= buffer->GetVertexCount()) {
                indicesValid = false;
                break;
            }
        }
        char drawInfo[128] = {};
        std::snprintf(drawInfo, sizeof(drawInfo),
                      "gu draw: range=%lu-%lu count=%lu verts=%lu max=%u valid=%u",
                      static_cast<unsigned long>(start),
                      static_cast<unsigned long>(runEnd),
                      static_cast<unsigned long>(runEnd - start),
                      static_cast<unsigned long>(buffer->GetVertexCount()),
                      static_cast<unsigned>(maxIndex),
                      indicesValid ? 1u : 0u);
        PspPerf::Trace(drawInfo);
        if (!indicesValid) {
            PspPerf::Trace("gu draw: invalid indices dropped");
            start = runEnd;
            if (!isTriangles) break;
            continue;
        }

        // Expand indexed geometry into a fixed, aligned staging batch. The
        // PSP GE can read this RAM after a cache writeback, while the display
        // list remains reserved for commands and small 2D batches. Keeping
        // chunks triangle-aligned preserves material and primitive topology.
        constexpr u32 kStagingCapacity = 8192;
        u32 drawStart = start;
        while (drawStart < runEnd) {
            u32 chunkCount = std::min(kStagingCapacity, runEnd - drawStart);
            if (isTriangles) {
                chunkCount -= chunkCount % 3u;
            }
            if (chunkCount == 0) break;

            PspVertex* guVertices = expandedVertices.data();
            PspPerf::Trace("gu memory: copy begin");
            for (u32 cursor = 0; cursor < chunkCount; ++cursor) {
                guVertices[cursor] = vertices[indices[drawStart + cursor]];
            }
            PspPerf::Trace("gu memory: copy done");
            sceKernelDcacheWritebackRange(
                guVertices,
                static_cast<unsigned int>(sizeof(PspVertex) * chunkCount));
            PspPerf::Trace("gu draw: begin");
            sceGumDrawArray(primitive,
                           GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                           | GU_TRANSFORM_3D,
                           static_cast<int>(chunkCount),
                           nullptr, guVertices);
            PspPerf::Trace("gu draw: done");
            PspPerf::RecordDrawSubmission(chunkCount);
            drawStart += chunkCount;
        }
        start = runEnd;
        if (!isTriangles) break;
    }
}

pddiDevice* pddiCreate() {
    return new pspDevice();
}

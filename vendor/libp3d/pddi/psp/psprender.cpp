#include "pddi/psp/psprender.h"
#include "pc/log.h"
#include "psp/psp_perf.h"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <pspsysmem.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <tuple>

namespace {

#if defined(PSP_DISABLE_RENDER_OPTIMIZATIONS)
constexpr bool kCacheGuState = false; // A/B baseline with the same counters.
#else
constexpr bool kCacheGuState = true;
#endif
u32 g_guStateGeneration = 1;
u32 g_matrixGeneration = 1;
u32 g_guCapabilityValid = 0;
u32 g_guCapabilityEnabled = 0;

// Every write to these states in this backend goes through the same cache,
// including overlays and standalone textures. A list split preserves GE state;
// a new frame invalidates the cache conservatively. Never cache TexImage or
// ClutLoad by pointer: their memory may have changed after a completed fence.
static void ResetGuStateCache() {
    ++g_guStateGeneration;
    ++g_matrixGeneration;
    g_guCapabilityValid = 0;
}

static void GuCapability(int state, bool enable) {
    const u32 bit = 1u << static_cast<u32>(state);
    if (kCacheGuState && (g_guCapabilityValid & bit)
        && ((g_guCapabilityEnabled & bit) != 0) == enable) return;
    g_guCapabilityValid |= bit;
    if (enable) g_guCapabilityEnabled |= bit;
    else g_guCapabilityEnabled &= ~bit;
    PspPerf::CountRenderer(PspPerf::RendererCounter::GuStateWrites);
    if (enable) sceGuEnable(state);
    else sceGuDisable(state);
}
static void GuEnable(int state) { GuCapability(state, true); }
static void GuDisable(int state) { GuCapability(state, false); }

#define PSP_CACHED_GU(name, signature, arguments) \
    static void Gu##name signature { \
        using Args = decltype(std::make_tuple arguments); \
        static Args previous{}; \
        static u32 generation = 0; \
        const Args current = std::make_tuple arguments; \
        if (kCacheGuState && generation == g_guStateGeneration && current == previous) return; \
        previous = current; \
        generation = g_guStateGeneration; \
        PspPerf::CountRenderer(PspPerf::RendererCounter::GuStateWrites); \
        sceGu##name arguments; \
    }
PSP_CACHED_GU(AlphaFunc, (int f, int r, int m), (f, r, m))
PSP_CACHED_GU(AmbientColor, (u32 c), (c))
PSP_CACHED_GU(BlendFunc, (int op, int src, int dst, u32 sf, u32 df), (op, src, dst, sf, df))
PSP_CACHED_GU(DepthFunc, (int f), (f))
PSP_CACHED_GU(DepthMask, (int m), (m))
PSP_CACHED_GU(DepthOffset, (u32 o), (o))
PSP_CACHED_GU(FrontFace, (int f), (f))
PSP_CACHED_GU(TexMode, (int f, int max, int a, int swizzle), (f, max, a, swizzle))
PSP_CACHED_GU(TexFunc, (int f, int a), (f, a))
PSP_CACHED_GU(TexFilter, (int min, int mag), (min, mag))
PSP_CACHED_GU(TexWrap, (int u, int v), (u, v))
PSP_CACHED_GU(TexScale, (float u, float v), (u, v))
PSP_CACHED_GU(TexOffset, (float u, float v), (u, v))
PSP_CACHED_GU(ClutMode, (int f, int s, int m, int start), (f, s, m, start))
#undef PSP_CACHED_GU

#if defined(PSP_GU_DIAGNOSTIC)
#define PSP_GU_DIAG(...) Log::Get().LogMessage(__VA_ARGS__)
#else
#define PSP_GU_DIAG(...) ((void)0)
#endif

constexpr int kWidth = 480;
constexpr int kHeight = 272;
constexpr int kBufferWidth = 512;
constexpr int kVramWidth = 1024;
constexpr int kVramHeight = 512;
constexpr int kPageSize = 256;
constexpr u32 kEdramBytes = 2u * 1024u * 1024u;
constexpr u32 kFramebufferBytes = kBufferWidth * kHeight * 4u * 2u;
constexpr u32 kDepthBytes = kBufferWidth * kHeight * 2u;
constexpr u32 kReservedEdramBytes = kFramebufferBytes + kDepthBytes;
// Keep enough command-list headroom for a complete gameplay frame.  The
// original 1 MiB list was reached by the real hub benchmark and caused about
// five Finish/Sync fences per frame.  A 1.5 MiB resident list is still small
// beside the PSP's available user RAM and removes those avoidable GE drains.
constexpr int kMaxDisplayListWords = 393216;
// More than one complete state/matrix/texture setup plus FINISH/END. Check
// before a draw or setter, never after allocating data for an unsubmitted draw.
constexpr std::size_t kListCommandReserve = 4096;
constexpr u32 kDrawChunk = 8190; // even and divisible by three, below GE's 16-bit count
constexpr unsigned int kDisplayListGuard = 0xD15A1157u;
constexpr std::uintptr_t kDcacheLineMask = 64u - 1u;

static void WritebackDcacheRange(const void* address, std::size_t bytes) {
    if (!address || bytes == 0) return;
    const std::uintptr_t rawBegin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t begin = rawBegin & ~kDcacheLineMask;
    const std::uintptr_t end = (rawBegin + bytes + kDcacheLineMask) & ~kDcacheLineMask;
    sceKernelDcacheWritebackRange(reinterpret_cast<void*>(begin),
                                   static_cast<unsigned int>(end - begin));
}

// The PSP GU's native 2D sprite path is the most predictable path for
// glyphs: it uses the exact packed vertex layout used by the PSPSDK samples
// (16-bit texels and positions, 4444 vertex colour).  Keeping this separate
// from PspVertex also avoids asking the GE to decode float attributes for a
// batch that never needs them.
struct PspSpriteVertex {
    u16 u, v;
    u16 color;
    s16 x, y, z;
} __attribute__((packed));

struct DisplayListStorage {
    alignas(16) unsigned int words[kMaxDisplayListWords];
    unsigned int guard[4];
};

struct StaticPageSlot {
    void* pixels = nullptr;
    u32 format = 0;
    bool inUse = false;
};

struct StaticPspVram {
    void* frameBuffer = nullptr;
    void* displayBuffer = nullptr;
    void* depthBuffer = nullptr;
    std::array<StaticPageSlot, 4> t4{};
    std::array<StaticPageSlot, 4> t8{};
    std::array<StaticPageSlot, 2> rgba16{};
    bool initialized = false;
    u32 allocatedBytes = 0;
};

alignas(16) DisplayListStorage g_displayListStorage = {};
StaticPspVram g_staticVram = {};
pspContext* g_activeContext = nullptr;
pspShader* g_activeShader = nullptr;
// A direct indexed draw borrows a prim buffer's persistent vertex storage.
// Incrementing this generation after every GU fence gives those buffers a
// cheap, allocation-free way to tell whether a CPU mutation needs a flush.
u32 g_submissionSerial = 1;

static void ResetDisplayListGuard() {
    for (unsigned int& value : g_displayListStorage.guard) {
        value = kDisplayListGuard;
    }
}

static void CheckDisplayListGuard(const char* stage) {
    (void)stage;
    for (const unsigned int value : g_displayListStorage.guard) {
        if (value != kDisplayListGuard) {
            LOG("[PSP GU] display list guard corrupted at %s value=%08X",
                stage ? stage : "unknown", value);
            return;
        }
    }
}

template <std::size_t N>
static StaticPageSlot* AcquireStaticPageSlot(std::array<StaticPageSlot, N>& slots, u32 format) {
    for (auto& slot : slots) {
        if (slot.format == format && slot.pixels && !slot.inUse) {
            slot.inUse = true;
            return &slot;
        }
    }
    return nullptr;
}

[[maybe_unused]] static StaticPageSlot* AcquireStaticPageSlot(u32 format) {
    if (!g_staticVram.initialized) return nullptr;
    if (format == GU_PSM_T4) return AcquireStaticPageSlot(g_staticVram.t4, format);
    if (format == GU_PSM_T8) return AcquireStaticPageSlot(g_staticVram.t8, format);
    if (format == GU_PSM_5551) return AcquireStaticPageSlot(g_staticVram.rgba16, format);
    return nullptr;
}

template <std::size_t N>
static bool ReleaseStaticPageSlot(std::array<StaticPageSlot, N>& slots, void* pixels) {
    for (auto& slot : slots) {
        if (slot.pixels == pixels) {
            slot.inUse = false;
            return true;
        }
    }
    return false;
}

static void ReleaseStaticPageSlot(void* pixels) {
    if (!pixels) return;
    if (ReleaseStaticPageSlot(g_staticVram.t4, pixels)) return;
    if (ReleaseStaticPageSlot(g_staticVram.t8, pixels)) return;
    ReleaseStaticPageSlot(g_staticVram.rgba16, pixels);
}

static int NextPowerOfTwo(int value) {
    int result = 1;
    while (result < value && result < 1024) result <<= 1;
    return result;
}

static u8 ToByte(float value) {
    if (value <= 1.0f) value *= 255.0f;
    return static_cast<u8>(std::clamp(value, 0.0f, 255.0f));
}

// PSX primitive packets arrive through the shared geometry path as fixed
// point colour channels in the range 0..2 (the packet byte divided by 128).
// Values above one are still normalized fixed-point values, not literal PSP
// bytes. Keep this conversion local to the native backend so 0..255 UI colour
// callers retain the existing MakeColor behaviour.
static u8 ToPspVertexByte(float value) {
    // GU vertex channels cannot represent the shared shader's gain above 1.
    // Saturate it continuously: changing scale at 1 halves a channel exactly
    // when the CPU lighting makes it brighter (128/128 -> 129/128).
    return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

static u32 MakeGeometryColor(float r, float g, float b) {
    return static_cast<u32>(ToPspVertexByte(r))
        | (static_cast<u32>(ToPspVertexByte(g)) << 8)
        | (static_cast<u32>(ToPspVertexByte(b)) << 16)
        | 0xFF000000u;
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

static u16 MakeColor4444(u32 color) {
    return static_cast<u16>(((color >> 4) & 0x000Fu)
        | ((color >> 8) & 0x00F0u)
        | ((color >> 12) & 0x0F00u)
        | ((color >> 16) & 0xF000u));
}

static s16 ToSpriteCoord(float value) {
    return static_cast<s16>(std::clamp(
        static_cast<int>(std::lround(value)), -32768, 32767));
}

static u16 ToSpriteTexel(float value) {
    return static_cast<u16>(std::clamp(
        static_cast<int>(std::lround(value)), 0, 65535));
}

// PSX texture pages use bit 15 as STP/semi-transparency metadata, while the
// PSP GU 5551 format interprets bit 15 as the texel alpha bit.  The RGB
// payload is already 5:5:5, so the native conversion only needs to normalize
// alpha. Match the shared renderer: opaque pages key magenta, blended pages
// key index zero (CLUT) or word zero (direct colour), independent of STP.
static u16 ConvertPsxTexelToGu5551(u16 source, bool transparent) {
    const u16 rgb = static_cast<u16>(source & 0x7FFFu);
    return static_cast<u16>(rgb | (transparent ? 0u : 0x8000u));
}

static bool LoadGumMatrix(int mode, const Mat4& source) {
    static std::array<Mat4, 4> uploaded{};
    static std::array<u32, 4> generations{};
    if (kCacheGuState && generations[mode] == g_matrixGeneration
        && std::memcmp(uploaded[mode].Data(), source.Data(), sizeof(ScePspFMatrix4)) == 0) {
        return false;
    }
    uploaded[mode] = source;
    generations[mode] = g_matrixGeneration;
    PspPerf::CountRenderer(PspPerf::RendererCounter::MatrixUploads);
    ScePspFMatrix4 matrix;
    std::memcpy(&matrix, source.Data(), sizeof(matrix));
    if (mode == GU_PROJECTION) {
        // The engine supplies reversed Z in [0,1]; GE clips in [-1,1].
        // Remap only the GPU copy, preserving the engine's culling matrices.
        float* values = reinterpret_cast<float*>(&matrix);
        for (int column = 0; column < 4; ++column) {
            values[column * 4 + 2] = 2.0f * source.m[column * 4 + 2]
                - source.m[column * 4 + 3];
        }
    }
    sceGumMatrixMode(mode);
    sceGumLoadMatrix(&matrix);
    return true;
}

static void SetBlend(pddiBlendMode mode) {
    if (mode == PDDI_BLEND_NONE) {
        GuDisable(GU_BLEND);
        return;
    }

    GuEnable(GU_BLEND);
    switch (mode) {
        case PDDI_BLEND_ADD:
            // PSP GU has no GU_ONE factor; GU_FIX with all RGB bits set is
            // the native equivalent for a constant-one destination factor.
            GuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0x00FFFFFF);
            break;
        case PDDI_BLEND_SUBTRACT:
            // PDDI/PSX shadows darken the destination by the source shade.
            GuBlendFunc(GU_REVERSE_SUBTRACT, GU_SRC_ALPHA, GU_FIX, 0, 0x00FFFFFF);
            break;
        case PDDI_BLEND_PSX_QUARTER:
            GuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x40404040, 0x40404040);
            break;
        case PDDI_BLEND_ALPHA:
        default:
            GuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            break;
    }
}

static void ConfigureTexture(pspTexture* texture, bool pageTexture) {
    if (!texture || !texture->GetPixels()) {
        GuDisable(GU_TEXTURE_2D);
        return;
    }

    texture->Bind(0);
    GuEnable(GU_TEXTURE_2D);
    if (pageTexture) {
        // PDDI UVs for PSX materials are page pixels in the 0..256 range.
        GuTexScale(1.0f / 256.0f, 1.0f / 256.0f);
    }
    else {
        // The GE expects texture coordinates in texels.  The regular PDDI
        // geometry path already supplies texel coordinates; the 2D helper
        // path converts its normalized coordinates in Draw2D below.
        GuTexScale(1.0f, 1.0f);
    }
}

static void Convert2DUvToTexels(PspVertex* vertices, int count, pddiTexture* texture) {
    if (!vertices || count <= 0 || !texture) return;

    auto* nativeTexture = static_cast<pspTexture*>(texture);
    const float width = static_cast<float>(nativeTexture->GetWidth());
    const float height = static_cast<float>(nativeTexture->GetHeight());
    for (int i = 0; i < count; ++i) {
        vertices[i].u *= width;
        vertices[i].v *= height;
    }
}

} // namespace

pspPrimBuffer::pspPrimBuffer(const pddiPrimBufferDesc& desc)
    : primType(desc.primType), vertexFormat(desc.vertexFormat) {
    vertices.resize(desc.vertexCount);
    materialKeys.resize(desc.vertexCount, 0u);
    materialValid.resize(desc.vertexCount, 0u);
    indices.resize(desc.indexCount);
}

pspPrimBuffer::~pspPrimBuffer() {
    PrepareForMutation();
    if (packedVerticesAllocatedBytes != 0) {
        PspPerf::AddRendererMemory(-static_cast<s32>(packedVerticesAllocatedBytes));
    }
}

void pspPrimBuffer::RebuildPackedVertices() {
    packedVerticesValid = false;
    if (vertices.empty()
        || (vertexFormat & (PDDI_V_UV | PDDI_V_TEXINFO)) != (PDDI_V_UV | PDDI_V_TEXINFO)) {
        return;
    }

    const auto packTexCoord = [](float value, s16* out) {
        if (!std::isfinite(value)) return false;
        const float rounded = std::round(value);
        if (value != rounded || rounded < -32768.0f || rounded > 32767.0f) {
            return false;
        }
        *out = static_cast<s16>(rounded);
        return true;
    };

    // Validate before reserving so real-texture buffers with fractional UVs
    // do not retain a needless second vertex array after falling back.
    for (const PspVertex& source : vertices) {
        s16 u = 0, v = 0;
        if (!packTexCoord(source.u, &u) || !packTexCoord(source.v, &v)) {
            return;
        }
    }

    if (packedVertices.capacity() < vertices.size()) {
        packedVertices.reserve(vertices.size());
    }
    packedVertices.resize(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const PspVertex& source = vertices[i];
        PspPackedVertex& target = packedVertices[i];
        target.u = static_cast<s16>(std::lround(source.u));
        target.v = static_cast<s16>(std::lround(source.v));
        target.color = source.color;
        target.x = source.x;
        target.y = source.y;
        target.z = source.z;
    }

    const std::size_t allocatedBytes = packedVertices.capacity() * sizeof(PspPackedVertex);
    if (allocatedBytes > packedVerticesAllocatedBytes) {
        PspPerf::AddRendererMemory(static_cast<s32>(allocatedBytes - packedVerticesAllocatedBytes));
    }
    packedVerticesAllocatedBytes = allocatedBytes;
    WritebackDcacheRange(packedVertices.data(), packedVertices.size() * sizeof(PspPackedVertex));
    packedVerticesValid = true;
}

bool pspPrimBuffer::UsesStagedIndexedVertices() const {
    return preferStagedIndexedVertices
        || (stagedIndexedVertices && stagedIndexedSerial == g_submissionSerial);
}

void pspPrimBuffer::PrepareForMutation() {
    if (directSubmissionSerial != 0 && directSubmissionSerial == g_submissionSerial
        && g_activeContext) {
        g_activeContext->FlushList();
    }
    directSubmissionSerial = 0;
}

void pspPrimBuffer::MarkDirectSubmission() {
    directSubmissionSerial = g_submissionSerial;
}

bool pspPrimBuffer::SetDynamicVertexData(const void* data, u32 count,
                                         bool uvIsIntegral) {
    // Model skinning supplies the canonical position/colour/UV/tpage/CBA
    // layout.  Keep this fast path deliberately strict; unexpected formats
    // fall back to SetVertexData rather than silently changing semantics.
    constexpr u32 kDynamicFormat = PDDI_V_POSITION | PDDI_V_COLOUR
        | PDDI_V_UV | PDDI_V_TEXINFO;
    if (!data || count != vertices.size() || vertexFormat != kDynamicFormat) {
        return false;
    }

    // Staged dynamic buffers are copied by DrawPrimBuffer into the current
    // list before submission; the GE never reads these persistent arrays.
    // Avoid flushing their CPU cache lines and keep the writeback for the
    // direct indexed path, where the GE borrows the persistent storage.
    const bool sourceWillBeStaged = UsesStagedIndexedVertices();
    PrepareForMutation();
    const float* source = static_cast<const float*>(data);
    bool packedUvValid = packedVerticesValid;
    for (u32 i = 0; i < count; ++i) {
        const float* input = source + static_cast<std::size_t>(i) * 10u;
        PspVertex& target = vertices[i];
        target.x = input[0];
        target.y = input[1];
        target.z = input[2];
        target.color = MakeGeometryColor(input[3], input[4], input[5]);
        target.u = input[6];
        target.v = input[7];
        const float tpage = input[8];
        const float cba = input[9];

        const bool materialIsValid = std::isfinite(tpage) && tpage >= 0.0f
            && tpage <= 65535.0f && std::isfinite(cba) && cba >= 0.0f
            && cba <= 65535.0f;
        materialValid[i] = materialIsValid ? 1u : 0u;
        materialKeys[i] = materialIsValid
            ? (static_cast<u32>(static_cast<u16>(tpage)) << 16)
                | static_cast<u16>(cba)
            : 0u;

        if (!packedUvValid) continue;
        s16 u = 0;
        s16 v = 0;
        if (uvIsIntegral) {
            // The character skinning path sources UVs from u8 PSX packet
            // fields and only changes material lanes during suit animation.
            // Keep this contract local to that call site: it removes two
            // round/isfinite pairs per vertex without weakening the generic
            // real-texture/fractional-UV path below.
            u = static_cast<s16>(target.u);
            v = static_cast<s16>(target.v);
        }
        else {
            const float roundedU = std::round(target.u);
            const float roundedV = std::round(target.v);
            if (!std::isfinite(target.u) || !std::isfinite(target.v)
                || target.u != roundedU || target.v != roundedV
                || roundedU < -32768.0f || roundedU > 32767.0f
                || roundedV < -32768.0f || roundedV > 32767.0f) {
                packedUvValid = false;
                continue;
            }
            u = static_cast<s16>(roundedU);
            v = static_cast<s16>(roundedV);
        }
        PspPackedVertex& packed = packedVertices[i];
        packed.u = u;
        packed.v = v;
        packed.color = target.color;
        packed.x = target.x;
        packed.y = target.y;
        packed.z = target.z;
    }
    packedVerticesValid = packedUvValid;
    if (!sourceWillBeStaged) {
        WritebackDcacheRange(vertices.data(), sizeof(PspVertex) * vertices.size());
        if (packedVerticesValid) {
            WritebackDcacheRange(packedVertices.data(),
                sizeof(PspPackedVertex) * packedVertices.size());
        }
    }
    return true;
}

void pspPrimBuffer::SetVertexData(const void* data, u32 count) {
    if (!data) return;
    if (directSubmissionSerial != 0 && directSubmissionSerial == g_submissionSerial) {
        stagedIndexedVertices = true;
        stagedIndexedSerial = g_submissionSerial;
        preferStagedIndexedVertices = true;
    }
    PrepareForMutation();
#if !defined(NDEBUG)
    static u32 setVertexProbe = 0;
    const u32 probe = setVertexProbe++;
    if (probe < 12u) {
        LOG("[PSP Prim] vertex begin #%lu this=%p count=%lu capacity=%lu",
            static_cast<unsigned long>(probe), static_cast<void*>(this),
            static_cast<unsigned long>(count), static_cast<unsigned long>(vertices.capacity()));
    }
#endif
    vertices.resize(count);
    materialKeys.resize(count, 0u);
    materialValid.resize(count, 0u);
    RevalidateIndices();
#if !defined(NDEBUG)
    if (probe < 12u) LOG("[PSP Prim] vertex resized #%lu", static_cast<unsigned long>(probe));
#endif

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
        vertices[i] = {u, v, MakeGeometryColor(r, g, b), x, y, z};
        const bool materialIsValid = hasTexInfo
            && std::isfinite(tpage) && tpage >= 0.0f && tpage <= 65535.0f
            && std::isfinite(cba) && cba >= 0.0f && cba <= 65535.0f;
        materialValid[i] = materialIsValid ? 1u : 0u;
        materialKeys[i] = materialIsValid
            ? (static_cast<u32>(static_cast<u16>(tpage)) << 16)
                | static_cast<u16>(cba)
            : 0u;
    }
    WritebackDcacheRange(vertices.data(), sizeof(PspVertex) * vertices.size());
    RebuildPackedVertices();
#if !defined(NDEBUG)
    if (probe < 12u) LOG("[PSP Prim] vertex done #%lu", static_cast<unsigned long>(probe));
#endif
}

bool pspPrimBuffer::PatchDynamicAttributes(const u8* rawData, u32 rawSize,
                                           const u32* uvOffsets,
                                           const u32* colourOffsets,
                                           u32 count, bool updateUv,
                                           bool updateColour) {
    if (!rawData || count != vertices.size()
        || (updateUv && !uvOffsets) || (updateColour && !colourOffsets)) {
        return false;
    }

    constexpr u32 kInvalidOffset = 0xFFFFFFFFu;
    // Validate the complete patch before touching the persistent vertex array.
    // A failed map must fall back to a full rebuild rather than leave a mixed
    // frame with only part of its animated attributes refreshed.
    for (u32 i = 0; i < count; ++i) {
        if (updateUv && uvOffsets[i] != kInvalidOffset
            && (uvOffsets[i] + 1u >= rawSize)) {
            return false;
        }
        if (updateColour && (colourOffsets[i] == kInvalidOffset
            || colourOffsets[i] + 2u >= rawSize)) {
            return false;
        }
    }

    if (directSubmissionSerial != 0 && directSubmissionSerial == g_submissionSerial) {
        stagedIndexedVertices = true;
        stagedIndexedSerial = g_submissionSerial;
        preferStagedIndexedVertices = true;
    }
    PrepareForMutation();
    for (u32 i = 0; i < count; ++i) {
        if (updateUv && uvOffsets[i] != kInvalidOffset) {
            const u32 offset = uvOffsets[i];
            vertices[i].u = static_cast<float>(rawData[offset + 0u]);
            vertices[i].v = static_cast<float>(rawData[offset + 1u]);
        }
        if (updateColour) {
            const u32 offset = colourOffsets[i];
            vertices[i].color = MakeGeometryColor(
                rawData[offset + 0u] / 128.0f,
                rawData[offset + 1u] / 128.0f,
                rawData[offset + 2u] / 128.0f);
        }
        if (packedVerticesValid) {
            if (updateUv && uvOffsets[i] != kInvalidOffset) {
                packedVertices[i].u = static_cast<s16>(std::clamp(
                    static_cast<int>(std::lround(vertices[i].u)), -32768, 32767));
                packedVertices[i].v = static_cast<s16>(std::clamp(
                    static_cast<int>(std::lround(vertices[i].v)), -32768, 32767));
            }
            if (updateColour) {
                packedVertices[i].color = vertices[i].color;
            }
        }
    }
    WritebackDcacheRange(vertices.data(), sizeof(PspVertex) * vertices.size());
    if (packedVerticesValid) {
        WritebackDcacheRange(packedVertices.data(),
            sizeof(PspPackedVertex) * packedVertices.size());
    }
    return true;
}

void pspPrimBuffer::SetIndices(const u16* source, u32 count) {
    if (!source) return;
    if (directSubmissionSerial != 0 && directSubmissionSerial == g_submissionSerial) {
        stagedIndexedVertices = true;
        stagedIndexedSerial = g_submissionSerial;
        preferStagedIndexedVertices = true;
    }
    PrepareForMutation();
#if !defined(NDEBUG)
    static u32 setIndexProbe = 0;
    const u32 probe = setIndexProbe++;
    if (probe < 12u) {
        LOG("[PSP Prim] index begin #%lu this=%p count=%lu capacity=%lu",
            static_cast<unsigned long>(probe), static_cast<void*>(this),
            static_cast<unsigned long>(count), static_cast<unsigned long>(indices.capacity()));
    }
#endif
    // Keep dynamic PSP buffers allocation-free after their first warm-up.
    // assign() is correct, but its growth policy is implementation-defined
    // and can introduce avoidable heap churn when a sorted batch changes size.
    if (indices.capacity() < count) indices.reserve(count);
    indices.resize(count);
    std::copy(source, source + count, indices.begin());
    RevalidateIndices();
    // The GE reads the persistent index stream asynchronously after this
    // method returns. Publish it once here; per-draw range write-backs are
    // otherwise paid again for every material run in every frame.
    WritebackDcacheRange(indices.data(), sizeof(u16) * indices.size());
#if !defined(NDEBUG)
    if (probe < 12u) LOG("[PSP Prim] index done #%lu", static_cast<unsigned long>(probe));
#endif
}

void pspPrimBuffer::RevalidateIndices() {
    indicesValid = true;
    const u32 vertexCount = static_cast<u32>(vertices.size());
    for (const u16 index : indices) {
        if (index >= vertexCount) {
            indicesValid = false;
            return;
        }
    }
}

void PreparePspPrimBufferForDynamicSubmission(pddiPrimBuffer* base) {
    if (!base) return;
    static_cast<pspPrimBuffer*>(base)->PreferStagedIndexedVertices();
}

bool UpdatePspPrimBufferDynamicVertices(pddiPrimBuffer* base,
                                         const void* data, u32 count,
                                         bool uvIsIntegral) {
    if (!base) return false;
    auto* buffer = static_cast<pspPrimBuffer*>(base);
    buffer->PreferStagedIndexedVertices();
    return buffer->SetDynamicVertexData(data, count, uvIsIntegral);
}

bool UpdatePspPrimBufferDynamic(pddiPrimBuffer* base, const u8* rawData,
                                u32 rawSize, const u32* uvOffsets,
                                const u32* colourOffsets, u32 vertexCount,
                                bool updateUv, bool updateColour) {
    if (!base) return false;
    return static_cast<pspPrimBuffer*>(base)->PatchDynamicAttributes(
        rawData, rawSize, uvOffsets, colourOffsets, vertexCount,
        updateUv, updateColour);
}

pspTexture::~pspTexture() {
    if (pixels && g_activeContext) g_activeContext->FlushList();
    if (pixels) {
        std::free(pixels);
        pixels = nullptr;
    }
    PspPerf::AddRendererMemory(-static_cast<s32>(allocatedBytes));
    allocatedBytes = 0;
}

void pspTexture::SetData(int w, int h, int bppIn, int alphaDepthIn, const void* rgba) {
    if (pixels && g_activeContext) g_activeContext->FlushList();
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
        if (!resized) return;
        pixels = static_cast<u16*>(resized);
    }
    if (!pixels) return;

    if (bytes > allocatedBytes) {
        PspPerf::AddRendererMemory(static_cast<s32>(bytes - allocatedBytes));
    }
    else if (allocatedBytes > bytes) {
        PspPerf::AddRendererMemory(-static_cast<s32>(allocatedBytes - bytes));
    }
    allocatedBytes = bytes;

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
#if !defined(NDEBUG)
    if (width == 512 && height == 512) {
        unsigned int nonZeroAlpha = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if ((pixels[y * storageWidth + x] & 0xF000u) != 0) {
                    ++nonZeroAlpha;
                }
            }
        }
        LOG("[PSP Texture] atlas alpha tex=%p nonZero=%u first=%04X",
            static_cast<void*>(pixels), nonZeroAlpha,
            static_cast<unsigned>(pixels[0]));
    }
#endif
    dirty = true;
}

void pspTexture::SetNative4444Data(int w, int h, const u16* source) {
    if (pixels && g_activeContext) g_activeContext->FlushList();
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
        if (!resized) return;
        pixels = static_cast<u16*>(resized);
    }
    if (!pixels) return;

    if (bytes > allocatedBytes) {
        PspPerf::AddRendererMemory(static_cast<s32>(bytes - allocatedBytes));
    }
    else if (allocatedBytes > bytes) {
        PspPerf::AddRendererMemory(-static_cast<s32>(allocatedBytes - bytes));
    }
    allocatedBytes = bytes;

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
        GuDisable(GU_TEXTURE_2D);
        return;
    }
    if (dirty) {
        WritebackDcacheRange(pixels,
            static_cast<std::size_t>(storageWidth) * storageHeight * sizeof(u16));
        dirty = false;
    }
    GuTexMode(GU_PSM_4444, 0, 0, 0);
    sceGuTexImage(0, storageWidth, storageHeight, storageWidth, pixels);
    // The atlas RGB channels are white and its alpha channel is glyph
    // coverage.  Keep the texture's alpha authoritative; the vertex colour
    // still controls the final blend colour for the regular 2D path.
    // Font atlases are white with coverage in alpha. Multiply by the glyph
    // colour/alpha so outlines, tint and popup/legal fades remain effective.
    GuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexEnvColor(0x00000000);
    GuTexOffset(0.0f, 0.0f);
    GuAmbientColor(0xffffffff);
    GuTexFilter(filterMode == PDDI_FILTER_NONE ? GU_NEAREST : GU_LINEAR,
                   filterMode == PDDI_FILTER_NONE ? GU_NEAREST : GU_LINEAR);
    GuTexWrap(GU_CLAMP, GU_CLAMP);
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
    ResetGuStateCache();
    // Own a bounded layout instead of the SDK's monotonic static allocator
    // (which has no capacity check/reset and returns valid offset zero).
    g_staticVram = {};
    const u32 edramBytes = static_cast<u32>(sceGeEdramGetSize());
    if (edramBytes < kReservedEdramBytes) return false;
    const auto edramBase = reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr());
    g_staticVram.frameBuffer = nullptr; // relative offset zero is valid
    g_staticVram.displayBuffer = reinterpret_cast<void*>(kBufferWidth * kHeight * 4u);
    g_staticVram.depthBuffer = reinterpret_cast<void*>(kFramebufferBytes);
    u32 offset = kReservedEdramBytes;
    const auto reservePages = [&](auto& slots, u32 format, u32 bytes) {
        for (auto& slot : slots) {
            slot.format = format;
            if (bytes <= edramBytes - offset) {
                // Textures and copy destinations take absolute GE addresses;
                // framebuffer commands above take relative eDRAM offsets.
                slot.pixels = reinterpret_cast<void*>(edramBase + offset);
                offset += bytes;
            }
        }
    };
    reservePages(g_staticVram.t4, GU_PSM_T4, kPageSize * kPageSize / 2u);
    reservePages(g_staticVram.t8, GU_PSM_T8, kPageSize * kPageSize);
    reservePages(g_staticVram.rgba16, GU_PSM_5551, kPageSize * kPageSize * 2u);
    g_staticVram.allocatedBytes = offset;
    g_staticVram.initialized = true;
    // The PSP SDK supports both next-HSync and next-VSync buffer changes.
    // Pin presentation to the LCD's native refresh boundary so the renderer
    // cannot tear when a frame finishes between scanlines.  The SDK swaps its
    // internal buffer pointers regardless of this setting, so the explicit
    // VBlank wait in SwapBuffers() remains part of the frame contract.
    guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_NEXTVSYNC);
    ResetDisplayListGuard();
    sceGuStart(GU_DIRECT, g_displayListStorage.words);
    sceGuDrawBuffer(GU_PSM_8888,
                    g_staticVram.initialized ? g_staticVram.frameBuffer : reinterpret_cast<void*>(0),
                    kBufferWidth);
    sceGuDispBuffer(kWidth, kHeight,
                    g_staticVram.initialized
                        ? g_staticVram.displayBuffer
                        : reinterpret_cast<void*>(kBufferWidth * kHeight * 4),
                    kBufferWidth);
    // Two 32-bit colour buffers consume 2 * (512 * 272 * 4).  The PSP depth
    // surface is 16-bit, so placing it immediately after those buffers keeps
    // the complete framebuffer layout below the 2 MiB eDRAM limit.  The old
    // *8 offset treated depth as 64-bit and overran VRAM, corrupting GE state.
    sceGuDepthBuffer(g_staticVram.initialized
                         ? g_staticVram.depthBuffer
                         : reinterpret_cast<void*>(kBufferWidth * kHeight * 4 * 2),
                     kBufferWidth);
    sceGuOffset(2048 - kWidth / 2, 2048 - kHeight / 2);
    sceGuViewport(2048, 2048, kWidth, kHeight);
    sceGuDepthRange(0, 0xffff);
    sceGuScissor(0, 0, kWidth, kHeight);
    GuEnable(GU_SCISSOR_TEST);
    GuEnable(GU_CLIP_PLANES);
    GuEnable(GU_DEPTH_TEST);
    GuDepthFunc(GU_GEQUAL);
    GuDepthMask(GU_FALSE);
    GuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuClearColor(0xff000000);
    sceGuClearDepth(0);
    sceGuFinish();
    CheckDisplayListGuard("init");
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    return true;
}

void pspDisplay::SwapBuffers() {
    if (!g_activeContext) return;
    g_activeContext->EndFrame();
    g_activeContext->FinishList();
    sceGuSwapBuffers();
    sceDisplayWaitVblankStart();
#if defined(PSP_STAGE_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP Frame] after swap");
#endif
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
#if defined(PSP_GU_DIAGNOSTIC)
    static u32 pollProbe = 0;
    if (pollProbe < 24u || (buttons & (PSP_CTRL_START | PSP_CTRL_SELECT))
        == (PSP_CTRL_START | PSP_CTRL_SELECT)) {
        Log::Get().LogMessage("[PSP Input] poll #%lu buttons=%08lX lx=%u ly=%u",
                              static_cast<unsigned long>(pollProbe++),
                              static_cast<unsigned long>(buttons),
                              static_cast<unsigned>(data.Lx),
                              static_cast<unsigned>(data.Ly));
    }
#endif
    axes[GamepadAxis::LeftX] = (static_cast<float>(data.Lx) - 128.0f) / 127.0f;
    axes[GamepadAxis::LeftY] = (static_cast<float>(data.Ly) - 128.0f) / 127.0f;
    if (std::abs(axes[GamepadAxis::LeftX]) < 0.12f) axes[GamepadAxis::LeftX] = 0.0f;
    if (std::abs(axes[GamepadAxis::LeftY]) < 0.12f) axes[GamepadAxis::LeftY] = 0.0f;

#if !defined(PSP_DISABLE_EXIT_COMBO)
    if ((buttons & PSP_CTRL_START) && (buttons & PSP_CTRL_SELECT) && display) {
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP Input] close combo detected buttons=%08lX",
                              static_cast<unsigned long>(buttons));
#endif
        display->RequestClose();
    }
#endif
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
    PspPerf::AddRendererMemory(static_cast<s32>(sizeof(g_displayListStorage.words)
#if !defined(PSP_ENABLE_NATIVE_INDEXED) || !defined(PSP_INDEXED_LIST_MEMORY)
        + sizeof(frameVertices)
#endif
#if !defined(PSP_INDEXED_LIST_MEMORY)
        + sizeof(frameIndices)
#endif
        + sizeof(framePalettes)
        + sizeof(clutCache)
        + sizeof(nativeRemap) + sizeof(nativeRemapStamp) + sizeof(nativeRemapSource)));
    VramImage& psxVram = vramImages[0];
    psxVram.width = kVramWidth;
    psxVram.height = kVramHeight;
    psxVram.pixels = static_cast<u16*>(memalign(16, kVramWidth * kVramHeight * sizeof(u16)));
    if (psxVram.pixels) {
        std::memset(psxVram.pixels, 0, kVramWidth * kVramHeight * sizeof(u16));
        PspPerf::AddRendererMemory(static_cast<s32>(kVramWidth * kVramHeight * sizeof(u16)));
    }
    // Page decoding is a load-time operation. Reserve every cache slot here
    // so the first texture bind during gameplay cannot call memalign from the
    // render path and introduce a frame-time spike.
    for (auto& cache : pageCache) {
        cache.pixels = static_cast<u16*>(memalign(16, kPageSize * kPageSize * sizeof(u16)));
        if (cache.pixels) {
            PspPerf::AddRendererMemory(static_cast<s32>(kPageSize * kPageSize * sizeof(u16)));
        }
    }
}

pspContext::~pspContext() {
    FinishList();
    for (auto& image : vramImages) {
        if (image.pixels) std::free(image.pixels);
        image = {};
    }
    for (auto& cache : pageCache) {
        ReleaseStaticPageSlot(cache.gpuPixels);
        if (cache.pixels) {
            std::free(cache.pixels);
            PspPerf::AddRendererMemory(-static_cast<s32>(kPageSize * kPageSize * sizeof(u16)));
        }
        cache = {};
    }
    PspPerf::AddRendererMemory(-static_cast<s32>(sizeof(g_displayListStorage.words)
#if !defined(PSP_ENABLE_NATIVE_INDEXED) || !defined(PSP_INDEXED_LIST_MEMORY)
        + sizeof(frameVertices)
#endif
#if !defined(PSP_INDEXED_LIST_MEMORY)
        + sizeof(frameIndices)
#endif
        + sizeof(framePalettes)
        + sizeof(clutCache)
        + sizeof(nativeRemap) + sizeof(nativeRemapStamp) + sizeof(nativeRemapSource)));
    if (g_activeContext == this) g_activeContext = nullptr;
}

void pspContext::BeginFrame() {
    PspPerf::Mark(PspPerf::RuntimeStage::FrameBegin);
    FinishList();
    g_activeContext = this;
    g_activeShader = nullptr;
    frameVertexUsed = 0;
    frameIndexUsed = 0;
    framePaletteUsed = 0;
    CheckDisplayListGuard("begin-before-reset");
    ResetDisplayListGuard();
    PspPerf::BeginFrame();
    ResetGuStateCache();
    gu3DStateReady = false;
    // A new direct list starts with a conservative texture key.  Keep the
    // key alive across 3D primitive buffers within this frame; Setup3DState
    // does not modify the texture registers, and invalidating it there turns
    // every otherwise identical material into a full page/CLUT bind.
    pageBindingValid = false;
    pageTexScaleValid = false;
#if defined(PSP_PERF_TELEMETRY)
    // Sample allocator/kernel state at most once per 60 presented frames.
    // Guard the arguments too: release must not execute diagnostic syscalls.
    static u32 memorySampleFrame = 0;
    if ((memorySampleFrame++ % 60u) == 0u) PspPerf::RecordMemorySnapshot(
        static_cast<u32>(sceKernelTotalFreeMemSize()),
        static_cast<u32>(sceKernelMaxFreeMemSize()),
        g_staticVram.allocatedBytes,
        kEdramBytes > g_staticVram.allocatedBytes ? kEdramBytes - g_staticVram.allocatedBytes : 0u);
#endif
    sceGuStart(GU_DIRECT, g_displayListStorage.words);
    listOpen = true;
    Setup3DState();
}

void pspContext::EndFrame() {
    // The display owns the final sceGuFinish/swap sequence.  Keeping the list
    // open here lets the existing overlay callback draw into the same frame.
}

void pspContext::FinishList() {
    if (!listOpen) return;
#if defined(PSP_STAGE_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP Frame] finish/sync list=%ld vertices=%lu indices=%lu cluts=%lu",
        static_cast<long>(sceGuCheckList()), static_cast<unsigned long>(frameVertexUsed),
        static_cast<unsigned long>(frameIndexUsed), static_cast<unsigned long>(framePaletteUsed));
#endif
    PspPerf::Mark(PspPerf::RuntimeStage::FinishBegin, sceGuCheckList(), frameVertexUsed);
    sceGuFinish();
    PspPerf::Mark(PspPerf::RuntimeStage::FinishDone);
    CheckDisplayListGuard("finish");
    PspPerf::Mark(PspPerf::RuntimeStage::SyncBegin);
    sceGuSync(0, 0);
    PspPerf::Mark(PspPerf::RuntimeStage::SyncDone);
    listOpen = false;
    // TEX_SYNC only orders GE transfers; only this CPU/GE fence retires RAM
    // sources, texture slots, CLUT snapshots and the command list itself.
    for (auto& cache : pageCache) cache.inFlight = false;
    for (auto& cache : clutCache) cache.inFlight = false;
    frameVertexUsed = frameIndexUsed = framePaletteUsed = 0;
    ++g_submissionSerial;
    if (g_submissionSerial == 0) g_submissionSerial = 1;
}

void pspContext::FlushList() {
    if (!listOpen) return;
    PspPerf::CountRenderer(PspPerf::RendererCounter::ListFlushes);
    FinishList();
    ResetDisplayListGuard();
    // GE state and GUM matrices survive a list boundary. Do not clear, swap,
    // reset material state, or wait for VBlank in the middle of a frame.
    sceGuStart(GU_DIRECT, g_displayListStorage.words);
    listOpen = true;
}

void pspContext::EnsureListSpace(std::size_t payloadBytes) {
    assert(payloadBytes + kListCommandReserve <= sizeof(g_displayListStorage.words));
    if (listOpen && static_cast<std::size_t>(sceGuCheckList()) + payloadBytes
        + kListCommandReserve > sizeof(g_displayListStorage.words)) {
        FlushList();
    }
}

void pspContext::EnsureDrawSpace(u32 vertexCount, u32 indexCount) {
    EnsureListSpace();
    const u32 vertexBase = (frameVertexUsed + 1u) & ~1u;
    const u32 indexBase = (frameIndexUsed + 7u) & ~7u;
    if (vertexCount > kFrameVertexCapacity - vertexBase
        || indexCount > kFrameIndexCapacity - indexBase
        || framePaletteUsed == kFramePaletteCapacity) {
        FlushList();
    }
}

void* pspContext::AllocateListMemory(std::size_t bytes) {
    // Callers reserve their entire draw (including all allocations) first.
    // Never restart here: earlier allocations may not have been submitted.
    if (!listOpen || bytes + 32u + 64u + static_cast<std::size_t>(sceGuCheckList())
        > sizeof(g_displayListStorage.words)) return nullptr;
    const auto raw = reinterpret_cast<std::uintptr_t>(sceGuGetMemory(static_cast<int>(bytes + 15u)));
    return reinterpret_cast<void*>((raw + 15u) & ~std::uintptr_t(15u));
}

void pspContext::Setup3DState() {
    if (gu3DStateReady) return;
    EnsureListSpace();
    // DrawPrimBuffer/Draw2D explicitly invalidate this key when they change
    // texture sources.  Preserve it here so consecutive 3D buffers can reuse
    // the same GU page and CLUT without replaying all texture registers.
    if (!pageBindingValid) GuDisable(GU_TEXTURE_2D);
    // 2D overlays and page rendering share the same GU state. Reset the
    // lighting/color-test state explicitly so a previous overlay cannot
    // multiply the next scene by a stale ambient or test value.
    GuDisable(GU_LIGHTING);
    GuDisable(GU_COLOR_TEST);
    GuDisable(GU_STENCIL_TEST);
    GuDisable(GU_COLOR_LOGIC_OP);
    GuDisable(GU_CLIP_PLANES);
    GuAmbientColor(0xffffffff);
    SetBlend(blendMode);
    if (zBufferEnabled) GuEnable(GU_DEPTH_TEST);
    else GuDisable(GU_DEPTH_TEST);
    GuDepthFunc(GU_GEQUAL);
    GuDepthMask(zBufferEnabled && blendMode == PDDI_BLEND_NONE ? GU_FALSE : GU_TRUE);
    GuDepthOffset(polygonOffsetEnabled ? polygonOffset : 0);
    switch (cullMode == PDDI_CULL_NONE ? PDDI_CULL_NONE
                                       : (worldMirror ? PDDI_CULL_INVERTED : cullMode)) {
        case PDDI_CULL_NONE: GuDisable(GU_CULL_FACE); break;
        case PDDI_CULL_INVERTED: GuEnable(GU_CULL_FACE); GuFrontFace(GU_CCW); break;
        case PDDI_CULL_NORMAL: default: GuEnable(GU_CULL_FACE); GuFrontFace(GU_CW); break;
    }
    gu3DStateReady = true;
}

void pspContext::Setup2DState() {
    EnsureListSpace();
    gu3DStateReady = false;
    pageBindingValid = false;
    ++g_matrixGeneration; // These direct GUM writes replace all 3D matrices.
    GuDisable(GU_DEPTH_TEST);
    GuDisable(GU_CULL_FACE);
    GuDisable(GU_ALPHA_TEST);
    GuDisable(GU_COLOR_TEST);
    GuDisable(GU_STENCIL_TEST);
    GuDisable(GU_COLOR_LOGIC_OP);
    GuDisable(GU_LIGHTING);
    GuDisable(GU_CLIP_PLANES);
    GuDepthMask(GU_TRUE);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumOrtho(0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, -1.0f, 1.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
}

void pspContext::Clear(int flags) {
    EnsureListSpace();
    const u32 color = MakeColor(clearColour);
    sceGuClearColor(color);
    sceGuClearDepth(0);
    int clearFlags = 0;
    if (flags & PDDI_BUFFER_COLOUR) clearFlags |= GU_COLOR_BUFFER_BIT;
    if (flags & PDDI_BUFFER_DEPTH) clearFlags |= GU_DEPTH_BUFFER_BIT;
    if (clearFlags) sceGuClear(clearFlags);
}

void pspContext::SetCullMode(pddiCullMode mode) {
    // World/block rendering sets the same cull mode for every primitive
    // buffer.  Once the 3D state is known to be current, avoid even the
    // list-space probe and the cached GU calls.  A 2D pass clears this key,
    // so the next 3D setter still re-emits the state when required.
    if (gu3DStateReady && cullMode == mode) return;
    EnsureListSpace();
    cullMode = mode;
    if (mode == PDDI_CULL_NONE) GuDisable(GU_CULL_FACE);
    else {
        GuEnable(GU_CULL_FACE);
        GuFrontFace(mode == PDDI_CULL_INVERTED ? GU_CCW : GU_CW);
    }
}

void pspContext::EnableZBuffer(bool enable) {
    if (gu3DStateReady && zBufferEnabled == enable) return;
    EnsureListSpace();
    zBufferEnabled = enable;
    if (enable) GuEnable(GU_DEPTH_TEST);
    else GuDisable(GU_DEPTH_TEST);
    GuDepthMask(enable && blendMode == PDDI_BLEND_NONE ? GU_FALSE : GU_TRUE);
}

void pspContext::SetBlendMode(pddiBlendMode mode) {
    if (gu3DStateReady && blendMode == mode) return;
    EnsureListSpace();
    blendMode = mode;
    SetBlend(mode);
    GuDepthMask(zBufferEnabled && mode == PDDI_BLEND_NONE ? GU_FALSE : GU_TRUE);
}

void pspContext::SetPolygonOffset(bool enable, f32 factor, f32 units) {
    // PSP exposes one fixed depth offset rather than the desktop slope/factor
    // pair.  The PDDI shadow path supplies a positive units value, so preserve
    // that term and use factor only for callers that omit units.
    u32 requestedOffset = 0;
    if (enable) {
        const f32 requested = std::fabs(units) > 0.0f ? units : factor;
        requestedOffset = static_cast<u32>(std::clamp(
            static_cast<int>(std::lround(requested)), 0, 65535));
    }

    if (gu3DStateReady && polygonOffsetEnabled == enable
        && polygonOffset == requestedOffset) return;

    EnsureListSpace();
    polygonOffsetEnabled = enable;
    polygonOffset = requestedOffset;
    GuDepthOffset(polygonOffsetEnabled ? polygonOffset : 0);
}

void pspContext::SetScissor(int x, int y, int w, int h) {
    EnsureListSpace();
    const int right = std::clamp(x + w, 0, kWidth);
    const int bottom = std::clamp(y + h, 0, kHeight);
    sceGuScissor(std::clamp(x, 0, kWidth), std::clamp(y, 0, kHeight), right, bottom);
}

void pspContext::ApplyShader(const pspShader* shader) {
    shaderTexture = shader ? shader->GetTexture(0) : nullptr;
    if (shader) SetBlendMode(PDDI_BLEND_ALPHA);
}

void pspContext::SetPageTextureScale(bool packedUv) {
    // GU 16-bit texture coordinates are normalized signed values. The PSX
    // page path historically supplies float texels and scales them by 1/256;
    // use the equivalent fixed-coordinate scale only while the compact UV
    // stream is active. Include pageBindingValid because a 2D/real-texture
    // pass may have changed GU state without changing this cache.
    if (pageTexScaleValid && pageBindingValid && pageTexScalePacked == packedUv) {
        return;
    }
    const float scale = packedUv ? (32767.0f / 256.0f) : (1.0f / 256.0f);
    GuTexScale(scale, scale);
    pageTexScaleValid = true;
    pageTexScalePacked = packedUv;
}

void pspContext::DrawVRAMQuad(u32 handle, u16 tpage, u16 cba,
                              float x, float y, float w, float h,
                              float u0, float v0, float u1, float v1, pddiColour color) {
    if (!listOpen || !GetVram(handle)) return;
    EnsureListSpace(sizeof(PspVertex) * 6u + 64u);
    const auto previousBlend = blendMode;
    const u32 previousHandle = vramHandle;
    vramHandle = handle;
    Setup2DState();
    SetBlendMode(PDDI_BLEND_ALPHA);
    // DecodePage's background semantics key zero words, including coloured
    // index zero and opaque STP black. Do not use the 3D index-zero key.
    BindPage(tpage, cba, true, false);
    SetPageTextureScale(false);
    // BindPage may fence/restart under cache pressure: allocate afterwards.
    auto* vertices = static_cast<PspVertex*>(AllocateListMemory(sizeof(PspVertex) * 6));
    if (vertices) {
        const u32 tint = MakeColor(color);
        vertices[0] = {u0, v1, tint, x, y + h, 0};
        vertices[1] = {u1, v1, tint, x + w, y + h, 0};
        vertices[2] = {u1, v0, tint, x + w, y, 0};
        vertices[3] = vertices[0];
        vertices[4] = vertices[2];
        vertices[5] = {u0, v0, tint, x, y, 0};
        // TRANSFORM_2D consumes texel coordinates directly, without GUM.
        sceGuDrawArray(GU_TRIANGLES,
            GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
            6, nullptr, vertices);
        PspPerf::RecordDrawSubmission(6);
    }
    vramHandle = previousHandle;
    SetBlendMode(previousBlend);
    Setup3DState();
}

void pspContext::DrawQuad(pddiBaseShader* shader, float x, float y, float w, float h,
                          float u0, float v0, float u1, float v1) {
    if (!listOpen) return;
    EnsureListSpace(sizeof(PspVertex) * 6u + 32u);
    pspShader* psp = static_cast<pspShader*>(shader);
    Setup2DState();
    if (psp) psp->PreRender();
    PspVertex* vertices = static_cast<PspVertex*>(AllocateListMemory(sizeof(PspVertex) * 6));
    if (!vertices) return;
    const u32 color = psp ? MakeColor(psp->GetDiffuse()) : 0xffffffffu;
    vertices[0] = {u0, v1, color, x, y + h, 0.0f};
    vertices[1] = {u1, v1, color, x + w, y + h, 0.0f};
    vertices[2] = {u1, v0, color, x + w, y, 0.0f};
    vertices[3] = vertices[0];
    vertices[4] = vertices[2];
    vertices[5] = {u0, v0, color, x, y, 0.0f};
    pddiTexture* texture = psp ? psp->GetTexture() : shaderTexture;
    Convert2DUvToTexels(vertices, 6, texture);
    Draw2D(vertices, 6, texture);
}

void pspContext::DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                               const pddiBatchVertex* source, s32 vertCount) {
    if (!listOpen || !source || vertCount <= 0) return;
    if (vertCount > static_cast<s32>(kDrawChunk)) {
        for (s32 offset = 0; offset < vertCount; offset += kDrawChunk) {
            DrawQuadBatch(tex, blend, source + offset,
                std::min(static_cast<s32>(kDrawChunk), vertCount - offset));
        }
        return;
    }
    EnsureListSpace((sizeof(PspVertex) + sizeof(PspSpriteVertex)) * vertCount + 64u);
    Setup2DState();
    SetBlend(blend);
    PspVertex* vertices = static_cast<PspVertex*>(AllocateListMemory(sizeof(PspVertex) * vertCount));
    if (!vertices) return;
    for (s32 i = 0; i < vertCount; ++i) {
        vertices[i] = { source[i].u, source[i].v,
                        MakeColor(pddiColour(source[i].r, source[i].g, source[i].b, source[i].a)),
                        source[i].x, source[i].y, 0.0f };
    }
    Convert2DUvToTexels(vertices, vertCount, tex);
#if !defined(NDEBUG)
    static unsigned int overlayBatchLogCount = 0;
    if (overlayBatchLogCount < 12) {
        ++overlayBatchLogCount;
        auto* nativeTexture = tex ? static_cast<pspTexture*>(tex) : nullptr;
        LOG("[PSP GU] 2D batch #%u verts=%ld tex=%p pixels=%p logical=%dx%d storage=%dx%d uv=%.2f,%.2f xy=%.2f,%.2f color=%08lX",
            overlayBatchLogCount,
            static_cast<long>(vertCount),
            static_cast<void*>(tex),
            nativeTexture ? static_cast<const void*>(nativeTexture->GetPixels()) : nullptr,
            nativeTexture ? nativeTexture->GetWidth() : 0,
            nativeTexture ? nativeTexture->GetHeight() : 0,
            nativeTexture ? nativeTexture->GetStorageWidth() : 0,
            nativeTexture ? nativeTexture->GetStorageHeight() : 0,
            static_cast<double>(vertices[0].u),
            static_cast<double>(vertices[0].v),
            static_cast<double>(vertices[0].x),
            static_cast<double>(vertices[0].y),
            static_cast<unsigned long>(vertices[0].color));
        if (nativeTexture && nativeTexture->GetAlphaDepth() >= 8) {
            const int sampleX = std::clamp(static_cast<int>(vertices[0].u), 0, nativeTexture->GetStorageWidth() - 1);
            const int sampleY = std::clamp(static_cast<int>(vertices[0].v), 0, nativeTexture->GetStorageHeight() - 1);
            unsigned int maxAlpha = 0;
            for (int sy = sampleY; sy < std::min(sampleY + 24, nativeTexture->GetStorageHeight()); ++sy) {
                for (int sx = sampleX; sx < std::min(sampleX + 24, nativeTexture->GetStorageWidth()); ++sx) {
                    maxAlpha = std::max(maxAlpha, static_cast<unsigned int>(nativeTexture->GetPixels()[sy * nativeTexture->GetStorageWidth() + sx] >> 12));
                }
            }
            LOG("[PSP GU] font sample texel=%04X at=%d,%d",
                static_cast<unsigned>(nativeTexture->GetPixels()[sampleY * nativeTexture->GetStorageWidth() + sampleX]),
                sampleX, sampleY);
            LOG("[PSP GU] font sample max alpha in 24x24=%u", maxAlpha);
        }
    }
#endif

    // Text glyphs are submitted as axis-aligned quads.  The GE's sprite path
    // is both more reliable for the alpha-only font atlas and cheaper than
    // expanding every glyph into two triangles; keep the generic triangle
    // path for all other overlays and for circles.
    auto* nativeTexture = tex ? static_cast<pspTexture*>(tex) : nullptr;
    if (nativeTexture && nativeTexture->GetAlphaDepth() >= 8 && (vertCount % 6) == 0) {
        const s32 spriteVertexCount = vertCount / 3;
        PspSpriteVertex* sprites = static_cast<PspSpriteVertex*>(
            AllocateListMemory(sizeof(PspSpriteVertex) * spriteVertexCount));
        if (!sprites) return;
        for (s32 i = 0, out = 0; i < vertCount; i += 6, out += 2) {
            const PspVertex& topLeft = vertices[i + 5];
            const PspVertex& bottomRight = vertices[i + 1];
            sprites[out] = {
                ToSpriteTexel(topLeft.u), ToSpriteTexel(topLeft.v),
                MakeColor4444(topLeft.color),
                ToSpriteCoord(topLeft.x), ToSpriteCoord(topLeft.y), 0
            };
            sprites[out + 1] = {
                ToSpriteTexel(bottomRight.u), ToSpriteTexel(bottomRight.v),
                MakeColor4444(bottomRight.color),
                ToSpriteCoord(bottomRight.x), ToSpriteCoord(bottomRight.y), 0
            };
        }
#if !defined(NDEBUG)
        static unsigned int spriteBatchLogCount = 0;
        if (spriteBatchLogCount < 4) {
            ++spriteBatchLogCount;
            LOG("[PSP GU] native sprite batch #%u sprites=%ld uv=%u,%u..%u,%u xy=%d,%d..%d,%d",
                spriteBatchLogCount,
                static_cast<long>(spriteVertexCount / 2),
                static_cast<unsigned>(sprites[0].u), static_cast<unsigned>(sprites[0].v),
                static_cast<unsigned>(sprites[1].u), static_cast<unsigned>(sprites[1].v),
                static_cast<int>(sprites[0].x), static_cast<int>(sprites[0].y),
                static_cast<int>(sprites[1].x), static_cast<int>(sprites[1].y));
        }
#endif
        ConfigureTexture(nativeTexture, false);
        // Use the direct GU submission used by the PSPSDK 2D sprite sample.
        // The GUM wrapper is for transformed geometry; this path already
        // contains final screen-space coordinates.
        sceGuDrawArray(GU_SPRITES,
                       GU_TEXTURE_16BIT | GU_COLOR_4444 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       spriteVertexCount, nullptr, sprites);
        PspPerf::RecordDrawSubmission(static_cast<u32>(spriteVertexCount));
        // The next submission may be 3D and can use a different texture
        // source.  Do not let the page-binding fast path inherit this 2D
        // state when the caller does not rebuild the full GU state first.
        pageBindingValid = false;
        gu3DStateReady = false;
        return;
    }
    Draw2D(vertices, vertCount, tex);
}

void pspContext::Draw2D(const PspVertex* vertices, int count, pddiTexture* texture) {
    if (texture) ConfigureTexture(static_cast<pspTexture*>(texture), false);
    else GuDisable(GU_TEXTURE_2D);

    // 2D vertices already contain final screen-space coordinates.  Submit
    // them through the direct GU path used by the PSPSDK samples so no GUM
    // matrix state from the previous 3D pass can leak into the overlay.
    sceGuDrawArray(GU_TRIANGLES,
                   GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   count, nullptr, vertices);
    PspPerf::RecordDrawSubmission(static_cast<u32>(count));
    // Draw2D changes the texture image/function state directly.  Invalidate
    // the key used by BindPage before a subsequent 3D submission.
    pageBindingValid = false;
    gu3DStateReady = false;
}

void pspContext::DrawFilledCircle(pddiBaseShader* shader, float cx, float cy,
                                  float rx, float ry, float u0, float v0, float u1, float v1,
                                  int segments) {
    if (!listOpen) return;
    segments = std::clamp(segments, 3, 48);
    EnsureListSpace(sizeof(PspVertex) * segments * 6u + 32u);
    pspShader* psp = static_cast<pspShader*>(shader);
    if (psp) psp->PreRender();
    Setup2DState();
    PspVertex* vertices = static_cast<PspVertex*>(AllocateListMemory(sizeof(PspVertex) * segments * 3));
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
    pddiTexture* texture = psp ? psp->GetTexture() : shaderTexture;
    Convert2DUvToTexels(vertices, segments * 3, texture);
    Draw2D(vertices, segments * 3, texture);
}

void pspContext::DrawCircle(pddiBaseShader* shader, float cx, float cy,
                            float rx, float ry, float thickness,
                            float u0, float v0, float u1, float v1, int segments) {
    if (thickness <= 0.0f) return;
    if (!listOpen) return;
    segments = std::clamp(segments, 3, 48);
    EnsureListSpace(sizeof(PspVertex) * segments * 6u + 32u);
    pspShader* psp = static_cast<pspShader*>(shader);
    if (psp) psp->PreRender();
    Setup2DState();
    PspVertex* vertices = static_cast<PspVertex*>(AllocateListMemory(sizeof(PspVertex) * segments * 6));
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
    pddiTexture* texture = psp ? psp->GetTexture() : shaderTexture;
    Convert2DUvToTexels(vertices, segments * 6, texture);
    Draw2D(vertices, segments * 6, texture);
}

void pspContext::DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                                 float x1, float y1, float r1, float g1, float b1, float a1,
                                 float x2, float y2, float r2, float g2, float b2, float a2,
                                 float x3, float y3, float r3, float g3, float b3, float a3) {
    if (!listOpen) return;
    EnsureListSpace(sizeof(PspVertex) * 6u + 32u);
    Setup2DState();
    GuDisable(GU_TEXTURE_2D);
    PspVertex* vertices = static_cast<PspVertex*>(AllocateListMemory(sizeof(PspVertex) * 6));
    if (!vertices) return;
    vertices[0] = {0, 0, MakeColor(r0, g0, b0, a0), x0, y0, 0};
    vertices[1] = {0, 0, MakeColor(r1, g1, b1, a1), x1, y1, 0};
    vertices[2] = {0, 0, MakeColor(r2, g2, b2, a2), x2, y2, 0};
    // PDDI supplies strip order: the second triangle shares edge (1, 2).
    vertices[3] = vertices[1];
    vertices[4] = {0, 0, MakeColor(r3, g3, b3, a3), x3, y3, 0};
    vertices[5] = vertices[2];
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
        // Slot 1 is reserved during context construction for the fixed-size
        // PSX VRAM image. Reusing that allocation avoids a 1 MiB memalign
        // after streamed level data has fragmented the PSP heap.
        if (image.pixels) {
            if (i == 0 && image.width == w && image.height == h) {
                InvalidatePages(i + 1);
                if (data) std::memcpy(image.pixels, data, static_cast<size_t>(w) * h * sizeof(u16));
                WritebackDcacheRange(image.pixels, static_cast<std::size_t>(w) * h * sizeof(u16));
                return i + 1;
            }
            continue;
        }
        image.width = w;
        image.height = h;
        image.pixels = static_cast<u16*>(memalign(16, static_cast<size_t>(w) * h * sizeof(u16)));
        if (!image.pixels) return 0;
        PspPerf::AddRendererMemory(static_cast<s32>(static_cast<size_t>(w) * h * sizeof(u16)));
        if (data) std::memcpy(image.pixels, data, static_cast<size_t>(w) * h * sizeof(u16));
        WritebackDcacheRange(image.pixels, static_cast<std::size_t>(w) * h * sizeof(u16));
        return i + 1;
    }
    return 0;
}

void pspContext::DestroyVRAMTexture(u32 handle) {
    VramImage* image = GetVram(handle);
    if (!image) return;
    const bool isReservedPsxVram = handle == 1 && image->width == kVramWidth
        && image->height == kVramHeight;
    if (!isReservedPsxVram) {
        std::free(image->pixels);
        PspPerf::AddRendererMemory(-static_cast<s32>(static_cast<size_t>(image->width)
            * image->height * sizeof(u16)));
        *image = {};
    }
    InvalidatePages(handle);
}

void pspContext::InvalidatePages(u32 handle) {
    if (clutBindingValid && lastClutHandle == handle) {
        clutBindingValid = false;
    }
    for (auto& cache : clutCache) {
        if (cache.valid && cache.handle == handle) cache.valid = false;
    }
    for (auto& cache : pageCache) {
        if (cache.handle == handle) cache.handle = 0;
    }
    // Retain invalidated storage/slots and pins: already queued draws still
    // reference that snapshot. GetDecodedPage reclaims it only after sync.
}

void pspContext::UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) {
    VramImage* image = GetVram(handle);
    if (!image || !data || image->width != w || image->height != h) return;
    std::memcpy(image->pixels, data, static_cast<size_t>(w) * h * sizeof(u16));
    InvalidatePages(handle);
}

void pspContext::UpdateVRAMTextureRegion(u32 handle, int textureW, int textureH,
                                         const u16* data, int x, int y,
                                         int w, int h) {
    VramImage* image = GetVram(handle);
    if (!image || !data || textureW != image->width || textureH != image->height ||
        w <= 0 || h <= 0) {
        return;
    }

    const int minX = std::max(0, x);
    const int minY = std::max(0, y);
    const int maxX = static_cast<int>(std::min<int64_t>(textureW, static_cast<int64_t>(x) + w));
    const int maxY = static_cast<int>(std::min<int64_t>(textureH, static_cast<int64_t>(y) + h));
    if (minX >= maxX || minY >= maxY) {
        return;
    }

    // A region update may touch only the CLUT row and therefore leave the
    // decoded page resident.  The GE palette is still stale in that case.
    if (clutBindingValid && lastClutHandle == handle) {
        clutBindingValid = false;
    }

    const size_t rowBytes = static_cast<size_t>(maxX - minX) * sizeof(u16);
    for (int row = minY; row < maxY; ++row) {
        u16* const destination = image->pixels + row * image->width + minX;
        const u16* const source = data + row * textureW + minX;
        std::memcpy(destination, source, rowBytes);
    }

    const auto overlaps = [](int aMin, int aMax, int bMin, int bMax) {
        return aMin < bMax && bMin < aMax;
    };

    for (auto& cache : pageCache) {
        if (cache.handle != handle) {
            continue;
        }

        const u32 depth = (cache.tpage >> 7) & 3u;
        const int pageX = static_cast<int>(cache.tpage & 0x0fu) * 64;
        const int pageY = static_cast<int>((cache.tpage >> 4) & 1u) * 256;
        const int pageWordsWide = depth == 0 ? 64 : (depth == 1 ? 128 : 256);
        const int pageEnd = pageX + pageWordsWide;
        const bool horizontal = overlaps(pageX, std::min(pageEnd, image->width), minX, maxX)
            || (pageEnd > image->width && overlaps(0, pageEnd - image->width, minX, maxX));
        if (horizontal && overlaps(pageY, pageY + kPageSize, minY, maxY)) {
            cache.handle = 0;
        }
        // Indexed texels do not depend on the CLUT. BindPage snapshots the
        // current palette, so palette-only updates must not evict T4/T8 pages.
    }
}

pspContext::PageCache* pspContext::GetDecodedPage(u32 handle, u16 tpage) {
    PspPerf::Mark(PspPerf::RuntimeStage::CacheBegin, tpage, handle);
#if defined(PSP_GU_DIAGNOSTIC)
    static u32 pageProbe = 0;
    const u32 pageId = pageProbe++;
    const bool probe = pageId < 32u;
    if (probe) {
        PSP_GU_DIAG("[PSP GU DIAG] page #%lu begin handle=%lu tpage=%04X",
            static_cast<unsigned long>(pageId), static_cast<unsigned long>(handle),
            static_cast<unsigned>(tpage));
    }
#endif
    tpage &= 0x019fu; // address/depth only; blend bits do not change indexed texels
    const bool zeroKey = ((tpage >> 7) & 3u) >= 2u && blendMode != PDDI_BLEND_NONE;
    VramImage* image = GetVram(handle);
    if (!image || image->width < kVramWidth || image->height < kVramHeight) {
#if defined(PSP_GU_DIAGNOSTIC)
        if (probe) PSP_GU_DIAG("[PSP GU DIAG] page #%lu invalid image=%p", static_cast<unsigned long>(pageId), static_cast<void*>(image));
#endif
        return nullptr;
    }
    for (auto& cache : pageCache) {
        if (cache.handle == handle && cache.tpage == tpage && cache.zeroKey == zeroKey) {
#if defined(PSP_GU_DIAGNOSTIC)
            if (probe) PSP_GU_DIAG("[PSP GU DIAG] page #%lu cache hit pixels=%p age=%lu",
                static_cast<unsigned long>(pageId), static_cast<void*>(cache.pixels),
                static_cast<unsigned long>(cache.age));
#endif
            PspPerf::Mark(PspPerf::RuntimeStage::CacheHit, tpage, static_cast<u32>(&cache - pageCache.data()));
            cache.age = cacheAge++;
            cache.inFlight = true;
            return &cache;
        }
    }

    const auto selectTarget = [&]() -> PageCache* {
        PageCache* oldest = nullptr;
        for (auto& cache : pageCache) {
            if (!cache.pixels || cache.inFlight) continue;
#if defined(PSP_DISABLE_PAGE_CACHE_RECYCLING)
            // Isolation build only: each slot is filled at most once. After
            // sixteen unique pages, mark exhaustion and draw without texture.
            // This tests retirement independently; it is not a shipping mode.
            if (cache.age != 0) continue;
#endif
            if (!cache.handle) return &cache;
            if (!oldest || cacheAge - cache.age > cacheAge - oldest->age) oldest = &cache;
        }
        return oldest;
    };
    PageCache* target = selectTarget();
    if (!target) {
        // All sixteen pages may be needed in one frame. Submit the pending
        // segment before eviction instead of dropping textures or overwriting
        // RAM/eDRAM that earlier commands still read.
        FlushList();
        target = selectTarget();
    }
    if (!target) {
        PspPerf::Mark(PspPerf::RuntimeStage::CacheFull, tpage);
        return nullptr; // allocation failure, or no-recycling diagnostic limit
    }
    PspPerf::Mark(PspPerf::RuntimeStage::CacheMiss, tpage, static_cast<u32>(target - pageCache.data()));
    ReleaseStaticPageSlot(target->gpuPixels);
    target->gpuPixels = nullptr;
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] page #%lu image=%p size=%ldx%ld pixels=%p miss target=%p oldHandle=%lu oldTpage=%04X age=%lu",
        static_cast<unsigned long>(pageId), static_cast<void*>(image),
        static_cast<long>(image->width), static_cast<long>(image->height),
        static_cast<void*>(image->pixels),
        static_cast<void*>(target->pixels),
        static_cast<unsigned long>(target->handle), static_cast<unsigned>(target->tpage),
        static_cast<unsigned long>(target->age));
#endif

    PspPerf::RecordPageCacheMiss();

    const u32 tx = tpage & 0x0fu;
    const u32 ty = (tpage >> 4) & 1u;
    const u32 depth = (tpage >> 7) & 3u;
    const u32 pageX = tx * 64u;
    const u32 pageY = ty * 256u;
    const u32 sourceY = pageY % static_cast<u32>(image->height);

#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] page #%lu decode params depth=%lu pageXY=%lu,%lu",
        static_cast<unsigned long>(pageId), static_cast<unsigned long>(depth),
        static_cast<unsigned long>(pageX), static_cast<unsigned long>(pageY));
#endif

    PspPerf::Mark(PspPerf::RuntimeStage::DecodeBegin, tpage, depth);
    // Indexed PSX pages stay indexed. The PSP GU can sample T4/T8 pages and
    // apply a native 5551 CLUT, so do not expand every texel into a separate
    // 16-bit page for every material palette.
    if (depth == 0 || depth == 1) {
        target->format = depth == 0 ? GU_PSM_T4 : GU_PSM_T8;
        const u32 wordsPerRow = depth == 0 ? 64u : 128u;
        const u32 bytesPerRow = depth == 0 ? 128u : 256u;
        u8* const destination = reinterpret_cast<u8*>(target->pixels);
        for (u32 y = 0; y < kPageSize; ++y) {
            const u16* sourceRow = image->pixels
                + ((sourceY + y) % static_cast<u32>(image->height)) * image->width;
            u8* const destinationRow = destination + y * bytesPerRow;
            if (pageX + wordsPerRow <= static_cast<u32>(image->width)) {
                for (u32 packedX = 0; packedX < wordsPerRow; ++packedX) {
                    const u16 word = sourceRow[pageX + packedX];
                    destinationRow[(packedX << 1) + 0] = static_cast<u8>(word & 0xffu);
                    destinationRow[(packedX << 1) + 1] = static_cast<u8>(word >> 8);
                }
            }
            else {
                for (u32 packedX = 0; packedX < wordsPerRow; ++packedX) {
                    u32 sourceX = pageX + packedX;
                    if (sourceX >= static_cast<u32>(image->width)) {
                        sourceX %= static_cast<u32>(image->width);
                    }
                    const u16 word = sourceRow[sourceX];
                    destinationRow[(packedX << 1) + 0] = static_cast<u8>(word & 0xffu);
                    destinationRow[(packedX << 1) + 1] = static_cast<u8>(word >> 8);
                }
            }
        }
        WritebackDcacheRange(target->pixels, static_cast<std::size_t>(bytesPerRow) * kPageSize);
    }
    else {
        target->format = GU_PSM_5551;
        u16* const destination = target->pixels;
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
                    const u16 word = sourceRow[pageX + x];
                    destinationRow[x] = ConvertPsxTexelToGu5551(word,
                        zeroKey ? word == 0 : (word & 0x7fffu) == 0x7c1fu);
                }
            }
            else {
                for (u32 x = 0; x < kPageSize; ++x) {
                    const u16 word = sourceRow[(pageX + x) % static_cast<u32>(image->width)];
                    destinationRow[x] = ConvertPsxTexelToGu5551(word,
                        zeroKey ? word == 0 : (word & 0x7fffu) == 0x7c1fu);
                }
            }
        }
        WritebackDcacheRange(target->pixels, static_cast<std::size_t>(kPageSize) * kPageSize * sizeof(u16));
    }
    PspPerf::Mark(PspPerf::RuntimeStage::DecodeDone, tpage, target->format);
#if !defined(PSP_DISABLE_STATIC_PAGE_UPLOAD)
    if (StaticPageSlot* slot = AcquireStaticPageSlot(target->format)) {
        target->gpuPixels = slot->pixels;
        // GE transfers support only 16/32-bit pixels. Transfer packed T4/T8
        // rows as 64/128 16-bit words, exactly filling their 32/64 KiB slots.
        const int wordsPerRow = target->format == GU_PSM_T4 ? 64
            : (target->format == GU_PSM_T8 ? 128 : kPageSize);
        PspPerf::Mark(PspPerf::RuntimeStage::UploadBegin, tpage, wordsPerRow);
        sceGuCopyImage(GU_PSM_5551, 0, 0, wordsPerRow, kPageSize,
                       wordsPerRow, target->pixels, 0, 0, wordsPerRow, target->gpuPixels);
        // Orders the GE copy before texture fetch; does not retire its source.
        PspPerf::Mark(PspPerf::RuntimeStage::CopyQueued, tpage);
        sceGuTexSync();
        PspPerf::Mark(PspPerf::RuntimeStage::TexSyncQueued, tpage);
    }
#endif
    target->handle = handle;
    target->tpage = tpage;
    target->zeroKey = zeroKey;
    target->age = cacheAge++;
    target->inFlight = true;
    PspPerf::Mark(PspPerf::RuntimeStage::CacheReady, tpage);
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] page #%lu prepared format=%lu", static_cast<unsigned long>(pageId), static_cast<unsigned long>(target->format));
#endif
    return target;
}

void pspContext::BindPage(float tpageFloat, float cbaFloat, bool wordZeroKey,
                          bool packedUv) {
    const u16 tpage = static_cast<u16>(std::max(tpageFloat, 0.0f));
    const u16 cba = static_cast<u16>(std::max(cbaFloat, 0.0f));
    const bool zeroKey = wordZeroKey || blendMode != PDDI_BLEND_NONE;
    const bool samePage = pageBindingValid && lastBoundHandle == vramHandle
        && lastBoundTpage == tpage && lastBoundZeroKey == zeroKey;
    // A page/material hit can still switch between the compact integer UV
    // stream and the generic float stream. Keep the texture image/CLUT
    // cached, but replay the scale command when the coordinate representation
    // changes so the cache never returns with the wrong UV interpretation.
    if (samePage && lastBoundCba == cba
        && pageTexScaleValid && pageTexScalePacked == packedUv) {
        return;
    }
    PspPerf::CountRenderer(PspPerf::RendererCounter::PageBinds);
    EnsureListSpace();
    if (framePaletteUsed == kFramePaletteCapacity) FlushList();
#if defined(PSP_GU_DIAGNOSTIC)
    static u32 bindProbe = 0;
    const u32 bindId = bindProbe++;
    const bool probe = bindId < 24u
        || (tpage == 0x0003u && cba == 0x2973u);
    if (probe) {
        PSP_GU_DIAG("[PSP GU DIAG] bind #%lu begin handle=%lu tpage=%04X cba=%04X",
            static_cast<unsigned long>(bindId), static_cast<unsigned long>(vramHandle),
            static_cast<unsigned>(tpage), static_cast<unsigned>(cba));
    }
#endif
    PageCache* page = GetDecodedPage(vramHandle, tpage);
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) {
        PSP_GU_DIAG("[PSP GU DIAG] bind #%lu decoded page=%p",
            static_cast<unsigned long>(bindId), static_cast<void*>(page ? page->pixels : nullptr));
    }
#endif
    if (!page) {
        pageBindingValid = false;
        GuDisable(GU_TEXTURE_2D);
        return;
    }
#if !defined(NDEBUG)
    static u32 pageProbeBudget = 6;
    if (pageProbeBudget > 0) {
        if (page->format == GU_PSM_5551) {
            u16 minR = 31, minG = 31, minB = 31;
            u16 maxR = 0, maxG = 0, maxB = 0;
            u32 nonZero = 0;
            for (u32 i = 0; i < 256u * 256u; ++i) {
                const u16 texel = page->pixels[i];
                const u16 r = texel & 31u;
                const u16 g = (texel >> 5) & 31u;
                const u16 b = (texel >> 10) & 31u;
                minR = std::min(minR, r); minG = std::min(minG, g); minB = std::min(minB, b);
                maxR = std::max(maxR, r); maxG = std::max(maxG, g); maxB = std::max(maxB, b);
                nonZero += (texel & 0x7FFFu) != 0 ? 1u : 0u;
            }
            LOG("[PSP GU] page tpage=%04X cba=%04X rgb5 min=%u,%u,%u max=%u,%u,%u nonzero=%lu",
                static_cast<unsigned>(tpage), static_cast<unsigned>(cba),
                static_cast<unsigned>(minR), static_cast<unsigned>(minG), static_cast<unsigned>(minB),
                static_cast<unsigned>(maxR), static_cast<unsigned>(maxG), static_cast<unsigned>(maxB),
                static_cast<unsigned long>(nonZero));
        }
        else {
            const u32 bytes = page->format == GU_PSM_T4 ? 128u * 256u : 256u * 256u;
            LOG("[PSP GU] indexed page tpage=%04X cba=%04X format=%s bytes=%lu",
                static_cast<unsigned>(tpage), static_cast<unsigned>(cba),
                page->format == GU_PSM_T4 ? "T4" : "T8",
                static_cast<unsigned long>(bytes));
        }
        --pageProbeBudget;
    }
#endif
    if (page->format == GU_PSM_T4 || page->format == GU_PSM_T8) {
        VramImage* image = GetVram(vramHandle);
        if (!image) {
            pageBindingValid = false;
            GuDisable(GU_TEXTURE_2D);
            return;
        }

        const u32 depth = (tpage >> 7) & 3u;
        const u32 clutX = (cba & 0x3fu) * 16u;
        const u32 clutY = (cba >> 6) & 0x1ffu;
        const u32 paletteSize = depth == 0 ? 16u : 256u;
        const bool blendUsesZeroKey = blendMode != PDDI_BLEND_NONE;
        const bool reuseClut = clutBindingValid
            && lastClutHandle == vramHandle
            && lastClutCba == cba
            && lastClutDepth == depth
            && lastClutWordZeroKey == wordZeroKey
            && lastClutBlend == blendUsesZeroKey;
        if (!reuseClut) {
            const u16* paletteSource = image->pixels
                + (clutY % static_cast<u32>(image->height)) * image->width;
            ClutCacheEntry* cached = nullptr;
            ClutCacheEntry* replacement = nullptr;
            u32 oldestAge = 0xffffffffu;
            for (auto& entry : clutCache) {
                if (entry.valid && entry.handle == vramHandle && entry.cba == cba
                    && entry.depth == depth && entry.entries == paletteSize
                    && entry.wordZeroKey == wordZeroKey
                    && entry.blend == blendUsesZeroKey) {
                    cached = &entry;
                    break;
                }
                if (!entry.inFlight && (!entry.valid || entry.age < oldestAge)) {
                    replacement = &entry;
                    oldestAge = entry.valid ? entry.age : 0;
                }
            }

            const u16* palette = nullptr;
            if (cached) {
                PspPerf::CountRenderer(PspPerf::RendererCounter::ClutCacheHits);
                palette = cached->palette.data();
                cached->age = clutCacheAge++;
                if (clutCacheAge == 0) clutCacheAge = 1;
            }
            else if (replacement) {
                replacement->handle = vramHandle;
                replacement->cba = cba;
                replacement->depth = static_cast<u8>(depth);
                replacement->entries = static_cast<u16>(paletteSize);
                replacement->wordZeroKey = wordZeroKey;
                replacement->blend = blendUsesZeroKey;
                replacement->valid = true;
                replacement->age = clutCacheAge++;
                if (clutCacheAge == 0) clutCacheAge = 1;
                palette = replacement->palette.data();
                for (u32 i = 0; i < paletteSize; ++i) {
                    const u16 word = paletteSource[(clutX + i) % static_cast<u32>(image->width)];
                    replacement->palette[i] = ConvertPsxTexelToGu5551(word,
                        wordZeroKey ? word == 0 : (blendUsesZeroKey ? i == 0
                            : (word & 0x7fffu) == 0x7c1fu));
                }
            }
            else {
                // More unique palettes than the bounded cache can pin in one
                // list. Retain the old per-submission fallback; it is safe
                // and the existing capacity check will fence before reuse.
                u16* const fallback = framePalettes.data() + framePaletteUsed++ * 256u;
                for (u32 i = 0; i < paletteSize; ++i) {
                    const u16 word = paletteSource[(clutX + i) % static_cast<u32>(image->width)];
                    fallback[i] = ConvertPsxTexelToGu5551(word,
                        wordZeroKey ? word == 0 : (blendUsesZeroKey ? i == 0
                            : (word & 0x7fffu) == 0x7c1fu));
                }
                palette = fallback;
            }

            const bool sameLoadedPalette = clutBindingValid
                && lastClutPaletteEntries == paletteSize
                && std::memcmp(lastClutPalette.data(), palette,
                    static_cast<std::size_t>(paletteSize) * sizeof(u16)) == 0;
            if (!sameLoadedPalette) {
                WritebackDcacheRange(palette,
                    static_cast<std::size_t>(paletteSize) * sizeof(u16));
                GuClutMode(GU_PSM_5551, 0, depth == 0 ? 0x0fu : 0xffu, 0);
                sceGuClutLoad(depth == 0 ? 1 : 16, palette); // 32-byte blocks, 16-bit entries
                PspPerf::CountRenderer(PspPerf::RendererCounter::ClutLoads);
                std::memcpy(lastClutPalette.data(), palette,
                    static_cast<std::size_t>(paletteSize) * sizeof(u16));
                lastClutPaletteEntries = static_cast<u16>(paletteSize);
                if (cached || replacement) {
                    (cached ? cached : replacement)->inFlight = true;
                }
            }
            else {
                PspPerf::CountRenderer(PspPerf::RendererCounter::ClutLoadSkips);
            }
            clutBindingValid = true;
            lastClutHandle = vramHandle;
            lastClutCba = cba;
            lastClutDepth = static_cast<u8>(depth);
            lastClutWordZeroKey = wordZeroKey;
            lastClutBlend = blendUsesZeroKey;
        }
        if (!samePage) {
            GuTexMode(page->format, 0, 0, 0);
            sceGuTexImage(0, kPageSize, kPageSize, kPageSize,
                          page->gpuPixels ? page->gpuPixels : page->pixels);
        }
    }
    else {
        if (!samePage) {
            GuTexMode(GU_PSM_5551, 0, 0, 0);
            sceGuTexImage(0, kPageSize, kPageSize, kPageSize,
                          page->gpuPixels ? page->gpuPixels : page->pixels);
        }
    }
    GuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    GuAmbientColor(0xffffffff);
    GuTexFilter(GU_NEAREST, GU_NEAREST);
    GuTexWrap(GU_REPEAT, GU_REPEAT);
    SetPageTextureScale(packedUv);
    GuEnable(GU_TEXTURE_2D);
    GuAlphaFunc(GU_GREATER, 0, 0xff);
    GuEnable(GU_ALPHA_TEST);
    pageBindingValid = true;
    lastBoundHandle = vramHandle;
    lastBoundTpage = tpage;
    lastBoundCba = cba;
    lastBoundZeroKey = zeroKey;
    PspPerf::Mark(PspPerf::RuntimeStage::BindDone, tpage, cba);
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] bind #%lu done", static_cast<unsigned long>(bindId));
#endif
}

void pspContext::DrawPrimBuffer(pddiPrimBuffer* base, u32 indexOffset, u32 indexCount) {
    PspPerf::Mark(PspPerf::RuntimeStage::PrimBegin, indexOffset, indexCount);
    // This device only manufactures pspPrimBuffer instances, and every
    // buffer reaching this context comes from that same pspDevice. Avoiding
    // RTTI here removes a type walk from every submitted draw call on the
    // 333 MHz CPU while preserving the backend's ownership invariant.
    auto* buffer = static_cast<pspPrimBuffer*>(base);
    if (!listOpen || !buffer || buffer->GetIndexCount() == 0) return;

#if defined(PSP_GU_DIAGNOSTIC)
    static u32 primProbe = 0;
    const u32 primId = primProbe++;
    const bool probe = primId < 40u;
    if (probe) {
        PSP_GU_DIAG("[PSP GU DIAG] prim #%lu entry buffer=%p indices=%lu vertices=%lu offset=%lu count=%lu",
            static_cast<unsigned long>(primId), static_cast<void*>(buffer),
            static_cast<unsigned long>(buffer->GetIndexCount()),
            static_cast<unsigned long>(buffer->GetVertexCount()),
            static_cast<unsigned long>(indexOffset), static_cast<unsigned long>(indexCount));
    }
#endif

    Setup3DState();
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu after state", static_cast<unsigned long>(primId));
#endif
    PspPerf::Mark(PspPerf::RuntimeStage::MatrixBegin);
    const bool projectionChanged = LoadGumMatrix(GU_PROJECTION, projection);
    const bool viewChanged = LoadGumMatrix(GU_VIEW, view);
    const bool worldChanged = LoadGumMatrix(GU_MODEL, world);
    if (projectionChanged || viewChanged || worldChanged) {
        sceGumUpdateMatrix();
    }
    PspPerf::Mark(PspPerf::RuntimeStage::MatrixDone);
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu after matrix", static_cast<unsigned long>(primId));
#endif

    const u32 total = buffer->GetIndexCount();
    u32 start = std::min(indexOffset, total);
    u32 end = indexCount ? start + std::min(indexCount, total - start) : total;
    if (end <= start) return;

    const PspVertex* vertices = buffer->GetVertices();
    const u16* indices = buffer->GetIndices();
    const u32 vertexCount = buffer->GetVertexCount();
    if (!vertices || !indices || vertexCount == 0) {
#if defined(PSP_GU_DIAGNOSTIC)
        if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu dropped null data", static_cast<unsigned long>(primId));
#endif
        return;
    }

    // Index validity is established when the persistent buffer is mutated,
    // outside the frame path. Diagnostic builds retain the full range check
    // so malformed streamed data still produces a precise breadcrumb.
    if (!buffer->HasValidIndices()) {
#if defined(PSP_GU_DIAGNOSTIC)
        PSP_GU_DIAG("[PSP GU DIAG] prim invalid index buffer=%p verts=%lu",
            static_cast<void*>(buffer), static_cast<unsigned long>(vertexCount));
#endif
        return;
    }
#if defined(PSP_TRACE_TELEMETRY) || defined(PSP_DISABLE_RENDER_OPTIMIZATIONS)
    for (u32 cursor = start; cursor < end; ++cursor) {
        if (indices[cursor] >= vertexCount) {
            PspPerf::Trace("gu draw: invalid indices dropped");
            return;
        }
    }
#endif
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu range valid start=%lu end=%lu",
        static_cast<unsigned long>(primId), static_cast<unsigned long>(start),
        static_cast<unsigned long>(end));
#endif
#if !defined(NDEBUG)
    static u32 colorProbeBudget = 8;
    if (colorProbeBudget > 0 && vertices && buffer->GetVertexCount() > 0) {
        u8 minR = 255, minG = 255, minB = 255;
        u8 maxR = 0, maxG = 0, maxB = 0;
        for (u32 i = 0; i < buffer->GetVertexCount(); ++i) {
            const u32 color = vertices[i].color;
            const u8 r = static_cast<u8>(color & 0xffu);
            const u8 g = static_cast<u8>((color >> 8) & 0xffu);
            const u8 b = static_cast<u8>((color >> 16) & 0xffu);
            minR = std::min(minR, r); minG = std::min(minG, g); minB = std::min(minB, b);
            maxR = std::max(maxR, r); maxG = std::max(maxG, g); maxB = std::max(maxB, b);
        }
        LOG("[PSP GU] 3D vertex colors min=%02X%02X%02X max=%02X%02X%02X verts=%lu",
            minR, minG, minB, maxR, maxG, maxB,
            static_cast<unsigned long>(buffer->GetVertexCount()));
        --colorProbeBudget;
    }
#endif
    const bool isTriangles = buffer->GetPrimType() == PDDI_PRIM_TRIANGLES;
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu draw setup triangles=%u vram=%lu currentTexture=%p realTexture=%u",
        static_cast<unsigned long>(primId), isTriangles ? 1u : 0u,
        static_cast<unsigned long>(vramHandle), static_cast<void*>(currentTexture),
        realTextureMode ? 1u : 0u);
#endif
    const bool triangleStrip = buffer->GetPrimType() == PDDI_PRIM_TRISTRIP;
    const bool lineStrip = buffer->GetPrimType() == PDDI_PRIM_LINESTRIP;
    const bool directIndexedVertices = !buffer->UsesStagedIndexedVertices()
        && blendMode != PDDI_BLEND_ALPHA
        && (reinterpret_cast<std::uintptr_t>(vertices) & 15u) == 0u;
#if defined(PSP_ENABLE_NATIVE_INDEXED) && defined(PSP_INDEXED_LIST_MEMORY)
    // Restrict the compact stream to page-backed PSX geometry. Its UVs are
    // integer texels; real-texture/glTF buffers commonly use fractional
    // normalized coordinates and retain the float format.
    const bool usePackedVertices = vramHandle != 0 && buffer->HasPackedVertices();
#else
    // The frame-arena variants do not have a packed arena yet.
    const bool usePackedVertices = false;
#endif
#if defined(PSP_ENABLE_NATIVE_INDEXED)
    const int nativeArrayFormat = (usePackedVertices ? GU_TEXTURE_16BIT : GU_TEXTURE_32BITF)
        | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
    const int nativeIndexedFormat = nativeArrayFormat | GU_INDEX_16BIT;
    const void* nativeVertices = usePackedVertices
        ? static_cast<const void*>(buffer->GetPackedVertices())
        : static_cast<const void*>(vertices);
#endif
#if !defined(PSP_ENABLE_NATIVE_INDEXED)
    (void)directIndexedVertices;
#endif
    const auto nextChunk = [&](u32 chunkEnd) {
        if (chunkEnd == end) return end;
        return chunkEnd - (triangleStrip ? 2u : (lineStrip ? 1u : 0u));
    };
#if defined(PSP_ENABLE_NATIVE_INDEXED) && defined(PSP_INDEXED_LIST_MEMORY)
    // Dynamic model buffers are deliberately staged because their persistent
    // vertices are rewritten by CPU skinning every frame. The old staged path
    // remapped and copied the referenced vertices again for every material
    // run. Keep one list-owned copy of the complete vertex stream for the
    // current submission instead; each run can then reuse it with a small,
    // isolated index range. The serial tracks a possible list fence, after
    // which the old pointer is no longer valid.
    const std::size_t wholeVertexBytes = (usePackedVertices
        ? sizeof(PspPackedVertex) : sizeof(PspVertex)) * vertexCount;
    const bool wholeVertexStagingFits = wholeVertexBytes
        + sizeof(u16) * static_cast<std::size_t>(kDrawChunk)
        + kListCommandReserve + 64u
        <= sizeof(g_displayListStorage.words);
    void* stagedWholeVertices = nullptr;
    u32 stagedWholeSerial = 0;
#endif
    while (start < end) {
        // Reserve before binding: a cache/CLUT miss may also fence the list,
        // but no pointer for this draw has been allocated yet.
#if defined(PSP_ENABLE_NATIVE_INDEXED) && defined(PSP_INDEXED_LIST_MEMORY)
        // BindPage performs the command-list capacity check immediately before
        // writing its state. The list-backed native path has no frame arenas,
        // so the old zero-payload EnsureDrawSpace() here only repeated the
        // same sceGuCheckList() call.
        if (framePaletteUsed == kFramePaletteCapacity) FlushList();
#else
        const u32 reservedCount = std::min(kDrawChunk, end - start);
        EnsureDrawSpace(reservedCount, reservedCount);
#endif
#if defined(PSP_ENABLE_NATIVE_INDEXED) && defined(PSP_INDEXED_LIST_MEMORY)
        // Do not reserve for the complete remaining chunk here. The material
        // scan below may split it into a tiny run; reserving the maximum
        // before that scan caused false list-overflow flushes and a costly
        // sceGuSync() even when the actual allocation was only a few vertices.
        EnsureListSpace();
#endif
#if defined(PSP_STAGE_DIAGNOSTIC)
        static u32 pspStageGuDrawProbe = 0;
        const u32 pspStageGuDrawId = pspStageGuDrawProbe++;
        const bool pspStageProbe = pspStageGuDrawId < 24u
            || (pspStageGuDrawId >= 280u && pspStageGuDrawId < 520u);
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] draw begin #%lu start=%lu end=%lu",
                static_cast<unsigned long>(pspStageGuDrawId),
                static_cast<unsigned long>(start), static_cast<unsigned long>(end));
        }
#endif
        u32 runEnd = start + std::min(kDrawChunk, end - start);
        if (isTriangles) runEnd -= (runEnd - start) % 3u;
        if (buffer->GetPrimType() == PDDI_PRIM_LINES) runEnd -= (runEnd - start) % 2u;
        if (runEnd <= start) break;
        u32 materialKey = 0;
        if (isTriangles && vramHandle && start + 2 < end) {
            const u16 firstIndex = indices[start];
            const bool firstMaterialValid = texInfoOverride || buffer->HasMaterialKey(firstIndex);
            const u32 firstMaterialKey = texInfoOverride
                ? texInfoWord : buffer->GetMaterialKey(firstIndex);
#if defined(PSP_GU_DIAGNOSTIC)
            if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu material scan firstIndex=%u firstTpage=%f firstCba=%f",
                static_cast<unsigned long>(primId), static_cast<unsigned>(firstIndex),
                static_cast<double>(firstMaterialKey >> 16),
                static_cast<double>(firstMaterialKey & 0xffffu));
#endif
            if (firstMaterialValid) {
                materialKey = firstMaterialKey;
                if (!texInfoOverride) {
                    for (u32 cursor = start + 3; cursor + 2 < runEnd; cursor += 3) {
#if defined(PSP_GU_DIAGNOSTIC)
                        if (probe && (cursor == start + 3 || cursor % 300u == 0u)) {
                            PSP_GU_DIAG("[PSP GU DIAG] prim #%lu material scan cursor=%lu",
                                static_cast<unsigned long>(primId), static_cast<unsigned long>(cursor));
                        }
#endif
                        const u16 triIndex = indices[cursor];
                        if (!buffer->HasMaterialKey(triIndex)) {
                            runEnd = cursor;
                            break;
                        }
                        const u32 key = buffer->GetMaterialKey(triIndex);
                        if (key != materialKey) {
                            runEnd = cursor;
                            break;
                        }
                    }
                }
#if defined(PSP_GU_DIAGNOSTIC)
                if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu material key=%08lX runEnd=%lu before bind",
                    static_cast<unsigned long>(primId), static_cast<unsigned long>(materialKey),
                    static_cast<unsigned long>(runEnd));
#endif
#if defined(PSP_STAGE_DIAGNOSTIC)
                if (pspStageProbe) {
                    Log::Get().LogMessage("[PSP GU Stage] before bind #%lu tpage=%04X cba=%04X",
                        static_cast<unsigned long>(pspStageGuDrawId),
                        static_cast<unsigned>(static_cast<u16>(firstTpage)),
                        static_cast<unsigned>(static_cast<u16>(firstCba)));
                }
#endif
                BindPage(static_cast<float>(firstMaterialKey >> 16),
                         static_cast<float>(firstMaterialKey & 0xffffu),
                         false, usePackedVertices);
#if defined(PSP_STAGE_DIAGNOSTIC)
                if (pspStageProbe) {
                    Log::Get().LogMessage("[PSP GU Stage] after bind #%lu",
                        static_cast<unsigned long>(pspStageGuDrawId));
                }
#endif
#if defined(PSP_GU_DIAGNOSTIC)
                if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu after bind", static_cast<unsigned long>(primId));
#endif
            }
            else {
        GuDisable(GU_TEXTURE_2D);
        GuDisable(GU_ALPHA_TEST);
            }
        }
        else if (currentTexture && realTextureMode) {
            ConfigureTexture(static_cast<pspTexture*>(currentTexture), false);
            GuDisable(GU_ALPHA_TEST);
        }
        else {
            GuDisable(GU_TEXTURE_2D);
            GuDisable(GU_ALPHA_TEST);
        }
        if (vramHandle != 0) SetPageTextureScale(usePackedVertices);
#if defined(PSP_GU_DIAGNOSTIC)
        if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu material setup done start=%lu runEnd=%lu",
            static_cast<unsigned long>(primId), static_cast<unsigned long>(start),
            static_cast<unsigned long>(runEnd));
#endif

        int primitive = GU_TRIANGLES;
        switch (buffer->GetPrimType()) {
            case PDDI_PRIM_TRISTRIP: primitive = GU_TRIANGLE_STRIP; break;
            case PDDI_PRIM_LINES: primitive = GU_LINES; break;
            case PDDI_PRIM_LINESTRIP: primitive = GU_LINE_STRIP; break;
            case PDDI_PRIM_POINTS: primitive = GU_POINTS; break;
            case PDDI_PRIM_TRIANGLES: default: primitive = GU_TRIANGLES; break;
        }
#if defined(PSP_TRACE_TELEMETRY) || defined(PSP_DISABLE_RENDER_OPTIMIZATIONS)
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

#endif // draw formatting is only needed when Trace actually records it
#if defined(PSP_GU_DIAGNOSTIC)
        if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu before staging start=%lu runEnd=%lu",
            static_cast<unsigned long>(primId), static_cast<unsigned long>(start),
            static_cast<unsigned long>(runEnd));
#endif

#if !defined(PSP_ENABLE_NATIVE_INDEXED)
        // Use the existing persistent arena, not sceGuGetMemory: expanded
        // geometry alone can exceed the entire 1 MiB command list in a frame.
        const u32 chunkCount = runEnd - start;
        frameVertexUsed = (frameVertexUsed + 1u) & ~1u;
        PspVertex* fallbackVertices = frameVertices.data() + frameVertexUsed;
        frameVertexUsed += chunkCount;
        for (u32 cursor = 0; cursor < chunkCount; ++cursor) {
            fallbackVertices[cursor] = vertices[indices[start + cursor]];
            if (blendMode == PDDI_BLEND_ALPHA) {
                fallbackVertices[cursor].color = (fallbackVertices[cursor].color & 0x00ffffffu) | 0x80000000u;
            }
        }
        WritebackDcacheRange(fallbackVertices, sizeof(PspVertex) * chunkCount);
        PspPerf::CountRenderer(PspPerf::RendererCounter::StagingBytes, sizeof(PspVertex) * chunkCount);
        PspPerf::CountRenderer(PspPerf::RendererCounter::StagingCopies);
        PspPerf::Mark(PspPerf::RuntimeStage::DrawBegin, start, runEnd - start);
        // The matrices were published once above DrawPrimBuffer(). The GUM
        // wrapper repeats its dirty-matrix check for every material run.
        sceGuDrawArray(primitive,
                       GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                       static_cast<int>(chunkCount), nullptr, fallbackVertices);
        PspPerf::Mark(PspPerf::RuntimeStage::DrawDone, start, chunkCount);
        PspPerf::RecordDrawSubmission(chunkCount);
        start = nextChunk(runEnd);
        continue;
#else
        // Keep the native indexed stream compact.  The source buffer can
        // contain a large vertex arena while a material run references only
        // a small subset of it; copying that whole arena both wastes GE/RAM
        // bandwidth and makes the display-list lifetime harder to reason
        // about.  Remap the run to local 16-bit indices and stage only the
        // vertices actually used by this draw.
        const u32 runCount = runEnd - start;
        // PspVertex is already the exact GU 32-bit-float layout. For opaque
        // submissions we can borrow the persistent source array and stage
        // only the run's indices. This removes the per-material vertex copy
        // and remap while retaining the compact path for alpha blending
        // (which rewrites vertex alpha) and unaligned allocations.
        if (directIndexedVertices) {
#if defined(PSP_INDEXED_PERSISTENT_INDICES)
            // The aligned allocator plus the fence in PrepareForMutation()
            // make the source index stream safe for the GE. A run beginning
            // on an odd u16 cannot be borrowed on PSP firmware that requires
            // a word-aligned index pointer, so retain the staged fallback for
            // that rare boundary.
            if ((reinterpret_cast<std::uintptr_t>(indices + start) & 3u) == 0u) {
                const u16* const guIndices = indices + start;
                PspPerf::Mark(PspPerf::RuntimeStage::DrawBegin, start, runCount);
                sceGuDrawArray(primitive,
                               nativeIndexedFormat,
                               static_cast<int>(runCount), guIndices, nativeVertices);
                buffer->MarkDirectSubmission();
                PspPerf::Mark(PspPerf::RuntimeStage::DrawDone, start, runCount);
                PspPerf::RecordDrawSubmission(runCount);
                start = nextChunk(runEnd);
                continue;
            }
#endif
#if defined(PSP_INDEXED_LIST_MEMORY)
            EnsureListSpace(sizeof(u16) * runCount + 64u);
            u16* guIndices = static_cast<u16*>(
                AllocateListMemory(sizeof(u16) * runCount));
#else
            const u32 alignedIndexUsed = (frameIndexUsed + 7u) & ~7u;
            frameIndexUsed = alignedIndexUsed;
            u16* guIndices = frameIndices.data() + frameIndexUsed;
            frameIndexUsed += runCount;
#endif
            if (!guIndices) {
                PspPerf::Trace("gu indexed: direct index staging overflow");
                start = runEnd;
                if (!isTriangles) break;
                continue;
            }
            std::copy(indices + start, indices + runEnd, guIndices);
            WritebackDcacheRange(guIndices, sizeof(u16) * runCount);
            PspPerf::CountRenderer(PspPerf::RendererCounter::StagingBytes,
                sizeof(u16) * runCount);
            PspPerf::CountRenderer(PspPerf::RendererCounter::StagingCopies);
            PspPerf::Mark(PspPerf::RuntimeStage::DrawBegin, start, runCount);
            sceGuDrawArray(primitive,
                           nativeIndexedFormat,
                           static_cast<int>(runCount), guIndices, nativeVertices);
            buffer->MarkDirectSubmission();
            PspPerf::Mark(PspPerf::RuntimeStage::DrawDone, start, runCount);
            PspPerf::RecordDrawSubmission(runCount);
            start = nextChunk(runEnd);
            continue;
        }
#if defined(PSP_INDEXED_LIST_MEMORY)
        // Prefer one whole-buffer staging allocation for opaque/native draws.
        // A CLUT or command-list fence can invalidate list memory between
        // material runs, so re-stage whenever the submission serial changes.
        if (stagedWholeVertices && stagedWholeSerial != g_submissionSerial) {
            stagedWholeVertices = nullptr;
        }
        if (wholeVertexStagingFits && blendMode != PDDI_BLEND_ALPHA) {
            if (stagedWholeVertices && stagedWholeSerial == g_submissionSerial) {
                EnsureListSpace(sizeof(u16) * runCount + 64u);
                if (stagedWholeSerial != g_submissionSerial) {
                    stagedWholeVertices = nullptr;
                }
            }
            if (!stagedWholeVertices) {
                EnsureListSpace(wholeVertexBytes + sizeof(u16) * runCount + 64u);
                stagedWholeVertices = AllocateListMemory(wholeVertexBytes);
                if (stagedWholeVertices) {
                    if (usePackedVertices) {
                        std::copy(buffer->GetPackedVertices(),
                                  buffer->GetPackedVertices() + vertexCount,
                                  static_cast<PspPackedVertex*>(stagedWholeVertices));
                    }
                    else {
                        std::copy(vertices, vertices + vertexCount,
                                  static_cast<PspVertex*>(stagedWholeVertices));
                    }
                    WritebackDcacheRange(stagedWholeVertices, wholeVertexBytes);
                    stagedWholeSerial = g_submissionSerial;
                    PspPerf::CountRenderer(PspPerf::RendererCounter::StagingBytes,
                        static_cast<u32>(wholeVertexBytes));
                    PspPerf::CountRenderer(PspPerf::RendererCounter::StagingCopies);
                }
            }
            if (stagedWholeVertices) {
                EnsureListSpace(sizeof(u16) * runCount + 64u);
                if (stagedWholeSerial == g_submissionSerial) {
                    u16* guIndices = static_cast<u16*>(
                        AllocateListMemory(sizeof(u16) * runCount));
                    if (guIndices) {
                        std::copy(indices + start, indices + runEnd, guIndices);
                        WritebackDcacheRange(guIndices, sizeof(u16) * runCount);
                        PspPerf::CountRenderer(PspPerf::RendererCounter::StagingBytes,
                            sizeof(u16) * runCount);
                        PspPerf::CountRenderer(PspPerf::RendererCounter::StagingCopies);
                        PspPerf::Mark(PspPerf::RuntimeStage::DrawBegin, start, runCount);
                        sceGuDrawArray(primitive,
                                       nativeIndexedFormat,
                                       static_cast<int>(runCount),
                                       guIndices, stagedWholeVertices);
                        PspPerf::Mark(PspPerf::RuntimeStage::DrawDone, start, runCount);
                        PspPerf::RecordDrawSubmission(runCount);
                        start = nextChunk(runEnd);
                        continue;
                    }
                }
            }
        }
#endif
        ++nativeRemapGeneration;
        if (nativeRemapGeneration == 0) {
            nativeRemapStamp.fill(0);
            nativeRemapGeneration = 1;
        }
        const u32 remapGeneration = nativeRemapGeneration;
        u32 localVertexCount = 0;
        bool remapOverflow = false;
        for (u32 cursor = start; cursor < runEnd; ++cursor) {
            const u16 sourceIndex = indices[cursor];
            if (sourceIndex >= vertexCount) {
                remapOverflow = true;
                break;
            }
            if (nativeRemapStamp[sourceIndex] == remapGeneration) continue;
            if (localVertexCount >= kNativeRemapCapacity) {
                remapOverflow = true;
                break;
            }
            nativeRemapStamp[sourceIndex] = remapGeneration;
            nativeRemap[sourceIndex] = static_cast<u16>(localVertexCount);
            nativeRemapSource[localVertexCount] = sourceIndex;
            ++localVertexCount;
        }
        if (remapOverflow || localVertexCount == 0) {
            PspPerf::Trace("gu indexed: remap overflow");
            start = runEnd;
            if (!isTriangles) break;
            continue;
        }
        const std::size_t vertexStride = usePackedVertices
            ? sizeof(PspPackedVertex) : sizeof(PspVertex);
#if defined(PSP_INDEXED_LIST_MEMORY)
        // Isolation path: sceGuGetMemory is the exact list-owned allocation
        // used by the PSPSDK samples.  The compact local stream keeps this
        // list allocation bounded even for a large source vertex arena.
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] remap done #%lu indices=%lu vertices=%lu",
                static_cast<unsigned long>(pspStageGuDrawId),
                static_cast<unsigned long>(runCount),
                static_cast<unsigned long>(localVertexCount));
            Log::Get().LogMessage("[PSP GU Stage] list memory before vertices #%lu",
                static_cast<unsigned long>(pspStageGuDrawId));
        }
#endif
        EnsureListSpace(vertexStride * localVertexCount
                        + sizeof(u16) * runCount + 64u);
        void* guVertices = AllocateListMemory(vertexStride * localVertexCount);
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] list memory after vertices #%lu ptr=%p",
                static_cast<unsigned long>(pspStageGuDrawId), guVertices);
            Log::Get().LogMessage("[PSP GU Stage] list memory before indices #%lu",
                static_cast<unsigned long>(pspStageGuDrawId));
        }
#endif
        u16* guIndices = static_cast<u16*>(
            AllocateListMemory(sizeof(u16) * runCount));
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] list memory #%lu verts=%p indices=%p",
                static_cast<unsigned long>(pspStageGuDrawId),
                static_cast<void*>(guVertices), static_cast<void*>(guIndices));
        }
#endif
        if (!guVertices || !guIndices) {
            PspPerf::Trace("gu indexed: list memory overflow");
            start = runEnd;
            if (!isTriangles) break;
            continue;
        }
#if defined(PSP_GU_DIAGNOSTIC) || defined(PSP_STAGE_DIAGNOSTIC)
        const u32 stagedVertexBase = 0;
#endif
#else
        // GE reads asynchronously; use aligned, compact streams. Capacity
        // was reserved before material binding, so neither allocation fences.
        const u32 alignedVertexUsed = (frameVertexUsed + 1u) & ~1u;
        const u32 alignedIndexUsed = (frameIndexUsed + 7u) & ~7u;
        frameVertexUsed = alignedVertexUsed;
        frameIndexUsed = alignedIndexUsed;
        void* guVertices = frameVertices.data() + frameVertexUsed;
#if defined(PSP_GU_DIAGNOSTIC) || defined(PSP_STAGE_DIAGNOSTIC)
        const u32 stagedVertexBase = frameVertexUsed;
#endif
        frameVertexUsed += localVertexCount;
        u16* guIndices = frameIndices.data() + frameIndexUsed;
        frameIndexUsed += runCount;
#endif
#if defined(PSP_GU_DIAGNOSTIC)
        if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu indexed stage count=%lu vertices=%lu vbase=%lu vmem=%p imem=%p",
            static_cast<unsigned long>(primId), static_cast<unsigned long>(runCount),
            static_cast<unsigned long>(localVertexCount), static_cast<unsigned long>(stagedVertexBase),
            static_cast<void*>(guVertices), static_cast<void*>(guIndices));
#endif
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] indexed stage #%lu indices=%lu vertices=%lu vbase=%lu",
                static_cast<unsigned long>(pspStageGuDrawId),
                static_cast<unsigned long>(runCount), static_cast<unsigned long>(localVertexCount),
                static_cast<unsigned long>(stagedVertexBase));
        }
#endif
        PspPerf::Trace("gu indexed: copy begin");
        for (u32 localIndex = 0; localIndex < localVertexCount; ++localIndex) {
            const u16 sourceIndex = nativeRemapSource[localIndex];
            if (usePackedVertices) {
                auto* packed = static_cast<PspPackedVertex*>(guVertices);
                packed[localIndex] = buffer->GetPackedVertices()[sourceIndex];
                if (blendMode == PDDI_BLEND_ALPHA) {
                    packed[localIndex].color = (packed[localIndex].color & 0x00ffffffu) | 0x80000000u;
                }
            }
            else {
                auto* floats = static_cast<PspVertex*>(guVertices);
                floats[localIndex] = vertices[sourceIndex];
                if (blendMode == PDDI_BLEND_ALPHA) {
                    floats[localIndex].color = (floats[localIndex].color & 0x00ffffffu) | 0x80000000u;
                }
            }
        }
        for (u32 cursor = start; cursor < runEnd; ++cursor) {
            guIndices[cursor - start] = nativeRemap[indices[cursor]];
        }
        PspPerf::CountRenderer(PspPerf::RendererCounter::StagingBytes,
            vertexStride * localVertexCount + sizeof(u16) * runCount);
        PspPerf::CountRenderer(PspPerf::RendererCounter::StagingCopies, 2);
        PspPerf::Trace("gu indexed: copy done");
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] indexed copy done #%lu",
                static_cast<unsigned long>(pspStageGuDrawId));
        }
#endif
#if defined(PSP_GU_DIAGNOSTIC)
        if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu indexed copy done before dcache",
            static_cast<unsigned long>(primId));
#endif
        WritebackDcacheRange(
            guVertices,
            vertexStride * localVertexCount);
        WritebackDcacheRange(
            guIndices,
            sizeof(u16) * runCount);
#if defined(PSP_NATIVE_INDEXED_FULL_DCACHE)
        // The SDK's indexed GU samples flush the complete data cache before
        // submitting RAM-backed vertex/index streams.  Keep this diagnostic
        // switch separate from the normal range write-back so we can prove
        // whether a stale cache line is the source of the PPSSPP artefacts
        // without making the shipping renderer pay that cost per material.
        sceKernelDcacheWritebackAll();
#endif
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] indexed dcache done #%lu",
                static_cast<unsigned long>(pspStageGuDrawId));
        }
#endif
#if defined(PSP_GU_DIAGNOSTIC)
        if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu indexed before draw",
            static_cast<unsigned long>(primId));
#endif
#if !defined(NDEBUG)
        static u32 drawProbeCount = 0;
        const u32 drawProbeId = drawProbeCount++;
        if (drawProbeId < 180u || (drawProbeId >= 170u && drawProbeId < 300u)) {
            LOG("[PSP GU] indexed draw begin #%lu buffer=%p start=%lu count=%lu vertices=%p indices=%p",
                static_cast<unsigned long>(drawProbeId), static_cast<void*>(buffer),
                static_cast<unsigned long>(start), static_cast<unsigned long>(runCount),
                static_cast<void*>(guVertices), static_cast<void*>(guIndices));
        }
#endif
        PspPerf::Trace("gu indexed: draw begin");
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] indexed before draw #%lu count=%lu verts=%p indices=%p",
                static_cast<unsigned long>(pspStageGuDrawId),
                static_cast<unsigned long>(runCount), static_cast<void*>(guVertices),
                static_cast<void*>(guIndices));
        }
#endif
        // GUM publishes dirty matrices then delegates to GU; both streams
        // remain immutable until FinishList's sceGuSync completes.
        PspPerf::Mark(PspPerf::RuntimeStage::DrawBegin, start, runEnd - start);
        sceGuDrawArray(primitive,
                       nativeIndexedFormat,
                       static_cast<int>(runCount), guIndices, guVertices);
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageProbe) {
            Log::Get().LogMessage("[PSP GU Stage] indexed after draw #%lu",
                static_cast<unsigned long>(pspStageGuDrawId));
        }
#endif
#if defined(PSP_GU_DIAGNOSTIC)
        if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu indexed after draw", static_cast<unsigned long>(primId));
#endif
#if !defined(NDEBUG)
        if (drawProbeId < 180u || (drawProbeId >= 170u && drawProbeId < 300u)) {
            LOG("[PSP GU] indexed draw done #%lu", static_cast<unsigned long>(drawProbeId));
        }
#endif
        PspPerf::Trace("gu indexed: draw done");
        PspPerf::Mark(PspPerf::RuntimeStage::DrawDone, start, runCount);
        PspPerf::RecordDrawSubmission(runCount);
        start = nextChunk(runEnd);
#endif
    }
#if defined(PSP_GU_DIAGNOSTIC)
    if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu done", static_cast<unsigned long>(primId));
#endif
}

pddiDevice* pddiCreate() {
    return new pspDevice();
}

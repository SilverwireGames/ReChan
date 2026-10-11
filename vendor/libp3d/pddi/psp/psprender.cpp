#include "pddi/psp/psprender.h"
#include "pc/log.h"
#include "psp/psp_perf.h"
#include "p3d/fileio.h"
#if defined(PSP_DRAW_MICROBENCH)
#include "p3d/p3dmath.h"
#include "p3d/skeleton.h"
#endif

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
#include <new>
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

// The GE holds exactly one CLUT. g_clutGeRaw is the raw PSX row last uploaded
// to it, with the flags that shape conversion. A BindPage whose raw row and
// flags match is a no-op for the GE, even when its CBA differs.
bool g_clutGeContentValid = false;
u8 g_clutGeDepth = 0;
bool g_clutGeWordZeroKey = false;
bool g_clutGeBlend = false;
u16 g_clutGeRaw[256];
u16 g_clutRawScratch[256];

// Every write to these states in this backend goes through the same cache,
// including overlays and standalone textures. A list split preserves GE state;
// a new frame invalidates the cache conservatively. Never cache TexImage or
// ClutLoad by pointer: their memory may have changed after a completed fence.
static void ResetGuStateCache() {
    ++g_guStateGeneration;
    ++g_matrixGeneration;
    g_guCapabilityValid = 0;
    g_clutGeContentValid = false;
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

// Same cache contract as PSP_CACHED_GU, but readable: Setup2DState checks it
// to know whether its depth-mask state is still in place.
int g_guDepthMaskValue = -1;
u32 g_guDepthMaskGeneration = 0;
static void GuDepthMask(int mask) {
    if (kCacheGuState && g_guDepthMaskGeneration == g_guStateGeneration
        && g_guDepthMaskValue == mask) return;
    g_guDepthMaskValue = mask;
    g_guDepthMaskGeneration = g_guStateGeneration;
    PspPerf::CountRenderer(PspPerf::RendererCounter::GuStateWrites);
    sceGuDepthMask(mask);
}

// Incremented by every GUM matrix load from the 3D path. Setup2DState replaces
// the GUM matrices; it can skip that work while no 3D load happened since.
u32 g_gumMatrixSerial = 1;
// 16-bit colour buffers: 3D draws dither (PSX gouraud/blend look); 2D
// overlays do not, so exact 15-bit artwork stays exact.
#if defined(PSP_TRIPLE_BUFFER)
constexpr u32 kDitherCapability = 1u << GU_DITHER;
#else
constexpr u32 kDitherCapability = 0u;
#endif
// Capabilities Setup2DState disables. Their cached bits prove the 2D state.
constexpr u32 k2DDisabledCapabilities = (1u << GU_DEPTH_TEST) | (1u << GU_CULL_FACE)
    | (1u << GU_ALPHA_TEST) | (1u << GU_COLOR_TEST) | (1u << GU_STENCIL_TEST)
    | (1u << GU_COLOR_LOGIC_OP) | (1u << GU_LIGHTING) | (1u << GU_CLIP_PLANES)
    | kDitherCapability;
// sceGuSwapBuffers alternates the draw target between the two colour
// buffers; the pause backdrop needs the one currently being rendered.
bool g_drawingSecondBuffer = false;
// Display vcount at the last swap, while a VBlank is still owed before the GE
// may draw into the buffer that swap released (see WaitForSwapVblank).
u32 g_swapVcount = 0;
bool g_swapVblankPending = false;
#if defined(PSP_TRIPLE_BUFFER)
// Triple buffering: the buffer being drawn, the one last handed to the
// display (on screen, or queued for the next VBlank) and the one it replaces.
// The draw target is always the third: never scanned out, never queued.
u32 g_drawFramebuffer = 0;
u32 g_shownFramebuffer = 1;
u32 g_replacedFramebuffer = 2;
#endif

// A swap queued with PSP_DISPLAY_SETBUF_NEXTVSYNC keeps scanning out the
// released buffer until the next VBlank. One VBlank after the swap is all the
// draw target needs, so a frame whose logic outlasted it starts at once
// instead of waiting for a further VBlank (a 35 ms frame presents at 35 ms
// rather than being rounded up to 50 ms). The vcount is read after the swap,
// so a VBlank racing the swap only costs one extra wait, never a tear.
static void WaitForVblankSinceSwap() {
    if (!g_swapVblankPending) return;
    g_swapVblankPending = false;
    if (static_cast<u32>(sceDisplayGetVcount()) != g_swapVcount) return;
#if defined(PSP_PERF_TELEMETRY)
    const u32 vblankStart = sceKernelGetSystemTimeLow();
#endif
    sceDisplayWaitVblankStart();
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::CountRenderer(PspPerf::RendererCounter::VblankUs,
        sceKernelGetSystemTimeLow() - vblankStart);
#endif
}

static void WaitForSwapVblank() {
#if !defined(PSP_TRIPLE_BUFFER)
    WaitForVblankSinceSwap();
#endif
    // Triple buffering never draws into a buffer on screen or queued for it;
    // SwapBuffers paces instead (see pspDisplay::SwapBuffers).
}

// Only the list-memory native path submits PspPackedVertex. Elsewhere the
// float stream is the one the GE (or the CPU staging copy) consumes.
#if defined(PSP_ENABLE_NATIVE_INDEXED) && defined(PSP_INDEXED_LIST_MEMORY)
constexpr bool kPackedStreamConsumed = true;
#else
constexpr bool kPackedStreamConsumed = false;
#endif

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
#if defined(PSP_TRIPLE_BUFFER)
// Three RGB565 colour buffers take less eDRAM than two RGBA8888 ones, so the
// texture slots keep their space. The PSX itself outputs 15-bit colour; 3D
// draws dither like its GPU (sceGuStart loads the PSX dither matrix).
constexpr int kFramebufferPsm = GU_PSM_5650;
constexpr u32 kFramebufferCount = 3u;
constexpr u32 kFramebufferPixelBytes = 2u;
#else
constexpr int kFramebufferPsm = GU_PSM_8888;
constexpr u32 kFramebufferCount = 2u;
constexpr u32 kFramebufferPixelBytes = 4u;
#endif
constexpr u32 kFramebufferStride = kBufferWidth * kHeight * kFramebufferPixelBytes;
constexpr u32 kFramebufferBytes = kFramebufferStride * kFramebufferCount;
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

// The command words sceGuDrawArray writes (PSPSDK sceGuDrawArray.c) for
// 32-bit GE addresses: VTYPE, BASE+IADDR and BASE+VADDR when present, PRIM.
u32 EncodeGuDrawArray(int prim, int vtype, int count, u32 indices, u32 vertices,
                      u32 words[6]) {
    u32 n = 0;
    if (vtype) words[n++] = (0x12u << 24) | (static_cast<u32>(vtype) & 0xffffffu);
    if (indices) {
        words[n++] = (0x10u << 24) | ((indices >> 8) & 0xf0000u);
        words[n++] = (0x02u << 24) | (indices & 0xffffffu);
    }
    if (vertices) {
        words[n++] = (0x10u << 24) | ((vertices >> 8) & 0xf0000u);
        words[n++] = (0x01u << 24) | (vertices & 0xffffffu);
    }
    words[n++] = (0x04u << 24)
        | (((static_cast<u32>(prim) << 16) | static_cast<u32>(count)) & 0xffffffu);
    return n;
}

// sceGuDrawArray moves the GE stall address after every draw, a syscall.
// PSP_DRAWS_PER_STALL_UPDATE=N publishes draws in groups instead: the same
// words without the update, and a real sceGuDrawArray every N draws
// (sceGuGetMemory also updates the stall; sceGuFinish publishes the rest
// before any sync). Research only: on the PSP-3000 benchmark, groups of 4
// made submission ~0.8 ms/frame slower than one update per draw (the GE
// then reads in bursts while the CPU writes the list), so the default is 1.
// Only 32-bit targets can encode their pointers as GE addresses.
#if defined(PSP_DRAWS_PER_STALL_UPDATE)
constexpr u32 kDrawsPerStallUpdate = PSP_DRAWS_PER_STALL_UPDATE;
#else
constexpr u32 kDrawsPerStallUpdate = 1;
#endif
u32 g_drawsSinceStallUpdate = 0;

static void GuDrawArray(int prim, int vtype, int count, const void* indices,
                        const void* vertices) {
    if constexpr (sizeof(void*) == sizeof(u32)) {
        if (++g_drawsSinceStallUpdate < kDrawsPerStallUpdate) {
            u32 words[6];
            const u32 n = EncodeGuDrawArray(prim, vtype, count,
                static_cast<u32>(reinterpret_cast<std::uintptr_t>(indices)),
                static_cast<u32>(reinterpret_cast<std::uintptr_t>(vertices)), words);
            for (u32 i = 0; i < n; ++i) {
                sceGuSendCommandi(static_cast<int>(words[i] >> 24),
                                  static_cast<int>(words[i] & 0xffffffu));
            }
            return;
        }
    }
    g_drawsSinceStallUpdate = 0;
    sceGuDrawArray(prim, vtype, count, indices, vertices);
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

// eDRAM page slots per format (32/64/128 KiB each). A page without a slot
// is sampled from RAM: slower for the GE and contending with the CPU, but
// identical. Overridable to trade eDRAM for that bandwidth.
#if !defined(PSP_VRAM_T4_SLOTS)
#define PSP_VRAM_T4_SLOTS 4
#endif
// 3 T8 and 1 RGBA16 slot keep the eDRAM at 1.5 MiB (with the three 5650
// buffers and depth): on the benchmark the second RGBA16 slot was never used
// and the fourth T8 one saved half a RAM-sampled bind per frame in one scene.
#if !defined(PSP_VRAM_T8_SLOTS)
#define PSP_VRAM_T8_SLOTS 3
#endif
#if !defined(PSP_VRAM_RGBA16_SLOTS)
#define PSP_VRAM_RGBA16_SLOTS 1
#endif

struct StaticPspVram {
    void* frameBuffer = nullptr;
    void* displayBuffer = nullptr;
    void* depthBuffer = nullptr;
    std::array<StaticPageSlot, PSP_VRAM_T4_SLOTS> t4{};
    std::array<StaticPageSlot, PSP_VRAM_T8_SLOTS> t8{};
    std::array<StaticPageSlot, PSP_VRAM_RGBA16_SLOTS> rgba16{};
    bool initialized = false;
    u32 allocatedBytes = 0;
};

alignas(16) DisplayListStorage g_displayListStorage = {};
StaticPspVram g_staticVram = {};
pspContext* g_activeContext = nullptr;
pspShader* g_activeShader = nullptr;

// Relative eDRAM offset of the colour buffer the GE is drawing.
static std::uintptr_t DrawFramebufferOffset() {
#if defined(PSP_TRIPLE_BUFFER)
    return static_cast<std::uintptr_t>(g_drawFramebuffer) * kFramebufferStride;
#else
    return reinterpret_cast<std::uintptr_t>(
        g_drawingSecondBuffer ? g_staticVram.displayBuffer : g_staticVram.frameBuffer);
#endif
}

#if defined(PSP_TRIPLE_BUFFER)
// sceGuStart re-emits the SDK's own draw buffer, which SwapBuffers no longer
// rotates; point the GE at ours after every list start.
static void BindDrawFramebuffer() {
    sceGuDrawBufferList(kFramebufferPsm, reinterpret_cast<void*>(DrawFramebufferOffset()),
                        kBufferWidth);
}
#endif
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
    // Allegrex converts floats to signed words directly. Converting straight
    // to an unsigned byte makes GCC emit the general >=2^31 unsigned path,
    // despite this value being clamped to 0..255.
    return static_cast<u8>(static_cast<s32>(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

static u32 MakeGeometryColor(float r, float g, float b) {
    return static_cast<u32>(ToPspVertexByte(r))
        | (static_cast<u32>(ToPspVertexByte(g)) << 8)
        | (static_cast<u32>(ToPspVertexByte(b)) << 16)
        | 0xFF000000u;
}

static bool PackIntegralTexCoord(float value, s16* out) {
    // Ordered bounds reject NaN/Inf before conversion. Truncation is exact
    // for integral UVs; the round trip rejects fractional coordinates.
    if (!(value >= -32768.0f && value <= 32767.0f)) return false;
    const s16 packed = static_cast<s16>(value);
    if (static_cast<float>(packed) != value) return false;
    *out = packed;
    return true;
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
    ++g_gumMatrixSerial;
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
            // PSX semi-transparency mode 3: B + F/4 (as the GL renderer's
            // constant 0.25 source, one destination). A quarter destination
            // darkened everything behind trails and mode-3 effects.
            GuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00404040, 0x00FFFFFF);
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

bool pspPrimBuffer::GetSingleMaterialKey(u32 start, u32 end, u32* key) const {
    if (!key || primType != PDDI_PRIM_TRIANGLES || start >= end
        || end > indices.size()) {
        return false;
    }

    const u16 firstIndex = indices[start];
    if (!HasMaterialKey(firstIndex)) return false;
    const u32 firstKey = GetMaterialKey(firstIndex);
    for (u32 cursor = start + 1u; cursor < end; ++cursor) {
        const u16 index = indices[cursor];
        if (!HasMaterialKey(index) || GetMaterialKey(index) != firstKey) return false;
    }
    *key = firstKey;
    return true;
}

pspPrimBuffer::~pspPrimBuffer() {
    PrepareForMutation();
    if (packedVerticesAllocatedBytes != 0) {
        PspPerf::AddRendererMemory(-static_cast<s32>(packedVerticesAllocatedBytes));
    }
}

void pspPrimBuffer::RefreshFloatVertices() {
    if (!floatStale) return;
    // Only reached on the list-memory path, where the compact stream holds
    // validated integral UVs; the round trip to float is exact.
    const std::size_t count = std::min(vertices.size(), packedVertices.size());
    for (std::size_t i = 0; i < count; ++i) {
        const PspPackedVertex& source = packedVertices[i];
        vertices[i] = {static_cast<float>(source.u), static_cast<float>(source.v),
                       source.color, source.x, source.y, source.z};
    }
    floatStale = false;
    // Staged submissions copy from the cached CPU view; only a direct indexed
    // submission lets the GE read this persistent array.
    if (!UsesStagedIndexedVertices()) {
        WritebackDcacheRange(vertices.data(), sizeof(PspVertex) * vertices.size());
    }
}

void pspPrimBuffer::RebuildPackedVertices() {
    packedVerticesValid = false;
    baseColoursValid = false;
    colourPatchActive = false;
    if (vertices.empty()
        || (vertexFormat & (PDDI_V_UV | PDDI_V_TEXINFO)) != (PDDI_V_UV | PDDI_V_TEXINFO)) {
        return;
    }

    // Validate before reserving so real-texture buffers with fractional UVs
    // do not retain a needless second vertex array after falling back.
    for (const PspVertex& source : vertices) {
        s16 u = 0, v = 0;
        if (!PackIntegralTexCoord(source.u, &u) || !PackIntegralTexCoord(source.v, &v)) {
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
        // The validation pass established exact integral, in-range values.
        target.u = static_cast<s16>(source.u);
        target.v = static_cast<s16>(source.v);
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
    // A queued opaque draw keeps a pointer to this buffer until the next
    // render barrier. Flush before fencing, replacing or destroying storage
    // so the queue cannot replay stale geometry.
    if (g_activeContext) g_activeContext->FlushQueuedOpaqueDraws();
    if (directSubmissionSerial != 0 && directSubmissionSerial == g_submissionSerial
        && g_activeContext) {
        g_activeContext->FlushList();
    }
    directSubmissionSerial = 0;
    ++contentSerial;
}

void pspPrimBuffer::MarkDirectSubmission() {
    directSubmissionSerial = g_submissionSerial;
}

// One vertex's page material: tpage/CBA must be finite and within 0..65535.
static bool PackMaterialKey(float tpageValue, float cbaValue, u32* key) {
    const bool materialIsValid = std::isfinite(tpageValue) && tpageValue >= 0.0f
        && tpageValue <= 65535.0f && std::isfinite(cbaValue) && cbaValue >= 0.0f
        && cbaValue <= 65535.0f;
    *key = materialIsValid
        ? (static_cast<u32>(static_cast<s32>(tpageValue)) << 16)
            | static_cast<u32>(static_cast<s32>(cbaValue))
        : 0u;
    return materialIsValid;
}

void BuildPspMaterialKeys(const float* tpage, u32 tpageStride, const float* cba, u32 cbaStride,
                          u32 count, u32* keys, u8* valid) {
    for (u32 i = 0; i < count; ++i) {
        valid[i] = PackMaterialKey(tpage[static_cast<std::size_t>(i) * tpageStride],
                                   cba[static_cast<std::size_t>(i) * cbaStride], &keys[i]) ? 1u : 0u;
    }
}

bool pspPrimBuffer::UpdateDynamicMaterials(const float* tpage, u32 tpageStride,
                                           const float* cba, u32 cbaStride, u32 count) {
    if (materialRawBits.size() != count) {
        materialRawBits.assign(count, 0u);
        materialRawBitsValid = false;
    }
    const bool reuseMaterialKeys = materialRawBitsValid;
    bool materialChanged = false;
    for (u32 i = 0; i < count; ++i) {
        const float* tpageLane = tpage + static_cast<std::size_t>(i) * tpageStride;
        const float* cbaLane = cba + static_cast<std::size_t>(i) * cbaStride;
        // Suit animation rewrites few material lanes; skip the finite/range
        // validation and key packing when the raw bits are unchanged.
        u32 tpageBits = 0;
        u32 cbaBits = 0;
        std::memcpy(&tpageBits, tpageLane, sizeof(tpageBits));
        std::memcpy(&cbaBits, cbaLane, sizeof(cbaBits));
        const u64 rawMaterial = (static_cast<u64>(tpageBits) << 32) | cbaBits;
        if (!reuseMaterialKeys || materialRawBits[i] != rawMaterial) {
            materialRawBits[i] = rawMaterial;
            materialChanged = true;
            materialValid[i] = PackMaterialKey(*tpageLane, *cbaLane, &materialKeys[i]) ? 1u : 0u;
        }
    }
    materialRawBitsValid = true;
    if (materialChanged) materialRunsValid = false;
    return materialChanged;
}

bool pspPrimBuffer::SetDynamicVertexData(const void* data, u32 count,
                                         bool uvIsIntegral) {
    lastVertexInputValid = false;
    PspPerf::ScopeTimer packTimer(PspPerf::RendererCounter::VertexPackUs);
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
    packedLaneSource = 0;
    UpdateDynamicMaterials(source + 8, 10u, source + 9, 10u, count);
    // List-memory builds submit page geometry through the compact stream
    // only. With integral UVs, write just that stream and rebuild the float
    // copy on demand (vramHandle == 0 draws). Other builds never read the
    // compact stream, so stop maintaining it here.
    const bool packedOnly = kPackedStreamConsumed && uvIsIntegral && packedVerticesValid;
    bool packedUvValid = kPackedStreamConsumed && packedVerticesValid;
    for (u32 i = 0; i < count; ++i) {
        const float* input = source + static_cast<std::size_t>(i) * 10u;
        const u32 color = MakeGeometryColor(input[3], input[4], input[5]);
        if (packedOnly) {
            // Same values as the generic path below; see uvIsIntegral.
            PspPackedVertex& packed = packedVertices[i];
            packed.u = static_cast<s16>(input[6]);
            packed.v = static_cast<s16>(input[7]);
            packed.color = color;
            packed.x = input[0];
            packed.y = input[1];
            packed.z = input[2];
            continue;
        }

        PspVertex& target = vertices[i];
        target.x = input[0];
        target.y = input[1];
        target.z = input[2];
        target.color = color;
        target.u = input[6];
        target.v = input[7];

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
            if (!PackIntegralTexCoord(target.u, &u)
                || !PackIntegralTexCoord(target.v, &v)) {
                packedUvValid = false;
                continue;
            }
        }
        PspPackedVertex& packed = packedVertices[i];
        packed.u = u;
        packed.v = v;
        packed.color = target.color;
        packed.x = target.x;
        packed.y = target.y;
        packed.z = target.z;
    }
    packedVerticesValid = packedOnly || packedUvValid;
    floatStale = packedOnly;
    // Colours were rewritten: a pending colour patch no longer applies.
    baseColoursValid = false;
    colourPatchActive = false;
    if (!sourceWillBeStaged) {
        if (!packedOnly) {
            WritebackDcacheRange(vertices.data(), sizeof(PspVertex) * vertices.size());
        }
        if (packedVerticesValid) {
            WritebackDcacheRange(packedVertices.data(),
                sizeof(PspPackedVertex) * packedVertices.size());
        }
    }
    return true;
}

PspPackedVertex* pspPrimBuffer::BeginPackedUpload(u32 count, bool exclusive) {
    lastVertexInputValid = false;
    constexpr u32 kDynamicFormat = PDDI_V_POSITION | PDDI_V_COLOUR
        | PDDI_V_UV | PDDI_V_TEXINFO;
    // SetDynamicVertexData's compact-only path (uvIsIntegral) and nothing else.
    if (!kPackedStreamConsumed || !packedVerticesValid
        || count != vertices.size() || vertexFormat != kDynamicFormat) {
        return nullptr;
    }
    // A shared buffer is rewritten for each drawable within a frame, so its
    // draws copy the vertices into the list (as UpdatePspPrimBufferDynamic-
    // Vertices). An exclusive one changes once per frame, after the previous
    // list was fenced; PrepareForMutation still fences a second same-frame
    // upload.
    if (!exclusive) PreferStagedIndexedVertices();
    packedUploadStaged = UsesStagedIndexedVertices();
    PrepareForMutation();
    return packedVertices.data();
}

void pspPrimBuffer::EndPackedUpload(const float* tpage, u32 tpageStride,
                                    const float* cba, u32 cbaStride, u32 count,
                                    u32 laneSource, const u32* laneKeys,
                                    const u8* laneValid) {
    PspPerf::ScopeTimer packTimer(PspPerf::RendererCounter::VertexPackUs);
    const bool lanesHeld = laneSource != 0u && laneSource == packedLaneSource
        && (packedLaneKeysCopied || (materialRawBitsValid && materialRawBits.size() == count))
        && packedLaneTpage == tpage && packedLaneCba == cba
        && packedLaneTpageStride == tpageStride && packedLaneCbaStride == cbaStride
        && packedLaneCount == count;
    PspPerf::CountRenderer(lanesHeld ? PspPerf::RendererCounter::PackedLaneHolds
                                     : PspPerf::RendererCounter::PackedLaneCompares);
    if (!lanesHeld) {
        if (laneKeys && laneValid && laneSource != 0u && materialKeys.size() == count
            && materialValid.size() == count) {
            // Precomputed lanes (another instance's suit on this shared
            // buffer): the same keys UpdateDynamicMaterials would produce,
            // compared and copied as arrays.
            if (std::memcmp(materialKeys.data(), laneKeys, count * sizeof(u32)) != 0
                || std::memcmp(materialValid.data(), laneValid, count) != 0) {
                std::memcpy(materialKeys.data(), laneKeys, count * sizeof(u32));
                std::memcpy(materialValid.data(), laneValid, count);
                materialRunsValid = false;
            }
            // The raw bits no longer describe the keys; a later lane update
            // without precomputed keys converts every vertex.
            materialRawBitsValid = false;
            packedLaneKeysCopied = true;
        }
        else {
            UpdateDynamicMaterials(tpage, tpageStride, cba, cbaStride, count);
            packedLaneKeysCopied = false;
        }
        packedLaneSource = laneSource;
        packedLaneTpage = tpage;
        packedLaneCba = cba;
        packedLaneTpageStride = tpageStride;
        packedLaneCbaStride = cbaStride;
        packedLaneCount = count;
    }
    packedVerticesValid = true;
    floatStale = true;
    baseColoursValid = false;
    colourPatchActive = false;
    if (!packedUploadStaged) {
        WritebackDcacheRange(packedVertices.data(),
            sizeof(PspPackedVertex) * packedVertices.size());
    }
}

bool pspPrimBuffer::UploadColoursOnly(const float* source, u32 count) {
    // While the kept input is valid the buffer holds exactly what
    // SetVertexData(lastVertexInput) left (every other writer invalidates
    // it). An input equal to it, bit for bit, outside the colour lanes would
    // make the full upload rewrite only the colours: do just that.
    if ((vertexFormat & PDDI_V_COLOUR) == 0) return false;
    const std::size_t stride = VertexInputStride();
    const std::size_t colourLane = (vertexFormat & PDDI_V_POSITION) ? 3u : 0u;
    const std::size_t tail = stride - colourLane - 3u;
    const float* last = lastVertexInput.data();
    for (u32 i = 0; i < count; ++i) {
        const float* a = source + static_cast<std::size_t>(i) * stride;
        const float* b = last + static_cast<std::size_t>(i) * stride;
        if ((colourLane != 0u && std::memcmp(a, b, colourLane * sizeof(float)) != 0)
            || (tail != 0u && std::memcmp(a + colourLane + 3u, b + colourLane + 3u,
                                          tail * sizeof(float)) != 0)) {
            return false;
        }
    }
    if (directSubmissionSerial != 0 && directSubmissionSerial == g_submissionSerial) {
        stagedIndexedVertices = true;
        stagedIndexedSerial = g_submissionSerial;
        preferStagedIndexedVertices = true;
    }
    PrepareForMutation();
    // As the full upload leaves them (the float stream was current: nothing
    // since the kept upload made it stale).
    baseColoursValid = false;
    colourPatchActive = false;
    for (u32 i = 0; i < count; ++i) {
        const float* input = source + static_cast<std::size_t>(i) * stride + colourLane;
        const u32 color = MakeGeometryColor(input[0], input[1], input[2]);
        vertices[i].color = color;
        if (packedVerticesValid) packedVertices[i].color = color;
    }
    WritebackDcacheRange(vertices.data(), sizeof(PspVertex) * vertices.size());
    if (packedVerticesValid) {
        WritebackDcacheRange(packedVertices.data(), packedVertices.size() * sizeof(PspPackedVertex));
    }
    std::memcpy(lastVertexInput.data(), source, lastVertexInput.size() * sizeof(float));
    PspPerf::CountRenderer(PspPerf::RendererCounter::VertexColourUploads);
    return true;
}

std::size_t pspPrimBuffer::VertexInputStride() const {
    return ((vertexFormat & PDDI_V_POSITION) ? 3u : 0u)
        + ((vertexFormat & PDDI_V_COLOUR) ? 3u : 0u)
        + ((vertexFormat & PDDI_V_UV) ? 2u : 0u)
        + ((vertexFormat & PDDI_V_TEXINFO) ? 2u : 0u);
}

void pspPrimBuffer::SetVertexData(const void* data, u32 count) {
    PspPerf::ScopeTimer packTimer(PspPerf::RendererCounter::VertexPackUs);
    if (!data) return;
    const std::size_t inputFloats = static_cast<std::size_t>(count) * VertexInputStride();
    if (lastVertexInputValid && lastVertexInput.size() == inputFloats
        && std::memcmp(lastVertexInput.data(), data, inputFloats * sizeof(float)) == 0) {
        // The same input as the upload the buffer holds (effect animations
        // re-send unchanged frames): nothing would change, and its recorded
        // draw stays valid.
        PspPerf::CountRenderer(PspPerf::RendererCounter::VertexUploadSkips);
        return;
    }
    if (lastVertexInputValid && lastVertexInput.size() == inputFloats
        && count == vertices.size() && UploadColoursOnly(static_cast<const float*>(data), count)) {
        return;
    }
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
    materialRawBitsValid = false;
    floatStale = false;
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
    // Per-frame effect geometry re-sends whole arrays: once the compact
    // stream has its storage, build it in this pass (exactly what
    // RebuildPackedVertices derives from the float vertices) instead of
    // re-reading the floats to validate and then to copy them.
    const bool packInline = count > 0 && hasUv && hasTexInfo
        && packedVertices.capacity() >= count;
    bool packedOk = packInline;
    if (packInline) {
        packedVerticesValid = false;
        baseColoursValid = false;
        colourPatchActive = false;
        packedVertices.resize(count);
    }
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
        const u32 color = MakeGeometryColor(r, g, b);
        vertices[i] = {u, v, color, x, y, z};
        if (packedOk) {
            s16 packedU = 0, packedV = 0;
            if (PackIntegralTexCoord(u, &packedU) && PackIntegralTexCoord(v, &packedV)) {
                packedVertices[i] = {packedU, packedV, color, x, y, z};
            }
            else {
                packedOk = false;
            }
        }
        // Without TEXINFO, tpage stays -1: no page material.
        materialValid[i] = PackMaterialKey(tpage, cba, &materialKeys[i]) ? 1u : 0u;
    }
    WritebackDcacheRange(vertices.data(), sizeof(PspVertex) * vertices.size());
    if (++vertexDataUploads >= 2u) {
        lastVertexInput.assign(source, source + inputFloats);
        lastVertexInputValid = true;
    }
    if (packedOk) {
        const std::size_t allocatedBytes = packedVertices.capacity() * sizeof(PspPackedVertex);
        if (allocatedBytes > packedVerticesAllocatedBytes) {
            PspPerf::AddRendererMemory(static_cast<s32>(allocatedBytes - packedVerticesAllocatedBytes));
        }
        packedVerticesAllocatedBytes = allocatedBytes;
        WritebackDcacheRange(packedVertices.data(), packedVertices.size() * sizeof(PspPackedVertex));
        packedVerticesValid = true;
    }
    else if (!packInline) {
        RebuildPackedVertices();
    }
#if !defined(NDEBUG)
    if (probe < 12u) LOG("[PSP Prim] vertex done #%lu", static_cast<unsigned long>(probe));
#endif
}

bool pspPrimBuffer::PatchDynamicAttributes(const u8* rawData, u32 rawSize,
                                           const u32* uvOffsets,
                                           const u32* colourOffsets,
                                           u32 count, bool updateUv,
                                           bool updateColour,
                                           bool packedOnly) {
    lastVertexInputValid = false;
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
    // This patch updates both streams from the float one; bring it current.
    RefreshFloatVertices();
    if (updateColour) {
        baseColoursValid = false;
        colourPatchActive = false;
    }
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
                // UV animation originates in the PSX packet's byte lanes.
                // The page path already satisfies the packed GU coordinate
                // contract; avoid two lround/clamp pairs per vertex.
                packedVertices[i].u = static_cast<s16>(rawData[uvOffsets[i]]);
                packedVertices[i].v = static_cast<s16>(rawData[uvOffsets[i] + 1u]);
            }
            if (updateColour) {
                packedVertices[i].color = vertices[i].color;
            }
        }
    }
    // Block geometry is submitted through the packed page stream. Do not
    // write back the parallel float stream that the GE will not read. The
    // generic path keeps both writebacks for callers that may submit float
    // vertices in a later pass.
    if (!packedOnly || !packedVerticesValid) {
        WritebackDcacheRange(vertices.data(), sizeof(PspVertex) * vertices.size());
    }
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

void pspPrimBuffer::BuildMaterialRuns() {
    PspPerf::CountRenderer(PspPerf::RendererCounter::MaterialRunBuilds);
    materialRuns.clear();
    const u32 count = static_cast<u32>(indices.size());
    for (u32 triangle = 0; triangle + 2u < count; triangle += 3u) {
        const u16 index = indices[triangle];
        const bool valid = HasMaterialKey(index);
        const u32 key = valid ? GetMaterialKey(index) : 0u;
        if (!materialRuns.empty() && materialRuns.back().valid == valid
            && materialRuns.back().key == key) {
            materialRuns.back().end = triangle + 3u;
        }
        else {
            materialRuns.push_back({triangle + 3u, key, valid});
        }
    }
    materialRunsValid = true;
}

bool pspPrimBuffer::FindMaterialRun(u32 start, bool* valid, u32* key, u32* end) {
#if defined(PSP_DISABLE_MATERIAL_RUN_CACHE)
    (void)valid;
    (void)key;
    (void)end;
    return false;  // A/B: DrawPrimBuffer keeps its per-triangle scan
#endif
    if (start % 3u != 0u) return false;
    if (!materialRunsValid) BuildMaterialRuns();
    // First run ending after start: the run that contains its triangle.
    const auto run = std::upper_bound(materialRuns.begin(), materialRuns.end(), start,
        [](u32 position, const MaterialRun& candidate) { return position < candidate.end; });
    if (run == materialRuns.end()) return false;
    *valid = run->valid;
    *key = run->key;
    *end = run->end;
    return true;
}

void pspPrimBuffer::RevalidateIndices() {
    // Indices or (in SetVertexData) material keys changed.
    materialRunsValid = false;
    packedLaneSource = 0;
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

PspPackedVertex* BeginPspPrimBufferPackedUpload(pddiPrimBuffer* base, u32 count, bool exclusive) {
    if (!base) return nullptr;
    return static_cast<pspPrimBuffer*>(base)->BeginPackedUpload(count, exclusive);
}

pspPrimBuffer* pspPrimBuffer::CloneDynamic() const {
    auto* clone = new (std::nothrow) pspPrimBuffer(pddiPrimBufferDesc(
        primType, vertexFormat, static_cast<u32>(vertices.size()), static_cast<u32>(indices.size())));
    if (!clone) return nullptr;
    // Floats as RefreshFloatVertices would make them, without touching the
    // source (the shared buffer may be mid-frame).
    if (floatStale) {
        const std::size_t count = std::min(vertices.size(), packedVertices.size());
        for (std::size_t i = 0; i < count; ++i) {
            const PspPackedVertex& source = packedVertices[i];
            clone->vertices[i] = {static_cast<float>(source.u), static_cast<float>(source.v),
                                  source.color, source.x, source.y, source.z};
        }
    }
    else {
        std::copy(vertices.begin(), vertices.end(), clone->vertices.begin());
    }
    if (packedVerticesValid) {
        clone->packedVertices.assign(packedVertices.begin(), packedVertices.end());
        clone->packedVerticesAllocatedBytes = clone->packedVertices.capacity() * sizeof(PspPackedVertex);
        PspPerf::AddRendererMemory(static_cast<s32>(clone->packedVerticesAllocatedBytes));
        clone->packedVerticesValid = true;
    }
    clone->materialKeys = materialKeys;
    clone->materialValid = materialValid;
    clone->materialRawBits = materialRawBits;
    clone->materialRawBitsValid = materialRawBitsValid;
    std::copy(indices.begin(), indices.end(), clone->indices.begin());
    clone->indicesValid = indicesValid;
    WritebackDcacheRange(clone->vertices.data(), sizeof(PspVertex) * clone->vertices.size());
    if (clone->packedVerticesValid) {
        WritebackDcacheRange(clone->packedVertices.data(),
            sizeof(PspPackedVertex) * clone->packedVertices.size());
    }
    WritebackDcacheRange(clone->indices.data(), sizeof(u16) * clone->indices.size());
    return clone;
}

pddiPrimBuffer* ClonePspDynamicPrimBuffer(pddiPrimBuffer* source) {
    return source ? static_cast<pspPrimBuffer*>(source)->CloneDynamic() : nullptr;
}

void EndPspPrimBufferPackedUpload(pddiPrimBuffer* base, const float* tpage, u32 tpageStride,
                                  const float* cba, u32 cbaStride, u32 count,
                                  u32 laneSource, const u32* laneKeys, const u8* laneValid) {
    if (!base) return;
    static_cast<pspPrimBuffer*>(base)->EndPackedUpload(tpage, tpageStride, cba, cbaStride, count,
                                                       laneSource, laneKeys, laneValid);
}

bool UpdatePspPrimBufferDynamicVertices(pddiPrimBuffer* base,
                                         const void* data, u32 count,
                                         bool uvIsIntegral) {
    if (!base) return false;
    auto* buffer = static_cast<pspPrimBuffer*>(base);
    buffer->PreferStagedIndexedVertices();
    return buffer->SetDynamicVertexData(data, count, uvIsIntegral);
}

bool pspPrimBuffer::PatchColours(const u32* colours, u32 count) {
    lastVertexInputValid = false;
    // Nothing to restore: never patched, or an upload already replaced it.
    if (!colours && !colourPatchActive) return true;
    if (count == 0 || count != vertices.size()) return false;
    // The consumed stream must be current: the compact one on list-memory
    // builds (float rebuilt from it on demand), the float one elsewhere.
    if (kPackedStreamConsumed ? !packedVerticesValid : floatStale) return false;
    if (colours && !baseColoursValid) {
        if (baseColours.capacity() < count) baseColours.reserve(count);
        baseColours.resize(count);
        for (u32 i = 0; i < count; ++i) {
            baseColours[i] = kPackedStreamConsumed ? packedVertices[i].color : vertices[i].color;
        }
        baseColoursValid = true;
    }
    const u32* source = colours ? colours : baseColours.data();
    if (colours && colourPatchActive) {
        bool same = true;
        for (u32 i = 0; i < count && same; ++i) {
            same = (kPackedStreamConsumed ? packedVertices[i].color : vertices[i].color) == colours[i];
        }
        if (same) return true;  // already holds them: no mutation
    }

    // A buffer the GE may still read in this list (an earlier instance drawn
    // directly) would need a fence: stage it from now on, as SetVertexData
    // does, so alternating colours between instances never fence again.
    if (directSubmissionSerial != 0 && directSubmissionSerial == g_submissionSerial) {
        stagedIndexedVertices = true;
        stagedIndexedSerial = g_submissionSerial;
        preferStagedIndexedVertices = true;
    }
    PrepareForMutation();
    if (kPackedStreamConsumed) {
        for (u32 i = 0; i < count; ++i) packedVertices[i].color = source[i];
        if (colours) {
            // The float copy keeps its base colours; mark it stale while lit.
            colourPatchStaledFloat = !floatStale;
            floatStale = true;
        }
        else if (floatStale && colourPatchStaledFloat) {
            // Not rebuilt while lit: it still holds exactly the base colours.
            floatStale = false;
        }
        else if (!floatStale) {
            // Rebuilt from the lit stream during the patch: restore it too.
            for (u32 i = 0; i < count; ++i) vertices[i].color = source[i];
        }
    }
    else {
        for (u32 i = 0; i < count; ++i) vertices[i].color = source[i];
        if (packedVerticesValid) {
            for (u32 i = 0; i < count; ++i) packedVertices[i].color = source[i];
        }
    }
    if (!colours) colourPatchStaledFloat = false;
    colourPatchActive = colours != nullptr;
    return true;
}

u32 MakePspGeometryColour(float r, float g, float b) {
    return MakeGeometryColor(r, g, b);
}

bool PatchPspPrimBufferColours(pddiPrimBuffer* base, const u32* colours, u32 count) {
    if (!base || !colours) return false;
    return static_cast<pspPrimBuffer*>(base)->PatchColours(colours, count);
}

bool RestorePspPrimBufferColours(pddiPrimBuffer* base, u32 count) {
    if (!base) return false;
    return static_cast<pspPrimBuffer*>(base)->PatchColours(nullptr, count);
}

bool PspPrimBufferHasPatchedColours(const pddiPrimBuffer* base) {
    return base && static_cast<const pspPrimBuffer*>(base)->HasPatchedColours();
}

bool ReadPspPrimBufferColours(const pddiPrimBuffer* base, u32* colours, u32 count) {
    if (!base || !colours) return false;
    const auto* buffer = static_cast<const pspPrimBuffer*>(base);
    if (count != buffer->GetVertexCount()) return false;
    if (kPackedStreamConsumed) {
        if (!buffer->HasPackedVertices()) return false;
        for (u32 i = 0; i < count; ++i) colours[i] = buffer->GetPackedVertices()[i].color;
    }
    else {
        for (u32 i = 0; i < count; ++i) colours[i] = buffer->GetVertices()[i].color;
    }
    return true;
}

namespace {
const pddiPrimBuffer* g_litColourDrawBuffer = nullptr;
}

void BeginPspLitColourDraw(pddiPrimBuffer* buffer) { g_litColourDrawBuffer = buffer; }
void EndPspLitColourDraw() { g_litColourDrawBuffer = nullptr; }

bool UpdatePspPrimBufferDynamic(pddiPrimBuffer* base, const u8* rawData,
                                u32 rawSize, const u32* uvOffsets,
                                const u32* colourOffsets, u32 vertexCount,
                                bool updateUv, bool updateColour,
                                bool packedOnly) {
    if (!base) return false;
    return static_cast<pspPrimBuffer*>(base)->PatchDynamicAttributes(
        rawData, rawSize, uvOffsets, colourOffsets, vertexCount,
        updateUv, updateColour, packedOnly);
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
    g_drawingSecondBuffer = false;
#if defined(PSP_TRIPLE_BUFFER)
    g_drawFramebuffer = 0;
    g_shownFramebuffer = 1;
    g_replacedFramebuffer = 2;
#endif
    const u32 edramBytes = static_cast<u32>(sceGeEdramGetSize());
    if (edramBytes < kReservedEdramBytes) return false;
    const auto edramBase = reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr());
    g_staticVram.frameBuffer = nullptr; // relative offset zero is valid
    g_staticVram.displayBuffer = reinterpret_cast<void*>(kFramebufferStride);
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
    sceGuDrawBuffer(kFramebufferPsm,
                    g_staticVram.initialized ? g_staticVram.frameBuffer : reinterpret_cast<void*>(0),
                    kBufferWidth);
    sceGuDispBuffer(kWidth, kHeight,
                    g_staticVram.initialized
                        ? g_staticVram.displayBuffer
                        : reinterpret_cast<void*>(kFramebufferStride),
                    kBufferWidth);
    // Two 32-bit colour buffers consume 2 * (512 * 272 * 4).  The PSP depth
    // surface is 16-bit, so placing it immediately after those buffers keeps
    // the complete framebuffer layout below the 2 MiB eDRAM limit.  The old
    // *8 offset treated depth as 64-bit and overran VRAM, corrupting GE state.
    // (Three 16-bit buffers: 3 * (512 * 272 * 2), depth right after them.)
    sceGuDepthBuffer(g_staticVram.initialized
                         ? g_staticVram.depthBuffer
                         : reinterpret_cast<void*>(kFramebufferBytes),
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
#if defined(PSP_TRIPLE_BUFFER)
    // The previous swap must reach the screen before this one is queued: a
    // second queued buffer would replace it, leaving on screen the buffer
    // drawn next. Frames slower than one refresh had a VBlank since the last
    // swap and never wait; faster ones are held to the display rate.
    WaitForVblankSinceSwap();
    const auto edramBase = reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr());
    sceDisplaySetFrameBuf(reinterpret_cast<void*>(edramBase + DrawFramebufferOffset()),
                          kBufferWidth, kFramebufferPsm, PSP_DISPLAY_SETBUF_NEXTVSYNC);
    // Read after queueing, as in the double-buffered path below.
    g_swapVcount = sceDisplayGetVcount();
    g_swapVblankPending = true;
    // Queued: the buffer just finished. On screen until the next VBlank: the
    // one shown so far. Free to draw: the buffer that one replaced.
    const u32 finished = g_drawFramebuffer;
    g_drawFramebuffer = g_replacedFramebuffer;
    g_replacedFramebuffer = g_shownFramebuffer;
    g_shownFramebuffer = finished;
#else
    sceGuSwapBuffers();
    g_drawingSecondBuffer = !g_drawingSecondBuffer;
#endif
#if defined(PSP_TRIPLE_BUFFER)
#elif defined(PSP_IMMEDIATE_VBLANK_WAIT)
#if defined(PSP_PERF_TELEMETRY)
    const u32 vblankStart = sceKernelGetSystemTimeLow();
#endif
    sceDisplayWaitVblankStart();
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::CountRenderer(PspPerf::RendererCounter::VblankUs,
        sceKernelGetSystemTimeLow() - vblankStart);
#endif
#else
    // Deferred VBlank: the buffer just released for drawing stays on screen
    // until the next VBlank, but only GE writes can tear it. The CPU goes on
    // with the next frame's logic now; WaitForSwapVblank() blocks right
    // before that frame's first GE command if no VBlank has happened yet.
    g_swapVcount = sceDisplayGetVcount();
    g_swapVblankPending = true;
#endif
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

#if defined(PSP_PERF_TELEMETRY)
bool WritePspLastFrame(const char* path) {
    if (!path) return false;
    // The frame queued by the last SwapBuffers: FinishList synced the GE, and
    // the buffer is not drawn to again until two swaps later.
#if defined(PSP_TRIPLE_BUFFER)
    const std::uintptr_t offset = static_cast<std::uintptr_t>(g_shownFramebuffer) * kFramebufferStride;
#else
    const std::uintptr_t offset = g_drawingSecondBuffer ? 0u : kFramebufferStride;
#endif
    const auto* pixels = reinterpret_cast<const u8*>(
        (reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr()) + offset) | 0x40000000u);
    constexpr u32 kRowBytes = kWidth * 3u;  // 1440: already a multiple of 4
    constexpr u32 kImageBytes = kRowBytes * kHeight;
    std::vector<u8> bmp(54u + kImageBytes, 0u);
    const auto put32 = [&](u32 at, u32 value) {
        for (u32 i = 0; i < 4u; ++i) bmp[at + i] = static_cast<u8>(value >> (8u * i));
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, 54u + kImageBytes);
    put32(10, 54u);
    put32(14, 40u);
    put32(18, kWidth);
    put32(22, kHeight);
    bmp[26] = 1;
    bmp[28] = 24;
    put32(34, kImageBytes);
    u8* out = bmp.data() + 54u;
    for (int y = kHeight - 1; y >= 0; --y) {
        const u8* line = pixels + static_cast<std::size_t>(y) * kBufferWidth * kFramebufferPixelBytes;
        for (int x = 0; x < kWidth; ++x, out += 3) {
            u32 r, g, b;
            if (kFramebufferPixelBytes == 2u) {
                const u32 c = line[x * 2] | (static_cast<u32>(line[x * 2 + 1]) << 8);
                r = (c & 0x1Fu) << 3; r |= r >> 5;
                g = ((c >> 5) & 0x3Fu) << 2; g |= g >> 6;
                b = ((c >> 11) & 0x1Fu) << 3; b |= b >> 5;
            }
            else {
                r = line[x * 4]; g = line[x * 4 + 1]; b = line[x * 4 + 2];
            }
            out[0] = static_cast<u8>(b);
            out[1] = static_cast<u8>(g);
            out[2] = static_cast<u8>(r);
        }
    }
    // Same location as the benchmark reports (p3d::io resolves the data dir).
    return p3d::io::WriteFile(path, bmp.data(), bmp.size());
}
#endif

#if defined(PSP_BENCH_COMBAT)
// Frame of the scripted combat input; restarted with each benchmark scenario
// (PSP_BENCH_FIXED_STEP) so the input no longer depends on loading time.
static u32 g_benchCombatFrame = 0;

void ResetPspBenchCombatInput() {
    g_benchCombatFrame = 0;
}
#endif

void pspGamepad::Poll() {
    SceCtrlData data{};
    // Peek, not Read: Read blocks until the next VBlank sample whenever the
    // frame's sample was already consumed, idling the game thread for up to
    // a whole refresh in what the frame timing counts as logic.
    if (sceCtrlPeekBufferPositive(&data, 1) < 0) {
        connected = false;
        buttons = 0;
        axes.fill(0.0f);
        return;
    }
    connected = true;
#if defined(PSP_AUTOPLAY)
    // QA only: scripted input so the game can be exercised without a person.
    // Walks left/right/up/down in 2 s blocks and presses confirm, punch, kick and
    // jump on staggered periods, so each phase gets walking, combat and dialogue.
    static u32 autoFrame = 0;
    const u32 autoF = autoFrame++;
    const u32 autoPhase = (autoF / 120u) % 4u;
    data.Buttons = 0;
    data.Lx = 128;
    data.Ly = 128;
    if (autoPhase == 0u) data.Lx = 255;
    else if (autoPhase == 1u) data.Ly = 0;
    else if (autoPhase == 2u) data.Lx = 0;
    else data.Ly = 255;
    if (autoF % 45u == 0u) data.Buttons |= PSP_CTRL_CROSS;
    if (autoF % 37u == 10u) data.Buttons |= PSP_CTRL_SQUARE;
    if (autoF % 53u == 20u) data.Buttons |= PSP_CTRL_TRIANGLE;
    if (autoF % 61u == 30u) data.Buttons |= PSP_CTRL_CIRCLE;
    // Heartbeat that does not depend on the diagnostic log: one line every 300
    // frames in AUTOPLAY_LOG.TXT (appended), so QA can prove input is reaching the game.
    if (autoF % 300u == 0u) {
        if (FILE* autoLog = std::fopen("AUTOPLAY_LOG.TXT", "a")) {
            std::fprintf(autoLog, "frame=%lu phase=%lu buttons=%08lX\n",
                         static_cast<unsigned long>(autoF), static_cast<unsigned long>(autoPhase),
                         static_cast<unsigned long>(data.Buttons));
            std::fclose(autoLog);
        }
    }
#endif
#if defined(PSP_BENCH_COMBAT)
    // Benchmark only: scripted combat input, no file I/O. Equal steps right
    // and left keep Jackie near the checkpoint (walking on blindly found a
    // deadly drop in scenario 6), with punches (Square) and kicks (Triangle)
    // against the enemies around it.
    const u32 benchFrame = g_benchCombatFrame++;
    data.Buttons = 0;
    data.Lx = (benchFrame % 120u) < 60u ? 255 : 0;
    data.Ly = 128;
    if (benchFrame % 8u < 2u) data.Buttons |= PSP_CTRL_SQUARE;
    if (benchFrame % 12u == 6u) data.Buttons |= PSP_CTRL_TRIANGLE;
#endif
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
        + sizeof(nativeRemap) + sizeof(nativeRemapStamp) + sizeof(nativeRemapSource)));
    if (g_activeContext == this) g_activeContext = nullptr;
}

void pspContext::BeginFrame() {
    PspPerf::Mark(PspPerf::RuntimeStage::FrameBegin);
    FlushQueuedOpaqueDraws();
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
    geClutEntry = kClutCacheNone;
    gu3DStateReady = false;
    gu2DStateReady = false;
    // A pause backdrop lives only while consecutive frames draw it. If the
    // previous frame did not, release it and clear depth below.
    if (pauseBackdropValid && !pauseBackdropUsedThisFrame) {
        pauseBackdropValid = false;
        pendingDepthClear = true;
    }
    pauseBackdropUsedThisFrame = false;
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
    // First GE command of the frame: the draw target must be off screen.
    WaitForSwapVblank();
    sceGuStart(GU_DIRECT, g_displayListStorage.words);
#if defined(PSP_TRIPLE_BUFFER)
    BindDrawFramebuffer();
    // The PSX dither matrix (the PSPSDK's default, which only the module's
    // first sceGuStart loads): reloaded every frame so a lost GE register
    // cannot leave the 3D draws undithered (seen once for a whole run).
    static ScePspIMatrix4 psxDither = {
        {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    sceGuSetDither(&psxDither);
#endif
    listOpen = true;
    Setup3DState();
    if (pendingDepthClear) ClearDepthNow();
#if defined(PSP_DRAW_MICROBENCH)
    static u32 microbenchFrame = 0;
    ++microbenchFrame;
    if (microbenchFrame == 600u || microbenchFrame == 2400u) RunDrawMicrobench();
#endif
}

#if defined(PSP_DRAW_MICROBENCH)
// Diagnostic only: times 200 repetitions of each piece of a draw call at the
// start of a frame (GE idle) and records the totals in the perf report:
// 0 clock read, 1 list allocation (sceGuGetMemory), 2 eight NOP command
// words (uncached list writes), 3 an off-screen 2D triangle (sceGuDrawArray
// with its stall update), 4 CLUT load + texture image, 5 a 64-byte data-cache
// writeback, 6 a page/CLUT bind alternating between two cached CLUTs,
// 7 a whole off-screen untextured DrawPrimBuffer.
void pspContext::RunDrawMicrobench() {
    constexpr u32 kReps = 200;
    EnsureListSpace(kReps * 160u + 4096u);
    const auto time = [&](u32 slot, auto&& body) {
        const u32 start = sceKernelGetSystemTimeLow();
        for (u32 i = 0; i < kReps; ++i) body(i);
        PspPerf::RecordMicrobench(slot, sceKernelGetSystemTimeLow() - start);
    };
    time(0, [](u32) { (void)sceKernelGetSystemTimeLow(); });
    time(1, [&](u32) { (void)AllocateListMemory(16); });
    time(2, [](u32) {
        for (int word = 0; word < 8; ++word) sceGuSendCommandi(0, 0);
    });
    Setup2DState();
    GuDisable(GU_TEXTURE_2D);
    auto* vertices = static_cast<PspVertex*>(AllocateListMemory(sizeof(PspVertex) * 3));
    if (vertices) {
        for (u32 v = 0; v < 3; ++v) vertices[v] = {0.0f, 0.0f, 0u, -64.0f, -64.0f, 0.0f};
        time(3, [&](u32) {
            sceGuDrawArray(GU_TRIANGLES,
                GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                3, nullptr, vertices);
        });
    }
    if (g_staticVram.initialized && g_staticVram.t8[0].pixels) {
        time(4, [&](u32 i) {
            sceGuClutLoad(16, clutCachePalettes.data() + (i & 1u) * 256u);
            sceGuTexImage(0, kPageSize, kPageSize, kPageSize, g_staticVram.t8[0].pixels);
        });
    }
    time(5, [&](u32 i) { WritebackDcacheRange(framePalettes.data() + (i & 7u) * 32u, 64u); });
    const u32 savedVramHandle = vramHandle;
    if (lastBoundHandle != 0) {
        const u32 handle = lastBoundHandle;
        const u16 tpage = lastBoundTpage;
        const u16 cba = lastBoundCba;
        time(6, [&](u32 i) {
            vramHandle = handle;
            BindPage(tpage, (i & 1u) ? cba : static_cast<u16>(cba ^ 1u), false, true);
        });
    }
    // 7: a whole untextured DrawPrimBuffer of one off-screen triangle (3D
    // state, matrices, staging, draw), hot: compare with the in-game cost.
    {
        static pspPrimBuffer* triangle = nullptr;
        if (!triangle) {
            triangle = new pspPrimBuffer({PDDI_PRIM_TRIANGLES, PDDI_V_POSITION | PDDI_V_COLOUR, 3, 3});
            // Degenerate (zero area): the GE rasterizes nothing.
            const float data[3 * 6] = {
                0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
                0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
                0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
            };
            const u16 indices[3] = {0, 1, 2};
            triangle->SetVertexData(data, 3);
            triangle->SetIndices(indices, 3);
        }
        vramHandle = 0;
        gu3DStateReady = false;
        Setup3DState();
        time(7, [&](u32) { DrawPrimBuffer(triangle, 0, 3); });
    }
    // 8 Mat4 products, 9 sin/cos of distinct (memo-missing) angles, 10 the
    // selected VFPU product mode + 1 (1 = scalar).
    {
        Mat4 a;
        Mat4 b;
        for (int i = 0; i < 16; ++i) {
            a.m[i] = static_cast<float>(i) * 0.25f;
            b.m[i] = static_cast<float>(16 - i) * 0.125f;
        }
        static Mat4 sink;
        time(8, [&](u32) { sink = a * b; a.m[3] = sink.m[5]; });
        static float trig = 0.0f;
        static u32 angleBase = 0;
        time(9, [&](u32 i) {
            float s = 0.0f;
            float c = 0.0f;
            p3dSinCosPsxAngle(static_cast<s32>(angleBase + i * 337u), s, c);
            trig += s + c;
        });
        angleBase += kReps * 337u;
#if defined(P3D_VFPU_MAT4)
        PspPerf::RecordMicrobench(10, static_cast<u32>(g_p3dVfpuMat4Mode + 1));
#endif
    }
    // Skeleton pieces (per rep): 11 three sin/cos pairs of spread angles,
    // 12 a YZX joint rotation with translation, 13 a 64-byte Mat4 copy,
    // 14 a whole 16-joint skeleton build (TRANSFORM on every joint, a few
    // PUSH/POP, no callbacks) whose data stays in cache between reps.
    {
        static float trig = 0.0f;
        static u32 angleBase = 12345u;
        time(11, [&](u32 i) {
            float s = 0.0f, c = 0.0f;
            const u32 a = angleBase + i * 7919u;
            p3dSinCosPsxAngle(static_cast<s32>(a), s, c); trig += s + c;
            p3dSinCosPsxAngle(static_cast<s32>(a * 3u + 101u), s, c); trig += s - c;
            p3dSinCosPsxAngle(static_cast<s32>(a * 5u + 977u), s, c); trig += c - s;
        });
        angleBase += kReps * 7919u;
        static Mat4 rot;
        time(12, [&](u32 i) {
            const s16 a = static_cast<s16>(i * 4099u);
            p3dBuildRotMatrixYZX(a, static_cast<s16>(a * 3), static_cast<s16>(a * 7), rot);
            rot.SetTranslation(static_cast<f32>(i), 2.0f, 3.0f);
        });
        static Mat4 copyDst;
        time(13, [&](u32 i) { rot.m[0] = static_cast<f32>(i); copyDst = rot; });
        static STreeData* skeleton = nullptr;
        static Mat4* matrices = nullptr;
        if (!skeleton) {
            skeleton = new STreeData();
            skeleton->numJoints = 16;
            skeleton->joints = static_cast<STreeJoint*>(std::calloc(16, sizeof(STreeJoint)));
            matrices = new Mat4[16];
            for (u32 j = 0; j < 16u && skeleton->joints; ++j) {
                STreeJoint& joint = skeleton->joints[j];
                new (&joint) STreeJoint();
                joint.flags = STF_TRANSFORM | ((j % 4u) == 0u ? STF_PUSH_MATRIX : 0u)
                    | ((j % 4u) == 3u ? STF_POP_MATRIX : 0u);
                joint.translationX = static_cast<s32>(j * 37u);
                joint.translationY = static_cast<s32>(j * 11u);
                joint.translationZ = -static_cast<s32>(j * 5u);
                joint.rotationX = static_cast<s16>(j * 2311u);
                joint.rotationY = static_cast<s16>(j * 4447u);
                joint.rotationZ = static_cast<s16>(j * 9973u);
                joint.renderScale = 1.0f;
                joint.captureBufferIdx = -1;
            }
        }
        if (skeleton->joints) {
            time(14, [&](u32 i) {
                skeleton->joints[i & 15u].rotationY = static_cast<s16>(i * 131u);
                skeleton->ComputeWorldMatrices(matrices);
            });
        }
    }
    vramHandle = savedVramHandle;
    // The texture/CLUT registers changed outside the caches.
    pageBindingValid = false;
    clutBindingValid = false;
    geClutEntry = kClutCacheNone;
    g_clutGeContentValid = false;
    gu3DStateReady = false;
    Setup3DState();
}
#endif

void pspContext::EndFrame() {
    // Opaque submissions can be reordered by material until the frame barrier.
    // Transparent/effect paths remain immediate and therefore force this
    // queue to drain before they alter GU state.
    FlushQueuedOpaqueDraws();
    // The display owns the final sceGuFinish/swap sequence.  Keeping the list
    // open here lets the existing overlay callback draw into the same frame.
}

void pspContext::FlushQueuedOpaqueDraws() {
#if defined(PSP_OPAQUE_RENDER_QUEUE) && defined(PSP_ENABLE_NATIVE_INDEXED) \
    && defined(PSP_INDEXED_LIST_MEMORY)
    if (replayingOpaqueQueue || opaqueQueueCount == 0) return;

    const u32 count = opaqueQueueCount;
    PspPerf::CountRenderer(PspPerf::RendererCounter::OpaqueQueueFlushes);
    std::sort(opaqueQueue.begin(), opaqueQueue.begin() + count,
        [](const QueuedOpaqueDraw& lhs, const QueuedOpaqueDraw& rhs) {
            if (lhs.vramHandle != rhs.vramHandle) return lhs.vramHandle < rhs.vramHandle;
            if (lhs.materialKey != rhs.materialKey) return lhs.materialKey < rhs.materialKey;
            if (lhs.texInfoOverride != rhs.texInfoOverride) {
                return lhs.texInfoOverride < rhs.texInfoOverride;
            }
            return lhs.texInfoWord < rhs.texInfoWord;
        });

    const Mat4 savedProjection = projection;
    const Mat4 savedView = view;
    const Mat4 savedWorld = world;
    const u32 savedVramHandle = vramHandle;
    const bool savedTexInfoOverride = texInfoOverride;
    const u32 savedTexInfoWord = texInfoWord;
    replayingOpaqueQueue = true;
    for (u32 i = 0; i < count; ++i) {
        const QueuedOpaqueDraw& command = opaqueQueue[i];
        projection = command.projection;
        view = command.view;
        world = command.world;
        vramHandle = command.vramHandle;
        texInfoOverride = command.texInfoOverride;
        texInfoWord = command.texInfoWord;
        DrawPrimBuffer(command.buffer, command.indexOffset, command.indexCount);
    }
    replayingOpaqueQueue = false;
    opaqueQueueCount = 0;

    projection = savedProjection;
    view = savedView;
    world = savedWorld;
    vramHandle = savedVramHandle;
    texInfoOverride = savedTexInfoOverride;
    texInfoWord = savedTexInfoWord;
    // The last replayed command may belong to a different page than the
    // caller's current state. Force the next immediate draw to bind its page.
    pageBindingValid = false;
    pageTexScaleValid = false;
#else
    opaqueQueueCount = 0;
#endif
}

bool pspContext::QueueOpaqueDraw(pspPrimBuffer* buffer, u32 indexOffset,
                                 u32 indexCount) {
#if defined(PSP_OPAQUE_RENDER_QUEUE) && defined(PSP_ENABLE_NATIVE_INDEXED) \
    && defined(PSP_INDEXED_LIST_MEMORY)
    if (replayingOpaqueQueue || !listOpen || !buffer
        || buffer->GetPrimType() != PDDI_PRIM_TRIANGLES
        || blendMode != PDDI_BLEND_NONE || !zBufferEnabled
        || polygonOffsetEnabled || realTextureMode || vramHandle == 0
        || !buffer->HasPackedVertices() || !buffer->HasValidIndices()) {
        return false;
    }

    const u32 total = buffer->GetIndexCount();
    const u32 start = std::min(indexOffset, total);
    const u32 end = indexCount
        ? start + std::min(indexCount, total - start) : total;
    if (end <= start || end - start > kDrawChunk) return false;

    u32 materialKey = 0;
    if (texInfoOverride) materialKey = texInfoWord;
    else if (!buffer->GetSingleMaterialKey(start, end, &materialKey)) return false;

    if (opaqueQueueCount == kOpaqueQueueCapacity) FlushQueuedOpaqueDraws();
    if (opaqueQueueCount == kOpaqueQueueCapacity) return false;

    QueuedOpaqueDraw& command = opaqueQueue[opaqueQueueCount++];
    command.buffer = buffer;
    command.indexOffset = start;
    command.indexCount = end - start;
    command.materialKey = materialKey;
    command.vramHandle = vramHandle;
    command.texInfoOverride = texInfoOverride;
    command.texInfoWord = texInfoWord;
    command.projection = projection;
    command.view = view;
    command.world = world;
    PspPerf::CountRenderer(PspPerf::RendererCounter::OpaqueQueuedDraws);
    return true;
#else
    (void)buffer;
    (void)indexOffset;
    (void)indexCount;
    return false;
#endif
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
#if defined(PSP_PERF_TELEMETRY)
    const u32 syncStart = sceKernelGetSystemTimeLow();
#endif
    sceGuSync(0, 0);
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::CountRenderer(PspPerf::RendererCounter::GeSyncUs,
        sceKernelGetSystemTimeLow() - syncStart);
#endif
    PspPerf::Mark(PspPerf::RuntimeStage::SyncDone);
    listOpen = false;
    // TEX_SYNC only orders GE transfers; only this CPU/GE fence retires RAM
    // sources, texture slots, CLUT snapshots and the command list itself.
    for (auto& cache : pageCache) cache.inFlight = false;
    frameVertexUsed = frameIndexUsed = framePaletteUsed = 0;
    ++g_submissionSerial;
    if (g_submissionSerial == 0) g_submissionSerial = 1;
}

void pspContext::FlushList() {
    if (!listOpen) return;
    if (!replayingOpaqueQueue) FlushQueuedOpaqueDraws();
    PspPerf::CountRenderer(PspPerf::RendererCounter::ListFlushes);
    FinishList();
    ResetDisplayListGuard();
    // GE state and GUM matrices survive a list boundary. Do not clear, swap,
    // reset material state, or wait for VBlank in the middle of a frame.
    sceGuStart(GU_DIRECT, g_displayListStorage.words);
#if defined(PSP_TRIPLE_BUFFER)
    BindDrawFramebuffer();
#endif
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
    // The memory is in the list's uncached alias (sceGuStart ORs 0x40000000
    // into the list address): CPU writes there are in RAM by the time the GE
    // reads them, so it needs no data-cache writeback (a syscall per draw).
    // Callers reserve their entire draw (including all allocations) first.
    // Never restart here: earlier allocations may not have been submitted.
    if (!listOpen || bytes + 32u + 64u + static_cast<std::size_t>(sceGuCheckList())
        > sizeof(g_displayListStorage.words)) return nullptr;
    const auto raw = reinterpret_cast<std::uintptr_t>(sceGuGetMemory(static_cast<int>(bytes + 15u)));
    g_drawsSinceStallUpdate = 0;  // sceGuGetMemory published every draw before it
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
    if (kDitherCapability) GuEnable(GU_DITHER);
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
    FlushQueuedOpaqueDraws();
    EnsureListSpace();
    gu3DStateReady = false;
    pageBindingValid = false;
    // Consecutive overlay batches: skip the ~15 state calls and GUM ortho
    // rebuild while the cached GU state proves nothing changed them (a 3D
    // setter, BindPage or matrix load since the last full setup).
    if (kCacheGuState && gu2DStateReady && gu2DMatrixSerial == g_gumMatrixSerial
        && (g_guCapabilityValid & k2DDisabledCapabilities) == k2DDisabledCapabilities
        && (g_guCapabilityEnabled & k2DDisabledCapabilities) == 0u
        && g_guDepthMaskGeneration == g_guStateGeneration
        && g_guDepthMaskValue == GU_TRUE) {
        return;
    }
    ++g_matrixGeneration; // These direct GUM writes replace all 3D matrices.
    GuDisable(GU_DEPTH_TEST);
    GuDisable(GU_CULL_FACE);
    GuDisable(GU_ALPHA_TEST);
    GuDisable(GU_COLOR_TEST);
    GuDisable(GU_STENCIL_TEST);
    GuDisable(GU_COLOR_LOGIC_OP);
    GuDisable(GU_LIGHTING);
    GuDisable(GU_CLIP_PLANES);
    if (kDitherCapability) GuDisable(GU_DITHER);
    GuDepthMask(GU_TRUE);
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumOrtho(0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, -1.0f, 1.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    gu2DStateReady = true;
    gu2DMatrixSerial = g_gumMatrixSerial;
}

void pspContext::Clear(int flags) {
    FlushQueuedOpaqueDraws();
    EnsureListSpace();
    const u32 color = MakeColor(clearColour);
    sceGuClearColor(color);
    sceGuClearDepth(0);
    int clearFlags = 0;
    if (flags & PDDI_BUFFER_COLOUR) clearFlags |= GU_COLOR_BUFFER_BIT;
    // The depth buffer holds the pause backdrop; menu frames are 2D only.
    if ((flags & PDDI_BUFFER_DEPTH) && !pauseBackdropValid) clearFlags |= GU_DEPTH_BUFFER_BIT;
    if (clearFlags) sceGuClear(clearFlags);
}

void pspContext::ClearDepthNow() {
    pendingDepthClear = false;
    if (!listOpen) {
        pendingDepthClear = true;
        return;
    }
    FlushQueuedOpaqueDraws();
    EnsureListSpace();
    // Clear the complete surface even if a menu narrowed the scissor.
    sceGuScissor(0, 0, kWidth, kHeight);
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    sceGuScissor(scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]);
}

// Full-screen 1:1 copy into the current draw target in 64 px sprite strips
// (texture-cache friendly), with blending/filtering off. Through-mode UVs are
// texels, so TexScale/TexOffset do not apply.
void pspContext::DrawScreenCopy(u32 sourceFormat, const void* sourceTexture) {
    struct CopyVertex { u16 u, v; s16 x, y, z; };
    constexpr int kStripWidth = 64;
    constexpr int kStrips = (kWidth + kStripWidth - 1) / kStripWidth;
    auto* strips = static_cast<CopyVertex*>(
        AllocateListMemory(sizeof(CopyVertex) * kStrips * 2u));
    if (!strips) return;
    for (int strip = 0; strip < kStrips; ++strip) {
        const int left = strip * kStripWidth;
        const int right = std::min(left + kStripWidth, kWidth);
        strips[strip * 2] = {static_cast<u16>(left), 0,
                             static_cast<s16>(left), 0, 0};
        strips[strip * 2 + 1] = {static_cast<u16>(right), static_cast<u16>(kHeight),
                                 static_cast<s16>(right), static_cast<s16>(kHeight), 0};
    }
    sceGuScissor(0, 0, kWidth, kHeight);
    GuDisable(GU_BLEND);
    GuDisable(GU_DITHER);
    GuEnable(GU_TEXTURE_2D);
    GuTexMode(static_cast<int>(sourceFormat), 0, 0, 0);
    sceGuTexImage(0, kBufferWidth, kBufferWidth, kBufferWidth, sourceTexture);
    GuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    GuTexFilter(GU_NEAREST, GU_NEAREST);
    GuTexWrap(GU_CLAMP, GU_CLAMP);
    // The source was rendered by the GE; order it before the texture fetch
    // and drop any stale texture-cache lines.
    sceGuTexSync();
    sceGuTexFlush();
    GuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   kStrips * 2, nullptr, strips);
    PspPerf::RecordDrawSubmission(static_cast<u32>(kStrips * 2));
    sceGuScissor(scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]);
    // Texture/blend registers changed outside the page/3D caches.
    SetBlend(blendMode);
    pageBindingValid = false;
    gu3DStateReady = false;
}

bool pspContext::CapturePauseBackdrop() {
    if (!listOpen || !g_staticVram.initialized) return false;
    EnsureListSpace(256u);
    Setup2DState(); // depth test off and depth writes masked: depth is the target
    const auto edramBase = reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr());
    const std::uintptr_t drawOffset = DrawFramebufferOffset();
    const auto depthOffset = reinterpret_cast<std::uintptr_t>(g_staticVram.depthBuffer);
    // 512x272 RGB565 is exactly the 16-bit depth surface (278 KiB).
    sceGuDrawBufferList(GU_PSM_5650, reinterpret_cast<void*>(depthOffset), kBufferWidth);
    DrawScreenCopy(kFramebufferPsm, reinterpret_cast<const void*>(edramBase + drawOffset));
    sceGuDrawBufferList(kFramebufferPsm, reinterpret_cast<void*>(drawOffset), kBufferWidth);
    pauseBackdropValid = true;
    pauseBackdropUsedThisFrame = true;
    pendingDepthClear = false;
    return true;
}

bool pspContext::DrawPauseBackdrop() {
    if (!pauseBackdropValid || !listOpen) return false;
    EnsureListSpace(256u);
    Setup2DState();
    const auto edramBase = reinterpret_cast<std::uintptr_t>(sceGeEdramGetAddr());
    const auto depthOffset = reinterpret_cast<std::uintptr_t>(g_staticVram.depthBuffer);
    DrawScreenCopy(GU_PSM_5650, reinterpret_cast<const void*>(edramBase + depthOffset));
    pauseBackdropUsedThisFrame = true;
    return true;
}

void pspContext::ReleasePauseBackdrop() {
    if (!pauseBackdropValid) return;
    pauseBackdropValid = false;
    pauseBackdropUsedThisFrame = false;
    // Restore a valid depth surface before any 3D draw of this frame.
    ClearDepthNow();
}

bool PspCapturePauseBackdrop() {
    return g_activeContext && g_activeContext->CapturePauseBackdrop();
}

bool PspDrawPauseBackdrop() {
    return g_activeContext && g_activeContext->DrawPauseBackdrop();
}

void PspReleasePauseBackdrop() {
    if (g_activeContext) g_activeContext->ReleasePauseBackdrop();
}

bool PspHasPauseBackdrop() {
    return g_activeContext && g_activeContext->HasPauseBackdrop();
}

void pspContext::SetCullMode(pddiCullMode mode) {
    // World/block rendering sets the same cull mode for every primitive
    // buffer.  Once the 3D state is known to be current, avoid even the
    // list-space probe and the cached GU calls.  A 2D pass clears this key,
    // so the next 3D setter still re-emits the state when required.
    if (gu3DStateReady && cullMode == mode) return;
    FlushQueuedOpaqueDraws();
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
    FlushQueuedOpaqueDraws();
    EnsureListSpace();
    zBufferEnabled = enable;
    if (enable) GuEnable(GU_DEPTH_TEST);
    else GuDisable(GU_DEPTH_TEST);
    GuDepthMask(enable && blendMode == PDDI_BLEND_NONE ? GU_FALSE : GU_TRUE);
}

void pspContext::SetBlendMode(pddiBlendMode mode) {
    if (gu3DStateReady && blendMode == mode) return;
    FlushQueuedOpaqueDraws();
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

    FlushQueuedOpaqueDraws();
    EnsureListSpace();
    polygonOffsetEnabled = enable;
    polygonOffset = requestedOffset;
    GuDepthOffset(polygonOffsetEnabled ? polygonOffset : 0);
}

void pspContext::SetScissor(int x, int y, int w, int h) {
    FlushQueuedOpaqueDraws();
    EnsureListSpace();
    const int right = std::clamp(x + w, 0, kWidth);
    const int bottom = std::clamp(y + h, 0, kHeight);
    scissorRect = {{std::clamp(x, 0, kWidth), std::clamp(y, 0, kHeight), right, bottom}};
    sceGuScissor(scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]);
}

void pspContext::ApplyShader(const pspShader* shader) {
    shaderTexture = shader ? shader->GetTexture(0) : nullptr;
    if (shader) SetBlendMode(PDDI_BLEND_ALPHA);
}

void pspContext::SetPageTextureScale(bool packedUv) {
    // In transform mode the GE normalizes 16-bit texture coordinates by
    // 32768 (0x8000 == 1.0). The PSX page path historically supplies float
    // texels and scales them by 1/256; use the equivalent fixed-coordinate
    // scale only while the compact UV stream is active. It must be exactly
    // 32768/256: 32767/256 maps every integer texel u to u * 32767/32768,
    // just below u, so a triangle whose corners share one texel (PSX colour
    // swatches, e.g. the player's arms) samples the texel above/left. Include
    // pageBindingValid because a 2D/real-texture pass may have changed GU
    // state without changing this cache.
    //
    // Half-texel offset: the PSX truncates an integral UV to that texel, but
    // the GE's perspective-corrected nearest lookup lands a constant integral
    // UV a hair below the texel corner on some pixels, so a swatch triangle
    // (all three corners on one texel, e.g. the player's sleeves at (112,32))
    // speckles with the texel above/left (hair). Sampling the texel centre
    // keeps the same nearest texel for every integral UV. Offset is in
    // normalized units (applied after TexScale); through-mode 2D ignores it,
    // and pspTexture::Bind resets it to zero for real textures. GuTexOffset is
    // state-cached, so issuing it ahead of the scale cache test is cheap.
    GuTexOffset(0.5f / 256.0f, 0.5f / 256.0f);
    if (pageTexScaleValid && pageBindingValid && pageTexScalePacked == packedUv) {
        return;
    }
    const float scale = packedUv ? (32768.0f / 256.0f) : (1.0f / 256.0f);
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
        GuDrawArray(GU_TRIANGLES,
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
    pddiTexture* texture = psp ? psp->GetTexture() : shaderTexture;
    // Built locally: list memory is uncached, and Convert2DUvToTexels would
    // read the UVs back from it.
    PspVertex quad[6] = {
        {u0, v1, color, x, y + h, 0.0f},
        {u1, v1, color, x + w, y + h, 0.0f},
        {u1, v0, color, x + w, y, 0.0f},
        {u0, v1, color, x, y + h, 0.0f},
        {u1, v0, color, x + w, y, 0.0f},
        {u0, v0, color, x, y, 0.0f},
    };
    Convert2DUvToTexels(quad, 6, texture);
    std::copy(quad, quad + 6, vertices);
    Draw2D(vertices, 6, texture);
}

void pspContext::DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                               const pddiBatchVertex* source, s32 vertCount) {
    if (!listOpen || !source || vertCount <= 0) return;
    PspPerf::CountRenderer(PspPerf::RendererCounter::OverlayQuads,
                           static_cast<u32>(vertCount) / 6u);
    PspPerf::ScopeTimer overlayTimer(PspPerf::RendererCounter::OverlayUs);
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
    // Each vertex is converted in registers and only written to list memory,
    // which is uncached: building it there and reading it back for the UV
    // conversion and the sprite corners cost a RAM access per field.
    // Same values as Convert2DUvToTexels.
    auto* nativeTexture = tex ? static_cast<pspTexture*>(tex) : nullptr;
    const float texelWidth = nativeTexture ? static_cast<float>(nativeTexture->GetWidth()) : 1.0f;
    const float texelHeight = nativeTexture ? static_cast<float>(nativeTexture->GetHeight()) : 1.0f;
    const auto convert = [&](const pddiBatchVertex& in) {
        PspVertex out = { in.u, in.v, MakeColor(pddiColour(in.r, in.g, in.b, in.a)),
                          in.x, in.y, 0.0f };
        if (nativeTexture) {
            out.u *= texelWidth;
            out.v *= texelHeight;
        }
        return out;
    };
#if !defined(NDEBUG)
    static unsigned int overlayBatchLogCount = 0;
    if (overlayBatchLogCount < 12) {
        ++overlayBatchLogCount;
        const PspVertex vertices[1] = {convert(source[0])};
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
    if (nativeTexture && nativeTexture->GetAlphaDepth() >= 8 && (vertCount % 6) == 0) {
        const s32 spriteVertexCount = vertCount / 3;
        PspSpriteVertex* sprites = static_cast<PspSpriteVertex*>(
            AllocateListMemory(sizeof(PspSpriteVertex) * spriteVertexCount));
        if (!sprites) return;
        for (s32 i = 0, out = 0; i < vertCount; i += 6, out += 2) {
            const PspVertex topLeft = convert(source[i + 5]);
            const PspVertex bottomRight = convert(source[i + 1]);
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
        GuDrawArray(GU_SPRITES,
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
    PspVertex* vertices = static_cast<PspVertex*>(AllocateListMemory(sizeof(PspVertex) * vertCount));
    if (!vertices) return;
    for (s32 i = 0; i < vertCount; ++i) {
        vertices[i] = convert(source[i]);
    }
    Draw2D(vertices, vertCount, tex);
}

void pspContext::Draw2D(const PspVertex* vertices, int count, pddiTexture* texture) {
    if (texture) ConfigureTexture(static_cast<pspTexture*>(texture), false);
    else GuDisable(GU_TEXTURE_2D);

    // 2D vertices already contain final screen-space coordinates.  Submit
    // them through the direct GU path used by the PSPSDK samples so no GUM
    // matrix state from the previous 3D pass can leak into the overlay.
    GuDrawArray(GU_TRIANGLES,
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
    PspPerf::CountRenderer(PspPerf::RendererCounter::OverlayQuads);
    PspPerf::ScopeTimer overlayTimer(PspPerf::RendererCounter::OverlayUs);
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
    // Create/update/destroy of the PSX VRAM image: no cached palette survives.
    if (handle == 1u) ++vramEpoch;
    for (auto& cache : pageCache) {
        if (cache.handle == handle) {
            cache.handle = 0;
            ++cache.fill;
        }
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
        // Any palette read from this row must be converted again.
        if (handle == 1u && static_cast<u32>(row) < kVramRows) {
            vramRowGeneration[static_cast<u32>(row)] = ++vramWriteCounter;
        }
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
            ++cache.fill;
        }
        // Indexed texels do not depend on the CLUT. BindPage snapshots the
        // current palette, so palette-only updates must not evict T4/T8 pages.
    }
}

// A free or least-recently-used palette slot that no draw of the open list
// references (the GE may still read those until the next fence).
u32 pspContext::AcquireClutCacheEntry() {
    u32 victim = kClutCacheNone;
    u32 victimTick = 0;
    for (u32 index = 0; index < kClutCacheEntries; ++index) {
        const ClutCacheEntry& entry = clutCache[index];
        if (!entry.valid) {
            if (entry.lastUseSerial != g_submissionSerial) return index;
            continue;
        }
        if (entry.lastUseSerial == g_submissionSerial) continue;
        if (victim == kClutCacheNone || entry.lastUseTick < victimTick) {
            victim = index;
            victimTick = entry.lastUseTick;
        }
    }
    return victim;
}

static u32 PageCacheHintIndex(u32 handle, u16 tpage, bool zeroKey) {
    const u32 key = (static_cast<u32>(tpage) | (handle << 9)) ^ (zeroKey ? 0x5a5a5a5au : 0u);
    return (key * 0x9E3779B1u) >> 27;  // 32 buckets
}

static u32 ClutCacheHintIndex(u16 cba, u32 depth, u8 flags) {
    const u32 key = static_cast<u32>(cba) | (depth << 16) | (static_cast<u32>(flags) << 18);
    return (key * 0x9E3779B1u) >> 26;  // 64 buckets
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
    const u32 hintIndex = PageCacheHintIndex(handle, tpage, zeroKey);
    if (const u8 hinted = pageCacheHint[hintIndex]) {
        PageCache& cache = pageCache[hinted - 1u];
        if (cache.handle == handle && cache.tpage == tpage && cache.zeroKey == zeroKey) {
            PspPerf::Mark(PspPerf::RuntimeStage::CacheHit, tpage, hinted - 1u);
            cache.age = cacheAge++;
            cache.inFlight = true;
            return &cache;
        }
    }
    for (auto& cache : pageCache) {
        if (cache.handle == handle && cache.tpage == tpage && cache.zeroKey == zeroKey) {
            pageCacheHint[hintIndex] = static_cast<u8>(&cache - pageCache.data() + 1);
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
    ++target->fill;
    pageCacheHint[hintIndex] = static_cast<u8>(target - pageCache.data() + 1);
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
    PspPerf::ScopeTimer bindTimer(PspPerf::RendererCounter::BindUs);
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
            const u32 clutRow = clutY % static_cast<u32>(image->height);
            const u16* paletteSource = image->pixels + clutRow * image->width;
            const u8 clutFlags = static_cast<u8>((wordZeroKey ? 1u : 0u)
                | (blendUsesZeroKey ? 2u : 0u));
#if defined(PSP_PALETTE_REFERENCE)
            constexpr bool clutCacheable = false;
#else
            // Palettes of the PSX VRAM image are reused while their row's
            // write generation holds (no gather, compare or conversion).
            const bool clutCacheable = vramHandle == 1u
                && static_cast<u32>(image->height) == kVramRows;
#endif
            u32 cachedIndex = kClutCacheNone;
            const u32 clutHintIndex = ClutCacheHintIndex(cba, depth, clutFlags);
            if (clutCacheable) {
                if (const u8 hinted = clutCacheHint[clutHintIndex]) {
                    const ClutCacheEntry& entry = clutCache[hinted - 1u];
                    if (entry.valid && entry.cba == cba && entry.depth == depth
                        && entry.flags == clutFlags && entry.epoch == vramEpoch
                        && entry.rowGeneration == vramRowGeneration[clutRow]) {
                        cachedIndex = hinted - 1u;
                    }
                }
            }
            if (clutCacheable && cachedIndex == kClutCacheNone) {
                for (u32 index = 0; index < kClutCacheEntries; ++index) {
                    ClutCacheEntry& entry = clutCache[index];
                    if (!entry.valid || entry.cba != cba || entry.depth != depth
                        || entry.flags != clutFlags) {
                        continue;
                    }
                    if (entry.epoch == vramEpoch
                        && entry.rowGeneration == vramRowGeneration[clutRow]) {
                        cachedIndex = index;
                        clutCacheHint[clutHintIndex] = static_cast<u8>(index + 1u);
                        break;
                    }
                    // Stale. Free it unless the open list still reads it; a
                    // fresh duplicate refilled after a mid-list write may follow.
                    if (entry.lastUseSerial != g_submissionSerial) entry.valid = false;
                }
            }
            if (cachedIndex != kClutCacheNone) {
                ClutCacheEntry& entry = clutCache[cachedIndex];
                entry.lastUseSerial = g_submissionSerial;
                entry.lastUseTick = ++clutCacheClock;
                if (geClutEntry != cachedIndex) {
                    GuClutMode(GU_PSM_5551, 0, depth == 0 ? 0x0fu : 0xffu, 0);
                    sceGuClutLoad(depth == 0 ? 1 : 16,
                        clutCachePalettes.data() + cachedIndex * 256u);
                    PspPerf::CountRenderer(PspPerf::RendererCounter::ClutLoads);
                    geClutEntry = cachedIndex;
                    // The raw-row tracker below no longer describes the GE.
                    g_clutGeContentValid = false;
                }
            }
            else {
                // Gather the raw row once. Unlike the CBA key above, its contents
                // decide whether the GE already holds the same converted palette.
                // Keep row wrapping for nonstandard widths and right-edge CLUTs.
                u32 sourceX = clutX % static_cast<u32>(image->width);
                for (u32 i = 0; i < paletteSize; ++i) {
                    g_clutRawScratch[i] = paletteSource[sourceX];
                    if (++sourceX == static_cast<u32>(image->width)) sourceX = 0;
                }
                const bool sameGeContent = g_clutGeContentValid
                    && g_clutGeDepth == depth
                    && g_clutGeWordZeroKey == wordZeroKey
                    && g_clutGeBlend == blendUsesZeroKey
                    && std::memcmp(g_clutRawScratch, g_clutGeRaw,
                                   static_cast<std::size_t>(paletteSize) * sizeof(u16)) == 0;
                if (!sameGeContent) {
                    const u32 fillIndex = clutCacheable ? AcquireClutCacheEntry() : kClutCacheNone;
                    u16* palette = nullptr;
                    if (fillIndex != kClutCacheNone) {
                        ClutCacheEntry& entry = clutCache[fillIndex];
                        entry.rowGeneration = vramRowGeneration[clutRow];
                        entry.epoch = vramEpoch;
                        entry.lastUseSerial = g_submissionSerial;
                        entry.lastUseTick = ++clutCacheClock;
                        entry.cba = cba;
                        entry.depth = static_cast<u8>(depth);
                        entry.flags = clutFlags;
                        entry.valid = true;
                        clutCacheHint[clutHintIndex] = static_cast<u8>(fillIndex + 1u);
                        palette = clutCachePalettes.data() + fillIndex * 256u;
                    }
                    else {
                        palette = framePalettes.data() + framePaletteUsed++ * 256u;
                    }
                    PspPerf::CountRenderer(PspPerf::RendererCounter::PaletteEntries, paletteSize);
                    for (u32 i = 0; i < paletteSize; ++i) {
#if defined(PSP_PALETTE_REFERENCE)
                        // Reference A/B: convert straight from the row, one divide per colour.
                        const u16 word = paletteSource[(clutX + i) % static_cast<u32>(image->width)];
#else
                        const u16 word = g_clutRawScratch[i];
#endif
                        palette[i] = ConvertPsxTexelToGu5551(word,
                            wordZeroKey ? word == 0 : (blendUsesZeroKey ? i == 0 : (word & 0x7fffu) == 0x7c1fu));
                    }
                    WritebackDcacheRange(palette,
                        static_cast<std::size_t>(paletteSize) * sizeof(u16));
                    GuClutMode(GU_PSM_5551, 0, depth == 0 ? 0x0fu : 0xffu, 0);
                    sceGuClutLoad(depth == 0 ? 1 : 16, palette); // 32-byte blocks, 16-bit entries
                    PspPerf::CountRenderer(PspPerf::RendererCounter::ClutLoads);
                    std::memcpy(g_clutGeRaw, g_clutRawScratch,
                                static_cast<std::size_t>(paletteSize) * sizeof(u16));
                    g_clutGeDepth = static_cast<u8>(depth);
                    g_clutGeWordZeroKey = wordZeroKey;
                    g_clutGeBlend = blendUsesZeroKey;
                    g_clutGeContentValid = true;
                    geClutEntry = fillIndex;
                }
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
            if (!page->gpuPixels) PspPerf::CountRenderer(PspPerf::RendererCounter::RamPageBinds);
        }
    }
    else {
        if (!samePage) {
            GuTexMode(GU_PSM_5551, 0, 0, 0);
            sceGuTexImage(0, kPageSize, kPageSize, kPageSize,
                          page->gpuPixels ? page->gpuPixels : page->pixels);
            if (!page->gpuPixels) PspPerf::CountRenderer(PspPerf::RendererCounter::RamPageBinds);
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

#if defined(PSP_ENABLE_NATIVE_INDEXED) && defined(PSP_INDEXED_LIST_MEMORY) \
    && defined(PSP_INDEXED_PERSISTENT_INDICES) && !defined(PSP_DISABLE_RECORDED_DRAWS)
constexpr bool kRecordedDraws = true;
#else
constexpr bool kRecordedDraws = false;
#endif

// GetDecodedPage's lookup without its side effects: the slot holding the
// page, or -1 (also on a miss, where GetDecodedPage would decode).
int pspContext::FindCachedPageSlot(u32 handle, u16 tpage) const {
    tpage &= 0x019fu;
    const bool zeroKey = ((tpage >> 7) & 3u) >= 2u && blendMode != PDDI_BLEND_NONE;
    for (u32 slot = 0; slot < pageCache.size(); ++slot) {
        const PageCache& cache = pageCache[slot];
        if (cache.handle == handle && cache.tpage == tpage && cache.zeroKey == zeroKey) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

// BindPage's CLUT cache lookup without side effects: the current entry
// for the key, or -1.
int pspContext::FindCurrentClutEntry(u16 cba, u32 depth, u8 flags, u32 row) const {
    for (u32 index = 0; index < kClutCacheEntries; ++index) {
        const ClutCacheEntry& entry = clutCache[index];
        if (entry.valid && entry.cba == cba && entry.depth == depth && entry.flags == flags
            && entry.epoch == vramEpoch && entry.rowGeneration == vramRowGeneration[row]) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

// A called list leaves texture, CLUT and alpha-test state the CPU-side
// caches did not see.
void pspContext::ForgetTextureState() {
    ++g_guStateGeneration;
    g_guCapabilityValid &= ~((1u << GU_TEXTURE_2D) | (1u << GU_ALPHA_TEST));
    g_clutGeContentValid = false;
    geClutEntry = kClutCacheNone;
    pageBindingValid = false;
    clutBindingValid = false;
    pageTexScaleValid = false;
}

bool pspContext::RecordedDrawCurrent(pspPrimBuffer* buffer, bool packedUv) const {
    const pspPrimBuffer::RecordedDraw& recorded = buffer->Recorded();
    if (!recorded.valid || recorded.contentSerial != buffer->GetContentSerial()
        || recorded.vramHandle != vramHandle
        || recorded.blendMode != static_cast<u32>(blendMode)
        || recorded.packedUv != packedUv
        || recorded.indexCount != buffer->GetIndexCount()) {
        return false;
    }
    for (const pspPrimBuffer::RecordedPage& page : recorded.pages) {
        if (pageCache[page.slot].fill != page.fill || pageCache[page.slot].handle != vramHandle) {
            return false;
        }
    }
    for (const pspPrimBuffer::RecordedClut& clut : recorded.cluts) {
        const ClutCacheEntry& entry = clutCache[clut.entry];
        if (!entry.valid || entry.epoch != clut.epoch || clut.epoch != vramEpoch
            || entry.rowGeneration != clut.rowGeneration
            || vramRowGeneration[clut.row] != clut.rowGeneration) {
            return false;
        }
    }
    return true;
}

// Records the whole draw of a static buffer as a GE call list: per material
// run, the state BindPage and SetPageTextureScale would set, then the indexed
// draw on the persistent arrays. Only when every page and CLUT is already
// cached, so no decode, upload or palette conversion can land in a list that
// is replayed later; otherwise the normal path draws (and fills the caches).
bool pspContext::RecordPrimBufferCallList(pspPrimBuffer* buffer, int indexedFormat,
                                          bool packedUv) {
    pspPrimBuffer::RecordedDraw& recorded = buffer->Recorded();
    // The previous list may still be read by the GE in this display list.
    if (recorded.submissionSerial == g_submissionSerial) return false;
    VramImage* image = GetVram(vramHandle);
    if (!image) return false;
    const bool clutCacheable = vramHandle == 1u && static_cast<u32>(image->height) == kVramRows;
    const u8 clutFlags = blendMode != PDDI_BLEND_NONE ? 2u : 0u;

    struct Run {
        u32 start;
        u32 count;
        int page;   // -1: untextured
        int clut;   // -1: none (16-bit page)
        u32 copy;   // offset in indexCopies, or kNoCopy (persistent indices)
    };
    constexpr u32 kNoCopy = 0xFFFFFFFFu;
    static std::vector<Run> runs;
    runs.clear();
    std::vector<pspPrimBuffer::RecordedPage> pages;
    std::vector<pspPrimBuffer::RecordedClut> cluts;
    const u16* indices = buffer->GetIndices();
    const u32 total = buffer->GetIndexCount();
    for (u32 start = 0; start < total;) {
        u32 runEnd = start + std::min(kDrawChunk, total - start);
        runEnd -= (runEnd - start) % 3u;
        if (runEnd <= start) break;
        Run run{start, 0, -1, -1, kNoCopy};
        if (start + 2 < total) {
            bool valid = false;
            u32 key = 0;
            u32 limit = 0;
            if (!buffer->FindMaterialRun(start, &valid, &key, &limit)) return false;
            if (limit < runEnd) runEnd = limit;
            if (valid) {
                const u16 tpage = static_cast<u16>(key >> 16);
                const u16 cba = static_cast<u16>(key & 0xffffu);
                run.page = FindCachedPageSlot(vramHandle, tpage);
                if (run.page < 0) return false;
                const PageCache& page = pageCache[static_cast<u32>(run.page)];
                pages.push_back({static_cast<u32>(run.page), page.fill});
                if (page.format == GU_PSM_T4 || page.format == GU_PSM_T8) {
                    if (!clutCacheable) return false;
                    const u32 depth = (tpage >> 7) & 3u;
                    const u32 row = ((cba >> 6) & 0x1ffu) % static_cast<u32>(image->height);
                    run.clut = FindCurrentClutEntry(cba, depth, clutFlags, row);
                    if (run.clut < 0) return false;
                    const ClutCacheEntry& entry = clutCache[static_cast<u32>(run.clut)];
                    cluts.push_back({static_cast<u32>(run.clut), entry.rowGeneration, entry.epoch,
                                     static_cast<u16>(row)});
                }
            }
        }
        run.count = runEnd - start;
        runs.push_back(run);
        start = runEnd;
    }
    if (runs.empty()) return false;
    u32 copiedIndices = 0;
    for (Run& run : runs) {
        if ((reinterpret_cast<std::uintptr_t>(indices + run.start) & 3u) == 0u) continue;
        run.copy = copiedIndices;
        copiedIndices += (run.count + 1u) & ~1u;  // keep the next copy word-aligned
    }
    if (copiedIndices != 0) {
        recorded.indexCopies.resize(copiedIndices);
        for (const Run& run : runs) {
            if (run.copy == kNoCopy) continue;
            std::copy(indices + run.start, indices + run.start + run.count,
                      recorded.indexCopies.data() + run.copy);
        }
        WritebackDcacheRange(recorded.indexCopies.data(), copiedIndices * sizeof(u16));
    }

    // Per run at most: CLUT mode/load 4, texture mode/image 7, enables 2,
    // draw 6 words; the shared texture state 16 more, and the return.
    const std::size_t capacity = runs.size() * 24u + 32u;
    if (recorded.words.size() < capacity) {
        recorded.words.assign(capacity, 0u);
        // The GE list is written through the uncached alias: no stale dirty
        // line from the zero fill may be written back over it later.
        sceKernelDcacheWritebackInvalidateRange(recorded.words.data(),
            static_cast<unsigned int>(recorded.words.size() * sizeof(u32)));
    }
    EnsureListSpace(64u);

    const PspPackedVertex* vertices = buffer->GetPackedVertices();
    sceGuStart(GU_CALL, recorded.words.data());
    int textured = -1;  // unknown, off, on
    bool sharedState = false;
    const void* boundPixels = nullptr;
    int boundFormat = -1;
    const void* boundClut = nullptr;
    u32 boundClutDepth = 0;
    for (const Run& run : runs) {
        if (run.page >= 0) {
            const PageCache& page = pageCache[static_cast<u32>(run.page)];
            if (!sharedState) {
                sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
                sceGuAmbientColor(0xffffffff);
                sceGuTexFilter(GU_NEAREST, GU_NEAREST);
                sceGuTexWrap(GU_REPEAT, GU_REPEAT);
                sceGuTexOffset(0.5f / 256.0f, 0.5f / 256.0f);
                const float scale = packedUv ? (32768.0f / 256.0f) : (1.0f / 256.0f);
                sceGuTexScale(scale, scale);
                sceGuAlphaFunc(GU_GREATER, 0, 0xff);
                sharedState = true;
            }
            if (textured != 1) {
                sceGuEnable(GU_TEXTURE_2D);
                sceGuEnable(GU_ALPHA_TEST);
                textured = 1;
            }
            if (run.clut >= 0) {
                const u16* palette = clutCachePalettes.data() + static_cast<u32>(run.clut) * 256u;
                const u32 depth = clutCache[static_cast<u32>(run.clut)].depth;
                if (palette != boundClut || depth != boundClutDepth) {
                    sceGuClutMode(GU_PSM_5551, 0, depth == 0 ? 0x0fu : 0xffu, 0);
                    sceGuClutLoad(depth == 0 ? 1 : 16, palette);
                    boundClut = palette;
                    boundClutDepth = depth;
                }
            }
            const void* pixels = page.gpuPixels ? page.gpuPixels : page.pixels;
            if (pixels != boundPixels || static_cast<int>(page.format) != boundFormat) {
                sceGuTexMode(static_cast<int>(page.format), 0, 0, 0);
                sceGuTexImage(0, kPageSize, kPageSize, kPageSize, pixels);
                boundPixels = pixels;
                boundFormat = static_cast<int>(page.format);
            }
        }
        else if (textured != 0) {
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDisable(GU_ALPHA_TEST);
            textured = 0;
        }
        const u16* runIndices = run.copy == kNoCopy
            ? indices + run.start : recorded.indexCopies.data() + run.copy;
        sceGuDrawArray(GU_TRIANGLES, indexedFormat, static_cast<int>(run.count),
                       runIndices, vertices);
    }
    const int bytes = sceGuFinish();
    if (bytes < 0 || static_cast<std::size_t>(bytes) > recorded.words.size() * sizeof(u32)) {
        // Unreachable with the bound above; never call an overrun list.
        recorded.valid = false;
        return false;
    }
    PspPerf::CountRenderer(PspPerf::RendererCounter::RecordedDrawBuilds);
    recorded.pages = std::move(pages);
    recorded.cluts = std::move(cluts);
    recorded.contentSerial = buffer->GetContentSerial();
    recorded.vramHandle = vramHandle;
    recorded.blendMode = static_cast<u32>(blendMode);
    recorded.packedUv = packedUv;
    recorded.indexCount = total;
    recorded.runs = static_cast<u32>(runs.size());
    recorded.valid = true;
    return true;
}

// Draws a static buffer through its recorded call list (recording it first
// when its content was unchanged since the previous draw). False: draw it
// the normal way.
bool pspContext::DrawRecordedPrimBuffer(pspPrimBuffer* buffer, int indexedFormat,
                                        bool packedUv) {
    if (!buffer->DrawnUnchanged()) return false;
    if (!RecordedDrawCurrent(buffer, packedUv)
        && !RecordPrimBufferCallList(buffer, indexedFormat, packedUv)) {
        return false;
    }
    pspPrimBuffer::RecordedDraw& recorded = buffer->Recorded();
    // Reserve before pinning: a fence here would release the pins.
    EnsureListSpace(64u);
    // Pin what the list reads until the GE consumed this display list.
    for (const pspPrimBuffer::RecordedPage& page : recorded.pages) {
        PageCache& cache = pageCache[page.slot];
        cache.age = cacheAge++;
        cache.inFlight = true;
    }
    for (const pspPrimBuffer::RecordedClut& clut : recorded.cluts) {
        ClutCacheEntry& entry = clutCache[clut.entry];
        entry.lastUseSerial = g_submissionSerial;
        entry.lastUseTick = ++clutCacheClock;
    }
    sceGuCallList(recorded.words.data());
    recorded.submissionSerial = g_submissionSerial;
    buffer->MarkDirectSubmission();
    ForgetTextureState();
    PspPerf::CountRenderer(PspPerf::RendererCounter::RecordedDrawCalls);
    PspPerf::RecordDrawSubmission(recorded.indexCount);
    return true;
}

void pspContext::DrawPrimBuffer(pddiPrimBuffer* base, u32 indexOffset, u32 indexCount) {
    PspPerf::ScopeTimer submitTimer(PspPerf::RendererCounter::SubmitUs);
    PspPerf::Mark(PspPerf::RuntimeStage::PrimBegin, indexOffset, indexCount);
    // This device only manufactures pspPrimBuffer instances, and every
    // buffer reaching this context comes from that same pspDevice. Avoiding
    // RTTI here removes a type walk from every submitted draw call on the
    // 333 MHz CPU while preserving the backend's ownership invariant.
    auto* buffer = static_cast<pspPrimBuffer*>(base);
    if (!listOpen || !buffer || buffer->GetIndexCount() == 0) return;
    // Lit colours stay in a prop buffer after its lit draw; every other draw
    // of the buffer gets the base colours back.
    if (buffer->HasPatchedColours() && base != g_litColourDrawBuffer) {
        buffer->PatchColours(nullptr, buffer->GetVertexCount());
    }
    // 3D geometry needs the depth buffer back: drop a pause backdrop stored
    // there (clears depth). The pause path itself only draws 2D over it.
    if (pauseBackdropValid) ReleasePauseBackdrop();

    if (!replayingOpaqueQueue) {
        if (QueueOpaqueDraw(buffer, indexOffset, indexCount)) return;
        FlushQueuedOpaqueDraws();
    }

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

#if defined(PSP_ENABLE_NATIVE_INDEXED) && defined(PSP_INDEXED_LIST_MEMORY)
    // Restrict the compact stream to page-backed PSX geometry. Its UVs are
    // integer texels; real-texture/glTF buffers commonly use fractional
    // normalized coordinates and retain the float format.
    const bool usePackedVertices = vramHandle != 0 && buffer->HasPackedVertices();
#else
    // The frame-arena variants do not have a packed arena yet.
    const bool usePackedVertices = false;
#endif
#if defined(NDEBUG)
    if (!usePackedVertices) buffer->RefreshFloatVertices();
#else
    buffer->RefreshFloatVertices(); // the colour probe below reads it
#endif
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
#if defined(PSP_ENABLE_NATIVE_INDEXED)
    // Static page geometry (world blocks, unlit props): call the buffer's
    // recorded GE list instead of rebinding and redrawing every run.
    if (kRecordedDraws && start == 0 && end == total && isTriangles && vramHandle != 0
        && !texInfoOverride && !realTextureMode && usePackedVertices
        && directIndexedVertices && !replayingOpaqueQueue
        && DrawRecordedPrimBuffer(buffer, nativeIndexedFormat, usePackedVertices)) {
        return;
    }
#endif
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
            bool firstMaterialValid = texInfoOverride || buffer->HasMaterialKey(firstIndex);
            u32 firstMaterialKey = texInfoOverride
                ? texInfoWord : buffer->GetMaterialKey(firstIndex);
            // The cached run gives the scans below their result directly.
            bool runKnown = false;
            if (!texInfoOverride) {
                bool runValid = false;
                u32 runKey = 0;
                u32 runLimit = 0;
                if (buffer->FindMaterialRun(start, &runValid, &runKey, &runLimit)) {
                    firstMaterialValid = runValid;
                    firstMaterialKey = runKey;
                    if (runLimit < runEnd) runEnd = runLimit;
                    runKnown = true;
                }
            }
#if defined(PSP_GU_DIAGNOSTIC)
            if (probe) PSP_GU_DIAG("[PSP GU DIAG] prim #%lu material scan firstIndex=%u firstTpage=%f firstCba=%f",
                static_cast<unsigned long>(primId), static_cast<unsigned>(firstIndex),
                static_cast<double>(firstMaterialKey >> 16),
                static_cast<double>(firstMaterialKey & 0xffffu));
#endif
            if (firstMaterialValid) {
                materialKey = firstMaterialKey;
                if (!texInfoOverride && !runKnown) {
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
                        static_cast<unsigned>(static_cast<u16>(firstMaterialKey >> 16)),
                        static_cast<unsigned>(static_cast<u16>(firstMaterialKey & 0xffffu)));
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
                // An untextured triangle (tpage < 0) starts this run. End the
                // run at the next textured triangle; otherwise every textured
                // face after it in the chunk is drawn untextured (hub gates:
                // untextured edge strips precede the textured door faces).
                for (u32 cursor = start + 3; !runKnown && cursor + 2 < runEnd; cursor += 3) {
                    if (buffer->HasMaterialKey(indices[cursor])) {
                        runEnd = cursor;
                        break;
                    }
                }
                // BindPage skips an unchanged page/CLUT, which would leave
                // texturing disabled for the next run on the previous page.
                pageBindingValid = false;
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
        GuDrawArray(primitive,
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
                GuDrawArray(primitive,
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
#if !defined(PSP_INDEXED_LIST_MEMORY)
            WritebackDcacheRange(guIndices, sizeof(u16) * runCount);
#endif
            PspPerf::CountRenderer(PspPerf::RendererCounter::StagingBytes,
                sizeof(u16) * runCount);
            PspPerf::CountRenderer(PspPerf::RendererCounter::StagingCopies);
            PspPerf::Mark(PspPerf::RuntimeStage::DrawBegin, start, runCount);
            GuDrawArray(primitive,
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
                PspPerf::ScopeTimer stageTimer(PspPerf::RendererCounter::StageUs);
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
                        PspPerf::CountRenderer(PspPerf::RendererCounter::StagingBytes,
                            sizeof(u16) * runCount);
                        PspPerf::CountRenderer(PspPerf::RendererCounter::StagingCopies);
                        PspPerf::Mark(PspPerf::RuntimeStage::DrawBegin, start, runCount);
                        GuDrawArray(primitive,
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
#if !defined(PSP_INDEXED_LIST_MEMORY)
        WritebackDcacheRange(
            guVertices,
            vertexStride * localVertexCount);
        WritebackDcacheRange(
            guIndices,
            sizeof(u16) * runCount);
#endif
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
        GuDrawArray(primitive,
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

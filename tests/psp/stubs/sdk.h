#pragma once
#include "constants.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>

inline uint64_t sceKernelGetSystemTimeWide() {
    static uint64_t timeUs = 1000000;
    return ++timeUs;
}
inline int sceKernelGetThreadId() { return 1; }
inline unsigned int sceKernelGetSystemTimeLow() {
    return static_cast<unsigned int>(sceKernelGetSystemTimeWide());
}

// A delayed-consumption GE model: every RAM source must still contain its
// submitted bytes at sync. It also checks real list/transfer/count limits.
namespace FakeGe {
inline void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "GE regression: %s\n", message); std::abort(); }
}
inline uint64_t hash(const void* ptr, size_t size) {
    auto* bytes = static_cast<const unsigned char*>(ptr);
    uint64_t value = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 1099511628211ULL;
    return value;
}
struct Read { const void* ptr; size_t bytes; uint64_t value; };
inline std::vector<Read> reads;
inline unsigned char* list = nullptr;
inline size_t used = 0, peak = 0;
inline bool open = false, textureEnabled = false, alphaEnabled = false;
inline int syncs = 0, swaps = 0, clears = 0, draws = 0, cluts = 0, copies = 0;
// Palette memory of the most recent sceGuClutLoad (what the GE CLUT holds).
inline const void* lastClut = nullptr;
inline uint64_t primitives = 0;
inline int textureFormat = GU_PSM_5551, matrixMode = 0;
inline int textureFunction = GU_TFX_REPLACE;
inline int blendEquation = GU_ADD;
inline int depthMask = GU_FALSE;
inline std::array<float, 16> projection{};
inline int depthNear = 0, depthFar = 0;
inline const void* texture = nullptr;
inline int textureWidth = 0, textureHeight = 0;
inline std::array<unsigned char, 2 * 1024 * 1024> edram{};
inline std::array<int, 3> copyFormats{};
inline std::vector<uint64_t> drawSignatures;
inline const void* lastVertices = nullptr;
inline size_t lastStride = 0;
inline bool lastDrawTextured = false;
inline void emit(size_t bytes = 4) {
    require(open, "commands outside an open list");
    used += bytes;
    peak = std::max(used, peak);
    // Keep the fake GE aligned with the production PSP renderer's resident
    // 1.5 MiB command list.  The previous 1 MiB assertion hid the intended
    // list-headroom optimization behind a harness-only abort.
    require(used <= 1536 * 1024, "display list overflow");
}
inline void read(const void* ptr, size_t bytes) {
    require(ptr && bytes, "empty GPU source");
    reads.push_back({ptr, bytes, hash(ptr, bytes)});
}
inline bool inEdram(const void* ptr) {
    auto value = reinterpret_cast<uintptr_t>(ptr);
    return value >= 0x04000000 && value < 0x04200000;
}
// Colour/depth surfaces below the texture slots: two RGBA8888 buffers plus
// depth, or three RGB565 buffers plus depth.
#if defined(PSP_TRIPLE_BUFFER)
constexpr size_t kSurfaceBytes = 512 * 272 * 2 * 4;
#else
constexpr size_t kSurfaceBytes = 512 * 272 * 10;
#endif
}

// GE call lists: commands written while a GU_CALL list is open are kept and
// run, with all of the model's checks, each time the list is called.
namespace FakeGe {
inline std::map<const void*, std::vector<std::function<void()>>> callLists;
inline std::vector<std::function<void()>>* recording = nullptr;
inline int callListCalls = 0;
template <class F> bool Defer(F&& run) {
    if (!recording) return false;
    recording->emplace_back(std::forward<F>(run));
    return true;
}
}

// Display model (relative eDRAM offsets): the buffer scanned out, the one
// queued for the next VBlank, and the PSPSDK's own draw/display pair, which
// sceGuSwapBuffers exchanges and sceGuStart re-emits as the GE target.
namespace FakeDisplay {
constexpr uintptr_t kNone = ~uintptr_t(0);
inline bool live = false;
inline uintptr_t shown = kNone, queued = kNone;
inline int vcount = 0;
inline void* sdkDraw = nullptr;
inline void* sdkDisplay = nullptr;
inline void Vblank() {
    if (queued != kNone) { shown = queued; queued = kNone; }
    ++vcount;
}
inline void Queue(uintptr_t offset) {
    // Replacing a queued buffer drops that frame and, with three buffers,
    // leaves on screen the one the renderer draws next.
    FakeGe::require(queued == kNone, "presented over a frame still queued");
    queued = offset; ++FakeGe::swaps;
}
}
struct ScePspFMatrix4 { float m[16]; };
struct SceCtrlData { unsigned int Buttons; unsigned char Lx, Ly; };
inline void* memalign(size_t alignment, size_t bytes) {
    void* ptr = nullptr;
    return posix_memalign(&ptr, alignment, bytes) ? nullptr : ptr;
}
inline int sceGuInit() { return 0; }
inline void sceGuTerm() { FakeGe::require(!FakeGe::open, "terminating live list"); }
inline void* sceGeEdramGetAddr() { return reinterpret_cast<void*>(0x04000000); }
inline int sceGeEdramGetSize() { return 2 * 1024 * 1024; }
namespace FakeGe {
inline int clearFlags = 0;
inline void* drawBuffer = nullptr;
inline int drawBufferFormat = GU_PSM_8888;
// The GE must never write the buffer on screen or queued for it.
inline void requireOffScreenTarget() {
    if (!FakeDisplay::live) return;
    const auto target = reinterpret_cast<uintptr_t>(drawBuffer);
    require(target != FakeDisplay::shown && target != FakeDisplay::queued,
            "GE draws into a displayed buffer");
}
}
inline int sceGuStart(int context, void* ptr) {
    if (context == GU_CALL) {
        FakeGe::require(FakeGe::open && !FakeGe::recording, "call list outside a display list");
        FakeGe::require((reinterpret_cast<uintptr_t>(ptr) & 15) == 0, "unaligned call list");
        FakeGe::recording = &FakeGe::callLists[ptr];
        FakeGe::recording->clear();
        return 0;
    }
    FakeGe::require(!FakeGe::open && FakeGe::reads.empty(), "restarted before sync");
    FakeGe::list = static_cast<unsigned char*>(ptr);
    FakeGe::used = 0; FakeGe::open = true; FakeGe::emit(8);
    if (FakeDisplay::live) { FakeGe::emit(8); FakeGe::drawBuffer = FakeDisplay::sdkDraw; }
    return 0;
}
inline int sceGuCheckList() { return static_cast<int>(FakeGe::used); }
inline void* sceGuGetMemory(int size) {
    void* result = FakeGe::list + FakeGe::used + 8;
    FakeGe::emit(((size + 3) & ~3) + 8);
    return result;
}
inline int sceGuFinish() {
    if (FakeGe::recording) {
        const int bytes = static_cast<int>(FakeGe::recording->size() * 16 + 4);
        FakeGe::recording = nullptr;
        return bytes;
    }
    FakeGe::emit(8); FakeGe::open = false;
    return static_cast<int>(FakeGe::used);
}
inline void sceGuCallList(const void* list) {
    FakeGe::require(!FakeGe::recording, "nested call list");
    FakeGe::emit(16);
    const auto found = FakeGe::callLists.find(list);
    FakeGe::require(found != FakeGe::callLists.end(), "called an unrecorded list");
    ++FakeGe::callListCalls;
    for (const auto& command : found->second) command();
}
inline void sceGuSync(int, int) {
    FakeGe::require(!FakeGe::open, "sync on unfinished list");
    for (const auto& r : FakeGe::reads) {
        FakeGe::require(FakeGe::hash(r.ptr, r.bytes) == r.value, "in-flight RAM overwritten");
    }
    FakeGe::reads.clear(); ++FakeGe::syncs;
}
// Raw command words carry 32-bit GE addresses; the renderer only writes them
// on the PSP itself and calls sceGuDrawArray on a 64-bit host.
inline void sceGuSendCommandi(int, int) {
    FakeGe::require(false, "raw GE command words on a 64-bit host");
}
inline void sceGuSwapBuffers() {
    std::swap(FakeDisplay::sdkDraw, FakeDisplay::sdkDisplay);
    FakeDisplay::Queue(reinterpret_cast<uintptr_t>(FakeDisplay::sdkDisplay));
}
inline int sceDisplaySetFrameBuf(void* top, int width, int format, int sync) {
    const auto offset = reinterpret_cast<uintptr_t>(top) - 0x04000000;
    FakeGe::require(FakeDisplay::live && width == 512 && sync == PSP_DISPLAY_SETBUF_NEXTVSYNC
        && format == FakeGe::drawBufferFormat && offset % (512 * 272 * 2) == 0
        && offset < FakeGe::kSurfaceBytes, "bad display buffer");
    FakeDisplay::Queue(offset);
    return 0;
}
inline int sceDisplayGetVcount() { return FakeDisplay::vcount; }
inline int sceDisplayWaitVblankStart() { FakeDisplay::Vblank(); return 0; }
inline void sceGuDrawBuffer(int format, void* buffer, int) {
    FakeGe::emit(20); FakeGe::drawBufferFormat = format; FakeGe::drawBuffer = buffer;
    FakeDisplay::sdkDraw = buffer;
}
inline void sceGuDispBuffer(int, int, void* buffer, int) { FakeDisplay::sdkDisplay = buffer; }
inline int sceGuDisplay(int on) {
    FakeDisplay::live = on != 0;
    FakeDisplay::shown = reinterpret_cast<uintptr_t>(FakeDisplay::sdkDisplay);
    FakeDisplay::queued = FakeDisplay::kNone;
    return 0;
}
inline void sceGuClear(int flags) {
    FakeGe::emit(32); ++FakeGe::clears; FakeGe::clearFlags = flags;
    FakeGe::requireOffScreenTarget();
}
inline void sceGuDrawBufferList(int format, void* buffer, int) {
    FakeGe::emit(12); FakeGe::drawBufferFormat = format; FakeGe::drawBuffer = buffer;
}
inline void sceGuEnable(int state) {
    if (FakeGe::Defer([=] { sceGuEnable(state); })) return;
    FakeGe::emit();
    if (state == GU_TEXTURE_2D) FakeGe::textureEnabled = true;
    if (state == GU_ALPHA_TEST) FakeGe::alphaEnabled = true;
}
inline void sceGuDisable(int state) {
    if (FakeGe::Defer([=] { sceGuDisable(state); })) return;
    FakeGe::emit();
    if (state == GU_TEXTURE_2D) FakeGe::textureEnabled = false;
    if (state == GU_ALPHA_TEST) FakeGe::alphaEnabled = false;
}
inline void sceGuDepthRange(int nearValue, int farValue) {
    FakeGe::emit(16); FakeGe::depthNear = nearValue; FakeGe::depthFar = farValue;
}
inline void sceGuDepthMask(int mask) { FakeGe::emit(); FakeGe::depthMask = mask; }
inline void sceGuTexMode(int format, int a, int b, int c) {
    if (FakeGe::Defer([=] { sceGuTexMode(format, a, b, c); })) return;
    FakeGe::emit(8); FakeGe::textureFormat = format;
}
inline void sceGuTexFunc(int function, int a) {
    if (FakeGe::Defer([=] { sceGuTexFunc(function, a); })) return;
    FakeGe::emit(16); FakeGe::textureFunction = function;
}
namespace FakeGe {
inline int blendSource = 0, blendDestination = 0;
inline unsigned int blendSourceFix = 0, blendDestinationFix = 0;
}
inline void sceGuBlendFunc(int equation, int source, int destination, unsigned int sourceFix,
                           unsigned int destinationFix) {
    if (FakeGe::Defer([=] { sceGuBlendFunc(equation, source, destination, sourceFix, destinationFix); })) return;
    FakeGe::emit(16); FakeGe::blendEquation = equation;
    FakeGe::blendSource = source; FakeGe::blendDestination = destination;
    FakeGe::blendSourceFix = sourceFix; FakeGe::blendDestinationFix = destinationFix;
}
inline void sceGuTexImage(int, int width, int height, int stride, const void* ptr) {
    if (FakeGe::Defer([=] { sceGuTexImage(0, width, height, stride, ptr); })) return;
    FakeGe::require((reinterpret_cast<uintptr_t>(ptr) & 15) == 0, "unaligned texture");
    FakeGe::emit(16); FakeGe::texture = ptr;
    FakeGe::textureWidth = stride; FakeGe::textureHeight = height;
    FakeGe::require(width <= stride, "invalid texture stride");
}
inline void sceGuClutLoad(int blocks, const void* ptr) {
    if (FakeGe::Defer([=] { sceGuClutLoad(blocks, ptr); })) return;
    FakeGe::require(blocks == 1 || blocks == 16, "wrong 5551 CLUT byte count");
    FakeGe::require((reinterpret_cast<uintptr_t>(ptr) & 15) == 0, "unaligned CLUT");
    FakeGe::emit(12); FakeGe::read(ptr, blocks * 32); ++FakeGe::cluts;
    FakeGe::lastClut = ptr;
}
inline void sceGuCopyImage(int format, int, int, int width, int height, int srcw,
                           void* src, int, int, int destw, void* dest) {
    FakeGe::require(format == GU_PSM_5551, "indexed format in GE transfer");
    FakeGe::require(width == srcw && srcw == destw, "bad packed transfer stride");
    FakeGe::require(width == 64 || width == 128 || width == 256, "bad packed transfer width");
    FakeGe::require(FakeGe::inEdram(dest), "destination outside eDRAM (double alias)");
    const auto offset = reinterpret_cast<uintptr_t>(dest) - 0x04000000;
    const size_t bytes = width * height * 2;
    FakeGe::require(offset >= FakeGe::kSurfaceBytes && offset + bytes <= FakeGe::edram.size(), "slot overlaps surfaces/end");
    FakeGe::emit(32); FakeGe::read(src, bytes); ++FakeGe::copies;
    ++FakeGe::copyFormats[width == 64 ? 0 : (width == 128 ? 1 : 2)];
}
inline void sceGuDrawArray(int primitive, int format, int count, const void* indices, const void* vertices) {
    if (FakeGe::Defer([=] { sceGuDrawArray(primitive, format, count, indices, vertices); })) return;
    FakeGe::lastVertices = vertices;
    FakeGe::lastDrawTextured = FakeGe::textureEnabled;
    FakeGe::require(count > 0 && count <= 65535, "GE draw count overflow");
    FakeGe::require((reinterpret_cast<uintptr_t>(vertices) & 15) == 0, "unaligned vertices");
    FakeGe::emit(24); ++FakeGe::draws;
    FakeGe::requireOffScreenTarget();
    const bool indexed = (format & GU_INDEX_16BIT) != 0;
    const int textureType = format & GU_TEXTURE_BITS;
    const int colourType = format & GU_COLOR_BITS;
    const int vertexType = format & GU_VERTEX_BITS;
    const size_t textureBytes = textureType == GU_TEXTURE_8BIT ? 2
        : (textureType == GU_TEXTURE_16BIT ? 4 : (textureType == GU_TEXTURE_32BITF ? 8 : 0));
    const size_t colourBytes = colourType == GU_COLOR_5650 || colourType == GU_COLOR_5551
        || colourType == GU_COLOR_4444 ? 2 : (colourType == GU_COLOR_8888 ? 4 : 0);
    const size_t vertexBytes = vertexType == GU_VERTEX_8BIT ? 3
        : (vertexType == GU_VERTEX_16BIT ? 6 : (vertexType == GU_VERTEX_32BITF ? 12 : 0));
    const size_t stride = textureBytes + colourBytes + vertexBytes;
    FakeGe::require(stride != 0, "unknown vertex stride");
    FakeGe::lastStride = stride;
    unsigned int vertexCount = count;
    const auto* ids = static_cast<const uint16_t*>(indices);
    if (indexed) {
        // Match the production word-alignment guard for persistent ranges;
        // the 16-byte texture/CLUT DMA rule is not an index-stream rule.
        FakeGe::require(ids && (reinterpret_cast<uintptr_t>(ids) & 3) == 0, "unaligned indices");
        FakeGe::read(ids, count * sizeof(uint16_t));
        vertexCount = *std::max_element(ids, ids + count) + 1u;
    }
    FakeGe::read(vertices, vertexCount * stride);
    if (primitive == GU_TRIANGLES || primitive == GU_TRIANGLE_STRIP || primitive == GU_LINE_STRIP) {
        // Hash logical vertices to compare expanded/indexed semantics and
        // preserve strip continuity across submission boundaries.
        for (int i = 0; i < count; ++i) {
            const auto* v = static_cast<const unsigned char*>(vertices) + (indexed ? ids[i] : i) * stride;
            FakeGe::drawSignatures.push_back(FakeGe::hash(v, stride));
        }
    }
    FakeGe::primitives += primitive == GU_TRIANGLES ? count / 3
        : (primitive == GU_TRIANGLE_STRIP ? count - 2 : (primitive == GU_LINE_STRIP ? count - 1 : count));
    if (FakeGe::textureEnabled && !FakeGe::inEdram(FakeGe::texture)) {
        const size_t texels = FakeGe::textureWidth * FakeGe::textureHeight;
        FakeGe::read(FakeGe::texture, FakeGe::textureFormat == GU_PSM_T4 ? texels / 2
            : (FakeGe::textureFormat == GU_PSM_T8 ? texels : texels * 2));
    }
}
inline void sceGumDrawArray(int p, int f, int c, const void* i, const void* v) { sceGuDrawArray(p, f, c, i, v); }
inline void sceGumMatrixMode(int mode) { FakeGe::matrixMode = mode; }
inline void sceGumLoadMatrix(const ScePspFMatrix4* matrix) {
    if (FakeGe::matrixMode == GU_PROJECTION) std::copy(matrix->m, matrix->m + 16, FakeGe::projection.begin());
}
inline void sceGumUpdateMatrix() { FakeGe::emit(192); }
inline int sceCtrlReadBufferPositive(SceCtrlData* data, int) { *data = {0, 128, 128}; return 1; }
inline int sceKernelMaxFreeMemSize() { return 16 * 1024 * 1024; }
inline int sceKernelTotalFreeMemSize() { return 16 * 1024 * 1024; }
#define GE_COMMAND(name) template<class... T> inline int name(T... args) { \
    if (FakeGe::Defer([=] { name(args...); })) return 0; \
    FakeGe::emit(16); return 0; }
GE_COMMAND(sceGuAlphaFunc) GE_COMMAND(sceGuAmbientColor)
GE_COMMAND(sceGuClearColor) GE_COMMAND(sceGuClearDepth) GE_COMMAND(sceGuClutMode)
GE_COMMAND(sceGuDepthBuffer) GE_COMMAND(sceGuDepthFunc)
GE_COMMAND(sceGuDepthOffset)
GE_COMMAND(sceGuFrontFace) GE_COMMAND(sceGuOffset) GE_COMMAND(sceGuScissor)
GE_COMMAND(sceGuShadeModel) GE_COMMAND(sceGuTexEnvColor) GE_COMMAND(sceGuTexFilter)
GE_COMMAND(sceGuTexOffset) GE_COMMAND(sceGuTexScale)
GE_COMMAND(sceGuTexSync) GE_COMMAND(sceGuTexWrap) GE_COMMAND(sceGuViewport)
GE_COMMAND(sceGuTexFlush)
#undef GE_COMMAND
#define SDK_NOOP(name) template<class... T> inline int name(T...) { return 0; }
SDK_NOOP(guSwapBuffersBehaviour)
SDK_NOOP(sceCtrlSetSamplingCycle) SDK_NOOP(sceCtrlSetSamplingMode)
SDK_NOOP(sceGumLoadIdentity) SDK_NOOP(sceGumOrtho)
SDK_NOOP(sceKernelDcacheWritebackAll) SDK_NOOP(sceKernelDcacheWritebackRange)
SDK_NOOP(sceKernelDcacheWritebackInvalidateRange)
#undef SDK_NOOP

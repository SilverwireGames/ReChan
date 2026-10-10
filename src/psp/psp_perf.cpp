#include "psp/psp_perf.h"

#include "p3d/fileio.h"

#if defined(PSP_RUNTIME_DIAGNOSTIC)
namespace PspPerf {
volatile RuntimeMarker g_runtimeMarkers[256] = {};
volatile u32 g_runtimeMarkerSequence = 0;
void Mark(RuntimeStage stage, u32 value, u32 detail) {
    const u32 sequence = g_runtimeMarkerSequence + 1u;
    volatile auto& marker = g_runtimeMarkers[sequence & 255u];
    marker.stage = static_cast<u32>(stage);
    marker.value = value;
    marker.detail = detail;
    marker.sequence = sequence;
    g_runtimeMarkerSequence = sequence;
}
} // namespace PspPerf
#endif

#if defined(PSP_PERF_TELEMETRY)

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <string>
#include <pspkernel.h>

#if defined(PSP_TRACE_TELEMETRY)
#include <pspiofilemgr.h>
#endif

namespace {

constexpr std::size_t kWindowSize = 600;
// PSP display cadence is approximately 59.94 Hz.
constexpr u32 kFrameBudgetUs = 16684;

struct MetricRing {
    std::array<u32, kWindowSize> values = {};
    std::size_t count = 0;
    std::size_t next = 0;
    unsigned long long sum = 0;

    void Reset() {
        values.fill(0);
        count = 0;
        next = 0;
        sum = 0;
    }

    void Push(u32 value) {
        if (count == kWindowSize) {
            sum -= values[next];
        }
        else {
            ++count;
        }
        values[next] = value;
        sum += value;
        next = (next + 1u) % kWindowSize;
    }

    u32 Average() const {
        return count == 0 ? 0u : static_cast<u32>(sum / count);
    }
};

MetricRing g_frame;
MetricRing g_logic;
MetricRing g_draw;
MetricRing g_swap;
MetricRing g_drawCalls;
MetricRing g_submittedVertices;
MetricRing g_pageCacheMisses;
std::array<u64, static_cast<u32>(PspPerf::RendererCounter::Count)> g_rendererTotals{};
u32 g_totalFrames = 0;
u64 g_runStartUs = 0;
u32 g_currentDrawCalls = 0;
u32 g_currentSubmittedVertices = 0;
u32 g_currentPageCacheMisses = 0;
#if defined(PSP_TRACE_TELEMETRY)
SceUID g_traceFd = -1;
u32 g_tracePlayFrame = 0;
bool g_traceFrameActive = false;
#endif
u32 g_rendererMemoryBytes = 0;
u32 g_audioMemoryBytes = 0;
std::atomic_uint g_audioCallbackCount{0};
std::atomic_uint g_audioUnderruns{0};
std::atomic_uint g_audioMaxUs{0};
std::atomic_uint g_audioCallbackTotalUs{0};
std::atomic_uint g_musicDecodeCount{0};
std::atomic_uint g_musicDecodeTotalUs{0};
std::atomic_uint g_audioMusicStarts{0};
std::atomic_uint g_audioMusicMixCallbacks{0};
std::atomic_uint g_audioMusicMixedFrames{0};
std::atomic_uint g_audioWorkerErrors{0};

struct MemoryMetrics {
    u32 samples = 0;
    u32 firstHeapFree = 0;
    u32 lastHeapFree = 0;
    u32 minHeapFree = std::numeric_limits<u32>::max();
    u32 maxHeapFree = 0;
    u32 lastHeapMax = 0;
    u32 minHeapMax = std::numeric_limits<u32>::max();
    u32 maxHeapMax = 0;
    u32 lastVramUsed = 0;
    u32 minVramUsed = std::numeric_limits<u32>::max();
    u32 maxVramUsed = 0;
    u32 lastVramFree = 0;
    u32 minVramFree = std::numeric_limits<u32>::max();
    u32 maxVramFree = 0;
    u32 libcArena = 0;
    u32 libcUsed = 0;
    u32 libcFree = 0;
    u32 libcFreeChunks = 0;
    u32 libcUsedMin = std::numeric_limits<u32>::max();
    u32 libcUsedMax = 0;
};

MemoryMetrics g_memory;

struct HandlerMetric {
    const void* callback = nullptr;
    u64 totalUs = 0;
    u32 calls = 0;
};
constexpr u32 kMaxHandlerMetrics = 24;
std::array<HandlerMetric, kMaxHandlerMetrics> g_handlers{};
u32 g_handlerCount = 0;

void ResetHandlerMetrics() {
    g_handlers.fill({});
    g_handlerCount = 0;
}

int g_mainThreadId = -1;  // SceUID of the thread that called Reset()
u32 g_scenarioNote = 0;
std::array<u32, 16> g_microbench{};
u32 g_fusedSkinChecks = 0;
u32 g_fusedSkinMismatches = 0;

// The slowest frames since the measurement started, with what they spent the
// time on, plus slowdown counts over the whole session (not just the window).
constexpr u32 kIoPathChars = 40;
struct WorstFrame {
    u32 frameUs = 0;
    u32 logicUs = 0;
    u32 drawUs = 0;
    u32 vblankUs = 0;
    u32 ioUs = 0;
    u32 ioOps = 0;
    u32 ioBytes = 0;
    u32 entitiesUs = 0;
    u32 ticks = 0;
    u32 frameIndex = 0;
    u32 slowIoUs = 0;
    char slowIoPath[kIoPathChars] = {};
};

// Session totals per file (main thread), to name what stalls the frame.
struct IoFileMetric {
    char path[kIoPathChars] = {};
    u64 totalUs = 0;
    u32 ops = 0;
    u32 maxUs = 0;
};
constexpr u32 kIoFiles = 16;
std::array<IoFileMetric, kIoFiles> g_ioFiles{};
char g_lastIoPath[kIoPathChars] = {};
u32 g_frameSlowIoUs = 0;
char g_frameSlowIoPath[kIoPathChars] = {};

// Keep the tail of a path (the file name matters most).
void CopyIoPath(char* out, const char* path) {
    const std::size_t length = std::strlen(path);
    const char* tail = length >= kIoPathChars ? path + (length - (kIoPathChars - 1)) : path;
    std::strncpy(out, tail, kIoPathChars - 1);
    out[kIoPathChars - 1] = '\0';
}

void ChargeIoFile(const char* path, u32 elapsed) {
    IoFileMetric* slot = nullptr;
    for (IoFileMetric& file : g_ioFiles) {
        if (file.path[0] && std::strncmp(file.path, path, kIoPathChars) == 0) {
            slot = &file;
            break;
        }
    }
    if (!slot) {
        // A free slot, else replace the cheapest entry if this op costs more.
        IoFileMetric* cheapest = &g_ioFiles[0];
        for (IoFileMetric& file : g_ioFiles) {
            if (!file.path[0]) { cheapest = &file; break; }
            if (file.totalUs < cheapest->totalUs) cheapest = &file;
        }
        if (cheapest->path[0] && cheapest->totalUs >= elapsed) return;
        *cheapest = {};
        std::strncpy(cheapest->path, path, kIoPathChars - 1);
        slot = cheapest;
    }
    slot->totalUs += elapsed;
    ++slot->ops;
    if (elapsed > slot->maxUs) slot->maxUs = elapsed;
}
constexpr u32 kWorstFrames = 8;
std::array<WorstFrame, kWorstFrames> g_worstFrames{};
u32 g_sessionFrames = 0;
u32 g_framesOver33Ms = 0;
u32 g_framesOver50Ms = 0;
u64 g_sessionIoUs = 0;

void ResetSessionMetrics() {
    g_worstFrames.fill({});
    g_ioFiles.fill({});
    g_frameSlowIoUs = 0;
    g_frameSlowIoPath[0] = '\0';
    g_sessionFrames = 0;
    g_framesOver33Ms = 0;
    g_framesOver50Ms = 0;
    g_sessionIoUs = 0;
}

#if defined(PSP_TRACE_TELEMETRY)
static bool IsTraceInteresting(const char* message) {
    return std::strstr(message, "play step:") != nullptr
        || std::strstr(message, "render:") != nullptr
        || std::strstr(message, "play:") != nullptr
        || std::strstr(message, "state ") != nullptr
        || std::strstr(message, "block draw:") != nullptr
        || std::strstr(message, "geometry clip:") != nullptr
        || std::strstr(message, "geometry fog:") != nullptr
        || std::strstr(message, "world render:") != nullptr
        || std::strstr(message, "gu draw:") != nullptr
        || std::strstr(message, "gu memory:") != nullptr;
}
#endif

static u32 MillisecondsToMicroseconds(f32 milliseconds) {
    if (milliseconds <= 0.0f) {
        return 0;
    }
    const f32 micros = milliseconds * 1000.0f;
    return micros >= 4294967295.0f ? 0xFFFFFFFFu : static_cast<u32>(micros + 0.5f);
}

static u32 Percentile(const MetricRing& metric, int percentile) {
    if (metric.count == 0) {
        return 0;
    }

    std::array<u32, kWindowSize> sorted = metric.values;
    std::sort(sorted.begin(), sorted.begin() + metric.count);
    const std::size_t rank = (metric.count * static_cast<std::size_t>(percentile) + 99u) / 100u;
    const std::size_t index = rank == 0 ? 0 : std::min(rank - 1u, metric.count - 1u);
    return sorted[index];
}

static u32 CountOverBudget() {
    u32 count = 0;
    for (std::size_t i = 0; i < g_frame.count; ++i) {
        if (g_frame.values[i] > kFrameBudgetUs) {
            ++count;
        }
    }
    return count;
}

static double FramesPerSecond(u32 averageUs) {
    return averageUs == 0 ? 0.0 : 1000000.0 / static_cast<double>(averageUs);
}

} // namespace

namespace PspPerf {

void WriteReport();
u32 g_rendererCounters[static_cast<u32>(RendererCounter::Count)] = {};

ScopeTimer::ScopeTimer(RendererCounter counter)
    : counter(counter), start(sceKernelGetSystemTimeLow()) {}

void ScopeTimer::Stop() {
    if (!active) return;
    CountRenderer(counter, sceKernelGetSystemTimeLow() - start);
    active = false;
}

u32 HandlerClock() {
    return sceKernelGetSystemTimeLow();
}

u32 IoClock() {
    return sceKernelGetSystemTimeLow();
}

void RecordIo(u32 startClock, u32 bytes, const char* path) {
    const u32 elapsed = sceKernelGetSystemTimeLow() - startClock;
    if (sceKernelGetThreadId() != g_mainThreadId) return;
    CountRenderer(RendererCounter::IoUs, elapsed);
    CountRenderer(RendererCounter::IoOps);
    CountRenderer(RendererCounter::IoBytes, bytes);
    if (path && path[0]) CopyIoPath(g_lastIoPath, path);
    ChargeIoFile(g_lastIoPath, elapsed);
    if (elapsed > g_frameSlowIoUs) {
        g_frameSlowIoUs = elapsed;
        std::memcpy(g_frameSlowIoPath, g_lastIoPath, kIoPathChars);
    }
}

void RecordHandler(const void* callback, u32 startClock) {
    const u32 elapsed = sceKernelGetSystemTimeLow() - startClock;
    for (u32 i = 0; i < g_handlerCount; ++i) {
        if (g_handlers[i].callback == callback) {
            g_handlers[i].totalUs += elapsed;
            ++g_handlers[i].calls;
            return;
        }
    }
    if (g_handlerCount < kMaxHandlerMetrics) {
        g_handlers[g_handlerCount++] = { callback, elapsed, 1u };
    }
}

void Reset() {
    g_runStartUs = sceKernelGetSystemTimeWide();
    g_mainThreadId = sceKernelGetThreadId();
    ResetHandlerMetrics();
    ResetSessionMetrics();
    g_rendererTotals.fill(0);
    std::fill_n(g_rendererCounters, static_cast<u32>(RendererCounter::Count), 0u);
    g_frame.Reset();
    g_logic.Reset();
    g_draw.Reset();
    g_swap.Reset();
    g_drawCalls.Reset();
    g_submittedVertices.Reset();
    g_pageCacheMisses.Reset();
    g_totalFrames = 0;
    g_currentDrawCalls = 0;
    g_currentSubmittedVertices = 0;
    g_currentPageCacheMisses = 0;
#if defined(PSP_TRACE_TELEMETRY)
    g_tracePlayFrame = 0;
    g_traceFrameActive = false;
#endif
    g_rendererMemoryBytes = 0;
    g_audioMemoryBytes = 0;
    g_audioCallbackCount.store(0, std::memory_order_relaxed);
    g_audioUnderruns.store(0, std::memory_order_relaxed);
    g_audioMaxUs.store(0, std::memory_order_relaxed);
    g_audioCallbackTotalUs.store(0, std::memory_order_relaxed);
    g_musicDecodeCount.store(0, std::memory_order_relaxed);
    g_musicDecodeTotalUs.store(0, std::memory_order_relaxed);
    g_audioMusicStarts.store(0, std::memory_order_relaxed);
    g_audioMusicMixCallbacks.store(0, std::memory_order_relaxed);
    g_audioMusicMixedFrames.store(0, std::memory_order_relaxed);
    g_audioWorkerErrors.store(0, std::memory_order_relaxed);
    g_memory = {};
    g_memory.minHeapFree = std::numeric_limits<u32>::max();
    g_memory.minHeapMax = std::numeric_limits<u32>::max();
    g_memory.minVramUsed = std::numeric_limits<u32>::max();
    g_memory.minVramFree = std::numeric_limits<u32>::max();
}

void BeginMeasurementWindow() {
    ResetHandlerMetrics();
    ResetSessionMetrics();
    g_rendererTotals.fill(0);
    std::fill_n(g_rendererCounters, static_cast<u32>(RendererCounter::Count), 0u);
    g_frame.Reset();
    g_logic.Reset();
    g_draw.Reset();
    g_swap.Reset();
    g_drawCalls.Reset();
    g_submittedVertices.Reset();
    g_pageCacheMisses.Reset();
    g_totalFrames = 0;
    g_currentDrawCalls = 0;
    g_currentSubmittedVertices = 0;
    g_currentPageCacheMisses = 0;
}

void BeginFrame() {
    // Renderer counters only: the play-logic breakdown (PlayLogicTicks and
    // after) is accumulated by gsPlayState before the frame's render begins,
    // so it is cleared by RecordFrame instead.
    std::fill_n(g_rendererCounters, static_cast<u32>(RendererCounter::PlayLogicTicks), 0u);
    g_currentDrawCalls = 0;
    g_currentSubmittedVertices = 0;
    g_currentPageCacheMisses = 0;
}

void RecordDrawSubmission(u32 vertexCount) {
    if (g_currentDrawCalls != 0xFFFFFFFFu) {
        ++g_currentDrawCalls;
    }
    if (0xFFFFFFFFu - g_currentSubmittedVertices < vertexCount) {
        g_currentSubmittedVertices = 0xFFFFFFFFu;
    }
    else {
        g_currentSubmittedVertices += vertexCount;
    }
}

void RecordPageCacheMiss() {
    if (g_currentPageCacheMisses != 0xFFFFFFFFu) {
        ++g_currentPageCacheMisses;
    }
}

void RecordMemorySnapshot(u32 heapFree, u32 heapMax, u32 vramUsed, u32 vramFree) {
    const struct mallinfo allocator = mallinfo();
    g_memory.libcArena = static_cast<u32>(allocator.arena);
    g_memory.libcUsed = static_cast<u32>(allocator.uordblks);
    g_memory.libcFree = static_cast<u32>(allocator.fordblks);
    g_memory.libcFreeChunks = static_cast<u32>(allocator.ordblks);
    g_memory.libcUsedMin = std::min(g_memory.libcUsedMin, g_memory.libcUsed);
    g_memory.libcUsedMax = std::max(g_memory.libcUsedMax, g_memory.libcUsed);
    if (g_memory.samples == 0) {
        g_memory.firstHeapFree = heapFree;
    }
    g_memory.lastHeapFree = heapFree;
    g_memory.minHeapFree = std::min(g_memory.minHeapFree, heapFree);
    g_memory.maxHeapFree = std::max(g_memory.maxHeapFree, heapFree);
    g_memory.lastHeapMax = heapMax;
    g_memory.minHeapMax = std::min(g_memory.minHeapMax, heapMax);
    g_memory.maxHeapMax = std::max(g_memory.maxHeapMax, heapMax);
    g_memory.lastVramUsed = vramUsed;
    g_memory.minVramUsed = std::min(g_memory.minVramUsed, vramUsed);
    g_memory.maxVramUsed = std::max(g_memory.maxVramUsed, vramUsed);
    g_memory.lastVramFree = vramFree;
    g_memory.minVramFree = std::min(g_memory.minVramFree, vramFree);
    g_memory.maxVramFree = std::max(g_memory.maxVramFree, vramFree);
    ++g_memory.samples;
}

void AddRendererMemory(s32 bytes) {
    if (bytes >= 0) {
        g_rendererMemoryBytes += static_cast<u32>(bytes);
    }
    else {
        const u32 amount = static_cast<u32>(-bytes);
        g_rendererMemoryBytes = amount > g_rendererMemoryBytes
            ? 0u : g_rendererMemoryBytes - amount;
    }
}

void SetAudioMemory(u32 bytes) { g_audioMemoryBytes = bytes; }

void RecordMusicDecode(u32 durationUs) {
    g_musicDecodeCount.fetch_add(1, std::memory_order_relaxed);
    g_musicDecodeTotalUs.fetch_add(durationUs, std::memory_order_relaxed);
}

void RecordAudioCallback(u32 durationUs) {
    g_audioCallbackCount.fetch_add(1, std::memory_order_relaxed);
    g_audioCallbackTotalUs.fetch_add(durationUs, std::memory_order_relaxed);
    unsigned int measuredUs = static_cast<unsigned int>(
        std::min<u32>(durationUs, 0xFFFFFFFFu));
    unsigned int previous = g_audioMaxUs.load(std::memory_order_relaxed);
    while (measuredUs > previous
        && !g_audioMaxUs.compare_exchange_weak(
            previous, measuredUs, std::memory_order_relaxed)) {
    }
}

void RecordAudioUnderrun() {
    g_audioUnderruns.fetch_add(1, std::memory_order_relaxed);
}

void RecordAudioMusicStart() {
    g_audioMusicStarts.fetch_add(1, std::memory_order_relaxed);
}

void RecordAudioMusicMix(u32 mixedFrames) {
    if (mixedFrames == 0) return;
    g_audioMusicMixCallbacks.fetch_add(1, std::memory_order_relaxed);
    g_audioMusicMixedFrames.fetch_add(mixedFrames, std::memory_order_relaxed);
}

void RecordAudioWorkerError() {
    g_audioWorkerErrors.fetch_add(1, std::memory_order_relaxed);
}

void RecordFrame(u32 frameUs, f32 logicMs, f32 drawMs, f32 swapMs) {
    for (u32 i = 0; i < static_cast<u32>(RendererCounter::Count); ++i) {
        g_rendererTotals[i] += g_rendererCounters[i];
    }
    {
        ++g_sessionFrames;
        if (frameUs > 33400u) ++g_framesOver33Ms;
        if (frameUs > 50000u) ++g_framesOver50Ms;
        const auto counter = [](RendererCounter c) {
            return g_rendererCounters[static_cast<u32>(c)];
        };
        g_sessionIoUs += counter(RendererCounter::IoUs);
        WorstFrame* slot = &g_worstFrames[0];
        for (WorstFrame& candidate : g_worstFrames) {
            if (candidate.frameUs < slot->frameUs) slot = &candidate;
        }
        if (frameUs > slot->frameUs) {
            slot->frameUs = frameUs;
            slot->logicUs = MillisecondsToMicroseconds(logicMs);
            slot->drawUs = MillisecondsToMicroseconds(drawMs);
            slot->vblankUs = counter(RendererCounter::VblankUs);
            slot->ioUs = counter(RendererCounter::IoUs);
            slot->ioOps = counter(RendererCounter::IoOps);
            slot->ioBytes = counter(RendererCounter::IoBytes);
            slot->entitiesUs = counter(RendererCounter::EntitiesUs);
            slot->ticks = counter(RendererCounter::PlayLogicTicks);
            slot->frameIndex = g_sessionFrames;
            slot->slowIoUs = g_frameSlowIoUs;
            std::memcpy(slot->slowIoPath, g_frameSlowIoPath, kIoPathChars);
        }
        g_frameSlowIoUs = 0;
        g_frameSlowIoPath[0] = '\0';
    }
    std::fill(g_rendererCounters + static_cast<u32>(RendererCounter::PlayLogicTicks),
        g_rendererCounters + static_cast<u32>(RendererCounter::Count), 0u);
    g_frame.Push(frameUs);
    g_logic.Push(MillisecondsToMicroseconds(logicMs));
    g_draw.Push(MillisecondsToMicroseconds(drawMs));
    g_swap.Push(MillisecondsToMicroseconds(swapMs));
    g_drawCalls.Push(g_currentDrawCalls);
    g_submittedVertices.Push(g_currentSubmittedVertices);
    g_pageCacheMisses.Push(g_currentPageCacheMisses);
    ++g_totalFrames;
#if defined(PSP_PERF_DIAGNOSTIC)
    // Only explicit fault diagnostics may perturb scheduling with I/O.
    if (g_totalFrames <= 5u || (g_totalFrames % 60u) == 0u) {
        WriteReport();
    }
#endif
}

bool ShouldStop() {
#if defined(PSP_PERF_SOAK)
    return false;
#else
    return g_totalFrames >= kWindowSize;
#endif
}

void WriteReport() {
    WriteReportTo("PSP_PERF.TXT");
}

void WriteReportTo(const char* path, u32 scenario, s32 level) {
    // Keep enough room for the complete profile, including every renderer
    // counter. Formatting happens only after the measurement window.
    char report[10240] = {};
    const u32 averageFrameUs = g_frame.Average();
    const u32 p50FrameUs = Percentile(g_frame, 50);
    const u32 p95FrameUs = Percentile(g_frame, 95);
    const u32 p99FrameUs = Percentile(g_frame, 99);
    const u32 maxFrameUs = g_frame.count == 0
        ? 0u
        : *std::max_element(g_frame.values.begin(), g_frame.values.begin() + g_frame.count);

    std::snprintf(
        report,
        sizeof(report),
        "Re:Chain PSP frame profile\n"
        "target_fps=60\n"
        "budget_us=%lu\n"
        "total_frames=%lu\n"
        "window_frames=%lu\n"
        "avg_frame_us=%lu\n"
        "avg_fps=%.3f\n"
        "p50_frame_us=%lu\n"
        "p95_frame_us=%lu\n"
        "p99_frame_us=%lu\n"
        "max_frame_us=%lu\n"
        "over_budget_window=%lu\n"
        "logic_avg_us=%lu\n"
        "draw_avg_us=%lu\n"
        "swap_avg_us=%lu\n"
        "draw_calls_avg=%lu\n"
        "submitted_vertices_avg=%lu\n"
        "page_cache_misses_avg=%lu\n",
        static_cast<unsigned long>(kFrameBudgetUs),
        static_cast<unsigned long>(g_totalFrames),
        static_cast<unsigned long>(g_frame.count),
        static_cast<unsigned long>(averageFrameUs),
        FramesPerSecond(averageFrameUs),
        static_cast<unsigned long>(p50FrameUs),
        static_cast<unsigned long>(p95FrameUs),
        static_cast<unsigned long>(p99FrameUs),
        static_cast<unsigned long>(maxFrameUs),
        static_cast<unsigned long>(CountOverBudget()),
        static_cast<unsigned long>(g_logic.Average()),
        static_cast<unsigned long>(g_draw.Average()),
        static_cast<unsigned long>(g_swap.Average()),
        static_cast<unsigned long>(g_drawCalls.Average()),
        static_cast<unsigned long>(g_submittedVertices.Average()),
        static_cast<unsigned long>(g_pageCacheMisses.Average()));

    const u32 heapUsedSinceProfileStart = g_memory.samples == 0
        || g_memory.firstHeapFree < g_memory.lastHeapFree
        ? 0u : g_memory.firstHeapFree - g_memory.lastHeapFree;
    const u32 memoryLength = static_cast<u32>(std::strlen(report));
    std::snprintf(
        report + memoryLength,
        sizeof(report) - memoryLength,
        "\nMemory/VRAM\n"
        "memory_samples=%lu\n"
        "heap_free_last=%lu\n"
        "heap_free_min=%lu\n"
        "heap_free_max=%lu\n"
        "heap_max_free_last=%lu\n"
        "heap_used_since_profile_start=%lu\n"
        "vram_used_last=%lu\n"
        "vram_used_max=%lu\n"
        "vram_free_last=%lu\n"
        "vram_free_min=%lu\n"
        "renderer_ram_bytes=%lu\n"
        "audio_stream_ram_bytes=%lu\n"
        "audio_callback_count=%lu\n"
        "audio_underruns=%lu\n"
        "audio_callback_max_us=%lu\n",
        static_cast<unsigned long>(g_memory.samples),
        static_cast<unsigned long>(g_memory.lastHeapFree),
        static_cast<unsigned long>(g_memory.minHeapFree == std::numeric_limits<u32>::max() ? 0u : g_memory.minHeapFree),
        static_cast<unsigned long>(g_memory.maxHeapFree),
        static_cast<unsigned long>(g_memory.lastHeapMax),
        static_cast<unsigned long>(heapUsedSinceProfileStart),
        static_cast<unsigned long>(g_memory.lastVramUsed),
        static_cast<unsigned long>(g_memory.maxVramUsed),
        static_cast<unsigned long>(g_memory.lastVramFree),
        static_cast<unsigned long>(g_memory.minVramFree == std::numeric_limits<u32>::max() ? 0u : g_memory.minVramFree),
        static_cast<unsigned long>(g_rendererMemoryBytes),
        static_cast<unsigned long>(g_audioMemoryBytes),
        static_cast<unsigned long>(g_audioCallbackCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_audioUnderruns.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_audioMaxUs.load(std::memory_order_relaxed)));

    const u32 allocatorLength = static_cast<u32>(std::strlen(report));
    std::snprintf(report + allocatorLength, sizeof(report) - allocatorLength,
        "libc_arena=%lu\nlibc_used=%lu\nlibc_free=%lu\nlibc_free_chunks=%lu\n"
        "libc_used_min=%lu\nlibc_used_max=%lu\n",
        static_cast<unsigned long>(g_memory.libcArena),
        static_cast<unsigned long>(g_memory.libcUsed),
        static_cast<unsigned long>(g_memory.libcFree),
        static_cast<unsigned long>(g_memory.libcFreeChunks),
        static_cast<unsigned long>(g_memory.libcUsedMin),
        static_cast<unsigned long>(g_memory.libcUsedMax));

    const u32 audioLength = static_cast<u32>(std::strlen(report));
    std::snprintf(
        report + audioLength,
        sizeof(report) - audioLength,
        "audio_music_starts=%lu\n"
        "audio_music_mix_callbacks=%lu\n"
        "audio_music_mixed_frames=%lu\n"
        "audio_worker_errors=%lu\n"
        "audio_callback_total_us=%lu\n"
        "music_decodes=%lu\n"
        "music_decode_total_us=%lu\n",
        static_cast<unsigned long>(g_audioMusicStarts.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_audioMusicMixCallbacks.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_audioMusicMixedFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_audioWorkerErrors.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_audioCallbackTotalUs.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_musicDecodeCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(g_musicDecodeTotalUs.load(std::memory_order_relaxed)));

    const std::size_t metadataOffset = std::strlen(report);
    std::snprintf(report + metadataOffset, sizeof(report) - metadataOffset,
        "schema_version=2\nbuild_stamp=%s %s\nrun_start_us=%llu\n"
        "report_uptime_us=%llu\nscenario=%lu\nlevel=%ld\nscenario_note=%lu\npalette_path=%s\n",
        __DATE__, __TIME__, static_cast<unsigned long long>(g_runStartUs),
        static_cast<unsigned long long>(sceKernelGetSystemTimeWide()),
        static_cast<unsigned long>(scenario), static_cast<long>(level),
        static_cast<unsigned long>(g_scenarioNote),
#if defined(PSP_PALETTE_REFERENCE)
        "reference"
#else
        "sequential"
#endif
    );
    for (u32 slot = 0; slot < g_microbench.size(); ++slot) {
        if (g_microbench[slot] == 0) continue;
        const std::size_t length = std::strlen(report);
        std::snprintf(report + length, sizeof(report) - length, "microbench_%lu_us=%lu\n",
            static_cast<unsigned long>(slot), static_cast<unsigned long>(g_microbench[slot]));
    }
    static constexpr const char* rendererNames[] = {
        "page_binds_avg", "gu_state_writes_avg", "list_flushes_avg",
        "staging_bytes_avg", "staging_copies_avg", "matrix_uploads_avg", "clut_loads_avg",
        "geometry_builds_avg", "geometry_cache_hits_avg",
        "geometry_dynamic_patches_avg", "geometry_dynamic_patch_vertices_avg",
        "opaque_queued_draws_avg", "opaque_queue_flushes_avg",
        "ge_sync_avg_us", "vblank_avg_us", "palette_entries_avg",
        "entities_avg_us", "blocks_avg_us", "submit_avg_us", "skin_prepare_avg_us",
        "skin_light_avg_us", "vertex_pack_avg_us", "light_evaluations_avg", "light_cache_hits_avg",
        "skinned_vertices_avg", "skin_view_culls_avg",
        "ent_humanoid_avg_us", "ent_inactive_pickup_avg_us", "ent_pickup_avg_us", "ent_move_avg_us",
        "ent_humanoid_draws_avg", "ent_inactive_pickup_draws_avg", "ent_pickup_draws_avg",
        "ent_move_draws_avg",
        "play_logic_ticks_avg", "logic_handlers_avg_us", "anim_tick_avg_us",
        "effects_avg_us", "camera_avg_us", "play_audio_avg_us",
        "io_avg_us", "io_ops_avg", "io_bytes_avg",
        "skeleton_avg_us", "skeleton_builds_avg", "skeleton_joints_avg",
        "packed_lane_holds_avg", "packed_lane_compares_avg", "material_run_builds_avg",
        "suit_lane_builds_avg", "suit_lane_mismatches_avg",
        "skin_kernel_avg_us", "skin_suit_avg_us", "bind_avg_us", "stage_avg_us",
        "overlay_quads_avg", "overlay_avg_us",
        "recorded_draw_calls_avg", "recorded_draw_builds_avg",
        "geo_light_reuses_avg", "geo_light_builds_avg", "geo_light_mismatches_avg",
        "skel_builds_display_avg", "skel_builds_etree_avg", "skel_builds_capture_avg",
        "skel_builds_effect_avg", "skel_builds_stack_avg", "hud_avg_us"
    };
    static_assert(sizeof(rendererNames) / sizeof(rendererNames[0])
        == static_cast<u32>(RendererCounter::Count), "renderer schema mismatch");
    for (u32 i = 0; i < static_cast<u32>(RendererCounter::Count); ++i) {
        const std::size_t length = std::strlen(report);
        // Retain fractional averages: e.g. 599 flushes in 600 frames must
        // not appear as zero. Formatting runs only when writing the report.
        std::snprintf(report + length, sizeof(report) - length, "%s=%.3f\n",
            rendererNames[i], g_totalFrames
                ? static_cast<double>(g_rendererTotals[i]) / g_totalFrames : 0.0);
    }
    {
        const std::size_t length = std::strlen(report);
        std::snprintf(report + length, sizeof(report) - length,
            "session_frames=%lu\nframes_over_33ms=%lu\nframes_over_50ms=%lu\n"
            "session_io_ms=%lu\nfused_skin_checks=%lu\nfused_skin_mismatches=%lu\n",
            static_cast<unsigned long>(g_sessionFrames),
            static_cast<unsigned long>(g_framesOver33Ms),
            static_cast<unsigned long>(g_framesOver50Ms),
            static_cast<unsigned long>(g_sessionIoUs / 1000u),
            static_cast<unsigned long>(g_fusedSkinChecks),
            static_cast<unsigned long>(g_fusedSkinMismatches));
    }
    {
        std::array<WorstFrame, kWorstFrames> sorted = g_worstFrames;
        std::sort(sorted.begin(), sorted.end(), [](const WorstFrame& a, const WorstFrame& b) {
            return a.frameUs > b.frameUs;
        });
        for (u32 i = 0; i < kWorstFrames; ++i) {
            const WorstFrame& frame = sorted[i];
            if (frame.frameUs == 0) break;
            const std::size_t length = std::strlen(report);
            std::snprintf(report + length, sizeof(report) - length,
                "worst_%lu=frame_us:%lu logic_us:%lu draw_us:%lu vblank_us:%lu io_us:%lu "
                "io_ops:%lu io_bytes:%lu entities_us:%lu ticks:%lu at_frame:%lu "
                "slow_io_us:%lu slow_io:%s\n",
                static_cast<unsigned long>(i), static_cast<unsigned long>(frame.frameUs),
                static_cast<unsigned long>(frame.logicUs), static_cast<unsigned long>(frame.drawUs),
                static_cast<unsigned long>(frame.vblankUs), static_cast<unsigned long>(frame.ioUs),
                static_cast<unsigned long>(frame.ioOps), static_cast<unsigned long>(frame.ioBytes),
                static_cast<unsigned long>(frame.entitiesUs), static_cast<unsigned long>(frame.ticks),
                static_cast<unsigned long>(frame.frameIndex),
                static_cast<unsigned long>(frame.slowIoUs),
                frame.slowIoPath[0] ? frame.slowIoPath : "-");
        }
        std::array<IoFileMetric, kIoFiles> files = g_ioFiles;
        std::sort(files.begin(), files.end(), [](const IoFileMetric& a, const IoFileMetric& b) {
            return a.totalUs > b.totalUs;
        });
        for (u32 i = 0; i < kIoFiles; ++i) {
            if (!files[i].path[0]) break;
            const std::size_t length = std::strlen(report);
            std::snprintf(report + length, sizeof(report) - length,
                "io_file_%02lu=total_ms:%lu ops:%lu max_us:%lu path:%s\n",
                static_cast<unsigned long>(i),
                static_cast<unsigned long>(files[i].totalUs / 1000u),
                static_cast<unsigned long>(files[i].ops),
                static_cast<unsigned long>(files[i].maxUs), files[i].path);
        }
    }
    // Handler callbacks as offsets from PspPerf::Reset (module-relative, so
    // the same table resolves against the PRX or the static ELF symbols).
    {
        const std::size_t length = std::strlen(report);
        std::snprintf(report + length, sizeof(report) - length,
            "handler_ref=0x%08lx\nhandler_count=%lu\n",
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&Reset)),
            static_cast<unsigned long>(g_handlerCount));
    }
    for (u32 i = 0; i < g_handlerCount; ++i) {
        const HandlerMetric& metric = g_handlers[i];
        const long offset = static_cast<long>(reinterpret_cast<uintptr_t>(metric.callback))
            - static_cast<long>(reinterpret_cast<uintptr_t>(&Reset));
        const std::size_t length = std::strlen(report);
        std::snprintf(report + length, sizeof(report) - length,
            "handler_%02lu=fn_minus_ref:%ld avg_us:%.1f calls_per_frame:%.2f\n",
            static_cast<unsigned long>(i), offset,
            g_totalFrames ? static_cast<double>(metric.totalUs) / g_totalFrames : 0.0,
            g_totalFrames ? static_cast<double>(metric.calls) / g_totalFrames : 0.0);
    }
    p3d::io::WriteTextFile(
        path && path[0] ? path : "PSP_PERF.TXT", std::string(report));
}

void RecordMicrobench(u32 slot, u32 totalUs) {
    if (slot < g_microbench.size()) g_microbench[slot] = totalUs;
}

void SetScenarioNote(u32 note) {
    g_scenarioNote = note;
}

void RecordFusedSkinCheck(bool matched) {
    ++g_fusedSkinChecks;
    if (!matched) ++g_fusedSkinMismatches;
}

void WriteState(const char* state, s32 levelId, s32 levelIndex, s32 targetIndex) {
#if defined(PSP_BENCH_STATE_IO)
    char report[192] = {};
    std::snprintf(report, sizeof(report),
        "state=%s\nlevel_id=%ld\nlevel_index=%ld\ntarget_index=%ld\n",
        state && state[0] ? state : "unknown",
        static_cast<long>(levelId),
        static_cast<long>(levelIndex),
        static_cast<long>(targetIndex));
    p3d::io::WriteTextFile("PSP_BENCH_STATE.TXT", std::string(report));
#else
    // Detailed transition breadcrumbs are intentionally disabled in the
    // benchmark build. DemandLoading() can call WEffect_PopulateWEffects()
    // while the player crosses a block boundary; opening/truncating/writing
    // a file for every effect makes that gameplay frame miss its budget on
    // both real PSP hardware and PPSSPP. Define PSP_BENCH_STATE_IO only for
    // a dedicated transition-debug build, never for performance measurement.
    (void)state;
    (void)levelId;
    (void)levelIndex;
    (void)targetIndex;
#endif
}

void GetSnapshot(Snapshot* out) {
    if (!out) return;
    *out = {};
    out->totalFrames = g_totalFrames;
    out->windowFrames = static_cast<u32>(g_frame.count);
    out->averageFrameUs = g_frame.Average();
    out->p50FrameUs = Percentile(g_frame, 50);
    out->p95FrameUs = Percentile(g_frame, 95);
    out->p99FrameUs = Percentile(g_frame, 99);
    out->maxFrameUs = g_frame.count == 0
        ? 0u
        : *std::max_element(g_frame.values.begin(), g_frame.values.begin() + g_frame.count);
    out->overBudgetFrames = CountOverBudget();
    out->logicAverageUs = g_logic.Average();
    out->drawAverageUs = g_draw.Average();
    out->swapAverageUs = g_swap.Average();
    out->drawCallsAverage = g_drawCalls.Average();
    out->submittedVerticesAverage = g_submittedVertices.Average();
    out->pageCacheMissesAverage = g_pageCacheMisses.Average();
    for (u32 i = 0; i < static_cast<u32>(RendererCounter::Count); ++i) {
        out->rendererAverage[i] = g_totalFrames
            ? static_cast<u32>(g_rendererTotals[i] / g_totalFrames) : 0u;
    }
    out->rendererMemoryBytes = g_rendererMemoryBytes;
    out->audioMemoryBytes = g_audioMemoryBytes;
    out->audioUnderruns = g_audioUnderruns.load(std::memory_order_relaxed);
    out->audioWorkerErrors = g_audioWorkerErrors.load(std::memory_order_relaxed);
}

void Trace(const char* message) {
#if defined(PSP_TRACE_TELEMETRY)
    if (!message) return;
    if (std::strcmp(message, "play: before Step") == 0) {
        // Capture the first complete gameplay frame. The previous value of
        // one skipped the first frame and left PSP_TRACE.TXT empty whenever
        // the test stopped before the next logic tick.
        // Capture a small number of complete/incomplete frames. The first
        // frame often succeeds while the next one stalls in a streamed model
        // or GU path; limiting this to three frames keeps the diagnostic file
        // bounded and avoids turning a profile into unbounded I/O.
        g_traceFrameActive = (g_tracePlayFrame < 3u);
        if (g_traceFrameActive) {
            if (g_traceFd >= 0) sceIoClose(g_traceFd);
            g_traceFd = sceIoOpen("PSP_TRACE.TXT", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        }
        return;
    }
    if (std::strcmp(message, "play: after RecordFrame") == 0) {
        if (g_traceFrameActive && g_traceFd >= 0) {
            const char* line = "play: after RecordFrame\n";
            sceIoWrite(g_traceFd, line, static_cast<SceSize>(std::strlen(line)));
            sceIoClose(g_traceFd);
            g_traceFd = -1;
        }
        g_traceFrameActive = false;
        ++g_tracePlayFrame;
        return;
    }
    if (!g_traceFrameActive || !IsTraceInteresting(message)) return;
    if (g_traceFd < 0) {
        g_traceFd = sceIoOpen("PSP_TRACE.TXT", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    }
    if (g_traceFd < 0) return;
    char line[192] = {};
    const int length = std::snprintf(line, sizeof(line), "%s\n", message);
    if (length > 0) sceIoWrite(g_traceFd, line, static_cast<SceSize>(length));
#else
    // Trace is intentionally disabled in optimized builds. The debug trace
    // writes synchronously to the emulated Memory Stick and can starve the
    // render/audio scheduling path, invalidating both gameplay and profiling.
    (void)message;
#endif
}

} // namespace PspPerf

#else

namespace PspPerf {

void Reset() {}
void BeginMeasurementWindow() {}
void BeginFrame() {}
void RecordDrawSubmission(u32) {}
void RecordPageCacheMiss() {}
void RecordFrame(u32, f32, f32, f32) {}
void RecordMemorySnapshot(u32, u32, u32, u32) {}
void AddRendererMemory(s32) {}
void SetAudioMemory(u32) {}
void RecordAudioCallback(u32) {}
void RecordMusicDecode(u32) {}
void RecordMicrobench(u32, u32) {}
void RecordAudioUnderrun() {}
void RecordAudioMusicStart() {}
void RecordAudioMusicMix(u32) {}
void RecordAudioWorkerError() {}
bool ShouldStop() { return false; }
void WriteReport() {}
void WriteReportTo(const char*, u32, s32) {}
void WriteState(const char*, s32, s32, s32) {}
void SetScenarioNote(u32) {}
void RecordFusedSkinCheck(bool) {}
void GetSnapshot(Snapshot* out) { if (out) *out = {}; }
void Trace(const char*) {}

} // namespace PspPerf

#endif

#include "psp/psp_perf.h"

#include "p3d/fileio.h"

#if defined(PSP_PERF_TELEMETRY)

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include <pspiofilemgr.h>

namespace {

constexpr std::size_t kWindowSize = 600;
constexpr u32 kFrameBudgetUs = 16667;

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
u32 g_totalFrames = 0;
u32 g_currentDrawCalls = 0;
u32 g_currentSubmittedVertices = 0;
u32 g_currentPageCacheMisses = 0;
SceUID g_traceFd = -1;
u32 g_tracePlayFrame = 0;
bool g_traceFrameActive = false;

static bool IsTraceInteresting(const char* message) {
    return std::strstr(message, "block draw:") != nullptr
        || std::strstr(message, "geometry clip:") != nullptr
        || std::strstr(message, "gu draw:") != nullptr
        || std::strstr(message, "gu memory:") != nullptr;
}

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

void Reset() {
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
    g_tracePlayFrame = 0;
    g_traceFrameActive = false;
}

void BeginFrame() {
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

void RecordFrame(u32 frameUs, f32 logicMs, f32 drawMs, f32 swapMs) {
    g_frame.Push(frameUs);
    g_logic.Push(MillisecondsToMicroseconds(logicMs));
    g_draw.Push(MillisecondsToMicroseconds(drawMs));
    g_swap.Push(MillisecondsToMicroseconds(swapMs));
    g_drawCalls.Push(g_currentDrawCalls);
    g_submittedVertices.Push(g_currentSubmittedVertices);
    g_pageCacheMisses.Push(g_currentPageCacheMisses);
    ++g_totalFrames;
    // Temporary progress checkpoint while validating unattended PPSSPP runs.
    // The final profile writes once during shutdown.
    if (g_totalFrames == 1u || (g_totalFrames % 60u) == 0u) {
        WriteReport();
    }
}

bool ShouldStop() {
    return g_totalFrames >= kWindowSize;
}

void WriteReport() {
    char report[2048] = {};
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

    p3d::io::WriteTextFile("PSP_PERF.TXT", std::string(report));
}

void Trace(const char* message) {
    if (!message) return;
    if (std::strcmp(message, "play: before Step") == 0) {
        g_traceFrameActive = (g_tracePlayFrame == 1u);
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
}

} // namespace PspPerf

#else

namespace PspPerf {

void Reset() {}
void BeginFrame() {}
void RecordDrawSubmission(u32) {}
void RecordPageCacheMiss() {}
void RecordFrame(u32, f32, f32, f32) {}
bool ShouldStop() { return false; }
void WriteReport() {}
void Trace(const char*) {}

} // namespace PspPerf

#endif

#pragma once

#include "gen/common.h"

namespace PspPerf {

// Main-thread counters only: no allocations, clocks or I/O on a draw.
enum class RendererCounter : u32 {
    PageBinds, GuStateWrites, ListFlushes, StagingBytes, StagingCopies,
    MatrixUploads, ClutLoads, ClutCacheHits, ClutLoadSkips, Count
};

struct Snapshot {
    u32 totalFrames = 0;
    u32 windowFrames = 0;
    u32 averageFrameUs = 0;
    u32 p50FrameUs = 0;
    u32 p95FrameUs = 0;
    u32 p99FrameUs = 0;
    u32 maxFrameUs = 0;
    u32 overBudgetFrames = 0;
    u32 logicAverageUs = 0;
    u32 drawAverageUs = 0;
    u32 swapAverageUs = 0;
    u32 drawCallsAverage = 0;
    u32 submittedVerticesAverage = 0;
    u32 pageCacheMissesAverage = 0;
    u32 rendererAverage[static_cast<u32>(RendererCounter::Count)] = {};
    u32 rendererMemoryBytes = 0;
    u32 audioMemoryBytes = 0;
    u32 audioUnderruns = 0;
    u32 audioWorkerErrors = 0;
};
#if defined(PSP_PERF_TELEMETRY)
extern u32 g_rendererCounters[static_cast<u32>(RendererCounter::Count)];
inline void CountRenderer(RendererCounter counter, u32 amount = 1) {
    g_rendererCounters[static_cast<u32>(counter)] += amount;
}
#else
inline void CountRenderer(RendererCounter, u32 = 1) {}
#endif

// Optional RAM-only breadcrumbs. Read g_runtimeMarkers with the debugger;
// never perform file I/O, allocation or a clock syscall from a draw marker.
enum class RuntimeStage : u32 {
    FrameBegin = 1, WorldBegin, WorldDemandBegin, WorldDemandDone, WorldBlock,
    WorldGeometryBegin, WorldGeometryDone, WorldEnd,
    PrimBegin, MatrixBegin, MatrixDone, CacheBegin, CacheHit, CacheMiss,
    DecodeBegin, DecodeDone, UploadBegin, CopyQueued, TexSyncQueued,
    CacheReady, CacheFull, BindDone, DrawBegin, DrawDone,
    FinishBegin, FinishDone, SyncBegin, SyncDone
};
#if defined(PSP_RUNTIME_DIAGNOSTIC)
struct RuntimeMarker { u32 sequence, stage, value, detail; };
extern volatile RuntimeMarker g_runtimeMarkers[256];
extern volatile u32 g_runtimeMarkerSequence;
void Mark(RuntimeStage stage, u32 value = 0, u32 detail = 0);
#else
inline void Mark(RuntimeStage, u32 = 0, u32 = 0) {}
#endif

// The profile build keeps these calls allocation-free on the frame path. In a
// normal release build they compile to no-ops; profile builds enable the
// fixed-size rolling window and write PSP_PERF.TXT on shutdown.
void Reset();
// Clears the rolling timing/counter window without discarding resident
// renderer/audio memory accounting collected during warmup.
void BeginMeasurementWindow();
void BeginFrame();
void RecordDrawSubmission(u32 vertexCount);
void RecordPageCacheMiss();
void RecordFrame(u32 frameUs, f32 logicMs, f32 drawMs, f32 swapMs);
void RecordMemorySnapshot(u32 heapFree, u32 heapMax, u32 vramUsed, u32 vramFree);
void AddRendererMemory(s32 bytes);
void SetAudioMemory(u32 bytes);
void RecordAudioCallback(u32 durationUs);
void RecordAudioUnderrun();
void RecordAudioMusicStart();
void RecordAudioMusicMix(u32 mixedFrames);
void RecordAudioWorkerError();
bool ShouldStop();
void WriteReport();
void WriteReportTo(const char* path);
// Transition-only breadcrumb. Unlike frame telemetry, this performs I/O only
// at level/benchmark state boundaries and is safe to inspect over PSPLINK.
void WriteState(const char* state, s32 levelId, s32 levelIndex, s32 targetIndex);
void GetSnapshot(Snapshot* out);
void Trace(const char* message);

} // namespace PspPerf

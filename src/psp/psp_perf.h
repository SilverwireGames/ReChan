#pragma once

#include "gen/common.h"

namespace PspPerf {

// Main-thread counters only: no allocations, clocks or I/O on a draw.
enum class RendererCounter : u32 {
    PageBinds, GuStateWrites, ListFlushes, StagingBytes, StagingCopies,
    MatrixUploads, ClutLoads, Count
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
void Trace(const char* message);

} // namespace PspPerf

#pragma once

#include "gen/common.h"

namespace PspPerf {

// Main-thread counters only: increments do not allocate, read clocks or do I/O.
// Optional ScopeTimer instrumentation below reads the clock in profile builds.
enum class RendererCounter : u32 {
    PageBinds, GuStateWrites, ListFlushes, StagingBytes, StagingCopies,
    MatrixUploads, ClutLoads, GeometryBuilds, GeometryCacheHits,
    GeometryDynamicPatches, GeometryDynamicPatchVertices, OpaqueQueuedDraws,
    OpaqueQueueFlushes, GeSyncUs, VblankUs, PaletteEntries,
    EntitiesUs, BlocksUs, SubmitUs, SkinPrepareUs, SkinLightUs, VertexPackUs,
    LightEvaluations, LightCacheHits, SkinnedVertices, SkinViewCulls,
    // EntitiesUs split by AI list (humanoid, inactive pickup, pickup, move),
    // with the number of Thing::Draw calls per list.
    EntHumanoidUs, EntInactivePickupUs, EntPickupUs, EntMoveUs,
    EntHumanoidDraws, EntInactivePickupDraws, EntPickupDraws, EntMoveDraws,
    // Play-state breakdown of the "logic" share (everything in Game::Step
    // outside World::Render and SwapBuffers). AnimTickUs includes EffectsUs.
    PlayLogicTicks, LogicHandlersUs, AnimTickUs, EffectsUs, CameraUs, PlayAudioUs,
    // Main-thread file access (Memory Stick open/read/seek/stat): it blocks
    // the frame, unlike the music worker's streaming reads.
    IoUs, IoOps, IoBytes,
    // Skeleton world-matrix builds (logic and render) and their joints.
    SkeletonUs, SkeletonBuilds, SkeletonJoints,
    // Fused character uploads whose material lanes the buffer already held
    // vs. compared per vertex, rebuilt material runs, built suit CBA lanes,
    // and cached suit lanes that differed from a fresh build (must stay 0).
    PackedLaneHolds, PackedLaneCompares, MaterialRunBuilds, SuitLaneBuilds,
    SuitLaneMismatches,
    // Inside SkinPrepareUs: the fused skinning loop and the suit lane lookup.
    SkinKernelUs, SkinSuitUs,
    // Inside SubmitUs: page/CLUT binds and list-memory staging copies.
    BindUs, StageUs,
    // HUD/overlay quads (DrawQuadBatch, DrawGouraudQuad) and their time.
    OverlayQuads, OverlayUs,
    // Static buffers drawn by calling their recorded GE list, and lists built.
    RecordedDrawCalls, RecordedDrawBuilds,
    // Lit prop draws whose colours were reused, built, and reused colours that
    // differed from a fresh build (re-checked every 64 reuses; must stay 0).
    GeoLightReuses, GeoLightBuilds, GeoLightMismatches,
    // Skeleton builds by caller: skinned draw, rigid parts (ETree), attack
    // capture, joint-attached effects (WEffect), pushable stacks.
    SkelBuildsDisplay, SkelBuildsETree, SkelBuildsCapture, SkelBuildsEffect, SkelBuildsStack,
    // The whole section HUD (HUD::Display: update, text layout and overlay
    // submission), which OverlayUs only covers in the renderer.
    HudUs,
    // Inside SkeletonUs: the joints' pre/post callbacks (game code).
    SkelCallbackUs,
    // Director overlay pass (DrawDirectorOverlays): its time, the effects it
    // displayed and the effects list it walked to find them.
    DirectorOverlayUs, OverlayEffectDraws, OverlayEffectWalk,
    // SetVertexData calls that repeated the buffer's current input.
    VertexUploadSkips,
    // Page binds whose texture the GE samples from RAM (no eDRAM slot).
    RamPageBinds,
    Count
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

// Main-thread file I/O timing (see RendererCounter::IoUs). Reads issued by
// other threads (music streaming) are ignored.
// path names the file for open/stat; reads and seeks pass nullptr and are
// charged to the file opened last on the main thread.
#if defined(PSP_PERF_TELEMETRY)
u32 IoClock();
void RecordIo(u32 startClock, u32 bytes, const char* path = nullptr);
#else
inline u32 IoClock() { return 0; }
inline void RecordIo(u32, u32, const char* = nullptr) {}
#endif

// Per-handler time for Game's handler lists, keyed by callback address. The
// report prints each callback's offset from PspPerf::Reset so a PRX address
// can be matched against the ELF symbol table (psp-addr2line / nm).
#if defined(PSP_PERF_TELEMETRY)
u32 HandlerClock();
void RecordHandler(const void* callback, u32 startClock);
#else
inline u32 HandlerClock() { return 0; }
inline void RecordHandler(const void*, u32) {}
#endif

// Inclusive main-thread scopes; nested categories must not be summed.
// Compiles away in release/desktop builds and never performs I/O. Each one
// reads the system clock twice (a syscall, ~0.57 us each on the PSP-3000);
// PSP_PERF_FRAME_ONLY drops them to measure a frame without that overhead.
class ScopeTimer {
public:
#if defined(PSP_PERF_TELEMETRY) && !defined(PSP_PERF_FRAME_ONLY)
    explicit ScopeTimer(RendererCounter counter);
    ~ScopeTimer() { Stop(); }
    void Stop();
private:
    RendererCounter counter;
    u32 start;
    bool active = true;
#else
    explicit ScopeTimer(RendererCounter) {}
    void Stop() {}
#endif
    ScopeTimer(const ScopeTimer&) = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;
};

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
// Music worker: one ADPCM pair decode (both channels), read excluded.
void RecordMusicDecode(u32 durationUs);
void RecordAudioUnderrun();
void RecordAudioMusicStart();
void RecordAudioMusicMix(u32 mixedFrames);
void RecordAudioWorkerError();
bool ShouldStop();
void WriteReport();
void WriteReportTo(const char* path, u32 scenario = 0, s32 level = -1);
// Free-form number written as scenario_note= (e.g. enemies gathered).
void SetScenarioNote(u32 note);
// PSP_DRAW_MICROBENCH: total time of one repeated renderer operation,
// written as microbench_<slot>_us= in every later report.
void RecordMicrobench(u32 slot, u32 totalUs);
// On-device equivalence check of the fused character upload (model.cpp):
// reported as fused_skin_checks= / fused_skin_mismatches=.
void RecordFusedSkinCheck(bool matched);
// Transition-only breadcrumb. Unlike frame telemetry, this performs I/O only
// at level/benchmark state boundaries and is safe to inspect over PSPLINK.
void WriteState(const char* state, s32 levelId, s32 levelIndex, s32 targetIndex);
void GetSnapshot(Snapshot* out);
void Trace(const char* message);

} // namespace PspPerf

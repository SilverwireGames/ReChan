#pragma once

#include "gen/common.h"

namespace PspPerf {

// The profile build keeps these calls allocation-free on the frame path. In a
// normal release build they compile to no-ops; profile builds enable the
// fixed-size rolling window and write PSP_PERF.TXT on shutdown.
void Reset();
void BeginFrame();
void RecordDrawSubmission(u32 vertexCount);
void RecordPageCacheMiss();
void RecordFrame(u32 frameUs, f32 logicMs, f32 drawMs, f32 swapMs);
bool ShouldStop();
void WriteReport();
void Trace(const char* message);

} // namespace PspPerf

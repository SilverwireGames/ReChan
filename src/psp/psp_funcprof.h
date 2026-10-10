#pragma once

// Function-level sampling profiler for real PSP hardware (PSP_FUNC_PROFILE).
//
// Instrumented objects (-finstrument-functions, see psp/Makefile) keep a
// shadow call stack of the main thread; a 2 kHz VTimer samples it and charges
// the top function (self) and every function on the stack (inclusive). Unlike
// libpspprof it needs no text-sized buffers (~50 KB total), so it runs as a
// PSPLink PRX, and it attributes samples to the function actually running
// instead of the last one entered. Calls from other threads (audio) are
// ignored by checking the stack pointer against the main thread's stack.
namespace PspFuncProfile {

#if defined(PSP_FUNC_PROFILE)
// Clears the table and starts sampling. Call from the main thread.
void Start();
// Stops sampling and writes "<offset from Start> self incl" lines to path.
void StopAndWrite(const char* path);
#else
inline void Start() {}
inline void StopAndWrite(const char*) {}
#endif

} // namespace PspFuncProfile

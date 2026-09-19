#pragma once
#include "sdk.h"
#if defined(PSP_PERF_TELEMETRY)
struct mallinfo { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
inline struct mallinfo testMallinfo{};
inline struct mallinfo mallinfo() { return testMallinfo; }
#endif

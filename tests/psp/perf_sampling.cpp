#include "stubs/pspgu.h"
#include "../../src/psp/psp_perf.cpp"

static int reportWrites = 0;
static std::string lastReport;
namespace p3d::io {
bool WriteTextFile(const std::string&, const std::string& text) {
    ++reportWrites;
    lastReport = text;
    return true;
}
}

int main() {
    using FakeGe::require;
    PspPerf::Reset();
    testMallinfo = {4096, 3, 0, 0, 0, 0, 0, 1024, 3072, 2048};
    PspPerf::RecordMemorySnapshot(8192, 4096, 2048000, 49152);
    testMallinfo.uordblks = 1536;
    testMallinfo.fordblks = 2560;
    PspPerf::RecordMemorySnapshot(8192, 4096, 2048000, 49152);
    require(g_memory.lastHeapFree == 8192 && g_memory.libcUsed == 1536
        && g_memory.libcUsedMin == 1024 && g_memory.libcUsedMax == 1536,
        "libc growth must be visible even when kernel free space is unchanged");
    for (int i = 0; i < 720; ++i) {
        PspPerf::BeginFrame();
        PspPerf::RecordFrame(20000, 3, 12, 5);
    }
    require(reportWrites == 0, "measurement performed gameplay file I/O");
    require(g_totalFrames == 720 && g_frame.count == 600 && g_frame.Average() == 20000,
        "long session must keep a bounded rolling window");
#if defined(PSP_PERF_SOAK)
    require(!PspPerf::ShouldStop(), "soak stopped at 600 frames");
#else
    require(PspPerf::ShouldStop(), "unattended profile no longer stops");
#endif
    PspPerf::WriteReport();
    require(reportWrites == 1 && lastReport.find("libc_used=1536") != std::string::npos,
        "shutdown report lost allocator metrics");
    std::puts("PASS: bounded sampling, separate allocator metrics, no gameplay I/O");
}

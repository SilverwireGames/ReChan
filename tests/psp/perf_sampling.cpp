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
        {
            PspPerf::ScopeTimer timer(PspPerf::RendererCounter::SubmitUs);
            timer.Stop();
            timer.Stop(); // Explicit stop and destructor must not double count.
        }
        PspPerf::CountRenderer(PspPerf::RendererCounter::GeSyncUs, 1100);
        PspPerf::CountRenderer(PspPerf::RendererCounter::VblankUs, 3900);
        PspPerf::CountRenderer(PspPerf::RendererCounter::PaletteEntries, 4096);
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
    PspPerf::WriteReportTo("PSP_PERF_S02_L02.TXT", 2, 2);
    require(lastReport.find("schema_version=2\nbuild_stamp=") != std::string::npos
        && lastReport.find("run_start_us=1000001\n") != std::string::npos
        && lastReport.find("scenario=2\nlevel=2\n") != std::string::npos,
        "report must identify the build, run and scenario");
    require(lastReport.find("ge_sync_avg_us=1100.000") != std::string::npos
        && lastReport.find("vblank_avg_us=3900.000") != std::string::npos
        && lastReport.find("palette_entries_avg=4096.000") != std::string::npos,
        "report lost or truncated renderer counters");
    require(lastReport.find("submit_avg_us=1.000") != std::string::npos
        && lastReport.find("light_cache_hits_avg=0.000") != std::string::npos,
        "scope timer double-counted or detailed report was truncated");
    std::puts("PASS: bounded sampling, separate allocator metrics, no gameplay I/O");
}

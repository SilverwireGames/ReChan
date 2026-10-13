#include "pc/log.h"

#include <pspdebug.h>

#include <cstdarg>
#include <cstdio>

namespace {

std::FILE* g_pspLogFile = nullptr;

} // namespace

Log& Log::Get() {
    static Log instance;
    return instance;
}

Log::Log() = default;
Log::~Log() = default;
void Log::Init(const char* filename) {
    if (g_pspLogFile) return;
    g_pspLogFile = std::fopen(filename ? filename : "PSP_LOG.TXT", "wb");
}

void Log::Shutdown() {
    if (g_pspLogFile) {
        std::fflush(g_pspLogFile);
        std::fclose(g_pspLogFile);
        g_pspLogFile = nullptr;
    }
}

void Log::WriteToConsole(const char* text) {
    if (text) pspDebugScreenPrintf("%s", text);
}

void Log::LogMessage(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogMessageV(fmt, args);
    va_end(args);
}

void Log::LogMessageV(const char* fmt, va_list args) {
    char buffer[512];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
#if defined(RC_PLATFORM_PSP)
    // The PSP path logs directly to the debug file/console. Retaining every
    // message in Log::m_MessageStack creates one heap allocation per draw
    // probe and eventually starves/corrupts the small game heap while a
    // streamed level is rendering.
#else
    m_MessageStack.emplace_back(buffer);
#endif
    // Console output is tied to the log file: a release EBOOT never opens it,
    // and upstream's LOG_ALWAYS call sites must not push text through
    // pspDebugScreenPrintf in a shipping build.
    if (g_pspLogFile) {
        WriteToConsole(buffer);
        WriteToConsole("\n");
        std::fprintf(g_pspLogFile, "%s\n", buffer);
        std::fflush(g_pspLogFile);
    }
}

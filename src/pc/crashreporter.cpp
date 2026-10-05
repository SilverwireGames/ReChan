#include "pc/crashreporter.h"

#include "extra/version.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

static constexpr const char* kIssueUrl =
"https://github.com/SilverwireGames/ReChan/issues/new";

#if defined(RC_PLATFORM_WINDOWS)

#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

static volatile LONG s_handlingCrash = 0;
static char s_reportPath[MAX_PATH] = "rechan-crash.txt";
static char s_dumpPath[MAX_PATH] = "rechan-crash.dmp";

static void WriteHandle(HANDLE file, const char* text) {
    if (file == INVALID_HANDLE_VALUE || !text) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

static void AppendFileToHandle(HANDLE output, const char* path) {
    HANDLE input = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) return;

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(input, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        DWORD written = 0;
        WriteFile(output, buffer, read, &written, nullptr);
    }
    CloseHandle(input);
}

static bool EnsureMinidumpDirectory() {
    if (CreateDirectoryA("minidumps", nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesA("minidumps");
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void BuildCrashPaths() {
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
    const bool hasDirectory = EnsureMinidumpDirectory();
    std::snprintf(s_reportPath, sizeof(s_reportPath),
                  hasDirectory ? "minidumps\\rechan-crash-%lu.txt" : "rechan-crash-%lu.txt",
                  pid);
    std::snprintf(s_dumpPath, sizeof(s_dumpPath),
                  hasDirectory ? "minidumps\\rechan-crash-%lu.dmp" : "rechan-crash-%lu.dmp",
                  pid);
}

static void WriteMiniDump(EXCEPTION_POINTERS* exceptionInfo) {
    HANDLE dump = CreateFileA(s_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION info = {};
    MINIDUMP_EXCEPTION_INFORMATION* infoPtr = nullptr;
    if (exceptionInfo) {
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = exceptionInfo;
        info.ClientPointers = FALSE;
        infoPtr = &info;
    }

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                      MiniDumpScanMemory |
                      MiniDumpWithThreadInfo),
                      infoPtr, nullptr, nullptr);
    CloseHandle(dump);
}

static void WriteCrashReport(EXCEPTION_POINTERS* exceptionInfo, const char* reason) {
    BuildCrashPaths();
    WriteMiniDump(exceptionInfo);

    HANDLE report = CreateFileA(s_reportPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char buffer[2048];
    std::snprintf(buffer, sizeof(buffer),
                  "ReChan crash report\r\n"
                  "===================\r\n"
                  "Version: %s\r\n"
                  "Platform: Windows x86_64\r\n"
                  "Time: %04u-%02u-%02u %02u:%02u:%02u\r\n"
                  "Process ID: %lu\r\n"
                  "Thread ID: %lu\r\n"
                  "Reason: %s\r\n",
                  GAME_VERSION,
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long>(GetCurrentThreadId()),
                  reason ? reason : "Unhandled exception");
    WriteHandle(report, buffer);

    if (exceptionInfo && exceptionInfo->ExceptionRecord) {
        std::snprintf(buffer, sizeof(buffer),
                      "Exception code: 0x%08lX\r\nFault address: %p\r\n",
                      static_cast<unsigned long>(exceptionInfo->ExceptionRecord->ExceptionCode),
                      exceptionInfo->ExceptionRecord->ExceptionAddress);
        WriteHandle(report, buffer);

        const EXCEPTION_RECORD* record = exceptionInfo->ExceptionRecord;
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            record->NumberParameters >= 2) {
            const char* operation = record->ExceptionInformation[0] == 1 ? "write"
                : record->ExceptionInformation[0] == 8 ? "execute" : "read";
            std::snprintf(buffer, sizeof(buffer),
                          "Access violation operation: %s\r\nReferenced address: %p\r\n",
                          operation, reinterpret_cast<void*>(record->ExceptionInformation[1]));
            WriteHandle(report, buffer);
        }
    }

    WriteHandle(report, "\r\nStack addresses:\r\n");
    void* frames[64] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, 64, frames, nullptr);

    const BOOL symInitialized = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    if (symInitialized) {
        SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    }

    for (USHORT i = 0; i < frameCount; ++i) {
        bool resolved = false;
        if (symInitialized) {
            alignas(SYMBOL_INFO) char symBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
            SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symBuffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
            if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line)) {
                    std::snprintf(buffer, sizeof(buffer), "  #%02u %p %s+0x%llx (%s:%lu)\r\n", i,
                                  frames[i], symbol->Name, static_cast<unsigned long long>(displacement),
                                  line.FileName, line.LineNumber);
                }
                else {
                    std::snprintf(buffer, sizeof(buffer), "  #%02u %p %s+0x%llx\r\n", i, frames[i],
                                  symbol->Name, static_cast<unsigned long long>(displacement));
                }
                WriteHandle(report, buffer);
                resolved = true;
            }
        }
        if (!resolved) {
            std::snprintf(buffer, sizeof(buffer), "  #%02u %p\r\n", i, frames[i]);
            WriteHandle(report, buffer);
        }
    }

    if (symInitialized) {
        SymCleanup(GetCurrentProcess());
    }

    WriteHandle(report,
                "\r\nPlease open an issue at:\r\n"
                "https://github.com/SilverwireGames/ReChan/issues/new\r\n"
                "Attach this report and its matching .dmp file.\r\n");
    WriteHandle(report,
                "\r\nrechan.log\r\n"
                "==========\r\n");
    AppendFileToHandle(report, "rechan.log");
    CloseHandle(report);
}

static void NotifyUser() {
#if defined(RC_PLATFORM_NULL)
    // Headless/null-hardware build: no display to show a dialog on.
    return;
#endif
    if (std::getenv("RECHAN_CRASH_REPORTER_NO_DIALOG")) return;

    char message[1024];
    std::snprintf(message, sizeof(message),
                  "ReChan crashed and created:\n%s\n%s\n\n"
                  "Please open an issue and attach both files:\n%s",
                  s_reportPath, s_dumpPath, kIssueUrl);
    MessageBoxA(nullptr, message, "ReChan crashed", MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    if (InterlockedCompareExchange(&s_handlingCrash, 1, 0) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    WriteCrashReport(exceptionInfo, "Unhandled structured exception");
    NotifyUser();
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] static void TerminateHandler() {
    if (InterlockedCompareExchange(&s_handlingCrash, 1, 0) == 0) {
        WriteCrashReport(nullptr, "std::terminate (uncaught C++ exception or fatal runtime error)");
        NotifyUser();
    }
    TerminateProcess(GetCurrentProcess(), 3);
    std::abort();
}

static void CrtSignalHandler(int signalNumber) {
    if (InterlockedCompareExchange(&s_handlingCrash, 1, 0) == 0) {
        char reason[128];
        std::snprintf(reason, sizeof(reason), "C runtime fatal signal %d", signalNumber);
        WriteCrashReport(nullptr, reason);
        NotifyUser();
    }
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(128 + signalNumber));
}

void CrashReporter::Install() {
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    std::set_terminate(TerminateHandler);
    std::signal(SIGABRT, CrtSignalHandler);
}

void CrashReporter::ShowFatalMessage(const char* title, const char* message) {
    std::fprintf(stderr, "%s: %s\n", title, message);
#if !defined(RC_PLATFORM_NULL)
    if (std::getenv("RECHAN_CRASH_REPORTER_NO_DIALOG")) return;
    MessageBoxA(nullptr, message, title, MB_OK | MB_ICONERROR | MB_TASKMODAL);
#endif
}

#elif defined(RC_PLATFORM_SWITCH)

// Deliberately minimal no-op for this toolchain-validation milestone: no
// <execinfo.h> (glibc-only, absent from newlib), no fork()/execlp() (no
// process model on Horizon OS), no sigaction-based fault handling (Switch
// has no equivalent POSIX signal delivery for hardware faults -- real crash
// reporting here would hook libnx's own fatal-error/diagFatal path instead,
// left for a later pass once there's an actual Switch build to crash).
void CrashReporter::Install() {}

void CrashReporter::ShowFatalMessage(const char* title, const char* message) {
    std::printf("%s: %s\n", title, message);
}

#else

#include <cerrno>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t s_handlingCrash = 0;
static char s_reportPath[PATH_MAX] = "rechan-crash.txt";
static bool s_suppressDialog = false;

static bool EnsureMinidumpDirectory() {
    if (mkdir("minidumps", 0755) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat info = {};
    return stat("minidumps", &info) == 0 && S_ISDIR(info.st_mode);
}

static void WriteFd(int fd, const char* text) {
    if (fd < 0 || !text) return;
    const size_t length = std::strlen(text);
    size_t offset = 0;
    while (offset < length) {
        const ssize_t written = write(fd, text + offset, length - offset);
        if (written <= 0) break;
        offset += static_cast<size_t>(written);
    }
}

static void AppendFileToFd(int output, const char* path) {
    const int input = open(path, O_RDONLY);
    if (input < 0) return;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = read(input, buffer, sizeof(buffer))) > 0) {
        size_t offset = 0;
        while (offset < static_cast<size_t>(count)) {
            const ssize_t written = write(output, buffer + offset,
                                          static_cast<size_t>(count) - offset);
            if (written <= 0) break;
            offset += static_cast<size_t>(written);
        }
    }
    close(input);
}

static const char* SignalName(int signalNumber) {
    switch (signalNumber) {
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGSEGV: return "SIGSEGV";
        default:      return "unknown signal";
    }
}

static void NotifyUser() {
    char message[1024];
    std::snprintf(message, sizeof(message),
                  "ReChan crashed and created %s.\n\n"
                  "Please open an issue and attach that report:\n%s",
                  s_reportPath, kIssueUrl);
    WriteFd(STDERR_FILENO, "\n");
    WriteFd(STDERR_FILENO, message);
    WriteFd(STDERR_FILENO, "\n");

#if defined(RC_PLATFORM_NULL)
    // Headless/null-hardware build: no display server to show a dialog on.
    // The stderr write above is the only notification.
#else
    if (s_suppressDialog) return;

    const pid_t child = fork();
    if (child == 0) {
        execlp("zenity", "zenity", "--error", "--title=ReChan crashed",
               "--width=520", "--text", message, static_cast<char*>(nullptr));
        execlp("xmessage", "xmessage", "-center", message, static_cast<char*>(nullptr));
        _exit(127);
    }
#endif
}

static void FatalSignalHandler(int signalNumber, siginfo_t* signalInfo, void*) {
    if (s_handlingCrash) {
        _exit(128 + signalNumber);
    }
    s_handlingCrash = 1;

    // Create the minidumps directory only now, at crash time, so nothing is
    // written to the filesystem on a clean run.
    const bool hasDirectory = EnsureMinidumpDirectory();
    std::snprintf(s_reportPath, sizeof(s_reportPath),
                  hasDirectory ? "minidumps/rechan-crash-%ld.txt" : "rechan-crash-%ld.txt",
                  static_cast<long>(getpid()));

    const int report = open(s_reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (report >= 0) {
        char buffer[2048];
        std::snprintf(buffer, sizeof(buffer),
                      "ReChan crash report\n"
                      "===================\n"
                      "Version: %s\n"
                      "Platform: Linux x86_64\n"
                      "Process ID: %ld\n"
                      "Signal: %s (%d)\n"
                      "Signal code: %d\n"
                      "Fault address: %p\n\n"
                      "Native stack trace:\n",
                      GAME_VERSION, static_cast<long>(getpid()), SignalName(signalNumber),
                      signalNumber, signalInfo ? signalInfo->si_code : 0,
                      signalInfo ? signalInfo->si_addr : nullptr);
        WriteFd(report, buffer);

        void* frames[64] = {};
        const int frameCount = backtrace(frames, 64);
        backtrace_symbols_fd(frames, frameCount, report);

        WriteFd(report, "\nProcess memory map:\n");
        AppendFileToFd(report, "/proc/self/maps");
        WriteFd(report, "\nOperating system:\n");
        AppendFileToFd(report, "/etc/os-release");
        WriteFd(report,
                "\nPlease open an issue at:\n"
                "https://github.com/SilverwireGames/ReChan/issues/new\n"
                "Attach this crash report.\n");
        WriteFd(report,
                "\nrechan.log\n"
                "==========\n");
        AppendFileToFd(report, "rechan.log");
        fsync(report);
        close(report);
    }

    NotifyUser();

    struct sigaction action = {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(signalNumber, &action, nullptr);

    sigset_t unblockSet;
    sigemptyset(&unblockSet);
    sigaddset(&unblockSet, signalNumber);
    sigprocmask(SIG_UNBLOCK, &unblockSet, nullptr);
    kill(getpid(), signalNumber);
    _exit(128 + signalNumber);
}

[[noreturn]] static void TerminateHandler() {
    raise(SIGABRT);
    _exit(134);
}

void CrashReporter::ShowFatalMessage(const char* title, const char* message) {
    std::fprintf(stderr, "%s: %s\n", title, message);
#if !defined(RC_PLATFORM_NULL)
    if (std::getenv("RECHAN_CRASH_REPORTER_NO_DIALOG")) return;

    const pid_t child = fork();
    if (child == 0) {
        execlp("zenity", "zenity", "--error", "--title", title,
               "--width=520", "--text", message, static_cast<char*>(nullptr));
        execlp("xmessage", "xmessage", "-center", message, static_cast<char*>(nullptr));
        _exit(127);
    }
#endif
}

void CrashReporter::Install() {
    s_suppressDialog = std::getenv("RECHAN_CRASH_REPORTER_NO_DIALOG") != nullptr;

    struct sigaction action = {};
    action.sa_sigaction = FatalSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;

    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGSEGV, &action, nullptr);
    std::set_terminate(TerminateHandler);
}

#endif

[[noreturn]] void CrashReporter::TriggerTestCrash() {
    std::raise(SIGABRT);
    std::abort();
}

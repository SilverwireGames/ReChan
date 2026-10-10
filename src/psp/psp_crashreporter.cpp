#include "pc/crashreporter.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <malloc.h>
#include <new>

// The PSP has no crash dialog: an out-of-memory `new`, std::terminate or abort()
// simply returns to the XMB. Record why in CRASH.TXT (next to the game data, the
// working directory after startup) before that happens. Everything here avoids
// the heap, which is usually what just ran out.
namespace {

void AppendText(char*& out, const char* end, const char* text) {
    while (*text && out < end) *out++ = *text++;
}

void AppendHex(char*& out, const char* end, unsigned long value) {
    AppendText(out, end, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        if (out >= end) return;
        const unsigned digit = static_cast<unsigned>((value >> shift) & 0xFu);
        *out++ = static_cast<char>(digit < 10u ? '0' + digit : 'a' + (digit - 10u));
    }
}

void RecordFailure(const char* what, std::size_t size, const void* caller) {
    char line[192];
    char* out = line;
    const char* end = line + sizeof(line) - 1;
    AppendText(out, end, what);
    AppendText(out, end, " size=");
    AppendHex(out, end, static_cast<unsigned long>(size));
    AppendText(out, end, " caller=");
    AppendHex(out, end, reinterpret_cast<unsigned long>(caller));
    const struct mallinfo heap = mallinfo();
    AppendText(out, end, " arena=");
    AppendHex(out, end, static_cast<unsigned long>(heap.arena));
    AppendText(out, end, " used=");
    AppendHex(out, end, static_cast<unsigned long>(heap.uordblks));
    AppendText(out, end, " free=");
    AppendHex(out, end, static_cast<unsigned long>(heap.fordblks));
    AppendText(out, end, " kfree=");
    AppendHex(out, end, static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
    *out++ = '\n';

    const SceUID fd = sceIoOpen("CRASH.TXT", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, static_cast<SceSize>(out - line));
        sceIoClose(fd);
    }
}

void OnTerminate() {
    RecordFailure("terminate", 0, __builtin_return_address(0));
    sceKernelExitGame();
    for (;;) sceKernelDelayThread(1000000);
}

void OnAbortSignal(int) {
    RecordFailure("abort", 0, __builtin_return_address(0));
}

void* AllocateOrRecord(std::size_t size, const void* caller) {
    void* block = std::malloc(size ? size : 1);
    if (!block) {
        RecordFailure("new_failed", size, caller);
        std::abort();
    }
    return block;
}

} // namespace

// Global allocation goes through malloc as libstdc++ does, but a failure now
// leaves the requested size and the calling address in CRASH.TXT.
void* operator new(std::size_t size) { return AllocateOrRecord(size, __builtin_return_address(0)); }
void* operator new[](std::size_t size) { return AllocateOrRecord(size, __builtin_return_address(0)); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return std::malloc(size ? size : 1); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return std::malloc(size ? size : 1); }
void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }

namespace CrashReporter {
void Install() {
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbortSignal);
}

[[noreturn]] void TriggerTestCrash() {
    sceKernelExitGame();
    for (;;) sceKernelDelayThread(1000000);
}
}

#include "psp/psp_funcprof.h"

#if defined(PSP_FUNC_PROFILE)

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// This file is never built with -finstrument-functions (psp/Makefile only
// instruments the game directories); the attribute also keeps the hooks safe
// if that ever changes.
#define PSP_NO_INSTRUMENT __attribute__((no_instrument_function))

namespace {

constexpr unsigned kMaxDepth = 128;
constexpr unsigned kTableBits = 11;
constexpr unsigned kTableSize = 1u << kTableBits;
// 2 kHz: ~40k samples over a 600-frame benchmark window.
constexpr unsigned kSamplePeriodUs = 500;

struct Entry {
    const void* fn;
    unsigned self;
    unsigned incl;
};

const void* volatile g_stack[kMaxDepth];
volatile unsigned g_depth = 0;
// Main-thread stack range; size 0 (before Start) makes every hook return.
std::uintptr_t g_stackLow = 0;
std::uintptr_t g_stackSize = 0;
Entry g_table[kTableSize];
volatile bool g_sampling = false;
unsigned g_samples = 0;
unsigned g_emptySamples = 0;
unsigned g_lostSamples = 0;
unsigned g_overflowSamples = 0;
SceUID g_timer = -1;

PSP_NO_INSTRUMENT inline bool OnMainStack() {
    const auto sp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    return sp - g_stackLow < g_stackSize;
}

PSP_NO_INSTRUMENT Entry* Lookup(const void* fn) {
    const unsigned key = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(fn)) >> 2;
    const unsigned hash = (key * 2654435761u) >> (32u - kTableBits);
    for (unsigned probe = 0; probe < kTableSize; ++probe) {
        Entry& entry = g_table[(hash + probe) & (kTableSize - 1u)];
        if (entry.fn == fn) return &entry;
        if (!entry.fn) {
            entry.fn = fn;
            return &entry;
        }
    }
    return nullptr;
}

// VTimer handler (interrupt context): charge the sampled main-thread stack.
PSP_NO_INSTRUMENT SceUInt OnSample(SceUID, SceKernelSysClock*, SceKernelSysClock*, void*) {
    if (!g_sampling) return kSamplePeriodUs;
    ++g_samples;
    unsigned depth = g_depth;
    if (depth == 0) {
        ++g_emptySamples;
        return kSamplePeriodUs;
    }
    if (depth > kMaxDepth) {
        ++g_overflowSamples;
        depth = kMaxDepth;
    }
    Entry* top = Lookup(g_stack[depth - 1u]);
    if (!top) {
        ++g_lostSamples;
        return kSamplePeriodUs;
    }
    ++top->self;
    for (unsigned i = 0; i < depth; ++i) {
        const void* fn = g_stack[i];
        bool seen = false;
        for (unsigned j = 0; j < i && !seen; ++j) seen = g_stack[j] == fn;
        if (seen) continue;  // recursion: count a function once per sample
        if (Entry* entry = Lookup(fn)) ++entry->incl;
    }
    return kSamplePeriodUs;
}

} // namespace

extern "C" PSP_NO_INSTRUMENT void __cyg_profile_func_enter(void* fn, void*) {
    if (!OnMainStack()) return;
    const unsigned depth = g_depth;
    if (depth < kMaxDepth) g_stack[depth] = fn;
    g_depth = depth + 1u;  // after the slot write: the sampler reads depth first
}

extern "C" PSP_NO_INSTRUMENT void __cyg_profile_func_exit(void*, void*) {
    if (!OnMainStack()) return;
    const unsigned depth = g_depth;
    // Frames entered before Start() were never pushed; their exits stop at 0.
    if (depth) g_depth = depth - 1u;
}

namespace PspFuncProfile {

PSP_NO_INSTRUMENT void Start() {
    g_sampling = false;
    if (g_stackSize == 0) {
        SceKernelThreadInfo info;
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(sceKernelGetThreadId(), &info) < 0) return;
        g_depth = 0;
        g_stackLow = reinterpret_cast<std::uintptr_t>(info.stack);
        g_stackSize = static_cast<std::uintptr_t>(info.stackSize);
    }
    std::memset(g_table, 0, sizeof(g_table));
    g_samples = g_emptySamples = g_lostSamples = g_overflowSamples = 0;
    if (g_timer < 0) {
        g_timer = sceKernelCreateVTimer("rechan_prof", nullptr);
        if (g_timer < 0) return;
        SceKernelSysClock first = {};
        first.low = kSamplePeriodUs;
        sceKernelSetVTimerHandler(g_timer, &first, OnSample, nullptr);
        sceKernelStartVTimer(g_timer);
    }
    g_sampling = true;
}

PSP_NO_INSTRUMENT void StopAndWrite(const char* path) {
    g_sampling = false;
    // Sampling is off, so the hash layout is no longer needed: sort in place.
    std::sort(g_table, g_table + kTableSize, [](const Entry& lhs, const Entry& rhs) {
        if (lhs.incl != rhs.incl) return lhs.incl > rhs.incl;
        return lhs.self > rhs.self;
    });
    const SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    static char buffer[4096];
    std::size_t used = 0;
    const auto flush = [&]() {
        if (used) sceIoWrite(fd, buffer, static_cast<SceSize>(used));
        used = 0;
    };
    used = static_cast<std::size_t>(std::snprintf(buffer, sizeof(buffer),
        "ref=0x%08lx\nsamples=%u\nempty=%u\nlost=%u\noverflow=%u\nperiod_us=%u\n",
        static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(&Start)),
        g_samples, g_emptySamples, g_lostSamples, g_overflowSamples, kSamplePeriodUs));
    const auto ref = static_cast<long>(reinterpret_cast<std::uintptr_t>(&Start));
    for (const Entry& entry : g_table) {
        if (!entry.fn || (entry.incl == 0 && entry.self == 0)) continue;
        if (sizeof(buffer) - used < 48u) flush();
        const int length = std::snprintf(buffer + used, sizeof(buffer) - used, "%ld %u %u\n",
            static_cast<long>(reinterpret_cast<std::uintptr_t>(entry.fn)) - ref,
            entry.self, entry.incl);
        if (length > 0) used += static_cast<std::size_t>(length);
    }
    flush();
    sceIoClose(fd);
    std::memset(g_table, 0, sizeof(g_table));
}

} // namespace PspFuncProfile

#endif

#pragma once
// Host shim: a "thread" that runs to completion inside sceKernelStartThread.
#include "pspiofilemgr.h"

#define PSP_THREAD_ATTR_USER 0x80000000

typedef int (*SceKernelThreadEntry)(SceSize args, void* argp);

inline SceKernelThreadEntry g_testThreadEntry = nullptr;

inline SceUID sceKernelCreateThread(const char*, SceKernelThreadEntry entry, int, int, unsigned int, void*) {
    g_testThreadEntry = entry;
    return 1;
}

inline int sceKernelStartThread(SceUID, SceSize args, void* argp) {
    return g_testThreadEntry(args, argp);
}

inline int sceKernelExitDeleteThread(int) { return 0; }

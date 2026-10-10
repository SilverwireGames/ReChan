#pragma once
#include <cstdint>
using SceUID = int;
using SceSize = unsigned int;
using SceUInt = unsigned int;
template<class... T> inline int sceKernelCreateThread(T...) { return -1; }
template<class... T> inline int sceKernelCreateSema(T...) { return -1; }
template<class... T> inline int sceKernelWaitSema(T...) { return -1; }
#define AUDIO_KERNEL_STUB(name) template<class... T> inline int name(T...) { return 0; }
AUDIO_KERNEL_STUB(sceKernelExitThread)
AUDIO_KERNEL_STUB(sceKernelSignalSema)
AUDIO_KERNEL_STUB(sceKernelDeleteSema)
AUDIO_KERNEL_STUB(sceKernelStartThread)
AUDIO_KERNEL_STUB(sceKernelDeleteThread)
AUDIO_KERNEL_STUB(sceKernelWaitThreadEnd)
AUDIO_KERNEL_STUB(sceKernelTerminateThread)
AUDIO_KERNEL_STUB(sceKernelDelayThread)
#undef AUDIO_KERNEL_STUB
inline std::uint64_t sceKernelGetSystemTimeWide() { return 0; }

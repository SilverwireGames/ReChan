#include "pc/crashreporter.h"

#include <pspkernel.h>

namespace CrashReporter {
void Install() {}

[[noreturn]] void TriggerTestCrash() {
    sceKernelExitGame();
    for (;;) sceKernelDelayThread(1000000);
}
}

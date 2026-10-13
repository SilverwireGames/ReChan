#pragma once

namespace CrashReporter {
    // Installs process-wide fatal exception and signal handlers. Crash reports are
    // written beside the executable's working data and never uploaded automatically.
    void Install();

    // Startup failure that is not a crash.
    void ShowFatalMessage(const char* title, const char* message);

    // Intentional fatal signal used to verify packaging and report permissions.
    [[noreturn]] void TriggerTestCrash();
}

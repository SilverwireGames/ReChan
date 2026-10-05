#pragma once

namespace CrashReporter {
    // Installs process-wide fatal exception and signal handlers. Crash reports are
    // written beside the executable's working data and never uploaded automatically.
    void Install();

    // Tells the user about a startup failure that is not a crash (missing game data,
    // wrong working directory). Always goes to stderr; shows a dialog where one exists.
    void ShowFatalMessage(const char* title, const char* message);

    // Intentional fatal signal used to verify packaging and report permissions.
    [[noreturn]] void TriggerTestCrash();
}

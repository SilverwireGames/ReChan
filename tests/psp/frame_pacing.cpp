#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// Clock/sleep aren't used by the fixed-step regression below.
static void sceKernelDelayThread(unsigned int) {}
#include "../../src/gen/time.cpp"

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "Frame pacing regression: %s\n", message); std::abort(); }
}

int main() {
    Time clock;
    clock.ResetPlayPresentationState();
    int steps = 0;
    // At 50 Hz presentation, one second must still advance 30 logic ticks.
    // The old one-tick accumulator clamp instead advanced just 25 ticks.
    for (int i = 0; i < 100; ++i) {
        clock.Tick(0.020001f);
        int count = clock.BeginPlayFixedStep();
        require(count <= 1, "PSP ran multiple logic steps in one frame");
        steps += count;
    }
    require(steps == 60, "render rate slowed the 30 Hz simulation");

    clock.ResetPlayPresentationState();
    clock.Tick(0.020f); clock.BeginPlayFixedStep();
    clock.Tick(0.016f);
    require(clock.BeginPlayFixedStep() == 1, "irregular frame lost its tick");
    require(std::fabs(clock.GetPlayPresentationAlpha() - 0.08f) < 0.0001f,
        "irregular frame discarded interpolation remainder");

    clock.ResetPlayPresentationState();
    clock.Tick(0.245f);
    require(clock.BeginPlayFixedStep() == 1, "stall triggered catch-up burst");
    require(std::fabs(clock.GetPlayPresentationAlpha() - 0.35f) < 0.0001f,
        "stall lost fractional presentation phase");
    clock.Tick(0.001f);
    require(clock.BeginPlayFixedStep() == 0, "stall retained whole-step debt");

    clock.targetFPS = 30;
    clock.Tick(0.020f);
    require(clock.BeginPlayFixedStep() == 1 && clock.GetPlayPresentationAlpha() == 1,
        "30 Hz compatibility mode changed");
    std::puts("PASS: 30 Hz simulation, fractional interpolation, bounded stall recovery");
}

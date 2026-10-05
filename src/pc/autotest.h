#pragma once
#include "gen/common.h"

enum class GameState : s32;

// Scripted run for regression checks. Boots straight into a level, plays a fixed number
// of frames on a fixed timestep with optional held pad buttons, optionally writes a
// screenshot of the last frame, then exits through the normal shutdown path.
//
//   rechan --autotest <levelIndex> [--petal N] [--frames N] [--fps N]
//          [--hold up,cross,...] [--screenshot out.png]
//
// Exit code: 0 finished, 10 game data missing, 11 never reached gameplay, 12 screenshot failed.
namespace AutoTest {
    void ParseArgs(int argc, char** argv);
    bool IsEnabled();

    s32 GetLevelIndex();
    s32 GetPetalIndex();
    bool TraceSpawns();
    s32 GetFps();
    f32 GetFixedDeltaTime();
    u32 GetHeldButtons();

    void Fail(int exitCode, const char* reason);

    // Call once per main-loop iteration. Returns false when the run is over.
    bool Step(GameState state);
    int GetExitCode();
}

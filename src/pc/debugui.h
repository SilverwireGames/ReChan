#pragma once

class Camera;
class Game;

namespace DebugUI {
    void Init();
    void Draw();
#if defined(RC_PLATFORM_PSP)
    void BeginBenchmarkFrame(Game* game);
    void UpdateBenchmarkCamera(Camera* camera);
    void OnBenchmarkFrameRecorded();
    bool IsBenchmarkFinished();
    bool QueueNextBenchmarkScenario(Game* game);
    // PSP_MENU_QA: walks the menu pages from the title screen, capturing each.
    void UpdateMenuQa(Game* game);
    // PSP_MOVIE_QA: plays the movies on the Memory Stick from the title
    // screen, each writing a MOVIE_<name>.TXT report.
    void UpdateMovieQa(Game* game);
#endif
    bool IsEnabled();
    bool IsHumanoidDamageDisabled();
    bool ShouldBlockGameInput();
    bool IsPlayerInputAllowed();
    bool IsDebugCameraInputAllowed();
}

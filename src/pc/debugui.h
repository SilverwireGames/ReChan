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
#endif
    bool IsEnabled();
    bool IsHumanoidDamageDisabled();
    bool ShouldBlockGameInput();
    bool IsPlayerInputAllowed();
    bool IsDebugCameraInputAllowed();
}

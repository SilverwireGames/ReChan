#include "pc/debugui.h"

namespace DebugUI {
void Init() {}
void Draw() {}
bool IsEnabled() { return false; }
bool IsHumanoidDamageDisabled() { return false; }
bool ShouldBlockGameInput() { return false; }
bool IsPlayerInputAllowed() { return true; }
bool IsDebugCameraInputAllowed() { return false; }
}

#include "pc/inputaction.h"
#include "p3d/input.h"
#include "pddi/pddidev.h"
#include "gen/control.h"

static const char* kActionTokens[ACTION_COUNT] = {
    "JUMP",
    "PUNCH",
    "KICK",
    "GRAB",
    "DIVE_ROLL",
    "STRAFE",
    "COUNTER",
    "STATUS_DISPLAY",
    "MOVE_UP",
    "MOVE_DOWN",
    "MOVE_LEFT",
    "MOVE_RIGHT",
    "OPEN_CLOSE_MENU",
    "MENU_UP",
    "MENU_DOWN",
    "MENU_LEFT",
    "MENU_RIGHT",
    "MENU_CONFIRM",
    "MENU_BACK",
    "MENU_CLEAR",
};

static constexpr s32 kDesktopBindingSlotCount = 2;

#if MENU_BACK_USES_CIRCLE
static constexpr s32 kDefaultMenuBackGamepadButton = GpBtn::B;
#else
static constexpr s32 kDefaultMenuBackGamepadButton = GpBtn::Y;
#endif

#if defined(RC_PLATFORM_SWITCH)
static constexpr s32 kJumpGamepadButton = GpBtn::B;
static constexpr s32 kPunchGamepadButton = GpBtn::Y;
static constexpr s32 kKickGamepadButton = GpBtn::X;
static constexpr s32 kGrabGamepadButton = GpBtn::A;
#else
static constexpr s32 kJumpGamepadButton = GpBtn::A;
static constexpr s32 kPunchGamepadButton = GpBtn::X;
static constexpr s32 kKickGamepadButton = GpBtn::Y;
static constexpr s32 kGrabGamepadButton = GpBtn::B;
#endif


#if !defined(RC_PLATFORM_SWITCH) && !defined(RC_PLATFORM_PSP)
static const s32 kBindableKeys[] = {
    KEY_SPACE,
    KEY_APOSTROPHE,
    KEY_COMMA,
    KEY_MINUS,
    KEY_PERIOD,
    KEY_SLASH,
    KEY_0,
    KEY_1,
    KEY_2,
    KEY_3,
    KEY_4,
    KEY_5,
    KEY_6,
    KEY_7,
    KEY_8,
    KEY_9,
    KEY_SEMICOLON,
    KEY_EQUAL,
    KEY_A,
    KEY_B,
    KEY_C,
    KEY_D,
    KEY_E,
    KEY_F,
    KEY_G,
    KEY_H,
    KEY_I,
    KEY_J,
    KEY_K,
    KEY_L,
    KEY_M,
    KEY_N,
    KEY_O,
    KEY_P,
    KEY_Q,
    KEY_R,
    KEY_S,
    KEY_T,
    KEY_U,
    KEY_V,
    KEY_W,
    KEY_X,
    KEY_Y,
    KEY_Z,
    KEY_LEFT_BRACKET,
    KEY_BACKSLASH,
    KEY_RIGHT_BRACKET,
    KEY_GRAVE,
    KEY_ESCAPE,
    KEY_ENTER,
    KEY_TAB,
    KEY_BACKSPACE,
    KEY_INSERT,
    KEY_DELETE,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_DOWN,
    KEY_UP,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_CAPS_LOCK,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_LEFT_SHIFT,
    KEY_LEFT_CONTROL,
    KEY_LEFT_ALT,
    KEY_RIGHT_SHIFT,
    KEY_RIGHT_CONTROL,
    KEY_RIGHT_ALT,
};
#endif // !RC_PLATFORM_SWITCH && !RC_PLATFORM_PSP

static bool IsValidDesktopSlot(s32 slot) {
    return slot >= 0 && slot < kDesktopBindingSlotCount;
}

static u8 AxisToPadByte(s32 axis) {
    s32 value = 128 + axis;
    if (value < 0) {
        value = 0;
    }
    else if (value > 255) {
        value = 255;
    }
    return (u8)value;
}

static const char* MouseButtonToLabel(s32 button) {
    switch (button) {
        case MouseBtn::Left: return "Mouse1";
        case MouseBtn::Right: return "Mouse2";
        case MouseBtn::Middle: return "Mouse3";
        default: return "";
    }
}

static void CopyLabel(char* dst, s32 dstLen, const char* src) {
    if (!dst || dstLen <= 0) {
        return;
    }

    dst[0] = '\0';
    if (!src) {
        return;
    }

    s32 i = 0;
    while (i + 1 < dstLen && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void KeyToLabel(s32 key, char* outLabel, s32 outLabelLen) {
    if (!outLabel || outLabelLen <= 0) {
        return;
    }

    outLabel[0] = '\0';

    if (key >= KEY_A && key <= KEY_Z) {
        outLabel[0] = (char)key;
        outLabel[1] = '\0';
        return;
    }

    if (key >= KEY_0 && key <= KEY_9) {
        outLabel[0] = (char)key;
        outLabel[1] = '\0';
        return;
    }

    if (key >= KEY_F1 && key <= KEY_F12) {
        const s32 fn = key - KEY_F1 + 1;
        snprintf(outLabel, outLabelLen, "F%d", fn);
        return;
    }

    switch (key) {
        case KEY_ENTER: CopyLabel(outLabel, outLabelLen, "Enter"); return;
        case KEY_ESCAPE: CopyLabel(outLabel, outLabelLen, "Esc"); return;
        case KEY_LEFT: CopyLabel(outLabel, outLabelLen, "Left"); return;
        case KEY_RIGHT: CopyLabel(outLabel, outLabelLen, "Right"); return;
        case KEY_UP: CopyLabel(outLabel, outLabelLen, "Up"); return;
        case KEY_DOWN: CopyLabel(outLabel, outLabelLen, "Down"); return;
        case KEY_SPACE: CopyLabel(outLabel, outLabelLen, "Space"); return;
        case KEY_TAB: CopyLabel(outLabel, outLabelLen, "Tab"); return;
        case KEY_BACKSPACE: CopyLabel(outLabel, outLabelLen, "Backsp"); return;
        case KEY_INSERT: CopyLabel(outLabel, outLabelLen, "Ins"); return;
        case KEY_DELETE: CopyLabel(outLabel, outLabelLen, "Del"); return;
        case KEY_HOME: CopyLabel(outLabel, outLabelLen, "Home"); return;
        case KEY_END: CopyLabel(outLabel, outLabelLen, "End"); return;
        case KEY_PAGE_UP: CopyLabel(outLabel, outLabelLen, "PgUp"); return;
        case KEY_PAGE_DOWN: CopyLabel(outLabel, outLabelLen, "PgDn"); return;
        case KEY_LEFT_SHIFT: CopyLabel(outLabel, outLabelLen, "LShift"); return;
        case KEY_RIGHT_SHIFT: CopyLabel(outLabel, outLabelLen, "RShift"); return;
        case KEY_LEFT_CONTROL: CopyLabel(outLabel, outLabelLen, "LCtrl"); return;
        case KEY_RIGHT_CONTROL: CopyLabel(outLabel, outLabelLen, "RCtrl"); return;
        case KEY_LEFT_ALT: CopyLabel(outLabel, outLabelLen, "LAlt"); return;
        case KEY_RIGHT_ALT: CopyLabel(outLabel, outLabelLen, "RAlt"); return;
        case KEY_MINUS: CopyLabel(outLabel, outLabelLen, "-"); return;
        case KEY_EQUAL: CopyLabel(outLabel, outLabelLen, "="); return;
        case KEY_LEFT_BRACKET: CopyLabel(outLabel, outLabelLen, "["); return;
        case KEY_RIGHT_BRACKET: CopyLabel(outLabel, outLabelLen, "]"); return;
        case KEY_BACKSLASH: CopyLabel(outLabel, outLabelLen, "\\"); return;
        case KEY_SEMICOLON: CopyLabel(outLabel, outLabelLen, ";"); return;
        case KEY_APOSTROPHE: CopyLabel(outLabel, outLabelLen, "'"); return;
        case KEY_COMMA: CopyLabel(outLabel, outLabelLen, ","); return;
        case KEY_PERIOD: CopyLabel(outLabel, outLabelLen, "."); return;
        case KEY_SLASH: CopyLabel(outLabel, outLabelLen, "/"); return;
        case KEY_GRAVE: CopyLabel(outLabel, outLabelLen, "`"); return;
        default:
            snprintf(outLabel, outLabelLen, "Key%d", key);
            return;
    }
}

const char* ActionToToken(Action action) {
    if (action < 0 || action >= ACTION_COUNT) {
        return nullptr;
    }
    return kActionTokens[action];
}

Action ActionFromToken(const char* token) {
    if (!token || token[0] == '\0') {
        return ACTION_COUNT;
    }

    for (s32 i = 0; i < ACTION_COUNT; i++) {
        const char* actionToken = kActionTokens[i];
        if (actionToken && strcmp(token, actionToken) == 0) {
            return (Action)i;
        }
    }

    static const char kPrefix[] = "ACTION_";
    const s32 prefixLen = (s32)(sizeof(kPrefix) - 1);
    if (strncmp(token, kPrefix, prefixLen) == 0) {
        const char* trimmed = token + prefixLen;
        for (s32 i = 0; i < ACTION_COUNT; i++) {
            const char* actionToken = kActionTokens[i];
            if (actionToken && strcmp(trimmed, actionToken) == 0) {
                return (Action)i;
            }
        }
    }

    return ACTION_COUNT;
}

ActionInput* g_actionInput = nullptr;

ActionInput::ActionInput() {
    // { keyboard slots }, { mouse slots }, gpBtn, gpBtn2, gpAxis, threshold
    bindings[ACTION_JUMP] = { { KEY_SPACE, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, kJumpGamepadButton, GpBtn::NONE, GpAxis::NONE, 0 };
    bindings[ACTION_PUNCH] = { { KEY_J, 0 }, { MouseBtn::NONE, MouseBtn::Left }, kPunchGamepadButton, GpBtn::NONE, GpAxis::NONE, 0 };
    bindings[ACTION_KICK] = { { KEY_K, 0 }, { MouseBtn::NONE, MouseBtn::Right }, kKickGamepadButton, GpBtn::NONE, GpAxis::NONE, 0 };
    bindings[ACTION_GRAB] = { { KEY_E, 0 }, { MouseBtn::NONE, MouseBtn::Middle }, kGrabGamepadButton, GpBtn::NONE, GpAxis::NONE, 0 };
    bindings[ACTION_DIVE_ROLL] = { { KEY_LEFT_CONTROL, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::RB, GpBtn::NONE, GpAxis::NONE, 0 };
    bindings[ACTION_STRAFE] = { { KEY_LEFT_SHIFT, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::NONE, GpBtn::NONE, GpAxis::RTrigger, 0.5f };
    bindings[ACTION_COUNTER] = { { KEY_C, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::LB, GpBtn::NONE, GpAxis::NONE, 0 };
    bindings[ACTION_STATUS_DISPLAY] = { { KEY_TAB, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::NONE, GpBtn::NONE, GpAxis::LTrigger, 0.5f };

    bindings[ACTION_MOVE_UP] = { { KEY_W, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::NONE, GpBtn::DpadUp, GpAxis::LeftY, -0.3f };
    bindings[ACTION_MOVE_DOWN] = { { KEY_S, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::NONE, GpBtn::DpadDown, GpAxis::LeftY, 0.3f };
    bindings[ACTION_MOVE_LEFT] = { { KEY_A, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::NONE, GpBtn::DpadLeft, GpAxis::LeftX, -0.3f };
    bindings[ACTION_MOVE_RIGHT] = { { KEY_D, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::NONE, GpBtn::DpadRight, GpAxis::LeftX, 0.3f };

    bindings[ACTION_OPEN_CLOSE_MENU] = { { KEY_ESCAPE, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::Start, GpBtn::NONE, GpAxis::NONE, 0 };

    bindings[ACTION_MENU_UP] = { { KEY_UP, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::DpadUp, GpBtn::NONE, GpAxis::LeftY, -0.5f };
    bindings[ACTION_MENU_DOWN] = { { KEY_DOWN, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::DpadDown, GpBtn::NONE, GpAxis::LeftY, 0.5f };
    bindings[ACTION_MENU_LEFT] = { { KEY_LEFT, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::DpadLeft, GpBtn::NONE, GpAxis::LeftX, -0.5f };
    bindings[ACTION_MENU_RIGHT] = { { KEY_RIGHT, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::DpadRight, GpBtn::NONE, GpAxis::LeftX, 0.5f };
    bindings[ACTION_MENU_CONFIRM] = { { KEY_ENTER, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::A, GpBtn::NONE, GpAxis::NONE, 0 };
    bindings[ACTION_MENU_BACK] = { { KEY_ESCAPE, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, kDefaultMenuBackGamepadButton, GpBtn::NONE, GpAxis::NONE, 0 };
    bindings[ACTION_MENU_CLEAR] = { { KEY_DELETE, 0 }, { MouseBtn::NONE, MouseBtn::NONE }, GpBtn::Y, GpBtn::NONE, GpAxis::NONE, 0 };
}

bool ActionInput::PollAction(Action action, PlatformInput* platform) const {
    const ActionBinding& b = bindings[action];

#if !defined(RC_PLATFORM_SWITCH) && !defined(RC_PLATFORM_PSP)
    for (s32 slot = 0; slot < kDesktopBindingSlotCount; slot++) {
        if (b.keyboardKeys[slot] && platform->IsKeyDown(b.keyboardKeys[slot])) {
            return true;
        }

        if (b.mouseButtons[slot] != MouseBtn::NONE && platform->IsMouseButtonDown(b.mouseButtons[slot])) {
            return true;
        }
    }
#endif

    // Check gamepad button
    if (b.gamepadButton != GpBtn::NONE && platform->IsGamepadButtonDown(b.gamepadButton)) {
        return true;
    }

    // Check alternate gamepad button (e.g. D-pad)
    if (b.gamepadButton2 != GpBtn::NONE && platform->IsGamepadButtonDown(b.gamepadButton2)) {
        return true;
    }

    // Check gamepad axis
    if (b.gamepadAxis != GpAxis::NONE) {
        float val = platform->GetGamepadAxis(b.gamepadAxis);
        if (b.axisThreshold > 0 && val >= b.axisThreshold) {
            return true;
        }
        if (b.axisThreshold < 0 && val <= b.axisThreshold) {
            return true;
        }
    }

    return false;
}

void ActionInput::Update(PlatformInput* platform) {
    if (!platform) {
        return;
    }

#if !defined(RC_PLATFORM_SWITCH) && !defined(RC_PLATFORM_PSP)
    for (s32 i = 0; i < 3; i++) {
        mousePrev[i] = mouseDown[i];
        mouseDown[i] = platform->IsMouseButtonDown(i);
    }
    platform->GetMousePosition(mouseX, mouseY);

    triggeredKeyThisFrame = 0;
    for (u32 i = 0; i < (u32)(sizeof(kBindableKeys) / sizeof(kBindableKeys[0])); i++) {
        const s32 key = kBindableKeys[i];
        if (platform->IsKeyTriggered(key)) {
            triggeredKeyThisFrame = key;
            break;
        }
    }

    triggeredMouseButtonThisFrame = MouseBtn::NONE;
    for (s32 i = 0; i < 3; i++) {
        if (platform->IsMouseButtonTriggered(i)) {
            triggeredMouseButtonThisFrame = i;
            break;
        }
    }

    // Ensure all bound keyboard keys are tracked for HasAnyKeyboardInput().
    if (!keysRegistered) {
        for (s32 i = 0; i < ACTION_COUNT; i++) {
            for (s32 slot = 0; slot < kDesktopBindingSlotCount; slot++) {
                if (bindings[i].keyboardKeys[slot]) {
                    platform->TrackKey(bindings[i].keyboardKeys[slot]);
                }
            }
        }
        keysRegistered = true;
    }
#endif // !RC_PLATFORM_SWITCH

    // Switch active device based on last input used.
#if defined(RC_PLATFORM_SWITCH)
    const bool hasKeyboard = false;
    const bool hasMouse = false;
#else
    const bool hasKeyboard = platform->HasAnyKeyboardInput();
    const bool hasMouse = platform->HasAnyMouseInput();
#endif
    const bool hasDesktop = hasKeyboard || hasMouse;
    bool gpConnected = platform->IsGamepadConnected();
    if (!gpConnected) {
        hadKeyboardInputThisFrame = hasKeyboard;
        hadMouseInputThisFrame = hasMouse;
        hadGamepadInputThisFrame = false;
        gamepadActive = false;
    }
    else {
        bool hasGp = platform->HasAnyGamepadInput();
        hadKeyboardInputThisFrame = hasKeyboard;
        hadMouseInputThisFrame = hasMouse;
        hadGamepadInputThisFrame = hasGp;
        if (hasGp) {
            gamepadActive = true;
        }
        if (hasDesktop) {
            gamepadActive = false;
        }
    }

    // Update all action states.
    for (s32 i = 0; i < ACTION_COUNT; i++) {
        InputState& s = states[i];
        s.prevDown = s.down;
        s.down = PollAction(static_cast<Action>(i), platform);

        if (s.down) {
            s.duration++;
        }
        else {
            s.duration = 0;
        }
    }

    // Compute analog movement axes.
    // Start with digital input from action states (works for both keyboard and D-pad).
    moveX = 0;
    moveY = 0;
    if (states[ACTION_MOVE_LEFT].down) moveX -= 127;
    if (states[ACTION_MOVE_RIGHT].down) moveX += 127;
    if (states[ACTION_MOVE_UP].down) moveY -= 127;
    if (states[ACTION_MOVE_DOWN].down) moveY += 127;

    // Analog stick overrides digital when active.
    if (gamepadActive) {
        float lx = platform->GetLeftStickX();
        float ly = platform->GetLeftStickY();
        s32 stickX = (s32)(lx * 127.0f);
        s32 stickY = (s32)(ly * 127.0f);
        if (stickX != 0 || stickY != 0) {
            moveX = stickX;
            moveY = stickY;
        }

        float rx = platform->GetRightStickX();
        float ry = platform->GetRightStickY();
        lookX = (s32)(rx * 127.0f);
        lookY = (s32)(ry * 127.0f);
    }
    else {
        lookX = 0;
        lookY = 0;
    }

    if (moveX > 127) moveX = 127;
    if (moveX < -127) moveX = -127;
    if (moveY > 127) moveY = 127;
    if (moveY < -127) moveY = -127;
}

bool ActionInput::JustPressed(Action action) const {
    if (action < 0 || action >= ACTION_COUNT) {
        return false;
    }
    const InputState& s = states[action];
    return s.down && !s.prevDown;
}

bool ActionInput::IsHeld(Action action) const {
    if (action < 0 || action >= ACTION_COUNT) {
        return false;
    }
    return states[action].down;
}

bool ActionInput::JustReleased(Action action) const {
    if (action < 0 || action >= ACTION_COUNT) {
        return false;
    }
    const InputState& s = states[action];
    return !s.down && s.prevDown;
}

s16 ActionInput::GetDuration(Action action) const {
    if (action < 0 || action >= ACTION_COUNT) {
        return 0;
    }
    return states[action].duration;
}

bool ActionInput::AnyJustPressed() const {
    for (s32 i = 0; i < ACTION_COUNT; i++) {
        if (states[i].down && !states[i].prevDown) {
            return true;
        }
    }
    return false;
}

bool ActionInput::AnyJustPressed2() const {
    for (s32 i = 0; i < ACTION_COUNT; i++) {
        if (states[i].down && !states[i].prevDown && i != ACTION_MOVE_UP && i != ACTION_MOVE_DOWN && i != ACTION_MOVE_LEFT && i != ACTION_MOVE_RIGHT && i != ACTION_MENU_UP && i != ACTION_MENU_DOWN && i != ACTION_MENU_LEFT && i != ACTION_MENU_RIGHT) {
            return true;
        }
    }
    return false;
}


void ActionInput::SetKeyBinding(Action action, int key) {
    SetKeyBindingSlot(action, 0, key);
}

void ActionInput::SetMouseButtonBinding(Action action, s32 mouseButton) {
    SetMouseButtonBindingSlot(action, 0, mouseButton);
}

void ActionInput::SetKeyBindingSlot(Action action, s32 slot, int key) {
    if (action < 0 || action >= ACTION_COUNT || !IsValidDesktopSlot(slot)) {
        return;
    }

    bindings[action].keyboardKeys[slot] = key;
    bindings[action].mouseButtons[slot] = MouseBtn::NONE;
    keysRegistered = false;
}

void ActionInput::SetMouseButtonBindingSlot(Action action, s32 slot, s32 mouseButton) {
    if (action < 0 || action >= ACTION_COUNT || !IsValidDesktopSlot(slot)) {
        return;
    }

    if (mouseButton < MouseBtn::Left || mouseButton > MouseBtn::Middle) {
        mouseButton = MouseBtn::NONE;
    }

    bindings[action].keyboardKeys[slot] = 0;
    bindings[action].mouseButtons[slot] = mouseButton;
}

void ActionInput::SetGamepadButtonBinding(Action action, s32 gpButton) {
    if (action >= 0 && action < ACTION_COUNT) {
        bindings[action].gamepadButton = gpButton;
    }
}

int ActionInput::GetKeyBinding(Action action) const {
    if (action < 0 || action >= ACTION_COUNT) {
        return 0;
    }

    const ActionBinding& b = bindings[action];
    if (b.keyboardKeys[0]) {
        return b.keyboardKeys[0];
    }
    if (b.keyboardKeys[1]) {
        return b.keyboardKeys[1];
    }
    return 0;
}

s32 ActionInput::GetMouseButtonBinding(Action action) const {
    if (action < 0 || action >= ACTION_COUNT) {
        return MouseBtn::NONE;
    }

    const ActionBinding& b = bindings[action];
    if (b.mouseButtons[0] != MouseBtn::NONE) {
        return b.mouseButtons[0];
    }
    if (b.mouseButtons[1] != MouseBtn::NONE) {
        return b.mouseButtons[1];
    }
    return MouseBtn::NONE;
}

int ActionInput::GetKeyBindingSlot(Action action, s32 slot) const {
    if (action < 0 || action >= ACTION_COUNT || !IsValidDesktopSlot(slot)) {
        return 0;
    }
    return bindings[action].keyboardKeys[slot];
}

s32 ActionInput::GetMouseButtonBindingSlot(Action action, s32 slot) const {
    if (action < 0 || action >= ACTION_COUNT || !IsValidDesktopSlot(slot)) {
        return MouseBtn::NONE;
    }
    return bindings[action].mouseButtons[slot];
}

s32 ActionInput::GetGamepadButtonBinding(Action action) const {
    if (action >= 0 && action < ACTION_COUNT) {
        return bindings[action].gamepadButton;
    }
    return GpBtn::NONE;
}

s32 ActionInput::EncodeKeyboardBindingCode(s32 key) {
    if (key <= 0) {
        return 0;
    }
    return key;
}

s32 ActionInput::EncodeMouseBindingCode(s32 mouseButton) {
    if (mouseButton < MouseBtn::Left || mouseButton > MouseBtn::Middle) {
        return 0;
    }
    return -(mouseButton + 1);
}

void ActionInput::SetDesktopBindingCode(Action action, s32 slot, s32 code) {
    if (action < 0 || action >= ACTION_COUNT || !IsValidDesktopSlot(slot)) {
        return;
    }

    if (code > 0) {
        SetKeyBindingSlot(action, slot, code);
        return;
    }

    if (code < 0) {
        const s32 mouseButton = (-code) - 1;
        if (mouseButton >= MouseBtn::Left && mouseButton <= MouseBtn::Middle) {
            SetMouseButtonBindingSlot(action, slot, mouseButton);
            return;
        }
    }

    bindings[action].keyboardKeys[slot] = 0;
    bindings[action].mouseButtons[slot] = MouseBtn::NONE;
}

s32 ActionInput::GetDesktopBindingCode(Action action, s32 slot) const {
    if (action < 0 || action >= ACTION_COUNT || !IsValidDesktopSlot(slot)) {
        return 0;
    }

    const ActionBinding& b = bindings[action];
    if (b.keyboardKeys[slot] > 0) {
        return EncodeKeyboardBindingCode(b.keyboardKeys[slot]);
    }
    if (b.mouseButtons[slot] != MouseBtn::NONE) {
        return EncodeMouseBindingCode(b.mouseButtons[slot]);
    }
    return 0;
}

void ActionInput::GetDesktopBindingLabel(Action action, s32 slot, char* outLabel, s32 outLabelLen) const {
    if (!outLabel || outLabelLen <= 0) {
        return;
    }

    outLabel[0] = '\0';

    if (action < 0 || action >= ACTION_COUNT || !IsValidDesktopSlot(slot)) {
        return;
    }

    const ActionBinding& b = bindings[action];
    if (b.keyboardKeys[slot] > 0) {
        KeyToLabel(b.keyboardKeys[slot], outLabel, outLabelLen);
        return;
    }

    if (b.mouseButtons[slot] != MouseBtn::NONE) {
        CopyLabel(outLabel, outLabelLen, MouseButtonToLabel(b.mouseButtons[slot]));
        return;
    }

    CopyLabel(outLabel, outLabelLen, "-");
}

u32 ActionInput::GetPadButtons(bool menuActionsEnabled) const {
    u32 buttons = 0;

    if (IsHeld(ACTION_MOVE_UP) || (menuActionsEnabled && IsHeld(ACTION_MENU_UP))) {
        buttons |= 0x1000;
    }
    if (IsHeld(ACTION_MOVE_RIGHT) || (menuActionsEnabled && IsHeld(ACTION_MENU_RIGHT))) {
        buttons |= 0x2000;
    }
    if (IsHeld(ACTION_MOVE_DOWN) || (menuActionsEnabled && IsHeld(ACTION_MENU_DOWN))) {
        buttons |= 0x4000;
    }
    if (IsHeld(ACTION_MOVE_LEFT) || (menuActionsEnabled && IsHeld(ACTION_MENU_LEFT))) {
        buttons |= 0x8000;
    }

    if (IsHeld(ACTION_STATUS_DISPLAY)) {
        buttons |= 0x0001;
    }
    if (IsHeld(ACTION_STRAFE)) {
        buttons |= 0x0002;
    }
    if (IsHeld(ACTION_DIVE_ROLL)) {
        buttons |= 0x0008;
    }
    if (IsHeld(ACTION_COUNTER)) {
        buttons |= 0x0004;
    }
    if (IsHeld(ACTION_KICK)) {
        buttons |= 0x0010;
    }
    if (IsHeld(ACTION_GRAB)) {
        buttons |= 0x0020;
    }
    if (menuActionsEnabled && IsHeld(ACTION_MENU_BACK)) {
        buttons |= PsxPad::MenuBack;
    }
    if (IsHeld(ACTION_JUMP) || (menuActionsEnabled && IsHeld(ACTION_MENU_CONFIRM))) {
        buttons |= 0x0040;
    }
    if (IsHeld(ACTION_PUNCH)) {
        buttons |= 0x0080;
    }
    if (IsHeld(ACTION_OPEN_CLOSE_MENU)) {
        buttons |= 0x0800;
    }

    return buttons;
}

void ActionInput::GetPadAnalog(u8& lx, u8& ly, u8& rx, u8& ry) const {
    lx = AxisToPadByte(moveX);
    ly = AxisToPadByte(moveY);
    rx = AxisToPadByte(lookX);
    ry = AxisToPadByte(lookY);
}

void ActionInput::GetMousePosition(double& x, double& y) const {
    x = mouseX;
    y = mouseY;
}

bool ActionInput::IsMouseButtonDown(s32 button) const {
    if (button < 0 || button >= 3) {
        return false;
    }
    return mouseDown[button];
}

bool ActionInput::IsMouseButtonTriggered(s32 button) const {
    if (button < 0 || button >= 3) {
        return false;
    }
    return mouseDown[button] && !mousePrev[button];
}

s32 ActionInput::ConsumeScrollDelta() {
    const s32 delta = scrollDelta;
    scrollDelta = 0;
    return delta;
}

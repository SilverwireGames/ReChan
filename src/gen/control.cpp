#include "gen/control.h"
#include "config.h"
#include "pc/inputaction.h"
#include "pc/autotest.h"
#include "p3d/context.h"
#include "p3d/input.h"

// Global singleton (PSX: 0x800DD69C)
InputManager* g_inputManager = nullptr;

// PSX: 4Game_con_GameMode and 7MenuMgr_sControlMode expanded to 16 s16 entries.
static const s16 sGameControlMode[16] = {
    3, 2, 3, 3,
    3, 3, 3, 3,
    3, 1, 1, 3,
    2, 2, 2, 2,
};

static const s16 sTitleControlMode[16] = {
    3, 3, 3, 3,
    3, 3, 3, 3,
    3, 1, 1, 3,
    3, 3, 3, 3,
};

static const s16 sMenuControlMode[16] = {
    3, 3, 3, 3,
    3, 3, 3, 3,
    3, 3, 3, 3,
    3, 3, 3, 3,
};

const s16* GameControlModeArray() {
    return sGameControlMode;
}

const s16* TitleControlModeArray() {
    return sTitleControlMode;
}

const s16* MenuControlModeArray() {
    return sMenuControlMode;
}

// PSX: 12InputManager_sDefaultMapArray / 12InputManager_sPlayerMap
static const u8 sDefaultMapArray[16] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15
};

static const u8 sPlayerMap[3][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 0, 2, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 1, 2, 0, 3, 5, 6, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
};

static u32 RemapButtonsForControlMap(u32 logicalButtons, const u8* controlMap) {
    if (!controlMap) {
        return logicalButtons;
    }

    u32 physicalButtons = logicalButtons & ~0xFFFFu;
    for (u32 i = 0; i < 16; ++i) {
        const u32 logicalBit = (u32)(controlMap[i] & 0x0F);
        if ((logicalButtons & (1u << logicalBit)) != 0) {
            physicalButtons |= (1u << i);
        }
    }

    return physicalButtons;
}


// Button


// Button::Input (0x8002D8C4) = process raw bit (0 or 1)
// PSX: RawHandler__6Buttonl updates duration (frames held) and state.
void Button::Input(s32 bit) {
    MARKFUNCTION(0x8002D8C4);
    prevInput = rawInput;
    rawInput = bit;

    if (rawInput) {
        duration++;
    }
    else {
        duration = 0;
    }

    if (mode == BUTTON_MODE_ONESHOT || mode == BUTTON_MODE_REPEAT) {
        state = (rawInput && !prevInput) ? 1 : 0;
    }
    else {
        state = rawInput ? 1 : 0;
    }
}

// Button::GetState (0x8002D898) = return state based on mode
s32 Button::GetState() const {
    MARKFUNCTION(0x8002D898);
    s32 out = state;
    if (mode == BUTTON_MODE_ONESHOT || mode == BUTTON_MODE_REPEAT) {
        const_cast<Button*>(this)->state = 0;
    }
    return out;
}

// Button::SetMode (0x8002D9E8)
void Button::SetMode(s16 m) {
    MARKFUNCTION(0x8002DB40);
    mode = static_cast<ButtonMode>(m);
}


// Control


// Control::Input (0x8002DCF0) = dispatch raw button bits to individual Buttons
// PSX iterates 16 buttons, looks up controlMap[i] for physical->logical mapping,
// then calls Button::Input with the corresponding bit.
void Control::Input(u32 buttonBits) {
    MARKFUNCTION(0x8002DCF0);
    for (int i = 0; i < 16; ++i) {
        int mapped = controlMap[i];
        s32 bit = ((buttonBits & (1u << i)) != 0) ? 1 : 0;
        buttons[mapped].Input(bit);
    }
}

// Control::GetMask (0x8002DDEC) = combine 16 Button::GetState results into bitmask
u32 Control::GetMask() const {
    MARKFUNCTION(0x8002DDEC);
    u32 mask = 0;
    for (int i = 0; i < 16; ++i) {
        if (buttons[i].GetState()) {
            mask |= (1u << i);
        }
    }
    return mask;
}

// Control::SetControlMapArray = set button remapping table
void Control::SetControlMapArray(const u8* map) {
    for (int i = 0; i < 16; ++i) {
        controlMap[i] = map[i];
    }
}

void Control::ApplyCurrentModeMap() {
    MARKFUNCTION(0x8002DE88);
    if (!modeMap) {
        return;
    }

    for (int i = 0; i < 16; ++i) {
        buttons[i].SetMode(modeMap[i]);
    }
}


// InputManager


// InputManager::InputManager (0x8002DF74)
InputManager::InputManager() {
    MARKFUNCTION(0x8002DF74);
    UpdateReverseMap();
    InternalReset();
}

// InputManager::~InputManager (0x8002E048)
InputManager::~InputManager() {
    MARKFUNCTION(0x8002E048);
    // PSX: destroys Control objects, calls ~Manager
}

// InputManager::ServiceInput (0x8002E0D4)
// Core pad processing: stores raw buttons, handles analog-to-digital,
// dispatches to Control::Input for per-button processing.
// PSX reads Sony pad buffer; on PC we pass pre-built button bits.
void InputManager::ServiceInput(u32 buttons, u16 padIndex) {
    MARKFUNCTION(0x8002E0D4);
    if (padIndex > 1) return;

    Control& ctrl = controls[padIndex];

    if (ctrl.padType == 's' || ctrl.padType == 'S') {
        if (ctrl.analogLX < 0x21u) {
            buttons |= PsxPad::Left;
        }
        else if (ctrl.analogLX >= 0xE0u) {
            buttons |= PsxPad::Right;
        }

        if (ctrl.analogLY < 0x21u) {
            buttons |= PsxPad::Up;
        }
        else if (ctrl.analogLY >= 0xE0u) {
            buttons |= PsxPad::Down;
        }
    }

    // Store raw button bitmask (PSX: control+32)
    ctrl.rawButtons = buttons;

    // Mark as connected and updated (PSX: control+28, bits 0+1)
    ctrl.flags = 0x03;
    ctrl.hasConnectedPad = 1;

    // Dispatch to per-button processing
    ctrl.Input(buttons);
}

void InputManager::ServiceHostPads(const ActionInput* actionInput, bool commitNow) {
    const bool analogPad = actionInput && actionInput->UsesAnalogPad();
    u32 buttons = 0;
    u16 padType = analogPad ? 's' : 'A';
    u8 analogLX = 128;
    u8 analogLY = 128;
    u8 analogRX = 128;
    u8 analogRY = 128;

    if (actionInput) {
        const bool menuActionsEnabled = (controls[0].modeMap == sMenuControlMode);
        buttons = actionInput->GetPadButtons(menuActionsEnabled);

        // Keep desktop action bindings stable across player-config variants.
        // PSX remap semantics still apply to gamepad input paths.
        if (!analogPad) {
            buttons = RemapButtonsForControlMap(buttons, controls[0].controlMap);
        }

        if (controls[0].modeMap == sTitleControlMode) {
            buttons &= ~PsxPad::Start;
            if (actionInput->AnyJustPressed2()) {
                buttons |= PsxPad::Start;
            }
        }
    }

    if (analogPad && actionInput) {
        actionInput->GetPadAnalog(analogLX, analogLY, analogRX, analogRY);
    }

    buttons |= AutoTest_GetHeldButtons();

    sampledPressLatch[0] |= (buttons & ~sampledButtons[0]);
    sampledButtons[0] = buttons;
    sampledPadType[0] = padType;
    sampledAnalogLX[0] = analogLX;
    sampledAnalogLY[0] = analogLY;
    sampledAnalogRX[0] = analogRX;
    sampledAnalogRY[0] = analogRY;

    sampledButtons[1] = 0;
    sampledPressLatch[1] = 0;
    sampledPadType[1] = 0;
    sampledAnalogLX[1] = 128;
    sampledAnalogLY[1] = 128;
    sampledAnalogRX[1] = 128;
    sampledAnalogRY[1] = 128;

    if (commitNow) {
        CommitHostPads();
    }
}

void InputManager::CommitHostPads() {
    Control& pad0 = controls[0];
    Control& pad1 = controls[1];

    pad0.padType = sampledPadType[0];
    pad0.analogLX = sampledAnalogLX[0];
    pad0.analogLY = sampledAnalogLY[0];
    pad0.analogRX = sampledAnalogRX[0];
    pad0.analogRY = sampledAnalogRY[0];

    const u32 effectiveButtons0 = sampledButtons[0] | sampledPressLatch[0];
    ServiceInput(effectiveButtons0, 0);
    sampledPressLatch[0] = 0;

    pad1.padType = sampledPadType[1];
    pad1.rawButtons = 0;
    pad1.flags = 0;
    pad1.hasConnectedPad = 0;
    pad1.analogLX = sampledAnalogLX[1];
    pad1.analogLY = sampledAnalogLY[1];
    pad1.analogRX = sampledAnalogRX[1];
    pad1.analogRY = sampledAnalogRY[1];
    pad1.Input(0);
}

// InputManager::Step (0x8002E6E8)
// Per-frame cleanup: clears controlValFlags
void InputManager::Step() {
    MARKFUNCTION(0x8002E6E8);
    controlValFlags[0] = 0;
    controlValFlags[1] = 0;
    UpdateActuator(0);
}

// InputManager::GetControlVal (0x8002E5BC)
// Returns processed button bitmask from Control::GetMask
u32 InputManager::GetControlVal(u16 padIndex) {
    MARKFUNCTION(0x8002E5BC);
    if (padIndex > 1) return 0;

    Control& ctrl = controls[padIndex];

    // PSX checks flags: bit 1 (updated) and bit 0 (connected)
    if (!(ctrl.flags & 0x02)) return 0; // not updated this frame
    if (!(ctrl.flags & 0x01)) return 0; // not connected

    return ctrl.GetMask();
}

// InputManager::GetRawButtons
u32 InputManager::GetRawButtons(u16 padIndex) const {
    if (padIndex > 1) return 0;
    return controls[padIndex].rawButtons;
}

// PSX: GetMappedButton__C7Controlc (0x8002DF14)
// Maps PSX button bit index through reverseMap + controlMap to Button*.
Button* InputManager::GetButtonForBit(u16 padIndex, u8 bitIndex) {
    MARKFUNCTION(0x8002DF14);
    if (padIndex > 1) return &controls[0].buttons[0];
    if (bitIndex >= 16) return &controls[padIndex].buttons[0];
    Control& ctrl = controls[padIndex];
    u8 logical = reverseMap[bitIndex];
    u8 physical = ctrl.controlMap[logical & 0xF];
    return &ctrl.buttons[physical & 0xF];
}

// InputManager::GetAnalog
void InputManager::GetAnalog(u16 padIndex, u8& lx, u8& ly, u8& rx, u8& ry) const {
    if (padIndex > 1) { lx = ly = rx = ry = 128; return; }
    const Control& ctrl = controls[padIndex];
    lx = ctrl.analogLX;
    ly = ctrl.analogLY;
    rx = ctrl.analogRX;
    ry = ctrl.analogRY;
}

// InputManager::SetControlMapArray (0x8002E2F0)
void InputManager::SetControlMapArray(s16 padIndex, const u8* map) {
    MARKFUNCTION(0x8002E2F0);
    if (padIndex < 0 || padIndex > 1) return;
    controls[padIndex].SetControlMapArray(map);
}

void InputManager::SetControlModeArray(s16 padIndex, const s16* modeMap) {
    MARKFUNCTION(0x8002E33C);
    if (padIndex < 0 || padIndex > 1 || !modeMap) {
        return;
    }

    Control& ctrl = controls[padIndex];
    ctrl.modeMap = modeMap;
    ctrl.ApplyCurrentModeMap();
}

const u8* InputManager::PlayerMapArray() const {
    return sPlayerMap[playerConfig % 3];
}

const u8* InputManager::DefaultMapArray() const {
    MARKFUNCTION(0x8002E460);
    return sDefaultMapArray;
}

u8 InputManager::GetPlayerConfig() const {
    return playerConfig;
}

void InputManager::SetPlayerConfig(u8 config) {
    MARKFUNCTION(0x8002E3F0);
    playerConfig = config % 3;
    UpdateReverseMap();
}

void InputManager::UpdateReverseMap() {
    MARKFUNCTION(0x8002E414);
    const u8* map = PlayerMapArray();
    for (u8 i = 0; i < 16; ++i) {
        reverseMap[map[i]] = i;
    }
}

// InputManager::InternalReset (0x8002E550)
void InputManager::InternalReset() {
    MARKFUNCTION(0x8002E550);
    // PSX default resets map and button state, then applies mode map later.
    for (int p = 0; p < 2; ++p) {
        for (int i = 0; i < 16; ++i) {
            controls[p].buttons[i].SetMode(BUTTON_MODE_DEFAULT);
        }
        controls[p].SetControlMapArray(sDefaultMapArray);
        controls[p].modeMap = nullptr;
        controls[p].flags = 0;
        controls[p].rawButtons = 0;
        controls[p].padType = 0;
        controls[p].hasConnectedPad = 0;
        controls[p].analogLX = controls[p].analogLY = 128;
        controls[p].analogRX = controls[p].analogRY = 128;
    }
    controlValFlags[0] = controlValFlags[1] = 0;

    sampledButtons[0] = sampledButtons[1] = 0;
    sampledPressLatch[0] = sampledPressLatch[1] = 0;
    sampledPadType[0] = sampledPadType[1] = 0;
    sampledAnalogLX[0] = sampledAnalogLX[1] = 128;
    sampledAnalogLY[0] = sampledAnalogLY[1] = 128;
    sampledAnalogRX[0] = sampledAnalogRX[1] = 128;
    sampledAnalogRY[0] = sampledAnalogRY[1] = 128;
}

// PSX: gp+144 - shock enabled flag (0 on PC, no DualShock)
static s32 g_shockEnabled = 0;
static s32 g_actuatorFramesRemaining = 0;
static u8 g_actuatorMotor = 0;
static u8 g_actuatorSpeed = 0;
static s32 g_actuatorSkipNextUpdate = 0;

static bool ApplyActuatorVibration(u8 motor, u8 speed) {
    if (!p3d::input) {
        return false;
    }
    
    if (!g_actionInput->IsGamepadActive())
        return false;

    f32 amplitude = (f32)speed / 255.0f;
    f32 low = 0.0f;
    f32 high = 0.0f;

    if (motor == 0) {
        low = amplitude;
    }
    else {
        high = amplitude;
    }

    return p3d::input->SetGamepadVibration(low, high);
}

s32 GetShock() {
    return g_shockEnabled;
}

s32 IsDualShock() {
    return PadGetState(0) == 6 ? 1 : 0;
}

void SetShock(s32 enabled) {
    g_shockEnabled = enabled ? 1 : 0;
    if (!g_shockEnabled) {
        ClearActuator();
    }
}

// PSX: PadGetState (Sony lib) - host reports state 6 when rumble-capable pad 0 is available
s32 PadGetState(s32 port) {
    if (port != 0) {
        return 0;
    }

    if (!p3d::input) {
        return 0;
    }

    if (!p3d::input->IsGamepadConnected()) {
        return 0;
    }

    return p3d::input->IsGamepadVibrationSupported() ? 6 : 0;
}

// PSX: SetActuator (CONTROL.CPP:250, 0x8002D540)
void SetActuator(u8 motor, u8 speed, u32 duration) {
    g_actuatorMotor = motor;
    g_actuatorSpeed = speed;
    g_actuatorFramesRemaining = (s32)duration;

    if (g_actuatorFramesRemaining <= 0 || g_actuatorSpeed == 0) {
        ClearActuator();
        return;
    }

    g_actuatorSkipNextUpdate = 1;
    (void)ApplyActuatorVibration(g_actuatorMotor, g_actuatorSpeed);
}

// PSX: ClearActuator (CONTROL.CPP:242, 0x8002D52C)
void ClearActuator() {
    g_actuatorFramesRemaining = 0;
    g_actuatorMotor = 0;
    g_actuatorSpeed = 0;
    g_actuatorSkipNextUpdate = 0;

    if (p3d::input) {
        (void)p3d::input->SetGamepadVibration(0.0f, 0.0f);
    }
}

// PSX: UpdateActuator (CONTROL.CPP:262, 0x8002D564)
void UpdateActuator(s32 /*param*/) {
    if (g_actuatorSkipNextUpdate) {
        g_actuatorSkipNextUpdate = 0;
        return;
    }

    if (g_actuatorFramesRemaining <= 0) {
        return;
    }

    g_actuatorFramesRemaining--;
    if (g_actuatorFramesRemaining <= 0) {
        ClearActuator();
    }
}

// PSX: Shock (CONTROL.CPP, 0x8002D6C0)
void Shock(ShockEnum type) {
    MARKFUNCTION(0x8002D6C0);

    if (type == SHOCK_CLEAR) {
        ClearActuator();
        UpdateActuator(0);
        return;
    }

    if (!g_shockEnabled) {
        return;
    }
    if (PadGetState(0) != 6) {
        return;
    }
    u8 motor;
    u8 speed;
    u32 duration;
    switch (type) {
        case SHOCK_0:  motor = 0; speed = 128; duration = 15; break;
        case SHOCK_1:  motor = 0; speed = 96; duration = 15; break;
        case SHOCK_2:  motor = 0; speed = 96; duration = 15; break;
        case SHOCK_3:  motor = 0; speed = 128; duration = 15; break;
        case SHOCK_4:  motor = 0; speed = 128; duration = 15; break;
        case SHOCK_5:  motor = 0; speed = 128; duration = 15; break;
        case SHOCK_6:  motor = 0; speed = 188; duration = 15; break;
        case SHOCK_7:  motor = 0; speed = 128; duration = 15; break;
        case SHOCK_8:  motor = 0; speed = 255; duration = 15; break;
        case SHOCK_9:  motor = 0; speed = 255; duration = 15; break;
        case SHOCK_10: motor = 0; speed = 128; duration = 15; break;
        case SHOCK_11: motor = 0; speed = 128; duration = 15; break;
        case SHOCK_12: motor = 0; speed = 188; duration = 15; break;
        case SHOCK_13: motor = 0; speed = 128; duration = 10; break;
        case SHOCK_14: motor = 0; speed = 96; duration = 15; break;
        case SHOCK_15: motor = 0; speed = 255; duration = 15; break;
        case SHOCK_16: motor = 0; speed = 255; duration = 20; break;
        case SHOCK_17: motor = 0; speed = 255; duration = 20; break;
        default:
            motor = 1; speed = 0; duration = 15; break;
    }
    SetActuator(motor, speed, duration);
    UpdateActuator(0);
}

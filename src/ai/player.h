#pragma once
#include "ai/humanoid.h"
#include "gen/scoremgr.h"

// All animation IDs routed through PlayerModel::SetAnim or referenced by Player handlers.
// IDs named ANIM_ID_X are placeholders for anims not yet identified.
enum PlayerAnimEnum : s32 {
    PLAYER_ANIM_INACTIVE_IDLE = 1,
    PLAYER_ANIM_RUN = 2,
    PLAYER_ANIM_ID_3 = 3,
    PLAYER_ANIM_STUN = 4,
    PLAYER_ANIM_ID_17 = 17,
    PLAYER_ANIM_ID_21 = 21,
    PLAYER_ANIM_IDLE_UNARMED = 22,
    PLAYER_ANIM_STRAFE = 24,
    PLAYER_ANIM_FORWARD_ROLL = 27,
    PLAYER_ANIM_ID_29 = 29,
    PLAYER_ANIM_LEDGE_PULLUP = 30,
    PLAYER_ANIM_LEDGE_LATCH = 31,
    PLAYER_ANIM_WALL_JUMP_START = 32,
    PLAYER_ANIM_WALL_JUMP_LAUNCH = 33,
    PLAYER_ANIM_ID_34 = 34,
    PLAYER_ANIM_ID_35 = 35,
    PLAYER_ANIM_ID_36 = 36,
    PLAYER_ANIM_ID_37 = 37,
    PLAYER_ANIM_ID_38 = 38,
    PLAYER_ANIM_FALL = 39,
    PLAYER_ANIM_HARD_FALL = 40,
    PLAYER_ANIM_ID_41 = 41,
    PLAYER_ANIM_ID_42 = 42,
    PLAYER_ANIM_DIVE_ROLL_TURN = 46,
    PLAYER_ANIM_TAUNT_IDLE = 47,
    PLAYER_ANIM_ID_48 = 48,
    PLAYER_ANIM_STRAFE_FORWARD_BACKWARD = 51,
    PLAYER_ANIM_STRAFE_LEFT_RIGHT = 52,
    PLAYER_ANIM_TABLE_ROLL = 86,
    PLAYER_ANIM_TABLE_ROLL_END = 87,
    PLAYER_ANIM_ID_152 = 152,
    PLAYER_ANIM_ID_189 = 189,
    PLAYER_ANIM_ID_206 = 206,
    PLAYER_ANIM_ID_220 = 220,
    PLAYER_ANIM_ID_231 = 231,
    PLAYER_ANIM_ID_244 = 244,
    PLAYER_ANIM_ID_254 = 254,
    PLAYER_ANIM_ID_261 = 261,
    PLAYER_ANIM_ID_281 = 281,
    PLAYER_ANIM_ID_282 = 282,
    PLAYER_ANIM_POLE_SWING_BACK = 285,
    PLAYER_ANIM_POLE_SWING_FWD = 286,
    PLAYER_ANIM_FORWARD_FLIP = 287,
    PLAYER_ANIM_TURNING_FLIP = 288,
    PLAYER_ANIM_FLIP_VARIANT = 295,
    PLAYER_ANIM_ID_296 = 296,
};

// Player flags (s32 playerFlags bitmask)
enum PlayerFlags : s32 {
    PF_COMBAT_READY = 0x04,  // bit 2: combat-ready
};

// Player - the player character (Jackie Chan)
class Player : public Humanoid {
public:
    // PSX +616 (s32): combat state
    s32 field616 = 0;
    // PSX +620 (s32): combat state
    s32 field620 = 0;

    // PSX +624 (u16): hit combo counter (incremented by SignalEnemyGetUp)
    u16 hitCombo = 0;
    // PSX +628 (s32): combo timer in frames (max 1800)
    s32 comboTimer = 0;

    // PSX +632 (u8): encounter state byte (1=active, 2=enemy dead)
    u8 encounterState = 0;

    // PSX +636..+688: embedded CheckpointInfo (56 bytes)
    CheckpointInfo checkpoint = {};

    // PSX +692 (s32): current lives left
    s32 livesLeft = 4;
    static constexpr s32 kMaxLives = 4;

    // PSX +696 (s32): player flags (bit 2 = combat-ready)
    s32 playerFlags = 0;

    // PSX +700 (s32): jump phase (0=initial, 1=holding, 2=peak, 3=landing)
    s32 field700 = 0;
    // PSX +704 (u16): jump hold button flag
    u16 field704 = 0;
    // PSX +706 (u16): jump tap completion flag
    u16 field706 = 0;
    // PSX +708 (u16): weapon dialog ID (set from global data)
    u16 weaponDialogID = 0;

    // PSX +712 (ptr): jump parameter table pointer (standingJumpHold/Tap, runJumpHold/Tap)
    const s32* field712 = nullptr;
    // PSX +716 (s32): jump return height (set to homePos.y in DoJump)
    s32 jumpReturnHeight = 0;
    // PSX +720 (s32): reserved
    s32 field720 = 0;
    // PSX +724 (s32): reserved
    s32 field724 = 0;
    // PSX +728 (s32): reserved
    s32 field728 = 0;

    // PSX +732 (u16): cleared at SetActionState entry
    u16 actionStateFlag = 0;

    // PSX +736,+740,+744: last position (copied from orientation on Reset)
    LVector lastPos = {};

    // PSX +748 (s32): CharMgrCallback first field (animation callback data)
    s32 animCallbackData = 0;
    // PSX +752 (ptr): CharMgrCallback vtable
    void* animCallbackVtable = nullptr;
    // PSX +756 (s32): current animation enum
    s32 currentAnimEnum = 0;
    // PSX +760 (s32): animation load state (set to 2 during loads)
    s32 animLoadState = 0;

    // PSX gp-relative globals (player-specific, one instance)
    // gp+392: running force accumulator (ramps up while running)
    s32 forceAccum = 0;
    // gp+396: runAccel (PSX .data: 0x800DCAD8 = 0x2000)
    static constexpr s32 FORCE_RAMP_RATE = 0x2000;
    // gp+400: decayRate (PSX .data: 0x800DCADC = 0x1000)
    static constexpr s32 FORCE_DECEL_RATE = 0x1000;
    // gp+420: idle animation timer (incremented each frame in _Stand)
    s32 idleTimer = 0;
    // gp+444: turn-around flag (set when angle difference in turn range)
    s32 turnAroundFlag = 0;
    // gp+476: turn delay threshold (s16, frames before turn-around completes)
    s16 turnDelayThreshold = 2;
    // gp+478: idle anim threshold (s16, frames before idle animation change)
    s16 idleAnimThreshold = 300;
    // gp+3436: stored turn target angle
    s32 turnTargetAngle = 0;
    // gp+3440: turn-around timer (s16)
    s16 turnAroundTimer = 0;

    // gp+536: encounter distance threshold for PlayerSingleEncounterCheak
    s32 encounterRange = 0;
    // gp+540: encounter check initialized flag
    s32 encounterInitialized = 0;
    // gp+3428: cached pointer to FightingCollision humanoid array
    void* encounterHumanoidArray = nullptr;

    // Debug animation override state (PC only)
    bool debugAnimOverrideActive = false;
    bool debugAnimOverridePaused = false;
    bool debugAnimOverrideApplying = false;
    s32 debugAnimOverrideEnum = -1;
    s32 debugAnimOverrideLoopType = 0;
    u32 debugModelCharType = 0; // char type used for anim lookups; 0 = Jackie

    // Global player pointer - PSX: gp+3432
    static Player* s_player;


    Player(const LVector* initialPos);
    ~Player() override;


    void Think() override;
    void Reset() override;
    void Move() override;
    void CreateModel(const char* name) override;
    void SetActionState(u32 state, s32 param) override;
    void ProcessAction() override;
    void GetViewSpot(LVector* outPos, LVector* outTarget) override;

    void _Stand() override;
    void _Run() override;
    void _Jump() override;
    void _Fall() override;
    void _Straif() override;
    void _Collapse() override;
    void _Dead() override;

    // Player-only states
    virtual void _InactiveIdle();
    virtual void _Flip();
    virtual void _HardFall();
    virtual void _HardLand();
    virtual void _Push();
    virtual void _PushObject();
    virtual void _Teetering();
    virtual void _WallJump();
    virtual void _DoStand();
    virtual void _HorizontalPoleSwing();
    virtual void _LedgeLatch();
    virtual void _LedgePullup();
    virtual void _SlopeSlide();
    virtual void _LadderDismount();
    virtual void _ClimbLadder();
    virtual void _TableRoll();

    void DoJump();
    void DoJump(s32 height);
    void DoWallJump();
    void FallingPhysics();
    void CheckForLanding();
    void OnCheckpoint();
    void SetLivesLeft(s32 lives);

    s32 GetLivesLeft() const { return livesLeft; }

    void SignalEnemyGetUp();
    void SignalEnemyDead(Humanoid* enemy);
    bool EnterCombatCombo();
    s32 LoadCombatDialog() override;
    void PlayCombatKnockDownDialog(s32 damageType) override;
    void HandleHitShock(s32 damageType) override;
    void PlayCombatThrowDialog() override;
    bool PlayerSingleEncounterCheak();
    void LoadPlayerTauntResponse(Humanoid* target);
    void PlayPlayerTauntResponse();

    // PC helpers for debug/iteration: direct animation control on player model.
    void Debug_ApplyForcedAnimation();
    bool Debug_PlayAnimation(s32 animEnum, s32 loopType);
    void Debug_PauseAnimation();
    void Debug_ResumeAnimation();
    void Debug_StopAnimation();
    bool Debug_IsAnimationOverrideActive() const;
    bool Debug_IsAnimationOverrideApplying() const;
    bool Debug_IsAnimationPaused() const;
};

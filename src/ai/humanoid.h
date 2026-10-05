#pragma once
#include "ai/thing.h"
#include "p3d/p3dmath.h"
#include "ai/behaviour.h"
#include "pc/inputaction.h"
#include "snd/hmndsnd.h"

class ActiveZone;
class Pickup;
class Obstacle;
struct FightingComboNode;
struct PsxFightingMoveRaw;
struct PsxFightingJointRaw;

// Action state IDs for Humanoid::SetActionState
// PSX: 74-case switch at 0x80065680. IDs confirmed from handler transitions.
enum ActionState : u32 {
    AS_INACTIVE_IDLE = 0,
    AS_STAND = 1,
    AS_STAND_ANIM = 2,
    AS_WALL_JUMP_TAUNT = 3,   // PSX Player case 3: wall jump taunt/idle
    AS_TAUNT_ENTRY = 4,       // PSX Humanoid case 4: taunt/provoke entry
    AS_TAUNT_PAUSE = 5,       // PSX Humanoid case 5: taunt/pause hold
    AS_PAUSE = 6,             // Humanoid: pause/guard. Player: running jump
    AS_JUMP = 8,              // standing jump (no tables)
    AS_WALL_JUMP = 9,         // PSX Player case 9: wall jump
    AS_RUN = 10,
    AS_STRAFE = 11,           // PSX case 11: strafe/targeting init path
    AS_DIVE_ROLL = 12,        // PSX case 12: dive-roll path
    AS_FALL = 13,
    AS_HARDFALL = 14,
    AS_HARDLAND = 15,
    AS_FLIP = 16,             // PSX: pole swing dismount / flip variant 295
    AS_FLIP_VARIANT = 17,     // PSX Player case 17: flip variant
    AS_POLE_IDLE = 18,        // PSX Player case 18: pole/table idle setup
    AS_PUSH_OBJECT = 19,      // PSX Player case 19: push object
    AS_SLOPE_SLIDE = 20,      // PSX Player case 20: slope slide
    AS_TABLE_ROLL = 21,       // PSX Player case 21: table roll
    AS_PUSH = 22,             // PSX Player case 22: push/contact state
    AS_LEDGE_LATCH = 23,      // PSX: ledge hang sequence
    AS_LEDGE_PULLUP = 24,     // PSX: ledge pull-up sequence
    AS_LADDER_CLIMB_DOWN = 25, // PSX 0x19: climbing down on ladder (entered from top)
    AS_LADDER_CLIMB_UP = 26,   // PSX 0x1A: climbing up on ladder (entered from bottom)
    AS_LADDER_CLIMBING = 27,   // PSX 0x1B: active ladder climbing / position adjustment
    AS_LADDER_DISMOUNT = 28,  // PSX: exit ladder (jump off)
    AS_HOTFOOT = 30,          // PSX case 30: feet-on-fire movement state
    AS_PUNCH_ATTACK = 32,
    AS_STATE_33 = 33,        // PSX state 33: referenced by pressure gates
    AS_KICK_ATTACK = 34,
    AS_STATE_35 = 35,        // PSX state 35: referenced by pressure/ledge gates
    AS_COMBAT_IDLE = 36,
    AS_BACK_GRAB_LATCH = 37,
    AS_BACK_GRAB = 38,
    AS_BACK_GRAB_RELEASE = 39,
    AS_COUNTER_ATTACK_PRE_LATCH = 40,
    AS_COUNTER_ATTACK_LATCH = 41,
    AS_COUNTER_ATTACK = 42,
    AS_COUNTER_ATTACK_RECOVERY = 43,
    AS_PICKUP = 44,           // PSX Player case 44: pick up object
    AS_THROW_PICKUP = 45,
    AS_HIT_REACT_PUNCH_A = 46, // PSX case 46: punch-react setup, got-hit-high dispatch
    AS_HIT_REACT_KICK_A = 47,  // PSX case 47: kick-react setup, got-hit-med dispatch
    AS_HIT_REACT_COMBO_A = 49, // PSX case 49: combo-react setup, got-hit-high dispatch
    AS_HIT_REACT_COMBO_B = 50, // PSX case 50: replay-current react, got-hit-med dispatch
    AS_STUN_REACT_PUNCH = 51,  // PSX case 51: punch-react setup, stunned dispatch
    AS_STUN_REACT_KICK = 52,   // PSX case 52: kick-react setup, stunned dispatch
    AS_GOT_HIT_FREEFORM = 53, // PSX case 53: got-hit freeform hang/fall
    AS_STUNNED = 55,
    AS_SPIN_BACK = 56,        // PSX case 56: spin-back knockback
    AS_FLYING_BACK = 57,      // PSX case 57: flying-back knockback
    AS_FLYING_BACK_LAND = 58,
    AS_THROW_CHARACTER_RECEIVE = 59,
    AS_BACK_GRAB_RECEIVE_PRE_LATCH = 60,
    AS_BACK_GRAB_RECEIVE_LATCH = 61,
    AS_BACK_GRAB_RECEIVE = 62,
    AS_THROW_FREE_FALL = 63,
    AS_GOT_HIT_CRUSHER = 64, // PSX case 64: crush death (dispatches via GotHitCrusher)
    AS_GOT_HIT_FIRE = 65,    // PSX case 65: fire death (dispatches via GotHitFire), not yet wired
    AS_GET_UP = 68,
    AS_COLLAPSE_STUN = 69,   // PSX case 69: drop pickups + collapse recover dispatch
    AS_FLYING_BACK_CHECK = 70,
    AS_SPIN_BACK_RECOVER = 71,
    AS_DEAD = 72,
    AS_NIS_MODE = 73,
    AS_HIT_EXPLOSION = 74,
    AS_HIT_ENVIRONMENT = 75,
    AS_MISSILE_ATTACK = 0x4E,
    AS_MISSILE_PREPARE = 0x4F,
    AS_TARGET_MISSILE_ATTACK = 0x50,
    AS_COUNT = 0x51,
};

// State dispatch indices for Humanoid::ProcessAction
// PSX: stateDispatch > 0 = vtable index, stateDispatch < 0 = direct function pointer
enum StateDispatch : u16 {
    SD_NONE = 0,
    SD_STAND = 22,
    // PSX dispatch values are class-specific vtable slots.
    // Slot 23: Humanoid -> _Taunt, Player -> _DiveRoll.
    SD_DIVE_ROLL = 23,
    SD_PAUSE = 24,
    SD_RUN = 25,
    SD_STRAFE = 26,
    // Slot 27: Humanoid -> _Straif, Player -> _DiveRoll.
    SD_DIVE_ROLL_CHAIN = 27,
    SD_JUMP = 28,
    SD_FALL = 29,
    SD_GOT_HIT_HIGH = 30,
    SD_GOT_HIT_MED = 31,
    SD_GOT_HIT_LOW = 32,
    SD_TEETER = 32,
    SD_WALLJUMP = 33,
    // PSX vtable slot 34 dispatches to _Hotfoot__8Humanoid.
    SD_COLLAPSE = 34,
    SD_DEAD = 35,
    SD_SPIN_BACK = 36,
    SD_FLYING_BACK = 37,
    SD_STUNNED = 38,
    SD_FLOATING = 274,
    SD_THROW = 39,
    // PSX vtable slot 40 (case 53) -> GotHitFreeForm.
    SD_GOT_HIT_FREEFORM = 40,
    // PSX state 70/71 dispatch target (vtable slot 41) for knockdown recovery.
    SD_COLLAPSE_RECOVER = 41,
    // PSX vtable indices 41-48 include shared and Player-specific handlers.
    SD_GET_UP = 43,   // PSX case 68: get up from knockdown
    SD_HORIZONTAL_POLE = 44,   // PSX case 18: horizontal pole swing setup/state
    SD_LEDGE_LATCH = 45,   // PSX case 23: ledge latch
    SD_LEDGE_PULLUP = 46,   // PSX case 24: ledge pull-up
    SD_SLOPE_SLIDE = 47,   // PSX case 20: slope slide
    SD_DEAD_PLAYER = 48,   // PSX case 72: player death
    SD_CLIMB_LADDER = 49,   // PSX case 27: climb ladder
    SD_LADDER_DISMOUNT = 50,   // PSX case 28: ladder dismount
    SD_NIS_MODE = 51,   // PSX case 73: NISMode
    SD_DANTE_MISSILE_ATTACK = 0x46,
    SD_DANTE_MISSILE_PREPARE = 0x47,
    SD_DANTE_TARGET_MISSILE_ATTACK = 0x48,
    // Player-specific dispatch via direct function pointer (stateDispatch = -1 on PSX)
    SD_HARDFALL = 250,
    SD_HARDLAND = 251,
    SD_FLIP = 252,
    SD_INACTIVE_IDLE = 253,
    SD_PUSH_OBJECT = 254,
    SD_TABLE_ROLL = 255,
    SD_PUSH = 256,
    SD_KILLED = 257,
    SD_DO_STAND = 258,
    SD_LADDER_LATCH_TOP = 259,
    SD_LADDER_LATCH = 260,
    SD_FIGHTING_COMBO = 261,
    SD_BACK_GRAB_LATCH = 262,
    SD_BACK_GRAB = 263,
    SD_BACK_GRAB_RELEASE = 264,
    SD_BACK_GRAB_RECEIVE_PRE_LATCH = 265,
    SD_BACK_GRAB_RECEIVE_LATCH = 266,
    SD_BACK_GRAB_RECEIVE = 267,
    SD_THROW_CHARACTER_RECEIVE = 268,
    SD_THROW_FREE_FALL = 269,
    SD_COUNTER_ATTACK_PRE_LATCH = 270,
    SD_COUNTER_ATTACK_LATCH = 271,
    SD_COUNTER_ATTACK = 272,
    SD_COUNTER_ATTACK_RECOVERY = 273,
    // PSX Butch direct-callback states from BOSS.CPP cases 74-76.
    SD_BUTCH_STOMP = 275,
    SD_BUTCH_CHARGE = 276,
    SD_BUTCH_THROW_POT = 277,
    SD_GOT_HIT_CRUSHER = 278,
};

// Humanoid - DynamicThing with combat, animation, and AI state
class Humanoid : public DynamicThing {
public:
    // PSX +200 (s32): attack joint/bone index for GetAttack (0-9, -1=none)
    s32 attackJointIndex = -1;
    // PSX +204 (s32): previous attack joint/bone index (-1=none)
    s32 prevAttackJointIndex = -1;

    // PSX +208 (s32): facing range / attack range
    s32 moveSpeed = 0;

    // PSX +212 (s32): run speed for AddForce
    s32 runSpeed = 0;

    // PSX +216 (s32): character subtype enum from HUMNDATA.CPP
    s32 characterSubType = 0;

    // PSX +220 (ptr): face angle data table
    void* faceAngleData = nullptr;

    // PSX +224 (s32): transient effect handle/signal used by stunned state flow.
    s32 animControl = 0;

    // PSX +228 (s32): reserved
    s32 field228 = 0;

    // PSX +232,+236,+240: cached spawn position from AddThingNoTagList.
    LVector spawnPos = {};

    // PSX +244,+248,+252: cached spawn orientation from AddThingNoTagList.
    LVector spawnOrientation = {};

    // PSX +256 (ptr on host): current target humanoid
    uintptr_t field256 = 0;

    // PSX +260 (u8): reserved
    u8 field260 = 0;

    // PSX +264..275: undeclared fields
    s32 field264 = 0;
    s32 field268 = 0;
    s32 field272 = 0;

    // PSX +276 (s32): desired face angle (binary angle units)
    s32 faceAngle = 0;

    // PSX +280 (s32,s32,s32): bounding box for collision
    // Initialized to {175, 0, 768} then {175, 0, 768} (two sets)
    // Note: fields at +296..+307 are reused as pole anchor position
    // during HorizontalPoleSwing (p3dFillTransMatrix reads 3 ints at +296)
    LVector collBboxMin = { 175, 0, 768 };
    LVector collBboxMax = { 175, 0, 768 };

    // PSX +304 (s32): undeclared (part of pole anchor z when on pole)
    s32 field304 = 0;

    // PSX +308 (s32): state timer (cleared on reset / state change)
    s32 stateTimer = 0;
    // PSX +312 (s32): think counter (incremented each Think)
    s32 thinkCounter = 0;

    // PSX +316..+336: combat state fields
    s32 field316 = 0;
    s32 field320 = 0;
    s32 field324 = 0;
    // PSX +328 (ptr): CHumanoidSound*
    CHumanoidSound* humanoidSound = nullptr;
    // PSX +332 (s32): sound handle
    s32 soundHandle = 0;
    // PSX +336 (s32): sound param
    s32 soundParam = 0;

    // PSX +340 (u8): combat flag
    u8 combatFlag = 0;

    // PSX +342 (u16): turn rate (2730 = ~15 degrees/frame)
    u16 turnRate = 2730;

    // PSX +344 (u16): reserved
    u16 field344 = 0;
    // PSX +346 (u16): state handler dispatch index (see StateDispatch enum)
    u16 stateDispatch = SD_STAND;

    // PSX +348 (u16): vtable pointer offset (always 8 on PSX)
    u16 field348 = 8;

    // PSX +352 (s32): input command bitfield (set by AI/controls, read by action handlers)
    // Bits: 1=guard, 2=kick, 3=punch, 4=taunt, 5=strafe, 6=strafe alternate,
    //        7=combat, 8-20=combos, 21=dive roll, 30=env hit, 31=explosion
    s32 commandBits = 0;

    // Action query helpers - read commandBits by named action.
    // Returns false if the action bit is not set or controls are disabled.
    inline bool HasAction(GameAction action) const {
        return (commandBits >> static_cast<s32>(action)) & 1;
    }

    inline bool HasGuardRelease() const { return HasAction(GA_GUARD_RELEASE); }
    inline bool HasMove() const { return HasAction(GA_MOVE); }
    inline bool HasJump() const { return HasAction(GA_JUMP); }
    inline bool HasJumpDirectional() const { return HasAction(GA_JUMP_DIRECTIONAL); }
    inline bool HasDiveRoll() const { return HasAction(GA_DIVE_ROLL); }
    inline bool HasStrafe() const { return HasAction(GA_STRAFE); }
    inline bool HasGrab() const { return HasAction(GA_GRAB); }
    inline bool HasPunch() const { return HasAction(GA_PUNCH); }
    inline bool HasKick() const { return HasAction(GA_KICK); }
    inline bool HasBackPunch() const { return HasAction(GA_BACK_PUNCH); }
    inline bool HasBackKick() const { return HasAction(GA_BACK_KICK); }
    inline bool HasHeavyPunch() const { return HasAction(GA_HEAVY_PUNCH); }
    inline bool HasHeavyKick() const { return HasAction(GA_HEAVY_KICK); }
    inline bool HasSpecialGrab() const { return HasAction(GA_SPECIAL_GRAB); }
    inline bool HasGrabForward() const { return HasAction(GA_GRAB_FORWARD); }
    inline bool HasGrabHeld() const { return HasAction(GA_GRAB_HELD); }
    inline bool HasGrabFwdHeld() const { return HasAction(GA_GRAB_FWD_HELD); }
    inline bool HasAIDiveRoll() const { return HasAction(GA_AI_DIVE_ROLL); }

    // Check if any attack bit (7-20) is set
    inline bool HasAnyAttack() const {
        return (commandBits >> GA_GRAB) & 0x3FFF;
    }

    // Check if any pickup/grab bit is set (bits 7, 15, 17, 18)
    inline bool HasAnyPickup() const {
        return ((commandBits >> GA_GRAB) & 1) ||
            ((commandBits >> GA_GRAB_FORWARD) & 1) ||
            ((commandBits >> GA_GRAB_HELD) & 1) ||
            ((commandBits >> GA_GRAB_FWD_HELD) & 1);
    }

    // PSX +356 (s32): current action state number
    s32 actionState = -1;
    // PSX +360 (s32): walk cycle flag
    s32 walkCycleFlag = 0;
    // PSX +364 (s32): reserved
    s32 field364 = 0;
    // PSX +368 (s32): cleared each Think
    s32 field368 = 0;
    // PSX +372 (s32): delta time multiplier (0x10000 = 1.0)
    s32 deltaTime = FIX16_ONE;

    // PSX +376,+378,+380 (u16): punch/kick/combo direction indices
    u16 punchDir = 0;
    u16 kickDir = 0;
    u16 comboDir = 0;

    // PSX +384..+416: targeting / combat tracking
    s32 field384 = 0;
    s32 field388 = 0;
    s32 field392 = 0;
    s32 field396 = 0;
    s32 field400 = 0;
    s32 field404 = 0;
    s32 field408 = -1;
    s32 field412 = 0;
    s32 field416 = 0;

    // PSX +420 (ptr): current HorizontalPole obstacle pointer
    Obstacle* field420 = nullptr;

    // PSX +424,+428,+432: reserved (combat state)
    s32 field424 = 0;
    s32 field428 = 0;
    s32 field432 = 0;

    // PSX +436 (s32): reserved
    s32 field436 = 0;

    // PSX +440 (ptr): Behaviour* (AI behaviour tree)
    Behaviour* behaviour = nullptr;

    // PSX +444 (s32): reserved
    s32 field444 = 0;

    // PSX +448 (s32): spawn count (set to 1)
    s32 spawnCount = 1;

    // PSX +452 (s32): reserved
    s32 field452 = 0;

    // PSX +456 (u32): behaviour name hash override from HUMNDATA.CPP
    u32 behaviourNameHash = 0;

    // PSX +460 (s32): humanoid data ptr (from GetHumanoidData)
    void* humanoidData = nullptr;

    // PSX +464 (u16): humanoid data ID
    u16 humanoidDataID = 20;

    // PSX +466,+468,+470 (s16,u16,u16): combat counters
    s16 field466 = 0;
    u16 field468 = 0;
    u16 comboCount = 1;

    // PSX +472 (s32): fighting hit latch/reset in SetCurrentFightingNode
    s32 field472 = 0;

    // PSX +476 (ptr): FightingSystem* (current)
    void* fightingSystem = nullptr;
    // PSX +480 (ptr): FightingSystem* (default)
    void* defaultFightingSystem = nullptr;

    // PSX +484,+488: reserved
    s32 field484 = 0;
    s32 field488 = 0;

    // PSX +492 (s32): distant target range (16000)
    s32 distantTargetRange = 16000;

    // PSX +496 (ptr): obstacle interaction pointer (DynamicObstacle sets this)
    Obstacle* field496 = nullptr;
    // PSX +500 (ptr): right hand held object (Pickup*)
    Thing* rightHandObj = nullptr;
    // PSX +504 (ptr): left hand held object (Pickup*)
    Thing* leftHandObj = nullptr;

    // PSX +508 (u16): from global (gp+1756)
    u16 field508 = 0;

    // PSX +512 (ptr): Trails* (visual trail effect)
    void* trails = nullptr;

    // PSX +516,+520,+524 (s32): cleared during ladder climb-up
    s32 field516 = 0;
    s32 field520 = 0;
    s32 field524 = 0;

    // PSX +528 (s32): reserved
    s32 field528 = 0;

    // PSX +532: cached owning active zone pointer
    ActiveZone* activeZone = nullptr;

    // PSX +536: embedded ccNode for combat/targeting list
    ccNode combatNode;

    Humanoid(const LVector* initialPos, u16 type);
    ~Humanoid() override;
    void Think() override;
    void Draw() override;
    void Reset() override;
    void Activate() override;
    void Deactivate() override;
    void Move() override;
    s32 SubtractHitPoints(u16 hitPoints);
    void HandleLand(s32 height) override;
    s32 HandleAnimationControl();
    void CreateModel(const char* name) override;
    void DeleteModel() override;
    void HandleCollision(Thing* other, s32 damage, ...) override;
    void HandleCollisionReactionStates(s32 hitType, s32 impactRegion);
    void HandleCollisionSound(s32 hitType);
    void AnalyzeMesh(DBRoot* root) override;

    void Teleport(const LVector& newPos);

    // PC: call after any direct (non-physics-integrated) write to pos/orientation
    // so the HIGH_FPS render interpolation doesn't slide from the pre-snap
    // position/pose instead of cutting straight to it. No-op when the
    // HIGH_FPS presentation layer is disabled.
    void ResetRenderInterpolation();

    void OnVisualModelPromoted() override;
    void OnVisualModelDemoted() override;
    void AdvanceRenderInterpolationTick();
    void UpdateCollisionBoundsFromJoints();

    virtual void SetActionState(u32 state, s32 param);
    virtual void ProcessAction();
    virtual void ProcessControl();
    void Kill() override;
    void Killed();
    s32 GetStraifPhase();

    void RequestAction(u32 actionID);
    void FaceThing(Thing* target, s32 immediate);
    void FacePoint(const LVector& point, s32 immediate);
    bool FaceThingDesired(Thing* target);
    bool FacePointDesired(const LVector& point);
    virtual Humanoid* FindFoe(u32 range, s32 param, s32 immediate);
    void SetTarget(Humanoid* target);
    void ReleaseTarget();
    void FaceAngleY(s32 angle, s32 immediate);
    void SetTauntAnim(s32 index);
    void SetIdleAnimation(s32 loopType, s32 doTransition);
    bool TestIdleAnimation();
    s32 LoadDialog(u32 dialogID, s32 priority);
    void LoadEnemyTaunts();
    s32 PlayDialog(u32 dialogID, s32 priority);
    s32 PlayDialogBasedOnPriority(s32 minPriority, s32 maxPriority);
    s32 KillDialog(s32 force, s32 minPriority, s32 maxPriority);
    s32 EnterCombatCombo();
    s32 ProcessSoundEvent(s32 eventType);
    s32 ProcessFightingMove(const PsxFightingMoveRaw* move, s32 frame);
    s32 ProcessFightingMoveStrikeJoint(
        const PsxFightingJointRaw* joint,
        s32 frame,
        s32 attackType,
        s32 fightingPoints,
        s32 stylePointsFlag,
        s32 weaponBreakOnEmpty);
    virtual s32 GetTargetingFrame(const PsxFightingMoveRaw* move) const;
    s32 ProcessGenericFightingMove(const PsxFightingMoveRaw* move, s32 frame);
    s32 ProcessBodyThrow(const PsxFightingMoveRaw* move, s32 frame);
    s32 BodyThrowAttack(s32 radius);
    s32 ThrowCharacterReceive();
    s32 ThrowFreeFall();
    s32 GetImpactRegion(const LVector& point);
    s32 FindSiblingWithRequestedCommand(const FightingComboNode* root, u32 requestedBits);
    s32 FindSiblingWithRequestedCommand(const FightingComboNode* root, u32 requestedBits, s32 frame);
    s32 FindChildWithRequestedCommand(const FightingComboNode* root, u32 requestedBits);
    s32 FindChildWithRequestedCommand(const FightingComboNode* root, u32 requestedBits, s32 frame);
    Humanoid* FightTargetAndThrowLatch(u8 fightingType);
    s32 SetCurrentFightingNode();
    s32 ReSyncOrientation(const PsxFightingMoveRaw* move);
    s32 ProcessFightingComboNode();
    s32 TestAndSetWeaponKungFU();
    s32 TestWallContextFightingRequestRemap();
    void GotHitFreeForm();
    void GotHitCrusher();
    void PrepareLedgeLatch(const LVector& correctionPos, const LVector& normal);
    s32 CheckForLanding();
    bool CheckForLedges();
    bool CheckForLedges2(LVector& outNormal, LVector& outCorrectionPos, s32 clearance);
    s32 CheckForPickup();
    s32 CheckforPickup(u32 pickupHash);
    s32 RestorePositionFromBip01();
    void SetDesiredMoveDirection(s32 angle) { faceAngle = angle; }
    virtual s32 TestAndSetBackGrab();
    virtual s32 TestAndSetRisingAttack();
    s32 BackGrabCharacterLatch();
    s32 BackGrabCharacter();
    s32 BackGrabCharacterRelease();
    s32 BackGrabCharacterReceivePreLatch();
    s32 BackGrabCharacterReceiveLatch();
    s32 BackGrabCharacterReceive();
    s32 CounterAttack();
    s32 CounterAttackPreLatch();
    s32 CounterAttackLatch();
    s32 CounterAttackRecovery();
    s32 LetGoOfLedge();
    void SetHumanoidTarget(Humanoid* target);
    bool IsInActiveZone() const;
    bool IsTargetInActiveZone() const;
    s32 QuickCheckWallCollision(s16 angle, s32 distance, s32 radius, s32 height);
    s32 CheckWallCollision(
        s16 angle,
        s32 distance,
        s32 radius,
        s32 height,
        s32& outCollisionRatio,
        LVector& outWallNormal,
        LVector& outHitPoint,
        s32& outVerticalSpan,
        s32& outWallMaterial);
    s32 CheckWallConstraint(
        u32 minWallHeight,
        s32 distance,
        s32 minAngle,
        s32& outWallAngle,
        LVector& outHitPoint);
    s32 CheckDWOCollision(s16 angle, s32 distance);
    bool HasEnemyTauntDialog();
    virtual void _CrouchUp();

    virtual void _Stand();
    virtual void _Run();
    virtual void _Jump();
    virtual void _Fall();
    virtual void _Straif();
    virtual void _DiveRoll();
    virtual void _Taunt();
    virtual void _Pause();
    virtual void _GotHitHigh();
    virtual void _GotHitMed();
    virtual void _GotHitLow();
    virtual void _Collapse();
    virtual void _Dead();
    virtual void _SpinBack();
    virtual void _FlyingBack();
    virtual void _Floating();
    virtual void _Stunned();
    virtual void _TableThrow();
    virtual void _Throw();
    virtual void _Pickup();
    virtual void _LedgeLatch();
    virtual void _LedgePullup();
    virtual void _LadderLatchTop();
    virtual void _LadderLatch();
    virtual void _LadderDismount();
    virtual void _ClimbLadder();
    virtual void _Hotfoot();
    virtual void _NISMode();
    // PSX Butch-only vtable slots (BOSS.CPP cases 74-76); no-op on base Humanoid.
    virtual void _Stomp();
    virtual void _Charge();
    virtual void _ThrowPot();
    // PSX Dante-only vtable slots (BOSS.CPP missile states); no-op on base Humanoid.
    virtual void _MissilePrepare();
    virtual void _MissileAttack();
    virtual void _TargetMissileAttack();
    virtual void CreateSound();
    virtual void ReleaseSound();
    virtual void DoJump();
    virtual void _DoStand();
    virtual void _DoRun();
    virtual s32 LoadCombatDialog();
    virtual void PlayCombatKnockDownDialog(s32 damageType);
    virtual void HandleHitShock(s32 damageType);
    virtual void PlayCombatThrowDialog();

    s32 SetRightHandObj(Pickup* pickup);
    void DeleteRightHandObj();
    void DeleteLeftHandObj();
    void DropPickup(s32 dropRight, s32 dropLeft);
};

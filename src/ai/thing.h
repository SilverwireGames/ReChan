#pragma once

#include "core.h"
#include "p3d/lvector.h"
#include "p3d/p3dmath.h"
#include "gen/cclist.h"
#include "gen/config.h"

struct DBRoot;
class BlockManager;
struct Ticket;

// ThingHandle - lazy-allocated safe reference (8 bytes on PSX)
struct ThingHandle {
    class Thing* owner = nullptr;
    u16 refCount = 0;
};

// Block number sentinel (unassigned)
static constexpr u16 BLOCK_UNASSIGNED = 0x1000;

// PSX AI:: scope enums (namespace renamed to avoid class AI conflict)
namespace AITypes {
    // ThingTypes - entity type IDs dispatched by AI::AddThingNoTagList
    // PSX: enum AI::ThingTypes (used for CharacterManager, etc.)
    // Values match PSX AddThingNoTagList dispatch at 0x80054404
    enum ThingTypes : u16 {
        TT_PLAYER = 0,
        // Humanoid range: 1-28 (generic thugs + named bosses)
        TT_HUMANOID_FIRST = 1,
        TT_THUG1 = 1,
        TT_THUG2 = 2,
        TT_THUG3 = 3,
        TT_THUG4 = 4,
        TT_THUG5 = 5,
        TT_THUG6 = 6,
        TT_THUG7 = 7,
        TT_THUG8 = 8,
        TT_GRONTAR = 10,   // Grontar class (616 bytes)
        TT_PAUL = 12,      // Paul class (616 bytes)
        TT_OSCAR = 13,     // Oscar class (616 bytes)
        TT_DANTE = 15,     // Dante class (684 bytes)
        TT_BUTCH = 17,     // Butch class (620 bytes)
        TT_OSCAR_HENCHMAN = 23, // "MIME" in g_charNameTable; checked by Dead__8Humanoid (0x800691FC)
        TT_HUMANOID_LAST = 28,
        // Pickup-backed collectible entry; PSX AddThingNoTagList routes 101 through Pickup.
        // This is distinct from TT_COLLECTIBLE_OBJ (436), which is the real Collectible class.
        TT_COLLECTIBLE = 101,
        // Platform (goes to moveList)
        TT_PLATFORM = 201,
        // Obstacle range: 301-328 (Pickup class, goes to pickupList)
        TT_OBSTACLE_FIRST = 301,
        TT_OBSTACLE_LAST = 328,
        // Interactive objects (go to moveList)
        TT_LAUNCHER = 402,
        TT_CONVEYOR = 404,
        TT_SLIPPERYFLOOR = 405,
        TT_HORIZONTALPOLE = 407,
        TT_EXPLOSIVE = 410,
        TT_DESTRUCTIBLE = 412,
        TT_CRUSHER = 413,
        TT_KNOCKDOWN = 420,
        TT_STACK = 421,
        TT_KICKNROLL = 422,
        TT_DESTRUCTIBLE_DP = 424,  // with dead pool check
        TT_UNTOUCHABLE = 435,
        TT_COLLECTIBLE_OBJ = 436, // Collectible class (Obstacle subclass)
        TT_TRIGGERTHING = 451,
        TT_GENERATOR = 455,
        TT_ENEMYGENERATOR = 456,
        TT_EXPLOSIVE_OBJ = 457,
        TT_BLAST = 459,
        TT_THROWGENERATOR = 460,
        TT_PUSHABLE = 462,
        TT_DOOR = 463,
        TT_TELEPORTER = 464,
        TT_PENDULUM = 466,
        TT_TRAPDOOR = 467,
        TT_TABLE = 468,
        TT_BOSS = 469,
        TT_LADDER = 470,
        TT_CHAIR = 471,
        TT_ARROW = 472,
        // Special analysis types (not real entities)
        TT_LIGHTSPHERE = 65533,
        TT_LIGHTSPHERE2 = 65534,
        TT_MESHANALYSIS = 65535,
    };
}

// Thing flags (u32 flags bitmask)
enum ThingFlags : u32 {
    TF_DEAD = 0x0001,  // marked for death transfer
    TF_BIT1 = 0x0002,  // cleared each Think frame
    TF_NEEDS_ACTIVATION = 0x0004,  // needs activation check
    TF_BIT3 = 0x0008,
    TF_ACTIVATED = 0x0010,  // currently activated
    TF_BIT5 = 0x0020,
    TF_MODEL_CREATED = 0x0040,  // model has been created
    TF_DIRECTOR_ACTIVE = 0x0200,  // director-owned mark set during script processing
    TF_BIT8 = 0x0100,
    TF_DYNAMIC = 0x0800,  // is a DynamicThing
    TF_ON_GROUND = 0x1000,  // on ground
};

// Thing secondary flags (u32 flags2 bitmask)
enum ThingFlags2 : u32 {
    TF2_KILLED = 0x0001,  // marked for removal
    TF2_BIT3 = 0x0008,  // cleared each Think frame
    TF2_NIS_ENTER = 0x0010,  // entered NIS control
    TF2_NIS_FROZEN = 0x0020,  // NIS frozen (no movement)
    TF2_NIS_MASK = 0x0070,  // mask for NIS state bits
    TF2_TAUNT_PENDING = 0x0080,  // awaiting LoadPlayerTauntResponse (Think__8Humanoid step 4)
};

// Thing - base class for all game entities
class Thing : public ccNode {
public:
    // PSX +24 (u16): entity type from AI::ThingTypes
    u16 thingType = 0;
    // PSX +26 (u16): initialized to INVALID_HANDLE
    u16 collisionRadius = INVALID_HANDLE;

    // PSX +28,+32,+36: world position (s32 fixed-point)
    LVector pos = {};

    // PSX +40,+44,+48: orientation/rotation (reset to 0)
    LVector orientation = {};

    // PSX +52 (s32): general-purpose state counter (initialized to 1)
    s32 stateCounter = 1;

    // PSX +56 (u16): current hit points
    u16 health = 1;
    // PSX +58 (u16): max hit points (restored on Reset)
    u16 maxHealth = 1;

    // PSX +60 (ptr): ThingHandle used for safe cross-references
    ThingHandle* thingHandle = nullptr;

    // PSX +64: embedded ccNode for secondary list insertion (entity lists in BlockManager)
    ccNode subNode;

    // PSX +76 (ptr): reserved
    u32 modelHash = 0;

    // PSX +80 (ptr): tDrawable* model for rendering
    void* model = nullptr;

    // PSX +84 (u16): block number this Thing is in (BLOCK_UNASSIGNED = unassigned)
    u16 blockNum = BLOCK_UNASSIGNED;

    // PSX +86 (u16): unique ID assigned from global counter
    u16 uniqueID = 0;

    // PSX +88 (u32): bitfield flags (see ThingFlags enum)
    u32 flags = 0;

    // PSX +92 (u32): secondary flags (see ThingFlags2 enum)
    u32 flags2 = 0;

    // Global Thing counter - PSX: gp+3868
    static u16 s_nextUniqueID;

    Thing(const LVector* initialPos, u16 type);
    ~Thing() override;

    virtual void Think();
    virtual void Draw();
    virtual void Reset();
    virtual void UpdatePosition();
    virtual void Activate();
    virtual void Deactivate();
    virtual void Move();
    virtual void CreateModel(const char* name);
    virtual void DeleteModel();

    // PC: display-only model (widescreen).
    bool pcVisualModelOnly = false;
    virtual void OnVisualModelPromoted();
    virtual void OnVisualModelDemoted();
    virtual void HandleCollision(Thing* other, s32 damage, ...);
    virtual void AnalyzeMesh(DBRoot* root);
    virtual void GetViewSpot(LVector* outPos, LVector* outTarget);
    virtual void Kill();
    virtual LVector* GetSoundPosPtr();
    virtual const LVector* GetInitialPos();


    void AddPassenger(class DynamicThing* passenger);
    void RemPassenger(Ticket* ticket);
    void RemAllPassengers();
    ThingHandle* GetThingHandle();
    void ClearFloorHeight();
    void SetFloorHeight(s32 height);

    void GetObjectToWorldSpaceVector(const SVector& in, SVector& out);
	void GetObjectToWorldSpaceVector(const LVector& in, LVector& out);
    s32 DistanceFromPointXZ(const LVector& point) const;
    s32 DistanceFromPoint(const LVector& point) const;
};

// DynamicThing - Thing with physics (velocity, gravity, forces)
class DynamicThing : public Thing {
public:
    // PSX +96 (s32): max displacement magnitude per frame (speed cap)
    s32 maxSpeed = 100;

    // PSX +100,+104,+108: velocity vector
    LVector velocity = {};

    // PSX +112,+116,+120: applied force (subtracted from velocity each frame)
    LVector force = {};

    // PSX +124,+128,+132: home/previous position (set from pos on init)
    LVector homePos = {};

    // PSX +136,+140,+144: contact/push force
    LVector contactForce = {};

    // PSX padding/fields +148..+183
    s32 field148[9] = {};

    // PSX +184 (s32): ground standing Y height (set when TF_ON_GROUND)
    s32 groundStandHeight = 0;

    // PSX +188 (ptr): embark ticket
    Ticket* ticket = nullptr;

    // PSX +192 (s32): gravity magnitude (0x8000 = default)
    s32 gravity = 0x8000;

    // PSX +196 (s32): max fall speed divisor (10)
    s32 maxFallDivisor = 10;

    DynamicThing(const LVector* initialPos, u16 type);
    ~DynamicThing() override;
    void Reset() override;
    void Move() override;
    void UpdatePosition() override;
    void AddForce(s32 magnitude, const SVector* direction);
    void Land();
    void DisembarkObstacle(const LVector& newPos);
    void Disembark();
    Thing* GetTicketIssuer();
    virtual void HandleLand(s32 height);
};

// Ticket - embark/passenger link between a Thing and a DynamicThing
struct Ticket : public ccNode {
    Thing* issuer = nullptr;           // PSX +24: the Thing being stood on
    DynamicThing* passenger = nullptr; // PSX +28: the DynamicThing standing on it

    Ticket(Thing* iss, DynamicThing* pass);
    ~Ticket() override;
};

// DynamicThing physics constants (PSX: gp+1740, gp+1744, defined in world.cpp)
extern s32 g_maxFallSpeed;
extern s32 g_dampingFactor;

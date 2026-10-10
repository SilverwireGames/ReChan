#pragma once
#include "ai/thing.h"
#include "gen/colvol.h"

class Humanoid;
class Obstacle;
class Pickup;
struct DBLine;
struct tSphere;

// Obstacle (116 bytes on PSX) - base class for interactive objects
// Inherits Thing (96 bytes)
// PSX vtable at 0x800CE7A0
class Obstacle : public Thing {
public:
    // PSX +96: collision bounding box (16 bytes: 6 s16s + extent + pad)
    tagCollisionBox collBox = { 0x7FFF, 0x7FFF, 0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0 };

    // PSX +112: physical type byte
    u8 physicalType = 0;

    // PSX +113: lighting flag (1 = use hardware lights)
    u8 lightingFlag = 1;

    // PSX +114: shadow flag (1 = has shadow)
    u8 shadowFlag = 1;

    // PSX +115: alive flag (0 = dead/destroyed; set to 1 by destructible subclass ctors)
    u8 aliveFlag = 0;

    Obstacle(const LVector* pos, u16 type);
    ~Obstacle() override;

    void Think() override;
    void Draw() override;
    void Reset() override;
    void Move() override;
    void UpdatePosition() override;
    void CreateModel(const char* name) override;
    void DeleteModel() override;
    void AnalyzeMesh(DBRoot* root) override;
    void AllocateAndCreateShadow();
    void UpdateShadowFloorHeight();
    void MovePassengersBasic();

    virtual void FillSphere(tSphere& sphere) const;
    virtual void HandlePickupCollision(Thing* pickup);
    virtual void HandleHumanoidCollision(Humanoid* hum);
    virtual void Trigger();
    virtual void TriggerByName(Thing* source, const char* name, const char* param);
    virtual void ExplosiveTrigger(s32 damage, const char* name);
    virtual const LVector* GetDeltaVelocity() const;
    virtual bool CareAboutAttack() const;
    // Transitional ABI shim for stale incremental objects; forwards to 4-arg overload.
    virtual void HandleAttack(Humanoid* attacker, s32 damageType, s32 damage);
    virtual void HandleAttack(Humanoid* attacker, s32 damageType, s32 attackMagnitude, s32 damage);
    virtual s32 GetFloorMaterial() const;
    virtual s32 GetObstacleFloorHeight(const LVector& pos) const;
    bool LedgeCheck(const tagCollisionBox& box, const LVector& normal, const LVector& correctionPos, Humanoid* hum) const;
    static bool DetectObstacleAboveLedge(const LVector& normal, const LVector& ledgePos);
    static bool DetectObstacle(const LVector& startPos, const LVector& endPos, s32 radius);

    s32 GetPhysical() const;
    void SetCollisionBox(const tagCollisionBox& box);

    static void FillBoxCentre(LVector& outPos, const LVector& pos, const LVector& orientation, const tagCollisionBox& box);
    static s32 GetWorldFloorHeight(const LVector& pos);
    static s32 GetYRotation(s32 x, s32 z);
    static s32 StaticGetObstacleFloorHeight(const LVector& pos);
    static void HandlePickupObstacleCollision(Pickup* pickup);
    static void HandleHumanoidObstacleCollision(Humanoid* hum);

    // Pure-cost broad-phase cache for the per-step pickup/humanoid vs
    // obstacle passes (AI::MoveThings).  Inside a batch the two functions
    // above scan a compact {x, z, extent} snapshot of the model-created
    // moveList nodes instead of chasing every Obstacle object; the snapshot
    // is dropped whenever a call that may change obstacle state runs.
    static void BeginCollisionPassBatch();
    static void EndCollisionPassBatch();
};

// Global zero delta velocity (PSX: 0x800CE818)
extern const LVector ZERO_DELTA_VELOCITY;

// PSX: FillVectorArray__8ObstacleP10tagLVectorRC6DBLine (0x8007AEF0)
// Copies two DBLine vertices into out[0..1].
bool FillVectorArray(LVector* out, const DBLine& line);

// PSX: FillVectorArray__8ObstacleP10tagLVectorUlRC6DBLine (0x8007AF14)
// Copies up to count vertices from DBLine into out[]; return value matches PSX helper.
bool FillVectorArray(LVector* out, u32 count, const DBLine& line);

// Obstacle animation table (PSX: g_animTable at 0x800E0E58, g_animCount at GP+0xF38)
// Populated by Obstacle::Load during petal loading, cleared by ClearPetalAnimList.
// Entries are MiscAnimNode* from AnimationManager. Access TransformAnim* via ->anim.
struct MiscAnimNode;
MiscAnimNode* Obstacle_GetAnimation(s32 animIndex);
void Obstacle_ClearPetalAnimList();
void Obstacle_AddAnimation(MiscAnimNode* node);

// PSX: Load__8ObstacleR10tReadChunkPPv (0x8007CAA4)
// Parses chunk 0x8A20 body and appends matching misc anim to obstacle table.
void Obstacle_LoadAnimChunk(const u8* body, u32 bodySize);

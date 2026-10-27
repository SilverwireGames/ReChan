#pragma once
#include "ai/obstacle.h"

struct Path;
class CPlatformSound;

// Platform (type 201) - movable platforms, manhole covers, ladders in hub
// PSX: PLATFORM.CPP / PLATFORM.HPP, overlay 6
// PSX vtable at 0x80025F08, 324 bytes total
class Platform : public Obstacle {
public:
    s32 platformFlags = 0;      // +116: behavior flags
    s32 isActive = 0;           // +120: active state
    s32 moveState = 0;          // +124: 0=idle, 1=moving
    s32 moveDone = 0;           // +128
    s32 loopCount = 0;          // +132
    s32 triggerDelay = 0;       // +136
    s32 field8C = 0;            // +140
    s32 initialCountdown = 0;   // +144
    s32 deathCountdown = 3;     // +148
    s32 countdown2 = 0;         // +152
    s32 field9C = 0;            // +156
    LVector velocity = {};      // +160
    LVector prevVelocity = {};  // +172: previous frame's velocity (copied in Think)
    s32 speed = 0;              // +184
    s32 field_188 = 0;          // +188
    s32 field_192 = 0;          // +192
    s32 pathSignal = 0;         // +196
    s32 field_200 = 0;          // +200
    s32 fieldCC = 0;            // +204
    Path* splinePath = nullptr; // +208
    s32 pathInstance = 0;       // +212
    s32 drawDistSq = 0;         // +216
    tagCollisionBox localCollBox = { 0x7FFF, 0x7FFF, 0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0 };
    LVector initialPos = {};    // +236
    LVector initialRot = {};    // +248
    tagCollisionBox* field104 = nullptr; // +260: dual collision boxes for split geometry (PSX: ptr to tagCollisionBox[2])
    s32 childThingCRC = 0;      // +264
    Thing* childThing = nullptr; // +268
    LVector childInitialRot = {}; // +272
    s32 field120 = 0;           // +288
    s32 field124 = 0;           // +292
    s32 field128 = 0;           // +296
    s32* field12C = 0;           // +300
    s32* field130 = 0;           // +304
    u32 field134 = 0;           // +308: trigger param token (PSX raw word compare)
    s32 killThingsCRC = 0;      // +312
    Thing* killTarget = nullptr; // +316
    CPlatformSound* platformSound = nullptr; // +320

    Platform(const LVector* pos, u16 type);
    ~Platform() override;

    void AnalyzeMesh(DBRoot* root) override;
    void CreateModel(const char* name) override;
    void DeleteModel() override;
    void Reset() override;
    void Think() override;
    void Move() override;
    void Draw() override;
    void UpdatePosition() override;
    void FillSphere(tSphere& sphere) const override;
    void HandlePickupCollision(Thing* pickup) override;
    void HandleHumanoidCollision(Humanoid* hum) override;
    void TriggerByName(Thing* source, const char* name, const char* param) override;
    void OnXorZRot();
    void Teeter();
    void Bob();
    void MovePassengers();
    bool HandleEnvironmentCollision(LVector& nextPos);
    void HandlePathNodeTransition();
    void SetPlatformToPathNode(const char* name);
    const LVector* GetDeltaVelocity() const override;
    s32 GetFloorMaterial() const override;
    const LVector* GetInitialPos() override;
    void DeathCheck();
    bool CheckForSquash(const LVector& outPos, const LVector& outNormal, const LVector& collBbox);

    // PSX: AtEndOfPath__8Platform (0x80024BA0)
    // Returns nonzero when the platform has no spline or has finished its current path.
    s32 AtEndOfPath() const;

    static void FillVehicleCollisionBoxes(
        tagCollisionBox& box1, tagCollisionBox& box2,
        const DBRoot& dbRoot, u32 attribNum, s16 yThreshold);
};

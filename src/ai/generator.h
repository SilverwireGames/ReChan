#pragma once
#include "ai/obstacle.h"
#include "gen/database.h"
#include "p3d/lvector.h"

class ActiveZone;
class SubZoneVolume;

class Generator : public Obstacle {
public:
    // PSX +116: embedded DBRoot for attribute storage (60 bytes)
    DBRoot dbRoot;

    // PSX +176 (s32): unknown
    s32 field176 = 0;
    // PSX +180 (s32): unknown
    s32 field180 = 0;
    // PSX +184 (s32): unknown
    s32 field184 = 0;
    // PSX +188 (u16): type of entity to generate (read by EnemyGenerator::GenerateObject)
    u16 generateType = 0;
    // PSX +190 (u16): padding
    u16 field190 = 0;
    // PSX +192 (s32): current generate count
    s32 generateCount = 0;
    // PSX +196 (s32): unknown
    s32 field196 = 0;
    // PSX +200 (s32): unknown
    s32 field200 = 0;
    // PSX +204 (s32): permanent attribute count (a1[51])
    s32 attribArrayCount = 0;
    // PSX +208 (ptr): generated object handle slots (a1[52])
    ThingHandle** attribArray = nullptr;
    // PSX +212 (ptr): model name buffer (a1[53])
    char* modelNameBuffer = nullptr;

    Generator(const LVector* pos, u16 type);
    ~Generator() override;

    void AnalyzeMesh(DBRoot* root) override;
    void CreateModel(const char* name) override;
    void DeleteModel() override;
    void Reset() override;
    void Think() override;
    void UpdatePosition() override;
    void HandlePickupCollision(Thing* pickup) override;
    void HandleHumanoidCollision(Humanoid* hum) override;
    void Trigger() override;

    virtual void GenerateObject(s32 param);
};

class EnemyGenerator : public Generator {
public:
    // PSX +216..+251: spawn point positions (3 LVectors × 12 bytes = 36 bytes)
    LVector spawnPoints[3] = {};
    // PSX +252..+263: spawn zone hashes (3 u32s)
    u32 zoneHashes[3] = { 0, 0, 0 };
    // PSX +264..+275: resolved SubZoneVolume pointers for each target point
    SubZoneVolume* subZoneVolumes[3] = { nullptr, nullptr, nullptr };
    // PSX +276 (u32): number of active spawn points (max 3)
    u32 targetCount = 0;
    // PSX +280 (u32): active-zone hash from attrib 10
    u32 activeZoneHash = 0;
    // PSX +284 (ptr): cached ActiveZone* resolved by activeZoneHash
    ActiveZone* activeZone = nullptr;
    // PSX +288 (s32): active-zone member threshold
    s32 field288 = 0;
    // PSX +292 (s32): spawn burst count
    s32 field292 = 0;
    // PSX +296 (s32): pending spawn trigger flag
    s32 field296 = 0;

    // PC: Encounter tracking; the original AI counters remain intact.
    ActiveZone* encounterZone = nullptr;
    bool encounterPopulationTracked = false;
    s32 encounterSpawnsRemaining = 0;

    EnemyGenerator(const LVector* pos, u16 type);
    ~EnemyGenerator() override;

    void AnalyzeMesh(DBRoot* root) override;
    void Reset() override;
    void Think() override;
    void GenerateObject(s32 param) override;

    void SetupTargets(const char* pointNames);
};

class ThrowingGenerator : public Generator {
public:
    // PSX +216..+227: unknown (3 dwords)
    s32 field216 = 0;
    s32 field220 = 0;
    s32 field224 = 0;
    // PSX +228 (s32): field-of-fire start angle (a1[57])
    s32 fofAngleStart = 0;
    // PSX +232 (s32): field-of-fire end angle (a1[58])
    s32 fofAngleEnd = 0;
    // PSX +236 (s32): unknown
    s32 field236 = 0;
    // PSX +240 (s32): player tracking X (a1[60])
    s32 playerTrackX = 0;
    // PSX +244 (s32): unknown
    s32 field244 = 0;
    // PSX +248 (s32): player tracking Z (a1[62])
    s32 playerTrackZ = 0;
    // PSX +252..+255: unknown
    s32 field252 = 0;

    ThrowingGenerator(const LVector* pos, u16 type);
    ~ThrowingGenerator() override;

    void AnalyzeMesh(DBRoot* root) override;
    void Reset() override;
    void Think() override;
    void GenerateObject(s32 param) override;

    bool TargetInFOF() const;
};

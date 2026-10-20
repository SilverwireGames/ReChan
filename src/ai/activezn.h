#pragma once
#include "gen/cclist.h"
#include "ai/simplbox.h"

class Humanoid;
class EnemyGenerator;
struct LinearPath;
struct DBVolume;

// SubZoneVolume (48 bytes) - ccNode(24) + SimpleBox(24)
// PSX source: C:\CHAN\GAME\SRC\AI\ACTIVEZN.CPP:176
class SubZoneVolume : public ccNode {
public:
    SimpleBox box;  // +24

    SubZoneVolume(DBVolume* vol);
    ~SubZoneVolume() override = default;

    bool IsInSubZoneVolume(class Thing* thing) const;
};

// ActiveZone (104-byte PSX prefix) - manages an area with overlord AI, paths, and sub-zones
// PSX layout:
//   +0:  ccNode base (24 bytes)
//   +24: SimpleBox (24 bytes)
//   +48: pathList (ccList, 12 bytes) - LinearPath entries
//   +60: subZoneList (ccList, 12 bytes) - SubZoneVolume entries
//   +72: overlordType (s32)
//   +76: overlordValue (u8)
//   +80: overlordID (s32)
//   +84: memberCount (u8)
//   +88: members[3] (Humanoid* x3 = 12 bytes)
//   +100: specialFlag (s32)
// PC: Encounter counts follow the original 104-byte layout.
// PSX source: C:\CHAN\GAME\SRC\AI\ACTIVEZN.CPP
class ActiveZone : public ccNode {
public:
    SimpleBox box;          // +24
    ccList pathList;        // +48: LinearPath nodes
    ccList subZoneList;     // +60: SubZoneVolume nodes
    s32 overlordType = 0;   // +72
    u8 overlordValue = 0;   // +76
    s32 overlordID = 0;     // +80
    u8 memberCount = 0;     // +84
    Humanoid* members[3] = {};  // +88
    s32 specialFlag = 0;    // +100

    // PC: Encounter state. Unlike members[], these counts include
    // authored enemies that have not been activated by streaming yet.
    s32 encounterHumanoidsRemaining = 0;
    s32 pendingEnemySpawns = 0;

    ActiveZone(DBVolume* vol);
    ~ActiveZone() override;

    void AddLinearPath(LinearPath* path);

    void AddSubZoneVolume(SubZoneVolume* szv);
    void AddHumanoidToOverlordMembers(Humanoid* h);
    void RemoveHumanoidFromOverlordMembers(Humanoid* h);
    void RegisterEncounterHumanoid(const Humanoid* humanoid);
    void ResolveEncounterHumanoid(const Humanoid* humanoid);
    void RegisterPendingEnemySpawns(const EnemyGenerator* generator, s32 count);
    void ResolvePendingEnemySpawn(const EnemyGenerator* generator);
    void RemovePendingEnemySpawns(const EnemyGenerator* generator, s32 count);
    s32 GetEncounterHumanoidsRemaining() const { return encounterHumanoidsRemaining; }
    s32 GetPendingEnemySpawns() const { return pendingEnemySpawns; }
    s32 GetUnresolvedEncounterCount() const {
        return encounterHumanoidsRemaining + pendingEnemySpawns;
    }
    bool HasUnresolvedEncounter() const { return GetUnresolvedEncounterCount() > 0; }
    s32 GetNumberOfThinkingMembers() const;
    s32 AllowedToMoveIn(Humanoid* humanoid);
    bool IsInActiveZone(class Thing* thing) const;
    void GetActiveZoneCenterPoint(LVector& outCenter) const;
    LinearPath* FindFirstValidPath(Humanoid* humanoid);
    s32 DoAICheck(LinearPath* path, s32 nodeIndex, Humanoid* humanoid);
    s32 DoActionsAtNode(LinearPath* path, s32 nodeIndex, Humanoid* humanoid);
    bool IsPathNodeTerminator(LinearPath* path, s32 nodeIndex) const;
    bool AllowBreakoffOfDestinationNode(LinearPath* path, s32 nodeIndex) const;
};

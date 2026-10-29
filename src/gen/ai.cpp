#include "gen/ai.h"
#include "gen/game.h"
#include "gen/director.h"
#include "gen/world.h"
#include "gen/database.h"
#include "gen/envmgr.h"
#include "gen/charmgr.h"
#include "gen/model.h"
#include "gen/deadpool.h"
#include "gen/animstruct.h"
#include "gen/scoremgr.h"
#include "gen/blockmgr.h"
#include "gen/colmgr.h"
#include "ai/colfight.h"
#include "ai/colfight.h"
#include "ai/humndata.h"
#include "pc/log.h"
#include "gen/camera.h"
#include "gen/path.h"
#include "gen/ccfile.h"
#include "gen/psxmath_helpers.h"
#include "p3d/p3dmath.h"
#include "p3d/hash.h"
#include "ai/obstacle.h"
#include "ai/boss.h"
#include "ai/player.h"
#include "ai/humanoid.h"
#include "ai/fevolume.h"
#include "ai/hpole.h"
#include "ai/activezn.h"
#include "ai/arrow.h"
#include "ai/collect.h"
#include "ai/door.h"
#include "ai/ladder.h"
#include "ai/launcher.h"
#include "ai/pickup.h"
#include "ai/trapdoor.h"
#include "ai/platform.h"
#include "ai/teleporter.h"
#include "ai/trigger.h"
#include "ai/conveyor.h"
#include "ai/slippery.h"
#include "ai/explosive.h"
#include "ai/destroy.h"
#include "ai/crusher.h"
#include "ai/kicknroll.h"
#include "ai/untouchable.h"
#include "ai/generator.h"
#include "ai/blast.h"
#include "ai/pushable.h"
#include "ai/pendulum.h"
#include "ai/table.h"


AI* g_ai = nullptr;

// WorldPoints system - stores NIS reference points parsed from WDB database.
// PSX: WorldPointLists global at gp+FF0 (theWorldPoints).
// Populated from DBPoints with type 110 during AI::Populate.
static ccList g_worldPointList;
static const char s_worldPointParName[] = "PAR";

// PSX: gp+0x5E0 (0x800DCF2C)
static s32 humanoidVolumeRadius = 0xC0;

static bool IsAsciiAlphaNum(char c) {
    return (c >= '0' && c <= '9')
        || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z');
}

static char* GetNextAlphaNumToken(char* outToken, char* cursor, const char* end) {
    while (cursor < end && !IsAsciiAlphaNum(*cursor)) {
        cursor++;
    }

    char* out = outToken;
    while (cursor < end && IsAsciiAlphaNum(*cursor)) {
        *out++ = *cursor++;
    }
    *out = '\0';
    return cursor;
}

static u32 AlphaToULong(const char* token) {
    u32 value = 0;
    while (*token >= '0' && *token <= '9') {
        value = value * 10 + (u32)(*token - '0');
        token++;
    }
    return value;
}

static void CopyComboToken(char* dst, const char* src) {
    s32 index = 0;
    while (index < 7 && src[index] != '\0') {
        dst[index] = src[index];
        index++;
    }
    dst[index] = '\0';
}

BehaviourAttrib::BehaviourAttrib() {
    MARKFUNCTION(0x80074434);
}

BehaviourAttrib::~BehaviourAttrib() {
    MARKFUNCTION(0x800744C0);

    delete[] comboChances;
    comboChances = nullptr;

    if (comboStrings) {
        for (s32 index = 0; index < comboCount; index++) {
            delete[] comboStrings[index];
        }
        delete[] comboStrings;
        comboStrings = nullptr;
    }
}

static bool IsHumanoidCollisionSkipStateA(s32 state) {
    if ((u32)(state - 69) < 4u) {
        return true;
    }
    return state == 56;
}

static bool IsHumanoidCollisionSkipStateB(s32 state) {
    return state == 36 || state == 59 || state == 37 || state == 38 ||
        state == 60 || state == 61 || state == 62;
}

void WorldPoints_Reset() {
    while (ccNode* n = static_cast<ccNode*>(g_worldPointList.RemHead())) {
        delete n;
    }
}

void WorldPoints_AddPoint(DBPoint* pt) {
    if (!pt) return;

    if (pt->subType == 0) {
        // Position point (40 bytes on PSX)
        WorldPointNode* node = new WorldPointNode();
        node->pos = pt->pos;
        node->parValue = 9999;
        // Must own a copy: pt (DBPoint) is deleted by Database::Close() shortly
        // after Populate(), which would otherwise leave this name dangling.
        node->SetName(pt->GetName(), 1);

        const DBAttrib* a15 = pt->FindAttrib(15);
        if (a15) {
            node->parValue = (s32)a15->value;
        }

        g_worldPointList.AddNode(g_worldPointList.tail, node);
    }
    else if (pt->subType == 2) {
        // Par value point (28 bytes on PSX) - stores attrib 4 value
        WorldPointNode* node = new WorldPointNode();
        const DBAttrib* a4 = pt->FindAttrib(4);
        if (a4) {
            node->parValue = (s32)a4->value;
        }
        // PSX uses a fixed "PAR" node name (dword_800DD2E8) for par lookup.
        node->SetName(s_worldPointParName, 0);
        g_worldPointList.AddNode(g_worldPointList.tail, node);
    }
}

WorldPointNode* WorldPoints_GetNISPoint(u32 crc) {
    return static_cast<WorldPointNode*>(g_worldPointList.FindNodeCRC(crc, nullptr));
}

s32 WorldPoints_GetParValue() {
    // PSX GetParPointValue__11WorldPoints finds the fixed "PAR" node directly.
    WorldPointNode* parNode = static_cast<WorldPointNode*>(g_worldPointList.FindNode(s_worldPointParName, nullptr));
    if (!parNode) {
        return 0;
    }

    return parNode->parValue;
}

// PSX Populate__2AI: mesh loop (lst TEXT:80056698) then volume loop (80056748); same nameCRC yields two moveList
// nodes. Skip volume AddThing when a type-6 mesh twin already exists.
static bool SceneVolumeHasMeshTwin(const DBVolume* vol, u16 subType) {
    if (!g_database) {
        return false;
    }
    for (DBMesh* mesh = g_database->GetFirstMesh(); mesh; mesh = static_cast<DBMesh*>(mesh->next)) {
        if (mesh->type == 6 && mesh->subType == subType && mesh->nameCRC == vol->nameCRC) {
            return true;
        }
    }
    return false;
}

// PSX: KillThingsInList__FR6ccListl (AI.CPP:1252 helper)
// For each Thing in list, if blockNum < param, call Kill().
// When param=0, kills nothing (unsigned comparison).
static void KillThingsInList(ccList& list, s32 param) {
    for (ccMinNode* n = list.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        if (thing == Player::s_player) {
            continue;
        }
        if (thing->blockNum < (u32)param) {
            thing->Kill();
        }
    }
}

// PSX: PopulateBlockHelper__FR6ccList (AI.CPP:1958 helper)
// For each Thing in list, call Activate().
static void PopulateBlockHelper(ccList& list) {
    for (ccMinNode* n = list.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        thing->Activate();
    }
}

// PSX: UnpopulateBlockHelper__FR6ccList (AI.CPP:1979 helper)
// For each Thing in list, call Deactivate().
static void UnpopulateBlockHelper(ccList& list) {
    for (ccMinNode* n = list.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        thing->Deactivate();
    }
}

// PSX: __2AI (AI.CPP:297, 0x80054180)
AI::AI() {
    MARKFUNCTION(0x80054180);

    InternalReset();
    g_game->GetHandlerSet1().AddHandler(aiPrivHandler, 10);
    pri = -64;
}

// PSX: _._2AI (AI.CPP:317, 0x800542C4)
AI::~AI() {
    MARKFUNCTION(0x800542C4);
    while (thingList.RemHead()) {}
    while (activeZoneList.RemHead()) {}
    while (humanoidList.RemHead()) {}
    while (pickupList.RemHead()) {}
    while (inactivePickupList.RemHead()) {}
    while (moveList.RemHead()) {}
    while (behaviourList.RemHead()) {}
}

// PSX: InternalOpen__2AI (AI.CPP:322, 0x8005436C)
void AI::InternalOpen() {
    MARKFUNCTION(0x8005436C);

    ParseBehaviourAttribScript();
}

// PSX: InternalClose__2AI (AI.CPP:327, 0x8005438C)
void AI::InternalClose() {
    MARKFUNCTION(0x8005438C);
    KillThings(0);
}

// PSX: InternalReset__2AI (AI.CPP:874, 0x800553A4)
void AI::InternalReset() {
    MARKFUNCTION(0x800553A4);
    populateFlags = 1;
    UnPopulate(0);
}

// PSX: AddActiveZone__2AIP8DBVolume (AI.CPP:338, 0x800543AC)
void AI::AddActiveZone(DBVolume* vol) {
    MARKFUNCTION(0x800543AC);
    ActiveZone* az = new ActiveZone(vol);
    activeZoneList.AddNodeTail(az);
}

// PSX: AddThingNoTagList (AI.CPP:365, 0x80054404)
// Enormous function (~4000 bytes) that creates entities by type.
// Dispatches based on AI::ThingTypes to create Player, enemies, objects, etc.
// PSX: after creation, calls SetName, AnalyzeMesh(root), Reset(), AddNode(targetList)
Thing* AI::AddThingNoTagList(const char* name, u16 type,
                             const LVector* pos, const SVector* orient,
                             const char* modelName, const DBRoot* root) {
    MARKFUNCTION(0x80054404);

    if (type == AITypes::TT_LIGHTSPHERE || type == AITypes::TT_LIGHTSPHERE2) {
        if (g_environmentManager) {
            g_environmentManager->lighting.AnalyzeSphere(static_cast<DBPoint*>(const_cast<DBRoot*>(root)));
        }
        return nullptr;
    }

    if (type == AITypes::TT_MESHANALYSIS) {
        if (g_database) {
            g_database->AnalyzeMesh(const_cast<DBRoot*>(root));
        }
        return nullptr;
    }

    Thing* thing = nullptr;
    ccList* targetList = &humanoidList; // default: humanoidList (offset +52)

    // PSX: types 1-28 = Humanoid enemies (Thug1-8, bosses, etc.)
    if ((u16)(type - 1) < 28u) {
        switch (type) {
            case AITypes::TT_BUTCH:
                thing = new Butch(pos);
                break;
            case AITypes::TT_GRONTAR:
                thing = new Grontar(pos);
                break;
            case AITypes::TT_DANTE:
                thing = new Dante(pos);
                break;
            case AITypes::TT_PAUL:
                thing = new Paul(pos);
                break;
            case AITypes::TT_OSCAR:
                thing = new Oscar(pos);
                break;
            default:
            {
                Humanoid* h = new Humanoid(pos, type);
                thing = h;
                break;
            }
        }
    }
    // PSX: types 301-328 = Pickups. Type 101 also routes through Pickup.
    // The separate Collectible class is type 436 and stays on the obstacle/moveList path.
    else if ((u16)(type - 301) < 28u) {
        targetList = &pickupList;
        thing = new Pickup(pos, type);
    }
    // PSX: all other types (0, 101, 201, 402-472, etc.)
    else {
        // Type 0 = Player
        if (type == AITypes::TT_PLAYER) {
            Player* player = new Player(pos);
            thing = player;
            if (orient) {
                thing->orientation.x = orient->x;
                thing->orientation.y = orient->y;
                thing->orientation.z = orient->z;
            }
        }
        // Type 101 = Pickup-backed collectible entry, not the obstacle Collectible class (436).
        else if (type == AITypes::TT_COLLECTIBLE) {
            targetList = &pickupList;
            thing = new Pickup(pos, type);
        }
        // Type 201 = Platform (goes to moveList)
        // Types 402-472 = Interactive objects (go to moveList)
        else if (type == AITypes::TT_PLATFORM || type >= 402) {
            targetList = &moveList;
            // Type 469 = FrontEndVolume (hub level door triggers)
            if (type == AITypes::TT_BOSS) {
                FrontEndVolume* vol = new FrontEndVolume(pos, type);
                thing = vol;
            }
            else if (type == AITypes::TT_LAUNCHER) {
                thing = new Launcher(pos, type);
            }
            else if (type == AITypes::TT_TRIGGERTHING) {
                thing = new TriggerThing(pos, type);
            }
            else if (type == AITypes::TT_DOOR) {
                Door* d = new Door(pos, type);
                thing = d;
            }
            else if (type == AITypes::TT_TELEPORTER) {
                thing = new Teleporter(pos, type);
            }
            else if (type == AITypes::TT_LADDER) {
                thing = new Ladder(pos, type);
            }
            else if (type == AITypes::TT_HORIZONTALPOLE) {
                thing = new HorizontalPole(pos, type);
            }
            else if (type == AITypes::TT_TRAPDOOR) {
                thing = new TrapDoor(pos, type);
            }
            else if (type == AITypes::TT_COLLECTIBLE_OBJ) {
                thing = new Collectible(pos, type);
            }
            else if (type == AITypes::TT_PLATFORM) {
                thing = new Platform(pos, type);
            }
            else if (type == AITypes::TT_CONVEYOR) {
                thing = new Conveyor(pos, type);
            }
            else if (type == AITypes::TT_SLIPPERYFLOOR) {
                thing = new SlipperyFloor(pos, type);
            }
            else if (type == AITypes::TT_EXPLOSIVE || type == AITypes::TT_EXPLOSIVE_OBJ) {
                thing = new Explosive(pos, type);
            }
            else if (type == AITypes::TT_DESTRUCTIBLE) {
                thing = new DestructibleThing(pos, type);
            }
            else if (type == AITypes::TT_DESTRUCTIBLE_DP) {
                const u32 uidHash = p3dHash(name);
                if (!levelDeadPool.IsUIDInDeadPool(uidHash)) {
                    thing = new DestructibleThing(pos, type);
                }
            }
            else if (type == AITypes::TT_CRUSHER) {
                thing = new Crusher(pos, type);
            }
            else if (type == AITypes::TT_KNOCKDOWN) {
                thing = new KnockDown(pos, type);
            }
            else if (type == AITypes::TT_STACK) {
                thing = new Stack(pos, type);
            }
            else if (type == AITypes::TT_KICKNROLL) {
                thing = new KickNRoll(pos, type);
            }
            else if (type == AITypes::TT_UNTOUCHABLE) {
                thing = new Untouchable(pos, type);
            }
            else if (type == AITypes::TT_GENERATOR) {
                thing = new Generator(pos, type);
            }
            else if (type == AITypes::TT_ENEMYGENERATOR) {
                thing = new EnemyGenerator(pos, type);
            }
            else if (type == AITypes::TT_BLAST) {
                thing = new Blast(pos, type);
            }
            else if (type == AITypes::TT_THROWGENERATOR) {
                thing = new ThrowingGenerator(pos, type);
            }
            else if (type == AITypes::TT_PUSHABLE) {
                thing = new Pushable(pos, type);
            }
            else if (type == AITypes::TT_PENDULUM) {
                thing = new Pendulum(pos, type);
            }
            else if (type == AITypes::TT_TABLE) {
                thing = new Table(pos, type);
            }
            else if (type == AITypes::TT_CHAIR) {
                thing = new Chair(pos, type);
            }
            // Type 472 = Arrow (hub navigational arrow)
            else if (type == AITypes::TT_ARROW) {
                Arrow* arrow = new Arrow(pos, type);
                thing = arrow;
            }
        }
    }

    if (!thing)
        return nullptr;

    // PSX AddThingNoTagList supplemental player animation preloads for
    // specific humanoid/object types (AI.CPP:365, 0x80054404).
    if (g_characterManager) {
        s32 playerAnimStart = -1;
        u32 playerAnimCount = 0;
        bool togglePlayerCache = false;

        if ((u16)(type - 1) < 28u) {
            switch (type) {
                case AITypes::TT_BUTCH:
                    playerAnimStart = 133;
                    playerAnimCount = 4;
                    break;
                case AITypes::TT_GRONTAR:
                    playerAnimStart = 137;
                    playerAnimCount = 2;
                    break;
                case AITypes::TT_DANTE:
                    playerAnimStart = 143;
                    playerAnimCount = 5;
                    break;
                case AITypes::TT_PAUL:
                    playerAnimStart = 139;
                    playerAnimCount = 2;
                    break;
                case AITypes::TT_OSCAR:
                    playerAnimStart = 141;
                    playerAnimCount = 2;
                    break;
                default:
                    break;
            }
        }
        else if (type == AITypes::TT_LAUNCHER) {
            playerAnimStart = 295;
            playerAnimCount = 2;
            togglePlayerCache = true;
        }
        else if (type == AITypes::TT_HORIZONTALPOLE) {
            playerAnimStart = 285;
            playerAnimCount = 4;
            togglePlayerCache = true;
        }
        else if (type == AITypes::TT_LADDER) {
            playerAnimStart = 289;
            playerAnimCount = 6;
            togglePlayerCache = true;
        }

        if (playerAnimStart >= 0 && playerAnimCount > 0) {
            if (togglePlayerCache) {
                g_characterManager->EnableCache(AITypes::TT_PLAYER, 1);
            }
            g_characterManager->LoadAnimation(
                AITypes::TT_PLAYER,
                playerAnimStart,
                playerAnimCount,
                nullptr);
            if (togglePlayerCache) {
                g_characterManager->EnableCache(AITypes::TT_PLAYER, 0);
            }
        }
    }

    if (name) {
        // Must own a copy: 'name' typically points into a DBPoint/DBRoot owned
        // string that Database::Close() frees shortly after Populate() runs.
        thing->SetName(name, 1);
    }

    if (root) {
        const DBAttrib* a3 = root->FindAttrib(3);
        if (a3) {
            thing->collisionRadius = (u16)a3->value;
        }

        // PSX: humanoid attrib 31 can request a pre-active stand animation.
        // This animation is loaded from player animation data before AnalyzeMesh.
        if (g_characterManager && (u16)(type - 1) < 28u) {
            const DBAttrib* a31 = root->FindAttrib(31);
            if (a31) {
                const s32 preActiveIdle = GetPreActiveIdle((s32)p3dHash(a31->GetAttribString()));
                if (preActiveIdle != 22) {
                    g_characterManager->LoadAnimationBatch(AITypes::TT_PLAYER, preActiveIdle, nullptr);
                }
            }
        }
    }

    if (root) {
        thing->AnalyzeMesh(const_cast<DBRoot*>(root));
    }

    // PSX: for humanoids (type 1-28), OpenCharacter always, then load only when
    // character is not already loaded and loaded-count is below 4.
    if ((u16)(type - 1) < 28u) {
        if (g_characterManager) {
            g_characterManager->OpenCharacter(type);

            bool shouldLoadCharacter = false;
            if (!g_characterManager->IsCharacterLoaded(type)) {
                shouldLoadCharacter = g_characterManager->GetNumberCharactersLoaded() < 4;
            }

            if (shouldLoadCharacter) {
                g_characterManager->EnableCache(type, 1);
                g_characterManager->LoadCharacter(type, nullptr);
                // PSX: LoadAnimation(type, 0, 124, callback) - loads anims 0-123.
                g_characterManager->LoadAnimation(type, 0, 124, nullptr);
                g_characterManager->EnableCache(type, 0);
            }
        }
    }

    // PSX LABEL_162: CreateModel(modelName) then Reset()
    // PSX: if (!type || GetBlockNumber(pos) != 4096) CreateModel(modelName);
    if (!type || !g_blockManager || g_blockManager->GetBlockNumber(*pos) != 4096) {
        thing->CreateModel(modelName);
    }

    if (type == AITypes::TT_BOSS) {
        LOG("[AI] FrontEndVolume post-create: flags=0x%08X modelHash=%p collRadius=%d",
            thing->flags, thing->modelHash, thing->collisionRadius);
    }

    thing->Reset();

    if ((u16)(type - 1) < 28u) {
        Humanoid* humanoid = static_cast<Humanoid*>(thing);
        humanoid->orientation = humanoid->spawnOrientation;
        humanoid->faceAngle = humanoid->spawnOrientation.y;
    }

    targetList->AddNodeTail(thing);
    return thing;
}

// PSX: HandleHumanoidHumanoidCollision__FP8HumanoidT0 (AI.CPP:986, 0x80055584)
static void HandleHumanoidHumanoidCollision(Humanoid* a, Humanoid* b) {
    const s32 yTolerance = 0x300;
    const s32 xzRadius = humanoidVolumeRadius;
    const s32 stateA = a->actionState;
    const s32 stateB = b->actionState;

    bool skipStateA = IsHumanoidCollisionSkipStateA(stateA) || IsHumanoidCollisionSkipStateA(stateB);
    bool skipStateB = IsHumanoidCollisionSkipStateB(stateA) && IsHumanoidCollisionSkipStateB(stateB);
    bool skipState63 = stateA == 63 || stateB == 63;
    bool skipState73 = stateA == 73 || stateB == 73;
    bool skipState23 = stateA == 23 || stateB == 23;

    if (skipStateA || skipStateB || skipState63 || skipState73 || skipState23) {
        return;
    }

    s32 dx = b->homePos.x - a->homePos.x;
    s32 dy = b->homePos.y - a->homePos.y;
    s32 dz = b->homePos.z - a->homePos.z;
    s32 twiceRadius = xzRadius + xzRadius;

    if (dx < -twiceRadius || dx > twiceRadius) {
        return;
    }
    if (dy < -yTolerance || dy > yTolerance) {
        return;
    }
    if (dz < -twiceRadius || dz > twiceRadius) {
        return;
    }

    bool aMoved = false;
    if (PsxAbsS32(a->pos.x - a->homePos.x) >= 11 || PsxAbsS32(a->pos.z - a->homePos.z) >= 11) {
        aMoved = true;
    }

    bool bMoved = false;
    if (PsxAbsS32(b->pos.x - b->homePos.x) >= 11 || PsxAbsS32(b->pos.z - b->homePos.z) >= 11) {
        bMoved = true;
    }

    bool lockA = false;
    if (a->velocity.x || a->velocity.z || aMoved) {
        lockA = true;
    }

    bool lockB = false;
    if (b->velocity.x || b->velocity.z || bMoved) {
        lockB = true;
    }

    LVector posA = a->homePos;
    LVector posB = b->homePos;

    s32 diffX = posB.x - posA.x;
    s32 diffZ = posB.z - posA.z;

    s32 half = rmDiv16i(xzRadius, twiceRadius);

    if (PsxAbsS32(diffZ) >= PsxAbsS32(diffX)) {
        if (lockA) {
            if (!lockB) {
                if (posA.z >= posB.z) {
                    posA.z = posB.z + twiceRadius;
                }
                else {
                    posA.z = posB.z - twiceRadius;
                }
            }
            else {
                s32 split = posA.z + (s32)(((s64)half * (s64)diffZ) >> 16);
                if (posA.z >= split) {
                    posA.z = split + xzRadius;
                    posB.z = split - xzRadius;
                }
                else {
                    posA.z = split - xzRadius;
                    posB.z = split + xzRadius;
                }
            }
        }
        else if (lockB) {
            if (posB.z >= posA.z) {
                posB.z = posA.z + twiceRadius;
            }
            else {
                posB.z = posA.z - twiceRadius;
            }
        }
        else {
            s32 split = posA.z + (s32)(((s64)half * (s64)diffZ) >> 16);
            if (posA.z >= split) {
                posA.z = split + xzRadius;
                posB.z = split - xzRadius;
            }
            else {
                posA.z = split - xzRadius;
                posB.z = split + xzRadius;
            }
        }
    }
    else {
        if (lockA) {
            if (!lockB) {
                if (posA.x >= posB.x) {
                    posA.x = posB.x + twiceRadius;
                }
                else {
                    posA.x = posB.x - twiceRadius;
                }
            }
            else {
                s32 split = posA.x + (s32)(((s64)half * (s64)diffX) >> 16);
                if (posA.x >= split) {
                    posA.x = split + xzRadius;
                    posB.x = split - xzRadius;
                }
                else {
                    posA.x = split - xzRadius;
                    posB.x = split + xzRadius;
                }
            }
        }
        else if (lockB) {
            if (posB.x >= posA.x) {
                posB.x = posA.x + twiceRadius;
            }
            else {
                posB.x = posA.x - twiceRadius;
            }
        }
        else {
            s32 split = posA.x + (s32)(((s64)half * (s64)diffX) >> 16);
            if (posA.x >= split) {
                posA.x = split + xzRadius;
                posB.x = split - xzRadius;
            }
            else {
                posA.x = split - xzRadius;
                posB.x = split + xzRadius;
            }
        }
    }

    a->homePos = posA;
    b->homePos = posB;
}

// PSX: HandleHumanoidHumanoidCollision__Fv (AI.CPP:951, 0x800554D0)
static void HandleHumanoidHumanoidCollision() {
    MARKFUNCTION(0x800554D0);
    Humanoid** arr = FightingCollision::GetHumanoidArray();
    for (s32 i = 0; i < FIGHTING_COLLISION_MAX; ++i) {
        Humanoid* a = arr[i];
        if (!a) {
            continue;
        }

        for (s32 j = i + 1; j < FIGHTING_COLLISION_MAX; ++j) {
            Humanoid* b = arr[j];
            if (!b) {
                continue;
            }
            HandleHumanoidHumanoidCollision(a, b);
        }
    }
}

// PSX: MoveThings__2AI (AI.CPP:1268, 0x80055D10)
// Full per-frame pipeline matching PSX exactly.
void AI::MoveThings() {
    MARKFUNCTION(0x80055D10);

    // 1. Drain thingList (death staging) - delete completed deaths
    while (ccMinNode* n = thingList.RemHead()) {
        delete static_cast<Thing*>(n);
    }

    if (g_game) {
        if (World* world = g_game->GetWorld()) {
            world->ProcessPendingSwitchActions();
        }
    }

    // 2. Clear floor heights for humanoids
    ClearThingFloorHeights(humanoidList);

    // 3. Humanoid vs humanoid collision
    HandleHumanoidHumanoidCollision();

    // 4. Obstacle collisions
    MoveThingsObstacleCollisions();
    HandleHumanoidObstacleCollisions(humanoidList);

    // 5. Pickup collisions
    MoveThingsPickupCollisions();
    HandleHumanoidPickupCollisions(humanoidList, inactivePickupList);

    // 6. Environment collisions
    HandleThingEnvironmentCollisions(humanoidList);
    HandleThingEnvironmentCollisions(pickupList);

    // 7. Clear humanoid command bits
    for (ccMinNode* n = humanoidList.head; n; n = n->next) {
        Humanoid* humanoid = static_cast<Humanoid*>(n);
        humanoid->commandBits = 0;
    }

    // 8. Pickup deactivation: move deactivated pickups from pickupList to inactivePickupList
    for (ccMinNode* n = pickupList.head; n;) {
        ccMinNode* next = n->next;
        Pickup* pickup = static_cast<Pickup*>(n);
        if (pickup->PickupDeactivate()) {
            pickupList.RemNode(n);
            inactivePickupList.AddNode(nullptr, n);
        }
        n = next;
    }

    // 9. Update positions
    UpdatePositions(humanoidList);
    UpdatePositions(pickupList);

    // 10. Think pass (privMoveList handles Think + death transfer)
    privMoveList(moveList);
    privMoveList(humanoidList);
    privMoveList(pickupList);

    // 11. Camera
    MoveCamera();

    // 12. Transfer flagged-for-death from inactivePickupList to thingList
    for (ccMinNode* n = inactivePickupList.head; n;) {
        Thing* thing = static_cast<Thing*>(n);
        ccMinNode* next = n->next;
        if (thing->flags & TF_DEAD) {
            inactivePickupList.RemNode(n);
            thingList.AddNodeTail(n);
        }
        n = next;
    }
}

// PSX: privMoveList__2AIR6ccList (AI.CPP:886, 0x800553DC)
void AI::privMoveList(ccList& list) {
    MARKFUNCTION(0x800553DC);
    for (ccMinNode* n = list.head; n;) {
        Thing* thing = static_cast<Thing*>(n);
        ccMinNode* nextNode = n->next;

        u32 fl = thing->flags;
        if (!(fl & TF_DEAD)) {
            if (!(fl & TF_ACTIVATED)) {
                n = nextNode;
                continue;
            }
            thing->Think();
            fl = thing->flags;
            if (!(fl & TF_DEAD) || (fl & 0x0400)) {
                n = nextNode;
                continue;
            }
        }

        // Transfer thing from source list to thingList (death staging)
        if (Player::s_player == thing) {
            g_game->GetCamera().SetLookAtTarget(nullptr, 1);
        }
        list.RemNode(n);
        thingList.AddNodeTail(n);

        n = nextNode;
    }
}

// PSX: UpdatePositions__2AIR6ccList (AI.CPP:1197, 0x80055BE4)
void AI::UpdatePositions(ccList& list) {
    MARKFUNCTION(0x80055BE4);
    for (ccMinNode* n = list.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        thing->UpdatePosition();
    }
}

// PSX: KillThings__2AIl (AI.CPP:1252, 0x80055CB4)
void AI::KillThings(s32 param) {
    MARKFUNCTION(0x80055CB4);
    KillThingsInList(moveList, param);
    KillThingsInList(inactivePickupList, param);
    KillThingsInList(pickupList, param);
    KillThingsInList(humanoidList, param);
}

// PSX: MoveThingsObstacleCollisions__2AI (AI.CPP:1407, 0x80055F14)
void AI::MoveThingsObstacleCollisions() {
    MARKFUNCTION(0x80055F14);
    HandlePickupObstacleCollisions(pickupList);
    HandlePickupObstacleCollisions(inactivePickupList);
}

// PSX: MoveThingsPickupCollisions__2AI (AI.CPP:1413, 0x80055F44)
void AI::MoveThingsPickupCollisions() {
    MARKFUNCTION(0x80055F44);
    HandleHumanoidPickupCollisions(humanoidList, pickupList);
}

// PSX: MoveCamera__2AI (AI.CPP:1442, 0x80055F6C)
void AI::MoveCamera() {
    MARKFUNCTION(0x80055F6C);
    if (g_game) {
        Camera& camera = g_game->GetCamera();
#if HIGH_FPS_PLAY_PRESENTATION
        camera.BeginLogicStepHighFPS();
#endif
        camera.Think();
        camera.Update();
    }
}

// PSX: Populate__2AI (AI.CPP:1575, 0x80056214)
// Iterates DB points, meshes, lines, volumes and spawns entities.
// Special case: type==6/subType==0 = player spawn point (sets position).
void AI::Populate() {
    MARKFUNCTION(0x80056214);

    // AI entities and ActiveZones are rebuilt on checkpoint death, but the
    // player's checkpoint object survives. Restore its cleared gate encounters
    // before any door or enemy population registers against the new zones.
    Player* player = Player::s_player;
    World* world = g_game ? g_game->GetWorld() : nullptr;
    if (player) {
        const bool matchingCheckpoint = player->checkpoint.IsValid() && world &&
            player->checkpoint.levelIndex == static_cast<s32>(world->GetCurrentLevelIndex()) &&
            player->checkpoint.petalIndex == static_cast<s32>(world->GetCurrentPetalIndex());
        player->RestoreDoorGateCheckpointProgress(matchingCheckpoint);
    }

    PopulateActiveZones();
    PopulateActiveZonesPaths();
    PopulateActiveZonesSubZones();

    if (!g_database)
        return;

    // PSX: iterate points list - type 6 = spawnable entity, type 110 = NIS world points
    for (DBPoint* pt = g_database->GetFirstPoint(); pt; pt = static_cast<DBPoint*>(pt->next)) {
        if (pt->type == 110) {
            WorldPoints_AddPoint(pt);
            continue;
        }
        if (pt->type != 6) {
            LOG("[AI::Populate] point: type=%u subType=%u name=%s crc=0x%08X (skipped, type!=6)",
                pt->type, pt->subType, pt->GetName() ? pt->GetName() : "null", pt->GetNameCRC());
            continue;
        }

        u16 subType = pt->subType;
        LOG("[AI::Populate] point: type=%u subType=%u name=%s crc=0x%08X pos=(%d,%d,%d)",
            pt->type, subType, pt->GetName() ? pt->GetName() : "null", pt->GetNameCRC(),
            pt->pos.x, pt->pos.y, pt->pos.z);

        if (subType != 0) {
            // PSX: attrib 5 = model name string for entity creation
            const char* modelName = nullptr;
            const DBAttrib* a5 = pt->FindAttrib(5);
            if (a5) {
                modelName = a5->strValue;
            }
            AddThingNoTagList(pt->GetName(), subType, &pt->pos,
                              (const SVector*)&pt->field40, modelName, pt);
        }
        else {
            // Player spawn point (type 6, subType 0)
            // PSX: player must already exist; Populate configures position/orientation.
            if (!Player::s_player) {
                AddThingNoTagList(pt->GetName(), AITypes::TT_PLAYER, &pt->pos,
                                  (const SVector*)&pt->field40, nullptr, pt);
            }

            Player* player = Player::s_player;
            if (player) {
                World* world = g_game ? g_game->GetWorld() : nullptr;
                if (world) {
                    player->Reset();
                }

                player->ClearFloorHeight();
                if (player->checkpoint.IsValid()) {
                    // PSX: checkpoint-valid branch restores position/rotation/block from CheckpointInfo.
                    player->homePos.x = player->checkpoint.field0;
                    player->homePos.y = player->checkpoint.field4;
                    player->homePos.z = player->checkpoint.field8;
                    player->pos = player->homePos;

                    s32 yRot = player->checkpoint.field16;
                    player->SetDesiredMoveDirection(yRot);
                    player->FaceAngleY(yRot, 0);
                    player->blockNum = (u16)player->checkpoint.field24;
                }
                else {
                    player->homePos = pt->pos;
                    player->pos = pt->pos;

                    if (const DBAttrib* a52 = pt->FindAttrib(52)) {
                        if (const char* loadGroupName = a52->GetAttribString()) {
                            player->checkpoint.field44 = (s32)p3dHash(loadGroupName);
                        }
                        else {
                            player->checkpoint.field44 = 0;
                        }
                    }
                    else {
                        player->checkpoint.field44 = 0;
                    }

                    if (const DBAttrib* a53 = pt->FindAttrib(53)) {
                        if (const char* loadGroupName = a53->GetAttribString()) {
                            player->checkpoint.field48 = (s32)p3dHash(loadGroupName);
                        }
                        else {
                            player->checkpoint.field48 = 0;
                        }
                    }
                    else {
                        player->checkpoint.field48 = 0;
                    }

                    s32 yRot = pt->field44;
                    player->SetDesiredMoveDirection(yRot);
                    player->FaceAngleY(yRot, 0);

                    const DBAttrib* a15 = pt->FindAttrib(15);
                    if (a15) {
                        player->blockNum = (u16)a15->value;
                    }
                }

                // PSX mirrors the checkpoint-backed load-group hashes into the
                // live player load-group state during Populate before World
                // later re-syncs those groups onto the player slot.
                SeedPlayerLoadGroups(
                    static_cast<u32>(player->checkpoint.field44),
                    static_cast<u32>(player->checkpoint.field48));

                player->groundStandHeight = player->pos.y;
                player->jumpReturnHeight = player->pos.y;

                player->UpdatePosition();
                player->flags |= TF_ACTIVATED;

                if (g_characterManager) {
                    const s32 desiredMeshType = (g_scoreManager && g_scoreManager->IsDrunkenMasterSuitEnabled()) ? 1 : 0;
                    if (desiredMeshType != *GetPlayerMeshType()) {
                        g_characterManager->ReloadCharacter(0, desiredMeshType, nullptr);

                        Model* model = static_cast<Model*>(player->model);
                        if (model) {
                            SModel* sModel = static_cast<SModel*>(model);
                            if (sModel) {
                                sModel->SetupModelCallbacks();
                            }
                            AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
                            if (anim) {
                                anim->ReAttachTree(0, 0);
                            }
                            model->ApplyAnimToModel(0, 0, 2, 0, 0);
                            player->SetActionState(AS_STAND, 0);
                        }
                    }
                }
            }
        }
    }

    // PSX: iterate meshes list - passes mesh->fileName (+60) as modelName
    for (DBMesh* mesh = g_database->GetFirstMesh(); mesh; mesh = static_cast<DBMesh*>(mesh->next)) {
        if (mesh->type != 6)
            continue;
        AddThingNoTagList(mesh->GetName(), mesh->subType, &mesh->pos,
                          (const SVector*)&mesh->field40, mesh->fileName, mesh);
    }

    // PSX: iterate lines list
    for (DBLine* line = g_database->GetFirstLine(); line; line = static_cast<DBLine*>(line->next)) {
        if (line->type != 6)
            continue;
        AddThingNoTagList(line->GetName(), line->subType, &line->pos,
                          nullptr, nullptr, line);
    }

    // PSX: iterate volumes list
    for (DBVolume* vol = g_database->GetFirstVolume(); vol; vol = static_cast<DBVolume*>(vol->next)) {
        if (vol->type != 6)
            continue;
        if ((vol->subType == AITypes::TT_DOOR || vol->subType == AITypes::TT_TELEPORTER) &&
            SceneVolumeHasMeshTwin(vol, vol->subType)) {
            continue;
        }
        AddThingNoTagList(vol->GetName(), vol->subType, &vol->pos,
                          nullptr, nullptr, vol);
    }
}

// PSX: UnPopulate__2AIs (AI.CPP:1906, 0x800567CC)
// PSX preserves the player when blockNum == 0 (normal level transition).
// When blockNum != 0 (UnloadPermanent), the player is deleted.
void AI::UnPopulate(s16 blockNum) {
    MARKFUNCTION(0x800567CC);

    // PSX: remove player from humanoidList before clearing
    Player* player = Player::s_player;
    if (player) {
        humanoidList.RemNode(static_cast<ccMinNode*>(player));
    }

    // PSX: clear all entity lists
    ccMinNode* n;
    while ((n = activeZoneList.RemHead()) != nullptr) { delete n; }
    while ((n = humanoidList.RemHead()) != nullptr) { delete n; }
    while ((n = inactivePickupList.RemHead()) != nullptr) { delete n; }
    while ((n = pickupList.RemHead()) != nullptr) { delete n; }
    while ((n = moveList.RemHead()) != nullptr) { delete n; }

    // PSX: handle player after list cleanup
    if (player) {
        if (blockNum != 0) {
            // UnloadPermanent path: delete the player
            delete player;
            // ~Player sets s_player = nullptr
        }
        else {
            // Normal level transition: keep player, re-add to humanoidList
            player->DeleteRightHandObj();
            player->DeleteLeftHandObj();
            humanoidList.AddNode(nullptr, static_cast<ccMinNode*>(player));
        }
    }

    // PSX: clear thingList (death staging)
    while ((n = thingList.RemHead()) != nullptr) { delete n; }

    // PSX: clear world points (NIS reference points)
    WorldPoints_Reset();
}

// PSX: PopulateActiveZones__2AI (AI.CPP:1449, 0x80055FC8)
void AI::PopulateActiveZones() {
    MARKFUNCTION(0x80055FC8);
    if (!g_database)
        return;
    for (DBVolume* vol = g_database->GetFirstVolume(); vol; vol = static_cast<DBVolume*>(vol->next)) {
        if (vol->subType == 300) {
            AddActiveZone(vol);
        }
    }
}

// PSX: PopulateActiveZonesPaths__2AI (AI.CPP:1471, 0x80056038)
// Iterates DB paths, finds parent ActiveZone via attrib 7 name hash,
// creates LinearPath, adds to matching ActiveZone.
void AI::PopulateActiveZonesPaths() {
    MARKFUNCTION(0x80056038);
    if (!g_database)
        return;
    for (DBPath* path = g_database->GetFirstPath(); path; path = static_cast<DBPath*>(path->next)) {
        // PSX: check path.points.head exists, then
        // check the first point's type==33 and subType==50
        DBPoint* firstPt = static_cast<DBPoint*>(path->points.head);
        if (!firstPt)
            continue;
        if (firstPt->type != 33 || firstPt->subType != 50)
            continue;

        // PSX: attrib 7 on the first point = parent zone name hash
        const DBAttrib* a7 = firstPt->FindAttrib(7);
        if (!a7)
            continue;
        const char* zoneName = a7->strValue;
        if (!zoneName)
            continue;
        u32 crc = p3dHash(zoneName);

        ActiveZone* az = static_cast<ActiveZone*>(activeZoneList.FindNodeCRC(crc));
        if (!az)
            continue;

        LinearPath* lp = new LinearPath();
        lp->Init(path);
        az->AddLinearPath(lp);

        LOG("[AI::PopulateActiveZonesPaths] zone=%s path=%s pointCount=%u",
            zoneName,
            path->GetName() ? path->GetName() : "null",
            path->pointCount);
    }
}

// PSX: PopulateActiveZonesSubZones__2AI (AI.CPP:1538, 0x80056164)
// Iterates DB volumes with subType 301, creates SubZoneVolume, adds to parent ActiveZone.
void AI::PopulateActiveZonesSubZones() {
    MARKFUNCTION(0x80056164);
    if (!g_database)
        return;
    for (DBVolume* vol = g_database->GetFirstVolume(); vol; vol = static_cast<DBVolume*>(vol->next)) {
        if (vol->subType != 301)
            continue;

        // PSX: attrib 4 = parent zone name hash
        const DBAttrib* a4 = vol->FindAttrib(4);
        if (!a4)
            continue;
        const char* zoneName = a4->strValue;
        if (!zoneName)
            continue;
        u32 crc = p3dHash(zoneName);

        ActiveZone* az = static_cast<ActiveZone*>(activeZoneList.FindNodeCRC(crc));
        if (!az)
            continue;

        SubZoneVolume* szv = new SubZoneVolume(vol);
        az->AddSubZoneVolume(szv);
    }
}

// PSX: PopulateBlock__2AI (AI.CPP:1958, 0x80056A34)
void AI::PopulateBlock() {
    MARKFUNCTION(0x80056A34);
    PopulateBlockHelper(humanoidList);
    PopulateBlockHelper(pickupList);
    PopulateBlockHelper(inactivePickupList);
    PopulateBlockHelper(moveList);
}

// PSX: UnpopulateBlock__2AI (AI.CPP:1979, 0x80056B04)
void AI::UnpopulateBlock() {
    MARKFUNCTION(0x80056B04);
    UnpopulateBlockHelper(humanoidList);
    UnpopulateBlockHelper(pickupList);
    UnpopulateBlockHelper(inactivePickupList);
    UnpopulateBlockHelper(moveList);
}

// PSX: GetPickupWithinReach__2AIP8Humanoid (AI.CPP:1999, 0x80056B04)
Thing* AI::GetPickupWithinReach(Humanoid* humanoid) {
    MARKFUNCTION(0x80056B04);
    if (!humanoid)
        return nullptr;

    LVector reachPt;
    reachPt.x = humanoid->pos.x + (s32)(((s64)rmSin16((s16)humanoid->orientation.y) * 300) >> 16);
    reachPt.y = humanoid->pos.y + 300;
    reachPt.z = humanoid->pos.z + (s32)(((s64)rmSin16((s16)(humanoid->orientation.y + 0x4000)) * 300) >> 16);

    for (ccMinNode* n = inactivePickupList.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        if (thing->DistanceFromPoint(reachPt) < 550) {
            return thing;
        }
    }
    return nullptr;
}

// PSX: CheckObstacleAttack__2AIPP8ObstacleiPC8HumanoidPC27FightingCollisionAttackType
// (AI.CPP:2043, 0x80056BFC)
s32 AI::CheckObstacleAttack(
    Obstacle** outObstacles,
    s32 maxCount,
    const Humanoid* humanoid,
    const FightingCollisionAttackType* attackType) {
    MARKFUNCTION(0x80056BFC);

    if (!humanoid || !attackType) {
        return 0;
    }

    LVector sphereCenters[2] = {};
    tagCollisionSphere spheres[2] = {};

    sphereCenters[0] = attackType->endA;
    spheres[0].radius = attackType->radiusA;

    s32 sphereCount = 1;
    if (attackType->hasSecondary != 0) {
        sphereCenters[1] = attackType->endB;
        spheres[1].radius = attackType->radiusB;
        sphereCount = 2;
    }

    s32 obstacleCount = 0;
    for (ccMinNode* node = moveList.head; node; node = node->next) {
        Obstacle* obstacle = dynamic_cast<Obstacle*>(static_cast<Thing*>(node));
        if (!obstacle) {
            continue;
        }

        if (!obstacle->CareAboutAttack()) {
            continue;
        }

        const s32 xzDistance = PsxAbsS32(obstacle->pos.x - humanoid->pos.x)
            + PsxAbsS32(obstacle->pos.z - humanoid->pos.z);
        if (xzDistance >= 2001) {
            continue;
        }

        for (s32 sphereIndex = 0; sphereIndex < sphereCount; sphereIndex++) {
            if (CheckStaticBoxSphereCollision(
                    obstacle->pos,
                    obstacle->collBox,
                    obstacle->orientation.y,
                    sphereCenters[sphereIndex],
                    spheres[sphereIndex])) {
                if (obstacleCount < maxCount && outObstacles) {
                    outObstacles[obstacleCount] = obstacle;
                }
                obstacleCount++;
            }
        }
    }

    return obstacleCount;
}

// PSX: FindThing__2AIUl (AI.CPP:2321, 0x80056DE4)
Thing* AI::FindThing(u32 id) {
    MARKFUNCTION(0x80056DE4);

    Thing* thing = static_cast<Thing*>(humanoidList.FindNodeCRC(id, nullptr));
    if (!thing) {
        thing = static_cast<Thing*>(pickupList.FindNodeCRC(id, nullptr));
        if (!thing) {
            thing = static_cast<Thing*>(inactivePickupList.FindNodeCRC(id, nullptr));
            if (!thing) {
                // PSX still probes moveList here but returns null regardless.
                moveList.FindNodeCRC(id, nullptr);
                return nullptr;
            }
        }
    }

    return thing;
}

// PSX: ParseBehaviourAttribScript__2AI (AI.CPP:2168, 0x800CA650)
// Opens scr\behave.txt via ccFile, tokenizes by keyword hash,
// builds BehaviourAttrib objects, adds to behaviourList (+100).
// Requires ccFile + BehaviourAttrib class reversal.
void AI::ParseBehaviourAttribScript() {
    MARKFUNCTION(0x800CA650);

    ccFile file;
    if (!file.Open("scr/behave.txt", ccFile::OPEN_READ)) {
        LOG_ALWAYS("[AI] Failed to open: scr/behave.txt");
        return;
    }

    const s32 length = file.GetLength();
    if (length <= 0) {
        file.Close();
        return;
    }

    char* buffer = new char[length];
    if (file.Read(buffer, (u32)length) != length) {
        delete[] buffer;
        file.Close();
        return;
    }

    char token[32] = {};
    char* cursor = buffer;
    const char* end = buffer + length;
    BehaviourAttrib* current = nullptr;
    s32 comboIndex = -1;

    while (cursor < end) {
        cursor = GetNextAlphaNumToken(token, cursor, end);
        if (token[0] == '\0') {
            break;
        }

        switch (p3dHash(token)) {
            case 0x00469BDE: // p3dHash("BEGIN")
                cursor = GetNextAlphaNumToken(token, cursor, end);
                if (token[0] != '\0') {
                    current = new BehaviourAttrib();
                    current->nameCRC = p3dHash(token);
                    comboIndex = -1;
                }
                break;
            case 0x057BDF21: // p3dHash("ATTACKFREQ") -> BehaviourAttrib+0x18 (attackFreq)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->attackFreq = (u16)AlphaToULong(token);
                }
                break;
            case 0x06AC27FE: // p3dHash("AGGRESSION") -> BehaviourAttrib+0x1A (aggression)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->aggression = (u16)AlphaToULong(token);
                }
                break;
            case 0x0866F4A7: // p3dHash("DISTANCING") -> BehaviourAttrib+0x1C (distancing)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->distancing = (u16)AlphaToULong(token);
                }
                break;
            case 0x0528416F: // p3dHash("NCOMBO")
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->comboCount = (u16)AlphaToULong(token);
                    delete[] current->comboChances;
                    current->comboChances = nullptr;
                    if (current->comboStrings) {
                        for (u16 index = 0; index < current->comboCount; index++) {
                            delete[] current->comboStrings[index];
                        }
                        delete[] current->comboStrings;
                        current->comboStrings = nullptr;
                    }
                    if (current->comboCount > 0) {
                        current->comboChances = new u16[current->comboCount]();
                        current->comboStrings = new char*[current->comboCount]();
                        for (u16 index = 0; index < current->comboCount; index++) {
                            current->comboStrings[index] = new char[8]();
                        }
                    }
                    comboIndex = 0;
                }
                break;
            case 0x0048416F: // p3dHash("COMBO")
                if (current && current->comboCount > 0 && comboIndex >= 0 && comboIndex < current->comboCount) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->comboChances[comboIndex] = (u16)AlphaToULong(token);
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    CopyComboToken(current->comboStrings[comboIndex], token);
                    comboIndex++;
                }
                break;
            case 0x0E38C93B: // p3dHash("MINTHINK") -> BehaviourAttrib+0x28 (minThinkFreq)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->minThinkFreq = (u16)AlphaToULong(token);
                }
                break;
            case 0x06D8C93B: // p3dHash("MAXTHINK") -> BehaviourAttrib+0x2A (maxThinkFreq)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->maxThinkFreq = (u16)AlphaToULong(token);
                }
                break;
            case 0x07B27B74: // p3dHash("RUNNINGSPEED") -> BehaviourAttrib+0x2C (runningSpeed)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->runningSpeed = (u16)AlphaToULong(token);
                }
                break;
            case 0x0BAD96F4: // p3dHash("STRAIFINGSPEED") -> BehaviourAttrib+0x2E (strafingSpeed)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->strafingSpeed = (u16)AlphaToULong(token);
                }
                break;
            case 0x0055A278: // p3dHash("PUNCH") -> BehaviourAttrib+0x30 (punchPerc)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->punchPerc = (u16)AlphaToULong(token);
                }
                break;
            case 0x0004FD7B: // p3dHash("KICK") -> BehaviourAttrib+0x32 (kickPerc)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->kickPerc = (u16)AlphaToULong(token);
                }
                break;
            case 0x0058D747: // p3dHash("THROW") -> BehaviourAttrib+0x34 (throwPerc)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->throwPerc = (u16)AlphaToULong(token);
                }
                break;
            case 0x0E680A57: // p3dHash("CIRCLING") -> BehaviourAttrib+0x36 (circling)
                if (current) {
                    cursor = GetNextAlphaNumToken(token, cursor, end);
                    current->circling = (u16)AlphaToULong(token);
                }
                break;
            case 0x00004A24: // p3dHash("END")
                if (current) {
                    behaviourList.AddNodeTail(current);
                    current = nullptr;
                    comboIndex = -1;
                }
                break;
            default:
                break;
        }
    }

    delete[] buffer;
    file.Close();

    LOG_ALWAYS("[AI] behave.txt: %d behaviours", behaviourList.GetNumElements());
}

// PSX: aiPrivHandler (AI.CPP:257, 0x800540E0) - handler callback
void aiPrivHandler(Handler* h) {
    MARKFUNCTION(0x800540E0);
    if (g_ai) {
        g_ai->MoveThings();
    }
}

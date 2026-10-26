#include "ai/generator.h"
#include "pc/autotest.h"
#include "gen/blockmgr.h"
#include "ai/activezn.h"
#include "ai/colfight.h"
#include "ai/humanoid.h"
#include "ai/pickup.h"
#include "ai/player.h"
#include "gen/common.h"
#include "gen/ai.h"
#include "gen/database.h"
#include "p3d/p3dmath.h"
#include "p3d/hash.h"
#include "ai/obstacle_shared.h"

#include <cmath>
#include <cstring>

static bool IsActiveZoneStillRegistered(const ActiveZone* zone) {
    if (!zone || !g_ai) {
        return false;
    }

    for (ccMinNode* node = g_ai->activeZoneList.head; node; node = node->next) {
        if (node == static_cast<const ccMinNode*>(zone)) {
            return true;
        }
    }

    return false;
}

Generator::Generator(const LVector* pos, u16 type)
    : Obstacle(pos, type) {
    MARKFUNCTION(0x80010D54);

    field176 = 0;
    field180 = 0;
    field184 = 0;
    generateType = 0x65;
    field190 = 0;
    generateCount = 0;
    field196 = 0;
    field200 = 0;
    attribArrayCount = 1;
    attribArray = nullptr;
    modelNameBuffer = nullptr;

    dbRoot.AllocatePermanentAttributeArray(20);
}

Generator::~Generator() {
    MARKFUNCTION(0x80010DE8);

    if (modelNameBuffer) {
        delete[] modelNameBuffer;
        modelNameBuffer = nullptr;
    }

    if (attribArray) {
        for (s32 i = 0; i < attribArrayCount; i++) {
            ThingHandle* handle = attribArray[i];
            if (handle) {
                handle->refCount--;
                if (handle->refCount == 0) {
                    delete handle;
                }
            }
        }

        delete[] attribArray;
        attribArray = nullptr;
    }

    dbRoot.DeallocatePermanentAttributeArray();
}

void Generator::GenerateObject(s32 param) {
    MARKFUNCTION(0x80010EEC);

    ThingHandle* oldHandle = attribArray[param];
    if (oldHandle) {
        oldHandle->refCount--;
        if (oldHandle->refCount == 0) {
            delete oldHandle;
        }
    }

    attribArray[param] = nullptr;

    if (field196 < attribArrayCount) {
        Thing* generated = g_ai->AddThingNoTagList(
            nullptr,
            generateType,
            &pos,
            nullptr,
            modelNameBuffer,
            &dbRoot);

        if (!generated) {
            return;
        }

        generated->orientation.x = orientation.x;
        generated->orientation.y = orientation.y;
        generated->orientation.z = orientation.z;

        ThingHandle* newHandle = generated->GetThingHandle();
        attribArray[param] = newHandle;
        newHandle->refCount++;

        generateCount--;
        field196++;
    }
}

void Generator::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x80011018);

    orientation.x = root->field40;
    orientation.y = root->field44;
    orientation.z = root->field48;

    const DBAttrib* attrib5 = root->FindAttrib(5);
    if (modelNameBuffer) {
        delete[] modelNameBuffer;
    }

    const char* modelName = attrib5->GetAttribString();
    const u32 modelLen = static_cast<u32>(std::strlen(modelName));
    modelNameBuffer = new char[modelLen + 1];
    std::strcpy(modelNameBuffer, modelName);

    const DBAttrib* attrib6 = root->FindAttrib(6);
    generateType = static_cast<u16>(attrib6->value);

    const DBAttrib* attrib7 = root->FindAttrib(7);
    field200 = static_cast<s32>(attrib7->value);

    const DBAttrib* attrib8 = root->FindAttrib(8);
    attribArrayCount = static_cast<s32>(attrib8->value);
    attribArray = new ThingHandle*[attribArrayCount];
    for (s32 i = 0; i < attribArrayCount; i++) {
        attribArray[i] = nullptr;
    }

    const DBAttrib* attrib9 = root->FindAttrib(9);
    if (attrib9) {
        field184 = static_cast<s32>(attrib9->value);
        field180 = static_cast<s32>(attrib9->value);
    }

    const DBAttrib* attrib15 = root->FindAttrib(15);
    blockNum = static_cast<u16>(attrib15->value);

    dbRoot.SetName(GetName(), 0);
    dbRoot.pos = pos;
    dbRoot.field40 = orientation.x;
    dbRoot.field44 = orientation.y;
    dbRoot.field48 = orientation.z;

    dbRoot.AddPermanentAttribNumber((u32)field176, 3, (u32)collisionRadius);
    field176++;
    dbRoot.AddPermanentAttribString((u32)field176, 5, modelNameBuffer);
    field176++;
    dbRoot.AddPermanentAttribNumber((u32)field176, 15, (u32)blockNum);
    field176++;
}

void Generator::CreateModel(const char* name) {
    MARKFUNCTION(0x8001121C);
    (void)name;
    flags |= TF_MODEL_CREATED;
}

void Generator::DeleteModel() {
    MARKFUNCTION(0x80011230);
    Thing::DeleteModel();
}

void Generator::Reset() {
    MARKFUNCTION(0x80011250);

    field196 = 0;
    generateCount = field200;

    for (s32 i = 0; i < attribArrayCount; i++) {
        ThingHandle* handle = attribArray[i];
        if (handle) {
            handle->refCount--;
            if (handle->refCount == 0) {
                delete handle;
            }
        }
        attribArray[i] = nullptr;
    }
}

void Generator::Think() {
    MARKFUNCTION(0x80011300);

    for (s32 i = 0; i < attribArrayCount; i++) {
        ThingHandle* handle = attribArray[i];
        if (handle && handle->owner) {
            field180 = 0;
        }
        else {
            field180++;
            if (static_cast<u32>(field180) > static_cast<u32>(field184)) {
                if (generateCount > 0) {
                    field196--;
                    GenerateObject(i);
                }
                else {
                    attribArray[i] = nullptr;
                }
            }
        }
    }
}

void Generator::UpdatePosition() {
    MARKFUNCTION(0x800113F4);
}

void Generator::Trigger() {
    MARKFUNCTION(0x800113FC);
}

void Generator::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x80011404);
}

void Generator::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x8001140C);
}

EnemyGenerator::EnemyGenerator(const LVector* pos, u16 type)
    : Generator(pos, type) {
}

EnemyGenerator::~EnemyGenerator() {
    MARKFUNCTION(0x80012274);
    if (encounterPopulationTracked && encounterSpawnsRemaining > 0 &&
        IsActiveZoneStillRegistered(encounterZone)) {
        encounterZone->RemovePendingEnemySpawns(this, encounterSpawnsRemaining);
    }
}

void EnemyGenerator::GenerateObject(s32 param) {
    MARKFUNCTION(0x80011414);

    (void)param;

    LVector spawnPos = pos;

    if (targetCount != 0) {
        const u32 idx = rmRangedRandom(targetCount);
        SubZoneVolume* subZone = subZoneVolumes[idx];

        if (!subZone || !subZone->IsInSubZoneVolume(Player::s_player)) {
            spawnPos = spawnPoints[idx];
        }
    }

    Thing* generated = g_ai->AddThingNoTagList(
        nullptr,
        generateType,
        &spawnPos,
        nullptr,
        modelNameBuffer,
        &dbRoot);

    if (AutoTest_TraceSpawns()) {
        Humanoid* h = static_cast<Humanoid*>(generated);
        LOG_ALWAYS("[Spawn] gen 0x%08X spawned %p type=%u at (%d,%d,%d) posBlock=%u thingBlock=%u flags=0x%X zone=%p aiParam=%d zoneMembers=%d made=%d/%d",
                   nameCRC, (void*)generated, (u32)generateType, spawnPos.x, spawnPos.y, spawnPos.z,
                   g_blockManager ? (u32)g_blockManager->GetBlockNumber(spawnPos) : 0u,
                   generated ? (u32)generated->blockNum : 0u, generated ? generated->flags : 0u,
                   generated ? (void*)h->activeZone : nullptr, generated ? h->field452 : 0,
                   activeZone ? (s32)activeZone->memberCount : -1, generateCount + 1, field200);
    }

    if (!generated) {
        return;
    }

    generated->orientation.x = 0;
    generated->orientation.y = orientation.y;
    generated->orientation.z = 0;

    generateCount++;
    FightingCollision::InsertHumanoid(static_cast<Humanoid*>(generated));

    if (encounterPopulationTracked && encounterSpawnsRemaining > 0) {
        if (IsActiveZoneStillRegistered(encounterZone)) {
            encounterZone->ResolvePendingEnemySpawn(this);
        }
        encounterSpawnsRemaining--;
    }
}

void EnemyGenerator::Reset() {
    MARKFUNCTION(0x8001157C);

    if (encounterPopulationTracked && IsActiveZoneStillRegistered(encounterZone)) {
        const s32 pendingDelta = field200 - encounterSpawnsRemaining;
        if (pendingDelta > 0) {
            encounterZone->RegisterPendingEnemySpawns(this, pendingDelta);
        }
        else if (pendingDelta < 0) {
            encounterZone->RemovePendingEnemySpawns(this, -pendingDelta);
        }
        encounterSpawnsRemaining = field200;
    }

    field296 = 0;
    activeZone = nullptr;
    generateCount = 0;
}

void EnemyGenerator::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8001158C);

    orientation.x = 0;
    orientation.y = root->field44;
    orientation.z = 0;

    Obstacle::AnalyzeMesh(root);

    const DBAttrib* attrib6 = root->FindAttrib(6);
    if (attrib6) {
        generateType = static_cast<u16>(attrib6->value);
    }
    else {
        generateType = 1;
    }

    const DBAttrib* attrib7 = root->FindAttrib(7);
    if (attrib7) {
        field200 = static_cast<s32>(attrib7->value);
    }
    else {
        field200 = 1;
    }

    const DBAttrib* attrib8 = root->FindAttrib(8);
    if (attrib8) {
        field292 = static_cast<s32>(attrib8->value);
    }
    else {
        field292 = 1;
    }

    const DBAttrib* attrib9 = root->FindAttrib(9);
    if (attrib9) {
        field288 = static_cast<s32>(attrib9->value);
    }
    else {
        field288 = 0;
    }

    const DBAttrib* attrib10 = root->FindAttrib(10);
    if (attrib10) {
        activeZoneHash = p3dHash(attrib10->GetAttribString());
    }
    else {
        activeZoneHash = 0;
    }

    const DBAttrib* attrib11 = root->FindAttrib(11);
    if (attrib11) {
        SetupTargets(attrib11->GetAttribString());
    }
    else {
        targetCount = 0;
    }

    dbRoot.pos = pos;
    dbRoot.field40 = orientation.x;
    dbRoot.field44 = orientation.y;
    dbRoot.field48 = orientation.z;

    for (u32 i = 0; i < root->attribCount; i++) {
        const DBAttrib* attr = root->GetAttribByIndex(i);
        if (!attr) {
            continue;
        }

        const u32 attrId = attr->id;
        if (attrId < 12 && attrId != 3 && attrId != 5) {
            continue;
        }

        if (attr->type == 1) {
            dbRoot.AddPermanentAttribNumber((u32)field176, attrId, attr->value);
        }
        else {
            dbRoot.AddPermanentAttribString((u32)field176, attrId, attr->GetAttribString());
        }

        field176++;
    }

    encounterZone = (g_ai && activeZoneHash != 0)
        ? static_cast<ActiveZone*>(g_ai->activeZoneList.FindNodeCRC(activeZoneHash, nullptr))
        : nullptr;
    if (field200 > 0 && encounterZone && IsActiveZoneStillRegistered(encounterZone)) {
        if (!encounterPopulationTracked) {
            encounterPopulationTracked = true;
        }
        const s32 pendingDelta = field200 - encounterSpawnsRemaining;
        if (pendingDelta > 0) {
            encounterZone->RegisterPendingEnemySpawns(this, pendingDelta);
        }
        else if (pendingDelta < 0) {
            encounterZone->RemovePendingEnemySpawns(this, -pendingDelta);
        }
        encounterSpawnsRemaining = field200;
    }
}

void EnemyGenerator::Think() {
    MARKFUNCTION(0x800117D8);

    if (!activeZone) {
        if (g_ai) {
            activeZone = static_cast<ActiveZone*>(g_ai->activeZoneList.FindNodeCRC(activeZoneHash, nullptr));
        }
        if (AutoTest_TraceSpawns()) {
            LOG_ALWAYS("[Spawn] gen 0x%08X first think: pos=(%d,%d,%d) block=%u type=%u total=%d burst=%d threshold=%d zoneHash=0x%08X zone=%p targets=%u",
                       nameCRC, pos.x, pos.y, pos.z, (u32)blockNum, (u32)generateType, field200, field292, field288,
                       activeZoneHash, (void*)activeZone, targetCount);
            LOG_ALWAYS("[Spawn]   playerBlock=%u genBlockActive=%d", Player::s_player ? (u32)Player::s_player->blockNum : 0u,
                       (g_blockManager && g_blockManager->InActiveList(blockNum)) ? 1 : 0);
        }

        for (u32 i = 0; i < 3; i++) {
            if (zoneHashes[i] != 0 && activeZone) {
                subZoneVolumes[i] = static_cast<SubZoneVolume*>(activeZone->subZoneList.FindNodeCRC(zoneHashes[i], nullptr));
            }
            else {
                subZoneVolumes[i] = nullptr;
            }
        }
    }

    if (field296) {
        if (field292 > 0) {
            s32 generatedThisStep = 0;
            while (generatedThisStep < field292 && generatedThisStep < (field200 - generateCount)) {
                GenerateObject(0);
                generatedThisStep++;
            }
        }

        field296 = 0;
    }
    else if (activeZone) {
        if ((s32)activeZone->memberCount <= field288 && generateCount <= field200) {
            field296 = 1;
        }
    }
}

void EnemyGenerator::SetupTargets(const char* pointNames) {
    MARKFUNCTION(0x80011914);

    const u32 nameLen = static_cast<u32>(std::strlen(pointNames));
    char* tempNames = new char[nameLen + 1];
    std::strcpy(tempNames, pointNames);

    char* cursor = tempNames;
    targetCount = 0;

    while (targetCount < 3 && cursor && *cursor) {
        char* comma = std::strchr(cursor, ',');
        char* next = comma ? (comma + 1) : nullptr;
        if (comma) {
            *comma = '\0';
        }

        DBPoint* point = g_database ? g_database->FindPoint(cursor) : nullptr;
        if (point) {
            spawnPoints[targetCount].x = point->pos.x;
            spawnPoints[targetCount].y = point->pos.y;
            spawnPoints[targetCount].z = point->pos.z;

            const DBAttrib* attr = point->FindAttrib(1);
            if (attr) {
                zoneHashes[targetCount] = p3dHash(attr->GetAttribString());
            }
            else {
                zoneHashes[targetCount] = 0;
            }

            targetCount++;
        }

        cursor = next;
    }

    delete[] tempNames;
}

ThrowingGenerator::ThrowingGenerator(const LVector* pos, u16 type)
    : Generator(pos, type) {
}

ThrowingGenerator::~ThrowingGenerator() {
    MARKFUNCTION(0x8001224C);
}

void ThrowingGenerator::GenerateObject(s32 param) {
    MARKFUNCTION(0x80011A80);

    const s32 slot = param;

    s32 playerMinY = 0x7FFF;
    s32 playerMinZ = -0x7FFF;
    if (field236) {
        Player* player = Player::s_player;
        playerMinY = player->collBboxMin.y;
        playerMinZ = player->collBboxMin.z;
    }
    else {
        playerMinY = 0;
        playerMinZ = 0;
    }

    ThingHandle* oldHandle = attribArray[slot];
    if (oldHandle) {
        oldHandle->refCount--;
        if (oldHandle->refCount == 0) {
            delete oldHandle;
        }
    }

    attribArray[slot] = nullptr;

    Thing* generated = g_ai->AddThingNoTagList(
        nullptr,
        generateType,
        &pos,
        nullptr,
        modelNameBuffer,
        &dbRoot);

    if (!generated) {
        return;
    }

    generated->orientation.x = orientation.x;
    generated->orientation.y = orientation.y;
    generated->orientation.z = orientation.z;

    ThingHandle* newHandle = generated->GetThingHandle();
    attribArray[slot] = newHandle;
    newHandle->refCount++;

    const s32 range = field252;
    const s32 randX = static_cast<s32>(rmRangedRandom(static_cast<u32>(range << 1))) - range;
    const s32 randZ = static_cast<s32>(rmRangedRandom(static_cast<u32>(range << 1))) - range;

    const s32 dx = (playerTrackX - pos.x) + randX;
    const s32 dz = (playerTrackZ - pos.z) + randZ;
    const s32 mag = static_cast<s32>(rmMag2(static_cast<f32>(dx), static_cast<f32>(dz)));

    const s32 pair = playerMinY + playerMinZ;
    const s32 halfPair = (pair + static_cast<s32>(static_cast<u32>(pair) >> 31)) >> 1;
    const s32 vertical = (field244 - pos.y) + halfPair;

    const s32 div5 = (mag - vertical) / 5;
    const u32 sqrtArg = static_cast<u32>(div5) << 16;
    const s32 sqrtValue = static_cast<s32>(std::sqrt(static_cast<f64>(sqrtArg)));
    const s32 scale = rmDiv16i(mag, sqrtValue << 8);

    s32 velX = 0;
    s32 velZ = 0;
    if (mag != 0) {
        velX = static_cast<s32>(static_cast<s64>(dx) * static_cast<s64>(scale)) / mag;
        velZ = static_cast<s32>(static_cast<s64>(dz) * static_cast<s64>(scale)) / mag;
    }

    DynamicThing* dynamic = static_cast<DynamicThing*>(generated);
    dynamic->velocity.x = velX;
    dynamic->velocity.y = scale;
    dynamic->velocity.z = velZ;
    dynamic->gravity = 0;
    dynamic->maxFallDivisor = 10;

    generateCount--;
    field196++;
}

bool ThrowingGenerator::TargetInFOF() const {
    MARKFUNCTION(0x80011DC8);
    const s32 angle = static_cast<s32>(static_cast<s16>(
        rmATan216((s32)((playerTrackX - pos.x) << 16), (s32)((playerTrackZ - pos.z) << 16))));
    return angle >= fofAngleStart && angle <= fofAngleEnd;
}

void ThrowingGenerator::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x80011E34);

    Obstacle::AnalyzeMesh(root);

    orientation.x = root->field40;
    orientation.y = root->field44;
    orientation.z = root->field48;

    attribArrayCount = 1;
    field200 = 1;
    generateCount = 1;

    const DBAttrib* attrib5 = root->FindAttrib(5);
    const char* modelName = attrib5->GetAttribString();
    const u32 modelLen = static_cast<u32>(std::strlen(modelName));

    if (modelNameBuffer) {
        delete[] modelNameBuffer;
    }

    modelNameBuffer = new char[modelLen + 1];
    std::strcpy(modelNameBuffer, modelName);

    attribArray = new ThingHandle*[1];
    attribArray[0] = nullptr;

    const DBAttrib* attrib6 = root->FindAttrib(6);
    field216 = attrib6 ? static_cast<s32>(attrib6->value) : 30;

    const DBAttrib* attrib7 = root->FindAttrib(7);
    field220 = attrib7 ? static_cast<s32>(attrib7->value) : 0;

    const DBAttrib* attrib8 = root->FindAttrib(8);
    if (attrib8) {
        const s32 raw = static_cast<s32>(attrib8->value);
        const s32 half = (raw + static_cast<s32>(static_cast<u32>(raw) >> 31)) >> 1;
        const s32 scaled = half << 16;
        const s64 mul = static_cast<s64>(scaled) * static_cast<s64>(static_cast<s32>(0xB60B60B7u));
        const s32 hi = static_cast<s32>(mul >> 32);
        const s32 converted = ((hi + scaled) >> 8) - (scaled >> 31);

        fofAngleStart = orientation.y - converted;
        fofAngleEnd = orientation.y + converted;
    }
    else {
        fofAngleStart = -0x8000;
        fofAngleEnd = 0x8000;
    }

    const DBAttrib* attrib9 = root->FindAttrib(9);
    field236 = attrib9 ? static_cast<s32>(attrib9->value) : 1;

    if (!field236) {
        const DBAttrib* attrib10 = root->FindAttrib(10);
        playerTrackX = attrib10 ? static_cast<s32>(attrib10->value) : 0;

        const DBAttrib* attrib11 = root->FindAttrib(11);
        field244 = attrib11 ? static_cast<s32>(attrib11->value) : 0;

        const DBAttrib* attrib12 = root->FindAttrib(12);
        playerTrackZ = attrib12 ? static_cast<s32>(attrib12->value) : 0;
    }

    const DBAttrib* attrib13 = root->FindAttrib(13);
    field252 = attrib13 ? static_cast<s32>(attrib13->value) : 0;
}

void ThrowingGenerator::Reset() {
    MARKFUNCTION(0x800120F4);

    generateCount = 0;
    field196 = 0;
    field224 = 0;
}

void ThrowingGenerator::Think() {
    MARKFUNCTION(0x80012104);

    if (field236) {
        Player* player = Player::s_player;
        playerTrackX = player->pos.x;
        field244 = player->pos.y;
        playerTrackZ = player->pos.z;
    }

    Thing* spawned = nullptr;
    ThingHandle* handle = attribArray[0];
    if (handle) {
        spawned = handle->owner;
    }

    if (spawned) {
        if (spawned->flags & TF_ON_GROUND) {
            static_cast<Pickup*>(spawned)->PlayEffect();
            spawned->Kill();
        }
        else {
            DynamicThing* dynamic = static_cast<DynamicThing*>(spawned);
            dynamic->gravity = 0;
            dynamic->maxFallDivisor = 10;
        }

        return;
    }

    if (playerTrackX != 0 || field244 != 0 || playerTrackZ != 0) {
        if (field224 <= 0) {
            const bool inFOF = TargetInFOF();
            if (inFOF) {
                GenerateObject(0);
                field224 = field220;
            }
        }
        else {
            field224--;
        }
    }
}

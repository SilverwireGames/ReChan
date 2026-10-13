#include "ai/kicknroll.h"
#include "ai/humanoid.h"
#include "ai/pickup.h"
#include "ai/player.h"
#include "gen/common.h"
#include "gen/ai.h"
#include "gen/animmgr.h"
#include "gen/animstruct.h"
#include "gen/database.h"
#include "gen/geffect.h"
#include "gen/model.h"
#include "gen/scoremgr.h"
#include "gen/colsect.h"
#include "gen/blockmgr.h"
#include "gen/game.h"
#include "gen/time.h"
#include "p3d/hash.h"
#include "p3d/p3dmath.h"
#include "ai/obstacle_shared.h"
#include "snd/kndnsnd.h"
#include "snd/kicksnd.h"
#include "snd/rsevent.h"
#include "snd/sndfact.h"
#include "p3d/skeleton.h"
#include <cstdio>

static constexpr s32 KNOCKDOWN_COLLISION_TAG_IMPACT_REGION = static_cast<s32>(0x80000002u);
static constexpr s32 KNOCKDOWN_COLLISION_TAG_HIT_TYPE = static_cast<s32>(0x80000003u);
static constexpr s32 KNOCKDOWN_COLLISION_TAG_DAMAGE = static_cast<s32>(0x80000007u);
static constexpr s32 KNOCKDOWN_COLLISION_TAG_END = 0;
static constexpr u32 KNOCKDOWN_DEFAULT_EFFECT_HASH = 0x065C8E90;

static LVector KnockDownLocalDominantDirection(const KnockDown& knockDown, const LVector& point) {
    LVector direction = {
        point.x - knockDown.pos.x,
        point.y - knockDown.pos.y,
        point.z - knockDown.pos.z
    };

    const s32 absX = direction.x < 0 ? -direction.x : direction.x;
    const s32 absZ = direction.z < 0 ? -direction.z : direction.z;
    const s32 dominantX = absZ < absX ? direction.x : 0;
    const s32 dominantZ = absX < absZ ? direction.z : 0;
    direction.x = dominantX;
    direction.z = dominantZ;

    switch (static_cast<u16>(knockDown.field132)) {
        case 0x4000:
            direction.x = -dominantZ;
            direction.y = dominantX;
            direction.z = dominantX;
            break;
        case 0x8000:
            direction.x = -dominantX;
            direction.z = -dominantZ;
            break;
        case 0xC000:
            direction.x = dominantZ;
            direction.y = -dominantX;
            direction.z = -dominantX;
            break;
        default:
            break;
    }

    return direction;
}

KickNRoll::KickNRoll(const LVector* pos, u16 type)
    : Obstacle(pos, type) {
    MARKFUNCTION(0x8001C398);
    sound = nullptr;
    aliveFlag = 1;
}

KickNRoll::~KickNRoll() {
    MARKFUNCTION(0x8001C3DC);

    if (sound) {
        delete sound;
        sound = nullptr;
    }
}

void KickNRoll::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8001C444);
    orientation.x = root->field40;
    orientation.y = root->field44;
    orientation.z = root->field48;
    Obstacle::AnalyzeMesh(root);
    tagCollisionBox localBox = INVALID_COLLISION_BOX;
    ObstacleFillCollisionBox(localBox, root, 5);
    SetCollisionBox(localBox);

    // PSX rotates template {0,0,0x10000}, then scales by (attrib7 / 100) / halfHeight.
    Mat4 rotMatrix;
    p3dBuildRotMatrixXYZ((s32)orientation.x, (s32)orientation.y, (s32)orientation.z, rotMatrix);
    Vec3 vel = p3dVecTimesRotMatrix(Vec3(0.0f, 0.0f, (f32)FIX16_ONE), rotMatrix);
    field136 = (s32)vel.x;
    field140 = (s32)vel.y;
    field144 = (s32)vel.z;
    if (const DBAttrib* a7 = root->FindAttrib(7)) {
        field116 = rmDiv16i((s32)((u32)a7->GetAttribValue() << 16), 100 << 16);
        const s32 halfHeight = Div2TowardZero((s32)localBox.maxY - (s32)localBox.minY);
        if (halfHeight != 0) {
            const s32 speedScale = rmDiv16i(field116, halfHeight << 16);
            field136 = (s32)(((s64)field136 * (s64)speedScale) >> 16);
            field140 = (s32)(((s64)field140 * (s64)speedScale) >> 16);
            field144 = (s32)(((s64)field144 * (s64)speedScale) >> 16);
        }
    }

    // Attrib 20: optional destruction particle effect
    if (const DBAttrib* a = root->FindAttrib(20)) {
        const char* name = a->GetAttribString();
        if (name) {
            effectHash = p3dHash(name);
        }
    }
}

void KickNRoll::CreateModel(const char* name) {
    MARKFUNCTION(0x8001C6FC);
    Obstacle::CreateModel(name);

    if (!sound) {
        CSound* soundObj = nullptr;
        if (CSoundFactory::CreateObject(10030, &soundObj, modelHash) >= 0) {
            sound = static_cast<CKickNRollSound*>(soundObj);
            if (sound) {
                sound->Initialize(&pos);
            }
        }
    }
}

void KickNRoll::DeleteModel() {
    MARKFUNCTION(0x8001C750);
    Obstacle::DeleteModel();

    if (sound) {
        delete sound;
        sound = nullptr;
    }
}

void KickNRoll::Reset() {
    MARKFUNCTION(0x8001C7A0);
}

void KickNRoll::Think() {
    MARKFUNCTION(0x8001C7B8);
    Move();
    MovePassengers();
}

void KickNRoll::Move() {
    MARKFUNCTION(0x8001C850);

    // Capture old position before applying velocity
    LVector oldPos = pos;

    const s32 dirFlag = ((s16)rollTimer != 0) ? (field120 < 1) : field120;

    if (!aliveFlag) {
        return;
    }

    if (dirFlag) {
        pos.x += velX;
        pos.z += velZ;
    }
    else {
        pos.x -= velX;
        pos.z -= velZ;
    }
    pos.y += velY;

    // Resolve environment collisions against the previous position.
    if (HandleEnvironmentCollision(oldPos)) {
        if (breakable) {
            Destroy();
            return;
        }
        rollTimer = 8;
    }

    // Update visual roll rotation when moving horizontally.
    if (velX || velZ) {
        const s32 mag = (s32)rmMag3((f32)(velX << 16), 0.0f, (f32)(velZ << 16));
        const s32 height = (s32)collBox.maxY - (s32)collBox.minY;
        const s32 halfHeightFp = Div2TowardZero(height) << 16;
        if (halfHeightFp != 0) {
            s32 rotDelta = (s32)(((s64)(rmDiv16i(mag, halfHeightFp) << 16)) / 411774);
            if (rotDelta < 0) rotDelta = -rotDelta;
            if (dirFlag) {
                orientation.x += rotDelta;
            } else {
                orientation.x -= rotDelta;
            }
        }
    }

    // Stop rolling when the wall-bounce timer reaches 1.
    if (rollTimer == 1) {
        velZ = 0;
        velX = 0;
        if (sound) {
            sound->EndRoll();
        }
    }
    if ((s16)rollTimer > 0) {
        rollTimer--;
    }

    velY -= 9;

    if (g_blockManager) {
        blockNum = (u16)g_blockManager->GetBlockNumber(pos);
    }

    if (sound) {
        sound->Think();
    }
}

bool KickNRoll::HandleEnvironmentCollision(LVector& prevPos) {
    MARKFUNCTION(0x8001CB60);

    // Horizontal collision radius: half of the widest horizontal span.
    const s32 xSpan = (s32)collBox.maxX - (s32)collBox.minX;
    const s32 zSpan = (s32)collBox.maxZ - (s32)collBox.minZ;
    const s32 maxSpan = (xSpan < zSpan) ? zSpan : xSpan;
    const s32 radius = Div2TowardZero(maxSpan);

    // Half-height used as the Y extent for wall/floor checks.
    const s32 halfHeight = Div2TowardZero((s32)collBox.maxY - (s32)collBox.minY);

    // Current position becomes the movement endpoint; prevPos is the start.
    LVector newPos = pos;
    bool hitWall = false;

    // Iterate wall collision up to 2 times to resolve corner cases.
    for (s32 iter = 0; iter < 2; ++iter) {
        s32 wallRatio = 0;
        LVector wallNormal = {};
        LVector wallHitPoint = {};
        s32 wallHorizontal = 0;
        if (!CollisionSector::CheckWorldWallCollision(
                prevPos, newPos, radius,
                -halfHeight, halfHeight,
                wallRatio, wallNormal, wallHitPoint, wallHorizontal)) {
            break;
        }

        // CorrectCollision: find contact point and reflect new position off the wall.
        // contact = lerp(prevPos, newPos, wallRatio)
        LVector contact;
        contact.x = prevPos.x + (s32)(((s64)wallRatio * (s64)(newPos.x - prevPos.x)) >> 16);
        contact.y = prevPos.y + (s32)(((s64)wallRatio * (s64)(newPos.y - prevPos.y)) >> 16);
        contact.z = prevPos.z + (s32)(((s64)wallRatio * (s64)(newPos.z - prevPos.z)) >> 16);

        const s32 nx = wallNormal.x;
        const s32 ny = wallNormal.y;
        const s32 nz = wallNormal.z;

        s32 projDist = (s32)(((s64)nx * (s64)(newPos.x - contact.x)) >> 16)
                     + (s32)(((s64)ny * (s64)(newPos.y - contact.y)) >> 16)
                     + (s32)(((s64)nz * (s64)(newPos.z - contact.z)) >> 16)
                     - 2;

        LVector reflected;
        reflected.x = newPos.x - (s32)(((s64)nx * (s64)projDist) >> 16);
        reflected.y = newPos.y - (s32)(((s64)ny * (s64)projDist) >> 16);
        reflected.z = newPos.z - (s32)(((s64)nz * (s64)projDist) >> 16);

        // Nudge contact point away from wall.
        contact.x += (s32)((2LL * nx) >> 16);
        contact.y += (s32)((2LL * ny) >> 16);
        contact.z += (s32)((2LL * nz) >> 16);

        // Update prevPos (x/z only) with the contact point and newPos with the reflected position.
        prevPos.x = contact.x;
        prevPos.z = contact.z;
        newPos.x = reflected.x;
        newPos.z = reflected.z;
        hitWall = true;
    }

    // Floor collision at both old and new horizontal positions.
    LVector floorNormalOld = {}, ceilNormalOld = {};
    s32 floorHOld = 0, ceilHOld = 0;
    LVector oldSearch = { prevPos.x, prevPos.y, prevPos.z };
    CollisionSector::GetWorldFloorAndCeilingHeight(
        floorHOld, ceilHOld, floorNormalOld, ceilNormalOld, oldSearch, radius / 2);

    LVector floorNormalNew = {}, ceilNormalNew = {};
    s32 floorHNew = 0, ceilHNew = 0;
    LVector newSearch = { newPos.x, prevPos.y, newPos.z };
    CollisionSector::GetWorldFloorAndCeilingHeight(
        floorHNew, ceilHNew, floorNormalNew, ceilNormalNew, newSearch, radius / 2);

    // Compute approach depth along the floor normal from velocity.
    s32 approachOld = 0;
    if (floorNormalOld.y != 0) {
        approachOld = rmDiv16i(
            (s32)(((s64)floorNormalOld.x * (s64)velX) >> 16) + (s32)(((s64)floorNormalOld.z * (s64)velZ) >> 16),
            floorNormalOld.y) + 2;
    }

    s32 approachNew = 0;
    if (floorNormalNew.y != 0) {
        approachNew = rmDiv16i(
            (s32)(((s64)floorNormalNew.x * (s64)velX) >> 16) + (s32)(((s64)floorNormalNew.z * (s64)velZ) >> 16),
            floorNormalNew.y) + 2;
    }

    if (approachNew < approachOld) approachNew = approachOld;
    if (approachNew < 0) approachNew = 0;

    // PSX sentinel: floor height > 0x80000001 means a real floor was found.
    const s32 floorSurface = floorHNew + halfHeight;
    if (floorHNew > (s32)0x80000001 && prevPos.y >= floorSurface - 640 && pos.y < floorSurface + approachNew) {
        newPos.y = floorSurface + 1;
        if (velY < 0) {
            velY = 0;
        }
    }

    pos.x = newPos.x;
    pos.y = newPos.y;
    pos.z = newPos.z;
    return hitWall;
}

void KickNRoll::Destroy() {
    MARKFUNCTION(0x8001CEEC);
    aliveFlag = 0;

    if (effectHash) {
        LVector center = {};
        FillBoxCentre(center, pos, orientation, collBox);
        GEffect_Create(effectHash, &center, nullptr, nullptr, 0, 0, effectParam);
    }

    extern const tagCollisionBox INVALID_COLLISION_BOX;
    SetCollisionBox(INVALID_COLLISION_BOX);
}

void KickNRoll::UpdatePosition() {
    MARKFUNCTION(0x8001CF74);
}

void KickNRoll::Draw() {
    MARKFUNCTION(0x8001CF7C);
    Obstacle::Draw();
}

void KickNRoll::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x8001CFAC);
    Pickup* p = static_cast<Pickup*>(pickup);
    p->PlayEffect();
    p->Kill();
}

void KickNRoll::MovePassengers() {
    MARKFUNCTION(0x8001CFEC);
    MovePassengersBasic();
}

void KickNRoll::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x8001D178);

    LVector correctedPos = {};
    LVector correctionNormal = {};
    LVector pushedPos = {};

    CorrectThingPositionObstacle(
        pos, pos,
        orientation.y, orientation.y,
        collBox,
        hum->pos, hum->homePos,
        hum->collBboxMin.x, hum->collBboxMin.y, hum->collBboxMin.z,
        correctedPos, correctionNormal, pushedPos);

    hum->homePos = correctedPos;

    if (correctionNormal.y > 0) {
        hum->SetFloorHeight(pos.y + (s32)collBox.maxY);
        AddPassenger(hum);
    } else {
        const s32 directionSign = field120 ? 1 : -1;
        const bool movingIntoHumanoid =
            (((s64)velX * (s64)correctionNormal.x * (s64)directionSign) > 0)
            || (((s64)velZ * (s64)correctionNormal.z * (s64)directionSign) > 0);

        if (!movingIntoHumanoid) {
            return;
        }

        hum->HandleCollision(this, 1,
            static_cast<s32>(0x80000002u), 4,
            static_cast<s32>(0x80000005u), 10000,
            static_cast<s32>(0x80000003u), 15,
            static_cast<s32>(0x80000007u), field160,
            0);
        if (sound) {
            sound->HitHumanoid();
        }
    }
}

void KickNRoll::HandleAttack(Humanoid* attacker, s32 damageType, s32 attackMagnitude, s32 damage) {
    MARKFUNCTION(0x8001D45C);
    (void)attackMagnitude;
    (void)damage;

    // Only kicks (damageType == 4) set this barrel rolling.
    if (damageType != 4) {
        return;
    }

    // PSX: compare normalized kick direction with the preset roll direction.
    const s32 dx = pos.x - attacker->pos.x;
    const s32 dz = pos.z - attacker->pos.z;
    const s32 mag = (s32)rmMag2((f32)dx, (f32)dz);
    const s32 speedMag = (s32)rmMag2((f32)field136, (f32)field144);

    s32 dot = 0;
    if (mag > 0 && speedMag > 0) {
        const s32 ndx = rmDiv16i(dx, mag);
        const s32 ndz = rmDiv16i(dz, mag);
        const s32 normSx = rmDiv16i(field136, speedMag);
        const s32 normSz = rmDiv16i(field144, speedMag);
        dot = (s32)(((s64)ndx * (s64)normSx) >> 16) + (s32)(((s64)ndz * (s64)normSz) >> 16);
        const s32 absDot = (dot < 0) ? -dot : dot;
        if (absDot <= 0x7FFF) {
            return;
        }
    }

    const bool wasRolling = (velX || velZ);
    velX = field136;
    velY = field140;
    velZ = field144;
    field120 = (dot > 0) ? 1 : 0;
    rollTimer = 0;

    if (!wasRolling && sound) {
        sound->Kick();
        sound->BeginRoll();
    }
}

bool KickNRoll::CareAboutAttack() const {
    MARKFUNCTION(0x8001D454);
    return true;
}

KnockDown::KnockDown(const LVector* pos, u16 type)
    : Obstacle(pos, type) {
    MARKFUNCTION(0x8001D5D4);
    field116 = 0;
    field168 = 0;
    field172 = 0;
    field184 = nullptr;
    aliveFlag = 1;
}

KnockDown::~KnockDown() {
    MARKFUNCTION(0x8001D650);

    if (field184) {
        delete field184;
        field184 = nullptr;
    }
}

void KnockDown::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8001D6B8);
    Obstacle::AnalyzeMesh(root);

    field128 = root->field40;
    field132 = root->field44;
    field136 = root->field48;
    while (field132 > 0xFF49) field132 -= 0x10000;
    while (field132 < 0) field132 += 0x10000;

    field140 = field128;
    field144 = field132;
    field148 = field136;
    orientation = { field128, field132, field136 };

    tagCollisionBox localBox = INVALID_COLLISION_BOX;
    ObstacleFillCollisionBox(localBox, root, 5);
    SetCollisionBox(localBox);
    savedCollBox = localBox;

    if (const DBAttrib* attr6 = root->FindAttrib(6)) {
        const char* name = attr6->GetAttribString();
        field168 = name ? static_cast<s32>(p3dHash(name)) : 0;
    }

    if (const DBAttrib* attr7 = root->FindAttrib(7)) {
        const char* name = attr7->GetAttribString();
        field172 = name ? static_cast<s32>(p3dHash(name)) : 0;
    } else {
        field172 = static_cast<s32>(KNOCKDOWN_DEFAULT_EFFECT_HASH);
    }

    if (const DBAttrib* attr8 = root->FindAttrib(8)) {
        field124 = static_cast<s32>((static_cast<s64>(static_cast<s32>(attr8->GetAttribValue())) * 0x10000) / 360);
    } else {
        field124 = 0xB6;
    }

    field176 = root->FindAttrib(9) ? static_cast<s32>(root->FindAttrib(9)->GetAttribValue()) : 50;
    field180 = root->FindAttrib(10) ? static_cast<s32>(root->FindAttrib(10)->GetAttribValue()) : 0;
}

void KnockDown::CreateModel(const char* name) {
    MARKFUNCTION(0x8001D8D8);
    Obstacle::CreateModel(name);

    if (!field184) {
        CSound* soundObject = nullptr;
        if (CSoundFactory::CreateObject(0x2792, &soundObject, 0) >= 0) {
            field184 = static_cast<CKnockDownSound*>(soundObject);
            if (field184) field184->Initialize(&pos);
        }
    }
}

void KnockDown::Draw() {
    MARKFUNCTION(0x8001D92C);
    Obstacle::Draw();
}

void KnockDown::DeleteModel() {
    MARKFUNCTION(0x8001D9A8);
    Obstacle::DeleteModel();

    if (field184) {
        delete field184;
        field184 = nullptr;
    }
}

void KnockDown::Reset() {
    MARKFUNCTION(0x8001D9F8);
    field116 = 0;
    field120 = 0;
    field140 = field128;
    field144 = field132;
    field148 = field136;
    orientation = { field128, field132, field136 };
    SetCollisionBox(savedCollBox);
    aliveFlag = 1;
}

void KnockDown::Think() {
    MARKFUNCTION(0x8001DA48);

    if (!aliveFlag) {
        return;
    }

    if (field116 == 1) {
        Move();
        UpdateCollisionBox();
    } else if (field116 == 2) {
        if (field172) {
            GEffect_Create(static_cast<u32>(field172), &pos, nullptr, nullptr, 0, 0, 0);
        }
        if (field184) {
            field184->EndFall();
            field184->Impact();
        }
        if (field168 && g_ai) {
            ccNode* target = g_ai->moveList.FindNodeCRC(static_cast<u32>(field168), nullptr);
            if (target) {
                static_cast<Obstacle*>(static_cast<Thing*>(target))->TriggerByName(Player::s_player, nullptr, nullptr);
            }
        }
        if (!field180) aliveFlag = 0;
    }
}

void KnockDown::Move() {
    MARKFUNCTION(0x8001DB8C);

    field120 += 0x48;
    if (field120 > field124) field120 = field124;

    switch (field188) {
        case 0:
            field140 += field120;
            if (field140 > 0x4000) { field140 = 0x4000; field116 = 2; }
            break;
        case 1:
            field148 -= field120;
            if (field148 < -0x4000) { field148 = -0x4000; field116 = 2; }
            break;
        case 2:
            field140 -= field120;
            if (field140 < -0x4000) { field140 = -0x4000; field116 = 2; }
            break;
        case 3:
            field148 += field120;
            if (field148 > 0x4000) { field148 = 0x4000; field116 = 2; }
            break;
        default:
            break;
    }
    orientation = { field140, field144, field148 };
}

void KnockDown::UpdateCollisionBox() {
    MARKFUNCTION(0x8001DCA0);

    tagCollisionBox box = savedCollBox;
    switch (field188) {
        case 0:
            box.maxY = static_cast<s16>(MulShift16(savedCollBox.maxY, rmSin16(-field140 + 0x4000)));
            box.minZ = static_cast<s16>(savedCollBox.minZ - MulShift16(savedCollBox.maxY, rmSin16(-field140)));
            break;
        case 1:
            box.maxY = static_cast<s16>(MulShift16(savedCollBox.maxY, rmSin16(field148 + 0x4000)));
            box.maxX = static_cast<s16>(savedCollBox.maxX + MulShift16(savedCollBox.maxY, rmSin16(field148)));
            break;
        case 2:
            box.maxY = static_cast<s16>(MulShift16(savedCollBox.maxY, rmSin16(field140 + 0x4000)));
            box.maxZ = static_cast<s16>(savedCollBox.maxZ + MulShift16(savedCollBox.maxY, rmSin16(field140)));
            break;
        case 3:
            box.maxY = static_cast<s16>(MulShift16(savedCollBox.maxY, rmSin16(-field148 + 0x4000)));
            box.minX = static_cast<s16>(savedCollBox.minX - MulShift16(savedCollBox.maxY, rmSin16(-field148)));
            break;
        default:
            break;
    }
    box.maxY = static_cast<s16>(box.maxY + savedCollBox.maxZ - savedCollBox.minZ);
    SetCollisionBox(box);
}

void KnockDown::UpdatePosition() {
    MARKFUNCTION(0x8001F674);
}

void KnockDown::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x8001E120);
    if (aliveFlag && pickup) {
        Pickup* pickupObject = static_cast<Pickup*>(pickup);
        pickupObject->PlayEffect();
        pickupObject->DamageExtra();
    }
}

void KnockDown::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x8001E16C);

    if (!aliveFlag || !hum) {
        return;
    }

    LVector correctedPos = {};
    LVector correctionNormal = {};
    LVector pushedPos = {};

    CorrectThingPositionObstacle(
        pos, pos,
        orientation.y, orientation.y,
        collBox,
        hum->pos, hum->homePos,
        hum->collBboxMin.x, hum->collBboxMin.y, hum->collBboxMin.z,
        correctedPos, correctionNormal, pushedPos);

    if (field116 == 0) {
        hum->homePos = correctedPos;
        return;
    }

    if (field116 == 2) {
        hum->homePos = correctedPos;
        if (correctionNormal.y > 0 && hum->velocity.y <= 0) {
            hum->SetFloorHeight(pos.y + static_cast<s32>(collBox.maxY));
            hum->velocity.y = 0;
            AddPassenger(hum);
        }
        return;
    }

    if (field116 != 1) return;

    const LVector direction = KnockDownLocalDominantDirection(*this, hum->homePos);
    bool hit = false;
    switch (field188) {
        case 0: hit = direction.z > 0; break;
        case 1: hit = direction.x < 0; break;
        case 2: hit = direction.z < 0; break;
        case 3: hit = direction.x > 0; break;
        default: break;
    }

    if (!hit) {
        hum->homePos = correctedPos;
        return;
    }

    hum->HandleCollision(
        this,
        1,
        KNOCKDOWN_COLLISION_TAG_IMPACT_REGION,
        4,
        KNOCKDOWN_COLLISION_TAG_HIT_TYPE,
        14,
        KNOCKDOWN_COLLISION_TAG_DAMAGE,
        field176,
        KNOCKDOWN_COLLISION_TAG_END);
    if (field184) {
        field184->HitHumanoid();
    }
}

void KnockDown::HandleAttack(Humanoid* attacker, s32 damageType, s32 attackMagnitude, s32 damage) {
    MARKFUNCTION(0x8001E4EC);
    (void)damageType;
    (void)attackMagnitude;
    (void)damage;

    if (field116 != 0 || !attacker) {
        return;
    }

    const LVector direction = KnockDownLocalDominantDirection(*this, attacker->homePos);
    if (direction.x > 0) field188 = 1;
    else if (direction.x < 0) field188 = 3;
    else if (direction.z > 0) field188 = 2;
    else field188 = 0;

    field116 = 1;
    if (field184) {
        field184->Kick();
        field184->BeginFall();
    }
}

bool KnockDown::CareAboutAttack() const {
    MARKFUNCTION(0x8001F66C);
    return true;
}

static const u32 STACK_DEFAULT_EFFECT_HASH = 0x065C8E90;
static const char STACK_DEFAULT_PUSHABLE_NAME[] = "BoxStack1";
static constexpr u32 STACK_CALLBACK_JOINT_HASH_A = 0x08857E62;
static constexpr u32 STACK_CALLBACK_JOINT_HASH_B = 0x08857E63;

static STreeJoint* StackFindJointByHash(STreeData* skeleton, u32 jointHash) {
    if (!skeleton || !skeleton->joints || jointHash == 0) {
        return nullptr;
    }

    for (u32 i = 0; i < skeleton->numJoints; i++) {
        STreeJoint* joint = &skeleton->joints[i];
        if (joint->nameUID == jointHash) {
            return joint;
        }
    }

    return nullptr;
}

static s32 StackEJointCallback(STreeJoint* joint, u32 jointIndex, const Mat4& currentMatrix) {
    MARKFUNCTION(0x8001F5A0);

    if (!joint || !joint->callbackData) {
        return 1;
    }

    Stack* stack = static_cast<Stack*>(joint->callbackData);

    // Model posX/rot are only written by Draw().
    Mat4 modelWorld = Mat4();
    p3dBuildRotMatrixZYX(
        (u16)(stack->orientation.x & 0xFFFF),
        (u16)(stack->orientation.y & 0xFFFF),
        (u16)(stack->orientation.z & 0xFFFF),
        modelWorld);
    modelWorld.SetTranslation((f32)stack->pos.x, (f32)stack->pos.y, (f32)stack->pos.z);
    const Mat4 worldMatrix = modelWorld * currentMatrix;

    LVector jointPos = {};
    jointPos.x = (s32)worldMatrix.GetTransX();
    jointPos.y = (s32)worldMatrix.GetTransY();
    jointPos.z = (s32)worldMatrix.GetTransZ();

    s32 callbackJointIndex = (s32)jointIndex;
    if (joint->nameUID == STACK_CALLBACK_JOINT_HASH_A) {
        callbackJointIndex = 2;
    }
    else if (joint->nameUID == STACK_CALLBACK_JOINT_HASH_B) {
        callbackJointIndex = 3;
    }

    stack->SetupJointPosition(callbackJointIndex, jointPos);
    return 1;
}

s32 Stack::LoadDialog(u32 dialogId, u32 priority) {
    MARKFUNCTION(0x8001E6E0);

    s32 handle = 0;
    if (jcsQueryDialogPriority() < (s32)priority) {
        // PSX uses event 0x1A here directly.
        handle = rsEvent(0x1A, 0, (s32)dialogId, (s32)priority);
    }

    if (handle) {
        dialogHandle = handle;
        dialogID = (s32)dialogId;
    }

    return 1;
}

Stack::Stack(const LVector* pos, u16 type)
    : Obstacle(pos, type) {
    MARKFUNCTION(0x8001E768);
    knockDownSound = nullptr;
    anim = nullptr;
    state = 0;
    effectHash = 0;
    dialogHandle = 0;
    dialogID = 0;
}

Stack::~Stack() {
    MARKFUNCTION(0x8001E7B8);

    if (knockDownSound) {
        delete knockDownSound;
        knockDownSound = nullptr;
    }
}

void Stack::Draw() {
    MARKFUNCTION(0x8001E820);

    if (state == 3) {
        return;
    }

#if HIGH_FPS_PLAY_PRESENTATION
    TransformFlip* renderFlip = nullptr;
    s32 savedFrameReal = 0;
    LVector savedJoint0 = {};
    LVector savedJoint1 = {};
    bool restoreGameplayPose = false;

    const bool stackInPlay = (g_time && g_game && g_game->GetState() == GameState::Play);
    if (stackInPlay && anim && anim->flip) {
        f32 alpha = g_time->GetPlayPresentationAlpha();
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        // Lerp in 16.16 fixed-point ("real" frame) space, not integer frame
        // space, or the fraction gets truncated away and the anim looks
        // like it's stepping at the 30Hz logic rate instead of smoothly
        // blending between ticks.
        const s32 prevFrameReal = renderPrevFrame << 16;
        const s32 curFrameReal = currentFrame << 16;
        const s32 interpFrameReal = prevFrameReal + (s32)((f32)(curFrameReal - prevFrameReal) * alpha);

        renderFlip = anim->flip;
        savedFrameReal = renderFlip->frameReal;
        savedJoint0 = jointPositions[0];
        savedJoint1 = jointPositions[1];
        restoreGameplayPose = true;

        renderFlip->SetFrameReal(interpFrameReal);
        renderFlip->UpdateJoints();
    }
#endif

    Obstacle::Draw();

#if HIGH_FPS_PLAY_PRESENTATION
    if (restoreGameplayPose) {
        if (renderFlip) {
            renderFlip->SetFrameReal(savedFrameReal);
            renderFlip->UpdateJoints();
        }
        jointPositions[0] = savedJoint0;
        jointPositions[1] = savedJoint1;
    }
#endif
}

void Stack::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8001E850);

    orientation.x = root->field40;
    orientation.y = root->field44;
    orientation.z = root->field48;

    Obstacle::AnalyzeMesh(root);

    tagCollisionBox localBox = { 0x7FFF, 0x7FFF, 0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0 };
    LVector localPos = root->pos;

    // PSX Stack grounds from DBVolume bounds before normalizing Y into local space.
    FillCollisionBox(localBox, *static_cast<const DBVolume*>(root));
    localPos.y += (s32)localBox.minY;

    pos = localPos;
    localBox.maxY = (s16)((u16)localBox.maxY - (u16)localBox.minY);
    localBox.minY = 0;

    SetCollisionBox(localBox);

    const DBAttrib* attr21 = root->FindAttrib(21);
    modelIndex = attr21 ? (s32)attr21->value : 0;

    // PSX queries attrib 6 here but still initializes wobble count to zero.
    (void)root->FindAttrib(6);
    timesToWobble = 0;

    const DBAttrib* attr7 = root->FindAttrib(7);
    damage = attr7 ? (s32)attr7->value : 0x32;

    const DBAttrib* attr20 = root->FindAttrib(20);
    if (attr20) {
        effectHash = p3dHash(attr20->GetAttribString());
    }
    else {
        effectHash = STACK_DEFAULT_EFFECT_HASH;
    }

    const DBAttrib* attr8 = root->FindAttrib(8);
    if (attr8) {
        const char* baseName = attr8->GetAttribString();
        if (baseName && baseName[0] != '\0') {
            const size_t baseLen = std::strlen(baseName);
            if (baseLen > 0 && baseName[baseLen - 1] == 'P') {
                std::snprintf(pushableName, sizeof(pushableName), "%s", baseName);
            }
            else {
                std::snprintf(pushableName, sizeof(pushableName), "%sP", baseName);
            }
        }
        else {
            std::snprintf(pushableName, sizeof(pushableName), "%s", STACK_DEFAULT_PUSHABLE_NAME);
        }
    }
    else {
        std::snprintf(pushableName, sizeof(pushableName), "%s", STACK_DEFAULT_PUSHABLE_NAME);
    }
    pushableName[sizeof(pushableName) - 1] = '\0';

    SetupCallbacks();
}

void Stack::CreateModel(const char* name) {
    MARKFUNCTION(0x8001EA18);

    anim = nullptr;
    animBasic = nullptr;

    if (!modelHash) {
        flags |= TF_MODEL_CREATED;
        return;
    }

    Thing::CreateModel(nullptr);

    if (!knockDownSound) {
        CSound* soundObj = nullptr;
        if (CSoundFactory::CreateObject(0x2792, &soundObj, 0) >= 0) {
            knockDownSound = static_cast<CKnockDownSound*>(soundObj);
            if (knockDownSound) {
                knockDownSound->Initialize(&pos);
            }
        }
    }

    animBasic = Obstacle_GetAnimation(modelIndex);
    TransformAnim* baseAnim = animBasic ? animBasic->anim : nullptr;

    Model* modelPtr = static_cast<Model*>(model);
    if (baseAnim && modelPtr && modelPtr->drawableType == 3) {
        EModel* eModel = static_cast<EModel*>(modelPtr);
        anim = eModel->ApplyAnimToModel(baseAnim, 3);
    }

    if (lightingFlag && modelPtr) {
        modelPtr->AllocateHardwareLights(3);
        modelPtr->AllocateAmbientLight();
    }

    SetupCallbacks();
}

void Stack::DeleteModel() {
    MARKFUNCTION(0x8001EB0C);

    Obstacle::DeleteModel();

    anim = nullptr;
    animBasic = nullptr;

    if (knockDownSound) {
        delete knockDownSound;
        knockDownSound = nullptr;
    }
}

void Stack::Reset() {
    MARKFUNCTION(0x8001EB5C);

    currentFrame = 0;
    state = 0;
}

void Stack::UpdatePosition() {
    MARKFUNCTION(0x8001EB68);
}

void Stack::Wobble() {
    MARKFUNCTION(0x8001EB70);

    currentFrame++;
    if (currentFrame < 20) {
        return;
    }

    if (timesToWobble > 0) {
        timesToWobble--;
        currentFrame = 0;
    }
    else {
        state = 2;
        LoadDialog(0x1B, 0x32);
        UpdateCollisionBox();
    }
}

void Stack::Fall() {
    MARKFUNCTION(0x8001EBE8);

    currentFrame++;
    if (currentFrame >= 42) {
        FinishStack();
    }
}

void Stack::FinishStack() {
    MARKFUNCTION(0x8001EC24);

    state = 3;

    if (effectHash) {
        GEffect_Create(effectHash, &jointPositions[0], nullptr, nullptr, 0, 0, 0);
        GEffect_Create(effectHash, &jointPositions[1], nullptr, nullptr, 0, 0, 0);
    }

    if (knockDownSound) {
        knockDownSound->Impact();
    }

    tagCollisionBox localBox = INVALID_COLLISION_BOX;
    SetCollisionBox(localBox);

    DBRoot tempRoot = {};
    tempRoot.name = const_cast<char*>("");
    tempRoot.AllocatePermanentAttributeArray(2);
    tempRoot.AddAttribString(0, 5, pushableName);
    tempRoot.AddAttribNumber(1, 0x0F, blockNum);

    Thing* spawned = g_ai->AddThingNoTagList(nullptr, 0x1CE, &pos, nullptr, pushableName, &tempRoot);

    tempRoot.DeallocatePermanentAttributeArray();

    if (spawned) {
        spawned->pos = pos;
        spawned->orientation = orientation;

        Model* spawnedModel = static_cast<Model*>(spawned->model);
        if (spawnedModel) {
            spawnedModel->rotY = (u16)(s16)(s8)physicalType;
        }

        Obstacle* spawnedObstacle = static_cast<Obstacle*>(spawned);
        spawnedObstacle->lightingFlag = lightingFlag;
        spawnedObstacle->shadowFlag = shadowFlag;
    }
}

void Stack::Think() {
    MARKFUNCTION(0x8001EE28);

#if HIGH_FPS_PLAY_PRESENTATION
    const s32 frameBeforeTick = currentFrame;
#endif

    if (state == 1) {
        Wobble();
    }
    else if (state == 2) {
        Fall();
    }
    else if (state == 3) {
        return;
    }

    TransformFlip* flip = anim->flip;

    flip->SetFrame(currentFrame);
    flip->UpdateJoints();

#if HIGH_FPS_PLAY_PRESENTATION
    renderPrevFrame = frameBeforeTick;

    if (flip->tree && flip->tree->numJoints > 0) {
        Mat4* jointMatrices = new Mat4[flip->tree->numJoints];
        flip->tree->ComputeWorldMatricesWithCallbacks(jointMatrices);
        delete[] jointMatrices;
    }
#endif

    if (knockDownSound) {
        knockDownSound->Think();
    }
}

void Stack::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x8001EEE8);

    pickup->Kill();
}

void Stack::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x8001EF1C);

    tagCollisionBox localBox = collBox;

    LVector correctedPos = {};
    LVector correctionNormal = {};
    LVector correctionPushedPos = {};

    if (state < 0) {
        return;
    }

    if (state < 2) {
        CorrectThingPositionObstacle(
            pos,
            pos,
            orientation.y,
            orientation.y,
            localBox,
            hum->pos,
            hum->homePos,
            hum->collBboxMin.x,
            hum->collBboxMin.y,
            hum->collBboxMin.z,
            correctedPos,
            correctionNormal,
            correctionPushedPos);

        bool triggerNow = false;

        if (LedgeCheck(localBox, correctionNormal, correctionPushedPos, hum)) {
            if (hum->thingType != 0) {
                triggerNow = true;
            }
        }
        else {
            const s32 action = hum->actionState;
            if ((action == 0x3F || action == 0x38 || action == 0x39 || action == 0x3A)
                && hum->velocity.y < -25) {
                triggerNow = true;
            }
        }

        if (triggerNow) {
            TriggerStackAnimation();
        }

        if (correctionNormal.y > 0 && hum->velocity.y <= 0) {
            hum->SetFloorHeight(pos.y + (s32)localBox.maxY);
            hum->velocity.y = 0;
            AddPassenger(hum);
            TriggerStackAnimation();
        }

        hum->homePos = correctedPos;
        return;
    }

    if (state != 2) {
        return;
    }

    const u16 box0 = (u16)localBox.minX;
    localBox.maxX = (s16)(-(s32)box0);
    localBox.maxY = (s16)(-(s32)box0 - (s32)box0);

    CorrectThingPositionObstacle(
        pos,
        pos,
        orientation.y,
        orientation.y,
        localBox,
        hum->pos,
        hum->homePos,
        hum->collBboxMin.x,
        hum->collBboxMin.y,
        hum->collBboxMin.z,
        correctedPos,
        correctionNormal,
        correctionPushedPos);

    hum->homePos = correctedPos;

    if (correctionNormal.y > 0 && hum->velocity.y <= 0) {
        hum->SetFloorHeight(pos.y + (s32)localBox.maxY);
        hum->velocity.y = 0;
        AddPassenger(hum);
    }

    const tagCollisionCylinder& humCylinder =
        *reinterpret_cast<const tagCollisionCylinder*>(&hum->collBboxMin);
    tagCollisionSphere sphere = { 200 };

    const bool hit0 = CheckStaticCylinderSphereCollision(hum->pos, humCylinder, jointPositions[0], sphere);
    bool hit1 = false;
    bool hit = hit0;
    if (!hit) {
        hit1 = CheckStaticCylinderSphereCollision(hum->pos, humCylinder, jointPositions[1], sphere);
        hit = hit1;
        if (hit && currentFrame < 31) {
            hit = false;
        }
    }

    if (!hit) {
        return;
    }

    hum->HandleCollision(this, 1, damage, 0x80000007, 0);
    hum->SetActionState(AS_COLLAPSE_STUN, 0);

    if (hum->thingType != 0) {
        g_scoreManager->AddStylePoints(100);
    }

    if (hum == static_cast<Humanoid*>(Player::s_player)) {
        hum->soundParam = dialogID;
        hum->soundHandle = dialogHandle;
    }

    FinishStack();
}

void Stack::HandleAttack(Humanoid* attacker, s32 damageType, s32 attackMagnitude, s32 damage) {
    MARKFUNCTION(0x8001F320);

    if (state == 0) {
        TriggerStackAnimation();
        if (knockDownSound) {
            knockDownSound->Kick();
        }
    }
}

void Stack::UpdateCollisionBox() {
    MARKFUNCTION(0x8001F370);

    tagCollisionBox localBox = collBox;
    localBox.maxX = (s16)((u16)localBox.maxX + (u16)((u16)localBox.maxY - (u16)localBox.minY));
    SetCollisionBox(localBox);
}

void Stack::TriggerStackAnimation() {
    MARKFUNCTION(0x8001F3E8);

    state = 1;
}

void Stack::SetupCallbacks() {
    MARKFUNCTION(0x8001F3F4);

    Model* modelPtr = static_cast<Model*>(model);
    if (!modelPtr || modelPtr->drawableType != 3) {
        return;
    }

    AnimStructure* animStruct = anim;
    if (!animStruct && modelPtr->animStructure) {
        animStruct = static_cast<AnimStructure*>(modelPtr->animStructure);
    }
    if (!animStruct || !animStruct->flip || !animStruct->flip->tree) {
        return;
    }

    STreeData* skeleton = animStruct->flip->tree;

    STreeJoint* jointA = StackFindJointByHash(skeleton, STACK_CALLBACK_JOINT_HASH_A);
    if (jointA) {
        jointA->postCallback = StackEJointCallback;
        jointA->callbackData = this;
        jointA->flags |= STF_POST_CALLBACK_MASK;
    }

    STreeJoint* jointB = StackFindJointByHash(skeleton, STACK_CALLBACK_JOINT_HASH_B);
    if (jointB) {
        jointB->postCallback = StackEJointCallback;
        jointB->callbackData = this;
        jointB->flags |= STF_POST_CALLBACK_MASK;
    }

    if (!jointA || !jointB) {
        for (s32 i = 2; i <= 3; i++) {
            STreeJoint* joint = skeleton->GetJoint(i);
            if (!joint) {
                continue;
            }
            joint->postCallback = StackEJointCallback;
            joint->callbackData = this;
            joint->flags |= STF_POST_CALLBACK_MASK;
        }
    }
}

void Stack::SetupJointPosition(s32 index, LVector pos) {
    MARKFUNCTION(0x8001F55C);

    index -= 2;
    if ((u32)index < 2) {
        jointPositions[index] = pos;
    }
}

bool Stack::CareAboutAttack() const {
    MARKFUNCTION(0x8001F664);
    return true;
}

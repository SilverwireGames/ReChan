#include "gen/colmgr.h"
#include "gen/colsect.h"
#include "gen/colwall.h"
#include "gen/colfloor.h"
#include "gen/cclist.h"
#include "ai/thing.h"
#include "ai/humanoid.h"
#include "ai/obstacle.h"
#include "ai/pickup.h"
#include "ai/player.h"
#include "p3d/p3dmath.h"
#include "gen/model.h"
#include "gen/animmat.h"
#include "gen/colvol.h"
#include "fe/hud.h"
#include "gen/scoremgr.h"

static s32 g_floorDebugCounter = 0;

// COLMGR globals (PSX: gp-relative, values from PSX .data section)
Wall* g_wallPtrArray[64] = {};         // gp+2828
s32 g_filledWallCount = 0;           // gp+4172
s32 g_maxWallIterations = 4;         // gp+2836
s32 g_wallHitCounter = 0;            // gp+2840
s32 g_floorSearchMargin = 320;       // gp+2844 (0x140): floor AABB search expansion
s32 g_floorYSearchOffset = 384;      // gp+2852 (0x180): vertical offset for floor search
s32 g_floorStandingTol = 640;        // gp+2856 (0x280): landing detection tolerance
s32 g_colDefaultHeight = 256;        // gp+2904 (0x100): short humanoid height threshold
s32 g_colMaxRadius = 512;            // gp+2908 (0x200): max skeletal collision radius
s32 g_colDefaultRadius = 192;        // gp+2912 (0xC0): default collision radius
s32 g_colSlopeTol = 0xB50B;          // gp+2884: slope tolerance (~cos 45)
s32 g_colSlopeForce = 0x4E0000;      // gp+2888: slope sliding force
s32 g_colFallVelDivisor = 2;         // gp+2900: fall velocity divisor (s16)
s32 g_colFallVelThreshold = 100;     // gp+2902 (0x64): fall velocity threshold (s16)
Floor* g_floorPtrArrayGlobal[64] = {};   // gp+2828+256 (global floor scratch, 64 ptrs)
s32 g_climbSearchRadius = 128;       // gp+2848 (0x80): climbing state search radius
s32 g_boneFloorRadius = 48;          // gp+2860 (0x30): bone floor search radius
s32 g_camLookAheadDist = 144;        // gp+2868 (0x90): camera look-ahead distance
s32 g_camEdgeThreshold = 1536;       // gp+2872 (0x600): camera edge height threshold
s32 g_camEdgeCounter = 0;            // gp+2876: camera edge frame counter
s32 g_camEdgeMaxCount = 15;          // gp+2880 (0xF): camera edge max frames
DynamicThing* g_combatTarget1 = nullptr; // gp+4176
DynamicThing* g_combatTarget2 = nullptr; // gp+4180

// HTW working globals - copies of entity pos/homePos used during wall processing
// PSX: 0x800E28CC-0x800E28E0
static s32 HTW_PosX;      // 800E28CC
static s32 HTW_PosY;      // 800E28D0
static s32 HTW_PosZ;      // 800E28D4
static s32 HTW_HomePosX;  // 800E28D8
static s32 HTW_HomePosY;  // 800E28DC
static s32 HTW_HomePosZ;  // 800E28E0

// PSX: ExtendRange__FRllT0 (COLMGR.CPP:195) 0x800A7B80
void ExtendRange(s32& rangeMin, s32 value, s32& rangeMax) {
    MARKFUNCTION(0x800A7B80);
    if (value < rangeMin) rangeMin = value;
    if (value > rangeMax) rangeMax = value;
}

// PSX: CorrectCollision__FRC10tagLVectorT0lG9_RMVECT16R10tagLVectorT4 (COLWALL.CPP) 0x800C4CA4
static void CorrectCollision(
    const LVector& pos, const LVector& homePos, s32 ratio,
    s32 normalX, s32 normalY, s32 normalZ,
    LVector& outPos, LVector& outHomePos) {
    // Lerp pos toward homePos by collision ratio (contact point)
    outPos.x = pos.x + (s32)((ratio * (s64)(homePos.x - pos.x)) >> 16);
    outPos.y = pos.y + (s32)((ratio * (s64)(homePos.y - pos.y)) >> 16);
    outPos.z = pos.z + (s32)((ratio * (s64)(homePos.z - pos.z)) >> 16);

    // Project remaining distance onto wall normal, minus 2 for epsilon
    s32 projDist = (s32)((normalX * (s64)(homePos.x - outPos.x)) >> 16)
        + (s32)((normalY * (s64)(homePos.y - outPos.y)) >> 16)
        + (s32)((normalZ * (s64)(homePos.z - outPos.z)) >> 16)
        - 2;

    // Push homePos back along normal by projected distance
    outHomePos.x = homePos.x - (s32)((normalX * (s64)projDist) >> 16);
    outHomePos.y = homePos.y - (s32)((normalY * (s64)projDist) >> 16);
    outHomePos.z = homePos.z - (s32)((normalZ * (s64)projDist) >> 16);

    // Nudge contact point 2 units along normal (away from wall)
    outPos.x -= (s32)((-2LL * normalX) >> 16);
    outPos.y -= (s32)((-2LL * normalY) >> 16);
    outPos.z -= (s32)((-2LL * normalZ) >> 16);
}

// PSX: HTW_FillWallArray__Flll (COLMGR.CPP:240) 0x800A7BBC
// Fills g_wallPtrArray with wall pointers in the entity's vicinity
void HTW_FillWallArray(s32 radius, s32 height, s32 checkHeight) {
    MARKFUNCTION(0x800A7BBC);

    // Primary AABB: pos +/- 1024 on X/Z, Y from pos extended to homePos + height offsets
    s32 searchMinX = HTW_PosX - 1024;
    s32 searchMaxX = HTW_PosX + 1024;
    s32 searchMinY = HTW_PosY;
    s32 searchMaxY = HTW_PosY;
    ExtendRange(searchMinY, HTW_HomePosY, searchMaxY);
    searchMinY += height;
    searchMaxY += checkHeight;
    s32 searchMinZ = HTW_PosZ - 1024;
    s32 searchMaxZ = HTW_PosZ + 1024;

    // Secondary AABB: center of pos-homePos +/- half-extent +/- radius
    s32 centerX = (HTW_PosX + HTW_HomePosX) / 2;
    s32 centerY = (HTW_PosY + HTW_HomePosY) / 2;
    s32 centerZ = (HTW_PosZ + HTW_HomePosZ) / 2;

    s32 diffX = HTW_HomePosX - HTW_PosX;
    s32 halfExtX = (diffX < 0 ? -diffX : diffX) / 2;
    s32 diffY = HTW_HomePosY - HTW_PosY;
    s32 halfExtY = (diffY < 0 ? -diffY : diffY) / 2;
    s32 diffZ = HTW_HomePosZ - HTW_PosZ;
    s32 halfExtZ = (diffZ < 0 ? -diffZ : diffZ) / 2;

    s32 secMinX = centerX - halfExtX - radius;
    s32 secMinY = centerY - halfExtY;
    s32 secMinZ = centerZ - halfExtZ - radius;
    s32 secMaxX = centerX + halfExtX + radius;
    s32 secMaxY = centerY + halfExtY;
    s32 secMaxZ = centerZ + halfExtZ + radius;

    // If secondary box extends beyond primary in any direction, use secondary entirely
    if (secMinX < searchMinX || secMinZ < searchMinZ ||
        searchMaxX < secMaxX || searchMaxZ < secMaxZ) {
        searchMinX = secMinX;
        searchMinY = secMinY;
        searchMinZ = secMinZ;
        searchMaxX = secMaxX;
        searchMaxY = secMaxY;
        searchMaxZ = secMaxZ;
    }

    LVector searchMin = { searchMinX, searchMinY, searchMinZ };
    LVector searchMax = { searchMaxX, searchMaxY, searchMaxZ };

    s32 count = CollisionSector::FillWorldWallArray(searchMin, searchMax, g_wallPtrArray, 64);
    if (count > 64) count = 64;
    g_filledWallCount = count;
}

// PSX: HTW_HandleWallCollisions__FP12DynamicThinglll (COLMGR.CPP:319) 0x800A7E38
// Iterative wall collision correction loop - operates on HTW globals
s32 HTW_HandleWallCollisions(DynamicThing* thing, s32 radius, s32 height, s32 checkHeight) {
    MARKFUNCTION(0x800A7E38);

    // Compute on-ground check flag: on ground AND not standing on something
    s32 onGround = (thing->flags & TF_ON_GROUND) ? 1 : 0;
    Thing* issuer = thing->GetTicketIssuer();
    s32 checkFlag = 0;
    if (onGround) {
        checkFlag = (issuer == nullptr) ? 1 : 0;
    }

    s32 wallHit = 0;
    s32 iter = 0;
    thing->flags &= ~(u32)0x8000;

    LVector posVec = { HTW_PosX, HTW_PosY, HTW_PosZ };
    LVector homePosVec = { HTW_HomePosX, HTW_HomePosY, HTW_HomePosZ };

    do {
        if (!CollisionSector::CheckArrayWallCollision(
            g_wallPtrArray, g_filledWallCount,
            posVec, homePosVec,
            radius, height, checkHeight, checkFlag)) {
            break;
        }

        wallHit = 1;
        thing->flags |= 0x8000;

        LVector correctedPos, correctedHomePos;
        CorrectCollision(
            posVec, homePosVec,
            g_wallCollisionInfo.collisionRatio,
            g_wallCollisionInfo.wallNormal.x,
            g_wallCollisionInfo.wallNormal.y,
            g_wallCollisionInfo.wallNormal.z,
            correctedPos, correctedHomePos);

        // Update globals from corrected values (X and Z only, per PSX)
        HTW_PosX = correctedPos.x;
        HTW_PosZ = correctedPos.z;
        HTW_HomePosX = correctedHomePos.x;
        HTW_HomePosZ = correctedHomePos.z;

        // Refresh local vectors for next iteration
        posVec = { HTW_PosX, HTW_PosY, HTW_PosZ };
        homePosVec = { HTW_HomePosX, HTW_HomePosY, HTW_HomePosZ };
    } while (++iter < g_maxWallIterations);

    return wallHit;
}

// PSX: HTW_HandleHandFootCollisions__FP12DynamicThing (COLMGR.CPP:409) 0x800A7FAC
// Checks wall collisions for attack limb positions (player only)
// Adjusts HTW_HomePosX/Z if an attack bone hits a wall toward the player
void HTW_HandleHandFootCollisions(DynamicThing* thing) {
    MARKFUNCTION(0x800A7FAC);

    // PSX: bne $a0, thePlayer -> only runs for the player
    if (thing != Player::s_player) {
        return;
    }

    Humanoid* player = (Humanoid*)thing;

    // PSX +200: attack joint index, must be < 10
    if ((u32)player->attackJointIndex >= 10) {
        return;
    }

    // Get prev/cur frame bone positions for the attacking joint
    HumanoidModel* hmodel = (HumanoidModel*)player->model;
    AnimationMatrices* animMat = hmodel->animMatrices;

    LVector prevPos, curPos;
    animMat->GetAttack((u32)player->attackJointIndex, prevPos, curPos);

    // Build a small direction vector from orientation.y, scaled by 0x200
    LVector dir = {};
    dir.x = (s32)(((s64)rmSin16(thing->orientation.y) * 0x200) >> 16);
    dir.z = (s32)(((s64)rmSin16((s16)(thing->orientation.y + 0x4000)) * 0x200) >> 16);

    // Offset prevPos backward by direction
    prevPos.x = curPos.x - dir.x;
    prevPos.y = curPos.y - dir.y;
    prevPos.z = curPos.z - dir.z;

    // Check wall collision for the attack limb (zero radius/height)
    if (!CollisionSector::CheckArrayWallCollision(
        g_wallPtrArray, g_filledWallCount,
        prevPos, curPos,
        0, 0, 0, 0)) {
        return;
    }

    // Check if wall normal points toward the entity (dot < 0)
    s32 dot = fixmul16(g_wallCollisionInfo.wallNormal.x,
                       g_wallCollisionInfo.hitPoint.x - HTW_HomePosX)
        + fixmul16(g_wallCollisionInfo.wallNormal.z,
                   g_wallCollisionInfo.hitPoint.z - HTW_HomePosZ);

    if (dot >= 0) {
        return;
    }

    // Compute slide amount: project attack movement onto wall normal,
    // scaled by half the remaining collision fraction
    s32 projMove = fixmul16(g_wallCollisionInfo.wallNormal.x,
                            curPos.x - prevPos.x)
        + fixmul16(g_wallCollisionInfo.wallNormal.z,
                   curPos.z - prevPos.z);

    s32 halfComplement = fixmul16(0x10000 - g_wallCollisionInfo.collisionRatio, 0x8000);
    s32 pushAmt = fixmul16(halfComplement, projMove);

    // Push homePos away from wall
    HTW_HomePosX -= fixmul16(g_wallCollisionInfo.wallNormal.x, pushAmt);
    HTW_HomePosZ -= fixmul16(g_wallCollisionInfo.wallNormal.z, pushAmt);
}

// PSX: HandleThingWall__FP12DynamicThinglll (COLMGR.CPP:510) 0x800A8290
// Main wall collision handler for a single DynamicThing
void HandleThingWall(DynamicThing* thing, s32 radius, s32 height, s32 checkHeight) {
    MARKFUNCTION(0x800A8290);

    // Save original homePos for correction transfer
    s32 savedHomePosX = thing->homePos.x;
    s32 savedHomePosY = thing->homePos.y;
    s32 savedHomePosZ = thing->homePos.z;

    // Copy entity state to HTW globals
    HTW_PosX = thing->pos.x;
    HTW_PosY = thing->pos.y;
    HTW_PosZ = thing->pos.z;
    HTW_HomePosX = thing->homePos.x;
    HTW_HomePosY = thing->homePos.y;
    HTW_HomePosZ = thing->homePos.z;

    // Fill wall array using HTW globals for search area
    HTW_FillWallArray(radius, height, checkHeight);

    // Run iterative wall correction (modifies HTW globals)
    s32 wallHit = HTW_HandleWallCollisions(thing, radius, height, checkHeight);

    // Compute on-ground check for intersection test
    s32 onGround = (thing->flags & TF_ON_GROUND) ? 1 : 0;
    Thing* issuer = thing->GetTicketIssuer();
    s32 checkFlag = 0;
    if (onGround) {
        checkFlag = (issuer == nullptr) ? 1 : 0;
    }

    // Wall intersection test using HTW globals
    LVector moveDir = { HTW_HomePosX, HTW_PosY + 2, HTW_HomePosZ };
    LVector hitOutput = {};
    if (CollisionSector::CheckArrayWallIntersection(
        g_wallPtrArray, g_filledWallCount,
        hitOutput, moveDir,
        radius, height, checkHeight, checkFlag)) {
        HTW_HomePosX = hitOutput.x;
        HTW_HomePosZ = hitOutput.z;
    }

    // Hand/foot collision for player combat
    HTW_HandleHandFootCollisions(thing);

    // Combat wall hit tracking for humanoids
    s32 alreadyTarget = 0;
    if (thing->thingType < 29) {
        if (thing == g_combatTarget1 || thing == g_combatTarget2) {
            alreadyTarget = 1;
        }

        Humanoid* hum = (Humanoid*)thing;
        s32 state = hum->actionState;
        s32 isCombat = 0;
        if (state == 36 || state == 59 || state == 37 ||
            state == 38 || state == 60 || state == 61 || state == 62) {
            isCombat = 1;
        }

        if (isCombat && !alreadyTarget) {
            if (g_combatTarget1 == nullptr) {
                g_combatTarget1 = thing;
            }
            else if (g_combatTarget2 == nullptr) {
                g_combatTarget2 = thing;
            }

            // Transfer wall correction to combat opponent (PSX +256)
            DynamicThing* opponent = (DynamicThing*)(uintptr_t)hum->field256;
            g_wallHitCounter++;
            if (opponent) {
                opponent->homePos.x += HTW_HomePosX - savedHomePosX;
                opponent->homePos.y += HTW_HomePosY - savedHomePosY;
                opponent->homePos.z += HTW_HomePosZ - savedHomePosZ;
            }
        }
    }

    // Write globals back to thing->homePos only if TF_DYNAMIC
    if (thing->flags & TF_DYNAMIC) {
        thing->homePos.x = HTW_HomePosX;
        thing->homePos.y = HTW_HomePosY;
        thing->homePos.z = HTW_HomePosZ;
    }

    const u16 thingType = thing->thingType;
    const bool isPickupWeaponType =
        static_cast<u16>(thingType - AITypes::TT_OBSTACLE_FIRST) < 28u;
    if (wallHit && isPickupWeaponType) {
        Pickup* pickup = static_cast<Pickup*>(thing);
        pickup->PlayEffect();
        pickup->Kill();
    }
}

// PSX: HandleThingFloor__FP12DynamicThinglll (COLMGR.CPP:795) 0x800A8614
// PSX params: (thing, searchRadius, yMinOffset, checkHeight)
// Reversed from PSX decompile at line 100880-101330
void HandleThingFloor(DynamicThing* thing, s32 radius, s32 yMinOffset, s32 checkHeight) {
    MARKFUNCTION(0x800A8614);

    s32 halfRadius = radius / 2;

    // Work on local copies (PSX writes back only if TF_DYNAMIC at end)
    s32 localHomePosX = thing->homePos.x;
    s32 localHomePosY = thing->homePos.y;
    s32 localHomePosZ = thing->homePos.z;
    s32 localVelX = thing->velocity.x;
    s32 localVelY = thing->velocity.y;
    s32 localVelZ = thing->velocity.z;

    // Compute displacement from pos to homePos
    s32 dispX = localHomePosX - thing->pos.x;
    s32 dispY = localHomePosY - thing->pos.y;
    s32 dispZ = localHomePosZ - thing->pos.z;

    // Build search AABB from min/max of pos and homePos, expanded by halfRadius + searchMargin
    LVector searchMin, searchMax;
    searchMin.x = thing->pos.x;
    searchMin.y = thing->pos.y;
    searchMin.z = thing->pos.z;
    searchMax.x = thing->pos.x;
    searchMax.y = thing->pos.y;
    searchMax.z = thing->pos.z;
    ExtendRange(searchMin.x, localHomePosX, searchMax.x);
    ExtendRange(searchMin.y, localHomePosY, searchMax.y);
    ExtendRange(searchMin.z, localHomePosZ, searchMax.z);

    searchMin.x -= halfRadius + g_floorSearchMargin;
    searchMax.x += halfRadius + g_floorSearchMargin;
    searchMin.z -= halfRadius + g_floorSearchMargin;
    searchMax.z += halfRadius + g_floorSearchMargin;

    // Fill floor array
    Floor* floorArray[64];
    s32 floorCount = CollisionSector::FillWorldFloorArray(searchMin, searchMax, floorArray, 64);
    if (floorCount > 64) floorCount = 64;

    // PSX: climbing offset (v83, v85)
    s32 climbOffX = 0;
    s32 climbOffY = 0;
    s32 climbOffZ = 0;

    // PSX: detect humanoid for special handling (thingType < 29)
    Humanoid* humanoid = nullptr;
    if (thing->thingType < 29) {
        humanoid = (Humanoid*)thing;
    }

    // PSX: climbing state 23 search offset
    s32 isClimbing = 0;
    if ((!humanoid && Player::s_player && Player::s_player->actionState == (s32)AS_LEDGE_LATCH) ||
        (humanoid && humanoid->actionState == (s32)AS_LEDGE_LATCH)) {
        isClimbing = 1;
    }
    if (isClimbing) {
        s32 sinVal = rmSin16(thing->orientation.y);
        climbOffX = -(s32)(((s64)sinVal * (s64)g_climbSearchRadius) >> 16);
        s32 cosVal = rmSin16((s16)(thing->orientation.y + 0x4000));
        climbOffZ = -(s32)(((s64)cosVal * (s64)g_climbSearchRadius) >> 16);
    }

    // Pass 1: floor/ceiling at OLD position (pos + climbOffset + ySearchOffset)
    LVector testPosOld;
    testPosOld.x = thing->pos.x + climbOffX;
    testPosOld.y = thing->pos.y + g_floorYSearchOffset;
    testPosOld.z = thing->pos.z + climbOffZ;

    s32 floorHOld, ceilingHOld;
    LVector floorNormOld = {};
    LVector ceilingNormOld = {};
    s32 hasRailing = 0;
    LVector railCorrection = {};

    CollisionSector::GetArrayFloorAndCeilingHeight(
        floorArray, floorCount,
        floorHOld, ceilingHOld, floorNormOld, ceilingNormOld,
        &hasRailing, &railCorrection, &testPosOld, 2);

    // Pass 2: floor/ceiling at NEW position (homePos + climbOffset + ySearchOffset)
    LVector testPosNew;
    testPosNew.x = localHomePosX + climbOffX;
    testPosNew.y = thing->pos.y + g_floorYSearchOffset;
    testPosNew.z = localHomePosZ + climbOffZ;

    s32 floorHNew, ceilingHNew;
    LVector floorNormNew = {};
    LVector ceilingNormNew = {};
    s32 hasRailingNew = 0;
    LVector railCorrectionNew = {};

    CollisionSector::GetArrayFloorAndCeilingHeight(
        floorArray, floorCount,
        floorHNew, ceilingHNew, floorNormNew, ceilingNormNew,
        &hasRailingNew, &railCorrectionNew, &testPosNew, 2);

    // Apply railing correction
    if (hasRailingNew) {
        localHomePosX += railCorrectionNew.x;
        localHomePosZ += railCorrectionNew.z;
    }

    s32 bestFloorH = floorHNew;

    if (thing == (DynamicThing*)Player::s_player) {
        HumanoidModel* hmodel = (HumanoidModel*)thing->model;
        if (hmodel && hmodel->animMatrices) {
            s32* boneMatrix = AnimationMatrices::GetMatrix(hmodel->animMatrices, 5);
            if (boneMatrix) {
                LVector bonePos;
                bonePos.x = boneMatrix[5];
                bonePos.y = boneMatrix[6];
                bonePos.z = boneMatrix[7];

                s32 boneFloorH, boneCeilH;
                LVector boneFloorNorm = {};
                LVector boneCeilNorm = {};
                s32 boneRailing = 0;
                LVector boneRailCorr = {};

                CollisionSector::GetArrayFloorAndCeilingHeight(
                    floorArray, floorCount,
                    boneFloorH, boneCeilH, boneFloorNorm, boneCeilNorm,
                    &boneRailing, &boneRailCorr, &bonePos, g_boneFloorRadius);

                bestFloorH = boneFloorH;
            }
        }

        // Camera look-ahead edge detection
        s32 sinVal = rmSin16(thing->orientation.y);
        s32 cosVal = rmSin16((s16)(thing->orientation.y + 0x4000));

        LVector lookAheadPos;
        lookAheadPos.x = testPosOld.x + (s32)(((s64)g_camLookAheadDist * (s64)sinVal) >> 16);
        lookAheadPos.y = testPosOld.y + checkHeight;
        lookAheadPos.z = testPosOld.z + (s32)(((s64)g_camLookAheadDist * (s64)cosVal) >> 16);

        s32 lookFloorH, lookCeilH;
        LVector lookFloorNorm = {};
        LVector lookCeilNorm = {};
        s32 lookRailing = 0;
        LVector lookRailCorr = {};

        CollisionSector::GetArrayFloorAndCeilingHeight(
            floorArray, floorCount,
            lookFloorH, lookCeilH, lookFloorNorm, lookCeilNorm,
            &lookRailing, &lookRailCorr, &lookAheadPos, 16);

        if (lookFloorH + g_camEdgeThreshold >= floorHNew) {
            g_camEdgeCounter = 0;
        }
        else {
            g_camEdgeCounter++;
        }
        if (g_camEdgeMaxCount >= g_camEdgeCounter) {
            thing->flags &= ~0x40000;
        }
        else {
            thing->flags |= 0x40000;
        }
    }
    else if (humanoid) {
        HumanoidModel* hmodel = (HumanoidModel*)humanoid->model;
        if (hmodel && hmodel->animMatrices) {
            s32* boneMatrix = AnimationMatrices::GetMatrix(hmodel->animMatrices, 5);
            if (boneMatrix) {
                LVector bonePos;
                bonePos.x = boneMatrix[5];
                bonePos.y = boneMatrix[6];
                bonePos.z = boneMatrix[7];

                s32 boneFloorH, boneCeilH;
                LVector boneFloorNorm = {};
                LVector boneCeilNorm = {};
                s32 boneRailing = 0;
                LVector boneRailCorr = {};

                CollisionSector::GetArrayFloorAndCeilingHeight(
                    floorArray, floorCount,
                    boneFloorH, boneCeilH, boneFloorNorm, boneCeilNorm,
                    &boneRailing, &boneRailCorr, &bonePos, g_boneFloorRadius);

                bestFloorH = boneFloorH;
            }
        }
    }

    // Slope correction from floor normals
    s32 slopeCorr = 0;
    if (floorHOld > (s32)0x80000001) {
        s32 slopeDot = (s32)(((s64)floorNormOld.z * (s64)dispZ) >> 16)
            + (s32)(((s64)floorNormOld.x * (s64)dispX) >> 16);
        if (floorNormOld.y != 0) {
            slopeCorr = rmDiv16i(slopeDot, floorNormOld.y) + 2;
        }
    }
    s32 slopeCorr2 = 0;
    if (floorHNew > (s32)0x80000001) {
        s32 slopeDot = (s32)(((s64)floorNormNew.z * (s64)dispZ) >> 16)
            + (s32)(((s64)floorNormNew.x * (s64)dispX) >> 16);
        if (floorNormNew.y != 0) {
            slopeCorr2 = rmDiv16i(slopeDot, floorNormNew.y) + 2;
        }
    }
    if (slopeCorr2 < slopeCorr) slopeCorr2 = slopeCorr;
    if (slopeCorr2 < 0) slopeCorr2 = 0;

    // Ceiling collision
    if (ceilingHNew != 0x7FFFFFFF && ceilingHNew - checkHeight < localHomePosY) {
        localHomePosY = ceilingHNew - checkHeight - 1;
        if (localVelY > 0) localVelY = 0;
    }

    // Clear ground/slope/ceiling bits, preserve others
    bool wasOnGround = (thing->flags >> 12) & 1;
    thing->flags &= ~(TF_ON_GROUND | 0x10000 | 0x20000);

    // Floor normal output
    LVector outFloorNorm = {};
    outFloorNorm.y = FIX16_ONE;

    // Landing logic
    if (floorHNew > (s32)0x80000001) {
        s32 landingLevel = floorHNew - yMinOffset;

        if (thing->pos.y >= landingLevel - g_floorStandingTol
            && localHomePosY < landingLevel + slopeCorr2) {
            // PSX: falling velocity division for actionState 63 (flying back)
            if (!wasOnGround) {
                s32 absVelY = localVelY < 0 ? -localVelY : localVelY;
                s32 threshold = g_colFallVelThreshold < 0 ? -g_colFallVelThreshold : g_colFallVelThreshold;
                if (absVelY >= threshold && thing->thingType < 29) {
                    Humanoid* hum = (Humanoid*)thing;
                    if (hum->actionState == (s32)AS_THROW_FREE_FALL && g_colFallVelDivisor != 0) {
                        localVelY = -(localVelY / g_colFallVelDivisor);
                    }
                }
            }

            // Set floor-contact flag
            thing->flags |= 0x10000;

            // Land
            thing->Land();

            localHomePosY = landingLevel + 1;
            if (localVelY < 0) localVelY = 0;

            // Slope sliding force
            if (g_colSlopeTol >= floorNormNew.y) {
                s64 slopeScale = (s64)g_colSlopeForce * (s64)thing->stateCounter;
                LVector slopeForce;
                LVector slopeDir;
                slopeDir.x = (s32)(((s64)floorNormNew.x * (s64)floorNormNew.y) >> 16);
                slopeDir.y = (s32)(((s64)floorNormNew.y * (s64)floorNormNew.y) >> 16) - 0x10000;
                slopeDir.z = (s32)(((s64)floorNormNew.z * (s64)floorNormNew.y) >> 16);
                rmV3Scale(&slopeForce, &slopeDir, (s32)(slopeScale >> 16));
                slopeForce.y = 0;
                thing->contactForce.x += slopeForce.x;
                thing->contactForce.y += slopeForce.y;
                thing->contactForce.z += slopeForce.z;
                thing->flags |= 0x20000;
                outFloorNorm = floorNormNew;
            }

            // PSX: virtual HandleLand call if not already on ground
            if (!wasOnGround) {
                thing->HandleLand(localHomePosY);
            }

            // PSX: store ceiling norm into humanoid field436
            if (humanoid) {
                humanoid->field436 = ceilingNormNew.x;
            }

            const u16 thingType = thing->thingType;
            const bool isPickupWeaponType =
                static_cast<u16>(thingType - AITypes::TT_OBSTACLE_FIRST) < 28u;
            if (isPickupWeaponType) {
                Pickup* pickup = static_cast<Pickup*>(thing);
                if (pickup->field308 != 0) {
                    pickup->PlayEffect();
                    pickup->Kill();
                }
            }
        }
    }

    // Write back local variables if TF_DYNAMIC
    if (thing->flags & TF_DYNAMIC) {
        thing->homePos.x = localHomePosX;
        thing->homePos.y = localHomePosY;
        thing->homePos.z = localHomePosZ;
        thing->velocity.x = localVelX;
        thing->velocity.y = localVelY;
        thing->velocity.z = localVelZ;
    }

    // Store floor normal
    thing->field148[6] = outFloorNorm.x;
    thing->field148[7] = outFloorNorm.y;
    thing->field148[8] = outFloorNorm.z;

    // Set floor height on model
    thing->SetFloorHeight(bestFloorH);

    // Store ground height for standing-on detection
    if (thing->flags & TF_ON_GROUND) {
        thing->groundStandHeight = localHomePosY;
    }
}

// PSX: ClearThingFloorHeights__FR6ccList (COLMGR.CPP:1352) 0x800A9284
void ClearThingFloorHeights(ccList& list) {
    MARKFUNCTION(0x800A9284);
    for (ccMinNode* node = list.head; node != nullptr; node = node->next) {
        ((Thing*)node)->ClearFloorHeight();
    }
}

// PSX: HandleThingEnvironmentCollisions__FR6ccList (COLMGR.CPP:1395) 0x800A92C4
// Main collision dispatcher - iterates entity list, dispatches wall+floor handlers
void HandleThingEnvironmentCollisions(ccList& thingList) {
    MARKFUNCTION(0x800A92C4);

    g_combatTarget1 = nullptr;
    g_combatTarget2 = nullptr;

    for (ccMinNode* node = thingList.head; node != nullptr; node = node->next) {
        DynamicThing* thing = static_cast<DynamicThing*>(node);

        if ((thing->flags & TF_MODEL_CREATED) == 0) {
            continue;
        }

        s32 skipWall = 0;
        s32 radius = 128;
        s32 yMinOffset = 0;
        s32 checkHeight = 768;

        if (thing->thingType >= 29) {
            if (thing->thingType >= 301 && thing->thingType < 329) {
                yMinOffset = static_cast<Pickup*>(thing)->GetCollisionYMin();
            }
        }
        else {
            Humanoid* hum = static_cast<Humanoid*>(thing);
            const s32 state = hum->actionState;

            radius = g_colDefaultRadius;

            if (hum->collBboxMin.z < g_colDefaultHeight) {
                if (state != AS_THROW_CHARACTER_RECEIVE) {
                    radius = g_colMaxRadius;
                    checkHeight = g_colDefaultHeight;

                    bool useRootRadius = false;
                    if ((state >= 69 && state <= AS_DEAD) || state == 56) {
                        useRootRadius = true;
                    }
                    else if (state == 53 || state == 57 || state == AS_FLYING_BACK_LAND || state == AS_THROW_FREE_FALL) {
                        useRootRadius = true;
                    }

                    if (useRootRadius && thing->model) {
                        HumanoidModel* hmodel = static_cast<HumanoidModel*>(static_cast<Model*>(thing->model));
                        AnimationMatrices* animMatrices = hmodel ? hmodel->animMatrices : nullptr;
                        if (animMatrices) {
                            s32* rootMatrix = AnimationMatrices::GetMatrix(animMatrices, 0);
                            if (rootMatrix) {
                                const s32 rootRadius = (s32)rmMag2(
                                    (f32)(rootMatrix[5] - thing->pos.x),
                                    (f32)(rootMatrix[7] - thing->pos.z));

                                if (g_colDefaultRadius < rootRadius && rootRadius < g_colMaxRadius) {
                                    radius = rootRadius;
                                }
                            }
                        }
                    }
                }
            }
            // PSX 0x800A9358 initializes non-player height to 0x300. The
            // branch at 0x800A936C skips straight to 0x800A9470 when the
            // animated height is >= 256; it does NOT copy that height.
            // Only the player path (0x800A9560) uses the animated height.
            // In particular, state 59 keeps 768 even below the threshold.
            else if (thing == static_cast<DynamicThing*>(Player::s_player)) {
                checkHeight = hum->collBboxMin.z;
            }

            if (state == AS_LEDGE_LATCH || state == AS_LEDGE_PULLUP) {
                skipWall = 1;
            }
        }

        Thing* ticketIssuer = thing->GetTicketIssuer();

        if (skipWall == 0) {
            HandleThingWall(thing, radius, yMinOffset, checkHeight);
        }

        if (ticketIssuer) {
            thing->Land();
        }
        else {
            HandleThingFloor(thing, radius, yMinOffset, checkHeight);
        }

        if (skipWall != 0 && !thing->GetTicketIssuer() && (thing->flags & TF_ON_GROUND) == 0) {
            static_cast<Humanoid*>(thing)->LetGoOfLedge();
        }
    }

    // Post-loop: re-process combat targets with their collision bbox
    if (g_combatTarget1 != nullptr) {
        Humanoid* h = (Humanoid*)g_combatTarget1;
        HandleThingWall(g_combatTarget1, g_colDefaultRadius, h->collBboxMin.y, h->collBboxMin.z);
    }
    if (g_combatTarget2 != nullptr) {
        Humanoid* h = (Humanoid*)g_combatTarget2;
        HandleThingWall(g_combatTarget2, g_colDefaultRadius, h->collBboxMin.y, h->collBboxMin.z);
    }
}

// PSX: HandleHumanoidObstacleCollisions__FR6ccList (COLMGR.CPP:1667) 0x800A96EC
// Iterates humanoid list, dispatches per-humanoid obstacle collision handler
void HandleHumanoidObstacleCollisions(ccList& humanoidList) {
    MARKFUNCTION(0x800A96EC);

    for (ccMinNode* node = humanoidList.head; node != nullptr;) {
        ccMinNode* next = node->next;
        DynamicThing* thing = (DynamicThing*)node;
        if (thing->flags & TF_MODEL_CREATED) {
            Obstacle::HandleHumanoidObstacleCollision((Humanoid*)thing);
        }
        node = next;
    }
}

// PSX: HandlePickupObstacleCollisions__FR6ccList (COLMGR.CPP:1699) 0x800A9740
// Iterates a pickup list and dispatches each active pickup through Obstacle's scan helper.
void HandlePickupObstacleCollisions(ccList& pickupList) {
    MARKFUNCTION(0x800A9740);

    for (ccMinNode* node = pickupList.head; node != nullptr;) {
        ccMinNode* next = node->next;
        Pickup* pickup = static_cast<Pickup*>(static_cast<Thing*>(node));
        if (pickup->flags & TF_MODEL_CREATED) {
            Obstacle::HandlePickupObstacleCollision(pickup);
        }
        node = next;
    }
}

// PSX: HandleHumanoidPickupCollisions__FR6ccListT0 (COLMGR.CPP:1732) 0x800A9794
// Double loop: for each humanoid, check collision against each active pickup
void HandleHumanoidPickupCollisions(ccList& humanoidList, ccList& pickupList) {
    MARKFUNCTION(0x800A9794);

    tagCollisionSphere sphere;
    sphere.radius = 128;

    static constexpr s32 COLLISION_TAG_IMPACT_REGION = static_cast<s32>(0x80000002u);
    static constexpr s32 COLLISION_TAG_HIT_TYPE = static_cast<s32>(0x80000003u);
    static constexpr s32 COLLISION_TAG_FORCE = static_cast<s32>(0x80000005u);
    static constexpr s32 COLLISION_TAG_DAMAGE = static_cast<s32>(0x80000007u);
    static constexpr s32 COLLISION_TAG_END = 0;

    for (ccMinNode* hNode = humanoidList.head; hNode != nullptr;) {
        ccMinNode* hNext = hNode->next;
        Humanoid* humanoid = static_cast<Humanoid*>(static_cast<Thing*>(hNode));

        if (humanoid->flags & TF_MODEL_CREATED) {
            for (ccMinNode* pNode = pickupList.head; pNode != nullptr;) {
                ccMinNode* pNext = pNode->next;
                Pickup* pickup = static_cast<Pickup*>(static_cast<Thing*>(pNode));

                const s32 hasEffect = (pickup->velocity.x != 0 || pickup->velocity.z != 0) ? 1 : 0;
                if ((pickup->flags & TF_MODEL_CREATED) && hasEffect && pickup->ignoreCollisionOwner != humanoid) {
                    const tagCollisionCylinder& humCylinder =
                        *reinterpret_cast<const tagCollisionCylinder*>(&humanoid->collBboxMin);

                    if (CheckStaticCylinderSphereCollision(
                            humanoid->pos,
                            humCylinder,
                            pickup->pos,
                            sphere)) {
                        humanoid->HandleCollision(
                            pickup,
                            1,
                            COLLISION_TAG_HIT_TYPE,
                            14,
                            COLLISION_TAG_DAMAGE,
                            pickup->damage,
                            COLLISION_TAG_FORCE,
                            10000,
                            COLLISION_TAG_IMPACT_REGION,
                            sphere.radius,
                            COLLISION_TAG_END);

                        if (g_hud) {
                            g_hud->SetFoe(humanoid);
                        }

                        if (g_scoreManager) {
                            g_scoreManager->AddFightingPoints(100);
                            g_scoreManager->AddStylePoints(100);
                        }

                        pickup->PlayEffect();
                        pickup->Kill();
                    }
                }
                pNode = pNext;
            }
        }
        hNode = hNext;
    }
}

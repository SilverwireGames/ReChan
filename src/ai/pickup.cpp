#include "gen/common.h"
#include "ai/pickup.h"
#include "gen/animmat.h"
#include "gen/colsect.h"
#include "gen/database.h"
#include "gen/game.h"
#include "gen/geffect.h"
#include "gen/levelmgr.h"
#include "gen/model.h"
#include "gen/time.h"
#include "gen/world.h"
#include "p3d/hash.h"
#include "p3d/p3dmath.h"
#include "snd/sndfact.h"
#include "snd/wpnsnd.h"
#include <cstring>

static constexpr s32 PICKUP_THRESHOLD = 50;
static constexpr u32 PICKUP_IMPACT_EFFECT_HASH = 0x03E24164u;
static const LVector PICKUP_IMPACT_EFFECT_SCALE = { 39321, 39321, 39321 };

struct FightingSystemHashEntry {
    u32 hash = 0;
    u32 rootAddress = 0;
};

struct TypeFightingSystemEntry {
    u16 type = 0;
    u32 hash = 0;
};

struct PsxFightingNodeRaw {
    u32 address = 0;
    u32 packedCommand = 0;
    u32 field04 = 0;
    u32 moveData = 0;
    u32 childAddress = 0;
    u32 siblingAddress = 0;
};

struct PsxFightingMoveRaw {
    u32 address = 0;
    u32 firstWord = 0;
    s32 turnDelta = 0;
    u16 anim = 0;
    u16 fightingPoints = 0;
    u8 stylePointsFlag = 0;
    s8 moveWindowStart = 0;
    s8 moveWindowEnd = 0;
    s8 moveDelta = 0;
    s8 combatWindowStart = 0;
    s8 combatWindowEnd = 0;
    u8 weaponBreakOnEmpty = 0;
    u8 fightingType = 0;
    u32 data20 = 0;
    u32 data24 = 0;
    s16 throwVectorX = 0;
    s16 throwVectorY = 0;
    s16 throwVectorZ = 0;
    s16 throwAttachX = 0;
    s16 throwAttachY = 0;
    s16 throwAttachZ = 0;
    u16 throwTargetAnim = 0;
    s8 throwImpactFrame = 0;
    s8 throwAttachFrame = 0;
    s8 throwReleaseFrame = 0;
    s8 throwScoreFrame = 0;
};

struct PsxFightingJointRaw {
    u32 address = 0;
    u32 word0 = 0;
    u32 word1 = 0;
    u32 word2 = 0;
    u32 word3 = 0;
    u32 word4 = 0;
    u32 word5 = 0;

    s8 AttackStartFrame() const { return static_cast<s8>(word4 & 0xFFu); }
};

#include "ai/fightani_data.inl"
#include "ai/fightmove_data.inl"
#include "ai/fightjoint_data.inl"

struct WeaponTypePickupEntry {
    u16 type = 0;
    u16 idleAnim = 0;
    u32 highMove = 0;
    u32 lowMove = 0;
    u32 throwMove = 0;
};

static const s32 pole1Straif[] = {
    189, 0,
    193, 0,
    193, 1,
    194, 0,
    194, 1,
};

static const s32 pole2Straif[] = {
    206, 0,
    210, 0,
    210, 1,
    211, 0,
    211, 1,
};

static const s32 pole3Straif[] = {
    220, 0,
    224, 0,
    224, 1,
    225, 0,
    225, 1,
};

static const s32 drunkenStraif[] = {
    261, 0,
    264, 0,
    264, 1,
    266, 1,
    266, 0,
};

static const TypeFightingSystemEntry kWeaponTypeFightingSystemTable[] = {
    { 305, 0x0AB660C5u },
    { 302, 0x0B821985u },
    { 301, 0x02B224D5u },
    { 303, 0x05B590B5u },
    { 306, 0x0776A3B5u },
    { 307, 0x0D09E3D5u },
    { 308, 0x04DB1075u },
    { 309, 0x072FA3B5u },
    { 310, 0x0C300C25u },
    { 311, 0x0B5923E5u },
    { 312, 0x09C51505u },
    { 313, 0x02B224D5u },
    { 315, 0x0B822985u },
    { 316, 0x02B304D5u },
    { 317, 0x0B821985u },
    { 318, 0x0B82F985u },
    { 319, 0x0B821985u },
    { 320, 0x0D09E3D5u },
    { 321, 0x02B304D5u },
    { 322, 0x0B82F985u },
    { 323, 0x0D09E3D5u },
    { 324, 0x02B214D5u },
    { 325, 0x02B214D5u },
    { 326, 0x0B822985u },
    { 327, 0x02B214D5u },
    { 328, 0x0B5923E5u },
};

static const WeaponTypePickupEntry kWeaponTypePickupSystemTable[] = {
    { 301, 189, 0x800D0FA8u, 0x800D0F8Cu, 0x800D0FE0u },
    { 309, 231, 0x800D12B8u, 0x800D129Cu, 0x800D12D4u },
    { 310, 244, 0x800D1360u, 0x800D1344u, 0x800D137Cu },
    { 311, 254, 0x800D1408u, 0x800D13ECu, 0x800D1424u },
    { 312, 261, 0x800D14E8u, 0x800D14CCu, 0x800D1504u },
    { 313, 189, 0x800D0FA8u, 0x800D0F8Cu, 0x800D0FE0u },
    { 316, 220, 0x800D11D8u, 0x800D11BCu, 0x800D11A0u },
    { 321, 220, 0x800D11D8u, 0x800D11BCu, 0x800D11A0u },
    { 324, 206, 0x800D10DCu, 0x800D10C0u, 0x800D10A4u },
    { 325, 206, 0x800D10DCu, 0x800D10C0u, 0x800D10A4u },
    { 327, 206, 0x800D10DCu, 0x800D10C0u, 0x800D10A4u },
    { 328, 254, 0x800D1408u, 0x800D13ECu, 0x800D1424u },
};

static const s32* GetMoveStruct(u16 type) {
    MARKFUNCTION(0x8006D3F4);

    switch (type) {
    case 301:
    case 313:
        return pole1Straif;
    case 312:
        return drunkenStraif;
    case 316:
    case 321:
        return pole3Straif;
    case 324:
    case 325:
    case 327:
        return pole2Straif;
    default:
        return nullptr;
    }
}

static u32 FindPickupFightingRootAddress(u32 hash, u32 fallbackAddress = 0) {
    for (const FightingSystemHashEntry& entry : kFightingSystemTable) {
        if (entry.hash == hash) {
            return entry.rootAddress;
        }
    }

    return fallbackAddress;
}

static u32 GetPickupFighting(u16 type) {
    MARKFUNCTION(0x8007DD5C);

    for (const TypeFightingSystemEntry& entry : kWeaponTypeFightingSystemTable) {
        if (entry.type == type) {
            // PSX GetPickupFighting routes through FindFightingSystem, which
            // falls back to Player_Punch_Root when hash lookup misses.
            return FindPickupFightingRootAddress(entry.hash, kPlayerPunchRootAddress);
        }
    }

    return 0;
}

static const WeaponTypePickupEntry* FindWeaponTypePickupEntry(u16 type) {
    for (const WeaponTypePickupEntry& entry : kWeaponTypePickupSystemTable) {
        if (entry.type == type) {
            return &entry;
        }
    }

    return nullptr;
}

static u32 GetPickupFightingHighPickup(u16 type) {
    MARKFUNCTION(0x8007DD88);

    const WeaponTypePickupEntry* entry = FindWeaponTypePickupEntry(type);
    return entry ? entry->highMove : 0x800D042Cu;
}

static u32 GetPickupFightingLowPickup(u16 type) {
    MARKFUNCTION(0x8007DDDC);

    const WeaponTypePickupEntry* entry = FindWeaponTypePickupEntry(type);
    return entry ? entry->lowMove : 0x800D0410u;
}

static u32 GetPickupFightingThrow(u16 type) {
    MARKFUNCTION(0x8007DE30);

    const WeaponTypePickupEntry* entry = FindWeaponTypePickupEntry(type);
    return entry ? entry->throwMove : 0x800D0448u;
}

static s32 GetPickupFightingIdle(u16 type) {
    MARKFUNCTION(0x8007DE84);

    const WeaponTypePickupEntry* entry = FindWeaponTypePickupEntry(type);
    return entry ? entry->idleAnim : 22;
}

static const PsxFightingMoveRaw* ResolveFightingMoveAddress(u32 address) {
    if (!address) {
        return nullptr;
    }

    for (const PsxFightingMoveRaw& move : kPsxFightingMoveTable) {
        if (move.address == address) {
            return &move;
        }
    }

    return nullptr;
}

static const PsxFightingJointRaw* ResolveFightingJointAddress(u32 address) {
    if (!address) {
        return nullptr;
    }

    for (const PsxFightingJointRaw& joint : kPsxFightingJointTable) {
        if (joint.address == address) {
            return &joint;
        }
    }

    return nullptr;
}

struct PickupMoveFallbackEntry {
    u32 address = 0;
    u16 anim = 0;
    u32 jointAddress = 0;
};

struct PickupJointFallbackEntry {
    u32 address = 0;
    s8 attackStartFrame = 0;
};

// These rows are present in PSX pickup/fighting data but not yet in shared extracted tables.
static const PickupMoveFallbackEntry kPickupMoveFallbackTable[] = {
    { 0x800D0410u, 0x0014u, 0x800D1D64u },
    { 0x800D042Cu, 0x0013u, 0x800D1D7Cu },
    { 0x800D0448u, 0x0015u, 0x800D1D94u },
    { 0x800D0F8Cu, 0x00BFu, 0x800D273Cu },
    { 0x800D0FA8u, 0x00C0u, 0x800D2754u },
    { 0x800D0FE0u, 0x00CAu, 0x800D2784u },
    { 0x800D10A4u, 0x00D8u, 0x800D282Cu },
    { 0x800D10C0u, 0x00D0u, 0x800D2844u },
    { 0x800D10DCu, 0x00D1u, 0x800D285Cu },
    { 0x800D11A0u, 0x00E6u, 0x800D2904u },
    { 0x800D11BCu, 0x00DEu, 0x800D291Cu },
    { 0x800D11D8u, 0x00DFu, 0x800D2934u },
    { 0x800D129Cu, 0x00E8u, 0x800D29DCu },
    { 0x800D12B8u, 0x00E9u, 0x800D29F4u },
    { 0x800D12D4u, 0x00EEu, 0x800D2A0Cu },
    { 0x800D1344u, 0x00F5u, 0x800D2A6Cu },
    { 0x800D1360u, 0x00F6u, 0x800D2A84u },
    { 0x800D137Cu, 0x00F8u, 0x800D2A9Cu },
    { 0x800D13ECu, 0x00FFu, 0x800D2AFCu },
    { 0x800D1408u, 0x0100u, 0x800D2B14u },
    { 0x800D1424u, 0x0102u, 0x800D2B2Cu },
    { 0x800D14CCu, 0x0106u, 0x800D2BBCu },
    { 0x800D14E8u, 0x0107u, 0x800D2BD4u },
    { 0x800D1504u, 0x0111u, 0x800D2BECu },
};

static const PickupJointFallbackEntry kPickupJointFallbackTable[] = {
    { 0x800D1D64u, 6 },
    { 0x800D1D7Cu, 6 },
    { 0x800D1D94u, 8 },
    { 0x800D273Cu, 7 },
    { 0x800D2754u, 7 },
    { 0x800D2784u, 8 },
    { 0x800D282Cu, 8 },
    { 0x800D2844u, 7 },
    { 0x800D285Cu, 7 },
    { 0x800D2904u, 8 },
    { 0x800D291Cu, 7 },
    { 0x800D2934u, 7 },
    { 0x800D29DCu, 7 },
    { 0x800D29F4u, 7 },
    { 0x800D2A0Cu, 7 },
    { 0x800D2A6Cu, 7 },
    { 0x800D2A84u, 7 },
    { 0x800D2A9Cu, 7 },
    { 0x800D2AFCu, 7 },
    { 0x800D2B14u, 7 },
    { 0x800D2B2Cu, 7 },
    { 0x800D2BBCu, 7 },
    { 0x800D2BD4u, 7 },
    { 0x800D2BECu, 7 },
};

static const PickupMoveFallbackEntry* ResolvePickupMoveFallbackAddress(u32 address) {
    if (!address) {
        return nullptr;
    }

    for (const PickupMoveFallbackEntry& entry : kPickupMoveFallbackTable) {
        if (entry.address == address) {
            return &entry;
        }
    }

    return nullptr;
}

static s8 ResolvePickupJointAttackStartFrame(u32 address) {
    if (!address) {
        return 0;
    }

    const PsxFightingJointRaw* joint = ResolveFightingJointAddress(address);
    if (joint) {
        return joint->AttackStartFrame();
    }

    for (const PickupJointFallbackEntry& entry : kPickupJointFallbackTable) {
        if (entry.address == address) {
            return entry.attackStartFrame;
        }
    }

    return 0;
}

static s32 SetDefaultCollisionPoint(const DBRoot& root, u32 attribNum, LVector& outA, LVector& outB) {
    MARKFUNCTION(0x8006D208);

    const DBAttrib* attrib = root.FindAttrib(attribNum);
    if (!attrib) {
        return 0;
    }

    const char* name = attrib->GetAttribString();
    if (!name || !g_levelManager) {
        return 0;
    }

    OriginalBasic* original = g_levelManager->FindGeo((s32)p3dHash(name));
    if (!original || original->GetType() != 0) {
        return 0;
    }

    OriginalGeo* geo = static_cast<OriginalGeo*>(original);
    if (!geo->dynamicVerts || geo->dynamicVertCount == 0) {
        return 0;
    }

    const GeoRenderVertex& first = geo->dynamicVerts[0];
    outA.x = static_cast<s32>(first.x);
    outA.y = static_cast<s32>(first.y);
    outA.z = static_cast<s32>(first.z);
    outB = outA;

    s32 farthestMag = rmMag3ff(outA.x, outA.y, outA.z);
    s32 lowestY = outB.y;

    for (u32 i = 1; i < geo->dynamicVertCount; i++) {
        const GeoRenderVertex& vertex = geo->dynamicVerts[i];
        const s32 vx = static_cast<s32>(vertex.x);
        const s32 vy = static_cast<s32>(vertex.y);
        const s32 vz = static_cast<s32>(vertex.z);
        const s32 vertexMag = rmMag3ff(vx, vy, vz);

        if (vertexMag > farthestMag) {
            farthestMag = vertexMag;
            outA.x = vx;
            outA.y = vy;
            outA.z = vz;
        }

        if (vy < lowestY) {
            lowestY = vy;
            outB.x = vx;
            outB.y = vy;
            outB.z = vz;
        }
    }

    return 1;
}

static void TransformVectorByPsxMatrix(const s32* matrix, const LVector& in, LVector& out) {
    if (!matrix) {
        out = {};
        return;
    }

    const s16* rot = reinterpret_cast<const s16*>(matrix);
    out.x = matrix[5]
        + (s32)((((s64)in.x * rot[0])
            + ((s64)in.y * rot[1])
            + ((s64)in.z * rot[2])) >> 12);
    out.y = matrix[6]
        + (s32)((((s64)in.x * rot[3])
            + ((s64)in.y * rot[4])
            + ((s64)in.z * rot[5])) >> 12);
    out.z = matrix[7]
        + (s32)((((s64)in.x * rot[6])
            + ((s64)in.y * rot[7])
            + ((s64)in.z * rot[8])) >> 12);
}

Pickup::Pickup(const LVector* initialPos, u16 type)
    : DynamicThing(initialPos, type) {
    MARKFUNCTION(0x8006D45C);

    moveStruct = GetMoveStruct(type);
    health = 6;
    maxHealth = 6;
    weaponSound = nullptr;
    pickupFlags = 0;
    field328 = 0;
    fightingSystemRoot = GetPickupFighting(type);
    idleAnim = GetPickupFightingIdle(type);
    highPickupMove = GetPickupFightingHighPickup(type);
    lowPickupMove = GetPickupFightingLowPickup(type);
    currentPickupMove = highPickupMove;
    throwMove = GetPickupFightingThrow(type);
    collisionPointCount = 0;
    attachOffset = {};
    damage = 20;
    field308 = 0;
    deactivateFlag = 0;
    weaponField = 0;
    if (type == 314) {
        weaponField = 1;
    }
    field332 = 0;
    maxSpeed = 200;
}

Pickup::~Pickup() {
    MARKFUNCTION(0x8006D5B0);
    if (weaponSound) {
        weaponSound->Release();
        weaponSound = nullptr;
    }
}

void Pickup::Reset() {
    MARKFUNCTION(0x8006D618);

    const LVector savedOrientation = orientation;
    DynamicThing::Reset();
    orientation = savedOrientation;
    attachedOwner = nullptr;
    ignoreCollisionOwner = nullptr;
    maxSpeed = 200;
}

void Pickup::CreateModel(const char* name) {
    MARKFUNCTION(0x8006D688);

    if (!model) {
        model = new GModel();
    }

    Thing::CreateModel(name);

    Model* modelPtr = static_cast<Model*>(model);
    if (modelPtr) {
        if (!modelPtr->hwLights) {
            modelPtr->AllocateHardwareLights(3);
        }
        if (!modelPtr->ambientLight) {
            modelPtr->AllocateAmbientLight();
        }
    }

    if (!weaponSound) {
        CSound* tmp = nullptr;
        if (CSoundFactory::CreateObject(10050, &tmp, thingType) >= 0) {
            weaponSound = static_cast<CWeaponSound*>(tmp);
            weaponSound->Initialize(&pos);
        }
    }
}

void Pickup::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8006D710);

    if (!root) {
        return;
    }

    Thing::AnalyzeMesh(root);

    orientation.x = root->field40;
    orientation.y = root->field44;
    orientation.z = root->field48;

    collisionPointCount = 0;

    if (const DBAttrib* attrib = root->FindAttrib(9)) {
        collisionPoints[0].x = static_cast<s32>(attrib->value);
        collisionPointCount = 1;
    }
    if (const DBAttrib* attrib = root->FindAttrib(10)) {
        collisionPoints[0].y = static_cast<s32>(attrib->value);
        collisionPointCount = 1;
    }
    if (const DBAttrib* attrib = root->FindAttrib(11)) {
        collisionPoints[0].z = static_cast<s32>(attrib->value);
        collisionPointCount = 1;
    }
    if (const DBAttrib* attrib = root->FindAttrib(12)) {
        collisionPoints[1].x = static_cast<s32>(attrib->value);
        collisionPointCount = 2;
    }
    if (const DBAttrib* attrib = root->FindAttrib(13)) {
        collisionPoints[1].y = static_cast<s32>(attrib->value);
        collisionPointCount = 2;
    }
    if (const DBAttrib* attrib = root->FindAttrib(14)) {
        collisionPoints[1].z = static_cast<s32>(attrib->value);
        collisionPointCount = 2;
    }
    if (root->FindAttrib(22)) {
        field332 = 1;
    }
    if (const DBAttrib* attrib = root->FindAttrib(30)) {
        const char* attribString = attrib->GetAttribString();
        if (attribString) {
            // PSX AnalyzeMesh sets +212 via FindFightingSystem(hash), which falls back
            // to Player_Punch_Root when the hash is not present in fightingSystemTable.
            fightingSystemRoot = FindPickupFightingRootAddress((u32)p3dHash(attribString), kPlayerPunchRootAddress);
        }
    }
    if (root->FindAttrib(31)) {
        deactivateFlag = 1;
    }

    if (collisionPointCount == 0) {
        LVector outA = {};
        LVector outB = {};
        if (SetDefaultCollisionPoint(*root, 5, outA, outB) != 0) {
            collisionPoints[0] = outA;
            collisionPoints[1].x = outA.x / 2;
            collisionPoints[1].y = outA.y / 2;
            collisionPoints[1].z = outA.z / 2;
            collisionPoints[2] = outB;
            collisionPointCount = 3;
        }
        else {
            collisionPointCount = 1;
        }
    }
}

void Pickup::Think() {
    MARKFUNCTION(0x8006D984);

    Move();
    if (g_game) {
        if (World* world = g_game->GetWorld()) {
            world->CheckThingSwitches(this);
        }
    }
}

s32 Pickup::SetupPickup(Thing* owner, u32 joint) {
    MARKFUNCTION(0x8006D9D0);

    deactivateFlag = 0;
    pickupFlags |= 4u;
    s32 result = 0;
    if (model) {
        Model* pickupModel = static_cast<Model*>(model);
        pickupModel->modelFlags |= 1u;

        if (pickupModel->field36 && owner && owner->model) {
            const Model* ownerModel = static_cast<const Model*>(owner->model);
            const ModelFloorHeightState* ownerShadowState = GetModelFloorHeightState(ownerModel);
            ModelFloorHeightState* pickupShadowState = GetModelFloorHeightState(pickupModel);
            if (pickupShadowState && ownerShadowState) {
                pickupShadowState->previous = ownerShadowState->previous;
                pickupShadowState->current = ownerShadowState->current;
            }
        }

        result = static_cast<s32>(pickupModel->modelFlags);
    }
    attachedOwner = owner;
    attachJoint = static_cast<s32>(joint);
    return result;
}

void Pickup::UpdatePosition() {
    MARKFUNCTION(0x8006DA00);

    attachOffset = {};

    if ((pickupFlags & 4u) != 0 && attachedOwner && attachedOwner->model) {
        HumanoidModel* modelPtr = static_cast<HumanoidModel*>(attachedOwner->model);

        if (model && modelPtr && modelPtr->field36) {
            Model* pickupModel = static_cast<Model*>(model);
            ModelFloorHeightState* pickupShadowState = GetModelFloorHeightState(pickupModel);
            const ModelFloorHeightState* ownerShadowState = GetModelFloorHeightState(modelPtr);
            if (pickupShadowState && ownerShadowState) {
                pickupShadowState->previous = ownerShadowState->previous;
                pickupShadowState->current = ownerShadowState->current;
            }
        }

        AnimationMatrices* animMatrices = modelPtr ? modelPtr->animMatrices : nullptr;
        if (animMatrices) {
            s32 modelMatrix[8] = {};
            bool haveMatrix = false;

#if HIGH_FPS_PLAY_PRESENTATION
            const bool pickupInPlay = (g_time && g_game && g_game->GetState() == GameState::Play);
            if (pickupInPlay) {
                s32 prevMatrix[8] = {};
                s32 curMatrix[8] = {};
                if (animMatrices->GetMatrixPair(static_cast<u32>(attachJoint), prevMatrix, curMatrix)) {
                    f32 alpha = g_time->GetPlayPresentationAlpha();
                    if (alpha < 0.0f) alpha = 0.0f;
                    if (alpha > 1.0f) alpha = 1.0f;

                    const s16* prevRot = reinterpret_cast<const s16*>(prevMatrix);
                    const s16* curRot = reinterpret_cast<const s16*>(curMatrix);
                    s16* outRot = reinterpret_cast<s16*>(modelMatrix);
                    for (s32 i = 0; i < 9; i++) {
                        outRot[i] = (s16)(prevRot[i] + (s32)((f32)(curRot[i] - prevRot[i]) * alpha));
                    }

                    for (s32 i = 5; i <= 7; i++) {
                        modelMatrix[i] = prevMatrix[i] + (s32)((f32)(curMatrix[i] - prevMatrix[i]) * alpha);
                    }

                    haveMatrix = true;
                }
            }
#endif

            if (!haveMatrix) {
                const s32* matrix = AnimationMatrices::GetMatrix(animMatrices, static_cast<u32>(attachJoint));
                if (matrix) {
                    std::memcpy(modelMatrix, matrix, sizeof(modelMatrix));
                    haveMatrix = true;
                }
            }

            if (haveMatrix) {
                const s32 ownerScale = modelPtr->scale;
                if (ownerScale != FIX16_ONE) {
                    const s32 invScale = rmDiv16i(FIX16_ONE, ownerScale);
                    s16* rot = reinterpret_cast<s16*>(modelMatrix);
                    for (s32 i = 0; i < 9; i++) {
                        rot[i] = (s16)(((s64)rot[i] * invScale) >> 16);
                    }
                }

                LVector attachWorld = {};
                TransformVectorByPsxMatrix(modelMatrix, attachOffset, attachWorld);
                modelMatrix[5] = attachWorld.x;
                modelMatrix[6] = attachWorld.y;
                modelMatrix[7] = attachWorld.z;

                if (model) {
                    Model* pickupModel = static_cast<Model*>(model);
                    if (pickupModel->drawableType == 1) {
                        GModel* geoModel = static_cast<GModel*>(pickupModel);
                        std::memcpy(geoModel->attachedMatrix, modelMatrix, sizeof(modelMatrix));
                        geoModel->attachedMatrixActive = 1;
                    }
                }

                homePos = attachWorld;
            }
        }
    }

    pos = homePos;
    if (g_blockManager) {
        blockNum = g_blockManager->GetBlockNumber(homePos);
    }
}

s32 Pickup::Release(Thing* owner, ccList* list, const SVector* forceDir, s32 forceMag) {
    MARKFUNCTION(0x8006DBC0);

    if (list) {
        list->AddNode(nullptr, static_cast<ccMinNode*>(this));
    }

#if HIGH_FPS_PLAY_PRESENTATION
    // A held pickup is only positioned from Humanoid::Draw, off a render-interpolated
    // hand matrix. The wall check and throw origin below must start from the hand as
    // captured on the logic tick, not from wherever the last rendered frame put it.
    if ((pickupFlags & 4u) != 0 && attachedOwner && attachedOwner->model
        && g_game && g_game->GetState() == GameState::Play) {
        HumanoidModel* ownerModel = static_cast<HumanoidModel*>(attachedOwner->model);
        const s32* matrix = AnimationMatrices::GetMatrix(ownerModel->animMatrices, static_cast<u32>(attachJoint));
        if (matrix) {
            homePos.x = matrix[5];
            homePos.y = matrix[6];
            homePos.z = matrix[7];
            pos = homePos;
            if (g_blockManager) {
                blockNum = g_blockManager->GetBlockNumber(homePos);
            }
        }
    }
#endif

    flags &= ~TF_ON_GROUND;
    pickupFlags &= ~4u;
    if (model) {
        Model* pickupModel = static_cast<Model*>(model);
        pickupModel->modelFlags &= ~1u;
        if (pickupModel->drawableType == 1) {
            static_cast<GModel*>(pickupModel)->attachedMatrixActive = 0;
        }
    }
    ignoreCollisionOwner = owner;

    if (owner && owner->thingType == AITypes::TT_BUTCH) {
        if (forceDir && forceMag != 0) {
            LOG("[ChefPot] Pickup::Release owner=%p ownerName='%s' pickup=%p pickupName='%s' type=%u pos=(%d,%d,%d) forceDir=(%d,%d,%d) forceMag=%d",
                owner,
                owner->GetName(),
                this,
                GetName(),
                thingType,
                pos.x,
                pos.y,
                pos.z,
                forceDir->x,
                forceDir->y,
                forceDir->z,
                forceMag);
        }
        else {
            LOG("[ChefPot] Pickup::Release owner=%p ownerName='%s' pickup=%p pickupName='%s' type=%u pos=(%d,%d,%d) forceMag=0",
                owner,
                owner->GetName(),
                this,
                GetName(),
                thingType,
                pos.x,
                pos.y,
                pos.z);
        }
    }

    if (forceDir && forceMag != 0) {
        AddForce(forceMag, forceDir);
    }

    Thing* heldBy = attachedOwner;
    if (heldBy) {
        s32 hitRatio = 0;
        LVector wallNormal = {};
        LVector hitPoint = {};
        s32 wallHorizontal = 0;

        if (CollisionSector::CheckWorldWallCollision(
                heldBy->pos,
                pos,
                0,
                0,
                0,
                hitRatio,
                wallNormal,
                hitPoint,
                wallHorizontal)) {
            const s32 currentY = pos.y;
            const s32 currentZ = pos.z;
            pos.x = hitPoint.x;
            pos.y = currentY;
            pos.z = currentZ;
            PlayEffect();
            Kill();
        }
    }

    attachedOwner = nullptr;
    return 0;
}

void Pickup::Move() {
    MARKFUNCTION(0x8006DD00);

    if (deactivateFlag == 0) {
        DynamicThing::Move();
    }
}

void Pickup::HandleCollision(Thing* other, s32 damage, ...) {
    MARKFUNCTION(0x8006DD30);
    Thing::HandleCollision(other, damage);
}

void Pickup::DamageExtra() {
    MARKFUNCTION(0x8006DD5C);
}

s32 Pickup::PlayEffect() {
    MARKFUNCTION(0x8006DD64);

    GEffect_Create(PICKUP_IMPACT_EFFECT_HASH, &pos, &PICKUP_IMPACT_EFFECT_SCALE, nullptr, 0, 0, 0x80000010u);

    if (weaponSound) {
        weaponSound->Explode();
    }
    ignoreCollisionOwner = nullptr;
    return 0;
}

bool Pickup::PickupDeactivate() const {
    MARKFUNCTION(0x8006DDDC);

    const bool stopped = velocity.x == 0 && velocity.y == 0 && velocity.z == 0;
    return ((attachedOwner == nullptr && (flags & TF_ON_GROUND) != 0 && stopped) || deactivateFlag != 0);
}

s32 Pickup::GetCollisionYMin() const {
    MARKFUNCTION(0x8006DEDC);

    s32 yMin = 0;
    Mat4 rotMatrix;
    p3dBuildRotMatrixZYX((u16)orientation.x, (u16)orientation.y, (u16)orientation.z, rotMatrix);

    for (s32 index = 0; index < collisionPointCount; index++) {
        const LVector& point = collisionPoints[index];
        Vec3 rotated = p3dVecTimesRotMatrix(Vec3((f32)point.x, (f32)point.y, (f32)point.z), rotMatrix);
        const s32 rotatedY = static_cast<s32>(rotated.y);
        if (rotatedY < yMin) {
            yMin = rotatedY;
        }
    }

    return yMin;
}

CWeaponSound* Pickup::GetWeaponSoundPtr() {
    MARKFUNCTION(0x8006DF9C);
    return weaponSound;
}

const CWeaponSound* Pickup::GetWeaponSoundPtr() const {
    MARKFUNCTION(0x8006DF9C);
    return weaponSound;
}

s32 Pickup::SetPickupMove(s32 compareY) {
    MARKFUNCTION(0x8006DFA8);

    currentPickupMove = (PICKUP_THRESHOLD >= compareY - pos.y) ? highPickupMove : lowPickupMove;
    return static_cast<s32>(currentPickupMove);
}

u16 Pickup::GetPickupMove() const {
    MARKFUNCTION(0x8006DFD8);

    const PsxFightingMoveRaw* move = ResolveFightingMoveAddress(currentPickupMove);
    if (move) {
        return move->anim;
    }

    const PickupMoveFallbackEntry* fallback = ResolvePickupMoveFallbackAddress(currentPickupMove);
    return fallback ? fallback->anim : 0;
}

s8 Pickup::GetPickupMoveGrabFrame() const {
    MARKFUNCTION(0x8006DFEC);

    const PsxFightingMoveRaw* move = ResolveFightingMoveAddress(currentPickupMove);
    if (move) {
        return ResolvePickupJointAttackStartFrame(move->data20);
    }

    const PickupMoveFallbackEntry* fallback = ResolvePickupMoveFallbackAddress(currentPickupMove);
    if (!fallback) {
        return 0;
    }

    return ResolvePickupJointAttackStartFrame(fallback->jointAddress);
}

u16 Pickup::GetThrowMove() const {
    MARKFUNCTION(0x8006E008);

    const PsxFightingMoveRaw* move = ResolveFightingMoveAddress(throwMove);
    if (move) {
        return move->anim;
    }

    const PickupMoveFallbackEntry* fallback = ResolvePickupMoveFallbackAddress(throwMove);
    return fallback ? fallback->anim : 0;
}

s8 Pickup::GetThrowMoveThrowFrame() const {
    MARKFUNCTION(0x8006E01C);

    const PsxFightingMoveRaw* move = ResolveFightingMoveAddress(throwMove);
    if (move) {
        return ResolvePickupJointAttackStartFrame(move->data20);
    }

    const PickupMoveFallbackEntry* fallback = ResolvePickupMoveFallbackAddress(throwMove);
    if (!fallback) {
        return 0;
    }

    return ResolvePickupJointAttackStartFrame(fallback->jointAddress);
}

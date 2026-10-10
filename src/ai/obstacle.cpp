#include "gen/common.h"
#include "ai/obstacle.h"
#include "ai/obstacle_shared.h"
#include "ai/activezn.h"
#include "ai/humanoid.h"
#include "ai/pickup.h"
#include "ai/player.h"
#include "gen/ai.h"
#include "gen/animmat.h"
#include "gen/blockmgr.h"
#include "gen/camera.h"
#include "gen/colsect.h"
#include "gen/config.h"
#include "gen/colvol.h"
#include "gen/database.h"
#include "gen/director.h"
#include "gen/display.h"
#include "gen/game.h"
#include "gen/geffect.h"
#include "gen/levelmgr.h"
#include "gen/model.h"
#include "gen/path.h"
#include "gen/psxmath_helpers.h"
#include "gen/world.h"
#include "p3d/byteread.h"
#include "p3d/p3dmath.h"
#include "p3d/skeleton.h"
#include "snd/snddrct.h"
#include "gen/time.h"
#include "extra/shadowcsm.h"
#include <new>

const LVector ZERO_DELTA_VELOCITY = { 0, 0, 0 };

const tagCollisionBox INVALID_COLLISION_BOX = {
    0x7FFF, 0x7FFF, 0x7FFF,
    -0x7FFF, -0x7FFF, -0x7FFF,
    -0x7FFF, 0
};

#if HIGH_FPS_PLAY_PRESENTATION
static constexpr s32 MAX_OBSTACLE_RENDER_SMOOTH_STATES = 512;

struct ObstacleRenderSmoothState {
    const Thing* owner = nullptr;
    bool initialized = false;
    LVector prevPos = {};
    LVector curPos = {};
    LVector prevOrient = {};
    LVector curOrient = {};
};

static ObstacleRenderSmoothState s_obstacleRenderSmoothStates[MAX_OBSTACLE_RENDER_SMOOTH_STATES] = {};

static ObstacleRenderSmoothState* FindObstacleRenderSmoothState(const Thing* owner) {
    if (!owner) {
        return nullptr;
    }

    ObstacleRenderSmoothState* freeState = nullptr;
    for (s32 i = 0; i < MAX_OBSTACLE_RENDER_SMOOTH_STATES; i++) {
        ObstacleRenderSmoothState* state = &s_obstacleRenderSmoothStates[i];
        if (state->owner == owner) {
            return state;
        }
        if (!freeState && state->owner == nullptr) {
            freeState = state;
        }
    }

    if (!freeState) {
        return nullptr;
    }

    freeState->owner = owner;
    freeState->initialized = false;
    freeState->prevPos = {};
    freeState->curPos = {};
    freeState->prevOrient = {};
    freeState->curOrient = {};
    return freeState;
}

static void ClearObstacleRenderSmoothState(const Thing* owner) {
    if (!owner) {
        return;
    }

    for (s32 i = 0; i < MAX_OBSTACLE_RENDER_SMOOTH_STATES; i++) {
        ObstacleRenderSmoothState* state = &s_obstacleRenderSmoothStates[i];
        if (state->owner == owner) {
            state->owner = nullptr;
            state->initialized = false;
            state->prevPos = {};
            state->curPos = {};
            state->prevOrient = {};
            state->curOrient = {};
            return;
        }
    }
}
#endif

void ObstacleBuildRenderTransform(
    const Thing* owner,
    const LVector& logicPos,
    const LVector& logicOrientation,
    LVector& outPos,
    LVector& outOrientation) {
    outPos = logicPos;
    outOrientation = logicOrientation;

#if HIGH_FPS_PLAY_PRESENTATION
    const bool obstacleInPlay = (g_time && g_game && g_game->GetState() == GameState::Play);
    if (!obstacleInPlay) {
        ClearObstacleRenderSmoothState(owner);
        return;
    }

    ObstacleRenderSmoothState* smoothState = FindObstacleRenderSmoothState(owner);
    if (!smoothState) {
        return;
    }

    if (!smoothState->initialized) {
        smoothState->prevPos = logicPos;
        smoothState->curPos = logicPos;
        smoothState->prevOrient = logicOrientation;
        smoothState->curOrient = logicOrientation;
        smoothState->initialized = true;
    }

    const bool didLogicStep = g_time->DidPlayLogicStepThisFrame();
    if (didLogicStep) {
        smoothState->prevPos = smoothState->curPos;
        smoothState->curPos = logicPos;
        smoothState->prevOrient = smoothState->curOrient;
        smoothState->curOrient = logicOrientation;
    }

    f32 alpha = g_time->GetPlayPresentationAlpha();
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    else if (alpha > 1.0f) {
        alpha = 1.0f;
    }

    const s32 stepDx = smoothState->curPos.x - smoothState->prevPos.x;
    const s32 stepDy = smoothState->curPos.y - smoothState->prevPos.y;
    const s32 stepDz = smoothState->curPos.z - smoothState->prevPos.z;

    outPos.x = smoothState->prevPos.x + (s32)((f32)stepDx * alpha);
    outPos.y = smoothState->prevPos.y + (s32)((f32)stepDy * alpha);
    outPos.z = smoothState->prevPos.z + (s32)((f32)stepDz * alpha);

    const s32 stepRX = (s16)((u16)smoothState->curOrient.x - (u16)smoothState->prevOrient.x);
    const s32 stepRY = (s16)((u16)smoothState->curOrient.y - (u16)smoothState->prevOrient.y);
    const s32 stepRZ = (s16)((u16)smoothState->curOrient.z - (u16)smoothState->prevOrient.z);

    outOrientation.x = (u16)(smoothState->prevOrient.x + (s32)((f32)stepRX * alpha));
    outOrientation.y = (u16)(smoothState->prevOrient.y + (s32)((f32)stepRY * alpha));
    outOrientation.z = (u16)(smoothState->prevOrient.z + (s32)((f32)stepRZ * alpha));
#endif
}

void ObstacleForgetRenderTransform(const Thing* owner) {
#if HIGH_FPS_PLAY_PRESENTATION
    ClearObstacleRenderSmoothState(owner);
#else
    (void)owner;
#endif
}

// PSX: FillVectorArray__8ObstacleP10tagLVectorRC6DBLine (OBSTACLE.CPP:447, 0x8007AEF0)
bool FillVectorArray(LVector* out, const DBLine& line) {
    MARKFUNCTION(0x8007AEF0);
    return FillVectorArray(out, 2u, line);
}

// PSX: FillVectorArray__8ObstacleP10tagLVectorUlRC6DBLine (OBSTACLE.CPP:472, 0x8007AF14)
bool FillVectorArray(LVector* out, u32 count, const DBLine& line) {
    MARKFUNCTION(0x8007AF14);

    bool result = count < line.vertexCount;
    u32 index = 0;

    if (count >= line.vertexCount) {
        DBLineVertex* vertex = static_cast<DBLineVertex*>(line.vertices.head);
        while (true) {
            result = index < count;
            if (index >= count) {
                break;
            }

            index++;
            if (!vertex) {
                break;
            }

            out->x = vertex->x;
            out->y = vertex->y;
            out->z = vertex->z;
            vertex = static_cast<DBLineVertex*>(vertex->next);
            out++;
        }
    }

    return result;
}

// PSX: FillCollisionBox__8ObstacleR15tagCollisionBoxRC6DBRootUl (OBSTACLE.CPP:530, 0x8007AF6C)
bool ObstacleFillCollisionBox(tagCollisionBox& box, const DBRoot* root, u32 attribNum) {
    MARKFUNCTION(0x8007AF6C);

    const DBAttrib* attrib = root->FindAttrib(attribNum);
    if (!attrib) {
        return false;
    }

    const char* attribString = attrib->GetAttribString();
    if (!attribString || attribString[0] == '\0') {
        return false;
    }

    const s32 hash = (s32)p3dHash(attribString);

    if (!g_levelManager) {
        return false;
    }

    OriginalBasic* geo = g_levelManager->FindGeo(hash);
    if (!geo) {
        return false;
    }

    FillCollisionBox(box, *static_cast<OriginalGeo*>(geo));
    return true;
}

// PSX Door AnalyzeMesh widens Z before SetCollisionBox (DOOR.CPP; lst RAM:8001ABB4 region).
void ApplyDoorStandingZExtent(tagCollisionBox& box) {
    box.minZ = (s16)(box.minZ - 128);
    box.maxZ = (s16)(box.maxZ + 1024);
}

static bool CollisionBoxLooksValid(const tagCollisionBox& box) {
    return box.minX <= box.maxX && box.minY <= box.maxY && box.minZ <= box.maxZ;
}

static constexpr s32 OBSTACLE_LEDGE_MIN_SPAN = 0x100;
static constexpr s32 OBSTACLE_LEDGE_HAND_Y_TOL = 0x100;
static constexpr s32 OBSTACLE_LEDGE_FLOOR_MIN_HEIGHT = 0x380;
static constexpr s32 OBSTACLE_DETECT_OBSTACLE_OFFSET_XZ = 0x100;
static constexpr s32 OBSTACLE_DETECT_OBSTACLE_OFFSET_Y = 0x100;
static constexpr s32 OBSTACLE_DETECT_OBSTACLE_RADIUS = 0x20;

// ---------------------------------------------------------------------------
// moveList broad-phase snapshot (pure cost, identical results).
//
// Every step, AI::MoveThings runs HandlePickupObstacleCollision for each
// pickup (active and inactive) and HandleHumanoidObstacleCollision for each
// humanoid; both walk the whole moveList (every obstacle of the stage) and,
// per node, read flags, pos and collBox.extent from a large Obstacle object
// -- several cache misses per node, O(things x obstacles) per step.
//
// Inside BeginCollisionPassBatch/EndCollisionPassBatch the passes test a
// compact array of {pos.x, pos.z, extent, Obstacle*} for the TF_MODEL_CREATED
// nodes, in list order, and only touch the Obstacle when the original
// integer broad-phase passes.  Equivalence:
//  * The snapshot holds exactly the values the original loop would read, as
//    long as nothing changed obstacle pos/flags/extent or moveList since it
//    was taken.  Inside the passes, the only calls that can do so are the
//    mutating virtuals (HandlePickupCollision, HandleHumanoidCollision) and
//    the humanoid ticket/ledge/land transitions; the snapshot is invalidated
//    right after each of them (and by any Obstacle ctor/dtor).  Everything
//    else called in the passes is a pure query (GetPhysical,
//    GetObstacleFloorHeight, the static box tests, GetTicketIssuer,
//    AnimationMatrices::GetMatrix) or only writes the humanoid's own model
//    floor state (SetFloorHeight).
//  * When a mutating call happens mid-scan (pickup pass), the scan resumes on
//    the live list from that node's next pointer, exactly like the original.
//  * Outside a batch, or if the snapshot cannot be allocated, the original
//    live-list loops run unchanged.
// ---------------------------------------------------------------------------
namespace {

struct ObstacleBroadPhaseEntry {
    s32 x;
    s32 z;
    s32 extent;
    Obstacle* obstacle;
};

ObstacleBroadPhaseEntry* s_obstacleBroadPhase = nullptr;
s32 s_obstacleBroadPhaseCapacity = 0;
s32 s_obstacleBroadPhaseCount = 0;
bool s_obstacleBroadPhaseValid = false;
s32 s_obstacleBroadPhaseBatchDepth = 0;
s32 s_obstacleBroadPhaseSuspend = 0;

inline void InvalidateObstacleBroadPhase() {
    s_obstacleBroadPhaseValid = false;
}

// Wraps every call that may change obstacle state.  While it is open, any
// (nested) pass uses the live list; on both entry and exit the snapshot is
// dropped, so it is rebuilt from live data before the next use.
struct ObstacleMutationScope {
    ObstacleMutationScope() {
        ++s_obstacleBroadPhaseSuspend;
        InvalidateObstacleBroadPhase();
    }
    ~ObstacleMutationScope() {
        --s_obstacleBroadPhaseSuspend;
        InvalidateObstacleBroadPhase();
    }
    ObstacleMutationScope(const ObstacleMutationScope&) = delete;
    ObstacleMutationScope& operator=(const ObstacleMutationScope&) = delete;
};

// Returns true when the snapshot can be used for the current scan.
bool EnsureObstacleBroadPhase() {
    if (s_obstacleBroadPhaseBatchDepth <= 0 || s_obstacleBroadPhaseSuspend > 0 || !g_ai) {
        return false;
    }
    if (s_obstacleBroadPhaseValid) {
        return true;
    }

    s32 count = 0;
    for (ccMinNode* node = g_ai->moveList.head; node != nullptr; node = node->next) {
        const Obstacle* obstacle = static_cast<const Obstacle*>(static_cast<const Thing*>(node));
        if (obstacle->flags & TF_MODEL_CREATED) {
            count++;
        }
    }

    if (count > s_obstacleBroadPhaseCapacity) {
        const s32 newCapacity = (count + 63) & ~63;
        ObstacleBroadPhaseEntry* grown = new (std::nothrow) ObstacleBroadPhaseEntry[newCapacity];
        if (!grown) {
            return false;
        }
        delete[] s_obstacleBroadPhase;
        s_obstacleBroadPhase = grown;
        s_obstacleBroadPhaseCapacity = newCapacity;
    }

    s32 index = 0;
    for (ccMinNode* node = g_ai->moveList.head; node != nullptr; node = node->next) {
        Obstacle* obstacle = static_cast<Obstacle*>(static_cast<Thing*>(node));
        if ((obstacle->flags & TF_MODEL_CREATED) == 0) {
            continue;
        }
        ObstacleBroadPhaseEntry& entry = s_obstacleBroadPhase[index++];
        entry.x = obstacle->pos.x;
        entry.z = obstacle->pos.z;
        entry.extent = (s32)obstacle->collBox.extent;
        entry.obstacle = obstacle;
    }
    s_obstacleBroadPhaseCount = index;
    s_obstacleBroadPhaseValid = true;
    return true;
}

} // namespace

void Obstacle::BeginCollisionPassBatch() {
    if (s_obstacleBroadPhaseBatchDepth++ == 0) {
        InvalidateObstacleBroadPhase();
    }
}

void Obstacle::EndCollisionPassBatch() {
    if (s_obstacleBroadPhaseBatchDepth > 0 && --s_obstacleBroadPhaseBatchDepth == 0) {
        InvalidateObstacleBroadPhase();
    }
}

Obstacle::Obstacle(const LVector* pos, u16 type) : Thing(pos, type) {
    MARKFUNCTION(0x8007CA08);
    collBox = { 0x7FFF, 0x7FFF, 0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0 };
    physicalType = 0;
    lightingFlag = 1;
    shadowFlag = 1;
    InvalidateObstacleBroadPhase();
}

Obstacle::~Obstacle() {
    MARKFUNCTION(0x8007CA7C);
    InvalidateObstacleBroadPhase();
#if HIGH_FPS_PLAY_PRESENTATION
    ObstacleForgetRenderTransform(this);
#endif
}

void Obstacle::Think() {
    MARKFUNCTION(0x8007CDA4);
    Move();
}

void Obstacle::Draw() {
    MARKFUNCTION(0x8007AE04);
    if (model) {
        LVector drawPos = pos;
        LVector drawOrient = orientation;

#if MODERN_GRAPHICS
        // Shadow caster prepass draws every mover a second time this frame
        // (world.cpp DrawEntityCasterList) before the main pass reaches it.
        // Feeding that extra call through ObstacleBuildRenderTransform would
        // advance the HIGH_FPS smoothing state twice on a logic-step frame,
        // collapsing prevPos/curPos and snapping the interpolated draw pos.
        if (!ShadowCSM::IsCasterPrepass()) {
            ObstacleBuildRenderTransform(this, pos, orientation, drawPos, drawOrient);
        }
#else
        ObstacleBuildRenderTransform(this, pos, orientation, drawPos, drawOrient);
#endif

        // PSX: selects render table based on shadowFlag and lightingFlag
        // (litTable, ZSortTable, litFarTable, ZFarTable) - not needed on PC

        // PSX: copies pos and orientation to model fields
        Model* m = static_cast<Model*>(model);
        m->posX = drawPos.x;
        m->posY = drawPos.y;
        m->posZ = drawPos.z;
        m->rotX = (u16)(drawOrient.x & 0xFFFF);
        m->rotY = (u16)(drawOrient.y & 0xFFFF);
        m->rotZ = (u16)(drawOrient.z & 0xFFFF);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        static u32 pspObstacleDrawProbe = 0;
        const u32 pspObstacleDrawProbeId = pspObstacleDrawProbe++;
        if (thingType == AITypes::TT_PLATFORM && pspObstacleDrawProbeId < 8u) {
            LOG("[PSP Obstacle] platform draw begin #%lu thing=%p model=%p drawable=%p type=%ld",
                static_cast<unsigned long>(pspObstacleDrawProbeId), static_cast<void*>(this),
                static_cast<void*>(m), static_cast<void*>(m->drawable),
                static_cast<long>(m->drawableType));
            LOG("[PSP Obstacle] model dispatch begin #%lu model_word0=%08lX",
                static_cast<unsigned long>(pspObstacleDrawProbeId),
                static_cast<unsigned long>(reinterpret_cast<const u32*>(m)[0]));
        }
#else
        const u32 pspObstacleDrawProbeId = 0u;
#endif
        m->Show(0);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (thingType == AITypes::TT_PLATFORM && pspObstacleDrawProbeId < 8u) {
            LOG("[PSP Obstacle] model dispatch done #%lu",
                static_cast<unsigned long>(pspObstacleDrawProbeId));
            LOG("[PSP Obstacle] platform draw done #%lu thing=%p",
                static_cast<unsigned long>(pspObstacleDrawProbeId), static_cast<void*>(this));
        }
#endif
    }
}

void Obstacle::Reset() {
    MARKFUNCTION(0x8007CD9C);
}

void Obstacle::Move() {
    MARKFUNCTION(0x8007CDD4);
}

void Obstacle::UpdatePosition() {
    MARKFUNCTION(0x8007CDDC);
}

// PSX: FillBoxCentre__8ObstacleR10tagLVectorRC10tagLVectorRC9_RMVECT16RC15tagCollisionBox
// (OBSTACLE.CPP:651, 0x8007B184)
void Obstacle::FillBoxCentre(
    LVector& outPos,
    const LVector& pos,
    const LVector& orientation,
    const tagCollisionBox& box) {
    MARKFUNCTION(0x8007B184);

    const s32 sinY = rmSin16((s16)orientation.y);
    const s32 cosY = rmSin16((s16)(orientation.y + 0x4000));
    const s32 centreX = Div2TowardZero((s32)box.minX + (s32)box.maxX);
    const s32 centreY = Div2TowardZero((s32)box.minY + (s32)box.maxY);
    const s32 centreZ = Div2TowardZero((s32)box.minZ + (s32)box.maxZ);

    outPos.x = pos.x + (s32)((cosY * (s64)centreX) >> 16) + (s32)((sinY * (s64)centreZ) >> 16);
    outPos.y = pos.y + centreY;
    outPos.z = pos.z + (s32)((-sinY * (s64)centreX) >> 16) + (s32)((cosY * (s64)centreZ) >> 16);
}

// PSX: GetWorldFloorHeight__8ObstacleRC10tagLVector (OBSTACLE.CPP:680, 0x8007B2F0)
s32 Obstacle::GetWorldFloorHeight(const LVector& pos) {
    MARKFUNCTION(0x8007B2F0);

    s32 floorHeight = (s32)0x80000001;
    s32 ceilingHeight = 0;
    LVector floorNormal = {};
    LVector ceilingNormal = {};
    CollisionSector::GetWorldFloorAndCeilingHeight(
        floorHeight, ceilingHeight, floorNormal, ceilingNormal, pos, 0);
    return floorHeight;
}

// PSX: GetYRotation__8Obstaclell (OBSTACLE.CPP:749, 0x8007B328)
s32 Obstacle::GetYRotation(s32 x, s32 z) {
    MARKFUNCTION(0x8007B328);

    s32 clampedZ = z;
    if (clampedZ < -0x10000) {
        clampedZ = -0x10000;
    }
    else if (clampedZ > 0x10000) {
        clampedZ = 0x10000;
    }

    const s32 angle = PsxAcos16FromFix16Clamped(clampedZ);
    if (x < 0) {
        return 0x8000 - angle;
    }

    return angle + 0x8000;
}

// PSX: StaticGetObstacleFloorHeight__8ObstacleRC10tagLVector (OBSTACLE.CPP:2232, 0x8007D198)
s32 Obstacle::StaticGetObstacleFloorHeight(const LVector& pos) {
    MARKFUNCTION(0x8007D198);

    s32 bestFloorHeight = (s32)0x80000001;
    if (!g_ai) {
        return bestFloorHeight;
    }

    for (ccMinNode* node = g_ai->moveList.head; node != nullptr; node = node->next) {
        Obstacle* obstacle = static_cast<Obstacle*>(static_cast<Thing*>(node));
        if ((obstacle->flags & TF_MODEL_CREATED) == 0) {
            continue;
        }
        if (obstacle->GetPhysical() == 0) {
            continue;
        }

        s32 deltaX = pos.x - obstacle->pos.x;
        if (deltaX < 0) {
            deltaX = -deltaX;
        }
        if (deltaX >= obstacle->collBox.extent) {
            continue;
        }

        s32 deltaZ = pos.z - obstacle->pos.z;
        if (deltaZ < 0) {
            deltaZ = -deltaZ;
        }
        if (deltaZ >= obstacle->collBox.extent) {
            continue;
        }

        if (!CheckStaticHorizontalBoxPointCollision(
                obstacle->pos, obstacle->collBox, obstacle->orientation.y, pos)) {
            continue;
        }

        const s32 floorHeight = obstacle->GetObstacleFloorHeight(pos);
        if (bestFloorHeight < floorHeight && floorHeight < pos.y) {
            bestFloorHeight = floorHeight;
        }
    }

    return bestFloorHeight;
}

// PSX: AllocateAndCreateShadow__8Obstacle (OBSTACLE.CPP:1941, 0x8007CD2C)
void Obstacle::AllocateAndCreateShadow() {
    MARKFUNCTION(0x8007CD2C);

    if (!model) {
        return;
    }

    Model* modelPtr = static_cast<Model*>(model);
    Shadow* shadow = GetModelShadow(modelPtr);
    if (!shadow || shadow->GetFloorHeightState()->shadowType != MODEL_SHADOW_SIMPLE) {
        delete shadow;
        shadow = new SimpleShadow(modelPtr);
        modelPtr->field36 = shadow;
    }

    ModelFloorHeightState* shadowState = shadow->GetFloorHeightState();
    shadowState->shadowMinX = -100;
    shadowState->shadowMinY = -50;
    shadowState->shadowMaxX = 100;
    shadowState->shadowMaxZ = 50;

    UpdateShadowFloorHeight();
}

// PSX: UpdateShadowFloorHeight__8Obstacle (OBSTACLE.CPP:2300, 0x8007D354)
void Obstacle::UpdateShadowFloorHeight() {
    MARKFUNCTION(0x8007D354);

    if (!model) {
        return;
    }

    Model* modelPtr = static_cast<Model*>(model);
    ModelFloorHeightState* floorState = GetModelFloorHeightState(modelPtr);
    if (!floorState) {
        return;
    }

    LVector boxCentre = {};
    FillBoxCentre(boxCentre, pos, orientation, collBox);

    const s32 worldFloorHeight = GetWorldFloorHeight(boxCentre);
    s32 obstacleFloorHeight = StaticGetObstacleFloorHeight(boxCentre);
    if (obstacleFloorHeight < worldFloorHeight) {
        obstacleFloorHeight = worldFloorHeight;
    }

    // Shadow placement reads slot 1 (Thing::SetFloorHeight target) as current floor.
    floorState->previous = obstacleFloorHeight;
    floorState->shadowMinX = (s32)collBox.minX;
    floorState->shadowMinY = (s32)collBox.minY;
    floorState->shadowMaxX = (s32)collBox.maxX;
    floorState->shadowMaxZ = (s32)collBox.maxZ;
}

void Obstacle::CreateModel(const char* name) {
    MARKFUNCTION(0x8007CC64);
    if (!model) {
        GModel* gm = new GModel();
        gm->backPtr = this;
        model = gm;
    }

    Thing::CreateModel(name);

    Model* modelPtr = static_cast<Model*>(model);
    if (!modelPtr) {
        return;
    }

    if (lightingFlag) {
        if (!modelPtr->hwLights) {
            modelPtr->AllocateHardwareLights(3);
        }
        if (!modelPtr->ambientLight) {
            modelPtr->AllocateAmbientLight();
        }
    }
}

void Obstacle::DeleteModel() {
    MARKFUNCTION(0x8007CD94);
    Thing::DeleteModel();
}

void Obstacle::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8007CBC4);
    Thing::AnalyzeMesh(root);

    const DBAttrib* a = root->FindAttrib(50);
    if (a) {
        physicalType = (u8)a->value;
    }

    if (thingType == 436) {
        lightingFlag = 0;
    }
    else {
        const DBAttrib* a51 = root->FindAttrib(51);
        if (a51 && a51->value == 2) {
            lightingFlag = 0;
        }
    }

    a = root->FindAttrib(52);
    if (a) {
        shadowFlag = 0;
    }
}

void Obstacle::FillSphere(tSphere& sphere) const {
    MARKFUNCTION(0x8007CE08);
}

// PSX: HandlePickupObstacleCollision__8ObstacleP6Pickup (OBSTACLE.CPP:1212, 0x8007C034)
void Obstacle::HandlePickupObstacleCollision(Pickup* pickup) {
    MARKFUNCTION(0x8007C034);

    if (!pickup || !g_ai) {
        return;
    }

    tagCollisionSphere sphere = {};
    sphere.radius = 128;

    ccMinNode* liveStart = g_ai->moveList.head;
    if (EnsureObstacleBroadPhase()) {
        // Snapshot scan: same order, same integer broad-phase on the same
        // values.  A hit calls a mutating virtual, after which the snapshot is
        // stale, so the remaining nodes are scanned on the live list from the
        // node after the one just handled (the original loop's node->next).
        liveStart = nullptr;
        const s32 count = s_obstacleBroadPhaseCount;
        const ObstacleBroadPhaseEntry* entries = s_obstacleBroadPhase;
        for (s32 i = 0; i < count; i++) {
            const ObstacleBroadPhaseEntry& entry = entries[i];
            const s32 threshold = static_cast<s32>(pickup->collisionRadius) + sphere.radius;

            s32 deltaX = entry.x - pickup->pos.x;
            if (deltaX < 0) {
                deltaX = -deltaX;
            }
            if (deltaX >= threshold) {
                continue;
            }

            s32 deltaZ = entry.z - pickup->pos.z;
            if (deltaZ < 0) {
                deltaZ = -deltaZ;
            }
            if (deltaZ >= threshold) {
                continue;
            }

            Obstacle* obstacle = entry.obstacle;
            if (CheckStaticBoxSphereCollision(
                    obstacle->pos,
                    obstacle->collBox,
                    obstacle->orientation.y,
                    pickup->pos,
                    sphere)) {
                {
                    ObstacleMutationScope mutation;
                    obstacle->HandlePickupCollision(pickup);
                }
                liveStart = static_cast<ccMinNode*>(static_cast<Thing*>(obstacle))->next;
                break;
            }
        }
        if (!liveStart) {
            return;
        }
    }

    for (ccMinNode* node = liveStart; node != nullptr; node = node->next) {
        Obstacle* obstacle = static_cast<Obstacle*>(static_cast<Thing*>(node));
        if ((obstacle->flags & TF_MODEL_CREATED) == 0) {
            continue;
        }

        const s32 threshold = static_cast<s32>(pickup->collisionRadius) + sphere.radius;

        s32 deltaX = obstacle->pos.x - pickup->pos.x;
        if (deltaX < 0) {
            deltaX = -deltaX;
        }
        if (deltaX >= threshold) {
            continue;
        }

        s32 deltaZ = obstacle->pos.z - pickup->pos.z;
        if (deltaZ < 0) {
            deltaZ = -deltaZ;
        }
        if (deltaZ >= threshold) {
            continue;
        }

        if (CheckStaticBoxSphereCollision(
                obstacle->pos,
                obstacle->collBox,
                obstacle->orientation.y,
                pickup->pos,
                sphere)) {
            ObstacleMutationScope mutation;
            obstacle->HandlePickupCollision(pickup);
        }
    }
}

void Obstacle::HandlePickupCollision(Thing* pickup) {}

void Obstacle::HandleHumanoidCollision(Humanoid* hum) {}

void Obstacle::Trigger() {
    MARKFUNCTION(0x8007CE40);
}

void Obstacle::TriggerByName(Thing* source, const char* name, const char* param) {
    MARKFUNCTION(0x8007CE94);
}

void Obstacle::ExplosiveTrigger(s32 damage, const char* name) {
    MARKFUNCTION(0x8007CE60);
}

const LVector* Obstacle::GetDeltaVelocity() const {
    MARKFUNCTION(0x8007CE70);
    return &ZERO_DELTA_VELOCITY;
}

bool Obstacle::CareAboutAttack() const {
    return false;
}

void Obstacle::HandleAttack(Humanoid* attacker, s32 damageType, s32 damage) {
    HandleAttack(attacker, damageType, damage, damage);
}

void Obstacle::HandleAttack(Humanoid* attacker, s32 damageType, s32 attackMagnitude, s32 damage) {}

s32 Obstacle::GetFloorMaterial() const {
    MARKFUNCTION(0x8007D0B4);
    return 0;
}

s32 Obstacle::GetObstacleFloorHeight(const LVector& pos) const {
    MARKFUNCTION(0x8007D0CC);
    // PSX: return this->pos.y + (s32)(s16)collBox.maxY
    return this->pos.y + (s32)collBox.maxY;
}

bool Obstacle::LedgeCheck(const tagCollisionBox& box, const LVector& normal, const LVector& correctionPos, Humanoid* hum) const {
    MARKFUNCTION(0x8007BE84);

    if (!hum) {
        return false;
    }

    if (hum->velocity.y > 0)
        return false;

    const s32 actionState = hum->actionState;
    if (actionState == (s32)AS_PUNCH_ATTACK
        || actionState == (s32)AS_KICK_ATTACK
        || actionState == (s32)AS_STATE_35) {
        return false;
    }

    bool hasWideTop = false;
    if ((s32)box.maxX - (s32)box.minX >= OBSTACLE_LEDGE_MIN_SPAN) {
        hasWideTop = ((s32)box.maxZ - (s32)box.minZ) >= OBSTACLE_LEDGE_MIN_SPAN;
    }

    LVector facing = {};
    facing.x = rmSin16((s16)hum->orientation.y);
    facing.z = rmSin16((s16)(hum->orientation.y + 0x4000));

    const bool facingObstacleFront = rmV3Dot(&normal, &facing) < 0;

    HumanoidModel* model = hum->model ? static_cast<HumanoidModel*>(hum->model) : nullptr;
    if (!model || !model->animMatrices) {
        return false;
    }

    s32* leftHandMatrix = AnimationMatrices::GetMatrix(model->animMatrices, 1);
    s32* rightHandMatrix = AnimationMatrices::GetMatrix(model->animMatrices, 2);
    if (!leftHandMatrix || !rightHandMatrix) {
        return false;
    }

    const s32 avgHandY = (leftHandMatrix[6] + rightHandMatrix[6]) / 2;
    const bool nearHands = correctionPos.y >= avgHandY - OBSTACLE_LEDGE_HAND_Y_TOL
        && correctionPos.y <= avgHandY + OBSTACLE_LEDGE_HAND_Y_TOL;

    ModelFloorHeightState* floorState = GetModelFloorHeightState(model);
    if (!floorState) {
        return false;
    }

    if (!hasWideTop || !facingObstacleFront || !nearHands) {
        return false;
    }

    return correctionPos.y >= floorState->current + OBSTACLE_LEDGE_FLOOR_MIN_HEIGHT;
}

// PSX: DetectObstacleAboveLedge__8ObstacleRC9_RMVECT16RC10tagLVector (OBSTACLE.CPP:1576)
bool Obstacle::DetectObstacleAboveLedge(const LVector& normal, const LVector& ledgePos) {
    MARKFUNCTION(0x8007C6DC);

    const s32 offsetX = MulShift16(normal.x, OBSTACLE_DETECT_OBSTACLE_OFFSET_XZ);
    const s32 offsetZ = MulShift16(normal.z, OBSTACLE_DETECT_OBSTACLE_OFFSET_XZ);

    LVector startPos = {};
    startPos.x = ledgePos.x + offsetX;
    startPos.y = ledgePos.y + OBSTACLE_DETECT_OBSTACLE_OFFSET_Y;
    startPos.z = ledgePos.z + offsetZ;

    LVector endPos = {};
    endPos.x = ledgePos.x - offsetX;
    endPos.y = ledgePos.y + OBSTACLE_DETECT_OBSTACLE_OFFSET_Y;
    endPos.z = ledgePos.z - offsetZ;

    return DetectObstacle(startPos, endPos, OBSTACLE_DETECT_OBSTACLE_RADIUS);
}

// PSX: DetectObstacle__8ObstacleRC10tagLVectorT1l (OBSTACLE.CPP:1611)
bool Obstacle::DetectObstacle(const LVector& startPos, const LVector& endPos, s32 radius) {
    MARKFUNCTION(0x8007C7B0);

    if (!g_ai) {
        return false;
    }

    const s32 midX = (startPos.x + endPos.x) / 2;
    const s32 midY = (startPos.y + endPos.y) / 2;
    const s32 midZ = (startPos.z + endPos.z) / 2;
    const f32 searchExtent = rmMag2((f32)(endPos.x - startPos.x), (f32)(endPos.z - startPos.z));

    for (ccMinNode* node = g_ai->moveList.head; node; node = node->next) {
        Obstacle* obs = static_cast<Obstacle*>(static_cast<Thing*>(node));
        if ((obs->flags & TF_MODEL_CREATED) == 0) {
            continue;
        }
        if (!obs->GetPhysical()) {
            continue;
        }

        s32 threshold = (s32)obs->collBox.extent + (s32)searchExtent + radius;

        s32 deltaX = midX - obs->pos.x;
        if (deltaX < 0) {
            deltaX = -deltaX;
        }
        if (deltaX >= threshold) {
            continue;
        }

        s32 deltaZ = midZ - obs->pos.z;
        if (deltaZ < 0) {
            deltaZ = -deltaZ;
        }
        if (deltaZ >= threshold) {
            continue;
        }

        s32 deltaY = endPos.y - startPos.y;
        if (deltaY < 0) {
            deltaY = -deltaY;
        }

        if (midY + deltaY + radius < obs->pos.y + (s32)obs->collBox.minY) {
            continue;
        }
        if (obs->pos.y + (s32)obs->collBox.maxY < midY - deltaY - radius) {
            continue;
        }

        LVector outPos = {};
        LVector outNormal = {};
        LVector outPushedPos = {};
        if (CorrectThingPositionObstacle(
                obs->pos,
                obs->pos,
                obs->orientation.y,
                obs->orientation.y,
                obs->collBox,
                startPos,
                endPos,
                radius,
                -radius,
                radius,
                outPos,
                outNormal,
                outPushedPos)) {
            return true;
        }
    }

    return false;
}

s32 Obstacle::GetPhysical() const {
    MARKFUNCTION(0x8007D034);
    // PSX: switch on thingType (OBSTACLE.CPP:2152, 0x8007D034)
    switch (thingType) {
        case 404:  // Conveyor
        case 407:  // HorizontalPole
        case 435:  // Untouchable
        case 436:  // Collectible
        case 451:  // TriggerThing
        case 459:  // Blast
        case 463:  // Door
        case 464:  // Teleporter
        case 467:  // TrapDoor
        case 469:  // FrontEndVolume
        case 470:  // Ladder
            return 0;
        default:
            return 1;
    }
}

void Obstacle::SetCollisionBox(const tagCollisionBox& box) {
    MARKFUNCTION(0x8007BE24);
    collBox = box;
    SetCollisionBoxExtent(collBox);
}

// PSX: CheckXZStaticBoxCylinderCollision (OBSTACLE.CPP:193, 0x8007A740)
static bool CheckXZStaticBoxCylinderCollision(
    const LVector& obsPos, const tagCollisionBox& box, s32 orientY,
    const LVector& cylPos, const tagCollisionCylinder& cyl) {
    s32 minX = box.minX - cyl.radius;
    s32 maxX = box.maxX + cyl.radius;
    s32 maxZ = box.maxZ + cyl.radius;
    s32 minZ = box.minZ - cyl.radius;

    LVector delta;
    delta.x = cylPos.x - obsPos.x;
    delta.y = cylPos.y - obsPos.y;
    delta.z = cylPos.z - obsPos.z;

    s32 sinV = rmSin16(orientY);
    s32 cosV = rmSin16(orientY + 0x4000);

    s32 localX = (s32)(((s64)cosV * delta.x) >> 16) + (s32)((-(s64)sinV * delta.z) >> 16);
    s32 localZ = (s32)(((s64)sinV * delta.x) >> 16) + (s32)(((s64)cosV * delta.z) >> 16);

    if (localX >= minX && maxX >= localX && localZ >= minZ && maxZ >= localZ) {
        return true;
    }
    return false;
}

static constexpr s32 OBSTACLE_CORRECT_BUFFER = 4;
static constexpr s32 OBSTACLE_YMAX_BUFFER = 0x40;
static constexpr s32 CORRECT_THING_OBSTACLE_BOX_Y_MAX_OFFSET = 9;
static s32 CORRECT_THING_POSITION_RADIUS_BIAS = 0;
static constexpr s32 CORRECT_THING_POSITION_EXTRA = 0x20;

void SetCorrectThingPositionRadiusBias(s32 value) {
    CORRECT_THING_POSITION_RADIUS_BIAS = value;
}

// PSX: CorrectThingPosition__8ObstacleRC10tagLVectorT1llRC15tagCollisionBoxT1T1lllR10tagLVectorR9_RMVECT16T11_ (0x8007B398)
bool CorrectThingPositionObstacle(
    const LVector& basisA,
    const LVector& basisB,
    s32 rotA,
    s32 rotB,
    const tagCollisionBox& box,
    const LVector& pointA,
    const LVector& pointB,
    s32 radius,
    s32 yMinOffset,
    s32 yMaxOffset,
    LVector& outPos,
    LVector& outNormal,
    LVector& outPushedPos) {
    const s32 sinA = rmSin16(rotA);
    const s32 cosA = rmSin16(rotA + 0x4000);
    const s32 sinB = rmSin16(rotB);
    const s32 cosB = rmSin16(rotB + 0x4000);

    LVector localA = {
        pointA.x - basisA.x,
        pointA.y - basisA.y,
        pointA.z - basisA.z
    };
    LVector localB = {
        pointB.x - basisB.x,
        pointB.y - basisB.y,
        pointB.z - basisB.z
    };

    const s32 localAX = localA.x;
    localA.x = MulShift16(cosA, localA.x) + MulShift16(-sinA, localA.z);
    localA.z = MulShift16(sinA, localAX) + MulShift16(cosA, localA.z);

    const s32 localBX = localB.x;
    localB.x = MulShift16(cosB, localB.x) + MulShift16(-sinB, localB.z);
    localB.z = MulShift16(sinB, localBX) + MulShift16(cosB, localB.z);

    s32 minX = 0;
    s32 maxX = 0;
    if (localA.x >= localB.x) {
        minX = localB.x - radius;
        maxX = localA.x + radius;
    }
    else {
        minX = localA.x - radius;
        maxX = localB.x + radius;
    }

    s32 minY = 0;
    s32 maxY = 0;
    if (localA.y >= localB.y) {
        minY = localB.y + yMinOffset;
        maxY = localA.y + yMaxOffset;
    }
    else {
        minY = localA.y + yMinOffset;
        maxY = localB.y + yMaxOffset;
    }

    s32 minZ = 0;
    s32 maxZ = 0;
    if (localA.z >= localB.z) {
        minZ = localB.z - radius;
        maxZ = localA.z + radius;
    }
    else {
        minZ = localA.z - radius;
        maxZ = localB.z + radius;
    }

    bool overlapXZ = false;
    if (maxX >= (s32)box.minX && (s32)box.maxX >= minX &&
        maxZ >= (s32)box.minZ && (s32)box.maxZ >= minZ) {
        overlapXZ = true;
    }

    bool overlapY = false;
    if (maxY >= (s32)box.minY) {
        overlapY = ((s32)box.maxY - CORRECT_THING_OBSTACLE_BOX_Y_MAX_OFFSET) >= minY;
    }

    s32 correctedX = localB.x;
    s32 correctedY = localB.y;
    s32 correctedZ = localB.z;
    s32 normalX = 0;
    s32 normalY = 0;
    s32 normalZ = 0;

    const s32 centreX = Div2TowardZero((s32)box.minX + (s32)box.maxX);
    const s32 centreZ = Div2TowardZero((s32)box.minZ + (s32)box.maxZ);

    bool corrected = false;

    if (overlapXZ && overlapY) {
        s32 pushX = 0;
        s32 pushY = 0;
        s32 pushZ = 0;
        s32 distX = -0xFFFF;
        s32 distY = -0xFFFF;
        s32 distZ = -0xFFFF;
        s32 targetX = 0;
        s32 targetY = 0;
        s32 targetZ = 0;

        if (localA.x < centreX) {
            if (((s32)box.minX - radius - CORRECT_THING_POSITION_EXTRA) < localB.x) {
                pushX = -1;
                distX = (s32)box.minX - localA.x;
                targetX = (s32)box.minX - radius - OBSTACLE_CORRECT_BUFFER;
            }
        }
        else if (centreX < localA.x) {
            if (localB.x < ((s32)box.maxX + radius + CORRECT_THING_POSITION_EXTRA)) {
                pushX = 1;
                distX = localA.x - (s32)box.maxX;
                targetX = (s32)box.maxX + radius + OBSTACLE_CORRECT_BUFFER;
            }
        }

        if (localB.x + CORRECT_THING_POSITION_RADIUS_BIAS >= (s32)box.minX &&
            (s32)box.maxX >= localB.x - CORRECT_THING_POSITION_RADIUS_BIAS &&
            localB.z + CORRECT_THING_POSITION_RADIUS_BIAS >= (s32)box.minZ &&
            (s32)box.maxZ >= localB.z - CORRECT_THING_POSITION_RADIUS_BIAS) {
            if (localA.y < (s32)box.minY) {
                pushY = -1;
                distY = (s32)box.minY - (localA.y + yMaxOffset);
                targetY = (s32)box.minY - yMaxOffset;
            }
            else if (((s32)box.maxY - OBSTACLE_YMAX_BUFFER) < localA.y) {
                pushY = 1;
                distY = 0xFFFF;
                targetY = (s32)box.maxY - yMinOffset;
            }
        }

        if (localA.z < centreZ) {
            if (((s32)box.minZ - radius - CORRECT_THING_POSITION_EXTRA) < localB.z) {
                pushZ = -1;
                distZ = (s32)box.minZ - localA.z;
                targetZ = (s32)box.minZ - radius - OBSTACLE_CORRECT_BUFFER;
            }
        }
        else if (centreZ < localA.z) {
            if (localB.z < ((s32)box.maxZ + radius + CORRECT_THING_POSITION_EXTRA)) {
                pushZ = 1;
                distZ = localA.z - (s32)box.maxZ;
                targetZ = (s32)box.maxZ + radius + OBSTACLE_CORRECT_BUFFER;
            }
        }

        if (pushX != 0 || pushY != 0 || pushZ != 0) {
            corrected = true;

            if (distX < distZ) {
                if (distZ >= distY) {
                    correctedZ = targetZ;
                    normalZ = pushZ * 0x10000;
                }
                else {
                    correctedY = targetY;
                    normalY = pushY * 0x10000;
                }
            }
            else if (distX >= distY) {
                correctedX = targetX;
                normalX = pushX * 0x10000;
            }
            else {
                correctedY = targetY;
                normalY = pushY * 0x10000;
            }
        }
    }

    const s32 worldX = MulShift16(cosB, correctedX) + MulShift16(sinB, correctedZ);
    const s32 worldZ = MulShift16(-sinB, correctedX) + MulShift16(cosB, correctedZ);

    outPos.x = basisB.x + worldX;
    outPos.y = basisB.y + correctedY;
    outPos.z = basisB.z + worldZ;

    outNormal.x = MulShift16(cosB, normalX) + MulShift16(sinB, normalZ);
    outNormal.y = normalY;
    outNormal.z = MulShift16(-sinB, normalX) + MulShift16(cosB, normalZ);

    outPushedPos.x = outPos.x - MulShift16(radius, outNormal.x);
    outPushedPos.y = basisB.y + (s32)box.maxY;
    outPushedPos.z = outPos.z - MulShift16(radius, outNormal.z);

    CORRECT_THING_POSITION_RADIUS_BIAS = 0;
    return corrected;
}

// PSX: CheckStaticBoxCylinderCollision_Obstacle (OBSTACLE.CPP:328, 0x8007AB90)
static bool CheckStaticBoxCylinderCollision_Obstacle(
    const LVector& obsPos, const tagCollisionBox& box, s32 orientY,
    const LVector& cylPos, const tagCollisionCylinder& cyl) {
    s32 minX = box.minX - cyl.radius;
    s32 maxX = box.maxX + cyl.radius;
    s32 minY = box.minY - cyl.upperY;
    s32 maxY = box.maxY - cyl.lowerY;
    s32 maxZ = box.maxZ + cyl.radius;
    s32 minZ = box.minZ - cyl.radius;

    LVector delta;
    delta.x = cylPos.x - obsPos.x;
    delta.y = cylPos.y - obsPos.y;
    delta.z = cylPos.z - obsPos.z;

    s32 sinV = rmSin16(orientY);
    s32 cosV = rmSin16(orientY + 0x4000);

    s32 localX = (s32)(((s64)cosV * delta.x) >> 16) + (s32)((-(s64)sinV * delta.z) >> 16);
    s32 localZ = (s32)(((s64)sinV * delta.x) >> 16) + (s32)(((s64)cosV * delta.z) >> 16);

    if (localX >= minX && maxX >= localX &&
        delta.y >= minY && maxY >= delta.y &&
        localZ >= minZ && maxZ >= localZ) {
        return true;
    }
    return false;
}

// PSX: GetXZStaticBoxCylinderCollisionSortDistance (OBSTACLE.CPP:241, 0x8007A970)
static s32 GetXZStaticBoxCylinderCollisionSortDistance(
    const LVector& obsPos, const tagCollisionBox& box, s32 orientY,
    const LVector& testPos) {
    LVector delta;
    delta.x = testPos.x - obsPos.x;
    delta.y = testPos.y - obsPos.y;
    delta.z = testPos.z - obsPos.z;

    s32 sinV = rmSin16(orientY);
    s32 cosV = rmSin16(orientY + 0x4000);

    s32 localX = (s32)(((s64)cosV * delta.x) >> 16) + (s32)((-(s64)sinV * delta.z) >> 16);
    s32 localZ = (s32)(((s64)sinV * delta.x) >> 16) + (s32)(((s64)cosV * delta.z) >> 16);

    s32 dist = 0;
    if (localX < box.minX) {
        dist = box.minX - localX;
    }
    else if (localX > box.maxX) {
        dist = localX - box.maxX;
    }
    if (localZ < box.minZ) {
        dist += box.minZ - localZ;
    }
    else if (localZ > box.maxZ) {
        dist += localZ - box.maxZ;
    }
    return dist;
}

struct ObstaclePair {
    s32 distance;
    Obstacle* obstacle;
};

static ObstaclePair pairArray[8];

// PSX: Obstacle::HandleHumanoidObstacleCollision (OBSTACLE.CPP:1301, 0x8007C178)
void Obstacle::HandleHumanoidObstacleCollision(Humanoid* hum) {
    MARKFUNCTION(0x8007C178);

    if (!g_ai) {
        return;
    }

    DynamicThing* dt = (DynamicThing*)hum;

    s32 wasOnObstacle = (hum->flags >> 12) & 1;
    s32 bestFloorHeight = (s32)0x80000001;

    Thing* prevIssuer = dt->GetTicketIssuer();

    // Get bone 5 world position from animation matrices
    LVector bonePos = {};
    HumanoidModel* hmodel = (HumanoidModel*)hum->model;
    if (hmodel && hmodel->animMatrices) {
        s32* mat = AnimationMatrices::GetMatrix(hmodel->animMatrices, 5);
        if (mat) {
            bonePos.x = mat[5];
            bonePos.y = mat[6];
            bonePos.z = mat[7];
        }
    }

    // Foot-level cylinder (radius=0, with humanoid Y extents)
    tagCollisionCylinder footCyl;
    footCyl.radius = 0;
    footCyl.lowerY = hum->collBboxMin.y;
    footCyl.upperY = hum->collBboxMin.z;

    // Humanoid collision cylinder
    tagCollisionCylinder humCyl;
    humCyl.radius = hum->collBboxMin.x;
    humCyl.lowerY = hum->collBboxMin.y;
    humCyl.upperY = hum->collBboxMin.z;

    // Disembark if currently on something and not in an exempt state
    if (prevIssuer) {
        s32 exempt = 0;
        s32 state = hum->actionState;
        if (state == 36 || state == 59 || state == 37 ||
            state == 38 || state == 60 || state == 61 || state == 62) {
            exempt = 1;
        }
        if (!exempt) {
            ObstacleMutationScope mutation;
            dt->Disembark();
        }
    }

    s32 pairCount = 0;

    // Narrow phase for one obstacle that passed the integer broad-phase.
    // Only pure queries are called here (no obstacle state changes).
    auto narrowPhase = [&](Obstacle* obs) {
        s32 physical = obs->GetPhysical();

        // Y range check
        s32 dy = dt->homePos.y - obs->pos.y;
        s32 yLow = (s32)obs->collBox.minY - humCyl.upperY;
        s32 yHigh = (s32)obs->collBox.maxY - humCyl.lowerY;
        bool yPass = (dy >= yLow && yHigh >= dy);

        if (CheckXZStaticBoxCylinderCollision(
            obs->pos, obs->collBox, obs->orientation.y,
            dt->homePos, humCyl)) {
            // Floor height from physical obstacles
            if (physical) {
                if (CheckXZStaticBoxCylinderCollision(
                    obs->pos, obs->collBox, obs->orientation.y,
                    bonePos, footCyl)) {
                    s32 floorH = obs->GetObstacleFloorHeight(bonePos);
                    if (bestFloorHeight < floorH &&
                        floorH < dt->homePos.y + humCyl.upperY) {
                        bestFloorHeight = floorH;
                    }
                }
            }

            // Distance-sorted pair insertion for HandleHumanoidCollision
            if (yPass) {
                s32 sortDist = GetXZStaticBoxCylinderCollisionSortDistance(
                    obs->pos, obs->collBox, obs->orientation.y,
                    hum->pos);

                if (pairCount < 8) {
                    s32 insertIdx = pairCount;
                    if (pairCount > 0) {
                        s32 j = pairCount;
                        while (j > 0) {
                            if (sortDist >= pairArray[j - 1].distance) {
                                break;
                            }
                            pairArray[j] = pairArray[j - 1];
                            j--;
                        }
                        insertIdx = j;
                    }
                    pairArray[insertIdx].distance = sortDist;
                    pairArray[insertIdx].obstacle = obs;
                    pairCount++;
                }
            }
        }
    };

    if (EnsureObstacleBroadPhase()) {
        const s32 count = s_obstacleBroadPhaseCount;
        const ObstacleBroadPhaseEntry* entries = s_obstacleBroadPhase;
        const s32 homeX = dt->homePos.x;
        const s32 homeZ = dt->homePos.z;
        for (s32 i = 0; i < count; i++) {
            const ObstacleBroadPhaseEntry& entry = entries[i];
            // Broad-phase: extent + cylinder radius
            s32 threshold = entry.extent + humCyl.radius;

            s32 dx = homeX - entry.x;
            if (dx < 0) dx = -dx;

            s32 dz = homeZ - entry.z;
            if (dz < 0) dz = -dz;

            if (dx < threshold && dz < threshold) {
                narrowPhase(entry.obstacle);
            }
        }
    }
    else {
        for (ccMinNode* node = g_ai->moveList.head; node != nullptr;) {
            ccMinNode* next = node->next;
            Obstacle* obs = static_cast<Obstacle*>((Thing*)node);

            if (obs->flags & TF_MODEL_CREATED) {
                // Broad-phase: extent + cylinder radius
                s32 threshold = (s32)obs->collBox.extent + humCyl.radius;

                s32 dx = dt->homePos.x - obs->pos.x;
                if (dx < 0) dx = -dx;

                s32 dz = dt->homePos.z - obs->pos.z;
                if (dz < 0) dz = -dz;

                if (dx < threshold && dz < threshold) {
                    narrowPhase(obs);
                }
            }

            node = next;
        }
    }

    // Process closest obstacle first, then re-check rest
    if (pairCount > 0) {
        // HandleHumanoidCollision may move/kill obstacles (e.g. Pushable).
        ObstacleMutationScope mutation;
        pairArray[0].obstacle->HandleHumanoidCollision(hum);

        for (s32 i = 1; i < pairCount; i++) {
            Obstacle* obs = pairArray[i].obstacle;
            if (CheckStaticBoxCylinderCollision_Obstacle(
                obs->pos, obs->collBox, obs->orientation.y,
                dt->homePos, humCyl)) {
                obs->HandleHumanoidCollision(hum);
            }
        }
    }

    hum->SetFloorHeight(bestFloorHeight);

    Thing* curIssuer = dt->GetTicketIssuer();
    if (prevIssuer) {
        if (!curIssuer) {
            const LVector* obsPos = ((Obstacle*)prevIssuer)->GetDeltaVelocity();
            ObstacleMutationScope mutation;
            dt->DisembarkObstacle(*obsPos);
            hum->LetGoOfLedge();
        }
        else {
            // PSX: vtable+80 = HandleLand(homePos.y) when transitioning to obstacle
            if (!wasOnObstacle) {
                ObstacleMutationScope mutation;
                hum->HandleLand(dt->homePos.y);
            }
            s32 mat = ((Obstacle*)curIssuer)->GetFloorMaterial();
            hum->field436 = mat;
            hum->groundStandHeight = dt->homePos.y;
        }
    }
    else {
        if (curIssuer) {
            // PSX: vtable+80 = HandleLand(homePos.y) when transitioning to obstacle
            if (!wasOnObstacle) {
                ObstacleMutationScope mutation;
                hum->HandleLand(dt->homePos.y);
            }
            s32 mat = ((Obstacle*)curIssuer)->GetFloorMaterial();
            hum->field436 = mat;
            hum->groundStandHeight = dt->homePos.y;
        }
    }

    // Conservative: any ticket transition path above made virtual calls.
    if (prevIssuer || curIssuer) {
        InvalidateObstacleBroadPhase();
    }
}

// Obstacle animation table (PSX: g_animTable at 0x800E0E58, g_animCount at GP+0xF38)
#include "gen/animmgr.h"

static constexpr s32 MAX_OBSTACLE_ANIMS = 32;
static MiscAnimNode* g_obstacleAnimTable[MAX_OBSTACLE_ANIMS] = {};
static s32 g_obstacleAnimCount = 0;

// PSX: GetAnimation__8Obstaclelllll ? 0x8007CB88 (OBSTACLE.CPP:1803)
MiscAnimNode* Obstacle_GetAnimation(s32 animIndex) {
    MARKFUNCTION(0x8007CB88);
    if (animIndex < 0 || g_obstacleAnimCount <= animIndex) {
        return nullptr;
    }
    return g_obstacleAnimTable[animIndex];
}

// PSX: ClearPetalAnimList__8Obstacle ? 0x8007CB5C (OBSTACLE.CPP:1797)
void Obstacle_ClearPetalAnimList() {
    MARKFUNCTION(0x8007CB5C);
    for (s32 i = MAX_OBSTACLE_ANIMS - 1; i >= 0; i--) {
        g_obstacleAnimTable[i] = nullptr;
    }
    g_obstacleAnimCount = 0;
}

// PSX: AddAnimation portion of Obstacle::Load (0x8007CAA4)
void Obstacle_AddAnimation(MiscAnimNode* node) {
    if (g_obstacleAnimCount >= MAX_OBSTACLE_ANIMS) {
        return;
    }
    g_obstacleAnimTable[g_obstacleAnimCount] = node;
    g_obstacleAnimCount++;
}

// PSX: Load__8ObstacleR10tReadChunkPPv (0x8007CAA4)
// Chunk payload is two u32 values read from tFile::GetLong(); first is lookup hash.
void Obstacle_LoadAnimChunk(const u8* body, u32 bodySize) {
    MARKFUNCTION(0x8007CAA4);

    if (!body || bodySize < 8) {
        return;
    }

    u32 animHash = p3dReadU32LE(body + 0);
    (void)p3dReadU32LE(body + 4); // PSX reads and ignores second long here.

    MiscAnimNode* misc = nullptr;
    if (g_animMgr) {
        misc = g_animMgr->GetMiscAnim(animHash);
    }

    if (misc) {
        Obstacle_AddAnimation(misc);
        return;
    }

    MiscAnimNode* effectAnim = nullptr;
    if (GEffect_FindEffectAnim(animHash, &effectAnim)) {
        Obstacle_AddAnimation(effectAnim);
    }
}

// PSX: MovePassengersBasic__8Obstacle (OBSTACLE.CPP:2348, 0x8007D40C)
void Obstacle::MovePassengersBasic() {
    MARKFUNCTION(0x8007D40C);

    const s32 topY = pos.y + static_cast<s32>(collBox.maxY);
    for (ccMinNode* node = subNode.next; node != nullptr; node = node->next) {
        Ticket* ticket = static_cast<Ticket*>(node);
        DynamicThing* passenger = ticket->passenger;
        if (!passenger) {
            continue;
        }
        const s32 savedHomeX = passenger->homePos.x;
        s32 savedHomeY = passenger->homePos.y;
        const s32 savedHomeZ = passenger->homePos.z;
        const s32 savedVelX = passenger->velocity.x;
        s32 savedVelY = passenger->velocity.y;
        const s32 savedVelZ = passenger->velocity.z;

        if (savedHomeY < topY) {
            savedHomeY = topY - 2;
        }

        if (savedVelY <= 0) {
            savedVelY = 0;
            passenger->Land();
        }

        passenger->homePos.x = savedHomeX;
        passenger->homePos.y = savedHomeY;
        passenger->homePos.z = savedHomeZ;
        passenger->velocity.x = savedVelX;
        passenger->velocity.y = savedVelY;
        passenger->velocity.z = savedVelZ;
    }
}

#include "gen/common.h"
#include "ai/thing.h"
#include "ai/obstacle_shared.h"
#include "gen/blockmgr.h"
#include "gen/database.h"
#include "gen/game.h"
#include "gen/model.h"
#include "gen/levelmgr.h"
#include "gen/time.h"
#include "gen/world.h"
#include "p3d/hash.h"
#include "p3d/p3dmath.h"
#include "p3d/context.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "pc/log.h"
#include <vector>

#include "extra/shadowcsm.h"

// Global Thing unique ID counter (PSX: gp+3868)
u16 Thing::s_nextUniqueID = 0;

constexpr s32 COLLISION_SECTOR_MIN_HEIGHT = 0x80000001;

static s32 LerpS32(s32 a, s32 b, f32 alpha) {
    f64 af = (f64)a;
    f64 bf = (f64)b;
    f64 t = (f64)alpha;
    return (s32)(af + (bf - af) * t);
}

// PSX: __6TicketP5ThingP12DynamicThing (THING.CPP:1312)
Ticket::Ticket(Thing* iss, DynamicThing* pass) {
    issuer = iss;
    passenger = pass;
}

// PSX: _._6Ticket (THING.CPP:1318)
Ticket::~Ticket() {
    issuer = nullptr;
    passenger = nullptr;
}

// PSX: __5ThingPC10tagLVectorUs (THING.CPP:428)
Thing::Thing(const LVector* initialPos, u16 type) {
    MARKFUNCTION(0x80061558);

    thingType = type;
    collisionRadius = INVALID_HANDLE;

    pos = *initialPos;
    orientation = {};

    stateCounter = 1;
    health = 1;
    maxHealth = 1;

    thingHandle = nullptr;
    modelHash = 0;
    model = nullptr;
    blockNum = BLOCK_UNASSIGNED;

    // Assign unique ID from global counter
    uniqueID = s_nextUniqueID;
    s_nextUniqueID++;

    // Initialize flags: set needs activation, clear activated and model created
    flags = TF_NEEDS_ACTIVATION;
    flags2 = 0;
}

// PSX: _._5Thing (THING.CPP:458)
Thing::~Thing() {
    MARKFUNCTION(0x80061640);
    ObstacleForgetRenderTransform(this);
    DeleteModel();
    RemAllPassengers();
    if (thingHandle) {
        // PSX: Close__11ThingHandle - clear the handle's back-pointer
        thingHandle->owner = nullptr;
        thingHandle = nullptr;
    }
}

// PSX: Think__5Thing (THING.CPP:478)
// Calls UpdatePosition() through virtual dispatch
void Thing::Think() {
    MARKFUNCTION(0x800616BC);
    UpdatePosition();
}

// PSX: Draw__5Thing (THING.CPP:487)
// Sets model position/orientation from Thing fields, then calls model->Show
void Thing::Draw() {
    MARKFUNCTION(0x800616EC);

    LVector drawPos = pos;
    LVector drawOrient = orientation;

    if (model) {
#if MODERN_GRAPHICS
        if (!ShadowCSM::IsCasterPrepass()) {
            ObstacleBuildRenderTransform(this, pos, orientation, drawPos, drawOrient);
        }
#else
        ObstacleBuildRenderTransform(this, pos, orientation, drawPos, drawOrient);
#endif

        // PSX: copies pos/orientation to model, then calls Show
        Model* m = static_cast<Model*>(model);
        m->posX = drawPos.x;
        m->posY = drawPos.y;
        m->posZ = drawPos.z;
        m->rotX = (u16)(drawOrient.x & 0xFFFF);
        m->rotY = (u16)(drawOrient.y & 0xFFFF);
        m->rotZ = (u16)(drawOrient.z & 0xFFFF);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        static u32 pspPlatformDrawProbe = 0;
        const u32 pspPlatformDrawProbeId = pspPlatformDrawProbe++;
        if (thingType == AITypes::TT_PLATFORM) {
            if (pspPlatformDrawProbeId < 8u) {
                LOG("[PSP Thing] platform draw begin #%lu thing=%p model=%p drawable=%p type=%ld",
                    static_cast<unsigned long>(pspPlatformDrawProbeId), static_cast<void*>(this),
                    static_cast<void*>(m), static_cast<void*>(m->drawable),
                    static_cast<long>(m->drawableType));
            }
        }
#else
        const u32 pspPlatformDrawProbeId = 0u;
#endif
        m->Show(0);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (thingType == AITypes::TT_PLATFORM && pspPlatformDrawProbeId < 8u) {
            LOG("[PSP Thing] platform draw done #%lu thing=%p",
                static_cast<unsigned long>(pspPlatformDrawProbeId), static_cast<void*>(this));
        }
#endif
        return;
    }

#if MODERN_GRAPHICS
    if (ShadowCSM::IsCasterPrepass()) {
        return;
    }
#endif

    // PC debug: draw wireframe box at thing position when no model is loaded
    // Box size: 300 wide, 768 tall (approximate humanoid collision box)
    f32 hw = 300.0f;  // half-width
    f32 hh = 768.0f;  // full height
    f32 hd = 300.0f;  // half-depth

    f32 cx = (f32)drawPos.x;
    f32 cy = (f32)drawPos.y;
    f32 cz = (f32)drawPos.z;

    f32 x0 = cx - hw, x1 = cx + hw;
    f32 y0 = cy, y1 = cy + hh;
    f32 z0 = cz - hd, z1 = cz + hd;

    // 12 edges of a box = 24 line vertices, 24 indices
    struct DV { f32 x, y, z, r, g, b; };
    DV verts[24];
    u16 indices[24];
    u32 vi = 0;

    // Color: yellow for player (type 1), red for others
    f32 cr = (thingType == AITypes::TT_PLAYER) ? 1.0f : 1.0f;
    f32 cg = (thingType == AITypes::TT_PLAYER) ? 1.0f : 0.3f;
    f32 cb = (thingType == AITypes::TT_PLAYER) ? 0.0f : 0.3f;

    // Helper macro to push a line
#define PUSHLINE(ax,ay,az,bx,by,bz) \
        indices[vi] = (u16)vi; verts[vi] = {ax,ay,az,cr,cg,cb}; vi++; \
        indices[vi] = (u16)vi; verts[vi] = {bx,by,bz,cr,cg,cb}; vi++;

    // Bottom face
    PUSHLINE(x0, y0, z0, x1, y0, z0);
    PUSHLINE(x1, y0, z0, x1, y0, z1);
    PUSHLINE(x1, y0, z1, x0, y0, z1);
    PUSHLINE(x0, y0, z1, x0, y0, z0);
    // Top face
    PUSHLINE(x0, y1, z0, x1, y1, z0);
    PUSHLINE(x1, y1, z0, x1, y1, z1);
    PUSHLINE(x1, y1, z1, x0, y1, z1);
    PUSHLINE(x0, y1, z1, x0, y1, z0);
    // Verticals
    PUSHLINE(x0, y0, z0, x0, y1, z0);
    PUSHLINE(x1, y0, z0, x1, y1, z0);
    PUSHLINE(x1, y0, z1, x1, y1, z1);
    PUSHLINE(x0, y0, z1, x0, y1, z1);
#undef PUSHLINE

    pddiPrimBufferDesc desc(PDDI_PRIM_LINES,
                            PDDI_V_POSITION | PDDI_V_COLOUR,
                            vi, vi);
    pddiPrimBuffer* buf = p3d::device->NewPrimBuffer(desc);
    buf->SetVertexData(verts, vi);
    buf->SetIndices(indices, vi);

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    Mat4 identity;
    p3d::context->SetWorldMatrix(identity);
    p3d::context->SetVRAMHandle(0);
    p3d::context->DrawPrimBuffer(buf);
    p3d::context->SetWorldMatrix(savedWorld);
    if (g_game && g_game->GetWorld()) {
        p3d::context->SetVRAMHandle(g_game->GetWorld()->GetVRAMHandle());
    }
    buf->Release();
}

// PSX: Reset__5Thing (THING.CPP:502)
void Thing::Reset() {
    MARKFUNCTION(0x80061760);
    // PSX: flags |= 4 (needs activation)
    flags |= TF_NEEDS_ACTIVATION;
    // PSX: clear orientation
    orientation = {};
    // PSX: restore health from maxHealth
    health = maxHealth;
    // PSX: flags2 &= 1 (keep only bit 0)
    flags2 &= TF2_KILLED;
}

// PSX: UpdatePosition__5Thing (THING.HPP:440)
// Base implementation does nothing
void Thing::UpdatePosition() {
    MARKFUNCTION(0x800628F4);
}

// PSX: Activate__5Thing (THING.CPP:521, 0x80061790)
// PSX: checks InActiveList AND IsValidBlockNumber before activating.
void Thing::Activate() {
    MARKFUNCTION(0x80061790);
    bool valid = false;
    if (g_blockManager) {
        if (g_blockManager->InActiveList(blockNum)) {
            if (g_blockManager->IsValidBlockNumber(blockNum)) {
                valid = true;
            }
        }
    }
    if (valid) {
        flags |= TF_ACTIVATED;
        if (!(flags & TF_MODEL_CREATED)) {
            CreateModel(nullptr);
        }
    }
}

// PSX: Deactivate__5Thing (THING.CPP:546, 0x8006182C)
// PSX: checks flags and InActiveList before clearing activated.
void Thing::Deactivate() {
    MARKFUNCTION(0x8006182C);
    // PSX THING.CPP:546. v5 = original TF_ACTIVATED (bit 4) BEFORE it may be
    // cleared below. The model is deleted only when v5 was already 0, i.e. the
    // thing was *already* deactivated on entry (and stays deactivated with a
    // model). A still-active thing that is being deactivated this call keeps its
    // model. (The host previously inverted this to `wasActivated`, which deleted
    // NIS actors' models the instant their block left the active set.)
    const bool wasInactiveOnEntry = (flags & TF_ACTIVATED) == 0;
    const bool hasBit5 = (flags & TF_BIT5) != 0;

    if (!hasBit5) {
        if (g_blockManager && !g_blockManager->InActiveList(blockNum)) {
            flags &= ~TF_ACTIVATED;
        }
    }

    if (wasInactiveOnEntry) {
        if (!(flags & TF_ACTIVATED)) {
            if (flags & TF_MODEL_CREATED) {
                DeleteModel();
            }
        }
    }
}

// PSX: Move__5Thing (THING.CPP:835)
// Base implementation does nothing (pure virtual-like)
void Thing::Move() {
    MARKFUNCTION(0x80061D60);
}

// PSX: CreateModel__5ThingPCc (THING.CPP:585, 0x800618E0)
// PSX: looks up model by hash in LevelManager, creates SModel/GModel/EModel
// based on type, links drawable to the OriginalSTree/OriginalGeo data.
void Thing::CreateModel(const char* name) {
    MARKFUNCTION(0x800618E0);

    if (!g_levelManager)
        return;

    // PSX: if name provided, hash it; else use modelHash (nameHash from AnalyzeMesh)
    u32 hash;
    if (name) {
        hash = p3dHash(name);
    }
    else {
        hash = modelHash;
        if (!hash)
            return;
    }

    // PSX: FindModel__12LevelManagerl(theLevelMgr, hash)
    OriginalBasic* found = g_levelManager->FindModel(hash);
    if (!found) {
        LOG("[Thing::CreateModel] Model not found for hash 0x%08X", (u32)hash);
        return;
    }

    // PSX: check type at OriginalBasic+16 (0=Geo, 1=STree, 2=ETree)
    u16 modelType = found->GetType();
    Model* existing = static_cast<Model*>(model);

    if (modelType == 0) {
        GModel* gm = nullptr;
        if (existing && existing->drawableType == 1) {
            gm = static_cast<GModel*>(existing);
        }
        else {
            if (existing) {
                delete existing;
            }
            gm = new GModel();
            model = gm;
        }
        gm->backPtr = this;
        gm->SetOriginalGeo(static_cast<OriginalGeo*>(found));
    }
    else if (modelType == 1) {
        SModel* sm = nullptr;
        if (existing && existing->drawableType != 1) {
            sm = static_cast<SModel*>(existing);
        }
        else {
            if (existing) {
                delete existing;
            }
            sm = new SModel();
            model = sm;
        }
        sm->backPtr = this;
        sm->SetOriginalSTree(static_cast<OriginalSTree*>(found));
    }
    else if (modelType == 2) {
        EModel* em = nullptr;
        if (existing && existing->drawableType == 3) {
            em = static_cast<EModel*>(existing);
        }
        else {
            if (existing) {
                delete existing;
            }
            em = new EModel();
            model = em;
        }
        em->backPtr = this;
        em->SetOriginalETree(static_cast<OriginalETree*>(found), nullptr);
    }

    if (model) {
        // PSX: copy the loaded model's nameHash to model+20
        Model* m = static_cast<Model*>(model);
        m->nameCRC = found->nameCRC;
        // PSX: set backPtr to this Thing
        m->backPtr = this;
    }

    // PSX: flags |= 0x50 (TF_MODEL_CREATED | TF_ACTIVATED)
    flags |= (TF_MODEL_CREATED | TF_ACTIVATED);
}

// PSX: DeleteModel__5Thing (THING.CPP:689)
void Thing::DeleteModel() {
    MARKFUNCTION(0x80061AAC);
    // PSX: calls model destructor through vtable: (*(model+8+8))(model, 3)
    if (model) {
        Model* m = static_cast<Model*>(model);
        delete m;
        model = nullptr;
    }
    flags &= ~TF_MODEL_CREATED;
}

// PSX: HandleCollision__5ThingP5Thingle (THING.CPP:713)
void Thing::HandleCollision(Thing* /*other*/, s32 /*damage*/, ...) {
    MARKFUNCTION(0x80061B08);
    // Base does nothing
}

// PSX: AnalyzeMesh__5ThingP6DBRoot (THING.CPP:1224)
void Thing::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x80062680);
    if (!root)
        return;

    // PSX: FindAttrib(root, 5) - attrib 5 = mesh name hash
    const DBAttrib* a5 = root->FindAttrib(5);
    if (a5) {
        if (a5->type == 0) {
            // String attribute - hash it
            const char* str = a5->strValue ? a5->strValue : "";
            modelHash = p3dHash(str);
        }
        else if (a5->type == 1) {
            // Numeric attribute - use directly
            modelHash = a5->value;
        }
    }

    // PSX: FindAttrib(root, 15) - attrib 15 = block number
    const DBAttrib* a15 = root->FindAttrib(15);
    if (a15) {
        blockNum = (u16)a15->value;
    }
}

// PSX: GetViewSpot__5ThingP10tagLVectorT1 (THING.CPP:1210)
void Thing::GetViewSpot(LVector* outPos, LVector* /*outTarget*/) {
    MARKFUNCTION(0x80062638);
    if (outPos) {
        *outPos = pos;
    }
}

// PSX: Kill__5Thing (THING.HPP:518)
void Thing::Kill() {
    MARKFUNCTION(0x800628D0);
    // PSX: *(a1+88) |= 1 - sets bit 0 of flags (offset 88)
    flags |= TF_DEAD;
}

// PSX: GetSoundPosPtr__5Thing (THING.HPP:516)
LVector* Thing::GetSoundPosPtr() {
    MARKFUNCTION(0x800628E4);
    return &pos;
}

// PSX: GetInitialPos__5Thing (THING.HPP:512)
const LVector* Thing::GetInitialPos() {
    MARKFUNCTION(0x800628EC);
    return &pos;
}

// PSX: AddPassenger__5ThingP12DynamicThing (THING.CPP:1079)
// PSX treats Thing::subNode (offset 0x40) as an intrusive ccMinList whose
// head/tail pointers share layout with ccMinNode::next/prev. New tickets are
// appended at the tail via ccMinList::AddNode(tail, newNode).
void Thing::AddPassenger(DynamicThing* passenger) {
    MARKFUNCTION(0x80062400);
    if (passenger->ticket != nullptr) {
        return;
    }

    Ticket* t = new Ticket(this, passenger);

    // subNode.next == list head, subNode.prev == list tail.
    ccMinNode* after = subNode.prev;
    if (after != nullptr) {
        ccMinNode* afterNext = after->next;
        if (afterNext != nullptr) {
            t->next = afterNext;
            afterNext->prev = t;
        }
        else {
            t->next = nullptr;
        }
        t->prev = after;
        after->next = t;
    }
    else {
        ccMinNode* head = subNode.next;
        if (head != nullptr) {
            t->next = head;
            head->prev = t;
        }
        else {
            t->next = nullptr;
        }
        t->prev = nullptr;
        subNode.next = t;
    }
    if (subNode.prev == after) {
        subNode.prev = t;
    }

    passenger->ticket = t;
}

// PSX: RemPassenger__5ThingP6Ticket (THING.CPP:1104)
// PSX: ccMinList::RemNode(&subNode, t) — subNode's next/prev serve as the
// list's head/tail, so both must be updated when t is at either end.
void Thing::RemPassenger(Ticket* t) {
    MARKFUNCTION(0x8006247C);
    if (!t) return;

    // Update head/tail on the owning list before unlinking t itself.
    if (subNode.next == t) subNode.next = t->next;
    if (subNode.prev == t) subNode.prev = t->prev;
    if (t->prev) t->prev->next = t->next;
    if (t->next) t->next->prev = t->prev;
    t->next = nullptr;
    t->prev = nullptr;

    if (t->passenger) {
        t->passenger->ticket = nullptr;
    }
    delete t;
}

// PSX: RemAllPassengers__5Thing (THING.CPP:1144)
// PSX walks subNode's list head and calls RemPassenger on each node.
void Thing::RemAllPassengers() {
    MARKFUNCTION(0x80062504);
    while (subNode.next != nullptr) {
        Ticket* t = static_cast<Ticket*>(subNode.next);
        RemPassenger(t);
    }
    subNode.prev = nullptr;
}

// PSX: GetThingHandle__5Thing (THING.CPP:1170)
ThingHandle* Thing::GetThingHandle() {
    MARKFUNCTION(0x80062574);
    // PSX: lazily allocates ThingHandle (8 bytes) with owner=this, refCount=1
    if (!thingHandle) {
        ThingHandle* h = new ThingHandle();
        h->owner = this;
        h->refCount = 1;
        thingHandle = h;
    }
    return thingHandle;
}

// PSX: ClearFloorHeight__5Thing (THING.CPP:765)
void Thing::ClearFloorHeight() {
    MARKFUNCTION(0x80061BFC);
    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    ModelFloorHeightState* floorState = GetModelFloorHeightState(m);
    if (!floorState) {
        return;
    }

    // PSX slot order for floor state is [0]=previous floor, [1]=current floor.
    // Clear moves current->previous and invalidates current.
    floorState->current = floorState->previous;
    floorState->previous = (s32)0x80000001;
}

// PSX: SetFloorHeight__5Thingl (THING.CPP:777)
void Thing::SetFloorHeight(s32 height) {
    MARKFUNCTION(0x80061C38);
    if (!model) {
        return;
    }

    Model* m = static_cast<Model*>(model);
    ModelFloorHeightState* floorState = GetModelFloorHeightState(m);
    if (!floorState) {
        return;
    }

    // PSX updates slot+4 (current floor for this frame accumulation).
    if (height > floorState->previous) {
        floorState->previous = height;
    }
}

// PSX: GetObjectToWorldSpaceVector__5Thing (THING.CPP:1352)
void Thing::GetObjectToWorldSpaceVector(const SVector& in, SVector& out) {
    MARKFUNCTION(0x80062874);
    // PSX: reads orientation as u16 angles, builds YZX rotation matrix, transforms
    Mat4 rot;
    p3dBuildRotMatrixYZX(orientation.x, orientation.y, orientation.z, rot);

    Vec3 v((f32)in.x, (f32)in.y, (f32)in.z);
    Vec3 result = p3dVecTimesRotMatrix(v, rot);
    out.x = (s16)result.x;
    out.y = (s16)result.y;
    out.z = (s16)result.z;
    out.pad = 0;
}

void Thing::GetObjectToWorldSpaceVector(const LVector& in, LVector& out) {
    MARKFUNCTION(0x80062874);
    Mat4 rot;
    p3dBuildRotMatrixYZX(orientation.x, orientation.y, orientation.z, rot);

    Vec3 v((f32)in.x, (f32)in.y, (f32)in.z);
    Vec3 result = p3dVecTimesRotMatrix(v, rot);
    out.x = (s32)result.x;
    out.y = (s32)result.y;
    out.z = (s32)result.z;
}

// PSX: __12DynamicThingPC10tagLVectorUs (THING.CPP:840)
DynamicThing::DynamicThing(const LVector* initialPos, u16 type)
    : Thing(initialPos, type) {
    MARKFUNCTION(0x80061D68);

    // PSX: flags |= 0x0800 (mark as dynamic)
    flags |= TF_DYNAMIC;

    // PSX: homePos = initialPos
    homePos = *initialPos;

    groundStandHeight = COLLISION_SECTOR_MIN_HEIGHT;
    ticket = nullptr;
}

// PSX: _._12DynamicThing (THING.CPP:851)
DynamicThing::~DynamicThing() {
    MARKFUNCTION(0x80061DE0);
    if (ticket) {
        Disembark();
        ticket = nullptr;
    }
}

// PSX: Reset__12DynamicThing (THING.CPP:865)
void DynamicThing::Reset() {
    MARKFUNCTION(0x80061E38);
    Thing::Reset();

    maxSpeed = 100;
    gravity = FIX16_HALF;

    // Clear all movement vectors
    velocity = {};
    force = {};
    contactForce = {};

    // Reset home position to current position
    homePos = pos;

    maxFallDivisor = 10;

    groundStandHeight = COLLISION_SECTOR_MIN_HEIGHT;
    Disembark();
}

// PSX: Move__12DynamicThing (THING.CPP:891)
// PSX: 0x80061EC4, 1340 bytes.
// Full physics step: subtracts force from velocity, applies gravity friction,
// distributes contact forces over stateCounter frames, half-step integration,
// clamps XZ speed to health, damps XZ force by g_dampingFactor.
void DynamicThing::Move() {
    MARKFUNCTION(0x80061EC4);

    LVector localForce = {};
    LVector movement = {};

    // Step 1: subtract accumulated force from velocity
    velocity.x -= force.x;
    velocity.y -= force.y;
    velocity.z -= force.z;

    // Step 2: compute signs for velocity and contact force directions
    s32 sign_vx = (velocity.x >= 0) ? 1 : -1;
    s32 sign_vz = (velocity.z >= 0) ? 1 : -1;
    s32 sign_cfx = (contactForce.x >= 0) ? 1 : -1;
    s32 sign_cfz = (contactForce.z >= 0) ? 1 : -1;

    // Step 3: gravity friction (drag opposing velocity)
    // PSX: abs(vel) * gravity → 64-bit, extract bits 16..47, negate sign
    s32 abs_vx = velocity.x * sign_vx;
    s32 abs_vz = velocity.z * sign_vz;

    s32 drag_x = (s32)(((s64)abs_vx * (s64)gravity) >> 16);
    s32 drag_z = (s32)(((s64)abs_vz * (s64)gravity) >> 16);

    s32 friction_x = (-sign_vx) * drag_x;
    s32 friction_z = (-sign_vz) * drag_z;

    // Reset gravity to default (overridden each frame by ground contact)
    gravity = FIX16_HALF;

    // Step 4: contact force handling (if stateCounter != 0)
    if (stateCounter != 0) {
        // X axis
        if (contactForce.x != 0) {
            s32 abs_cfx = contactForce.x * sign_cfx;
            s32 divided = rmDiv16i(abs_cfx, stateCounter) >> 16;
            localForce.x = (divided * sign_cfx) + friction_x;
        }
        else {
            s32 av = (velocity.x >= 0) ? velocity.x : -velocity.x;
            if (av < 2) {
                velocity.x = 0;
            }
            else if (friction_x != 0) {
                localForce.x = friction_x;
            }
        }

        // Y axis (plain integer division, no rmDiv16i)
        if (contactForce.y != 0) {
            localForce.y = contactForce.y / stateCounter;
        }

        // Z axis
        if (contactForce.z != 0) {
            s32 abs_cfz = contactForce.z * sign_cfz;
            s32 divided = rmDiv16i(abs_cfz, stateCounter) >> 16;
            localForce.z = (divided * sign_cfz) + friction_z;
        }
        else {
            s32 av = (velocity.z >= 0) ? velocity.z : -velocity.z;
            if (av < 2) {
                velocity.z = 0;
            }
            else if (friction_z != 0) {
                localForce.z = friction_z;
            }
        }

        // Y gravity fall (maxFallDivisor accumulates downward acceleration)
        if (maxFallDivisor != 0) {
            localForce.y -= maxFallDivisor;
            if (localForce.y < -g_maxFallSpeed) {
                localForce.y = -g_maxFallSpeed;
            }
        }
        maxFallDivisor = 18;
    }

    // Step 5: half-step integration
    // PSX pattern: make positive, (n + (unsigned(n) >> 31)) >> 1, restore sign
    s32 sign_lfx = (localForce.x >= 0) ? 1 : -1;
    s32 sign_lfz = (localForce.z >= 0) ? 1 : -1;

    s32 abs_lfx = localForce.x * sign_lfx;
    s32 abs_lfz = localForce.z * sign_lfz;

    s32 half_lfx = (s32)(((u32)abs_lfx + ((u32)abs_lfx >> 31)) >> 1) * sign_lfx;
    s32 half_lfy = (localForce.y + (s32)((u32)localForce.y >> 31)) >> 1;
    s32 half_lfz = (s32)(((u32)abs_lfz + ((u32)abs_lfz >> 31)) >> 1) * sign_lfz;

    movement.x = velocity.x + half_lfx;
    movement.y = velocity.y + half_lfy;
    movement.z = velocity.z + half_lfz;

    // Step 6: clamp movement magnitude to maxSpeed (XZ only, Y untouched)
    // PSX: 80062204 calls rmMag3__Flll (integer overload)
    s32 mag = rmMag3(movement.x, movement.y, movement.z);
    if (maxSpeed < mag) {
        LVector normalized;
        rmV3Normalize(&normalized, &movement);
        movement.x = (s32)(((s64)normalized.x * maxSpeed) >> 16);
        // movement.y intentionally NOT clamped
        movement.z = (s32)(((s64)normalized.z * maxSpeed) >> 16);
    }

    // Step 7: add accumulated force back to movement
    movement.x += force.x;
    movement.y += force.y;
    movement.z += force.z;

    // Step 8: update homePos
    homePos.x += movement.x;
    homePos.y += movement.y;
    homePos.z += movement.z;

    // Step 9: update velocity with localForce
    velocity.x += localForce.x;
    velocity.y += localForce.y;
    velocity.z += localForce.z;

    // Step 10: damp XZ force and add to velocity
    // PSX: (g_dampingFactor * force) >> 16, stored back to force, then added to velocity
    s32 damped_fx = (s32)(((s64)g_dampingFactor * (s64)force.x) >> 16);
    s32 damped_fz = (s32)(((s64)g_dampingFactor * (s64)force.z) >> 16);

    force.x = damped_fx;
    force.z = damped_fz;

    velocity.x += damped_fx;
    velocity.y += force.y;  // Y force is NOT damped
    velocity.z += damped_fz;

    // Step 11: save contactForce to field148[0..2], clear contactForce
    field148[0] = contactForce.x;
    field148[1] = contactForce.y;
    field148[2] = contactForce.z;
    contactForce = {};
}

// PSX: UpdatePosition__12DynamicThing (THING.CPP:1280)
// PSX: 196 bytes. Computes displacement, updates block membership,
// then commits homePos to pos.
void DynamicThing::UpdatePosition() {
    MARKFUNCTION(0x8006272C);

    // Compute displacement (homePos - pos)
    field148[3] = homePos.x - pos.x;
    field148[4] = homePos.y - pos.y;
    field148[5] = homePos.z - pos.z;

    // Update block membership if activated
    if (flags & TF_ACTIVATED) {
        if (g_blockManager) {
            u16 newBlock = g_blockManager->GetBlockNumber(homePos);
            if (newBlock != BLOCK_UNASSIGNED) {
                blockNum = newBlock;
            }
            else if (!(flags & TF_BIT5)) {
                // Left all blocks and bit5 not set: deactivate
                flags &= ~TF_ACTIVATED;
            }
        }
    }

    // Commit homePos to pos
    pos = homePos;
}

// PSX: AddForce__12DynamicThinglPC9_RMVECT16 (THING.CPP:753, 0x80061B44)
// PSX: builds rotation matrix from SVector (Euler angles), rotates {0,0,magnitude}
// to get world-space force, adds to contactForce.
// direction is packed as int32[3] on PSX stack; as s16*: [rotX, 0, rotY, 0, rotZ, 0]
// p3dBuildRotMatrixZYX uses s16[0]=rotX, s16[2]=rotY, s16[4]=rotZ.
void DynamicThing::AddForce(s32 magnitude, const SVector* direction) {
    MARKFUNCTION(0x80061B44);
    if (!direction) return;

    Mat4 matrix;
    p3dBuildRotMatrixZYX(direction->x, direction->z, direction->pad, matrix);

    Vec3 rotated = p3dVecTimesRotMatrix(Vec3(0.0f, 0.0f, (f32)magnitude), matrix);
    contactForce.x += (s32)rotated.x;
    contactForce.y += (s32)rotated.y;
    contactForce.z += (s32)rotated.z;
}

// PSX: Land__12DynamicThing (THING.CPP:794)
void DynamicThing::Land() {
    MARKFUNCTION(0x80061C78);

    const LVector landedForce = force;

    force = {};
    flags |= TF_ON_GROUND;

    velocity.x -= landedForce.x;
    velocity.y -= landedForce.y;
    velocity.z -= landedForce.z;
}

// PSX: DisembarkObstacle__12DynamicThingRC10tagLVector (THING.CPP:810)
// The parameter is the previous issuer's delta velocity (from
// Obstacle::GetDeltaVelocity) - NOT a world-space position. The function
// stores it as the new force, clamps force.y to be non-negative, folds that
// force into velocity so the passenger keeps the platform's momentum, and
// clears TF_ON_GROUND. It deliberately does NOT touch pos/homePos or detach
// the ticket; Disembark() is invoked separately by the caller.
void DynamicThing::DisembarkObstacle(const LVector& deltaVelocity) {
    MARKFUNCTION(0x80061CC4);

    // PSX: store delta velocity at +0x70/+0x74/+0x78 (force)
    force.x = deltaVelocity.x;
    force.y = deltaVelocity.y;
    force.z = deltaVelocity.z;

    if (force.y < 0) {
        force.y = 0;
    }

    velocity.x += force.x;
    velocity.y += force.y;
    velocity.z += force.z;

    flags &= ~TF_ON_GROUND;
}

// PSX: Disembark__12DynamicThing (THING.CPP:1126)
void DynamicThing::Disembark() {
    MARKFUNCTION(0x800624C4);
    if (ticket) {
        if (ticket->issuer) {
            ticket->issuer->RemPassenger(ticket);
        }
    }
    ticket = nullptr;
}

// PSX: GetTicketIssuer__12DynamicThing (THING.CPP:1162)
Thing* DynamicThing::GetTicketIssuer() {
    MARKFUNCTION(0x80062550);
    if (ticket) {
        return ticket->issuer;
    }
    return nullptr;
}

// PSX: HandleLand__12DynamicThingl (THING.CPP:1360)
void DynamicThing::HandleLand(s32 /*height*/) {
    MARKFUNCTION(0x800628C8);
}

// PSX: DistanceFromPointXZ__C5ThingRC10tagLVector (THING.CPP:1180, 0x800625C0)
s32 Thing::DistanceFromPointXZ(const LVector& point) const {
    MARKFUNCTION(0x800625C0);
    return rmMag2ff(pos.x - point.x, pos.z - point.z);
}

// PSX: DistanceFromPoint__C5ThingRC10tagLVector (THING.CPP:1189, 0x800625F4)
s32 Thing::DistanceFromPoint(const LVector& point) const {
    MARKFUNCTION(0x800625F4);
    return rmMag3ff(pos.x - point.x, pos.y - point.y, pos.z - point.z);
}

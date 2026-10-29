#include "common.h"

#include "gen/pweffect.h"

#include "gen/ai.h"
#include "gen/colsect.h"
#include "gen/database.h"
#include "gen/effects.h"
#include "gen/particle.h"
#include "gen/path.h"

#include "ai/activezn.h"
#include "ai/obstacle.h"
#include "ai/platform.h"
#include "ai/player.h"
#include "ai/thing.h"

#include "snd/esound.h"
#include "snd/sndfact.h"

#include "p3d/context.h"
#include "p3d/p3dmath.h"

#include <cstring>

static const s32 kPathAttribNotFound = static_cast<s32>(0xABCDABCD);
static constexpr bool kDebugFreezePWParticleRenderAtSpawn = false;
static constexpr bool kDebugTracePWTransforms = false;

static ccList g_pWEffectPool;

static s32 AddEffect(Effects* effect, s32 addToHead) {
    return Effects_AddEffect(effect, addToHead);
}

static Effects* RemoveEffect(Effects* effect) {
    return Effects_RemoveEffect(effect);
}

static u32 GetAttribValue(const DBAttrib* attrib) {
    return attrib ? attrib->value : 0;
}

static f32 QuantizePsxScale16(s32 scale16) {
    const s16 matrixEntry = static_cast<s16>(scale16 >> 4);
    return static_cast<f32>(matrixEntry) / 4096.0f;
}

static Mat4 BuildEffectWorldMatrix(const LVector& pos, const LVector* scale) {
    Mat4 world = Mat4();
    if (scale) {
        world.m[0] = QuantizePsxScale16(scale->x);
        world.m[5] = QuantizePsxScale16(scale->y);
        world.m[10] = QuantizePsxScale16(scale->z);
    }

    world.m[12] = static_cast<f32>(pos.x);
    world.m[13] = static_cast<f32>(pos.y);
    world.m[14] = static_cast<f32>(pos.z);
    return world;
}

static s32 ResolveCollisionEffectBlockNumber(const LVector& pos) {
    return CollisionSector::GetBlockNumber(pos);
}

class PWPathInfo {
public:
    PWPathInfo() {
        MARKFUNCTION(0x800BE73C);
    }

    ~PWPathInfo() {
        MARKFUNCTION(0x800BE78C);

        if (path) {
            delete path;
            path = nullptr;
        }
    }

    s32 Init(const DBPath* pathSource, const DBPoint* pointSource) {
        MARKFUNCTION(0x800BE54C);

        if (!pathSource || !pointSource) {
            return 0;
        }

        u32 value = 0;
        if (!pointSource->FindAttribValue(6, &value)) {
            return 0;
        }

        if (path) {
            delete path;
            path = nullptr;
        }

        if (value == 1) {
            SplinePath* spline = new SplinePath();
            spline->Init(pathSource);
            path = spline;
            splinePath = true;
        }
        else {
            LinearPath* linear = new LinearPath();
            linear->Init(pathSource);
            path = linear;
            splinePath = false;
        }

        if (!path) {
            return 0;
        }

        if (pointSource->FindAttribValue(7, &value) && value) {
            if (value == 2 || value == 3) {
                pathMode = static_cast<s32>(value);
            }
            else {
                pathMode = 1;
            }
        }
        else {
            pathMode = 0;
        }

        if (pointSource->FindAttribValue(8, &value)) {
            lerpZ = static_cast<s32>(value);
        }

        if (pointSource->FindAttribValue(9, &value)) {
            lerpX = static_cast<s32>(value);
        }

        if (pointSource->FindAttribValue(10, &value)) {
            lerpY = static_cast<s32>(value);
        }

        return 1;
    }

    s32 Reset() {
        MARKFUNCTION(0x800BE7F8);

        if (!path) {
            return 0;
        }

        path->Reset();
        return OnNewPathNode(0);
    }

    s32 Update() {
        MARKFUNCTION(0x800BE844);

        if (!path) {
            return 0;
        }

        if (state != 0 && state != 2) {
            if (path->EndOfPath()) {
                if (pathMode == 0) {
                    return 1;
                }

                if (pathMode == 1) {
                    path->Flip();
                }
                else if (pathMode == 3) {
                    return 0;
                }

                path->Reset();
                OnNewPathNode(0);
            }

            if (allowMove) {
                s32 crossedNode = 0;
                if (splinePath) {
                    crossedNode = static_cast<SplinePath*>(path)->Move(moveSpeed);
                }
                else {
                    crossedNode = static_cast<LinearPath*>(path)->Move(moveSpeed);
                }

                if (crossedNode) {
                    OnNewPathNode(1);
                }
            }
            else {
                OnNewPathNode(0);
            }
        }

        if (state == 2) {
            path->Reset();
        }

        return 0;
    }

    s32 OnNewPathNode(s32 /*nodeChanged*/) {
        MARKFUNCTION(0x800BEA44);

        if (!path || !path->nodeAttribs || path->numPoints <= 0) {
            return 0;
        }

        const s32 segment = path->currentSegment;
        if (segment < 0 || segment >= path->numPoints) {
            return 0;
        }

        const NodeAttribs& attribs = path->nodeAttribs[segment];

        const s32 speedAttrib = attribs.GetAttrib(7);
        moveSpeed = (speedAttrib == kPathAttribNotFound) ? 5 : speedAttrib;

        const s32 rotZ = attribs.GetAttrib(8);
        if (rotZ != kPathAttribNotFound) {
            rotation.z = rotZ;
        }

        const s32 rotX = attribs.GetAttrib(9);
        if (rotX != kPathAttribNotFound) {
            rotation.x = rotX;
        }

        const s32 rotY = attribs.GetAttrib(10);
        if (rotY != kPathAttribNotFound) {
            rotation.y = rotY;
        }

        const s32 activeZoneHash = attribs.GetAttrib(11);
        if (activeZoneHash == kPathAttribNotFound) {
            allowMove = 1;
        }
        else {
            allowMove = 0;

            if (g_ai && Player::s_player) {
                for (ccMinNode* node = g_ai->activeZoneList.head; node; node = node->next) {
                    ActiveZone* zone = static_cast<ActiveZone*>(static_cast<ccNode*>(node));
                    if (zone->nameCRC == static_cast<u32>(activeZoneHash)
                        && zone->IsInActiveZone(Player::s_player)) {
                        allowMove = 1;
                        break;
                    }
                }
            }
        }

        return 1;
    }

    const LVector* GetPosition() const {
        MARKFUNCTION(0x800BED44);

        if (path) {
            return &path->current;
        }

        return &fallbackPosition;
    }

    const LVector* GetRotation() const {
        MARKFUNCTION(0x800BED50);
        return &rotation;
    }

    Path* path = nullptr;
    LVector fallbackPosition = {};
    LVector rotation = {};
    s32 moveSpeed = 5;
    s32 pathMode = 0;
    s32 state = 1;
    s32 allowMove = 1;
    s32 lerpX = 0;
    s32 lerpY = 0;
    s32 lerpZ = 0;
    bool splinePath = false;
};

class PWEffect : public Effects {
public:
    PWEffect();
    ~PWEffect() override;

    s32 Create() override;
    s32 Update() override;
    void Display(s32 inBlockNum) override;
    s32 PutBackEffect() override;
    bool IsDirectorOverlay() const override;
    bool GetDebugWorldPos(LVector* outPos) const override;

    s32 IsDone(s32& inDoneMask);

    s32 CreateSound();
    s32 UpdateSound();
    s32 ReleaseSound();

    LVector pos = {};
    LVector debugSpawnPos = {};

    LVector* direction = nullptr;
    LVector* scale = nullptr;

    u32 createFlags = 0;

    s16 lifeCounter = 0;
    s16 frameDelay = 0;
    s16 frameCounter = 0;
    s16 noPool = 0;
    s16 doneBits = 0;
    s16 startDelay = 0;
    s16 startDelayCounter = 0;
    s16 debugSpawnCaptured = 0;

    u32 mentorHash = 0;
    PWEffect* mentor = nullptr;

    CParticleEffectSound* sound = nullptr;
    PWPathInfo* pathInfo = nullptr;
    ParticleSystemMgr* particleMgr = nullptr;
};

class FPWEffect : public PWEffect {
public:
    FPWEffect();
    ~FPWEffect() override;

    s32 Create() override;
    s32 Update() override;
    void Display(s32 inBlockNum) override;
    bool GetDebugWorldPos(LVector* outPos) const override;

    s32 Create2(const LVector* inPos, const LVector* inDirection, s32 followPos);
    s32 SetMentor();

    u32 followThingHash = 0;
    s16 emissionCount = 30;
    s16 mode = 0;

    Thing* mentorThing = nullptr;
    LVector* mentorPosRef = nullptr;
    LVector mentorOffset = {};

    s16 canDisplay = 0;
};

PWEffect::PWEffect() {
    MARKFUNCTION(0x8009B6EC);

    direction = nullptr;
    scale = nullptr;
    pathInfo = nullptr;
    particleMgr = nullptr;
    sound = nullptr;
}

PWEffect::~PWEffect() {
    MARKFUNCTION(0x8009B748);

    delete direction;
    direction = nullptr;

    delete scale;
    scale = nullptr;

    delete pathInfo;
    pathInfo = nullptr;

    ReleaseSound();

    delete particleMgr;
    particleMgr = nullptr;
}

s32 PWEffect::CreateSound() {
    MARKFUNCTION(0x8009B3C4);

    if (sound) {
        return 0;
    }

    u32 soundHash = 0;
    if (particleMgr) {
        soundHash = particleMgr->GetSystemHash();
    }

    CSound* created = nullptr;
    const s32 result = CSoundFactory::CreateObject(10000, &created, soundHash);
    if (result < 0 || !created) {
        return result;
    }

    sound = static_cast<CParticleEffectSound*>(created);
    return sound->Initialize(&pos);
}

s32 PWEffect::UpdateSound() {
    MARKFUNCTION(0x8009B424);

    if (!sound) {
        return 0;
    }

    return sound->StartAnimating();
}

s32 PWEffect::ReleaseSound() {
    MARKFUNCTION(0x8009B454);

    if (!sound) {
        return 0;
    }

    sound->Release();
    sound = nullptr;
    return 0;
}

s32 PWEffect::PutBackEffect() {
    MARKFUNCTION(0x8009B4A0);

    if (mentor) {
        mentor->PutBackEffect();
    }

    if (particleMgr) {
        particleMgr->PurgeParticles();
        particleMgr->ResetParticleDirection();
    }

    if (!noPool) {
        g_pWEffectPool.AddNode(g_pWEffectPool.tail, this);
    }

    return ReleaseSound();
}

bool PWEffect::IsDirectorOverlay() const {
    return (createFlags & 0x800u) != 0u;
}

s32 PWEffect::Create() {
    MARKFUNCTION(0x8009B618);

    if (noPool) {
        return 0;
    }

    frameCounter = 0;
    lifeCounter = 0;
    doneBits = 0;

    if ((createFlags & 0x110u) != 0u && direction && particleMgr) {
        particleMgr->SetParticleDirection(direction);
    }

    if (pathInfo) {
        pathInfo->Reset();
        const LVector* pathPos = pathInfo->GetPosition();
        if (pathPos) {
            pos = *pathPos;
        }

        blockNum = ResolveCollisionEffectBlockNumber(pos);
    }

    if (mentor) {
        mentor->Create();
    }

    debugSpawnPos = pos;
    debugSpawnCaptured = 1;

    CreateSound();
    return 1;
}

s32 PWEffect::Update() {
    MARKFUNCTION(0x8009B860);

    if (mentor) {
        mentor->Update();
    }

    const s16 prevCounter = frameCounter;
    frameCounter = static_cast<s16>(frameCounter + 1);
    if (prevCounter == frameDelay) {
        frameCounter = 0;

        if (pathInfo) {
            const s32 pathResult = pathInfo->Update();
            if (pathResult && pathInfo->state == 0) {
                RemoveEffect(this);
                g_pWEffectPool.AddNode(g_pWEffectPool.tail, this);
                return blockNum;
            }

            const LVector* pathPos = pathInfo->GetPosition();
            if (pathPos) {
                pos = *pathPos;
            }

            blockNum = ResolveCollisionEffectBlockNumber(pos);
        }

        if (particleMgr) {
            const LVector localOrigin = { 0, 0, 0 };
            particleMgr->CreateParticles(localOrigin, nullptr);
            particleMgr->Update();
        }
    }

    return UpdateSound();
}

void PWEffect::Display(s32 inBlockNum) {
    MARKFUNCTION(0x8009B994);

    if (blockNum != inBlockNum) {
        return;
    }

    if (mentor) {
        mentor->Display(inBlockNum);
    }

    if (!particleMgr || !p3d::context) {
        return;
    }

    particleMgr->SetDisplayOffset(nullptr);

    const LVector& renderPos = (kDebugFreezePWParticleRenderAtSpawn && debugSpawnCaptured != 0) ? debugSpawnPos : pos;
    const LVector* renderScale = nullptr;
    if (!kDebugFreezePWParticleRenderAtSpawn && (createFlags & 4u) != 0u) {
        renderScale = scale;
    }

    if (kDebugTracePWTransforms) {
        static s32 sTraceBudget = 120;
        static s32 sTraceDecimator = 0;
        if (sTraceBudget > 0 && ((sTraceDecimator++ & 0x1F) == 0)) {
            LOG("[PWDbg] type=PW hash=0x%08X block=%d freeze=%d cflags=0x%08X pos=(%d,%d,%d) scale=(%d,%d,%d)",
                nameCRC,
                blockNum,
                kDebugFreezePWParticleRenderAtSpawn ? 1 : 0,
                createFlags,
                renderPos.x,
                renderPos.y,
                renderPos.z,
                renderScale ? renderScale->x : 0,
                renderScale ? renderScale->y : 0,
                renderScale ? renderScale->z : 0);
            sTraceBudget--;
        }
    }

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    const Mat4 effectWorld = BuildEffectWorldMatrix(renderPos, renderScale);
    p3d::context->SetWorldMatrix(savedWorld * effectWorld);
    particleMgr->Display();
    p3d::context->SetWorldMatrix(savedWorld);
}

bool PWEffect::GetDebugWorldPos(LVector* outPos) const {
    if (!outPos) {
        return false;
    }

    *outPos = pos;
    return true;
}

s32 PWEffect::IsDone(s32& inDoneMask) {
    MARKFUNCTION(0x8009BEE0);

    inDoneMask &= doneBits;
    if (mentor) {
        mentor->IsDone(inDoneMask);
    }

    return inDoneMask;
}

FPWEffect::FPWEffect() {
    MARKFUNCTION(0x8009BA80);

    mentorThing = nullptr;
    mentorPosRef = nullptr;
    mentorOffset = {};

    mode = 0;
    emissionCount = 30;
    canDisplay = 0;
}

FPWEffect::~FPWEffect() {
    MARKFUNCTION(0x8009C52C);
}

s32 FPWEffect::SetMentor() {
    MARKFUNCTION(0x8009BF24);

    if (followThingHash != 0 && g_ai) {
        ccNode* node = g_ai->moveList.FindNodeCRC(followThingHash, nullptr);
        if (node) {
            mentorThing = static_cast<Thing*>(node);
            const LVector* initialPos = mentorThing->GetInitialPos();
            if (!initialPos) {
                initialPos = &mentorThing->pos;
            }

            mentorOffset.x = pos.x - initialPos->x;
            mentorOffset.y = pos.y - initialPos->y;
            mentorOffset.z = pos.z - initialPos->z;

            mentorPosRef = &mentorThing->pos;

            canDisplay = 0;
            doneBits = 0;
            return 1;
        }
    }

    mentorThing = nullptr;
    mentorPosRef = nullptr;
    canDisplay = 1;
    return 1;
}

s32 FPWEffect::Create() {
    MARKFUNCTION(0x8009BB90);

    if (noPool) {
        return 0;
    }

    SetMentor();
    if (!mentorThing) {
        return 0;
    }

    lifeCounter = emissionCount;
    frameCounter = 0;
    doneBits = 0;
    startDelayCounter = startDelay;

    if ((createFlags & 0x110u) != 0u && direction && particleMgr) {
        particleMgr->SetParticleDirection(direction);
    }

    if (mentor) {
        mentor->Create();
    }

    debugSpawnPos = pos;
    debugSpawnCaptured = 1;

    CreateSound();
    return 1;
}

s32 FPWEffect::Create2(const LVector* inPos, const LVector* inDirection, s32 followPos) {
    MARKFUNCTION(0x8009BD60);

    if (inPos) {
        pos = *inPos;
    }
    else {
        pos = { 0, 0, 0 };
    }

    blockNum = ResolveCollisionEffectBlockNumber(pos);

    if (inDirection && direction) {
        *direction = *inDirection;
    }

    lifeCounter = emissionCount;
    frameCounter = 0;
    doneBits = 0;
    startDelayCounter = startDelay;
    canDisplay = 0;
    mentorThing = nullptr;

    if (followPos) {
        mentorPosRef = const_cast<LVector*>(inPos);
    }
    else {
        mentorPosRef = nullptr;
    }

    debugSpawnPos = pos;
    debugSpawnCaptured = 1;

    if ((createFlags & 0x110u) != 0u && direction && particleMgr) {
        particleMgr->SetParticleDirection(direction);
    }

    if (mentor && mentor->effectType == 3) {
        FPWEffect* mentorEffect = static_cast<FPWEffect*>(mentor);

        LVector mentorPos = pos;
        mentorPos.x += mentorEffect->mentorOffset.x - mentorOffset.x;
        mentorPos.y += mentorEffect->mentorOffset.y - mentorOffset.y;
        mentorPos.z += mentorEffect->mentorOffset.z - mentorOffset.z;

        mentorEffect->emissionCount = emissionCount;
        mentorEffect->Create2(&mentorPos, inDirection, 0);
    }

    return CreateSound();
}

s32 FPWEffect::Update() {
    MARKFUNCTION(0x8009C004);

    if (mentor) {
        mentor->Update();
    }

    if (startDelayCounter > 0) {
        startDelayCounter = static_cast<s16>(startDelayCounter - 1);
        return startDelayCounter;
    }

    const s16 prevCounter = frameCounter;
    frameCounter = static_cast<s16>(frameCounter + 1);
    if (prevCounter != frameDelay) {
        if (mentorThing) {
            blockNum = mentorThing->blockNum;
        }
        return blockNum;
    }

    frameCounter = 0;

    const LVector localOrigin = { 0, 0, 0 };

    if (mentorThing) {
        // Avoid RTTI dynamic_cast on potentially stale runtime memory.
        // PSX checks the obstacle alive byte at +115 for mode 1/2.
        // Read raw byte to match that behavior without RTTI.
        const bool mentorAlive = (*(reinterpret_cast<const u8*>(mentorThing) + 115) != 0u);

        Platform* platform = nullptr;
        if (mentorThing->thingType == AITypes::TT_PLATFORM) {
            platform = static_cast<Platform*>(mentorThing);
        }

        switch (mode) {
            case 0:
                canDisplay = 1;
                break;
            case 1:
                if (!mentorAlive) {
                    canDisplay = 1;
                    mode = 99;
                }
                break;
            case 2:
                canDisplay = 1;
                if (!mentorAlive) {
                    mode = 99;
                }
                break;
            case 3:
                if (platform && ((platform->platformFlags >> 1) & 1) != 0) {
                    canDisplay = 1;
                    mode = 99;
                }
                break;
            case 4:
                if (platform && platform->isActive && emissionCount >= platform->deathCountdown) {
                    canDisplay = 1;
                    mode = 99;
                }
                break;
            case 99:
                lifeCounter = static_cast<s16>(lifeCounter - 1);
                if (lifeCounter < 0 && particleMgr && !particleMgr->ActiveParticles()) {
                    particleMgr->PurgeParticles();
                    particleMgr->ResetParticleDirection();

                    RemoveEffect(this);
                    g_pWEffectPool.AddNode(g_pWEffectPool.tail, this);

                    if (sound) {
                        sound->StopAnimating();
                    }

                    blockNum = -1;
                    return -1;
                }
                break;
            default:
                break;
        }

        if (!canDisplay) {
            blockNum = mentorThing->blockNum;
            return blockNum;
        }

        if (mentorPosRef) {
            pos.x = mentorOffset.x + mentorPosRef->x;
            pos.y = mentorOffset.y + mentorPosRef->y;
            pos.z = mentorOffset.z + mentorPosRef->z;
        }

        if (lifeCounter > 0 && particleMgr) {
            particleMgr->CreateParticles(localOrigin, nullptr);
        }

        if (particleMgr) {
            particleMgr->Update();
        }

        UpdateSound();

        blockNum = mentorThing->blockNum;
        return blockNum;
    }

    lifeCounter = static_cast<s16>(lifeCounter - 1);
    canDisplay = 1;

    if (lifeCounter >= 0 && particleMgr) {
        particleMgr->CreateParticles(localOrigin, nullptr);
    }

    if (lifeCounter < 0 && particleMgr && !particleMgr->ActiveParticles()) {
        if (noPool) {
            doneBits = 1;
            particleMgr->PurgeParticles();
            particleMgr->ResetParticleDirection();
            return 0;
        }

        s32 doneMask = 1;
        if (mentor) {
            mentor->IsDone(doneMask);
        }

        if (doneMask) {
            particleMgr->PurgeParticles();
            particleMgr->ResetParticleDirection();

            RemoveEffect(this);
            g_pWEffectPool.AddNode(g_pWEffectPool.tail, this);

            if (sound) {
                sound->StopAnimating();
            }

            return blockNum;
        }

        return doneMask;
    }

    if (particleMgr) {
        particleMgr->Update();
    }

    UpdateSound();

    return blockNum;
}

void FPWEffect::Display(s32 inBlockNum) {
    MARKFUNCTION(0x8009C3D8);

    if (!canDisplay || blockNum != inBlockNum) {
        return;
    }

    if (mentor) {
        mentor->Display(inBlockNum);
    }

    if (!particleMgr || !p3d::context) {
        return;
    }

    const LVector* displayPos = &pos;
    if (!kDebugFreezePWParticleRenderAtSpawn && !mentorThing && mentorPosRef) {
        displayPos = mentorPosRef;
    }

    if (kDebugFreezePWParticleRenderAtSpawn && debugSpawnCaptured != 0) {
        displayPos = &debugSpawnPos;
    }

    const LVector* renderScale = nullptr;
    if (!kDebugFreezePWParticleRenderAtSpawn && (createFlags & 4u) != 0u) {
        renderScale = scale;
    }

    if (kDebugTracePWTransforms) {
        static s32 sTraceBudget = 120;
        static s32 sTraceDecimator = 0;
        if (sTraceBudget > 0 && ((sTraceDecimator++ & 0x1F) == 0)) {
            LOG("[PWDbg] type=FPW hash=0x%08X block=%d freeze=%d cflags=0x%08X pos=(%d,%d,%d) scale=(%d,%d,%d) mentor=%p mpos=%p",
                nameCRC,
                blockNum,
                kDebugFreezePWParticleRenderAtSpawn ? 1 : 0,
                createFlags,
                displayPos ? displayPos->x : 0,
                displayPos ? displayPos->y : 0,
                displayPos ? displayPos->z : 0,
                renderScale ? renderScale->x : 0,
                renderScale ? renderScale->y : 0,
                renderScale ? renderScale->z : 0,
                mentorThing,
                mentorPosRef);
            sTraceBudget--;
        }
    }

    particleMgr->SetDisplayOffset(nullptr);

    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    const Mat4 effectWorld = BuildEffectWorldMatrix(*displayPos, renderScale);
    p3d::context->SetWorldMatrix(savedWorld * effectWorld);
    particleMgr->Display();
    p3d::context->SetWorldMatrix(savedWorld);
}

bool FPWEffect::GetDebugWorldPos(LVector* outPos) const {
    if (!outPos) {
        return false;
    }

    if (!mentorThing && mentorPosRef) {
        *outPos = *mentorPosRef;
        return true;
    }

    *outPos = pos;
    return true;
}

static void PurgePool() {
    MARKFUNCTION(0x8009B7FC);

    while (ccMinNode* node = g_pWEffectPool.RemHead()) {
        delete static_cast<Effects*>(static_cast<ccNode*>(node));
    }
}

static void AddFragTemps(u32 hash, u32 particleHash0, u32 particleHash1) {
    MARKFUNCTION(0x8009B238);

    ParticleSystem* ps0 = ParticleSystem_Find(particleHash0);
    ParticleSystem* ps1 = ParticleSystem_Find(particleHash1);
    if (!ps0 || !ps1) {
        return;
    }

    FPWEffect* lead = new FPWEffect();
    FPWEffect* tail = new FPWEffect();
    if (!lead || !tail) {
        delete lead;
        delete tail;
        return;
    }

    lead->effectType = 3;
    lead->mode = 0;
    lead->mentorOffset = {};
    lead->emissionCount = 1;
    lead->createFlags = 0;
    lead->particleMgr = new ParticleSystemMgr(ps0);
    lead->nameCRC = hash;
    lead->blockNum = 0;
    lead->mentorHash = 210670097;
    lead->mentor = tail;
    lead->noPool = 0;

    tail->effectType = 3;
    tail->mode = 0;
    tail->mentorOffset = {};
    tail->emissionCount = 1;
    tail->nameCRC = 210670097;
    tail->blockNum = 0;
    tail->createFlags = 0;
    tail->particleMgr = new ParticleSystemMgr(ps1);
    tail->mentorHash = 0;
    tail->mentor = nullptr;
    tail->noPool = 1;

    g_pWEffectPool.AddNode(g_pWEffectPool.tail, lead);
    g_pWEffectPool.AddNode(g_pWEffectPool.tail, tail);
}

void PWEffect_InitWorldEffects(DBPoint* firstPoint) {
    MARKFUNCTION(0x8009AC58);

    PurgePool();

    s32 mentorCount = 0;

    for (DBPoint* point = firstPoint; point; point = static_cast<DBPoint*>(point->next)) {
        u32 value = 0;
        if (!point->FindAttribValue(1, &value) || value != 100) {
            continue;
        }

        if (!point->FindAttribValue(2, &value)) {
            continue;
        }

        const u32 subType = value;
        if (subType == 10 || subType == 40 || subType == 50) {
            continue;
        }

        u32 particleHash = 0;
        if (!point->FindAttribValue(5, &particleHash)) {
            continue;
        }

        ParticleSystem* particleSystem = ParticleSystem_Find(particleHash);
        if (!particleSystem) {
            continue;
        }

        PWEffect* effect = nullptr;
        if (subType == 20) {
            effect = new PWEffect();
            if (effect) {
                effect->effectType = 2;
            }
        }
        else if (subType == 30) {
            FPWEffect* fpw = new FPWEffect();
            if (fpw) {
                fpw->effectType = 3;
                fpw->mode = 0;

                if (point->FindAttribValue(53, &value)) {
                    fpw->mode = static_cast<s16>(value);
                }

                if (point->FindAttribValue(50, &value)) {
                    fpw->followThingHash = value;
                }
                else {
                    fpw->mentorOffset = point->pos;
                }

                if (point->FindAttribValue(55, &value)) {
                    fpw->emissionCount = static_cast<s16>(value);
                }
                else {
                    fpw->emissionCount = 30;
                }
            }

            effect = fpw;
        }

        if (!effect) {
            continue;
        }

        effect->pos = point->pos;
        effect->particleMgr = new ParticleSystemMgr(particleSystem);

        if (point->field40 != 0 || point->field44 != 0 || point->field48 != 0) {
            effect->direction = new LVector();
            if (effect->direction) {
                effect->direction->x = point->field40;
                effect->direction->y = point->field44;
                effect->direction->z = point->field48;
                effect->createFlags = 272;
            }
        }

        if (point->FindAttribValue(41, &value)) {
            if (!effect->scale) {
                effect->scale = new LVector();
                if (effect->scale) {
                    effect->scale->x = 0;
                    effect->scale->y = 0;
                    effect->scale->z = 0;
                }
            }

            if (effect->scale) {
                effect->scale->x = rmDiv16i(static_cast<s32>(value << 16), 6553600);
                effect->createFlags |= 4u;
            }
        }

        if (point->FindAttribValue(42, &value)) {
            if (!effect->scale) {
                effect->scale = new LVector();
                if (effect->scale) {
                    effect->scale->x = 0;
                    effect->scale->y = 0;
                    effect->scale->z = 0;
                }
            }

            if (effect->scale) {
                effect->scale->y = rmDiv16i(static_cast<s32>(value << 16), 6553600);
                effect->createFlags |= 4u;
            }
        }

        if (point->FindAttribValue(43, &value)) {
            if (!effect->scale) {
                effect->scale = new LVector();
                if (effect->scale) {
                    effect->scale->x = 0;
                    effect->scale->y = 0;
                    effect->scale->z = 0;
                }
            }

            if (effect->scale) {
                effect->scale->z = rmDiv16i(static_cast<s32>(value << 16), 6553600);
                effect->createFlags |= 4u;
            }
        }

        if (point->FindAttribValue(54, &value)) {
            effect->frameDelay = static_cast<s16>(value);
        }
        else {
            effect->frameDelay = 0;
        }

        if (point->FindAttribValue(28, &value)) {
            if (static_cast<s32>(value) <= 0) {
                effect->createFlags |= 0x1000000u;
            }
            else {
                effect->createFlags |= 0x800u;
            }
        }

        if (point->FindAttribValue(44, &value)) {
            mentorCount++;
            effect->mentorHash = value;
        }

        if (point->FindAttribValue(15, &value)) {
            effect->blockNum = static_cast<s32>(value);
        }
        else {
            effect->blockNum = -1;
        }

        if (point->FindAttribValue(57, &value)) {
            effect->startDelay = static_cast<s16>(value);
        }
        else {
            effect->startDelay = 0;
        }

        if (point->FindAttribValue(4, &value) && g_database) {
            DBPath* path = g_database->FindPath(value);
            if (path) {
                PWPathInfo* info = new PWPathInfo();
                if (info && info->Init(path, point)) {
                    effect->pathInfo = info;
                }
                else {
                    delete info;
                }
            }
        }

        effect->nameCRC = point->nameCRC;
        g_pWEffectPool.AddNode(g_pWEffectPool.tail, effect);
    }

    if (mentorCount > 0 && g_pWEffectPool.head) {
        for (ccMinNode* node = g_pWEffectPool.head; node; node = node->next) {
            PWEffect* effect = static_cast<PWEffect*>(static_cast<ccNode*>(node));
            if (effect->mentorHash == 0) {
                continue;
            }

            ccNode* mentorNode = g_pWEffectPool.FindNodeCRC(effect->mentorHash, nullptr);
            if (!mentorNode) {
                continue;
            }

            effect->mentor = static_cast<PWEffect*>(mentorNode);
            effect->mentor->noPool = 1;
        }
    }

    AddFragTemps(106729104, 5533331, 88651351);
    AddFragTemps(106729104, 5533331, 88651351);
    AddFragTemps(97053010, 5533331, 76244371);
    AddFragTemps(97053010, 5533331, 76244371);
}

void PWEffect_Unload() {
    MARKFUNCTION(0x8009B534);
    PurgePool();
}

void PWEffect_CreateForBlock(s32 blockNum) {
    MARKFUNCTION(0x8009B554);

    for (ccMinNode* node = g_pWEffectPool.head; node;) {
        ccMinNode* next = node->next;
        PWEffect* effect = static_cast<PWEffect*>(static_cast<ccNode*>(node));

        if (effect->effectType == 2 && effect->blockNum == blockNum) {
            if (effect->Create()) {
                g_pWEffectPool.RemNode(effect);
                AddEffect(effect, 0);
            }
        }

        node = next;
    }
}

void FPWEffect_CreateForBlock(s32 blockNum) {
    MARKFUNCTION(0x8009BACC);

    for (ccMinNode* node = g_pWEffectPool.head; node;) {
        ccMinNode* next = node->next;
        PWEffect* effect = static_cast<PWEffect*>(static_cast<ccNode*>(node));

        if (effect->effectType == 3 && effect->blockNum == blockNum) {
            if (effect->Create()) {
                g_pWEffectPool.RemNode(effect);
                AddEffect(effect, 0);
            }
        }

        node = next;
    }
}

s32 FPWEffect_Create2(u32 effectHash,
                      const LVector* pos,
                      const LVector* direction,
                      s32 lifeFrames,
                      s32 followPos)
{
    MARKFUNCTION(0x8009BC38);

    if (!pos || !g_pWEffectPool.head) {
        return 0;
    }

    if (ResolveCollisionEffectBlockNumber(*pos) == -1) {
        return 0;
    }

    for (ccMinNode* node = g_pWEffectPool.head; node; node = node->next) {
        PWEffect* base = static_cast<PWEffect*>(static_cast<ccNode*>(node));
        if (base->noPool || base->nameCRC != effectHash) {
            continue;
        }

        FPWEffect* effect = static_cast<FPWEffect*>(base);

        if (effectHash != 106729104 && effectHash != 97053010) {
            if (lifeFrames == 0) {
                effect->emissionCount = 30;
            }
            else {
                effect->emissionCount = static_cast<s16>(lifeFrames);
            }
        }

        effect->Create2(pos, direction, followPos);
        g_pWEffectPool.RemNode(effect);
        AddEffect(effect, 0);
        return 1;
    }

    return 0;
}

s32 FPWEffect_DebugSpawnParticle(u32 particleHash,
                                 const LVector* pos,
                                 const LVector* direction,
                                 s32 lifeFrames)
{
    if (!pos) {
        return -1;
    }

    const s32 collisionBlockNum = ResolveCollisionEffectBlockNumber(*pos);

    if (collisionBlockNum == -1) {
        return -3;
    }

    ParticleSystem* particleSystem = ParticleSystem_Find(particleHash);
    if (!particleSystem) {
        return -2;
    }

    FPWEffect* effect = new FPWEffect();
    if (!effect) {
        return -4;
    }

    effect->effectType = 3;
    effect->nameCRC = particleHash;
    effect->emissionCount = (lifeFrames <= 0) ? 30 : static_cast<s16>(lifeFrames);
    effect->blockNum = collisionBlockNum;
    effect->particleMgr = new ParticleSystemMgr(particleSystem);

    if (!effect->particleMgr) {
        delete effect;
        return -5;
    }

    if (direction) {
        effect->direction = new LVector();
        if (!effect->direction) {
            delete effect;
            return -6;
        }

        *effect->direction = *direction;
        effect->createFlags |= 0x110u;
    }

    effect->Create2(pos, direction, 0);

    // Debug visibility: draw in the player's current render block when available.
    if (Player::s_player && Player::s_player->blockNum >= 0) {
        effect->blockNum = Player::s_player->blockNum;
    }

    AddEffect(effect, 0);
    return 1;
}

u32 PWEffect_DebugGetParticleSystemHash(const Effects* effect) {
    if (!effect) {
        return 0;
    }

    const s32 effectType = effect->effectType;
    if (effectType != 2 && effectType != 3) {
        return 0;
    }

    const PWEffect* pwEffect = static_cast<const PWEffect*>(effect);
    if (!pwEffect->particleMgr) {
        return 0;
    }

    return pwEffect->particleMgr->GetSystemHash();
}

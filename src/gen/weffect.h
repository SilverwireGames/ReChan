#pragma once

#include "gen/effects.h"
#include "core.h"
#include "config.h"

struct DBPoint;
struct DBPath;
struct OriginalGeo;
struct OriginalETree;
struct OriginalSTree;
struct TransformAnim;
struct SkinData;
struct FrameListAnim;
struct CompositeAnimData;
struct SequenceAnim;
struct MiscAnimNode;
struct UVListAnim;
struct VizAnim;
struct ScaleData;
struct ComEffectScaleBinding;
struct CBVPrimData;
class PaletteData;
class CWorldEffectSound;
class Thing;
struct CBVParamAnimData;
struct ClutAnimData;

class Model;
class EModel;
class SModel;
class PathInfo;

struct WEffectUVData {
    // PSX layout: u0,v0,u1,v1,u2,v2,u3,v3
    u16 baseUVWords[8] = {};
    u16 stepU = 0;
    u16 stepV = 0;
    u16 accumU = 0;
    u16 accumV = 0;
    u16 maskU = 0;
    u16 maskV = 0;
};

class ComEffect {
public:
    ComEffect();
    ~ComEffect();

    bool LoadETree(s32 resourceHash, s32 miscAnimHash);
    bool LoadSTree(s32 resourceHash, s32 miscAnimHash);
    bool LoadGeo(s32 resourceHash, s32 miscAnimHash);

    void SetFrame(s32 frame);
    void SetFrameReal(s32 frameReal16);
    bool EndOfFrame(s32 frame) const;
    bool HasAnimation() const;
    bool PointInView(const LVector& pos, s32 radius) const;
    OriginalGeo* SetUpFirstGeo();

    void Render(const Mat4& worldMatrix, u32 flags);
    void Render(const LVector& pos, const LVector* scale, const LVector* rotation, u32 flags);
    void InitFastRender(OriginalGeo* geo);
    void DoFastRender();

    void SetUpUVlists();
    void AddUV(const WEffectUVData* uvData, u16 uOffset, u16 vOffset);

    void SetUpVertexlists();
    void SetVertexInfo(s16 frame, s16 speed);

    u32 GetGeoCount() const;
    OriginalGeo* GetGeo() const;
    OriginalGeo* GetGeo(s32 geoIndex) const;
    bool ResolveGeoByIndex(u32 geoIndex, OriginalGeo** outGeo, Mat4* outLocalMatrix);
    bool RenderGeoByIndex(u32 geoIndex, const Mat4& worldMatrix, u32 flags);
    u32* GetGeoFastWord1Slot(s32 geoIndex);
    bool FastRenderReady() const;

    u32 GetClut(s32 mode);
    void SetZFar();
    MiscAnimNode* GetMiscAnimNode() const { return miscAnimNode; }
    s32 GetFrameCount() const { return frameCount; }

    u32 resourceHash = 0;
    u32 miscAnimHash = 0;

private:
    struct VertexAnimInfo {
        u16 sourceIndex = 0;
        s16 baseY = 0;
        s16 deltaY = 0;
    };

    bool ApplyVizAnimFrame(VizAnim* vizAnimData, s32 frame);
    bool ApplyCBVParamAnimFrame(CBVParamAnimData* cbvParamAnimData, s32 frame);
    bool ApplyClutAnimFrame(ClutAnimData* clutAnimData, s32 frame);
    void ApplyUVListAnimFrame(const UVListAnim* uvAnim, s32 frame);
    bool ApplyMiscAnimFrame(MiscAnimNode* node, s32 frame, bool updateJoints);
    bool IsFastDrawGeoCurrent() const;
    bool IsUVGeoCacheLive() const;
    void FastZSortDisplayGCT3(u32 primCount);
    void FastZSortDisplayGCT4(u32 primCount);

    Model* model = nullptr;
    EModel* eModel = nullptr;
    SModel* sModel = nullptr;

    // PSX: ComEffect+36 = shared tAnimation* from GetMiscAnim (change-detection only).
    //      ComEffect+40 = per-instance tFlip* from MakePuppet (owned, ownsSubs=false).
    MiscAnimNode* sharedMiscAnimNode = nullptr;
    MiscAnimNode* ownedMiscAnimNode = nullptr;
    MiscAnimNode* miscAnimNode = nullptr;
    TransformAnim* miscAnim = nullptr;
    TransformAnim* boundTransformAnim = nullptr;
    FrameListAnim* frameListAnim = nullptr;
    CompositeAnimData* compositeAnim = nullptr;
    SequenceAnim* sequenceAnim = nullptr;
    VizAnim* vizAnim = nullptr;
    ScaleData* scaleData = nullptr;
    ComEffectScaleBinding* scaleBindings = nullptr;
    u16 scaleBindingCount = 0;
    u16 scaleBindingCapacity = 0;
    s32 frameCount = 0;
    s32 currentFrame = 0;
    s32 currentFrameReal16 = 0;

    u16* uvBaseWords = nullptr;
    u32 uvPrimCount = 0;
    u16** uvGeoBaseWords = nullptr;
    OriginalGeo** uvGeoList = nullptr;
    // Identity of each cached geo at capture time: owning ETree part slot
    // (0xFFFF for a plain Geo drawable) and the dynamicPrimCount used to size
    // uvGeoBaseWords. AddUV revalidates both before writing.
    u16* uvGeoPartIndices = nullptr;
    u32* uvGeoPrimCounts = nullptr;
    u16 uvGeoCount = 0;

    VertexAnimInfo* vertexAnimInfos = nullptr;
    u16 vertexAnimInfoCount = 0;

    bool warnedUVUnsupported = false;
    bool warnedVertexUnsupported = false;
    bool warnedSequenceUnsupported = false;

    static constexpr u32 kFastWordInactive = 0xFFFFFFFFu;
    static constexpr u32 kMaxFastDrawEntries = 32;
    struct FastDrawEntry {
        Mat4 worldMatrix = Mat4();
        // PSX fast queue entry[0]: tex lane (CBA lo16, TPAGE hi16).
        u32 word0 = 0;
        // PSX fast queue entry[1]: +68 colour lane slot0 override.
        u32 word1 = kFastWordInactive;
    };

    FastDrawEntry fastDrawEntries[kMaxFastDrawEntries];
    u32 fastDrawCount = 0;
    s32 fastDrawGeoIndex = -1;
    OriginalGeo* fastDrawGeo = nullptr;
    OriginalGeo* currentGeo = nullptr;
    s32 currentGeoIndex = -1;
    u32 geoWord0Slot = kFastWordInactive;
};

class WEffect : public Effects {
public:
    WEffect();
    ~WEffect() override;

    s32 Create() override;
    s32 Update() override;
    s32 IsDone(s32& doneMask);
    void EnablePath(s32 enable);
    void Display(s32 blockNum) override;
    s32 PutBackEffect() override;
    void NISRemoveEffect();
    bool IsDirectorOverlay() const override;
    bool GetDebugWorldPos(LVector* outPos) const override;

    s32 CreateSound(const LVector* posOverride);
    s32 UpdateSound();
    s32 ReleaseSound();
    PaletteData* SetupPaletteData(u32 paletteHash, u32 clutMode, u32 flags);
    static WEffect* Find(u32 effectHash);


    LVector pos = {};
    LVector spawnPos = {};
#if HIGH_FPS_PLAY_PRESENTATION
    LVector prevPos = {};
#endif

    LVector rotation = {};
    LVector scale = { 0x10000, 0x10000, 0x10000 };
    bool hasScale = false;

    u32 renderFlags = 0;

    s32 frame = 0;
    s16 frameDelay = 0;
    s16 frameCounter = 0;
    s16 active = 1;

    s16 clipDistance = 384;

    s16 vertexEnabled = 0;
    s16 vertexFrame = 0;
    s16 vertexUseGlobalFrame = 0;
    s16 vertexSpeed = 0;

    u32 mentorHash = 0;
    WEffect* mentor = nullptr;
    s16 isMentorTarget = 0;
    s16 doneFlag = 0;

    u32 triggerFWHash = 0;

    WEffectUVData* uvData = nullptr;
    PaletteData* paletteData = nullptr;

    ComEffect* comEffect = nullptr;
    CWorldEffectSound* sound = nullptr;
    PathInfo* pathInfo = nullptr;
};

class SpotLight : public WEffect {
public:
    SpotLight();
    ~SpotLight() override;

    s32 Create() override;
    s32 Update() override;
    void Display(s32 blockNum) override;
    s32 PutBackEffect() override;

    void SetUp(u32 effectHash, u32 range, u32 linkedHash);

private:
    LVector basePos = {};
    WEffect* linkedEffect = nullptr;
    u32 linkedEffectHash = 0;
    u32 setupValue = 0;
    u32 followRange = 0;
    s32 visible = 1;
    s32 zOffset = 0;
};

class FWEffect : public WEffect {
public:
    FWEffect();
    ~FWEffect() override;

    static s32 Create(s32 blockNum);
    static s32 Create2(u32 effectHash,
                       const LVector* posOverride,
                       const LVector* scaleOverride,
                       const LVector* rotationOverride,
                       s32 flags);

    static FWEffect* Find(u32 effectHash);

    s32 Create() override;
    s32 Create2(const LVector* posOverride,
                const LVector* scaleOverride,
                const LVector* rotationOverride,
                s32 flags);

    s32 SetMentor();
    void SetScaleRoll(s32 scaleValue, s32 rollValue);
    s32 Continue();
    s32 Update() override;
    void Display(s32 blockNum) override;

    s32* scaleRoll = nullptr;
    u32 followHash = 0;
    u16 createFlags = 0;
    u16 templateCreateFlags = 0;
    s16 pathMode = 5;
    s16 canDisplay = 1;
    s16 modeThreshold = 30;
    s16 createMode = 0;
    s16 oscillationMode = 0;
    s16 startDelay = 0;
    s16 startDelayCounter = 0;
    s16 pingPongReverse = 0;
    s16 activatedOnce = 0;
    u16 overrideFlags = 0;

    Thing* mentorLink = nullptr;
    LVector* mentorPosRef = nullptr;
    LVector mentorOffset = {};
#if HIGH_FPS_PLAY_PRESENTATION
    LVector prevMentorPos = {};
    LVector curMentorPos = {};
#endif

    LVector overridePos = {};
    LVector overrideScale = { 0x10000, 0x10000, 0x10000 };
    LVector overrideRotation = {};
};

class LensFlare : public FWEffect {
public:
    LensFlare();
    ~LensFlare() override;

    s32 Create() override;
    s32 InitLensFlare(s32 mode, DBPath* path);
    s32 Update() override;
    void Display(s32 blockNum) override;
    bool IsDirectorOverlay() const override;
    bool GetDebugWorldPos(LVector* outPos) const override;

private:
    u32 BigScreenGlow();
    s32 ComputeTracking(LVector& from, LVector& to);

    struct PathNode {
        LVector pos = {};
        u16 scale = 1024;
        u16 clamp = 0;
    };

    ComEffect* flareComEffects = nullptr;
    u32* clampColourEntry = nullptr;

    s32 frameScalePrimary = 0;
    s32 frameScaleSecondary = 0;
    s32 streakPitch = 0;
    s32 streakYaw = 5461;

    s32 flareVisible = 0;
    s32 bigScreenGlow = 0;
    s32 hasSecondaryGlow = 0;
    u32 trackingFlags = 0x800;

    s32 pathNodeIndex = 0;
    LVector flarePos = {};
    LVector flareTrailDir = {};
    u32 colourPrimary = 0;
    u32 colourSecondary = 0;

    WEffect* targetEffect = nullptr;
    LVector targetOffset = {};
    PathNode* pathNodes = nullptr;
    s32 pathNodeCount = 0;
};

class CBVEffect : public Effects {
public:
    CBVEffect();
    ~CBVEffect() override;

    s32 Create() override;
    s32 Update() override;
    void Display(s32 blockNum) override;
    s32 PutBackEffect() override;

    s32 Create2();

    CBVPrimData* cbvData = nullptr;
    u32 hash = 0;
    s32 restartDelay = 0;
    s32 holdDelay = 0;
    s32 frameCount = 1;
    s32 frameCountMin = 0;
    u32 fixedColour = 0;
    s32 mode = 1;
    s32 disabledOnCreate = 0;
};

// PSX: Create2__9CBVEffectUli (WEFFECT.CPP:1914, 0x8008CD74)
s32 CBVEffect_CreateForHash(u32 effectHash);

// PSX: Load__7WEffectR10tReadChunkPPv (WEFFECT.CPP:100, 0x8008A708)
void WEffect_LoadChunk(const u8* body, u32 bodySize);

// PSX: Unload__7WEffect (WEFFECT.CPP:156, 0x8008A848)
void WEffect_Unload();

// PSX: InitWorldEffects__7WEffectP7DBPoint (WEFFECT.CPP:245, 0x8008A9C0)
void WEffect_InitWorldEffects(DBPoint* firstPoint);

// PSX: Create__7WEffecti (WEFFECT.CPP:857, 0x8008B774)
void WEffect_CreateForBlock(s32 blockNum);

// PSX: Purge__7WEffect (WEFFECT.CPP:950, 0x8008B924)
void WEffect_PurgePool();

// PSX world wrappers (WORLD.CPP:1471/1520)
void WEffect_PopulateWEffects();
void WEffect_UnPopulateWEffects(s32 blockNum);

// Sets the per-block seam offset applied to ComEffect world-space render positions
// (pos-based Render path only). Call symmetrically around Effects_DrawEffects so
// direct-position effects depth-align with seam-shifted block geometry.
void ComEffect_SetSeamOffset(s32 x, s32 y, s32 z);
void ComEffect_ClearLateRenderQueue();
void ComEffect_BeginLateRenderQueue(s32 blockNum);
void ComEffect_EndLateRenderQueue();
void ComEffect_FlushLateRenderQueue(s32 blockNum);

// Debug helpers: expose resolved ComEffect identity for active world effects.
u32 WEffect_DebugGetComEffectResourceHash(const Effects* effect);
u32 WEffect_DebugGetComEffectGeoHash(const Effects* effect);

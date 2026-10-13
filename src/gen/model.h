#pragma once
#include "core.h"
#include "p3d/p3dmath.h"
#include "gen/cclist.h"
#include "gen/shadows.h"
#include "gen/config.h"
#if REAL_TEXTURE_RENDERING
#include "extra/realtexture.h"
#include <vector>
#endif

// Original hierarchy (data containers stored in LevelManager):
//   OriginalBasic -> OriginalTree -> OriginalSTree / OriginalGeo / OriginalETree
//
// Per-entity model instances:
//   Model (96 bytes) -> SModel -> HumanoidModel (136 bytes) -> PlayerModel (136 bytes)
//   Model (96 bytes) -> GModel (96 bytes)
//
// Drawable wrappers:
//   DrawableTree -> DrawableSTree / DrawableGeo / DrawableETree

class pddiPrimBuffer;
class Thing;
class AnimStructure;
class SModel;
class Model;
class AmbientLight;
class HardwareLight;
struct MiscAnimNode;
struct ClutAnimData;
struct AnimationMatrices;
struct BlendPoseState;
struct STreeJoint;
struct STreeData;
struct TransformAnim;
struct tPrimGeom;

// PSX: MODEL.CPP helpers 0x80071AF4 / 0x80071BCC
void MakeBillboardMatrix(const LVector& pos, Mat4& out, s32 lockY);
void MakeBillboardMatrixFlip(const LVector& pos, Mat4& out, s32 lockY);

using STreeXformVertsCallback = u32 (*)(tPrimGeom*, STreeJoint*, u32*, u16*);
using STreeFixUpPolysCallback = u8* (*)(tPrimGeom*, void*, u32, u32);

// Pre-parsed skinning data for CPU vertex skinning each frame
struct SkinVertex {
    f32 lx, ly, lz;          // joint-local position
    f32 r, g, b;             // vertex color
    f32 u, v;                // texture coords
    f32 tpage, cba;          // PSX texture info
    u32 jointIdx;            // owning joint index
    u16 sourceIndex = 0;     // original tPrimGeom vertex index
    u16 padSourceIndex = 0;
};

#if defined(RC_PLATFORM_PSP)
// Input of the fused character skinning loop, derived once from a skin's
// immutable SkinVertex array (48 bytes, two thirds unused there): half the
// bytes streamed per frame, colour and UV already in their integer form.
struct PspSkinStreamVertex {
    f32 x, y, z;      // SkinVertex lx, ly, lz (first: the VFPU loads 0/4/8)
    u8 r, g, b;       // SkinVertex r/g/b * 128 (exact for PspSkinColoursAreBytes)
    u8 joint;         // SkinVertex jointIdx (< 256)
    s16 u, v;         // static_cast<s16>(SkinVertex u, v)
    u16 source;       // SkinVertex sourceIndex
    u16 pad;
};
static_assert(sizeof(PspSkinStreamVertex) == 24, "compact skin stream vertex");

// Never 0; a new SkinData at a reused address still gets a new id.
inline u32 NextPspSkinInstanceId() {
    static u32 next = 0;
    if (++next == 0) ++next;
    return next;
}
#endif

struct SkinData {
    SkinVertex* verts = nullptr;
    u16* indices = nullptr;
    u32* primStart = nullptr;
    u8* primVertCount = nullptr;
    u32* primPacketOffset = nullptr;
    u8* primPacketSize = nullptr;
    u32 numVerts = 0;
    u32 numIndices = 0;
    u32 numPrims = 0;
    bool usesSemiTrans = false;
    u8 semiTransMode = 0;
#if defined(RC_PLATFORM_PSP)
    bool pspPacketRangesOrdered = false;
    // Lazily derived from the immutable skin vertices on the first skinned
    // draw: per joint, the largest joint-local vertex distance (-1 = joint
    // owns no vertex). Used only for conservative whole-character view
    // rejection; one float per joint, never rebuilt.
    bool pspJointRadiusBuilt = false;
    u32 pspJointRadiusCount = 0;
    f32* pspJointRadius = nullptr;
    // Lazily derived once from the immutable skin vertices: every r/g/b is a
    // PSX byte / 128, which lets the VFPU skinning use its short exact colour
    // chain (see psp/psp_vfpu_skin.h). No allocation.
    bool pspColourBytesBuilt = false;
    bool pspColourBytes = false;
    // Identifies this skin's immutable tpage/CBA lanes to the fused GE
    // upload, which then skips re-comparing lanes it already holds.
    u32 pspInstanceId = NextPspSkinInstanceId();
    // Lazily built on the first fused draw; null when a vertex does not fit
    // the compact form (the fused loop then reads `verts`).
    bool pspStreamBuilt = false;
    PspSkinStreamVertex* pspStream = nullptr;
#endif

    ~SkinData() {
        delete[] verts;
        delete[] indices;
        delete[] primStart;
        delete[] primVertCount;
        delete[] primPacketOffset;
        delete[] primPacketSize;
#if defined(RC_PLATFORM_PSP)
        delete[] pspJointRadius;
        delete[] pspStream;
#endif
    }
};

struct GeoRenderVertex {
    f32 x;
    f32 y;
    f32 z;
    f32 r;
    f32 g;
    f32 b;
    f32 u;
    f32 v;
    f32 tpage;
    f32 cba;
};

// PSX tCompositeAnim data loaded from character P3D chunk 0x4007.
// We preserve the composite definition and character-local clut lists used
// by ChangeSuit runtime updates.
struct CompositeAnimData;

struct CompositeAnimPartData {
    u16 field0 = 0;
    u16 field1 = 0;
    u32 animNameUID = 0;
    MiscAnimNode* animNode = nullptr;
    CompositeAnimData* compositeAnim = nullptr;
    ClutAnimData* clutAnim = nullptr;
    const tPrimGeom* targetPrimGeom = nullptr;
};

struct CompositeAnimData {
    u32 nameUID = 0;
    u16 field12 = 0;
    u16 numParts = 0;
    CompositeAnimPartData* parts = nullptr;
    u32 numCompositeAnims = 0;
    CompositeAnimData** compositeAnims = nullptr;
    u32 numClutAnims = 0;
    ClutAnimData** clutAnims = nullptr;

    ~CompositeAnimData();
};

// OriginalBasic - base for model data stored in LevelManager lists.
// PSX: ccNode-derived with type and storeID fields.
// PSX layout:
//   +0..23:  ccNode base
//   +16 (u16): type (0=Geo, 1=STree, 2=ETree) - overlaps ccNode.flags
//   +19 (s8):  storeID (0 or 2, for LevelManager purge)
//   +20 (u32): nameCRC (from ccNode)
struct OriginalBasic : public ccNode {
    // type is stored at +16 which IS ccNode.flags
    // storeID at +19 which IS ccNode.pri
    // nameCRC at +20 which IS ccNode.nameCRC

    u16 GetType() const { return (u16)flags; }
    void SetType(u16 t) { flags = (s16)t; }
    s8 GetStoreID() const { return pri; }
    void SetStoreID(s8 id) { pri = id; }
};

// OriginalSTree - skeleton tree model data (60 bytes on PSX)
// Stored in LevelManager::streeList, looked up by nameCRC.
// PSX layout:
//   +0..23:  OriginalBasic (ccNode)
//   +24..35: (OriginalTree fields, unused on PC)
//   +36:     tSTree* (PSX P3D STree pointer)
//   +52:     tCompositeAnim* (PSX composite animation)
//   +56:     suit/runtime object derived from the composite animation
//
// PC: instead of PSX-specific tSTree/tPrimGeom, we store PC-ready mesh data.
// We also preserve the raw 0x4007 composite definition, but not the derived
// suit/runtime object yet.
struct OriginalSTree : public OriginalBasic {
    // PC mesh data (replaces PSX tSTree + tPrimGeom + GTE rendering)
    pddiPrimBuffer* meshBuffer = nullptr;
    u32 meshVertCount = 0;
    u32 meshTriCount = 0;
#if REAL_TEXTURE_RENDERING
    // Index-buffer ranges grouped by (tpage, cba), built once alongside
    // meshBuffer, for the real-texture rendering path's sub-range draws.
    std::vector<RealTextureGroup> realTexGroups;
#endif

    // Skeleton data (parsed from P3D 0x6122/0x6121 chunks)
    STreeData* skeleton = nullptr;

    // Raw PSX primitive geometry retained so STree render callbacks can be
    // reinstated without reloading the source RR data.
    tPrimGeom* primGeom = nullptr;
    STreeXformVertsCallback xformVertsCallback = nullptr;
    STreeFixUpPolysCallback fixUpPolysCallback = nullptr;

    // CPU skinning data (local-space verts + joint indices)
    SkinData* skinData = nullptr;
    bool activeTreeInUse = false;

    // Character P3D 0x4007 composite animation definition.
    CompositeAnimData* compositeAnim = nullptr;

    OriginalSTree();
    ~OriginalSTree() override;

    // PSX: ChangeSuit__13OriginalSTreeP13DrawableSTrees (MODEL.CPP:3588)
    s32 ChangeSuit(struct DrawableSTree* drawable, s16 suitIndex);
};

// OriginalGeo - static/dynamic geometry model data (40 bytes on PSX)
// Stored in LevelManager geo lists, looked up by nameCRC.
// PC: stores a PC-ready prim buffer built from tDynGeom polygon data.
struct OriginalGeo : public OriginalBasic {
    pddiPrimBuffer* meshBuffer = nullptr;
#if REAL_TEXTURE_RENDERING
    // Index-buffer ranges grouped by (tpage, cba), built once alongside
    // meshBuffer, for the real-texture rendering path's sub-range draws.
    std::vector<RealTextureGroup> realTexGroups;
#endif
    tPrimGeom* primGeom = nullptr;
    s32 bboxMin[3] = {};
    s32 bboxMax[3] = {};
    bool usesSemiTrans = false;
    u8 semiTransMode = 0;
    u32 fastRenderWord1 = 0xFFFFFFFFu;
    GeoRenderVertex* dynamicVerts = nullptr;
    u32 dynamicVertCount = 0;
    u16* dynamicVertSourceIndex = nullptr;
    u32* dynamicColorList = nullptr;
    u32 dynamicColorCount = 0;
    u32* dynamicPrimStart = nullptr;
    u8* dynamicPrimVertCount = nullptr;
    u32* dynamicPrimMaterialUID = nullptr;
    u8* dynamicPrimCmd = nullptr;
    u16* dynamicPrimUVWords = nullptr;
    u32* dynamicPrimPacketOffset = nullptr;
    u32 dynamicPrimCount = 0;
#if defined(RC_PLATFORM_PSP)
    // Lighting inputs of the colours last patched into meshBuffer (see
    // DrawGeoPartMesh): the normalized world rotation, the port ambient and
    // the three port lights (direction, colour; zero when absent).
    struct PspLightKey {
        f32 rotation[9];
        u32 ambient;
        s32 lights[3][4];
    };
    PspLightKey pspLightKey{};
    bool pspLightKeyValid = false;
#endif

    OriginalGeo();
    ~OriginalGeo() override;
};

// OriginalETree - export-tree model data (52 bytes on PSX).
// Current PC port stores a single renderable mesh buffer, matching the
// existing Geo path until dedicated ETree data decoding is implemented.
struct OriginalETree : public OriginalBasic {
    pddiPrimBuffer* meshBuffer = nullptr;
    STreeData* skeleton = nullptr;
    OriginalGeo** geoParts = nullptr;
    u32* geoPartJointHashes = nullptr;
    u32* geoPartHashes = nullptr;
    u16 geoPartCount = 0;
    bool usesSemiTrans = false;
    u8 semiTransMode = 0;

    OriginalETree();
    ~OriginalETree() override;
};

// DrawableBasic - base drawable wrapper used by Model.
// STree models expose skeleton accessors for animation code; Geo models return null.
struct DrawableBasic {
    u16 displayFlag = 1;

    virtual ~DrawableBasic();
    virtual void Display(u32 flags) = 0;
    virtual OriginalSTree* GetOriginalSTree() const { return nullptr; }
    virtual OriginalSTree* GetAlternateSTree() const { return nullptr; }
};

inline OriginalSTree* GetActiveSTree(const DrawableBasic* drawable) {
    if (!drawable) {
        return nullptr;
    }

    OriginalSTree* active = drawable->GetAlternateSTree();
    return active ? active : drawable->GetOriginalSTree();
}

// DrawableSTree - wraps OriginalSTree for per-entity rendering (36 bytes on PSX)
// PSX layout:
//   +0..11:  DrawableTree base
//   +16:     u16 (display flag)
//   +24:     OriginalSTree* original
//   +28:     OriginalSTree* alternate (for suit changes)
//   +32:     (reserved)
struct DrawableSTree : public DrawableBasic {
    OriginalSTree* original = nullptr;   // +24 on PSX
    OriginalSTree* alternate = nullptr;  // +28 on PSX
    u32 mirrorFlags = 0;                 // +32 on PSX, bit 0 = mirrored tree
    u32* mirroredJointOrderMap = nullptr;
    s16 suitIndex = 0;
#if defined(RC_PLATFORM_PSP)
    // This drawable's copy of the model's skinned buffer (Display): the GE
    // draws it in place, where the shared buffer, rewritten for every
    // instance, had its vertices copied into the list on each draw.
    pddiPrimBuffer* pspSkinBuffer = nullptr;
    const pddiPrimBuffer* pspSkinBufferSource = nullptr;
    const SkinData* pspSkinBufferSkin = nullptr;
#endif

    DrawableSTree(OriginalSTree* orig);
    ~DrawableSTree() override;
    DrawableSTree(const DrawableSTree&) = delete;
    DrawableSTree& operator=(const DrawableSTree&) = delete;

    // PSX: Display__13DrawableSTree (calls OriginalSTree::Draw -> tPrimGeom::Display)
    // PC: draws the pddiPrimBuffer
    void Display(u32 flags) override;
    s32 MirrorTree(SModel* model);
    // PSX: ChangeSuit__13DrawableSTrees (MODEL.CPP:3630)
    s32 ChangeSuit(s16 suitIndexValue);
    OriginalSTree* GetOriginalSTree() const override { return original; }
    OriginalSTree* GetAlternateSTree() const override { return alternate; }
};

// DrawableGeo - wraps OriginalGeo for per-entity rendering.
struct DrawableGeo : public DrawableBasic {
    OriginalGeo* original = nullptr;

    DrawableGeo(OriginalGeo* orig);
    ~DrawableGeo() override;

    void Display(u32 flags) override;
};

// DrawableETree - wraps OriginalETree for EModel rendering.
struct DrawableETree : public DrawableBasic {
    OriginalETree* original = nullptr;
    u8* geoPartVisible = nullptr;
    Model* ownerModel = nullptr;

    DrawableETree(OriginalETree* orig);
    ~DrawableETree() override;

    void Display(u32 flags) override;
    void SetOwnerModel(Model* owner) { ownerModel = owner; }
    void SetGeoPartVisibleByHash(u32 hash, bool visible);
    void ResetGeoPartVisibility();
};

// Model - base class for per-entity 3D models (96 bytes on PSX)
// PSX layout (24 bytes ccNode base + 72 bytes Model-specific):
//   +24: DrawableBasic* drawable
//   +28: s32 drawableType (0=none, 1=Geo, 2=STree)
//   +32: void* animStructure (AnimStructure*)
//   +36: (reserved)
//   +40: void* ambientLight
//   +44: void* hwLights
//   +48: s32 hwLightCount
//   +52: u16 rotX (binary angle)
//   +54: u16 rotY
//   +56: u16 rotZ
//   +58: (padding)
//   +60: (padding)
//   +64: s32 posX
//   +68: s32 posY
//   +72: s32 posZ
//   +76: Thing* backPtr
//   +80: u32 modelFlags
//   +84: u16 shadowAngle
//   +86: (padding)
//   +88: s32 scale (SModel extension, 0x10000=1.0)
//   +92: s32 (reserved)
class Model : public ccNode {
public:
    // +24
    DrawableBasic* drawable = nullptr;
    // +28
    s32 drawableType = 0;
    // +32
    void* animStructure = nullptr;
    // +36
    void* field36 = nullptr;
    // +40
    void* ambientLight = nullptr;
    // +44
    void* hwLights = nullptr;
    // +48
    s32 hwLightCount = 0;
    // +52,+54,+56 rotation (binary angle units, 0-65535)
    u16 rotX = 0;
    u16 rotY = 0;
    u16 rotZ = 0;
    u16 pad58 = 0;
    // +60
    s32 pad60 = 0;
    // +64,+68,+72 position (PSX fixed-point)
    s32 posX = 0;
    s32 posY = 0;
    s32 posZ = 0;
    // +76
    Thing* backPtr = nullptr;
    // +80
    u32 modelFlags = 0;
    // +84
    u16 shadowAngle = 0;
    u16 pad86 = 0;

    Model();
    ~Model() override;
    void Reset();
    virtual void Show(u32 flags);
    void DrawShadow(u32 flags);

    virtual void Animate();
    virtual void ApplyAnimToModel(s32 thingType, s32 animEnum, s32 p3, s32 p4, s32 p5);
    virtual void SetAnim(s32 animEnum, s32 loopType, s32 flag, s32 extra);

    // PSX: animation boundary handlers dispatched via vtable from ExecuteHandler.
    // Base Model implementations are trampolines to AnimStructure methods.
    // Overridden by HumanoidModel (_Loop) and PlayerModel (_Loop, _RunToLast, _IncFrame).
    // PSX signature: void handler(Model* this_adjusted, AnimStructure* anim)
    virtual void HandleLoop(AnimStructure* anim);
    virtual void HandleLoopReverse(AnimStructure* anim);
    virtual void HandleHoldFirst(AnimStructure* anim);
    virtual void HandleHoldLast(AnimStructure* anim);
    virtual void HandleRunToLast(AnimStructure* anim);
    virtual void HandleHoldFrame(AnimStructure* anim);
    virtual void HandleRunToFrame(AnimStructure* anim);
    virtual void HandleIncFrame(AnimStructure* anim);
    virtual void HandleDecFrame(AnimStructure* anim);
    virtual void HandleLoopDesired(AnimStructure* anim);
    virtual void HandleRunToLastBlend(AnimStructure* anim);

    void DeleteAnimStructures();
    AmbientLight* AllocateAmbientLight();
    void DeleteAmbientLight();
    HardwareLight* AllocateHardwareLights(u32 count);
    void DeleteHardwareLights();

    // PSX: DeleteDrawable (called from destructor)
    void DeleteDrawable();
};

// SModel - skeleton-tree model (96 bytes on PSX, same allocation as Model)
// Extends Model with scale field for proportional scaling.
// PSX: _6SModel / Show__6SModelUl (MODEL.CPP:1013, 1454)
class SModel : public Model {
public:
    // +88. Scale fixed-point: 0x10000 = 1.0
    s32 scale = FIX16_ONE;
    // +92
    s32 field92 = 0;

    SModel();
    ~SModel() override;

    // PC: sets world matrix and draws the pddiPrimBuffer
    void Show(u32 flags) override;

    void Animate() override;
    void ApplyAnimToModel(s32 thingType, s32 animEnum, s32 loopType, s32 p4, s32 p5) override;
    void ApplyAnimToModelBasic(void* rawAnimation);
    BlendPoseState* InitBlendPose();
    AnimStructure* ApplyBlending(TransformAnim* animation, s32 blendFrames, s32 startFrame);
    void SetOriginalSTree(OriginalSTree* original);
    virtual void SetupModelCallbacks();
    virtual s32 MirrorTree();
    void InitSemiTransMode();
    s32 IsAnimationLoaded(s32 animEnum);
    void PlayDynamicAnim(s32 animEnum);
};

// GModel - geometry model (96 bytes on PSX, same allocation as Model)
// Used by props loaded through OriginalGeo/tDynGeom.
class GModel : public Model {
public:
    // Host-side matrix latch for PSX modelFlags bit0 path used by attached pickups.
    s32 attachedMatrix[8] = {};
    s32 attachedMatrixActive = 0;

    GModel();
    ~GModel() override;

    void Show(u32 flags) override;
    void SetOriginalGeo(OriginalGeo* original);
};

// EModel - export-tree model (120 bytes on PSX).
// Uses drawableType=3 so launcher logic can select the EModel anim path.
class EModel : public Model {
public:
    EModel();
    ~EModel() override;

    void Show(u32 flags) override;
    void Animate() override;
    void ApplyAnimToModel(s32 thingType, s32 animEnum, s32 loopType, s32 p4, s32 p5) override;
    AnimStructure* ApplyAnimToModel(TransformAnim* animation, s32 loopType);
    void SetOriginalETree(OriginalETree* original, TransformAnim* animation = nullptr);
};

// HumanoidModel - character model with animation matrices (136 bytes on PSX)
// PSX: _13HumanoidModel (MHUMAN.CPP:45)
class HumanoidModel : public SModel {
public:
    // +96: AnimationMatrices* (660 bytes, 10 joint slots, double-buffered)
    AnimationMatrices* animMatrices = nullptr;
    // +100: attack hand radius
    s32 attackHandRadius = 100;
    // +104: attack foot radius
    s32 attackFootRadius = 100;
    // +108
    s32 field108 = 100;
    // +112
    s32 field112 = 400;
    // +116
    s32 field116 = 100;
    // +120
    s32 field120 = 0;
    // +124..+132: persistent 16.16 head-track direction vector.
    LVector headTrackDir = { 0, 0, 0xFFFF };

#if HIGH_FPS_PLAY_PRESENTATION
    s32 nisJointTranslationYRender = 0;
    u32 nisJointTranslationYTick = 0xFFFFFFFFu;
#endif

#if NEW_CHEATS
    f32 bobblePhase = 0.0f;      // running oscillator phase
    f32 bobbleAmp = 0.0f;        // smoothed amplitude from continuous speed
    f32 bobbleKick = 0.0f;       // decaying amplitude injected by acceleration
    f32 bobbleAngleX = 0.0f;     // applied head pitch (binary angle)
    f32 bobbleAngleZ = 0.0f;     // applied head roll (binary angle)
    s32 bobblePrevX = 0;         // previous position for movement delta
    s32 bobblePrevZ = 0;
    s32 bobblePrevDX = 0;        // previous delta for acceleration
    s32 bobblePrevDZ = 0;
    bool bobblePrevValid = false;
    bool bobbleApplied = false;  // head joint currently holds our offset
    s16 bobbleBaseRotX = 0;      // head rotation before the offset
    s16 bobbleBaseRotZ = 0;
    s16 bobbleAppliedRotX = 0;   // head rotation after the offset
    s16 bobbleAppliedRotZ = 0;
    u32 bobbleTick = 0xFFFFFFFFu;
#endif

    HumanoidModel();
    ~HumanoidModel() override;

    void Animate() override;
    void SetupModelCallbacks() override;
    void SetAnim(s32 animEnum, s32 a3, s32 force, s32 extra) override;

    // Evaluates the active skeleton to world space with the joint post-callbacks
    // enabled, populating animMatrices->current with the AUTHORITATIVE attack-joint
    // world positions for hit detection. Normally this capture is a side effect of
    // Show() during rendering, but under HIGH_FPS_PLAY_PRESENTATION rendering is
    // decoupled (interpolated, capture disabled), so gameplay must drive this once
    // per logic tick instead. Safe to call independent of rendering: the skeleton
    // traversal restores joint state per joint.
    void CaptureAttackJointMatrices();

    // Only loops when mode == 0 (normal animation playback).
    void HandleLoop(AnimStructure* anim) override;
};

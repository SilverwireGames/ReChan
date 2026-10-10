#pragma once
#include "gen/cclist.h"
#include "gen/manager.h"
#include "p3d/flip.h"

struct CompositeAnimData;
struct MiscAnimNode;

struct SequenceAnimPart {
    u32 animHash = 0;
    MiscAnimNode* node = nullptr;
    TransformAnim* anim = nullptr;
};

// PSX tSequenceAnim-equivalent runtime object loaded from chunk 0x6040.
// numFrames is the packed sequence frame range used by tSequenceFlip
// (high bits = part index, low 8 bits = frame within that part).
struct SequenceAnim {
    u32 nameUID = 0;
    s32 numFrames = 0;
    u32 frameBits = 8;
    u32 numParts = 0;
    SequenceAnimPart* parts = nullptr;

    SequenceAnim() = default;
    ~SequenceAnim();

    TransformAnim* ResolvePartAnim(s32 index);
};

// PSX tFrameList-equivalent runtime object used by world/composite effects.
struct FrameListAnim {
    u32 nameUID = 0;
    u32 targetUID = 0;
    s32 numFrames = 0;
    u8* rawData = nullptr;
    u32 rawSize = 0;
    u32* frameOffsets = nullptr;

    FrameListAnim() = default;
    ~FrameListAnim();

    const s16* GetFrame(s32 index) const;
    u32 GetFrameVertexCount(s32 index) const;
};

struct VizAnimNodeEntry {
    u32 targetHash = 0;
    const u32* bitWords = nullptr;
};

// PSX tVizAnim-equivalent runtime object loaded from chunk 0x4020.
// Each node targets a child geo hash and uses a per-frame bitset to toggle
// visibility for that child.
struct VizAnim {
    u32 nameUID = 0;
    u32 targetUID = 0;
    s32 numFrames = 0;
    u32 numNodes = 0;
    VizAnimNodeEntry* nodes = nullptr;
    u8* rawData = nullptr;
    u32 rawSize = 0;

    VizAnim() = default;
    ~VizAnim();

    bool IsNodeVisible(u32 nodeIndex, s32 frame) const;
};

// PSX tParamAnim-equivalent scalar channel used by CBV param blending.
struct ParamAnimData {
    u32 nameUID = 0;
    s32 animType = 0;
    s32 numFrames = 0;
    u32 keyCount = 0;
    u16* keyTimes = nullptr;
    s32* keyValues = nullptr;

    ParamAnimData() = default;
    ~ParamAnimData();

    s32 EvaluateFrameReal(s32 frameReal) const;
};

// PSX tCBVParamAnim-equivalent table used by cbv_* misc anim parts.
struct CBVParamAnimData {
    u32 nameUID = 0;
    u32 targetUID = 0;
    u32 blendAnimUID = 0;
    s32 mode = 0;
    u32 valueRows = 0;
    u32 valueCols = 0;
    u32 numEntries = 0;
    u32* values = nullptr;
    u32* primOffsets = nullptr;

    CBVParamAnimData() = default;
    ~CBVParamAnimData();
};

// PSX tUVAnim-equivalent per-primitive UV frame animation loaded from chunk 0x600E.
// Targets a named child geo and replaces its UV words per-frame from a flat table.
struct UVListAnim {
    u32 nameUID = 0;
    u32 targetUID = 0;
    s32 numFrames = 0;
    s32 numEntries = 0;
    u16* uvData = nullptr;        // [numFrames * numEntries] packed UV words (U=low byte, V=high byte)
    u32* packetOffsets = nullptr; // PSX 0x6010: byte offsets into geo OT buffer, one per entry
    u32 numPacketOffsets = 0;
    u32* sortedPerm = nullptr;    // permutation: sortedPerm[k] = j such that packetOffsets[j] is k-th ascending

    UVListAnim() = default;
    ~UVListAnim();

    // Returns the packed UV word for a given frame and entry index.
    u16 GetUV(s32 frame, s32 entry) const;
};

// PSX tClutList-equivalent CLUT animation data loaded from chunk 0x6006.
// mode==0 updates material cba directly; mode!=0 targets primitive offsets.
struct ClutAnimData {
    u32 nameUID = 0;
    u32 materialUID = 0;
    u32 primUID = 0;
    u16 mode = 0;
    u16* frames = nullptr;
    s32 numFrames = 0;
    u16* offsets = nullptr;
    s32 numOffsets = 0;

    ClutAnimData() = default;
    ~ClutAnimData();

    u16 GetFrameValue(s32 frame) const;
};

// MiscAnimNode - node stored in AnimationManager's anim list (PSX: 28 bytes)
// PSX layout: ccMinNode(12) + field12(u32) + field16(s16) + field18(u8) + type(u8) + hash(u32) + anim(ptr)
struct MiscAnimNode : public ccMinNode {
    u32 field12 = 0;       // +12
    s16 field16 = 0;       // +16
    u8 field18 = 0;        // +18
    u8 type = 0;           // +19: 1=level, 2=petal
    u32 hash = 0;          // +20
    TransformAnim* anim = nullptr; // +24
    // Set when the raw anim data behind `anim` was actually multiple concatenated
    // tTransformAnim blocks (a frame-by-frame pose/texture flip-book) rather than
    // a single block. `anim` still points at parts[0] for callers that don't check
    // this; per-frame consumers should resolve the correct part via ResolvePart().
    CharSequenceAnim* animSequence = nullptr;
    ParamAnimData* paramAnim = nullptr;
    FrameListAnim* frameList = nullptr;
    CompositeAnimData* compositeAnim = nullptr;
    SequenceAnim* sequenceAnim = nullptr;
    VizAnim* vizAnim = nullptr;
    CBVParamAnimData* cbvParamAnim = nullptr;
    ClutAnimData* clutAnim = nullptr;
    UVListAnim* uvListAnim = nullptr;
    // PSX: MakePuppet creates a per-instance tFlip from the shared tAnimation.
    // When false, this node is a per-ComEffect clone whose sub-object pointers
    // are borrowed (owned by g_animMgr); the destructor skips deletion.
    bool ownsSubs = true;

    MiscAnimNode() = default;
    ~MiscAnimNode() override;
};

// AnimationManager (PSX: 40 bytes) - PSX GAME.CPP manager #15
// Stores a list of misc TransformAnim objects for level/petal prop animations.
// PSX ANIMMGR.CPP: 0x80057168
// Singleton stored at gp+3760 (0x800DD7FC)
class AnimationManager : public Manager {
public:
    ccMinList animList; // +28: list of MiscAnimNode*
    // Port addition, bumped by every list change (add, purge): name lookups
    // through GetMiscAnim may resolve differently afterwards.
    u32 generation = 0;

    AnimationManager();
    ~AnimationManager() override;

    void InternalOpen() override;
    void InternalClose() override;
    void InternalReset() override;

    // PSX: PurgePetal__16AnimationManager (0x80057230)
    // Removes and deletes all petal-type (type==2) anim nodes.
    void PurgePetal();

    // PSX: PurgeLevel__16AnimationManager (0x800572B4)
    // Removes and deletes all anim nodes.
    void PurgeLevel();

    // PSX: GetMiscAnim__16AnimationManagerUl (0x80057308)
    // Returns the first MiscAnimNode whose hash matches, or nullptr.
    MiscAnimNode* GetMiscAnim(u32 hash);

    // Add a node to the animation list (tail insertion).
    void AddAnim(MiscAnimNode* node);
};

extern AnimationManager* g_animMgr;

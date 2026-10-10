// skeleton.h - Pure3D v11.3 tSTree/tSJoint skeleton system
// PSX: TSTREE.CPP, STLOAD.CPP
// Chunk IDs: 0x6120 PSX_STREE, 0x6121 PSX_STREE_JOINT,
//            0x6122 PSX_MAPPED_STREE, 0x6125 PSX_STREE_REST_POSE
#pragma once

#include "core.h"
#include "p3d/matrix.h"

class pddiPrimBuffer;
struct STreeJoint;
using STreeJointCallback = s32 (*)(STreeJoint* joint, u32 jointIndex, const Mat4& currentMatrix);

// STreeJoint flag bits
enum STreeFlag : u32 {
    STF_TRANSFORM      = 0x01,  // apply TransMatrix + RotMatrixYZX
    STF_PUSH_MATRIX    = 0x02,  // save current to stack (_5tPort_PushMatrix)
    STF_POP_MATRIX     = 0x04,  // restore saved matrix from stack
    STF_HAS_MESH       = 0x08,  // this joint has renderable geometry
    STF_LOAD_MATRIX    = 0x10,  // load a previously captured matrix
    STF_TREE_CALLBACK  = 0x20,  // tree callback after draw
    STF_MULT_MATRIX    = 0x40,  // multiply by static view matrix (rendering only)
    STF_CAPTURE_MATRIX = 0x80,  // save current matrix to joint buffer
    STF_PRE_CALLBACK_MASK = 0x0F00,
    STF_POST_CALLBACK_MASK = 0xF000,
};

// tSJoint - per-joint skeletal data (68 bytes on PSX).
// Joints are stored in a flat array and processed sequentially by Display.
struct STreeJoint {
    u32 nameUID;              // +0: p3dHash of joint name
    u32 flags;                // +4: runtime flags (pre-computed in file)
    // PC-only: human-readable joint name (e.g. "Bip01 Head"), preserved from
    // the source PString for editor/export display. Not present on PSX.
    char name[64] = {};
    s32 translationX;         // +12: set by animation system
    s32 translationY;         // +16
    s32 translationZ;         // +20
    s16 rotationX;            // +24: set by animation system (binary angle)
    s16 rotationY;            // +26
    s16 rotationZ;            // +28
    s16 restPoseRotX;         // +32: rest-pose rotation (binary angle, from 0x6125)
    s16 restPoseRotY;         // +34
    s16 restPoseRotZ;         // +36
    // PC-only: bind baseline sampled from animation 0 frame 0.
    s32 bindTranslationX;
    s32 bindTranslationY;
    s32 bindTranslationZ;
    s16 bindRotationX;
    s16 bindRotationY;
    s16 bindRotationZ;
    u16 primGeomStartIdx;     // +52: first vertex index in tPrimGeom
    u16 primGeomCount;        // +54: number of vertices for this joint
    u16 polyStartIdx;         // +56: first polygon index
    s32 captureBufferIdx;      // STLOAD AddJoint matrix buffer dword offset
    void* callbackData = nullptr;
    STreeJointCallback preCallback = nullptr;
    STreeJointCallback postCallback = nullptr;
    Mat4 overrideMatrix;
    bool useOverrideMatrix = false;

    // PC-only: uniform scale applied to this joint's output world matrix only
    f32 renderScale = 1.0f;

    // PC-only: per-joint pddiPrimBuffer
    pddiPrimBuffer* meshBuffer = nullptr;

    // PC-only: the rotation part (m[0..2], m[4..6], m[8..10]) of the last
    // YZX matrix built for these exact angles. A character's skeleton is
    // built several times per frame with the same pose; the sin/cos and the
    // products are then skipped. Zero-filled arrays start invalid.
    s16 rotationCacheX = 0;
    s16 rotationCacheY = 0;
    s16 rotationCacheZ = 0;
    bool rotationCacheValid = false;
    f32 rotationCache[9] = {};
};

// tSTree - parsed skeleton tree (replaces PSX tSTree + tPrimGrouping).
struct STreeData {
    u32 numJoints = 0;
    u32 numMapEntries = 0;
    u32* jointOrderMap = nullptr;    // animation parameter mapping
    STreeJoint* joints = nullptr;    // flat joint array

    STreeData() = default;
    ~STreeData();

    // PSX: GetJoint__6tSTreei resolves through jointOrderMap when present.
    STreeJoint* GetJoint(s32 index);
    const STreeJoint* GetJoint(s32 index) const;

    // Compute joint world matrices from current rotation/translation values.
    // Uses the flags-based traversal (push/pop/capture/load).
    void ComputeWorldMatrices(Mat4* outMatrices) const;
    void ComputeWorldMatricesWithCallbacks(Mat4* outMatrices);
};

STreeData* CloneSTreeData(const STreeData* source);

// Parse a 0x6122 (PSX_MAPPED_STREE) or 0x6120 (PSX_STREE) chunk.
// chunkData points to the first byte AFTER the 6-byte chunk header.
// chunkDataSize = chunkSize - 6.
STreeData* ParseSTreeChunk(const u8* chunkData, u32 chunkDataSize, bool isMapped);

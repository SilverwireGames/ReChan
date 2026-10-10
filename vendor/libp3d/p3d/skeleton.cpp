// skeleton.cpp - Pure3D v11.3 tSTree/tSJoint implementation
// PSX: TSTREE.CPP (Display, ComputeMatrices), STLOAD.CPP (tSTreeLoader)
#include "p3d/skeleton.h"
#if defined(RC_PLATFORM_PSP)
#include "psp/psp_perf.h"
#endif
#include "p3d/byteread.h"
#include "p3d/hash.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "p3d/p3dmath.h"
#include <cstring>
#include <cstdlib>

// P3D chunk IDs for skeleton loading
static constexpr u16 CHUNK_STREE_JOINT   = 0x6121;
static constexpr u16 CHUNK_STREE_MAPPING = 0x4123;
static constexpr u16 CHUNK_REST_POSE     = 0x6125;

// Read PString: u8 length prefix + chars. Returns name hash.
// When outName is provided, also copies the decoded string there (truncated
// to outNameCap - 1, always null-terminated).
static u32 ReadPString(const u8* data, u32 maxLen, u32& outBytesRead,
                       char* outName = nullptr, u32 outNameCap = 0) {
    if (outName && outNameCap > 0) {
        outName[0] = '\0';
    }
    if (maxLen < 1) {
        outBytesRead = 0;
        return 0;
    }
    u8 len = data[0];
    if (1u + len > maxLen) {
        outBytesRead = 1;
        return 0;
    }
    char buf[256];
    u32 copyLen = (len < 255) ? len : 255;
    std::memcpy(buf, data + 1, copyLen);
    buf[copyLen] = '\0';
    outBytesRead = 1 + len;
    if (outName && outNameCap > 0) {
        u32 nameCopyLen = (copyLen < outNameCap - 1) ? copyLen : outNameCap - 1;
        std::memcpy(outName, buf, nameCopyLen);
        outName[nameCopyLen] = '\0';
    }
    return p3dHash(buf);
}

STreeData::~STreeData() {
    if (joints) {
        for (u32 i = 0; i < numJoints; i++) {
            if (joints[i].meshBuffer) {
                joints[i].meshBuffer->Release();
                joints[i].meshBuffer = nullptr;
            }
        }
        std::free(joints);
        joints = nullptr;
    }
    if (jointOrderMap) {
        std::free(jointOrderMap);
        jointOrderMap = nullptr;
    }
}

STreeData* CloneSTreeData(const STreeData* source) {
    if (!source) {
        return nullptr;
    }

    STreeData* clone = new STreeData();
    clone->numJoints = source->numJoints;
    clone->numMapEntries = source->numMapEntries;

    if (source->jointOrderMap && source->numMapEntries > 0) {
        clone->jointOrderMap = static_cast<u32*>(std::malloc(sizeof(u32) * source->numMapEntries));
        if (!clone->jointOrderMap) {
            delete clone;
            return nullptr;
        }
        std::memcpy(clone->jointOrderMap, source->jointOrderMap, sizeof(u32) * source->numMapEntries);
    }

    if (source->joints && source->numJoints > 0) {
        clone->joints = static_cast<STreeJoint*>(std::malloc(sizeof(STreeJoint) * source->numJoints));
        if (!clone->joints) {
            delete clone;
            return nullptr;
        }

        for (u32 i = 0; i < source->numJoints; i++) {
            clone->joints[i] = source->joints[i];
            clone->joints[i].callbackData = nullptr;
            clone->joints[i].preCallback = nullptr;
            clone->joints[i].postCallback = nullptr;
            clone->joints[i].flags &= ~(STF_PRE_CALLBACK_MASK | STF_POST_CALLBACK_MASK);
            clone->joints[i].useOverrideMatrix = false;
            clone->joints[i].renderScale = 1.0f;
            clone->joints[i].meshBuffer = nullptr;
        }
    }

    return clone;
}

STreeJoint* STreeData::GetJoint(s32 index) {
    return const_cast<STreeJoint*>(static_cast<const STreeData*>(this)->GetJoint(index));
}

const STreeJoint* STreeData::GetJoint(s32 index) const {
    if (index < 0 || !joints || numJoints == 0) {
        return nullptr;
    }

    u32 jointIndex = static_cast<u32>(index);
    if (jointOrderMap) {
        if (jointIndex >= numMapEntries) {
            return nullptr;
        }
        jointIndex = jointOrderMap[jointIndex];
    }

    if (jointIndex >= numJoints) {
        return nullptr;
    }

    return &joints[jointIndex];
}

// Mat4 storage without the identity constructor. The matrix stack and the
// capture buffer are only read after being written (push before pop, capture
// before a valid load), so initialising all 96 slots on every skeleton build
// was pure overhead (~1.5k stores per call, several calls per character per
// frame).
union UninitMat4 {
    Mat4 value;
    UninitMat4() {}
};

static const Mat4 kSkeletonIdentity;

static void ComputeWorldMatricesInternal(STreeData* skeleton, Mat4* outMatrices, bool invokeCallbacks) {
    if (!skeleton || !skeleton->joints || skeleton->numJoints == 0) {
        return;
    }
#if defined(RC_PLATFORM_PSP)
    PspPerf::ScopeTimer skeletonTimer(PspPerf::RendererCounter::SkeletonUs);
    PspPerf::CountRenderer(PspPerf::RendererCounter::SkeletonBuilds);
    PspPerf::CountRenderer(PspPerf::RendererCounter::SkeletonJoints, skeleton->numJoints);
#endif

    // The current matrix is never copied: it points at the identity, at an
    // output already written this build (the stack keeps such pointers), or
    // at a capture slot. Each joint's products are written straight into its
    // output. Same products in the same order as copying matrices around.
    const Mat4* stack[32];
    s32 stackTop = 0;
    const Mat4* current = &kSkeletonIdentity;
    UninitMat4 product;  // MULT_MATRIX result when a TRANSFORM follows

    // Capture/load buffer keys are the raw dword offsets loaded from the joint chunk.
    s32 captureKeys[64];
    UninitMat4 captureValues[64];
    bool captureValid[64];
    s32 captureCount = 0;

    auto findCaptureSlot = [&](s32 key) -> s32 {
        for (s32 i = 0; i < captureCount; i++) {
            if (captureKeys[i] == key) {
                return i;
            }
        }
        if (captureCount >= 64) {
            return -1;
        }
        captureKeys[captureCount] = key;
        captureValid[captureCount] = false;
        return captureCount++;
    };

    for (u32 i = 0; i < skeleton->numJoints; i++) {
        STreeJoint& j = skeleton->joints[i];
        Mat4& out = outMatrices[i];
        const bool preserveJointState = invokeCallbacks
            && ((j.flags & (STF_PRE_CALLBACK_MASK | STF_POST_CALLBACK_MASK)) != 0);

        u32 savedFlags = 0;
        s32 savedTranslationX = 0;
        s32 savedTranslationY = 0;
        s32 savedTranslationZ = 0;
        s16 savedRotationX = 0;
        s16 savedRotationY = 0;
        s16 savedRotationZ = 0;
        bool savedUseOverrideMatrix = false;
        UninitMat4 savedOverrideMatrix;
        f32 savedRenderScale = 1.0f;

        if (preserveJointState) {
            savedFlags = j.flags;
            savedTranslationX = j.translationX;
            savedTranslationY = j.translationY;
            savedTranslationZ = j.translationZ;
            savedRotationX = j.rotationX;
            savedRotationY = j.rotationY;
            savedRotationZ = j.rotationZ;
            savedUseOverrideMatrix = j.useOverrideMatrix;
            savedOverrideMatrix.value = j.overrideMatrix;
            savedRenderScale = j.renderScale;
        }

        if (j.flags & STF_PUSH_MATRIX) {
            if (stackTop < 32) {
                stack[stackTop++] = current;
            }
        }

        if (invokeCallbacks && (j.flags & STF_PRE_CALLBACK_MASK) && j.preCallback) {
#if defined(RC_PLATFORM_PSP)
            PspPerf::ScopeTimer callbackTimer(PspPerf::RendererCounter::SkelCallbackUs);
#endif
            j.preCallback(&j, i, *current);
        }

        if ((j.flags & STF_MULT_MATRIX) && j.useOverrideMatrix) {
            Mat4& target = (j.flags & STF_TRANSFORM) ? product.value : out;
            p3dMat4MulInto(*current, j.overrideMatrix, target);
            current = &target;
        }

        if (j.flags & STF_TRANSFORM) {
            Mat4 local;
            p3dBuildRotMatrixYZX(j.rotationX, j.rotationY, j.rotationZ, local);
            local.SetTranslation((f32)j.translationX, (f32)j.translationY, (f32)j.translationZ);
            p3dMat4MulInto(*current, local, out);
            current = &out;
        }

        if (j.renderScale != 1.0f) {
            if (current != &out) {
                out = *current;
                current = &out;
            }
            out.ScaleRotation(j.renderScale);
        }

        if (invokeCallbacks && (j.flags & STF_POST_CALLBACK_MASK) && j.postCallback) {
#if defined(RC_PLATFORM_PSP)
            PspPerf::ScopeTimer callbackTimer(PspPerf::RendererCounter::SkelCallbackUs);
#endif
            j.postCallback(&j, i, *current);
        }

        if (j.flags & STF_CAPTURE_MATRIX) {
            if (j.captureBufferIdx >= 0) {
                s32 slot = findCaptureSlot(j.captureBufferIdx);
                if (slot >= 0) {
                    captureValues[slot].value = *current;
                    captureValid[slot] = true;
                }
            }
        }

        if (j.flags & STF_LOAD_MATRIX) {
            if (j.captureBufferIdx >= 0) {
                s32 slot = findCaptureSlot(j.captureBufferIdx);
                if (slot >= 0 && captureValid[slot]) {
                    // Copied now: a later capture may rewrite the slot.
                    out = captureValues[slot].value;
                    current = &out;
                }
            }
        }

        if (current != &out) {
            out = *current;
            current = &out;
        }

        if (j.flags & STF_POP_MATRIX) {
            if (stackTop > 0) {
                current = stack[--stackTop];
            }
        }

        if (preserveJointState) {
            // PSX callbacks affect the matrix build for this draw only.
            j.flags = savedFlags;
            j.translationX = savedTranslationX;
            j.translationY = savedTranslationY;
            j.translationZ = savedTranslationZ;
            j.rotationX = savedRotationX;
            j.rotationY = savedRotationY;
            j.rotationZ = savedRotationZ;
            j.useOverrideMatrix = savedUseOverrideMatrix;
            j.overrideMatrix = savedOverrideMatrix.value;
            j.renderScale = savedRenderScale;
        }
    }
}

void STreeData::ComputeWorldMatrices(Mat4* outMatrices) const {
    ComputeWorldMatricesInternal(const_cast<STreeData*>(this), outMatrices, false);
}

void STreeData::ComputeWorldMatricesWithCallbacks(Mat4* outMatrices) {
    ComputeWorldMatricesInternal(this, outMatrices, true);
}

// Parse a single 0x6121 joint sub-chunk
static bool ParseJointChunk(const u8* data, u32 dataSize, STreeJoint* out) {
    u32 p = 0;

    // PString -> nameUID (+ human-readable name for PC-only tooling)
    u32 nameBytes = 0;
    out->nameUID = ReadPString(data + p, dataSize - p, nameBytes, out->name, sizeof(out->name));
    p += nameBytes;

    // Long -> flags (PSX stores pre-computed traversal flags here)
    if (p + 4 > dataSize) {
        return false;
    }
    out->flags = p3dReadU32LE(data + p);
    p += 4;

    // Word -> primGeomStartIdx
    if (p + 2 > dataSize) {
        return false;
    }
    out->primGeomStartIdx = p3dReadU16LE(data + p);
    p += 2;

    // Word -> primGeomCount
    if (p + 2 > dataSize) {
        return false;
    }
    out->primGeomCount = p3dReadU16LE(data + p);
    p += 2;

    // Word -> polyStartIdx
    if (p + 2 > dataSize) {
        return false;
    }
    out->polyStartIdx = p3dReadU16LE(data + p);
    p += 2;

    // Long -> matrix buffer dword offset (STLOAD AddJoint).
    if (p + 4 > dataSize) {
        return false;
    }
    out->captureBufferIdx = (s32)p3dReadU32LE(data + p);
    p += 4;

    // Initialize runtime fields
    out->translationX = 0;
    out->translationY = 0;
    out->translationZ = 0;
    out->rotationX = 0;
    out->rotationY = 0;
    out->rotationZ = 0;
    out->restPoseRotX = 0;
    out->restPoseRotY = 0;
    out->restPoseRotZ = 0;
    out->bindTranslationX = 0;
    out->bindTranslationY = 0;
    out->bindTranslationZ = 0;
    out->bindRotationX = 0;
    out->bindRotationY = 0;
    out->bindRotationZ = 0;
    out->callbackData = nullptr;
    out->preCallback = nullptr;
    out->postCallback = nullptr;
    out->useOverrideMatrix = false;
    out->renderScale = 1.0f;
    out->meshBuffer = nullptr;

    // Optional 0x6125 rest-pose sub-chunk (single optional child in STLOAD AddJoint).
    if (p + 6 <= dataSize) {
        u16 subId = p3dReadU16LE(data + p);
        u32 subSize = p3dReadU32LE(data + p + 2);
        if (subSize >= 6 && p + subSize <= dataSize && subId == CHUNK_REST_POSE) {
            u32 sp = p + 6;
            if (sp + 6 <= p + subSize) {
                out->restPoseRotX = p3dReadS16LE(data + sp);
                out->restPoseRotY = p3dReadS16LE(data + sp + 2);
                out->restPoseRotZ = p3dReadS16LE(data + sp + 4);
            }
        }
    }

    return true;
}

STreeData* ParseSTreeChunk(const u8* chunkData, u32 chunkDataSize, bool isMapped) {
    u32 p = 0;

    // PString -> skeleton name
    u32 nameBytes = 0;
    ReadPString(chunkData + p, chunkDataSize - p, nameBytes);
    p += nameBytes;

    // Word -> numJoints
    if (p + 2 > chunkDataSize) {
        return nullptr;
    }
    s16 numJoints = p3dReadS16LE(chunkData + p);
    p += 2;
    if (numJoints <= 0 || numJoints > 256) {
        return nullptr;
    }

    // PString -> material name (skip)
    u32 matBytes = 0;
    ReadPString(chunkData + p, chunkDataSize - p, matBytes);
    p += matBytes;

    // Long -> extraMemSize (skip)
    if (p + 4 > chunkDataSize) {
        return nullptr;
    }
    p += 4;

    STreeData* tree = new STreeData();
    tree->numJoints = (u32)numJoints;
    tree->joints = (STreeJoint*)std::calloc(numJoints, sizeof(STreeJoint));

    // Parse sub-chunks
    u32 jointIdx = 0;
    while (p + 6 <= chunkDataSize) {
        u16 subId = p3dReadU16LE(chunkData + p);
        u32 subSize = p3dReadU32LE(chunkData + p + 2);
        if (subSize < 6 || p + subSize > chunkDataSize) {
            break;
        }

        if (subId == CHUNK_STREE_MAPPING && isMapped) {
            u32 mp = p + 6;
            if (mp + 4 <= p + subSize) {
                u32 mapCount = p3dReadU32LE(chunkData + mp);
                mp += 4;
                tree->numMapEntries = mapCount;
                tree->jointOrderMap = (u32*)std::malloc(mapCount * sizeof(u32));
                for (u32 m = 0; m < mapCount && mp + 4 <= p + subSize; m++) {
                    tree->jointOrderMap[m] = p3dReadU32LE(chunkData + mp);
                    mp += 4;
                }
            }
        } else if (subId == CHUNK_STREE_JOINT) {
            if (jointIdx < (u32)numJoints) {
                ParseJointChunk(chunkData + p + 6, subSize - 6, &tree->joints[jointIdx]);
                jointIdx++;
            }
        }

        p += subSize;
    }

    // If no mapping was parsed, create identity mapping
    if (!tree->jointOrderMap) {
        tree->numMapEntries = tree->numJoints;
        tree->jointOrderMap = (u32*)std::malloc(tree->numJoints * sizeof(u32));
        for (u32 i = 0; i < tree->numJoints; i++) {
            tree->jointOrderMap[i] = i;
        }
    }

    return tree;
}


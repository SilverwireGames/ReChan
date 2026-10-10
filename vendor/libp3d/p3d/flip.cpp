#include "p3d/flip.h"
#include "p3d/byteread.h"
#include "p3d/p3dmath.h"
#include <cstring>

static s16 LerpAngle16(s16 a0, s16 a1, s32 frac16) {
    // PSX interpolates angle deltas in 16-bit space (wrapped delta).
    s16 delta = (s16)(a1 - a0);
    return (s16)(a0 + (s16)(((s32)delta * (s64)frac16) >> 16));
}

static s32 GetUnwrappedTime(const u8* times, s32 index) {
    if (index <= 0) {
        return times[0];
    }
    s32 currentWrap = 0;
    s32 prevRaw = times[0];
    for (s32 k = 1; k <= index; k++) {
        s32 currRaw = times[k];
        if (currRaw < prevRaw) {
            currentWrap += 256;
        }
        prevRaw = currRaw;
    }
    return times[index] + currentWrap;
}

static s32 InferChannelLastFrame(const u8* rawData, u32 rawSize, const TransformAnim::Channel& ch) {
    if (!ch.chData) {
        return 0;
    }

    if (ch.keyType != KEY_JOINT_1DOF_ANGLE &&
        ch.keyType != KEY_JOINT_3DOF_ANGLE &&
        ch.keyType != KEY_JOINT_3DOF_LP_PSX) {
        return 0;
    }

    if ((u32)(ch.chData - rawData) + 16 > rawSize) {
        return 0;
    }

    const u32 numKeys = p3dReadU32LE(ch.chData + 8);
    const u32 keyTimesOff = p3dReadU32LE(ch.chData + 12);
    const u32 keyTimesByteOff = keyTimesOff * 4;
    if (numKeys == 0 || keyTimesByteOff + numKeys > rawSize) {
        return 0;
    }

    return GetUnwrappedTime(rawData + keyTimesByteOff, (s32)numKeys - 1);
}

// CharSequenceAnim

CharSequenceAnim::~CharSequenceAnim() {
    if (parts) {
        for (u32 i = 0; i < numParts; i++) {
            delete parts[i];
        }
        delete[] parts;
        parts = nullptr;
    }
}

TransformAnim* CharSequenceAnim::ResolvePart(s32 globalFrame, s32& outLocalFrame) const {
    if (!parts || numParts == 0 || globalFrame < 0) {
        return nullptr;
    }
    s32 remaining = globalFrame;
    for (u32 i = 0; i < numParts; i++) {
        TransformAnim* part = parts[i];
        if (!part || part->numFrames <= 0) continue;
        const s32 partFrames = part->numFrames;
        if (remaining < partFrames || i == numParts - 1) {
            outLocalFrame = remaining;
            return part;
        }
        remaining -= partFrames;
    }
    return nullptr;
}

// TransformAnim

TransformAnim::TransformAnim()
    : nameUID(0), numFrames(0), targetType(0), targetNameUID(0), numRotChannels(0), numTransChannels(0),
      rotChannels(nullptr), transChannels(nullptr), ownedRawData(nullptr) {}

TransformAnim::~TransformAnim() {
    delete[] rotChannels;
    delete[] transChannels;
    if (ownedRawData) {
        std::free(ownedRawData);
    }
}

TransformAnim* TransformAnim::Parse(const u8* rawData, u32 rawSize) {
    if (!rawData || rawSize < 40) {
        return nullptr;
    }

    // tTransformAnim header layout (40 bytes):
    //   +0:  nameUID (u32)
    //   +4:  refCount (s32, ignored)
    //   +8:  vtable offset (u32, ignored)
    //   +12: numFrames (s32)
    //   +16: targetType (s32)
    //   +20: targetNameUID (u32)
    //   +24: numRotChannels (s32)
    //   +28: numTransChannels (s32)
    //   +32: rotChannelArrayOffset (u32 DWORD offset)
    //   +36: transChannelArrayOffset (u32 DWORD offset)

    s32 numRotCh = (s32)p3dReadU32LE(rawData + 24);
    s32 numTransCh = (s32)p3dReadU32LE(rawData + 28);
    u32 rotArrayByteOff = p3dReadU32LE(rawData + 32) * 4;
    u32 transArrayByteOff = p3dReadU32LE(rawData + 36) * 4;

    if (rotArrayByteOff + (u32)numRotCh * 4 > rawSize) {
        return nullptr;
    }
    if (transArrayByteOff + (u32)numTransCh * 4 > rawSize) {
        return nullptr;
    }

    TransformAnim* ta = new TransformAnim();
    ta->nameUID = p3dReadU32LE(rawData + 0);
    ta->numFrames = (s32)p3dReadU32LE(rawData + 12);
    ta->targetType = (s32)p3dReadU32LE(rawData + 16);
    ta->targetNameUID = p3dReadU32LE(rawData + 20);
    ta->numRotChannels = numRotCh;
    ta->numTransChannels = numTransCh;

    // Parse rotation channels
    if (numRotCh > 0) {
        ta->rotChannels = new Channel[numRotCh];
        for (s32 i = 0; i < numRotCh; i++) {
            u32 chByteOff = p3dReadU32LE(rawData + rotArrayByteOff + i * 4) * 4;
            if (chByteOff + 8 > rawSize) {
                ta->rotChannels[i] = { 0, 0, nullptr, rawData, rawSize };
                continue;
            }
            const u8* ch = rawData + chByteOff;
            ta->rotChannels[i] = {
                p3dReadU32LE(ch + 0),    // jointParam
                p3dReadU32LE(ch + 4),    // keyType
                ch,                 // chData
                rawData,            // rawBase
                rawSize             // rawSize
            };
        }
    }

    // Parse translation channels
    if (numTransCh > 0) {
        ta->transChannels = new Channel[numTransCh];
        for (s32 i = 0; i < numTransCh; i++) {
            u32 chByteOff = p3dReadU32LE(rawData + transArrayByteOff + i * 4) * 4;
            if (chByteOff + 8 > rawSize) {
                ta->transChannels[i] = { 0, 0, nullptr, rawData, rawSize };
                continue;
            }
            const u8* ch = rawData + chByteOff;
            ta->transChannels[i] = {
                p3dReadU32LE(ch + 0),    // jointParam
                p3dReadU32LE(ch + 4),    // keyType
                ch,                 // chData
                rawData,            // rawBase
                rawSize             // rawSize
            };
        }
    }

    s32 inferredFrames = ta->numFrames;
    for (s32 i = 0; i < ta->numRotChannels; i++) {
        const s32 lastFrame = InferChannelLastFrame(rawData, rawSize, ta->rotChannels[i]);
        if (lastFrame + 1 > inferredFrames) {
            inferredFrames = lastFrame + 1;
        }
    }
    for (s32 i = 0; i < ta->numTransChannels; i++) {
        const s32 lastFrame = InferChannelLastFrame(rawData, rawSize, ta->transChannels[i]);
        if (lastFrame + 1 > inferredFrames) {
            inferredFrames = lastFrame + 1;
        }
    }
    ta->numFrames = inferredFrames;

    return ta;
}

// Detect a tTransformAnim header at rawData+offset: vtable==0, plausible field ranges.
static bool IsValidTransformAnimHeader(const u8* rawData, u32 rawSize, u32 offset) {
    if (offset + 40 > rawSize) return false;
    const u32 vtable    = p3dReadU32LE(rawData + offset + 8);
    const s32 numFrames = (s32)p3dReadU32LE(rawData + offset + 12);
    const s32 targetType = (s32)p3dReadU32LE(rawData + offset + 16);
    const s32 numRot    = (s32)p3dReadU32LE(rawData + offset + 24);
    const s32 numTrans  = (s32)p3dReadU32LE(rawData + offset + 28);
    return vtable == 0
        && numFrames >= 1 && numFrames <= 512
        && targetType >= 0 && targetType <= 3
        && numRot >= 1 && numRot <= 100
        && numTrans >= 0 && numTrans <= 10;
}

void* TransformAnim::ParseMulti(u8* rawData, u32 rawSize) {
    if (!rawData || rawSize < 40) {
        return nullptr;
    }

    if (!IsValidTransformAnimHeader(rawData, rawSize, 0)) {
        return nullptr;
    }

    const u32 baseUID = p3dReadU32LE(rawData + 0);

    // Find all consecutive blocks: nameUID must be baseUID+0, +1, +2, ...
    // Scan the whole remaining buffer for each next UID; narrow search windows can
    // miss valid block starts when packed data does not match our size estimate.
    u32 blockOffsets[16];
    u32 numBlocks = 1;
    blockOffsets[0] = 0;

    u32 nextUID = baseUID + 1;
    while (numBlocks < 16) {
        // Keep block order monotonic and scan forward through the whole blob.
        const u32 searchStart = blockOffsets[numBlocks - 1] + 4;
        bool found = false;
        for (u32 off = searchStart; off + 40 <= rawSize; off += 4) {
            const u32 uid = p3dReadU32LE(rawData + off);
            if (uid == nextUID && IsValidTransformAnimHeader(rawData, rawSize, off)) {
                blockOffsets[numBlocks++] = off;
                nextUID++;
                found = true;
                break;
            }
        }
        if (!found) break;
    }

    if (numBlocks == 1) {
        // Single block – normal path; caller uses Parse() directly.
        TransformAnim* ta = TransformAnim::Parse(rawData, rawSize);
        if (ta) {
            ta->ownedRawData = rawData;
        }
        return ta;
    }

    // Multiple blocks: build a CharSequenceAnim owning one TransformAnim per block.
    // Each block's Parse() gets a view into rawData but does NOT own the data;
    // the CharSequenceAnim takes ownership of rawData via the first part.
    CharSequenceAnim* seq = new CharSequenceAnim();
    seq->numParts = numBlocks;
    seq->parts = new TransformAnim*[numBlocks];

    for (u32 i = 0; i < numBlocks; i++) {
        const u32 blockStart = blockOffsets[i];
        const u32 blockSize = rawSize - blockStart; // view to end; Parse stops at what it needs
        TransformAnim* part = TransformAnim::Parse(rawData + blockStart, blockSize);
        if (!part) {
            // Abort: free what we built so far
            for (u32 j = 0; j < i; j++) delete seq->parts[j];
            delete[] seq->parts;
            delete seq;
            std::free(rawData);
            return nullptr;
        }
        seq->parts[i] = part;
        seq->numFrames += part->numFrames;
    }

    // First part takes ownership of the raw buffer.
    seq->parts[0]->ownedRawData = rawData;

    return seq;
}

// TransformFlip

TransformFlip::TransformFlip()
        : frame(0), frameReal(0), dirty(1), additiveTranslation(false), mirrored(false), anim(nullptr), tree(nullptr),
            mirroredJointOrderMap(nullptr) {}

TransformFlip::~TransformFlip() {
    // We don't own anim or tree - they are managed externally
}

// PSX: Attach__9tTreeFlipP5tTreeP14tTransformAnim (CHANNEL.CPP:953)
void TransformFlip::Attach(STreeData* t, TransformAnim* a) {
    dirty = 1;
    tree = t;
    anim = a;
}

// PSX: SetFrame__15tTransformFlip2i (CHANNEL.CPP:730)
void TransformFlip::SetFrame(s32 f) {
    frame = f;
    frameReal = f << 16;
}

// PSX: SetFrameReal__15tTransformFlip2l (CHANNEL.CPP:737)
void TransformFlip::SetFrameReal(s32 f) {
    frame = f >> 16;
    frameReal = f;
}

// PSX: Reset__15tTransformFlip2 (CHANNEL.CPP:744)
void TransformFlip::Reset() {
    dirty = 1;
    SetFrame(0);
    UpdateJoints();
}

// PSX: UpdateJoints__15tTransformFlip2P5tTree (CHANNEL.CPP:778)
void TransformFlip::UpdateJoints() {
    if (!anim || !tree || !tree->joints) {
        return;
    }

    if (mirrored) {
        UpdateJointsMirrored();
        return;
    }

    const u32* jointOrderMap = tree->jointOrderMap;

    if (anim->transChannels) {
        for (s32 i = 0; i < anim->numTransChannels; i++) {
            const TransformAnim::Channel& ch = anim->transChannels[i];
            if (!ch.chData) {
                continue;
            }
            u32 jointParam = ch.jointParam;
            if (!jointOrderMap || jointParam >= tree->numMapEntries) {
                continue;
            }
            u32 jointIdx = jointOrderMap[jointParam];
            if (jointIdx >= tree->numJoints) {
                continue;
            }
            EvalTransChannel(ch, tree->joints[jointIdx]);
        }
    }

    if (anim->rotChannels) {
        for (s32 i = 0; i < anim->numRotChannels; i++) {
            const TransformAnim::Channel& ch = anim->rotChannels[i];
            if (!ch.chData) {
                continue;
            }
            u32 jointParam = ch.jointParam;
            if (!jointOrderMap || jointParam >= tree->numMapEntries) {
                continue;
            }
            u32 jointIdx = jointOrderMap[jointParam];
            if (jointIdx >= tree->numJoints) {
                continue;
            }
            EvalRotChannel(ch, tree->joints[jointIdx]);
        }
    }

    dirty = 0;
}

// PSX: UpdateJointsMirrored__15tTransformFlip2P5tTree (CHANNEL.CPP:832)
// PSX passes a stack triple (tx,ty,tz) / (rx,ry,rz) by pointer to the eval function,
// which fills them; reflection is then applied to those temps before writing to joint.
void TransformFlip::UpdateJointsMirrored() {
    const u32* jointOrderMap = mirroredJointOrderMap ? mirroredJointOrderMap : tree->jointOrderMap;

    if (anim->transChannels) {
        for (s32 i = 0; i < anim->numTransChannels; i++) {
            const TransformAnim::Channel& ch = anim->transChannels[i];
            if (!ch.chData) {
                continue;
            }
            u32 jointParam = ch.jointParam;
            if (!jointOrderMap || jointParam >= tree->numMapEntries) {
                continue;
            }
            u32 jointIdx = jointOrderMap[jointParam];
            if (jointIdx >= tree->numJoints) {
                continue;
            }
            STreeJoint& joint = tree->joints[jointIdx];
            STreeJoint tmp = joint;
            EvalTransChannel(ch, tmp);

            const bool lastOfParam = (i + 1 >= anim->numTransChannels)
                || (anim->transChannels[i + 1].jointParam != jointParam);
            if (lastOfParam) {
                if (jointParam == 0) {
                    tmp.translationX = -tmp.translationX;
                }
                else if (jointParam == 1) {
                    tmp.translationZ = -tmp.translationZ;
                }
                else {
                    tmp.translationY = -tmp.translationY;
                }
                joint.translationX = tmp.translationX;
                joint.translationY = tmp.translationY;
                joint.translationZ = tmp.translationZ;
            } else {
                joint.translationX = tmp.translationX;
                joint.translationY = tmp.translationY;
                joint.translationZ = tmp.translationZ;
            }
        }
    }

    if (anim->rotChannels) {
        for (s32 i = 0; i < anim->numRotChannels; i++) {
            const TransformAnim::Channel& ch = anim->rotChannels[i];
            if (!ch.chData) {
                continue;
            }
            u32 jointParam = ch.jointParam;
            if (!jointOrderMap || jointParam >= tree->numMapEntries) {
                continue;
            }
            u32 jointIdx = jointOrderMap[jointParam];
            if (jointIdx >= tree->numJoints) {
                continue;
            }
            STreeJoint& joint = tree->joints[jointIdx];
            STreeJoint tmp = joint;
            EvalRotChannel(ch, tmp);

            const bool lastOfParam = (i + 1 >= anim->numRotChannels)
                || (anim->rotChannels[i + 1].jointParam != jointParam);
            if (lastOfParam) {
                if (jointParam == 0) {
                    tmp.rotationY = (s16)(2 * (u16)joint.restPoseRotY - (u16)tmp.rotationY);
                    tmp.rotationZ = (s16)(2 * (u16)joint.restPoseRotZ - (u16)tmp.rotationZ);
                }
                else if (jointParam == 1) {
                    tmp.rotationX = (s16)(2 * (u16)joint.restPoseRotX - (u16)tmp.rotationX);
                    tmp.rotationY = (s16)(2 * (u16)joint.restPoseRotY - (u16)tmp.rotationY);
                }
                else {
                    tmp.rotationX = (s16)-tmp.rotationX;
                    tmp.rotationZ = (s16)-tmp.rotationZ;
                }
                joint.rotationX = tmp.rotationX;
                joint.rotationY = tmp.rotationY;
                joint.rotationZ = tmp.rotationZ;
            } else {
                joint.rotationX = tmp.rotationX;
                joint.rotationY = tmp.rotationY;
                joint.rotationZ = tmp.rotationZ;
            }
        }
    }

    dirty = 0;
}

// Find the bracket index for dynamic key lists using 16.16 frame time.
// PSX stores dynamic key times as u8 frame values.
s32 TransformFlip::FindBracket(const u8* rawBase, u32 keyTimesOff, s32 numKeys, s32 frameReal) {
    // Key times are stored as u8 array at rawBase + keyTimesOff * 4
    u32 byteOff = keyTimesOff * 4;
    const u8* times = rawBase + byteOff;

    // Check bounds
    s32 firstTimeReal = GetUnwrappedTime(times, 0) << 16;
    if (frameReal <= firstTimeReal) {
        return 0;
    }

    s32 lastTimeReal = GetUnwrappedTime(times, numKeys - 1) << 16;
    if (frameReal >= lastTimeReal) {
        return numKeys - 1;
    }

    // Linear search for bracket. Match PSX edge handling: exact key boundary
    // advances to next key. The unwrapped key times are produced in the same
    // pass (same wrap rule as GetUnwrappedTime) instead of rescanning the
    // list from key 0 for every candidate, which made this O(numKeys^2) per
    // channel and showed up as ~8% of a PSP frame.
    s32 wrap = 0;
    s32 prevRaw = times[0];
    s32 t0Real = firstTimeReal;
    for (s32 i = 0; i < numKeys - 1; i++) {
        const s32 currRaw = times[i + 1];
        if (currRaw < prevRaw) {
            wrap += 256;
        }
        prevRaw = currRaw;
        const s32 t1Real = (currRaw + wrap) << 16;
        if (frameReal >= t0Real && frameReal < t1Real) {
            return i;
        }
        if (frameReal == t1Real) {
            return i + 1;
        }
        t0Real = t1Real;
    }

    return numKeys - 1;
}

void TransformFlip::EvalRotChannel(const TransformAnim::Channel& ch, STreeJoint& joint) {
    const u8* data = ch.chData;
    const u8* raw = ch.rawBase;
    u32 rawSize = ch.rawSize;

    if (ch.keyType == KEY_STATIC_3DOF_ANGLE) {
        // tStatic3DOFKeyList: constant values at +8, +12, +16
        joint.rotationX = (s16)(s32)p3dReadU32LE(data + 8);
        joint.rotationY = (s16)(s32)p3dReadU32LE(data + 12);
        joint.rotationZ = (s16)(s32)p3dReadU32LE(data + 16);
    } else if (ch.keyType == KEY_JOINT_3DOF_ANGLE) {
        // tJoint3DOFangle: packed rotation values
        // +8: numKeys, +12: keyTimesOff, +16: keyValuesOff
        u32 numKeys = p3dReadU32LE(data + 8);
        if (numKeys == 0) {
            return;
        }
        u32 keyTimesOff = p3dReadU32LE(data + 12);
        u32 keyValsOff = p3dReadU32LE(data + 16);
        u32 keyValsByteOff = keyValsOff * 4;

        auto decodePacked = [](u32 packed, s32& rx, s32& ry, s32& rz) {
            // PSX CHANNEL.CPP packs 3DOF angles with 32-step quantization across axes.
            rx = (s16)((packed << 5) & 0xFFFF);
            ry = (s16)((packed >> 6) & 0xFFE0);
            rz = (s16)((packed >> 16) & 0xFFC0);
        };

        if (numKeys == 1) {
            s32 bracket = FindBracket(raw, keyTimesOff, (s32)numKeys, frameReal);
            u32 packed = p3dReadU32LE(raw + keyValsByteOff + bracket * 4);
            s32 rx, ry, rz;
            decodePacked(packed, rx, ry, rz);
            joint.rotationX = (s16)rx;
            joint.rotationY = (s16)ry;
            joint.rotationZ = (s16)rz;
            return;
        }

        // Multi-key interpolation
        s32 bracket = FindBracket(raw, keyTimesOff, (s32)numKeys, frameReal);

        if (bracket >= (s32)numKeys - 1) {
            // At or past last key
            u32 packed = p3dReadU32LE(raw + keyValsByteOff + (numKeys - 1) * 4);
            s32 rx, ry, rz;
            decodePacked(packed, rx, ry, rz);
            joint.rotationX = (s16)rx;
            joint.rotationY = (s16)ry;
            joint.rotationZ = (s16)rz;
            return;
        }

        // Unpack both bracket keyframes
        u32 packed0 = p3dReadU32LE(raw + keyValsByteOff + bracket * 4);
        u32 packed1 = p3dReadU32LE(raw + keyValsByteOff + (bracket + 1) * 4);

        s32 rx0, ry0, rz0;
        s32 rx1, ry1, rz1;
        decodePacked(packed0, rx0, ry0, rz0);
        decodePacked(packed1, rx1, ry1, rz1);

        // Get bracket key times for interpolation fraction
        u32 timesByteOff = keyTimesOff * 4;
        const u8* times = raw + timesByteOff;
        s32 t0 = GetUnwrappedTime(times, bracket);
        s32 t1 = GetUnwrappedTime(times, bracket + 1);
        s32 timeDelta = t1 - t0;
        if (timeDelta <= 0) {
            joint.rotationX = (s16)rx0;
            joint.rotationY = (s16)ry0;
            joint.rotationZ = (s16)rz0;
            return;
        }

        // Fraction in 16.16: (frameReal - t0<<16) / (timeDelta<<16)
        s32 frameDelta = frameReal - ((s32)t0 << 16);
        s32 frac16 = (s32)(((s64)frameDelta << 16) / ((s64)timeDelta << 16));

        // Lerp: v0 + (v1 - v0) * frac16 >> 16
        joint.rotationX = LerpAngle16((s16)rx0, (s16)rx1, frac16);
        joint.rotationY = LerpAngle16((s16)ry0, (s16)ry1, frac16);
        joint.rotationZ = LerpAngle16((s16)rz0, (s16)rz1, frac16);
    } else if (ch.keyType == KEY_JOINT_1DOF_ANGLE) {
        // tJoint1DOFangle: single axis rotation
        // +8=numKeys, +12=keyTimesOff(u8 array), +16=dofIndex, +20=keyValuesOff(s16 array)
        u32 numKeys = p3dReadU32LE(data + 8);
        if (numKeys == 0) {
            return;
        }
        u32 keyTimesOff = p3dReadU32LE(data + 12);
        u32 dofIndex = p3dReadU32LE(data + 16);
        u32 keyValsOff = p3dReadU32LE(data + 20);
        u32 keyTimesByteOff = keyTimesOff * 4;
        u32 keyValsByteOff = keyValsOff * 4;

        // Key times are u8, find bracket by frameReal
        const u8* times = raw + keyTimesByteOff;
        s32 bracket = FindBracket(raw, keyTimesOff, (s32)numKeys, frameReal);

        s16 val;
        if (numKeys == 1 || bracket >= (s32)numKeys - 1) {
            val = p3dReadS16LE(raw + keyValsByteOff + bracket * 2);
        } else {
            s32 t0 = GetUnwrappedTime(times, bracket);
            s32 t1 = GetUnwrappedTime(times, bracket + 1);
            s32 timeDelta = t1 - t0;
            if (timeDelta <= 0) {
                val = p3dReadS16LE(raw + keyValsByteOff + bracket * 2);
            } else {
                s16 v0 = p3dReadS16LE(raw + keyValsByteOff + bracket * 2);
                s16 v1 = p3dReadS16LE(raw + keyValsByteOff + (bracket + 1) * 2);
                s32 frameDelta = frameReal - (t0 << 16);
                s32 frac16 = (s32)(((s64)frameDelta << 16) / ((s64)timeDelta << 16));
                val = LerpAngle16(v0, v1, frac16);
            }
        }

        if (dofIndex == DOF_AXIS_X) {
            joint.rotationX = val;
        } else if (dofIndex == DOF_AXIS_Y) {
            joint.rotationY = val;
        } else {
            joint.rotationZ = val;
        }
    }
}

void TransformFlip::EvalTransChannel(const TransformAnim::Channel& ch, STreeJoint& joint) {
    const u8* data = ch.chData;
    const u8* raw = ch.rawBase;
    u32 rawSize = ch.rawSize;

    auto writeTranslation = [&](s32 x, s32 y, s32 z) {
        joint.translationX = x;
        joint.translationY = y;
        joint.translationZ = z;
    };

    if (ch.keyType == KEY_STATIC_3DOF_POS) {
        // tStatic3DOFKeyList: constant values at +8, +12, +16
        writeTranslation(
            (s32)p3dReadU32LE(data + 8),
            (s32)p3dReadU32LE(data + 12),
            (s32)p3dReadU32LE(data + 16));
    } else if (ch.keyType == KEY_JOINT_3DOF_LP_PSX) {
        // tJoint3DOFlpPSX: 3DOF linear position
        // +8: numKeys, +12: keyTimesOff, +16: keyValuesOff (3 x s16 per key)
        u32 numKeys = p3dReadU32LE(data + 8);
        if (numKeys == 0) {
            return;
        }
        u32 keyTimesOff = p3dReadU32LE(data + 12);
        u32 keyValsOff = p3dReadU32LE(data + 16);
        u32 keyValsByteOff = keyValsOff * 4;

        if (numKeys == 1) {
            s32 bracket = FindBracket(raw, keyTimesOff, (s32)numKeys, frameReal);
            u32 vOff = keyValsByteOff + bracket * 6;
            if (vOff + 6 > rawSize) {
                return;
            }
            writeTranslation(
                (s32)p3dReadS16LE(raw + vOff + 0),
                (s32)p3dReadS16LE(raw + vOff + 2),
                (s32)p3dReadS16LE(raw + vOff + 4));
            return;
        }

        // Multi-key interpolation
        s32 bracket = FindBracket(raw, keyTimesOff, (s32)numKeys, frameReal);

        if (bracket >= (s32)numKeys - 1) {
            u32 vOff = keyValsByteOff + (numKeys - 1) * 6;
            if (vOff + 6 > rawSize) {
                return;
            }
            writeTranslation(
                (s32)p3dReadS16LE(raw + vOff + 0),
                (s32)p3dReadS16LE(raw + vOff + 2),
                (s32)p3dReadS16LE(raw + vOff + 4));
            return;
        }

        u32 vOff0 = keyValsByteOff + bracket * 6;
        u32 vOff1 = keyValsByteOff + (bracket + 1) * 6;
        if (vOff1 + 6 > rawSize) {
            return;
        }

        s32 x0 = (s32)p3dReadS16LE(raw + vOff0 + 0);
        s32 y0 = (s32)p3dReadS16LE(raw + vOff0 + 2);
        s32 z0 = (s32)p3dReadS16LE(raw + vOff0 + 4);
        s32 x1 = (s32)p3dReadS16LE(raw + vOff1 + 0);
        s32 y1 = (s32)p3dReadS16LE(raw + vOff1 + 2);
        s32 z1 = (s32)p3dReadS16LE(raw + vOff1 + 4);

        u32 timesByteOff = keyTimesOff * 4;
        const u8* times = raw + timesByteOff;
        s32 t0 = GetUnwrappedTime(times, bracket);
        s32 t1 = GetUnwrappedTime(times, bracket + 1);
        s32 timeDelta = t1 - t0;
        if (timeDelta <= 0) {
            writeTranslation(x0, y0, z0);
            return;
        }

        s32 frameDelta = frameReal - ((s32)t0 << 16);
        s32 frac16 = (s32)(((s64)frameDelta << 16) / ((s64)timeDelta << 16));

        writeTranslation(
            (s32)(x0 + (((x1 - x0) * (s64)frac16) >> 16)),
            (s32)(y0 + (((y1 - y0) * (s64)frac16) >> 16)),
            (s32)(z0 + (((z1 - z0) * (s64)frac16) >> 16)));
    }
}


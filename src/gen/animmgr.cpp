#include "gen/animmgr.h"

#include "gen/model.h"

#include <cstdlib>

static constexpr u16 kParamOoTable[64] = {
    0, 65535, 32768, 21845, 16384, 13107, 10922, 9362,
    8192, 7281, 6553, 5957, 5461, 5041, 4681, 4369,
    4096, 3855, 3640, 3449, 3276, 3120, 2978, 2849,
    2730, 2621, 2520, 2427, 2340, 2259, 2184, 2114,
    2048, 1985, 1927, 1872, 1820, 1771, 1724, 1680,
    1638, 1598, 1560, 1524, 1489, 1456, 1424, 1394,
    1365, 1337, 1310, 1285, 1260, 1236, 1213, 1191,
    1170, 1149, 1129, 1110, 1092, 1074, 1057, 1040,
};

AnimationManager* g_animMgr = nullptr;

SequenceAnim::~SequenceAnim() {
    delete[] parts;
    parts = nullptr;
    numParts = 0;
    numFrames = 0;
}

TransformAnim* SequenceAnim::ResolvePartAnim(s32 index) {
    if (!parts || index < 0 || static_cast<u32>(index) >= numParts) {
        return nullptr;
    }

    SequenceAnimPart& part = parts[index];
    if (!part.node && part.animHash != 0 && g_animMgr) {
        part.node = g_animMgr->GetMiscAnim(part.animHash);
    }

    if (!part.anim && part.node) {
        part.anim = part.node->anim;
    }

    return part.anim;
}

FrameListAnim::~FrameListAnim() {
    delete[] frameOffsets;
    frameOffsets = nullptr;

    if (rawData) {
        std::free(rawData);
        rawData = nullptr;
    }

    rawSize = 0;
    numFrames = 0;
}

const s16* FrameListAnim::GetFrame(s32 index) const {
    if (!rawData || !frameOffsets || index < 0 || index >= numFrames) {
        return nullptr;
    }

    const u32 offset = frameOffsets[index];
    if (offset >= rawSize) {
        return nullptr;
    }

    return reinterpret_cast<const s16*>(rawData + offset);
}

u32 FrameListAnim::GetFrameVertexCount(s32 index) const {
    if (!rawData || !frameOffsets || index < 0 || index >= numFrames) {
        return 0;
    }

    const u32 start = frameOffsets[index];
    if (start >= rawSize) {
        return 0;
    }

    u32 end = rawSize;
    for (s32 i = 0; i < numFrames; i++) {
        const u32 off = frameOffsets[i];
        if (off > start && off < end) {
            end = off;
        }
    }

    if (end <= start) {
        return 0;
    }

    return (end - start) / 8u;
}

VizAnim::~VizAnim() {
    delete[] nodes;
    nodes = nullptr;

    if (rawData) {
        std::free(rawData);
        rawData = nullptr;
    }

    rawSize = 0;
    numNodes = 0;
    numFrames = 0;
}

bool VizAnim::IsNodeVisible(u32 nodeIndex, s32 frame) const {
    if (!nodes || nodeIndex >= numNodes || frame < 0) {
        return false;
    }

    const VizAnimNodeEntry& node = nodes[nodeIndex];
    if (!node.bitWords) {
        return false;
    }

    if (numFrames <= 0 || frame >= numFrames) {
        return false;
    }

    const u32 frameWord = static_cast<u32>(frame) >> 5;
    const u32 wordCount = (static_cast<u32>(numFrames) + 31u) >> 5;
    if (frameWord >= wordCount) {
        return false;
    }

    const u32 frameBit = static_cast<u32>(frame) & 31u;
    const u32 mask = 1u << frameBit;
    return (node.bitWords[frameWord] & mask) != 0u;
}

ParamAnimData::~ParamAnimData() {
    delete[] keyTimes;
    keyTimes = nullptr;

    delete[] keyValues;
    keyValues = nullptr;

    keyCount = 0;
    numFrames = 0;
}

s32 ParamAnimData::EvaluateFrameReal(s32 frameReal) const {
    if (!keyTimes || !keyValues || keyCount == 0) {
        return 0;
    }

    if (keyCount == 1) {
        return keyValues[0];
    }

    if (frameReal <= (static_cast<s32>(keyTimes[0]) << 16)) {
        return keyValues[0];
    }

    u32 keyIndex = keyCount - 1;
    for (u32 i = 0; i + 1 < keyCount; i++) {
        const s32 t0 = static_cast<s32>(keyTimes[i]) << 16;
        const s32 t1 = static_cast<s32>(keyTimes[i + 1]) << 16;
        if (frameReal >= t0 && frameReal <= t1) {
            keyIndex = i;
            break;
        }
    }

    if (keyIndex >= keyCount - 1) {
        return keyValues[keyCount - 1];
    }

    const s32 t0 = static_cast<s32>(keyTimes[keyIndex]) << 16;
    const s32 t1 = static_cast<s32>(keyTimes[keyIndex + 1]) << 16;
    const s32 v0 = keyValues[keyIndex];
    const s32 v1 = keyValues[keyIndex + 1];

    if (frameReal <= t0) {
        return v0;
    }
    if (frameReal >= t1) {
        return v1;
    }

    s32 deltaFrames = static_cast<s32>(keyTimes[keyIndex + 1]) - static_cast<s32>(keyTimes[keyIndex]);
    if (deltaFrames < 0 || deltaFrames >= 64) {
        deltaFrames = 0;
    }

    const s32 oo = static_cast<s32>(kParamOoTable[deltaFrames]);
    const s32 blend = static_cast<s32>((static_cast<s64>(oo) * static_cast<s64>(frameReal - t0)) >> 16);
    return v0 + static_cast<s32>((static_cast<s64>(blend) * static_cast<s64>(v1 - v0)) >> 16);
}

CBVParamAnimData::~CBVParamAnimData() {
    delete[] values;
    values = nullptr;

    delete[] primOffsets;
    primOffsets = nullptr;

    valueRows = 0;
    valueCols = 0;
    numEntries = 0;
}

UVListAnim::~UVListAnim() {
    delete[] uvData;
    uvData = nullptr;
    delete[] packetOffsets;
    packetOffsets = nullptr;
    delete[] sortedPerm;
    sortedPerm = nullptr;
    numFrames = 0;
    numEntries = 0;
    numPacketOffsets = 0;
}

u16 UVListAnim::GetUV(s32 frame, s32 entry) const {
    if (!uvData || frame < 0 || entry < 0 || entry >= numEntries) {
        return 0;
    }
    if (frame >= numFrames) {
        frame = numFrames - 1;
    }
    return uvData[frame * numEntries + entry];
}

ClutAnimData::~ClutAnimData() {
    delete[] frames;
    frames = nullptr;

    delete[] offsets;
    offsets = nullptr;

    numFrames = 0;
    numOffsets = 0;
}

u16 ClutAnimData::GetFrameValue(s32 frame) const {
    if (!frames || numFrames <= 0) {
        return 0;
    }

    if (frame < 0) {
        frame = 0;
    }
    else if (frame >= numFrames) {
        frame = numFrames - 1;
    }

    return frames[frame];
}

MiscAnimNode::~MiscAnimNode() {
    if (!ownsSubs) return;
    if (animSequence) {
        // anim aliases animSequence->parts[0]; the sequence owns it.
        delete animSequence;
        animSequence = nullptr;
        anim = nullptr;
    }
    else if (anim) {
        delete anim;
        anim = nullptr;
    }

    if (paramAnim) {
        delete paramAnim;
        paramAnim = nullptr;
    }

    if (frameList) {
        delete frameList;
        frameList = nullptr;
    }

    if (compositeAnim) {
        delete compositeAnim;
        compositeAnim = nullptr;
    }

    if (sequenceAnim) {
        delete sequenceAnim;
        sequenceAnim = nullptr;
    }

    if (vizAnim) {
        delete vizAnim;
        vizAnim = nullptr;
    }

    if (cbvParamAnim) {
        delete cbvParamAnim;
        cbvParamAnim = nullptr;
    }

    if (clutAnim) {
        delete clutAnim;
        clutAnim = nullptr;
    }

    if (uvListAnim) {
        delete uvListAnim;
        uvListAnim = nullptr;
    }
}

// PSX: __16AnimationManager (ANIMMGR.CPP:147, 0x80057174)
AnimationManager::AnimationManager() {
    MARKFUNCTION(0x80057168);
    g_animMgr = this;
}

// PSX: _._16AnimationManager (ANIMMGR.CPP:)
AnimationManager::~AnimationManager() {
    MARKFUNCTION(0x80057174);
    PurgeLevel();
    if (g_animMgr == this) {
        g_animMgr = nullptr;
    }
}

// PSX: InternalOpen__16AnimationManager (ANIMMGR.CPP:152, 0x800571CC)
void AnimationManager::InternalOpen() {
    MARKFUNCTION(0x800571CC);
}

// PSX: InternalClose__16AnimationManager (ANIMMGR.CPP:157, 0x800571D4)
void AnimationManager::InternalClose() {
    MARKFUNCTION(0x800571D4);
    PurgeLevel();
}

// PSX: InternalReset__16AnimationManager (ANIMMGR.CPP:162, 0x80057228)
void AnimationManager::InternalReset() {
    MARKFUNCTION(0x80057228);
}

// PSX: PurgePetal__16AnimationManager (ANIMMGR.CPP:168, 0x80057230)
void AnimationManager::PurgePetal() {
    MARKFUNCTION(0x80057230);
    ccMinNode* node = animList.head;
    while (node) {
        MiscAnimNode* an = static_cast<MiscAnimNode*>(node);
        ccMinNode* next = node->next;
        if (an->type == 2) {
            animList.RemNode(node);
            delete an;
            ++generation;
        }
        node = next;
    }
}

// PSX: PurgeLevel__16AnimationManager (ANIMMGR.CPP:185, 0x800572B4)
void AnimationManager::PurgeLevel() {
    MARKFUNCTION(0x800572B4);
    while (ccMinNode* n = animList.RemHead()) {
        delete n;
    }
    ++generation;
}

// PSX: GetMiscAnim__16AnimationManagerUl (ANIMMGR.CPP:191, 0x80057308)
MiscAnimNode* AnimationManager::GetMiscAnim(u32 hash) {
    MARKFUNCTION(0x80057308);
    for (ccMinNode* n = animList.head; n; n = n->next) {
        MiscAnimNode* an = static_cast<MiscAnimNode*>(n);
        if (an->hash == hash) {
            return an;
        }
    }
    return nullptr;
}

// PSX: AddNodeTail used from level loading code
void AnimationManager::AddAnim(MiscAnimNode* node) {
    animList.AddNodeTail(node);
    ++generation;
}

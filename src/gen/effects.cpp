#include "common.h"
#include "gen/effects.h"
#include "p3d/lvector.h"
#if defined(RC_PLATFORM_PSP)
#include "psp/psp_perf.h"
#endif

#include <vector>

u32 g_wEffectGlobalFrame = 0;

static ccMinList g_effectsList;
static constexpr s32 kEffectsDefaultResult = static_cast<s32>(0x800E0000);

static bool IsDirectorOverlayRoute(const Effects* effect) {
    if (!effect) {
        return false;
    }

    const s32 type = effect->effectType;
    if (type == 1 || type == 4 || type == 7 || type == 2 || type == 3 || type == 5) {
        return effect->IsDirectorOverlay();
    }

    return false;
}

Effects::Effects() {
    MARKFUNCTION(0x8004CA5C);
}

Effects::~Effects() {
    MARKFUNCTION(0x8004CA98);
}

s32 Effects::Update() {
    return 0;
}

void Effects::Display(s32 /*blockNum*/) {
}

s32 Effects::Create() {
    return 0;
}

s32 Effects::PutBackEffect() {
    return 0;
}

bool Effects::IsDirectorOverlay() const {
    return false;
}

bool Effects::GetDebugWorldPos(LVector* /*outPos*/) const {
    return false;
}

s32 Effects_Die(s32 blockNum, s32 effectType) {
    MARKFUNCTION(0x8004C6C8);

    s32 result = kEffectsDefaultResult;

    for (ccMinNode* node = g_effectsList.head; node;) {
        ccMinNode* next = node->next;
        Effects* effect = static_cast<Effects*>(static_cast<ccNode*>(node));

        result = effect->effectType;

        if (result == effectType) {
            result = effect->blockNum;
            if (result == blockNum || blockNum == -1) {
                Effects_RemoveEffect(effect);
                result = effect->PutBackEffect();
            }
        }

        node = next;
    }

    return result;
}

Effects* Effects_Find(s32 effectType, u32 effectHash) {
    MARKFUNCTION(0x8004C778);

    for (ccMinNode* node = g_effectsList.head; node; node = node->next) {
        Effects* effect = static_cast<Effects*>(static_cast<ccNode*>(node));
        if (effect->effectType == effectType && effect->nameCRC == effectHash) {
            return effect;
        }
    }

    return nullptr;
}

s32 Effects_UnloadAll() {
    MARKFUNCTION(0x8004C7D8);

    s32 result = kEffectsDefaultResult;

    for (ccMinNode* node = g_effectsList.head; node;) {
        ccMinNode* next = node->next;
        Effects* effect = static_cast<Effects*>(static_cast<ccNode*>(node));

        Effects_RemoveEffect(effect);
        result = effect->PutBackEffect();
        node = next;
    }

    g_effectsList.head = nullptr;
    g_effectsList.tail = nullptr;
    return result;
}

s32 Effects_UpdateAll() {
    MARKFUNCTION(0x8004C854);

    s32 result = kEffectsDefaultResult;

    if (!g_effectsList.head) {
        return result;
    }

    result = static_cast<s32>(++g_wEffectGlobalFrame);

    for (ccMinNode* node = g_effectsList.head; node;) {
        ccMinNode* next = node->next;
        Effects* effect = static_cast<Effects*>(static_cast<ccNode*>(node));
        result = effect->Update();
        node = next;
    }

    return result;
}

#if defined(RC_PLATFORM_PSP)
namespace {
constexpr u32 kEffectBucketMaxBlocks = 16;
constexpr u32 kEffectBucketNoSlot = 0xFFFFFFFFu;

struct EffectBlockBuckets {
    std::vector<Effects*> effects;
    u32 start[kEffectBucketMaxBlocks + 1] = {};
    s32 slotBlockNum[kEffectBucketMaxBlocks] = {};
    u32 slotCount = 0;
    bool ready = false;
};

EffectBlockBuckets s_effectBuckets;

u32 FindEffectBucketSlot(s32 blockNum) {
    for (u32 slot = 0; slot < s_effectBuckets.slotCount; ++slot) {
        if (s_effectBuckets.slotBlockNum[slot] == blockNum) return slot;
    }
    return kEffectBucketNoSlot;
}
}  // namespace

void Effects_BeginBlockBuckets(const s32* blockNums, u32 blockCount) {
    EffectBlockBuckets& buckets = s_effectBuckets;
    buckets.ready = false;
    buckets.slotCount = 0;
    for (u32 i = 0; i < blockCount; ++i) {
        // 4096 is the director overlay route; never bucket it.
        if (blockNums[i] == 4096) continue;
        if (FindEffectBucketSlot(blockNums[i]) != kEffectBucketNoSlot) continue;
        if (buckets.slotCount >= kEffectBucketMaxBlocks) {
            buckets.slotCount = 0;
            return;
        }
        buckets.slotBlockNum[buckets.slotCount++] = blockNums[i];
    }

    const u32 slotCount = buckets.slotCount;
    for (u32 slot = 0; slot <= slotCount; ++slot) buckets.start[slot] = 0;
    for (ccMinNode* node = g_effectsList.head; node; node = node->next) {
        const u32 slot = FindEffectBucketSlot(static_cast<Effects*>(static_cast<ccNode*>(node))->blockNum);
        if (slot != kEffectBucketNoSlot) ++buckets.start[slot + 1u];
    }
    for (u32 slot = 0; slot < slotCount; ++slot) buckets.start[slot + 1u] += buckets.start[slot];

    buckets.effects.resize(buckets.start[slotCount]);
    u32 cursor[kEffectBucketMaxBlocks];
    for (u32 slot = 0; slot < slotCount; ++slot) cursor[slot] = buckets.start[slot];
    for (ccMinNode* node = g_effectsList.head; node; node = node->next) {
        Effects* effect = static_cast<Effects*>(static_cast<ccNode*>(node));
        const u32 slot = FindEffectBucketSlot(effect->blockNum);
        if (slot != kEffectBucketNoSlot) buckets.effects[cursor[slot]++] = effect;
    }
    buckets.ready = true;
}

void Effects_EndBlockBuckets() {
    s_effectBuckets.ready = false;
}
#endif

s32 Effects_DrawEffects(s32 blockNum) {
    MARKFUNCTION(0x8004C8BC);

    s32 result = kEffectsDefaultResult;

#if defined(RC_PLATFORM_PSP)
    const u32 bucketSlot = s_effectBuckets.ready ? FindEffectBucketSlot(blockNum) : kEffectBucketNoSlot;
    if (bucketSlot != kEffectBucketNoSlot) {
        // Same filter as the full walk below, restricted to the effects whose
        // blockNum matched this block when the frame's buckets were built.
        // Effects removed from the list since then are skipped like before.
        const u32 end = s_effectBuckets.start[bucketSlot + 1u];
        for (u32 index = s_effectBuckets.start[bucketSlot]; index < end; ++index) {
            Effects* effect = s_effectBuckets.effects[index];
            if (!effect->inEffectsList || effect->debugDisabled) {
                continue;
            }
            if (IsDirectorOverlayRoute(effect)) {
                result = 4096;
                continue;
            }
            result = effect->blockNum;
            if (result == blockNum) {
                effect->Display(blockNum);
            }
        }
        return result;
    }
#endif

    for (ccMinNode* node = g_effectsList.head; node; node = node->next) {
        Effects* effect = static_cast<Effects*>(static_cast<ccNode*>(node));
#if defined(RC_PLATFORM_PSP)
        if (blockNum == 4096) PspPerf::CountRenderer(PspPerf::RendererCounter::OverlayEffectWalk);
#endif
        if (effect->debugDisabled) {
            continue;
        }

        if (IsDirectorOverlayRoute(effect)) {
            result = 4096;
            if (blockNum == 4096) {
#if defined(RC_PLATFORM_PSP)
                PspPerf::CountRenderer(PspPerf::RendererCounter::OverlayEffectDraws);
#endif
                effect->Display(blockNum);
            }
        }
        else {
            result = effect->blockNum;
            if (result == blockNum) {
                effect->Display(blockNum);
            }
        }
    }

    return result;
}

s32 Effects_AddEffect(Effects* effect, s32 addToHead) {
    MARKFUNCTION(0x8004C9D8);

    if (!effect) {
        return 0;
    }
    if (effect->inEffectsList) {
        return 1;
    }

    ccMinNode* after = addToHead ? nullptr : g_effectsList.tail;
    g_effectsList.AddNode(after, effect);
    effect->inEffectsList = 1;
    return 1;
}

Effects* Effects_RemoveEffect(Effects* effect) {
    MARKFUNCTION(0x8004CA28);

    if (!effect || !effect->inEffectsList) {
        return effect;
    }

    g_effectsList.RemNode(effect);
    effect->inEffectsList = 0;
    return effect;
}

s32 Effects_DebugGetActive(Effects** outEffects, s32 maxCount) {
    s32 count = 0;

    for (ccMinNode* node = g_effectsList.head; node; node = node->next) {
        Effects* effect = static_cast<Effects*>(static_cast<ccNode*>(node));
        if (outEffects && count < maxCount) {
            outEffects[count] = effect;
        }
        count++;
    }

    return count;
}

#pragma once

#include "gen/cclist.h"

struct LVector;

class Effects : public ccNode {
public:
    Effects();
    ~Effects() override;

    virtual s32 Update();
    virtual void Display(s32 blockNum);
    virtual s32 Create();
    virtual s32 PutBackEffect();
    virtual bool IsDirectorOverlay() const;
    virtual bool GetDebugWorldPos(LVector* outPos) const;

    s32 effectType = 0;
    s32 blockNum = -1;
    s32 inEffectsList = 0;

    // DebugUI-only: when set, Effects_DrawEffects skips this instance's
    // Display() call so its rendering can be isolated for inspection.
    bool debugDisabled = false;
};

// PSX: Die__7Effectsii (EFFECTS.CPP:60, 0x8004C6C8)
s32 Effects_Die(s32 blockNum, s32 effectType);

// PSX: Find__7EffectslUl (EFFECTS.CPP:106, 0x8004C778)
Effects* Effects_Find(s32 effectType, u32 effectHash);

// PSX: UnloadAll__7Effects (EFFECTS.CPP:137, 0x8004C7D8)
s32 Effects_UnloadAll();

// PSX: UpdateAll__7Effects (EFFECTS.CPP:174, 0x8004C854)
s32 Effects_UpdateAll();

// PSX: DrawEffects__7Effectsi (EFFECTS.CPP:207, 0x8004C8BC)
s32 Effects_DrawEffects(s32 blockNum);

#if defined(RC_PLATFORM_PSP)
// PSP: World pass 2 calls Effects_DrawEffects once per visible block, which
// walked the whole effect list each time. Between Begin/End the list is
// grouped once by block (list order preserved per block) and
// Effects_DrawEffects(blockNum) for a grouped block visits only its bucket,
// applying the same per-effect checks. Other block numbers use the full walk.
void Effects_BeginBlockBuckets(const s32* blockNums, u32 blockCount);
void Effects_EndBlockBuckets();
#endif

// PSX: AddEffect__7Effectsi (EFFECTS.CPP:279, 0x8004C9D8)
s32 Effects_AddEffect(Effects* effect, s32 addToHead);

// PSX: RemoveEffect__7Effects (EFFECTS.CPP:299, 0x8004CA28)
Effects* Effects_RemoveEffect(Effects* effect);

// Debug helper: copies pointers to currently active effects.
// Returns total active count; outEffects may be null to query count only.
s32 Effects_DebugGetActive(Effects** outEffects, s32 maxCount);

// PSX global frame counter used by WEffect vertex animation paths.
extern u32 g_wEffectGlobalFrame;

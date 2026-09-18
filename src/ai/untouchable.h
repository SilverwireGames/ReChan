#pragma once
#include "ai/obstacle.h"

class ParticleSystemMgr;
class CParticleEffectSound;

class Untouchable : public Obstacle {
public:
    // PSX +116 (ptr): ParticleSystemMgr* (allocated in ctor, released in dtor, a1[29])
    ParticleSystemMgr* particleMgr = nullptr;
    // PSX +120..+128: cached effect position
    s32 effectPosX = 0;
    s32 effectPosY = 0;
    s32 effectPosZ = 0;
    // PSX +132 (s32): pending particle creation flag
    s32 pendingCreate = 0;
    // PSX +136 (s32): hit-point damage amount (default 2, from attrib 6)
    s32 damageType = 2;
    // PSX +140 (s32): cooldown/reset timer value (default 3, from attrib 7)
    s32 damageValue = 3;
    // PSX +144 (s32): countdown timer (init = damageValue in Reset)
    s32 countdownTimer = 0;
    // PSX +148 (s32): active flag
    s32 field148 = 0;
    // PSX +152 (ptr): CParticleEffectSound*
    CParticleEffectSound* soundPtr = nullptr;

    Untouchable(const LVector* pos, u16 type);
    ~Untouchable() override;

    void AnalyzeMesh(DBRoot* root) override;
    void CreateModel(const char* name) override;
    void DeleteModel() override;
    void Reset() override;
    void Think() override;
    void Draw() override;
    void UpdatePosition() override;
    void HandlePickupCollision(Thing* pickup) override;
    void HandleHumanoidCollision(Humanoid* hum) override;

private:
    void DrawLate();
    s32 CreateSound();
    s32 UpdateSound();
    s32 ReleaseSound();
};

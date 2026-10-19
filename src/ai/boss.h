#pragma once

#include "ai/humanoid.h"
#include "gen/path.h"

class Boss : public Humanoid {
public:
    Boss(const LVector* initialPos, u16 type);
    ~Boss() override;

    void CreateModel(const char* name) override;
    void SetActionState(u32 state, s32 param) override;

    void _Collapse() override;
    void _CrouchUp();
    s32 TestAndSetBackGrab() override;
};

class Butch : public Boss {
public:
    Butch(const LVector* initialPos);
    ~Butch() override;

    void SetActionState(u32 state, s32 param) override;

    void _Stomp() override;
    void _Charge() override;
    void _ThrowPot() override;
};

class Grontar : public Boss {
public:
    Grontar(const LVector* initialPos);
    ~Grontar() override;

    void SetActionState(u32 state, s32 param) override;
    void ProcessAction() override;

    void _Stand() override;
    void _Run() override;
    void _Taunt() override;
    void _Straif() override;
    void _DiveRoll() override;
    void _GotHitHigh() override;
    void _GotHitMed() override;
    Humanoid* FindFoe(u32 range, s32 param, s32 immediate) override;
    s32 GetTargetingFrame(const PsxFightingMoveRaw* move) const override;
};

class Trails;

class Dante : public Boss {
public:
    Dante(const LVector* initialPos);
    ~Dante() override;

    void AnalyzeMesh(DBRoot* root) override;
    void Think() override;
    void SetActionState(u32 state, s32 param) override;

    void _Stand() override;
    void _Taunt() override;
    void _GotHitHigh() override;
    void _GotHitMed() override;
    void _GotHitFreeForm();
    s32 _ThrowFreeFall();

    s32 LoadCombatDialog() override;
    void PlayCombatKnockDownDialog(s32 damageType) override;
    s32 GetTargetingFrame(const PsxFightingMoveRaw* move) const override;
    void PlayCombatThrowDialog() override;

    void _MissilePrepare() override;
    void _MissileAttack() override;
    void _TargetMissileAttack() override;

    s32 missileFrameIndex = 0;
    s32 missilePatternIndex = 0;

    LinearPath** missilePaths = nullptr;
    s32 missilePathCount = 0;

    Trails* missileTrail0 = nullptr;
    Trails* missileTrail1 = nullptr;
    Trails* missileTrail2 = nullptr;
    Trails* missileTrail3 = nullptr;
    Trails* missileTrail4 = nullptr;
    Trails* missileTrail5 = nullptr;

    s32 field290 = 0;
    s32 field294 = 0;
    s32 missileRecoveryCounter = 0;
    s32 targetMissilePrimed = 0;

    LVector targetMissilePos = {};
};

class Paul : public Boss {
public:
    Paul(const LVector* initialPos);
    ~Paul() override;

    void SetActionState(u32 state, s32 param) override;

    void _Straif() override;
    void _GotHitHigh() override;
    void _GotHitMed() override;
};

class Oscar : public Boss {
public:
    Oscar(const LVector* initialPos);
    ~Oscar() override;

    void SetActionState(u32 state, s32 param) override;

    void _Straif() override;
};

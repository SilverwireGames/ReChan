#include "ai/untouchable.h"
#include "gen/common.h"
#include "gen/colvol.h"
#include "gen/database.h"
#include "gen/geffect.h"
#include "gen/particle.h"
#include "gen/shadows.h"
#include "ai/humanoid.h"
#include "ai/obstacle_shared.h"

#include "p3d/context.h"
#include "p3d/p3dmath.h"
#include "snd/esound.h"
#include "snd/sndfact.h"

static constexpr u32 UNTOUCHABLE_EFFECT_HASH = 0x039DD3FDu;

static const LVector s_untouchableLedgeEffectScale = { 0x8000, 0x8000, 0x10000 };
static const LVector s_untouchableLedgeEffectDirection = { 0x4000, 0, 0 };
static const LVector s_untouchableParticleDirection = { 0x4000, 0, 0 };

Untouchable::Untouchable(const LVector* pos, u16 type)
    : Obstacle(pos, type) {
    MARKFUNCTION(0x800A6330);
    particleMgr = new ParticleSystemMgr();
    soundPtr = nullptr;
}

Untouchable::~Untouchable() {
    MARKFUNCTION(0x800A6380);

    delete particleMgr;
    particleMgr = nullptr;

    ReleaseSound();
}

void Untouchable::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x800A63DC);
    Obstacle::AnalyzeMesh(root);

    tagCollisionBox localBox = { 0x7FFF, 0x7FFF, 0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0 };

    orientation.x = root->field40;
    orientation.y = root->field44;
    orientation.z = root->field48;

    FillCollisionBox(localBox, *static_cast<const DBVolume*>(root));
    SetCollisionBox(localBox);

    const DBAttrib* attrib = root->FindAttrib(6);
    damageType = attrib ? static_cast<s32>(attrib->value) : 2;

    attrib = root->FindAttrib(7);
    damageValue = attrib ? static_cast<s32>(attrib->value) : 3;

}

void Untouchable::CreateModel(const char* name) {
    MARKFUNCTION(0x800A64C8);
    (void)name;
    flags |= TF_MODEL_CREATED;
}

void Untouchable::DeleteModel() {
    MARKFUNCTION(0x800A64DC);
    flags &= ~TF_MODEL_CREATED;
}

void Untouchable::Reset() {
    MARKFUNCTION(0x800A64F0);
    field148 = 0;
    pendingCreate = 0;
    countdownTimer = damageValue;

    if (particleMgr) {
        particleMgr->InitMgr(ParticleSystem_Find(UNTOUCHABLE_EFFECT_HASH));
    }

    ReleaseSound();
}

void Untouchable::Think() {
    MARKFUNCTION(0x800A6540);

    if (countdownTimer <= 0) {
        countdownTimer = damageValue;
    }
    countdownTimer -= 1;

    if (!field148) {
        return;
    }

    if (pendingCreate) {
        const LVector origin = { 0, 0, 0 };
        particleMgr->CreateParticles(origin, nullptr);
        pendingCreate = 0;
    }

    particleMgr->Update();
    UpdateSound();

    if (!particleMgr->ActiveParticles()) {
        field148 = 0;
        particleMgr->PurgeParticles();
        particleMgr->ResetParticleDirection();
        ReleaseSound();
    }
}

void Untouchable::Draw() {
    MARKFUNCTION(0x800A6604);

    if (!field148) {
        return;
    }

#if defined(RC_PLATFORM_PSP)
    QueueLateEntityDraw([](void* context) {
        static_cast<Untouchable*>(context)->DrawLate();
    }, this);
#else
    QueueLateEntityDraw([this]() {
        DrawLate();
    });
#endif
}

void Untouchable::DrawLate() {
    const Mat4 savedWorld = p3d::context->GetWorldMatrix();
    Mat4 effectWorld;
    LVector effectPos = { effectPosX, effectPosY, effectPosZ };
    p3dBuildTransMatrix((f32)effectPos.x, (f32)effectPos.y, (f32)effectPos.z, effectWorld);
    p3d::context->SetWorldMatrix(effectWorld);
    particleMgr->Display();
    p3d::context->SetWorldMatrix(savedWorld);
}

void Untouchable::UpdatePosition() {
    MARKFUNCTION(0x800A6684);
}

void Untouchable::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x800A668C);
    (void)pickup;
}

void Untouchable::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x800A6694);

    if (!hum) {
        return;
    }

    switch (hum->actionState) {
        case AS_LEDGE_LATCH:
            hum->field368 |= 8u;
            GEffect_Create(
                UNTOUCHABLE_EFFECT_HASH,
                &hum->pos,
                &s_untouchableLedgeEffectScale,
                &s_untouchableLedgeEffectDirection,
                0,
                5,
                0);
            hum->HandleCollisionSound(18);
            hum->SubtractHitPoints(static_cast<u16>(damageType));
            if (hum->health == 0) {
                hum->SetActionState(AS_DEAD, 0);
            }
            hum->LetGoOfLedge();
            break;
        case 24:
        case 65:
        case 68:
        case AS_DEAD:
            return;
        case AS_HOTFOOT:
            hum->field368 |= 8u;
            if (countdownTimer == 0) {
                hum->SubtractHitPoints(static_cast<u16>(damageType));
                if (hum->health == 0) {
                    if (hum->thingType == 0) {
                        hum->LoadDialog(0x39, 0x40000100);
                    }
                    hum->SetActionState(AS_DEAD, 0);
                }
            }

            pendingCreate = 1;
            effectPosX = hum->pos.x;
            effectPosY = hum->pos.y;
            effectPosZ = hum->pos.z;
            break;
        default:
            if ((hum->flags & TF_ON_GROUND) != 0) {
                if (countdownTimer == 0) {
                    hum->SubtractHitPoints(static_cast<u16>(damageType));
                }

                hum->SetActionState(AS_HOTFOOT, 0);
                hum->field368 |= 8u;

                effectPosX = hum->pos.x;
                effectPosY = hum->pos.y;
                effectPosZ = hum->pos.z;
                particleMgr->SetParticleDirection(&s_untouchableParticleDirection);
                field148 = 1;

                CreateSound();
            }
            break;
    }
}

s32 Untouchable::CreateSound() {
    MARKFUNCTION(0x800A68FC);

    if (soundPtr) {
        return static_cast<s32>(reinterpret_cast<uintptr_t>(soundPtr));
    }

    u32 soundHash = 0;
    if (particleMgr) {
        soundHash = particleMgr->GetSystemHash();
    }

    CSound* created = nullptr;
    const s32 result = CSoundFactory::CreateObject(10000, &created, soundHash);
    if (result < 0 || !created) {
        return result;
    }

    soundPtr = static_cast<CParticleEffectSound*>(created);
    return soundPtr->Initialize(reinterpret_cast<const LVector*>(&effectPosX));
}

s32 Untouchable::UpdateSound() {
    MARKFUNCTION(0x800A695C);

    if (soundPtr) {
        return soundPtr->StartAnimating();
    }

    return 0;
}

s32 Untouchable::ReleaseSound() {
    MARKFUNCTION(0x800A698C);

    if (!soundPtr) {
        return 0;
    }

    soundPtr->Release();
    soundPtr = nullptr;
    return 0;
}

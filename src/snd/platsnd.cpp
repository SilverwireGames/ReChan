#include "snd/platsnd.h"
#include "snd/prstsnd.h"

#include <string.h>

// PSX: __14CPlatformSound (PLATSND.CPP:15, 0x800AC894)
CPlatformSound::CPlatformSound() {
    MARKFUNCTION(0x800AC894);

    pad16 = 0;
    beginMoveSfx = 0;
    tiltSfx = 0;
    impactSfx = 0;
    hitHumanoidSfx = 0;
    hitPathNodeSfx = 0;
    endMoveSfx = 0;
    movePersistentId = 0;
    idlePersistentId = 0;
    material = 0;
    soundFlags = 0;
    hitPathNodeCooldownReset = 0;
    pad39 = 0;

    movePersistent = nullptr;
    idlePersistent = nullptr;
    moving = 0;
    hitPathNodeCooldown = 0;
    hitHumanoidCooldown = 0;
    pad54 = 0;
}

// PSX: _._14CPlatformSound (PLATSND.CPP:26, 0x800AC8E0)
CPlatformSound::~CPlatformSound() {
    MARKFUNCTION(0x800AC8E0);
    EndPersistent(&idlePersistent);
    EndPersistent(&movePersistent);
}

// PSX: Initialize__14CPlatformSoundPC10tagLVector (PLATSND.CPP:32, 0x800AC94C)
s32 CPlatformSound::Initialize(void* pos) {
    MARKFUNCTION(0x800AC94C);

    s32 result = CSound::Initialize(pos);
    if (result < 0) {
        return result;
    }

    return BeginPersistent(idlePersistentId, &idlePersistent);
}

// PSX: BeginMove__14CPlatformSound (PLATSND.CPP:48, 0x800AC984)
s32 CPlatformSound::BeginMove() {
    MARKFUNCTION(0x800AC984);

    if (moving == 1) {
        return -3000;
    }

    BeginPersistent(movePersistentId, &movePersistent);
    s32 result = PlayTransient(beginMoveSfx, 8, 0);
    moving = 1;
    return result;
}

// PSX: EndMove__14CPlatformSound (PLATSND.CPP:69, 0x800AC9E0)
s32 CPlatformSound::EndMove() {
    MARKFUNCTION(0x800AC9E0);

    if (moving == 1) {
        moving = 0;
        PlayTransient(endMoveSfx, 8, 0);
    }

    return EndPersistent(&movePersistent);
}

// PSX: HitPathNode__14CPlatformSoundlll (PLATSND.CPP:86, 0x800ACA2C)
s32 CPlatformSound::HitPathNode(s32, s32, s32) {
    MARKFUNCTION(0x800ACA2C);

    if (hitPathNodeCooldown != 0) {
        return 100;
    }

    hitPathNodeCooldown = hitPathNodeCooldownReset;
    return PlayTransient(hitPathNodeSfx, 8, 0);
}

// PSX: Tilt__14CPlatformSound (PLATSND.CPP:100, 0x800ACA6C)
s32 CPlatformSound::Tilt() {
    MARKFUNCTION(0x800ACA6C);
    return PlayTransient(tiltSfx, 8, 0);
}

// PSX: Impact__14CPlatformSound (PLATSND.CPP:105, 0x800ACA94)
s32 CPlatformSound::Impact() {
    MARKFUNCTION(0x800ACA94);
    return PlayTransient(impactSfx, 8, 0);
}

// PSX: HitHumanoid__14CPlatformSound (PLATSND.CPP:110, 0x800ACABC)
s32 CPlatformSound::HitHumanoid() {
    MARKFUNCTION(0x800ACABC);

    if (hitHumanoidCooldown != 0) {
        return 100;
    }

    hitHumanoidCooldown = 60;
    return PlayTransient(hitHumanoidSfx, 8, 0);
}

// PSX: GetMaterial__14CPlatformSoundPl (0x800ACB70)
void CPlatformSound::GetMaterial(s32* outMaterial) const {
    MARKFUNCTION(0x800ACB70);
    *outMaterial = material;
}

// PSX: Think__14CPlatformSound (0x800ACAFC)
void CPlatformSound::Think() {
    MARKFUNCTION(0x800ACAFC);

    if (hitPathNodeCooldown != 0) {
        hitPathNodeCooldown--;
    }

    if (hitHumanoidCooldown != 0) {
        hitHumanoidCooldown--;
    }
}

// PSX: Load__14CPlatformSoundPCc (PLATSND.CPP:134, 0x800ACB2C)
s32 CPlatformSound::Load(const void* data) {
    MARKFUNCTION(0x800ACB2C);

    memcpy(&pad16, data, 24);
    flags = soundFlags;
    return 0;
}

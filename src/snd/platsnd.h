#pragma once

#include "snd/basesnd.h"

class CGenericPersistentSound;

// PSX: CPlatformSound (56 bytes) - platform movement/collision sound script.
// Source: SND/PLATSND.CPP
class CPlatformSound : public CSound {
public:
    u16 pad16;                    // +16
    u16 beginMoveSfx;             // +18
    u16 tiltSfx;                  // +20
    u16 impactSfx;                // +22
    u16 hitHumanoidSfx;           // +24
    u16 hitPathNodeSfx;           // +26
    u16 endMoveSfx;               // +28
    u8 movePersistentId;          // +30
    u8 idlePersistentId;          // +31
    s32 material;                 // +32
    u16 soundFlags;               // +36
    u8 hitPathNodeCooldownReset;  // +38
    u8 pad39;                     // +39
    CGenericPersistentSound* movePersistent; // +40
    CGenericPersistentSound* idlePersistent; // +44
    s32 moving;                   // +48
    u8 hitPathNodeCooldown;       // +52
    u8 hitHumanoidCooldown;       // +53
    u16 pad54;                    // +54

    CPlatformSound();
    ~CPlatformSound() override;

    s32 Initialize(void* pos);
    s32 BeginMove();
    s32 EndMove();
    s32 HitPathNode(s32, s32, s32);
    s32 Tilt();
    s32 Impact();
    s32 HitHumanoid();
    void GetMaterial(s32* outMaterial) const;
    void Think();
    s32 Load(const void* data);
};

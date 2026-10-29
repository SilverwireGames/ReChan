#pragma once

#include "core.h"
#include "gen/cclist.h"
#include "p3d/lvector.h"

struct ParticleStats;
class ParticleSystem;

// Hashes dumped from PSX particle chunks (0x8A30 tag 0x1100).
enum ParticleTypeHash : u32 {
    PARTICLE_PBITS = 0x00546E93u,
    PARTICLE_PBLKSMOKE = 0x01085785u,
    PARTICLE_PCRDUST = 0x04869AD4u,
    PARTICLE_PCRUMB = 0x05487A12u,
    PARTICLE_PDFRAG = 0x0548B657u,
    PARTICLE_PDFRAGS = 0x048B6593u,
    PARTICLE_PEXPLBLK = 0x0AD5044Bu,
    PARTICLE_PEXPLFRG = 0x0AD50E27u,
    PARTICLE_PEXPLOR3A = 0x0D5111D1u,
    PARTICLE_PFALLMIST = 0x06114A24u,
    PARTICLE_PFEATHER = 0x0A9689E2u,
    PARTICLE_PFLAMEVENT = 0x061FE334u,
    PARTICLE_PFLIMPCT = 0x0B0E21C4u,
    PARTICLE_PGLASSHAT = 0x006828A4u,
    PARTICLE_PGOO_L = 0x054C453Cu,
    PARTICLE_PGOO_L2 = 0x04C453A2u,
    PARTICLE_PGOO_L3 = 0x04C453A3u,
    PARTICLE_PGOO_R = 0x054C4542u,
    PARTICLE_PGOO_R2 = 0x04C45402u,
    PARTICLE_PGOO_R3 = 0x04C45403u,
    PARTICLE_PGRILSMK = 0x0C6E135Bu,
    PARTICLE_PLCRUMBL = 0x0087A43Cu,
    PARTICLE_PROOFSMOKE = 0x03B354B5u,
    PARTICLE_PSHARD = 0x0557C664u,
    PARTICLE_PSHARD2 = 0x057C6622u,
    PARTICLE_PVENTSTEAM = 0x039DD3FDu,
};

class ParticleSystemMgr {
public:
    ParticleSystemMgr();
    explicit ParticleSystemMgr(ParticleSystem* system);
    ~ParticleSystemMgr();

    void InitMgr(ParticleSystem* system);
    void SetSystem(ParticleSystem* system);
    s32 CreateParticles(const LVector& origin, ParticleStats* statsOverride = nullptr);
    s32 SetParticleDirection(const LVector* direction);
    s32 ResetParticleDirection();
    void SetDisplayOffset(const LVector* offset);
    s32 Update();
    void Display();
    s32 ActiveParticles();
    s32 PurgeParticles();
    u32 GetSystemHash() const;

    ParticleSystem* GetSystem() const {
        return system;
    }

private:
    void BindSystemList();
    void DetachSystem();
    void PurgeOwnedParticles();

    ParticleSystem* system = nullptr;
    LVector direction = {};
    ccMinList particles;
};

// PSX: Load__14ParticleSystemR10tReadChunkPPv (PARTICLE.CPP:205, 0x80095494)
s32 ParticleSystem_LoadChunk(const u8* body, u32 bodySize);

// PSX: InitParticleInfoMemory__14ParticleSystem (PARTICLE.CPP:312, 0x80095610)
s32 ParticleSystem_InitParticleInfoMemory();

// PSX: Unload__14ParticleSystem (PARTICLE.CPP:346, 0x800956D0)
void ParticleSystem_Unload();

// PSX: UnloadLevel__14ParticleSystem (PARTICLE.CPP:370, 0x80095740)
void ParticleSystem_UnloadLevel();

// PSX: CommonParticles__14ParticleSystemi (PARTICLE.CPP:408, 0x80095854)
void ParticleSystem_CommonParticles(s32 count);

// PSX: Find__14ParticleSystemUl (PARTICLE.CPP:908, 0x80096318)
ParticleSystem* ParticleSystem_Find(u32 hash);

// Debug helper: resolves a particle hash to loaded runtime particle-system name.
// Returns nullptr when hash is unknown or unnamed.
const char* ParticleSystem_GetNameByHash(u32 hash);

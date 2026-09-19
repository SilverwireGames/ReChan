#pragma once
#include "core.h"
#include "p3d/lvector.h"

struct DBVolume;
struct DBAttrib;
struct CollisionSector;
struct tPrimGeom;
class pddiPrimBuffer;

struct Block {
    // +0: parsed flag (1 if data has prims, 0 otherwise)
    u32 parsed;
    // +4,+8,+12: world position from DBVolume
    s32 posX;
    s32 posY;
    s32 posZ;
    // +16,+20,+24: dimensions (abs corner deltas)
    s32 dimX;
    s32 dimY;
    s32 dimZ;
    // +28: block number from attrib 15
    u16 blockNum;
    // +30-35: per-axis LOD near/far (attribs 10,11,25,26,27,28)
    u8 lodNearX;
    u8 lodFarX;
    u8 lodNearY;
    u8 lodFarY;
    u8 lodNearZ;
    u8 lodFarZ;
    // +36-37: LOD extra (attribs 12,13)
    u8 lodExtra0;
    u8 lodExtra1;
    // +38,+40: unknown (attribs 16,17)
    u16 unk38;
    u16 unk40;
    // +42,+44: script info (attrib 99)
    u16 hasScript;
    u16 scriptId;
    // +46: padding
    u16 pad46;
    // +48,+50: fog distance range (attribs 20,21)
    u16 fogMinDist;
    u16 fogMaxDist;
    // +52: fog color (attrib 22)
    u32 fogColor;
    // +56: unknown (attrib 23)
    u16 unk56;
    u16 pad58;
    // +60: unknown (attrib 24)
    u32 unk60;
    // +64: data pointer (BLK entry data)
    const u8* data;
    // +68: tPrimGeom
    tPrimGeom* primGeom;
    // +72: collision sector pointer
    CollisionSector* collision;
#if defined(RC_PLATFORM_PSP)
    // PSP streams reuse one scratch buffer for every .BLK entry.  Collision
    // sectors keep pointers into their source payload, so retain just this
    // block's collision section after Parse() returns.
    u8* ownedCollisionData = nullptr;
    u32 ownedCollisionSize = 0;
#endif
    // PC: cached unculled shadow-caster geometry for this block, built once from
    // primGeom and reused every frame by ShadowCSM::DrawBlockCasterIntoCascades,
    // instead of being rebuilt (CPU retriangulation + GL buffer create/upload) on
    // every single shadow pass. Released in Destroy() alongside primGeom.
    pddiPrimBuffer* shadowCasterBuffer;
    // +76: texture page animation frame counter
    s32 texPageFrame;
    // +80-100: half-extents (computed in SetDimension)
    s32 halfExtNegX;
    s32 halfExtNegY;
    s32 halfExtNegZ;
    s32 halfExtPosX;
    s32 halfExtPosY;
    s32 halfExtPosZ;

    Block();
    ~Block();

    void Init(const DBVolume* vol);                             // 0x80052BB0
    void SetDimension(const LVector* a, const LVector* b);      // 0x80052EA0
    void Parse(u32 size, const u8* blkData);                    // 0x80052F80
    void Unload();                                              // 0x80052FF0
    bool PointInBlock(const LVector* pt) const;                 // 0x80053024
    u32 GetNextBlockNumber() const;                             // 0x800530B0
    u32 GetPrevBlockNumber() const;                             // 0x800530D4
    bool Draw(const LVector* drawPos);                          // 0x800530F8
    void LoadPrim(const u8* primData, u32 primSize);            // 0x8005328C
    void Destroy();
};

// PSX globals in BLOCK.obj small-data region:
//   0x800DCF1C splat_fog_near
//   0x800DCF20 splat_fog_far
//   0x800DCF24 splat_fog_color
//   0x800DCF28 splat_dynamic_light
extern u32 splat_fog_near;
extern u32 splat_fog_far;
extern u32 splat_fog_color;
extern u32 splat_dynamic_light;

#include "common.h"
#include "gen/charmgr.h"
#include "gen/model.h"
#include "gen/levelmgr.h"
#include "gen/geometry.h"
#include "gen/skeleton.h"
#include "gen/game.h"
#include "gen/ai.h"
#include "gen/animstruct.h"
#include "gen/scoremgr.h"
#include "gen/world.h"
#include "p3d/hash.h"
#include "p3d/loadmanager.h"
#include "p3d/texture.h"
#include "p3d/inventory.h"
#include "p3d/context.h"
#include "p3d/flip.h"
#include "p3d/ramtexanim.h"
#include "gen/ccfile.h"
#include "p3d/fileio.h"
#include "gen/paramanim.h"
#include "extra/modloader.h"
#include "extra/gltfloader.h"
#include "extra/realtexture.h"
#include "pc/log.h"
#include <algorithm>

#if REAL_TEXTURE_RENDERING
// Registers a real-texture entry for a named VRAM-rect texture, preferring a
// ModLoader override (at its own native resolution) and falling back to
// decoding the original data already uploaded into world's VRAM (native PSX
// resolution -- no quality gain, just routed through the same pipeline).
static void RegisterRealTexture(World* world, const char* scope, const char* name,
                                 u16 tpage, u16 cba,
                                 float offsetX, float offsetY, float sizeX, float sizeY) {
    if (!world) return;

#if MOD_LOADER
    std::vector<u8> overrideRgba;
    int overrideW = 0, overrideH = 0;
    if (ModLoader::Instance().GetTextureOverrideRGBA(scope, name, overrideRgba, overrideW, overrideH)) {
        RealTextureRegistry::Instance().Register(tpage, cba, overrideRgba.data(), overrideW, overrideH,
                                                  offsetX, offsetY, sizeX, sizeY);
        return;
    }
#endif

    const int w = static_cast<int>(sizeX);
    const int h = static_cast<int>(sizeY);
    const int px0 = static_cast<int>(offsetX);
    const int py0 = static_cast<int>(offsetY);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || px0 < 0 || py0 < 0 || px0 + w > 256 || py0 + h > 256) return;

    std::vector<u8> page(256 * 256 * 4);
    world->GetVRAM().DecodePage(tpage, cba, page.data());
    std::vector<u8> crop(static_cast<size_t>(w) * h * 4);
    for (int row = 0; row < h; row++) {
        memcpy(crop.data() + static_cast<size_t>(row) * w * 4,
               page.data() + static_cast<size_t>(py0 + row) * 256 * 4 + static_cast<size_t>(px0) * 4,
               static_cast<size_t>(w) * 4);
    }
    RealTextureRegistry::Instance().Register(tpage, cba, crop.data(), w, h, offsetX, offsetY, sizeX, sizeY);
}
#endif // REAL_TEXTURE_RENDERING

// PSX: CharDataLoadCallback loads character textures into PSX VRAM via P3DLoad.
// PC: parse the 0xFF04/0x6008 chunks and upload raw u16 data to the World's
// PsxVRAM, matching PSX behavior (textures go into VRAM for tpage/cba lookup).
static void P3DLoadTextures(const u8* data, u32 size, const char* textureScope = nullptr) {
    if (!data || size < 6) return;

    World* world = g_game ? g_game->GetWorld() : nullptr;
    if (!world) return;

    // Parse P3D stream: root 0xFF04 container with 0x6008 sub-chunks
    u16 rootId = data[0] | (data[1] << 8);
    u32 rootSize = data[2] | (data[3] << 8) | (data[4] << 16) | (data[5] << 24);
    if (rootId != 0xFF04) return;

    u32 cpos = 6;
    u32 cend = (rootSize < size) ? rootSize : size;

#if MOD_LOADER || REAL_TEXTURE_RENDERING
    // CLUT chunks (e.g. "jeans5 CLUT") always precede their indexed data chunk
    // (e.g. "jeans5") within the stream. Remember each CLUT's VRAM rect by base
    // name so the matching data chunk can re-quantize+repaint (or register a
    // real-texture entry for) both as a pair.
    struct PendingClutRect { s16 rx, ry, rw, rh; };
    std::unordered_map<std::string, PendingClutRect> pendingCluts;
#endif

    while (cpos + 6 <= cend) {
        u16 chunkId = data[cpos] | (data[cpos + 1] << 8);
        u32 chunkSize = data[cpos + 2] | (data[cpos + 3] << 8) |
            (data[cpos + 4] << 16) | (data[cpos + 5] << 24);
        if (chunkSize < 6 || cpos + chunkSize > cend) break;

        if (chunkId == 0x6008) {
            u32 p = cpos + 6;
            u32 dend = cpos + chunkSize;

            // PString: u8 len + chars
            if (p >= dend) { cpos += chunkSize; continue; }
            u8 nameLen = data[p++];
            std::string texName(reinterpret_cast<const char*>(data + p), nameLen);
            p += nameLen;
            // PStrings may be padded with trailing nulls/spaces for alignment.
            while (!texName.empty() && (texName.back() == ' ' || texName.back() == '\0'))
                texName.pop_back();

            // RECT16: s16 x, y, w, h + u32 type
            if (p + 12 > dend) { cpos += chunkSize; continue; }
            s16 rx = (s16)(data[p] | (data[p + 1] << 8)); p += 2;
            s16 ry = (s16)(data[p] | (data[p + 1] << 8)); p += 2;
            s16 rw = (s16)(data[p] | (data[p + 1] << 8)); p += 2;
            s16 rh = (s16)(data[p] | (data[p + 1] << 8)); p += 2;
            p += 4; // skip type

            // Upload raw pixel data to VRAM, then repaint if a mod overrides this named texture
            if (rw > 0 && rh > 0 && rw <= 1024 && rh <= 512 &&
                p + (u32)(rw * rh * 2) <= dend) {
                world->UploadToVRAM(rx, ry, rw, rh, data + p);

#if MOD_LOADER || REAL_TEXTURE_RENDERING
                static constexpr const char* kClutSuffix = " CLUT";
                const size_t suffixPos = texName.size() >= 5 ? texName.size() - 5 : std::string::npos;
                const bool isClut = suffixPos != std::string::npos && texName.compare(suffixPos, 5, kClutSuffix) == 0;

                if (isClut) {
                    pendingCluts[texName.substr(0, suffixPos)] = PendingClutRect{ rx, ry, rw, rh };
                }
                else {
                    auto clutIt = pendingCluts.find(texName);
                    if (clutIt != pendingCluts.end()) {
                        const PendingClutRect& clutRect = clutIt->second;
                        const s16 paletteColorCount = clutRect.rw * clutRect.rh;
#if MOD_LOADER
                        std::vector<u16> clutOverride, indexOverride;
                        if (ModLoader::Instance().GetIndexedTextureOverride(
                                textureScope, texName.c_str(), rw, rh, paletteColorCount,
                                clutOverride, indexOverride)) {
                            world->UploadToVRAM(clutRect.rx, clutRect.ry, clutRect.rw, clutRect.rh,
                                                 reinterpret_cast<const u8*>(clutOverride.data()));
                            world->UploadToVRAM(rx, ry, rw, rh, reinterpret_cast<const u8*>(indexOverride.data()));
                            LOG("[ModLoader] Character texture override: %s", texName.c_str());
                        }
#endif
#if REAL_TEXTURE_RENDERING
                        {
                            const u8 ttx = static_cast<u8>(rx / 64), tty = static_cast<u8>(ry / 256);
                            const int dep = (paletteColorCount == 256) ? 1 : 0;
                            const u16 tpage = static_cast<u16>(ttx | (tty << 4) | (dep << 7));
                            const u16 cba = static_cast<u16>((clutRect.rx / 16) | (clutRect.ry << 6));
                            const int bppDiv = (paletteColorCount == 256) ? 2 : 4;
                            RegisterRealTexture(world, textureScope, texName.c_str(), tpage, cba,
                                                 static_cast<float>(rx - ttx * 64) * bppDiv,
                                                 static_cast<float>(ry - tty * 256),
                                                 static_cast<float>(rw) * bppDiv,
                                                 static_cast<float>(rh));
                        }
#endif
                    }
                    else {
#if MOD_LOADER
                        std::vector<u16> overridePixels;
                        if (ModLoader::Instance().GetTextureOverridePixels(textureScope, texName.c_str(), rw, rh, overridePixels)) {
                            world->UploadToVRAM(rx, ry, rw, rh, reinterpret_cast<const u8*>(overridePixels.data()));
                            LOG("[ModLoader] Character texture override: %s", texName.c_str());
                        }
#endif
#if REAL_TEXTURE_RENDERING
                        {
                            const u8 ttx = static_cast<u8>(rx / 64), tty = static_cast<u8>(ry / 256);
                            const u16 tpage = static_cast<u16>(ttx | (tty << 4) | (2 << 7));
                            RegisterRealTexture(world, textureScope, texName.c_str(), tpage, 0,
                                                 static_cast<float>(rx - ttx * 64),
                                                 static_cast<float>(ry - tty * 256),
                                                 static_cast<float>(rw),
                                                 static_cast<float>(rh));
                        }
#endif
                    }
                }
#endif
            }
        }
        cpos += chunkSize;
    }

    world->RefreshVRAMTexture();

    if (p3d::inventory) {
        tP3DFileHandler loader;
        loader.AddHandler(new tTextureLoader());
        loader.AddHandler(new tRAMTexAnimLoader());
        loader.LoadFromMemory(data, size, p3d::inventory);
    }
}

static u16 ReadRRU16(const u8* data) {
    return (u16)(data[0] | (data[1] << 8));
}

static u32 ReadRRU32(const u8* data) {
    return (u32)(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

static bool IsValidRRResourceIndex(const CharFile* cf, s32 index) {
    if (!cf || !cf->fileHandle || !cf->rrHeader || index < 0 || index >= cf->rrHeaderEntries) {
        return false;
    }

    const s32 size = rrSize(cf->rrHeader, index);
    const s32 offset = rrOffset(cf->rrHeader, index);
    if (size <= 0 || offset < 0) {
        return false;
    }

    const s32 fileSize = cf->fileHandle->GetLength();
    const s64 endOffset = (s64)offset + (s64)size;
    if (fileSize > 0 && endOffset > (s64)fileSize) {
        return false;
    }

    return true;
}

static bool IsCharacterPrimGeom(const u8* data, u32 size) {
    if (!data || size < 108) {
        return false;
    }

    if (data[12] != 'P' || data[13] != 'R' || data[14] != 'I' || data[15] != 'M') {
        return false;
    }

    u32 vertListOff = ReadRRU32(data + 0x10) << 2;
    u16 numVerts = ReadRRU16(data + 0x14);
    u16 numPolys = ReadRRU16(data + 0x16);
    u32 primListOff = ReadRRU32(data + 0x40) << 2;
    u32 polyDataOff = ReadRRU32(data + 0x54) << 2;

    if (numVerts == 0 || numPolys == 0) {
        return false;
    }

    if (vertListOff + (u32)numVerts * 8 > size) {
        return false;
    }

    if (polyDataOff + (u32)numPolys * 4 > size) {
        return false;
    }

    if (primListOff >= size) {
        return false;
    }

    return true;
}

static const u8* ResolveCharacterPrimGeom(const u8* data, u32 size, u32* outSize, u32* outOffset) {
    if (outSize) {
        *outSize = 0;
    }
    if (outOffset) {
        *outOffset = 0;
    }

    if (IsCharacterPrimGeom(data, size)) {
        if (outSize) {
            *outSize = size;
        }
        return data;
    }

    for (u32 offset = 4; offset + 108 <= size; offset += 4) {
        const u8* candidate = data + offset;
        u32 candidateSize = size - offset;
        if (!IsCharacterPrimGeom(candidate, candidateSize)) {
            continue;
        }

        if (outSize) {
            *outSize = candidateSize;
        }
        if (outOffset) {
            *outOffset = offset;
        }
        return candidate;
    }

    return nullptr;
}

static void ClearCharacterOriginalData(OriginalSTree* original) {
    if (!original) {
        return;
    }

    if (original->meshBuffer) {
        original->meshBuffer->Release();
        original->meshBuffer = nullptr;
    }
    if (original->skeleton) {
        delete original->skeleton;
        original->skeleton = nullptr;
    }
    if (original->skinData) {
        delete original->skinData;
        original->skinData = nullptr;
    }
    if (original->compositeAnim) {
        delete original->compositeAnim;
        original->compositeAnim = nullptr;
    }

    original->meshVertCount = 0;
    original->meshTriCount = 0;
}

static bool PopulateCharacterOriginal(OriginalSTree* original, CharFile* cf, u32 type,
                                      const u8* extraData, u32 extraSize,
                                      const u8* dataBuf, u32 dataSize,
                                      u32 expectedCompositeNameUID) {
    if (!original || !cf || !dataBuf || dataSize == 0) {
        return false;
    }

    CompositeAnimData* compositeAnim = nullptr;
    STreeData* skeleton = ParseP3DStreamFull(dataBuf, dataSize, &compositeAnim, expectedCompositeNameUID,
                                              g_charNameTable[type]);

    u32 primGeomSize = 0;
    u32 primGeomOffset = 0;
    const u8* primGeomData = ResolveCharacterPrimGeom(extraData, extraSize, &primGeomSize, &primGeomOffset);

    if (!skeleton && !primGeomData) {
        if (compositeAnim) {
            delete compositeAnim;
        }
        return false;
    }

    if (cf->dataBuffer && cf->dataSize > 1) {
        original->nameCRC = ReadRRU32((const u8*)cf->dataBuffer + 4);
    }
    original->SetStoreID(type == 0 ? 0 : 2);

    if (primGeomData && primGeomOffset != 0) {
        LOG("[CharMgr] Resolved embedded tPrimGeom for type %u at +0x%X", type, primGeomOffset);
    }

    ClearCharacterOriginalData(original);

    if (skeleton) {
        s32 idleAnimSize = 0;
        u8* idleAnimBuf = cf->ReadResource(8, &idleAnimSize);
        if (idleAnimBuf) {
            ApplyAnimFrame0(skeleton, idleAnimBuf, (u32)idleAnimSize);
            std::free(idleAnimBuf);
        }

        original->skeleton = skeleton;
        original->compositeAnim = compositeAnim;
        skeleton = nullptr;
        compositeAnim = nullptr;

        if (primGeomData) {
            BuildPerJointMeshes(original, primGeomData, primGeomSize);
        }

        LOG("[CharMgr] Populated OriginalSTree with skeleton for type %u (hash 0x%08X, %u joints)",
            type, original->nameCRC, original->skeleton->numJoints);
    }
    else {
        original->meshBuffer = primGeomData
            ? BuildPrimBufferFromRawPrimGeom(primGeomData, primGeomSize
#if REAL_TEXTURE_RENDERING
                                              , &original->realTexGroups
#endif
                                              )
            : nullptr;

        LOG("[CharMgr] Populated OriginalSTree (flat) for type %u (hash 0x%08X)",
            type, original->nameCRC);
    }

    if (skeleton) {
        delete skeleton;
    }
    if (compositeAnim) {
        delete compositeAnim;
    }

    return true;
}

// Global singleton (PSX: gp+796)
CharacterManager* g_characterManager = nullptr;

// Global CharFile list head (PSX: gp+800)
CharFile* g_charFileList = nullptr;

// PSX: gp+3468 - mesh type for reload
static s32 g_playerMeshType = 0;

static bool IsLiveCharFilePointer(const CharFile* cf) {
    if (!cf) {
        return false;
    }

    for (CharFile* cur = g_charFileList; cur; cur = cur->next) {
        if (cur == cf) {
            return true;
        }
    }

    return false;
}

struct AnimGroupEntry {
    s32 startEnum;
    u32 count;
};

// PSX: CharacterManager::g_AnimGroupTable at 0x800D68F4.
constexpr AnimGroupEntry kAnimGroupTable[] = {
    { 0x000, 0x7C },
    { 0x07C, 0x09 },
    { 0x085, 0x04 },
    { 0x089, 0x02 },
    { 0x08B, 0x02 },
    { 0x08D, 0x02 },
    { 0x08F, 0x05 },
    { 0x094, 0x0D },
    { 0x0A1, 0x07 },
    { 0x0A8, 0x07 },
    { 0x0AF, 0x07 },
    { 0x0B6, 0x07 },
    { 0x0BD, 0x0E },
    { 0x0CB, 0x0E },
    { 0x0D9, 0x0E },
    { 0x0E7, 0x0A },
    { 0x0F1, 0x0A },
    { 0x0FB, 0x0A },
    { 0x105, 0x0F },
    { 0x114, 0x05 },
    { 0x119, 0x04 },
    { 0x11D, 0x04 },
    { 0x121, 0x06 },
    { 0x127, 0x02 },
    { 0x129, 0x05 },
    { 0x12E, 0x04 },
    { 0x132, 0x02 },
    { 0x134, 0x08 },
    { 0x13C, 0x32 },
    { -1, 0 },
};

static const AnimGroupEntry* FindAnimGroupEntry(s32 startEnum) {
    for (const AnimGroupEntry& entry : kAnimGroupTable) {
        if (entry.startEnum < 0) {
            return nullptr;
        }
        if (entry.startEnum == startEnum) {
            return &entry;
        }
    }
    return nullptr;
}

// PSX: 0x800D6880 - character name table indexed by ThingType
// 29 entries for ThingTypes 0-28.
const char* g_charNameTable[] = {
    "JACKIE",   // 0 - player
    "LENNY",    // 1
    "ROSCOE",   // 2
    "FACTORY",  // 3
    "STEEL",    // 4
    "BROCK",    // 5
    "JANITOR",  // 6
    "IGOR",     // 7
    "YAK",      // 8
    "BLIND",    // 9
    "GRONTAR",  // 10
    "JACQUES",  // 11
    "DISCO",    // 12
    "CLOWN",    // 13
    "YAKUZA",   // 14
    "DANTE",    // 15
    "HOOD",     // 16
    "CHEF",     // 17
    "HAZARD",   // 18
    "VAGRANT",  // 19
    "SHO",      // 20
    "ELITE",    // 21
    "WAITER",   // 22
    "MIME",     // 23
    "LORNA",    // 24
    "SHAOLIN",  // 25
    "GOR",      // 26
    "TAO",      // 27
    "DM",       // 28
};


// PC platform: pending-free list for TransformAnim objects.
// On PSX the BIOS heap only overwrites the first 4 bytes of a freed block with
// a free-list next-pointer, leaving the rest of the data (including rotChannels
// at offset 16) intact for the remainder of the frame.  On Windows the CRT heap
// destructs the object immediately, freeing the rotChannels array and potentially
// corrupting the freed block, which causes a UAF crash in AnimateEverythingHandler
// (UpdateJoints) when DynamicAnimUnload fires during handlerSet1 of the same frame.
// Deferring the actual delete until EndFrameHandler (after AnimateEverythingHandler)
// restores the PSX allocator's same-frame-safe-read guarantee.
struct PendingFreeAnim {
    void* ptr = nullptr;
    bool isSequence = false;
};

static PendingFreeAnim s_pendingFreeAnims[CHAR_MAX_ANIMS] = {};
static s32 s_pendingFreeCount = 0;

static bool IsAnimResourceReferencedByAnimStructure(const AnimStructure* anim, const PendingFreeAnim& pending) {
    if (!anim || !pending.ptr) {
        return false;
    }

    if (pending.isSequence) {
        return anim->sequence == pending.ptr;
    }

    return anim->animation == pending.ptr ||
           (anim->flip && anim->flip->anim == pending.ptr);
}

static bool IsAnimResourceReferencedByThingList(ccList& list, const PendingFreeAnim& pending) {
    for (ccMinNode* node = list.head; node; node = node->next) {
        Thing* thing = static_cast<Thing*>(node);
        if (!thing || !thing->model) {
            continue;
        }

        Model* model = static_cast<Model*>(thing->model);
        AnimStructure* anim = static_cast<AnimStructure*>(model->animStructure);
        if (IsAnimResourceReferencedByAnimStructure(anim, pending)) {
            return true;
        }
    }

    return false;
}

static bool IsAnimResourceStillReferenced(const PendingFreeAnim& pending) {
    if (!g_ai) {
        return false;
    }

    return IsAnimResourceReferencedByThingList(g_ai->humanoidList, pending) ||
           IsAnimResourceReferencedByThingList(g_ai->pickupList, pending) ||
           IsAnimResourceReferencedByThingList(g_ai->inactivePickupList, pending) ||
           IsAnimResourceReferencedByThingList(g_ai->moveList, pending);
}

// Free functions
// PSX: FreeAnimMemory (CHARMGR.CPP:201, 0x800395F8)
void FreeAnimMemory(void* ptr) {
    MARKFUNCTION(0x800395F8);
    if (IsCameraParamAnim(ptr)) {
        delete static_cast<CameraParamAnim*>(ptr);
        return;
    }

    const bool isSequence = IsCharSequenceAnim(ptr);

    // Defer deletion until FlushPendingFree() to prevent same-frame UAF.
    // Remember the type now while the resource is known-live; stale sequence
    // memory must not be probed later to decide how to delete it.
    //
    // Guard against double-registration: if the same ptr is already queued
    // (e.g. from a rapid DynamicAnimUnload / PurgeLevel overlap), skip it.
    // A double-delete would crash because IsCharSequenceAnim on freed memory
    // returns false, causing a CharSequenceAnim to be deleted as TransformAnim.
    for (s32 i = 0; i < s_pendingFreeCount; ++i) {
        if (s_pendingFreeAnims[i].ptr == ptr) {
            return;
        }
    }

    PendingFreeAnim pending = { ptr, isSequence };
    if (s_pendingFreeCount < CHAR_MAX_ANIMS) {
        s_pendingFreeAnims[s_pendingFreeCount++] = pending;
    } else {
        if (IsAnimResourceStillReferenced(pending)) {
            LOG("[CharMgr] pending animation free queue full; leaking still-referenced anim %p", ptr);
            return;
        }
        if (isSequence)
            delete static_cast<CharSequenceAnim*>(ptr);
        else
            delete static_cast<TransformAnim*>(ptr);
    }
}

void CharacterManager::FlushPendingFree() {
    s32 writeIndex = 0;
    for (s32 i = 0; i < s_pendingFreeCount; ++i) {
        PendingFreeAnim pending = s_pendingFreeAnims[i];
        if (!pending.ptr) {
            continue;
        }

        if (IsAnimResourceStillReferenced(pending)) {
            s_pendingFreeAnims[writeIndex++] = pending;
            continue;
        }

        if (pending.isSequence)
            delete static_cast<CharSequenceAnim*>(pending.ptr);
        else
            delete static_cast<TransformAnim*>(pending.ptr);

        s_pendingFreeAnims[i] = {};

        // Null out any duplicate of this ptr further in the list so a
        // double-registration that slipped through FreeAnimMemory's guard
        // cannot fire a second delete on freed memory.
        for (s32 j = i + 1; j < s_pendingFreeCount; ++j) {
            if (s_pendingFreeAnims[j].ptr == pending.ptr) {
                s_pendingFreeAnims[j] = {};
            }
        }
    }

    for (s32 i = writeIndex; i < s_pendingFreeCount; ++i) {
        s_pendingFreeAnims[i] = {};
    }
    s_pendingFreeCount = writeIndex;
}

static u32 GetLoadedAnimationNameUID(const void* animation) {
    if (!animation) {
        return 0;
    }

    if (IsCharSequenceAnim(animation)) {
        const CharSequenceAnim* sequence = static_cast<const CharSequenceAnim*>(animation);
        return (sequence->parts && sequence->parts[0]) ? sequence->parts[0]->nameUID : 0;
    }

    return *reinterpret_cast<const u32*>(animation);
}

// PSX: GetCompositeAnimationNameHash (CHARMGR.CPP:267, 0x80039624)
u32 GetCompositeAnimationNameHash(const char* name) {
    MARKFUNCTION(0x80039624);
    char buf[32] = {};
    snprintf(buf, sizeof(buf), "COMP_ANM%sREF", name);
    return p3dHash(buf);
}

// PSX: GetPlayerMeshType (CHARMGR.CPP:301, 0x800396BC)
// PSX returns pointer to global: return gp+3468
s32* GetPlayerMeshType() {
    MARKFUNCTION(0x800396BC);
    return &g_playerMeshType;
}


// CharMgrCallback
// PSX: ~CharMgrCallback (CHARMGR.HPP:83, 0x8003BA98)
CharMgrCallback::~CharMgrCallback() {
    MARKFUNCTION(0x8003BA98);
}

// PSX: Callback__14CharMgrCallback (CHARMGR.HPP:82, 0x8003BACC)
void CharMgrCallback::Callback() {
    MARKFUNCTION(0x8003BACC);
    done = 1;
}


// AnimCallback
// PSX: __12AnimCallback (CHARMGR.CPP:2935, 0x8003B964)
AnimCallback::AnimCallback(u32 type, s32 animEnum, u32 count, CharMgrCallback* cb) {
    MARKFUNCTION(0x8003B964);
    done = 0;
    thingType = type;
    animEnumCounter = animEnum;
    remainingCount = (s32)count;
    userCallback = cb;
}

// PSX: _._12AnimCallback (CHARMGR.CPP, 0x8003BA4C)
AnimCallback::~AnimCallback() {
    MARKFUNCTION(0x8003BA4C);
}

// PSX: Callback__12AnimCallback (CHARMGR.CPP:2963, 0x8003B99C)
void AnimCallback::Callback() {
    MARKFUNCTION(0x8003B99C);
    animEnumCounter++;
    remainingCount--;

    if (remainingCount > 0) {
        // More to load - chain next animation
        if (g_characterManager) {
            g_characterManager->LoadAnimationBatch(thingType, animEnumCounter, this);
        }
        return;
    }

    // Done - invoke user callback
    if (userCallback) {
        userCallback->Callback();
    }
    delete this;
}


// CharFile
// PSX: __8CharFile (CHARMGR.CPP:2682, 0x8003B694)
// PSX: builds "RCHARS\\{name}.RR", calls rCDOpen, rrLoadHeaderOnly,
//      allocates buffer, reads resource 1 synchronously.
// PC: opens file from disk, reads header, reads resource 1.
CharFile::CharFile(u32 type) {
    MARKFUNCTION(0x8003B694);
    refCount = 1;
    thingType = type;
    dataBuffer = nullptr;
    dataSize = 0;
    fileHandle = nullptr;
    rrHeader = nullptr;
    rrHeaderEntries = 0;
    wholeFileBuf = nullptr;

    // Build path: "RCHARS/{name}.RR"
    char pathBuf[128];
    snprintf(pathBuf, sizeof(pathBuf), "RCHARS/%s.RR", g_charNameTable[type]);

    LOG("[CharFile] ReadFile begin: %s", pathBuf);
    p3d::io::FileHandle sourceFile;
    if (!sourceFile.Open(pathBuf, p3d::io::FileHandle::Mode::Read)) {
        LOG("[CharFile] Failed to open: %s", pathBuf);
        next = g_charFileList;
        g_charFileList = this;
        return;
    }

    const int64_t sourceSize = sourceFile.Size();
    if (sourceSize <= 0 || sourceSize > INT32_MAX) {
        LOG("[CharFile] Invalid file size: %lld", (long long)sourceSize);
        sourceFile.Close();
        next = g_charFileList;
        g_charFileList = this;
        return;
    }

    s32 fileSize = (s32)sourceSize;
    LOG("[CharFile] ReadFile size: %d", fileSize);
    wholeFileBuf = std::malloc((size_t)fileSize);
    LOG("[CharFile] whole file allocation: %s", wholeFileBuf ? "ok" : "failed");
    if (!wholeFileBuf) {
        sourceFile.Close();
        next = g_charFileList;
        g_charFileList = this;
        return;
    }
    if (sourceFile.Read(wholeFileBuf, (size_t)fileSize) != (size_t)fileSize) {
        LOG("[CharFile] whole file read failed");
        sourceFile.Close();
        std::free(wholeFileBuf);
        wholeFileBuf = nullptr;
        next = g_charFileList;
        g_charFileList = this;
        return;
    }
    sourceFile.Close();
    LOG("[CharFile] whole file read complete");

    fileHandle = new ccFile();
    LOG("[CharFile] ccFile allocated");
    fileHandle->OpenMem((u8*)wholeFileBuf, (u32)fileSize);
    s32 probeSize = (fileSize < (s32)(2 * sizeof(RREntry))) ? fileSize : (s32)(2 * sizeof(RREntry));
    rrHeader = (RREntry*)std::malloc(probeSize);
    LOG("[CharFile] RR header allocation: %s (%d)", rrHeader ? "ok" : "failed", probeSize);
    if (!rrHeader) {
        next = g_charFileList;
        g_charFileList = this;
        return;
    }
    fileHandle->Seek(0, ccFile::SEEK_FROM_START);
    fileHandle->Read(rrHeader, (u32)probeSize);
    LOG("[CharFile] RR header read complete");

    s32 headerSize = probeSize;
    if (probeSize >= (s32)(2 * sizeof(RREntry))) {
        s32 fullHeaderSize = rrOffset(rrHeader, 1);
        if (fullHeaderSize > probeSize && fullHeaderSize <= fileSize && (fullHeaderSize % (s32)sizeof(RREntry)) == 0) {
            RREntry* fullHeader = (RREntry*)std::malloc(fullHeaderSize);
            fileHandle->Seek(0, ccFile::SEEK_FROM_START);
            fileHandle->Read(fullHeader, (u32)fullHeaderSize);
            std::free(rrHeader);
            rrHeader = fullHeader;
            headerSize = fullHeaderSize;
        }
    }
    rrHeaderEntries = headerSize / (s32)sizeof(RREntry);

    // Read resource 1 (the animation hash data section)
    // PSX: dataBuffer = rPMalloc(heap, rrSize(header, 1), 0)
    //      rCDSeekA(handle, rrOffset(header, 1), 0)
    //      rCDReadA(handle, dataBuffer, rrSize(header, 1))
    //      rCDWaitUntilDone()
    s32 dataByteSize = rrSize(rrHeader, 1);
    LOG("[CharFile] resource 1 size: %d", dataByteSize);
    if (dataByteSize > 0) {
        dataBuffer = std::malloc(dataByteSize);
        LOG("[CharFile] resource 1 allocation: %s", dataBuffer ? "ok" : "failed");
        if (!dataBuffer) {
            next = g_charFileList;
            g_charFileList = this;
            return;
        }
        fileHandle->Seek((u32)rrOffset(rrHeader, 1), ccFile::SEEK_FROM_START);
        fileHandle->Read(dataBuffer, (u32)dataByteSize);
        dataSize = dataByteSize / 4; // PSX stores size in words
        LOG("[CharFile] resource 1 read complete");
    }

    // Link into global list (PSX: next = gp+800; gp+800 = this)
    next = g_charFileList;
    g_charFileList = this;

    LOG("[CharFile] Opened %s (type %u, %d resources)", pathBuf, type, rrHeaderEntries);
}

// PSX: _._8CharFile (CHARMGR.CPP:2751, 0x8003B798)
CharFile::~CharFile() {
    MARKFUNCTION(0x8003B798);
    // Unlink from global list
    CharFile** prev = &g_charFileList;
    while (*prev && *prev != this) {
        prev = &(*prev)->next;
    }
    if (*prev == this) {
        *prev = next;
    }

    // PSX: rCDCloseA, rPFree(rrHeader), rPFree(dataBuffer)
    if (fileHandle) {
        fileHandle->Close();
        delete fileHandle;
        fileHandle = nullptr;
    }
    if (wholeFileBuf) {
        std::free(wholeFileBuf);
        wholeFileBuf = nullptr;
    }
    std::free(rrHeader);
    rrHeader = nullptr;
    std::free(dataBuffer);
    dataBuffer = nullptr;
}

// PSX: AddRef__8CharFile (0x8003B83C)
void CharFile::AddRef() {
    MARKFUNCTION(0x8003B83C);
    refCount++;
}

// PSX: DeleteRef__8CharFile (CHARMGR.CPP:2805, 0x8003B850)
void CharFile::DeleteRef() {
    MARKFUNCTION(0x8003B850);
    refCount--;
    if (refCount <= 0) {
        delete this;
    }
}

// PSX: Find__8CharFileUs (CHARMGR.CPP:2831, 0x8003B88C)
CharFile* CharFile::Find(u32 type) {
    MARKFUNCTION(0x8003B88C);
    CharFile* cf = g_charFileList;
    while (cf) {
        if (cf->thingType == type) {
            return cf;
        }
        cf = cf->next;
    }
    return nullptr;
}

// PSX: FindAnim__8CharFileUl (CHARMGR.CPP:2861, 0x8003B8C4)
// Scans the RR data buffer (word array) from index 2 to dataSize for a matching hash.
// Returns 0-based index (i-2) on match, 0xFFFF if not found.
s32 CharFile::FindAnim(u32 hash) {
    MARKFUNCTION(0x8003B8C4);
    u32* data = (u32*)dataBuffer;
    for (s32 i = 2; i < dataSize; i++) {
        if (data[i] == hash) {
            return i - 2;
        }
    }
    return (s32)0xFFFF;
}

// PSX: EnableCache__8CharFilei (CHARMGR.CPP:2893, 0x8003B914)
// PSX: rCDCacheInit / rCDCacheTerm on the CD handle.
// PC: no-op (file I/O is already buffered)
void CharFile::EnableCache(s32 enable) {
    MARKFUNCTION(0x8003B914);
    (void)enable;
}

// PC helper: read a resource from the .RR file into a freshly allocated buffer.
// Caller owns the returned pointer (free with std::free).
u8* CharFile::ReadResource(s32 index, s32* outSize) {
    if (!fileHandle || !rrHeader || index < 0 || index >= rrHeaderEntries) {
        if (outSize) *outSize = 0;
        return nullptr;
    }
    s32 size = rrSize(rrHeader, index);
    s32 offset = rrOffset(rrHeader, index);
    if (size <= 0) {
        if (outSize) *outSize = 0;
        return nullptr;
    }
    u8* buf = (u8*)std::malloc(size);
    fileHandle->Seek((u32)offset, ccFile::SEEK_FROM_START);
    if (fileHandle->Read(buf, (u32)size) != size) {
        std::free(buf);
        if (outSize) *outSize = 0;
        return nullptr;
    }
    if (outSize) *outSize = size;
    return buf;
}


// CharacterManager
// PSX: __16CharacterManager (CHARMGR.CPP:334, 0x800396C8)
CharacterManager::CharacterManager() {
    MARKFUNCTION(0x800396C8);

    // Build free list: animPtrs[i] = &animPtrs[i+1]
    for (s32 i = 0; i < CHAR_MAX_ANIMS - 1; i++) {
        animRefCounts[i] = 0;
        animPtrs[i] = &animPtrs[i + 1];
    }
    animPtrs[CHAR_MAX_ANIMS - 1] = nullptr;
    animRefCounts[CHAR_MAX_ANIMS - 1] = 0;

    animCount = 0;
    freeListHead = &animPtrs[0];

    // Initialize 4 character slots
    for (s32 s = 0; s < CHAR_MAX_SLOTS; s++) {
        slots[s].thingType = CharSlot::EMPTY_SENTINEL;
        slots[s].field48 = 0;
        slots[s].field52 = 0;
        slots[s].field56 = 0;
        memset(slots[s].animIndexTable, 0xFF, CharSlot::ANIM_TABLE_SIZE);
    }

    // Set global singleton (PSX: gp+796 = this)
    g_characterManager = this;
}

// PSX: _._16CharacterManager (CHARMGR.CPP:388, 0x80039794)
CharacterManager::~CharacterManager() {
    MARKFUNCTION(0x80039794);
    g_characterManager = nullptr;
}

// Helper: find slot index for type
s32 CharacterManager::FindSlot(u32 type) {
    for (s32 i = 0; i < CHAR_MAX_SLOTS; i++) {
        if (slots[i].thingType == type) return i;
    }
    return -1;
}

// Helper: find empty slot
s32 CharacterManager::FindEmptySlot() {
    for (s32 i = 0; i < CHAR_MAX_SLOTS; i++) {
        if (slots[i].thingType == CharSlot::EMPTY_SENTINEL) return i;
    }
    return -1;
}

// PSX: OpenCharacter__16CharacterManagerUs (CHARMGR.CPP:425, 0x800397C4)
// PSX: CharFile::Find(type), if null creates new CharFile(type). Does NOT touch slots.
void CharacterManager::OpenCharacter(u32 type) {
    MARKFUNCTION(0x800397C4);
    CharFile* cf = CharFile::Find(type);
    if (!cf) {
        cf = new CharFile(type);
    }
}

// PSX: CloseCharacter__16CharacterManagerUs (CHARMGR.CPP:467, 0x80039808)
// PSX: CharFile::Find(type) then cf->DeleteRef(). Does NOT touch slots.
void CharacterManager::CloseCharacter(u32 type) {
    MARKFUNCTION(0x80039808);
    CharFile* cf = CharFile::Find(type);
    if (cf) {
        cf->DeleteRef();
    }
}

// PSX: LoadCharacter__16CharacterManagerUsP14CharMgrCallback (CHARMGR.CPP:496, 0x80039830)
// PSX: Full async CD pipeline. PC: synchronous file read from .RR.
// Reuses an existing NPC slot, while player type 0 still reloads slot 0 to refresh Jackie resources.
void CharacterManager::LoadCharacter(u32 type, CharMgrCallback* callback) {
#if MOD_LOADER
    {
        const char* characterName = g_charNameTable[type];
        std::string scope = characterName ? characterName : "";
        std::transform(scope.begin(), scope.end(), scope.begin(),
            [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        const std::string* modPath = ModLoader::Instance().FindModelOverridePath(
            scope.c_str(), scope.c_str());
        if (modPath) {
            OriginalSTree* modded = GLTFLoader::LoadSTree(modPath->c_str());
            const bool moddedHasMesh = modded
                && (modded->meshBuffer || (modded->skeleton && modded->skeleton->joints[0].meshBuffer));
            if (moddedHasMesh) {
                LOG("[ModLoader] Using modded model for %s: %s", characterName, modPath->c_str());
                s32 slotIdx = (type == 0) ? 0 : FindEmptySlot();
                if (slotIdx >= 0) {
                    slots[slotIdx].thingType = type;
                    slots[slotIdx].charFile = CharFile::Find(type);
                    if (slots[slotIdx].charFile) slots[slotIdx].charFile->AddRef();
                    slots[slotIdx].model = modded;
                    memset(slots[slotIdx].animIndexTable, 0xFF, CharSlot::ANIM_TABLE_SIZE);
                    if (g_levelManager) {
                        // Must match the native registration hash (see the
                        // non-modded path below): Thing::CreateModel looks
                        // models up by the nameHash baked into the character's
                        // own .RR data (resource 1, offset +4), NOT by
                        // p3dHash(characterName). Registering under the wrong
                        // hash means FindModel() never finds this character,
                        // TF_MODEL_CREATED never gets set on the Thing, and
                        // HandleThingEnvironmentCollisions skips it entirely
                        // (ai/thing.cpp, gen/colmgr.cpp) - the character falls
                        // forever because collision is never even attempted.
                        u32 nameHash = 0;
                        CharFile* cf = slots[slotIdx].charFile;
                        if (cf && cf->dataBuffer && cf->dataSize > 1) {
                            nameHash = *(u32*)((u8*)cf->dataBuffer + 4);
                        }
                        modded->nameCRC = nameHash;
                        modded->SetStoreID(type == 0 ? 0 : 2);
                        g_levelManager->AddOriginal(modded, 0);
                    }
                    if (callback) callback->Callback();
                    return;
                }
            }
            delete modded;
            LOG("[ModLoader] Character GLB rejected; using original: %s", modPath->c_str());
        }
    }
#endif
    MARKFUNCTION(0x80039830);

    CharFile* cf = CharFile::Find(type);
    if (!cf) {
        LOG("[CharMgr] LoadCharacter: no CharFile for type %u", type);
        if (callback) callback->Callback();
        return;
    }

    s32 slotIdx = FindSlot(type);
    if (type != 0 && slotIdx >= 0) {
        CharSlot& existingSlot = slots[slotIdx];
        if (existingSlot.charFile && IsLiveCharFilePointer(existingSlot.charFile)) {
            existingSlot.loadCount++;
            if (callback) {
                callback->Callback();
            }
            return;
        }

        if (existingSlot.dataBuffer) {
            std::free(existingSlot.dataBuffer);
            existingSlot.dataBuffer = nullptr;
        }

        existingSlot.charFile = nullptr;
        existingSlot.model = nullptr;
        existingSlot.loadCount = 0;
        existingSlot.thingType = CharSlot::EMPTY_SENTINEL;
        memset(existingSlot.animIndexTable, 0xFF, CharSlot::ANIM_TABLE_SIZE);
        slotIdx = -1;
    }

    // Find slot - PSX: type 0 -> slot 0, else search slots 1-3 for empty
    if (type == 0) {
        slotIdx = 0;
    }
    else {
        slotIdx = -1;
        for (s32 i = 1; i < CHAR_MAX_SLOTS; i++) {
            if (slots[i].thingType == CharSlot::EMPTY_SENTINEL) {
                slotIdx = i;
                break;
            }
        }
        if (slotIdx < 0) {
            LOG("[CharMgr] LoadCharacter: no empty slot for type %u", type);
            if (callback) callback->Callback();
            return;
        }
    }

    // PSX: player slot 0 uses RR pair 2/3, while NPC slots 1-3 use ordinal
    // 0-2 to select RR pairs 2/3, 4/5, 6/7.
    const s32 resourceSlot = (type == 0) ? slotIdx : (slotIdx - 1);
    s32 rrIdx = resourceSlot * 2 + 3;

    if (!IsValidRRResourceIndex(cf, rrIdx) || !IsValidRRResourceIndex(cf, rrIdx - 1)) {
        LOG("[CharMgr] LoadCharacter: invalid RR resources for type %u (slot %d, rrIdx %d)", type, slotIdx, rrIdx);
        if (callback) callback->Callback();
        return;
    }

    if (type == 0 && !IsValidRRResourceIndex(cf, slotIdx * 2 + 4)) {
        LOG("[CharMgr] LoadCharacter: invalid player texture resource for type %u (slot %d)", type, slotIdx);
        if (callback) callback->Callback();
        return;
    }

    CharSlot& slot = slots[slotIdx];
    slot.thingType = type;
    slot.charFile = cf;
    slot.model = nullptr;
    cf->AddRef();

    auto abortLoad = [&]() {
        if (slot.dataBuffer) {
            std::free(slot.dataBuffer);
            slot.dataBuffer = nullptr;
        }
        slot.model = nullptr;
        slot.loadCount = 0;
        slot.thingType = CharSlot::EMPTY_SENTINEL;
        memset(slot.animIndexTable, 0xFF, CharSlot::ANIM_TABLE_SIZE);
        if (slot.charFile) {
            slot.charFile->DeleteRef();
            slot.charFile = nullptr;
        }
    };

    // Read P3D model data (resource rrIdx)
    s32 dataSize = 0;
    u8* dataBuf = cf->ReadResource(rrIdx, &dataSize);
    if (!dataBuf || dataSize <= 0) {
        LOG("[CharMgr] LoadCharacter: failed to read model resource for type %u (resource %d)", type, rrIdx);
        if (dataBuf) {
            std::free(dataBuf);
        }
        abortLoad();
        if (callback) callback->Callback();
        return;
    }

    if (type == 0) {
        // Player: also read skeleton/extra buffer (resource rrIdx-1)
        // PSX: takes max of rrSize(hdr, rrIdx-1) and rrSize(hdr, slotIdx*2+4)
        s32 skelSize1 = rrSize(cf->rrHeader, rrIdx - 1);
        s32 skelSize2 = rrSize(cf->rrHeader, slotIdx * 2 + 4);
        s32 skelSize = std::max(skelSize1, skelSize2);
        if (skelSize <= 0) {
            LOG("[CharMgr] LoadCharacter: invalid player skeleton size for type %u", type);
            std::free(dataBuf);
            abortLoad();
            if (callback) callback->Callback();
            return;
        }

        u8* skelBuf = (u8*)std::malloc(skelSize);
        if (!skelBuf) {
            LOG("[CharMgr] LoadCharacter: out of memory allocating player skeleton (%d bytes)", skelSize);
            std::free(dataBuf);
            abortLoad();
            if (callback) callback->Callback();
            return;
        }

        cf->fileHandle->Seek((u32)rrOffset(cf->rrHeader, rrIdx - 1), ccFile::SEEK_FROM_START);
        if (cf->fileHandle->Read(skelBuf, (u32)skelSize) != skelSize) {
            LOG("[CharMgr] LoadCharacter: failed to read player skeleton resource for type %u", type);
            std::free(skelBuf);
            std::free(dataBuf);
            abortLoad();
            if (callback) callback->Callback();
            return;
        }

        slot.dataBuffer = skelBuf;
        g_playerMeshType = 0;
    }
    else {
        // NPC: read extra buffer (resource rrIdx-1)
        s32 extraSize = rrSize(cf->rrHeader, rrIdx - 1);
        if (extraSize <= 0) {
            LOG("[CharMgr] LoadCharacter: invalid NPC extra resource size for type %u", type);
            std::free(dataBuf);
            abortLoad();
            if (callback) callback->Callback();
            return;
        }

        u8* extraBuf = (u8*)std::malloc(extraSize);
        if (!extraBuf) {
            LOG("[CharMgr] LoadCharacter: out of memory allocating NPC extra buffer (%d bytes)", extraSize);
            std::free(dataBuf);
            abortLoad();
            if (callback) callback->Callback();
            return;
        }

        cf->fileHandle->Seek((u32)rrOffset(cf->rrHeader, rrIdx - 1), ccFile::SEEK_FROM_START);
        if (cf->fileHandle->Read(extraBuf, (u32)extraSize) != extraSize) {
            LOG("[CharMgr] LoadCharacter: failed to read NPC extra resource for type %u", type);
            std::free(extraBuf);
            std::free(dataBuf);
            abortLoad();
            if (callback) callback->Callback();
            return;
        }

        slot.dataBuffer = extraBuf;
    }

    // PSX: sets loadCount=2, then CharDataLoadCallback processes the P3D data.
    // PC: synchronous - process inline.
    slot.loadCount = 1;
    memset(slot.animIndexTable, 0xFF, CharSlot::ANIM_TABLE_SIZE);

    // PSX: CharDataLoadCallback creates 8 loaders on stack, calls P3DLoad.
    // P3D stream (resource rrIdx) contains:
    //   - 0x6008 Texture chunks (uploaded to VRAM)
    //   - 0x6122 Mapped STree (skeleton hierarchy)
    //   - 0x4007 CompAnim (composite animation reference)
    // The raw mesh data (tPrimGeom) is in slot.dataBuffer (resource rrIdx-1).
    //
    // PSX: P3DLoad with 8 loaders (tGeoLoader, tMatLoader, tPrimLoader,
    //      tSTreeLoader, tTexLoader, tClutAnimLoader, tTexAnimLoader, tCompAnimLoader)
    // PC: ParseP3DStreamFull extracts textures + skeleton and preserves the
    // character's 0x4007 composite animation definition for future suit work.
    const u32 compositeNameUID = GetCompositeAnimationNameHash(g_charNameTable[type]);
    CompositeAnimData* compositeAnim = nullptr;
    STreeData* skeleton = ParseP3DStreamFull(dataBuf, dataSize, &compositeAnim, compositeNameUID,
                                              g_charNameTable[type]);
    std::free(dataBuf);

    // PSX: CharDataLoadCallback post-processing:
    // 1. Finds loaded tSTree in P3D inventory
    // 2. Creates OriginalSTree (60 bytes), stores tSTree* at +36
    // 3. Sets rendering callbacks on the tSTree (RP_XformVertsLitCBF_CL, RP_FixUpPolysCBF_CL)
    // 4. Registers OriginalSTree in LevelManager for FindModel lookup
    // PC: parse tPrimGeom with skeleton to build per-joint mesh
    if (slot.dataBuffer && g_levelManager) {
        u32 nameHash = 0;
        if (cf->dataBuffer && cf->dataSize > 1) {
            nameHash = *(u32*)((u8*)cf->dataBuffer + 4);
        }

        OriginalBasic* existing = g_levelManager->FindModel((s32)nameHash);
        if (existing && existing->GetType() == 1) {
            slot.model = existing;
        }

        if (!existing) {
            OriginalSTree* original = new OriginalSTree();
            original->nameCRC = nameHash;
            original->SetStoreID(type == 0 ? 0 : 2);
            original->xformVertsCallback = RP_XformVertsLitCBF_CL;
            original->fixUpPolysCallback = RP_FixUpPolysCBF_CL;

            s32 bufSize = rrSize(cf->rrHeader, rrIdx - 1);
            u32 primGeomSize = 0;
            u32 primGeomOffset = 0;
            const u8* primGeomData = ResolveCharacterPrimGeom(
                (const u8*)slot.dataBuffer, (u32)bufSize, &primGeomSize, &primGeomOffset);

            if (primGeomData && primGeomOffset != 0) {
                LOG("[CharMgr] Resolved embedded tPrimGeom for type %u at +0x%X", type, primGeomOffset);
            }

            if (skeleton) {
                // Seed the freshly loaded STree once from anim 0 / frame 0 so joints with
                // no channels in the first active clip start from the same pose PSX has
                // after the default character animation is attached.
                s32 idleAnimSize = 0;
                u8* idleAnimBuf = cf->ReadResource(8, &idleAnimSize);
                if (idleAnimBuf) {
                    ApplyAnimFrame0(skeleton, idleAnimBuf, (u32)idleAnimSize);
                    std::free(idleAnimBuf);
                }

                original->skeleton = skeleton;
                skeleton = nullptr; // ownership transferred
                original->compositeAnim = compositeAnim;
                compositeAnim = nullptr;
                if (primGeomData) {
                    BuildPerJointMeshes(original, primGeomData, primGeomSize);
                }

                LOG("[CharMgr] Populated OriginalSTree with skeleton for type %u (hash 0x%08X, %u joints)",
                    type, nameHash, original->skeleton->numJoints);
            }
            else {
                // Fallback: flat mesh without skeleton
                pddiPrimBuffer* meshBuf = nullptr;
                if (primGeomData) {
                    meshBuf = BuildPrimBufferFromRawPrimGeom(primGeomData, primGeomSize
#if REAL_TEXTURE_RENDERING
                                                              , &original->realTexGroups
#endif
                                                              );
                }
                original->meshBuffer = meshBuf;

                LOG("[CharMgr] Populated OriginalSTree (flat) for type %u (hash 0x%08X)",
                    type, nameHash);
            }

            g_levelManager->AddOriginal(original, 0);
            slot.model = original;
        }
    }

    // Clean up skeleton if not transferred to OriginalSTree
    if (skeleton) {
        delete skeleton;
    }
    if (compositeAnim) {
        delete compositeAnim;
    }

    LOG("[CharMgr] Loaded character type %u into slot %d", type, slotIdx);

    if (callback) {
        callback->Callback();
    }
}

// PSX: UnloadCharacter__16CharacterManagerUs (CHARMGR.CPP:693, 0x80039C3C)
void CharacterManager::UnloadCharacter(u32 type) {
    MARKFUNCTION(0x80039C3C);

    s32 idx = FindSlot(type);
    if (idx < 0) return;

    CharSlot& slot = slots[idx];
    slot.loadCount--;
    // PSX branches on non-zero after decrement, so only an exact zero unloads.
    if (slot.loadCount != 0) return;

    u32 modelHash = 0;
    if (slot.charFile && slot.charFile->dataBuffer && slot.charFile->dataSize > 1) {
        modelHash = ReadRRU32((const u8*)slot.charFile->dataBuffer + 4);
    }

    // Host inventory does not expose the PSX list-handle delete API; removing
    // the registered OriginalSTree mirrors resource teardown on this path.
    if (g_levelManager && modelHash != 0) {
        OriginalBasic* original = g_levelManager->FindSTree((s32)modelHash);
        if (!original) {
            original = g_levelManager->FindModel(modelHash);
        }
        if (original) {
            g_levelManager->DeleteOriginal(original);
        }
    }

    slot.thingType = CharSlot::EMPTY_SENTINEL;

    // Release CharFile
    if (slot.charFile) {
        slot.charFile->DeleteRef();
        slot.charFile = nullptr;
    }

    // Free data buffer
    if (slot.dataBuffer) {
        std::free(slot.dataBuffer);
        slot.dataBuffer = nullptr;
    }

    slot.model = nullptr;
    slot.loadCount = 0;
    memset(slot.animIndexTable, 0xFF, CharSlot::ANIM_TABLE_SIZE);
}

// PSX: ReloadCharacter__16CharacterManagerUslP14CharMgrCallback (CHARMGR.CPP:792, 0x80039DC4)
void CharacterManager::ReloadCharacter(u32 type, s32 meshType, CharMgrCallback* callback) {
    MARKFUNCTION(0x80039DC4);

    s32 idx = FindSlot(type);
    if (idx < 0) {
        if (callback) callback->Callback();
        return;
    }

    g_playerMeshType = meshType;

    CharSlot& slot = slots[idx];
    CharFile* cf = slot.charFile;
    if (!cf) {
        if (callback) callback->Callback();
        return;
    }

    // PSX: meshType selects RR pair (2,3) or (4,5) before CharDataLoadCallback.
    s32 extraIdx = meshType ? 4 : 2;
    s32 rrIdx = extraIdx + 1;
    if (!IsValidRRResourceIndex(cf, extraIdx) || !IsValidRRResourceIndex(cf, rrIdx)) {
        if (callback) callback->Callback();
        return;
    }

    // Free old data buffer and re-read
    if (slot.dataBuffer) {
        std::free(slot.dataBuffer);
        slot.dataBuffer = nullptr;
    }

    s32 extraSize = rrSize(cf->rrHeader, extraIdx);
    u8* extraBuf = (u8*)std::malloc(extraSize);
    cf->fileHandle->Seek((u32)rrOffset(cf->rrHeader, extraIdx), ccFile::SEEK_FROM_START);
    cf->fileHandle->Read(extraBuf, (u32)extraSize);
    slot.dataBuffer = extraBuf;

    s32 dataSize = 0;
    u8* dataBuf = cf->ReadResource(rrIdx, &dataSize);
    if (!dataBuf || dataSize <= 0) {
        if (dataBuf) {
            std::free(dataBuf);
        }
        if (callback) callback->Callback();
        return;
    }

    // PSX: CharDataLoadCallback processes the TexturePage via P3DLoad
    P3DLoadTextures(dataBuf, dataSize, g_charNameTable[type]);

    if (g_levelManager) {
        u32 nameHash = 0;
        if (cf->dataBuffer && cf->dataSize > 1) {
            nameHash = ReadRRU32((const u8*)cf->dataBuffer + 4);
        }

        OriginalBasic* existing = g_levelManager->FindModel((s32)nameHash);
        OriginalSTree* original = nullptr;
        bool addOriginal = false;
        if (existing && existing->GetType() == 1) {
            original = static_cast<OriginalSTree*>(existing);
        }
        else if (!existing) {
            original = new OriginalSTree();
            addOriginal = true;
        }

        const u32 compositeNameUID = GetCompositeAnimationNameHash(g_charNameTable[type]);
        if (original && PopulateCharacterOriginal(original, cf, type,
                                                  static_cast<const u8*>(slot.dataBuffer), (u32)extraSize,
                                                  dataBuf, (u32)dataSize,
                                                  compositeNameUID)) {
            if (addOriginal) {
                g_levelManager->AddOriginal(original, 0);
            }
            slot.model = original;
        }
        else if (addOriginal) {
            delete original;
        }
    }

    std::free(dataBuf);

    if (callback) callback->Callback();
}

// PSX: LoadCharTexture__16CharacterManagerUs (CHARMGR.CPP:931, 0x8003A078)
void CharacterManager::LoadCharTexture(u32 type) {
    MARKFUNCTION(0x8003A078);

    s32 idx = FindSlot(type);
    if (idx < 0) return;

    CharSlot& slot = slots[idx];
    CharFile* cf = slot.charFile;
    if (!cf) return;

    // PSX: reloads the texture-bearing P3D resource (3 normally, 5 for player DM suit).
    s32 rrIdx = 3;
    if (type == 0 && g_scoreManager && g_scoreManager->IsDrunkenMasterSuitEnabled()) {
        if (IsValidRRResourceIndex(cf, 5)) {
            rrIdx = 5;
        }
    }

    s32 texSize = 0;
    u8* texBuf = cf->ReadResource(rrIdx, &texSize);
    if (!texBuf) return;

    // PSX: creates tTexLoader on stack, calls P3DLoad(loaders, texBuf, 0)
    // PC: use tP3DFileHandler to load the TexturePage resource
    P3DLoadTextures(texBuf, texSize, g_charNameTable[type]);
    std::free(texBuf);
}

// PSX: IsCharacterLoaded__16CharacterManagerUs (CHARMGR.CPP:1010, 0x8003A1A4)
bool CharacterManager::IsCharacterLoaded(u32 type) {
    MARKFUNCTION(0x8003A1A4);
    return FindSlot(type) >= 0;
}

// PSX: GetNumberCharactersLoaded__16CharacterManager (CHARMGR.CPP:1038, 0x8003A1D4)
s32 CharacterManager::GetNumberCharactersLoaded() {
    MARKFUNCTION(0x8003A1D4);
    s32 count = 0;
    for (s32 i = 0; i < CHAR_MAX_SLOTS; i++) {
        if (slots[i].thingType != CharSlot::EMPTY_SENTINEL) {
            count++;
        }
    }
    return count;
}

// PSX: EnableCache__16CharacterManagerUsi (CHARMGR.CPP:1073, 0x8003A20C)
// PSX: uses CharFile::Find(type) directly, not FindSlot.
void CharacterManager::EnableCache(u32 type, s32 enable) {
    MARKFUNCTION(0x8003A20C);
    CharFile* cf = CharFile::Find(type);
    if (cf) {
        cf->EnableCache(enable);
    }
}

// PSX: LoadAnimation (hash overload) (CHARMGR.CPP:1309, 0x8003A240)
// PSX: looks up hash in animHashTable to find animEnum, then delegates.
void CharacterManager::LoadAnimation(u32 type, u32 hash, CharMgrCallback* callback) {
    MARKFUNCTION(0x8003A240);

    const s32 idx = FindSlot(type);
    if (idx < 0) {
        return;
    }

    CharFile* cf = slots[idx].charFile;
    if (!cf) {
        return;
    }

    const s32 startEnum = cf->FindAnim(hash);
    const AnimGroupEntry* entry = FindAnimGroupEntry(startEnum);
    if (!entry) {
        return;
    }

    LoadAnimation(type, entry->startEnum, entry->count, callback);
}

// PSX: LoadAnimation (enum+count overload) (CHARMGR.CPP:1383, 0x8003A328)
// PSX: creates AnimCallback, delegates to LoadAnimationBatch.
void CharacterManager::LoadAnimation(u32 type, s32 animEnum, u32 count, CharMgrCallback* callback) {
    MARKFUNCTION(0x8003A328);

    if (count == 0) {
        if (callback) {
            callback->Callback();
        }
        return;
    }

    AnimCallback* ac = new AnimCallback(type, animEnum, count, callback);
    LoadAnimationBatch(type, animEnum, ac);
}

// PSX: LoadAnimationBatch (CHARMGR.CPP:1448, 0x8003A3EC)
// PSX: 1020 bytes. Checks if anim already loaded, allocates handle from free list,
//      reads P3D data from .RR, invokes callback.
void CharacterManager::LoadAnimationBatch(u32 type, s32 animEnum, CharMgrCallback* callback) {
    MARKFUNCTION(0x8003A3EC);

    s32 idx = FindSlot(type);
    if (idx < 0) {
        if (callback) callback->Callback();
        return;
    }

    CharSlot& slot = slots[idx];

    // Check if already loaded
    if (animEnum >= 0 && animEnum < (s32)CharSlot::ANIM_TABLE_SIZE) {
        if (slot.animIndexTable[animEnum] != 0xFF) {
            if (callback) callback->Callback();
            return;
        }
    }

    CharFile* cf = slot.charFile;
    if (!cf || !IsLiveCharFilePointer(cf)) {
        if (slot.dataBuffer) {
            std::free(slot.dataBuffer);
            slot.dataBuffer = nullptr;
        }

        slot.charFile = nullptr;
        slot.model = nullptr;
        slot.loadCount = 0;
        slot.thingType = CharSlot::EMPTY_SENTINEL;
        memset(slot.animIndexTable, 0xFF, CharSlot::ANIM_TABLE_SIZE);

        if (callback) callback->Callback();
        return;
    }

    u32 animNameUID = 0;
    if (cf->dataBuffer != nullptr) {
        const u32* animHashTable = static_cast<const u32*>(cf->dataBuffer);
        const s32 animHashIndex = animEnum + 2;
        if (animHashIndex >= 0 && animHashIndex < cf->dataSize) {
            animNameUID = animHashTable[animHashIndex];
        }
    }

    if (animNameUID == 0) {
        LOG("[AnimReq] type=%u enum=%d ABSENT idx=%d size=%d", type, animEnum, animEnum + 2, cf->dataSize);
        if (callback) callback->Callback();
        return;
    }

    for (s32 handleIdx = 0; handleIdx < CHAR_MAX_ANIMS; handleIdx++) {
        if ((animRefCounts[handleIdx] & 0x7F) == 0) {
            continue;
        }

        if (GetLoadedAnimationNameUID(animPtrs[handleIdx]) != animNameUID) {
            continue;
        }

        if (animEnum >= 0 && animEnum < (s32)CharSlot::ANIM_TABLE_SIZE) {
            animRefCounts[handleIdx]++;
            slot.animIndexTable[animEnum] = (u8)handleIdx;
            slot.loadCount++;
        }

        if (callback) callback->Callback();
        return;
    }

    // Allocate handle from free list
    if (freeListHead == nullptr) {
        if (callback) callback->Callback();
        return;
    }

    void** freeSlot = (void**)freeListHead;
    freeListHead = *freeSlot;
    s32 handleIdx = (s32)(freeSlot - &animPtrs[0]);

    animRefCounts[handleIdx] = 2; // PSX: initial refcount
    slot.loadCount++;

    // PSX: p3dIdx = animEnum * 2 + 9, paramIdx = animEnum * 2 + 8
    // Resource at paramIdx is raw animation data (P3D-independent binary format).
    // Resource at p3dIdx is a TexturePage (0xFF04) for animation-specific textures.
    s32 p3dIdx = animEnum * 2 + 9;
    s32 paramIdx = animEnum * 2 + 8;

    LOG("[AnimLoad] type=%u enum=%d nameUID=%08X paramIdx=%d p3dIdx=%d",
        type, animEnum, animNameUID, paramIdx, p3dIdx);

    // Read raw animation data from .RR (not P3D format)
    s32 animSize = 0;
    u8* animBuf = cf->ReadResource(paramIdx, &animSize);

    // Read and process TexturePage for this animation
    s32 p3dSize = 0;
    u8* p3dBuf = cf->ReadResource(p3dIdx, &p3dSize);
    if (p3dBuf) {
        // PSX: AnimLoadCallback creates loaders, calls P3DLoad for textures
        P3DLoadTextures(p3dBuf, p3dSize, g_charNameTable[type]);
    }

    // PSX decodes additional animation classes here; on the current host path,
    // character animation payloads resolve to TransformAnim or CameraParamAnim.

    if (animBuf && animSize > 0) {
        // ParseMulti handles single and multi-block (tSequenceAnim) payloads.
        // It takes ownership of animBuf on success; free it only on failure.
        void* parsed = TransformAnim::ParseMulti(animBuf, (u32)animSize);
        if (parsed) {
            animPtrs[handleIdx] = parsed;
        }
        else {
            LOG("[LoadAnimBatch] TransformAnim::ParseMulti FAILED for type=%u animEnum=%d size=%d - stored as CameraParamAnim", type, animEnum, animSize);
            CameraParamAnim* cameraAnim = ParseCameraParamAnim(animBuf, (u32)animSize, p3dBuf, (u32)p3dSize);
            std::free(animBuf);
            if (cameraAnim && cameraAnim->nameUID == 0) {
                cameraAnim->nameUID = animNameUID;
            }
            animPtrs[handleIdx] = cameraAnim;
        }
    }
    else {
        CameraParamAnim* cameraAnim = ParseCameraParamAnim(nullptr, 0, p3dBuf, (u32)p3dSize);
        if (cameraAnim && cameraAnim->nameUID == 0) {
            cameraAnim->nameUID = animNameUID;
        }
        animPtrs[handleIdx] = cameraAnim;
    }

    if (p3dBuf) {
        std::free(p3dBuf);
    }

    // Store handle in slot's anim table
    if (animEnum >= 0 && animEnum < (s32)CharSlot::ANIM_TABLE_SIZE) {
        slot.animIndexTable[animEnum] = (u8)handleIdx;
    }

    // Decrement initial refcount (PSX does this after callback)
    animRefCounts[handleIdx]--;

    if (callback) callback->Callback();
}

// PSX: UnloadAnimation (hash overload) (CHARMGR.CPP:1740, 0x8003A7E8)
void CharacterManager::UnloadAnimation(u32 type, u32 hash) {
    MARKFUNCTION(0x8003A7E8);

    const s32 idx = FindSlot(type);
    if (idx < 0) {
        return;
    }

    CharFile* cf = slots[idx].charFile;
    if (!cf) {
        return;
    }

    const s32 startEnum = cf->FindAnim(hash);
    const AnimGroupEntry* entry = FindAnimGroupEntry(startEnum);
    if (!entry) {
        return;
    }

    UnloadAnimation(type, entry->startEnum, entry->count);
}

// PSX: UnloadAnimation (enum+hash batch range) (CHARMGR.CPP:1813, 0x8003A8C0)
void CharacterManager::UnloadAnimation(u32 type, s32 startEnum, u32 count) {
    MARKFUNCTION(0x8003A8C0);

    s32 endEnum = startEnum + (s32)count;
    for (s32 e = startEnum; e < endEnum; e++) {
        UnloadAnimationBatch(type, e);
    }
}

// PSX: UnloadAnimationBatch (single enum) (CHARMGR.CPP:1838, 0x8003A930)
// PSX: 788 bytes. Complex cleanup of composite vs non-composite anims.
void CharacterManager::UnloadAnimationBatch(u32 type, s32 animEnum) {
    MARKFUNCTION(0x8003A930);

    s32 idx = FindSlot(type);
    if (idx < 0) return;

    CharSlot& slot = slots[idx];

    if (animEnum < 0 || animEnum >= (s32)CharSlot::ANIM_TABLE_SIZE) return;

    u8 handleIdx = slot.animIndexTable[animEnum];
    if (handleIdx == 0xFF) return;

    // Clear table entry
    slot.animIndexTable[animEnum] = 0xFF;

    // Decrement slot loadCount
    slot.loadCount--;

    // Decrement animation refcount
    animRefCounts[handleIdx]--;

    if ((animRefCounts[handleIdx] & 0x7F) != 0) {
        // Still referenced
        return;
    }

    // Host animation loads do not retain PSX inventory handles for these objects.
    // Release local memory and return the slot to the free list.
    bool isCached = (animRefCounts[handleIdx] & 0x80) != 0;
    if (!isCached) {
        FreeAnimMemory(animPtrs[handleIdx]);
    }

    animRefCounts[handleIdx] = 0;
    animPtrs[handleIdx] = freeListHead;
    freeListHead = &animPtrs[handleIdx];
}

// PSX: GetAnimation__16CharacterManagerUsQ2_2AI9AnimEnums (CHARMGR.CPP:1968, 0x8003AC44)
void* CharacterManager::GetAnimation(u32 type, s32 animEnum) {
    MARKFUNCTION(0x8003AC44);

    for (s32 i = 0; i < CHAR_MAX_SLOTS; i++) {
        if (slots[i].thingType == type) {
            if (animEnum < 0 || animEnum >= (s32)CharSlot::ANIM_TABLE_SIZE) return nullptr;
            u8 handle = slots[i].animIndexTable[animEnum];
            if (handle == 0xFF) return nullptr;
            return animPtrs[handle];
        }
    }
    return nullptr;
}

// PSX: LookUpAnimation__16CharacterManagerUsPCc (CHARMGR.CPP:2023, 0x8003ACBC)
// PSX: uses p3dHash(name) directly, NOT GetCompositeAnimationNameHash.
s32 CharacterManager::LookUpAnimation(u32 type, const char* name) {
    MARKFUNCTION(0x8003ACBC);

    u32 hash = p3dHash(name);

    for (s32 i = 0; i < CHAR_MAX_SLOTS; i++) {
        if (slots[i].thingType == type) {
            CharFile* cf = slots[i].charFile;
            if (cf) return cf->FindAnim(hash);
            return (s32)0xFFFF;
        }
    }
    return (s32)0xFFFF;
}

// PSX: PurgeLevel__16CharacterManager (CHARMGR.CPP:2054, 0x8003AD44)
// PSX: sets abort flag, drains task list, unloads slots 1-3 (NPCs),
//      deletes CharFiles for types 1-28, partially unloads player slot 0 anims.
void CharacterManager::PurgeLevel() {
    MARKFUNCTION(0x8003AD44);

    // PSX: g_abortFlag=1, drain task list, g_abortFlag=0
    // PC: no async tasks, skip drain

    // Unload slots 1-3 (NPCs)
    for (s32 i = 1; i < CHAR_MAX_SLOTS; i++) {
        u32 type = slots[i].thingType;
        if (type == CharSlot::EMPTY_SENTINEL) continue;

        // PSX: UnloadAnimation(type, 0, 392) = unload ALL animations
        UnloadAnimation(type, 0, CharSlot::ANIM_TABLE_SIZE);

        // Host guard: force full slot release even if instance-side references
        // were not balanced during UnPopulate.
        slots[i].loadCount = 1;

        UnloadCharacter(type);
        CloseCharacter(type);
    }

    // Delete all CharFiles for types 1-28
    for (u32 t = 1; t < 29; t++) {
        CharFile* cf = CharFile::Find(t);
        if (cf) cf->DeleteRef();
    }

    // Partially unload player slot 0 anims (range 124..392 = 268 count)
    u32 playerType = slots[0].thingType;
    if (playerType != CharSlot::EMPTY_SENTINEL) {
        UnloadAnimation(playerType, 124, 268);
    }
}

// PSX: InternalReset__16CharacterManager (0x8003BA80)
void CharacterManager::InternalReset() {
    MARKFUNCTION(0x8003BA80);
}

// PSX: InternalOpen__16CharacterManager (0x8003BA88)
void CharacterManager::InternalOpen() {
    MARKFUNCTION(0x8003BA88);
}

// PSX: InternalClose__16CharacterManager (0x8003BA90)
void CharacterManager::InternalClose() {
    MARKFUNCTION(0x8003BA90);
}

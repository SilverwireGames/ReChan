#include "common.h"
#include "gen/animmgr.h"
#include "gen/geometry.h"
#include "gen/skeleton.h"
#include "gen/model.h"
#include "gen/game.h"
#include "gen/world.h"
#include "p3d/byteread.h"
#include "p3d/context.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "extra/modloader.h"
#include "extra/realtexture.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <vector>

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

// P3D chunk IDs
static constexpr u16 CHUNK_P3D_CONTAINER = 0xFF04;
static constexpr u16 CHUNK_COMP_ANIM = 0x4007;
static constexpr u16 CHUNK_CLUT_ANIM = 0x6006;
static constexpr u16 CHUNK_P3D_TEXTURE = 0x6008;
static constexpr u16 CHUNK_STREE = 0x6120;
static constexpr u16 CHUNK_MAPPED_STREE = 0x6122;

static bool ReadPStringUID(const u8* data, u32 size, u32* pos, u32* outUID) {
    if (!data || !pos || !outUID || *pos >= size) {
        return false;
    }

    const u8 nameLen = data[*pos];
    (*pos)++;
    if (*pos + nameLen > size) {
        return false;
    }

    char nameBuf[256];
    memcpy(nameBuf, data + *pos, nameLen);
    nameBuf[nameLen] = 0;
    *pos += nameLen;

    *outUID = p3dHash(nameBuf);
    return true;
}

static CompositeAnimData* ParseCompositeAnimChunk(const u8* data, u32 size) {
    if (!data || size < 5) {
        return nullptr;
    }

    u32 pos = 0;
    u32 nameUID = 0;
    if (!ReadPStringUID(data, size, &pos, &nameUID) || pos + 4 > size) {
        return nullptr;
    }

    CompositeAnimData* compositeAnim = new CompositeAnimData();
    compositeAnim->nameUID = nameUID;
    compositeAnim->field12 = p3dReadU16LE(data + pos);
    pos += 2;
    compositeAnim->numParts = p3dReadU16LE(data + pos);
    pos += 2;

    if (compositeAnim->numParts != 0) {
        compositeAnim->parts = new CompositeAnimPartData[compositeAnim->numParts]();
    }

    for (u32 partIndex = 0; partIndex < compositeAnim->numParts; partIndex++) {
        CompositeAnimPartData& part = compositeAnim->parts[partIndex];
        if (!ReadPStringUID(data, size, &pos, &part.animNameUID) || pos + 4 > size) {
            delete compositeAnim;
            return nullptr;
        }

        part.field0 = p3dReadU16LE(data + pos);
        pos += 2;
        part.field1 = p3dReadU16LE(data + pos);
        pos += 2;
    }

    return compositeAnim;
}

static ClutAnimData* ParseClutAnimChunk(const u8* data, u32 size) {
    if (!data || size < 5) {
        return nullptr;
    }

    u32 pos = 0;
    u32 nameUID = 0;
    u32 materialUID = 0;
    u32 primUID = 0;
    if (!ReadPStringUID(data, size, &pos, &nameUID)
        || !ReadPStringUID(data, size, &pos, &materialUID)
        || !ReadPStringUID(data, size, &pos, &primUID)
        || pos + 2 > size) {
        return nullptr;
    }

    const u16 mode = p3dReadU16LE(data + pos);
    pos += 2;

    std::vector<u16> frames;
    std::vector<u16> offsets;

    while (pos + 6 <= size) {
        const u16 subChunkId = p3dReadU16LE(data + pos);
        const u32 subChunkSize = p3dReadU32LE(data + pos + 2);
        if (subChunkSize < 6 || pos + subChunkSize > size) {
            break;
        }

        const u8* subBody = data + pos + 6;
        const u32 subBodySize = subChunkSize - 6;
        if ((subChunkId == 0x600C || subChunkId == 0x600D) && subBodySize >= 2) {
            const u32 count = p3dReadU16LE(subBody + 0);
            if (count > 0 && 2u + count * 2u <= subBodySize) {
                std::vector<u16>& out = (subChunkId == 0x600C) ? frames : offsets;
                out.resize(count);
                for (u32 i = 0; i < count; i++) {
                    out[i] = p3dReadU16LE(subBody + 2u + i * 2u);
                }
            }
        }

        pos += subChunkSize;
    }

    if (nameUID == 0 || frames.empty()) {
        return nullptr;
    }

    ClutAnimData* clutAnim = new ClutAnimData();
    clutAnim->nameUID = nameUID;
    clutAnim->materialUID = materialUID;
    clutAnim->primUID = primUID;
    clutAnim->mode = mode;
    clutAnim->numFrames = static_cast<s32>(frames.size());
    clutAnim->frames = new u16[frames.size()];
    for (u32 i = 0; i < frames.size(); i++) {
        clutAnim->frames[i] = frames[i];
    }

    if (!offsets.empty()) {
        clutAnim->numOffsets = static_cast<s32>(offsets.size());
        clutAnim->offsets = new u16[offsets.size()];
        for (u32 i = 0; i < offsets.size(); i++) {
            clutAnim->offsets[i] = offsets[i];
        }
    }

    return clutAnim;
}

STreeData* ParseP3DStreamFull(const u8* data,
                              u32 size,
                              CompositeAnimData** outCompositeAnim,
                              u32 expectedCompositeNameUID,
                              const char* textureScope) {
    if (!data || size < 6) {
        if (outCompositeAnim) {
            *outCompositeAnim = nullptr;
        }
        return nullptr;
    }

    World* world = g_game ? g_game->GetWorld() : nullptr;
    STreeData* skeleton = nullptr;
    CompositeAnimData* compositeAnim = nullptr;
    std::vector<CompositeAnimData*> compositeAnims;
    std::vector<ClutAnimData*> clutAnims;

    u16 rootId = p3dReadU16LE(data);
    u32 rootSize = p3dReadU32LE(data + 2);
    if (rootId != CHUNK_P3D_CONTAINER) {
        return nullptr;
    }

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
        u16 chunkId = p3dReadU16LE(data + cpos);
        u32 chunkSize = p3dReadU32LE(data + cpos + 2);
        if (chunkSize < 6 || cpos + chunkSize > cend) {
            break;
        }

        if (chunkId == CHUNK_P3D_TEXTURE && world) {
            u32 tp = cpos + 6;
            u32 tend = cpos + chunkSize;

            std::string texName;
            if (tp < tend) {
                u8 nameLen = data[tp++];
                texName.assign(reinterpret_cast<const char*>(data + tp), nameLen);
                tp += nameLen;
                // PStrings may be padded with trailing nulls/spaces for alignment.
                while (!texName.empty() && (texName.back() == ' ' || texName.back() == '\0'))
                    texName.pop_back();
            }

            if (tp + 12 <= tend) {
                s16 rx = p3dReadS16LE(data + tp); tp += 2;
                s16 ry = p3dReadS16LE(data + tp); tp += 2;
                s16 rw = p3dReadS16LE(data + tp); tp += 2;
                s16 rh = p3dReadS16LE(data + tp); tp += 2;
                tp += 4; // type

                if (rw > 0 && rh > 0 && rw <= 1024 && rh <= 512 &&
                    tp + (u32)(rw * rh * 2) <= tend) {
                    world->UploadToVRAM(rx, ry, rw, rh, data + tp);

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
                                LOG("[ModLoader] Skeleton texture override: %s", texName.c_str());
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
                                LOG("[ModLoader] Skeleton texture override: %s", texName.c_str());
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
        }
        else if (chunkId == CHUNK_COMP_ANIM) {
            CompositeAnimData* parsedComposite = ParseCompositeAnimChunk(data + cpos + 6, chunkSize - 6);
            if (parsedComposite) {
                compositeAnims.push_back(parsedComposite);
            }
        }
        else if (chunkId == CHUNK_CLUT_ANIM) {
            ClutAnimData* clutAnim = ParseClutAnimChunk(data + cpos + 6, chunkSize - 6);
            if (clutAnim) {
                clutAnims.push_back(clutAnim);
            }
        }
        else if (chunkId == CHUNK_MAPPED_STREE) {
            if (!skeleton) {
                skeleton = ParseSTreeChunk(data + cpos + 6, chunkSize - 6, true);
            }
        }
        else if (chunkId == CHUNK_STREE) {
            if (!skeleton) {
                skeleton = ParseSTreeChunk(data + cpos + 6, chunkSize - 6, false);
            }
        }

        cpos += chunkSize;
    }

    if (world) {
        world->RefreshVRAMTexture();
    }

    if (skeleton) {
        LOG("[Skeleton] Parsed STree: %u joints, %u map entries",
            skeleton->numJoints, skeleton->numMapEntries);
    }

    CompositeAnimData* selectedComposite = nullptr;
    if (!compositeAnims.empty()) {
        if (expectedCompositeNameUID != 0) {
            for (CompositeAnimData* candidate : compositeAnims) {
                if (candidate && candidate->nameUID == expectedCompositeNameUID) {
                    selectedComposite = candidate;
                    break;
                }
            }

            if (!selectedComposite) {
                LOG("[Skeleton] Missing expected composite anim 0x%08X (parsed %u)",
                    expectedCompositeNameUID,
                    static_cast<u32>(compositeAnims.size()));
            }
        }
        else {
            selectedComposite = compositeAnims[0];
        }
    }

    if (selectedComposite) {
        u32 ownedCompositeCount = 0;
        for (CompositeAnimData* candidate : compositeAnims) {
            if (candidate && candidate != selectedComposite) {
                ownedCompositeCount++;
            }
        }

        if (ownedCompositeCount > 0) {
            selectedComposite->numCompositeAnims = ownedCompositeCount;
            selectedComposite->compositeAnims = new CompositeAnimData*[ownedCompositeCount]();

            u32 writeIndex = 0;
            for (CompositeAnimData* candidate : compositeAnims) {
                if (candidate && candidate != selectedComposite) {
                    selectedComposite->compositeAnims[writeIndex++] = candidate;
                }
            }
        }

        if (!clutAnims.empty()) {
            selectedComposite->numClutAnims = static_cast<u32>(clutAnims.size());
            selectedComposite->clutAnims = new ClutAnimData*[selectedComposite->numClutAnims]();
            for (u32 i = 0; i < selectedComposite->numClutAnims; i++) {
                selectedComposite->clutAnims[i] = clutAnims[i];
            }
        }

        for (CompositeAnimData* ownerComposite : compositeAnims) {
            if (!ownerComposite || ownerComposite->numParts == 0 || !ownerComposite->parts) {
                continue;
            }

            for (u32 partIndex = 0; partIndex < ownerComposite->numParts; partIndex++) {
                CompositeAnimPartData& part = ownerComposite->parts[partIndex];

                for (CompositeAnimData* targetComposite : compositeAnims) {
                    if (!targetComposite || targetComposite == ownerComposite) {
                        continue;
                    }

                    if (targetComposite->nameUID == part.animNameUID) {
                        part.compositeAnim = targetComposite;
                        break;
                    }
                }

                if (!part.compositeAnim && selectedComposite->clutAnims) {
                    for (u32 clutIndex = 0; clutIndex < selectedComposite->numClutAnims; clutIndex++) {
                        ClutAnimData* clutAnim = selectedComposite->clutAnims[clutIndex];
                        if (clutAnim && clutAnim->nameUID == part.animNameUID) {
                            part.clutAnim = clutAnim;
                            break;
                        }
                    }
                }
            }
        }

        compositeAnim = selectedComposite;
        LOG("[Skeleton] Selected composite anim 0x%08X with %u parts (linked composites=%u, clut=%u)",
            compositeAnim->nameUID,
            compositeAnim->numParts,
            compositeAnim->numCompositeAnims,
            compositeAnim->numClutAnims);

        compositeAnims.clear();
        clutAnims.clear();
    }
    else {
        for (CompositeAnimData* parsedComposite : compositeAnims) {
            delete parsedComposite;
        }
        compositeAnims.clear();

        for (ClutAnimData* parsedClut : clutAnims) {
            delete parsedClut;
        }
        clutAnims.clear();
    }

    if (outCompositeAnim) {
        *outCompositeAnim = compositeAnim;
    }
    else if (compositeAnim) {
        delete compositeAnim;
    }

    return skeleton;
}

// PSX tTransformAnim raw binary header layout (40 bytes):
//   +0:  uid (u32)
//   +4:  refCount (s32)
//   +8:  vtable slot (u32, unused pre-relocation)
//   +12: numFrames (s32)
//   +16: targetType (s32)
//   +20: targetNameUID (u32)
//   +24: numRotChannels (s32)
//   +28: numTransChannels (s32)
//   +32: rotChannelArrayOff (u32, DWORD offset)
//   +36: transChannelArrayOff (u32, DWORD offset)
//
// Channel layout (pre-relocation):
//   +0: jointParamIndex (u32) - index into jointOrderMap
//   +4: keyType (u32) - identifies key list type
//   +8+: type-specific data
//
// Key types for rotation channels:
//   11 = tStatic3DOFKeyList: +8/+12/+16 = static X/Y/Z values (s32, written as s16 to joint)
//    5 = tJoint3DOFangle: +8=numKeys, +12=keyTimesOff, +16=keyValuesOff (packed u32)
//    3 = tJoint1DOFangle: +8=numKeys, +12=keyTimesOff, +20=keyValuesOff (s16)
//
// Key types for translation channels:
//   12 = tStatic3DOFKeyList: +8/+12/+16 = static X/Y/Z values (s32)
//    8 = tJoint3DOFlpPSX: +8=numKeys, +12=keyTimesOff, +16=keyValuesOff (3 x s16)

void ApplyAnimFrame0(STreeData* skeleton, const u8* rawAnimData, u32 rawAnimSize) {
    if (!skeleton || !rawAnimData || rawAnimSize < 40) {
        return;
    }

    s32 numRotCh = (s32)p3dReadU32LE(rawAnimData + 24);
    s32 numTransCh = (s32)p3dReadU32LE(rawAnimData + 28);
    u32 rotArrayDwordOff = p3dReadU32LE(rawAnimData + 32);
    u32 transArrayDwordOff = p3dReadU32LE(rawAnimData + 36);

    u32 rotArrayByteOff = rotArrayDwordOff * 4;
    u32 transArrayByteOff = transArrayDwordOff * 4;

    if (rotArrayByteOff + (u32)numRotCh * 4 > rawAnimSize) {
        LOG("[Anim] Rotation channel array out of bounds");
        return;
    }
    if (transArrayByteOff + (u32)numTransCh * 4 > rawAnimSize) {
        LOG("[Anim] Translation channel array out of bounds");
        return;
    }

    // Process rotation channels
    for (s32 i = 0; i < numRotCh; i++) {
        u32 chDwordOff = p3dReadU32LE(rawAnimData + rotArrayByteOff + i * 4);
        u32 chByteOff = chDwordOff * 4;
        if (chByteOff + 20 > rawAnimSize) {
            continue;
        }

        const u8* ch = rawAnimData + chByteOff;
        u32 jointParam = p3dReadU32LE(ch + 0);
        u32 keyType = p3dReadU32LE(ch + 4);

        // Map animation parameter index to actual joint index
        if (jointParam >= skeleton->numMapEntries) {
            continue;
        }
        u32 jointIdx = skeleton->jointOrderMap[jointParam];
        if (jointIdx >= skeleton->numJoints) {
            continue;
        }
        STreeJoint& joint = skeleton->joints[jointIdx];

        if (keyType == 11) {
            // tStatic3DOFKeyList: values at +8, +12, +16
            s32 vx = (s32)p3dReadU32LE(ch + 8);
            s32 vy = (s32)p3dReadU32LE(ch + 12);
            s32 vz = (s32)p3dReadU32LE(ch + 16);
            joint.rotationX = (s16)vx;
            joint.rotationY = (s16)vy;
            joint.rotationZ = (s16)vz;
        }
        else if (keyType == 5) {
            // tJoint3DOFangle: read first keyframe value (packed u32)
            u32 numKeys = p3dReadU32LE(ch + 8);
            if (numKeys == 0) {
                continue;
            }
            u32 valDwordOff = p3dReadU32LE(ch + 16);
            u32 valByteOff = valDwordOff * 4;
            if (valByteOff + 4 > rawAnimSize) {
                continue;
            }
            // PSX CHANNEL.CPP packing for tJoint3DOFangle.
            u32 packed = p3dReadU32LE(rawAnimData + valByteOff);
            joint.rotationX = (s16)((packed << 5) & 0xFFFF);
            joint.rotationY = (s16)((packed >> 6) & 0xFFE0);
            joint.rotationZ = (s16)((packed >> 16) & 0xFFC0);
        }
        else if (keyType == 3) {
            // tJoint1DOFangle: +8=numKeys, +12=keyTimesOff(u8), +16=dofIndex,
            // +20=keyValuesOff(s16). Evaluate at frame 0.
            u32 numKeys = p3dReadU32LE(ch + 8);
            if (numKeys == 0) {
                continue;
            }
            u32 keyTimesOff = p3dReadU32LE(ch + 12);
            u32 dofIndex = p3dReadU32LE(ch + 16);
            u32 keyValsOff = p3dReadU32LE(ch + 20);
            u32 keyTimesByteOff = keyTimesOff * 4;
            u32 keyValsByteOff = keyValsOff * 4;
            if (keyTimesByteOff + numKeys > rawAnimSize) {
                continue;
            }
            if (keyValsByteOff + numKeys * 2 > rawAnimSize) {
                continue;
            }

            const u8* times = rawAnimData + keyTimesByteOff;
            s32 bracket = 0;
            if (numKeys > 1) {
                for (u32 k = 0; k < numKeys - 1; k++) {
                    if (0 >= (s32)times[k] && 0 < (s32)times[k + 1]) {
                        bracket = (s32)k;
                        break;
                    }
                    if (0 >= (s32)times[k + 1]) {
                        bracket = (s32)(k + 1);
                    }
                }
            }

            s16 val = p3dReadS16LE(rawAnimData + keyValsByteOff + bracket * 2);
            if (dofIndex == 0) {
                joint.rotationX = val;
            }
            else if (dofIndex == 1) {
                joint.rotationY = val;
            }
            else {
                joint.rotationZ = val;
            }
        }
        else {
            LOG("[Anim] Rot channel %d: unknown type %u", i, keyType);
        }
    }

    // Process translation channels
    for (s32 i = 0; i < numTransCh; i++) {
        u32 chDwordOff = p3dReadU32LE(rawAnimData + transArrayByteOff + i * 4);
        u32 chByteOff = chDwordOff * 4;
        if (chByteOff + 20 > rawAnimSize) {
            continue;
        }

        const u8* ch = rawAnimData + chByteOff;
        u32 jointParam = p3dReadU32LE(ch + 0);
        u32 keyType = p3dReadU32LE(ch + 4);

        if (jointParam >= skeleton->numMapEntries) {
            continue;
        }
        u32 jointIdx = skeleton->jointOrderMap[jointParam];
        if (jointIdx >= skeleton->numJoints) {
            continue;
        }
        STreeJoint& joint = skeleton->joints[jointIdx];

        if (keyType == 12) {
            // tStatic3DOFKeyList: values at +8, +12, +16
            joint.translationX = (s32)p3dReadU32LE(ch + 8);
            joint.translationY = (s32)p3dReadU32LE(ch + 12);
            joint.translationZ = (s32)p3dReadU32LE(ch + 16);
        }
        else if (keyType == 8) {
            // tJoint3DOFlpPSX: read first keyframe (3 x s16)
            u32 numKeys = p3dReadU32LE(ch + 8);
            if (numKeys == 0) {
                continue;
            }
            u32 valDwordOff = p3dReadU32LE(ch + 16);
            u32 valByteOff = valDwordOff * 4;
            if (valByteOff + 6 > rawAnimSize) {
                continue;
            }
            joint.translationX = p3dReadS16LE(rawAnimData + valByteOff + 0);
            joint.translationY = p3dReadS16LE(rawAnimData + valByteOff + 2);
            joint.translationZ = p3dReadS16LE(rawAnimData + valByteOff + 4);
        }
        else {
            LOG("[Anim] Trans channel %d: unknown type %u", i, keyType);
        }
    }

    // Save a stable bind baseline used by runtime animation evaluation.
    for (u32 i = 0; i < skeleton->numJoints; i++) {
        STreeJoint& j = skeleton->joints[i];
        j.bindTranslationX = j.translationX;
        j.bindTranslationY = j.translationY;
        j.bindTranslationZ = j.translationZ;
        j.bindRotationX = j.rotationX;
        j.bindRotationY = j.rotationY;
        j.bindRotationZ = j.rotationZ;
    }

}

void BuildPerJointMeshes(OriginalSTree* original, const u8* primGeomData, u32 primGeomSize) {
    if (original) {
        if (original->primGeom) {
            delete original->primGeom;
            original->primGeom = nullptr;
        }
        original->primGeom = CloneRawPrimGeom(primGeomData, primGeomSize);
    }

    STreeData* skeleton = original->skeleton;
    if (!skeleton || !primGeomData || primGeomSize < 108) {
        return;
    }

    // Parse tPrimGeom header
    u32 vertListOff = p3dReadU32LE(primGeomData + 0x10) << 2;
    u16 numVerts = p3dReadU16LE(primGeomData + 0x14);
    u16 numPolys = p3dReadU16LE(primGeomData + 0x16);
    u32 primListOff = p3dReadU32LE(primGeomData + 0x40) << 2;
    u32 polyDataOff = p3dReadU32LE(primGeomData + 0x54) << 2;
    u32 loopCtOff = p3dReadU32LE(primGeomData + 0x60) << 2;
    u32 loopPrimOff = p3dReadU32LE(primGeomData + 0x68) << 2;
    s16 numLoops = p3dReadS16LE(primGeomData + 0x66);

    if (numVerts == 0 || numPolys == 0) {
        return;
    }
    if (numLoops < 1) numLoops = 1;

    if (vertListOff + numVerts * 8 > primGeomSize) {
        return;
    }
    if (polyDataOff + numPolys * 4 > primGeomSize) {
        return;
    }

    const u8* verts = primGeomData + vertListOff;
    const u8* polys = primGeomData + polyDataOff;

    // Build per-loop vertex bases and primitive-class counts.
    // PSX layout:
    //   +0x60: loop vertex entries (lo16 = vert count)
    //   +0x68: loop primitive counts (u16[4] = GT4,G4,GT3,G3)
    struct LoopInfo {
        u16 vertCount;
        u16 vertBase;
        u16 primCounts[4];
    };
    std::vector<LoopInfo> loops;
    u16 vertAccum = 0;
    if (loopCtOff + numLoops * 4 <= primGeomSize && loopPrimOff + numLoops * 8 <= primGeomSize) {
        for (int i = 0; i < numLoops; i++) {
            LoopInfo li;
            li.vertCount = p3dReadU16LE(primGeomData + loopCtOff + i * 4);
            li.vertBase = vertAccum;
            li.primCounts[0] = p3dReadU16LE(primGeomData + loopPrimOff + i * 8 + 0);
            li.primCounts[1] = p3dReadU16LE(primGeomData + loopPrimOff + i * 8 + 2);
            li.primCounts[2] = p3dReadU16LE(primGeomData + loopPrimOff + i * 8 + 4);
            li.primCounts[3] = p3dReadU16LE(primGeomData + loopPrimOff + i * 8 + 6);
            vertAccum += li.vertCount;
            loops.push_back(li);
        }
    }
    if (loops.empty()) {
        LOG("[Skeleton] BuildPerJointMeshes missing loop table data (numLoops=%d, loopCtOff=%u, loopPrimOff=%u, size=%u)",
            numLoops,
            loopCtOff,
            loopPrimOff,
            primGeomSize);
        return;
    }

    if (vertAccum > numVerts) {
        LOG("[Skeleton] BuildPerJointMeshes invalid loop vert accumulation (%u > %u)", vertAccum, numVerts);
        return;
    }

    u32 expectedPolyCount = 0;
    for (const LoopInfo& li : loops) {
        expectedPolyCount += static_cast<u32>(li.primCounts[0]);
        expectedPolyCount += static_cast<u32>(li.primCounts[1]);
        expectedPolyCount += static_cast<u32>(li.primCounts[2]);
        expectedPolyCount += static_cast<u32>(li.primCounts[3]);
    }
    if (expectedPolyCount != numPolys) {
        LOG("[Skeleton] BuildPerJointMeshes poly-count mismatch expected=%u numPolys=%u", expectedPolyCount, numPolys);
    }

    // Build vertex-to-joint map: primGeomStartIdx/primGeomCount are vertex ranges
    std::vector<u32> vertJointMap(numVerts, 0);
    for (u32 j = 0; j < skeleton->numJoints; j++) {
        const STreeJoint& jt = skeleton->joints[j];
        if (!(jt.flags & STF_HAS_MESH)) continue;
        for (u32 v = 0; v < jt.primGeomCount; v++) {
            u32 vi = jt.primGeomStartIdx + v;
            if (vi < numVerts) {
                vertJointMap[vi] = j;
            }
        }
    }

    // Collect SkinVertex data (joint-local space) + indices
    std::vector<SkinVertex> skinVerts;
    std::vector<u16> allIndices;
    std::vector<u32> primStart;
    std::vector<u8> primVertCount;
    std::vector<u32> primPacketOffset;
    std::vector<u8> primPacketSize;
    std::vector<u32> builtPerLoop(loops.size(), 0);
    std::vector<u32> builtPerLoopBucket(loops.size() * 4u, 0);
    bool usesSemiTrans = false;
    bool hasSemiTransMode = false;
    u8 semiTransMode = 0;
    u32 bucketCommandMismatchWarnBudget = 16;
    u32 unknownCmdCount = 0;
    u32 headerOverrunCount = 0;
    u32 packetOverrunCount = 0;
    u32 polyExhaustCount = 0;
    u32 bucketCmdSizeMismatchCount = 0;

    u32 primCursor = primListOff;
    u32 polyIdx = 0;

    for (int loop = 0; loop < (int)loops.size(); loop++) {
        u16 vertBase = loops[loop].vertBase;

        for (u32 bucket = 0; bucket < 4; bucket++) {
            const u32 expectedPacketSize = (bucket == 0u) ? 52u : (bucket == 1u) ? 36u : (bucket == 2u) ? 40u : 28u;

            for (u32 count = 0; count < loops[loop].primCounts[bucket]; count++) {
                if (polyIdx >= numPolys) {
                    polyExhaustCount++;
                    break;
                }

                if (primCursor + 8u > primGeomSize) {
                    headerOverrunCount++;
                    LOG("[Skeleton] BuildPerJointMeshes packet header overrun: loop=%d bucket=%u polyIdx=%u rem=%u",
                        loop,
                        bucket,
                        polyIdx,
                        primGeomSize - primCursor);
                    break;
                }

                const u8* pkt = primGeomData + primCursor;
                u8 cmd = pkt[7];
                u8 cmdBase = cmd & 0xFC;

                u32 packetSize = 0;
                if (cmdBase == 0x3Cu) packetSize = 52u;
                else if (cmdBase == 0x2Cu) packetSize = 40u;
                else if (cmdBase == 0x38u) packetSize = 36u;
                else if (cmdBase == 0x28u) packetSize = 36u;
                else if (cmdBase == 0x34u) packetSize = 40u;
                else if (cmdBase == 0x24u) packetSize = 32u;
                else if (cmdBase == 0x30u) packetSize = 28u;
                else if (cmdBase == 0x20u) packetSize = 28u;
                else {
                    unknownCmdCount++;
                    if (bucketCommandMismatchWarnBudget > 0) {
                        LOG("[Skeleton] BuildPerJointMeshes unknown cmd base 0x%02X at loop=%d bucket=%u polyIdx=%u",
                            cmdBase,
                            loop,
                            bucket,
                            polyIdx);
                        bucketCommandMismatchWarnBudget--;
                    }
                    break;
                }

                if (packetSize != expectedPacketSize && bucketCommandMismatchWarnBudget > 0) {
                    bucketCmdSizeMismatchCount++;
                    LOG("[Skeleton] BuildPerJointMeshes bucket/cmd size mismatch loop=%d bucket=%u cmd=0x%02X expected=%u actual=%u polyIdx=%u",
                        loop,
                        bucket,
                        cmdBase,
                        expectedPacketSize,
                        packetSize,
                        polyIdx);
                    bucketCommandMismatchWarnBudget--;
                }

                if (primCursor + packetSize > primGeomSize) {
                    packetOverrunCount++;
                    LOG("[Skeleton] BuildPerJointMeshes packet overrun: loop=%d bucket=%u polyIdx=%u need=%u rem=%u",
                        loop,
                        bucket,
                        polyIdx,
                        packetSize,
                        primGeomSize - primCursor);
                    break;
                }

                if ((cmd & 0x2u) != 0u) {
                    usesSemiTrans = true;
                }

                const u32 packetOffset = primCursor - primListOff;
                const u8 packetSizeByte = static_cast<u8>(packetSize);

                if (bucket == 0u) ASSERT(cmdBase == 0x3C || cmdBase == 0x2C);
                if (bucket == 1u) ASSERT(cmdBase == 0x38 || cmdBase == 0x28);
                if (bucket == 2u) ASSERT(cmdBase == 0x34 || cmdBase == 0x24);
                if (bucket == 3u) ASSERT(cmdBase == 0x30 || cmdBase == 0x20);

                const u8* poly = polys + polyIdx * 4;
                u8 vi0 = poly[0], vi1 = poly[1], vi2 = poly[2], vi3 = poly[3];

            auto makeSkinVert = [&](u8 vi) -> SkinVertex {
                u32 idx = vertBase + vi;
                if (idx >= numVerts) idx = 0;
                const u8* vp = verts + idx * 8;
                SkinVertex sv;
                sv.lx = (f32)p3dReadS16LE(vp + 0);
                sv.ly = (f32)p3dReadS16LE(vp + 2);
                sv.lz = (f32)p3dReadS16LE(vp + 4);
                sv.r = 0.7f; sv.g = 0.7f; sv.b = 0.7f;
                sv.u = 0.0f; sv.v = 0.0f;
                sv.tpage = -1.0f; sv.cba = 0.0f;
                sv.jointIdx = vertJointMap[idx];
                sv.sourceIndex = static_cast<u16>(idx);
                sv.padSourceIndex = 0;
                return sv;
            };

                auto readRGB = [&](int off) {
                    if (off + 3 > (int)packetSize) return std::make_tuple(0.5f, 0.5f, 0.5f);
                f32 r = pkt[off] / 128.0f;
                f32 g = pkt[off + 1] / 128.0f;
                f32 b = pkt[off + 2] / 128.0f;
                return std::make_tuple(r, g, b);
            };

                if (cmdBase == 0x3C) {
                if (packetSize < 52) { primCursor += packetSize; polyIdx++; continue; }
                SkinVertex v0 = makeSkinVert(vi0), v1 = makeSkinVert(vi1);
                SkinVertex v2 = makeSkinVert(vi2), v3 = makeSkinVert(vi3);
                const u16 tpage = p3dReadU16LE(pkt + 26);
                f32 tp = (f32)tpage;
                f32 cb = (f32)p3dReadU16LE(pkt + 14);
                if ((cmd & 0x2u) != 0u && !hasSemiTransMode) {
                    semiTransMode = static_cast<u8>((tpage >> 5) & 3u);
                    hasSemiTransMode = true;
                }
                auto [r0, g0, b0] = readRGB(4);
                auto [r1, g1, b1] = readRGB(16);
                auto [r2, g2, b2] = readRGB(28);
                auto [r3, g3, b3] = readRGB(40);
                v0.r = r0; v0.g = g0; v0.b = b0; v1.r = r1; v1.g = g1; v1.b = b1;
                v2.r = r2; v2.g = g2; v2.b = b2; v3.r = r3; v3.g = g3; v3.b = b3;
                v0.u = pkt[12]; v0.v = pkt[13]; v0.tpage = tp; v0.cba = cb;
                v1.u = pkt[24]; v1.v = pkt[25]; v1.tpage = tp; v1.cba = cb;
                v2.u = pkt[36]; v2.v = pkt[37]; v2.tpage = tp; v2.cba = cb;
                v3.u = pkt[48]; v3.v = pkt[49]; v3.tpage = tp; v3.cba = cb;
                u16 base = (u16)skinVerts.size();
                primStart.push_back(base);
                primVertCount.push_back(4);
                primPacketOffset.push_back(packetOffset);
                primPacketSize.push_back(packetSizeByte);
                skinVerts.push_back(v0); skinVerts.push_back(v1);
                skinVerts.push_back(v2); skinVerts.push_back(v3);
                allIndices.push_back(base); allIndices.push_back(base + 1); allIndices.push_back(base + 2);
                allIndices.push_back(base + 1); allIndices.push_back(base + 3); allIndices.push_back(base + 2);

                }
                else if (cmdBase == 0x2C) {
                if (packetSize < 40) { primCursor += packetSize; polyIdx++; continue; }
                SkinVertex v0 = makeSkinVert(vi0), v1 = makeSkinVert(vi1);
                SkinVertex v2 = makeSkinVert(vi2), v3 = makeSkinVert(vi3);
                const u16 tpage = p3dReadU16LE(pkt + 22);
                f32 tp = (f32)tpage;
                f32 cb = (f32)p3dReadU16LE(pkt + 14);
                if ((cmd & 0x2u) != 0u && !hasSemiTransMode) {
                    semiTransMode = static_cast<u8>((tpage >> 5) & 3u);
                    hasSemiTransMode = true;
                }
                auto [r0, g0, b0] = readRGB(4);
                v0.r = r0; v0.g = g0; v0.b = b0; v1.r = r0; v1.g = g0; v1.b = b0;
                v2.r = r0; v2.g = g0; v2.b = b0; v3.r = r0; v3.g = g0; v3.b = b0;
                v0.u = pkt[12]; v0.v = pkt[13]; v0.tpage = tp; v0.cba = cb;
                v1.u = pkt[20]; v1.v = pkt[21]; v1.tpage = tp; v1.cba = cb;
                v2.u = pkt[28]; v2.v = pkt[29]; v2.tpage = tp; v2.cba = cb;
                v3.u = pkt[36]; v3.v = pkt[37]; v3.tpage = tp; v3.cba = cb;
                u16 base = (u16)skinVerts.size();
                primStart.push_back(base);
                primVertCount.push_back(4);
                primPacketOffset.push_back(packetOffset);
                primPacketSize.push_back(packetSizeByte);
                skinVerts.push_back(v0); skinVerts.push_back(v1);
                skinVerts.push_back(v2); skinVerts.push_back(v3);
                allIndices.push_back(base); allIndices.push_back(base + 1); allIndices.push_back(base + 2);
                allIndices.push_back(base + 1); allIndices.push_back(base + 3); allIndices.push_back(base + 2);

                }
                else if (cmdBase == 0x34) {
                if (packetSize < 40) { primCursor += packetSize; polyIdx++; continue; }
                SkinVertex v0 = makeSkinVert(vi0), v1 = makeSkinVert(vi1), v2 = makeSkinVert(vi2);
                const u16 tpage = p3dReadU16LE(pkt + 26);
                f32 tp = (f32)tpage;
                f32 cb = (f32)p3dReadU16LE(pkt + 14);
                if ((cmd & 0x2u) != 0u && !hasSemiTransMode) {
                    semiTransMode = static_cast<u8>((tpage >> 5) & 3u);
                    hasSemiTransMode = true;
                }
                auto [r0, g0, b0] = readRGB(4);
                auto [r1, g1, b1] = readRGB(16);
                auto [r2, g2, b2] = readRGB(28);
                v0.r = r0; v0.g = g0; v0.b = b0;
                v1.r = r1; v1.g = g1; v1.b = b1;
                v2.r = r2; v2.g = g2; v2.b = b2;
                v0.u = pkt[12]; v0.v = pkt[13]; v0.tpage = tp; v0.cba = cb;
                v1.u = pkt[24]; v1.v = pkt[25]; v1.tpage = tp; v1.cba = cb;
                v2.u = pkt[36]; v2.v = pkt[37]; v2.tpage = tp; v2.cba = cb;
                u16 base = (u16)skinVerts.size();
                primStart.push_back(base);
                primVertCount.push_back(3);
                primPacketOffset.push_back(packetOffset);
                primPacketSize.push_back(packetSizeByte);
                skinVerts.push_back(v0); skinVerts.push_back(v1); skinVerts.push_back(v2);
                allIndices.push_back(base); allIndices.push_back(base + 1); allIndices.push_back(base + 2);

                }
                else if (cmdBase == 0x24) {
                if (packetSize < 32) { primCursor += packetSize; polyIdx++; continue; }
                SkinVertex v0 = makeSkinVert(vi0), v1 = makeSkinVert(vi1), v2 = makeSkinVert(vi2);
                const u16 tpage = p3dReadU16LE(pkt + 22);
                f32 tp = (f32)tpage;
                f32 cb = (f32)p3dReadU16LE(pkt + 14);
                if ((cmd & 0x2u) != 0u && !hasSemiTransMode) {
                    semiTransMode = static_cast<u8>((tpage >> 5) & 3u);
                    hasSemiTransMode = true;
                }
                auto [r0, g0, b0] = readRGB(4);
                v0.r = r0; v0.g = g0; v0.b = b0;
                v1.r = r0; v1.g = g0; v1.b = b0;
                v2.r = r0; v2.g = g0; v2.b = b0;
                v0.u = pkt[12]; v0.v = pkt[13]; v0.tpage = tp; v0.cba = cb;
                v1.u = pkt[20]; v1.v = pkt[21]; v1.tpage = tp; v1.cba = cb;
                v2.u = pkt[28]; v2.v = pkt[29]; v2.tpage = tp; v2.cba = cb;
                u16 base = (u16)skinVerts.size();
                primStart.push_back(base);
                primVertCount.push_back(3);
                primPacketOffset.push_back(packetOffset);
                primPacketSize.push_back(packetSizeByte);
                skinVerts.push_back(v0); skinVerts.push_back(v1); skinVerts.push_back(v2);
                allIndices.push_back(base); allIndices.push_back(base + 1); allIndices.push_back(base + 2);

                }
                else if (cmdBase == 0x38 || cmdBase == 0x28) {
                SkinVertex v0 = makeSkinVert(vi0), v1 = makeSkinVert(vi1);
                SkinVertex v2 = makeSkinVert(vi2), v3 = makeSkinVert(vi3);
                if (cmdBase == 0x38) {
                    auto [r0, g0, b0] = readRGB(4);
                    auto [r1, g1, b1] = readRGB(12);
                    auto [r2, g2, b2] = readRGB(20);
                    auto [r3, g3, b3] = readRGB(28);
                    v0.r = r0; v0.g = g0; v0.b = b0; v1.r = r1; v1.g = g1; v1.b = b1;
                    v2.r = r2; v2.g = g2; v2.b = b2; v3.r = r3; v3.g = g3; v3.b = b3;
                }
                else {
                    auto [r0, g0, b0] = readRGB(4);
                    v0.r = r0; v0.g = g0; v0.b = b0; v1.r = r0; v1.g = g0; v1.b = b0;
                    v2.r = r0; v2.g = g0; v2.b = b0; v3.r = r0; v3.g = g0; v3.b = b0;
                }
                u16 base = (u16)skinVerts.size();
                primStart.push_back(base);
                primVertCount.push_back(4);
                primPacketOffset.push_back(packetOffset);
                primPacketSize.push_back(packetSizeByte);
                skinVerts.push_back(v0); skinVerts.push_back(v1);
                skinVerts.push_back(v2); skinVerts.push_back(v3);
                allIndices.push_back(base); allIndices.push_back(base + 1); allIndices.push_back(base + 2);
                allIndices.push_back(base + 1); allIndices.push_back(base + 3); allIndices.push_back(base + 2);

                }
                else if (cmdBase == 0x30 || cmdBase == 0x20) {
                SkinVertex v0 = makeSkinVert(vi0), v1 = makeSkinVert(vi1), v2 = makeSkinVert(vi2);
                if (cmdBase == 0x30) {
                    auto [r0, g0, b0] = readRGB(4);
                    auto [r1, g1, b1] = readRGB(12);
                    auto [r2, g2, b2] = readRGB(20);
                    v0.r = r0; v0.g = g0; v0.b = b0;
                    v1.r = r1; v1.g = g1; v1.b = b1;
                    v2.r = r2; v2.g = g2; v2.b = b2;
                }
                else {
                    auto [r0, g0, b0] = readRGB(4);
                    v0.r = r0; v0.g = g0; v0.b = b0;
                    v1.r = r0; v1.g = g0; v1.b = b0;
                    v2.r = r0; v2.g = g0; v2.b = b0;
                }
                u16 base = (u16)skinVerts.size();
                primStart.push_back(base);
                primVertCount.push_back(3);
                primPacketOffset.push_back(packetOffset);
                primPacketSize.push_back(packetSizeByte);
                skinVerts.push_back(v0); skinVerts.push_back(v1); skinVerts.push_back(v2);
                allIndices.push_back(base); allIndices.push_back(base + 1); allIndices.push_back(base + 2);
                }

                primCursor += packetSize;
                polyIdx++;
                builtPerLoop[loop]++;
                builtPerLoopBucket[loop * 4 + bucket]++;
            }
        }
    }

    u32 builtPrimTotal = 0;
    for (u32 i = 0; i < builtPerLoop.size(); i++) {
        builtPrimTotal += builtPerLoop[i];
    }

    LOG("[Skeleton] BuildPerJointMeshes summary expected=%u numPolys=%u builtPrims=%u polyIdxEnd=%u primCursorOff=%u breaks(polyExhaust=%u headerOverrun=%u packetOverrun=%u unknownCmd=%u sizeMismatch=%u)",
        expectedPolyCount,
        static_cast<u32>(numPolys),
        builtPrimTotal,
        polyIdx,
        primCursor - primListOff,
        polyExhaustCount,
        headerOverrunCount,
        packetOverrunCount,
        unknownCmdCount,
        bucketCmdSizeMismatchCount);

    for (u32 loop = 0; loop < builtPerLoop.size(); loop++) {
        LOG("[Skeleton] BuildPerJointMeshes loop=%u planned=%u/%u/%u/%u built=%u/%u/%u/%u",
            loop,
            static_cast<u32>(loops[loop].primCounts[0]),
            static_cast<u32>(loops[loop].primCounts[1]),
            static_cast<u32>(loops[loop].primCounts[2]),
            static_cast<u32>(loops[loop].primCounts[3]),
            builtPerLoopBucket[loop * 4 + 0],
            builtPerLoopBucket[loop * 4 + 1],
            builtPerLoopBucket[loop * 4 + 2],
            builtPerLoopBucket[loop * 4 + 3]);
    }

    if (allIndices.empty()) {
        return;
    }

#if REAL_TEXTURE_RENDERING
    // Re-sort the (already complete, triangle-list) index buffer by each
    // triangle's (tpage, cba) so the real-texture path can sub-range-draw
    // one material at a time. Vertex data/order is untouched -- only the
    // index order changes, so the legacy single-draw path is unaffected.
    if (original) {
        const u32 numTris = static_cast<u32>(allIndices.size() / 3);
        std::vector<u32> order(numTris);
        for (u32 t = 0; t < numTris; t++) order[t] = t;

        auto keyOf = [&](u32 tri) -> u32 {
            const SkinVertex& sv = skinVerts[allIndices[tri * 3]];
            return (static_cast<u32>(static_cast<u16>(sv.tpage)) << 16) | static_cast<u16>(sv.cba);
        };
        std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return keyOf(a) < keyOf(b); });

        std::vector<u16> groupedIndices(allIndices.size());
        original->realTexGroups.clear();
        u32 groupStart = 0;
        u32 prevKey = (numTris > 0) ? keyOf(order[0]) : 0;
        for (u32 i = 0; i < numTris; i++) {
            const u32 tri = order[i];
            groupedIndices[i * 3 + 0] = allIndices[tri * 3 + 0];
            groupedIndices[i * 3 + 1] = allIndices[tri * 3 + 1];
            groupedIndices[i * 3 + 2] = allIndices[tri * 3 + 2];

            const u32 key = keyOf(tri);
            if (key != prevKey) {
                original->realTexGroups.push_back(RealTextureGroup{
                    groupStart, i * 3 - groupStart,
                    static_cast<u16>(prevKey >> 16), static_cast<u16>(prevKey & 0xFFFFu) });
                groupStart = i * 3;
                prevKey = key;
            }
        }
        if (numTris > 0) {
            original->realTexGroups.push_back(RealTextureGroup{
                groupStart, static_cast<u32>(allIndices.size()) - groupStart,
                static_cast<u16>(prevKey >> 16), static_cast<u16>(prevKey & 0xFFFFu) });
        }
        allIndices.swap(groupedIndices);
    }
#endif

    // Store SkinData for per-frame CPU skinning
    SkinData* sd = new SkinData();
    sd->numVerts = (u32)skinVerts.size();
    sd->numIndices = (u32)allIndices.size();
    sd->numPrims = (u32)primStart.size();
    sd->usesSemiTrans = usesSemiTrans;
    sd->semiTransMode = semiTransMode;
    sd->verts = new SkinVertex[sd->numVerts];
    sd->indices = new u16[sd->numIndices];
    sd->primStart = new u32[sd->numPrims];
    sd->primVertCount = new u8[sd->numPrims];
    sd->primPacketOffset = new u32[sd->numPrims];
    sd->primPacketSize = new u8[sd->numPrims];
    memcpy(sd->verts, skinVerts.data(), sd->numVerts * sizeof(SkinVertex));
    memcpy(sd->indices, allIndices.data(), sd->numIndices * sizeof(u16));
    if (sd->numPrims > 0) {
        memcpy(sd->primStart, primStart.data(), sd->numPrims * sizeof(u32));
        memcpy(sd->primVertCount, primVertCount.data(), sd->numPrims * sizeof(u8));
        memcpy(sd->primPacketOffset, primPacketOffset.data(), sd->numPrims * sizeof(u32));
        memcpy(sd->primPacketSize, primPacketSize.data(), sd->numPrims * sizeof(u8));
    }
    original->skinData = sd;

    // Build initial mesh from current joint transforms
    Mat4* jointMatrices = new Mat4[skeleton->numJoints];
    skeleton->ComputeWorldMatrices(jointMatrices);

    u32 format = PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO;
    pddiPrimBufferDesc desc(PDDI_PRIM_TRIANGLES, format,
                            sd->numVerts, sd->numIndices);

    pddiPrimBuffer* buffer = p3d::device->NewPrimBuffer(desc);

    std::vector<f32> vertData(sd->numVerts * 10);
    for (u32 i = 0; i < sd->numVerts; i++) {
        const SkinVertex& sv = sd->verts[i];
        const Mat4& m = jointMatrices[sv.jointIdx];
        f32 wx, wy, wz;
        Mat4TransformPoint(m, sv.lx, sv.ly, sv.lz, wx, wy, wz);
        vertData[i * 10 + 0] = wx;
        vertData[i * 10 + 1] = wy;
        vertData[i * 10 + 2] = wz;
        vertData[i * 10 + 3] = sv.r;
        vertData[i * 10 + 4] = sv.g;
        vertData[i * 10 + 5] = sv.b;
        vertData[i * 10 + 6] = sv.u;
        vertData[i * 10 + 7] = sv.v;
        vertData[i * 10 + 8] = sv.tpage;
        vertData[i * 10 + 9] = sv.cba;
    }

    buffer->SetVertexData(vertData.data(), sd->numVerts);
    buffer->SetIndices(sd->indices, sd->numIndices);
    skeleton->joints[0].meshBuffer = buffer;

    delete[] jointMatrices;

    LOG("[Skeleton] Built skinned mesh: %u verts, %u indices across %u joints",
        sd->numVerts, sd->numIndices, skeleton->numJoints);
}


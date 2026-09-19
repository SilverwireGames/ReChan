#include "common.h"
#include "ai/activezn.h"
#include "ai/generator.h"
#include "ai/obstacle.h"
#include "ai/player.h"
#include "gen/world.h"
#include "gen/ai.h"
#include "gen/camera.h"
#include "gen/backg.h"
#include "gen/charmgr.h"
#include "gen/database.h"
#include "gen/display.h"
#include "gen/director.h"
#include "gen/envmgr.h"
#include "gen/game.h"
#include "gen/cammgr.h"
#include "gen/blockmgr.h"
#include "gen/deadpool.h"
#include "gen/geometry.h"
#include "gen/effects.h"
#include "gen/geffect.h"
#include "gen/levelmgr.h"
#include "gen/model.h"
#include "gen/mplayer.h"
#include "gen/paldata.h"
#include "gen/scaledata.h"
#include "gen/switch.h"
#include "gen/skeleton.h"
#include "gen/time.h"
#include "gen/animmgr.h"
#include "gen/particle.h"
#include "gen/psxmath_helpers.h"
#include "gen/pweffect.h"
#include "gen/scoremgr.h"
#include "gen/weffect.h"
#include "snd/rsevent.h"
#include "snd/snddrct.h"
#include "fe/hdmenu.h"
#include "fe/hud.h"
#include "fe/loadanim.h"
#include "p3d/hash.h"
#include "p3d/byteread.h"
#include "p3d/p3dmath.h"
#include "p3d/context.h"
#include "p3d/stream.h"
#include "p3d/texture.h"
#include "p3d/fileio.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"

#include "extra/shadowcsm.h"
#if defined(RC_PLATFORM_PSP)
#include "psp/psp_perf.h"
#endif

static void UploadRawTextureToWorldVRAM(s16 x, s16 y, s16 w, s16 h, const u8* raw) {
    if (!g_game || !g_game->GetWorld()) {
        return;
    }

    g_game->GetWorld()->UploadToVRAM(x, y, w, h, raw);
}
#include "ai/colfight.h"
#include "pc/log.h"
#include "gen/config.h"
#if MOD_LOADER
#include "extra/modloader.h"
#include "extra/gltfloader.h"
#include "extra/parameterdata.h"
#include "vendor/stb/stb_image.h"
#endif
#if REAL_TEXTURE_RENDERING
#include "extra/realtexture.h"
#endif

#include "gen/uvdata.h"
#include "ai/obstacle.h"

#if MOD_LOADER
#include <filesystem>
#endif
#include <algorithm>
#include <cstring>
#if defined(RC_PLATFORM_PSP)
#include <pspsysmem.h>
#endif
#include <cstdlib>
#include <unordered_map>
#include <vector>

// Global block manager pointer (PSX: gp scope, set by World)
BlockManager* g_blockManager = nullptr;

// PSX globals used by destination select return positioning.
LVector g_destSelectReturnPos = { 0, 0, 0 };
bool g_destSelectReturnPosValid = false;

// PSX: _5Arrow_gInside (0x800DD558) - set by Construct when returning to hub
u8 g_arrowInside = 0;

// DynamicThing physics globals (PSX: gp+1740, gp+1744)
// PSX: MAX_FALL_SPEED = 0x23 (0x800DD018), DYNAMIC_THING_OBSTACLE_VELOCITY_DECAY[0] = 0xFD70 (0x800DD01C)
s32 g_maxFallSpeed = 0x23;
s32 g_dampingFactor = 0xFD70;

// PSX: BLOCK_DRAW_SEAM_OFFSET_CODE (0x800DC9A8)
// Layout in PSX data: { code, differenceFactor, max, reserved }
// Defaults: { 1, 0x400, 0x10, 0 }
static s32 BLOCK_DRAW_SEAM_OFFSET_CODE[4] = { 1, 0x400, 0x10, 0 };

static u32 g_wEffectChunkCount = 0;
static u32 g_particleSystemChunkCount = 0;

// PSX BGR555 to RGBA8
static void PsxToRGBA(u16 c, u8& r, u8& g, u8& b, u8& a) {
    const u8 r5 = static_cast<u8>(c & 0x1F);
    const u8 g5 = static_cast<u8>((c >> 5) & 0x1F);
    const u8 b5 = static_cast<u8>((c >> 10) & 0x1F);
    r = static_cast<u8>((r5 << 3) | (r5 >> 2));
    g = static_cast<u8>((g5 << 3) | (g5 >> 2));
    b = static_cast<u8>((b5 << 3) | (b5 >> 2));
    a = (c == 0) ? 0 : 255;
}

void PsxVRAM::DecodePage(u16 tpage, u16 cba, u8* out) const {
    int tx = tpage & 0xF;
    int ty = (tpage >> 4) & 1;
    int depth = (tpage >> 7) & 3;

    int pageX = tx * 64;  // VRAM word column
    int pageY = ty * 256; // VRAM row

    int clutX = (cba & 0x3F) * 16;
    int clutY = (cba >> 6) & 0x1FF;

    if (depth == 0) {
        // 4-bit indexed: 16-color CLUT
        u16 clut[16];
        for (int i = 0; i < 16; i++)
            clut[i] = Get(clutX + i, clutY);

        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                int wordX = pageX + x / 4;
                int wordY = pageY + y;
                if (wordX >= 1024 || wordY >= 512) continue;
                u16 word = Get(wordX, wordY);
                int nibble = (word >> ((x % 4) * 4)) & 0xF;
                u16 color = clut[nibble];
                int idx = (y * 256 + x) * 4;
                PsxToRGBA(color, out[idx], out[idx + 1], out[idx + 2], out[idx + 3]);
            }
        }
    }
    else if (depth == 1) {
        // 8-bit indexed: 256-color CLUT
        u16 clut[256];
        for (int i = 0; i < 256; i++)
            clut[i] = Get(clutX + i, clutY);

        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                int wordX = pageX + x / 2;
                int wordY = pageY + y;
                if (wordX >= 1024 || wordY >= 512) continue;
                u16 word = Get(wordX, wordY);
                int byteIdx = (x & 1) ? ((word >> 8) & 0xFF) : (word & 0xFF);
                u16 color = clut[byteIdx];
                int idx = (y * 256 + x) * 4;
                PsxToRGBA(color, out[idx], out[idx + 1], out[idx + 2], out[idx + 3]);
            }
        }
    }
    else {
        // 15-bit direct color
        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                int wordX = pageX + x;
                int wordY = pageY + y;
                if (wordX >= 1024 || wordY >= 512) continue;
                u16 color = Get(wordX, wordY);
                int idx = (y * 256 + x) * 4;
                PsxToRGBA(color, out[idx], out[idx + 1], out[idx + 2], out[idx + 3]);
            }
        }
    }
}

static void LoadWEffectChunk(const u8* body, u32 bodySize) {
    g_wEffectChunkCount++;

    WEffect_LoadChunk(body, bodySize);
}

static void LoadParticleSystemChunk(const u8* body, u32 bodySize) {
    g_particleSystemChunkCount++;

    if (!ParticleSystem_LoadChunk(body, bodySize)) {
        LOG("[World] Failed loading ParticleSystem chunk 0x8A30 (entry=%u, size=%u)",
            g_particleSystemChunkCount,
            bodySize);
    }
}

static bool ReadChunkPStringHash(const u8* body, u32 bodySize, u32& cursor, u32* outHash) {
    if (!body || cursor >= bodySize) {
        return false;
    }

    const u8 nameLen = body[cursor++];
    if (cursor + nameLen > bodySize) {
        return false;
    }

    if (outHash) {
        char nameBuf[256];
        const u32 copyLen = (nameLen < 255) ? nameLen : 255;
        std::memcpy(nameBuf, body + cursor, copyLen);
        nameBuf[copyLen] = '\0';
        *outHash = p3dHash(nameBuf);
    }

    cursor += nameLen;
    return true;
}

static FrameListAnim* ParseFrameListAnimRaw(const u8* rawData, u32 rawSize) {
    if (!rawData || rawSize < 24) {
        return nullptr;
    }

    FrameListAnim* frameList = new FrameListAnim();
    frameList->nameUID = p3dReadU32LE(rawData + 0);
    frameList->numFrames = static_cast<s32>(p3dReadU32LE(rawData + 12));
    frameList->targetUID = p3dReadU32LE(rawData + 20);
    frameList->rawSize = rawSize;

    if (frameList->numFrames <= 0 || frameList->numFrames > 4096) {
        delete frameList;
        return nullptr;
    }

    const u32 frameArrayOff = p3dReadU32LE(rawData + 16) * 4u;
    if (frameArrayOff + static_cast<u32>(frameList->numFrames) * 4u > rawSize) {
        delete frameList;
        return nullptr;
    }

    frameList->rawData = static_cast<u8*>(std::malloc(rawSize));
    if (!frameList->rawData) {
        delete frameList;
        return nullptr;
    }
    std::memcpy(frameList->rawData, rawData, rawSize);

    frameList->frameOffsets = new u32[frameList->numFrames];
    for (s32 i = 0; i < frameList->numFrames; i++) {
        const u32 frameOff = p3dReadU32LE(frameList->rawData + frameArrayOff + i * 4u) * 4u;
        if (frameOff >= rawSize) {
            delete frameList;
            return nullptr;
        }
        frameList->frameOffsets[i] = frameOff;
    }

    return frameList;
}

static SequenceAnim* ParseSequenceAnimRaw(const u8* rawData, u32 rawSize) {
    if (!rawData || rawSize < 20) {
        return nullptr;
    }

    SequenceAnim* sequence = new SequenceAnim();
    sequence->nameUID = p3dReadU32LE(rawData + 0);
    const u32 packedFrames = p3dReadU32LE(rawData + 12);
    sequence->numFrames = static_cast<s32>(packedFrames);
    // PSX tSequenceFlip uses frame >> 8 for the part and frame & 0xFF
    // for the local frame.
    sequence->frameBits = 8;

    const u32 numParts = (packedFrames >> sequence->frameBits) + 1u;

    if (16u + numParts * 4u > rawSize) {
        delete sequence;
        return nullptr;
    }

    sequence->numParts = numParts;
    sequence->parts = new SequenceAnimPart[numParts]();
    for (u32 i = 0; i < numParts; i++) {
        sequence->parts[i].animHash = p3dReadU32LE(rawData + 16u + i * 4u);
    }

    return sequence;
}

static CompositeAnimData* ParseCompositeAnimChunkBody(const u8* body, u32 bodySize) {
    if (!body || bodySize < 5) {
        return nullptr;
    }

    u32 cursor = 0;
    u32 nameHash = 0;
    if (!ReadChunkPStringHash(body, bodySize, cursor, &nameHash) || cursor + 4 > bodySize) {
        return nullptr;
    }

    CompositeAnimData* composite = new CompositeAnimData();
    composite->nameUID = nameHash;
    composite->field12 = p3dReadU16LE(body + cursor);
    cursor += 2;
    composite->numParts = p3dReadU16LE(body + cursor);
    cursor += 2;

    if (composite->numParts > 0) {
        composite->parts = new CompositeAnimPartData[composite->numParts]();
    }

    for (u32 partIndex = 0; partIndex < composite->numParts; partIndex++) {
        CompositeAnimPartData& part = composite->parts[partIndex];
        if (!ReadChunkPStringHash(body, bodySize, cursor, &part.animNameUID) || cursor + 4 > bodySize) {
            delete composite;
            return nullptr;
        }

        part.field0 = p3dReadU16LE(body + cursor);
        cursor += 2;
        part.field1 = p3dReadU16LE(body + cursor);
        cursor += 2;
    }

    return composite;
}

static bool ShouldKeepParamChannel(s32 animType, s32 paramID) {
    if (animType == 257) {
        return paramID == 257 || (paramID >= 273 && paramID < 275);
    }

    if (animType >= 258) {
        return animType == 768 || animType < 0;
    }

    if (animType > 0 && animType < 3) {
        switch (paramID) {
            case 1:
            case 2:
            case 17:
            case 18:
            case 33:
            case 34:
                return true;
            default:
                return false;
        }
    }

    return animType < 0;
}

static ParamAnimData* ParseParamAnimChunkBody(const u8* body, u32 bodySize) {
    if (!body || bodySize < 18) {
        return nullptr;
    }

    u32 cursor = 0;
    u32 nameHash = 0;
    if (!ReadChunkPStringHash(body, bodySize, cursor, &nameHash) || cursor + 16 > bodySize) {
        return nullptr;
    }

    cursor += 4; // unknown/load flags field
    const s32 animType = p3dReadS32LE(body + cursor);
    cursor += 4;

    // target name string is stored but not required for CBV multiplier eval.
    if (!ReadChunkPStringHash(body, bodySize, cursor, nullptr) || cursor + 8 > bodySize) {
        return nullptr;
    }

    s32 numFrames = p3dReadS32LE(body + cursor);
    cursor += 4;
    cursor += 4; // expected param count

    std::vector<u16> keyTimes;
    std::vector<s32> keyValues;

    while (cursor + 6 <= bodySize) {
        const u16 subChunkId = p3dReadU16LE(body + cursor);
        const u32 subChunkSize = p3dReadU32LE(body + cursor + 2);
        if (subChunkSize < 6 || cursor + subChunkSize > bodySize) {
            break;
        }

        if (subChunkId == 0x4301) {
            const u8* paramBody = body + cursor + 6;
            const u32 paramBodySize = subChunkSize - 6;
            u32 paramCursor = 0;

            if (!ReadChunkPStringHash(paramBody, paramBodySize, paramCursor, nullptr) || paramCursor + 8 > paramBodySize) {
                cursor += subChunkSize;
                continue;
            }

            paramCursor += 4; // unknown
            const s32 paramID = p3dReadS32LE(paramBody + paramCursor);
            paramCursor += 4;

            if (!ShouldKeepParamChannel(animType, paramID)) {
                cursor += subChunkSize;
                continue;
            }

            std::vector<u16> candidateTimes;
            std::vector<s32> candidateValues;

            while (paramCursor + 6 <= paramBodySize) {
                const u16 valueChunkId = p3dReadU16LE(paramBody + paramCursor);
                const u32 valueChunkSize = p3dReadU32LE(paramBody + paramCursor + 2);
                if (valueChunkSize < 6 || paramCursor + valueChunkSize > paramBodySize) {
                    break;
                }

                const u8* valueBody = paramBody + paramCursor + 6;
                const u32 valueBodySize = valueChunkSize - 6;

                if (valueChunkId == 0x4203 && valueBodySize >= 4) {
                    const u32 count = p3dReadU32LE(valueBody + 0);
                    if (count > 0 && 4u + count * 2u <= valueBodySize) {
                        candidateTimes.resize(count);
                        for (u32 i = 0; i < count; i++) {
                            candidateTimes[i] = p3dReadU16LE(valueBody + 4 + i * 2u);
                        }
                    }
                }
                else if ((valueChunkId == 0x4210 || valueChunkId == 0x4283) && valueBodySize >= 8) {
                    const u32 count = p3dReadU32LE(valueBody + 0);
                    if (count > 0 && 8u + count * 4u <= valueBodySize) {
                        candidateValues.resize(count);
                        for (u32 i = 0; i < count; i++) {
                            candidateValues[i] = p3dReadS32LE(valueBody + 8 + i * 4u);
                        }
                    }
                }
                else if (valueChunkId == 0x4213 && valueBodySize >= 8) {
                    const u32 count = p3dReadU32LE(valueBody + 0);
                    if (count > 0 && 8u + count * 2u <= valueBodySize) {
                        candidateValues.resize(count);
                        for (u32 i = 0; i < count; i++) {
                            candidateValues[i] = static_cast<s32>(p3dReadU16LE(valueBody + 8 + i * 2u));
                        }
                    }
                }

                paramCursor += valueChunkSize;
            }

            const u32 keyCount = static_cast<u32>(std::min(candidateTimes.size(), candidateValues.size()));
            if (keyCount > 0) {
                keyTimes = candidateTimes;
                keyValues = candidateValues;
                keyTimes.resize(keyCount);
                keyValues.resize(keyCount);
                break;
            }
        }

        cursor += subChunkSize;
    }

    if (keyTimes.empty() || keyValues.empty()) {
        return nullptr;
    }

    ParamAnimData* paramAnim = new ParamAnimData();
    paramAnim->nameUID = nameHash;
    paramAnim->animType = animType;
    paramAnim->numFrames = numFrames;
    paramAnim->keyCount = static_cast<u32>(keyTimes.size());
    paramAnim->keyTimes = new u16[paramAnim->keyCount];
    paramAnim->keyValues = new s32[paramAnim->keyCount];
    for (u32 i = 0; i < paramAnim->keyCount; i++) {
        paramAnim->keyTimes[i] = keyTimes[i];
        paramAnim->keyValues[i] = keyValues[i];
    }

    if (paramAnim->numFrames <= 0 && paramAnim->keyCount > 0) {
        paramAnim->numFrames = static_cast<s32>(paramAnim->keyTimes[paramAnim->keyCount - 1]) + 1;
    }

    return paramAnim;
}

static bool RegisterParamAnimChunk(const u8* chunkBody, u32 chunkBodySize, u8 animType) {
    if (!chunkBody || chunkBodySize < 18) {
        return true;
    }

    ParamAnimData* paramAnim = ParseParamAnimChunkBody(chunkBody, chunkBodySize);
    if (!paramAnim) {
        return true;
    }

    if (!g_animMgr || paramAnim->nameUID == 0) {
        delete paramAnim;
        return true;
    }

    if (g_animMgr->GetMiscAnim(paramAnim->nameUID)) {
        delete paramAnim;
        return true;
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = paramAnim->nameUID;
    node->type = animType;
    node->paramAnim = paramAnim;
    g_animMgr->AddAnim(node);
    return true;
}

static CBVParamAnimData* ParseCBVParamAnimChunkBody(const u8* body, u32 bodySize) {
    if (!body || bodySize < 20) {
        return nullptr;
    }

    u32 cursor = 0;
    u32 nameHash = 0;
    u32 targetHash = 0;
    u32 blendHash = 0;
    if (!ReadChunkPStringHash(body, bodySize, cursor, &nameHash)
        || !ReadChunkPStringHash(body, bodySize, cursor, &targetHash)
        || !ReadChunkPStringHash(body, bodySize, cursor, &blendHash)
        || cursor + 8 > bodySize) {
        return nullptr;
    }

    // PSX tCBVParamAnimLoader reads two longs after the three pstrings and
    // uses the second one as mode (first is an unused/reserved field).
    cursor += 4; // reserved/unused
    const s32 mode = p3dReadS32LE(body + cursor);
    cursor += 4;

    u32 valueRows = 0;
    u32 valueCols = 0;
    std::vector<u32> values;
    std::vector<u32> offsets;

    while (cursor + 6 <= bodySize) {
        const u16 subChunkId = p3dReadU16LE(body + cursor);
        const u32 subChunkSize = p3dReadU32LE(body + cursor + 2);
        if (subChunkSize < 6 || cursor + subChunkSize > bodySize) {
            break;
        }

        const u8* subBody = body + cursor + 6;
        const u32 subBodySize = subChunkSize - 6;

        if (subChunkId == 0x6022 && subBodySize >= 8) {
            const u32 rows = p3dReadU32LE(subBody + 0);
            const u32 cols = p3dReadU32LE(subBody + 4);
            if (rows > 0 && cols > 0 && rows <= (0xFFFFFFFFu / cols)) {
                const u32 valueCount = rows * cols;
                if (8u + valueCount * 4u > subBodySize) {
                    cursor += subChunkSize;
                    continue;
                }

                valueRows = rows;
                valueCols = cols;
                values.resize(valueCount);
                for (u32 i = 0; i < valueCount; i++) {
                    values[i] = p3dReadU32LE(subBody + 8 + i * 4u);
                }
            }
        }
        else if (subChunkId == 0x6023 && subBodySize >= 4) {
            const u32 count = p3dReadU32LE(subBody + 0);
            if (count > 0 && 4u + count * 4u <= subBodySize) {
                offsets.resize(count);
                for (u32 i = 0; i < count; i++) {
                    offsets[i] = p3dReadU32LE(subBody + 4 + i * 4u);
                }
            }
        }

        cursor += subChunkSize;
    }

    if (nameHash == 0 || values.empty() || offsets.empty() || valueCols == 0) {
        return nullptr;
    }

    CBVParamAnimData* cbvParamAnim = new CBVParamAnimData();
    cbvParamAnim->nameUID = nameHash;
    cbvParamAnim->targetUID = targetHash;
    cbvParamAnim->blendAnimUID = blendHash;
    cbvParamAnim->mode = mode;
    cbvParamAnim->valueRows = valueRows;
    cbvParamAnim->valueCols = valueCols;
    cbvParamAnim->numEntries = static_cast<u32>(offsets.size());

    cbvParamAnim->values = new u32[values.size()];
    for (u32 i = 0; i < values.size(); i++) {
        cbvParamAnim->values[i] = values[i];
    }

    cbvParamAnim->primOffsets = new u32[offsets.size()];
    for (u32 i = 0; i < offsets.size(); i++) {
        cbvParamAnim->primOffsets[i] = offsets[i];
    }

    return cbvParamAnim;
}

static bool RegisterCBVParamAnimChunk(const u8* chunkBody, u32 chunkBodySize, u8 animType) {
    if (!chunkBody || chunkBodySize < 20) {
        return true;
    }

    CBVParamAnimData* cbvParamAnim = ParseCBVParamAnimChunkBody(chunkBody, chunkBodySize);
    if (!cbvParamAnim) {
        return true;
    }

    if (!g_animMgr || cbvParamAnim->nameUID == 0 || cbvParamAnim->numEntries == 0) {
        delete cbvParamAnim;
        return true;
    }

    if (g_animMgr->GetMiscAnim(cbvParamAnim->nameUID)) {
        delete cbvParamAnim;
        return true;
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = cbvParamAnim->nameUID;
    node->type = animType;
    node->cbvParamAnim = cbvParamAnim;
    g_animMgr->AddAnim(node);
    return true;
}

static ClutAnimData* ParseClutAnimChunkBody(const u8* body, u32 bodySize) {
    if (!body || bodySize < 5) {
        return nullptr;
    }

    u32 cursor = 0;
    u32 nameHash = 0;
    u32 materialHash = 0;
    u32 primHash = 0;
    if (!ReadChunkPStringHash(body, bodySize, cursor, &nameHash)
        || !ReadChunkPStringHash(body, bodySize, cursor, &materialHash)
        || !ReadChunkPStringHash(body, bodySize, cursor, &primHash)
        || cursor + 2 > bodySize) {
        return nullptr;
    }

    const u16 mode = p3dReadU16LE(body + cursor);
    cursor += 2;

    std::vector<u16> frames;
    std::vector<u16> offsets;

    while (cursor + 6 <= bodySize) {
        const u16 subChunkId = p3dReadU16LE(body + cursor);
        const u32 subChunkSize = p3dReadU32LE(body + cursor + 2);
        if (subChunkSize < 6 || cursor + subChunkSize > bodySize) {
            break;
        }

        const u8* subBody = body + cursor + 6;
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

        cursor += subChunkSize;
    }

    if (nameHash == 0 || frames.empty()) {
        return nullptr;
    }

    ClutAnimData* clutAnim = new ClutAnimData();
    clutAnim->nameUID = nameHash;
    clutAnim->materialUID = materialHash;
    clutAnim->primUID = primHash;
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

static bool RegisterClutAnimChunk(const u8* chunkBody, u32 chunkBodySize, u8 animType) {
    if (!chunkBody || chunkBodySize < 5) {
        return true;
    }

    ClutAnimData* clutAnim = ParseClutAnimChunkBody(chunkBody, chunkBodySize);
    if (!clutAnim) {
        return true;
    }

    if (!g_animMgr || clutAnim->nameUID == 0) {
        delete clutAnim;
        return true;
    }

    if (g_animMgr->GetMiscAnim(clutAnim->nameUID)) {
        delete clutAnim;
        return true;
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = clutAnim->nameUID;
    node->type = animType;
    node->clutAnim = clutAnim;
    g_animMgr->AddAnim(node);
    return true;
}

static bool RegisterTransformAnimChunk(const u8* chunkBody,
                                       u32 chunkBodySize,
                                       const u8* permData,
                                       u32& permCursor,
                                       u32 permSize,
                                       u8 animType) {
    if (!chunkBody || chunkBodySize < 5 || !permData) {
        return true;
    }

    u32 cursor = 0;
    u32 chunkNameHash = 0;
    if (!ReadChunkPStringHash(chunkBody, chunkBodySize, cursor, &chunkNameHash) || cursor + 4 > chunkBodySize) {
        LOG("[World] Malformed transform anim chunk (0x6400)");
        return false;
    }

    const u32 rawSize = p3dReadU32LE(chunkBody + cursor);
    if (permCursor + rawSize > permSize) {
        LOG("[World] Transform anim perm overflow at %u (size=%u total=%u)", permCursor, rawSize, permSize);
        return false;
    }

    TransformAnim* anim = nullptr;
    CharSequenceAnim* animSeq = nullptr;
    if (rawSize >= 40) {
        u8* rawCopy = static_cast<u8*>(std::malloc(rawSize));
        if (rawCopy) {
            std::memcpy(rawCopy, permData + permCursor, rawSize);
            // ParseMulti handles both single-block and multi-block (concatenated
            // tTransformAnim, i.e. a frame-by-frame pose flip-book) payloads;
            // Parse() alone would silently read only the first block.
            void* parsed = TransformAnim::ParseMulti(rawCopy, rawSize);
            if (parsed) {
                if (IsCharSequenceAnim(parsed)) {
                    animSeq = static_cast<CharSequenceAnim*>(parsed);
                    anim = animSeq->parts ? animSeq->parts[0] : nullptr;
                }
                else {
                    anim = static_cast<TransformAnim*>(parsed);
                }
                if (anim && chunkNameHash != 0) {
                    anim->nameUID = chunkNameHash;
                }
            }
            else {
                std::free(rawCopy);
            }
        }
    }

    permCursor += rawSize;

    if (!anim) {
        return true;
    }

    if (!g_animMgr) {
        if (animSeq) delete animSeq; else delete anim;
        return true;
    }

    if (g_animMgr->GetMiscAnim(anim->nameUID)) {
        if (animSeq) delete animSeq; else delete anim;
        return true;
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = anim->nameUID;
    node->type = animType;
    node->anim = anim;
    node->animSequence = animSeq;
    g_animMgr->AddAnim(node);
    return true;
}

// PSX tUVAnimLoader::Load (UVANIM.CPP:197)
// Chunk 0x600E: per-primitive UV frame animation targeting a named child geo.
// Sub-chunk 0x600F carries the frame data; 0x6010 (PSX OT byte offsets) is skipped.
static bool RegisterUVListAnimChunk(const u8* body, u32 bodySize, u8 animType) {
    if (!body || bodySize < 2 || !g_animMgr) {
        return true;
    }

    u32 cursor = 0;
    u32 nameHash = 0;
    u32 targetHash = 0;
    if (!ReadChunkPStringHash(body, bodySize, cursor, &nameHash)) {
        return true;
    }
    if (!ReadChunkPStringHash(body, bodySize, cursor, &targetHash)) {
        return true;
    }

    // Skip two u32 fields (PSX: first discarded, second = mode 0=INVGEO/1=INVPRM).
    if (cursor + 8 > bodySize) {
        return true;
    }
    cursor += 8;

    s32 numFrames = 0;
    s32 numEntries = 0;
    u16* uvData = nullptr;
    u32* packetOffsets = nullptr;
    u32 numPacketOffsets = 0;

    while (cursor + 6 <= bodySize) {
        const u16 subId = static_cast<u16>(body[cursor] | (body[cursor + 1] << 8));
        const u32 subTotal = static_cast<u32>(body[cursor + 2])
            | (static_cast<u32>(body[cursor + 3]) << 8)
            | (static_cast<u32>(body[cursor + 4]) << 16)
            | (static_cast<u32>(body[cursor + 5]) << 24);
        if (subTotal < 6 || cursor + subTotal > bodySize) {
            break;
        }

        const u8* subBody = body + cursor + 6;
        const u32 subBodySize = subTotal - 6;

        if (subId == 0x600F && subBodySize >= 8) {
            // numFrames (u32), numEntries (u32), then numFrames*numEntries u16 UV words.
            numFrames = static_cast<s32>(p3dReadU32LE(subBody + 0));
            numEntries = static_cast<s32>(p3dReadU32LE(subBody + 4));
            const u32 dataBytes = static_cast<u32>(numFrames) * static_cast<u32>(numEntries) * 2u;
            if (numFrames > 0 && numEntries > 0 && subBodySize >= 8u + dataBytes) {
                uvData = new u16[static_cast<u32>(numFrames) * static_cast<u32>(numEntries)];
                if (uvData) {
                    std::memcpy(uvData, subBody + 8, dataBytes);
                }
            }
        }
        else if (subId == 0x6010 && subBodySize >= 4) {
            // PSX OT byte offsets: numOffsets (u32) followed by numOffsets u32 values.
            // Each value is a byte offset into the geo's OT packet buffer for one UV word slot.
            const u32 n = p3dReadU32LE(subBody);
            const u32 dataBytes = n * 4u;
            if (n > 0 && subBodySize >= 4u + dataBytes) {
                delete[] packetOffsets;
                packetOffsets = new u32[n];
                if (packetOffsets) {
                    std::memcpy(packetOffsets, subBody + 4, dataBytes);
                    numPacketOffsets = n;
                }
            }
        }

        cursor += subTotal;
    }

    if (!uvData || numFrames <= 0 || numEntries <= 0) {
        delete[] uvData;
        delete[] packetOffsets;
        return true;
    }

    if (g_animMgr->GetMiscAnim(nameHash)) {
        delete[] uvData;
        delete[] packetOffsets;
        return true;
    }

    UVListAnim* uvAnim = new UVListAnim();
    uvAnim->nameUID = nameHash;
    uvAnim->targetUID = targetHash;
    uvAnim->numFrames = numFrames;
    uvAnim->numEntries = numEntries;
    uvAnim->uvData = uvData;
    uvAnim->packetOffsets = packetOffsets;
    uvAnim->numPacketOffsets = numPacketOffsets;

    // Pre-sort UV data indices by ascending OT byte offset so ApplyUVListAnimFrame
    // can walk prim/corners in OT order regardless of how 0x6010 lists entries.
    if (numPacketOffsets > 0 && packetOffsets) {
        u32* perm = new u32[numPacketOffsets];
        for (u32 i = 0; i < numPacketOffsets; i++) perm[i] = i;
        std::sort(perm, perm + numPacketOffsets, [&](u32 a, u32 b) {
            return packetOffsets[a] < packetOffsets[b];
        });
        uvAnim->sortedPerm = perm;
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = nameHash;
    node->type = animType;
    node->uvListAnim = uvAnim;
    g_animMgr->AddAnim(node);
    LOG("[World] UVListAnim 0x%08X -> target 0x%08X (%d frames x %d entries)",
        nameHash, targetHash, numFrames, numEntries);
    return true;
}

static bool RegisterFrameListAnimChunk(const u8* chunkBody,
                                       u32 chunkBodySize,
                                       const u8* permData,
                                       u32& permCursor,
                                       u32 permSize,
                                       u8 animType) {
    if (!chunkBody || chunkBodySize < 8 || !permData) {
        return true;
    }

    u32 cursor = 0;
    (void)p3dReadU32LE(chunkBody + cursor);
    cursor += 4;
    const u32 rawSize = p3dReadU32LE(chunkBody + cursor);
    cursor += 4;

    u32 chunkNameHash = 0;
    u32 targetHash = 0;
    ReadChunkPStringHash(chunkBody, chunkBodySize, cursor, &chunkNameHash);
    ReadChunkPStringHash(chunkBody, chunkBodySize, cursor, &targetHash);

    if (permCursor + rawSize > permSize) {
        LOG("[World] FrameList perm overflow at %u (size=%u total=%u)", permCursor, rawSize, permSize);
        return false;
    }

    FrameListAnim* frameList = ParseFrameListAnimRaw(permData + permCursor, rawSize);
    permCursor += rawSize;

    if (!frameList) {
        return true;
    }

    if (!g_animMgr) {
        delete frameList;
        return true;
    }

    if (chunkNameHash != 0) {
        frameList->nameUID = chunkNameHash;
    }
    if (targetHash != 0) {
        frameList->targetUID = targetHash;
    }

    if (g_animMgr->GetMiscAnim(frameList->nameUID)) {
        delete frameList;
        return true;
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = frameList->nameUID;
    node->type = animType;
    node->frameList = frameList;
    g_animMgr->AddAnim(node);
    return true;
}

static bool RegisterSequenceAnimChunk(const u8* chunkBody,
                                      u32 chunkBodySize,
                                      const u8* permData,
                                      u32& permCursor,
                                      u32 permSize,
                                      u8 animType) {
    if (!chunkBody || chunkBodySize < 4 || !permData) {
        return true;
    }

    const u32 rawSize = p3dReadU32LE(chunkBody + 0);
    if (permCursor + rawSize > permSize) {
        LOG("[World] SequenceAnim perm overflow at %u (size=%u total=%u)", permCursor, rawSize, permSize);
        return false;
    }

    SequenceAnim* sequence = ParseSequenceAnimRaw(permData + permCursor, rawSize);
    permCursor += rawSize;

    if (!sequence) {
        return true;
    }

    if (!g_animMgr) {
        delete sequence;
        return true;
    }

    for (u32 i = 0; i < sequence->numParts; i++) {
        SequenceAnimPart& part = sequence->parts[i];
        if (part.animHash != 0) {
            part.node = g_animMgr->GetMiscAnim(part.animHash);
            if (part.node && part.node->anim) {
                part.anim = part.node->anim;
            }
        }
    }

    if (g_animMgr->GetMiscAnim(sequence->nameUID)) {
        delete sequence;
        return true;
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = sequence->nameUID;
    node->type = animType;
    node->sequenceAnim = sequence;
    g_animMgr->AddAnim(node);
    return true;
}

static void RegisterCompositeAnimChunk(const u8* chunkBody, u32 chunkBodySize, u8 animType) {
    if (!chunkBody || chunkBodySize < 5 || !g_animMgr) {
        return;
    }

    CompositeAnimData* composite = ParseCompositeAnimChunkBody(chunkBody, chunkBodySize);
    if (!composite) {
        return;
    }

    if (g_animMgr->GetMiscAnim(composite->nameUID)) {
        delete composite;
        return;
    }

    for (u32 i = 0; i < composite->numParts; i++) {
        CompositeAnimPartData& part = composite->parts[i];
        if (part.animNameUID != 0) {
            part.animNode = g_animMgr->GetMiscAnim(part.animNameUID);
        }
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = composite->nameUID;
    node->type = animType;
    node->compositeAnim = composite;
    g_animMgr->AddAnim(node);
}

static VizAnim* ParseVizAnimChunkBody(const u8* body, u32 bodySize) {
    if (!body || bodySize < 10) {
        return nullptr;
    }

    u32 cursor = 0;
    u32 nameHash = 0;
    u32 targetHash = 0;
    if (!ReadChunkPStringHash(body, bodySize, cursor, &nameHash)
        || !ReadChunkPStringHash(body, bodySize, cursor, &targetHash)
        || cursor + 4 > bodySize) {
        return nullptr;
    }

    const s16 numFrames = p3dReadS16LE(body + cursor);
    cursor += 2;
    const u16 numNodes = p3dReadU16LE(body + cursor);
    cursor += 2;

    if (cursor + 6 > bodySize) {
        return nullptr;
    }

    const u16 subChunkId = p3dReadU16LE(body + cursor);
    cursor += 2;
    const u32 subChunkSize = p3dReadU32LE(body + cursor);
    cursor += 4;

    if (subChunkId != 0x4021 || subChunkSize < 6) {
        return nullptr;
    }

    const u32 subBodySize = subChunkSize - 6;
    if (cursor + subBodySize > bodySize || numNodes == 0 || subBodySize < static_cast<u32>(numNodes) * 8u) {
        return nullptr;
    }

    VizAnim* vizAnim = new VizAnim();
    vizAnim->nameUID = nameHash;
    vizAnim->targetUID = targetHash;
    vizAnim->numFrames = (numFrames > 0) ? numFrames : 0;
    vizAnim->numNodes = numNodes;

    vizAnim->rawData = static_cast<u8*>(std::malloc(subBodySize));
    if (!vizAnim->rawData) {
        delete vizAnim;
        return nullptr;
    }

    vizAnim->rawSize = subBodySize;
    std::memcpy(vizAnim->rawData, body + cursor, subBodySize);

    vizAnim->nodes = new VizAnimNodeEntry[numNodes]();
    for (u16 nodeIndex = 0; nodeIndex < numNodes; nodeIndex++) {
        const u32 entryOffset = static_cast<u32>(nodeIndex) * 8u;
        const u32 geoHash = p3dReadU32LE(vizAnim->rawData + entryOffset + 0);
        const u32 bitsOffsetWords = p3dReadU32LE(vizAnim->rawData + entryOffset + 4);

        VizAnimNodeEntry& outNode = vizAnim->nodes[nodeIndex];
        outNode.targetHash = geoHash;
        // tVizNode stores bit offsets in 32-bit words in packed data streams.
        const u32 bitsOffsetBytes = bitsOffsetWords * 4u;
        if (bitsOffsetBytes < vizAnim->rawSize) {
            outNode.bitWords = reinterpret_cast<const u32*>(vizAnim->rawData + bitsOffsetBytes);
        }
    }

    return vizAnim;
}

static void RegisterVizAnimChunk(const u8* chunkBody, u32 chunkBodySize, u8 animType) {
    if (!chunkBody || chunkBodySize < 10 || !g_animMgr) {
        return;
    }

    VizAnim* vizAnim = ParseVizAnimChunkBody(chunkBody, chunkBodySize);
    if (!vizAnim) {
        return;
    }

    if (g_animMgr->GetMiscAnim(vizAnim->nameUID)) {
        delete vizAnim;
        return;
    }

    MiscAnimNode* node = new MiscAnimNode();
    node->hash = vizAnim->nameUID;
    node->type = animType;
    node->vizAnim = vizAnim;
    g_animMgr->AddAnim(node);
}

static bool SwitchStringEqualsNoCase(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) {
        return false;
    }

    while (*lhs && *rhs) {
        unsigned char left = (unsigned char)*lhs++;
        unsigned char right = (unsigned char)*rhs++;

        if (left >= 'A' && left <= 'Z') {
            left = (unsigned char)(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = (unsigned char)(right + ('a' - 'A'));
        }

        if (left != right) {
            return false;
        }
    }

    return *lhs == '\0' && *rhs == '\0';
}

static s32 SwitchBehaviorTrigger(Thing* /*thing*/, u32 /*argc*/, const char** /*argv*/) {
    return 1;
}

static s32 SwitchSoundAmbiantSpace(Thing* thing, u32 argc, const char** argv) {
    MARKFUNCTION(0x80093E68);

    if (!thing || thing->thingType != AITypes::TT_PLAYER) {
        return 1;
    }

    if (argc == 0 || !argv || !argv[0]) {
        return 1;
    }

    const s32 ambienceSpace = atol(argv[0]);
    const s32 crossFade = (argc == 2 && argv[1]) ? atol(argv[1]) : 0;
    rsEvent(20, ambienceSpace, crossFade, 0);
    return 1;
}

static s32 SwitchEnemyObstacleDeathVol(Thing* thing, u32 /*argc*/, const char** /*argv*/) {
    if (!thing) {
        return 0;
    }

    if (g_scoreManager && thing->thingType >= AITypes::TT_HUMANOID_FIRST
        && thing->thingType <= AITypes::TT_HUMANOID_LAST) {
        g_scoreManager->AddStylePoints(100);
    }

    thing->Kill();
    return 0;
}

static Humanoid* ResolveSwitchDialogTarget(Thing* thing, s32 targetPlayer) {
    if (targetPlayer != 0) {
        return Player::s_player;
    }

    return thing ? static_cast<Humanoid*>(thing) : nullptr;
}

static s32 SwitchLoadDialog(Thing* thing, u32 argc, const char** argv) {
    if (argc != 3 || !argv) {
        return 0;
    }

    s32 targetPlayer = argv[0] ? atol(argv[0]) : 0;
    u32 dialogID = argv[1] ? (u32)atol(argv[1]) : 0;
    s32 priority = argv[2] ? atol(argv[2]) : 0;

    Humanoid* target = ResolveSwitchDialogTarget(thing, targetPlayer);
    if (!target) {
        return 0;
    }

    target->LoadDialog(dialogID, priority);
    return 1;
}

static s32 SwitchPlayDialog(Thing* thing, u32 argc, const char** argv) {
    if (argc != 3 || !argv) {
        return 0;
    }

    s32 targetPlayer = argv[0] ? atol(argv[0]) : 0;
    u32 dialogID = argv[1] ? (u32)atol(argv[1]) : 0;
    s32 priority = argv[2] ? atol(argv[2]) : 0;

    Humanoid* target = ResolveSwitchDialogTarget(thing, targetPlayer);
    if (!target) {
        return 0;
    }

    target->PlayDialog(dialogID, priority);
    return 1;
}

static s32 SwitchCheckpoint(Thing* /*thing*/, u32 argc, const char** argv) {
    if (!Player::s_player) {
        return 0;
    }

    Player::s_player->OnCheckpoint();

    if (argc < 2 || !argv) {
        Player::s_player->checkpoint.field44 = 0;
        Player::s_player->checkpoint.field48 = 0;
    }
    else {
        Player::s_player->checkpoint.field44 = (s32)p3dHash(argv[1]);
        Player::s_player->checkpoint.field48 = (argc < 3) ? 0 : (s32)p3dHash(argv[2]);
    }

    return 1;
}

struct PendingCharModelLoad {
    s32 oldType;
    s32 newType;
};

static std::vector<PendingCharModelLoad> g_pendingCharModelLoads;

static bool IsSwitchCharModelType(s32 thingType) {
    return (u32)(thingType - 1) < 0x1C;
}

static void ClearPendingCharModelLoads() {
    g_pendingCharModelLoads.clear();
}

static void SwitchUnloadLoadCharModel(s32 oldType, s32 newType) {
    if (!g_characterManager) {
        return;
    }

    if (IsSwitchCharModelType(oldType)) {
        g_characterManager->UnloadAnimation((u32)oldType, 0, 0x188);
        g_characterManager->UnloadCharacter((u32)oldType);
    }

    if (IsSwitchCharModelType(newType)) {
        g_characterManager->LoadCharacter((u32)newType, nullptr);
        g_characterManager->LoadAnimation((u32)newType, 0, 0x7C, nullptr);
    }
}

static void QueuePendingCharModelLoad(s32 oldType, s32 newType) {
    g_pendingCharModelLoads.push_back({ oldType, newType });
}

static u32 g_switchLoadGroupHash[2] = {};
static s32 g_switchLoadGroupIndex = 0;
// PSX: directorGOTO (0x800DD6E8)
static s32 g_directorGOTO = 0;

void SeedPlayerLoadGroups(u32 primaryHash, u32 secondaryHash) {
    g_switchLoadGroupHash[0] = primaryHash;
    g_switchLoadGroupHash[1] = secondaryHash;
}

static void PurgeSwitchLoadGroups() {
    for (s32 index = 0; index < 2; index++) {
        if (g_switchLoadGroupHash[index] != 0) {
            if (g_characterManager) {
                g_characterManager->UnloadAnimation(AITypes::TT_PLAYER, g_switchLoadGroupHash[index]);
            }
            g_switchLoadGroupHash[index] = 0;
            g_switchLoadGroupIndex = index;
        }
    }
}

static void LoadSwitchLoadGroup(const char* name, u32 hash) {
    if (!g_characterManager || !name) {
        return;
    }

    const s32 slot = g_switchLoadGroupIndex;
    if (g_switchLoadGroupHash[slot] != 0) {
        g_characterManager->UnloadAnimation(AITypes::TT_PLAYER, g_switchLoadGroupHash[slot]);
    }

    g_switchLoadGroupHash[slot] = 0;

    if (hash != 0) {
        g_switchLoadGroupHash[slot] = hash;
        g_characterManager->LoadAnimation(AITypes::TT_PLAYER, hash, nullptr);
    }

    g_switchLoadGroupIndex = (slot == 0) ? 1 : 0;
}

// PSX: gSyncLoadGroup__Fi (SWITCH.CPP:788, 0x80094418)
// Re-loads the stored async load-group hash for a slot after world setup.
static void SyncSwitchLoadGroup(s32 slot) {
    if (!g_characterManager || slot < 0 || slot >= 2) {
        return;
    }

    const u32 hash = g_switchLoadGroupHash[slot];
    if (hash == 0) {
        return;
    }

    g_switchLoadGroupIndex = (slot == 0) ? 1 : 0;
    g_characterManager->EnableCache(AITypes::TT_PLAYER, 1);
    g_characterManager->LoadAnimation(AITypes::TT_PLAYER, hash, nullptr);
    g_characterManager->EnableCache(AITypes::TT_PLAYER, 0);
}

static PlayerModel* ResolveSwitchPlayerModel(Thing* thing) {
    if (!thing || !thing->model) {
        return nullptr;
    }

    Model* model = static_cast<Model*>(thing->model);
    return dynamic_cast<PlayerModel*>(model);
}

// PSX: gfAsyncLoadGroup__FP5ThingUlPPCc (SWITCH.CPP:819, 0x80094518)
static s32 SwitchAsyncLoadGroup(Thing* /*thing*/, u32 argc, const char** argv) {
    MARKFUNCTION(0x80094518);

    if (argc == 0 || !argv || !SwitchStringEqualsNoCase(argv[0], "keep")) {
        PurgeSwitchLoadGroups();
    }

    for (u32 argIndex = 0; argv && argIndex < argc; argIndex++) {
        const char* name = argv[argIndex];
        if (!name) {
            continue;
        }

        if (!SwitchStringEqualsNoCase(name, "keep")) {
            LoadSwitchLoadGroup(name, p3dHash(name));
            continue;
        }

        argIndex++;
        if (argIndex >= argc) {
            break;
        }

        const char* keepName = argv[argIndex];
        if (!keepName) {
            continue;
        }

        const u32 keepHash = p3dHash(keepName);
        for (s32 slot = 0; slot < 2; slot++) {
            if (g_switchLoadGroupHash[slot] == keepHash) {
                g_switchLoadGroupIndex = (slot == 0) ? 1 : 0;
                break;
            }
        }
    }

    return 1;
}

// PSX: gfAsyncLoadNIS__FP5ThingUlPPCc (SWITCH.CPP:884, 0x80094684)
static s32 SwitchAsyncLoadNIS(Thing* thing, u32 argc, const char** argv) {
    MARKFUNCTION(0x80094684);

    u32 loadArgc = argc;
    if (argc == 0 || !argv || !argv[argc - 1]
        || !SwitchStringEqualsNoCase(argv[argc - 1], "nopurge")) {
        PurgeSwitchLoadGroups();
    }
    else {
        loadArgc--;
    }

    PlayerModel* playerModel = ResolveSwitchPlayerModel(thing);
    if (playerModel && argv && loadArgc > 0) {
        playerModel->LoadNIS(loadArgc, argv, 1, 0);
    }

    g_directorGOTO = 0;
    return 1;
}

// PSX: gfAsyncLoadNISGOTO__FP5ThingUlPPCc (SWITCH.CPP:912, 0x80094714)
static s32 SwitchAsyncLoadNISGOTO(Thing* thing, u32 argc, const char** argv) {
    MARKFUNCTION(0x80094714);

    if (g_directorGOTO != 0) {
        g_directorGOTO = 0;

        u32 loadArgc = argc;
        if (argc == 0 || !argv || !argv[argc - 1]
            || !SwitchStringEqualsNoCase(argv[argc - 1], "nopurge")) {
            PurgeSwitchLoadGroups();
        }
        else {
            loadArgc--;
        }

        PlayerModel* playerModel = ResolveSwitchPlayerModel(thing);
        if (playerModel && argv && loadArgc > 0) {
            playerModel->LoadNIS(loadArgc, argv, 1, 0);
        }
    }

    return 1;
}

static s32 SwitchCharModelLoad(Thing* /*thing*/, u32 argc, const char** argv) {
    if (argc < 2 || !argv) {
        return 0;
    }

    const s32 oldType = argv[0] ? atol(argv[0]) : 0;
    const s32 newType = argv[1] ? atol(argv[1]) : 0;

    s32 killedAny = 0;
    if (IsSwitchCharModelType(oldType) && g_ai) {
        for (ccMinNode* node = g_ai->humanoidList.head; node; node = node->next) {
            Humanoid* humanoid = static_cast<Humanoid*>(node);
            if (humanoid->thingType == (u16)oldType) {
                killedAny = 1;
                humanoid->Kill();
            }
        }
    }

    if (killedAny != 0) {
        QueuePendingCharModelLoad(oldType, newType);
    }
    else {
        SwitchUnloadLoadCharModel(oldType, newType);
    }

    return 1;
}

static void PlayThingDeathVolSound(s32 deathVolType) {
    if (deathVolType == 0 && g_game && g_game->GetWorld()) {
        const s32 levelID = g_game->GetWorld()->GetCurLevelID();
        deathVolType = ((levelID >= 2) && (levelID < 4)) ? 2 : 1;
    }

    u16 soundID = 0;
    if (deathVolType == 1) {
        soundID = 8;
    }
    else if (deathVolType == 2) {
        soundID = 25;
    }
    else {
        return;
    }

    CSoundDirect::PlayTransient(soundID, nullptr, 0, 0);
}

static s32 SwitchPlayerDeathVol(Thing* thing, u32 argc, const char** argv) {
    if (!thing) {
        return 0;
    }

    thing->flags |= TF_NEEDS_ACTIVATION;

    s32 deathType = -1;
    if (argc > 0 && argv && argv[0]) {
        deathType = atol(argv[0]);
    }

    if (thing->thingType != AITypes::TT_PLAYER) {
        if (deathType >= 0) {
            PlayThingDeathVolSound(deathType);
        }
        return SwitchEnemyObstacleDeathVol(thing, argc, argv);
    }

    if (g_blockManager) {
        g_blockManager->SetDeathVolumeFlag(0);
    }

    if (g_director && g_director->TriggerDeathVolume(deathType)) {
        if (deathType == 4) {
            thing->health = 1;
        }
    }

    return 1;
}

// PSX: gfDirectorVol__FP5ThingUlPPCc (SWITCH.CPP:500, 0x800940D4)
static s32 SwitchDirectorVol(Thing* thing, u32 argc, const char** argv) {
    MARKFUNCTION(0x800940D4);

    if (!g_director || argc == 0 || !argv || !argv[0]) {
        return 0;
    }

    const s32 scriptIndex = atol(argv[0]);
    s32* script = Director::GetGlobalScriptByIndex(scriptIndex);
    if (!script) {
        return 0;
    }

    LOG("[DirectorVol] scriptIndex=%d scriptState=%d anim361_type0=%d anim361_type12=%d",
        scriptIndex,
        g_director->scriptState,
        (g_characterManager && g_characterManager->GetAnimation(0, 361)) ? 1 : 0,
        (g_characterManager && g_characterManager->GetAnimation(12, 361)) ? 1 : 0);

    g_director->SetCodeSnip(script, thing);
    return 1;
}

static s32 SwitchGoToVol(Thing* thing, u32 argc, const char** argv) {
    if (argc < 3 || !argv || !g_director) {
        return 0;
    }

    const s32 x = argv[0] ? atol(argv[0]) : 0;
    const s32 y = argv[1] ? atol(argv[1]) : 0;
    const s32 z = argv[2] ? atol(argv[2]) : 0;

    g_director->TriggerGotoPoint(x, y, z, thing);
    return 1;
}

static bool IsLevelCompleteHumanoidType(u16 thingType) {
    // PSX switch table at 0x800D382C accepts only these thing types.
    return thingType == 10 || thingType == 12 || thingType == 13 ||
        thingType == 15 || thingType == 17;
}

static bool IsLevelCompleteMoveNodeReady(Thing* moveThing) {
    if (!moveThing) {
        return false;
    }

    const u16 thingType = moveThing->thingType;
    if (thingType != AITypes::TT_GENERATOR
        && thingType != AITypes::TT_ENEMYGENERATOR
        && thingType != AITypes::TT_THROWGENERATOR) {
        return false;
    }

    Generator* generator = static_cast<Generator*>(moveThing);
    if (generator->generateCount < generator->field200) {
        return false;
    }

    if (thingType == AITypes::TT_ENEMYGENERATOR) {
        EnemyGenerator* enemyGenerator = static_cast<EnemyGenerator*>(moveThing);
        if (enemyGenerator->activeZone && enemyGenerator->activeZone->memberCount != 0) {
            return false;
        }
    }

    return true;
}

// PSX: gfLevelComplete__FP5ThingUlPPCc (SWITCH.CPP:1096, 0x80094964)
static s32 SwitchLevelComplete(Thing* thing, u32 argc, const char** argv) {
    MARKFUNCTION(0x80094964);

    if (Player::s_player) {
        Player::s_player->encounterState = 3;
    }

    if (!g_ai || !g_director) {
        return 0;
    }

    for (u32 index = 0; index < argc; index++) {
        if (!argv || !argv[index]) {
            return 0;
        }

        const u32 crc = p3dHash(argv[index]);
        if (index == 0) {
            g_director->victoryBossCRC = crc;
        }

        ccNode* humNode = g_ai->humanoidList.FindNodeCRC(crc);
        if (humNode) {
            Humanoid* hum = static_cast<Humanoid*>(static_cast<Thing*>(humNode));
            if (!IsLevelCompleteHumanoidType(hum->thingType)) {
                return 0;
            }
            if (hum->actionState != AS_DEAD) {
                return 0;
            }
            continue;
        }

        ccNode* moveNode = g_ai->moveList.FindNodeCRC(crc);
        if (moveNode) {
            Thing* moveThing = static_cast<Thing*>(moveNode);
            if (!IsLevelCompleteMoveNodeReady(moveThing)) {
                return 0;
            }
        }
    }

    s32* levelEnd = Director::GetLevelEndScript();
    if (g_director->codeSnipPtr != levelEnd) {
        g_director->SetCodeSnip(levelEnd, thing);
    }

    return 1;
}

static s32 SwitchResetPlayer(Thing* /*thing*/, u32 /*argc*/, const char** /*argv*/) {
    MARKFUNCTION(0x800941DC);

    Player::s_player->Reset();
    g_display->GetCamera()->Reset();
    return 1;
}

static s32 SwitchBossVol(Thing* /*thing*/, u32 /*argc*/, const char** /*argv*/) {
    MARKFUNCTION(0x80094BBC);
    return 1;
}

static s32 SwitchGateCleanupVol(Thing* /*thing*/, u32 /*argc*/, const char** /*argv*/) {
    MARKFUNCTION(0x800942B8);

    if (!g_ai || !g_blockManager || g_blockManager->GetNumBlocks() == 0) {
        return 1;
    }

    const Block* loadedBlocks = g_blockManager->GetBlocks();
    u16 blockThreshold = BLOCK_UNASSIGNED;

    for (u32 index = 0; index < g_blockManager->GetNumBlocks(); index++) {
        const u16 blockNum = loadedBlocks[index].blockNum;
        if (blockNum < blockThreshold) {
            blockThreshold = blockNum;
        }
    }

    if (blockThreshold == BLOCK_UNASSIGNED) {
        return 1;
    }

    s32 cleanedCount = 0;

    for (ccMinNode* node = g_ai->humanoidList.head; node;) {
        Thing* current = static_cast<Thing*>(node);
        node = node->next;

        if (current->blockNum < blockThreshold) {
            cleanedCount++;
            current->Reset();
        }
    }

    for (ccMinNode* node = g_ai->moveList.head; node;) {
        Thing* current = static_cast<Thing*>(node);
        node = node->next;

        if (current->blockNum < blockThreshold) {
            cleanedCount++;
            current->Reset();
        }
    }

    LOG("[Switch] GateCleanupVol cleaned %d", cleanedCount);
    return 1;
}

static s32 SwitchExitTest(Thing* /*thing*/, u32 /*argc*/, const char** /*argv*/) {
    return 1;
}

static s32 SwitchDeathState(Thing* /*thing*/, u32 argc, const char** argv) {
    MARKFUNCTION(0x80094238);

    if (!Player::s_player) {
        return 1;
    }

    s32 deathStateIdx = 0;
    if (argc > 0 && argv && argv[0]) {
        deathStateIdx = atoi(argv[0]);
    }

    static constexpr s32 COLLISION_TAG_HIT_TYPE = static_cast<s32>(0x80000003u);
    Player::s_player->HandleCollision(Player::s_player, 0,
                                      -1, COLLISION_TAG_HIT_TYPE, 20, 0x80000004, deathStateIdx, 0);
    return 1;
}

struct SwitchGameFuncEntry {
    const char* name;
    SwitchGameFunc func;
    u32 bucket;
};

// PSX _9WDBSwitch_gameFuncs at 0x800D9778 (SWITCH.CPP)
// SoundAmbiantSpace, SwitchEntryTest, PlayerDeathVol, EnemyObstDeathVol,
// DirectorVol, GoToVol, SwitchExitTest, ResetPlayer, DeathState,
// BehaviorTrigger, ProximityEvent, GateCleanupVol, AsyncLoadNIS,
// AsyncLoadNISGOTO, AsyncLoadGroup, LevelComplete, Checkpoint,
// CharacterModelLoad, BossVol, PlayerLoadDialog, PlayerPlayDialog,
// EnemyLoadDialog, EnemyPlayDialog.
static const SwitchGameFuncEntry kSwitchGameFuncs[] = {
    { "SoundAmbiantSpace", SwitchSoundAmbiantSpace, 1 },
    { "SwitchEntryTest", SwitchPlayerDeathVol, 1 },
    { "PlayerDeathVol", SwitchPlayerDeathVol, 0 },
    { "EnemyObstDeathVol", SwitchEnemyObstacleDeathVol, 2 },
    { "DirectorVol", SwitchDirectorVol, 1 },
    { "GoToVol", SwitchGoToVol, 1 },
    { "SwitchExitTest", SwitchExitTest, 1 },
    { "ResetPlayer", SwitchResetPlayer, 1 },
    { "DeathState", SwitchDeathState, 1 },
    { "BehaviorTrigger", SwitchBehaviorTrigger, 1 },
    { "ProximityEvent", SwitchBehaviorTrigger, 1 },
    { "GateCleanupVol", SwitchGateCleanupVol, 1 },
    { "AsyncLoadNIS", SwitchAsyncLoadNIS, 1 },
    { "AsyncLoadNISGOTO", SwitchAsyncLoadNISGOTO, 1 },
    { "AsyncLoadGroup", SwitchAsyncLoadGroup, 1 },
    { "LevelComplete", SwitchLevelComplete, 1 },
    { "Checkpoint", SwitchCheckpoint, 1 },
    { "CharacterModelLoad", SwitchCharModelLoad, 1 },
    { "BossVol", SwitchBossVol, 1 },
    { "PlayerLoadDialog", SwitchLoadDialog, 1 },
    { "PlayerPlayDialog", SwitchPlayDialog, 1 },
    { "EnemyLoadDialog", SwitchLoadDialog, 2 },
    { "EnemyPlayDialog", SwitchPlayDialog, 2 },
    { nullptr, nullptr, 0 },
};

static bool ResolveSwitchGameFuncByName(const char* funcName, SwitchGameFunc& outFunc, u32& outBucket) {
    if (!funcName) {
        return false;
    }

    for (const SwitchGameFuncEntry* entry = kSwitchGameFuncs; entry->name; entry++) {
        if (!SwitchStringEqualsNoCase(funcName, entry->name)) {
            continue;
        }

        outFunc = entry->func;
        outBucket = entry->bucket;
        return true;
    }

    return false;
}

struct GeoMaterialInfo {
    u8 primCmd = 0;
    u16 cba = 0;
    u16 tpage = 0;
};

static u32 GetDynGeoPrimPacketSize(u8 primCmd) {
    switch (primCmd & 0xFCu) {
        case 0x3C:
        case 0x2C:
            return 52;
        case 0x38:
        case 0x28:
            return 36;
        case 0x34:
        case 0x24:
            return 40;
        case 0x30:
        case 0x20:
            return 28;
        default:
            return 0;
    }
}

static void DecodePackedUV(u16 packed, GeoRenderVertex& vertex, const GeoMaterialInfo& material) {
    vertex.u = static_cast<f32>(packed & 0xFF);
    vertex.v = static_cast<f32>((packed >> 8) & 0xFF);
    vertex.tpage = static_cast<f32>(material.tpage);
    vertex.cba = static_cast<f32>(material.cba);
}

static pddiPrimBuffer* ParseDynGeoPrims(
    const u8* geoData,
    u32 geoSize,
    const std::unordered_map<u32, GeoMaterialInfo>& materials,
    bool* outUsesSemiTrans,
    u8* outSemiTransMode,
    std::vector<GeoRenderVertex>* outVerts,
    std::vector<u16>* outVertSourceIndex,
    std::vector<u32>* outColorList,
    std::vector<u32>* outPrimStart,
    std::vector<u8>* outPrimVertCount,
    std::vector<u32>* outPrimMaterialUID,
    std::vector<u8>* outPrimCmd,
    std::vector<u16>* outPrimUVWords,
    std::vector<u32>* outPrimPacketOffset) {
    if (outUsesSemiTrans) {
        *outUsesSemiTrans = false;
    }
    if (outSemiTransMode) {
        *outSemiTransMode = 0;
    }

    if (!geoData || geoSize < 0x58) {
        return nullptr;
    }

    u32 vertListOff = p3dReadU32LE(geoData + 0x10) << 2;
    u16 numVerts = p3dReadU16LE(geoData + 0x14);
    u16 numPolys = p3dReadU16LE(geoData + 0x16);
    u32 polyListOff = p3dReadU32LE(geoData + 0x40) << 2;
    u32 colourListOff = p3dReadU32LE(geoData + 0x44) << 2;

    if (numVerts == 0 || numPolys == 0) {
        return nullptr;
    }
    if (vertListOff + numVerts * 8 > geoSize) {
        return nullptr;
    }
    if (polyListOff + numPolys * 24 > geoSize) {
        return nullptr;
    }

    const u8* verts = geoData + vertListOff;
    const u8* polys = geoData + polyListOff;
    const u8* colours = nullptr;
    if (colourListOff && colourListOff + numVerts * 4 <= geoSize) {
        colours = geoData + colourListOff;
    }

    std::vector<GeoRenderVertex> vertBuf;
    std::vector<u16> vertSourceIndexBuf;
    std::vector<u32> colorListBuf;
    std::vector<u16> idxBuf;
    std::vector<u32> primStart;
    std::vector<u8> primVertCount;
    std::vector<u32> primMaterialUID;
    std::vector<u8> primCmdList;
    std::vector<u16> primUVWords;
    std::vector<u32> primPacketOffset;
    bool usesSemiTrans = false;
    bool hasSemiTransMode = false;
    u8 semiTransMode = 0;
    u32 primListPacketOffset = 0;

    if (colours) {
        colorListBuf.resize(numVerts);
        for (u32 colourIndex = 0; colourIndex < numVerts; colourIndex++) {
            colorListBuf[colourIndex] = p3dReadU32LE(colours + colourIndex * 4) & 0x00FFFFFFu;
        }
    }

    auto makeVertex = [&](u16 index) -> GeoRenderVertex {
        GeoRenderVertex vertex = {};
        if (index >= numVerts) {
            return vertex;
        }

        const u8* src = verts + index * 8;
        vertex.x = static_cast<f32>(p3dReadS16LE(src + 0));
        vertex.y = static_cast<f32>(p3dReadS16LE(src + 2));
        vertex.z = static_cast<f32>(p3dReadS16LE(src + 4));

        // Default neutral colour for flat primitive paths.
        vertex.r = 0.85f;
        vertex.g = 0.85f;
        vertex.b = 0.85f;

        vertex.u = 0.0f;
        vertex.v = 0.0f;
        vertex.tpage = -1.0f; // default for untextured prims
        vertex.cba = 0.0f;
        return vertex;
    };

    auto applyVertexColour = [&](u16 index, GeoRenderVertex& vertex) {
        if (!colours || index >= numVerts) {
            return;
        }

        const u32 packedColour = p3dReadU32LE(colours + index * 4);
        vertex.r = static_cast<f32>(packedColour & 0xFFu) / 128.0f;
        vertex.g = static_cast<f32>((packedColour >> 8) & 0xFFu) / 128.0f;
        vertex.b = static_cast<f32>((packedColour >> 16) & 0xFFu) / 128.0f;
    };

    for (u16 polyIndex = 0; polyIndex < numPolys; polyIndex++) {
        const u8* poly = polys + polyIndex * 24;
        u32 materialHash = p3dReadU32LE(poly + 0);
        auto materialIt = materials.find(materialHash);
        if (materialIt == materials.end()) {
            continue;
        }

        const GeoMaterialInfo& material = materialIt->second;
        if ((material.primCmd & 0x02u) != 0u) {
            usesSemiTrans = true;
            if (!hasSemiTransMode) {
                semiTransMode = static_cast<u8>((material.tpage >> 5) & 3u);
                hasSemiTransMode = true;
            }
        }

        u8 primCmd = static_cast<u8>(material.primCmd & 0xFD);
        const u32 packetSize = GetDynGeoPrimPacketSize(primCmd);
        if (packetSize == 0) {
            continue;
        }
        const u32 packetOffset = primListPacketOffset;
        primListPacketOffset += packetSize;

        const u16 index0 = p3dReadU16LE(poly + 8);
        const u16 index1 = p3dReadU16LE(poly + 10);
        const u16 index2 = p3dReadU16LE(poly + 12);
        const u16 index3 = p3dReadU16LE(poly + 14);

        GeoRenderVertex v0 = makeVertex(index0);
        GeoRenderVertex v1 = makeVertex(index1);
        GeoRenderVertex v2 = makeVertex(index2);
        GeoRenderVertex v3 = makeVertex(index3);

        // PSX colour-table modulation is for gouraud primitive commands only.
        // Flat commands carry colour through a separate packet lane.
        if ((primCmd & 0x10u) != 0u) {
            applyVertexColour(index0, v0);
            applyVertexColour(index1, v1);
            applyVertexColour(index2, v2);
            applyVertexColour(index3, v3);
        }

        u16 primUVWord0 = p3dReadU16LE(poly + 16);
        u16 primUVWord1 = p3dReadU16LE(poly + 18);
        u16 primUVWord2 = p3dReadU16LE(poly + 20);
        u16 primUVWord3 = p3dReadU16LE(poly + 22);
        if (primCmd == 0x34 || primCmd == 0x24 || primCmd == 0x3C || primCmd == 0x2C) {
            DecodePackedUV(primUVWord0, v0, material);
            DecodePackedUV(primUVWord1, v1, material);
            DecodePackedUV(primUVWord2, v2, material);
            if (primCmd == 0x3C || primCmd == 0x2C) {
                DecodePackedUV(primUVWord3, v3, material);
            }
        }

        u16 base = static_cast<u16>(vertBuf.size());
        switch (primCmd) {
            case 0x30:
            case 0x20:
            case 0x34:
            case 0x24:
                vertBuf.push_back(v2);
                vertSourceIndexBuf.push_back(index2);
                vertBuf.push_back(v1);
                vertSourceIndexBuf.push_back(index1);
                vertBuf.push_back(v0);
                vertSourceIndexBuf.push_back(index0);

                // Keep rendered winding identical to (v0,v1,v2) while dynamic
                // vertex lanes follow PSX packet order (v2,v1,v0).
                idxBuf.push_back(base + 1);
                idxBuf.push_back(base + 2);
                idxBuf.push_back(base + 0);
                primStart.push_back(base);
                primVertCount.push_back(3);
                primMaterialUID.push_back(materialHash);
                primCmdList.push_back(primCmd);
                primUVWords.push_back(primUVWord0);
                primUVWords.push_back(primUVWord1);
                primUVWords.push_back(primUVWord2);
                primUVWords.push_back(primUVWord3);
                primPacketOffset.push_back(packetOffset);
                break;

            case 0x38:
            case 0x28:
            case 0x3C:
            case 0x2C:
                vertBuf.push_back(v3);
                vertSourceIndexBuf.push_back(index3);
                vertBuf.push_back(v2);
                vertSourceIndexBuf.push_back(index2);
                vertBuf.push_back(v1);
                vertSourceIndexBuf.push_back(index1);
                vertBuf.push_back(v0);
                vertSourceIndexBuf.push_back(index0);

                // Render as original (v0,v1,v2) + (v1,v3,v2) while packing
                // dynamic lanes in PSX packet order (v3,v2,v1,v0).
                idxBuf.push_back(base + 3);
                idxBuf.push_back(base + 2);
                idxBuf.push_back(base + 1);
                idxBuf.push_back(base + 2);
                idxBuf.push_back(base + 0);
                idxBuf.push_back(base + 1);
                primStart.push_back(base);
                primVertCount.push_back(4);
                primMaterialUID.push_back(materialHash);
                primCmdList.push_back(primCmd);
                primUVWords.push_back(primUVWord0);
                primUVWords.push_back(primUVWord1);
                primUVWords.push_back(primUVWord2);
                primUVWords.push_back(primUVWord3);
                primPacketOffset.push_back(packetOffset);
                break;

            default:
                break;
        }
    }

    if (idxBuf.empty()) {
        return nullptr;
    }

    u32 format = PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO;
    pddiPrimBufferDesc desc(
        PDDI_PRIM_TRIANGLES,
        format,
        static_cast<u32>(vertBuf.size()),
        static_cast<u32>(idxBuf.size()));

    pddiPrimBuffer* buffer = p3d::device->NewPrimBuffer(desc);
    buffer->SetVertexData(vertBuf.data(), static_cast<u32>(vertBuf.size()));
    buffer->SetIndices(idxBuf.data(), static_cast<u32>(idxBuf.size()));

    if (outUsesSemiTrans) {
        *outUsesSemiTrans = usesSemiTrans;
    }
    if (outSemiTransMode) {
        *outSemiTransMode = semiTransMode;
    }
    if (outVerts) {
        *outVerts = vertBuf;
    }
    if (outVertSourceIndex) {
        *outVertSourceIndex = vertSourceIndexBuf;
    }
    if (outColorList) {
        *outColorList = colorListBuf;
    }
    if (outPrimStart) {
        *outPrimStart = primStart;
    }
    if (outPrimVertCount) {
        *outPrimVertCount = primVertCount;
    }
    if (outPrimMaterialUID) {
        *outPrimMaterialUID = primMaterialUID;
    }
    if (outPrimCmd) {
        *outPrimCmd = primCmdList;
    }
    if (outPrimUVWords) {
        *outPrimUVWords = primUVWords;
    }
    if (outPrimPacketOffset) {
        *outPrimPacketOffset = primPacketOffset;
    }

    return buffer;
}

static tPrimGeom* CloneDynGeoVertexPrimGeom(const u8* geoData, u32 geoSize) {
    if (!geoData || geoSize < 0x30) {
        return nullptr;
    }

    const u32 vertListOff = p3dReadU32LE(geoData + 0x10) << 2;
    const u16 numVerts = p3dReadU16LE(geoData + 0x14);
    if (numVerts == 0 || vertListOff + static_cast<u32>(numVerts) * 8u > geoSize) {
        return nullptr;
    }

    tPrimGeom* geom = new tPrimGeom();
    geom->ownedRawData = new u8[geoSize];
    geom->ownedRawSize = geoSize;
    memcpy(geom->ownedRawData, geoData, geoSize);

    const u8* raw = geom->ownedRawData;
    geom->SetVertexList(raw + vertListOff);
    geom->numVerts = numVerts;
    geom->numPolys = p3dReadU16LE(raw + 0x16);
    geom->bboxMinX = p3dReadS32LE(raw + 0x18);
    geom->bboxMinY = p3dReadS32LE(raw + 0x1C);
    geom->bboxMinZ = p3dReadS32LE(raw + 0x20);
    geom->bboxMaxX = p3dReadS32LE(raw + 0x24);
    geom->bboxMaxY = p3dReadS32LE(raw + 0x28);
    geom->bboxMaxZ = p3dReadS32LE(raw + 0x2C);
    return geom;
}

#if REAL_TEXTURE_RENDERING
// Registers a real-texture entry for a named VRAM-rect geo texture, preferring
// a ModLoader override (at its own native resolution) and falling back to
// decoding the original data already uploaded into world's VRAM (native PSX
// resolution -- no quality gain, just routed through the same pipeline).
static void RegisterRealTexture(World* world, const char* name,
                                u16 tpage, u16 cba,
                                float offsetX, float offsetY, float sizeX, float sizeY) {
    if (!world) return;

#if MOD_LOADER
    std::vector<u8> overrideRgba;
    int overrideW = 0, overrideH = 0;
    char levelScope[16];
    std::snprintf(levelScope, sizeof(levelScope), "lev%02d", world->GetCurLevelID());
    if (ModLoader::Instance().GetTextureOverrideRGBA(levelScope, name, overrideRgba, overrideW, overrideH)) {
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

static void LoadGeoPair(
    World* world,
    const u8* permData,
    u32 permSize,
    const u8* p3dData,
    u32 p3dSize,
    s32 storeId) {
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] geo begin store=%ld perm=%lu p3d=%lu free=%lu max=%lu",
        static_cast<long>(storeId), static_cast<unsigned long>(permSize),
        static_cast<unsigned long>(p3dSize),
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    if (!g_levelManager || !permData || !p3dData || p3dSize < 6) {
        return;
    }

    if (p3dReadU16LE(p3dData) != 0xFF04) {
        return;
    }

    std::unordered_map<u32, GeoMaterialInfo> materials;

    // Track PRM (tPrimGeom) perm locations from 0x6009 chunks for STree lookup
    struct PrmInfo { u32 permOffset; u32 permSize; };
    std::unordered_map<u32, PrmInfo> prmMap; // nameHash = perm location

#if MOD_LOADER || REAL_TEXTURE_RENDERING
    // CLUT chunks (e.g. "name CLUT") always precede their indexed data chunk
    // within the stream. Remember each CLUT's VRAM rect by base name so the
    // matching data chunk can re-quantize+repaint (or register a real-texture
    // entry for) both as a pair.
    struct PendingClutRect { s16 rx, ry, rw, rh; };
    std::unordered_map<std::string, PendingClutRect> pendingGeoCluts;
#endif

    u32 rootSize = p3dReadU32LE(p3dData + 2);
    u32 chunkEnd = (rootSize < p3dSize) ? rootSize : p3dSize;
    u32 chunkPos = 6;
    u32 permCursor = 0;

    while (chunkPos + 6 <= chunkEnd) {
        u16 chunkId = p3dReadU16LE(p3dData + chunkPos);
        u32 chunkSize = p3dReadU32LE(p3dData + chunkPos + 2);
        if (chunkSize < 6 || chunkPos + chunkSize > chunkEnd) {
            break;
        }

        const u8* chunkBody = p3dData + chunkPos + 6;
        const u32 permBefore = permCursor;

        if (chunkId == 0x6001 || chunkId == 0x6002) {
            u32 nameCount = p3dReadU32LE(chunkBody + 0);
            u32 chunkPermSize = p3dReadU32LE(chunkBody + 4);
            u32 namesPos = 8;
            std::vector<std::string> names;
            names.reserve(nameCount);

            for (u32 i = 0; i < nameCount; i++) {
                if (namesPos >= chunkSize - 6) {
                    break;
                }
                u8 nameLen = chunkBody[namesPos++];
                if (namesPos + nameLen > chunkSize - 6) {
                    break;
                }
                names.emplace_back(reinterpret_cast<const char*>(chunkBody + namesPos), nameLen);
                namesPos += nameLen;
            }

            if (permCursor + chunkPermSize > permSize) {
                LOG("[World] Geo perm overflow for chunk 0x%04X (need 0x%X, have 0x%X)",
                    chunkId, permCursor + chunkPermSize, permSize);
                break;
            }

            if (chunkId == 0x6001) {
                if (nameCount != 0) {
                    u32 recordSize = chunkPermSize / nameCount;
                    if (recordSize >= 24) {
                        for (u32 i = 0; i < nameCount; i++) {
                            u32 recordOff = permCursor + i * recordSize;
                            const u8* record = permData + recordOff;

                            GeoMaterialInfo info = {};
                            // PSX primitive command byte lives in the high byte of this word.
                            info.primCmd = static_cast<u8>((p3dReadU32LE(record + 16) >> 24) & 0xFF);
                            u32 texInfo = p3dReadU32LE(record + 20);
                            info.cba = static_cast<u16>(texInfo & 0xFFFF);
                            info.tpage = static_cast<u16>(texInfo >> 16);

                            u32 materialHash = p3dReadU32LE(record + 0);
                            if (materialHash == 0 && i < names.size()) {
                                materialHash = p3dHash(names[i].c_str());
                            }
                            materials[materialHash] = info;
                            LOG("[GeoMat] hash=0x%08X primCmd=0x%02X tpage=%u cba=%u (tx=%u ty=%u depth=%u clutX=%u clutY=%u)",
                                materialHash, info.primCmd, info.tpage, info.cba,
                                info.tpage & 0xF, (info.tpage >> 4) & 1, (info.tpage >> 7) & 3,
                                (info.cba & 0x3F) * 16, (info.cba >> 6) & 0x1FF);
                        }
                    }
                }
            }
            else if (chunkId == 0x6002) {
                if (nameCount != 1 || names.empty()) {
                    LOG("[World] Unsupported multi-geo chunk with %u entries", nameCount);
                }
                else {
                    u32 modelHash = p3dReadU32LE(permData + permCursor + 0);
                    if (!g_levelManager->FindGeo(static_cast<s32>(modelHash))) {
#if MOD_LOADER
                        u32 lookupCrc = modelHash ? modelHash : p3dHash(names[0].c_str());
                        char levelScope[16];
                        std::snprintf(levelScope, sizeof(levelScope), "lev%02d", world->GetCurLevelID());
                        const std::string* glbPath = ModLoader::Instance().FindModelOverridePath(
                            levelScope, names[0].c_str());
                        if (glbPath) {
                            OriginalGeo* modGeo = GLTFLoader::LoadGeo(glbPath->c_str());
                            if (modGeo) {
                                modGeo->nameCRC = lookupCrc;
                                modGeo->SetStoreID(static_cast<s8>(storeId));
                                g_levelManager->AddOriginal(modGeo, 0);
                                LOG("[ModLoader] Level geo override: '%s' (hash 0x%08X)", names[0].c_str(), lookupCrc);
                                permCursor += chunkPermSize;
                                chunkPos += chunkSize;
                                goto next_chunk;
                            }
                        }
#endif
                        bool usesSemiTrans = false;
                        u8 semiTransMode = 0;
                        std::vector<GeoRenderVertex> dynamicVerts;
                        std::vector<u16> dynamicVertSourceIndex;
                        std::vector<u32> dynamicColorList;
                        std::vector<u32> dynamicPrimStart;
                        std::vector<u8> dynamicPrimVertCount;
                        std::vector<u32> dynamicPrimMaterialUID;
                        std::vector<u8> dynamicPrimCmd;
                        std::vector<u16> dynamicPrimUVWords;
                        std::vector<u32> dynamicPrimPacketOffset;
                        pddiPrimBuffer* buffer = ParseDynGeoPrims(
                            permData + permCursor,
                            chunkPermSize,
                            materials,
                            &usesSemiTrans,
                            &semiTransMode,
                            &dynamicVerts,
                            &dynamicVertSourceIndex,
                            &dynamicColorList,
                            &dynamicPrimStart,
                            &dynamicPrimVertCount,
                            &dynamicPrimMaterialUID,
                            &dynamicPrimCmd,
                            &dynamicPrimUVWords,
                            &dynamicPrimPacketOffset);
                        if (buffer) {
                            OriginalGeo* original = new OriginalGeo();
                            original->nameCRC = modelHash ? modelHash : p3dHash(names[0].c_str());
                            original->SetStoreID(static_cast<s8>(storeId));
                            original->meshBuffer = buffer;
                            original->primGeom = CloneRawPrimGeom(permData + permCursor, chunkPermSize);
                            if (!original->primGeom) {
                                original->primGeom = CloneDynGeoVertexPrimGeom(permData + permCursor, chunkPermSize);
                            }
                            original->usesSemiTrans = usesSemiTrans;
                            original->semiTransMode = semiTransMode;
                            original->bboxMin[0] = p3dReadS32LE(permData + permCursor + 0x18);
                            original->bboxMin[1] = p3dReadS32LE(permData + permCursor + 0x1C);
                            original->bboxMin[2] = p3dReadS32LE(permData + permCursor + 0x20);
                            original->bboxMax[0] = p3dReadS32LE(permData + permCursor + 0x24);
                            original->bboxMax[1] = p3dReadS32LE(permData + permCursor + 0x28);
                            original->bboxMax[2] = p3dReadS32LE(permData + permCursor + 0x2C);

                            if (!dynamicVerts.empty()) {
                                original->dynamicVertCount = static_cast<u32>(dynamicVerts.size());
                                original->dynamicVerts = new GeoRenderVertex[original->dynamicVertCount];
                                if (original->dynamicVerts) {
                                    memcpy(original->dynamicVerts,
                                           dynamicVerts.data(),
                                           sizeof(GeoRenderVertex) * original->dynamicVertCount);
                                }

                                if (dynamicVertSourceIndex.size() == original->dynamicVertCount) {
                                    original->dynamicVertSourceIndex = new u16[original->dynamicVertCount];
                                    if (original->dynamicVertSourceIndex) {
                                        memcpy(original->dynamicVertSourceIndex,
                                               dynamicVertSourceIndex.data(),
                                               sizeof(u16) * original->dynamicVertCount);
                                    }
                                }
                            }

                            if (!dynamicColorList.empty()) {
                                original->dynamicColorCount = static_cast<u32>(dynamicColorList.size());
                                original->dynamicColorList = new u32[original->dynamicColorCount];
                                if (original->dynamicColorList) {
                                    memcpy(original->dynamicColorList,
                                           dynamicColorList.data(),
                                           sizeof(u32) * original->dynamicColorCount);
                                }
                                else {
                                    original->dynamicColorCount = 0;
                                }
                            }

                            if (!dynamicPrimStart.empty()
                                && dynamicPrimStart.size() == dynamicPrimVertCount.size()
                                && dynamicPrimStart.size() == dynamicPrimMaterialUID.size()
                                && dynamicPrimStart.size() == dynamicPrimCmd.size()
                                && dynamicPrimUVWords.size() == dynamicPrimStart.size() * 4u
                                && dynamicPrimStart.size() == dynamicPrimPacketOffset.size()) {
                                original->dynamicPrimCount = static_cast<u32>(dynamicPrimStart.size());
                                original->dynamicPrimStart = new u32[original->dynamicPrimCount];
                                original->dynamicPrimVertCount = new u8[original->dynamicPrimCount];
                                original->dynamicPrimMaterialUID = new u32[original->dynamicPrimCount];
                                original->dynamicPrimCmd = new u8[original->dynamicPrimCount];
                                original->dynamicPrimUVWords = new u16[original->dynamicPrimCount * 4u];
                                original->dynamicPrimPacketOffset = new u32[original->dynamicPrimCount];
                                if (original->dynamicPrimStart
                                    && original->dynamicPrimVertCount
                                    && original->dynamicPrimMaterialUID
                                    && original->dynamicPrimCmd
                                    && original->dynamicPrimUVWords
                                    && original->dynamicPrimPacketOffset) {
                                    memcpy(original->dynamicPrimStart,
                                           dynamicPrimStart.data(),
                                           sizeof(u32) * original->dynamicPrimCount);
                                    memcpy(original->dynamicPrimVertCount,
                                           dynamicPrimVertCount.data(),
                                           sizeof(u8) * original->dynamicPrimCount);
                                    memcpy(original->dynamicPrimMaterialUID,
                                           dynamicPrimMaterialUID.data(),
                                           sizeof(u32) * original->dynamicPrimCount);
                                    memcpy(original->dynamicPrimCmd,
                                           dynamicPrimCmd.data(),
                                           sizeof(u8) * original->dynamicPrimCount);
                                    memcpy(original->dynamicPrimUVWords,
                                           dynamicPrimUVWords.data(),
                                           sizeof(u16) * original->dynamicPrimCount * 4u);
                                    memcpy(original->dynamicPrimPacketOffset,
                                           dynamicPrimPacketOffset.data(),
                                           sizeof(u32) * original->dynamicPrimCount);
                                }
                            }

                            g_levelManager->AddOriginal(original, 0);
                            LOG("[World] Loaded Geo model '%s' (hash 0x%08X, store %d)",
                                names[0].c_str(), original->nameCRC, storeId);
                        }
                    }
                }
            }

            permCursor += chunkPermSize;
        }

        // PSX: tUVAnimLoader::Load (UVANIM.CPP:197)
        // Chunk 0x600E registers tUVAnim in INVANI (per-primitive UV frame anim).
        else if (chunkId == 0x600E) {
            RegisterUVListAnimChunk(chunkBody, chunkSize - 6, static_cast<u8>(storeId));
        }

        // PSX: tVertAnimLoader::Load (TVRTLOAD.CPP:32)
        // Chunk 0x6004 consumes perm payload and registers tFrameList in INVANI.
        else if (chunkId == 0x6004) {
            if (!RegisterFrameListAnimChunk(chunkBody,
                chunkSize - 6,
                permData,
                permCursor,
                permSize,
                static_cast<u8>(storeId))) {
                break;
            }
        }

        // PSX: tClutAnimLoader::Load (TCLTLOAD.CPP:29)
        // Chunk 0x6006 registers tClutList in INVANI.
        else if (chunkId == 0x6006) {
            if (!RegisterClutAnimChunk(chunkBody, chunkSize - 6, static_cast<u8>(storeId))) {
                break;
            }
        }

        // PSX: tTranAnimLoader2::Load (TRANLOAD.CPP:98)
        // Chunk 0x6400 consumes perm payload and registers tTransformAnim in INVANI.
        else if (chunkId == 0x6400) {
            if (!RegisterTransformAnimChunk(chunkBody,
                chunkSize - 6,
                permData,
                permCursor,
                permSize,
                static_cast<u8>(storeId))) {
                break;
            }
        }

        // PSX: tSequenceAnimLoader::Load (SEQUENCE.CPP:26)
        // Chunk 0x6040 consumes perm payload and registers a tSequenceAnim in INVANI.
        else if (chunkId == 0x6040) {
            if (!RegisterSequenceAnimChunk(chunkBody,
                chunkSize - 6,
                permData,
                permCursor,
                permSize,
                static_cast<u8>(storeId))) {
                break;
            }
        }

        // PSX: tCompAnimLoader::Load (COMPANIM.CPP:78)
        // Chunk 0x4007 defines tCompositeAnim parts by animation name.
        else if (chunkId == 0x4007) {
            RegisterCompositeAnimChunk(chunkBody, chunkSize - 6, static_cast<u8>(storeId));
        }

        // PSX: tParamAnimLoader::Load (PARAMLOAD.CPP:279)
        // Chunk 0x4300 registers tParamAnim in INVANI.
        else if (chunkId == 0x4300) {
            if (!RegisterParamAnimChunk(chunkBody, chunkSize - 6, static_cast<u8>(storeId))) {
                break;
            }
        }

        // PSX: tVizAnimLoader::Load (VIZANIM.CPP:116)
        // Chunk 0x4020 registers tVizAnim in INVANI (used by composite VIZ_* parts).
        else if (chunkId == 0x4020) {
            RegisterVizAnimChunk(chunkBody, chunkSize - 6, static_cast<u8>(storeId));
        }

        // PSX: tCBVParamAnimLoader::Load (CBVPARAM.CPP:243)
        // Chunk 0x6021 registers cbv_* misc anim tables in INVANI.
        else if (chunkId == 0x6021) {
            if (!RegisterCBVParamAnimChunk(chunkBody, chunkSize - 6, static_cast<u8>(storeId))) {
                break;
            }
        }

        // PSX PALDATA.CPP: 0x8C00/0x8C01 use shared palette metadata + perm cursor.
        else if (chunkId == 0x8C00 || chunkId == 0x8C01) {
            if (!LoadPaletteData(chunkId, chunkBody, chunkSize - 6, permData, permCursor, permSize)) {
                LOG("[World] Failed loading palette chunk 0x%04X", chunkId);
                break;
            }
        }

        // PSX SCALEDAT.CPP: 0x8B01 consumes variable-size perm payload.
        else if (chunkId == 0x8B01) {
            if (!LoadScaleData(chunkId, chunkBody, chunkSize - 6, permData, permCursor, permSize)) {
                LOG("[World] Failed loading ScaleData chunk 0x%04X", chunkId);
                break;
            }
        }

        else if (chunkId == 0x8C20 || chunkId == 0x8C21) {
            LoadUVPrimData(chunkId, chunkBody, chunkSize - 6,
                           permData, permCursor, permSize);
        }

        else if (chunkId == 0x8C30 || chunkId == 0x8C31) {
            LoadCBVPrimData(chunkId, chunkBody, chunkSize - 6,
                            permData, permCursor, permSize);
        }

        else if (chunkId == 0x8A00) {
            LoadWEffectChunk(chunkBody, chunkSize - 6);
        }

        else if (chunkId == 0x8A10) {
            GEffect_LoadChunk(chunkBody, chunkSize - 6);
        }

        else if (chunkId == 0x8A20) {
            Obstacle_LoadAnimChunk(chunkBody, chunkSize - 6);
        }

        else if (chunkId == 0x8A30) {
            LoadParticleSystemChunk(chunkBody, chunkSize - 6);
        }

        // PSX: tETreeLoader::Load / myETreeLoaderCallback (STREAM.CPP:678)
        // Chunk 0x6140 = tETree. Body: pstring name, u16 jointCount, sub-chunks 0x6141.
        // Creates OriginalETree and registers in LevelManager for FindModel lookups.
        else if (chunkId == 0x6140) {
            u32 bodyLen = chunkSize - 6;
            if (bodyLen >= 3) {
                u8 nameLen = chunkBody[0];
                if ((u32)(1 + nameLen + 2) <= bodyLen) {
                    char nameBuf[256];
                    u32 copyLen = (nameLen < 255) ? nameLen : 255;
                    memcpy(nameBuf, chunkBody + 1, copyLen);
                    nameBuf[copyLen] = '\0';

                    u32 nameHash = p3dHash(nameBuf);
                    struct ETreePartEntry {
                        u32 jointHash;
                        u32 modelHash;
                        u32 jointFlags;
                        s16 rotX;
                        s16 rotY;
                        s16 rotZ;
                        s32 transX;
                        s32 transY;
                        s32 transZ;
                    };
                    std::vector<ETreePartEntry> geoPartEntries;

                    u32 subCursor = 1u + static_cast<u32>(nameLen) + 2u;
                    while (subCursor + 6 <= bodyLen) {
                        const u16 subChunkId = p3dReadU16LE(chunkBody + subCursor);
                        const u32 subChunkSize = p3dReadU32LE(chunkBody + subCursor + 2);
                        if (subChunkSize < 6 || subCursor + subChunkSize > bodyLen) {
                            break;
                        }

                        if (subChunkId == 0x6141) {
                            const u8* subBody = chunkBody + subCursor + 6;
                            const u32 subBodyLen = subChunkSize - 6;
                            u32 subBodyCursor = 0;
                            u32 jointNameHash = 0;
                            u32 modelNameHash = 0;

                            if (ReadChunkPStringHash(subBody, subBodyLen, subBodyCursor, &jointNameHash)
                                && ReadChunkPStringHash(subBody, subBodyLen, subBodyCursor, &modelNameHash)
                                && modelNameHash != 0
                                && modelNameHash != nameHash) {
                                ETreePartEntry entry = {};
                                entry.jointHash = jointNameHash;
                                entry.modelHash = modelNameHash;

                                // ETLOAD AddJoint reads these fields in order after the two names.
                                if (subBodyCursor + 22 <= subBodyLen) {
                                    entry.jointFlags = p3dReadU32LE(subBody + subBodyCursor);
                                    subBodyCursor += 4;
                                    entry.rotX = p3dReadS16LE(subBody + subBodyCursor);
                                    subBodyCursor += 2;
                                    entry.rotY = p3dReadS16LE(subBody + subBodyCursor);
                                    subBodyCursor += 2;
                                    entry.rotZ = p3dReadS16LE(subBody + subBodyCursor);
                                    subBodyCursor += 2;
                                    entry.transX = p3dReadS32LE(subBody + subBodyCursor);
                                    subBodyCursor += 4;
                                    entry.transY = p3dReadS32LE(subBody + subBodyCursor);
                                    subBodyCursor += 4;
                                    entry.transZ = p3dReadS32LE(subBody + subBodyCursor);
                                    subBodyCursor += 4;
                                }

                                geoPartEntries.push_back(entry);
                            }
                        }

                        subCursor += subChunkSize;
                    }

                    // PSX: always creates ETree (no duplicate check against Geo list).
                    // Both Geo and ETree can coexist with the same nameCRC in different lists.
                    OriginalETree* et = new OriginalETree();
                    et->nameCRC = nameHash;
                    et->SetStoreID(static_cast<s8>(storeId));

                    if (!geoPartEntries.empty()) {
                        const u16 partCount = static_cast<u16>(geoPartEntries.size());
                        et->geoPartCount = partCount;
                        et->geoPartHashes = new u32[partCount]();
                        et->geoPartJointHashes = new u32[partCount]();
                        et->geoParts = new OriginalGeo * [partCount]();

                        et->skeleton = new STreeData();
                        if (et->skeleton) {
                            et->skeleton->numJoints = partCount;
                            et->skeleton->numMapEntries = partCount;
                            et->skeleton->joints = static_cast<STreeJoint*>(std::calloc(partCount, sizeof(STreeJoint)));
                            et->skeleton->jointOrderMap = static_cast<u32*>(std::malloc(sizeof(u32) * partCount));
                            if (!et->skeleton->joints || !et->skeleton->jointOrderMap) {
                                delete et->skeleton;
                                et->skeleton = nullptr;
                            }
                        }

                        static constexpr u32 ETREE_FLAG_PUSH = 0x00010000;
                        static constexpr u32 ETREE_FLAG_POP = 0x00020000;
                        static constexpr u32 ETREE_FLAG_DRAW = 0x00040000;
                        static constexpr u32 ETREE_FLAG_TRANSFORM_MASK = 0x00300000;

                        u16 resolvedParts = 0;
                        for (u16 partIndex = 0; partIndex < partCount; partIndex++) {
                            const ETreePartEntry& entry = geoPartEntries[partIndex];
                            const u32 modelHash = entry.modelHash;
                            et->geoPartJointHashes[partIndex] = entry.jointHash;
                            et->geoPartHashes[partIndex] = modelHash;

                            OriginalBasic* geoBasic = g_levelManager->FindGeo(static_cast<s32>(modelHash));
                            if (geoBasic && geoBasic->GetType() == 0) {
                                et->geoParts[partIndex] = static_cast<OriginalGeo*>(geoBasic);
                                if (et->geoParts[partIndex] && et->geoParts[partIndex]->meshBuffer) {
                                    resolvedParts++;
                                }
                            }

                            if (et->skeleton && et->skeleton->joints && et->skeleton->jointOrderMap) {
                                STreeJoint& joint = et->skeleton->joints[partIndex];
                                et->skeleton->jointOrderMap[partIndex] = partIndex;
                                joint.nameUID = entry.jointHash;
                                joint.flags = 0;
                                if (entry.jointFlags & ETREE_FLAG_PUSH) {
                                    joint.flags |= STF_PUSH_MATRIX;
                                }
                                if (entry.jointFlags & ETREE_FLAG_POP) {
                                    joint.flags |= STF_POP_MATRIX;
                                }
                                if ((entry.jointFlags & ETREE_FLAG_TRANSFORM_MASK) == ETREE_FLAG_TRANSFORM_MASK) {
                                    joint.flags |= STF_TRANSFORM;
                                }
                                if (entry.jointFlags & ETREE_FLAG_DRAW) {
                                    joint.flags |= STF_HAS_MESH;
                                }

                                joint.rotationX = entry.rotX;
                                joint.rotationY = entry.rotY;
                                joint.rotationZ = entry.rotZ;
                                joint.translationX = entry.transX;
                                joint.translationY = entry.transY;
                                joint.translationZ = entry.transZ;

                                joint.bindRotationX = entry.rotX;
                                joint.bindRotationY = entry.rotY;
                                joint.bindRotationZ = entry.rotZ;
                                joint.bindTranslationX = entry.transX;
                                joint.bindTranslationY = entry.transY;
                                joint.bindTranslationZ = entry.transZ;
                                joint.captureBufferIdx = -1;
                                joint.renderScale = 1.0f;
                            }

                            LOG("[World] ETree '%s' part[%u] jointHash=0x%08X modelHash=0x%08X resolved=%d",
                                nameBuf,
                                partIndex,
                                et->geoPartJointHashes ? et->geoPartJointHashes[partIndex] : 0,
                                modelHash,
                                (et->geoParts[partIndex] && et->geoParts[partIndex]->meshBuffer) ? 1 : 0);
                        }

                        LOG("[World] ETree '%s' parts=%u resolved=%u", nameBuf, partCount, resolvedParts);
                    }

                    g_levelManager->AddOriginal(et, 0);
                    LOG("[World] Loaded ETree '%s' (hash 0x%08X, store %d)", nameBuf, nameHash, storeId);
                }
            }
        }

        else if (chunkId == 0x6008 && world) {
            u32 p = 0;
            u32 bodyLen = chunkSize - 6;
            if (bodyLen < 1) { chunkPos += chunkSize; continue; }
            u8 nameLen = chunkBody[p++];
            std::string geoTexName(reinterpret_cast<const char*>(chunkBody + p), nameLen);
            p += nameLen;
            // PStrings may be padded with trailing nulls/spaces for alignment.
            while (!geoTexName.empty() && (geoTexName.back() == ' ' || geoTexName.back() == '\0'))
                geoTexName.pop_back();
            if (p + 12 > bodyLen) { chunkPos += chunkSize; continue; }
            s16 rx = p3dReadS16LE(chunkBody + p); p += 2;
            s16 ry = p3dReadS16LE(chunkBody + p); p += 2;
            s16 rw = p3dReadS16LE(chunkBody + p); p += 2;
            s16 rh = p3dReadS16LE(chunkBody + p); p += 2;
            p += 4; // skip type
            if (rw > 0 && rh > 0 && rw <= 1024 && rh <= 512 &&
                p + (u32)(rw * rh * 2) <= bodyLen) {
                world->UploadToVRAM(rx, ry, rw, rh, chunkBody + p);
                LOG("[GeoTex] VRAM upload: x=%d y=%d w=%d h=%d", rx, ry, rw, rh);

#if MOD_LOADER || REAL_TEXTURE_RENDERING
                static constexpr const char* kClutSuffix = " CLUT";
                const size_t suffixPos = geoTexName.size() >= 5 ? geoTexName.size() - 5 : std::string::npos;
                const bool isClut = suffixPos != std::string::npos && geoTexName.compare(suffixPos, 5, kClutSuffix) == 0;

                if (isClut) {
                    pendingGeoCluts[geoTexName.substr(0, suffixPos)] = PendingClutRect{ rx, ry, rw, rh };
                }
                else {
                    auto clutIt = pendingGeoCluts.find(geoTexName);
                    if (clutIt != pendingGeoCluts.end()) {
                        const PendingClutRect& clutRect = clutIt->second;
                        const s16 paletteColorCount = clutRect.rw * clutRect.rh;
#if MOD_LOADER
                        std::vector<u16> clutOverride, indexOverride;
                        char levelScope[16];
                        std::snprintf(levelScope, sizeof(levelScope), "lev%02d", world->GetCurLevelID());
                        if (ModLoader::Instance().GetIndexedTextureOverride(
                            levelScope, geoTexName.c_str(), rw, rh, paletteColorCount,
                            clutOverride, indexOverride)) {
                            world->UploadToVRAM(clutRect.rx, clutRect.ry, clutRect.rw, clutRect.rh,
                                                reinterpret_cast<const u8*>(clutOverride.data()));
                            world->UploadToVRAM(rx, ry, rw, rh, reinterpret_cast<const u8*>(indexOverride.data()));
                            LOG("[ModLoader] Geo texture override: %s", geoTexName.c_str());
                        }
#endif
#if REAL_TEXTURE_RENDERING
                        {
                            const u8 ttx = static_cast<u8>(rx / 64), tty = static_cast<u8>(ry / 256);
                            const int dep = (paletteColorCount == 256) ? 1 : 0;
                            const u16 tpage = static_cast<u16>(ttx | (tty << 4) | (dep << 7));
                            const u16 cba = static_cast<u16>((clutRect.rx / 16) | (clutRect.ry << 6));
                            const int bppDiv = (paletteColorCount == 256) ? 2 : 4;
                            RegisterRealTexture(world, geoTexName.c_str(), tpage, cba,
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
                        char levelScope[16];
                        std::snprintf(levelScope, sizeof(levelScope), "lev%02d", world->GetCurLevelID());
                        if (ModLoader::Instance().GetTextureOverridePixels(levelScope, geoTexName.c_str(), rw, rh, overridePixels)) {
                            world->UploadToVRAM(rx, ry, rw, rh, reinterpret_cast<const u8*>(overridePixels.data()));
                            LOG("[ModLoader] Geo texture override: %s", geoTexName.c_str());
                        }
#endif
#if REAL_TEXTURE_RENDERING
                        {
                            const u8 ttx = static_cast<u8>(rx / 64), tty = static_cast<u8>(ry / 256);
                            const u16 tpage = static_cast<u16>(ttx | (tty << 4) | (2 << 7));
                            RegisterRealTexture(world, geoTexName.c_str(), tpage, 0,
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

        // PSX: tPrimLoader::Load (TPRMLOAD.CPP:61, 0x80088A80)
        // Chunk 0x6009 = tPrimGeom. Body: u32 permSize, p-string name.
        // Creates tPrimGeom from perm data at current cursor, stores in P3D inventory.
        // PC: record perm offset/size for later STree lookup, advance permCursor.
        else if (chunkId == 0x6009) {
            u32 bodyLen = chunkSize - 6;
            if (bodyLen >= 5) {
                u32 prmPermSize = p3dReadU32LE(chunkBody + 0);
                u8 prmNameLen = chunkBody[4];
                if ((u32)(5 + prmNameLen) <= bodyLen) {
                    char prmNameBuf[256];
                    u32 copyLen = (prmNameLen < 255) ? prmNameLen : 255;
                    memcpy(prmNameBuf, chunkBody + 5, copyLen);
                    prmNameBuf[copyLen] = '\0';
                    u32 prmHash = p3dHash(prmNameBuf);

                    if (permCursor + prmPermSize <= permSize) {
                        prmMap[prmHash] = { permCursor, prmPermSize };
                        LOG("[World] PRM '%s' (hash 0x%08X) at permOff=%u size=%u",
                            prmNameBuf, prmHash, permCursor, prmPermSize);
                    }
                    permCursor += prmPermSize;
                }
            }
        }

        // PSX: tSTreeLoader::Load / mySTreeLoaderCallback (STREAM.CPP:704, 0x8009976C)
        // Chunk 0x6120 = tSTree. Body: p-string name, u16 jointCount, p-string prmName,
        // u32 permSize, sub-chunks 0x6121 (tSJoint), optional 0x6122 (joint map).
        // Creates OriginalSTree with nameCRC, references PRM for geometry data.
        // PC: create OriginalSTree, build mesh from PRM perm data, register via AddOriginal.
        else if (chunkId == 0x6120) {
            u32 bodyLen = chunkSize - 6;
            u32 p = 0;
            if (bodyLen >= 3) {
                u8 nameLen = chunkBody[p++];
                if (p + nameLen + 2 <= bodyLen) {
                    char nameBuf[256];
                    u32 copyLen = (nameLen < 255) ? nameLen : 255;
                    memcpy(nameBuf, chunkBody + p, copyLen);
                    nameBuf[copyLen] = '\0';
                    p += nameLen;

                    u16 jointCount = p3dReadU16LE(chunkBody + p); p += 2;

                    // Read PRM name
                    char prmNameBuf[256] = {};
                    if (p < bodyLen) {
                        u8 prmNameLen = chunkBody[p++];
                        u32 prmCopy = (prmNameLen < 255) ? prmNameLen : 255;
                        if (p + prmCopy <= bodyLen) {
                            memcpy(prmNameBuf, chunkBody + p, prmCopy);
                            prmNameBuf[prmCopy] = '\0';
                            p += prmNameLen;
                        }
                    }

                    // Read permSize (perm consumed by STree joint data)
                    u32 streePermSize = 0;
                    if (p + 4 <= bodyLen) {
                        streePermSize = p3dReadU32LE(chunkBody + p);
                        p += 4;
                    }

                    u32 nameHash = p3dHash(nameBuf);

                    OriginalSTree* original = new OriginalSTree();
                    original->nameCRC = nameHash;
                    original->SetStoreID(static_cast<s8>(storeId));
                    original->xformVertsCallback = RP_XformVertsLitCBF_CL;
                    original->fixUpPolysCallback = RP_FixUpPolysCBF_CL;
                    original->skeleton = ParseSTreeChunk(chunkBody, bodyLen, false);

                    // Look up PRM geometry data from perm
                    u32 prmHash = p3dHash(prmNameBuf);
                    auto prmIt = prmMap.find(prmHash);
                    if (prmIt != prmMap.end()) {
                        const PrmInfo& prm = prmIt->second;
                        if (prm.permOffset + prm.permSize <= permSize) {
                            if (original->skeleton) {
                                BuildPerJointMeshes(original,
                                                    permData + prm.permOffset, prm.permSize);
                            }

                            if (original->skeleton && original->skinData &&
                                original->skeleton->joints &&
                                original->skeleton->numJoints > 0 &&
                                original->skeleton->joints[0].meshBuffer) {
                                LOG("[World] STree '%s' skinned mesh built from PRM '%s' (%u verts, %u joints)",
                                    nameBuf, prmNameBuf,
                                    original->skeleton->joints[0].meshBuffer->GetVertexCount(),
                                    original->skeleton->numJoints);
                            }
                            else {
                                pddiPrimBuffer* meshBuf = BuildPrimBufferFromRawPrimGeom(
                                    permData + prm.permOffset, prm.permSize
#if REAL_TEXTURE_RENDERING
                                    , &original->realTexGroups
#endif
                                );
                                if (meshBuf) {
                                    original->meshBuffer = meshBuf;
                                    LOG("[World] STree '%s' mesh built from PRM '%s' (%u verts)",
                                        nameBuf, prmNameBuf, meshBuf->GetVertexCount());
                                }
                                else {
                                    LOG("[World] STree '%s' PRM '%s' mesh parse failed",
                                        nameBuf, prmNameBuf);
                                }
                            }
                        }
                    }
                    else {
                        LOG("[World] STree '%s' PRM '%s' not found in prmMap",
                            nameBuf, prmNameBuf);
                    }

                    g_levelManager->AddOriginal(original, 0);
                    permCursor += streePermSize;

                    LOG("[World] Loaded STree '%s' (hash 0x%08X, %u joints, store %d)",
                        nameBuf, nameHash, jointCount, storeId);
                }
            }
        }

        if (storeId == 2 && ((permBefore >= 32000 && permBefore <= 34000)
            || chunkId == 0x8C20 || chunkId == 0x8C21)) {
            LOG("[World] GeoPair chunk: store=%d id=0x%04X chunkPos=%u bodyLen=%u permBefore=%u permAfter=%u",
                storeId,
                chunkId,
                chunkPos,
                chunkSize - 6,
                permBefore,
                permCursor);
        }

next_chunk:
        chunkPos += chunkSize;
    }
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] geo end store=%ld free=%lu max=%lu",
        static_cast<long>(storeId),
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
}

static void LoadGeoPairsInRange(
    World* world,
    const std::vector<tStreamEntry>& entries,
    const u8* fileData,
    u32 fileSize,
    u32 rangeStart,
    u32 rangeEnd,
    const char* permMagic,
    const char* p3dMagic,
    s32 storeId) {
    (void)p3dMagic;

    for (u32 i = rangeStart; i < rangeEnd; i++) {
        if (strncmp(entries[i].magic, permMagic, 4) != 0) {
            continue;
        }

        // PSX STREAM.CPP LoadPermChunk__6Stream consumes the current entry as
        // perm payload and then advances to the immediate next stream entry for
        // the paired P3D payload.
        const u32 pairIndex = i + 1;
        if (pairIndex >= rangeEnd) {
            continue;
        }

        const tStreamEntry& permEntry = entries[i];
        const tStreamEntry& p3dEntry = entries[pairIndex];
        if (permEntry.offset + permEntry.size > fileSize ||
            p3dEntry.offset + p3dEntry.size > fileSize) {
            continue;
        }

        LoadGeoPair(
            world,
            fileData + permEntry.offset,
            permEntry.size,
            fileData + p3dEntry.offset,
            p3dEntry.size,
            storeId);
    }
}

static bool LoadBlocksForPetalFromStream(
    BlockManager& blockMgr,
    const std::vector<u8>& streamData,
    u32 targetPetal,
    u32 startBlockNum,
    const char* logTag) {
    if (streamData.empty()) {
        return false;
    }

    const u32 dataSize = static_cast<u32>(streamData.size());
    const u8* data = streamData.data();
    const auto entries = ParseStreamHeader(data, dataSize);
    if (entries.empty()) {
        return false;
    }

    std::vector<u32> wdbIndices;
    for (u32 i = 0; i < (u32)entries.size(); i++) {
        if (strncmp(entries[i].magic, ".WDB", 4) == 0) {
            wdbIndices.push_back(i);
        }
    }
    if (wdbIndices.empty()) {
        return false;
    }

    u32 petalIndex = targetPetal;
    if (petalIndex >= (u32)wdbIndices.size()) {
        petalIndex = 0;
    }

    const u32 petalStart = wdbIndices[petalIndex];
    const u32 petalEnd = (petalIndex + 1 < (u32)wdbIndices.size())
        ? wdbIndices[petalIndex + 1]
        : (u32)entries.size();

    std::vector<const u8*> blkPtrs;
    std::vector<u32> blkSizes;
    for (u32 i = petalStart; i < petalEnd; i++) {
        if (strncmp(entries[i].magic, ".BLK", 4) != 0) {
            continue;
        }

        if (entries[i].offset + entries[i].size > dataSize) {
            blkPtrs.push_back(nullptr);
            blkSizes.push_back(0);
        }
        else {
            blkPtrs.push_back(data + entries[i].offset);
            blkSizes.push_back(entries[i].size);
        }
    }

    const u32 blkCount = static_cast<u32>(blkPtrs.size());
    blockMgr.LoadBlocks(startBlockNum, blkPtrs.data(), blkSizes.data(), blkCount);

    LOG("[%s] Parsed %u BLK entries for petal %u startBlock=%u", logTag, blkCount, petalIndex, startBlockNum);
    return true;
}
// Database::Scan handles WDB parsing now (see database.cpp).

#if MOD_LOADER
// Upload a PNG file as a PSX texture page into the VRAM simulation.
// tpage encodes: tx = bits 0-3 (64-word column), ty = bit 4 (256-row band).
// A texture page is 256x256 pixels; in 15-bit mode each pixel = 1 word at (tx*64+x, ty*256+y).
static void UploadPngToVRAMPage(PsxVRAM& vram, u16 tpage, const char* pngPath) {
    int w, h, ch;
    const std::string resolvedPngPath = p3d::io::ResolvePath(pngPath);
    unsigned char* px = stbi_load(resolvedPngPath.c_str(), &w, &h, &ch, 4);
    if (!px) {
        LOG("[ModLoader] Failed to load PNG for VRAM page: %s", pngPath);
        return;
    }

    const int pageX = (tpage & 0xF) * 64;
    const int pageY = ((tpage >> 4) & 1) * 256;
    const int imgW = (w < 256) ? w : 256;
    const int imgH = (h < 256) ? h : 256;

    for (int y = 0; y < imgH; y++) {
        for (int x = 0; x < imgW; x++) {
            const unsigned char* src = px + (y * w + x) * 4;
            const u8 r = src[0], g = src[1], b = src[2], a = src[3];
            const u16 abgr1555 = static_cast<u16>(
                ((a > 127 ? 1u : 0u) << 15) |
                (((u16)(b >> 3)) << 10) |
                (((u16)(g >> 3)) << 5) |
                ((u16)(r >> 3)));
            const int vx = pageX + x;
            const int vy = pageY + y;
            if (vx >= 0 && vx < 1024 && vy >= 0 && vy < 512)
                vram.Set(vx, vy, abgr1555);
        }
    }
    stbi_image_free(px);
    LOG("[ModLoader] Level texture override: tpg=%04x <- %s", tpage, pngPath);
}
#endif

void World::LoadTPGTexturePayload(const u8* d, u32 size) {
    if (!d || size < 6) return;
    u16 rootId = p3dReadU16LE(d);
    u32 rootSize = p3dReadU32LE(d + 2);
    if (rootId != 0xFF04) return;

        u32 cpos = 6;
        u32 cend = (rootSize < size) ? rootSize : size;
        while (cpos + 6 <= cend) {
            u16 chunkId = p3dReadU16LE(d + cpos);
            u32 chunkSize = p3dReadU32LE(d + cpos + 2);
            if (chunkSize < 6 || cpos + chunkSize > cend) break;

            if (chunkId == 0x6008) {
                u32 doff = cpos + 6;
                u32 dlen = chunkSize - 6;
                u32 p = doff;

                // PString: u8 len + chars
                if (p >= cpos + chunkSize) { cpos += chunkSize; continue; }
                u8 nameLen = d[p++];
                p += nameLen; // skip name

                // RECT16: s16 x, y, w, h + u32 type
                if (p + 12 > doff + dlen) { cpos += chunkSize; continue; }
                s16 rx = p3dReadS16LE(d + p); p += 2;
                s16 ry = p3dReadS16LE(d + p); p += 2;
                s16 rw = p3dReadS16LE(d + p); p += 2;
                s16 rh = p3dReadS16LE(d + p); p += 2;
                p += 4; // skip type

                // Upload raw pixel data to VRAM
                if (rw > 0 && rh > 0 && rw <= 1024 && rh <= 512 &&
                    p + rw * rh * 2 <= size) {
                    vram.Upload(rx, ry, rw, rh, d + p);
                    LOG("[World] VRAM upload: x=%d y=%d w=%d h=%d", rx, ry, rw, rh);
                }
            }
            cpos += chunkSize;
        }
}

void World::LoadTPGTextures(const u8* lcfData, u32 lcfSize) {
    // Re-parse stream header to find TPG entries
    if (lcfSize < 4) return;
    u32 count = (lcfData[0] << 24) | (lcfData[1] << 16) | (lcfData[2] << 8) | lcfData[3];
    u32 pos = 4;
    for (u32 i = 0; i < count; i++) {
        if (pos + 16 > lcfSize) break;
        char magic[5] = {};
        memcpy(magic, lcfData + pos, 4);
        u32 size = (lcfData[pos + 4] << 24) | (lcfData[pos + 5] << 16) | (lcfData[pos + 6] << 8) | lcfData[pos + 7];
        u32 offset = (lcfData[pos + 8] << 24) | (lcfData[pos + 9] << 16) | (lcfData[pos + 10] << 8) | lcfData[pos + 11];
        u32 extraLen = (lcfData[pos + 12] << 24) | (lcfData[pos + 13] << 16) | (lcfData[pos + 14] << 8) | lcfData[pos + 15];
        pos += 16;
        if (extraLen > 0)
            pos += (extraLen + 3) & ~3;

        if (strncmp(magic, ".TPG", 4) != 0) continue;
        if (offset + size > lcfSize) continue;
        LoadTPGTexturePayload(lcfData + offset, size);
    }

    // Upload raw VRAM as R16UI texture for shader-side palette lookup
    if (vramHandle) {
        p3d::context->DestroyVRAMTexture(vramHandle);
        vramHandle = 0;
    }
    vramHandle = p3d::context->CreateVRAMTexture(1024, 512, vram.data);
    LOG("[World] Uploaded raw VRAM as R16UI (1024x512, handle=%u)", vramHandle);
}

#if defined(RC_PLATFORM_PSP)
namespace {

static bool ReadPspStreamEntry(
    p3d::io::FileHandle& file,
    const tStreamEntry& entry,
    int64_t fileSize,
    std::vector<u8>& scratch) {
    if (entry.offset > static_cast<u64>(fileSize) ||
        entry.size > static_cast<u64>(fileSize) - entry.offset) {
        return false;
    }

    scratch.resize(entry.size);
    if (entry.size == 0) {
        return true;
    }

    if (!file.Seek(entry.offset, SEEK_SET)) {
        return false;
    }
    return file.Read(scratch.data(), scratch.size()) == scratch.size();
}

static bool OpenPspStreamDirectory(
    const std::string& lcfPath,
    p3d::io::FileHandle& file,
    int64_t& fileSize,
    std::vector<tStreamEntry>& entries) {
    if (!file.Open(p3d::io::ResolvePath(lcfPath), p3d::io::FileHandle::Mode::Read)) {
        return false;
    }

    fileSize = file.Size();
    if (fileSize < 4 || fileSize > INT32_MAX) {
        return false;
    }

    // Stream directory records are at the front of the file. Keep the header
    // bounded; payloads are fetched separately by offset below.
    const size_t headerBytes = std::min<size_t>(static_cast<size_t>(fileSize), 64 * 1024);
    std::vector<u8> header(headerBytes);
    if (file.Read(header.data(), header.size()) != header.size()) {
        return false;
    }

    entries = ParseStreamHeader(header.data(), static_cast<u32>(header.size()));
    return !entries.empty();
}

static bool FindPspPetalRange(
    const std::vector<tStreamEntry>& entries,
    u32 targetPetal,
    u32& outStart,
    u32& outEnd,
    u32& outPetal) {
    std::vector<u32> wdbIndices;
    wdbIndices.reserve(8);
    for (u32 i = 0; i < static_cast<u32>(entries.size()); i++) {
        if (strncmp(entries[i].magic, ".WDB", 4) == 0) {
            wdbIndices.push_back(i);
        }
    }
    if (wdbIndices.empty()) {
        return false;
    }

    outPetal = targetPetal < static_cast<u32>(wdbIndices.size()) ? targetPetal : 0;
    outStart = wdbIndices[outPetal];
    outEnd = (outPetal + 1 < static_cast<u32>(wdbIndices.size()))
        ? wdbIndices[outPetal + 1]
        : static_cast<u32>(entries.size());
    return true;
}

static bool LoadPspBlocksForPetal(
    BlockManager& blockMgr,
    p3d::io::FileHandle& file,
    int64_t fileSize,
    const std::vector<tStreamEntry>& entries,
    u32 petalStart,
    u32 petalEnd,
    u32 startBlockNum,
    std::vector<u8>& scratch,
    const char* logTag) {
    u32 blockIndex = 0;
    for (u32 i = petalStart; i < petalEnd; i++) {
        if (strncmp(entries[i].magic, ".BLK", 4) != 0) {
            continue;
        }

#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] blk begin entry=%lu block=%lu size=%lu free=%lu max=%lu",
            static_cast<unsigned long>(i), static_cast<unsigned long>(blockIndex),
            static_cast<unsigned long>(entries[i].size),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
        if (!ReadPspStreamEntry(file, entries[i], fileSize, scratch)) {
            LOG("[%s] Failed to read BLK entry %u", logTag, i);
            blockIndex++;
            continue;
        }
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] blk read entry=%lu bytes=%lu free=%lu max=%lu",
            static_cast<unsigned long>(i), static_cast<unsigned long>(scratch.size()),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
        blockMgr.LoadBlockAt(blockIndex, scratch.data(), static_cast<u32>(scratch.size()));
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] blk parsed entry=%lu block=%lu free=%lu max=%lu",
            static_cast<unsigned long>(i), static_cast<unsigned long>(blockIndex),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
        blockIndex++;
    }

    // Recompute the active/load lists after all individual BLK parses. The
    // zero-entry call updates manager state without requiring resident arrays.
    blockMgr.LoadBlocks(startBlockNum, nullptr, nullptr, 0);
    LOG("[%s] Parsed %u BLK entries for PSP petal", logTag, blockIndex);
    return blockIndex != 0;
}

} // namespace

bool World::LoadPspStream(const std::string& lcfPath) {
    if (!g_blockManager) {
        ASSERT(false);
        return false;
    }

    g_wEffectChunkCount = 0;
    g_particleSystemChunkCount = 0;
    streamPath = lcfPath;
    streamEntries.clear();
    streamData.clear();
    // Characters and UI share this VRAM with the level. Their permanent
    // textures are loaded before this stream; TPG uploads must only replace
    // their own rectangles, as in LoadStream. PsxVRAM starts zero-initialized.

    p3d::io::FileHandle file;
    int64_t fileSize = 0;
    LOG("[World] PSP stream directory open: %s", lcfPath.c_str());
    if (!OpenPspStreamDirectory(lcfPath, file, fileSize, streamEntries)) {
        LOG("[World] Failed to open/parse PSP stream: %s", lcfPath.c_str());
        streamPath.clear();
        streamEntries.clear();
        return false;
    }
    LOG("[World] PSP stream directory ready: %u entries, %lld bytes",
        static_cast<u32>(streamEntries.size()), static_cast<long long>(fileSize));
    PspPerf::Trace("psp stream: directory ready");

    FillMeter(20);

    // TPGs are independent payloads. Fetch and decode each one into the same
    // bounded scratch buffer, then create one persistent VRAM texture.
    for (const tStreamEntry& entry : streamEntries) {
        if (strncmp(entry.magic, ".TPG", 4) != 0) {
            continue;
        }
        if (!ReadPspStreamEntry(file, entry, fileSize, pspStreamScratch)) {
            LOG("[World] Failed to read TPG entry");
            continue;
        }
        LoadTPGTexturePayload(pspStreamScratch.data(), static_cast<u32>(pspStreamScratch.size()));
    }
    RefreshVRAMTexture();
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP VRAM] LoadPspStream after initial TPG handle=%lu",
        static_cast<unsigned long>(vramHandle));
#endif
    FillMeter(40);
    PspPerf::Trace("psp stream: tpg ready");
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] tpg ready entries=%lu free=%lu max=%lu",
        static_cast<unsigned long>(streamEntries.size()),
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif

    // RCI/RCP resources are level-wide and must be loaded before the selected
    // petal, exactly as on the resident desktop path.
    for (u32 i = 0; i + 1 < static_cast<u32>(streamEntries.size()); i++) {
        if (strncmp(streamEntries[i].magic, ".RCI", 4) != 0) {
            continue;
        }
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] rci pair begin index=%lu perm=%lu next=%s/%lu",
            static_cast<unsigned long>(i),
            static_cast<unsigned long>(streamEntries[i].size),
            streamEntries[i + 1].magic,
            static_cast<unsigned long>(streamEntries[i + 1].size));
#endif
        const tStreamEntry& p3dEntry = streamEntries[i + 1];
        if (!ReadPspStreamEntry(file, streamEntries[i], fileSize, pspPermScratch) ||
            !ReadPspStreamEntry(file, p3dEntry, fileSize, pspP3dScratch)) {
            LOG("[World] Failed to read RCI/RCP pair %u", i);
            continue;
        }
            LoadGeoPair(this,
                        pspPermScratch.data(), static_cast<u32>(pspPermScratch.size()),
                        pspP3dScratch.data(), static_cast<u32>(pspP3dScratch.size()), 1);
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] rci pair end index=%lu free=%lu max=%lu",
            static_cast<unsigned long>(i),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    }
    PspPerf::Trace("psp stream: permanent geometry ready");
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] rci complete free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif

    u32 petalStart = 0, petalEnd = 0, petalIdx = 0;
    if (!FindPspPetalRange(streamEntries, targetPetalIndex, petalStart, petalEnd, petalIdx)) {
        LOG("[World] No WDB entries in %s", lcfPath.c_str());
        return false;
    }
    targetPetalIndex = petalIdx;
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] petal range start=%lu end=%lu petal=%lu free=%lu max=%lu",
        static_cast<unsigned long>(petalStart), static_cast<unsigned long>(petalEnd),
        static_cast<unsigned long>(petalIdx),
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    LOG("[World] PSP loading petal %u: entries %u-%u", petalIdx, petalStart, petalEnd - 1);

    BlockManager& blockMgr = *g_blockManager;
    g_database->Close();
    for (u32 i = petalStart; i < petalEnd; i++) {
        if (strncmp(streamEntries[i].magic, ".WDB", 4) != 0) {
            continue;
        }
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] wdb begin index=%lu size=%lu free=%lu max=%lu",
            static_cast<unsigned long>(i),
            static_cast<unsigned long>(streamEntries[i].size),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
        if (!ReadPspStreamEntry(file, streamEntries[i], fileSize, pspStreamScratch)) {
            LOG("[World] Failed to read WDB entry %u", i);
            continue;
        }
        g_database->Scan(pspStreamScratch.data(), static_cast<u32>(pspStreamScratch.size()));
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] wdb end index=%lu free=%lu max=%lu",
            static_cast<unsigned long>(i),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    }
    PspPerf::Trace("psp stream: database ready");
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] database ready free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif

    for (u32 i = petalStart; i + 1 < petalEnd; i++) {
        if (strncmp(streamEntries[i].magic, ".PCI", 4) != 0) {
            continue;
        }
        const tStreamEntry& p3dEntry = streamEntries[i + 1];
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] pci pair begin index=%lu perm=%lu p3d=%lu free=%lu max=%lu",
            static_cast<unsigned long>(i),
            static_cast<unsigned long>(streamEntries[i].size),
            static_cast<unsigned long>(p3dEntry.size),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
        if (!ReadPspStreamEntry(file, streamEntries[i], fileSize, pspPermScratch) ||
            !ReadPspStreamEntry(file, p3dEntry, fileSize, pspP3dScratch)) {
            LOG("[World] Failed to read PCI/PCP pair %u", i);
            continue;
        }
        LoadGeoPair(this,
                    pspPermScratch.data(), static_cast<u32>(pspPermScratch.size()),
                    pspP3dScratch.data(), static_cast<u32>(pspP3dScratch.size()), 2);
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] pci pair end index=%lu free=%lu max=%lu",
            static_cast<unsigned long>(i),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    }
    PspPerf::Trace("psp stream: petal geometry ready");
    RefreshVRAMTexture();
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] petal geometry ready free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif

    std::vector<DBVolume*> blockVolumes;
    for (DBRoot* v = g_database->GetFirstBlock(); v; v = static_cast<DBRoot*>(v->next)) {
        blockVolumes.push_back(static_cast<DBVolume*>(v));
    }
    blockMgr.LoadBlocksFunc(blockVolumes);
    PspPerf::Trace("psp stream: block metadata ready");
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] block metadata ready count=%lu free=%lu max=%lu",
        static_cast<unsigned long>(blockMgr.GetNumBlocks()),
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif

    s32 minX = 0x7FFFFFFF, minY = 0x7FFFFFFF, minZ = 0x7FFFFFFF;
    s32 maxX = -0x7FFFFFFF, maxY = -0x7FFFFFFF, maxZ = -0x7FFFFFFF;
    for (u32 i = 0; i < blockMgr.GetNumBlocks(); i++) {
        Block* b = blockMgr.GetBlock(i);
        if (!b) continue;
        s32 bMinX = b->posX + b->halfExtNegX, bMaxX = b->posX + b->halfExtPosX;
        s32 bMinY = b->posY + b->halfExtNegY, bMaxY = b->posY + b->halfExtPosY;
        s32 bMinZ = b->posZ + b->halfExtNegZ, bMaxZ = b->posZ + b->halfExtPosZ;
        if (bMinX < minX) minX = bMinX;
        if (bMaxX > maxX) maxX = bMaxX;
        if (bMinY < minY) minY = bMinY;
        if (bMaxY > maxY) maxY = bMaxY;
        if (bMinZ < minZ) minZ = bMinZ;
        if (bMaxZ > maxZ) maxZ = bMaxZ;
    }
    levelMin = { minX, minY, minZ };
    levelMax = { maxX, maxY, maxZ };

    // The directory stays resident, but payload scratch must not remain
    // reserved while AI creates level entities. Re-acquire it on demand for
    // deferred BLK parsing and petal transitions; this returns roughly 300 KB
    // to the PSP heap before the first gameplay population pass.
    std::vector<u8>().swap(pspStreamScratch);
    std::vector<u8>().swap(pspPermScratch);
    std::vector<u8>().swap(pspP3dScratch);
    for (ccList& list : switchLists) {
        list.head = nullptr;
        list.tail = nullptr;
    }
    PspPerf::Trace("psp stream: scratch released");
    FillMeter(65);
#if defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("psp stream: memory query begin");
    LOG("[World] PSP heap after stream: free=%u max=%u",
        (u32)sceKernelTotalFreeMemSize(), (u32)sceKernelMaxFreeMemSize());
    PspPerf::Trace("psp stream: memory query done");
#endif
    LOG("[World] PSP stream metadata load complete: blocks=%u", blockMgr.GetNumBlocks());
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] stream return blocks=%lu free=%lu max=%lu",
        static_cast<unsigned long>(blockMgr.GetNumBlocks()),
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    return blockMgr.GetNumBlocks() > 0;
}

void World::LoadPspPetal(u32 petalIndex) {
    MARKFUNCTION(0x8004604C);

    if (levelList && currentLevelIndex < static_cast<u32>(levelCount) &&
        levelList[currentLevelIndex * 2] == 7) {
        previousLevelIndex = currentLevelIndex;
    }

    StartLogo("RUNFIRST.TIM");

    const s32 petalCount = GetCurLevelPetals();
    if (petalCount <= 0 || petalIndex >= static_cast<u32>(petalCount)) {
        petalIndex = 0;
    }

    if (petalSoundIDs && currentLevelIndex < static_cast<u32>(levelCount)) {
        const s32 soundLocation = static_cast<s32>(petalSoundIDs[currentLevelIndex][petalIndex]) - 1;
        rsEvent(RS_SET_LOCATION, soundLocation, 0, 0);
    }

    targetPetalIndex = petalIndex;
    currentPetalIndex = petalIndex;

    if (!g_blockManager || !g_database || streamPath.empty() || streamEntries.empty()) {
        StopLogo();
        return;
    }

    BlockManager& blockMgr = *g_blockManager;
    blockMgr.SetDeathVolumeFlag(1);
    g_wEffectChunkCount = 0;
    g_particleSystemChunkCount = 0;

    if (g_scoreManager) {
        if (Player::s_player && Player::s_player->checkpoint.IsValid()) {
            g_scoreManager->HandleCheckpointBegin();
        }
        else {
            g_scoreManager->HandleLevelBegin();
        }
    }
    if (g_hud) {
        g_hud->OnLoadLevel();
    }

    p3d::io::FileHandle file;
    const bool opened = file.Open(p3d::io::ResolvePath(streamPath),
                                  p3d::io::FileHandle::Mode::Read);
    const int64_t fileSize = opened ? file.Size() : 0;
    u32 petalStart = 0, petalEnd = 0, resolvedPetal = 0;
    if (!opened || !FindPspPetalRange(streamEntries, petalIndex,
                                      petalStart, petalEnd, resolvedPetal)) {
        StopLogo();
        return;
    }

    petalIndex = resolvedPetal;
    targetPetalIndex = resolvedPetal;
    currentPetalIndex = resolvedPetal;

    // Rebuild only the selected petal's database and geometry. Payloads are
    // fetched one at a time into bounded scratch buffers, matching the PSP
    // level-load memory budget used by LoadPspStream.
    PurgeSwitches();
    g_database->Close();
    for (u32 i = petalStart; i < petalEnd; i++) {
        if (strncmp(streamEntries[i].magic, ".WDB", 4) != 0) {
            continue;
        }
        if (!ReadPspStreamEntry(file, streamEntries[i], fileSize, pspStreamScratch)) {
            continue;
        }
        g_database->Scan(pspStreamScratch.data(), static_cast<u32>(pspStreamScratch.size()));
    }

    for (u32 i = petalStart; i + 1 < petalEnd; i++) {
        if (strncmp(streamEntries[i].magic, ".PCI", 4) != 0) {
            continue;
        }
        const tStreamEntry& p3dEntry = streamEntries[i + 1];
        if (!ReadPspStreamEntry(file, streamEntries[i], fileSize, pspPermScratch) ||
            !ReadPspStreamEntry(file, p3dEntry, fileSize, pspP3dScratch)) {
            continue;
        }
        LoadGeoPair(this,
                    pspPermScratch.data(), static_cast<u32>(pspPermScratch.size()),
                    pspP3dScratch.data(), static_cast<u32>(pspP3dScratch.size()), 2);
    }

    WEffect_InitWorldEffects(g_database->GetFirstPoint());
    PWEffect_InitWorldEffects(g_database->GetFirstPoint());
    ParticleSystem_InitParticleInfoMemory();

    std::vector<DBVolume*> blockVolumes;
    blockVolumes.reserve(blockMgr.GetNumBlocks());
    for (DBRoot* v = g_database->GetFirstBlock(); v;
         v = static_cast<DBRoot*>(v->next)) {
        blockVolumes.push_back(static_cast<DBVolume*>(v));
    }
    blockMgr.LoadBlocksFunc(blockVolumes);

    std::vector<u8>().swap(pspStreamScratch);
    std::vector<u8>().swap(pspPermScratch);
    std::vector<u8>().swap(pspP3dScratch);
    for (ccList& list : switchLists) {
        list.head = nullptr;
        list.tail = nullptr;
    }
    FillMeter(40);

    if (g_ai) {
        g_ai->Populate();
    }
    FillMeter(85);

    u32 startBlockNum = 0;
    if (Player::s_player) {
        startBlockNum = Player::s_player->blockNum;
    }
    if (file.Seek(0, SEEK_SET)) {
        LoadPspBlocksForPetal(blockMgr, file, fileSize, streamEntries,
                              petalStart, petalEnd, startBlockNum,
                              pspStreamScratch, "World::LoadPetal");
    }

    if (g_ai) {
        g_ai->PopulateBlock();
    }
    WEffect_PopulateWEffects();
    FillMeter(100);
    RefreshVRAMTexture();

    if (g_director) {
        g_director->Reset();
        g_director->SetScript();
    }
    ProcessSwitches();
    if (g_cameraManager) {
        g_cameraManager->SetupPaths();
    }
    SyncSwitchLoadGroup(0);
    SyncSwitchLoadGroup(1);
    rsEvent(RS_LEVEL_BEGIN, 0, 0, 0);
    StopLogo();
    if (g_director) {
        g_director->SkipNextWideScreenEnterAnim();
    }
}
#endif

World::World() {
    // Keep intrusive switch lists explicitly empty on PSP. The target's
    // low-memory allocator can return patterned blocks; relying only on the
    // aggregate's implicit initialization made stale list pointers visible
    // after the streaming load.
    for (ccList& list : switchLists) {
        list.head = nullptr;
        list.tail = nullptr;
    }
    p3d::rawTextureUploader = UploadRawTextureToWorldVRAM;
}

World::~World() {
    if (p3d::rawTextureUploader == UploadRawTextureToWorldVRAM) {
        p3d::rawTextureUploader = nullptr;
    }

    Unload();
    // Free level table data
    if (levelList) { delete[] levelList; levelList = nullptr; }
    if (highestPetal) { delete[] highestPetal; highestPetal = nullptr; }
    if (levelNames) {
        for (s32 i = 0; i < levelCount; i++)
            delete[] levelNames[i];
        delete[] levelNames;
        levelNames = nullptr;
    }
    if (petalNames) {
        for (s32 i = 0; i < levelCount; i++) {
            if (petalNames[i]) {
                s32 pc = levelList ? levelList[i * 2 + 1] : 0;
                for (s32 j = 0; j <= pc; j++)
                    delete[] petalNames[i][j];
                delete[] petalNames[i];
            }
        }
        delete[] petalNames;
        petalNames = nullptr;
    }
    if (petalSoundIDs) {
        for (s32 i = 0; i < levelCount; i++)
            delete[] petalSoundIDs[i];
        delete[] petalSoundIDs;
        petalSoundIDs = nullptr;
    }
}

void World::PurgeSwitches() {
    ClearPendingCharModelLoads();

    for (ccList& list : switchLists) {
        while (ccMinNode* node = list.RemHead()) {
            delete node;
        }
    }
}

void World::ProcessPendingSwitchActions() {
    if (g_pendingCharModelLoads.empty()) {
        return;
    }

    for (const PendingCharModelLoad& request : g_pendingCharModelLoads) {
        SwitchUnloadLoadCharModel(request.oldType, request.newType);
    }

    g_pendingCharModelLoads.clear();
}

void World::ProcessSwitches() {
    LOG("[World] ProcessSwitches: enter");
#if !defined(RC_PLATFORM_PSP)
    PurgeSwitches();
#endif
    LOG("[World] ProcessSwitches: purge complete");

    if (!g_database) {
        LOG("[World] ProcessSwitches: no database");
        return;
    }

    u32 switchCount = 0;
    DBRoot* firstVolume = static_cast<DBRoot*>(g_database->GetFirstVolume());
    LOG("[World] ProcessSwitches: first volume=%p", (void*)firstVolume);
    for (DBRoot* root = firstVolume; root;
         root = static_cast<DBRoot*>(root->next)) {
        if (root->type != 9 || root->subType != 0x9B) {
            continue;
        }

        LOG("[World] ProcessSwitches: setup #%u crc=0x%08X", switchCount, root->nameCRC);
        WDBVolumeSwitch* sw = new WDBVolumeSwitch();
        const bool setupOk = sw->Setup(root);
        const bool bindOk = setupOk && sw->Bind(ResolveSwitchGameFuncByName);
        LOG("[World] ProcessSwitches: setup #%u result setup=%d bind=%d",
            switchCount, setupOk ? 1 : 0, bindOk ? 1 : 0);
        switchCount++;
        if (!setupOk || !bindOk) {
            delete sw;
            continue;
        }

        LOG("[World] ProcessSwitches: set volume #%u", switchCount - 1);
        sw->SetVolume(static_cast<DBVolume*>(root));

        if (sw->listBucket < 4) {
            LOG("[World] ProcessSwitches: add bucket=%u", sw->listBucket);
            LOG("[World] ProcessSwitches: list head=%p tail=%p",
                (void*)switchLists[sw->listBucket].head,
                (void*)switchLists[sw->listBucket].tail);
            switchLists[sw->listBucket].AddNode(switchLists[sw->listBucket].tail, sw);
            LOG("[World] ProcessSwitches: add complete");
        }
        else {
            LOG("[World] ProcessSwitches: discard bucket=%u", sw->listBucket);
            delete sw;
        }
    }
    LOG("[World] ProcessSwitches: complete count=%u", switchCount);
}

void World::CheckSwitchList(ccList& list, Thing* thing) {
    if (!thing) {
        return;
    }

    for (ccMinNode* node = list.head; node;) {
        WDBSwitch* sw = static_cast<WDBSwitch*>(node);
        node = node->next;

        const bool inside = sw->IsInside(thing->pos);

        if (inside) {
            sw->Execute(thing);

            if (sw->persistent != 0) {
                list.RemNode(sw);
                delete sw;
            }
        }
        else {
            sw->Reject(thing);
        }
    }
}

void World::CheckThingSwitches(Thing* thing) {
    if (!thing) {
        return;
    }

    CheckSwitchList(switchLists[0], thing);
    CheckSwitchList(switchLists[3], thing);

    u32 bucket = (thing->thingType != 0) ? 2u : 1u;
    CheckSwitchList(switchLists[bucket], thing);
}

WDBSwitch* World::FindPrimarySwitchByCRC(u32 crc) {
    // PSX Platform path-node attrib 11 uses FindNodeCRC(theWorldMgr + 0x6C, crc, 0).
    ccNode* node = switchLists[0].FindNodeCRC(crc, nullptr);
    return node ? static_cast<WDBSwitch*>(node) : nullptr;
}

// Simple tokenizer matching PSX GetNextToken__4Game
static bool GetNextToken(char* out, char** cursor, const char* delims) {
    char* p = *cursor;
    while (*p && strchr(delims, *p))
        p++;
    if (!*p) {
        *out = '\0';
        return false;
    }
    char* dst = out;
    while (*p && !strchr(delims, *p))
        *dst++ = *p++;
    *dst = '\0';
    *cursor = p;
    return true;
}

// PSX: LoadLevelNames__5World (WORLD.CPP:990, 0x80045700)
void World::LoadLevelNames() {
    // Local arrays matching PSX stack layout (max 16 levels, 16 petals each)
    char* tmpLevNames[16] = {};
    s32 tmpLevIDs[16] = {};
    s32 tmpPetalIdx[16][16] = {};
    s32 tmpSoundBytes[16][16] = {};
    char* tmpPetalNames[16][16] = {};
    s32 tmpPetalCounts[16] = {};

    // Read RTARGET/GAME_LN.TXT
    char filename[128];
    std::snprintf(filename, sizeof(filename), "RTARGET/GAME_LN.TXT");

    auto fileContent = p3d::io::ReadTextFile(p3d::io::ResolvePath(filename));
    if (!fileContent)
        return;

    std::string content = std::move(*fileContent);

    levelCount = 0;
    char* cursor = content.data();
    char token[128];
    s32 levIdx = -1;
    s32 petalSeq = -1;

    // PSX: tokenize with " \r\n\t", matching the comma-operator ++v4 pattern
    while (GetNextToken(token, &cursor, " \r\n\t")) {
        if (token[0] == 'L' || token[0] == 'l') {
            // Level line: "levNN"
            ++levIdx;
            tmpLevIDs[levIdx] = atoi(token + 3);
            GetNextToken(token, &cursor, "\r\n");
            s32 len = (s32)strlen(token);
            tmpLevNames[levIdx] = new char[len + 1];
            memcpy(tmpLevNames[levIdx], token, len + 1);
            petalSeq = -1;
            ++levelCount;
        }
        else {
            // Petal line: <index> <soundByte> <name>
            ++petalSeq;
            tmpPetalIdx[levIdx][petalSeq] = atoi(token);
            GetNextToken(token, &cursor, " \t");
            tmpSoundBytes[levIdx][petalSeq] = atoi(token);
            GetNextToken(token, &cursor, "\r\n");
            s32 len = (s32)strlen(token);
            tmpPetalNames[levIdx][petalSeq] = new char[len + 1];
            memcpy(tmpPetalNames[levIdx][petalSeq], token, len + 1);
            ++tmpPetalCounts[levIdx];
        }
    }

    // Build levelList: pairs of {levelID, petalCount}
    levelList = new s32[levelCount * 2];
    highestPetal = new s32[levelCount];
    for (s32 i = 0; i < levelCount; i++) {
        levelList[i * 2] = tmpLevIDs[i];
        levelList[i * 2 + 1] = tmpPetalCounts[i];
    }

    // Build levelNames
    levelNames = new char* [levelCount + 1];
    levelNames[levelCount] = nullptr;

    // Build petalNames sub-arrays (null-initialized)
    petalNames = new char** [levelCount + 1];
    petalNames[levelCount] = nullptr;
    for (s32 i = 0; i <= levelCount; i++) {
        if (i == levelCount) break;
        s32 pc = tmpPetalCounts[i];
        petalNames[i] = new char* [pc + 1];
        for (s32 j = 0; j <= pc; j++)
            petalNames[i][j] = nullptr;
    }

    // Copy level names, petal names, and compute highestPetal
    for (s32 i = 0; i < levelCount; i++) {
        s32 len = (s32)strlen(tmpLevNames[i]);
        levelNames[i] = new char[len + 1];
        memcpy(levelNames[i], tmpLevNames[i], len + 1);
        delete[] tmpLevNames[i];

        s32 pc = tmpPetalCounts[i];
        for (s32 j = 0; j < pc; j++) {
            s32 pidx = tmpPetalIdx[i][j];
            s32 nlen = (s32)strlen(tmpPetalNames[i][j]);
            petalNames[i][pidx] = new char[nlen + 1];
            memcpy(petalNames[i][pidx], tmpPetalNames[i][j], nlen + 1);
            delete[] tmpPetalNames[i][j];
        }

        highestPetal[i] = tmpPetalIdx[i][pc - 1];
    }

    // Build petalSoundIDs (u8 arrays, sequential order)
    petalSoundIDs = new u8 * [levelCount];
    for (s32 i = 0; i < levelCount; i++) {
        s32 pc = tmpPetalCounts[i];
        petalSoundIDs[i] = new u8[pc];
        for (s32 j = 0; j < pc; j++)
            petalSoundIDs[i][j] = (u8)tmpSoundBytes[i][j];
    }
}

// PSX: LoadPermanent__5World (WORLD.CPP:1062, 0x80045D6C)
void World::LoadPermanent() {
    LOG("[World] LoadPermanent: level names begin");
    LoadLevelNames();
    LOG("[World] LoadPermanent: level names complete (%d levels)", levelCount);

    // PSX: LoadLevel__12LevelManager() - no-op on PSX
    // PSX: PurgeLevelP3DInventory__12LevelManager() - also no-op

    // PSX: OpenCharacter(type=0), EnableCache(type=0, 1)
    if (g_characterManager) {
        LOG("[World] LoadPermanent: character open");
        g_characterManager->OpenCharacter(0);
        LOG("[World] LoadPermanent: character open complete");
        g_characterManager->EnableCache(0, 1);
        LOG("[World] LoadPermanent: character cache enabled");

        // PSX: allocate CharMgrCallback, LoadCharacter(type=0, callback), spin until done
        CharMgrCallback* callback = new CharMgrCallback();
        LOG("[World] LoadPermanent: character load begin");
        g_characterManager->LoadCharacter(0, callback);
        // PSX spins: while (!callback->done) rDoTaskList(rMainTaskList, 0);
        // PC: LoadCharacter is synchronous, callback already fired

        // PSX: LoadAnimation(type=0, animEnum=0, count=124, callback), spin until done
        callback->done = 0;
        g_characterManager->LoadAnimation(0, 0, 124, callback);
        LOG("[World] LoadPermanent: character animations first batch complete");
        // PSX spins again - PC is synchronous

        // PSX immediately issues the same load again without waiting. This keeps
        // a second reference on the core 0..123 player set.
        g_characterManager->LoadAnimation(0, 0, 124, nullptr);
        LOG("[World] LoadPermanent: character animations second batch complete");

        // PSX: EnableCache(type=0, 0), delete callback
        g_characterManager->EnableCache(0, 0);
        delete callback;
        LOG("[World] LoadPermanent: character complete");
    }

    // PSX: AddThingNoTagList("Jackie", 0, {0,0,0}, {0,0,0}, "JACKIELOHIER", nullptr)
    if (g_ai) {
        LOG("[World] LoadPermanent: add Jackie begin");
        LVector zeroPos = { 0, 0, 0 };
        SVector zeroOrient = { 0, 0, 0 };
        g_ai->AddThingNoTagList("Jackie", 0, &zeroPos, &zeroOrient, "JACKIELOHIER", nullptr);
        LOG("[World] LoadPermanent: add Jackie complete");
    }
}

// PSX: LoadLevel__5WorldUl (WORLD.CPP:1389, 0x8004624C)
bool World::LoadLevelIndex(u32 levelIndex) {
    MARKFUNCTION(0x8004624C);

    // PSX: clamp levelIndex to valid range
    if (levelCount > 0 && levelIndex >= (u32)levelCount)
        levelIndex = (u32)(levelCount - 1);

    u32 prevLevel = currentLevelIndex;
    currentLevelIndex = levelIndex;
    previousLevelIndex = prevLevel;

    targetLevelIndex = levelIndex;

    // PSX callers keep target petal in-range for the destination level.
    // Clamp here so subsequent sound/block indexing stays consistent.
    u32 levelPetalCount = 1;
    if (levelList && levelIndex < (u32)levelCount) {
        s32 count = levelList[levelIndex * 2 + 1];
        if (count > 0) {
            levelPetalCount = (u32)count;
        }
    }
    if (targetPetalIndex >= levelPetalCount) {
        targetPetalIndex = 0;
    }

    // PSX: EstimateLoadTime, StartLogo, FillMeter(100)
    StartLogo("RUNFIRST.TIM");

    // PSX: rSPrintf(v8, "%slev%02d.lcf", "RTARGET\\", levelList[levelIndex * 2])
    char levelPath[64];
    s32 levNum = (levelList && levelCount > 0) ? levelList[levelIndex * 2] : (s32)(levelIndex + 1);
    std::snprintf(levelPath, sizeof(levelPath), "RTARGET/LEV%02d.LCF", levNum);

    // PSX sets the sound location before opening/loading the level stream.
    if (petalSoundIDs && levelIndex < (u32)levelCount) {
        s32 soundLocation = (s32)petalSoundIDs[levelIndex][targetPetalIndex] - 1;
        rsEvent(RS_SET_LOCATION, soundLocation, 0, 0);
    }

    if (!Load(levelPath)) {
        StopLogo();
        return false;
    }
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] LoadLevelIndex: stream load returned free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    PspPerf::Trace("level index: stream load returned");

    PumpLoadingScreen();
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] LoadLevelIndex: loading pump returned free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif

    currentPetalIndex = targetPetalIndex;

    // Level-begin stats must reset before Populate registers collectibles.
    if (g_scoreManager) {
        g_scoreManager->HandleLevelBegin();
    }

    if (g_hud) {
        g_hud->OnLoadLevel();
    }

    // PSX: ExecuteLoadCallbacks -> cameraLoadFunc -> SetupPaths
    if (g_cameraManager) {
        g_cameraManager->SetupPaths();
    }

    // PSX: Construct__5World (WORLD.CPP:1399, 0x80046E80)
    // On PSX this is a separate function called after LoadLevel.
    // It initializes fighting collision, effects, populates AI entities,
    // loads backgrounds, resets Director, and sets up the level script.
    // We inline the steps we can handle here.

    if (!g_blockManager) {
        ASSERT(false);
        StopLogo();
        return false;
    }
    BlockManager& blockMgr = *g_blockManager;

    blockMgr.SetDeathVolumeFlag(1);

    // PSX: Init__17FightingCollision, InsertHumanoid (player)
    FightingCollision::Init();
    if (Player::s_player) {
        FightingCollision::InsertHumanoid(static_cast<Humanoid*>(Player::s_player));
    }

    // PSX: CheckpointInfo
    u32 startBlockNum = 0;
    bool hasCheckpoint = false;
    if (Player::s_player && Player::s_player->checkpoint.IsValid()) {
        startBlockNum = (u32)Player::s_player->checkpoint.field24;
        hasCheckpoint = true;
        if (g_scoreManager) {
            g_scoreManager->HandleCheckpointBegin();
        }
    }

    // PSX: InitWorldEffects__7WEffectP7DBPoint
    if (g_database) {
        WEffect_InitWorldEffects(g_database->GetFirstPoint());
        PWEffect_InitWorldEffects(g_database->GetFirstPoint());
    }

    ParticleSystem_InitParticleInfoMemory();

    // PSX: Populate__2AI(0) - spawn entities from WDB database
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] Construct: before AI populate free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    if (g_ai) {
        g_ai->Populate();
    }
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] Construct: after AI populate free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    PspPerf::Trace("level index: ai populated");
    LOG("[World] Construct: AI populate complete");
    FillMeter(85);

    // PSX: v5 = player->blockNum (after Populate sets it from attrib 15)
    u16 playerBlockNum = 0x1000;
    if (Player::s_player) {
        playerBlockNum = Player::s_player->blockNum;
    }

    // PSX: if no checkpoint, start block = player's block
    if (!hasCheckpoint) {
        startBlockNum = playerBlockNum;
    }

    // PSX: LoadBG, InitBG - background rendering setup
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] Construct: before background free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    BackG::LoadBG();
    LOG("[World] Construct: background load complete");
    BackG::InitBG();
    LOG("[World] Construct: background init complete");
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] Construct: after background free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif

    // PSX: ScoreManager::SetPar
    if (g_scoreManager) {
        g_scoreManager->SetPar();
    }
    LOG("[World] Construct: score setup complete");

    // PSX: Director->Reset() then Director->SetScript()
    if (g_director) {
        g_director->Reset();
        g_director->SetScript();
    }
    LOG("[World] Construct: director setup complete");

    if (g_environmentManager && g_levelManager) {
        LOG("[World] Construct: ambient setup begin");
        g_environmentManager->SetupModelAmbientLighting(&g_levelManager->modelLists[0]);
        LOG("[World] Construct: ambient setup complete");
    }

    // PSX: SetupModelAmbientLighting, ProcessSwitches
    LOG("[World] Construct: switches begin");
    ProcessSwitches();
    PspPerf::Trace("level index: switches processed");
    LOG("[World] Construct: switches processed");

    // PSX: Close__8Database(0)
    if (g_database) {
        g_database->Close();
    }
    LOG("[World] Construct: database closed");

    // PSX: AllocBlockPool__12BlockManager(0) - allocate block node pool
    // PC: blocks already allocated by LoadBlocksFunc

    // PSX: LoadBlocks__12BlockManagerUl(0, startBlockNum)
    // On PC we deferred Parse() in Load() and do it here to match PSX timing.
#if defined(RC_PLATFORM_PSP)
    {
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] Construct: before block load free=%lu max=%lu",
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
        p3d::io::FileHandle pspStreamFile;
        const bool pspStreamOpen = pspStreamFile.Open(
            p3d::io::ResolvePath(streamPath), p3d::io::FileHandle::Mode::Read);
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] Construct: block file opened=%u size=%ld free=%lu max=%lu",
            pspStreamOpen ? 1u : 0u,
            static_cast<long>(pspStreamOpen ? pspStreamFile.Size() : 0),
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
        if (pspStreamOpen) {
            u32 pspPetalStart = 0, pspPetalEnd = 0, pspPetal = 0;
            if (FindPspPetalRange(streamEntries, currentPetalIndex,
                                  pspPetalStart, pspPetalEnd, pspPetal)) {
#if defined(PSP_GU_DIAGNOSTIC)
                Log::Get().LogMessage("[PSP LOAD] Construct: block range start=%lu end=%lu petal=%lu",
                    static_cast<unsigned long>(pspPetalStart),
                    static_cast<unsigned long>(pspPetalEnd),
                    static_cast<unsigned long>(pspPetal));
#endif
                LoadPspBlocksForPetal(blockMgr, pspStreamFile, pspStreamFile.Size(),
                                      streamEntries, pspPetalStart, pspPetalEnd,
                                      startBlockNum, pspStreamScratch, "World");
                PspPerf::Trace("level index: blocks loaded");
            }
        }
#if defined(PSP_GU_DIAGNOSTIC)
        Log::Get().LogMessage("[PSP LOAD] Construct: after block load free=%lu max=%lu",
            static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
            static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    }
#else
    LoadBlocksForPetalFromStream(blockMgr, streamData, currentPetalIndex, startBlockNum, "World");
#endif

    // PopulateBlock is called by LoadBlocks on PSX. On PC we call it explicitly.
    if (g_ai) {
        g_ai->PopulateBlock();
    }
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] Construct: after block populate free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    PspPerf::Trace("level index: block population done");

    WEffect_PopulateWEffects();
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] Construct: after effects free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    PspPerf::Trace("level index: effects populated");

    FillMeter(100);
    PspPerf::Trace("level index: meter complete");

    // PSX: if (IsValidBlockNumber(playerBlockNum) == 4096) -> reposition player
    // PSX returns 0x1000 (4096) when block is NOT valid.
    if (Player::s_player && g_blockManager) {
        if (!g_blockManager->IsValidBlockNumber(playerBlockNum)) {
            // PSX: get first loaded block position, add 2048 to Y
            Block* firstBlock = g_blockManager->GetBlock(0);
            if (firstBlock) {
                Player::s_player->pos.x = firstBlock->posX;
                Player::s_player->pos.y = firstBlock->posY + 2048;
                Player::s_player->pos.z = firstBlock->posZ;
                Player::s_player->homePos = Player::s_player->pos;
                LOG("[World] Player blockNum %u invalid, repositioned to block 0 (%d,%d,%d)",
                    playerBlockNum, firstBlock->posX, firstBlock->posY + 2048, firstBlock->posZ);
            }
        }
    }

    // PSX hub return flow: when current level ID == 7, apply saved return position
    if (levelList && currentLevelIndex < (u32)levelCount) {
        if (levelList[currentLevelIndex * 2] == 7 && Player::s_player) {
            Player* player = Player::s_player;

            bool mappedPrevLevel = false;
            if (previousLevelIndex < (u32)levelCount) {
                const s32 prevLevelID = levelList[previousLevelIndex * 2];
                switch (prevLevelID) {
                    case 6:
                        mappedPrevLevel = true;
                        break;
                    case 8:
                        previousLevelIndex = (u32)LevelIDToIndex(5);
                        mappedPrevLevel = true;
                        break;
                    case 11:
                        previousLevelIndex = (u32)LevelIDToIndex(1);
                        mappedPrevLevel = true;
                        break;
                    case 12:
                        previousLevelIndex = (u32)LevelIDToIndex(2);
                        mappedPrevLevel = true;
                        break;
                    case 13:
                        previousLevelIndex = (u32)LevelIDToIndex(3);
                        mappedPrevLevel = true;
                        break;
                    case 14:
                        previousLevelIndex = (u32)LevelIDToIndex(4);
                        mappedPrevLevel = true;
                        break;
                    default:
                        break;
                }
            }

            if (g_hud) {
                g_hud->ShowDestLevel();
            }

            // PSX: if previousLevelIndex >= levelCount, save player pos as original return pos
            static LVector sOrigDestSelectReturnPos = {};
            if (previousLevelIndex >= (u32)levelCount) {
                sOrigDestSelectReturnPos = player->homePos;
            }

            // PSX: determine if we should show level selection
            bool doShowLevel = false;
            if (previousLevelIndex < (u32)levelCount
                && previousLevelIndex != currentLevelIndex
                && (!mappedPrevLevel || !g_game || g_game->GetState() != GameState::EndLevelExit)) {
                doShowLevel = true;
            }

            LVector returnPos;
            if (doShowLevel) {
                returnPos = g_destSelectReturnPos;
                if (g_hud) {
                    g_hud->destSelect.ShowLevel(0);
                    if (previousLevelIndex < (u32)levelCount) {
                        s32 prevLevelID = levelList[previousLevelIndex * 2];
                        g_hud->destSelect.ShowLevel(prevLevelID);
                    }
                }
                g_arrowInside = 1;
            }
            else {
                returnPos = sOrigDestSelectReturnPos;
                if (g_hud) {
                    g_hud->DisplayTake(player->livesLeft, 1);
                }
                g_arrowInside = 0;
            }

            player->homePos = returnPos;
            player->pos = returnPos;

            if (g_display) {
                Camera* cam = g_display->GetCamera();
                if (cam) {
                    // PSX: SetLookAtTarget(theCamera, thePlayer, 1)
                    cam->SetLookAtTarget(player, 1);
                }
            }
        }
    }
    PspPerf::Trace("level index: return position complete");

    // PSX: gSyncLoadGroup(0); gSyncLoadGroup(1)
    SyncSwitchLoadGroup(0);
    SyncSwitchLoadGroup(1);
    PspPerf::Trace("level index: sync groups complete");

    // PSX: rsEvent(5, 0, 0, 0) - start music for current location
    PspPerf::Trace("level index: music begin");
    rsEvent(RS_LEVEL_BEGIN, 0, 0, 0);
    PspPerf::Trace("level index: music done");

    // PSX: StopLogo after load completes
    PspPerf::Trace("level index: logo stop begin");
    StopLogo();
    PspPerf::Trace("level index: logo stop done");
    if (g_director) {
        g_director->SkipNextWideScreenEnterAnim();
    }
    PspPerf::Trace("level index: complete");
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP LOAD] Construct: complete free=%lu max=%lu",
        static_cast<unsigned long>(sceKernelTotalFreeMemSize()),
        static_cast<unsigned long>(sceKernelMaxFreeMemSize()));
#endif
    return true;
}

bool World::Load(const std::string& lcfPath) {
    Unload();

    if (!g_blockManager) {
        ASSERT(false);
        return false;
    }
    BlockManager& blockMgr = *g_blockManager;

    g_wEffectChunkCount = 0;
    g_particleSystemChunkCount = 0;

    FillMeter(20);

#if defined(RC_PLATFORM_PSP)
    return LoadPspStream(lcfPath);
#endif

    // Read LCF file from disc (PC equivalent of Stream::Open + disc read).
    // Keep this as one direct allocation on PSP: ReadFile() first creates an
    // optional vector and its temporary lifetime made large level streams
    // fragile under the PSP heap's limited/fragmented memory.
    p3d::io::FileHandle lcfFile;
    LOG("[World] LCF open begin: %s", lcfPath.c_str());
    if (!lcfFile.Open(p3d::io::ResolvePath(lcfPath), p3d::io::FileHandle::Mode::Read)) {
        LOG("[World] Failed to open: %s", lcfPath.c_str());
        return false;
    }
    LOG("[World] LCF open complete");
    const int64_t lcfFileSize = lcfFile.Size();
    LOG("[World] LCF size: %lld", (long long)lcfFileSize);
    if (lcfFileSize <= 0 || lcfFileSize > INT32_MAX) {
        LOG("[World] Invalid LCF size: %lld", (long long)lcfFileSize);
        return false;
    }
#if defined(RC_PLATFORM_PSP)
    LOG("[World] PSP heap before LCF: free=%u max=%u",
        (u32)sceKernelTotalFreeMemSize(), (u32)sceKernelMaxFreeMemSize());
#endif
    streamData.resize(static_cast<size_t>(lcfFileSize));
    LOG("[World] LCF buffer allocated");
    if (lcfFile.Read(streamData.data(), streamData.size()) != streamData.size()) {
        LOG("[World] Failed to read LCF: %s", lcfPath.c_str());
        streamData.clear();
        return false;
    }
    LOG("[World] LCF read complete: %u bytes", (u32)streamData.size());

    u32 dataSize = static_cast<u32>(streamData.size());
    const u8* data = streamData.data();

    // Parse stream header (PSX Stream::Open reads this from disc)
    auto entries = ParseStreamHeader(data, dataSize);
    if (entries.empty()) {
        LOG("[World] No stream entries in: %s", lcfPath.c_str());
        streamData.clear();
        return false;
    }
    LOG("[World] LCF header parsed: %u entries", (u32)entries.size());

    // Load TPG textures into VRAM (PSX HandleTPGChunk)
    LOG("[World] TPG load begin");
    LoadTPGTextures(data, dataSize);
    LOG("[World] TPG load complete");
    FillMeter(40);

#if MOD_LOADER
    // After base VRAM load, let mods paint over individual texture pages.
    // Mod files are named "{levelname}_tpg{tpage:04x}.png" in mods/<mod>/textures/.
    {
        std::string levelStem = std::filesystem::path(lcfPath).stem().string();
        for (char& c : levelStem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // tpage field: bits[3:0]=tx (64-word columns), bit[4]=ty (256-row band), bits[8:7]=depth.
        // 15-bit pages use depth=2; tx can be 0-15, ty 0 or 1 → 32 possible page slots.
        for (int ty = 0; ty <= 1; ty++) {
            for (int tx = 0; tx <= 15; tx++) {
                u16 tpageKey = static_cast<u16>((2u << 7) | (ty << 4) | tx);
                char assetName[64];
                snprintf(assetName, sizeof(assetName), "%s_tpg%04x", levelStem.c_str(), tpageKey);
                u32 crc = p3dHash(assetName);
                if (ModLoader::Instance().HasTexture(crc)) {
                    const std::string* pngPath = ModLoader::Instance().GetTexturePath(crc);
                    if (pngPath)
                        UploadPngToVRAMPage(vram, tpageKey, pngPath->c_str());
                }
            }
        }
        // Keep the persistent VRAM image alive after custom texture uploads.
        // Destroying and reallocating the 1 MiB image here can fail on the
        // fragmented PSP heap even though the original allocation succeeded,
        // leaving the render context with handle 0 and silently disabling all
        // PSX page sampling.  Update only the accumulated dirty region.
        if (vramHandle) {
#if defined(RC_PLATFORM_PSP)
            s16 dirtyX = 0;
            s16 dirtyY = 0;
            s16 dirtyW = 0;
            s16 dirtyH = 0;
            if (vram.ConsumeDirtyRect(dirtyX, dirtyY, dirtyW, dirtyH)) {
                p3d::context->UpdateVRAMTextureRegion(vramHandle, 1024, 512, vram.data,
                                                       dirtyX, dirtyY, dirtyW, dirtyH);
            }
#else
            p3d::context->UpdateVRAMTexture(vramHandle, 1024, 512, vram.data);
#endif
        }
        else {
            vramHandle = p3d::context->CreateVRAMTexture(1024, 512, vram.data);
        }
    }
#endif

#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP VRAM] LoadPspStream before return handle=%lu entries=%lu",
        static_cast<unsigned long>(vramHandle),
        static_cast<unsigned long>(streamEntries.size()));
#endif

    // RCI/RCP resources are level-wide and survive petal reloads until PurgeLevel.
    LOG("[World] RCI/RCP load begin");
    LoadGeoPairsInRange(this, entries, data, dataSize, 0, (u32)entries.size(), ".RCI", ".RCP", 1);
    LOG("[World] RCI/RCP load complete");

    // PSX petal-based loading: the LCF contains multiple WDB+BLK groups,
    // one per petal. Each petal starts with a .WDB entry followed by .BLK entries.
    // PSX LoadPetal__6Stream finds the N-th WDB and reads only that petal's data.
    // We replicate that: find petal boundaries, load only the target petal.

    // Find WDB entry indices to identify petal boundaries
    std::vector<u32> wdbIndices;
    for (u32 i = 0; i < (u32)entries.size(); i++) {
        if (strncmp(entries[i].magic, ".WDB", 4) == 0) {
            wdbIndices.push_back(i);
        }
    }

    if (wdbIndices.empty()) {
        LOG("[World] No WDB entries in %s", lcfPath.c_str());
        streamData.clear();
        return false;
    }

    // Clamp target petal to valid range
    u32 petalIdx = targetPetalIndex;
    if (petalIdx >= (u32)wdbIndices.size()) {
        petalIdx = 0;
    }
    targetPetalIndex = petalIdx;

    // Determine entry range for this petal: [wdbIndex, nextWdbIndex)
    u32 petalStart = wdbIndices[petalIdx];
    u32 petalEnd = (petalIdx + 1 < (u32)wdbIndices.size())
        ? wdbIndices[petalIdx + 1]
        : (u32)entries.size();

    LOG("[World] Loading petal %u/%u (entries %u-%u) from %s",
        petalIdx, (u32)wdbIndices.size(), petalStart, petalEnd - 1, lcfPath.c_str());

    // Count BLK entries for this petal
    u32 blkCount = 0;
    for (u32 i = petalStart; i < petalEnd; i++) {
        if (strncmp(entries[i].magic, ".BLK", 4) == 0) blkCount++;
    }

    // Scan only this petal's WDB into the database
    LOG("[World] WDB scan begin");
    g_database->Close();
    for (u32 i = petalStart; i < petalEnd; i++) {
        if (strncmp(entries[i].magic, ".WDB", 4) != 0) continue;
        if (entries[i].offset + entries[i].size > dataSize) continue;
        g_database->Scan(data + entries[i].offset, entries[i].size);
    }
    LOG("[World] WDB scan complete");
#if MOD_LOADER
    {
        char levelScope[16], parameterName[32];
        std::snprintf(levelScope, sizeof(levelScope), "lev%02d", GetCurLevelID());
        std::snprintf(parameterName, sizeof(parameterName), "parameters_petal%02u", petalIdx);
        const std::string* path = ModLoader::Instance().FindDataOverridePath(levelScope, parameterName);
        if (!path) path = ModLoader::Instance().FindDataOverridePath(levelScope, "parameters");
        if (path) ApplyWDBParameterOverrides(g_database, path->c_str());
    }
#endif

    LOG("[World] PCI/PCP load begin");
    LoadGeoPairsInRange(this, entries, data, dataSize, petalStart, petalEnd, ".PCI", ".PCP", 2);
    LOG("[World] PCI/PCP load complete");

    LOG("[World] VRAM refresh begin");
    RefreshVRAMTexture();
    LOG("[World] VRAM refresh complete");

    // Build block volume list from this petal's WDB
    std::vector<DBVolume*> blockVolumes;
    for (DBRoot* v = g_database->GetFirstBlock(); v; v = static_cast<DBRoot*>(v->next)) {
        blockVolumes.push_back(static_cast<DBVolume*>(v));
    }
    LOG("[World] Parsed %u block volumes from petal %u WDB", (u32)blockVolumes.size(), petalIdx);

    // Initialize blocks from volumes (PSX _LoadBlocksFunc - Block::Init)
    LOG("[World] block metadata init begin");
    blockMgr.LoadBlocksFunc(blockVolumes);
    LOG("[World] block metadata init complete");

    // PSX timing: BLK parse/load is performed later during Construct, after AI::Populate.
    // Keep only block metadata (LoadBlocksFunc) here and defer Parse() to preserve
    // spawn-time activation semantics in AddThingNoTagList.
    LOG("[World] Deferring parse of %u BLK entries until post-populate construct step", blkCount);

    // Debug: log ALL block positions and compute level AABB
    s32 minX = 0x7FFFFFFF, minY = 0x7FFFFFFF, minZ = 0x7FFFFFFF;
    s32 maxX = -0x7FFFFFFF, maxY = -0x7FFFFFFF, maxZ = -0x7FFFFFFF;
    for (u32 i = 0; i < blockMgr.GetNumBlocks(); i++) {
        Block* b = blockMgr.GetBlock(i);
        if (!b) continue;
        LOG("[World] Block %u: blockNum=%u pos=(%d,%d,%d) dim=(%d,%d,%d) parsed=%d",
            i, b->blockNum, b->posX, b->posY, b->posZ, b->dimX, b->dimY, b->dimZ, b->parsed);
        s32 bMinX = b->posX + b->halfExtNegX, bMaxX = b->posX + b->halfExtPosX;
        s32 bMinY = b->posY + b->halfExtNegY, bMaxY = b->posY + b->halfExtPosY;
        s32 bMinZ = b->posZ + b->halfExtNegZ, bMaxZ = b->posZ + b->halfExtPosZ;
        if (bMinX < minX) minX = bMinX; if (bMaxX > maxX) maxX = bMaxX;
        if (bMinY < minY) minY = bMinY; if (bMaxY > maxY) maxY = bMaxY;
        if (bMinZ < minZ) minZ = bMinZ; if (bMaxZ > maxZ) maxZ = bMaxZ;
    }
    levelMin = { minX, minY, minZ };
    levelMax = { maxX, maxY, maxZ };
    LOG("[World] Level AABB: min=(%d,%d,%d) max=(%d,%d,%d)",
        minX, minY, minZ, maxX, maxY, maxZ);
    LOG("[World] Level size: (%d, %d, %d)",
        maxX - minX, maxY - minY, maxZ - minZ);

    FillMeter(65);
    return blockMgr.GetNumBlocks() > 0;
}

void World::UploadToVRAM(s16 x, s16 y, s16 w, s16 h, const u8* raw) {
    LOG("[VRAM] ext upload: x=%d y=%d w=%d h=%d", x, y, w, h);
    vram.Upload(x, y, w, h, raw);
}

void World::RefreshVRAMTexture() {
    // Called every frame while a Director VRAM flipbook animation is active
    // (Director::updateVramAnims), so update the existing texture in place
    // rather than destroying and recreating it each call -- the same
    // destroy+recreate cycle every frame indefinitely doesn't bother
    // desktop's driver/VRAM budget but exhausts the GPU allocator on the
    // Switch (both real hardware and emulation) after enough calls.
#if defined(RC_PLATFORM_PSP) && defined(PSP_GU_DIAGNOSTIC)
    static u32 refreshProbe = 0;
    const u32 refreshId = refreshProbe++;
    if (refreshId < 12u) {
        Log::Get().LogMessage("[PSP VRAM] refresh #%lu enter handle=%lu dirty=%u",
            static_cast<unsigned long>(refreshId),
            static_cast<unsigned long>(vramHandle),
            vram.IsDirty() ? 1u : 0u);
    }
#endif
    if (vramHandle) {
#if defined(RC_PLATFORM_PSP)
        s16 dirtyX = 0;
        s16 dirtyY = 0;
        s16 dirtyW = 0;
        s16 dirtyH = 0;
        if (vram.ConsumeDirtyRect(dirtyX, dirtyY, dirtyW, dirtyH)) {
            p3d::context->UpdateVRAMTextureRegion(vramHandle, 1024, 512, vram.data,
                                                   dirtyX, dirtyY, dirtyW, dirtyH);
#if defined(RC_PLATFORM_PSP) && defined(PSP_GU_DIAGNOSTIC)
            if (refreshId < 12u) {
                Log::Get().LogMessage("[PSP VRAM] refresh #%lu region handle=%lu rect=%d,%d %dx%d",
                    static_cast<unsigned long>(refreshId),
                    static_cast<unsigned long>(vramHandle), dirtyX, dirtyY, dirtyW, dirtyH);
            }
#endif
        }
#else
        p3d::context->UpdateVRAMTexture(vramHandle, 1024, 512, vram.data);
#endif
        return;
    }
    vramHandle = p3d::context->CreateVRAMTexture(1024, 512, vram.data);
    vram.ClearDirty();
#if defined(RC_PLATFORM_PSP) && defined(PSP_GU_DIAGNOSTIC)
    if (refreshId < 12u) {
        Log::Get().LogMessage("[PSP VRAM] refresh #%lu create result handle=%lu",
            static_cast<unsigned long>(refreshId),
            static_cast<unsigned long>(vramHandle));
    }
#endif
}

void World::Render(const LVector* playerPos) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_GU_DIAGNOSTIC)
    static u32 renderHandleProbe = 0;
    if (renderHandleProbe < 8u) {
        Log::Get().LogMessage("[PSP VRAM] world render #%lu handle=%lu",
            static_cast<unsigned long>(renderHandleProbe),
            static_cast<unsigned long>(vramHandle));
        ++renderHandleProbe;
    }
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    static u32 s_pspWorldRenderEntryProbe = 0;
    const u32 renderProbe = s_pspWorldRenderEntryProbe++;
    if (renderProbe < 4u) {
        LOG("[PSP World] Render enter #%lu world=%p manager=%p player=%p",
            static_cast<unsigned long>(renderProbe), static_cast<void*>(this),
            static_cast<void*>(g_blockManager), static_cast<const void*>(playerPos));
    }
#endif
    p3d::context->SetVRAMHandle(vramHandle);
    const f64 t0 = Time::GetTimeInSeconds();
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (renderProbe < 4u) LOG("[PSP World] Render before DrawEverything #%lu", static_cast<unsigned long>(renderProbe));
#endif
    DrawEverythingHandler(playerPos);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (renderProbe < 4u) LOG("[PSP World] Render after DrawEverything #%lu", static_cast<unsigned long>(renderProbe));
#endif
    g_frameProfile.drawSubmitMs = (f32)((Time::GetTimeInSeconds() - t0) * 1000.0);
    p3d::context->SetVRAMHandle(0);
}

// chanp3dClipCode - PC equivalent of PSX chanp3dClipCode.
// Projects a transformed tPort vector and computes the same clip-code bit layout
// used by computeBlockToPointDistances in PSX GAME.CPP.
static u32 chanp3dClipCode(const ChanProjectionState& portState, s32 vx, s32 vy, s32 vz) {
    const s32 denom = (vz == 0) ? 1 : vz;
    const f32 sxF = static_cast<f32>(portState.centerX)
        + (static_cast<f32>(vx) * portState.projectionDistanceX) / static_cast<f32>(denom);
    const f32 syF = static_cast<f32>(portState.centerY)
        + (static_cast<f32>(vy) * portState.projectionDistanceY) / static_cast<f32>(denom);

    // PSX tPort ProjectVector writes to short screen coordinates.
    s32 sx = 0;
    if (sxF < -32768.0f) {
        sx = -32768;
    }
    else if (sxF > 32767.0f) {
        sx = 32767;
    }
    else {
        sx = static_cast<s32>(sxF);
    }

    s32 sy = 0;
    if (syF < -32768.0f) {
        sy = -32768;
    }
    else if (syF > 32767.0f) {
        sy = 32767;
    }
    else {
        sy = static_cast<s32>(syF);
    }

    const s32 clipMaxX = (portState.width > 0) ? portState.width : 0x7FFF;
    const s32 clipMaxY = (portState.height > 0) ? portState.height : 0x7FFF;

    u32 code = 0;

    // PSX decomp bit layout:
    // x<0: 0x00004000, x>=clipX: 0x00008000
    // y<0: 0x40000000, y>=clipY: 0x80000000
    // z<0: 0x00000002
    if (sx < 0) {
        code |= 0x00004000u;
    }
    if (sx >= clipMaxX) {
        code |= 0x00008000u;
    }

    if (sy < 0) {
        code |= 0x40000000u;
    }
    if (sy >= clipMaxY) {
        code |= 0x80000000u;
    }

    // PSX chanp3dClipCode only checks z-sign (behind camera), not near/far clip planes.
    if (vz < 0) {
        code |= 0x00000002u;
    }

    return code;
}

// PSX: DrawLoop__FP6ccListUl (GAME.CPP:2543, 0x8002B224)
// Iterates a ccList and calls Draw() on entities in the given block.
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
static bool s_pspWorldRenderProbe = false;
static u32 s_pspWorldRenderProbeCalls = 0;
#endif

static void DrawEntityList(ccList& list, u16 blockNum, u32 vramHandle) {
    MARKFUNCTION(0x8002B224);
    for (ccMinNode* n = list.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        if (thing->blockNum == blockNum) {
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
            if (s_pspWorldRenderProbe) {
                LOG("[PSP World] entity begin block=%u thing=%p type=%u model=%p",
                    static_cast<unsigned>(blockNum), static_cast<void*>(thing),
                    static_cast<unsigned>(thing->thingType), thing->model);
            }
#endif
            p3d::context->SetVRAMHandle(vramHandle);
            p3d::context->SetTexInfoOverride(false, 0);
            thing->Draw();
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
            if (s_pspWorldRenderProbe) {
                LOG("[PSP World] entity done block=%u thing=%p type=%u",
                    static_cast<unsigned>(blockNum), static_cast<void*>(thing),
                    static_cast<unsigned>(thing->thingType));
            }
#endif
        }
    }
}

#if MODERN_GRAPHICS
static void DrawEntityCasterList(ccList& list, u16 blockNum, u32 vramHandle) {
    for (ccMinNode* n = list.head; n; n = n->next) {
        Thing* thing = static_cast<Thing*>(n);
        if (thing->blockNum == blockNum && thing->model) {
            p3d::context->SetVRAMHandle(vramHandle);
            p3d::context->SetTexInfoOverride(false, 0);
            thing->Draw();
        }
    }
}
#endif

// DrawEverythingHandler__FP7Handler (GAME.CPP:2211, 0x8002A98C)
// Reversed from PSX: builds draw list from loaded blocks, selection-sorts by distSq
// DESCENDING (farthest first for back-to-front rendering), applies OffsetToPreventSeams,
// checks InDrawList, renders entities + block geometry.
void World::DrawEverythingHandler(const LVector* playerPos) {
#if defined(RC_PLATFORM_PSP)
    PspPerf::Mark(PspPerf::RuntimeStage::WorldBegin);
#endif
    MARKFUNCTION(0x8002A98C);
#if defined(PSP_STAGE_DIAGNOSTIC)
    static u32 pspStageWorldCalls = 0;
    const u32 pspStageWorldCall = pspStageWorldCalls++;
    if (pspStageWorldCall < 4u) {
        Log::Get().LogMessage("[PSP Stage] draw world begin call=%lu", static_cast<unsigned long>(pspStageWorldCall));
    }
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    const bool probePspWorldRender = s_pspWorldRenderProbeCalls < 2;
    s_pspWorldRenderProbe = probePspWorldRender;
    if (probePspWorldRender) {
        LOG("[PSP World] begin render call=%lu", static_cast<unsigned long>(s_pspWorldRenderProbeCalls));
    }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("world render: begin");
#endif

    if (!g_blockManager) {
        ASSERT(false);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        s_pspWorldRenderProbe = false;
#endif
        return;
    }
    BlockManager& blockMgr = *g_blockManager;

    if (blockMgr.GetNumBlocks() == 0) {
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspWorldRender) LOG("[PSP World] no blocks");
        s_pspWorldRenderProbe = false;
        ++s_pspWorldRenderProbeCalls;
#endif
        return;
    }

    // PSX: DemandLoading when game state == 8
    if (g_game && g_game->GetState() == GameState::Play) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("world render: demand begin");
    PspPerf::Mark(PspPerf::RuntimeStage::WorldDemandBegin);
#endif
        blockMgr.DemandLoading();
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageWorldCall < 4u) {
            Log::Get().LogMessage("[PSP Stage] demand done call=%lu loaded=%lu", static_cast<unsigned long>(pspStageWorldCall), static_cast<unsigned long>(blockMgr.GetAlreadyLoadedCount()));
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("world render: demand done");
    PspPerf::Mark(PspPerf::RuntimeStage::WorldDemandDone);
#endif
    }

    // Build draw entry array: {Block*, distSq, zDepth}
    // PSX: iterates loaded block linked list (offset +144)
    struct DrawEntry {
        Block* block;
        s32 distSq;
        s32 zDepth;
    };
    DrawEntry drawArray[kBlockManagerListCapacity];
    u32 count = 0;

    const u32 loadedCount = blockMgr.GetAlreadyLoadedCount();
    for (u32 i = 0; i < loadedCount && count < kBlockManagerListCapacity; i++) {
        Block* block = blockMgr.GetBlockByBlockNumber(blockMgr.GetAlreadyLoadedBlockNum(i));
        if (!block || !block->primGeom) continue;

        s32 distSq, zDepth;
        computeBlockToPointDistances(block, playerPos, &distSq, &zDepth);
        drawArray[count].block = block;
        drawArray[count].distSq = distSq;
        drawArray[count].zDepth = zDepth;
        count++;
    }

    if (count == 0) {
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspWorldRender) LOG("[PSP World] empty draw list loaded=%lu", static_cast<unsigned long>(loadedCount));
        s_pspWorldRenderProbe = false;
        ++s_pspWorldRenderProbeCalls;
#endif
        return;
    }
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (probePspWorldRender) {
        LOG("[PSP World] draw list count=%lu loaded=%lu",
            static_cast<unsigned long>(count), static_cast<unsigned long>(loadedCount));
    }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("world render: draw list done");
#endif

    // PSX selection sort: DESCENDING by distSq (farthest first = back-to-front)
    // PSX inner loop finds the entry with the SMALLEST distSq, swaps to front.
    // After sorting: index 0 = farthest, last = nearest.
    for (u32 i = 0; i < count - 1; i++) {
        u32 minIdx = i;
        for (u32 j = i + 1; j < count; j++) {
            if (drawArray[minIdx].distSq < drawArray[j].distSq) {
                minIdx = j;
            }
        }
        if (minIdx != i) {
            DrawEntry tmp = drawArray[i];
            drawArray[i] = drawArray[minIdx];
            drawArray[minIdx] = tmp;
        }
    }

    // PSX: count visible blocks (positive distSq) = v27
    u32 visibleCount = 0;
    for (u32 i = 0; i < count; i++) {
        if (drawArray[i].distSq > 0) {
            visibleCount = i + 1;
        }
    }

    // PSX: find maxZDepth among far blocks (index >= 5), add 64, clamp to 0xFFFF
    // Used for OT layer setup on PSX - not functionally needed with z-buffer on PC.

    const Mat4 passBaseWorld = p3d::context->GetWorldMatrix();

    BeginModelShadowQueue();
    ComEffect_ClearLateRenderQueue();

#if MODERN_GRAPHICS
    if (ShadowCSM::IsFramePrepared()) {
        ShadowCSM::BeginCasterPrepass();
        for (u32 i = 0; i < count; i++) {
            DrawEntry& entry = drawArray[i];
            const u16 bn = entry.block->blockNum;
            if (!blockMgr.InDrawList(bn)) {
                continue;
            }

            LVector localPos;
            localPos.x = entry.block->posX;
            localPos.y = entry.block->posY;
            localPos.z = entry.block->posZ;
            OffsetToPreventSeams(localPos, *playerPos);

            p3d::context->SetTexInfoOverride(false, 0);
            p3d::context->SetBlendMode(PDDI_BLEND_NONE);
            p3d::context->SetCullMode(PDDI_CULL_NONE);
            ShadowCSM::DrawBlockCasterIntoCascades(entry.block, &localPos);

            if (!g_ai) {
                continue;
            }

            ShadowCSM::SetCasterWorldOffset((f32)(localPos.x - entry.block->posX),
                                            (f32)(localPos.y - entry.block->posY),
                                            (f32)(localPos.z - entry.block->posZ));
            DrawEntityCasterList(g_ai->humanoidList, bn, vramHandle);
            DrawEntityCasterList(g_ai->inactivePickupList, bn, vramHandle);
            DrawEntityCasterList(g_ai->pickupList, bn, vramHandle);
            DrawEntityCasterList(g_ai->moveList, bn, vramHandle);
        }
        ShadowCSM::SetCasterWorldOffset(0.0f, 0.0f, 0.0f);
        ShadowCSM::EndCasterPrepass();
    }
#endif

    // Pass 1: render visible block entities + geometry.
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("world render: pass1 begin");
#endif
    for (u32 i = 0; i < visibleCount; i++) {
        DrawEntry& entry = drawArray[i];
#if defined(PSP_STAGE_DIAGNOSTIC)
        if (pspStageWorldCall < 4u && i < 8u) {
            Log::Get().LogMessage("[PSP Stage] block begin call=%lu pass=1 index=%lu block=%u", static_cast<unsigned long>(pspStageWorldCall), static_cast<unsigned long>(i), static_cast<unsigned>(entry.block->blockNum));
        }
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        if (probePspWorldRender) {
            LOG("[PSP World] block begin pass1 index=%lu block=%u draw=%d",
                static_cast<unsigned long>(i), static_cast<unsigned>(entry.block->blockNum),
                blockMgr.InDrawList(entry.block->blockNum) ? 1 : 0);
        }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
        PspPerf::Trace("world render: pass1 block begin");
        PspPerf::Mark(PspPerf::RuntimeStage::WorldBlock, i, entry.block->blockNum);
#endif

        // Copy block->pos to local and apply OffsetToPreventSeams
        LVector localPos;
        localPos.x = entry.block->posX;
        localPos.y = entry.block->posY;
        localPos.z = entry.block->posZ;
        OffsetToPreventSeams(localPos, *playerPos);

        u16 bn = entry.block->blockNum;

        // Guard against world-matrix leakage between blocks.
        p3d::context->SetWorldMatrix(passBaseWorld);

        p3d::context->SetVRAMHandle(vramHandle);
        p3d::context->SetTexInfoOverride(false, 0);
        p3d::context->SetBlendMode(PDDI_BLEND_NONE);
        p3d::context->SetCullMode(PDDI_CULL_NONE);

        // PSX: only draw entities + geometry if block is in draw list
        if (blockMgr.InDrawList(bn)) {
            const Mat4 blockBaseWorld = passBaseWorld;

            // PSX: DrawLoop for each entity list
            if (g_ai) {
#if !defined(PSP_STAGE_SKIP_ENTITIES)
#if defined(PSP_STAGE_DIAGNOSTIC)
                if (pspStageWorldCall < 4u && i < 8u) {
                    Log::Get().LogMessage("[PSP Stage] entities begin call=%lu block=%u", static_cast<unsigned long>(pspStageWorldCall), static_cast<unsigned>(bn));
                }
#endif
#if MODERN_GRAPHICS
                p3d::context->SetReceiveShadows(false);
                ShadowCSM::SetCasterWorldOffset((f32)(localPos.x - entry.block->posX),
                                                (f32)(localPos.y - entry.block->posY),
                                                (f32)(localPos.z - entry.block->posZ));
#endif
                ComEffect_BeginLateRenderQueue(static_cast<s32>(bn));
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
                if (probePspWorldRender) LOG("[PSP World] humanoids begin block=%u", static_cast<unsigned>(bn));
#endif
                DrawEntityList(g_ai->humanoidList, bn, vramHandle);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
                if (probePspWorldRender) LOG("[PSP World] humanoids done block=%u", static_cast<unsigned>(bn));
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
                PspPerf::Trace("world render: humanoids done");
#endif
                DrawEntityList(g_ai->inactivePickupList, bn, vramHandle);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
                if (probePspWorldRender) LOG("[PSP World] inactive done block=%u", static_cast<unsigned>(bn));
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
                PspPerf::Trace("world render: inactive pickups done");
#endif
                DrawEntityList(g_ai->pickupList, bn, vramHandle);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
                if (probePspWorldRender) LOG("[PSP World] pickups done block=%u", static_cast<unsigned>(bn));
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
                PspPerf::Trace("world render: pickups done");
#endif
                DrawEntityList(g_ai->moveList, bn, vramHandle);
#if defined(PSP_STAGE_DIAGNOSTIC)
                if (pspStageWorldCall < 4u && i < 8u) {
                    Log::Get().LogMessage("[PSP Stage] entities done call=%lu block=%u", static_cast<unsigned long>(pspStageWorldCall), static_cast<unsigned>(bn));
                }
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
                if (probePspWorldRender) LOG("[PSP World] moves done block=%u", static_cast<unsigned>(bn));
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
                PspPerf::Trace("world render: moves done");
#endif
                ComEffect_EndLateRenderQueue();
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
                PspPerf::Trace("world render: late queue ended");
#endif
#if MODERN_GRAPHICS
                ShadowCSM::SetCasterWorldOffset(0.0f, 0.0f, 0.0f);
#endif
#endif
            }

            // Block geometry pass for this block.
            p3d::context->SetVRAMHandle(vramHandle);
            p3d::context->SetTexInfoOverride(false, 0);
            p3d::context->SetBlendMode(PDDI_BLEND_NONE);
            p3d::context->SetCullMode(PDDI_CULL_NONE);

#if MODERN_GRAPHICS
            p3d::context->SetReceiveShadows(true);
#endif
            p3d::context->SetWorldMatrix(blockBaseWorld);
#if defined(PSP_STAGE_DIAGNOSTIC)
            if (pspStageWorldCall < 4u && i < 8u) {
                Log::Get().LogMessage("[PSP Stage] geometry begin call=%lu block=%u", static_cast<unsigned long>(pspStageWorldCall), static_cast<unsigned>(bn));
            }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            PspPerf::Trace("world render: block geometry begin");
    PspPerf::Mark(PspPerf::RuntimeStage::WorldGeometryBegin);
#endif
            entry.block->Draw(&localPos);
#if defined(PSP_STAGE_DIAGNOSTIC)
            if (pspStageWorldCall < 4u && i < 8u) {
                Log::Get().LogMessage("[PSP Stage] geometry done call=%lu block=%u", static_cast<unsigned long>(pspStageWorldCall), static_cast<unsigned>(bn));
                Log::Get().LogMessage("[PSP Stage] block done call=%lu pass=1 index=%lu block=%u", static_cast<unsigned long>(pspStageWorldCall), static_cast<unsigned long>(i), static_cast<unsigned>(entry.block->blockNum));
            }
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
            if (probePspWorldRender) {
                LOG("[PSP World] block geometry done block=%u", static_cast<unsigned>(bn));
            }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            PspPerf::Trace("world render: block geometry done");
    PspPerf::Mark(PspPerf::RuntimeStage::WorldGeometryDone);
#endif
#if MODERN_GRAPHICS
            p3d::context->SetReceiveShadows(false);
#endif

            // Flush queued model shadows after block geometry.
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            PspPerf::Trace("world render: model shadows begin");
#endif
            FlushModelShadowQueue();
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            PspPerf::Trace("world render: model shadows done");
            PspPerf::Trace("world render: late entity begin");
#endif
            FlushLateEntityDrawQueue();
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
            PspPerf::Trace("world render: late entity done");
#endif
        }
    }

    EndModelShadowQueue();
#if defined(PSP_STAGE_DIAGNOSTIC)
    if (pspStageWorldCall < 4u) {
        Log::Get().LogMessage("[PSP Stage] pass1 done call=%lu", static_cast<unsigned long>(pspStageWorldCall));
    }
#endif
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (probePspWorldRender) LOG("[PSP World] pass1 done");
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("world render: pass1 done");
#endif

    // Pass 2: render per-block effects after all block geometry to prevent
    // cross-block overdraw from clipping effects drawn earlier in the frame.
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("world render: pass2 begin");
#endif
    for (u32 i = 0; i < visibleCount; i++) {
        DrawEntry& entry = drawArray[i];

        LVector localPos;
        localPos.x = entry.block->posX;
        localPos.y = entry.block->posY;
        localPos.z = entry.block->posZ;
        OffsetToPreventSeams(localPos, *playerPos);

        const u16 bn = entry.block->blockNum;

        p3d::context->SetWorldMatrix(passBaseWorld);

        p3d::context->SetVRAMHandle(vramHandle);
        p3d::context->SetTexInfoOverride(false, 0);
        p3d::context->SetBlendMode(PDDI_BLEND_NONE);
        p3d::context->SetCullMode(PDDI_CULL_NONE);

        if (!blockMgr.InDrawList(bn)) {
            continue;
        }

        const s32 seamOffsetX = localPos.x - entry.block->posX;
        const s32 seamOffsetY = localPos.y - entry.block->posY;
        const s32 seamOffsetZ = localPos.z - entry.block->posZ;

        Mat4 effectBaseWorld = passBaseWorld;
        effectBaseWorld.m[12] += static_cast<f32>(seamOffsetX);
        effectBaseWorld.m[13] += static_cast<f32>(seamOffsetY);
        effectBaseWorld.m[14] += static_cast<f32>(seamOffsetZ);
        p3d::context->SetWorldMatrix(effectBaseWorld);

        ComEffect_SetSeamOffset(seamOffsetX, seamOffsetY, seamOffsetZ);
        ComEffect_FlushLateRenderQueue(static_cast<s32>(bn));
        Effects_DrawEffects(static_cast<s32>(bn));
        ComEffect_SetSeamOffset(0, 0, 0);
    }
    ComEffect_ClearLateRenderQueue();
#if defined(PSP_STAGE_DIAGNOSTIC)
    if (pspStageWorldCall < 4u) {
        Log::Get().LogMessage("[PSP Stage] pass2 done call=%lu", static_cast<unsigned long>(pspStageWorldCall));
    }
#endif
    p3d::context->SetBlendMode(PDDI_BLEND_NONE);
    p3d::context->EnableZBuffer(true);
    p3d::context->SetDepthClamp(false);

    p3d::context->SetWorldMatrix(passBaseWorld);
#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
    if (probePspWorldRender) {
        LOG("[PSP World] pass2 done");
        s_pspWorldRenderProbe = false;
        ++s_pspWorldRenderProbeCalls;
    }
#endif
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("world render: pass2 done");
    PspPerf::Mark(PspPerf::RuntimeStage::WorldEnd);
#endif

    // PSX: DebugDrawSector, ExitLayer(2), profile end(7)
}

// computeBlockToPointDistances (GAME.CPP:1976)
// Reversed from PSX: builds 8 bounding box corners + center (9 points),
// transforms each through view matrix, computes clip codes + view-space distance,
// tests 13 clip code pairs for frustum culling.
// a0=block, a1=playerPos (unused - view matrix already set), a2=outDistSq, a3=outZDepth
void World::computeBlockToPointDistances(const Block* block, const LVector* playerPos,
                                         s32* outDistSq, s32* outZDepth) {
    MARKFUNCTION(0x8002A238);

    // PSX reads bounds via block->primGeom virtual path. Use tPrimGeom bounds
    // directly so culling volume matches rendered prim geometry.
    s32 bbox[6] = {};
    if (block->primGeom) {
        bbox[0] = block->primGeom->bboxMinX;
        bbox[1] = block->primGeom->bboxMinY;
        bbox[2] = block->primGeom->bboxMinZ;
        bbox[3] = block->primGeom->bboxMaxX;
        bbox[4] = block->primGeom->bboxMaxY;
        bbox[5] = block->primGeom->bboxMaxZ;
    }
    else {
        bbox[0] = block->halfExtNegX;
        bbox[1] = block->halfExtNegY;
        bbox[2] = block->halfExtNegZ;
        bbox[3] = block->halfExtPosX;
        bbox[4] = block->halfExtPosY;
        bbox[5] = block->halfExtPosZ;
    }

    // s5 = &block->posX (block position at offset +4)
    const s32* pos = &block->posX; // pos[0]=X, pos[1]=Y, pos[2]=Z

    // Get view matrix and current tPort projection state.
    const Mat4& vm = p3d::context->GetViewMatrix();
    ChanProjectionState portState;
    if (g_display) {
        portState = g_display->GetChanProjectionState();
    }

    s32 minDistSq = 0; // s7
    s32 maxZDepth = 0; // s6
    u32 clipCodes[8];
    s32 tvx, tvy, tvz; // transformed view-space coords

    // Build and process 8 bounding box corners
    // PSX corner pattern: (bbox[negX/posX], bbox[negY/posY], bbox[negZ/posZ]) + blockPos
    // Corner 0: pos + (negX, negY, negZ)
    {
        s32 wx = pos[0] + bbox[0], wy = pos[1] + bbox[1], wz = pos[2] + bbox[2];
        PsxTransformPointToPort(vm, wx, wy, wz, &tvx, &tvy, &tvz);
        s32 svx = tvx, svy = tvy, svz = tvz; // save pre-project coords
        clipCodes[0] = chanp3dClipCode(portState, tvx, tvy, tvz);
        minDistSq = PsxVecLengthSquaredQuarter(svx, svy, svz);
        s32 z = svz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Corner 1: pos + (posX, negY, negZ)
    {
        s32 wx = pos[0] + bbox[3], wy = pos[1] + bbox[1], wz = pos[2] + bbox[2];
        PsxTransformPointToPort(vm, wx, wy, wz, &tvx, &tvy, &tvz);
        s32 svx = tvx, svy = tvy, svz = tvz;
        clipCodes[1] = chanp3dClipCode(portState, tvx, tvy, tvz);
        s32 d = PsxVecLengthSquaredQuarter(svx, svy, svz);
        if (d < minDistSq) minDistSq = d;
        s32 z = svz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Corner 2: pos + (negX, posY, negZ)
    {
        s32 wx = pos[0] + bbox[0], wy = pos[1] + bbox[4], wz = pos[2] + bbox[2];
        PsxTransformPointToPort(vm, wx, wy, wz, &tvx, &tvy, &tvz);
        s32 svx = tvx, svy = tvy, svz = tvz;
        clipCodes[2] = chanp3dClipCode(portState, tvx, tvy, tvz);
        s32 d = PsxVecLengthSquaredQuarter(svx, svy, svz);
        if (d < minDistSq) minDistSq = d;
        s32 z = svz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Corner 3: pos + (posX, posY, negZ)
    {
        s32 wx = pos[0] + bbox[3], wy = pos[1] + bbox[4], wz = pos[2] + bbox[2];
        PsxTransformPointToPort(vm, wx, wy, wz, &tvx, &tvy, &tvz);
        s32 svx = tvx, svy = tvy, svz = tvz;
        clipCodes[3] = chanp3dClipCode(portState, tvx, tvy, tvz);
        s32 d = PsxVecLengthSquaredQuarter(svx, svy, svz);
        if (d < minDistSq) minDistSq = d;
        s32 z = svz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Corner 4: pos + (negX, negY, posZ)
    {
        s32 wx = pos[0] + bbox[0], wy = pos[1] + bbox[1], wz = pos[2] + bbox[5];
        PsxTransformPointToPort(vm, wx, wy, wz, &tvx, &tvy, &tvz);
        s32 svx = tvx, svy = tvy, svz = tvz;
        clipCodes[4] = chanp3dClipCode(portState, tvx, tvy, tvz);
        s32 d = PsxVecLengthSquaredQuarter(svx, svy, svz);
        if (d < minDistSq) minDistSq = d;
        s32 z = svz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Corner 5: pos + (posX, negY, posZ)
    {
        s32 wx = pos[0] + bbox[3], wy = pos[1] + bbox[1], wz = pos[2] + bbox[5];
        PsxTransformPointToPort(vm, wx, wy, wz, &tvx, &tvy, &tvz);
        s32 svx = tvx, svy = tvy, svz = tvz;
        clipCodes[5] = chanp3dClipCode(portState, tvx, tvy, tvz);
        s32 d = PsxVecLengthSquaredQuarter(svx, svy, svz);
        if (d < minDistSq) minDistSq = d;
        s32 z = svz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Corner 6: pos + (negX, posY, posZ)
    {
        s32 wx = pos[0] + bbox[0], wy = pos[1] + bbox[4], wz = pos[2] + bbox[5];
        PsxTransformPointToPort(vm, wx, wy, wz, &tvx, &tvy, &tvz);
        s32 svx = tvx, svy = tvy, svz = tvz;
        clipCodes[6] = chanp3dClipCode(portState, tvx, tvy, tvz);
        s32 d = PsxVecLengthSquaredQuarter(svx, svy, svz);
        if (d < minDistSq) minDistSq = d;
        s32 z = svz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Corner 7: pos + (posX, posY, posZ)
    {
        s32 wx = pos[0] + bbox[3], wy = pos[1] + bbox[4], wz = pos[2] + bbox[5];
        PsxTransformPointToPort(vm, wx, wy, wz, &tvx, &tvy, &tvz);
        s32 svx = tvx, svy = tvy, svz = tvz;
        clipCodes[7] = chanp3dClipCode(portState, tvx, tvy, tvz);
        s32 d = PsxVecLengthSquaredQuarter(svx, svy, svz);
        if (d < minDistSq) minDistSq = d;
        s32 z = svz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Center (9th point): just blockPos, no bbox offset
    // PSX: TransformVector + vecLengthSquared only (no ProjectVector/chanp3dClipCode)
    {
        PsxTransformPointToPort(vm, pos[0], pos[1], pos[2], &tvx, &tvy, &tvz);
        s32 d = PsxVecLengthSquaredQuarter(tvx, tvy, tvz);
        if (d < minDistSq) minDistSq = d;
        s32 z = tvz;
        if (z > 0xFFFF) z = 0xFFFF;
        if (z > maxZDepth) maxZDepth = z;
    }

    // Frustum cull test: 13 specific clip code pairs ANDed
    // If ANY pair ANDs to 0 - visible (at least one edge straddles a frustum plane)
    // If ALL pairs are non-zero - fully culled
    // PSX pairs: (0,1)(0,2)(0,4)(1,3)(1,5)(2,3)(2,6)(3,7)(4,5)(4,6)(5,7)(6,7)(0,7)
    if ((clipCodes[0] & clipCodes[1]) != 0 &&
        (clipCodes[0] & clipCodes[2]) != 0 &&
        (clipCodes[0] & clipCodes[4]) != 0 &&
        (clipCodes[1] & clipCodes[3]) != 0 &&
        (clipCodes[1] & clipCodes[5]) != 0 &&
        (clipCodes[2] & clipCodes[3]) != 0 &&
        (clipCodes[2] & clipCodes[6]) != 0 &&
        (clipCodes[3] & clipCodes[7]) != 0 &&
        (clipCodes[4] & clipCodes[5]) != 0 &&
        (clipCodes[4] & clipCodes[6]) != 0 &&
        (clipCodes[5] & clipCodes[7]) != 0 &&
        (clipCodes[6] & clipCodes[7]) != 0 &&
        (clipCodes[0] & clipCodes[7]) != 0) {
        // All 13 pairs non-zero - block is fully outside the frustum
        *outDistSq = -1;
        return;
    }

    // Visible - output minimum distance and maximum z-depth
    *outDistSq = minDistSq;
    *outZDepth = maxZDepth;
}

// OffsetToPreventSeams__FR10tagLVectorRC10tagLVector (GAME.CPP:2482)
// PSX: computes per-axis sign of (pos - playerPos),
// then offset = -sign * (sign * delta / divisor + 1), clamped to +/-limit.
void World::OffsetToPreventSeams(LVector& pos, const LVector& playerPos) {
    MARKFUNCTION(0x8002AF88);

    s32 dx = pos.x - playerPos.x;
    s32 dy = pos.y - playerPos.y;
    s32 dz = pos.z - playerPos.z;

    // Compute sign per axis: -1, 0, or +1 - sp[16], sp[20], sp[24]
    s32 signX = (dx < 0) ? -1 : (dx > 0) ? 1 : 0;
    s32 signY = (dy < 0) ? -1 : (dy > 0) ? 1 : 0;
    s32 signZ = (dz < 0) ? -1 : (dz > 0) ? 1 : 0;

    // PSX small-data values loaded from gp+0x60 / gp+0x64.
    // These correspond to BLOCK_DRAW_SEAM_OFFSET_CODE[1] and [2].
    s32 seamDivisor = BLOCK_DRAW_SEAM_OFFSET_CODE[1];
    s32 seamLimit = BLOCK_DRAW_SEAM_OFFSET_CODE[2];

    // PSX: a3 = (signX * dx) / seamDivisor
    s32 rawX = (signX * dx) / seamDivisor;
    s32 rawY = (signY * dy) / seamDivisor;
    s32 rawZ = (signZ * dz) / seamDivisor;

    // PSX: offset = (-sign) * (raw + 1) - pushes block position toward camera
    s32 offX = (-signX) * (rawX + 1);
    s32 offY = (-signY) * (rawY + 1);
    s32 offZ = (-signZ) * (rawZ + 1);

    // PSX: clamp each to +/-seamLimit (gp[100])
    if (offX < -seamLimit) offX = -seamLimit;
    else if (offX > seamLimit) offX = seamLimit;
    if (offY < -seamLimit) offY = -seamLimit;
    else if (offY > seamLimit) offY = seamLimit;
    if (offZ < -seamLimit) offZ = -seamLimit;
    else if (offZ > seamLimit) offZ = seamLimit;

    // PSX: add offsets to position
    pos.x += offX;
    pos.y += offY;
    pos.z += offZ;
}

void World::Unload() {
#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP VRAM] World::Unload enter handle=%lu entries=%lu",
        static_cast<unsigned long>(vramHandle),
        static_cast<unsigned long>(streamEntries.size()));
#endif
    if (g_hud) {
        g_hud->OnUnloadLevel();
    }

    const bool hadLoadedLevel =
#if defined(RC_PLATFORM_PSP)
        !streamEntries.empty() ||
#else
        !streamData.empty() ||
#endif
        (vramHandle != 0);
    if (hadLoadedLevel) {
        if (g_blockManager) {
            g_blockManager->InternalClose();
        }
        if (g_ai) {
            g_ai->UnPopulate(0);
        }

        WEffect_UnPopulateWEffects(-1);

        Obstacle_ClearPetalAnimList();
        Effects_UnloadAll();
        WEffect_Unload();
        PWEffect_Unload();
        GEffect_Unload();
        UnloadScaleData();
        UnloadPaletteData();
        ParticleSystem_Unload();
        UnloadUVPrimData();
        UnloadCBVPrimData();
        PurgeSwitches();

        if (g_director) {
            g_director->PurgeAnims();
        }
        if (g_display && g_display->GetCamera()) {
            g_display->GetCamera()->PurgeAnims();
        }
        if (g_levelManager) {
            g_levelManager->PurgeLevel();
        }
    }
    else {
        WEffect_UnPopulateWEffects(-1);
        Effects_UnloadAll();
        WEffect_Unload();
        PWEffect_Unload();
        GEffect_Unload();
        UnloadScaleData();
        UnloadPaletteData();
        ParticleSystem_Unload();
        UnloadUVPrimData();
        UnloadCBVPrimData();
        PurgeSwitches();
    }

    g_wEffectChunkCount = 0;
    g_particleSystemChunkCount = 0;

    BackG::DeleteBG();

    streamData.clear();
#if defined(RC_PLATFORM_PSP)
    streamPath.clear();
    streamEntries.clear();
    pspStreamScratch.clear();
    pspPermScratch.clear();
    pspP3dScratch.clear();
#endif
    if (vramHandle && p3d::context) {
        p3d::context->DestroyVRAMTexture(vramHandle);
        vramHandle = 0;
    }

#if defined(PSP_GU_DIAGNOSTIC)
    Log::Get().LogMessage("[PSP VRAM] World::Unload exit handle=%lu",
        static_cast<unsigned long>(vramHandle));
#endif

    rsEvent(RS_UNLOAD_LEVEL, 0, 0, 0);
}

// PSX: UnloadLevelPart2__5World (WORLD.CPP:1355, 0x80046208)
void World::UnloadLevelPart2() {
    MARKFUNCTION(0x80046208);

    streamData.clear();
    DeletePlayerBlendAndAnimData();
    WorldPoints_Reset();
}

// PSX: UnloadPermanent__5World (WORLD.CPP:1886, 0x80046CB0)
void World::UnloadPermanent() {
    MARKFUNCTION(0x80046CB0);
}

// PSX: UnloadPetal__5World (WORLD.CPP:1176, 0x80045F34)
void World::UnloadPetal() {
    MARKFUNCTION(0x80045F34);
    WEffect_UnPopulateWEffects(-1);
    WEffect_Unload();
    PWEffect_Unload();
    ParticleSystem_UnloadLevel();
    UnloadLevelScaleData();
    UnloadPaletteData();
    UnloadUVPrimData();
    UnloadCBVPrimData();
    PurgeSwitches();

    // Unload current blocks (collision sectors, geometry)
    if (g_blockManager) {
        g_blockManager->InternalClose();
    }

    // Clear all AI entities from previous petal
    if (g_ai) {
        g_ai->UnPopulate(0);
    }

    if (g_director) {
        g_director->PurgeAnims();
    }

    if (g_levelManager) {
        g_levelManager->PurgePetal();
    }

    rsEvent(RS_UNLOAD_LEVEL, 0, 0, 0);
}

// PSX: LoadPetal__5WorldUl (WORLD.CPP:1222, 0x8004604C)
void World::LoadPetal(u32 petalIndex) {
    MARKFUNCTION(0x8004604C);

#if defined(RC_PLATFORM_PSP)
    LoadPspPetal(petalIndex);
    return;
#endif

    // PSX: if current level is DestSelect (lev07), save as previous
    if (levelList && currentLevelIndex < (u32)levelCount) {
        if (levelList[currentLevelIndex * 2] == 7)
            previousLevelIndex = currentLevelIndex;
    }

    // PSX: EstimateLoadTime, StartLogo, FillMeter(100)
    StartLogo("RUNFIRST.TIM");

    // Keep petal selection valid for this level before indexing sound tables.
    s32 petalCount = GetCurLevelPetals();
    if (petalCount <= 0 || petalIndex >= (u32)petalCount) {
        petalIndex = 0;
    }

    // PSX: rsEvent(4, petalSoundIDs[currentLevelIndex][petalIndex] - 1, 0, 0)
    if (petalSoundIDs && currentLevelIndex < (u32)levelCount) {
        s32 soundLocation = (s32)petalSoundIDs[currentLevelIndex][petalIndex] - 1;
        rsEvent(RS_SET_LOCATION, soundLocation, 0, 0);
    }

    targetPetalIndex = petalIndex;
    currentPetalIndex = petalIndex;

    if (!g_blockManager) {
        ASSERT(false);
        StopLogo();
        return;
    }
    BlockManager& blockMgr = *g_blockManager;

    blockMgr.SetDeathVolumeFlag(1);
    g_wEffectChunkCount = 0;
    g_particleSystemChunkCount = 0;

    if (g_scoreManager) {
        if (Player::s_player && Player::s_player->checkpoint.IsValid()) {
            g_scoreManager->HandleCheckpointBegin();
        }
        else {
            g_scoreManager->HandleLevelBegin();
        }
    }

    if (g_hud) {
        g_hud->OnLoadLevel();
    }

    // PSX: LevelManager::LoadPetal re-reads from Stream at petal position.
    // PC: re-parse the already-loaded LCF data for the new petal.
    if (!streamData.empty()) {
        u32 dataSize = static_cast<u32>(streamData.size());
        const u8* data = streamData.data();

        auto entries = ParseStreamHeader(data, dataSize);

        // Find WDB entry indices (petal boundaries)
        std::vector<u32> wdbIndices;
        for (u32 i = 0; i < (u32)entries.size(); i++) {
            if (strncmp(entries[i].magic, ".WDB", 4) == 0) {
                wdbIndices.push_back(i);
            }
        }

        u32 pi = petalIndex;
        if (pi >= (u32)wdbIndices.size()) pi = 0;
        if (pi != petalIndex) {
            petalIndex = pi;
            targetPetalIndex = pi;
            currentPetalIndex = pi;
        }

        u32 petalStart = wdbIndices[pi];
        u32 petalEnd = (pi + 1 < (u32)wdbIndices.size())
            ? wdbIndices[pi + 1]
            : (u32)entries.size();

        LOG("[World] LoadPetal %u: entries %u-%u", pi, petalStart, petalEnd - 1);

        // Scan this petal's WDB
        PurgeSwitches();
        g_database->Close();
        for (u32 i = petalStart; i < petalEnd; i++) {
            if (strncmp(entries[i].magic, ".WDB", 4) != 0) continue;
            if (entries[i].offset + entries[i].size > dataSize) continue;
            g_database->Scan(data + entries[i].offset, entries[i].size);
        }
#if MOD_LOADER
        {
            char levelScope[16], parameterName[32];
            std::snprintf(levelScope, sizeof(levelScope), "lev%02d", GetCurLevelID());
            std::snprintf(parameterName, sizeof(parameterName), "parameters_petal%02u", pi);
            const std::string* path = ModLoader::Instance().FindDataOverridePath(levelScope, parameterName);
            if (!path) path = ModLoader::Instance().FindDataOverridePath(levelScope, "parameters");
            if (path) ApplyWDBParameterOverrides(g_database, path->c_str());
        }
#endif

        LoadGeoPairsInRange(this, entries, data, dataSize, petalStart, petalEnd, ".PCI", ".PCP", 2);

        WEffect_InitWorldEffects(g_database->GetFirstPoint());
        PWEffect_InitWorldEffects(g_database->GetFirstPoint());
        ParticleSystem_InitParticleInfoMemory();

        // Build block volumes
        std::vector<DBVolume*> blockVolumes;
        for (DBRoot* v = g_database->GetFirstBlock(); v; v = static_cast<DBRoot*>(v->next)) {
            blockVolumes.push_back(static_cast<DBVolume*>(v));
        }
        blockMgr.LoadBlocksFunc(blockVolumes);

        // PSX timing: BLK parse/load happens after AI::Populate.
        LOG("[World] LoadPetal: deferring BLK parse until post-populate");
    }
    FillMeter(40);

    // PSX: AI::Populate for new petal entities
    if (g_ai) {
        g_ai->Populate();
    }
    FillMeter(85);

    u32 startBlockNum = 0;
    if (Player::s_player) {
        startBlockNum = Player::s_player->blockNum;
    }

    LoadBlocksForPetalFromStream(blockMgr, streamData, currentPetalIndex, startBlockNum, "World::LoadPetal");

    if (g_ai) {
        g_ai->PopulateBlock();
    }

    WEffect_PopulateWEffects();
    FillMeter(100);

    RefreshVRAMTexture();

    if (g_director) {
        g_director->Reset();
        g_director->SetScript();
    }

    ProcessSwitches();

    // PSX: ExecuteLoadCallbacks -> cameraLoadFunc -> SetupPaths after petal load.
    if (g_cameraManager) {
        g_cameraManager->SetupPaths();
    }

    // PSX: gSyncLoadGroup(0); gSyncLoadGroup(1)
    SyncSwitchLoadGroup(0);
    SyncSwitchLoadGroup(1);

    rsEvent(RS_LEVEL_BEGIN, 0, 0, 0);

    // PSX: StopLogo after load completes
    StopLogo();
    if (g_director) {
        g_director->SkipNextWideScreenEnterAnim();
    }
}

// PSX: DeletePlayerBlendAndAnimData__Fv (WORLD.CPP:2059, 0x80047014)
s32 DeletePlayerBlendAndAnimData() {
    MARKFUNCTION(0x80047014);
    return 0;
}

// PSX: ResetLevel__5World (WORLD.CPP:1918, 0x80046DE0)
void World::ResetLevel() {
    MARKFUNCTION(0x80046DE0);

    pendingPlayerReset = true;

    if (Player::s_player) {
        Player::s_player->checkpoint.SetValidState(0);
    }

    if (g_director) {
        g_director->LevelReset();
    }

    levelDeadPool.InternalReset();
}

// PSX: LevelMenuExecute__5WorldP10hdMenuItem (WORLD.CPP:868, 0x80045634)
// Callback invoked when a level is selected in the level menu.
s32 World::LevelMenuExecute(hdMenuItem* item) {
    MARKFUNCTION(0x80045634);

    u32 levelIndex = 0;
    u32 petalIndex = 0;

    // PSX: item->data[5] holds the packed level name (set by InitLevelMenu)
    // hdMenuItem: +20 = itemFlags, +24 = itemID. But PSX uses offset +20 as value.
    // Actually PSX reads item[5] = *(item + 20) = itemFlags field repurposed as value.
    UnpackLevelName(item->itemFlags, levelIndex, petalIndex);

    World* world = g_game ? g_game->GetWorld() : nullptr;
    if (!world) {
        return 4;
    }

    u32 curLevel = world->currentLevelIndex;
    u32 curPetal = world->currentPetalIndex;

    world->targetLevelIndex = levelIndex;
    world->targetPetalIndex = petalIndex;

    // PSX: if same level + same petal -> QueuePetalLoad (21)
    // PSX: else -> stop music, QueueLevelLoad (20)
    bool sameLevel = (curLevel == levelIndex) && (curPetal == petalIndex);
    GameState nextState = GameState::QueuePetalLoad;

    if (!sameLevel) {
        rsEvent(RS_STOP_MUSIC, 0, 0, 0);
        nextState = GameState::QueueLevelLoad;
    }

    g_game->SetState(nextState);
    world->ResetLevel();

    return 4;
}

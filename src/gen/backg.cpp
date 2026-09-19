#include "common.h"
#include "ai/player.h"
#include "gen/backg.h"
#include "gen/blockmgr.h"
#include "gen/camera.h"
#include "gen/database.h"
#include "gen/display.h"
#include "gen/game.h"
#include "gen/geometry.h"
#include "gen/levelmgr.h"
#include "gen/model.h"
#include "gen/psxcolor_helpers.h"
#include "gen/world.h"
#include "p3d/byteread.h"
#include "p3d/hash.h"
#include "p3d/p3dmath.h"
#include "p3d/texture.h"
#include "pc/tim.h"
#if defined(RC_PLATFORM_PSP)
#include "p3d/context.h"
#include "pddi/psp/psprender.h"
#endif

static BackGWorkParams s_backgWork;
static BackGRuntimeParams s_backgRuntime;
static BackG* s_backg = nullptr;

static constexpr s32 kBackGMaxEntries = 100;
#if !defined(RC_PLATFORM_PSP)
static constexpr s32 kBackGVramCacheEntries = 32;
static BackGVramCacheEntry s_backgVramCache[kBackGVramCacheEntries];
static s32 s_backgVramCacheNext = 0;
static u8 s_backgDecodePageRGBA[256 * 256 * 4];
static tTexture* GetBackGVramTexture(const World* world, u16 tpage, u16 cba);
#endif

static void ClearBackGVramCache();

static s32 BackGMag2(s32 x, s32 y);
static s32 BackGCartesianToPolar(s32* outMagnitude, s32* outAngle, s32 y, s32 x);
static const u8* ResolveBackGSpritePacket(const tPrimGeom* geo);
static bool BuildBackGDynamicEntry(OriginalGeo* sourceGeo, BackGGeoEntry* entry);

static s16 ClampFloatToS16(f32 value) {
    s32 iv = static_cast<s32>(value);
    if (iv < -32768) {
        iv = -32768;
    }
    else if (iv > 32767) {
        iv = 32767;
    }

    return static_cast<s16>(iv);
}

static u8 ClampFloatToU8(f32 value) {
    s32 iv = static_cast<s32>(value);
    if (iv < 0) {
        iv = 0;
    }
    else if (iv > 255) {
        iv = 255;
    }

    return static_cast<u8>(iv);
}

static u16 ClampFloatToU16(f32 value) {
    s32 iv = static_cast<s32>(value);
    if (iv < 0) {
        iv = 0;
    }
    else if (iv > 65535) {
        iv = 65535;
    }

    return static_cast<u16>(iv);
}

static bool BuildBackGDynamicEntry(OriginalGeo* sourceGeo, BackGGeoEntry* entry) {
    if (!sourceGeo || !entry || !sourceGeo->dynamicVerts || sourceGeo->dynamicPrimCount == 0) {
        return false;
    }

    if (!sourceGeo->dynamicPrimStart || !sourceGeo->dynamicPrimVertCount || !sourceGeo->dynamicPrimCmd) {
        return false;
    }

    const u32 primStart = sourceGeo->dynamicPrimStart[0];
    const u32 primVertCount = sourceGeo->dynamicPrimVertCount[0];
    if (primVertCount < 4 || primStart + primVertCount > sourceGeo->dynamicVertCount) {
        return false;
    }

    bool haveVertex[4] = { false, false, false, false };
    bool haveUv[4] = { false, false, false, false };
    for (u32 i = 0; i < primVertCount; i++) {
        const u32 dynamicIndex = primStart + i;
        u16 sourceIndex = static_cast<u16>(i);
        if (sourceGeo->dynamicVertSourceIndex && dynamicIndex < sourceGeo->dynamicVertCount) {
            sourceIndex = sourceGeo->dynamicVertSourceIndex[dynamicIndex];
        }

        if (sourceIndex >= 4 || haveVertex[sourceIndex]) {
            continue;
        }

        const GeoRenderVertex& vertex = sourceGeo->dynamicVerts[dynamicIndex];
        entry->vx[sourceIndex] = ClampFloatToS16(vertex.x);
        entry->vy[sourceIndex] = ClampFloatToS16(vertex.y);
        entry->uByIndex[sourceIndex] = ClampFloatToU8(vertex.u);
        entry->vByIndex[sourceIndex] = ClampFloatToU8(vertex.v);

        if (sourceGeo->dynamicColorList && sourceIndex < sourceGeo->dynamicColorCount) {
            entry->color[sourceIndex] = sourceGeo->dynamicColorList[sourceIndex] & 0x00FFFFFFu;
        }
        else {
            const u8 cr = ClampFloatToU8(vertex.r * 128.0f);
            const u8 cg = ClampFloatToU8(vertex.g * 128.0f);
            const u8 cb = ClampFloatToU8(vertex.b * 128.0f);
            entry->color[sourceIndex] = static_cast<u32>(cr)
                                       | (static_cast<u32>(cg) << 8)
                                       | (static_cast<u32>(cb) << 16);
        }

        haveVertex[sourceIndex] = true;
        haveUv[sourceIndex] = true;
    }

    if (!(haveVertex[0] && haveVertex[1] && haveVertex[2] && haveVertex[3])) {
        return false;
    }

    if (!(haveUv[0] && haveUv[1] && haveUv[2] && haveUv[3])) {
        return false;
    }

    const u8 primCmd = static_cast<u8>(sourceGeo->dynamicPrimCmd[0] & 0xFCu);
    entry->drawAsPolyG4 = (primCmd == 0x38u || primCmd == 0x30u) ? 1u : 0u;
    entry->useDynamic = 1;
    entry->sourceGeo = sourceGeo;

    if (entry->drawAsPolyG4 != 0u) {
        return true;
    }

    const GeoRenderVertex& firstPrimVertex = sourceGeo->dynamicVerts[primStart];
    entry->tpage = ClampFloatToU16(firstPrimVertex.tpage);
    entry->cba = ClampFloatToU16(firstPrimVertex.cba);
    return true;
}

static s32 BackGMag2(s32 x, s32 y) {
    const f64 dx = static_cast<f64>(x);
    const f64 dy = static_cast<f64>(y);
    return static_cast<s32>(std::sqrt(dx * dx + dy * dy));
}

// PSX rmCartesianToPolar semantics:
// outMagnitude = rmMag2(x, y), outAngle = rmATan216(x, y), return outAngle.
static s32 BackGCartesianToPolar(s32* outMagnitude, s32* outAngle, s32 y, s32 x) {
    const s32 magnitude = BackGMag2(x, y);
    const s32 angle = static_cast<s32>(static_cast<s16>(rmATan216(x, y)));

    if (outMagnitude) {
        *outMagnitude = magnitude;
    }
    if (outAngle) {
        *outAngle = angle;
    }

    return angle;
}

static void ClearBackGVramCache() {
#if !defined(RC_PLATFORM_PSP)
    for (s32 i = 0; i < kBackGVramCacheEntries; i++) {
        if (s_backgVramCache[i].texture) {
            delete s_backgVramCache[i].texture;
            s_backgVramCache[i].texture = nullptr;
        }

        s_backgVramCache[i].world = nullptr;
        s_backgVramCache[i].tpage = 0;
        s_backgVramCache[i].cba = 0;
    }

    s_backgVramCacheNext = 0;
#endif
}

#if !defined(RC_PLATFORM_PSP)
static tTexture* GetBackGVramTexture(const World* world, u16 tpage, u16 cba) {
    if (!world) {
        return nullptr;
    }

    for (s32 i = 0; i < kBackGVramCacheEntries; i++) {
        const BackGVramCacheEntry& entry = s_backgVramCache[i];
        if (entry.texture && entry.world == world && entry.tpage == tpage && entry.cba == cba) {
            return entry.texture;
        }
    }

    world->GetVRAM().DecodePage(tpage, cba, s_backgDecodePageRGBA);

    tTexture* tex = new tTexture();
    if (!tex || !tex->Create(256, 256, 32, 8, s_backgDecodePageRGBA)) {
        delete tex;
        return nullptr;
    }

    BackGVramCacheEntry* slot = nullptr;
    for (s32 i = 0; i < kBackGVramCacheEntries; i++) {
        if (!s_backgVramCache[i].texture) {
            slot = &s_backgVramCache[i];
            break;
        }
    }

    if (!slot) {
        slot = &s_backgVramCache[s_backgVramCacheNext];
        s_backgVramCacheNext = (s_backgVramCacheNext + 1) % kBackGVramCacheEntries;
        if (slot->texture) {
            delete slot->texture;
            slot->texture = nullptr;
        }
    }

    slot->world = world;
    slot->tpage = tpage;
    slot->cba = cba;
    slot->texture = tex;
    return tex;
}
#endif

static const u8* ResolveBackGSpritePacket(const tPrimGeom* geo) {
    if (!geo || !geo->primList) {
        return nullptr;
    }

    const u8* packet = geo->primList;
    if ((packet[7] & 0xFCu) == 0x64u) {
        return packet;
    }

    if (!geo->ownedRawData || !geo->ownedRawSize) {
        return packet;
    }

    const u32 pointerWord = p3dReadU32LE(geo->primList);
    if (pointerWord < geo->ownedRawSize) {
        const u8* candidate = geo->ownedRawData + pointerWord;
        if ((candidate[7] & 0xFCu) == 0x64u) {
            return candidate;
        }
    }

    const u32 pointerWordBytes = pointerWord << 2;
    if (pointerWordBytes < geo->ownedRawSize) {
        const u8* candidate = geo->ownedRawData + pointerWordBytes;
        if ((candidate[7] & 0xFCu) == 0x64u) {
            return candidate;
        }
    }

    return packet;
}

// PSX: __5BackG (BACKG.CPP:164, 0x80057D4C)
BackG::BackG() {
    MARKFUNCTION(0x80057D4C);

    field10 = 0;
    field0 = nullptr;

    s_backgWork.fieldED4 = 0x80;
    s_backgWork.fieldED5 = 0x80;
    s_backgWork.fieldED6 = 0x80;

    field0 = new BackGGeoEntry[kBackGMaxEntries]();
    if (!field0) {
        return;
    }

    s16 xVals[4] = {};
    s16 yVals[4] = {};
    for (s32 bgIndex = 0; bgIndex < kBackGMaxEntries; bgIndex++) {
        char name[0x20] = {};
        std::snprintf(name, sizeof(name), "BG%.2d", bgIndex);

        OriginalBasic* geoBasic = g_levelManager ? g_levelManager->FindGeo(static_cast<s32>(p3dHash(name))) : nullptr;
        if (!geoBasic) {
            continue;
        }

        OriginalGeo* originalGeo = static_cast<OriginalGeo*>(geoBasic);
        if (!originalGeo) {
            continue;
        }

        BackGGeoEntry* entry = &field0[field10];
        *entry = BackGGeoEntry();
        entry->sourceGeo = originalGeo;
        entry->geo = originalGeo->primGeom;
        entry->field4 = 0;

        bool haveVertexData = false;
        if (entry->geo && entry->geo->primList) {
            const u8* vertices = entry->geo->GetVertexList();
            if (vertices) {
                for (s32 i = 0; i < 4; i++) {
                    entry->vx[i] = p3dReadS16LE(vertices + i * 8 + 0);
                    entry->vy[i] = p3dReadS16LE(vertices + i * 8 + 2);
                    xVals[i] = static_cast<s16>(-(static_cast<s32>(entry->vx[i]) / 8));
                    yVals[i] = static_cast<s16>(-(static_cast<s32>(entry->vy[i]) / 8));
                }
                haveVertexData = true;
            }
        }

        if (!haveVertexData) {
            if (!BuildBackGDynamicEntry(originalGeo, entry)) {
                continue;
            }

            for (s32 i = 0; i < 4; i++) {
                xVals[i] = static_cast<s16>(-(static_cast<s32>(entry->vx[i]) / 8));
                yVals[i] = static_cast<s16>(-(static_cast<s32>(entry->vy[i]) / 8));
            }
        }

        entry->field8 = 0;
        for (u16 i = 1; i < 4; i++) {
            if (xVals[entry->field8] < xVals[i] || yVals[entry->field8] < yVals[i]) {
                entry->field8 = i;
            }
        }

        entry->fieldE = 3;
        for (u16 i = 0; i < 3; i++) {
            if (xVals[entry->fieldE] > xVals[i] || yVals[entry->fieldE] > yVals[i]) {
                entry->fieldE = i;
            }
        }

        entry->fieldA = entry->fieldE;
        entry->fieldC = entry->field8;
        for (u16 i = 0; i < 4; i++) {
            if (xVals[i] == xVals[entry->fieldE] && i != entry->fieldE) {
                entry->fieldA = i;
            }

            if (xVals[i] == xVals[entry->field8] && i != entry->field8) {
                entry->fieldC = i;
            }
        }

        if (field10 == 0) {
            field14 = static_cast<s32>(xVals[entry->fieldE]) - static_cast<s32>(xVals[entry->field8]);
            field18 = static_cast<s32>(yVals[entry->fieldE]) - static_cast<s32>(yVals[entry->field8]);
        }

        field10 += 1;
    }
}

// PSX: ___5BackG (BACKG.CPP:253, 0x8005805C)
BackG::~BackG() {
    MARKFUNCTION(0x8005805C);

    delete[] field0;
    field0 = nullptr;
    field10 = 0;
}

// PSX: InitBG__5BackG (BACKG.CPP:263, 0x800580B4)
void BackG::InitBG() {
    MARKFUNCTION(0x800580B4);
    if (!s_backg) {
        s_backg = new BackG();
    }
}

// PSX: LoadBG__5BackG (BACKG.CPP:269, 0x800580DC)
void BackG::LoadBG() {
    MARKFUNCTION(0x800580DC);

    ClearBackGVramCache();

    bool found = false;
    for (DBPoint* point = g_database ? g_database->GetFirstPoint() : nullptr;
         point;
         point = static_cast<DBPoint*>(point->next)) {
        // PSX: type==0x32 && subType==0x32 gate.
        if (point->type != 0x32 || point->subType != 0x32) {
            continue;
        }

        for (u32 i = 0; i < point->attribCount; i++) {
            const DBAttrib* attrib = point->GetAttribByIndex(i);
            if (!attrib) {
                continue;
            }

            switch (attrib->id) {
                case 0x10:
                    s_backgWork.fieldEE0 = static_cast<s32>(attrib->value);
                    break;
                case 0x11:
                    s_backgWork.fieldEC4 = static_cast<s32>(attrib->value);
                    break;
                case 0x12:
                    s_backgWork.fieldEC8 = static_cast<s32>(attrib->value);
                    break;
                case 0x13:
                    s_backgWork.fieldED0 = static_cast<u16>(attrib->value & 0xFFFF);
                    break;
                case 0x14:
                    s_backgWork.fieldECC = static_cast<s32>(attrib->value);
                    break;
                case 0x15:
                    s_backgWork.fieldED8 = static_cast<u16>(attrib->value & 0xFFFF);
                    break;
                case 0x16:
                    s_backgWork.fieldEC0 = static_cast<s32>(attrib->value);
                    break;
                case 0x17:
                    s_backgWork.fieldEE4 = static_cast<u8>(attrib->value & 0xFF);
                    break;
                case 0x18:
                    s_backgWork.fieldEE5 = static_cast<u8>(attrib->value & 0xFF);
                    break;
                case 0x19:
                    s_backgWork.fieldEE6 = static_cast<u8>(attrib->value & 0xFF);
                    break;
                case 0x1A:
                    s_backgWork.fieldEDC = static_cast<s32>(attrib->value);
                    break;
                case 0x1B:
                    s_backgWork.fieldED4 = static_cast<u8>(attrib->value & 0xFF);
                    break;
                case 0x1C:
                    s_backgWork.fieldED5 = static_cast<u8>(attrib->value & 0xFF);
                    break;
                case 0x1D:
                    s_backgWork.fieldED6 = static_cast<u8>(attrib->value & 0xFF);
                    break;
                default:
                    break;
            }
        }

        found = true;
        break;
    }

    // PSX fallback defaults when no matching DB point is present.
    if (!found) {
        s_backgWork.fieldEC8 = 0;
        s_backgWork.fieldED0 = 0x14;
        s_backgWork.fieldECC = 0x2FB;
        s_backgWork.fieldED8 = 0x23;
        s_backgWork.fieldEC0 = 0x140;
    }

    s_backgRuntime.field614 = s_backgWork.fieldEE0;
    s_backgRuntime.field618 = s_backgWork.fieldEC4;
    s_backgRuntime.field61C = s_backgWork.fieldEC8;
    s_backgRuntime.field620 = s_backgWork.fieldED0;
    s_backgRuntime.field624 = s_backgWork.fieldECC;
    s_backgRuntime.field628 = s_backgWork.fieldED8;
    s_backgRuntime.field62C = s_backgWork.fieldEC0;

    if (s_backgWork.fieldEDC != 0) {
        s_backgRuntime.field630 = s_backgWork.fieldED4;
        s_backgRuntime.field631 = s_backgWork.fieldED5;
        s_backgRuntime.field632 = s_backgWork.fieldED6;
    }
    else {
        s_backgRuntime.field630 = s_backgWork.fieldEE4;
        s_backgRuntime.field631 = s_backgWork.fieldEE5;
        s_backgRuntime.field632 = s_backgWork.fieldEE6;
    }
}

// PSX: DeleteBG__5BackG (BACKG.CPP:379, 0x80058370)
void BackG::DeleteBG() {
    MARKFUNCTION(0x80058370);

    delete s_backg;
    s_backg = nullptr;

    ClearBackGVramCache();
}

// PSX: GetScrollY__5BackGl (BACKG.CPP:392, 0x8005839C)
s32 BackG::GetScrollY(s32 angle) {
    MARKFUNCTION(0x8005839C);

    const s32 divisor = static_cast<s16>(s_backgWork.fieldED8);
    if (divisor == 0) {
        return 0;
    }

    const s32 scroll = (angle + s_backgWork.fieldECC) / divisor;
    return static_cast<s16>(scroll);
}

// PSX: Draw__5BackG (BACKG.CPP:429, 0x80058448)
s32 BackG::Draw() {
    MARKFUNCTION(0x80058448);

    if (!field0) {
        return 1;
    }

    s32 absX = field4;
    if (absX < 0) {
        absX = -absX;
    }

    s32 absZ = fieldC;
    if (absZ < 0) {
        absZ = -absZ;
    }

    const s32 magnitudeXZ = BackGMag2(absX, absZ);

    s32 polarMagnitude = 0;
    s32 pitchAngle = 0;
    BackGCartesianToPolar(&polarMagnitude, &pitchAngle, field8, magnitudeXZ);
    const s32 scrollY = GetScrollY(pitchAngle);

    s32 yawAngle = 0;
    BackGCartesianToPolar(&polarMagnitude, &yawAngle, field4, fieldC);

    const s32 scrollDivisor = static_cast<s16>(s_backgWork.fieldED0);
    if (scrollDivisor == 0) {
        return 1;
    }

    const s32 scrollX = (yawAngle + s_backgWork.fieldEC8) / scrollDivisor;

    s32 cullMinX = -0x7F;
    s32 cullMaxX = 0x1FF;
#if FIX_ASPECT_RATIO
    {
        const f32 sx0 = SCALE_AND_CENTER_X(0.0f);
        const f32 sx1 = SCALE_AND_CENTER_X(1.0f);
        const f32 dx = sx1 - sx0;
        if (std::fabs(dx) > 0.0001f) {
            f32 visibleMinX = (0.0f - sx0) / dx;
            f32 visibleMaxX = (SCREEN_WIDTH - sx0) / dx;
            if (visibleMinX > visibleMaxX) {
                const f32 tmp = visibleMinX;
                visibleMinX = visibleMaxX;
                visibleMaxX = tmp;
            }

            const s32 wideMin = static_cast<s32>(std::floor(visibleMinX));
            const s32 wideMax = static_cast<s32>(std::ceil(visibleMaxX));
            if (wideMin < cullMinX) {
                cullMinX = wideMin;
            }
            if (wideMax > cullMaxX) {
                cullMaxX = wideMax;
            }
        }
    }
#endif

    const bool needsWideWrapCull = (cullMinX < -0x7F || cullMaxX > 0x1FF);

    for (u32 i = 0; i < field10; i++) {
        BackGGeoEntry* entry = &field0[i];
        if (!entry->useDynamic && !entry->geo) {
            continue;
        }

        const bool drawAsPoly = (entry->useDynamic != 0u)
            ? (entry->drawAsPolyG4 != 0u)
            : (entry->geo && entry->geo->geoType == 7);

        s32 dynamicW = 0;
        s32 dynamicH = 0;
        bool haveDynamicExtents = false;
        if (entry->useDynamic != 0u) {
            const u16 idx8 = entry->field8 & 3u;
            const u16 idxA = entry->fieldA & 3u;
            const u16 idxC = entry->fieldC & 3u;

            const s32 x8 = -(static_cast<s32>(entry->vx[idx8]) / 8);
            const s32 xA = -(static_cast<s32>(entry->vx[idxA]) / 8);
            const s32 y8 = -(static_cast<s32>(entry->vy[idx8]) / 8);
            const s32 yC = -(static_cast<s32>(entry->vy[idxC]) / 8);

            dynamicW = xA - x8;
            dynamicH = yC - y8;
            haveDynamicExtents = (dynamicW != 0 && dynamicH != 0);
        }

        const u16 idx8 = entry->field8 & 3u;
        const u16 idxA = entry->fieldA & 3u;
        const u16 idxC = entry->fieldC & 3u;

        const s32 x8 = -(static_cast<s32>(entry->vx[idx8]) / 8);
        const s32 xA = -(static_cast<s32>(entry->vx[idxA]) / 8);
        const s32 y8 = -(static_cast<s32>(entry->vy[idx8]) / 8);
        const s32 yC = -(static_cast<s32>(entry->vy[idxC]) / 8);

        const s32 entryW = xA - x8;
        const s32 entryH = yC - y8;
        const bool haveEntryExtents = (entryW != 0 && entryH != 0);

        const s16 vx = entry->vx[entry->field8 & 3u];
        const s16 vy = entry->vy[entry->field8 & 3u];

        const s32 baseX = 0x100 - (static_cast<s32>(vx) / 8);
        const s32 baseY = 0x80 - (static_cast<s32>(vy) / 8);

        const s32 wrappedX = (baseX - scrollX + 0x80) & 0x3FF;
        const s32 drawX = wrappedX - 0x80;
        const s16 drawY = static_cast<s16>(baseY + scrollY);

        if (entry->useDynamic != 0u && !haveDynamicExtents) {
            continue;
        }

        const bool useWrapCandidates = (entry->useDynamic != 0u) || needsWideWrapCull;
        if (useWrapCandidates) {
            const s32 drawXCandidates[3] = {
                drawX,
                drawX - 0x400,
                drawX + 0x400,
            };

            for (s32 candidateIndex = 0; candidateIndex < 3; candidateIndex++) {
                const s32 candidateX = drawXCandidates[candidateIndex];

                const s32 x0 = candidateX;
                const s32 y0 = drawY;
                const s32 x1 = x0 + entryW;
                const s32 y1 = y0 + entryH;

                const s32 minX = (x0 < x1) ? x0 : x1;
                const s32 maxX = (x0 > x1) ? x0 : x1;
                const s32 minY = (y0 < y1) ? y0 : y1;
                const s32 maxY = (y0 > y1) ? y0 : y1;

                if (maxX < cullMinX || minX > cullMaxX || maxY < -0x7F || minY >= 0x100) {
                    continue;
                }

                if (drawAsPoly) {
                    DrawPolyG4(entry, static_cast<s16>(candidateX), drawY);
                }
                else {
                    DrawSprite(entry, static_cast<s16>(candidateX), drawY);
                }
            }

            continue;
        }

        if (entry->useDynamic == 0u && (drawX < cullMinX || drawX > cullMaxX)) {
            continue;
        }

        if (drawAsPoly) {
            if (!haveEntryExtents && (drawY < -0x7F || drawY >= 0x100)) {
                continue;
            }

            DrawPolyG4(entry, static_cast<s16>(drawX), drawY);
        }
        else {
            if (!haveEntryExtents && (drawY < -0x7F || drawY >= 0x100)) {
                continue;
            }

            DrawSprite(entry, static_cast<s16>(drawX), drawY);
        }
    }

    return 1;
}

// PSX: DrawSprite__5BackGP5BGGEOii (BACKG.CPP:480, 0x80058660)
s32 BackG::DrawSprite(BackGGeoEntry* entry, s16 x, s16 y) {
    MARKFUNCTION(0x80058660);

    if (!entry) {
        return 0;
    }

    u16 cba = 0;
    u16 tpage = 0;
    u8 u = 0;
    u8 v = 0;
    s16 w = 0;
    s16 h = 0;
    u8 u1Byte = 0;
    u8 v1Byte = 0;
    s16 drawX = x;
    s16 drawY = y;

    if (entry->useDynamic != 0u) {
        cba = entry->cba;
        tpage = entry->tpage;

        const u16 idx8 = entry->field8 & 3u;
        const u16 idxA = entry->fieldA & 3u;
        const u16 idxC = entry->fieldC & 3u;
        const u16 idxE = entry->fieldE & 3u;

        const s32 x8 = -(static_cast<s32>(entry->vx[idx8]) / 8);
        const s32 xA = -(static_cast<s32>(entry->vx[idxA]) / 8);
        const s32 y8 = -(static_cast<s32>(entry->vy[idx8]) / 8);
        const s32 yC = -(static_cast<s32>(entry->vy[idxC]) / 8);

        w = static_cast<s16>(xA - x8);
        h = static_cast<s16>(yC - y8);
        if (w == 0 || h == 0) {
            return 0;
        }

        u = entry->uByIndex[idx8];
        v = entry->vByIndex[idx8];
        u1Byte = entry->uByIndex[idxE];
        v1Byte = entry->vByIndex[idxE];
    }
    else {
        if (!entry->geo) {
            return 0;
        }

        const u8* packet = ResolveBackGSpritePacket(entry->geo);
        if (!packet) {
            return 0;
        }

        const u32 tpageAndCba = p3dReadU32LE(packet + 0x14);
        const u32 uvWord = p3dReadU32LE(packet + 0x18);
        const u32 sizeWord = p3dReadU32LE(packet + 0x1C);

        cba = static_cast<u16>(tpageAndCba & 0xFFFFu);
        tpage = static_cast<u16>(tpageAndCba >> 16);
        u = static_cast<u8>(uvWord & 0xFFu);
        v = static_cast<u8>((uvWord >> 16) & 0xFFu);
        w = static_cast<s16>(sizeWord & 0xFFFFu);
        h = static_cast<s16>((sizeWord >> 16) & 0xFFFFu);
        u1Byte = static_cast<u8>(u + w);
        v1Byte = static_cast<u8>(v + h);
    }

    if (w == 0 || h == 0) {
        return 0;
    }

    if (w < 0) {
        drawX = static_cast<s16>(drawX + w);
        w = static_cast<s16>(-w);

        const u8 tmp = u;
        u = u1Byte;
        u1Byte = tmp;
    }

    if (h < 0) {
        drawY = static_cast<s16>(drawY + h);
        h = static_cast<s16>(-h);

        const u8 tmp = v;
        v = v1Byte;
        v1Byte = tmp;
    }

    World* world = g_game ? g_game->GetWorld() : nullptr;
    if (!world) {
        return 0;
    }

    const f32 nx = SCALE_AND_CENTER_X(static_cast<f32>(drawX));
    const f32 ny = SCREEN_SCALE_Y(static_cast<f32>(drawY));
    const f32 nw = SCALE_AND_CENTER_X(static_cast<f32>(drawX) + static_cast<f32>(w)) - nx;
    const f32 nh = SCREEN_SCALE_Y(static_cast<f32>(drawY) + static_cast<f32>(h)) - ny;

#if defined(RC_PLATFORM_PSP)
    // The desktop cache allocates a full RGBA page per palette. Under PSP
    // heap pressure Create fails and every visible sprite decodes it again.
    // Background runs before world drawing, outside ScreenDraw batches.
    static_cast<pspContext*>(p3d::context)->DrawVRAMQuad(
        world->GetVRAMHandle(), tpage, cba, nx, ny, nw, nh, u, v, u1Byte, v1Byte,
        pddiColour(PsxColMod128To255(s_backgWork.fieldED4),
                   PsxColMod128To255(s_backgWork.fieldED5),
                   PsxColMod128To255(s_backgWork.fieldED6), 255));
#else
    tTexture* texture = GetBackGVramTexture(world, tpage, cba);
    if (!texture) {
        return 0;
    }

    const f32 u0 = static_cast<f32>(u) / 256.0f;
    const f32 v0 = static_cast<f32>(v) / 256.0f;
    const f32 u1 = static_cast<f32>(u1Byte) / 256.0f;
    const f32 v1 = static_cast<f32>(v1Byte) / 256.0f;

    ScreenDraw::DrawQuad(
        texture,
        nx,
        ny,
        nw,
        nh,
        u0,
        v0,
        u1,
        v1,
        PsxColMod128To255(s_backgWork.fieldED4),
        PsxColMod128To255(s_backgWork.fieldED5),
        PsxColMod128To255(s_backgWork.fieldED6),
        255);
#endif

    return 1;
}

// PSX: DrawPolyG4__5BackGP5BGGEOss (BACKG.CPP:511, 0x8005878C)
s32 BackG::DrawPolyG4(BackGGeoEntry* entry, s16 x, s16 y) {
    MARKFUNCTION(0x8005878C);

    if (!entry) {
        return 0;
    }

    u32 color0 = 0;
    u32 color1 = 0;
    u32 color2 = 0;
    u32 color3 = 0;
    if (entry->useDynamic != 0u) {
        color0 = entry->color[entry->field8 & 3u];
        color1 = entry->color[entry->fieldA & 3u];
        color2 = entry->color[entry->fieldC & 3u];
        color3 = entry->color[entry->fieldE & 3u];
    }
    else {
        if (!entry->geo || !entry->geo->primListAlt) {
            return 0;
        }

        const u8* colorTable = entry->geo->primListAlt;
        color0 = p3dReadU32LE(colorTable + static_cast<u32>(entry->field8) * 4u);
        color1 = p3dReadU32LE(colorTable + static_cast<u32>(entry->fieldA) * 4u);
        color2 = p3dReadU32LE(colorTable + static_cast<u32>(entry->fieldC) * 4u);
        color3 = p3dReadU32LE(colorTable + static_cast<u32>(entry->fieldE) * 4u);
    }

    const s32 x0 = static_cast<s32>(x);
    const s32 y0 = static_cast<s32>(y);
    const s32 x1 = x0 + field14;
    const s32 y1 = y0;
    const s32 x2 = x0;
    const s32 y2 = y0 + field18;
    const s32 x3 = x1;
    const s32 y3 = y2;

    ScreenDraw::DrawGouraudQuad(
        SCALE_AND_CENTER_X(static_cast<f32>(x0)), SCREEN_SCALE_Y(static_cast<f32>(y0)),
        static_cast<u8>(color0 & 0xFFu), static_cast<u8>((color0 >> 8) & 0xFFu), static_cast<u8>((color0 >> 16) & 0xFFu), 255,
        SCALE_AND_CENTER_X(static_cast<f32>(x1)), SCREEN_SCALE_Y(static_cast<f32>(y1)),
        static_cast<u8>(color1 & 0xFFu), static_cast<u8>((color1 >> 8) & 0xFFu), static_cast<u8>((color1 >> 16) & 0xFFu), 255,
        SCALE_AND_CENTER_X(static_cast<f32>(x2)), SCREEN_SCALE_Y(static_cast<f32>(y2)),
        static_cast<u8>(color2 & 0xFFu), static_cast<u8>((color2 >> 8) & 0xFFu), static_cast<u8>((color2 >> 16) & 0xFFu), 255,
        SCALE_AND_CENTER_X(static_cast<f32>(x3)), SCREEN_SCALE_Y(static_cast<f32>(y3)),
        static_cast<u8>(color3 & 0xFFu), static_cast<u8>((color3 >> 8) & 0xFFu), static_cast<u8>((color3 >> 16) & 0xFFu), 255);

    return 1;
}

// PSX: DrawBG__5BackG (BACKG.CPP:415, 0x800583E4)
void BackG::DrawBG() {
    MARKFUNCTION(0x800583E4);

    if (!Player::s_player || !g_blockManager) {
        return;
    }

    Block* block = g_blockManager->GetBlock(Player::s_player->blockNum);
    if (!block) {
        return; // PC safety guard for out-of-range index; PSX indexes unconditionally
    }

    if (block->unk60 != 0) {
        return;
    }

    if (s_backg) {
        s_backg->UpdateBG();
        s_backg->Draw();
    }
}

// PSX: UpdateSplat__5BackG (BACKG.CPP:540, 0x800588B4)
void BackG::UpdateSplat() {
    MARKFUNCTION(0x800588B4);

    s_backgWork.fieldEE0 = s_backgRuntime.field614;
    s_backgWork.fieldEC4 = s_backgRuntime.field618;
    s_backgWork.fieldEC8 = s_backgRuntime.field61C;
    s_backgWork.fieldED0 = s_backgRuntime.field620;
    s_backgWork.fieldECC = s_backgRuntime.field624;
    s_backgWork.fieldED8 = s_backgRuntime.field628;
    s_backgWork.fieldEC0 = s_backgRuntime.field62C;

    if (s_backgWork.fieldEDC != 0) {
        s_backgWork.fieldED4 = s_backgRuntime.field630;
        s_backgWork.fieldED5 = s_backgRuntime.field631;
        s_backgWork.fieldED6 = s_backgRuntime.field632;
    }

    s_backgWork.fieldEE4 = s_backgRuntime.field630;
    s_backgWork.fieldEE5 = s_backgRuntime.field631;
    s_backgWork.fieldEE6 = s_backgRuntime.field632;
}

// PSX: UpdateBG__5BackG (BACKG.CPP:567, 0x80058938)
void BackG::UpdateBG() {
    MARKFUNCTION(0x80058938);

    UpdateSplat();
    GetCamVect();
}

// PSX: GetCamVect__5BackG (BACKG.CPP:576, 0x8005896C)
void BackG::GetCamVect() {
    MARKFUNCTION(0x8005896C);

    if (!g_display || !g_display->GetCamera()) {
        return;
    }

    const LVector cameraVector = g_display->GetCamera()->GetCameraVector();
    field4 = cameraVector.x * 0x10000;
    field8 = -cameraVector.y * 0x10000;
    fieldC = -cameraVector.z * 0x10000;
}

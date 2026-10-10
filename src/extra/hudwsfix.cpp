#include "hudwsfix.h"

#if FIX_ASPECT_RATIO

#include "fe/oxovl.h"
#include "gen/common.h"
#include "gen/game.h"
#include "gen/world.h"
#include "xclib/xclib.h"
#include "p3d/context.h"
#include "pddi/pddidev.h"
#include "pc/tim.h"

s32 HUDWidescreenPatches::RoundToNearestS32(f32 value) {
    return (value >= 0.0f) ? static_cast<s32>(value + 0.5f) : static_cast<s32>(value - 0.5f);
}

s32 HUDWidescreenPatches::GetShiftX() {
    const f32 scaledWidth = SCREEN_SCALE_X(static_cast<f32>(DEFAULT_SCREEN_WIDTH));
    const f32 targetOffsetPx = (SCREEN_EFFECTIVE_WIDTH - scaledWidth) * 0.5f;
    if (targetOffsetPx <= 0.0f) {
        return 0;
    }

    const f32 screenUnitsPerHudPixel = SCREEN_SCALE_X(1.0f);
    if (screenUnitsPerHudPixel <= 0.0f) {
        return 0;
    }

    return RoundToNearestS32(targetOffsetPx / screenUnitsPerHudPixel);
}

bool HUDWidescreenPatches::GetOverlayBoundsX(xcOverlayData* overlay, u8* rawData, s32& outMinX, s32& outMaxX) {
    if (!overlay || !rawData) {
        return false;
    }

    const xcOverlayItem* items = overlay->GetItems();
    bool found = false;
    s32 minX = 0;
    s32 maxX = 0;

    auto trackX = [&](s32 x) {
        if (!found) {
            minX = x;
            maxX = x;
            found = true;
            return;
        }

        if (x < minX) {
            minX = x;
        }
        if (x > maxX) {
            maxX = x;
        }
    };

    auto trackPolyBounds = [&](const auto* poly) {
        s16 bx0 = 0;
        s16 by0 = 0;
        s16 bx1 = 0;
        s16 by1 = 0;
        poly->GetBounds(bx0, by0, bx1, by1);
        trackX(bx0);
        trackX(bx1);
    };

    for (u32 i = 0; i < overlay->primCount; i++) {
        u8* primData = rawData + items[i].dataOffset;
        xcPrimHeader* hdr = reinterpret_cast<xcPrimHeader*>(primData);
        if (hdr->subtype == 5) {
            continue;
        }

        if (hdr->type == XC_PRIM_POLYF4) {
            trackPolyBounds(reinterpret_cast<const xcPolyF4Prim*>(primData));
            continue;
        }
        if (hdr->type == XC_PRIM_POLYG4) {
            trackPolyBounds(reinterpret_cast<const xcPolyG4Prim*>(primData));
            continue;
        }

        oxOvl helper;
        helper.overlay = overlay;
        s16 x = 0;
        s16 y = 0;
        if (helper.GetPrimPos(primData, x, y)) {
            trackX(x);
        }
    }

    if (!found) {
        return false;
    }

    outMinX = minX;
    outMaxX = maxX;
    return true;
}

s32 HUDWidescreenPatches::ClassifyOverlayAnchorX(xcOverlayData* overlay, u8* rawData) {
    s32 minX = 0;
    s32 maxX = 0;
    if (!GetOverlayBoundsX(overlay, rawData, minX, maxX)) {
        return 0;
    }

    const s32 centerX = (minX + maxX) / 2;
    const s32 leftThreshold = static_cast<s32>(DEFAULT_SCREEN_WIDTH * 3.0f / 8.0f);
    const s32 rightThreshold = static_cast<s32>(DEFAULT_SCREEN_WIDTH * 5.0f / 8.0f);
    if (centerX <= leftThreshold) {
        return -1;
    }
    if (centerX >= rightThreshold) {
        return 1;
    }

    return 0;
}

void HUDWidescreenPatches::ShiftOverlayX(xcOverlayData* overlay, u8* rawData, s32 deltaX) {
    if (!overlay || !rawData || deltaX == 0) {
        return;
    }

    const xcOverlayItem* items = overlay->GetItems();
    auto shiftPoly = [&](auto* poly) {
        poly->x0 = static_cast<s16>(poly->x0 + deltaX);
        poly->x1 = static_cast<s16>(poly->x1 + deltaX);
        poly->x2 = static_cast<s16>(poly->x2 + deltaX);
        poly->x3 = static_cast<s16>(poly->x3 + deltaX);
    };

    for (u32 i = 0; i < overlay->primCount; i++) {
        u8* primData = rawData + items[i].dataOffset;
        xcPrimHeader* hdr = reinterpret_cast<xcPrimHeader*>(primData);
        if (hdr->subtype == 5) {
            continue;
        }

        if (hdr->type == XC_PRIM_POLYF4) {
            shiftPoly(reinterpret_cast<xcPolyF4Prim*>(primData));
            continue;
        }
        if (hdr->type == XC_PRIM_POLYG4) {
            shiftPoly(reinterpret_cast<xcPolyG4Prim*>(primData));
            continue;
        }

        oxOvl helper;
        helper.overlay = overlay;
        s16 x = 0;
        s16 y = 0;
        if (helper.GetPrimPos(primData, x, y)) {
            helper.SetPrimPos(primData, static_cast<s16>(x + deltaX), y);
        }
    }
}

bool HUDWidescreenPatches::StretchHubBottomBarsInOverlay(xcSection* section, xcOverlayData* ovl, u8* rawData, s16 leftX, s16 rightX) {
    if (!ovl || !rawData) {
        return false;
    }

    bool changed = false;
    const xcOverlayItem* items = ovl->GetItems();

    for (u32 i = 0; i < ovl->primCount; i++) {
        u8* primData = rawData + items[i].dataOffset;
        xcPrimHeader* hdr = reinterpret_cast<xcPrimHeader*>(primData);
        if (hdr->subtype == 5) {
            continue;
        }

        if (hdr->type == XC_PRIM_POLYF4) {
            xcPolyF4Prim* poly = reinterpret_cast<xcPolyF4Prim*>(primData);

            s16 minX = poly->x0;
            s16 maxX = poly->x0;
            s16 minY = poly->y0;
            s16 maxY = poly->y0;
            if (poly->x1 < minX) minX = poly->x1; if (poly->x1 > maxX) maxX = poly->x1;
            if (poly->x2 < minX) minX = poly->x2; if (poly->x2 > maxX) maxX = poly->x2;
            if (poly->x3 < minX) minX = poly->x3; if (poly->x3 > maxX) maxX = poly->x3;
            if (poly->y1 < minY) minY = poly->y1; if (poly->y1 > maxY) maxY = poly->y1;
            if (poly->y2 < minY) minY = poly->y2; if (poly->y2 > maxY) maxY = poly->y2;
            if (poly->y3 < minY) minY = poly->y3; if (poly->y3 > maxY) maxY = poly->y3;

            if (minY < static_cast<s16>(DEFAULT_SCREEN_HEIGHT - 96.0f) ||
                (maxX - minX) < static_cast<s16>(DEFAULT_SCREEN_WIDTH * 0.35f) ||
                (maxY - minY) < 8) {
                continue;
            }

            const s16 mid = static_cast<s16>((minX + maxX) / 2);
            poly->x0 = (poly->x0 <= mid) ? leftX : rightX;
            poly->x1 = (poly->x1 <= mid) ? leftX : rightX;
            poly->x2 = (poly->x2 <= mid) ? leftX : rightX;
            poly->x3 = (poly->x3 <= mid) ? leftX : rightX;
            changed = true;
            continue;
        }

        if (hdr->type == XC_PRIM_POLYG4) {
            xcPolyG4Prim* poly = reinterpret_cast<xcPolyG4Prim*>(primData);

            s16 minX = poly->x0;
            s16 maxX = poly->x0;
            s16 minY = poly->y0;
            s16 maxY = poly->y0;
            if (poly->x1 < minX) minX = poly->x1; if (poly->x1 > maxX) maxX = poly->x1;
            if (poly->x2 < minX) minX = poly->x2; if (poly->x2 > maxX) maxX = poly->x2;
            if (poly->x3 < minX) minX = poly->x3; if (poly->x3 > maxX) maxX = poly->x3;
            if (poly->y1 < minY) minY = poly->y1; if (poly->y1 > maxY) maxY = poly->y1;
            if (poly->y2 < minY) minY = poly->y2; if (poly->y2 > maxY) maxY = poly->y2;
            if (poly->y3 < minY) minY = poly->y3; if (poly->y3 > maxY) maxY = poly->y3;

            if (minY < static_cast<s16>(DEFAULT_SCREEN_HEIGHT - 96.0f) ||
                (maxX - minX) < static_cast<s16>(DEFAULT_SCREEN_WIDTH * 0.35f) ||
                (maxY - minY) < 8) {
                continue;
            }

            const s16 mid = static_cast<s16>((minX + maxX) / 2);
            poly->x0 = (poly->x0 <= mid) ? leftX : rightX;
            poly->x1 = (poly->x1 <= mid) ? leftX : rightX;
            poly->x2 = (poly->x2 <= mid) ? leftX : rightX;
            poly->x3 = (poly->x3 <= mid) ? leftX : rightX;
            changed = true;
        }
    }

    return changed;
}

void HUDWidescreenPatches::Clear() {
    anchorCount = 0;
}

void HUDWidescreenPatches::Rebuild(xcSection* section, u8* rawData) {
    Clear();
    if (!section || !rawData || !section->overlays) {
        return;
    }

    const xcInventory* inv = section->overlays;
    const xcInventoryItem* items = inv->GetItems();
    for (u32 i = 0; i < inv->itemCount && anchorCount < kMaxAnchors; i++) {
        OverlayAnchor& anchor = anchors[anchorCount++];
        anchor.overlay = reinterpret_cast<xcOverlayData*>(rawData + items[i].dataOffset);
        anchor.direction = static_cast<s8>(ClassifyOverlayAnchorX(anchor.overlay, rawData));
        anchor.appliedX = 0;
    }

    if (inv->itemCount > static_cast<u32>(kMaxAnchors)) {
        LOG("[HUD] widescreen anchors truncated: overlays=%u max=%d", inv->itemCount, kMaxAnchors);
    }
}

bool HUDWidescreenPatches::AnchorsLookPreShifted(u8* rawData) const {
    if (!rawData || anchorCount == 0) {
        return false;
    }

    const s32 shiftX = GetShiftX();
    if (shiftX <= 0) {
        return false;
    }

    const s32 threshold = (shiftX > 16) ? (shiftX / 4) : 4;
    bool leftShifted = false;
    bool rightShifted = false;

    for (s32 i = 0; i < anchorCount; i++) {
        const OverlayAnchor& anchor = anchors[i];
        if (!anchor.overlay || anchor.direction == 0) {
            continue;
        }

        s32 minX = 0;
        s32 maxX = 0;
        if (!GetOverlayBoundsX(anchor.overlay, rawData, minX, maxX)) {
            continue;
        }

        if (anchor.direction < 0 && minX < -threshold) {
            leftShifted = true;
        }
        if (anchor.direction > 0 && maxX > static_cast<s32>(DEFAULT_SCREEN_WIDTH) + threshold) {
            rightShifted = true;
        }

        if (leftShifted && rightShifted) {
            return true;
        }
    }

    return false;
}

void HUDWidescreenPatches::PrimeAppliedOffsetsIfNeeded(u8* rawData) {
    if (!rawData || anchorCount == 0) {
        return;
    }

    const s32 shiftX = GetShiftX();
    if (shiftX <= 0 || !AnchorsLookPreShifted(rawData)) {
        return;
    }

    for (s32 i = 0; i < anchorCount; i++) {
        OverlayAnchor& anchor = anchors[i];
        if (anchor.overlay && anchor.direction != 0) {
            anchor.appliedX = anchor.direction * shiftX;
        }
    }
}

void HUDWidescreenPatches::EnsureInitialized(xcSection* section, u8* rawData) {
    if (!rawData || anchorCount != 0) {
        return;
    }

    Rebuild(section, rawData);
    PrimeAppliedOffsetsIfNeeded(rawData);
}

void HUDWidescreenPatches::Initialize(xcSection* section, u8* rawData) {
    Clear();
    EnsureInitialized(section, rawData);
}

void HUDWidescreenPatches::Apply(u8* rawData) {
    if (!rawData || anchorCount == 0) {
        return;
    }

    const s32 shiftX = GetShiftX();
    for (s32 i = 0; i < anchorCount; i++) {
        OverlayAnchor& anchor = anchors[i];
        if (!anchor.overlay || anchor.direction == 0) {
            continue;
        }

        const s32 targetX = anchor.direction * shiftX;
        const s32 deltaX = targetX - anchor.appliedX;
        if (deltaX != 0) {
            ShiftOverlayX(anchor.overlay, rawData, deltaX);
            anchor.appliedX = targetX;
        }
    }
}

void HUDWidescreenPatches::Reset(u8* rawData) {
    if (!rawData || anchorCount == 0) {
        return;
    }

    for (s32 i = 0; i < anchorCount; i++) {
        OverlayAnchor& anchor = anchors[i];
        if (!anchor.overlay || anchor.appliedX == 0) {
            continue;
        }

        ShiftOverlayX(anchor.overlay, rawData, -anchor.appliedX);
        anchor.appliedX = 0;
    }
}

void HUDWidescreenPatches::FixHubPromptBottomBar(xcSection* section, u8* rawData) {
    if (!section || !rawData || !section->overlays) {
        return;
    }
    if (!g_game || !g_game->GetWorld() || g_game->GetWorld()->GetCurLevelID() != 7) {
        return;
    }

    const f32 screenUnitsPerHudPixel = SCREEN_SCALE_X(1.0f);
    if (screenUnitsPerHudPixel <= 0.0f) {
        return;
    }

    const f32 scaledWidth = SCREEN_SCALE_X(DEFAULT_SCREEN_WIDTH);
    const f32 centerOffsetPx = (SCREEN_WIDTH - scaledWidth) * 0.5f;
    const f32 leftPx = SCREEN_EFFECTIVE_OFFSET_X;
    const f32 rightPx = leftPx + SCREEN_EFFECTIVE_WIDTH;

    const s16 leftX = static_cast<s16>(RoundToNearestS32((leftPx - centerOffsetPx) / screenUnitsPerHudPixel));
    const s16 rightX = static_cast<s16>(RoundToNearestS32((rightPx - centerOffsetPx) / screenUnitsPerHudPixel));

    const xcInventoryItem* overlayItem = section->overlays->FindItem(kHubPromptOverlayHash);
    if (!overlayItem) {
        return;
    }

    xcOverlayData* overlay = reinterpret_cast<xcOverlayData*>(rawData + overlayItem->dataOffset);
    StretchHubBottomBarsInOverlay(section, overlay, rawData, leftX, rightX);
}

void HUDWidescreenPatches::BeginUpdate(xcSection* section, u8* rawData) {
    EnsureInitialized(section, rawData);
    Reset(rawData);
}

void HUDWidescreenPatches::EndUpdate(xcSection* section, u8* rawData) {
    Apply(rawData);
    FixHubPromptBottomBar(section, rawData);
}

void HUDWidescreenPatches::OnLevelLoad(xcSection* section, u8* rawData) {
    EnsureInitialized(section, rawData);
    Apply(rawData);
    FixHubPromptBottomBar(section, rawData);
}

void HUDWidescreenPatches::OnLevelUnload(u8* rawData) {
    Reset(rawData);
}

void HUDWidescreenPatches::DrawSection(xcSection* section) const {
    if (!section) {
        return;
    }

    if (!p3d::context) {
        section->Draw();
        return;
    }

    const s32 scissorX = RoundToNearestS32(SCREEN_EFFECTIVE_OFFSET_X);
    const s32 scissorW = RoundToNearestS32(SCREEN_EFFECTIVE_WIDTH);
    const s32 scissorH = RoundToNearestS32(SCREEN_HEIGHT);
    p3d::context->SetScissor(scissorX, 0, scissorW, scissorH);
#if defined(RC_PLATFORM_PSP) && !defined(PSP_UNBATCHED_HUD)
    // Every section prim draws through ScreenDraw, which keeps their order
    // inside a batch (a Gouraud quad or a texture change flushes it). Outside
    // one, each glyph swapped the projection and 2D state twice and was a
    // draw of its own; consecutive glyphs of a font page are now one draw.
    // The batch ends before the scissor is restored.
    ScreenDraw::BeginBatch();
    section->Draw();
    ScreenDraw::EndBatch();
#else
    section->Draw();
#endif
    p3d::context->SetScissor(0, 0, RoundToNearestS32(SCREEN_WIDTH), scissorH);
}

#endif
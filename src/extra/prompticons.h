#pragma once

#include "gen/config.h"

#if CUSTOM_MENU

#include "core.h"
#include "extra/controllerpromptstyle.h"
#include "pc/inputaction.h"
#include "pc/tim.h"
#include "p3d/texture.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

class PromptIcons {
public:
    struct PromptRect {
        s16 x = -1;
        s16 y = -1;
        s16 w = 0;
        s16 h = 0;

        bool IsValid() const {
            return x >= 0 && y >= 0 && w > 0 && h > 0;
        }
    };

    struct ResolvedPrompt {
        tTexture* tex = nullptr;
        s32 texW = 0;
        s32 texH = 0;
        PromptRect rect;
        bool isGamepad = false;
        char fallback[24] = {};
#if defined(RC_PLATFORM_PSP)
        // Atlas column of a PSP glyph (face button or D-pad), or -1.
        s8 pspGlyph = -1;
        // START/SELECT/L/R: the text renderer draws the fallback name on a
        // dark badge (see DrawPspBadge).
        bool pspBadge = false;
#endif

        bool HasIcon() const {
            return tex && texW > 0 && texH > 0 && rect.IsValid();
        }

        bool HasFallback() const {
            return fallback[0] != '\0';
        }
    };

    struct PromptGrid {
        s16 baseTexW = 0;
        s16 baseTexH = 0;
        s16 originX = 0;
        s16 originY = 0;
        s16 cellW = 0;
        s16 cellH = 0;
        s16 stepX = 0;
        s16 stepY = 0;

        bool IsValid() const {
            return baseTexW > 0 && baseTexH > 0 && cellW > 0 && cellH > 0;
        }
    };

    struct PromptGridIndex {
        const PromptGrid* grid = nullptr;
        s16 col = -1;
        s16 row = -1;

        bool IsValid() const {
            return grid && grid->IsValid() && col >= 0 && row >= 0;
        }
    };

    static bool ParseInlineActionToken(const char* text, s32 start, s32* outConsumed, Action* outAction) {
        if (!text || start < 0 || text[start] != '<') {
            return false;
        }

        static const char kPrefix[] = "<ACT:";
        const s32 prefixLen = (s32)(sizeof(kPrefix) - 1);
        if (strncmp(text + start, kPrefix, prefixLen) != 0) {
            return false;
        }

        const char* tokenStart = text + start + prefixLen;
        const char* tokenEnd = strchr(tokenStart, '>');
        if (!tokenEnd || tokenEnd == tokenStart) {
            return false;
        }

        char token[32] = {};
        s32 tokenLen = (s32)(tokenEnd - tokenStart);
        if (tokenLen >= (s32)sizeof(token)) {
            return false;
        }
        memcpy(token, tokenStart, tokenLen);
        token[tokenLen] = '\0';

        Action action = ::ActionFromToken(token);
        if (action < 0 || action >= ACTION_COUNT) {
            return false;
        }

        if (outConsumed) {
            *outConsumed = prefixLen + tokenLen + 1;
        }
        if (outAction) {
            *outAction = action;
        }
        return true;
    }

    static bool ParseInlineDesktopBindingToken(const char* text, s32 start, s32* outConsumed, s32* outCode) {
        if (!text || start < 0 || text[start] != '<') {
            return false;
        }

        static const char kPrefix[] = "<BND:";
        const s32 prefixLen = (s32)(sizeof(kPrefix) - 1);
        if (strncmp(text + start, kPrefix, prefixLen) != 0) {
            return false;
        }

        const char* tokenStart = text + start + prefixLen;
        const char* tokenEnd = strchr(tokenStart, '>');
        if (!tokenEnd || tokenEnd == tokenStart) {
            return false;
        }

        const s32 tokenLen = (s32)(tokenEnd - tokenStart);
        char token[32] = {};
        if (tokenLen >= (s32)sizeof(token)) {
            return false;
        }
        memcpy(token, tokenStart, tokenLen);
        token[tokenLen] = '\0';

        char* parseEnd = nullptr;
        const long parsedCode = strtol(token, &parseEnd, 10);
        if (!parseEnd || *parseEnd != '\0') {
            return false;
        }

        if (outConsumed) {
            *outConsumed = prefixLen + tokenLen + 1;
        }
        if (outCode) {
            *outCode = (s32)parsedCode;
        }
        return true;
    }

    static bool ResolveActionPrompt(Action action, ResolvedPrompt& out) {
        out = {};

#if defined(RC_PLATFORM_PSP)
        // The PSP has no keyboard and no Xbox sheet. Its pad is not always reported
        // as "active", so never fall back to keyboard glyphs: face buttons and the
        // D-pad come from the PSP prompt atlas, START/SELECT/L/R are text badges,
        // and plain PSP names remain if the atlas is missing.
        const s32 pspButton = g_actionInput ? g_actionInput->GetGamepadButtonBinding(action) : GpBtn::NONE;
        CopyText(out.fallback, (s32)sizeof(out.fallback), PspButtonToLabel(pspButton));
        SheetState& pspSheet = GetPspPromptSheet();
        if (!EnsurePspPromptSheetLoaded(pspSheet)) {
            return out.HasFallback();
        }
        const s8 glyph = PspButtonToGlyph(pspButton);
        if (glyph >= 0) {
            out.tex = pspSheet.tex;
            out.texW = pspSheet.width;
            out.texH = pspSheet.height;
            out.rect = PspGlyphRect(glyph, kPspPromptRowCount - 1);
            out.isGamepad = true;
            out.pspGlyph = glyph;
            return true;
        }
        out.pspBadge = out.HasFallback();
        return out.HasFallback();
#endif

        const bool useGamepad = g_actionInput && g_actionInput->IsGamepadActive();
        if (useGamepad) {
            const s32 button = g_actionInput ? g_actionInput->GetGamepadButtonBinding(action) : GpBtn::NONE;
            CopyText(out.fallback, (s32)sizeof(out.fallback), GamepadButtonToLabel(button));

            SheetState& sheet = GetXboxSheet();
            if (!EnsureSheetLoaded(sheet, false)) {
                return out.HasFallback();
            }

            PromptRect rect = GamepadButtonToRect(button, sheet.width, sheet.height);
            if (!rect.IsValid()) {
                return out.HasFallback();
            }

            out.tex = sheet.tex;
            out.texW = sheet.width;
            out.texH = sheet.height;
            out.rect = rect;
            out.isGamepad = true;
            return true;
        }

        const s32 key = g_actionInput ? g_actionInput->GetKeyBinding(action) : 0;
        CopyText(out.fallback, (s32)sizeof(out.fallback), KeyToLabel(key));

        SheetState& sheet = GetKeyboardSheet();
        if (!EnsureSheetLoaded(sheet, true)) {
            return out.HasFallback();
        }

        PromptRect rect = KeyToRect(key, sheet.width, sheet.height);
        if (!rect.IsValid()) {
            return out.HasFallback();
        }

        out.tex = sheet.tex;
        out.texW = sheet.width;
        out.texH = sheet.height;
        out.rect = rect;
        out.isGamepad = false;
        return true;
    }

    static bool ResolveDesktopBindingCodePrompt(s32 code, ResolvedPrompt& out) {
        out = {};

#if defined(RC_PLATFORM_PSP)
        // Desktop binding codes name keyboard keys or mouse buttons, which the
        // PSP does not have. Never load the keyboard atlas on PSP and draw
        // nothing (no icon, no fallback text) so the token simply disappears.
        (void)code;
        return false;
#endif

        if (code == 0) {
            CopyText(out.fallback, (s32)sizeof(out.fallback), "-");
            return true;
        }

        if (code > 0) {
            CopyText(out.fallback, (s32)sizeof(out.fallback), KeyToLabel(code));

            SheetState& sheet = GetKeyboardSheet();
            if (!EnsureSheetLoaded(sheet, true)) {
                return out.HasFallback();
            }

            PromptRect rect = KeyToRect(code, sheet.width, sheet.height);
            if (!rect.IsValid()) {
                return out.HasFallback();
            }

            out.tex = sheet.tex;
            out.texW = sheet.width;
            out.texH = sheet.height;
            out.rect = rect;
            out.isGamepad = false;
            return true;
        }

        const s32 mouseButton = (-code) - 1;
        CopyText(out.fallback, (s32)sizeof(out.fallback), MouseButtonToLabel(mouseButton));

        SheetState& sheet = GetKeyboardSheet();
        if (!EnsureSheetLoaded(sheet, true)) {
            return out.HasFallback();
        }

        PromptRect rect = MouseButtonToRect(mouseButton, sheet.width, sheet.height);
        if (!rect.IsValid()) {
            return out.HasFallback();
        }

        out.tex = sheet.tex;
        out.texW = sheet.width;
        out.texH = sheet.height;
        out.rect = rect;
        out.isGamepad = false;
        return true;
    }

    static f32 GetPromptScale(const ResolvedPrompt& prompt) {
        return prompt.isGamepad ? 1.0f : 1.5f;
    }

    static f32 GetDrawHeight(const ResolvedPrompt& prompt, f32 drawHeight) {
        return drawHeight * GetPromptScale(prompt);
    }

    static f32 GetDrawWidth(const ResolvedPrompt& prompt, f32 drawHeight) {
        if (!prompt.HasIcon() || drawHeight <= 0.0f) {
            return 0.0f;
        }
#if defined(RC_PLATFORM_PSP)
        if (prompt.pspGlyph >= 0) {
            return PspGlyphSize(drawHeight);
        }
#endif
        const f32 effectiveHeight = GetDrawHeight(prompt, drawHeight);
        return effectiveHeight * ((f32)prompt.rect.w / (f32)prompt.rect.h);
    }

    static void DrawPrompt(const ResolvedPrompt& prompt, f32 x, f32 y, f32 drawHeight, u8 alpha) {
        if (!prompt.HasIcon() || drawHeight <= 0.0f) {
            return;
        }
#if defined(RC_PLATFORM_PSP)
        if (prompt.pspGlyph >= 0) {
            const s32 row = PspPromptRow(drawHeight);
            const f32 size = PspGlyphSize(drawHeight);
            DrawPspAtlasQuad(PspGlyphRect(prompt.pspGlyph, row), 0.0f, 1.0f,
                PspGlyphLeft(x, drawHeight), PspGlyphTop(y, drawHeight), size, size, alpha);
            return;
        }
#endif

        const f32 effectiveHeight = GetDrawHeight(prompt, drawHeight);
        const f32 drawWidth = GetDrawWidth(prompt, drawHeight);
        const f32 drawY = y - ((effectiveHeight - drawHeight) * 0.5f) - (drawHeight * 0.1f);
        f32 u0 = (f32)prompt.rect.x / (f32)prompt.texW;
        f32 v0 = (f32)prompt.rect.y / (f32)prompt.texH;
        f32 u1 = (f32)(prompt.rect.x + prompt.rect.w) / (f32)prompt.texW;
        f32 v1 = (f32)(prompt.rect.y + prompt.rect.h) / (f32)prompt.texH;

        if (u1 < u0) {
            const f32 tmp = u0;
            u0 = u1;
            u1 = tmp;
        }
        if (v1 < v0) {
            const f32 tmp = v0;
            v0 = v1;
            v1 = tmp;
        }

        if (u0 < 0.0f) u0 = 0.0f;
        if (v0 < 0.0f) v0 = 0.0f;
        if (u1 > 1.0f) u1 = 1.0f;
        if (v1 > 1.0f) v1 = 1.0f;

        ScreenDraw::DrawQuad(prompt.tex, x, drawY, drawWidth, effectiveHeight,
                             u0, v0, u1, v1,
                             255, 255, 255, alpha);
    }

#if defined(RC_PLATFORM_PSP)
    // Width of a START/SELECT/L/R badge around a label textWidth pixels wide:
    // the label plus side padding, never narrower than the badge is tall.
    static f32 GetPspBadgeWidth(f32 textWidth, f32 drawHeight) {
        const f32 size = PspBadgeSize(drawHeight);
        f32 width = textWidth + 2.0f * std::floor(size * 0.3f + 0.5f);
        if (width < size) {
            width = size;
        }
        return PspPromptIsNative(drawHeight) ? std::floor(width + 0.5f) : width;
    }

    // Dark rimmed badge behind a START/SELECT/L/R label, one pixel taller
    // than the button glyphs on each side so the rim clears the label: the
    // atlas badge disc's left half, its centre column stretched, then its
    // right half.
    static void DrawPspBadge(f32 x, f32 y, f32 width, f32 drawHeight, u8 alpha) {
        if (drawHeight <= 0.0f || !EnsurePspPromptSheetLoaded(GetPspPromptSheet())) {
            return;
        }
        const PromptRect disc = PspBadgeRect(PspPromptRow(drawHeight));
        const f32 size = PspBadgeSize(drawHeight);
        const f32 half = size * 0.5f;
        const f32 left = PspGlyphLeft(x, drawHeight);
        const f32 top = PspGlyphTop(y, drawHeight) - 1.0f;
        const f32 centreTexel = 0.5f / (f32)disc.w;
        DrawPspAtlasQuad(disc, 0.0f, 0.5f, left, top, half, size, alpha);
        if (width > size) {
            DrawPspAtlasQuad(disc, 0.5f - centreTexel, 0.5f + centreTexel,
                left + half, top, width - size, size, alpha);
        }
        DrawPspAtlasQuad(disc, 0.5f, 1.0f, left + width - half, top, half, size, alpha);
    }
#endif

private:
    struct SheetState {
        tTexture* tex = nullptr;
        s32 width = 0;
        s32 height = 0;
        bool tried = false;
    };

#if defined(RC_PLATFORM_PSP)
    // psp_prompts.png, built by scripts/make_psp_prompt_atlas.py: one row of
    // glyphs per native size, columns kPspPromptColumnStep pixels apart, then
    // one badge disc per row, two pixels taller than the row's glyphs.
    static constexpr const char* kPspPromptAtlasPath = "pc/textures/frontend/psp_prompts.png";
    static constexpr s32 kPspPromptRowCount = 2;
    static constexpr s16 kPspPromptRowSize[kPspPromptRowCount] = { 14, 18 };
    static constexpr s16 kPspPromptRowTop[kPspPromptRowCount] = { 0, 16 };
    static constexpr s16 kPspPromptColumnStep = 20;
    static constexpr s16 kPspPromptBadgeLeft = 9 * kPspPromptColumnStep;
    // A text line within this many pixels of a row's size draws that row 1:1,
    // as sharp as the font strikes; other sizes scale the nearest row.
    static constexpr f32 kPspPromptSnapTolerance = 1.5f;

    enum : s8 {
        kPspGlyphCross,
        kPspGlyphCircle,
        kPspGlyphTriangle,
        kPspGlyphSquare,
        kPspGlyphDPad,
        kPspGlyphDPadLeft,
        kPspGlyphDPadRight,
        kPspGlyphDPadUp,
        kPspGlyphDPadDown,
    };

    // GpBtn layout on PSP: A = Cross, B = Circle, X = Square, Y = Triangle.
    static s8 PspButtonToGlyph(s32 button) {
        switch (button) {
            case GpBtn::A: return kPspGlyphCross;
            case GpBtn::B: return kPspGlyphCircle;
            case GpBtn::X: return kPspGlyphSquare;
            case GpBtn::Y: return kPspGlyphTriangle;
            case GpBtn::DpadLeft: return kPspGlyphDPadLeft;
            case GpBtn::DpadRight: return kPspGlyphDPadRight;
            case GpBtn::DpadUp: return kPspGlyphDPadUp;
            case GpBtn::DpadDown: return kPspGlyphDPadDown;
            default: return -1;
        }
    }

    static s32 PspPromptRow(f32 drawHeight) {
        s32 best = 0;
        for (s32 row = 1; row < kPspPromptRowCount; row++) {
            if (std::fabs(drawHeight - (f32)kPspPromptRowSize[row])
                < std::fabs(drawHeight - (f32)kPspPromptRowSize[best])) {
                best = row;
            }
        }
        return best;
    }

    static bool PspPromptIsNative(f32 drawHeight) {
        return std::fabs(drawHeight - (f32)kPspPromptRowSize[PspPromptRow(drawHeight)]) <= kPspPromptSnapTolerance;
    }

    static f32 PspGlyphSize(f32 drawHeight) {
        return PspPromptIsNative(drawHeight) ? (f32)kPspPromptRowSize[PspPromptRow(drawHeight)] : drawHeight;
    }

    static f32 PspGlyphLeft(f32 x, f32 drawHeight) {
        return PspPromptIsNative(drawHeight) ? std::floor(x + 0.5f) : x;
    }

    // Centred where DrawPrompt centres desktop prompts: 40% down the line.
    static f32 PspGlyphTop(f32 y, f32 drawHeight) {
        const f32 top = y + drawHeight * 0.4f - PspGlyphSize(drawHeight) * 0.5f;
        return PspPromptIsNative(drawHeight) ? std::floor(top + 0.5f) : top;
    }

    static PromptRect PspGlyphRect(s8 glyph, s32 row) {
        PromptRect rect;
        rect.x = (s16)(glyph * kPspPromptColumnStep);
        rect.y = kPspPromptRowTop[row];
        rect.w = kPspPromptRowSize[row];
        rect.h = kPspPromptRowSize[row];
        return rect;
    }

    static f32 PspBadgeSize(f32 drawHeight) {
        return PspGlyphSize(drawHeight) + 2.0f;
    }

    static PromptRect PspBadgeRect(s32 row) {
        PromptRect rect;
        rect.x = (s16)(kPspPromptBadgeLeft + row * kPspPromptColumnStep);
        rect.y = 0;
        rect.w = (s16)(kPspPromptRowSize[row] + 2);
        rect.h = rect.w;
        return rect;
    }

    // Draws the [uFrom, uTo] horizontal slice (fractions of the cell) of an
    // atlas cell.
    static void DrawPspAtlasQuad(const PromptRect& rect, f32 uFrom, f32 uTo,
        f32 x, f32 y, f32 w, f32 h, u8 alpha) {
        const SheetState& sheet = GetPspPromptSheet();
        if (!sheet.tex || w <= 0.0f || h <= 0.0f) {
            return;
        }
        const f32 invW = 1.0f / (f32)sheet.width;
        const f32 invH = 1.0f / (f32)sheet.height;
        ScreenDraw::DrawQuad(sheet.tex, x, y, w, h,
            ((f32)rect.x + uFrom * (f32)rect.w) * invW, (f32)rect.y * invH,
            ((f32)rect.x + uTo * (f32)rect.w) * invW, (f32)(rect.y + rect.h) * invH,
            255, 255, 255, alpha);
    }

    static SheetState& GetPspPromptSheet() {
        static SheetState sheet;
        return sheet;
    }

    static bool EnsurePspPromptSheetLoaded(SheetState& sheet) {
        if (sheet.tex) {
            return true;
        }
        if (sheet.tried) {
            return false;
        }
        sheet.tried = true;
        sheet.tex = tTexture::LoadFromImagePath(kPspPromptAtlasPath, &sheet.width, &sheet.height);
        return sheet.tex != nullptr;
    }
#endif

    static void CopyText(char* dst, s32 dstLen, const char* src) {
        if (!dst || dstLen <= 0) {
            return;
        }
        dst[0] = '\0';
        if (!src) {
            return;
        }
        s32 write = 0;
        while (write + 1 < dstLen && src[write] != '\0') {
            dst[write] = src[write];
            write++;
        }
        dst[write] = '\0';
    }

    static const char* KeyToLabel(s32 key) {
        static char dynLabel[24];

        if (key >= KEY_A && key <= KEY_Z) {
            dynLabel[0] = (char)key;
            dynLabel[1] = '\0';
            return dynLabel;
        }

        if (key >= KEY_0 && key <= KEY_9) {
            dynLabel[0] = (char)key;
            dynLabel[1] = '\0';
            return dynLabel;
        }

        if (key >= KEY_F1 && key <= KEY_F12) {
            const s32 fn = key - KEY_F1 + 1;
            dynLabel[0] = 'F';
            if (fn >= 10) {
                dynLabel[1] = (char)('0' + (fn / 10));
                dynLabel[2] = (char)('0' + (fn % 10));
                dynLabel[3] = '\0';
            }
            else {
                dynLabel[1] = (char)('0' + fn);
                dynLabel[2] = '\0';
            }
            return dynLabel;
        }

        switch (key) {
            case KEY_ENTER: return "Enter";
            case KEY_ESCAPE: return "Esc";
            case KEY_LEFT: return "Left";
            case KEY_RIGHT: return "Right";
            case KEY_UP: return "Up";
            case KEY_DOWN: return "Down";
            case KEY_SPACE: return "Space";
            case KEY_TAB: return "Tab";
            case KEY_BACKSPACE: return "Backsp";
            case KEY_INSERT: return "Ins";
            case KEY_DELETE: return "Del";
            case KEY_HOME: return "Home";
            case KEY_END: return "End";
            case KEY_PAGE_UP: return "PgUp";
            case KEY_PAGE_DOWN: return "PgDn";
            case KEY_CAPS_LOCK: return "Caps";
            case KEY_LEFT_SHIFT: return "LShift";
            case KEY_RIGHT_SHIFT: return "RShift";
            case KEY_LEFT_CONTROL: return "LCtrl";
            case KEY_RIGHT_CONTROL: return "RCtrl";
            case KEY_LEFT_ALT: return "LAlt";
            case KEY_RIGHT_ALT: return "RAlt";
            case KEY_MINUS: return "-";
            case KEY_EQUAL: return "=";
            case KEY_LEFT_BRACKET: return "[";
            case KEY_RIGHT_BRACKET: return "]";
            case KEY_BACKSLASH: return "\\";
            case KEY_SEMICOLON: return ";";
            case KEY_APOSTROPHE: return "'";
            case KEY_COMMA: return ",";
            case KEY_PERIOD: return ".";
            case KEY_SLASH: return "/";
            case KEY_GRAVE: return "`";
            default: return nullptr;
        }
    }

    // Short PSP names for the GpBtn layout (A = Cross, B = Circle, X = Square, Y = Triangle).
    // Cross/Circle use the usual one-letter "X"/"O" so a three-prompt help bar
    // fits 480px; D-pad directions are named so "LEFT/RIGHT Adjust" is unambiguous.
    static const char* PspButtonToLabel(s32 button) {
        switch (button) {
            case GpBtn::A: return "X";
            case GpBtn::B: return "O";
            case GpBtn::X: return "SQUARE";
            case GpBtn::Y: return "TRIANGLE";
            case GpBtn::LB: return "L";
            case GpBtn::RB: return "R";
            case GpBtn::Back: return "SELECT";
            case GpBtn::Start: return "START";
            case GpBtn::DpadLeft: return "LEFT";
            case GpBtn::DpadRight: return "RIGHT";
            case GpBtn::DpadUp: return "UP";
            case GpBtn::DpadDown: return "DOWN";
            default: return "";
        }
    }

    static const char* GamepadButtonToLabel(s32 button) {
        switch (button) {
            case GpBtn::A: return "A";
            case GpBtn::B: return "B";
            case GpBtn::X: return "X";
            case GpBtn::Y: return "Y";
            case GpBtn::LB: return "LB";
            case GpBtn::RB: return "RB";
            case GpBtn::Back: return "View";
            case GpBtn::Start: return "Menu";
            case GpBtn::DpadLeft:
            case GpBtn::DpadRight:
            case GpBtn::DpadUp:
            case GpBtn::DpadDown:
                return "D-Pad";
            default: return nullptr;
        }
    }

    static const char* MouseButtonToLabel(s32 button) {
        switch (button) {
            case MouseBtn::Left: return "Mouse1";
            case MouseBtn::Right: return "Mouse2";
            case MouseBtn::Middle: return "Mouse3";
            default: return nullptr;
        }
    }

    static const s16 kKeyboardAtlasCols = 16;
    static const s16 kKeyboardAtlasRows = 8;

    static PromptRect KeyboardRectByIndex(s16 index, s32 texW, s32 texH) {
        if (index < 0) {
            return {};
        }

        const s16 maxCells = (s16)(kKeyboardAtlasCols * kKeyboardAtlasRows);
        if (index >= maxCells) {
            return {};
        }

        const s16 col = (s16)(index % kKeyboardAtlasCols);
        const s16 row = (s16)(index / kKeyboardAtlasCols);
        return RectFromUniformGrid(col, row, kKeyboardAtlasCols, kKeyboardAtlasRows, texW, texH);
    }

    static s16 KeyToAtlasIndex(s32 key) {
        // Keep this in keycode declaration order so atlas slots match key enum order.
        switch (key) {
            case KEY_SPACE: return 0;
            case KEY_APOSTROPHE: return 1;
            case KEY_COMMA: return 2;
            case KEY_MINUS: return 3;
            case KEY_PERIOD: return 4;
            case KEY_SLASH: return 5;
            case KEY_0: return 6;
            case KEY_1: return 7;
            case KEY_2: return 8;
            case KEY_3: return 9;
            case KEY_4: return 10;
            case KEY_5: return 11;
            case KEY_6: return 12;
            case KEY_7: return 13;
            case KEY_8: return 14;
            case KEY_9: return 15;
            case KEY_SEMICOLON: return 16;
            case KEY_EQUAL: return 17;
            case KEY_A: return 18;
            case KEY_B: return 19;
            case KEY_C: return 20;
            case KEY_D: return 21;
            case KEY_E: return 22;
            case KEY_F: return 23;
            case KEY_G: return 24;
            case KEY_H: return 25;
            case KEY_I: return 26;
            case KEY_J: return 27;
            case KEY_K: return 28;
            case KEY_L: return 29;
            case KEY_M: return 30;
            case KEY_N: return 31;
            case KEY_O: return 32;
            case KEY_P: return 33;
            case KEY_Q: return 34;
            case KEY_R: return 35;
            case KEY_S: return 36;
            case KEY_T: return 37;
            case KEY_U: return 38;
            case KEY_V: return 39;
            case KEY_W: return 40;
            case KEY_X: return 41;
            case KEY_Y: return 42;
            case KEY_Z: return 43;
            case KEY_LEFT_BRACKET: return 44;
            case KEY_BACKSLASH: return 45;
            case KEY_RIGHT_BRACKET: return 46;
            case KEY_GRAVE: return 47;
            case KEY_ESCAPE: return 48;
            case KEY_ENTER: return 49;
            case KEY_TAB: return 50;
            case KEY_BACKSPACE: return 51;
            case KEY_INSERT: return 52;
            case KEY_DELETE: return 53;
            case KEY_RIGHT: return 54;
            case KEY_LEFT: return 55;
            case KEY_DOWN: return 56;
            case KEY_UP: return 57;
            case KEY_PAGE_UP: return 58;
            case KEY_PAGE_DOWN: return 59;
            case KEY_HOME: return 60;
            case KEY_END: return 61;
            case KEY_CAPS_LOCK: return 62;
            case KEY_F1: return 63;
            case KEY_F2: return 64;
            case KEY_F3: return 65;
            case KEY_F4: return 66;
            case KEY_F5: return 67;
            case KEY_F6: return 68;
            case KEY_F7: return 69;
            case KEY_F8: return 70;
            case KEY_F9: return 71;
            case KEY_F10: return 72;
            case KEY_F11: return 73;
            case KEY_F12: return 74;
            case KEY_KP_0: return 75;
            case KEY_KP_1: return 76;
            case KEY_KP_2: return 77;
            case KEY_KP_3: return 78;
            case KEY_KP_4: return 79;
            case KEY_KP_5: return 80;
            case KEY_KP_6: return 81;
            case KEY_KP_7: return 82;
            case KEY_KP_8: return 83;
            case KEY_KP_9: return 84;
            case KEY_KP_DECIMAL: return 85;
            case KEY_KP_DIVIDE: return 86;
            case KEY_KP_MULTIPLY: return 87;
            case KEY_KP_SUBTRACT: return 88;
            case KEY_KP_ADD: return 89;
            case KEY_KP_ENTER: return 90;
            case KEY_KP_EQUAL: return 91;
            case KEY_LEFT_SHIFT: return 92;
            case KEY_LEFT_CONTROL: return 93;
            case KEY_LEFT_ALT: return 94;
            case KEY_RIGHT_SHIFT: return 95;
            case KEY_RIGHT_CONTROL: return 96;
            case KEY_RIGHT_ALT: return 97;
            default: return -1;
        }
    }

    static s16 MouseButtonToAtlasIndex(s32 button) {
        switch (button) {
            case MouseBtn::Left: return 112;
            case MouseBtn::Middle: return 113;
            case MouseBtn::Right: return 114;
            default: return -1;
        }
    }

    static PromptRect KeyToRect(s32 key, s32 texW, s32 texH) {
        return KeyboardRectByIndex(KeyToAtlasIndex(key), texW, texH);
    }

    static PromptRect GamepadButtonToRect(s32 button, s32 texW, s32 texH) {
        PromptGridIndex idx;
        switch (button) {
            case GpBtn::Y:
                idx = MakeGridIndex(GetControllerFaceGrid(), 0, 0);
                break;
            case GpBtn::B:
                idx = MakeGridIndex(GetControllerFaceGrid(), 1, 0);
                break;
            case GpBtn::A:
                idx = MakeGridIndex(GetControllerFaceGrid(), 2, 0);
                break;
            case GpBtn::X:
                idx = MakeGridIndex(GetControllerFaceGrid(), 3, 0);
                break;
            case GpBtn::Start:
                idx = MakeGridIndex(GetControllerMenuGrid(), 0, 0);
                break;
            case GpBtn::Back:
                idx = MakeGridIndex(GetControllerMenuGrid(), 1, 0);
                break;
            case GpBtn::DpadLeft:
            case GpBtn::DpadRight:
            case GpBtn::DpadUp:
            case GpBtn::DpadDown:
                idx = MakeGridIndex(GetControllerDPadGrid(), 0, 0);
                break;
            default:
                return {};
        }
        return RectFromGridIndex(idx, texW, texH);
    }

    static PromptRect MouseButtonToRect(s32 button, s32 texW, s32 texH) {
        return KeyboardRectByIndex(MouseButtonToAtlasIndex(button), texW, texH);
    }

    static PromptGridIndex MakeGridIndex(const PromptGrid& grid, s16 col, s16 row) {
        PromptGridIndex idx;
        idx.grid = &grid;
        idx.col = col;
        idx.row = row;
        return idx;
    }

    static PromptRect RectFromGridIndex(const PromptGridIndex& idx, s32 texW, s32 texH) {
        if (!idx.IsValid() || texW <= 0 || texH <= 0) {
            return {};
        }

        const PromptGrid& grid = *idx.grid;
        const f32 scaleX = (f32)texW / (f32)grid.baseTexW;
        const f32 scaleY = (f32)texH / (f32)grid.baseTexH;

        const f32 baseX0 = (f32)grid.originX + (f32)idx.col * (f32)grid.stepX;
        const f32 baseY0 = (f32)grid.originY + (f32)idx.row * (f32)grid.stepY;
        const f32 baseX1 = baseX0 + (f32)grid.cellW;
        const f32 baseY1 = baseY0 + (f32)grid.cellH;

        s32 x = (s32)(baseX0 * scaleX + 0.5f);
        s32 y = (s32)(baseY0 * scaleY + 0.5f);
        s32 x1 = (s32)(baseX1 * scaleX + 0.5f);
        s32 y1 = (s32)(baseY1 * scaleY + 0.5f);
        s32 w = x1 - x;
        s32 h = y1 - y;

        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= texW || y >= texH) {
            return {};
        }
        if (x + w > texW) w = texW - x;
        if (y + h > texH) h = texH - y;
        if (w <= 0 || h <= 0) {
            return {};
        }

        PromptRect out;
        out.x = (s16)x;
        out.y = (s16)y;
        out.w = (s16)w;
        out.h = (s16)h;
        return out;
    }

    static PromptRect RectFromUniformGrid(s16 col, s16 row, s16 cols, s16 rows, s32 texW, s32 texH) {
        if (cols <= 0 || rows <= 0 || texW <= 0 || texH <= 0) {
            return {};
        }
        if (col < 0 || col >= cols || row < 0 || row >= rows) {
            return {};
        }

        const s32 x0 = (s32)(((s64)col * texW) / cols);
        const s32 y0 = (s32)(((s64)row * texH) / rows);
        const s32 x1 = (s32)(((s64)(col + 1) * texW) / cols);
        const s32 y1 = (s32)(((s64)(row + 1) * texH) / rows);

        s32 w = x1 - x0;
        s32 h = y1 - y0;
        if (w <= 0 || h <= 0) {
            return {};
        }

        PromptRect out;
        out.x = (s16)x0;
        out.y = (s16)y0;
        out.w = (s16)w;
        out.h = (s16)h;
        return out;
    }

    static const PromptGrid& GetControllerFaceGrid() {
        static const PromptGrid grid = {
            632, 1808, // base atlas size
            16, 16,    // first glyph top-left
            96, 96,    // glyph size
            168, 168   // column/row step
        };
        return grid;
    }

    static const PromptGrid& GetControllerMenuGrid() {
        static const PromptGrid grid = {
            632, 1808,
            16, 1696,
            96, 96,
            154, 154
        };
        return grid;
    }

    static const PromptGrid& GetControllerDPadGrid() {
        static const PromptGrid grid = {
            632, 1808,
            4, 1350,
            120, 121,
            120, 121
        };
        return grid;
    }

    static bool EnsureSheetLoaded(SheetState& sheet, bool keyboard) {
        if (sheet.tex) {
            return true;
        }
        if (sheet.tried) {
            return false;
        }
        sheet.tried = true;

        char builtPath[128];
        const char* path = "pc/textures/frontend/keyboard_mouse_sheet_default.png";
        if (!keyboard) {
            ControllerPromptManager::BuildSheetPath(builtPath, (s32)sizeof(builtPath));
            path = builtPath;
        }
        sheet.tex = tTexture::LoadFromImagePath(path, &sheet.width, &sheet.height);
        return sheet.tex != nullptr;
    }

    static SheetState& GetKeyboardSheet() {
        static SheetState sheet;
        return sheet;
    }

    static SheetState& GetXboxSheet() {
        static SheetState sheet;
        return sheet;
    }

public:
    // Call after ControllerPromptManager::SetStyle() reports a change, so the
    // next gamepad button prompt reloads from the new style's sheet instead
    // of keeping the old one cached.
    static void ResetGamepadSheet() {
        SheetState& sheet = GetXboxSheet();
        if (sheet.tex) {
            sheet.tex->Release();
        }
        sheet = SheetState{};
    }
};

#endif
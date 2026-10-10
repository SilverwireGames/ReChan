#pragma once
#include "core.h"
#include <cstdio>

// Which controller-brand glyph set to use for on-screen button prompts
enum ControllerPromptStyle : s32 {
    ControllerPromptStyle_Default = 0, // Xbox-style glyphs
    ControllerPromptStyle_Switch,
    ControllerPromptStyle_PlayStation, // DualSense / PlayStation glyphs
    ControllerPromptStyle_Count,
};

struct ControllerPromptStyleInfo {
    const char* fileSuffix;    // controller_overlay_<suffix>.png / controller_sheet_<suffix>.png
    const char* displayToken;  // localization token shown in the menu list
};

static constexpr ControllerPromptStyleInfo kControllerPromptStyles[ControllerPromptStyle_Count] = {
    { "default", "FE_DEFX" },
    { "switch",  "FE_NX" },
    { "ps",      "FE_PS" },
};

#if defined(RC_PLATFORM_SWITCH)
static constexpr ControllerPromptStyle kDefaultControllerPromptStyle = ControllerPromptStyle_Switch;
#elif defined(RC_PLATFORM_PSP)
// PSP has one fixed glyph set (PlayStation); it is not user-selectable.
static constexpr ControllerPromptStyle kDefaultControllerPromptStyle = ControllerPromptStyle_PlayStation;
#else
static constexpr ControllerPromptStyle kDefaultControllerPromptStyle = ControllerPromptStyle_Default;
#endif

static inline s32 ClampControllerPromptStyle(s32 style) {
    if (style < 0 || style >= (s32)ControllerPromptStyle_Count) {
        return (s32)kDefaultControllerPromptStyle;
    }
    return style;
}

class ControllerPromptManager {
public:
    static s32 GetStyle() { return s_style; }

    // Returns true if the style actually changed.
    static bool SetStyle(s32 style) {
#if defined(RC_PLATFORM_PSP)
        // Ignores saved values from other platforms; the PSP style never changes.
        (void)style;
        return false;
#endif
        style = ClampControllerPromptStyle(style);
        if (style == s_style) {
            return false;
        }
        s_style = style;
        return true;
    }

    static void BuildOverlayPath(char* outPath, s32 outPathLen) {
        std::snprintf(outPath, (size_t)outPathLen, "pc/textures/frontend/controller_overlay_%s.png",
                      kControllerPromptStyles[s_style].fileSuffix);
    }

    static void BuildSheetPath(char* outPath, s32 outPathLen) {
        std::snprintf(outPath, (size_t)outPathLen, "pc/textures/frontend/controller_sheet_%s.png",
                      kControllerPromptStyles[s_style].fileSuffix);
    }

private:
    static inline s32 s_style = (s32)kDefaultControllerPromptStyle;
};

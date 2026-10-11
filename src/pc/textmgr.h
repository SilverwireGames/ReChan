#pragma once

#include "core.h"
#include "gen/config.h"
#include "gen/utf8text.h"
#include <string>
#include <vector>

typedef u32 TextFontHandle;

enum TextAlign : s32 {
    TextAlign_Left = 0,
    TextAlign_Center,
    TextAlign_Right,
};

struct TextColor {
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
    u8 a = 255;
};

struct TextShadowState {
    bool enabled = false;
    f32 offsetX = 1.0f;
    f32 offsetY = 1.0f;
    TextColor color = { 0, 0, 0, 192 };
};

struct TextOutlineState {
    bool enabled = false;
    f32 thickness = 1.0f;
    TextColor color = { 0, 0, 0, 255 };
};

struct TextRenderState {
    TextFontHandle font = 0;
    f32 scaleX = 1.0f;
    f32 scaleY = 1.0f;
    f32 wrapWidth = 0.0f;
    s32 lineSpacing = 0;
    TextAlign align = TextAlign_Left;
    TextColor color = {};
    TextShadowState shadow = {};
    TextOutlineState outline = {};
    bool promptsEnabled = true;
};

struct TextBounds {
    f32 width = 0.0f;
    f32 height = 0.0f;
    s32 lineCount = 0;
};

struct TextFontDesc {
    const char* name = nullptr;
    const char* path = nullptr;
    s32 pixelHeight = 0;
};

class ITextBackend;

class TextManager {
public:
    TextManager() = default;
    ~TextManager();

    void Init();
    void Shutdown();

    TextFontHandle LoadFont(const TextFontDesc& desc);
    // Resolves a font name, honouring aliases set with SetFontAlias.
    TextFontHandle FindFont(const char* name) const;
    // Adds an exact-size rasterisation of a loaded font (see ITextBackend::AddStrike).
    bool AddFontStrike(const char* name, s32 pixelHeight, bool bakedOutline);
    // Makes `name` resolve to the loaded font `target`; a null or unloaded
    // target removes the alias, so `name` resolves to its own font again.
    void SetFontAlias(const char* name, const char* target);

    void ResetState();
    void PushState();
    void PopState();

    const TextRenderState& GetState() const { return m_state; }
    void SetState(const TextRenderState& state) { m_state = state; }

    void SetFont(TextFontHandle font);
    bool SetFontByName(const char* name);
    void SetScale(f32 scaleX, f32 scaleY);
    void SetScale(f32 uniformScale) { SetScale(uniformScale, uniformScale); }
    void SetWrapWidth(f32 wrapWidth);
    void SetLineSpacing(s32 lineSpacing);
    void SetAlignment(TextAlign align);
    void SetColor(u8 r, u8 g, u8 b, u8 a = 255);
    void SetColor(const TextColor& color) { m_state.color = color; }
    void SetShadow(bool enabled, f32 offsetX = 1.0f, f32 offsetY = 1.0f,
                   u8 r = 0, u8 g = 0, u8 b = 0, u8 a = 192);
    void SetOutline(bool enabled, f32 thickness = 1.0f,
                    u8 r = 0, u8 g = 0, u8 b = 0, u8 a = 255);
    void SetPromptsEnabled(bool enabled);

    TextBounds MeasureString(Utf8TextView text) const;
    s32 CountWrappedLines(Utf8TextView text) const;
    TextBounds MeasureToken(const char* token) const;
    s32 CountWrappedLinesToken(const char* token) const;
    void PrintToken(const char* token, f32 x, f32 y) const;
    void PrintString(const char* text, f32 x, f32 y) const;
    void PrintString(Utf8TextView text, f32 x, f32 y) const;

private:
    struct LoadedFont {
        TextFontHandle handle = 0;
        std::string name = {};
        std::string path = {};
        s32 pixelHeight = 0;
        std::vector<u8> fileData = {};
    };

    struct FontAlias {
        std::string name = {};
        TextFontHandle handle = 0;
    };

    static bool ReadWholeFile(const char* path, std::vector<u8>& outData);
    const LoadedFont* FindLoadedFont(TextFontHandle handle) const;
    TextFontHandle FindRegisteredFont(const char* name) const;

    TextRenderState m_state = {};
    std::vector<TextRenderState> m_stateStack = {};
    std::vector<LoadedFont> m_fonts = {};
    std::vector<FontAlias> m_aliases = {};
    ITextBackend* m_backend = nullptr;
    TextFontHandle m_nextFontHandle = 1;
    bool m_warnedNoBackend = false;
};

extern TextManager* g_textManager;
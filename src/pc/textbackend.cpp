#include "pc/textbackend.h"

#include "pc/log.h"
#include "pc/tim.h"
#include "p3d/texture.h"
#include <cmath>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#if CUSTOM_MENU
#include "extra/prompticons.h"
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../vendor/stb/stb_truetype.h"

static u32 PackRgba(u8 r, u8 g, u8 b, u8 a) {
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

#if defined(RC_PLATFORM_PSP)
class PspLineWidthBuffer {
public:
    static constexpr size_t kCapacity = 128;

    void clear() { count = 0; }
    bool empty() const { return count == 0; }
    size_t size() const { return count; }
    f32& operator[](size_t index) { return values[index]; }
    const f32& operator[](size_t index) const { return values[index]; }
    void push_back(f32 value) {
        if (count < kCapacity) {
            values[count++] = value;
        }
    }

private:
    std::array<f32, kCapacity> values = {};
    size_t count = 0;
};

using LineWidthBuffer = PspLineWidthBuffer;
#else
using LineWidthBuffer = std::vector<f32>;
#endif

static bool DecodeNextUtf8(const char* text, s32 len, s32& cursor, u32& outCodepoint) {
    if (!text || cursor >= len) {
        return false;
    }

    const u8 c0 = (u8)text[cursor++];
    if (c0 < 0x80) {
        outCodepoint = c0;
        return true;
    }

    if ((c0 & 0xE0) == 0xC0 && cursor < len) {
        const u8 c1 = (u8)text[cursor];
        if ((c1 & 0xC0) == 0x80) {
            cursor++;
            outCodepoint = ((u32)(c0 & 0x1F) << 6) | (u32)(c1 & 0x3F);
            return true;
        }
    }
    else if ((c0 & 0xF0) == 0xE0 && cursor + 1 < len) {
        const u8 c1 = (u8)text[cursor + 0];
        const u8 c2 = (u8)text[cursor + 1];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
            cursor += 2;
            outCodepoint = ((u32)(c0 & 0x0F) << 12) | ((u32)(c1 & 0x3F) << 6) | (u32)(c2 & 0x3F);
            return true;
        }
    }
    else if ((c0 & 0xF8) == 0xF0 && cursor + 2 < len) {
        const u8 c1 = (u8)text[cursor + 0];
        const u8 c2 = (u8)text[cursor + 1];
        const u8 c3 = (u8)text[cursor + 2];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
            cursor += 3;
            outCodepoint = ((u32)(c0 & 0x07) << 18)
                | ((u32)(c1 & 0x3F) << 12)
                | ((u32)(c2 & 0x3F) << 6)
                | (u32)(c3 & 0x3F);
            return true;
        }
    }

    outCodepoint = (u32)'?';
    return true;
}

class StbTextBackend : public ITextBackend {
public:
    static constexpr s32 kLayoutReferencePixelHeight = 32;
#if defined(RC_PLATFORM_PSP)
    // The PSP GE supports texture pages up to 512².  A 512² atlas is enough
    // for the native UI when the rarely used extended glyph block is omitted;
    // larger atlases silently fail to sample on PPSSPP/real hardware.
    static constexpr s32 kAtlasSize = 512;
    static constexpr u32 kOversampleX = 1;
    static constexpr u32 kOversampleY = 1;
#else
    static constexpr s32 kAtlasSize = 2048;
#endif
    static constexpr s32 kPackPadding = 2;
#if !defined(RC_PLATFORM_PSP)
    static constexpr u32 kOversampleX = 3;
    static constexpr u32 kOversampleY = 3;
#endif

    struct PackedRange {
        u32 first = 0;
        s32 count = 0;
        std::vector<stbtt_packedchar> chars = {};
    };

    struct FontRecord {
        TextFontHandle handle = 0;
        stbtt_fontinfo info = {};
        std::vector<u8> ttfData = {};
        std::vector<PackedRange> ranges = {};
        tTexture* atlas = nullptr;
        s32 atlasW = 0;
        s32 atlasH = 0;
        f32 scale = 1.0f;
        f32 metricScale = 1.0f;
        f32 ascentPx = 0.0f;
        f32 lineHeightPx = 0.0f;
    };

    struct PromptToken {
        bool matched = false;
        s32 consumed = 0;
        f32 width = 0.0f;
    };

    bool Init() override {
        m_initialized = true;
        return true;
    }

    void Shutdown() override {
        for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
            if (m_fonts[i].atlas) {
                m_fonts[i].atlas->Release();
                m_fonts[i].atlas = nullptr;
            }
        }
        m_fonts.clear();
        m_initialized = false;
    }

    bool IsReady() const override {
        return m_initialized;
    }

    bool RegisterFont(const TextBackendFontDesc& desc) override {
        if (!desc.fileData || desc.fileSize <= 0 || !desc.handle || desc.pixelHeight <= 0) {
            return false;
        }

        FontRecord font = {};
        font.handle = desc.handle;
        font.ttfData.assign(desc.fileData, desc.fileData + desc.fileSize);

        if (!stbtt_InitFont(&font.info, font.ttfData.data(), 0)) {
            LOG("[TextBackend/STB] stbtt_InitFont failed for %s", desc.name ? desc.name : "<unnamed>");
            return false;
        }

        font.scale = stbtt_ScaleForPixelHeight(&font.info, (f32)desc.pixelHeight);
        font.metricScale = (f32)kLayoutReferencePixelHeight / (f32)desc.pixelHeight;

        // stb_truetype's public ABI uses native C++ int regardless of the
        // platform typedefs used by the game (s32 is long on PSP).
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &lineGap);
        font.ascentPx = (f32)ascent * font.scale * font.metricScale;
        font.lineHeightPx = ((f32)(ascent - descent + lineGap)) * font.scale * font.metricScale;
        if (font.lineHeightPx < 1.0f) {
            font.lineHeightPx = (f32)kLayoutReferencePixelHeight;
        }

        struct RangeDef { u32 first; s32 count; };
        static const RangeDef kRanges[] = {
            { 32, 95 },        // Basic Latin
            { 160, 96 },       // Latin-1 Supplement
#if !defined(RC_PLATFORM_PSP)
            { 0x0100, 128 },   // Latin Extended-A
            { 0x0180, 208 },   // Latin Extended-B subset
#endif
            { 0x20AC, 1 },     // Euro sign
        };

        font.ranges.clear();
        for (s32 i = 0; i < (s32)(sizeof(kRanges) / sizeof(kRanges[0])); i++) {
            PackedRange range = {};
            range.first = kRanges[i].first;
            range.count = kRanges[i].count;
            range.chars.resize((size_t)range.count);
            font.ranges.push_back(range);
        }

        font.atlasW = kAtlasSize;
        font.atlasH = kAtlasSize;
        std::vector<u8> alpha((size_t)font.atlasW * (size_t)font.atlasH, 0);

        stbtt_pack_context pack = {};
        if (!stbtt_PackBegin(&pack, alpha.data(), font.atlasW, font.atlasH, 0, kPackPadding, nullptr)) {
            LOG("[TextBackend/STB] stbtt_PackBegin failed for %s", desc.name ? desc.name : "<unnamed>");
            return false;
        }

        stbtt_PackSetOversampling(&pack, kOversampleX, kOversampleY);

        std::vector<stbtt_pack_range> packRanges(font.ranges.size());
        for (s32 i = 0; i < (s32)font.ranges.size(); i++) {
            packRanges[i].font_size = (f32)desc.pixelHeight;
            packRanges[i].first_unicode_codepoint_in_range = (s32)font.ranges[i].first;
            packRanges[i].num_chars = font.ranges[i].count;
            packRanges[i].chardata_for_range = font.ranges[i].chars.data();
            packRanges[i].array_of_unicode_codepoints = nullptr;
        }

        if (!stbtt_PackFontRanges(&pack, font.ttfData.data(), 0,
            packRanges.data(), (s32)packRanges.size())) {
            stbtt_PackEnd(&pack);
            LOG("[TextBackend/STB] stbtt_PackFontRanges failed for %s", desc.name ? desc.name : "<unnamed>");
            return false;
        }

        stbtt_PackEnd(&pack);

        std::vector<u32> rgba((size_t)font.atlasW * (size_t)font.atlasH, 0);
        for (s32 i = 0; i < font.atlasW * font.atlasH; i++) {
            rgba[(size_t)i] = PackRgba(255, 255, 255, alpha[(size_t)i]);
        }

        font.atlas = new tTexture();
        if (!font.atlas->Create(font.atlasW, font.atlasH, 32, 8, rgba.data())) {
            font.atlas->Release();
            font.atlas = nullptr;
            LOG("[TextBackend/STB] atlas texture upload failed for %s", desc.name ? desc.name : "<unnamed>");
            return false;
        }

        m_fonts.push_back(std::move(font));
        RefreshAllFontInfos();
        return true;
    }

    void UnregisterFont(TextFontHandle handle) override {
        for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
            if (m_fonts[i].handle == handle) {
                if (m_fonts[i].atlas) {
                    m_fonts[i].atlas->Release();
                    m_fonts[i].atlas = nullptr;
                }
                m_fonts.erase(m_fonts.begin() + i);
                RefreshAllFontInfos();
                return;
            }
        }
    }

    TextBounds Measure(Utf8TextView text, const TextRenderState& state) const override {
        TextBounds bounds = {};
        if (text.IsEmpty()) {
            return bounds;
        }

        const FontRecord* font = FindFont(state.font);
        if (!font) {
            return bounds;
        }

        LineWidthBuffer widths;
        BuildLineWidths(*font, text, state, widths);
        if (widths.empty()) {
            return bounds;
        }

        bounds.lineCount = (s32)widths.size();
        for (s32 i = 0; i < (s32)widths.size(); i++) {
            if (widths[(size_t)i] > bounds.width) {
                bounds.width = widths[(size_t)i];
            }
        }

        const f32 lineStep = font->lineHeightPx * state.scaleY + (f32)state.lineSpacing;
        bounds.height = font->lineHeightPx * state.scaleY;
        if (bounds.lineCount > 1) {
            bounds.height += (f32)(bounds.lineCount - 1) * lineStep;
        }
        return bounds;
    }

    s32 CountWrappedLines(Utf8TextView text, const TextRenderState& state) const override {
        return Measure(text, state).lineCount;
    }

    void Draw(Utf8TextView text, f32 x, f32 y, const TextRenderState& state) const override {
        if (text.IsEmpty()) {
            return;
        }

        const FontRecord* font = FindFont(state.font);
        if (!font || !font->atlas) {
            return;
        }

        LineWidthBuffer lineWidths;
        BuildLineWidths(*font, text, state, lineWidths);
        if (lineWidths.empty()) {
            return;
        }

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        static u32 pspTextDrawLogCount = 0;
        if (pspTextDrawLogCount < 12) {
            ++pspTextDrawLogCount;
            LOG("[TextBackend/PSP] draw #%u font=%u textBytes=%d pos=%.2f,%.2f scale=%.3f,%.3f color=%u atlas=%dx%d",
                pspTextDrawLogCount,
                state.font,
                text.length,
                static_cast<double>(x), static_cast<double>(y),
                static_cast<double>(state.scaleX), static_cast<double>(state.scaleY),
                static_cast<unsigned>(state.color.a),
                font->atlasW, font->atlasH);
        }
#endif

        const f32 lineHeight = font->lineHeightPx * state.scaleY;
        const f32 lineStep = lineHeight + (f32)state.lineSpacing;

        s32 cursor = 0;
        s32 lineIndex = 0;
        f32 penX = x + AlignmentOffset(state.align, lineWidths[0]);
        f32 lineTop = y;
        f32 baseline = lineTop + font->ascentPx * state.scaleY;
        f32 lineConsumed = 0.0f;
        bool atWordStart = true;
        f32 pendingSpace = 0.0f;

        ScreenDraw::BeginBatch();

        while (cursor < text.length) {
            const s32 tokenStart = cursor;
            const char c = text.data[cursor];

            if (c == '\r') {
                cursor++;
                continue;
            }

            if (c == '\n') {
                lineIndex++;
                if (lineIndex >= (s32)lineWidths.size()) {
                    break;
                }
                cursor++;
                penX = x + AlignmentOffset(state.align, lineWidths[(size_t)lineIndex]);
                lineTop = y + (f32)lineIndex * lineStep;
                baseline = lineTop + font->ascentPx * state.scaleY;
                lineConsumed = 0.0f;
                atWordStart = true;
                pendingSpace = 0.0f;
                continue;
            }

            if (state.wrapWidth > 0.0f && lineConsumed > 0.0f && atWordStart
                && c != ' ' && c != '\t') {
                const f32 wordWidth = MeasureUpcomingWordWidth(*font, text, tokenStart, state, lineHeight);
                if (wordWidth > 0.0f && (lineConsumed + pendingSpace + wordWidth) > state.wrapWidth) {
                    lineIndex++;
                    if (lineIndex >= (s32)lineWidths.size()) {
                        break;
                    }
                    penX = x + AlignmentOffset(state.align, lineWidths[(size_t)lineIndex]);
                    lineTop = y + (f32)lineIndex * lineStep;
                    baseline = lineTop + font->ascentPx * state.scaleY;
                    lineConsumed = 0.0f;
                    atWordStart = true;
                    pendingSpace = 0.0f;
                }
            }

            if (state.promptsEnabled && c == '<') {
                PromptToken token = HandlePromptToken(*font, state, text, tokenStart, penX, lineTop, lineHeight,
                    state.color.a, false);
                if (token.matched) {
                    if (pendingSpace > 0.0f && lineConsumed > 0.0f) {
                        penX += pendingSpace;
                        lineConsumed += pendingSpace;
                        pendingSpace = 0.0f;
                    }

                    if (token.width > 0.0f) {
                        HandlePromptToken(*font, state, text, tokenStart, penX, lineTop, lineHeight,
                            state.color.a, true);
                    }

                    cursor += token.consumed;
                    penX += token.width;
                    lineConsumed += token.width;
                    atWordStart = false;
                    continue;
                }
            }

            u32 codepoint = 0;
            if (!DecodeNextUtf8(text.data, text.length, cursor, codepoint)) {
                break;
            }

            if (codepoint == '\n' || codepoint == '\r') {
                continue;
            }

            u32 nextCodepoint = 0;
            s32 nextCursor = cursor;
            DecodeNextUtf8(text.data, text.length, nextCursor, nextCodepoint);

            const stbtt_packedchar* glyph = FindGlyph(*font, codepoint);
            if (!glyph) {
                glyph = FindGlyph(*font, (u32)'?');
                if (!glyph) {
                    continue;
                }
            }

            const f32 advance = GlyphAdvance(*font, codepoint, nextCodepoint, *glyph, state.scaleX);

            if (codepoint == ' ' || codepoint == '\t') {
                if (!atWordStart && lineConsumed > 0.0f) {
                    pendingSpace = advance;
                }
                atWordStart = true;
                continue;
            }

            if (pendingSpace > 0.0f && lineConsumed > 0.0f) {
                penX += pendingSpace;
                lineConsumed += pendingSpace;
                pendingSpace = 0.0f;
            }

            const f32 gx0 = penX + glyph->xoff * state.scaleX * font->metricScale;
            const f32 gy0 = baseline + glyph->yoff * state.scaleY * font->metricScale;
            const f32 gx1 = penX + glyph->xoff2 * state.scaleX * font->metricScale;
            const f32 gy1 = baseline + glyph->yoff2 * state.scaleY * font->metricScale;

            if (gx1 > gx0 && gy1 > gy0) {
                const f32 u0 = (f32)glyph->x0 / (f32)font->atlasW;
                const f32 v0 = (f32)glyph->y0 / (f32)font->atlasH;
                const f32 u1 = (f32)glyph->x1 / (f32)font->atlasW;
                const f32 v1 = (f32)glyph->y1 / (f32)font->atlasH;

                if (state.shadow.enabled) {
                    ScreenDraw::DrawQuad(font->atlas,
                        gx0 + state.shadow.offsetX,
                        gy0 + state.shadow.offsetY,
                        gx1 - gx0,
                        gy1 - gy0,
                        u0, v0, u1, v1,
                        state.shadow.color.r,
                        state.shadow.color.g,
                        state.shadow.color.b,
                        state.shadow.color.a);
                }

                if (state.outline.enabled && state.outline.thickness > 0.0f) {
                    const f32 t = state.outline.thickness;
                    static const f32 kDir[8][2] = {
                        {-1, -1}, {0, -1}, {1, -1},
                        {-1,  0},           {1,  0},
                        {-1,  1}, {0,  1}, {1,  1},
                    };
                    for (s32 d = 0; d < 8; d++) {
                        ScreenDraw::DrawQuad(font->atlas,
                            gx0 + kDir[d][0] * t,
                            gy0 + kDir[d][1] * t,
                            gx1 - gx0,
                            gy1 - gy0,
                            u0, v0, u1, v1,
                            state.outline.color.r,
                            state.outline.color.g,
                            state.outline.color.b,
                            state.outline.color.a);
                    }
                }

                ScreenDraw::DrawQuad(font->atlas,
                    gx0, gy0,
                    gx1 - gx0,
                    gy1 - gy0,
                    u0, v0, u1, v1,
                    state.color.r,
                    state.color.g,
                    state.color.b,
                    state.color.a);
            }

            penX += advance;
            lineConsumed += advance;
            atWordStart = false;
        }

        ScreenDraw::EndBatch();
    }

private:
    static bool RefreshFontInfo(FontRecord& font) {
        if (font.ttfData.empty()) {
            return false;
        }
        return stbtt_InitFont(&font.info, font.ttfData.data(), 0) != 0;
    }

    void RefreshAllFontInfos() {
        for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
            if (!RefreshFontInfo(m_fonts[(size_t)i])) {
                LOG("[TextBackend/STB] Failed to refresh font info for handle %u", m_fonts[(size_t)i].handle);
            }
        }
    }

    const FontRecord* FindFont(TextFontHandle handle) const {
        for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
            if (m_fonts[i].handle == handle) {
                return &m_fonts[i];
            }
        }
        return nullptr;
    }

    const stbtt_packedchar* FindGlyph(const FontRecord& font, u32 codepoint) const {
        for (s32 i = 0; i < (s32)font.ranges.size(); i++) {
            const PackedRange& range = font.ranges[(size_t)i];
            const u32 end = range.first + (u32)range.count;
            if (codepoint >= range.first && codepoint < end) {
                return &range.chars[(size_t)(codepoint - range.first)];
            }
        }
        return nullptr;
    }

    static f32 AlignmentOffset(TextAlign align, f32 lineWidth) {
        if (align == TextAlign_Center) {
            return -lineWidth * 0.5f;
        }
        if (align == TextAlign_Right) {
            return -lineWidth;
        }
        return 0.0f;
    }

    f32 GlyphAdvance(const FontRecord& font, u32 codepoint, u32 nextCodepoint,
        const stbtt_packedchar& glyph, f32 scaleX) const {
        s32 kern = 0;
        if (nextCodepoint) {
            kern = stbtt_GetCodepointKernAdvance(&font.info, (s32)codepoint, (s32)nextCodepoint);
        }
        return (glyph.xadvance + (f32)kern * font.scale) * scaleX * font.metricScale;
    }

    static bool IsWordBoundaryCodepoint(u32 codepoint) {
        return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r';
    }

    f32 MeasureUpcomingWordWidth(const FontRecord& font, Utf8TextView text, s32 start,
                                 const TextRenderState& state, f32 lineHeight) const {
        f32 width = 0.0f;
        s32 cursor = start;

        while (cursor < text.length) {
            const s32 tokenStart = cursor;
            const char c = text.data[cursor];

            if (c == '\r' || c == '\n') {
                break;
            }

            if (state.promptsEnabled && c == '<') {
                PromptToken token = HandlePromptToken(font, state, text, tokenStart, 0.0f, 0.0f,
                                                      lineHeight, state.color.a, false);
                if (token.matched) {
                    width += token.width;
                    cursor += token.consumed;
                    continue;
                }
            }

            u32 codepoint = 0;
            if (!DecodeNextUtf8(text.data, text.length, cursor, codepoint)) {
                break;
            }

            if (IsWordBoundaryCodepoint(codepoint)) {
                break;
            }

            u32 nextCodepoint = 0;
            s32 nextCursor = cursor;
            DecodeNextUtf8(text.data, text.length, nextCursor, nextCodepoint);

            const stbtt_packedchar* glyph = FindGlyph(font, codepoint);
            if (!glyph) {
                glyph = FindGlyph(font, (u32)'?');
                if (!glyph) {
                    continue;
                }
            }

            width += GlyphAdvance(font, codepoint, nextCodepoint, *glyph, state.scaleX);
        }

        return width;
    }

    // Measures/draws ASCII fallback text (e.g. "START", "CROSS") with the active font.
    f32 MeasureFallbackWidth(const FontRecord& font, const char* fallback, f32 scaleX) const {
        f32 width = 0.0f;
        for (s32 i = 0; fallback[i] != '\0'; i++) {
            const u32 codepoint = (u8)fallback[i];
            const stbtt_packedchar* glyph = FindGlyph(font, codepoint);
            if (glyph) {
                width += GlyphAdvance(font, codepoint, (u8)fallback[i + 1], *glyph, scaleX);
            }
        }
        return width;
    }

    void DrawFallbackText(const char* fallback, f32 x, f32 y, const TextRenderState& state,
        u8 alpha) const {
        TextRenderState fallbackState = state;
        fallbackState.promptsEnabled = false;
        fallbackState.wrapWidth = 0.0f;
        fallbackState.color.a = alpha;
        Draw(Utf8TextView(fallback), x, y, fallbackState);
    }

    PromptToken HandlePromptToken(const FontRecord& font, const TextRenderState& state,
        Utf8TextView text, s32 start, f32 x, f32 y,
        f32 lineHeight, u8 alpha, bool draw) const {
        PromptToken token = {};

#if CUSTOM_MENU
        s32 consumed = 0;
        PromptIcons::ResolvedPrompt prompt = {};
        Action action = ACTION_COUNT;

        if (PromptIcons::ParseInlineActionToken(text.data, start, &consumed, &action)) {
            token.matched = true;
            token.consumed = consumed;
            if (PromptIcons::ResolveActionPrompt(action, prompt)) {
                if (prompt.HasIcon()) {
                    token.width = PromptIcons::GetDrawWidth(prompt, lineHeight);
                    if (draw) {
                        PromptIcons::DrawPrompt(prompt, x, y, lineHeight, alpha);
                    }
#if defined(RC_PLATFORM_PSP)
                } else if (prompt.HasFallback()) {
                    // PSP has no glyph sheet for this action: draw its name in text instead.
                    token.width = MeasureFallbackWidth(font, prompt.fallback, state.scaleX);
                    if (draw) {
                        DrawFallbackText(prompt.fallback, x, y, state, alpha);
                    }
#endif
                }
            }
            return token;
        }

        s32 code = 0;
        if (PromptIcons::ParseInlineDesktopBindingToken(text.data, start, &consumed, &code)) {
            token.matched = true;
            token.consumed = consumed;
            if (PromptIcons::ResolveDesktopBindingCodePrompt(code, prompt)) {
                if (prompt.HasIcon()) {
                    token.width = PromptIcons::GetDrawWidth(prompt, lineHeight);
                    if (draw) {
                        PromptIcons::DrawPrompt(prompt, x, y, lineHeight, alpha);
                    }
#if defined(RC_PLATFORM_PSP)
                } else if (prompt.HasFallback()) {
                    token.width = MeasureFallbackWidth(font, prompt.fallback, state.scaleX);
                    if (draw) {
                        DrawFallbackText(prompt.fallback, x, y, state, alpha);
                    }
#endif
                }
            }
            return token;
        }
#else
        (void)text;
        (void)start;
        (void)x;
        (void)y;
        (void)lineHeight;
        (void)alpha;
        (void)draw;
#endif

        return token;
    }

    void BuildLineWidths(const FontRecord& font, Utf8TextView text,
        const TextRenderState& state, LineWidthBuffer& outLineWidths) const {
        outLineWidths.clear();
        if (text.IsEmpty()) {
            return;
        }

        s32 cursor = 0;
        f32 lineWidth = 0.0f;
        const f32 lineHeight = font.lineHeightPx * state.scaleY;
        bool atWordStart = true;
        f32 pendingSpace = 0.0f;

        while (cursor < text.length) {
            const s32 tokenStart = cursor;
            const char c = text.data[cursor];

            if (c == '\r') {
                cursor++;
                continue;
            }

            if (c == '\n') {
                outLineWidths.push_back(lineWidth);
                lineWidth = 0.0f;
                cursor++;
                atWordStart = true;
                pendingSpace = 0.0f;
                continue;
            }

            if (state.wrapWidth > 0.0f && lineWidth > 0.0f && atWordStart
                && c != ' ' && c != '\t') {
                const f32 wordWidth = MeasureUpcomingWordWidth(font, text, tokenStart, state, lineHeight);
                if (wordWidth > 0.0f && (lineWidth + pendingSpace + wordWidth) > state.wrapWidth) {
                    outLineWidths.push_back(lineWidth);
                    lineWidth = 0.0f;
                    atWordStart = true;
                    pendingSpace = 0.0f;
                }
            }

            if (state.promptsEnabled && c == '<') {
                PromptToken token = HandlePromptToken(font, state, text, tokenStart, 0.0f, 0.0f,
                    lineHeight, state.color.a, false);
                if (token.matched) {
                    if (pendingSpace > 0.0f && lineWidth > 0.0f) {
                        lineWidth += pendingSpace;
                        pendingSpace = 0.0f;
                    }
                    lineWidth += token.width;
                    cursor += token.consumed;
                    atWordStart = false;
                    continue;
                }
            }

            u32 codepoint = 0;
            if (!DecodeNextUtf8(text.data, text.length, cursor, codepoint)) {
                break;
            }
            if (codepoint == '\n' || codepoint == '\r') {
                continue;
            }

            u32 nextCodepoint = 0;
            s32 nextCursor = cursor;
            DecodeNextUtf8(text.data, text.length, nextCursor, nextCodepoint);

            const stbtt_packedchar* glyph = FindGlyph(font, codepoint);
            if (!glyph) {
                glyph = FindGlyph(font, (u32)'?');
                if (!glyph) {
                    continue;
                }
            }

            const f32 advance = GlyphAdvance(font, codepoint, nextCodepoint, *glyph, state.scaleX);

            if (codepoint == ' ' || codepoint == '\t') {
                if (!atWordStart && lineWidth > 0.0f) {
                    pendingSpace = advance;
                }
                atWordStart = true;
                continue;
            }

            if (pendingSpace > 0.0f && lineWidth > 0.0f) {
                lineWidth += pendingSpace;
                pendingSpace = 0.0f;
            }

            lineWidth += advance;
            atWordStart = false;
        }

        outLineWidths.push_back(lineWidth);
    }

    std::vector<FontRecord> m_fonts = {};
    bool m_initialized = false;
};

ITextBackend* CreateDefaultTextBackend() {
    return new StbTextBackend();
}

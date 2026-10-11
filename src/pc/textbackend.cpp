#include "pc/textbackend.h"

#include "pc/log.h"
#include "pc/textoutline.h"
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
    // A strike within this scale of the requested size draws exactly 1:1.
    static constexpr f32 kStrikeSnapTolerance = 0.12f;
    static constexpr s16 kKernUnknown = 0x7FFF;
    static constexpr s32 kAsciiFirst = 32;
    static constexpr s32 kAsciiCount = 95;

    struct PackedRange {
        u32 first = 0;
        s32 count = 0;
        std::vector<stbtt_packedchar> chars = {};
    };

    // One rasterisation of a font at a given pixel height.
    struct FontStrike {
        s32 pixelHeight = 0;
        f32 scale = 1.0f;  // font units -> strike pixels
        std::vector<PackedRange> ranges = {};
        tTexture* atlas = nullptr;
        s32 atlasW = 0;
        s32 atlasH = 0;
        // Texels carry a 1px black outline (see BakeTextOutline).
        bool bakedOutline = false;
        // The atlas is the backend's shared one, not owned by this strike.
        bool sharedAtlas = false;
    };

    // Baked-outline strikes are packed together, so all outlined UI text
    // draws from one texture; its solid texel lets colour fills join the
    // same batch (ScreenDraw::SetSolidTexel).
    struct SharedAtlas {
        std::vector<u8> coverage = {};
        stbtt_pack_context pack = {};
        bool open = false;
        bool full = false;
        tTexture* texture = nullptr;
        f32 solidU = 0.0f;
        f32 solidV = 0.0f;
    };
    static constexpr s32 kSharedAtlasSize = 512;

    struct FontRecord {
        TextFontHandle handle = 0;
        stbtt_fontinfo info = {};
        std::vector<u8> ttfData = {};
        // [0] is the registration strike; AddStrike appends exact sizes.
        std::vector<FontStrike> strikes = {};
        // Line metrics in kLayoutReferencePixelHeight units.
        f32 ascentPx = 0.0f;
        f32 lineHeightPx = 0.0f;
        bool hasKerning = false;
        // Printable-ASCII pair kerning in font units, filled on first use.
        mutable std::vector<s16> asciiKern = {};
    };

    // The strike chosen for one Draw/Measure call.
    struct Face {
        const FontRecord* font = nullptr;
        const FontStrike* strike = nullptr;
        f32 sx = 1.0f;         // strike pixels -> screen pixels
        f32 sy = 1.0f;
        f32 lineScale = 1.0f;  // reference units -> screen pixels (vertical)
        bool snap = false;     // 1:1 strike: glyphs sit on whole pixels
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
            ReleaseStrikes(m_fonts[(size_t)i]);
        }
        m_fonts.clear();
        ReleaseSharedAtlas();
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
        font.hasKerning = font.info.kern != 0 || font.info.gpos != 0;

        FontStrike base = {};
        base.pixelHeight = desc.pixelHeight;
        base.scale = stbtt_ScaleForPixelHeight(&font.info, (f32)desc.pixelHeight);
        const f32 metricScale = (f32)kLayoutReferencePixelHeight / (f32)desc.pixelHeight;

        // stb_truetype's public ABI uses native C++ int regardless of the
        // platform typedefs used by the game (s32 is long on PSP).
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &lineGap);
        font.ascentPx = (f32)ascent * base.scale * metricScale;
        font.lineHeightPx = ((f32)(ascent - descent + lineGap)) * base.scale * metricScale;
        if (font.lineHeightPx < 1.0f) {
            font.lineHeightPx = (f32)kLayoutReferencePixelHeight;
        }

        std::vector<u8> alpha;
        if (!PackStrike(font, base, kAtlasSize, kAtlasSize, 0, alpha)) {
            LOG("[TextBackend/STB] stbtt_PackFontRanges failed for %s", desc.name ? desc.name : "<unnamed>");
            return false;
        }

        std::vector<u32> rgba(alpha.size(), 0);
        for (size_t i = 0; i < alpha.size(); i++) {
            rgba[i] = PackRgba(255, 255, 255, alpha[i]);
        }

        if (!CreateStrikeAtlas(base, rgba)) {
            LOG("[TextBackend/STB] atlas texture upload failed for %s", desc.name ? desc.name : "<unnamed>");
            return false;
        }

        font.strikes.push_back(std::move(base));
        m_fonts.push_back(std::move(font));
        RefreshAllFontInfos();
        return true;
    }

    bool AddStrike(TextFontHandle handle, s32 pixelHeight, bool bakedOutline) override {
        FontRecord* font = FindMutableFont(handle);
        if (!font || pixelHeight <= 0) {
            return false;
        }
        for (const FontStrike& existing : font->strikes) {
            if (existing.pixelHeight == pixelHeight && existing.bakedOutline == bakedOutline) {
                return true;
            }
        }

        FontStrike strike = {};
        strike.pixelHeight = pixelHeight;
        strike.scale = stbtt_ScaleForPixelHeight(&font->info, (f32)pixelHeight);
        strike.bakedOutline = bakedOutline;

        if (bakedOutline && PackSharedStrike(*font, strike)) {
            font->strikes.push_back(std::move(strike));
            return true;
        }

        // Smallest atlas that holds the glyph set. The baked outline grows
        // each glyph by a texel, so keep a texel of margin around the pack.
        static const s32 kAtlasDims[][2] = {
            { 128, 64 }, { 128, 128 }, { 256, 128 }, { 256, 256 }, { 512, 256 }, { 512, 512 },
        };
        const s32 margin = bakedOutline ? 1 : 0;
        std::vector<u8> alpha;
        bool packed = false;
        for (const auto& dims : kAtlasDims) {
            if (PackStrike(*font, strike, dims[0], dims[1], margin, alpha)) {
                packed = true;
                break;
            }
        }
        if (!packed) {
            LOG("[TextBackend/STB] strike %dpx does not fit a 512² atlas", (int)pixelHeight);
            return false;
        }

        std::vector<u32> rgba(alpha.size(), 0);
        if (bakedOutline) {
            BakeTextOutline(alpha.data(), strike.atlasW, strike.atlasH, rgba.data());
        }
        else {
            for (size_t i = 0; i < alpha.size(); i++) {
                rgba[i] = PackRgba(255, 255, 255, alpha[i]);
            }
        }

        if (!CreateStrikeAtlas(strike, rgba)) {
            LOG("[TextBackend/STB] strike %dpx atlas upload failed", (int)pixelHeight);
            return false;
        }

        font->strikes.push_back(std::move(strike));
        return true;
    }

    void UnregisterFont(TextFontHandle handle) override {
        for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
            if (m_fonts[i].handle == handle) {
                ReleaseStrikes(m_fonts[(size_t)i]);
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

        const Face face = ResolveFace(*font, state);
        LineWidthBuffer widths;
        BuildLineWidths(face, text, state, widths);
        if (widths.empty()) {
            return bounds;
        }

        bounds.lineCount = (s32)widths.size();
        for (s32 i = 0; i < (s32)widths.size(); i++) {
            if (widths[(size_t)i] > bounds.width) {
                bounds.width = widths[(size_t)i];
            }
        }

        const f32 lineStep = font->lineHeightPx * face.lineScale + (f32)state.lineSpacing;
        bounds.height = font->lineHeightPx * face.lineScale;
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
        if (!font) {
            return;
        }

        const Face face = ResolveFace(*font, state);
        const FontStrike& strike = *face.strike;
        if (!strike.atlas) {
            return;
        }

        LineWidthBuffer lineWidths;
        BuildLineWidths(face, text, state, lineWidths);
        if (lineWidths.empty()) {
            return;
        }

#if defined(RC_PLATFORM_PSP) && !defined(NDEBUG)
        static u32 pspTextDrawLogCount = 0;
        if (pspTextDrawLogCount < 12) {
            ++pspTextDrawLogCount;
            LOG("[TextBackend/PSP] draw #%u font=%u textBytes=%d pos=%.2f,%.2f scale=%.3f,%.3f strike=%dpx%s color=%u atlas=%dx%d",
                pspTextDrawLogCount,
                state.font,
                text.length,
                static_cast<double>(x), static_cast<double>(y),
                static_cast<double>(state.scaleX), static_cast<double>(state.scaleY),
                (int)strike.pixelHeight, face.snap ? " 1:1" : "",
                static_cast<unsigned>(state.color.a),
                strike.atlasW, strike.atlasH);
        }
#endif

        const f32 lineHeight = font->lineHeightPx * face.lineScale;
        const f32 lineStep = lineHeight + (f32)state.lineSpacing;
        const f32 ascent = font->ascentPx * face.lineScale;
        // A baked-outline glyph is drawn one texel larger on every side.
        const f32 bakedGrowX = strike.bakedOutline ? face.sx : 0.0f;
        const f32 bakedGrowY = strike.bakedOutline ? face.sy : 0.0f;
        const f32 bakedGrowTexels = strike.bakedOutline ? 1.0f : 0.0f;
        const f32 invAtlasW = 1.0f / (f32)strike.atlasW;
        const f32 invAtlasH = 1.0f / (f32)strike.atlasH;

        s32 cursor = 0;
        s32 lineIndex = 0;
        f32 penX = x + AlignmentOffset(state.align, lineWidths[0]);
        f32 lineTop = y;
        f32 baseline = Baseline(face, lineTop, ascent);
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
                baseline = Baseline(face, lineTop, ascent);
                lineConsumed = 0.0f;
                atWordStart = true;
                pendingSpace = 0.0f;
                continue;
            }

            if (state.wrapWidth > 0.0f && lineConsumed > 0.0f && atWordStart
                && c != ' ' && c != '\t') {
                const f32 wordWidth = MeasureUpcomingWordWidth(face, text, tokenStart, state, lineHeight);
                if (wordWidth > 0.0f && (lineConsumed + pendingSpace + wordWidth) > state.wrapWidth) {
                    lineIndex++;
                    if (lineIndex >= (s32)lineWidths.size()) {
                        break;
                    }
                    penX = x + AlignmentOffset(state.align, lineWidths[(size_t)lineIndex]);
                    lineTop = y + (f32)lineIndex * lineStep;
                    baseline = Baseline(face, lineTop, ascent);
                    lineConsumed = 0.0f;
                    atWordStart = true;
                    pendingSpace = 0.0f;
                }
            }

            if (state.promptsEnabled && c == '<') {
                PromptToken token = HandlePromptToken(face, state, text, tokenStart, penX, lineTop, lineHeight,
                    state.color.a, false);
                if (token.matched) {
                    if (pendingSpace > 0.0f && lineConsumed > 0.0f) {
                        penX += pendingSpace;
                        lineConsumed += pendingSpace;
                        pendingSpace = 0.0f;
                    }

                    if (token.width > 0.0f) {
                        HandlePromptToken(face, state, text, tokenStart, penX, lineTop, lineHeight,
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

            const stbtt_packedchar* glyph = FindGlyph(strike, codepoint);
            if (!glyph) {
                glyph = FindGlyph(strike, (u32)'?');
                if (!glyph) {
                    continue;
                }
            }

            const f32 advance = GlyphAdvance(face, codepoint, nextCodepoint, *glyph);

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

            f32 gx0 = penX + glyph->xoff * face.sx;
            f32 gy0 = baseline + glyph->yoff * face.sy;
            f32 gx1 = penX + glyph->xoff2 * face.sx;
            f32 gy1 = baseline + glyph->yoff2 * face.sy;
            if (face.snap) {
                gx0 = std::floor(gx0 + 0.5f);
                gy0 = std::floor(gy0 + 0.5f);
                gx1 = gx0 + (f32)(glyph->x1 - glyph->x0);
                gy1 = gy0 + (f32)(glyph->y1 - glyph->y0);
            }

            if (gx1 > gx0 && gy1 > gy0) {
                gx0 -= bakedGrowX;
                gy0 -= bakedGrowY;
                gx1 += bakedGrowX;
                gy1 += bakedGrowY;
                const f32 u0 = ((f32)glyph->x0 - bakedGrowTexels) * invAtlasW;
                const f32 v0 = ((f32)glyph->y0 - bakedGrowTexels) * invAtlasH;
                const f32 u1 = ((f32)glyph->x1 + bakedGrowTexels) * invAtlasW;
                const f32 v1 = ((f32)glyph->y1 + bakedGrowTexels) * invAtlasH;

                if (state.shadow.enabled) {
                    ScreenDraw::DrawQuad(strike.atlas,
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

                if (!strike.bakedOutline && state.outline.enabled && state.outline.thickness > 0.0f) {
                    const f32 t = state.outline.thickness;
                    static const f32 kDir[8][2] = {
                        {-1, -1}, {0, -1}, {1, -1},
                        {-1,  0},           {1,  0},
                        {-1,  1}, {0,  1}, {1,  1},
                    };
                    for (s32 d = 0; d < 8; d++) {
                        ScreenDraw::DrawQuad(strike.atlas,
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

                ScreenDraw::DrawQuad(strike.atlas,
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

    static void ReleaseStrikes(FontRecord& font) {
        for (FontStrike& strike : font.strikes) {
            if (strike.atlas && !strike.sharedAtlas) {
                strike.atlas->Release();
            }
            strike.atlas = nullptr;
        }
    }

    void ReleaseSharedAtlas() {
        if (m_shared.open) {
            stbtt_PackEnd(&m_shared.pack);
        }
        if (m_shared.texture) {
#if defined(RC_PLATFORM_PSP)
            ScreenDraw::SetSolidTexel(nullptr, 0.0f, 0.0f);
#endif
            m_shared.texture->Release();
        }
        m_shared = SharedAtlas{};
    }

    static void InitStrikeRanges(FontStrike& strike) {
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

        strike.ranges.clear();
        for (s32 i = 0; i < (s32)(sizeof(kRanges) / sizeof(kRanges[0])); i++) {
            PackedRange range = {};
            range.first = kRanges[i].first;
            range.count = kRanges[i].count;
            range.chars.resize((size_t)range.count);
            strike.ranges.push_back(range);
        }
    }

    static bool PackStrikeRanges(stbtt_pack_context& pack, const FontRecord& font, FontStrike& strike) {
        std::vector<stbtt_pack_range> packRanges(strike.ranges.size());
        for (s32 i = 0; i < (s32)strike.ranges.size(); i++) {
            packRanges[i].font_size = (f32)strike.pixelHeight;
            packRanges[i].first_unicode_codepoint_in_range = (s32)strike.ranges[i].first;
            packRanges[i].num_chars = strike.ranges[i].count;
            packRanges[i].chardata_for_range = strike.ranges[i].chars.data();
            packRanges[i].array_of_unicode_codepoints = nullptr;
        }
        return stbtt_PackFontRanges(&pack, font.ttfData.data(), 0,
            packRanges.data(), (s32)packRanges.size()) != 0;
    }

    static void OffsetStrikeGlyphs(FontStrike& strike, s32 margin) {
        for (PackedRange& range : strike.ranges) {
            for (stbtt_packedchar& ch : range.chars) {
                ch.x0 = (unsigned short)(ch.x0 + margin);
                ch.x1 = (unsigned short)(ch.x1 + margin);
                ch.y0 = (unsigned short)(ch.y0 + margin);
                ch.y1 = (unsigned short)(ch.y1 + margin);
            }
        }
    }

    // Packs a baked-outline strike into the shared atlas and re-bakes it.
    // False when the atlas is full: the strike then gets its own atlas.
    bool PackSharedStrike(const FontRecord& font, FontStrike& strike) {
        SharedAtlas& atlas = m_shared;
        const s32 size = kSharedAtlasSize;
        // One texel of margin around the pack keeps room for the baked ring.
        const s32 margin = 1;
        if (atlas.full) {
            return false;
        }
        if (!atlas.open) {
            atlas.coverage.assign((size_t)size * (size_t)size, 0);
            u8* origin = atlas.coverage.data() + (size_t)margin * (size_t)size + (size_t)margin;
            if (!stbtt_PackBegin(&atlas.pack, origin, size - margin * 2, size - margin * 2, size,
                kPackPadding, nullptr)) {
                atlas.full = true;
                return false;
            }
            stbtt_PackSetOversampling(&atlas.pack, kOversampleX, kOversampleY);
            atlas.open = true;

            // An opaque 4x4 block: its centre stays pure white after baking.
            stbrp_rect solid = {};
            solid.w = 4 + kPackPadding;
            solid.h = 4 + kPackPadding;
            stbtt_PackFontRangesPackRects(&atlas.pack, &solid, 1);
            if (solid.was_packed) {
                for (s32 y = 0; y < 4; y++) {
                    u8* row = origin + (size_t)(solid.y + y) * (size_t)size + (size_t)solid.x;
                    std::memset(row, 255, 4);
                }
                atlas.solidU = ((f32)(solid.x + margin) + 2.0f) / (f32)size;
                atlas.solidV = ((f32)(solid.y + margin) + 2.0f) / (f32)size;
            }
        }

        InitStrikeRanges(strike);
        if (!PackStrikeRanges(atlas.pack, font, strike)) {
            // Glyphs that did fit stay in the coverage unused; stop sharing.
            atlas.full = true;
            return false;
        }
        OffsetStrikeGlyphs(strike, margin);

        std::vector<u32> rgba((size_t)size * (size_t)size, 0);
        BakeTextOutline(atlas.coverage.data(), size, size, rgba.data());
        if (!atlas.texture) {
            atlas.texture = new tTexture();
        }
        if (!atlas.texture->Create(size, size, 32, 8, rgba.data())) {
            atlas.full = true;
            return false;
        }
#if defined(RC_PLATFORM_PSP)
        ScreenDraw::SetSolidTexel(atlas.texture, atlas.solidU, atlas.solidV);
#endif

        strike.atlas = atlas.texture;
        strike.atlasW = size;
        strike.atlasH = size;
        strike.sharedAtlas = true;
        return true;
    }

    // Packs the glyph ranges into an atlasW x atlasH coverage image, keeping
    // `margin` empty texels around the packed area.
    static bool PackStrike(const FontRecord& font, FontStrike& strike, s32 atlasW, s32 atlasH,
                           s32 margin, std::vector<u8>& alpha) {
        InitStrikeRanges(strike);
        strike.atlasW = atlasW;
        strike.atlasH = atlasH;
        alpha.assign((size_t)atlasW * (size_t)atlasH, 0);

        stbtt_pack_context pack = {};
        u8* origin = alpha.data() + (size_t)margin * (size_t)atlasW + (size_t)margin;
        if (!stbtt_PackBegin(&pack, origin, atlasW - margin * 2, atlasH - margin * 2, atlasW,
            kPackPadding, nullptr)) {
            return false;
        }

        stbtt_PackSetOversampling(&pack, kOversampleX, kOversampleY);
        const bool packed = PackStrikeRanges(pack, font, strike);
        stbtt_PackEnd(&pack);
        if (!packed) {
            return false;
        }

        if (margin > 0) {
            OffsetStrikeGlyphs(strike, margin);
        }
        return true;
    }

    static bool CreateStrikeAtlas(FontStrike& strike, const std::vector<u32>& rgba) {
        strike.atlas = new tTexture();
        if (!strike.atlas->Create(strike.atlasW, strike.atlasH, 32, 8, rgba.data())) {
            strike.atlas->Release();
            strike.atlas = nullptr;
            return false;
        }
        return true;
    }

    const FontRecord* FindFont(TextFontHandle handle) const {
        for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
            if (m_fonts[i].handle == handle) {
                return &m_fonts[i];
            }
        }
        return nullptr;
    }

    FontRecord* FindMutableFont(TextFontHandle handle) {
        for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
            if (m_fonts[i].handle == handle) {
                return &m_fonts[i];
            }
        }
        return nullptr;
    }

    static f32 StrikeDistance(s32 pixelHeight, f32 targetPixelHeight) {
        const f32 px = (f32)pixelHeight;
        return (px > targetPixelHeight) ? (px / targetPixelHeight) : (targetPixelHeight / px);
    }

    // Picks the rasterisation closest to the requested on-screen size.
    // Baked-outline strikes only serve text with a black outline, the one
    // style their texels can reproduce.
    static Face ResolveFace(const FontRecord& font, const TextRenderState& state) {
        Face face = {};
        face.font = &font;
        face.strike = &font.strikes[0];

        const f32 target = (f32)kLayoutReferencePixelHeight * state.scaleY;
        if (font.strikes.size() > 1 && target > 0.0f) {
            const bool blackOutline = state.outline.enabled && state.outline.thickness > 0.0f
                && state.outline.color.r == 0 && state.outline.color.g == 0 && state.outline.color.b == 0;
            f32 best = StrikeDistance(face.strike->pixelHeight, target);
            for (size_t i = 1; i < font.strikes.size(); i++) {
                const FontStrike& candidate = font.strikes[i];
                if (candidate.bakedOutline != blackOutline) {
                    continue;
                }
                const f32 distance = StrikeDistance(candidate.pixelHeight, target);
                if (distance < best) {
                    best = distance;
                    face.strike = &candidate;
                }
            }
        }

        const f32 px = (f32)face.strike->pixelHeight;
        face.sx = state.scaleX * (f32)kLayoutReferencePixelHeight / px;
        face.sy = state.scaleY * (f32)kLayoutReferencePixelHeight / px;
        if (face.strike != &font.strikes[0]
            && std::fabs(face.sx - 1.0f) <= kStrikeSnapTolerance
            && std::fabs(face.sy - 1.0f) <= kStrikeSnapTolerance) {
            face.sx = 1.0f;
            face.sy = 1.0f;
            face.snap = true;
        }
        face.lineScale = face.sy * px / (f32)kLayoutReferencePixelHeight;
        return face;
    }

    static f32 Baseline(const Face& face, f32 lineTop, f32 ascent) {
        const f32 baseline = lineTop + ascent;
        return face.snap ? std::floor(baseline + 0.5f) : baseline;
    }

    static const stbtt_packedchar* FindGlyph(const FontStrike& strike, u32 codepoint) {
        for (s32 i = 0; i < (s32)strike.ranges.size(); i++) {
            const PackedRange& range = strike.ranges[(size_t)i];
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

    // Pair kerning in font units. stb_truetype resolves each pair through
    // cmap and kern/GPOS lookups, so printable-ASCII pairs are memoised.
    static s32 KernUnits(const FontRecord& font, u32 codepoint, u32 nextCodepoint) {
        if (!nextCodepoint || !font.hasKerning) {
            return 0;
        }

        const u32 first = codepoint - (u32)kAsciiFirst;
        const u32 second = nextCodepoint - (u32)kAsciiFirst;
        if (first < (u32)kAsciiCount && second < (u32)kAsciiCount) {
            if (font.asciiKern.empty()) {
                font.asciiKern.assign((size_t)kAsciiCount * (size_t)kAsciiCount, kKernUnknown);
            }
            s16& cached = font.asciiKern[(size_t)first * (size_t)kAsciiCount + second];
            if (cached == kKernUnknown) {
                cached = (s16)stbtt_GetCodepointKernAdvance(&font.info, (s32)codepoint, (s32)nextCodepoint);
            }
            return cached;
        }

        return stbtt_GetCodepointKernAdvance(&font.info, (s32)codepoint, (s32)nextCodepoint);
    }

    static f32 GlyphAdvance(const Face& face, u32 codepoint, u32 nextCodepoint,
        const stbtt_packedchar& glyph) {
        const s32 kern = KernUnits(*face.font, codepoint, nextCodepoint);
        return (glyph.xadvance + (f32)kern * face.strike->scale) * face.sx;
    }

    static bool IsWordBoundaryCodepoint(u32 codepoint) {
        return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r';
    }

    f32 MeasureUpcomingWordWidth(const Face& face, Utf8TextView text, s32 start,
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
                PromptToken token = HandlePromptToken(face, state, text, tokenStart, 0.0f, 0.0f,
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

            const stbtt_packedchar* glyph = FindGlyph(*face.strike, codepoint);
            if (!glyph) {
                glyph = FindGlyph(*face.strike, (u32)'?');
                if (!glyph) {
                    continue;
                }
            }

            width += GlyphAdvance(face, codepoint, nextCodepoint, *glyph);
        }

        return width;
    }

    // Measures/draws ASCII fallback text (e.g. "START", "CROSS") with the active font.
    static f32 MeasureFallbackWidth(const Face& face, const char* fallback) {
        f32 width = 0.0f;
        for (s32 i = 0; fallback[i] != '\0'; i++) {
            const u32 codepoint = (u8)fallback[i];
            const stbtt_packedchar* glyph = FindGlyph(*face.strike, codepoint);
            if (glyph) {
                width += GlyphAdvance(face, codepoint, (u8)fallback[i + 1], *glyph);
            }
        }
        return width;
    }

    void DrawFallbackText(const char* fallback, f32 x, f32 y, const TextRenderState& state,
        u8 alpha) const {
        TextRenderState fallbackState = state;
        fallbackState.promptsEnabled = false;
        fallbackState.wrapWidth = 0.0f;
        // x is the pen position inside the parent line, already aligned.
        fallbackState.align = TextAlign_Left;
        fallbackState.color.a = alpha;
        Draw(Utf8TextView(fallback), x, y, fallbackState);
    }

    PromptToken HandlePromptToken(const Face& face, const TextRenderState& state,
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
                    token.width = MeasureFallbackWidth(face, prompt.fallback);
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
                    token.width = MeasureFallbackWidth(face, prompt.fallback);
                    if (draw) {
                        DrawFallbackText(prompt.fallback, x, y, state, alpha);
                    }
#endif
                }
            }
            return token;
        }
#else
        (void)face;
        (void)state;
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

    void BuildLineWidths(const Face& face, Utf8TextView text,
        const TextRenderState& state, LineWidthBuffer& outLineWidths) const {
        outLineWidths.clear();
        if (text.IsEmpty()) {
            return;
        }

        s32 cursor = 0;
        f32 lineWidth = 0.0f;
        const f32 lineHeight = face.font->lineHeightPx * face.lineScale;
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
                const f32 wordWidth = MeasureUpcomingWordWidth(face, text, tokenStart, state, lineHeight);
                if (wordWidth > 0.0f && (lineWidth + pendingSpace + wordWidth) > state.wrapWidth) {
                    outLineWidths.push_back(lineWidth);
                    lineWidth = 0.0f;
                    atWordStart = true;
                    pendingSpace = 0.0f;
                }
            }

            if (state.promptsEnabled && c == '<') {
                PromptToken token = HandlePromptToken(face, state, text, tokenStart, 0.0f, 0.0f,
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

            const stbtt_packedchar* glyph = FindGlyph(*face.strike, codepoint);
            if (!glyph) {
                glyph = FindGlyph(*face.strike, (u32)'?');
                if (!glyph) {
                    continue;
                }
            }

            const f32 advance = GlyphAdvance(face, codepoint, nextCodepoint, *glyph);

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
    SharedAtlas m_shared = {};
    bool m_initialized = false;
};

ITextBackend* CreateDefaultTextBackend() {
    return new StbTextBackend();
}

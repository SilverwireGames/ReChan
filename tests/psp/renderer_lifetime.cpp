// Exercise the production renderer against delayed GE reads, not a duplicate
// implementation of the cache. Run with scripts/test_psp_renderer.sh.
#include <random>
#include <array>
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include <limits>
#define private public
#include "pddi/psp/psprender.h"
#undef private
#include "../../vendor/libp3d/pddi/psp/psprender.cpp"

namespace PspPerf {
void BeginFrame() {}
void RecordDrawSubmission(u32) {}
void RecordPageCacheMiss() {}
void RecordMemorySnapshot(u32, u32, u32, u32) {}
void AddRendererMemory(s32) {}
void Trace(const char*) {}
}
using FakeGe::require;

int main() {
    // Signed intermediate conversion must preserve saturation and fractional
    // truncation, not just the 256 quantized PSX channel values.
    for (int i = -4096; i <= 12288; ++i) {
        const float value = static_cast<float>(i) / 4096.0f;
        const auto reference = static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
        require(ToPspVertexByte(value) == reference, "signed colour conversion changed output");
    }
    s16 packedUv = 0;
    for (int i = -32768; i <= 32767; ++i) {
        require(PackIntegralTexCoord(static_cast<float>(i), &packedUv)
            && packedUv == i, "integral UV packing changed");
        require(!PackIntegralTexCoord(static_cast<float>(i) + 0.5f, &packedUv),
            "fractional UV must use float vertices");
    }
    for (float value : { -32769.0f, 32768.0f,
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity() }) {
        require(!PackIntegralTexCoord(value, &packedUv), "invalid UV accepted");
    }
    require(MakeColor4444(0xffffffffu) == 0xffffu, "sprite white lost opacity");
    require(MakeColor4444(0x12345678u) == 0x1357u, "sprite RGBA channels crossed");
    require(MakeColor4444(0x00ffffffu) == 0x0fffu, "transparent sprite gained opacity");
    // The shared model path supplies PSX channels divided by 128. Increasing
    // light past neutral (128) must never make a vertex darker on the GE.
    pspPrimBuffer colourBuffer({PDDI_PRIM_TRIANGLES,
        PDDI_V_POSITION | PDDI_V_COLOUR, 256, 0});
    std::vector<float> colours(256 * 6, 0.0f);
    for (int i = 0; i < 256; ++i) {
        colours[i * 6 + 3] = colours[i * 6 + 4] = colours[i * 6 + 5] = i / 128.0f;
    }
    colourBuffer.SetVertexData(colours.data(), 256);
    require(colourBuffer.vertices[0].color == 0xff000000u, "unlit geometry must stay black");
    require(colourBuffer.vertices[64].color == 0xff7f7f7fu, "half intensity geometry");
    require(colourBuffer.vertices[128].color == 0xffffffffu, "neutral geometry must stay white");
    for (int i = 1; i < 256; ++i) {
        require((colourBuffer.vertices[i].color & 255u)
            >= (colourBuffer.vertices[i - 1].color & 255u), "increasing light darkens geometry");
        if (i >= 128) require(colourBuffer.vertices[i].color == 0xffffffffu,
            "overbright geometry must saturate, not wrap or dim");
    }
    pspDisplay display;
    require(display.InitDisplay(pddiDisplayInit{}), "display init");
    require(g_staticVram.initialized && !g_staticVram.frameBuffer, "offset zero is valid");
    require(g_staticVram.allocatedBytes == 2048000, "bounded eDRAM layout");
    auto context = std::make_unique<pspContext>(&display);
    auto& ctx = *context;
    std::vector<u16> image(1024 * 512, 0x4321);
    const u32 handle = ctx.CreateVRAMTexture(1024, 512, image.data());
    ctx.SetVRAMHandle(handle);
    ctx.BeginFrame();
    ctx.SetBlendMode(PDDI_BLEND_SUBTRACT);
    // A near-zero edge shade must preserve the floor, not clamp it to black.
    require(FakeGe::blendEquation == GU_REVERSE_SUBTRACT,
        "shadow must subtract its shade from the scene");
    ctx.EnableZBuffer(true);
    ctx.Setup3DState();
    require(FakeGe::depthMask == GU_TRUE, "blended shadow must not occlude later geometry");
    ctx.SetBlendMode(PDDI_BLEND_NONE);
    require(FakeGe::depthMask == GU_FALSE, "opaque geometry must restore depth writes");
    // Native T4/T8/5551 copies have exactly the packed byte extent. Blend bits
    // cannot create duplicate indexed cache entries.
    auto* t4 = ctx.GetDecodedPage(handle, 0x42);
    require(t4 == ctx.GetDecodedPage(handle, 0x02), "blend bits duplicate page");
    ctx.GetDecodedPage(handle, 0x82);
    ctx.GetDecodedPage(handle, 0x102);
#ifndef PSP_DISABLE_STATIC_PAGE_UPLOAD
    require(FakeGe::copyFormats == std::array<int, 3>{1, 1, 1}, "all upload depths covered");
#endif
    // Invalidate after submission: no early slot release, RAM overwrite or
    // stale hit, even when the same texture is destroyed/recreated mid-list.
    const void* oldGpu = t4->gpuPixels;
    image[2 * 64] = 0xabcd;
    ctx.UpdateVRAMTextureRegion(handle, 1024, 512, image.data(), 128, 0, 1, 1);
    require(t4->handle == 0 && t4->inFlight && t4->gpuPixels == oldGpu, "invalidation retired live slot");
    auto* updated = ctx.GetDecodedPage(handle, 2);
    require(updated != t4 && updated->pixels[0] == 0xabcd && t4->pixels[0] == 0x4321, "mutable page snapshot");
    ctx.DestroyVRAMTexture(handle);
    require(ctx.CreateVRAMTexture(1024, 512, image.data()) == handle, "reserved VRAM reuse");
    ctx.BindPage(2, 0x2674);
    require(FakeGe::alphaEnabled, "keyed pixels must fail alpha test");
    // More than 16 distinct pages and more than 256 palettes: both must
    // continue with a fence, never return an untextured draw or mutable CLUT.
    const int beforePressure = FakeGe::syncs;
    for (unsigned int i = 0; i < 40; ++i) {
        require(ctx.GetDecodedPage(handle, static_cast<u16>((i % 32) | ((i / 32) << 7))), "cache saturation dropped page");
    }
    for (unsigned int i = 0; i < 600; ++i) ctx.BindPage(2, static_cast<float>(i));
    require(FakeGe::syncs > beforePressure, "pressure must retire pending reads");
    display.SwapBuffers();
    // Wrapped T8 and 16-bit pages include x=0 even when origin is x=960.
    ctx.BeginFrame();
    auto* wrapped8 = ctx.GetDecodedPage(handle, 0x8f);
    auto* wrapped16 = ctx.GetDecodedPage(handle, 0x10f);
    ctx.UpdateVRAMTextureRegion(handle, 1024, 512, image.data(), 0, 0, 1, 1);
    require(!wrapped8->handle && !wrapped16->handle, "wrapped page not invalidated");
    auto* page = ctx.GetDecodedPage(handle, 2);
    ctx.UpdateVRAMTextureRegion(handle, 1024, 512, image.data(), 320, 150, 16, 1);
    require(page->handle == handle, "palette-only update evicted indexed texels");
    // Opaque magenta/black and blended index zero differ. Direct STP black
    // is visible; index zero is keyed even if its palette entry is coloured.
    image[0] = 0x1234; image[1] = 0x7c1f; image[2] = 0x0000; image[3] = 0x8000;
    ctx.UpdateVRAMTexture(handle, 1024, 512, image.data());
    ctx.SetBlendMode(PDDI_BLEND_NONE); ctx.BindPage(2, 0);
    auto* palette = static_cast<const u16*>(FakeGe::lastClut);
    require((palette[0] & 0x8000) && !(palette[1] & 0x8000) && (palette[2] & 0x8000), "opaque CLUT key");
    ctx.SetBlendMode(PDDI_BLEND_ALPHA); ctx.BindPage(2, 0);
    palette = static_cast<const u16*>(FakeGe::lastClut);
    require(!(palette[0] & 0x8000) && (palette[1] & 0x8000) && (palette[2] & 0x8000), "blended CLUT key");
    auto* direct = ctx.GetDecodedPage(handle, 0x100);
    require((direct->pixels[2] & 0x8000) == 0 && (direct->pixels[3] & 0x8000), "direct zero word key");
    display.SwapBuffers();
    // Exercise production CLUT conversion with every 16-bit colour and all
    // key modes, including a T8 palette that wraps at the right row edge.
    const auto savedImage = image;
    for (unsigned int y = 0; y < 256; ++y) {
        for (unsigned int x = 0; x < 1024; ++x)
            image[y * 1024 + x] = static_cast<u16>(y * 256 + (x & 255));
    }
    ctx.UpdateVRAMTexture(handle, 1024, 512, image.data());
    ctx.BeginFrame();
    for (unsigned int mode = 0; mode < 3; ++mode) {
        ctx.SetBlendMode(mode == 1 ? PDDI_BLEND_ALPHA : PDDI_BLEND_NONE);
        for (unsigned int y = 0; y < 256; ++y) {
            const unsigned int cba = (y << 6) | 63;
            ctx.BindPage(0x82, static_cast<float>(cba), mode == 2);
            const auto* actual = static_cast<const u16*>(FakeGe::lastClut);
            for (unsigned int i = 0; i < 256; ++i) {
                const u16 word = image[y * 1024 + ((1008 + i) % 1024)];
                const bool keyed = mode == 2 ? word == 0
                    : (mode == 1 ? i == 0 : (word & 0x7fff) == 0x7c1f);
                require(actual[i] == ConvertPsxTexelToGu5551(word, keyed),
                    "CLUT conversion changed a colour or transparency key");
            }
        }
    }
    ctx.FinishList();
    image = savedImage;
    ctx.UpdateVRAMTexture(handle, 1024, 512, image.data());
    {
        // Converted-palette cache: an animated CLUT row reaches the GE inside
        // the same list (its old entry is still referenced) and in later
        // frames (BeginFrame fences the list and resets GE state tracking), and
        // an unchanged row is reloaded from the cache.
        const unsigned int cbaAnimated = (300u << 6) | 2u;  // row 300, x = 32
        const auto loaded = [] { return static_cast<const u16*>(FakeGe::lastClut); };
        const auto setEntry = [&](u16 word) {
            image[300 * 1024 + 33] = word;
            ctx.UpdateVRAMTextureRegion(handle, 1024, 512, image.data(), 32, 300, 16, 1);
        };
        setEntry(0x03e0);
        ctx.BeginFrame();
        ctx.BindPage(2, static_cast<float>(cbaAnimated));
        require(loaded()[1] == ConvertPsxTexelToGu5551(0x03e0, false), "cached palette first load");
        setEntry(0x7c00);
        // Another material in between (consecutive identical binds are a
        // no-op by design; animated palettes change between frames).
        ctx.BindPage(2, static_cast<float>((301u << 6) | 2u));
        ctx.BindPage(2, static_cast<float>(cbaAnimated));
        require(loaded()[1] == ConvertPsxTexelToGu5551(0x7c00, false), "palette update inside a list");
        ctx.BeginFrame();
        ctx.BindPage(2, static_cast<float>(cbaAnimated));
        require(loaded()[1] == ConvertPsxTexelToGu5551(0x7c00, false), "palette lost after a frame");
        ctx.BeginFrame();
        setEntry(0x0001);
        ctx.BindPage(2, static_cast<float>(cbaAnimated));
        require(loaded()[1] == ConvertPsxTexelToGu5551(0x0001, false), "palette update across frames");
        ctx.FinishList();
        image = savedImage;
        ctx.UpdateVRAMTexture(handle, 1024, 512, image.data());
    }
    // Two heavy frames reproduce the old fallback list overflow. Native
    // remapping must advance by 3 local vertices, not 20000 source vertices.
    pspPrimBuffer buffer({PDDI_PRIM_TRIANGLES, PDDI_V_POSITION, 20000, 3});
    buffer.vertices[0] = {0, 0, 0xffffffff, 0, 1, 2};
    buffer.vertices[1] = {0, 0, 0xffffffff, 1, 2, 3};
    buffer.vertices[2] = {0, 0, 0xffffffff, 2, 3, 4};
    u16 ids[] = {0, 1, 2}; buffer.SetIndices(ids, 3);
    ctx.SetProjectionMatrix(PerspectiveReversedZ(1.0f, 1.7f, 1.0f, 100.0f));

    // Exercise the PSP page-backed compact UV stream. The positions remain
    // float 3D coordinates; only UVs use the normalized 16-bit GU lane.
    pspPrimBuffer packedBuffer({PDDI_PRIM_TRIANGLES,
        PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO, 3, 3});
    const std::array<float, 30> packedData = {
        0, 1, 2, 1, 1, 1, 5, 6, 2, 0,
        1, 2, 3, 1, 1, 1, 15, 6, 2, 0,
        2, 3, 4, 1, 1, 1, 5, 16, 2, 0,
    };
    packedBuffer.SetVertexData(packedData.data(), 3);
    u16 packedIds[] = {0, 1, 2};
    packedBuffer.SetIndices(packedIds, 3);
    ctx.SetVRAMHandle(handle);
    ctx.BeginFrame();
    ctx.DrawPrimBuffer(&packedBuffer);
#if defined(PSP_ENABLE_NATIVE_INDEXED) && defined(PSP_INDEXED_LIST_MEMORY)
    require(FakeGe::lastStride == sizeof(PspPackedVertex), "packed page vertices not submitted");
#else
    require(FakeGe::lastStride == sizeof(PspVertex), "fallback vertex format changed");
#endif
    // This probe is about the source stride, not the cross-variant hash.
    FakeGe::drawSignatures.clear();
    display.SwapBuffers();
    ctx.SetVRAMHandle(0);

    // Shared skinned buffers are rewritten, drawn, then restored in the same
    // frame.  The staged opt-in must keep both mutations list-owned instead
    // of fencing the GE between the draw and the restore.
    pspPrimBuffer dynamicBuffer({PDDI_PRIM_TRIANGLES,
        PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO, 3, 3});
    const std::array<float, 30> dynamicA = {
        0, 0, 1, 1, 1, 1, 5, 6, 2, 0,
        1, 0, 1, 1, 1, 1, 15, 6, 2, 0,
        0, 1, 1, 1, 1, 1, 5, 16, 2, 0};
    const std::array<float, 30> dynamicB = {
        2, 0, 1, 1, 0.5f, 1, 5, 6, 2, 0,
        3, 0, 1, 1, 0.5f, 1, 15, 6, 2, 0,
        2, 1, 1, 1, 0.5f, 1, 5, 16, 2, 0};
    dynamicBuffer.SetVertexData(dynamicA.data(), 3);
    dynamicBuffer.SetIndices(ids, 3);
    PreparePspPrimBufferForDynamicSubmission(&dynamicBuffer);
    ctx.BeginFrame();
    const int syncsBeforeDynamicRestore = FakeGe::syncs;
    require(UpdatePspPrimBufferDynamicVertices(&dynamicBuffer, dynamicB.data(), 3, true),
        "dynamic vertex fast path rejected canonical format");
    ctx.DrawPrimBuffer(&dynamicBuffer);
    require(dynamicBuffer.vertices[0].x == 2.0f
        && dynamicBuffer.vertices[0].u == 5.0f
        && dynamicBuffer.GetMaterialKey(0) == (2u << 16),
        "dynamic vertex fast path changed stable material data");
    require(UpdatePspPrimBufferDynamicVertices(&dynamicBuffer, dynamicA.data(), 3, true),
        "dynamic vertex restore failed");
    require(FakeGe::syncs == syncsBeforeDynamicRestore,
        "dynamic buffer restore fenced the GU list");
    display.SwapBuffers();

    const auto primitivesBefore = FakeGe::primitives;
    for (int frame = 0; frame < 2; ++frame) {
        ctx.BeginFrame();
        for (int draw = 0; draw < 30000; ++draw) ctx.DrawPrimBuffer(&buffer);
        display.SwapBuffers();
    }
    require(FakeGe::primitives - primitivesBefore == 60000, "draws lost under pressure");
    const auto& m = FakeGe::projection;
    const float nearZ = (m[10] * -1.0f + m[14]) / (-m[11] + m[15]);
    const float farZ = (m[10] * -100.0f + m[14]) / (-100 * m[11] + m[15]);
    require(std::abs(nearZ - 1) < 0.0001f && std::abs(farZ + 1) < 0.0001f
        && FakeGe::depthNear < FakeGe::depthFar, "reversed Z conversion");
    // Count >65535 must split without changing triangles or strip topology.
    std::vector<u16> longIndices(70002);
    for (size_t i = 0; i < longIndices.size(); ++i) longIndices[i] = static_cast<u16>(i % 3);
    buffer.SetIndices(longIndices.data(), longIndices.size());
    for (auto primitive : {PDDI_PRIM_TRIANGLES, PDDI_PRIM_TRISTRIP, PDDI_PRIM_LINESTRIP}) {
        buffer.primType = primitive;
        const auto before = FakeGe::primitives;
        ctx.BeginFrame(); ctx.DrawPrimBuffer(&buffer); display.SwapBuffers();
        const auto expected = primitive == PDDI_PRIM_TRIANGLES ? 23334u
            : (primitive == PDDI_PRIM_TRISTRIP ? 70000u : 70001u);
        require(FakeGe::primitives - before == expected, "chunked primitive topology");
    }
    // Overlay allocations share the list: both allocations of a sprite batch
    // must survive until draw, including a boundary close to 1 MiB.
    auto texture = std::make_unique<pspTexture>();
    std::vector<u32> rgba(16 * 16, 0xffffffff);
    texture->SetData(16, 16, 32, 8, rgba.data());
    std::vector<pddiBatchVertex> overlay(9000);
    ctx.BeginFrame();
    ctx.SetBlendMode(PDDI_BLEND_NONE);
    // Native background reuses PSX pages even with coloured index zero.
    // Pressure must fence CLUTs without allocating standalone RGBA textures.
    for (int i = 0; i < 520; ++i) {
        ctx.DrawVRAMQuad(handle, 2, 0, 10, 20, 30, 40, 5, 6, 35, 46,
            pddiColour(255, 255, 255, 255));
        require(FakeGe::lastDrawTextured, "background lost its texture under pressure");
    }
    palette = static_cast<const u16*>(FakeGe::lastClut);
    require((palette[0] & 0x8000) && (palette[1] & 0x8000)
        && !(palette[2] & 0x8000) && (palette[3] & 0x8000), "background zero-word key");
    const auto* background = static_cast<const PspVertex*>(FakeGe::lastVertices);
    require(background[5].x == 10 && background[5].y == 20
        && background[5].u == 5 && background[5].v == 6
        && background[1].x == 40 && background[1].y == 60
        && background[1].u == 35 && background[1].v == 46, "background XY/UV changed");
    require(ctx.vramHandle == 0 && ctx.blendMode == PDDI_BLEND_NONE
        && FakeGe::depthMask == GU_FALSE, "background leaked overlay state into world draw");
    for (int i = 0; i < 80; ++i) ctx.DrawQuadBatch(texture.get(), PDDI_BLEND_ALPHA, overlay.data(), overlay.size());
    require(FakeGe::textureFunction == GU_TFX_MODULATE,
        "font atlas must respect glyph tint, black outlines and fade alpha");
    // A strip-ordered HUD rectangle must cover each interior sample once:
    // the old fan diagonal overlapped one half and left the other uncovered.
    ctx.DrawGouraudQuad(0, 0, 1, 1, 0, 1, 20, 0, 1, 1, 0, 1,
        0, 10, 1, 0, 0, 1, 20, 10, 1, 0, 0, 1);
    const auto* hudVertices = static_cast<const PspVertex*>(FakeGe::lastVertices);
    auto cross = [](const PspVertex& a, const PspVertex& b, float x, float y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    };
    for (const auto& sample : {std::pair<float, float>{3, 2}, {17, 3}, {3, 8}, {17, 8}}) {
        int coverage = 0;
        for (int t = 0; t < 6; t += 3) {
            const float a = cross(hudVertices[t], hudVertices[t + 1], sample.first, sample.second);
            const float b = cross(hudVertices[t + 1], hudVertices[t + 2], sample.first, sample.second);
            const float c = cross(hudVertices[t + 2], hudVertices[t], sample.first, sample.second);
            coverage += (a > 0 && b > 0 && c > 0) || (a < 0 && b < 0 && c < 0);
        }
        require(coverage == 1, "HUD rectangle has a hole or overlapping triangles");
    }
    texture->SetData(16, 16, 32, 8, rgba.data()); // must fence live standalone texture
    display.SwapBuffers();
    require(FakeGe::swaps == 10, "internal fence presented an extra frame");
    require(FakeGe::clears == 0, "internal fence cleared the framebuffer");
    uint64_t signature = 0;
    for (auto value : FakeGe::drawSignatures) signature = signature * 31 + value;
    // The cross-variant summary covers the scenes above; the focused checks
    // below must not perturb it.
    const int summaryDraws = FakeGe::draws, summaryCopies = FakeGe::copies;
    const int summaryCluts = FakeGe::cluts, summarySyncs = FakeGe::syncs;
    const size_t summaryPeak = FakeGe::peak;

    // Dynamic uploads: list-memory builds write only the compact stream and
    // rebuild the float one on demand; others keep only the float stream.
    // Unchanged material lanes skip key conversion.
    {
        pspPrimBuffer lazy({PDDI_PRIM_TRIANGLES,
            PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO, 3, 3});
        lazy.SetVertexData(dynamicA.data(), 3);
        lazy.SetIndices(ids, 3);
        require(lazy.HasPackedVertices() && !lazy.HasStaleFloatVertices(), "full upload state");
        require(UpdatePspPrimBufferDynamicVertices(&lazy, dynamicB.data(), 3, true),
            "dynamic upload rejected");
        const u32 litB = MakeGeometryColor(1.0f, 0.5f, 1.0f);
        if (kPackedStreamConsumed) {
            require(lazy.HasStaleFloatVertices() && lazy.vertices[0].x == 0.0f
                && lazy.packedVertices[0].x == 2.0f && lazy.packedVertices[1].u == 15
                && lazy.packedVertices[0].color == litB, "compact-only dynamic upload");
            lazy.RefreshFloatVertices();
        }
        else {
            require(!lazy.HasPackedVertices() && !lazy.HasStaleFloatVertices(),
                "unused compact stream must not be maintained");
        }
        require(!lazy.HasStaleFloatVertices() && lazy.vertices[0].x == 2.0f
            && lazy.vertices[1].u == 15.0f && lazy.vertices[2].v == 16.0f
            && lazy.vertices[0].color == litB, "float stream rebuilt exactly");
        lazy.materialKeys[0] = 0xdeadbeefu;
        std::array<float, 30> suit = dynamicB;
        suit[1 * 10 + 9] = 7.0f;
        require(UpdatePspPrimBufferDynamicVertices(&lazy, suit.data(), 3, true), "suit upload");
        require(lazy.GetMaterialKey(0) == 0xdeadbeefu, "unchanged material lane recomputed");
        require(lazy.GetMaterialKey(1) == ((2u << 16) | 7u), "changed material lane ignored");
        suit[2 * 10 + 8] = -1.0f;
        require(UpdatePspPrimBufferDynamicVertices(&lazy, suit.data(), 3, true)
            && !lazy.HasMaterialKey(2), "invalid material lane accepted");
        lazy.SetVertexData(dynamicA.data(), 3);
        require(lazy.GetMaterialKey(0) == (2u << 16) && !lazy.HasStaleFloatVertices(),
            "full upload must rebuild keys and float stream");
    }

    // Lit props patch only the colour lane, then restore the base colours.
    {
        pspPrimBuffer prop({PDDI_PRIM_TRIANGLES,
            PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO, 3, 3});
        prop.SetVertexData(dynamicA.data(), 3);
        prop.SetIndices(ids, 3);
        const u32 base = MakePspGeometryColour(1.0f, 1.0f, 1.0f);
        const std::array<u32, 3> lit = {0xff102030u, 0xff405060u, 0xff708090u};
        require(RestorePspPrimBufferColours(&prop, 3), "restore without patch is a no-op");
        require(!PatchPspPrimBufferColours(&prop, lit.data(), 2), "count mismatch accepted");
        ctx.BeginFrame();
        const int syncsBeforePatch = FakeGe::syncs;
        require(PatchPspPrimBufferColours(&prop, lit.data(), 3), "colour patch rejected");
        require(prop.UsesStagedIndexedVertices(), "patched prop must stay list-staged");
        require(prop.packedVertices[1].color == lit[1], "compact colour not patched");
        ctx.SetVRAMHandle(0);
        ctx.DrawPrimBuffer(&prop); // float path while lit
        require(prop.vertices[2].color == lit[2] && prop.vertices[2].u == 5.0f,
            "float stream must show the lit colour with unchanged UVs");
        require(RestorePspPrimBufferColours(&prop, 3), "colour restore failed");
        require(FakeGe::syncs == syncsBeforePatch, "colour patch/restore fenced the GU list");
        prop.RefreshFloatVertices();
        for (int i = 0; i < 3; ++i) {
            require(prop.packedVertices[i].color == base && prop.vertices[i].color == base,
                "restore left a lit colour behind");
        }
        // Patch then restore without an intermediate float draw.
        require(PatchPspPrimBufferColours(&prop, lit.data(), 3)
            && RestorePspPrimBufferColours(&prop, 3), "second patch cycle");
        require(!prop.HasStaleFloatVertices() && prop.vertices[0].color == base
            && prop.packedVertices[0].color == base, "untouched float copy must stay current");
        // An upload while lit replaces the patch: restore must not undo it.
        require(PatchPspPrimBufferColours(&prop, lit.data(), 3), "third patch");
        prop.SetVertexData(dynamicB.data(), 3);
        require(RestorePspPrimBufferColours(&prop, 3)
            && prop.vertices[0].color == MakeGeometryColor(1.0f, 0.5f, 1.0f),
            "restore overwrote a newer upload");
        display.SwapBuffers();
    }

    // Setup2DState skips re-emitting its state between overlay batches, but
    // not after a 3D setter or matrix load changed it.
    {
        ctx.BeginFrame();
        ctx.Setup2DState();
        const size_t afterFirst2D = FakeGe::used;
        ctx.Setup2DState();
        require(FakeGe::used == afterFirst2D, "repeated 2D setup emitted GU state");
        ctx.SetCullMode(PDDI_CULL_NORMAL);
        const size_t afterCull = FakeGe::used;
        ctx.Setup2DState();
        require(FakeGe::used > afterCull
            && (g_guCapabilityEnabled & k2DDisabledCapabilities) == 0u,
            "2D setup must undo a 3D setter");
        ctx.EnableZBuffer(true);
        ctx.SetBlendMode(PDDI_BLEND_NONE);
        ctx.Setup3DState();
        require(FakeGe::depthMask == GU_FALSE, "3D depth writes restored");
        ctx.Setup2DState();
        require(FakeGe::depthMask == GU_TRUE, "2D must mask depth writes again");
        ctx.SetCullMode(PDDI_CULL_NONE);
        ctx.EnableZBuffer(false);
        ctx.SetBlendMode(PDDI_BLEND_ALPHA);
        ctx.DrawPrimBuffer(&buffer, 0, 3); // uploads the 3D matrices
        require(ctx.gu2DMatrixSerial != g_gumMatrixSerial, "3D draw must load its matrices");
        const u32 generationBefore2D = g_matrixGeneration;
        ctx.Setup2DState();
        require(ctx.gu2DMatrixSerial == g_gumMatrixSerial
            && g_matrixGeneration != generationBefore2D,
            "2D setup must rebuild GUM state after a 3D matrix load");
        ctx.EnableZBuffer(true);
        ctx.SetBlendMode(PDDI_BLEND_NONE);
        display.SwapBuffers();
    }

    // Pause backdrop: captured once into the depth surface, redrawn alone on
    // later frames; any 3D draw or a frame that skips it releases it.
    {
        const auto edram = reinterpret_cast<uintptr_t>(sceGeEdramGetAddr());
        const auto depth = reinterpret_cast<uintptr_t>(g_staticVram.depthBuffer);
        ctx.BeginFrame();
        const void* drawTarget = g_drawingSecondBuffer
            ? g_staticVram.displayBuffer : g_staticVram.frameBuffer;
        ctx.Clear(PDDI_BUFFER_ALL);
        require(FakeGe::clearFlags == (GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT), "normal clear");
        require(!PspDrawPauseBackdrop(), "backdrop drawn before capture");
        require(PspCapturePauseBackdrop() && PspHasPauseBackdrop(), "capture failed");
        require(FakeGe::texture == reinterpret_cast<const void*>(edram
            + reinterpret_cast<uintptr_t>(drawTarget)) && FakeGe::textureFormat == GU_PSM_8888,
            "capture must read the frame being drawn");
        require(FakeGe::drawBufferFormat == GU_PSM_8888 && FakeGe::drawBuffer == drawTarget
            && FakeGe::depthMask == GU_TRUE, "capture must restore the colour target");
        display.SwapBuffers();
        for (int frame = 0; frame < 3; ++frame) {
            ctx.BeginFrame();
            require(PspHasPauseBackdrop(), "backdrop lost between pause frames");
            const int clearsBefore = FakeGe::clears;
            ctx.Clear(PDDI_BUFFER_ALL);
            require(FakeGe::clears == clearsBefore + 1
                && FakeGe::clearFlags == GU_COLOR_BUFFER_BIT, "pause frame cleared the backdrop");
            require(PspDrawPauseBackdrop(), "backdrop draw failed");
            require(FakeGe::texture == reinterpret_cast<const void*>(edram + depth)
                && FakeGe::textureFormat == GU_PSM_5650 && FakeGe::lastStride == 10,
                "backdrop must sample the 5650 depth-surface copy");
            display.SwapBuffers();
        }
        // Resume: the first 3D draw gets a cleared depth buffer back.
        ctx.BeginFrame();
        ctx.Clear(PDDI_BUFFER_ALL);
        ctx.DrawPrimBuffer(&buffer, 0, 3);
        require(!PspHasPauseBackdrop() && FakeGe::clearFlags == GU_DEPTH_BUFFER_BIT,
            "3D draw must release the backdrop and clear depth");
        display.SwapBuffers();
        // A frame that does not redraw it releases it at the next BeginFrame.
        ctx.BeginFrame();
        require(PspCapturePauseBackdrop(), "recapture failed");
        display.SwapBuffers();
        ctx.BeginFrame();
        display.SwapBuffers();
        const int clearsBefore = FakeGe::clears;
        ctx.BeginFrame();
        require(!PspHasPauseBackdrop() && FakeGe::clears == clearsBefore + 1
            && FakeGe::clearFlags == GU_DEPTH_BUFFER_BIT, "stale backdrop not released");
        ctx.Clear(PDDI_BUFFER_ALL);
        require(FakeGe::clearFlags == (GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT),
            "depth clear not restored after pause");
        PspReleasePauseBackdrop(); // idempotent
        display.SwapBuffers();
    }

    {
        // Cached material runs must give exactly the per-triangle scan's
        // boundaries (valid, invalid and mixed keys, partial tails), also
        // after a dynamic upload rewrites material lanes.
        std::mt19937 rng(20261010u);
        const auto checkRuns = [&](pspPrimBuffer& runs, const std::vector<u16>& idx) {
            for (u32 start = 0; start + 2 < idx.size(); start += 3) {
                for (const u32 chunk : {3u, 9u, 30u, 8190u}) {
                    u32 runEnd = start + std::min<u32>(chunk, static_cast<u32>(idx.size()) - start);
                    runEnd -= (runEnd - start) % 3u;
                    const bool firstValid = runs.HasMaterialKey(idx[start]);
                    const u32 firstKey = runs.GetMaterialKey(idx[start]);
                    u32 scanEnd = runEnd;
                    for (u32 cursor = start + 3; cursor + 2 < runEnd; cursor += 3) {
                        const u16 tri = idx[cursor];
                        const bool differs = firstValid
                            ? (!runs.HasMaterialKey(tri) || runs.GetMaterialKey(tri) != firstKey)
                            : runs.HasMaterialKey(tri);
                        if (differs) { scanEnd = cursor; break; }
                    }
                    bool valid = false;
                    u32 key = 0;
                    u32 limit = 0;
                    require(runs.FindMaterialRun(start, &valid, &key, &limit), "material run missing");
                    require(valid == firstValid && (!valid || key == firstKey)
                        && std::min(runEnd, limit) == scanEnd, "material run differs from the scan");
                }
            }
        };
        for (int round = 0; round < 300; ++round) {
            const u32 vertexCount = 1u + rng() % 48u;
            const u32 indexCount = 3u * (1u + rng() % 60u) + rng() % 3u;
            pspPrimBuffer runs({PDDI_PRIM_TRIANGLES,
                PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO, vertexCount, indexCount});
            std::vector<float> data(vertexCount * 10u, 1.0f);
            const auto material = [&](float* lane) {
                const u32 kind = rng() % 4u;
                lane[0] = kind == 0u ? -1.0f : static_cast<float>(kind);
                lane[1] = kind == 0u ? 0.0f : static_cast<float>(rng() % 2u);
            };
            for (u32 v = 0; v < vertexCount; ++v) material(&data[v * 10u + 8u]);
            runs.SetVertexData(data.data(), vertexCount);
            std::vector<u16> idx(indexCount);
            for (u16& index : idx) index = static_cast<u16>(rng() % vertexCount);
            runs.SetIndices(idx.data(), indexCount);
            checkRuns(runs, idx);
            for (u32 v = 0; v < vertexCount; ++v) {
                if (rng() % 3u == 0u) material(&data[v * 10u + 8u]);
            }
            require(runs.SetDynamicVertexData(data.data(), vertexCount, true), "dynamic upload refused");
            checkRuns(runs, idx);
        }
    }

    std::printf("PASS: draws=%d copies=%d cluts=%d syncs=%d peak_list=%zu signature=%016llx\n",
        summaryDraws, summaryCopies, summaryCluts, summarySyncs, summaryPeak,
        static_cast<unsigned long long>(signature));
}

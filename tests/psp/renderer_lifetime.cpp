// Exercise the production renderer against delayed GE reads, not a duplicate
// implementation of the cache. Run with scripts/test_psp_renderer.sh.
#include <array>
#include <string>
#include <utility>
#include <vector>
#include <memory>
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
    // Different CBA metadata can still reference identical converted colors.
    // The second bind must keep the resident GE CLUT instead of submitting a
    // redundant palette load.
    ctx.SetBlendMode(PDDI_BLEND_NONE);
    ctx.BindPage(2, 0);
    const int clutsBeforeEquivalent = FakeGe::cluts;
    ctx.BindPage(2, 64);
    require(FakeGe::cluts == clutsBeforeEquivalent,
        "equivalent palette was redundantly loaded");
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
    const auto* palette = ctx.lastClutPalette.data();
    require(ctx.lastClutPaletteEntries == 16, "opaque CLUT snapshot size");
    require((palette[0] & 0x8000) && !(palette[1] & 0x8000) && (palette[2] & 0x8000), "opaque CLUT key");
    ctx.SetBlendMode(PDDI_BLEND_ALPHA); ctx.BindPage(2, 0);
    palette = ctx.lastClutPalette.data();
    require(!(palette[0] & 0x8000) && (palette[1] & 0x8000) && (palette[2] & 0x8000), "blended CLUT key");
    auto* direct = ctx.GetDecodedPage(handle, 0x100);
    require((direct->pixels[2] & 0x8000) == 0 && (direct->pixels[3] & 0x8000), "direct zero word key");
    display.SwapBuffers();
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
    palette = ctx.lastClutPalette.data();
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
    std::printf("PASS: draws=%d copies=%d cluts=%d syncs=%d peak_list=%zu signature=%016llx\n",
        FakeGe::draws, FakeGe::copies, FakeGe::cluts, FakeGe::syncs, FakeGe::peak,
        static_cast<unsigned long long>(signature));
}

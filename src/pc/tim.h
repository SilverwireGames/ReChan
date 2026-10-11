#pragma once
#include "core.h"
#include "pddi/pddi.h"

class tTexture;
class pddiBaseShader;

// Loaded TIM image data
struct TimImage {
    s32 width = 0;      // pixel width
    s32 height = 0;     // pixel height
    u32* rgba = nullptr; // RGBA32 pixel data (width * height)

    ~TimImage() { delete[] rgba; }
};

// PSX TIM file loader
namespace Tim {
    // Load a PSX TIM file from disk, decode to RGBA32.
    // Returns nullptr on failure. Caller owns the result.
    TimImage* LoadFromFile(const char* path);

    // Decode a TIM from a memory buffer.
    // Returns nullptr on failure. Caller owns the result.
    TimImage* LoadFromMemory(const u8* data, u32 size);

    // Create a tTexture from a TimImage (uploads to GPU).
    // Caller owns the returned texture.
    tTexture* CreateTexture(const TimImage* img);
}

// 2D overlay drawing
// Creates/caches an internal shader on first use.
namespace ScreenDraw {
    void BeginBatch(f32 canvasW = 0.0f, f32 canvasH = 0.0f);
    void EndBatch();

#if defined(RC_PLATFORM_PSP)
    // Registers an opaque white texel of `tex` at (u, v); null clears it.
    // Batched colour fills are then drawn as `tex` sampled there, which is
    // the same colour, so fills and the text drawn from `tex` share one
    // batch instead of flushing at every switch between them.
    void SetSolidTexel(tTexture* tex, f32 u, f32 v);
#endif

    // RAII helper: opens a batch for the lifetime of the scope.
    // Safe for functions with multiple return paths.
    struct Batch {
        explicit Batch(f32 canvasW = 0.0f, f32 canvasH = 0.0f) {
            BeginBatch(canvasW, canvasH);
        }

        ~Batch() {
            EndBatch();
        }

        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
    };

    // Draw a texture filling the entire screen.
    void DrawFullscreen(tTexture* tex);

    void DrawFullscreenAdvanced(tTexture* tex,
                                            f32 sourceAspect,
                                            f32 desiredAspect,
                                            bool fillScreen);

    // Draw a textured quad with optional UV and color tint.
    void DrawQuad(tTexture* tex,
                  f32 x, f32 y, f32 w, f32 h,
                  f32 u0 = 0.0f, f32 v0 = 0.0f,
                  f32 u1 = 1.0f, f32 v1 = 1.0f,
                  u8 r = 255, u8 g = 255, u8 b = 255, u8 a = 255);

    // Draw through a caller-owned programmable shader while preserving the
    // same overlay state and coordinate system as the regular 2D helpers.
    void DrawShaderQuad(pddiBaseShader* shader,
                        f32 x, f32 y, f32 w, f32 h,
                        f32 u0 = 0.0f, f32 v0 = 0.0f,
                        f32 u1 = 1.0f, f32 v1 = 1.0f,
                        pddiBlendMode blendMode = PDDI_BLEND_ALPHA,
                        f32 canvasW = 0.0f, f32 canvasH = 0.0f);

    // Draw a solid colored rectangle.
    void DrawColoredRect(f32 x, f32 y, f32 w, f32 h,
                         u8 r, u8 g, u8 b, u8 a);

    // Draw a fullscreen colored quad, used for fade transitions.
    void DrawColoredQuad(u8 r, u8 g, u8 b, u8 a);

    // Draw a textured circle outline/ring.
    void DrawCircle(tTexture* tex,
                    f32 centerX, f32 centerY,
                    f32 radiusX, f32 radiusY,
                    f32 thickness,
                    f32 u0 = 0.0f, f32 v0 = 0.0f,
                    f32 u1 = 1.0f, f32 v1 = 1.0f,
                    s32 segments = 32,
                    u8 r = 255, u8 g = 255, u8 b = 255, u8 a = 255);

    // Draw a colored circle outline/ring.
    void DrawCircle(f32 centerX, f32 centerY,
                    f32 radiusX, f32 radiusY,
                    f32 thickness,
                    f32 u0 = 0.0f, f32 v0 = 0.0f,
                    f32 u1 = 1.0f, f32 v1 = 1.0f,
                    s32 segments = 32,
                    u8 r = 255, u8 g = 255, u8 b = 255, u8 a = 255);

    // Draw a textured filled circle.
    void DrawFilledCircle(tTexture* tex,
                          f32 centerX, f32 centerY,
                          f32 radiusX, f32 radiusY,
                          f32 u0 = 0.0f, f32 v0 = 0.0f,
                          f32 u1 = 1.0f, f32 v1 = 1.0f,
                          s32 segments = 32,
                          u8 r = 255, u8 g = 255, u8 b = 255, u8 a = 255);

    // Draw a colored filled circle.
    void DrawFilledCircle(f32 centerX, f32 centerY,
                          f32 radiusX, f32 radiusY,
                          f32 u0 = 0.0f, f32 v0 = 0.0f,
                          f32 u1 = 1.0f, f32 v1 = 1.0f,
                          s32 segments = 32,
                          u8 r = 255, u8 g = 255, u8 b = 255, u8 a = 255);

    void SetScissor(s32 x, s32 y, s32 w, s32 h);

    // Draw a Gouraud-shaded quad with 4 per-vertex colors.
    void DrawGouraudQuad(f32 x0, f32 y0, u8 r0, u8 g0, u8 b0, u8 a0,
                         f32 x1, f32 y1, u8 r1, u8 g1, u8 b1, u8 a1,
                         f32 x2, f32 y2, u8 r2, u8 g2, u8 b2, u8 a2,
                         f32 x3, f32 y3, u8 r3, u8 g3, u8 b3, u8 a3);

    // Cleanup cached resources.
    void Shutdown();
}
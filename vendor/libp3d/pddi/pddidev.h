// pddidev.h - pddiDevice, pddiDisplay, pddiRenderContext interfaces
#pragma once

#include "pddi/pddi.h"
#include <functional>
#include <vector>

class pddiTexture;
class pddiBaseShader;
class pddiPrimBuffer;
class pddiRenderTarget;
struct pddiPrimBufferDesc;

enum pddiRenderTargetFormat {
    PDDI_RENDER_TARGET_RGBA8,
    PDDI_RENDER_TARGET_RGBA16F,
    // Depth-only target (no colour attachment), used for shadow-map passes.
    PDDI_RENDER_TARGET_DEPTH24,
    PDDI_RENDER_TARGET_DEPTH32F,
    PDDI_RENDER_TARGET_DEPTH = PDDI_RENDER_TARGET_DEPTH32F,
};

class pddiRenderTarget : public pddiObject {
public:
    virtual bool Resize(int width, int height) = 0;
    virtual int GetWidth() const = 0;
    virtual int GetHeight() const = 0;
    virtual pddiTexture* GetTexture() const = 0;
    virtual bool IsValid() const = 0;
    // Companion instance-ID texture (R32UI), only non-null when the target
    // was created with withInstanceId=true (see CreateRenderTarget).
    virtual pddiTexture* GetIdTexture() const { return nullptr; }
};

struct pddiBatchVertex {
    float x = 0.0f, y = 0.0f;
    float u = 0.0f, v = 0.0f;
    u8 r = 255, g = 255, b = 255, a = 255;
};

// Video mode description

struct pddiVideoMode {
    int width = 0;
    int height = 0;
    int refreshRate = 0;
};

// Display init params

struct pddiDisplayInit {
    int xSize = 960;
    int ySize = 720;
    const char* title = "Pure3D";
    bool fullscreen = false;
    bool vsync = true;
    int msaa = 0; // 0 = disabled, 2/4/8/16 = sample count
};

// WndProc-style event types the app can hook into

enum pddiWndEvent {
    PDDI_WND_RESIZE,      // param1=width, param2=height
    PDDI_WND_FOCUS,       // param1=focused (1 or 0)
    PDDI_WND_CLOSE,       // window close requested
    PDDI_WND_KEYDOWN,     // param1=key, param2=scancode
    PDDI_WND_KEYUP,       // param1=key, param2=scancode
    PDDI_WND_MOUSEBUTTON, // param1=button, param2=pressed (1 or 0)
    PDDI_WND_MOUSEMOVE,   // fparam1=x, fparam2=y
    PDDI_WND_SCROLL,      // fparam1=xoffset, fparam2=yoffset
};

struct pddiWndMessage {
    pddiWndEvent event;
    int param1 = 0;
    int param2 = 0;
    double fparam1 = 0.0;
    double fparam2 = 0.0;
};

// Callback signature: return true to consume the event (stop further processing)
using pddiWndProc = std::function<bool(const pddiWndMessage& msg)>;

// pddiDisplay - window/framebuffer management

namespace pddiInput {
    // Letter keys use ASCII: 'A' = 65, 'W' = 87, etc.
    constexpr int KeyLeftShift = 340;
    constexpr int KeyPageUp    = 266;
    constexpr int KeyPageDown  = 267;
    constexpr int KeyF1        = 290;
    constexpr int KeyF3        = 292;

    constexpr int MouseLeft   = 0;
    constexpr int MouseRight  = 1;
    constexpr int MouseMiddle = 2;
}

// Gamepad button indices (backend-agnostic)
namespace GamepadButton {
    static constexpr int A             = 0;  // Cross
    static constexpr int B             = 1;  // Circle
    static constexpr int X             = 2;  // Square
    static constexpr int Y             = 3;  // Triangle
    static constexpr int LeftBumper    = 4;  // L1
    static constexpr int RightBumper   = 5;  // R1
    static constexpr int Back          = 6;  // Select
    static constexpr int Start         = 7;
    static constexpr int Guide         = 8;
    static constexpr int LeftThumb     = 9;  // L3
    static constexpr int RightThumb    = 10; // R3
    static constexpr int DpadUp        = 11;
    static constexpr int DpadRight     = 12;
    static constexpr int DpadDown      = 13;
    static constexpr int DpadLeft      = 14;
    static constexpr int COUNT         = 15;
}

// Gamepad axis indices (backend-agnostic)
namespace GamepadAxis {
    static constexpr int LeftX         = 0;
    static constexpr int LeftY         = 1;
    static constexpr int RightX        = 2;
    static constexpr int RightY        = 3;
    static constexpr int LeftTrigger   = 4;  // L2
    static constexpr int RightTrigger  = 5;  // R2
    static constexpr int COUNT         = 6;
}

class pddiDisplay : public pddiObject {
public:
    virtual bool InitDisplay(const pddiDisplayInit& init) = 0;
    virtual void SwapBuffers() = 0;
    virtual int  GetWidth() = 0;
    virtual int  GetHeight() = 0;
    virtual bool ShouldClose() = 0;
    virtual void PollEvents() = 0;

    // Input polling
    virtual bool IsKeyDown(int key) = 0;
    virtual bool IsMouseButtonDown(int button) = 0;
    virtual void GetMousePosition(double& x, double& y) = 0;

    // Set window icon from RGBA pixel data (32-bit, row-major, top-to-bottom)
    virtual void SetIcon(int w, int h, const unsigned char* rgba) = 0;

    // Video mode
    virtual int  GetVideoModeCount() = 0;
    virtual void GetVideoMode(int index, pddiVideoMode& mode) = 0;
    virtual void SetFullscreen(bool fullscreen) = 0;
    virtual bool IsFullscreen() = 0;
    virtual void SetBorderless(bool borderless) = 0;
    virtual bool IsBorderless() = 0;
    virtual void SetResolution(int w, int h) = 0;
    virtual void SetVSync(bool enabled) = 0;
    virtual void SetMSAA(int samples) = 0;
    virtual int  GetMSAA() = 0;
    virtual void SetWindowPos(int x, int y) = 0;

    // Window title
    virtual void SetTitle(const char* title) = 0;

    // Cursor
    virtual void ShowCursor(bool visible) = 0;
    virtual void ClipCursor(bool clip) = 0;

    // WndProc callback
    virtual void SetWndProc(pddiWndProc proc) = 0;

    // Overlay draw callback (called by RenderOverlay)
    using OverlayCallback = void(*)();
    virtual void AddOverlayCallback(OverlayCallback cb) = 0;
    virtual void RenderOverlay() = 0;

    // RGB rows, top first; valid between the last draw and SwapBuffers.
    virtual bool ReadBackBuffer(std::vector<unsigned char>& rgb, int& width, int& height) { return false; }
};

// pddiRenderContext — frame management and draw state

class pddiRenderContext : public pddiObject {
public:
    // Frame
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;

    // Camera projection override.
    virtual void SetCameraAspect(float aspect) = 0;
    virtual float GetCameraAspect() const = 0;

    // Clear
    virtual void SetClearColour(pddiColour c) = 0;
    virtual void Clear(int flags) = 0;

    // Transforms
    virtual void SetProjectionMatrix(const Mat4& m) = 0;
    virtual void SetViewMatrix(const Mat4& m) = 0;
    virtual void SetWorldMatrix(const Mat4& m) = 0;
    virtual const Mat4& GetWorldMatrix() const = 0;
    virtual const Mat4& GetViewMatrix() const = 0;
    virtual const Mat4& GetProjectionMatrix() const = 0;

    virtual void SetWorldMirror(bool enable) = 0;
    virtual bool GetWorldMirror() const = 0;

    // State
    virtual void SetCullMode(pddiCullMode mode) = 0;
    virtual void EnableZBuffer(bool enable) = 0;
    virtual void SetBlendMode(pddiBlendMode mode) = 0;
    // Enables/disables depth clamp (clip-space Z clamp instead of near/far clip).
    virtual void SetDepthClamp(bool enable) = 0;
    // Override polygon offset for all subsequent draws (including BLEND_NONE).
    // Disable by calling with enable=false. While enabled, SetBlendMode(BLEND_NONE)
    // restores these values instead of zeroing polygon offset.
    virtual void SetPolygonOffset(bool enable, f32 factor = 0.0f, f32 units = 0.0f) = 0;
    virtual void SetScissor(int x, int y, int w, int h) = 0;
    // Controls whether immediate-mode 2D draws should use multisample rasterization.
    virtual void SetMultisampleEnabled(bool enable) = 0;
    // Resolves the MSAA scene and switches subsequent draws in this frame to the single-sample backbuffer.
    virtual void ResolveForOverlayPass() = 0;

    // Texture-backed off-screen rendering. Passing nullptr restores the
    // framebuffer and viewport that were active before the first target bind.
    // withInstanceId requests a companion R32UI colour attachment alongside
    // a depth-format target (see pddiRenderTarget::GetIdTexture), used for
    // shadow-caster instance tagging; ignored for non-depth formats.
    virtual pddiRenderTarget* CreateRenderTarget(int width, int height,
                                                  pddiRenderTargetFormat format,
                                                  bool withInstanceId = false) = 0;
    virtual bool SetRenderTarget(pddiRenderTarget* target) = 0;

    virtual void DrawFilledCircle(pddiBaseShader* shader,
                          float centerX, float centerY,
                          float radiusX, float radiusY,
                          float u0, float v0, float u1, float v1,
                          int segments) = 0;

    virtual void DrawCircle(pddiBaseShader* shader,
                    float centerX, float centerY,
                    float radiusX, float radiusY,
                    float thickness,
                    float u0, float v0, float u1, float v1,
                    int segments) = 0;

    // Immediate-mode textured quad (for UI / debug rendering)
    virtual void DrawQuad(pddiBaseShader* shader,
                          float x, float y, float w, float h,
                          float u0 = 0, float v0 = 0,
                          float u1 = 1, float v1 = 1) = 0;

    virtual void DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                               const pddiBatchVertex* verts, s32 vertCount) = 0;

    // Draw a retained-mode primitive buffer. indexCount=0 draws the full buffer
    // (buffer->GetIndexCount()); otherwise draws indexCount indices starting at indexOffset.
    virtual void DrawPrimBuffer(pddiPrimBuffer* buffer, u32 indexOffset = 0, u32 indexCount = 0) = 0;

    // Gouraud-shaded quad with 4 per-vertex colors (PSX POLYG4)
    virtual void DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                                 float x1, float y1, float r1, float g1, float b1, float a1,
                                 float x2, float y2, float r2, float g2, float b2, float a2,
                                 float x3, float y3, float r3, float g3, float b3, float a3) = 0;

    // Set texture for 3D primitive rendering (nullptr to disable)
    virtual void SetTexture(pddiTexture* tex) = 0;

    // Set raw VRAM texture handle for PSX VRAM-in-shader lookup (0 to disable)
    virtual void SetVRAMHandle(u32 handle) = 0;

    // Override per-vertex PSX texinfo (tpage/cba) for legacy swap-word effects.
    virtual void SetTexInfoOverride(bool enabled, u32 texInfoWord) = 0;

    // Create/destroy a raw R16UI texture for PSX VRAM upload
    virtual u32  CreateVRAMTexture(int w, int h, const u16* data) = 0;
    virtual void DestroyVRAMTexture(u32 handle) = 0;
    // Re-uploads pixel data into an existing VRAM texture (same w/h as it
    // was created with) without reallocating GPU storage. Used for frequent,
    // same-size refreshes (e.g. Director::updateVramAnims' per-frame
    // flipbook updates) where a destroy+recreate cycle every frame is both
    // wasteful and, on tighter GPU memory budgets, can exhaust the
    // allocator; a no-op safety net if handle is 0.
    virtual void UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) = 0;

    // Re-uploads a sub-rectangle only. data points at the rect's first texel
    // inside a larger buffer whose rows are srcPitchTexels apart, so a small
    // dirty region can be pushed without touching the rest of the texture.
    // Palette cycling dirties 256x1 of a 1024x512 texture, so the full-surface
    // path moves 1 MiB to deliver 512 bytes.
    virtual void UpdateVRAMTextureRect(u32 handle, int x, int y, int w, int h,
                                       int srcPitchTexels, const u16* data) = 0;

    // Real-texture rendering: an alternate 3D texturing path that samples a
    // real 2D texture (bound via SetTexture) instead of the PSX VRAM atlas.
    // Off by default; the texture set via SetTexture is only used for 3D
    // draws while this mode is enabled. offsetX/Y, sizeX/Y are in the same
    // raw page-pixel UV space the legacy VRAM path uses (0-256 per page),
    // describing the sub-rect of that space the bound texture corresponds to.
    virtual void SetRealTextureMode(bool enabled) = 0;
    virtual bool IsRealTextureModeEnabled() const = 0;
    virtual void SetRealTextureRect(float offsetX, float offsetY, float sizeX, float sizeY) = 0;

    // Shadow mapping (MODERN_GRAPHICS). When a shadow-caster pass is active,
    // DrawPrimBuffer renders depth-only using lightVP*worldMatrix instead of
    // the normal view/projection. SetReceiveShadows marks subsequent draws
    // (world block geometry) as shadow receivers in the main pass; casters
    // (characters/obstacles) leave it disabled. SetShadowCascades supplies
    // the cascade depth textures/matrices/splits the main pass samples.
    virtual void SetShadowCasterPass(bool enable, const Mat4& lightVP) = 0;
    virtual void SetReceiveShadows(bool enable) = 0;
    // idTextures (one per cascade, may be nullptr/contain nullptrs if the
    // renderer doesn't support instance-tagged self-shadow exclusion) are
    // R32UI textures written by the caster prepass via
    // SetShadowCasterInstanceId; the main pass samples them to implement
    // SetShadowReceiverInstanceId self-exclusion (see below).
    virtual void SetShadowCascades(pddiTexture* const* depthTextures, const Mat4* lightVP,
                                   const float* splits, const float* texelWorldSizes,
                                   pddiTexture* const* idTextures, int count) = 0;
    // World-space camera position, used by the main pass to measure distance
    // for cascade selection (avoids relying on reversed-Z gl_FragCoord.z).
    virtual void SetCameraWorldPos(float x, float y, float z) = 0;
    virtual void SetShadowLightDirection(float x, float y, float z) = 0;
    // Instance-ID shadow self-exclusion (MODERN_GRAPHICS). During the caster
    // prepass, SetShadowCasterInstanceId(id) tags every subsequent depth-pass
    // draw with `id` in a companion per-cascade ID render target (0 reserved
    // for "no instance", e.g. static world geometry). During the main pass,
    // SetShadowReceiverInstanceId(id) makes that instance ignore shadow
    // samples whose caster ID equals its own `id`, so a humanoid/platform's
    // own nearby limbs or segments don't self-shadow it, while every other
    // caster still shadows it normally regardless of distance. Pass 0 to
    // disable self-exclusion for a draw (e.g. static receivers).
    virtual void SetShadowCasterInstanceId(u32 id) = 0;
    virtual void SetShadowReceiverInstanceId(u32 id) = 0;
    // Clears the currently-bound render target's instance-ID attachment to 0
    virtual void ClearShadowCasterIdTarget() = 0;
    // Debug visualization for the DebugUI Shadows panel: 0=off,
    // 1=tint receivers by selected cascade index, 2=force-darken receivers.
    virtual void SetShadowDebugMode(int mode) = 0;
};

// pddiGamepad - abstract gamepad/controller interface

class pddiGamepad : public pddiObject {
public:
    virtual void Poll() = 0;
    virtual bool IsConnected() const = 0;
    virtual bool IsButtonDown(int button) const = 0;
    virtual float GetAxis(int axis) const = 0;
    virtual bool SupportsVibration() const = 0;
    virtual bool SetVibration(float lowFrequency, float highFrequency) = 0;
};

// pddiDevice - factory for all pddi objects

class pddiDevice : public pddiObject {
public:
    virtual pddiDisplay* NewDisplay() = 0;
    virtual pddiRenderContext* NewRenderContext(pddiDisplay* display) = 0;
    virtual pddiGamepad* NewGamepad() = 0;
    virtual pddiTexture* NewTexture() = 0;
    virtual pddiPrimBuffer* NewPrimBuffer(const pddiPrimBufferDesc& desc) = 0;
    virtual pddiBaseShader* NewShader(const char* type = "simple") = 0;
};

// Create the platform-appropriate device (implemented by GL backend)
pddiDevice* pddiCreate();

// metalrender.h — Metal implementation of the pddi interfaces (macOS)
//
// Selected by RC_PLATFORM_MACOS. The whole backend is implemented in
// metalrender.mm (Objective-C++); this header stays valid C++ by keeping native
// Metal/Objective-C objects behind opaque void* handles.
//
// Milestone 2: vertex/index buffers, RGBA8 + R16UI (PSX VRAM) textures, a
// depth buffer, runtime-compiled MSL 3D shaders and DrawPrimBuffer.
#pragma once

#include "pddi/pddi.h"
#include "pddi/pdditex.h"
#include "pddi/pddishad.h"
#include "pddi/pddidev.h"
#include <string>
#include <array>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

// mtPrimBuffer — retained vertex/index buffer.

class mtPrimBuffer : public pddiPrimBuffer {
public:
    explicit mtPrimBuffer(const pddiPrimBufferDesc& desc);
    ~mtPrimBuffer() override;

    void SetVertexData(const void* data, u32 count) override;
    void SetIndices(const u16* indices, u32 count) override;
    u32 GetIndexCount() const override { return indexCount; }
    u32 GetVertexCount() const override { return vertexCount; }
    pddiPrimType GetPrimType() const override { return primType; }

    u32 GetVertexFormat() const { return vertexFormat; }
    u32 GetStride() const { return stride; }
    u32 GetPosOffset() const { return posOffset; }
    u32 GetColOffset() const { return colOffset; }
    u32 GetUVOffset() const { return uvOffset; }
    u32 GetTexInfoOffset() const { return texInfoOffset; }
    void* GetVertexBuffer() const { return vertexBuffer; }
    void* GetIndexBuffer() const { return indexBuffer; }

private:
    void EnsureVertexBuffer(u32 byteSize);
    void EnsureIndexBuffer(u32 byteSize);

    pddiPrimType primType;
    u32 vertexFormat = 0;
    u32 vertexCount = 0;
    u32 indexCount = 0;
    u32 stride = 0;
    u32 posOffset = 0;
    u32 colOffset = 0;
    u32 uvOffset = 0;
    u32 texInfoOffset = 0;
    u32 vertexCapacity = 0;
    u32 indexCapacity = 0;
    void* vertexBuffer = nullptr;  // id<MTLBuffer>
    void* indexBuffer = nullptr;   // id<MTLBuffer>
};

// mtTexture — RGBA8 texture (real-texture path and debug previews).

enum mtTextureKind {
    MT_TEX_RGBA8 = 0,
    MT_TEX_RGBA16F = 1,
    MT_TEX_DEPTH = 2,
    MT_TEX_ID_UINT = 3,
};

class mtTexture : public pddiTexture {
public:
    mtTexture();
    ~mtTexture() override;

    int GetWidth() override { return width; }
    int GetHeight() override { return height; }
    int GetBpp() override { return bpp; }
    int GetAlphaDepth() override { return alphaDepth; }

    void SetData(int w, int h, int bpp, int alphaDepth, const void* rgba) override;
    void SetFilterMode(pddiFilterMode mode) override;
    void Bind(int unit) override;

    // Wraps an already-created MTLTexture (render targets). Takes ownership.
    void AdoptMetalTexture(void* tex, int w, int h, u32 kindIn);
    void* GetMetalTexture() const { return texture; }
    u32 GetKind() const { return kind; }
    bool IsRenderTarget() const { return isRenderTarget; }
    pddiFilterMode GetFilterMode() const { return filterMode; }

private:
    void* texture = nullptr;  // id<MTLTexture>
    u32 kind = MT_TEX_RGBA8;
    bool isRenderTarget = false;
    int width = 0;
    int height = 0;
    int bpp = 0;
    int alphaDepth = 0;
    pddiFilterMode filterMode = PDDI_FILTER_NONE;
};

// mtShader — material parameter storage (programmable path TBD).

class mtShader : public pddiBaseShader {
public:
    explicit mtShader(const char* type);
    ~mtShader() override;

    const char* GetType() override { return type.c_str(); }

    void SetTexture(u32 param, pddiTexture* tex) override;
    void SetInt(u32 param, int value) override;
    void SetFloat(u32 param, float value) override;
    void SetColour(u32 param, pddiColour c) override;
    void SetInt(const char* param, int value) override;
    void SetFloat(const char* param, float value) override;
    void SetVector(const char* param, float x, float y, float z, float w) override;
    void SetMatrix(const char* param, const float* matrix4x4) override;

    void PreRender() override;
    void PostRender() override;

    pddiTexture* GetBoundTexture() const { return texture; }
    pddiColour GetDiffuse() const { return diffuse; }
    const std::unordered_map<std::string, std::array<float, 4>>& GetVectors() const {
        return vectorParams;
    }
    const std::unordered_map<std::string, float>& GetFloats() const { return floatParams; }
    const std::unordered_map<std::string, int>& GetInts() const { return intParams; }

private:
    std::string type;
    pddiTexture* texture = nullptr;
    pddiColour diffuse = pddiColour(255, 255, 255);
    std::unordered_map<std::string, int> intParams;
    std::unordered_map<std::string, float> floatParams;
    std::unordered_map<std::string, std::array<float, 4>> vectorParams;
};

// mtRenderTarget — off-screen target (real GPU backing lands in milestone 3).

class mtRenderTarget : public pddiRenderTarget {
public:
    mtRenderTarget(int width, int height, pddiRenderTargetFormat format,
                   bool withInstanceId = false);
    ~mtRenderTarget() override;

    bool Resize(int width, int height) override;
    int GetWidth() const override { return width; }
    int GetHeight() const override { return height; }
    pddiTexture* GetTexture() const override { return texture; }
    pddiTexture* GetIdTexture() const override { return idTexture; }
    bool IsValid() const override { return valid; }

    pddiRenderTargetFormat GetFormat() const { return format; }
    bool IsDepthFormat() const;
    bool HasInstanceId() const { return idTexture != nullptr; }
    void* GetColorMetalTexture() const;
    void* GetDepthMetalTexture() const;
    void* GetIdMetalTexture() const;

private:
    bool CreateStorage(int w, int h);

    mtTexture* texture = nullptr;
    mtTexture* idTexture = nullptr;
    int width = 0;
    int height = 0;
    pddiRenderTargetFormat format;
    bool valid = false;
};

// mtDisplay — GLFW window backed by a CAMetalLayer drawable + depth buffer.

class mtDisplay : public pddiDisplay {
public:
    mtDisplay();
    ~mtDisplay() override;

    bool  InitDisplay(const pddiDisplayInit& init) override;
    void  SwapBuffers() override;
    int   GetWidth() override { return fbWidth; }
    int   GetHeight() override { return fbHeight; }
    bool  ShouldClose() override;
    void  PollEvents() override;

    bool IsKeyDown(int key) override;
    bool IsMouseButtonDown(int button) override;
    void GetMousePosition(double& x, double& y) override;

    void SetIcon(int w, int h, const unsigned char* rgba) override;

    int  GetVideoModeCount() override;
    void GetVideoMode(int index, pddiVideoMode& mode) override;
    void SetFullscreen(bool fullscreen) override;
    bool IsFullscreen() override { return fullscreen; }
    void SetBorderless(bool borderless) override;
    bool IsBorderless() override { return borderless; }
    void SetResolution(int w, int h) override;
    void SetVSync(bool enabled) override { vsync = enabled; }
    void SetMSAA(int samples) override;
    int  GetMSAA() override { return msaaSamples; }
    void SetWindowPos(int x, int y) override;

    void SetTitle(const char* title) override;

    void ShowCursor(bool visible) override;
    void ClipCursor(bool clip) override;

    void SetWndProc(pddiWndProc proc) override { wndProc = std::move(proc); }

    void AddOverlayCallback(OverlayCallback cb) override;
    void RenderOverlay() override;

    // Frame lifecycle driven by mtContext.
    void SetClearColour(pddiColour c) { clearColour = c; }
    void BeginFrame();
    void EndFrame();

    GLFWwindow* GetWindow() const { return static_cast<GLFWwindow*>(window); }
    void* GetDevice() const { return device; }
    void* GetQueue() const { return queue; }
    void* GetEncoder() const { return renderEncoder; }
    bool IsFrameActive() const { return frameActive; }
    int GetPixelWidth() const { return fbWidth; }
    int GetPixelHeight() const { return fbHeight; }

    // Off-screen render target passes (switches encoders and restores the
    // default drawable encoder afterwards).
    bool BeginRenderTargetPass(mtRenderTarget* target);
    void EndRenderTargetPass();

private:
    void* CreateEncoderForPass(void* colorTex, void* depthTex, void* idTex,
                               bool clearColor, bool clearDepth, int width, int height);
    void EndCurrentEncoder();
    void* window = nullptr;          // GLFWwindow*
    void* device = nullptr;          // id<MTLDevice>
    void* queue = nullptr;           // id<MTLCommandQueue>
    void* layer = nullptr;           // CAMetalLayer*
    void* drawable = nullptr;        // id<CAMetalDrawable>
    void* commandBuffer = nullptr;   // id<MTLCommandBuffer>
    void* renderEncoder = nullptr;   // id<MTLRenderCommandEncoder>
    void* depthTexture = nullptr;    // id<MTLTexture> (Depth32Float)
    void* drawableTexture = nullptr; // id<MTLTexture> (drawable colour)
    int depthWidth = 0;
    int depthHeight = 0;
    bool inTargetPass = false;
    bool imguiInitialized = false;
    bool imguiFrameStarted = false;

    bool frameActive = false;
    bool vsync = true;
    bool fullscreen = false;
    bool borderless = false;
    bool cursorVisible = true;
    bool cursorClipped = false;
    int fbWidth = 0;
    int fbHeight = 0;
    int windowedX = 100;
    int windowedY = 100;
    int windowedW = 960;
    int windowedH = 720;
    int msaaSamples = 0;
    pddiColour clearColour = pddiColour(0, 0, 0);
    std::vector<OverlayCallback> overlayCallbacks;
    pddiWndProc wndProc;

    bool SyncDrawableSize();
    bool EnsureDepthTexture();
};

// mtContext — render context with Metal pipeline state and draw encoding.

class mtContext : public pddiRenderContext {
public:
    explicit mtContext(mtDisplay* disp);
    ~mtContext() override;

    void BeginFrame() override;
    void EndFrame() override;

    void SetCameraAspect(float aspect) override { cameraAspect = aspect; }
    float GetCameraAspect() const override { return cameraAspect; }

    void SetClearColour(pddiColour c) override;
    void Clear(int flags) override;

    void SetProjectionMatrix(const Mat4& m) override { projection = m; }
    void SetViewMatrix(const Mat4& m) override { viewMatrix = m; }
    void SetWorldMatrix(const Mat4& m) override { worldMatrix = m; }
    const Mat4& GetWorldMatrix() const override { return worldMatrix; }
    const Mat4& GetViewMatrix() const override { return viewMatrix; }
    const Mat4& GetProjectionMatrix() const override { return projection; }

    void SetWorldMirror(bool enable) override { worldMirror = enable; }
    bool GetWorldMirror() const override { return worldMirror; }

    void SetCullMode(pddiCullMode mode) override { cullMode = mode; }
    void EnableZBuffer(bool enable) override { zBufferEnabled = enable; }
    void SetBlendMode(pddiBlendMode mode) override { blendMode = mode; }
    void SetDepthClamp(bool enable) override { depthClamp = enable; }
    void SetPolygonOffset(bool enable, f32 factor = 0.0f, f32 units = 0.0f) override;
    void SetScissor(int x, int y, int w, int h) override;
    void SetMultisampleEnabled(bool enable) override { multisampleEnabled = enable; }
    void ResolveForOverlayPass() override {}

    pddiRenderTarget* CreateRenderTarget(int width, int height,
                                         pddiRenderTargetFormat format,
                                         bool withInstanceId = false) override;
    bool SetRenderTarget(pddiRenderTarget* target) override;

    void DrawFilledCircle(pddiBaseShader* shader, float centerX, float centerY,
                          float radiusX, float radiusY, float u0, float v0,
                          float u1, float v1, int segments) override;
    void DrawCircle(pddiBaseShader* shader, float centerX, float centerY,
                    float radiusX, float radiusY, float thickness, float u0,
                    float v0, float u1, float v1, int segments) override;
    void DrawQuad(pddiBaseShader* shader, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1) override;
    void DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                       const pddiBatchVertex* verts, s32 vertCount) override;
    void DrawPrimBuffer(pddiPrimBuffer* buffer, u32 indexOffset = 0,
                        u32 indexCount = 0) override;
    void DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                         float x1, float y1, float r1, float g1, float b1, float a1,
                         float x2, float y2, float r2, float g2, float b2, float a2,
                         float x3, float y3, float r3, float g3, float b3, float a3) override;

    void SetTexture(pddiTexture* tex) override { currentTexture = tex; }
    void SetVRAMHandle(u32 handle) override { vramHandle = handle; }
    void SetTexInfoOverride(bool enabled, u32 texInfoWord) override;
    u32  CreateVRAMTexture(int w, int h, const u16* data) override;
    void DestroyVRAMTexture(u32 handle) override;
    void UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) override;
    void UpdateVRAMTextureRect(u32 handle, int x, int y, int w, int h,
                               int srcPitchTexels, const u16* data) override;

    void SetRealTextureMode(bool enabled) override { realTextureMode = enabled; }
    bool IsRealTextureModeEnabled() const override { return realTextureMode; }
    void SetRealTextureRect(float offsetX, float offsetY, float sizeX, float sizeY) override;

    void SetShadowCasterPass(bool enable, const Mat4& lightVP) override;
    void SetReceiveShadows(bool enable) override { receiveShadows = enable; }
    void SetShadowCascades(pddiTexture* const* depthTextures, const Mat4* lightVP,
                           const float* splits, const float* texelWorldSizes,
                           pddiTexture* const* idTextures, int count) override;
    void SetCameraWorldPos(float x, float y, float z) override;
    void SetShadowLightDirection(float x, float y, float z) override;
    void SetShadowCasterInstanceId(u32 id) override { shadowCasterInstanceId = id; }
    void SetShadowReceiverInstanceId(u32 id) override { shadowReceiverInstanceId = id; }
    void ClearShadowCasterIdTarget() override {}
    void SetShadowDebugMode(int mode) override { shadowDebugMode = mode; }

private:
    void EnsureLibrary();
    enum MtProgram {
        MT_PROG_3D = 0,
        MT_PROG_QUAD2D = 1,
        MT_PROG_GOURAUD = 2,
        MT_PROG_BATCH = 3,
        MT_PROG_TILT = 4,
        MT_PROG_GLOW = 5,
        MT_PROG_GODRAYS = 6,
        MT_PROG_DOT = 7,
        MT_PROG_MOVIEDENOISE = 8,
        MT_PROG_MOVIEUPSCALE = 9,
        MT_PROG_MOVIESHARP = 10,
        MT_PROG_SHADOWDEPTH = 11,
    };
    enum MtSurface {
        MT_SURF_DEFAULT = 0,
        MT_SURF_RGBA8 = 1,
        MT_SURF_RGBA16F = 2,
        MT_SURF_SHADOW = 3,
    };
    void* GetPipelineState(int program, pddiBlendMode blend, pddiCullMode cull,
                           bool depthTest, bool depthWrite, int surface);
    void* GetSampler(pddiFilterMode filter);
    void* GetDepthStencilState(bool depthTest, bool depthWrite);
    bool GetComparisonSampler();
    void ApplyEncoderState(void* encoder, pddiBlendMode blend, pddiCullMode cull,
                           bool depthTest, bool depthWrite);
    void* ResolveVRAMTexture(u32 handle);
    void* GetDummyTexture();
    void* GetDummyVRAMTexture();
    void* GetDummyDepthTexture();
    void* GetDummyIdTexture();
    void* GetWhiteTexture();
    void* ResolveShaderTexture(pddiBaseShader* shader);
    void* UploadDynamic(const void* data, u32 bytes);
    u32 GetDynamicOffset() const { return dynamicOffset; }
    int ActiveSurface() const;
    void BindVariantUniforms(void* encoder, pddiBaseShader* shader);

    mtDisplay* display = nullptr;
    float cameraAspect = 0.0f;
    pddiColour clearColour = pddiColour(0, 0, 0);
    Mat4 projection;
    Mat4 viewMatrix;
    Mat4 worldMatrix;
    pddiTexture* currentTexture = nullptr;
    u32 vramHandle = 0;
    bool texInfoOverrideEnabled = false;
    u32 texInfoOverrideWord = 0;
    bool realTextureMode = false;
    float realTexOffsetX = 0.0f;
    float realTexOffsetY = 0.0f;
    float realTexSizeX = 1.0f;
    float realTexSizeY = 1.0f;
    s32 scissor[4] = {};
    pddiCullMode cullMode = PDDI_CULL_NONE;
    bool zBufferEnabled = false;
    pddiBlendMode blendMode = PDDI_BLEND_NONE;
    bool depthClamp = false;
    bool multisampleEnabled = true;
    bool worldMirror = false;
    bool polyOffsetActive = false;
    f32 polyOffsetFactor = 0.0f;
    f32 polyOffsetUnits = 0.0f;
    mtRenderTarget* activeRenderTarget = nullptr;

    void* library = nullptr;  // id<MTLLibrary>
    std::unordered_map<u64, void*> pipelines;
    std::unordered_map<u64, void*> samplers;
    std::unordered_map<u64, void*> depthStates;
    std::unordered_map<u32, void*> vramTextures;
    u32 nextVramHandle = 1;
    void* dummyTexture = nullptr;
    void* dummyVRAM = nullptr;
    void* dummyDepth = nullptr;
    void* dummyId = nullptr;
    void* whiteTexture = nullptr;
    std::vector<void*> dynamicBuffers;
    u32 dynamicUsed = 0;
    u32 dynamicCapacity = 0;
    u32 dynamicOffset = 0;

    static constexpr int kShadowCascadeCount = 3;
    pddiTexture* shadowDepthTextures[kShadowCascadeCount] = {};
    pddiTexture* shadowIdTextures[kShadowCascadeCount] = {};
    Mat4 shadowLightVP[kShadowCascadeCount];
    float shadowCascadeSplits[kShadowCascadeCount] = {};
    float shadowCascadeBlendDistances[kShadowCascadeCount] = {};
    float shadowTexelWorldSize[kShadowCascadeCount] = {};
    float shadowBias[kShadowCascadeCount] = { 0.00115f, 0.00085f, 0.00062f };
    s32 shadowCascadeCount = 0;
    s32 shadowFilterQuality = 0;
    void* compareSampler = nullptr;

    bool receiveShadows = false;
    bool shadowCasterPass = false;
    Mat4 shadowCasterLightVP;
    u32 shadowCasterInstanceId = 0;
    u32 shadowReceiverInstanceId = 0;
    s32 shadowDebugMode = 0;
    float cameraWorldPos[3] = {};
    float shadowLightDir[3] = { 0.35f, -0.85f, 0.25f };
};

// mtGamepad — controller support (GLFW gamepad wiring lands later).

class mtGamepad : public pddiGamepad {
public:
    mtGamepad();
    ~mtGamepad() override;
    void Poll() override;
    bool IsConnected() const override { return connected; }
    bool IsButtonDown(int button) const override;
    float GetAxis(int axis) const override;
    bool SupportsVibration() const override;
    bool SetVibration(float lowFrequency, float highFrequency) override;
    void SetLight(unsigned char r, unsigned char g, unsigned char b) override;

private:
    bool EnsureHidDevice();
    void SendRumbleReport();

    bool connected = false;
    bool buttons[GamepadButton::COUNT] = {};
    float axes[GamepadAxis::COUNT] = {};
    void* hidDevice = nullptr;  // IOHIDDeviceRef (Sony controller)
    unsigned int hidProductId = 0;
    bool vibrationSupported = false;
    bool hidLookupFailed = false;
    float rumbleLow = 0.0f;
    float rumbleHigh = 0.0f;
    unsigned char lightR = 0, lightG = 0, lightB = 0;
};

// mtDevice — factory for all Metal pddi objects.

class mtDevice : public pddiDevice {
public:
    pddiDisplay* NewDisplay() override;
    pddiRenderContext* NewRenderContext(pddiDisplay* display) override;
    pddiGamepad* NewGamepad() override;
    pddiTexture* NewTexture() override;
    pddiPrimBuffer* NewPrimBuffer(const pddiPrimBufferDesc& desc) override;
    pddiBaseShader* NewShader(const char* type) override;
};

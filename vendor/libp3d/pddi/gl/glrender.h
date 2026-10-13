// glrender.h — OpenGL implementation of pddi interfaces
#pragma once

#include "pddi/pddi.h"
#include "pddi/pdditex.h"
#include "pddi/pddishad.h"
#include "pddi/pddidev.h"
#include "pddi/pddishadersrc.h"
#include <array>
#include <string>
#include <unordered_map>

struct GLFWwindow;

// glPrimBuffer──

class glPrimBuffer : public pddiPrimBuffer {
public:
    glPrimBuffer(const pddiPrimBufferDesc& desc);
    ~glPrimBuffer() override;

    void SetVertexData(const void* data, u32 count) override;
    void SetIndices(const u16* indices, u32 count) override;
    u32 GetIndexCount() const override { return indexCount; }
    u32 GetVertexCount() const override { return vertexCount; }
    pddiPrimType GetPrimType() const override { return primType; }

    u32 GetVAO() const { return vao; }
    u32 GetVertexFormat() const { return vertexFormat; }

private:
    pddiPrimType primType;
    u32 vertexFormat;
    u32 vertexCount = 0;
    u32 indexCount = 0;
    // Allocated GPU storage, which can exceed the live counts: callers that
    // refill a buffer every frame then update in place instead of reallocating.
    u32 vertexCapacity = 0;
    u32 indexCapacity = 0;
    u32 stride = 0;
    u32 vao = 0;
    u32 vbo = 0;
    u32 ebo = 0;
    // Set when the GL objects came from the recycle pool: the first write has
    // to orphan the storage rather than patch a buffer still in flight.
    bool orphanOnNextWrite = false;

    void SetupVertexAttribs();
};

// glTexture──

class glTexture : public pddiTexture {
public:
    glTexture();
    ~glTexture() override;

    int  GetWidth() override { return width; }
    int  GetHeight() override { return height; }
    int  GetBpp() override { return bpp; }
    int  GetAlphaDepth() override { return alphaDepth; }

    void SetData(int w, int h, int bpp, int alphaDepth, const void* rgba) override;
    void SetFilterMode(pddiFilterMode mode) override;
    void Bind(int unit) override;

    u32 GetGLHandle() const { return handle; }
    unsigned int GetNativeHandle() const override { return handle; }
    bool SetRenderTargetStorage(int w, int h, pddiRenderTargetFormat format);
    // R32UI storage for shadow-caster instance-ID render targets.
    bool SetIdTargetStorage(int w, int h);

private:
    u32 handle = 0;
    int width = 0;
    int height = 0;
    int bpp = 0;
    int alphaDepth = 0;
    pddiFilterMode filterMode = PDDI_FILTER_NONE;
};

// glShader──

class glShader : public pddiBaseShader {
public:
    explicit glShader(const char* type);
    ~glShader() override;

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

    u32 GetProgram() const { return program; }
    bool IsValid() const { return program != 0; }

private:
    static constexpr s32 kMaxTextureSlots = 16;
    u32 program = 0;
    std::string type;
    pddiTexture* texSlots[kMaxTextureSlots] = {};
    pddiColour diffuse = pddiColour(255, 255, 255);
    pddiBlendMode blendMode = PDDI_BLEND_NONE;

    std::unordered_map<std::string, int> intParams;
    std::unordered_map<std::string, float> floatParams;
    std::unordered_map<std::string, std::array<float, 4>> vectorParams;
    std::unordered_map<std::string, std::array<float, 16>> matrixParams;
    std::unordered_map<std::string, int> uniformLocations;

    void CreateProgram();
    int FindUniform(const std::string& name);
};

class glRenderTarget : public pddiRenderTarget {
public:
    glRenderTarget(int width, int height, pddiRenderTargetFormat format, bool withInstanceId = false);
    ~glRenderTarget() override;

    bool Resize(int width, int height) override;
    int GetWidth() const override { return width; }
    int GetHeight() const override { return height; }
    pddiTexture* GetTexture() const override { return texture; }
    pddiTexture* GetIdTexture() const override { return idTexture; }
    bool IsValid() const override { return valid; }
    u32 GetFramebuffer() const { return framebuffer; }

private:
    glTexture* texture = nullptr;
    glTexture* idTexture = nullptr;
    bool wantsIdAttachment = false;
    u32 framebuffer = 0;
    int width = 0;
    int height = 0;
    pddiRenderTargetFormat format;
    bool valid = false;
};

// glDisplay

class glDisplay : public pddiDisplay {
public:
    glDisplay();
    ~glDisplay() override;

    bool  InitDisplay(const pddiDisplayInit& init) override;
    void  SwapBuffers() override;
    bool  ReadBackBuffer(std::vector<unsigned char>& rgb, int& width, int& height) override;
    int   GetWidth() override;
    int   GetHeight() override;
    bool  ShouldClose() override;
    void  PollEvents() override;

    // Input polling
    bool IsKeyDown(int key) override;
    bool IsMouseButtonDown(int button) override;
    void GetMousePosition(double& x, double& y) override;

    void SetIcon(int w, int h, const unsigned char* rgba) override;

    // Video mode
    int  GetVideoModeCount() override;
    void GetVideoMode(int index, pddiVideoMode& mode) override;
    void SetFullscreen(bool fullscreen) override;
    bool IsFullscreen() override;
    void SetBorderless(bool borderless) override;
    bool IsBorderless() override { return borderless; }
    void SetResolution(int w, int h) override;
    void SetVSync(bool enabled) override;
    void SetMSAA(int samples) override;
    int  GetMSAA() override { return msaaSamples; }
    void SetWindowPos(int x, int y) override;

    void SetTitle(const char* title) override;

    // Cursor
    void ShowCursor(bool visible) override;
    void ClipCursor(bool clip) override;

    // WndProc callback
    void SetWndProc(pddiWndProc proc) override;

    // Overlay
    void AddOverlayCallback(OverlayCallback cb) override;
    void RenderOverlay() override;

    // Viewport
    void GetViewport(int& x, int& y, int& w, int& h);

    GLFWwindow* GetWindow() const { return window; }

private:
    GLFWwindow* window = nullptr;
    bool imguiInitialized = false;
    bool imguiFrameStarted = false;
    std::vector<OverlayCallback> overlayCallbacks = {};
    int fbWidth = 0;
    int fbHeight = 0;
    int windowedX = 100, windowedY = 100;
    int windowedW = 960, windowedH = 720;
    bool borderless = false;
    bool cursorVisible = true;
    bool cursorClipped = false;
    bool focused = true;
    int msaaSamples = 0;
    int maxMsaaSamples = 0;
    pddiWndProc wndProc;

    void UpdateCursorClip();
    int ClampMSAASamples(int samples) const;
    void SyncFramebufferSize();
    void QueryFramebufferSize(int& width, int& height) const;
    void PresentBlackFrame();

    static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void WindowFocusCallback(GLFWwindow* window, int focused);
    static void WindowCloseCallback(GLFWwindow* window);
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double x, double y);
    static void ScrollCallback(GLFWwindow* window, double xoff, double yoff);
};

// glContext──

class glContext : public pddiRenderContext {
public:
    glContext(glDisplay* disp);
    ~glContext() override;

    void BeginFrame() override;
    void EndFrame() override;

    void SetCameraAspect(float aspect) override { cameraAspect = aspect; }
    float GetCameraAspect() const override { return cameraAspect; }

    void SetClearColour(pddiColour c) override;
    void Clear(int flags) override;

    void SetProjectionMatrix(const Mat4& m) override;
    void SetViewMatrix(const Mat4& m) override;
    void SetWorldMatrix(const Mat4& m) override;
    const Mat4& GetWorldMatrix() const override { return worldMatrix; }
    const Mat4& GetViewMatrix() const override { return viewMatrix; }
    const Mat4& GetProjectionMatrix() const override { return projection; }

    void SetWorldMirror(bool enable) override;
    bool GetWorldMirror() const override { return worldMirror; }

    void SetCullMode(pddiCullMode mode) override;
    void EnableZBuffer(bool enable) override;
    void SetBlendMode(pddiBlendMode mode) override;
    void SetDepthClamp(bool enable) override;
    void SetPolygonOffset(bool enable, f32 factor = 0.0f, f32 units = 0.0f) override;
    void SetScissor(int x, int y, int w, int h) override;
    void SetMultisampleEnabled(bool enable) override;
    void ResolveForOverlayPass() override;
    pddiRenderTarget* CreateRenderTarget(int width, int height,
                                         pddiRenderTargetFormat format,
                                         bool withInstanceId = false) override;
    bool SetRenderTarget(pddiRenderTarget* target) override;

    void DrawFilledCircle(pddiBaseShader* shader,
                                     float centerX, float centerY,
                                     float radiusX, float radiusY,
                                     float u0, float v0, float u1, float v1,
                                     int segments) override;

    void DrawCircle(pddiBaseShader* shader,
                               float centerX, float centerY,
                               float radiusX, float radiusY,
                               float thickness,
                               float u0, float v0, float u1, float v1,
                               int segments) override;

    void DrawQuad(pddiBaseShader* shader,
                  float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1) override;

    void DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                       const pddiBatchVertex* verts, s32 vertCount) override;

    void DrawPrimBuffer(pddiPrimBuffer* buffer, u32 indexOffset = 0, u32 indexCount = 0) override;

    void SetTexture(pddiTexture* tex) override;
    void SetVRAMHandle(u32 handle) override;
    void SetTexInfoOverride(bool enabled, u32 texInfoWord) override;

    u32  CreateVRAMTexture(int w, int h, const u16* data) override;
    void DestroyVRAMTexture(u32 handle) override;
    void UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) override;
    void UpdateVRAMTextureRect(u32 handle, int x, int y, int w, int h,
                               int srcPitchTexels, const u16* data) override;

    void SetRealTextureMode(bool enabled) override { realTextureModeEnabled = enabled; }
    bool IsRealTextureModeEnabled() const override { return realTextureModeEnabled; }
    void SetRealTextureRect(float offsetX, float offsetY, float sizeX, float sizeY) override;

    void SetShadowCasterPass(bool enable, const Mat4& lightVP) override;
    void SetReceiveShadows(bool enable) override { receiveShadowsEnabled = enable; }
    void SetShadowCascades(pddiTexture* const* depthTextures, const Mat4* lightVP,
                           const float* splits, const float* texelWorldSizes,
                           pddiTexture* const* idTextures, int count) override;
    void SetCameraWorldPos(float x, float y, float z) override {
        cameraWorldPos[0] = x; cameraWorldPos[1] = y; cameraWorldPos[2] = z;
        frameConstUniformsDirty = true;
    }
    void SetShadowLightDirection(float x, float y, float z) override {
        shadowLightDir[0] = x; shadowLightDir[1] = y; shadowLightDir[2] = z;
    }
    void SetShadowDebugMode(int mode) override { shadowDebugMode = mode; frameConstUniformsDirty = true; }
    void SetShadowCasterInstanceId(u32 id) override { shadowCasterInstanceId = id; }
    void SetShadowReceiverInstanceId(u32 id) override { shadowReceiverInstanceId = id; }
    void ClearShadowCasterIdTarget() override;

    u32 Get3DProgram() const { return program3D; }

    void DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                         float x1, float y1, float r1, float g1, float b1, float a1,
                         float x2, float y2, float r2, float g2, float b2, float a2,
                         float x3, float y3, float r3, float g3, float b3, float a3);

private:
    glDisplay* display;
    float cameraAspect = 0.0f;
    pddiColour clearColour;
    Mat4 projection;
    Mat4 viewMatrix;
    Mat4 worldMatrix;
    pddiTexture* currentTexture = nullptr;
    u32 vramHandle = 0;
    bool texInfoOverrideEnabled = false;
    u32 texInfoOverrideWord = 0;
    bool realTextureModeEnabled = false;
    float realTexOffsetX = 0.0f, realTexOffsetY = 0.0f, realTexSizeX = 1.0f, realTexSizeY = 1.0f;
    u32 quadVAO = 0;
    u32 quadVBO = 0;
    u32 program3D = 0;
    u32 shadowDepthProgram = 0;
    u32 shadowCompareSampler = 0;
    bool shadowCasterPassActive = false;
    Mat4 shadowCasterLightVP;
    bool receiveShadowsEnabled = false;
    static constexpr s32 kShadowCascadeCount = 3;

    // Uniform locations for program3D/shadowDepthProgram, cached once at link time
    // instead of re-hashing the name string on every DrawPrimBuffer call.
    struct Prog3DUniforms {
        s32 alphaScale, useZeroTexelKey, mvp, worldMatrix, viewMatrix, cameraPos,
            shadowDebugMode, receiveShadows, shadowCascadeCount, shadowFilterQuality,
            shadowBias, shadowLightDir, lightVP, cascadeSplits, cascadeBlendDistances,
            shadowTexelWorldSize, receiverInstanceId,
            shadowMap[kShadowCascadeCount], shadowIdMap[kShadowCascadeCount],
            hasVRAM, texInfoOverrideEnabled, texInfoOverride, vram,
            realTextureMode, realTex, realTexOffset, realTexSize;
    } u3D{};
    struct ShadowDepthUniforms {
        s32 lightMVP, casterInstanceId, hasUV, hasTexInfo, hasVRAM, useZeroTexelKey,
            texInfoOverrideEnabled, texInfoOverride, vram,
            realTextureMode, realTex, realTexOffset, realTexSize;
    } uShadowDepth{};
    pddiTexture* shadowDepthTextures[kShadowCascadeCount] = {};
    pddiTexture* shadowIdTextures[kShadowCascadeCount] = {};
    u32 shadowCasterInstanceId = 0;
    u32 shadowReceiverInstanceId = 0;
    Mat4 shadowLightVP[kShadowCascadeCount];
    float shadowCascadeSplits[kShadowCascadeCount] = {};
    float shadowCascadeBlendDistances[kShadowCascadeCount] = {};
    s32 shadowCascadeCount = 0;
    s32 shadowFilterQuality = 0;
    // Raised versus the original tiers to compensate for the tightened
    // normal-offset clamp in SampleCoveredCascade (grazing-angle faces now
    // rely more on this constant/slope bias than on a large normal offset).
    float shadowBias[kShadowCascadeCount] = { 0.00115f, 0.00085f, 0.00062f };
    static constexpr float shadowBiasLow[kShadowCascadeCount] = { 0.00170f, 0.00125f, 0.00090f };
    static constexpr float shadowBiasMedium[kShadowCascadeCount] = { 0.00115f, 0.00085f, 0.00062f };
    static constexpr float shadowBiasHigh[kShadowCascadeCount] = { 0.00072f, 0.00055f, 0.00040f };
    static constexpr float shadowBiasVeryHigh[kShadowCascadeCount] = { 0.00046f, 0.00035f, 0.00026f };
    float cameraWorldPos[3] = {};
    float shadowLightDir[3] = { 0.35f, -0.85f, 0.25f };
    float shadowTexelWorldSize[kShadowCascadeCount] = {};
    s32 shadowDebugMode = 0;

    // Last values pushed to program3D. Uniform values live in the program
    // object and survive glUseProgram, so comparing against the last write is
    // enough to skip a redundant call. The hundreds of effect/trail draws in a
    // frame all share these; only the matrices genuinely change per draw.
    struct Prog3DCache {
        f32 alphaScale = -1.0f;
        s32 useZeroTexelKey = -1;
        s32 receiveShadows = -1;
        s32 shadowCascadeCount = -1;
        s32 shadowFilterQuality = -1;
        s32 texInfoOverrideEnabled = -1;
        f32 texInfoTpage = -2.0f;
        f32 texInfoCba = -2.0f;
        s32 hasVRAM = -1;
        s32 realTextureMode = -1;
    } prog3DCache;


    bool shadowConstUniformsDirty = true;
    bool shadowCascadeUniformsDirty = true;
    bool frameConstUniformsDirty = true;
    u32 gouraudVAO = 0;
    u32 gouraudVBO = 0;
    u32 gouraudProgram = 0;
    s32 gouraudUProjLoc = -1;
    u32 batchVAO = 0;
    u32 batchVBO = 0;
    u32 batchProgram = 0;
    s32 batchUProjLoc = -1;
    s32 batchUTexLoc = -1;
    size_t batchVBOCapacityBytes = 0;
    u32 msaaFbo = 0;
    u32 msaaColorRbo = 0;
    u32 msaaDepthStencilRbo = 0;
    s32 msaaWidth = 0;
    s32 msaaHeight = 0;
    s32 activeMsaaSamples = 0;
    bool usingMsaaFramebuffer = false;
    bool multisampleEnabled = true;
    bool resolvedForOverlay = false;
    glRenderTarget* activeRenderTarget = nullptr;
    s32 savedFramebuffer = 0;
    s32 savedViewport[4] = {};
    s32 savedScissor[4] = {};
    bool savedScissorEnabled = false;

    // PC-only cheat hook: see pddiRenderContext::SetWorldMirror.
    bool worldMirror = false;

    // Renderstate cache
    pddiCullMode cachedCullMode = PDDI_CULL_NONE;
    bool cachedZBuffer = false;
    bool cachedDepthClamp = false;
    pddiBlendMode cachedBlendMode = PDDI_BLEND_NONE;
    bool stateDirty = true;
    // Polygon offset override: when true, SetBlendMode(BLEND_NONE) restores these
    // values instead of disabling polygon offset.
    bool polyOffsetOverride = false;
    f32 polyOffsetFactor = 0.0f;
    f32 polyOffsetUnits = 0.0f;

    void InitQuadMesh();
    void InitGouraudMesh();
    void InitBatchMesh();
    void Init3DShader();
    void InitShadowDepthShader();
    void UpdateMultisampleState();
    void EnsureMSAAFramebuffer(s32 samples, s32 width, s32 height);
    void DestroyMSAAFramebuffer();
};

// glDevice

class glDevice : public pddiDevice {
public:
    pddiDisplay* NewDisplay() override;
    pddiRenderContext* NewRenderContext(pddiDisplay* display) override;
    pddiGamepad* NewGamepad() override;
    pddiTexture* NewTexture() override;
    pddiPrimBuffer* NewPrimBuffer(const pddiPrimBufferDesc& desc) override;
    pddiBaseShader* NewShader(const char* type) override;
};

// glGamepad

class glGamepad : public pddiGamepad {
public:
    ~glGamepad() override;
    void Poll() override;
    bool IsConnected() const override { return connected; }
    bool IsButtonDown(int button) const override;
    float GetAxis(int axis) const override;
    bool SupportsVibration() const override;
    bool SetVibration(float lowFrequency, float highFrequency) override;
    void SetLight(unsigned char r, unsigned char g, unsigned char b) override;

private:
    bool connected = false;
    bool buttons[GamepadButton::COUNT] = {};
    float axes[GamepadAxis::COUNT] = {};
    int activeJoystickId = -1;
    float lastLow = 0.0f;
    float lastHigh = 0.0f;
    unsigned char lightR = 0;
    unsigned char lightG = 0;
    unsigned char lightB = 0;
};

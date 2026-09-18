#pragma once

// Native PSP PDDI backend.  The renderer deliberately uses the fixed
// function GU instead of a desktop compatibility layer: the game submits the
// same PDDI primitives, while PSX VRAM/CLUT pages are decoded once into a
// bounded page cache in aligned main memory; the eDRAM remains reserved for
// the double framebuffer and depth buffer.

#include "pddi/pddi.h"
#include "pddi/pdditex.h"
#include "pddi/pddishad.h"
#include "pddi/pddidev.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

struct PspVertex {
    float u, v;
    u32 color;
    float x, y, z;
};

class pspPrimBuffer final : public pddiPrimBuffer {
public:
    explicit pspPrimBuffer(const pddiPrimBufferDesc& desc);

    void SetVertexData(const void* data, u32 count) override;
    void SetIndices(const u16* indices, u32 count) override;
    u32 GetIndexCount() const override { return static_cast<u32>(indices.size()); }
    u32 GetVertexCount() const override { return static_cast<u32>(vertices.size()); }
    pddiPrimType GetPrimType() const override { return primType; }

    const PspVertex* GetVertices() const { return vertices.data(); }
    const u16* GetIndices() const { return indices.data(); }
    float GetTPage(u16 index) const { return tpages[index]; }
    float GetCba(u16 index) const { return cbas[index]; }

private:
    pddiPrimType primType;
    u32 vertexFormat;
    std::vector<PspVertex> vertices;
    std::vector<float> tpages;
    std::vector<float> cbas;
    std::vector<u16> indices;
};

class pspTexture final : public pddiTexture {
public:
    pspTexture() = default;
    ~pspTexture() override;

    int GetWidth() override { return width; }
    int GetHeight() override { return height; }
    int GetBpp() override { return bpp; }
    int GetAlphaDepth() override { return alphaDepth; }

    void SetData(int w, int h, int bppIn, int alphaDepthIn, const void* rgba) override;
    void SetNative4444Data(int w, int h, const u16* source);
    void SetFilterMode(pddiFilterMode mode) override { filterMode = mode; }
    void Bind(int unit) override;

    int GetStorageWidth() const { return storageWidth; }
    int GetStorageHeight() const { return storageHeight; }
    const u16* GetPixels() const { return pixels; }
    bool IsValid() const { return pixels != nullptr; }

private:
    int width = 0;
    int height = 0;
    int bpp = 0;
    int alphaDepth = 0;
    int storageWidth = 0;
    int storageHeight = 0;
    pddiFilterMode filterMode = PDDI_FILTER_NONE;
    u16* pixels = nullptr;
    bool dirty = true;
};

class pspContext;

class pspShader final : public pddiBaseShader {
public:
    explicit pspShader(const char* shaderType) : type(shaderType ? shaderType : "simple") {}

    const char* GetType() override { return type.c_str(); }
    void SetTexture(u32 param, pddiTexture* tex) override;
    void SetInt(u32, int) override {}
    void SetFloat(u32, float) override {}
    void SetColour(u32, pddiColour c) override { diffuse = c; }
    void SetInt(const char*, int) override {}
    void SetFloat(const char*, float) override {}
    void SetVector(const char*, float, float, float, float) override {}
    void SetMatrix(const char*, const float*) override {}
    void PreRender() override;
    void PostRender() override {}

    pddiTexture* GetTexture(u32 slot = 0) const { return slot < textures.size() ? textures[slot] : nullptr; }
    pddiColour GetDiffuse() const { return diffuse; }

private:
    std::string type;
    std::array<pddiTexture*, 4> textures{};
    pddiColour diffuse = pddiColour(255, 255, 255, 255);
};

class pspDisplay final : public pddiDisplay {
public:
    pspDisplay() = default;
    ~pspDisplay() override;

    bool InitDisplay(const pddiDisplayInit& init) override;
    void SwapBuffers() override;
    int GetWidth() override { return 480; }
    int GetHeight() override { return 272; }
    bool ShouldClose() override { return closeRequested; }
    void PollEvents() override {}

    bool IsKeyDown(int) override { return false; }
    bool IsMouseButtonDown(int) override { return false; }
    void GetMousePosition(double& x, double& y) override { x = 0.0; y = 0.0; }
    void SetIcon(int, int, const unsigned char*) override {}

    int GetVideoModeCount() override { return 1; }
    void GetVideoMode(int index, pddiVideoMode& mode) override;
    void SetFullscreen(bool) override {}
    bool IsFullscreen() override { return true; }
    void SetBorderless(bool) override {}
    bool IsBorderless() override { return true; }
    void SetResolution(int, int) override {}
    void SetVSync(bool) override {}
    void SetMSAA(int) override {}
    int GetMSAA() override { return 0; }
    void SetWindowPos(int, int) override {}
    void SetTitle(const char*) override {}
    void ShowCursor(bool) override {}
    void ClipCursor(bool) override {}
    void SetWndProc(pddiWndProc proc) override { wndProc = std::move(proc); }
    void AddOverlayCallback(OverlayCallback cb) override;
    void RenderOverlay() override;

    void RequestClose() { closeRequested = true; }

private:
    bool closeRequested = false;
    pddiWndProc wndProc;
    std::vector<OverlayCallback> overlayCallbacks;
};

class pspGamepad final : public pddiGamepad {
public:
    explicit pspGamepad(pspDisplay* display) : display(display) {}
    void Poll() override;
    bool IsConnected() const override { return connected; }
    bool IsButtonDown(int button) const override;
    float GetAxis(int axis) const override;
    bool SupportsVibration() const override { return false; }
    bool SetVibration(float, float) override { return false; }

private:
    pspDisplay* display = nullptr;
    bool connected = true;
    u32 buttons = 0;
    std::array<float, GamepadAxis::COUNT> axes{};
};

class pspContext final : public pddiRenderContext {
public:
    explicit pspContext(pspDisplay* display);
    ~pspContext() override;

    void BeginFrame() override;
    void EndFrame() override;
    void SetCameraAspect(float aspect) override { cameraAspect = aspect; }
    float GetCameraAspect() const override { return cameraAspect; }
    void SetClearColour(pddiColour c) override { clearColour = c; }
    void Clear(int flags) override;
    void SetProjectionMatrix(const Mat4& m) override { projection = m; }
    void SetViewMatrix(const Mat4& m) override { view = m; }
    void SetWorldMatrix(const Mat4& m) override { world = m; }
    const Mat4& GetWorldMatrix() const override { return world; }
    const Mat4& GetViewMatrix() const override { return view; }
    const Mat4& GetProjectionMatrix() const override { return projection; }
    void SetWorldMirror(bool enable) override { worldMirror = enable; }
    bool GetWorldMirror() const override { return worldMirror; }

    void SetCullMode(pddiCullMode mode) override;
    void EnableZBuffer(bool enable) override;
    void SetBlendMode(pddiBlendMode mode) override;
    void SetDepthClamp(bool) override {}
    void SetPolygonOffset(bool enable, f32 factor = 0.0f, f32 units = 0.0f) override;
    void SetScissor(int x, int y, int w, int h) override;
    void SetMultisampleEnabled(bool) override {}
    void ResolveForOverlayPass() override {}
    pddiRenderTarget* CreateRenderTarget(int, int, pddiRenderTargetFormat, bool = false) override { return nullptr; }
    bool SetRenderTarget(pddiRenderTarget*) override { return false; }

    void DrawFilledCircle(pddiBaseShader*, float, float, float, float, float, float, float, float, int) override;
    void DrawCircle(pddiBaseShader*, float, float, float, float, float, float, float, float, float, int) override;
    void DrawQuad(pddiBaseShader* shader, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1) override;
    void DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                       const pddiBatchVertex* verts, s32 vertCount) override;
    void DrawPrimBuffer(pddiPrimBuffer* buffer, u32 indexOffset = 0, u32 indexCount = 0) override;
    void DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                         float x1, float y1, float r1, float g1, float b1, float a1,
                         float x2, float y2, float r2, float g2, float b2, float a2,
                         float x3, float y3, float r3, float g3, float b3, float a3) override;

    void SetTexture(pddiTexture* texture) override { currentTexture = texture; }
    void SetVRAMHandle(u32 handle) override { vramHandle = handle; }
    void SetTexInfoOverride(bool enabled, u32 word) override { texInfoOverride = enabled; texInfoWord = word; }
    u32 CreateVRAMTexture(int w, int h, const u16* data) override;
    void DestroyVRAMTexture(u32 handle) override;
    void UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) override;
    void SetRealTextureMode(bool enabled) override { realTextureMode = enabled; }
    bool IsRealTextureModeEnabled() const override { return realTextureMode; }
    void SetRealTextureRect(float, float, float, float) override {}

    void SetShadowCasterPass(bool, const Mat4&) override {}
    void SetReceiveShadows(bool) override {}
    void SetShadowCascades(pddiTexture* const*, const Mat4*, const float*, const float*,
                           pddiTexture* const*, int) override {}
    void SetCameraWorldPos(float, float, float) override {}
    void SetShadowLightDirection(float, float, float) override {}
    void SetShadowCasterInstanceId(u32) override {}
    void SetShadowReceiverInstanceId(u32) override {}
    void ClearShadowCasterIdTarget() override {}
    void SetShadowDebugMode(int) override {}

    void ApplyShader(const pspShader* shader);

private:
    struct VramImage { u16* pixels = nullptr; int width = 0; int height = 0; };
    struct PageCache {
        u16* pixels = nullptr;
        u32 handle = 0;
        u16 tpage = 0;
        u16 cba = 0;
        u32 age = 0;
    };

    pspDisplay* display = nullptr;
    float cameraAspect = 480.0f / 272.0f;
    pddiColour clearColour;
    Mat4 projection;
    Mat4 view;
    Mat4 world;
    pddiTexture* currentTexture = nullptr;
    pddiTexture* shaderTexture = nullptr;
    u32 vramHandle = 0;
    bool texInfoOverride = false;
    u32 texInfoWord = 0;
    bool realTextureMode = false;
    bool worldMirror = false;
    bool zBufferEnabled = true;
    pddiCullMode cullMode = PDDI_CULL_NORMAL;
    pddiBlendMode blendMode = PDDI_BLEND_NONE;
    std::array<VramImage, 8> vramImages{};
    std::array<PageCache, 8> pageCache{};
    // Dynamic world geometry can submit more vertices than the GU display
    // list can safely reserve for a whole frame. Keep one aligned, reusable
    // staging batch in normal RAM and submit it in triangle-aligned chunks.
    alignas(16) std::array<PspVertex, 8192> expandedVertices{};
    u32 cacheAge = 1;

    void Setup3DState();
    void Setup2DState();
    void BindPage(float tpage, float cba);
    u16* GetDecodedPage(u32 handle, u16 tpage, u16 cba);
    VramImage* GetVram(u32 handle);
    void Draw2D(const PspVertex* vertices, int count, pddiTexture* texture);
};

class pspDevice final : public pddiDevice {
public:
    pddiDisplay* NewDisplay() override { display = new pspDisplay(); return display; }
    pddiRenderContext* NewRenderContext(pddiDisplay* display) override {
        return new pspContext(static_cast<pspDisplay*>(display));
    }
    pddiGamepad* NewGamepad() override { return new pspGamepad(display); }
    pddiTexture* NewTexture() override { return new pspTexture(); }
    pddiPrimBuffer* NewPrimBuffer(const pddiPrimBufferDesc& desc) override { return new pspPrimBuffer(desc); }
    pddiBaseShader* NewShader(const char* type) override { return new pspShader(type); }

private:
    pspDisplay* display = nullptr;
};

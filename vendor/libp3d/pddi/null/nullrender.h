// nullrender.h - no-op implementation of the pddi interfaces
#pragma once

#include "pddi/pddi.h"
#include "pddi/pdditex.h"
#include "pddi/pddishad.h"
#include "pddi/pddidev.h"
#include <string>

class nullTexture : public pddiTexture {
public:
    int GetWidth() override { return width; }
    int GetHeight() override { return height; }
    int GetBpp() override { return bpp; }
    int GetAlphaDepth() override { return alphaDepth; }

    void SetData(int w, int h, int bppIn, int alphaDepthIn, const void*) override {
        width = w;
        height = h;
        bpp = bppIn;
        alphaDepth = alphaDepthIn;
    }
    void SetFilterMode(pddiFilterMode) override {}
    void Bind(int) override {}

private:
    int width = 0;
    int height = 0;
    int bpp = 0;
    int alphaDepth = 0;
};

class nullShader : public pddiBaseShader {
public:
    explicit nullShader(const char* t) : type(t ? t : "simple") {}

    const char* GetType() override { return type.c_str(); }

    void SetTexture(u32, pddiTexture*) override {}
    void SetInt(u32, int) override {}
    void SetFloat(u32, float) override {}
    void SetColour(u32, pddiColour) override {}
    void SetInt(const char*, int) override {}
    void SetFloat(const char*, float) override {}
    void SetVector(const char*, float, float, float, float) override {}
    void SetMatrix(const char*, const float*) override {}
    void PreRender() override {}
    void PostRender() override {}

private:
    std::string type;
};

class nullPrimBuffer : public pddiPrimBuffer {
public:
    explicit nullPrimBuffer(const pddiPrimBufferDesc& desc)
        : primType(desc.primType), vertexCount(desc.vertexCount), indexCount(desc.indexCount) {}

    void SetVertexData(const void*, u32) override {}
    void SetIndices(const u16*, u32) override {}
    u32 GetIndexCount() const override { return indexCount; }
    u32 GetVertexCount() const override { return vertexCount; }
    pddiPrimType GetPrimType() const override { return primType; }

protected:
    ~nullPrimBuffer() override = default;

private:
    pddiPrimType primType;
    u32 vertexCount = 0;
    u32 indexCount = 0;
};

class nullRenderTarget : public pddiRenderTarget {
public:
    nullRenderTarget(int w, int h) : width(w), height(h), texture(new nullTexture()) {}
    ~nullRenderTarget() override { delete texture; }

    bool Resize(int w, int h) override { width = w; height = h; return true; }
    int GetWidth() const override { return width; }
    int GetHeight() const override { return height; }
    pddiTexture* GetTexture() const override { return texture; }
    bool IsValid() const override { return true; }

private:
    int width;
    int height;
    nullTexture* texture;
};

class nullGamepad : public pddiGamepad {
public:
    void Poll() override {}
    bool IsConnected() const override { return false; }
    bool IsButtonDown(int) const override { return false; }
    float GetAxis(int) const override { return 0.0f; }
    bool SupportsVibration() const override { return false; }
    bool SetVibration(float, float) override { return false; }
};

class nullDisplay : public pddiDisplay {
public:
    bool InitDisplay(const pddiDisplayInit& init) override;
    void SwapBuffers() override {}
    int  GetWidth() override { return width; }
    int  GetHeight() override { return height; }
    bool ShouldClose() override;
    void PollEvents() override;

    bool IsKeyDown(int) override { return false; }
    bool IsMouseButtonDown(int) override { return false; }
    void GetMousePosition(double& x, double& y) override { x = 0.0; y = 0.0; }

    void SetIcon(int, int, const unsigned char*) override {}

    int  GetVideoModeCount() override { return 0; }
    void GetVideoMode(int, pddiVideoMode&) override {}
    void SetFullscreen(bool) override {}
    bool IsFullscreen() override { return false; }
    void SetBorderless(bool) override {}
    bool IsBorderless() override { return false; }
    void SetResolution(int w, int h) override { width = w; height = h; }
    void SetVSync(bool) override {}
    void SetMSAA(int) override {}
    int  GetMSAA() override { return 0; }
    void SetWindowPos(int, int) override {}

    void SetTitle(const char*) override {}

    void ShowCursor(bool) override {}
    void ClipCursor(bool) override {}

    void SetWndProc(pddiWndProc proc) override { wndProc = proc; }

    void AddOverlayCallback(OverlayCallback) override {}
    void RenderOverlay() override {}

private:
    int width = 960;
    int height = 720;
    u64 frameCount = 0;
    // ShouldClose() has no OS window-close event to key off of; terminate
    // after this many frames so a headless run is a bounded smoke test
    // rather than an infinite spin. 0 = run until killed. Overridable via
    // the RECHAN_HEADLESS_MAX_FRAMES env var (see nullrender.cpp).
    u64 maxFrames = 600;
    pddiWndProc wndProc;
};

class nullContext : public pddiRenderContext {
public:
    void BeginFrame() override {}
    void EndFrame() override {}

    void SetCameraAspect(float aspect) override { cameraAspect = aspect; }
    float GetCameraAspect() const override { return cameraAspect; }

    void SetClearColour(pddiColour) override {}
    void Clear(int) override {}

    void SetProjectionMatrix(const Mat4& m) override { projection = m; }
    void SetViewMatrix(const Mat4& m) override { viewMatrix = m; }
    void SetWorldMatrix(const Mat4& m) override { worldMatrix = m; }
    const Mat4& GetWorldMatrix() const override { return worldMatrix; }
    const Mat4& GetViewMatrix() const override { return viewMatrix; }
    const Mat4& GetProjectionMatrix() const override { return projection; }

    void SetWorldMirror(bool enable) override { worldMirror = enable; }
    bool GetWorldMirror() const override { return worldMirror; }

    void SetCullMode(pddiCullMode) override {}
    void EnableZBuffer(bool) override {}
    void SetBlendMode(pddiBlendMode) override {}
    void SetDepthClamp(bool) override {}
    void SetPolygonOffset(bool, f32 = 0.0f, f32 = 0.0f) override {}
    void SetScissor(int, int, int, int) override {}
    void SetMultisampleEnabled(bool) override {}
    void ResolveForOverlayPass() override {}

    pddiRenderTarget* CreateRenderTarget(int width, int height,
                                          pddiRenderTargetFormat, bool = false) override {
        return new nullRenderTarget(width, height);
    }
    bool SetRenderTarget(pddiRenderTarget*) override { return true; }

    void DrawFilledCircle(pddiBaseShader*, float, float, float, float,
                          float, float, float, float, int) override {}

    void DrawCircle(pddiBaseShader*, float, float, float, float, float,
                    float, float, float, float, int) override {}

    void DrawQuad(pddiBaseShader*, float, float, float, float,
                  float, float, float, float) override {}

    void DrawQuadBatch(pddiTexture*, pddiBlendMode, const pddiBatchVertex*, s32) override {}

    void DrawPrimBuffer(pddiPrimBuffer*, u32 = 0, u32 = 0) override {}

    void DrawGouraudQuad(float, float, float, float, float, float,
                         float, float, float, float, float, float,
                         float, float, float, float, float, float,
                         float, float, float, float, float, float) override {}

    void SetTexture(pddiTexture*) override {}
    void SetVRAMHandle(u32) override {}
    void SetTexInfoOverride(bool, u32) override {}

    u32  CreateVRAMTexture(int, int, const u16*) override { return 0; }
    void DestroyVRAMTexture(u32) override {}
    void UpdateVRAMTexture(u32, int, int, const u16*) override {}
    void UpdateVRAMTextureRect(u32, int, int, int, int, int, const u16*) override {}

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

private:
    Mat4 worldMatrix;
    Mat4 viewMatrix;
    Mat4 projection;
    float cameraAspect = 1.0f;
    bool worldMirror = false;
    bool realTextureMode = false;
};

class nullDevice : public pddiDevice {
public:
    pddiDisplay* NewDisplay() override { return new nullDisplay(); }
    pddiRenderContext* NewRenderContext(pddiDisplay*) override { return new nullContext(); }
    pddiGamepad* NewGamepad() override { return new nullGamepad(); }
    pddiTexture* NewTexture() override { return new nullTexture(); }
    pddiPrimBuffer* NewPrimBuffer(const pddiPrimBufferDesc& desc) override { return new nullPrimBuffer(desc); }
    pddiBaseShader* NewShader(const char* type) override { return new nullShader(type); }
};

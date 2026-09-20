#pragma once

// Native PSP PDDI backend.  The renderer deliberately uses the fixed
// function GU instead of a desktop compatibility layer: the game submits the
// same PDDI primitives, while PSX VRAM/CLUT pages are decoded once into a
// bounded page cache in aligned main memory and the eDRAM left after the
// double framebuffer and depth buffer.

#include "pddi/pddi.h"
#include "pddi/pdditex.h"
#include "pddi/pddishad.h"
#include "pddi/pddidev.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct PspVertex {
    float u, v;
    u32 color;
    float x, y, z;
};

// PSX packet geometry is authored with byte UVs. Keep the UV lanes as native
// 16-bit GU coordinates while retaining float positions: the latter avoids
// changing the model-space scale used by the engine's 3D matrices. This is a
// 20-byte stream instead of the regular 24-byte float stream and is valid for
// page-backed PSX geometry whose UVs are integral.
struct PspPackedVertex {
    s16 u, v;
    u32 color;
    float x, y, z;
};
static_assert(sizeof(PspPackedVertex) == 20, "PSP packed vertex must match GU layout");

template <typename T, std::size_t Alignment>
struct PspAlignedAllocator {
    using value_type = T;

    PspAlignedAllocator() noexcept = default;
    template <typename U>
    PspAlignedAllocator(const PspAlignedAllocator<U, Alignment>&) noexcept {}

    T* allocate(std::size_t count) {
        if (count > static_cast<std::size_t>(-1) / sizeof(T)) throw std::bad_alloc();
        const std::size_t bytes = count * sizeof(T);
        void* memory = memalign(Alignment, bytes ? bytes : Alignment);
        if (!memory) throw std::bad_alloc();
        return static_cast<T*>(memory);
    }

    void deallocate(T* memory, std::size_t) noexcept { std::free(memory); }

    template <typename U>
    struct rebind { using other = PspAlignedAllocator<U, Alignment>; };
};

template <typename T, typename U, std::size_t Alignment>
constexpr bool operator==(const PspAlignedAllocator<T, Alignment>&,
                          const PspAlignedAllocator<U, Alignment>&) noexcept { return true; }
template <typename T, typename U, std::size_t Alignment>
constexpr bool operator!=(const PspAlignedAllocator<T, Alignment>&,
                          const PspAlignedAllocator<U, Alignment>&) noexcept { return false; }

class pspPrimBuffer final : public pddiPrimBuffer {
public:
    explicit pspPrimBuffer(const pddiPrimBufferDesc& desc);
    ~pspPrimBuffer() override;

    void SetVertexData(const void* data, u32 count) override;
    void SetIndices(const u16* indices, u32 count) override;
    u32 GetIndexCount() const override { return static_cast<u32>(indices.size()); }
    u32 GetVertexCount() const override { return static_cast<u32>(vertices.size()); }
    pddiPrimType GetPrimType() const override { return primType; }

    const PspVertex* GetVertices() const { return vertices.data(); }
    const PspPackedVertex* GetPackedVertices() const { return packedVertices.data(); }
    const u16* GetIndices() const { return indices.data(); }
    bool HasValidIndices() const { return indicesValid; }
    bool HasPackedVertices() const { return packedVerticesValid; }
    // Shared model buffers are temporarily rewritten for CPU skinning or
    // lighting and restored after their draw.  Keep those submissions in the
    // list-owned staging path from the first frame so the restore cannot
    // force a mid-frame GE fence.
    void PreferStagedIndexedVertices() { preferStagedIndexedVertices = true; }
    bool SetDynamicVertexData(const void* data, u32 count);
    bool PatchDynamicAttributes(const u8* rawData, u32 rawSize,
                                const u32* uvOffsets, const u32* colourOffsets,
                                u32 count, bool updateUv, bool updateColour);
    bool UsesStagedIndexedVertices() const;
    // Direct indexed submissions borrow the persistent vertex array until the
    // current GU list is fenced. These hooks keep mutation/destruction from
    // racing the asynchronous GE read.
    void PrepareForMutation();
    void MarkDirectSubmission();
    bool HasMaterialKey(u16 index) const {
        return index < materialValid.size() && materialValid[index] != 0;
    }
    u32 GetMaterialKey(u16 index) const {
        return index < materialKeys.size() ? materialKeys[index] : 0u;
    }

private:
    pddiPrimType primType;
    u32 vertexFormat;
    std::vector<PspVertex, PspAlignedAllocator<PspVertex, 16>> vertices;
    std::vector<PspPackedVertex, PspAlignedAllocator<PspPackedVertex, 16>> packedVertices;
    bool packedVerticesValid = false;
    std::size_t packedVerticesAllocatedBytes = 0;
    // Material lanes are supplied as float fields by the shared PDDI API,
    // but PSP texture page/CLUT identifiers are 16-bit integers. Encode them
    // once when the buffer is mutated instead of converting and validating
    // them for every triangle on the frame path.
    std::vector<u32> materialKeys;
    std::vector<u8> materialValid;
    // Native indexed GU draws can borrow this storage until the current
    // display list is fenced. Keep it aligned so the common path can avoid
    // copying every material run into sceGuGetMemory().
    std::vector<u16, PspAlignedAllocator<u16, 16>> indices;
    bool indicesValid = true;
    u32 directSubmissionSerial = 0;
    // A buffer that is mutated after a direct indexed submission cannot keep
    // borrowing its persistent vertex/index storage within the same GU list:
    // the GE may still be reading it. Keep the promotion scoped to the
    // submission generation; after the fence the persistent storage is safe
    // to borrow directly again.
    bool stagedIndexedVertices = false;
    u32 stagedIndexedSerial = 0;
    // If a buffer is mutated after a direct submission in the same list, a
    // fence is required once to protect the GE. Remember that behavior so
    // later frames stage this dynamic buffer from the start instead of
    // repeating a costly Finish/Sync in the middle of every frame.
    bool preferStagedIndexedVertices = false;

    void RevalidateIndices();
    void RebuildPackedVertices();
};

// The shared model path only knows the PDDI buffer interface.  This PSP hook
// lets it opt a temporarily mutated buffer into list-owned staging without
// exposing the backend's storage layout to game code.
void PreparePspPrimBufferForDynamicSubmission(pddiPrimBuffer* buffer);
bool UpdatePspPrimBufferDynamicVertices(pddiPrimBuffer* buffer,
                                        const void* data, u32 count);

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
    std::size_t GetAllocatedBytes() const { return allocatedBytes; }

private:
    int width = 0;
    int height = 0;
    int bpp = 0;
    int alphaDepth = 0;
    int storageWidth = 0;
    int storageHeight = 0;
    pddiFilterMode filterMode = PDDI_FILTER_NONE;
    u16* pixels = nullptr;
    std::size_t allocatedBytes = 0;
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
    void SetWorldMirror(bool enable) override {
        if (worldMirror != enable) gu3DStateReady = false;
        worldMirror = enable;
    }
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

    void SetTexture(pddiTexture* texture) override {
        currentTexture = texture;
        pageBindingValid = false;
    }
    void SetVRAMHandle(u32 handle) override {
        if (vramHandle != handle) pageBindingValid = false;
        vramHandle = handle;
    }
    void SetTexInfoOverride(bool enabled, u32 word) override {
        if (texInfoOverride != enabled || texInfoWord != word) pageBindingValid = false;
        texInfoOverride = enabled;
        texInfoWord = word;
    }
    u32 CreateVRAMTexture(int w, int h, const u16* data) override;
    void DestroyVRAMTexture(u32 handle) override;
    void UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) override;
    void UpdateVRAMTextureRegion(u32 handle, int textureW, int textureH,
                                 const u16* data, int x, int y,
                                 int w, int h) override;
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
    // Native PSX background sprite: reuse indexed pages, no RGBA heap copy.
    void DrawVRAMQuad(u32 handle, u16 tpage, u16 cba,
                      float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1, pddiColour color);

private:
    friend class pspDisplay;
    friend class pspPrimBuffer;
    friend class pspTexture;
    struct VramImage { u16* pixels = nullptr; int width = 0; int height = 0; };
    struct PageCache {
        u16* pixels = nullptr;
        void* gpuPixels = nullptr;
        u32 handle = 0;
        u16 tpage = 0;
        bool zeroKey = false;
        u32 format = 0;
        u32 age = 0;
        // Invalidation clears the key, but must retain both storage and this
        // pin until the GE has consumed the submission that references them.
        bool inFlight = false;
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
    bool pageBindingValid = false;
    bool pageTexScaleValid = false;
    bool pageTexScalePacked = false;
    u32 lastBoundHandle = 0;
    u16 lastBoundTpage = 0;
    u16 lastBoundCba = 0;
    bool lastBoundZeroKey = false;
    bool clutBindingValid = false;
    u32 lastClutHandle = 0;
    u16 lastClutCba = 0;
    u8 lastClutDepth = 0;
    bool lastClutWordZeroKey = false;
    bool lastClutBlend = false;
    bool worldMirror = false;
    bool zBufferEnabled = true;
    bool polygonOffsetEnabled = false;
    u32 polygonOffset = 0;
    pddiCullMode cullMode = PDDI_CULL_NORMAL;
    pddiBlendMode blendMode = PDDI_BLEND_NONE;
    // Setup3DState is a recovery barrier after 2D/overlay passes. Most 3D
    // buffers already leave the same state active; avoid replaying the
    // cached wrapper calls once that state is known to be valid.
    bool gu3DStateReady = false;
    std::array<VramImage, 8> vramImages{};
    // A decoded 256x256 PSX page is 128 KiB. Sixteen resident pages keep the
    // active level inside the PSP user heap while avoiding render-path
    // allocations; a larger cache starves streamed BLK geometry during load.
    static constexpr u32 kPageCacheCapacity = 16;
    std::array<PageCache, kPageCacheCapacity> pageCache{};
    // These fixed arenas belong to the current GU submission. On saturation
    // finish/sync the list before recycling; normal draws need no allocation
    // or CPU/GE fence. Both expanded and indexed geometry use this storage.
    static constexpr u32 kFrameVertexCapacity = 65536;
#if !defined(PSP_ENABLE_NATIVE_INDEXED) || !defined(PSP_INDEXED_LIST_MEMORY)
    alignas(16) std::array<PspVertex, kFrameVertexCapacity> frameVertices{};
#endif
    u32 frameVertexUsed = 0;
    // Native GU indexed draws keep the source vertex once per prim buffer and
    // submit a compact u16 index stream for each material run. The arena
    // remains submission-owned because the GE reads it asynchronously after the
    // CPU returns from sceGuDrawArray().
    static constexpr u32 kFrameIndexCapacity = 131072;
#if !defined(PSP_INDEXED_LIST_MEMORY)
    alignas(16) std::array<u16, kFrameIndexCapacity> frameIndices{};
#endif
    u32 frameIndexUsed = 0;
    // Native indexed batches use local indices so each GU submission is
    // self-contained.  The stamp table avoids clearing 65K entries for every
    // material run while keeping the remap allocation-free.
    static constexpr u32 kNativeRemapCapacity = 65536;
    std::array<u16, kNativeRemapCapacity> nativeRemap{};
    std::array<u32, kNativeRemapCapacity> nativeRemapStamp{};
    std::array<u16, kNativeRemapCapacity> nativeRemapSource{};
    u32 nativeRemapGeneration = 1;
    // CLUT uploads are also asynchronous.  A per-page palette was mutable and
    // could be changed by the next material before the GE consumed the prior
    // sceGuClutLoad command; keep every palette in a submission immutable.
    // The gameplay scene binds just under 500 material palettes per frame.
    // Keep one complete frame in immutable RAM so CLUT pressure does not
    // force a mid-frame Finish/Sync fence and stall the CPU every frame.
    static constexpr u32 kFramePaletteCapacity = 512;
    alignas(16) std::array<u16, kFramePaletteCapacity * 256> framePalettes{};
    u32 framePaletteUsed = 0;
    u32 cacheAge = 1;
    bool listOpen = false;

    void FinishList();
    void FlushList();
    void EnsureListSpace(std::size_t payloadBytes = 0);
    void EnsureDrawSpace(u32 vertexCount, u32 indexCount);
    void* AllocateListMemory(std::size_t bytes);
    void InvalidatePages(u32 handle);
    void Setup3DState();
    void Setup2DState();
    void BindPage(float tpage, float cba, bool wordZeroKey = false);
    void SetPageTextureScale(bool packedUv);
    PageCache* GetDecodedPage(u32 handle, u16 tpage);
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

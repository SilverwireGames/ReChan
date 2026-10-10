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
    // The float stream may be left stale while the GE consumes only the
    // compact stream. Call before reading/submitting GetVertices().
    void RefreshFloatVertices();
    bool HasStaleFloatVertices() const { return floatStale; }
    const PspPackedVertex* GetPackedVertices() const { return packedVertices.data(); }
    const u16* GetIndices() const { return indices.data(); }
    bool HasValidIndices() const { return indicesValid; }
    bool HasPackedVertices() const { return packedVerticesValid; }
    bool GetSingleMaterialKey(u32 start, u32 end, u32* key) const;
    // Shared model buffers are temporarily rewritten for CPU skinning or
    // lighting and restored after their draw.  Keep those submissions in the
    // list-owned staging path from the first frame so the restore cannot
    // force a mid-frame GE fence.
    void PreferStagedIndexedVertices() { preferStagedIndexedVertices = true; }
    bool SetDynamicVertexData(const void* data, u32 count,
                              bool uvIsIntegral = false);
    // Fused dynamic upload: the caller writes the compact stream in place, then
    // EndPackedUpload applies the material lanes. Together they leave the
    // buffer exactly as SetDynamicVertexData(..., uvIsIntegral = true) would on
    // its compact-only path. Begin returns nullptr when that path is not
    // available (the caller then falls back to SetDynamicVertexData).
    // exclusive: the buffer belongs to one drawable (drawn at most once per
    // frame), so its draws may keep borrowing the persistent arrays.
    PspPackedVertex* BeginPackedUpload(u32 count, bool exclusive = false);
    // A new buffer with this one's streams, indices and material state, for a
    // drawable that uploads its own vertices (null if out of memory).
    pspPrimBuffer* CloneDynamic() const;
    // laneSource != 0 promises the lanes are immutable memory identified by
    // that value: repeating the last upload's source and lanes skips the
    // per-vertex material comparison (it would find nothing to change).
    // laneKeys/laneValid, when given, are BuildPspMaterialKeys of these lanes:
    // a different source then copies them instead of re-reading the lanes.
    void EndPackedUpload(const float* tpage, u32 tpageStride,
                         const float* cba, u32 cbaStride, u32 count,
                         u32 laneSource = 0, const u32* laneKeys = nullptr,
                         const u8* laneValid = nullptr);
    bool PatchDynamicAttributes(const u8* rawData, u32 rawSize,
                                const u32* uvOffsets, const u32* colourOffsets,
                                u32 count, bool updateUv, bool updateColour,
                                bool packedOnly);
    // Rewrites only the per-vertex colour lane. colours == nullptr restores
    // the colours captured before the first patch since the last upload.
    // Patching the colours the buffer already holds changes nothing, so a
    // lit prop under steady light stays static (and replayable).
    bool PatchColours(const u32* colours, u32 count);
    bool HasPatchedColours() const { return colourPatchActive; }
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
    // Material run of the triangle starting at index position `start` (a
    // multiple of 3): its class (valid, key) and the index position where the
    // next class begins. Same boundaries as DrawPrimBuffer's per-triangle
    // scan, which otherwise re-reads indices and both material arrays for
    // every draw of an unchanged buffer. False when start is not a triangle.
    bool FindMaterialRun(u32 start, bool* valid, u32* key, u32* end);

    // A static buffer's whole draw recorded as a GE call list (see
    // pspContext::DrawRecordedPrimBuffer): the texture state and indexed
    // draws of its material runs, on the buffer's persistent arrays. Valid
    // only for the buffer content, render state and cached pages/CLUTs it
    // was built from.
    struct RecordedPage {
        u32 slot = 0;
        u32 fill = 0;
    };
    struct RecordedClut {
        u32 entry = 0;
        u32 rowGeneration = 0;
        u32 epoch = 0;
        u16 row = 0;
    };
    struct RecordedDraw {
        std::vector<u32, PspAlignedAllocator<u32, 16>> words;
        // Word-aligned copies of the runs whose index ranges start on an odd
        // u16 (the GE reads indices from word-aligned addresses).
        std::vector<u16, PspAlignedAllocator<u16, 16>> indexCopies;
        std::vector<RecordedPage> pages;
        std::vector<RecordedClut> cluts;
        u32 contentSerial = 0;
        u32 vramHandle = 0;
        u32 blendMode = 0;
        u32 submissionSerial = 0;  // display list that last called it
        u32 runs = 0;
        u32 indexCount = 0;
        bool packedUv = false;
        bool valid = false;
    };
    RecordedDraw& Recorded() { return recorded; }
    // Bumped by every mutation (PrepareForMutation).
    u32 GetContentSerial() const { return contentSerial; }
    // True when the previous draw saw the same content; records this draw.
    bool DrawnUnchanged() {
        const bool unchanged = lastDrawnContentSerial == contentSerial;
        lastDrawnContentSerial = contentSerial;
        return unchanged;
    }

private:
    u32 contentSerial = 1;
    u32 lastDrawnContentSerial = 0;
    RecordedDraw recorded;
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
    // Dynamic uploads usually keep the same tpage/CBA lanes. Remember their
    // raw float bits so unchanged lanes skip the finite/range/key conversion.
    std::vector<u64> materialRawBits;
    bool materialRawBitsValid = false;
    // Maximal runs of whole triangles (from index 0) whose first vertex has
    // the same material class; rebuilt lazily after indices or keys change.
    struct MaterialRun {
        u32 end;
        u32 key;
        bool valid;
    };
    std::vector<MaterialRun> materialRuns;
    bool materialRunsValid = false;
    void BuildMaterialRuns();
    // Applies tpage/CBA lanes (float bits, as the dynamic upload layout).
    bool UpdateDynamicMaterials(const float* tpage, u32 tpageStride,
                                const float* cba, u32 cbaStride, u32 count);
    bool packedUploadStaged = false;
    // Lanes of the last full material update by EndPackedUpload; any other
    // material writer clears packedLaneSource.
    u32 packedLaneSource = 0;
    const float* packedLaneTpage = nullptr;
    const float* packedLaneCba = nullptr;
    u32 packedLaneTpageStride = 0;
    u32 packedLaneCbaStride = 0;
    u32 packedLaneCount = 0;
    // The keys of packedLaneSource were copied from precomputed lane keys
    // (the raw bits are then stale, but the keys match the source).
    bool packedLaneKeysCopied = false;
    // True when the compact stream is current and the float stream is not.
    bool floatStale = false;
    // Colour patch state (PatchColours). Base colours are captured lazily on
    // the first patch after a full/dynamic upload invalidates them.
    std::vector<u32> baseColours;
    bool baseColoursValid = false;
    bool colourPatchActive = false;
    bool colourPatchStaledFloat = false;
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
                                        const void* data, u32 count,
                                        bool uvIsIntegral);
#if defined(PSP_PERF_TELEMETRY)
// Benchmark: writes the last presented frame as a 24-bit BMP.
bool WritePspLastFrame(const char* path);
#endif
#if defined(PSP_BENCH_COMBAT)
// Restarts the scripted combat input (benchmark scenario start).
void ResetPspBenchCombatInput();
#endif
// Fused form of UpdatePspPrimBufferDynamicVertices(..., uvIsIntegral = true)
// for callers that can produce the compact stream directly (PSP character
// skinning): write `count` vertices to the returned array, then call End with
// the tpage/CBA lanes. Begin returns nullptr when the buffer cannot take it.
PspPackedVertex* BeginPspPrimBufferPackedUpload(pddiPrimBuffer* buffer, u32 count,
                                                bool exclusive = false);
// Per-drawable copy of a shared dynamic buffer (see CloneDynamic).
pddiPrimBuffer* ClonePspDynamicPrimBuffer(pddiPrimBuffer* source);
void EndPspPrimBufferPackedUpload(pddiPrimBuffer* buffer, const float* tpage, u32 tpageStride,
                                  const float* cba, u32 cbaStride, u32 count,
                                  u32 laneSource = 0, const u32* laneKeys = nullptr,
                                  const u8* laneValid = nullptr);
// Material keys and validity of `count` vertices' tpage/CBA lanes, exactly as
// the upload paths derive them (for lanes uploaded repeatedly).
void BuildPspMaterialKeys(const float* tpage, u32 tpageStride, const float* cba, u32 cbaStride,
                          u32 count, u32* keys, u8* valid);
// GU vertex colour for PSX fixed-point channels (packet byte / 128), exactly
// as the buffer upload paths convert them.
u32 MakePspGeometryColour(float r, float g, float b);
// Temporary per-frame colour override (CPU-lit props): writes only the colour
// lane instead of re-uploading every attribute. Returns false when the buffer
// cannot take the fast path; the caller must then use SetVertexData.
bool PatchPspPrimBufferColours(pddiPrimBuffer* buffer, const u32* colours, u32 count);
// Restores the colours captured by the first PatchPspPrimBufferColours().
bool RestorePspPrimBufferColours(pddiPrimBuffer* buffer, u32 count);
// True while patched colours are in place (no restore or upload since).
bool PspPrimBufferHasPatchedColours(const pddiPrimBuffer* buffer);
// Copies the colour lane the GE reads (telemetry re-checks).
bool ReadPspPrimBufferColours(const pddiPrimBuffer* buffer, u32* colours, u32 count);
// Brackets the lit draw of a colour-patched buffer. The patch stays in place
// after it (patching the same colours again is free); any other draw of that
// buffer gets its base colours back first (pspContext::DrawPrimBuffer).
void BeginPspLitColourDraw(pddiPrimBuffer* buffer);
void EndPspLitColourDraw();

// Pause backdrop: the world is drawn once, copied (as 5650) into the depth
// buffer, which a 2D-only menu does not use, and later frames redraw only that
// image plus the menu. The backdrop survives only while every frame draws it;
// any 3D draw or a frame that skips it releases it and clears the depth.
bool PspCapturePauseBackdrop();
bool PspDrawPauseBackdrop();
void PspReleasePauseBackdrop();
bool PspHasPauseBackdrop();

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
    // Recorded GE call lists of static buffers (DrawPrimBuffer).
    bool DrawRecordedPrimBuffer(pspPrimBuffer* buffer, int indexedFormat, bool packedUv);
    bool RecordPrimBufferCallList(pspPrimBuffer* buffer, int indexedFormat, bool packedUv);
    bool RecordedDrawCurrent(pspPrimBuffer* buffer, bool packedUv) const;
    int FindCachedPageSlot(u32 handle, u16 tpage) const;
    int FindCurrentClutEntry(u16 cba, u32 depth, u8 flags, u32 row) const;
    void ForgetTextureState();
#if defined(PSP_DRAW_MICROBENCH)
    void RunDrawMicrobench();
#endif
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
        if (worldMirror != enable) {
            FlushQueuedOpaqueDraws();
            gu3DStateReady = false;
        }
        worldMirror = enable;
    }
    bool GetWorldMirror() const override { return worldMirror; }

    void SetCullMode(pddiCullMode mode) override;
    void EnableZBuffer(bool enable) override;
    void SetBlendMode(pddiBlendMode mode) override;
    void SetDepthClamp(bool) override {}
    void SetPolygonOffset(bool enable, f32 factor = 0.0f, f32 units = 0.0f) override;
    void SetScissor(int x, int y, int w, int h) override;
    bool CapturePauseBackdrop();
    bool DrawPauseBackdrop();
    void ReleasePauseBackdrop();
    bool HasPauseBackdrop() const { return pauseBackdropValid; }
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
        if (currentTexture != texture) FlushQueuedOpaqueDraws();
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
    void SetRealTextureMode(bool enabled) override {
        if (realTextureMode != enabled) FlushQueuedOpaqueDraws();
        realTextureMode = enabled;
    }
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
    void FlushQueuedOpaqueDraws();
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
        // Bumped whenever the slot is refilled or invalidated: a recorded
        // call list naming this slot is current only while it holds.
        u32 fill = 0;
    };

    struct QueuedOpaqueDraw {
        pspPrimBuffer* buffer = nullptr;
        u32 indexOffset = 0;
        u32 indexCount = 0;
        u32 materialKey = 0;
        u32 vramHandle = 0;
        bool texInfoOverride = false;
        u32 texInfoWord = 0;
        Mat4 projection{};
        Mat4 view{};
        Mat4 world{};
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
    // Setup2DState is called for every overlay batch. Once its state is in
    // place, later batches only re-emit it if a 3D pass/setter changed it.
    bool gu2DStateReady = false;
    u32 gu2DMatrixSerial = 0;
    // GU scissor last requested through SetScissor (full screen by default).
    std::array<int, 4> scissorRect{{0, 0, 480, 272}};
    // Pause backdrop state; see PspCapturePauseBackdrop().
    bool pauseBackdropValid = false;
    bool pauseBackdropUsedThisFrame = false;
    bool pendingDepthClear = false;
    std::array<VramImage, 8> vramImages{};
    // A decoded 256x256 PSX page is 128 KiB. Sixteen resident pages keep the
    // active level inside the PSP user heap while avoiding render-path
    // allocations; a larger cache starves streamed BLK geometry during load.
    static constexpr u32 kPageCacheCapacity = 16;
    std::array<PageCache, kPageCacheCapacity> pageCache{};
    // Direct-mapped hints (slot + 1, 0 = none) into pageCache and clutCache:
    // a bind usually finds its entry with one probe instead of scanning both
    // arrays, which the frame's vertex traffic keeps evicting from the data
    // cache. A hint is only a guess; the entry's own key decides.
    std::array<u8, 32> pageCacheHint{};
    std::array<u8, 64> clutCacheHint{};
#if defined(PSP_OPAQUE_RENDER_QUEUE)
    static constexpr u32 kOpaqueQueueCapacity = 256;
    std::array<QueuedOpaqueDraw, kOpaqueQueueCapacity> opaqueQueue{};
#endif
    u32 opaqueQueueCount = 0;
    bool replayingOpaqueQueue = false;
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
    // The gameplay scene binds just under 500 material palettes per frame, but
    // overlays and character material changes can cross that boundary. Keep
    // two complete frame budgets in immutable RAM so CLUT pressure does not
    // force a mid-frame Finish/Sync fence and stall the CPU.
    static constexpr u32 kFramePaletteCapacity = 1024;
    alignas(16) std::array<u16, kFramePaletteCapacity * 256> framePalettes{};
    u32 framePaletteUsed = 0;
    // Converted PSX VRAM palettes reused across binds and frames (BindPage).
    // An entry is valid while the VRAM row it was read from keeps its write
    // generation, and is only refilled when no draw of the open list uses it
    // (lastUseSerial differs from the current submission serial).
    struct ClutCacheEntry {
        u32 rowGeneration = 0;
        u32 epoch = 0;
        u32 lastUseSerial = 0;
        u32 lastUseTick = 0;
        u16 cba = 0;
        u8 depth = 0;
        u8 flags = 0;
        bool valid = false;
    };
    static constexpr u32 kClutCacheEntries = 64;
    static constexpr u32 kClutCacheNone = 0xFFFFFFFFu;
    static constexpr u32 kVramRows = 512;
    std::array<ClutCacheEntry, kClutCacheEntries> clutCache{};
    alignas(16) std::array<u16, kClutCacheEntries * 256> clutCachePalettes{};
    // Write generation per row of the PSX VRAM image (handle 1); vramEpoch
    // covers whole-image rewrites and frees.
    std::array<u32, kVramRows> vramRowGeneration{};
    u32 vramWriteCounter = 1;
    u32 vramEpoch = 1;
    u32 clutCacheClock = 0;
    // Cache entry whose palette the GE CLUT currently holds (or none).
    u32 geClutEntry = kClutCacheNone;
    u32 AcquireClutCacheEntry();
    u32 cacheAge = 1;
    bool listOpen = false;

    void FinishList();
    void FlushList();
    bool QueueOpaqueDraw(pspPrimBuffer* buffer, u32 indexOffset, u32 indexCount);
    void EnsureListSpace(std::size_t payloadBytes = 0);
    void EnsureDrawSpace(u32 vertexCount, u32 indexCount);
    void* AllocateListMemory(std::size_t bytes);
    void InvalidatePages(u32 handle);
    void Setup3DState();
    void Setup2DState();
    void ClearDepthNow();
    void DrawScreenCopy(u32 sourceFormat, const void* sourceTexture);
    void BindPage(float tpage, float cba, bool wordZeroKey = false,
                  bool packedUv = false);
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

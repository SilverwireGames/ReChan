#include "pddi/gles/glesrender.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(RC_PLATFORM_SWITCH)
static void LogGles(const char* fmt, ...) {
    FILE* f = std::fopen("sdmc:/switch/rechan/gles_debug.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}
#else
static void LogGles(const char*, ...) {}
#endif

// GLSL ES 3.00 ports of pddi/gl/glrender.cpp's shader set.

// Prepended to every application-supplied shader body. Unlike desktop GL, none
// of the optional feature blocks are enabled: cascade sampling and an integer
// fragment output both crash the Eden/Suyu shader recompiler, and shadows are
// force-disabled on Switch anyway. See pddi/pddishadersrc.h for the contract.
//
// NOPERSP stays empty. GLES has no core "noperspective" and Eden's recompiler
// rejects GL_NV_shader_noperspective_interpolation outright (the scene goes
// black), so UVs are perspective-correct here rather than PSX-affine. Tried and
// rejected; do not reintroduce.
static const char* kGLESPreamble =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "#define NOPERSP\n"
    "#define HIGHP highp\n"
    "#define VRAM_FLOAT 1\n";

static u32 CompileGLESShader(u32 type, const char* src, const char* programName, const char* stageName) {
    if (!src) {
        std::fprintf(stderr, "GLSL ES missing source: program=%s stage=%s\n", programName, stageName);
        LogGles("GLSL ES missing source: program=%s stage=%s", programName, stageName);
        return 0;
    }

    const char* sources[2] = { kGLESPreamble, src };
    u32 shader = glCreateShader(type);
    glShaderSource(shader, 2, sources, nullptr);
    glCompileShader(shader);

    int ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "GLSL ES compile error: program=%s stage=%s\n%s\n", programName, stageName, log);
        LogGles("GLSL ES compile error: program=%s stage=%s\n%s", programName, stageName, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Builds a program entirely from application-supplied GLSL. Returns 0 if the
// application has no source for programName, which is a configuration error
// rather than something to paper over with a fallback.
static u32 LinkGLESProgram(const char* programName) {
    if (!pddiShaderSource) {
        std::fprintf(stderr, "GLSL ES no source provider installed (program=%s)\n", programName);
        LogGles("GLSL ES no source provider installed (program=%s)", programName);
        return 0;
    }

    u32 vs = CompileGLESShader(GL_VERTEX_SHADER,
                               pddiShaderSource(programName, PDDI_SHADER_STAGE_VERTEX),
                               programName, "vertex");
    u32 fs = CompileGLESShader(GL_FRAGMENT_SHADER,
                               pddiShaderSource(programName, PDDI_SHADER_STAGE_FRAGMENT),
                               programName, "fragment");
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    u32 program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    int ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "GLSL ES link error: program=%s\n%s\n", programName, log);
        LogGles("GLSL ES link error: program=%s\n%s", programName, log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

static bool IsDepthRenderTargetFormat(pddiRenderTargetFormat format) {
    return format == PDDI_RENDER_TARGET_DEPTH24 ||
           format == PDDI_RENDER_TARGET_DEPTH32F ||
           format == PDDI_RENDER_TARGET_DEPTH;
}

static void ApplyTextureFilterMode(u32 handle, pddiFilterMode mode) {
    if (!handle) {
        return;
    }

    GLint minFilter = GL_NEAREST;
    GLint magFilter = GL_NEAREST;
    if (mode == PDDI_FILTER_BILINEAR || mode == PDDI_FILTER_TRILINEAR) {
        minFilter = GL_LINEAR;
        magFilter = GL_LINEAR;
    }

    glBindTexture(GL_TEXTURE_2D, handle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static pddiTexture* s_defaultWhiteTexture = nullptr;
static s32 s_defaultWhiteTextureUsers = 0;

static pddiTexture* GetDefaultWhiteTexture() {
    if (s_defaultWhiteTexture) {
        return s_defaultWhiteTexture;
    }

    glesTexture* tex = new glesTexture();
    const u8 whitePixel[4] = { 255, 255, 255, 255 };
    tex->SetData(1, 1, 32, 8, whitePixel);
    s_defaultWhiteTexture = tex;
    return s_defaultWhiteTexture;
}

static void ReleaseDefaultWhiteTexture() {
    if (s_defaultWhiteTexture) {
        s_defaultWhiteTexture->Release();
        s_defaultWhiteTexture = nullptr;
    }
}

// glesTexture

glesTexture::glesTexture() = default;

glesTexture::~glesTexture() {
    if (handle)
        glDeleteTextures(1, &handle);
}

void glesTexture::SetData(int w, int h, int b, int a, const void* rgba) {
    width = w;
    height = h;
    bpp = b;
    alphaDepth = a;

    if (handle)
        glDeleteTextures(1, &handle);

    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    ApplyTextureFilterMode(handle, filterMode);
}

void glesTexture::SetFilterMode(pddiFilterMode mode) {
    filterMode = mode;
    ApplyTextureFilterMode(handle, filterMode);
}

void glesTexture::Bind(int unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, handle);
}

bool glesTexture::SetRenderTargetStorage(int w, int h, pddiRenderTargetFormat format) {
    if (w <= 0 || h <= 0) return false;
    width = w;
    height = h;
    if (IsDepthRenderTargetFormat(format)) {
        const bool useFloatDepth = format != PDDI_RENDER_TARGET_DEPTH24;
        bpp = useFloatDepth ? 32 : 24;
        alphaDepth = 0;
        if (handle) glDeleteTextures(1, &handle);
        glGenTextures(1, &handle);
        glBindTexture(GL_TEXTURE_2D, handle);
        const GLint internalFormat = useFloatDepth ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24;
        const GLenum uploadType = useFloatDepth ? GL_FLOAT : GL_UNSIGNED_INT;
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0,
                     GL_DEPTH_COMPONENT, uploadType, nullptr);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        glBindTexture(GL_TEXTURE_2D, 0);
        filterMode = PDDI_FILTER_NONE;
        return handle != 0;
    }

    bpp = (format == PDDI_RENDER_TARGET_RGBA16F) ? 64 : 32;
    alphaDepth = (format == PDDI_RENDER_TARGET_RGBA16F) ? 16 : 8;
    if (handle) glDeleteTextures(1, &handle);
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    const GLint internalFormat = (format == PDDI_RENDER_TARGET_RGBA16F) ? GL_RGBA16F : GL_RGBA8;
    const GLenum dataType = (format == PDDI_RENDER_TARGET_RGBA16F) ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, GL_RGBA, dataType, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    filterMode = PDDI_FILTER_BILINEAR;
    ApplyTextureFilterMode(handle, filterMode);
    return handle != 0;
}

bool glesTexture::SetIdTargetStorage(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    width = w;
    height = h;
    bpp = 32;
    alphaDepth = 8;
    if (handle) glDeleteTextures(1, &handle);
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    filterMode = PDDI_FILTER_NONE;
    return handle != 0;
}

// glesShader

glesShader::glesShader(const char* shaderType) : type(shaderType ? shaderType : "simple") {
    for (s32 i = 0; i < kMaxTextureSlots; i++) {
        texSlots[i] = nullptr;
    }
    CreateProgram();
}

glesShader::~glesShader() {
    if (program)
        glDeleteProgram(program);
}

void glesShader::CreateProgram() {
    // The shader type string is the program name the application registers.
    program = LinkGLESProgram(type.c_str());
    LogGles("glesShader(type=%s): program=%u", type.c_str(), program);
}

void glesShader::SetTexture(u32 param, pddiTexture* t) {
    s32 slot = 0;
    if (param < (u32)kMaxTextureSlots) {
        slot = (s32)param;
    }
    texSlots[slot] = t;
}
void glesShader::SetInt(u32, int) {}
void glesShader::SetFloat(u32, float) {}
void glesShader::SetColour(u32, pddiColour c) { diffuse = c; }

void glesShader::SetInt(const char* param, int value) {
    if (param) intParams[param] = value;
}

void glesShader::SetFloat(const char* param, float value) {
    if (param) floatParams[param] = value;
}

void glesShader::SetVector(const char* param, float x, float y, float z, float w) {
    if (param) vectorParams[param] = { x, y, z, w };
}

void glesShader::SetMatrix(const char* param, const float* matrix4x4) {
    if (!param || !matrix4x4) return;
    std::array<float, 16>& value = matrixParams[param];
    std::memcpy(value.data(), matrix4x4, sizeof(float) * value.size());
}

int glesShader::FindUniform(const std::string& name) {
    const auto existing = uniformLocations.find(name);
    if (existing != uniformLocations.end()) {
        return existing->second;
    }
    const int location = glGetUniformLocation(program, name.c_str());
    uniformLocations.emplace(name, location);
    return location;
}

void glesShader::PreRender() {
    glUseProgram(program);

    pddiTexture* fallback = GetDefaultWhiteTexture();
    pddiTexture* slot0 = texSlots[0] ? texSlots[0] : fallback;
    if (slot0) {
        slot0->Bind(0);
        int baseSamplerLoc = FindUniform("uTex");
        if (baseSamplerLoc >= 0) {
            glUniform1i(baseSamplerLoc, 0);
        }
    }

    for (s32 slot = 1; slot < kMaxTextureSlots; slot++) {
        if (!texSlots[slot]) {
            continue;
        }
        texSlots[slot]->Bind(slot);
        char samplerName[32];
        std::snprintf(samplerName, sizeof(samplerName), "uTex%d", slot);
        int samplerLoc = FindUniform(samplerName);
        if (samplerLoc >= 0) {
            glUniform1i(samplerLoc, slot);
        }
    }

    int tintLoc = FindUniform("uTint");
    if (tintLoc >= 0) {
        glUniform4f(tintLoc,
                    diffuse.r / 255.0f, diffuse.g / 255.0f,
                    diffuse.b / 255.0f, diffuse.a / 255.0f);
    }

    for (const auto& [name, value] : intParams) {
        int location = FindUniform(name);
        if (location >= 0) glUniform1i(location, value);
    }
    for (const auto& [name, value] : floatParams) {
        int location = FindUniform(name);
        if (location >= 0) glUniform1f(location, value);
    }
    for (const auto& [name, value] : vectorParams) {
        int location = FindUniform(name);
        if (location >= 0) glUniform4fv(location, 1, value.data());
    }
    for (const auto& [name, value] : matrixParams) {
        int location = FindUniform(name);
        if (location >= 0) glUniformMatrix4fv(location, 1, GL_FALSE, value.data());
    }
}

void glesShader::PostRender() {
    glUseProgram(0);
}

// glesPrimBuffer

// VAO/VBO/EBO recycling.
//
// The effect, trail and shadow paths build their geometry by creating a
// pddiPrimBuffer per triangle/quad, drawing it and releasing it immediately, so
// a busy frame runs hundreds of glGen*/glDelete* pairs. Handing the GL objects
// back to a free list instead makes that churn disappear; the vertex layout is
// baked into the VAO, so buffers are only reused for a matching format.
struct glesPrimBufferPoolEntry {
    u32 vertexFormat;
    u32 vao, vbo, ebo;
    u32 vertexCapacity, indexCapacity;
};
static std::vector<glesPrimBufferPoolEntry> s_primBufferPool;

glesPrimBuffer::glesPrimBuffer(const pddiPrimBufferDesc& desc)
    : primType(desc.primType), vertexFormat(desc.vertexFormat)
    , vertexCount(desc.vertexCount), indexCount(desc.indexCount) {
    stride = 0;
    if (vertexFormat & PDDI_V_POSITION) stride += 3 * sizeof(f32);
    if (vertexFormat & PDDI_V_COLOUR)   stride += 3 * sizeof(f32);
    if (vertexFormat & PDDI_V_UV)       stride += 2 * sizeof(f32);
    if (vertexFormat & PDDI_V_TEXINFO)  stride += 2 * sizeof(f32);

    for (size_t i = 0; i < s_primBufferPool.size(); i++) {
        if (s_primBufferPool[i].vertexFormat != vertexFormat) {
            continue;
        }
        const glesPrimBufferPoolEntry& entry = s_primBufferPool[i];
        vao = entry.vao;
        vbo = entry.vbo;
        ebo = entry.ebo;
        vertexCapacity = entry.vertexCapacity;
        indexCapacity = entry.indexCapacity;
        s_primBufferPool[i] = s_primBufferPool.back();
        s_primBufferPool.pop_back();
        // The GPU may still be reading the storage from the draw that used
        // this buffer last, so the first write orphans it (glBufferData with
        // fresh storage) instead of patching it in place with glBufferSubData
        // and forcing the driver to sync.
        orphanOnNextWrite = true;
        return;
    }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, stride * desc.vertexCount, nullptr, GL_STATIC_DRAW);

    SetupVertexAttribs();

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, desc.indexCount * sizeof(u16), nullptr, GL_STATIC_DRAW);

    vertexCapacity = desc.vertexCount;
    indexCapacity = desc.indexCount;

    glBindVertexArray(0);
}

glesPrimBuffer::~glesPrimBuffer() {
    if (!vao) {
        return;
    }
    // Cap the pool so a one-off spike doesn't pin GL objects forever.
    if (s_primBufferPool.size() < 256) {
        s_primBufferPool.push_back({ vertexFormat, vao, vbo, ebo, vertexCapacity, indexCapacity });
        return;
    }
    if (ebo) glDeleteBuffers(1, &ebo);
    if (vbo) glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

void glesPrimBuffer::SetVertexData(const void* data, u32 count) {
    vertexCount = count;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    if (count <= vertexCapacity && !orphanOnNextWrite) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, stride * count, data);
        return;
    }
    const u32 capacity = (count > vertexCapacity) ? count : vertexCapacity;
    glBufferData(GL_ARRAY_BUFFER, stride * capacity, nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, stride * count, data);
    vertexCapacity = capacity;
}

void glesPrimBuffer::SetIndices(const u16* indices, u32 count) {
    indexCount = count;
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    if (count <= indexCapacity && !orphanOnNextWrite) {
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, count * sizeof(u16), indices);
    }
    else {
        const u32 capacity = (count > indexCapacity) ? count : indexCapacity;
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, capacity * sizeof(u16), nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, count * sizeof(u16), indices);
        indexCapacity = capacity;
    }
    glBindVertexArray(0);
    // Callers always fill vertices then indices, so clearing here covers both.
    orphanOnNextWrite = false;
}

void glesPrimBuffer::SetupVertexAttribs() {
    u32 offset = 0;
    u32 loc = 0;

    if (vertexFormat & PDDI_V_POSITION) {
        glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, stride, (void*)(uintptr_t)offset);
        glEnableVertexAttribArray(loc);
        offset += 3 * sizeof(f32);
        loc++;
    }
    if (vertexFormat & PDDI_V_COLOUR) {
        glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, stride, (void*)(uintptr_t)offset);
        glEnableVertexAttribArray(loc);
        offset += 3 * sizeof(f32);
        loc++;
    }
    if (vertexFormat & PDDI_V_UV) {
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, stride, (void*)(uintptr_t)offset);
        glEnableVertexAttribArray(loc);
        offset += 2 * sizeof(f32);
        loc++;
    }
    if (vertexFormat & PDDI_V_TEXINFO) {
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, stride, (void*)(uintptr_t)offset);
        glEnableVertexAttribArray(loc);
        offset += 2 * sizeof(f32);
        loc++;
    }
}

// glesRenderTarget

glesRenderTarget::glesRenderTarget(int w, int h, pddiRenderTargetFormat targetFormat, bool withInstanceId)
    : wantsIdAttachment(withInstanceId), format(targetFormat) {
    Resize(w, h);
}

glesRenderTarget::~glesRenderTarget() {
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    if (texture) texture->Release();
    if (idTexture) idTexture->Release();
}

bool glesRenderTarget::Resize(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    const bool needsId = wantsIdAttachment && IsDepthRenderTargetFormat(format);
    if (valid && framebuffer && texture && (!needsId || idTexture) && width == w && height == h) return true;
    valid = false;
    if (!texture) texture = new glesTexture();
    if (!texture->SetRenderTargetStorage(w, h, format)) return false;
    if (needsId) {
        if (!idTexture) idTexture = new glesTexture();
        if (!idTexture->SetIdTargetStorage(w, h)) return false;
    }
    if (!framebuffer) glGenFramebuffers(1, &framebuffer);

    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    if (IsDepthRenderTargetFormat(format)) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               texture->GetGLHandle(), 0);
        if (needsId) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   idTexture->GetGLHandle(), 0);
            static const GLenum kColorDrawBuffer[1] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, kColorDrawBuffer);
            glReadBuffer(GL_NONE);
        }
        else {
            // GLES has no glDrawBuffer(GLenum) (that's desktop-GL-only) --
            // select "no colour attachment" via glDrawBuffers(1, {GL_NONE}).
            static const GLenum kNoDrawBuffer[1] = { GL_NONE };
            glDrawBuffers(1, kNoDrawBuffer);
            glReadBuffer(GL_NONE);
        }
    }
    else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               texture->GetGLHandle(), 0);
    }
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
    if (!complete) {
        std::fprintf(stderr, "GLES: texture render target incomplete (%dx%d, format=%d)\n",
                     w, h, (int)format);
        LogGles("glesRenderTarget: incomplete (%dx%d, format=%d, withInstanceId=%d)",
                w, h, (int)format, wantsIdAttachment ? 1 : 0);
        return false;
    }
    LogGles("glesRenderTarget: OK (%dx%d, format=%d, withInstanceId=%d)",
            w, h, (int)format, wantsIdAttachment ? 1 : 0);
    width = w;
    height = h;
    valid = true;
    return true;
}

// glesDisplay

static char s_glesInitError[160] = "";

const char* GetLastGlesInitError() {
    return s_glesInitError;
}

glesDisplay::glesDisplay() = default;

glesDisplay::~glesDisplay() {
    EGLDisplay dpy = static_cast<EGLDisplay>(eglDisplay_);
    if (!dpy) {
        return;
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (eglContext_) {
        eglDestroyContext(dpy, static_cast<EGLContext>(eglContext_));
        eglContext_ = nullptr;
    }
    if (eglSurface_) {
        eglDestroySurface(dpy, static_cast<EGLSurface>(eglSurface_));
        eglSurface_ = nullptr;
    }
    eglTerminate(dpy);
    eglDisplay_ = nullptr;
}

bool glesDisplay::InitDisplay(const pddiDisplayInit& init) {
    width = init.xSize;
    height = init.ySize;

    s_glesInitError[0] = '\0';

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!dpy) {
        std::snprintf(s_glesInitError, sizeof(s_glesInitError),
                      "eglGetDisplay failed (0x%04x)", eglGetError());
        return false;
    }
    if (!eglInitialize(dpy, nullptr, nullptr)) {
        std::snprintf(s_glesInitError, sizeof(s_glesInitError),
                      "eglInitialize failed (0x%04x)", eglGetError());
        return false;
    }

    static const EGLint kConfigAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,     8,
        EGL_GREEN_SIZE,   8,
        EGL_BLUE_SIZE,    8,
        EGL_ALPHA_SIZE,   8,
        EGL_DEPTH_SIZE,   24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint numConfigs = 0;
    eglChooseConfig(dpy, kConfigAttribs, &config, 1, &numConfigs);
    if (numConfigs == 0) {
        std::snprintf(s_glesInitError, sizeof(s_glesInitError),
                      "eglChooseConfig: 0 configs (0x%04x)", eglGetError());
        eglTerminate(dpy);
        return false;
    }


    EGLSurface surface = eglCreateWindowSurface(dpy, config, nwindowGetDefault(), nullptr);
    if (!surface) {
        std::snprintf(s_glesInitError, sizeof(s_glesInitError),
                      "eglCreateWindowSurface failed (0x%04x)", eglGetError());
        eglTerminate(dpy);
        return false;
    }

    static const EGLint kContextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    EGLContext context = eglCreateContext(dpy, config, EGL_NO_CONTEXT, kContextAttribs);
    if (!context) {
        std::snprintf(s_glesInitError, sizeof(s_glesInitError),
                      "eglCreateContext failed (0x%04x)", eglGetError());
        eglDestroySurface(dpy, surface);
        eglTerminate(dpy);
        return false;
    }

    if (!eglMakeCurrent(dpy, surface, surface, context)) {
        std::snprintf(s_glesInitError, sizeof(s_glesInitError),
                      "eglMakeCurrent failed (0x%04x)", eglGetError());
        eglDestroyContext(dpy, context);
        eglDestroySurface(dpy, surface);
        eglTerminate(dpy);
        return false;
    }
    eglSwapInterval(dpy, init.vsync ? 1 : 0);

    // One-shot capability dump. The depth/stencil sizes are what the driver
    // actually handed back, not what was asked for: a 16-bit depth buffer
    // behind a 24-bit request would explain z-fighting the desktop never sees.
    EGLint cfgDepth = 0, cfgStencil = 0, cfgRed = 0, cfgAlpha = 0, cfgSamples = 0;
    eglGetConfigAttrib(dpy, config, EGL_DEPTH_SIZE, &cfgDepth);
    eglGetConfigAttrib(dpy, config, EGL_STENCIL_SIZE, &cfgStencil);
    eglGetConfigAttrib(dpy, config, EGL_RED_SIZE, &cfgRed);
    eglGetConfigAttrib(dpy, config, EGL_ALPHA_SIZE, &cfgAlpha);
    eglGetConfigAttrib(dpy, config, EGL_SAMPLES, &cfgSamples);
    GLint depthBits = 0, stencilBits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depthBits);
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    LogGles("GL vendor=%s renderer=%s", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    LogGles("GL version=%s glsl=%s", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    LogGles("EGL config: red=%d alpha=%d depth=%d stencil=%d samples=%d",
            cfgRed, cfgAlpha, cfgDepth, cfgStencil, cfgSamples);
    LogGles("Default framebuffer: depth=%d stencil=%d bits", depthBits, stencilBits);
    LogGles("Surface requested %dx%d", init.xSize, init.ySize);

    eglDisplay_ = dpy;
    eglSurface_ = surface;
    eglContext_ = context;
    return true;
}

void glesDisplay::SwapBuffers() {
    eglSwapBuffers(static_cast<EGLDisplay>(eglDisplay_), static_cast<EGLSurface>(eglSurface_));
}

void glesDisplay::SetVSync(bool enabled) {
    if (!eglDisplay_) return;
    eglSwapInterval(static_cast<EGLDisplay>(eglDisplay_), enabled ? 1 : 0);
}

void glesDisplay::PollEvents() {
    shouldClose = !appletMainLoop();
}

bool glesDisplay::ShouldClose() {
    return shouldClose;
}

// glesContext

glesContext::glesContext(glesDisplay* disp) : display(disp) {
    // Pooled VAO/VBO/EBO names belong to the GL context that made them, so a
    // fresh context must not inherit any.
    s_primBufferPool.clear();
    static int totalContexts = 0;
    totalContexts++;
    LogGles("glesContext: constructing (#%d this run)", totalContexts);
    GetDefaultWhiteTexture();
    s_defaultWhiteTextureUsers++;
    // No shadowCompareSampler (unlike desktop): the GLES preamble leaves
    // HAS_SHADOW_CASCADES undefined, so the mesh shader compiles without any
    // sampler2DShadow use, which the recompiler rejects.
    InitQuadMesh();
    LogGles("glesContext: InitQuadMesh done");
    InitGouraudMesh();
    LogGles("glesContext: InitGouraudMesh done");
    InitBatchMesh();
    LogGles("glesContext: InitBatchMesh done");
    Init3DShader();
    InitShadowDepthShader();
    LogGles("glesContext: constructed");
}

glesContext::~glesContext() {
    // The context owns the pooled GL names; they die with it.
    s_primBufferPool.clear();
    if (quadVBO) glDeleteBuffers(1, &quadVBO);
    if (quadVAO) glDeleteVertexArrays(1, &quadVAO);
    if (gouraudVBO) glDeleteBuffers(1, &gouraudVBO);
    if (gouraudVAO) glDeleteVertexArrays(1, &gouraudVAO);
    if (gouraudProgram) glDeleteProgram(gouraudProgram);
    if (batchVBO) glDeleteBuffers(1, &batchVBO);
    if (batchVAO) glDeleteVertexArrays(1, &batchVAO);
    if (batchProgram) glDeleteProgram(batchProgram);
    if (program3D) glDeleteProgram(program3D);
    if (shadowDepthProgram) glDeleteProgram(shadowDepthProgram);

    if (s_defaultWhiteTextureUsers > 0) {
        s_defaultWhiteTextureUsers--;
    }
    if (s_defaultWhiteTextureUsers == 0) {
        ReleaseDefaultWhiteTexture();
    }
}

void glesContext::BeginFrame() {
    int vx, vy, vw, vh;
    display->GetViewport(vx, vy, vw, vh);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(vx, vy, vw, vh);
    glScissor(vx, vy, vw, vh);
    glEnable(GL_SCISSOR_TEST);
    stateDirty = true;
    activeRenderTarget = nullptr;
}

void glesContext::EndFrame() {
    glDisable(GL_SCISSOR_TEST);
    glFlush();
}

void glesContext::SetClearColour(pddiColour c) { clearColour = c; }

void glesContext::Clear(int flags) {
    GLbitfield mask = 0;
    if (flags & PDDI_BUFFER_COLOUR) {
        glClearColor(clearColour.r / 255.0f, clearColour.g / 255.0f,
                     clearColour.b / 255.0f, clearColour.a / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & PDDI_BUFFER_DEPTH) {
        glClearDepthf(0.0f);
        // glClear obeys the depth write mask. A blended draw (HUD, effects)
        // at the end of the previous frame leaves it off, which would skip
        // the clear and leave last frame's depth in place. stateDirty makes
        // the next SetBlendMode restore the mask this blend mode wants.
        glDepthMask(GL_TRUE);
        stateDirty = true;
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask)
        glClear(mask);
}

void glesContext::SetCullMode(pddiCullMode mode) {
    if (!stateDirty && mode == cachedCullMode)
        return;
    cachedCullMode = mode;

    // PSX uses CW winding (left-handed). X-flip in projection preserves CW.
    glFrontFace(GL_CW);

    pddiCullMode effectiveMode = mode;
    if (worldMirror) {
        if (mode == PDDI_CULL_NORMAL) effectiveMode = PDDI_CULL_INVERTED;
        else if (mode == PDDI_CULL_INVERTED) effectiveMode = PDDI_CULL_NORMAL;
    }

    switch (effectiveMode) {
        case PDDI_CULL_NONE:
            glDisable(GL_CULL_FACE);
            break;
        case PDDI_CULL_NORMAL:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            break;
        case PDDI_CULL_INVERTED:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            break;
    }
}

void glesContext::EnableZBuffer(bool enable) {
    if (!stateDirty && enable == cachedZBuffer)
        return;
    cachedZBuffer = enable;
    if (enable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
    }
    else {
        glDisable(GL_DEPTH_TEST);
    }
}

void glesContext::SetPolygonOffset(bool enable, f32 factor, f32 units) {
    polyOffsetOverride = enable;
    polyOffsetFactor = factor;
    polyOffsetUnits = units;
    if (enable) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(factor, units);
    }
    else {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
    }
    stateDirty = true;
}

void glesContext::SetDepthClamp(bool enable) {
    if (cachedDepthClamp == enable) {
        return;
    }
    cachedDepthClamp = enable;

#ifdef GL_DEPTH_CLAMP
    if (enable) {
        glEnable(GL_DEPTH_CLAMP);
    }
    else {
        glDisable(GL_DEPTH_CLAMP);
    }
#endif
}

void glesContext::SetBlendMode(pddiBlendMode mode) {
    if (!stateDirty && mode == cachedBlendMode)
        return;

    cachedBlendMode = mode;
    switch (mode) {
        case PDDI_BLEND_NONE:
            glDisable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glDepthMask(GL_TRUE);
            if (polyOffsetOverride) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(polyOffsetFactor, polyOffsetUnits);
            }
            else {
                glDisable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(0.0f, 0.0f);
            }
            break;
        case PDDI_BLEND_ALPHA:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBlendEquation(GL_FUNC_ADD);
            glDepthMask(GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(polyOffsetOverride ? polyOffsetFactor : 1.0f,
                            polyOffsetOverride ? polyOffsetUnits : 1.0f);
            break;
        case PDDI_BLEND_ADD:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            glDepthMask(GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(polyOffsetOverride ? polyOffsetFactor : 1.0f,
                            polyOffsetOverride ? polyOffsetUnits : 1.0f);
            break;
        case PDDI_BLEND_SUBTRACT:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glDepthMask(GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(polyOffsetOverride ? polyOffsetFactor : 1.0f,
                            polyOffsetOverride ? polyOffsetUnits : 1.0f);
            break;
        case PDDI_BLEND_PSX_QUARTER:
            glEnable(GL_BLEND);
            glBlendColor(0.0f, 0.0f, 0.0f, 0.25f);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            glDepthMask(GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(polyOffsetOverride ? polyOffsetFactor : 1.0f,
                            polyOffsetOverride ? polyOffsetUnits : 1.0f);
            break;
    }
    stateDirty = false;
}

void glesContext::SetScissor(int x, int y, int w, int h) {
    glScissor(x, y, w, h);
}

pddiRenderTarget* glesContext::CreateRenderTarget(int width, int height,
                                                   pddiRenderTargetFormat format,
                                                   bool withInstanceId) {
    LogGles("CreateRenderTarget: requested %dx%d format=%d withInstanceId=%d",
            width, height, (int)format, withInstanceId ? 1 : 0);
    glesRenderTarget* target = new glesRenderTarget(width, height, format, withInstanceId);
    if (!target->IsValid()) {
        LogGles("CreateRenderTarget: FAILED, returning nullptr");
        target->Release();
        return nullptr;
    }
    return target;
}

bool glesContext::SetRenderTarget(pddiRenderTarget* target) {
    glesRenderTarget* glTarget = dynamic_cast<glesRenderTarget*>(target);
    if (target && (!glTarget || !glTarget->IsValid())) return false;

    if (glTarget && !activeRenderTarget) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFramebuffer);
        glGetIntegerv(GL_VIEWPORT, savedViewport);
        glGetIntegerv(GL_SCISSOR_BOX, savedScissor);
        savedScissorEnabled = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    }

    if (glTarget) {
        glBindFramebuffer(GL_FRAMEBUFFER, glTarget->GetFramebuffer());
        glViewport(0, 0, glTarget->GetWidth(), glTarget->GetHeight());
        glScissor(0, 0, glTarget->GetWidth(), glTarget->GetHeight());
        glEnable(GL_SCISSOR_TEST);
        activeRenderTarget = glTarget;
    }
    else if (activeRenderTarget) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFramebuffer);
        glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
        glScissor(savedScissor[0], savedScissor[1], savedScissor[2], savedScissor[3]);
        if (!savedScissorEnabled) glDisable(GL_SCISSOR_TEST);
        activeRenderTarget = nullptr;
    }
    stateDirty = true;
    return true;
}

void glesContext::ClearShadowCasterIdTarget() {
    if (!activeRenderTarget || !activeRenderTarget->GetIdTexture()) {
        return;
    }

    static const GLfloat kZeroId[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glClearBufferfv(GL_COLOR, 0, kZeroId);
}

void glesContext::DrawFilledCircle(pddiBaseShader* shader,
                                   float centerX, float centerY,
                                   float radiusX, float radiusY,
                                   float u0, float v0, float u1, float v1,
                                   int segments) {
    if (!shader)
        return;

    if (segments < 3) segments = 3;
    if (segments > 64) segments = 64;

    shader->SetMatrix("uProj", projection.Data());
    shader->PreRender();

    constexpr float PI2 = 6.28318530718f;

    float verts[64 * 3 * 4];
    int out = 0;

    const float uvCenterX = (u0 + u1) * 0.5f;
    const float uvCenterY = (v0 + v1) * 0.5f;
    const float uvRadiusX = (u1 - u0) * 0.5f;
    const float uvRadiusY = (v1 - v0) * 0.5f;

    for (int i = 0; i < segments; ++i) {
        const float a0 = ((float)i / (float)segments) * PI2;
        const float a1 = ((float)(i + 1) / (float)segments) * PI2;

        const float c0 = std::cos(a0);
        const float s0 = std::sin(a0);
        const float c1 = std::cos(a1);
        const float s1 = std::sin(a1);

        const float x0 = centerX;
        const float y0 = centerY;

        const float x1p = centerX + c0 * radiusX;
        const float y1p = centerY + s0 * radiusY;

        const float x2p = centerX + c1 * radiusX;
        const float y2p = centerY + s1 * radiusY;

        const float tu0 = uvCenterX;
        const float tv0 = uvCenterY;

        const float tu1 = uvCenterX + c0 * uvRadiusX;
        const float tv1 = uvCenterY + s0 * uvRadiusY;

        const float tu2 = uvCenterX + c1 * uvRadiusX;
        const float tv2 = uvCenterY + s1 * uvRadiusY;

        verts[out++] = x0; verts[out++] = y0; verts[out++] = tu0; verts[out++] = tv0;
        verts[out++] = x1p; verts[out++] = y1p; verts[out++] = tu1; verts[out++] = tv1;
        verts[out++] = x2p; verts[out++] = y2p; verts[out++] = tu2; verts[out++] = tv2;
    }

    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);

    const GLsizeiptr sizeBytes = (GLsizeiptr)(out * sizeof(float));
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeBytes, verts);

    glDrawArrays(GL_TRIANGLES, 0, segments * 3);

    shader->PostRender();
}

void glesContext::DrawCircle(pddiBaseShader* shader,
                             float centerX, float centerY,
                             float radiusX, float radiusY,
                             float thickness,
                             float u0, float v0, float u1, float v1,
                             int segments) {
    if (!shader)
        return;

    if (segments < 3) segments = 3;
    if (segments > 64) segments = 64;
    if (thickness <= 0.0f) return;

    shader->SetMatrix("uProj", projection.Data());
    shader->PreRender();

    constexpr float PI2 = 6.28318530718f;

    const float innerRadiusX = std::max(0.0f, radiusX - thickness);
    const float innerRadiusY = std::max(0.0f, radiusY - thickness);

    float verts[64 * 6 * 4];
    int out = 0;

    const float uvCenterX = (u0 + u1) * 0.5f;
    const float uvCenterY = (v0 + v1) * 0.5f;
    const float uvRadiusX = (u1 - u0) * 0.5f;
    const float uvRadiusY = (v1 - v0) * 0.5f;

    for (int i = 0; i < segments; ++i) {
        const float a0 = ((float)i / (float)segments) * PI2;
        const float a1 = ((float)(i + 1) / (float)segments) * PI2;

        const float c0 = std::cos(a0);
        const float s0 = std::sin(a0);
        const float c1 = std::cos(a1);
        const float s1 = std::sin(a1);

        const float outerX0 = centerX + c0 * radiusX;
        const float outerY0 = centerY + s0 * radiusY;
        const float outerX1 = centerX + c1 * radiusX;
        const float outerY1 = centerY + s1 * radiusY;

        const float innerX0 = centerX + c0 * innerRadiusX;
        const float innerY0 = centerY + s0 * innerRadiusY;
        const float innerX1 = centerX + c1 * innerRadiusX;
        const float innerY1 = centerY + s1 * innerRadiusY;

        const float outerU0 = uvCenterX + c0 * uvRadiusX;
        const float outerV0 = uvCenterY + s0 * uvRadiusY;
        const float outerU1 = uvCenterX + c1 * uvRadiusX;
        const float outerV1 = uvCenterY + s1 * uvRadiusY;

        const float innerU0 = uvCenterX + c0 * uvRadiusX * (innerRadiusX / std::max(radiusX, 0.0001f));
        const float innerV0 = uvCenterY + s0 * uvRadiusY * (innerRadiusY / std::max(radiusY, 0.0001f));
        const float innerU1 = uvCenterX + c1 * uvRadiusX * (innerRadiusX / std::max(radiusX, 0.0001f));
        const float innerV1 = uvCenterY + s1 * uvRadiusY * (innerRadiusY / std::max(radiusY, 0.0001f));

        verts[out++] = outerX0; verts[out++] = outerY0; verts[out++] = outerU0; verts[out++] = outerV0;
        verts[out++] = outerX1; verts[out++] = outerY1; verts[out++] = outerU1; verts[out++] = outerV1;
        verts[out++] = innerX1; verts[out++] = innerY1; verts[out++] = innerU1; verts[out++] = innerV1;

        verts[out++] = outerX0; verts[out++] = outerY0; verts[out++] = outerU0; verts[out++] = outerV0;
        verts[out++] = innerX1; verts[out++] = innerY1; verts[out++] = innerU1; verts[out++] = innerV1;
        verts[out++] = innerX0; verts[out++] = innerY0; verts[out++] = innerU0; verts[out++] = innerV0;
    }

    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);

    const GLsizeiptr sizeBytes = (GLsizeiptr)(out * sizeof(float));
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeBytes, verts);

    glDrawArrays(GL_TRIANGLES, 0, segments * 6);

    shader->PostRender();
}

void glesContext::InitQuadMesh() {
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 64 * 3 * 4, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                          (void*)(sizeof(float) * 2));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void glesContext::InitGouraudMesh() {
    glGenVertexArrays(1, &gouraudVAO);
    glGenBuffers(1, &gouraudVBO);
    glBindVertexArray(gouraudVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gouraudVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 6 * 6, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 6, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 6,
                          (void*)(sizeof(float) * 2));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    gouraudProgram = LinkGLESProgram("gouraud");
    if (gouraudProgram) {
        gouraudUProjLoc = glGetUniformLocation(gouraudProgram, "uProj");
    }
}

void glesContext::InitBatchMesh() {
    glGenVertexArrays(1, &batchVAO);
    glGenBuffers(1, &batchVBO);
    glBindVertexArray(batchVAO);
    glBindBuffer(GL_ARRAY_BUFFER, batchVBO);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(pddiBatchVertex),
                          (void*)offsetof(pddiBatchVertex, x));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(pddiBatchVertex),
                          (void*)offsetof(pddiBatchVertex, u));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(pddiBatchVertex),
                          (void*)offsetof(pddiBatchVertex, r));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    batchProgram = LinkGLESProgram("batch");
    if (batchProgram) {
        batchUProjLoc = glGetUniformLocation(batchProgram, "uProj");
        batchUTexLoc = glGetUniformLocation(batchProgram, "uTex");
    }
}

void glesContext::DrawQuad(pddiBaseShader* shader,
                           float x, float y, float w, float h,
                           float u0, float v0, float u1, float v1) {
    shader->SetMatrix("uProj", projection.Data());
    shader->PreRender();

    const float yTop = y;
    const float yBottom = y + h;

    float verts[] = {
        x,     yBottom, u0, v1,
        x + w, yBottom, u1, v1,
        x + w, yTop,    u1, v0,
        x,     yBottom, u0, v1,
        x + w, yTop,    u1, v0,
        x,     yTop,    u0, v0,
    };

    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    shader->PostRender();
}

void glesContext::DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                                const pddiBatchVertex* verts, s32 vertCount) {
    if (!batchProgram || !verts || vertCount <= 0) {
        return;
    }

    SetBlendMode(blend);
    glUseProgram(batchProgram);
    if (batchUProjLoc >= 0) {
        glUniformMatrix4fv(batchUProjLoc, 1, GL_FALSE, projection.Data());
    }

    pddiTexture* boundTex = tex ? tex : GetDefaultWhiteTexture();
    if (boundTex) {
        boundTex->Bind(0);
    }
    if (batchUTexLoc >= 0) {
        glUniform1i(batchUTexLoc, 0);
    }

    glBindVertexArray(batchVAO);
    glBindBuffer(GL_ARRAY_BUFFER, batchVBO);

    const size_t neededBytes = sizeof(pddiBatchVertex) * (size_t)vertCount;
    if (neededBytes > batchVBOCapacityBytes) {
        batchVBOCapacityBytes = neededBytes + neededBytes / 2;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)batchVBOCapacityBytes, nullptr, GL_STREAM_DRAW);
    }
    else {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)batchVBOCapacityBytes, nullptr, GL_STREAM_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)neededBytes, verts);

    glDrawArrays(GL_TRIANGLES, 0, vertCount);
    glUseProgram(0);
}

void glesContext::DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                                  float x1, float y1, float r1, float g1, float b1, float a1,
                                  float x2, float y2, float r2, float g2, float b2, float a2,
                                  float x3, float y3, float r3, float g3, float b3, float a3) {
    if (!gouraudProgram) return;

    float verts[] = {
        x0, y0, r0, g0, b0, a0,
        x1, y1, r1, g1, b1, a1,
        x2, y2, r2, g2, b2, a2,
        x1, y1, r1, g1, b1, a1,
        x3, y3, r3, g3, b3, a3,
        x2, y2, r2, g2, b2, a2,
    };

    glUseProgram(gouraudProgram);
    glUniformMatrix4fv(gouraudUProjLoc, 1, GL_FALSE, projection.Data());

    glBindVertexArray(gouraudVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gouraudVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void glesContext::Init3DShader() {
    program3D = LinkGLESProgram("mesh");

    if (program3D == 0) {
        LogGles("Init3DShader: unavailable; mesh rendering disabled");
    }
    else {
        LogGles("Init3DShader: OK, program3D=%u", program3D);
        u3D.alphaScale = glGetUniformLocation(program3D, "uAlphaScale");
        u3D.useZeroTexelKey = glGetUniformLocation(program3D, "uUseZeroTexelKey");
        u3D.mvp = glGetUniformLocation(program3D, "uMVP");
        u3D.worldMatrix = glGetUniformLocation(program3D, "uWorldMatrix");
        u3D.viewMatrix = glGetUniformLocation(program3D, "uViewMatrix");
        u3D.cameraPos = glGetUniformLocation(program3D, "uCameraPos");
        u3D.shadowDebugMode = glGetUniformLocation(program3D, "uShadowDebugMode");
        u3D.receiveShadows = glGetUniformLocation(program3D, "uReceiveShadows");
        u3D.shadowCascadeCount = glGetUniformLocation(program3D, "uShadowCascadeCount");
        u3D.shadowFilterQuality = glGetUniformLocation(program3D, "uShadowFilterQuality");
        u3D.shadowBias = glGetUniformLocation(program3D, "uShadowBias[0]");
        u3D.shadowLightDir = glGetUniformLocation(program3D, "uShadowLightDir");
        u3D.lightVP = glGetUniformLocation(program3D, "uLightVP[0]");
        u3D.cascadeSplits = glGetUniformLocation(program3D, "uCascadeSplits[0]");
        u3D.cascadeBlendDistances = glGetUniformLocation(program3D, "uCascadeBlendDistances[0]");
        u3D.shadowTexelWorldSize = glGetUniformLocation(program3D, "uShadowTexelWorldSize[0]");
        u3D.receiverInstanceId = glGetUniformLocation(program3D, "uReceiverInstanceId");
        u3D.shadowMap[0] = glGetUniformLocation(program3D, "uShadowMap0");
        u3D.shadowMap[1] = glGetUniformLocation(program3D, "uShadowMap1");
        u3D.shadowMap[2] = glGetUniformLocation(program3D, "uShadowMap2");
        u3D.shadowIdMap[0] = glGetUniformLocation(program3D, "uShadowIdMap0");
        u3D.shadowIdMap[1] = glGetUniformLocation(program3D, "uShadowIdMap1");
        u3D.shadowIdMap[2] = glGetUniformLocation(program3D, "uShadowIdMap2");
        u3D.hasVRAM = glGetUniformLocation(program3D, "uHasVRAM");
        u3D.texInfoOverrideEnabled = glGetUniformLocation(program3D, "uTexInfoOverrideEnabled");
        u3D.texInfoOverride = glGetUniformLocation(program3D, "uTexInfoOverride");
        u3D.vram = glGetUniformLocation(program3D, "uVRAM");
        u3D.realTextureMode = glGetUniformLocation(program3D, "uRealTextureMode");
        u3D.realTex = glGetUniformLocation(program3D, "uRealTex");
        u3D.realTexOffset = glGetUniformLocation(program3D, "uRealTexOffset");
        u3D.realTexSize = glGetUniformLocation(program3D, "uRealTexSize");
    }
}

void glesContext::InitShadowDepthShader() {
    shadowDepthProgram = LinkGLESProgram("shadow_depth");

    if (shadowDepthProgram == 0) {
        LogGles("InitShadowDepthShader: unavailable; shadow casting disabled");
    }
    else {
        LogGles("InitShadowDepthShader: OK, shadowDepthProgram=%u", shadowDepthProgram);
        uShadowDepth.lightMVP = glGetUniformLocation(shadowDepthProgram, "uLightMVP");
        uShadowDepth.casterInstanceId = glGetUniformLocation(shadowDepthProgram, "uCasterInstanceId");
        uShadowDepth.hasUV = glGetUniformLocation(shadowDepthProgram, "uHasUV");
        uShadowDepth.hasTexInfo = glGetUniformLocation(shadowDepthProgram, "uHasTexInfo");
        uShadowDepth.hasVRAM = glGetUniformLocation(shadowDepthProgram, "uHasVRAM");
        uShadowDepth.useZeroTexelKey = glGetUniformLocation(shadowDepthProgram, "uUseZeroTexelKey");
        uShadowDepth.texInfoOverrideEnabled = glGetUniformLocation(shadowDepthProgram, "uTexInfoOverrideEnabled");
        uShadowDepth.texInfoOverride = glGetUniformLocation(shadowDepthProgram, "uTexInfoOverride");
        uShadowDepth.vram = glGetUniformLocation(shadowDepthProgram, "uVRAM");
        uShadowDepth.realTextureMode = glGetUniformLocation(shadowDepthProgram, "uRealTextureMode");
        uShadowDepth.realTex = glGetUniformLocation(shadowDepthProgram, "uRealTex");
        uShadowDepth.realTexOffset = glGetUniformLocation(shadowDepthProgram, "uRealTexOffset");
        uShadowDepth.realTexSize = glGetUniformLocation(shadowDepthProgram, "uRealTexSize");
    }
}

void glesContext::SetShadowCasterPass(bool enable, const Mat4& lightVP) {
    shadowCasterPassActive = enable;
    shadowCasterLightVP = lightVP;
}

void glesContext::SetShadowCascades(pddiTexture* const* depthTextures, const Mat4* lightVPs,
                                    const float* splits, const float* texelWorldSizes,
                                    pddiTexture* const* idTextures, int count) {
    shadowCascadeCount = (count < 0) ? 0 : (count > kShadowCascadeCount ? kShadowCascadeCount : count);
    shadowFilterQuality = 0;
    for (s32 i = 0; i < shadowCascadeCount; i++) {
        shadowDepthTextures[i] = depthTextures[i];
        shadowIdTextures[i] = idTextures ? idTextures[i] : nullptr;
        shadowLightVP[i] = lightVPs[i];
        shadowCascadeSplits[i] = splits[i];
        shadowTexelWorldSize[i] = texelWorldSizes ? texelWorldSizes[i] : 0.0f;
        const float previousSplit = (i > 0) ? shadowCascadeSplits[i - 1] : 0.0f;
        const float cascadeRange = std::max(shadowCascadeSplits[i] - previousSplit, 1.0f);
        shadowCascadeBlendDistances[i] = std::min(std::max(cascadeRange * 0.12f, 384.0f), 1536.0f);
        if (depthTextures[i] && depthTextures[i]->GetWidth() >= 8192) {
            shadowFilterQuality = 3;
        }
        else if (depthTextures[i] && depthTextures[i]->GetWidth() >= 4096) {
            shadowFilterQuality = 2;
        }
        else if (depthTextures[i] && depthTextures[i]->GetWidth() >= 2048) {
            shadowFilterQuality = 1;
        }
    }
    const float* biasSet = (shadowFilterQuality >= 3) ? shadowBiasVeryHigh
        : (shadowFilterQuality == 2) ? shadowBiasHigh
        : (shadowFilterQuality == 1) ? shadowBiasMedium : shadowBiasLow;
    for (s32 i = 0; i < kShadowCascadeCount; i++) {
        shadowBias[i] = biasSet[i];
    }
    for (s32 i = shadowCascadeCount; i < kShadowCascadeCount; i++) {
        shadowDepthTextures[i] = nullptr;
        shadowIdTextures[i] = nullptr;
        shadowCascadeSplits[i] = 0.0f;
        shadowCascadeBlendDistances[i] = 0.0f;
        shadowTexelWorldSize[i] = 0.0f;
    }
    shadowConstUniformsDirty = true;
    shadowCascadeUniformsDirty = true;
}

void glesContext::SetTexture(pddiTexture* t) {
    currentTexture = t;
}

void glesContext::SetVRAMHandle(u32 h) {
    vramHandle = h;
}

void glesContext::SetTexInfoOverride(bool enabled, u32 texInfoWord) {
    texInfoOverrideEnabled = enabled;
    texInfoOverrideWord = texInfoWord;
}

void glesContext::SetRealTextureRect(float offsetX, float offsetY, float sizeX, float sizeY) {
    realTexOffsetX = offsetX;
    realTexOffsetY = offsetY;
    realTexSizeX = sizeX;
    realTexSizeY = sizeY;
}

u32 glesContext::CreateVRAMTexture(int w, int h, const u16* data) {
    u32 tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // RG8 rather than desktop's R16UI: each halfword is uploaded as its two
    // little-endian bytes (R = low, G = high) and recombined in the shader with
    // float maths. See kVramDecode -- the integer sampler path is not evaluated
    // faithfully by the Switch emulators' shader recompilers.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w, h, 0, GL_RG, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void glesContext::DestroyVRAMTexture(u32 handle) {
    if (handle) glDeleteTextures(1, &handle);
}

void glesContext::UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) {
    // In-place RG8 re-upload (no realloc) -- called every frame by
    // Director::updateVramAnims while a VRAM flipbook animates.
    if (!handle || !data || w <= 0 || h <= 0) return;
    glBindTexture(GL_TEXTURE_2D, handle);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void glesContext::UpdateVRAMTextureRect(u32 handle, int x, int y, int w, int h,
                                        int srcPitchTexels, const u16* data) {
    if (!handle || !data || w <= 0 || h <= 0) return;
    glBindTexture(GL_TEXTURE_2D, handle);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    // Rows of the rect are srcPitchTexels apart in the source buffer.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, srcPitchTexels);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RG, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void glesContext::DrawPrimBuffer(pddiPrimBuffer* buffer, u32 indexOffset, u32 indexCount) {
    if (!buffer)
        return;

    if (shadowCasterPassActive) {
        if (!shadowDepthProgram) {
            return;
        }
        glUseProgram(shadowDepthProgram);
        Mat4 lightMvp = shadowCasterLightVP * worldMatrix;
        glUniformMatrix4fv(uShadowDepth.lightMVP, 1, GL_FALSE, lightMvp.Data());
        glUniform1ui(uShadowDepth.casterInstanceId, shadowCasterInstanceId);

        auto* glBufShadow = static_cast<glesPrimBuffer*>(buffer);
        const u32 vertexFormat = glBufShadow->GetVertexFormat();
        glUniform1i(uShadowDepth.hasUV, (vertexFormat & PDDI_V_UV) ? 1 : 0);
        glUniform1i(uShadowDepth.hasTexInfo, (vertexFormat & PDDI_V_TEXINFO) ? 1 : 0);
        const int useZeroTexelKey = (cachedBlendMode != PDDI_BLEND_NONE) ? 1 : 0;
        glUniform1i(uShadowDepth.useZeroTexelKey, useZeroTexelKey);

        const int texInfoOverride = texInfoOverrideEnabled ? 1 : 0;
        glUniform1i(uShadowDepth.texInfoOverrideEnabled, texInfoOverride);
        if (texInfoOverride != 0) {
            const float tpage = static_cast<float>((texInfoOverrideWord >> 16) & 0xFFFFu);
            const float cba = static_cast<float>(texInfoOverrideWord & 0xFFFFu);
            glUniform2f(uShadowDepth.texInfoOverride, tpage, cba);
        }
        else {
            glUniform2f(uShadowDepth.texInfoOverride, -1.0f, 0.0f);
        }

        glUniform1i(uShadowDepth.hasVRAM, vramHandle ? 1 : 0);
        if (vramHandle) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, vramHandle);
            glUniform1i(uShadowDepth.vram, 0);
        }

        const int useRealTexture = (realTextureModeEnabled && currentTexture) ? 1 : 0;
        glUniform1i(uShadowDepth.realTextureMode, useRealTexture);
        if (useRealTexture) {
            static_cast<glesTexture*>(currentTexture)->Bind(1);
            glUniform1i(uShadowDepth.realTex, 1);
            glUniform2f(uShadowDepth.realTexOffset, realTexOffsetX, realTexOffsetY);
            glUniform2f(uShadowDepth.realTexSize, realTexSizeX, realTexSizeY);
        }

        GLenum glModeShadow = GL_TRIANGLES;
        switch (buffer->GetPrimType()) {
            case PDDI_PRIM_TRIANGLES: glModeShadow = GL_TRIANGLES; break;
            case PDDI_PRIM_TRISTRIP:  glModeShadow = GL_TRIANGLE_STRIP; break;
            case PDDI_PRIM_LINES:     glModeShadow = GL_LINES; break;
            case PDDI_PRIM_LINESTRIP: glModeShadow = GL_LINE_STRIP; break;
            case PDDI_PRIM_POINTS:    glModeShadow = GL_POINTS; break;
        }
        const u32 drawCountShadow = (indexCount != 0) ? indexCount : buffer->GetIndexCount();
        const void* indexPtrShadow = reinterpret_cast<const void*>(static_cast<uintptr_t>(indexOffset) * sizeof(u16));
        glBindVertexArray(glBufShadow->GetVAO());
        glDrawElements(glModeShadow, drawCountShadow, GL_UNSIGNED_SHORT, indexPtrShadow);
        return;
    }
    glUseProgram(program3D);

    const float alphaScale = (cachedBlendMode == PDDI_BLEND_ALPHA) ? 0.5f : 1.0f;
    if (prog3DCache.alphaScale != alphaScale) {
        glUniform1f(u3D.alphaScale, alphaScale);
        prog3DCache.alphaScale = alphaScale;
    }
    const int useZeroTexelKey = (cachedBlendMode != PDDI_BLEND_NONE) ? 1 : 0;
    if (prog3DCache.useZeroTexelKey != useZeroTexelKey) {
        glUniform1i(u3D.useZeroTexelKey, useZeroTexelKey);
        prog3DCache.useZeroTexelKey = useZeroTexelKey;
    }

    Mat4 mvp = projection * (viewMatrix * worldMatrix);
    glUniformMatrix4fv(u3D.mvp, 1, GL_FALSE, mvp.Data());
    glUniformMatrix4fv(u3D.worldMatrix, 1, GL_FALSE, worldMatrix.Data());
    if (frameConstUniformsDirty) {
        glUniformMatrix4fv(u3D.viewMatrix, 1, GL_FALSE, viewMatrix.Data());
        glUniform3f(u3D.cameraPos, cameraWorldPos[0], cameraWorldPos[1], cameraWorldPos[2]);
        glUniform1i(u3D.shadowDebugMode, shadowDebugMode);
        frameConstUniformsDirty = false;
    }

    const int receiveShadows = (receiveShadowsEnabled && shadowCascadeCount > 0) ? 1 : 0;
    const int activeCascadeCount = receiveShadows ? shadowCascadeCount : 0;
    const int activeFilterQuality = receiveShadows ? shadowFilterQuality : 0;
    if (prog3DCache.receiveShadows != receiveShadows) {
        glUniform1i(u3D.receiveShadows, receiveShadows);
        prog3DCache.receiveShadows = receiveShadows;
    }
    if (prog3DCache.shadowCascadeCount != activeCascadeCount) {
        glUniform1i(u3D.shadowCascadeCount, activeCascadeCount);
        prog3DCache.shadowCascadeCount = activeCascadeCount;
    }
    if (prog3DCache.shadowFilterQuality != activeFilterQuality) {
        glUniform1i(u3D.shadowFilterQuality, activeFilterQuality);
        prog3DCache.shadowFilterQuality = activeFilterQuality;
    }
    if (shadowConstUniformsDirty) {
        glUniform1fv(u3D.shadowBias, kShadowCascadeCount, shadowBias);
        glUniform3f(u3D.shadowLightDir, shadowLightDir[0], shadowLightDir[1], shadowLightDir[2]);
        shadowConstUniformsDirty = false;
    }
    if (receiveShadows) {
        if (shadowCascadeUniformsDirty) {
            glUniformMatrix4fv(u3D.lightVP, shadowCascadeCount, GL_FALSE, shadowLightVP[0].Data());
            glUniform1fv(u3D.cascadeSplits, shadowCascadeCount, shadowCascadeSplits);
            glUniform1fv(u3D.cascadeBlendDistances, shadowCascadeCount, shadowCascadeBlendDistances);
            glUniform1fv(u3D.shadowTexelWorldSize, kShadowCascadeCount, shadowTexelWorldSize);
            shadowCascadeUniformsDirty = false;
        }
        glUniform1ui(u3D.receiverInstanceId, shadowReceiverInstanceId);
        for (s32 i = 0; i < shadowCascadeCount; i++) {
            if (!shadowDepthTextures[i]) continue;
            // No sampler-object override (unlike desktop): the texture's own NEAREST/
            // COMPARE_MODE=NONE state already matches what the manual-compare shader wants.
            static_cast<glesTexture*>(shadowDepthTextures[i])->Bind(2 + i);
            glUniform1i(u3D.shadowMap[i], 2 + i);
            if (shadowIdTextures[i]) {
                static_cast<glesTexture*>(shadowIdTextures[i])->Bind(5 + i);
                glUniform1i(u3D.shadowIdMap[i], 5 + i);
            }
        }
    }

    const int texInfoOverride = texInfoOverrideEnabled ? 1 : 0;
    if (prog3DCache.texInfoOverrideEnabled != texInfoOverride) {
        glUniform1i(u3D.texInfoOverrideEnabled, texInfoOverride);
        prog3DCache.texInfoOverrideEnabled = texInfoOverride;
    }
    const float tpage = (texInfoOverride != 0)
        ? static_cast<float>((texInfoOverrideWord >> 16) & 0xFFFFu) : -1.0f;
    const float cba = (texInfoOverride != 0)
        ? static_cast<float>(texInfoOverrideWord & 0xFFFFu) : 0.0f;
    if (prog3DCache.texInfoTpage != tpage || prog3DCache.texInfoCba != cba) {
        glUniform2f(u3D.texInfoOverride, tpage, cba);
        prog3DCache.texInfoTpage = tpage;
        prog3DCache.texInfoCba = cba;
    }

    const int hasVRAM = vramHandle ? 1 : 0;
    if (prog3DCache.hasVRAM != hasVRAM) {
        glUniform1i(u3D.hasVRAM, hasVRAM);
        prog3DCache.hasVRAM = hasVRAM;
    }
    if (vramHandle) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vramHandle);
        glUniform1i(u3D.vram, 0);
    }

    const int useRealTexture = (realTextureModeEnabled && currentTexture) ? 1 : 0;
    if (prog3DCache.realTextureMode != useRealTexture) {
        glUniform1i(u3D.realTextureMode, useRealTexture);
        prog3DCache.realTextureMode = useRealTexture;
    }
    if (useRealTexture) {
        static_cast<glesTexture*>(currentTexture)->Bind(1);
        glUniform1i(u3D.realTex, 1);
        glUniform2f(u3D.realTexOffset, realTexOffsetX, realTexOffsetY);
        glUniform2f(u3D.realTexSize, realTexSizeX, realTexSizeY);
    }

    GLenum glMode = GL_TRIANGLES;
    switch (buffer->GetPrimType()) {
        case PDDI_PRIM_TRIANGLES: glMode = GL_TRIANGLES; break;
        case PDDI_PRIM_TRISTRIP:  glMode = GL_TRIANGLE_STRIP; break;
        case PDDI_PRIM_LINES:     glMode = GL_LINES; break;
        case PDDI_PRIM_LINESTRIP: glMode = GL_LINE_STRIP; break;
        case PDDI_PRIM_POINTS:    glMode = GL_POINTS; break;
    }

    const u32 drawCount = (indexCount != 0) ? indexCount : buffer->GetIndexCount();
    const void* indexPtr = reinterpret_cast<const void*>(static_cast<uintptr_t>(indexOffset) * sizeof(u16));

    auto* glBuf = static_cast<glesPrimBuffer*>(buffer);
    glBindVertexArray(glBuf->GetVAO());
    glDrawElements(glMode, drawCount, GL_UNSIGNED_SHORT, indexPtr);
}

// glesDevice

pddiDisplay* glesDevice::NewDisplay() { return new glesDisplay(); }
pddiRenderContext* glesDevice::NewRenderContext(pddiDisplay* d) { return new glesContext(static_cast<glesDisplay*>(d)); }
pddiGamepad* glesDevice::NewGamepad() { return new glesGamepad(); }
pddiTexture* glesDevice::NewTexture() { return new glesTexture(); }
pddiPrimBuffer* glesDevice::NewPrimBuffer(const pddiPrimBufferDesc& desc) { return new glesPrimBuffer(desc); }
pddiBaseShader* glesDevice::NewShader(const char* type) {
    glesShader* shader = new glesShader(type);
    if (!shader->IsValid()) {
        shader->Release();
        return nullptr;
    }
    return shader;
}

pddiDevice* pddiCreate() {
    return new glesDevice();
}

// glesGamepad — real libnx PadState-backed implementation. Single
// player-1 standard pad; the menu is single-player only.

namespace {
struct ButtonMapEntry {
    int pddiButton;
    HidNpadButton switchButton;
};

constexpr ButtonMapEntry kButtonMap[] = {
    { GamepadButton::A,          HidNpadButton_A },
    { GamepadButton::B,          HidNpadButton_B },
    { GamepadButton::X,          HidNpadButton_X },
    { GamepadButton::Y,          HidNpadButton_Y },
    { GamepadButton::LeftBumper, HidNpadButton_L },
    { GamepadButton::RightBumper,HidNpadButton_R },
    { GamepadButton::Back,       HidNpadButton_Minus },
    { GamepadButton::Start,      HidNpadButton_Plus },
    { GamepadButton::LeftThumb,  HidNpadButton_StickL },
    { GamepadButton::RightThumb, HidNpadButton_StickR },
    { GamepadButton::DpadUp,     HidNpadButton_Up },
    { GamepadButton::DpadRight,  HidNpadButton_Right },
    { GamepadButton::DpadDown,   HidNpadButton_Down },
    { GamepadButton::DpadLeft,   HidNpadButton_Left },
};
} // namespace

glesGamepad::glesGamepad() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
}

void glesGamepad::Poll() {
    padUpdate(&pad);
    connected = padIsConnected(&pad);
    heldButtons = padGetButtons(&pad);

    HidAnalogStickState left = padGetStickPos(&pad, 0);
    HidAnalogStickState right = padGetStickPos(&pad, 1);
    axes[GamepadAxis::LeftX] = left.x / 32767.0f;
    axes[GamepadAxis::LeftY] = -left.y / 32767.0f;
    axes[GamepadAxis::RightX] = right.x / 32767.0f;
    axes[GamepadAxis::RightY] = -right.y / 32767.0f;
    axes[GamepadAxis::LeftTrigger] = (heldButtons & HidNpadButton_ZL) ? 1.0f : 0.0f;
    axes[GamepadAxis::RightTrigger] = (heldButtons & HidNpadButton_ZR) ? 1.0f : 0.0f;
}

bool glesGamepad::IsButtonDown(int button) const {
    for (const auto& entry : kButtonMap) {
        if (entry.pddiButton == button) {
            return (heldButtons & entry.switchButton) != 0;
        }
    }
    return false;
}

float glesGamepad::GetAxis(int axis) const {
    if (axis < 0 || axis >= GamepadAxis::COUNT) {
        return 0.0f;
    }
    return axes[axis];
}

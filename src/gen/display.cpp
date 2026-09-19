#include "gen/common.h"
#include "gen/display.h"
#include "gen/camera.h"
#include "gen/config.h"
#include "gen/psxmath_helpers.h"
#include "gen/time.h"
#include "gen/game.h"
#include "p3d/context.h"
#include "p3d/camera.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "pc/controllerlight.h"
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
#include "psp/psp_perf.h"
#endif

Display* g_display = nullptr;
s32 Display::s_defaultScreenMode = ScreenMode_Windowed;
s32 Display::s_defaultVsync = 1;
s32 Display::s_defaultMsaa = 0;

static s32 NormalizeMSAASamples(s32 samples) {
    if (samples <= 0) {
        return 0;
    }

    static const s32 kAllowed[] = { 2, 4, 8, 16 };
    s32 closest = kAllowed[0];
    s32 closestDist = (samples > closest) ? (samples - closest) : (closest - samples);

    for (u32 i = 1; i < (u32)(sizeof(kAllowed) / sizeof(kAllowed[0])); i++) {
        const s32 candidate = kAllowed[i];
        const s32 dist = (samples > candidate) ? (samples - candidate) : (candidate - samples);
        if (dist < closestDist) {
            closest = candidate;
            closestDist = dist;
        }
    }

    return closest;
}

// PSX: __7Display (0x800C9A30) / _7Display (overlay, 0x8012BAD0)
Display::Display() {
    MARKFUNCTION(0x800C9A30);
    g_display = this;
    pri = 60; // PSX: byte at offset 18 = 60
    screenMode = s_defaultScreenMode;
    vsync = s_defaultVsync;
    msaa = NormalizeMSAASamples(s_defaultMsaa);
    resolutionIndex = 0;
}

// PSX: __7Display (DISPLAY.CPP:67, 0x800DB408)
Display::~Display() {
    MARKFUNCTION(0x800DB408);
    InternalClose();
    g_display = nullptr;
}

ChanProjectionState Display::GetChanProjectionState() const {
    ChanProjectionState state;
    f32 cameraAspect = DEFAULT_ASPECT_RATIO;

    if (screenWidth > 0) {
        state.width = screenWidth;
        state.centerX = screenWidth / 2;
    }
    if (screenHeight > 0) {
        state.height = screenHeight;
        state.centerY = screenHeight / 2;
    }

#if FIX_ASPECT_RATIO
    if (aspectRatio > 0.0f) {
        cameraAspect = aspectRatio;
    }
#endif

    tCamera* camera = theCamera ? theCamera->GetP3DCamera() : nullptr;
    if (!camera) {
        return state;
    }

    s32 fovX = 0, fovY = 0x2666;
    camera->GetFOV(&fovX, &fovY);
    camera->GetClipPlanes(&state.nearClip, &state.farClip);

    const f32 tanHalfY = PsxTanHalfFromFov(fovY);
    if (tanHalfY > 0.0f) {
        state.projectionDistanceY = PsxProjectionDistanceFromTan(state.centerY, tanHalfY);
        const f32 tanHalfX = tanHalfY * cameraAspect;
        state.projectionDistanceX = PsxProjectionDistanceFromTan(state.centerX, tanHalfX);
    }

    return state;
}

// PSX: InternalOpen__7Display (overlay, 0x8012BAE4)
// Calls platOpen which creates the camera, sets view layers, viewport, lighting.
void Display::SetCursorCaptured(bool captured) {
    if (p3d::display) {
        p3d::display->ShowCursor(!captured);
        p3d::display->ClipCursor(captured);
    }
}

void Display::SetCursorVisible(bool visible) {
    if (p3d::display) {
        p3d::display->ShowCursor(visible);
    }
}

void Display::InternalOpen() {
    MARKFUNCTION(0x8012BAE4);

    // PSX: platOpen__7Display (overlay, 0x8012BCC0)
    // PSX: _builtin_new(492) -> _6CameraPC10tagLVector({0, 800, 6553})
    // PSX: theCamera->vtable+12() (Reset)
    // PSX: SetCamera(view0, theCamera+404)   [+404 = embedded tMatrixCamera]
    theCamera = new Camera();
    theCamera->SetPosition(0, 800, 6553); // PSX init position {0, 0x320, 0x1999}
    theCamera->Reset();

    tMatrixCamera* p3dCam = theCamera->GetP3DCamera();
    p3dCam->SetNearPlane(Camera::kRenderNearPlane);
    p3dCam->SetFarPlane(65535.0f);
    view.SetCamera(p3dCam);
    view.SetBackgroundColour(pddiColour(0, 0, 0));
    view.SetClearMask(PDDI_BUFFER_ALL);

    // PSX: SetupLayer calls for 7 view layers, SetViewPort, SetLighting
    // PC: tView doesn't have layer system, viewport is full-screen by default.

    frameCounter = 0;
    SetMSAA(msaa);
    SetCursorCaptured(true);
}

// PSX: InternalClose__7Display (DISPLAY.CPP:76, 0x800DB418)
void Display::InternalClose() {
    MARKFUNCTION(0x800DB418);
    // PSX: platClose -> theCamera = 0
    // PSX doesn't free the camera here (memory allocator handles it).
    // PC: we own it, so delete it.
    SetCursorCaptured(false);
    view.SetCamera(nullptr);
    if (theCamera) {
        delete theCamera;
        theCamera = nullptr;
    }
}

// PSX: InternalReset__7Display (DISPLAY.CPP:83, 0x800DB428)
void Display::InternalReset() {
    MARKFUNCTION(0x800DB428);
    // PSX: platReset - empty
}

void Display::SetFullscreen(s32 enabled) {
    SetScreenMode(enabled ? ScreenMode_Fullscreen : ScreenMode_Windowed);
}

void Display::SetScreenMode(s32 mode) {
    if (mode < (s32)ScreenMode_Fullscreen) mode = (s32)ScreenMode_Fullscreen;
    if (mode > (s32)ScreenMode_Windowed) mode = (s32)ScreenMode_Windowed;

    screenMode = mode;
    s_defaultScreenMode = screenMode;

    if (p3d::display) {
        if (screenMode == ScreenMode_Fullscreen) {
            p3d::display->SetBorderless(false);
            p3d::display->SetFullscreen(true);
        }
        else if (screenMode == ScreenMode_Borderless) {
            p3d::display->SetFullscreen(false);
            p3d::display->SetBorderless(true);
        }
        else {
            p3d::display->SetBorderless(false);
            p3d::display->SetFullscreen(false);
        }

        // Re-apply the currently selected display mode dimensions after changing
        // window mode so startup/menu ordering cannot force a stale size.
        pddiVideoMode selectedMode;
        if (GetResolutionMode(resolutionIndex, selectedMode)) {
            p3d::display->SetResolution(selectedMode.width, selectedMode.height);
        }
    }
}

void Display::SetVsync(s32 enabled) {
    vsync = enabled ? 1 : 0;
    s_defaultVsync = vsync;
    if (p3d::display) {
        p3d::display->SetVSync(vsync != 0);
    }
}

void Display::SetMSAA(s32 samples) {
    msaa = NormalizeMSAASamples(samples);
    s_defaultMsaa = msaa;

    if (p3d::display) {
        p3d::display->SetMSAA(msaa);
        msaa = p3d::display->GetMSAA();
        s_defaultMsaa = msaa;
    }
}

void Display::SetDefaultMSAA(s32 samples) {
    s_defaultMsaa = NormalizeMSAASamples(samples);
}

s32 Display::GetResolutionCount() const {
    if (!p3d::display) {
        return 0;
    }
    pddiVideoMode modes[128];
    return BuildUniqueVideoModes(p3d::display, modes, 128);
}

bool Display::GetResolutionMode(s32 index, pddiVideoMode& mode) const {
    if (!p3d::display) {
        return false;
    }
    pddiVideoMode modes[128];
    const s32 count = BuildUniqueVideoModes(p3d::display, modes, 128);
    if (index < 0 || index >= count) {
        return false;
    }
    mode = modes[index];
    return true;
}

void Display::SetResolutionIndex(s32 index) {
    if (!p3d::display) {
        return;
    }

    pddiVideoMode modes[128];
    const s32 count = BuildUniqueVideoModes(p3d::display, modes, 128);
    if (count <= 0) {
        return;
    }

    if (index < 0) index = 0;
    if (index >= count) index = count - 1;

    const pddiVideoMode mode = modes[index];
    p3d::display->SetResolution(mode.width, mode.height);

    resolutionIndex = index;
    screenWidth = mode.width;
    screenHeight = mode.height;
    if (mode.height > 0) {
        aspectRatio = (f32)mode.width / (f32)mode.height;
    }
}

// PSX: BeginFrame__7Display (PSXDISP.CPP:75, 0x80026CA0)
void Display::BeginFrame() {
    MARKFUNCTION(0x80026CA0);
    // PSX: P3D::BeginFrame() + save prim ptr + tView::BeginRender(view0)

    f32 cameraAspect = 4.0f / 3.0f;
    if (p3d::display) {
        s32 width = p3d::display->GetWidth();
        s32 height = p3d::display->GetHeight();
        if (height > 0) {
#if FIX_ASPECT_RATIO
            cameraAspect = (f32)width / (f32)height;
#endif
            aspectRatio = (f32)width / (f32)height;
        }

        screenWidth = width;
        screenHeight = height;
    }

    p3d::context->SetCameraAspect(cameraAspect);

    // Compute and push projection to the camera before BeginRender triggers SetState.
    if (theCamera) {
        tMatrixCamera* p3dCam = theCamera->GetP3DCamera();
        s32 fovA = 0x3333, fovB = 0x2666;
        p3dCam->GetFOV(&fovA, &fovB);

        f32 tanHalfY = PsxTanHalfFromFov(fovB);
        if (tanHalfY < 0.0001f) {
            tanHalfY = std::tan(0.35f);
        }
        f32 tanHalfX;
#if DIRECTOR_CUTSCENE_VERT_FOV
        const bool inDirectedShot = theCamera->HasActiveCameraAnim() ||
            !theCamera->HasCollisionEnabled();
        if (inDirectedShot) {
            const f32 intendedHalfX = tanHalfY * DEFAULT_ASPECT_RATIO;
            const f32 intendedHalfY = intendedHalfX / TARGET_ASPECT_RATIO;
            if (cameraAspect > TARGET_ASPECT_RATIO) {
                tanHalfY = intendedHalfY;
                tanHalfX = tanHalfY * cameraAspect;
            }
            else {
                tanHalfX = intendedHalfX;
                tanHalfY = tanHalfX / cameraAspect;
            }
        }
        else
#endif
        {
            tanHalfX = tanHalfY * cameraAspect; // Hor+
        }
        p3dCam->SetProjectionMatrix(
            PerspectiveReversedZTangents(tanHalfX, tanHalfY,
                p3dCam->GetNearPlane(), p3dCam->GetFarPlane()));
    }

    p3d::context->BeginFrame();
    p3d::context->Clear(PDDI_BUFFER_ALL);
    view.BeginRender();
}

// PSX: EndFrame__7Display (PSXDISP.CPP:86, 0x80026CB4)
void Display::EndFrame() {
    MARKFUNCTION(0x80026CB4);
    // PSX: tView::EndRender + ++_MyFrameCounter + P3D::EndFrame(1) -> SwapBuffers
    view.EndRender();
    ++frameCounter;
    p3d::context->EndFrame();
    p3d::display->RenderOverlay();
    ControllerLight::Update();
#if defined(RC_PLATFORM_SWITCH)
    DrawDebugInfo();
#endif
    const f64 t0 = Time::GetTimeInSeconds();
    p3d::display->SwapBuffers();
    g_frameProfile.swapMs = (f32)((Time::GetTimeInSeconds() - t0) * 1000.0);
}

// PSX: dispBeginFrameHandler (DISPLAY.CPP:105, 0x800DB43C)
void Display::dispBeginFrameHandler(Handler*) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: display begin");
#endif
    if (g_display) {
        g_display->BeginFrame();
    }
}

// PSX: dispEndFrameHandler (DISPLAY.CPP:112, 0x800DB448)
void Display::dispEndFrameHandler(Handler*) {
#if defined(RC_PLATFORM_PSP) && defined(PSP_PERF_TELEMETRY)
    PspPerf::Trace("render: display end");
#endif
    if (g_display) {
        g_display->EndFrame();
    }
}

s32 Display::BuildUniqueVideoModes(pddiDisplay* display, pddiVideoMode* outModes, s32 maxModes) {
    if (!display || !outModes || maxModes <= 0) {
        return 0;
    }

    const s32 rawCount = (s32)display->GetVideoModeCount();
    s32 uniqueCount = 0;

    for (s32 i = 0; i < rawCount; i++) {
        pddiVideoMode mode;
        display->GetVideoMode(i, mode);
        if (mode.width <= 0 || mode.height <= 0) {
            continue;
        }

        s32 existing = -1;
        for (s32 j = 0; j < uniqueCount; j++) {
            if (outModes[j].width == mode.width && outModes[j].height == mode.height) {
                existing = j;
                break;
            }
        }

        if (existing >= 0) {
            // Keep the highest refresh variant for this resolution.
            if (mode.refreshRate > outModes[existing].refreshRate) {
                outModes[existing] = mode;
            }
            continue;
        }

        if (uniqueCount < maxModes) {
            outModes[uniqueCount++] = mode;
        }
    }

    return uniqueCount;
}

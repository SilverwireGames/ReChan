// camera.cpp - Pure3D v11.3 camera hierarchy implementation
// PSX: TCAMERA.CPP, TMATRIXCAM.CPP, T2POINTCAM.CPP
#include "p3d/camera.h"
#include "p3d/context.h"
#include "p3d/p3dmath.h"
#include "pddi/pddidev.h"

// Global 2-point camera instance (PSX: G_2ptcam at 0x800D3964)
t2PointMatrixCamera G_2ptcam;

// --- tCamera (TCAMERA.CPP) ---

// PSX: _7tCamera (0x8008EC44)
tCamera::tCamera()
    : nearClip(1)
    , farClip(0xFFFF)
    , fovA(0x3333)   // 13107
    , fovB(0x2666)   // 9830
    , nearPlaneF(1.0f)
    , farPlaneF(65535.0f) {
}

tCamera::~tCamera() = default;

// PSX: SetFOV__7tCamerall (0x8008ECC0)
void tCamera::SetFOV(s32 a, s32 b) {
    fovA = a;
    fovB = b;
}

// PSX: GetFOV__7tCameraPlT1 (0x8008ECCC)
void tCamera::GetFOV(s32* outA, s32* outB) const {
    *outA = fovA;
    *outB = fovB;
}

// PSX: GetClipPlanes__7tCameraPUsT1 (0x8008ECE4)
void tCamera::GetClipPlanes(u16* outNear, u16* outFar) const {
    *outNear = nearClip;
    *outFar = farClip;
}

// PC bridge: push the game-provided projection matrix to the render context.
// The game is responsible for calling SetProjectionMatrix() each frame before
// view.BeginRender() is called (e.g. in Display::BeginFrame).
void tCamera::SetState() {
    if (hasProjection) {
        Mat4 proj = projection;
        if (p3d::context->GetWorldMirror()) {
            proj.m[0] = -proj.m[0];
        }
        p3d::context->SetProjectionMatrix(proj);
    }
}

// PSX: _13tMatrixCameraP6MATRIX (0x8009D5E8)
tMatrixCamera::tMatrixCamera(const Mat4* initialMatrix) {
    if (initialMatrix) {
        cameraMatrix = *initialMatrix;
    }
    // worldToCamera starts as identity
}

tMatrixCamera::~tMatrixCamera() = default;

// PSX: SetCameraMatrix__13tMatrixCameraP6MATRIX (0x8009D670)
void tMatrixCamera::SetCameraMatrix(const Mat4& ctw) {
    cameraMatrix = ctw;

    // PC: pre-compute world-to-camera by inverting the orthogonal camera-to-world.
    // R_inv = R^T, T_inv = -R^T * T
    // Then negate row 2 (the Z row) to convert PSX left-handed (+Z forward)
    // to GL right-handed (-Z forward). This is equivalent to:
    //   V_gl = diag(1, 1, -1, 1) * R^T_padded
    worldToCamera.m[0]  =  ctw.m[0];
    worldToCamera.m[1]  =  ctw.m[4];
    worldToCamera.m[2]  = -ctw.m[8];   // Z-negate
    worldToCamera.m[3]  =  0.0f;
    worldToCamera.m[4]  =  ctw.m[1];
    worldToCamera.m[5]  =  ctw.m[5];
    worldToCamera.m[6]  = -ctw.m[9];   // Z-negate
    worldToCamera.m[7]  =  0.0f;
    worldToCamera.m[8]  =  ctw.m[2];
    worldToCamera.m[9]  =  ctw.m[6];
    worldToCamera.m[10] = -ctw.m[10];  // Z-negate
    worldToCamera.m[11] =  0.0f;
    f32 tx = ctw.m[12], ty = ctw.m[13], tz = ctw.m[14];
    worldToCamera.m[12] = -(worldToCamera.m[0]*tx + worldToCamera.m[4]*ty + worldToCamera.m[8]*tz);
    worldToCamera.m[13] = -(worldToCamera.m[1]*tx + worldToCamera.m[5]*ty + worldToCamera.m[9]*tz);
    worldToCamera.m[14] = -(worldToCamera.m[2]*tx + worldToCamera.m[6]*ty + worldToCamera.m[10]*tz);
    worldToCamera.m[15] =  1.0f;
}

// PSX: UpdateMatrix__13tMatrixCamera (0x8009D6A0)
// PSX copies to global CVM for GTE. PC: no-op, worldToCamera already computed.
void tMatrixCamera::UpdateMatrix() {
    // PC: worldToCamera is computed in SetCameraMatrix.
    // SetState pushes it to pddi when rendering.
}

// PSX: GetPosition__13tMatrixCameraP9_RMVECT16 (0x8009D6C8)
void tMatrixCamera::GetPosition(LVector* out) const {
    out->x = (s32)(cameraMatrix.GetTransX());
    out->y = (s32)(cameraMatrix.GetTransY());
    out->z = (s32)(cameraMatrix.GetTransZ());
}

// PC: tMatrixCamera SetState pushes projection + view
void tMatrixCamera::SetState() {
    // Projection from base tCamera
    tCamera::SetState();
    // View matrix from the pre-computed inverse
    p3d::context->SetViewMatrix(worldToCamera);
}

// --- t2PointMatrixCamera (T2POINTCAM.CPP) ---

// PSX: _19t2PointMatrixCameraP6MATRIX (0x8009D3DC)
t2PointMatrixCamera::t2PointMatrixCamera(const Mat4* initialMatrix)
    : tMatrixCamera(initialMatrix)
    , position{}
    , target{0, -65536, 0}  // PSX default: target.y = -65536 (-1.0 in Q16)
    , twist(0) {
}

t2PointMatrixCamera::~t2PointMatrixCamera() = default;

void t2PointMatrixCamera::SetPosition(const LVector* pos) { position = *pos; }
void t2PointMatrixCamera::SetTarget(const LVector* tgt) { target = *tgt; }
void t2PointMatrixCamera::SetTwist(u16 t) { twist = t; }

void t2PointMatrixCamera::GetPosition(LVector* out) const { *out = position; }
void t2PointMatrixCamera::GetTarget(LVector* out) const { *out = target; }
u16 t2PointMatrixCamera::GetTwist() const { return twist; }

// PSX: UpdateMatrix (0x8009D4F4)
// Builds heading matrix from position toward target, applies twist rotation,
// fills translation, then stores as the camera matrix.
void t2PointMatrixCamera::UpdateMatrix() {
    LVector heading;
    heading.x = target.x - position.x;
    heading.y = target.y - position.y;
    heading.z = target.z - position.z;

    rmV3Normalize(&heading, &heading);

    LVector up;
    if (heading.x == 0 && heading.z == 0) {
        up = {0x10000, 0, 0};
    } else {
        up = {0, 0x10000, 0};
    }

    // Convert to float for PC matrix operations
    Vec3 headingF(FIX16_TO_FLOAT(heading.x), FIX16_TO_FLOAT(heading.y), FIX16_TO_FLOAT(heading.z));
    Vec3 upF(FIX16_TO_FLOAT(up.x), FIX16_TO_FLOAT(up.y), FIX16_TO_FLOAT(up.z));

    // PSX: p3dFillHeadingMatrix, p3dBuildRotMatrixZ, p3dMultMatrix, p3dFillTransMatrix
    Mat4 headingMatrix;
    p3dFillHeadingMatrix(headingF, upF, headingMatrix);

    Mat4 twistMatrix;
    p3dBuildRotMatrixZ(ANGLE2RAD(twist), twistMatrix);

    // Roll around the camera's forward axis, not the world Z axis.
    Mat4 combined = headingMatrix * twistMatrix;
    p3dFillTransMatrix(position, combined);

    // Store as our camera matrix (triggers worldToCamera computation)
    SetCameraMatrix(combined);
}

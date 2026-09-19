#pragma once
#include "core.h"
#include "p3d/lvector.h"
#include "gen/cammgr.h" // CameraAnchor
#include "p3d/camera.h" // tCamera, tMatrixCamera, t2PointMatrixCamera, G_2ptcam

class Thing;
class AnimStructure;

// Camera mode identifiers (PSX SetMode parameter)
enum CameraMode : s32 {
    CAM_MODE_DEFAULT = 0, // _DebugCam = free-look via pad input
    CAM_MODE_FOLLOW = 1, // _FollowPath = spline follow + track target
    CAM_MODE_RIGID = 2, // _RigidCam = fixed offset from target
};

// EvalCubic = cubic Hermite interpolation (FXP.CPP:81)
// Smoothly moves *curValue toward target, updating *accel.
// Returns true if curValue reached (or overshot) target.
bool EvalCubic(s32* curValue, s32* accel, s32 target, s32 velocity, s32 time);

class Camera {
public:
    // World coordinates span thousands of units. Near=1 collapses distant
    // surfaces into the same values in the PSP's 16-bit depth buffer.
#if defined(RC_PLATFORM_PSP)
    static constexpr f32 kRenderNearPlane = 64.0f;
#else
    static constexpr f32 kRenderNearPlane = 1.0f;
#endif
    Camera();
    ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    void Reset();
    void PurgeAnims();
    void UpdateAnim();

    void Think();
    void Move();
    void Update();
    void LookAtTarget(const LVector* target);

#if HIGH_FPS_PLAY_PRESENTATION
    void BeginLogicStepHighFPS();
    void UpdateHighFPS();
#endif

    void SetMode(CameraMode mode);

    void SetFOV(s32 fov);
    void SetCurFOV(s32 fov);
    void SetMovementTime(const LVector* t);
    void SetTrackingTime(const LVector* t);
    void SetLookAtTarget(Thing* thing, u16 mode);
    void SetLookAtMode(s32 mode) { lookAtMode = mode; }
    void SetCollisionEnabled(s32 enabled) { hasCollision = enabled; }
    void SetShakeStrength(s32 x, s32 y, s32 z) { shakeStrength = { x, y, z }; }
    void ShakeCamera(s32 frames);

    // Camera animation (used by Director for cutscene cameras)
    void LoadAsyncAnim(s32 animEnum);
    void PlayAsyncAnim();
    void DeleteAsyncAnim();
    LVector GetCameraVector() const;
    bool HasAsyncAnimLoaded() const { return asyncAnim != nullptr; }
    bool HasActiveCameraAnim() const { return cameraAnim != nullptr; }
    bool HasCollisionEnabled() const { return hasCollision != 0; }
    bool IsCameraAnimComplete() const;

    // Accessors
    tMatrixCamera* GetP3DCamera() { return &p3dCamera; }
    const LVector& GetPosition() const { return position; }  // +28
    CameraMode GetMode() const { return currentMode; }
    s32 GetCurFOV() const { return curFOV; }
    s32 GetDesiredFOV() const { return desiredFOV; }
    const LVector& GetTargetPos() const { return targetPos; }
    s32 GetOrientY() const { return orientAngles.y; }  // +44
    s32 GetCamAngleY() const { return camAngleY; }     // +384

    // Projects world coordinates into screen-space pixels.
    // Returns true when point is in front of camera and within the viewport.
    bool WorldToScreen(const LVector& worldPos, f32* outScreenX, f32* outScreenY, f32* outDepth = nullptr) const;

    // cameraAnchor assigned by CameraManager::SetupPaths
    void SetCameraAnchor(CameraAnchor* a) { cameraAnchor = a; }

    // Set all position fields at once (for initial placement)
    void SetPosition(s32 x, s32 y, s32 z) {
        position = { x, y, z };
        curPos = { x, y, z };
        prevPosition = { x, y, z };
    }

    // PSX Director camera opcode '7' writes only position + prevPosition.
    void SetPositionAndPrev(s32 x, s32 y, s32 z) {
        position = { x, y, z };
        prevPosition = { x, y, z };
    }

private:
    // Mode dispatch functions
    void DebugCam();                           // 0x80048718 = mode 0
    void FollowPath();                         // 0x80048AC0 = mode 1
    void RigidCam();                           // 0x8004897C = mode 2
    void CameraShake();                        // 0x80049DEC

    // --- DynamicThing base fields (PSX offsets +0..+199) ---
    // +0..+27: DynamicThing vtable & type (not replicated)

    // +28,+32,+36: world position
    LVector position = {};

    // +40,+44,+48: orientation (angles, used during Update path 1)
    LVector orientAngles = {};

    // +124,+128,+132: previous position (saved each Move)
    LVector prevPosition = {};

    // +200: hasCollision flag = controls whether Move() is called
    s32 hasCollision = 0;

    // --- Camera-specific fields (PSX offsets +204..+491) ---

    // +204,+208,+212: current camera position (interp target for eye)
    LVector curPos = {};

    // +216,+220,+224: look-at target position (interp target for target)
    LVector targetPos = {};

    // +228,+232,+236: previous target position
    LVector prevTargetPos = {};

    // +240,+244,+248: movement acceleration (EvalCubic state for position)
    LVector movementAccel = {};

    // +252,+256,+260: movement velocity
    LVector movementVel = {};

    // +264,+268,+272: tracking acceleration (EvalCubic state for target)
    LVector trackingAccel = {};

    // +276,+280,+284: tracking velocity
    LVector trackingVel = {};

    // +288,+292,+296: (unused/reserved in reversal)

    // +300,+304,+308: movement time per axis (SetMovementTime: input * 1000)
    LVector movementTime = {};

    // +312,+316,+320: tracking time per axis
    LVector trackingTime = {};

    // +324: current FOV (internal fixed-point units)
    s32 curFOV = 0;

    // +328: desired FOV (user-facing value, e.g. 10)
    s32 desiredFOV = 0;

    // +332: FOV acceleration (EvalCubic state)
    s32 fovAccel = 0;

    // +336: FOV velocity
    s32 fovVel = 0;

    // +340: twist angle (PSX u16 angle, 8192 = ~45 degrees)
    s32 twist = 0;

    // +344,+348,+352: velocity magnitudes per axis (init 1966 in Reset)
    LVector velocityMag = {};

    // +356: pointer to target Thing
    Thing* targetThing = nullptr;

    // +360: look-at joint index (-1 = none)
    s32 lookAtJoint = -1;

    // +364: look-at mode
    s32 lookAtMode = 0;

    // --- Mode dispatch (PSX OrderHandler at +368) ---
    // +368: thisOffset (s16) = added to 'this' before calling mode func
    // +370: modeIndex (s16) = -1=direct call, 0=skip, >0=vtable index
    // +372: function pointer / vtable base
    // On PC we use CameraMode enum + function pointer instead.
    CameraMode currentMode = CAM_MODE_DEFAULT;
    using ModeFn = void (Camera::*)();
    ModeFn modeFunc = nullptr;

    // +376,+378: quadrant flags (set by LookAtTarget)
    s16 quadrantYZ = 0;
    s16 quadrantXZ = 0;

    // +380,+384,+388: camera Euler angles (PSX u16 angle units)
    // Used in LookAtTarget output and Update path 1
    s32 camAngleX = 0; // pitch
    s32 camAngleY = 0; // yaw
    s32 camAngleZ = 0; // roll

    // +392: CameraAnchor = owns camera rail paths for current level
    CameraAnchor* cameraAnchor = nullptr;

    // +396: (reserved)

    // +400: shake frames remaining
    s32 shakeFrames = 0;

    // +404: embedded tMatrixCamera (PSX: 56 bytes)
    tMatrixCamera p3dCamera;

    // +460: async anim enum (0xFFFF = none)
    u16 asyncAnimEnum = 0xFFFF;

    // +464: async anim pointer (0 = none) - raw animation data from CharacterManager
    void* asyncAnim = nullptr;

    // +468: camera anim (AnimStructure*, nullptr = use angle-based path in Update)
    AnimStructure* cameraAnim = nullptr;

    // +472: flags bitfield
    // bit 0: path-follow position mode (1 = skip position interpolation in Move)
    // bit 1: look-at-target mode (1 = call LookAtTarget from Move)
    u32 flags = 0;

    // +476: (reserved)

    // +480,+484,+488: shake strength per axis (init 100 in Reset)
    LVector shakeStrength = {};

#if HIGH_FPS_PLAY_PRESENTATION
    LVector highFpsPrevPosition = {};
    LVector highFpsPrevTargetPos = {};
    s32 highFpsPrevCamAngleX = 0;
    s32 highFpsPrevCamAngleY = 0;
    s32 highFpsPrevCamAngleZ = 0;
    LVector highFpsPrevAnimPos = {};
    LVector highFpsPrevAnimTarget = {};
    u16 highFpsPrevAnimTwist = 0;
    s32 highFpsPrevAnimFovA = 0;
    s32 highFpsPrevAnimFovB = 0;
    bool highFpsSampleValid = false;
#endif

};

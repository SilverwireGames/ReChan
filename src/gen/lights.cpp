#include "common.h"
#include "ai/thing.h"
#include "gen/blockmgr.h"
#include "gen/camera.h"
#include "gen/database.h"
#include "gen/display.h"
#include "gen/lights.h"
#include "gen/model.h"
#include "p3d/view.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static u32 s_worldAmbientLightColour = 0x585858;
static u32 s_portAmbientLightColour = 0x585858;
static HardwareLight s_portHardwareLights[3];

struct LightBlockRange {
    u16 count = 0;
    u16 offset = 0;
};

static u32 s_lightSphereCountA = 0;
static u32 s_lightSphereCountB = 0;
static u32 s_lightBlockIndexCapacity = 0;
static u32 s_lightBlockRangeCount = 0;
static u16* s_lightBlockIndices = nullptr;
static LightBlockRange* s_lightBlockRanges = nullptr;
static u32 s_humanoidAmbientAdd[3] = {};
// PSX globals (splat_human_rclamp block) default ambient clamps to 0xF0
static u32 s_humanoidAmbientClamp[3] = { 240, 240, 240 };
static u32 s_worldAmbientAdd[3] = {};
static u32 s_worldAmbientClamp[3] = { 240, 240, 240 };

static bool LightPointIsInsideBounds(const LVector& bboxMin, const LVector& bboxMax, const LVector& point) {
    return point.x >= bboxMin.x && point.x <= bboxMax.x && point.y >= bboxMin.y && point.y <= bboxMax.y
        && point.z >= bboxMin.z && point.z <= bboxMax.z;
}

static void ParseRGBStringClamped(const char* text, u8* r, u8* g, u8* b) {
    u32 red = 0;
    u32 green = 0;
    u32 blue = 0;
    if (text) {
        std::sscanf(text, "%u,%u,%u", &red, &green, &blue);
    }

    if (red >= 0x100u) {
        red = 255;
    }
    if (green >= 0x100u) {
        green = 255;
    }
    if (blue >= 0x100u) {
        blue = 255;
    }

    *r = (u8)red;
    *g = (u8)green;
    *b = (u8)blue;
}

static void ParseRGBStringRaw(const char* text, u8* r, u8* g, u8* b) {
    s32 red = 0;
    s32 green = 0;
    s32 blue = 0;
    if (text) {
        std::sscanf(text, "%d,%d,%d", &red, &green, &blue);
    }

    *r = (u8)red;
    *g = (u8)green;
    *b = (u8)blue;
}

static s32 ParseFixedString(const char* text) {
    if (!text || !*text) {
        return 0;
    }

    const double value = std::strtod(text, nullptr);
    return (s32)(value * 65536.0);
}

static s32 AbsLightDelta(s32 value) {
    return (value < 0) ? -value : value;
}

static u32 PackRGB(u32 r, u32 g, u32 b) {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16);
}

LightColourVolume::LightColourVolume() {
    MARKFUNCTION(0x800A3B98);
}

LightColourVolume::~LightColourVolume() = default;

bool LightColourVolume::IsInside(const LVector& point) const {
    return LightPointIsInsideBounds(bboxMin, bboxMax, point);
}

HardwareLightVolume::HardwareLightVolume() {
    MARKFUNCTION(0x800A3E08);
}

HardwareLightVolume::~HardwareLightVolume() = default;

bool HardwareLightVolume::IsInside(const LVector& point) const {
    return LightPointIsInsideBounds(bboxMin, bboxMax, point);
}

LightAnchor::LightAnchor() {
    MARKFUNCTION(0x800A39E8);
}

LightAnchor::~LightAnchor() {
    MARKFUNCTION(0x800A3A34);
    delete[] radialLights;
    radialLights = nullptr;
    radialLightCount = 0;
}

void LightAnchor::SetupLightMemory(u32 count) {
    MARKFUNCTION(0x800A3B10);

    delete[] radialLights;
    radialLights = nullptr;
    radialLightCount = count;

    if (count != 0) {
        radialLights = new RadialLight[count]();
    }
}

void LightAnchor::AddColourVolume(const DBVolume* volume) {
    MARKFUNCTION(0x800A3B98);

    if (!volume) {
        return;
    }

    LightColourVolume* lightVolume = new LightColourVolume();
    lightVolume->bboxMin = volume->bboxMin;
    lightVolume->bboxMax = volume->bboxMax;

    for (u32 i = 0; i < volume->attribCount; i++) {
        const DBAttrib* attrib = volume->GetAttribByIndex(i);
        if (!attrib) {
            continue;
        }

        switch (attrib->id) {
            case 4:
                ParseRGBStringClamped(
                    attrib->GetAttribString(), &lightVolume->colourR, &lightVolume->colourG, &lightVolume->colourB);
                break;

            case 5: {
                const s32 parsed = attrib->GetAttribString() ? std::atoi(attrib->GetAttribString()) : 0;
                lightVolume->falloff = parsed;
                if (lightVolume->falloff > 0x10000) {
                    lightVolume->falloff = 0x10000;
                }
                break;
            }

            case 6:
                ParseRGBStringRaw(attrib->GetAttribString(), &lightVolume->addR, &lightVolume->addG, &lightVolume->addB);
                break;

            default:
                break;
        }
    }

    colourVolumes.AddNodeTail(lightVolume);
}

void LightAnchor::AddHardwareLightVolume(const DBVolume* volume) {
    MARKFUNCTION(0x800A3E08);

    if (!volume) {
        return;
    }

    HardwareLightVolume* lightVolume = new HardwareLightVolume();
    lightVolume->bboxMin = volume->bboxMin;
    lightVolume->bboxMax = volume->bboxMax;

    for (u32 i = 0; i < volume->attribCount; i++) {
        const DBAttrib* attrib = volume->GetAttribByIndex(i);
        if (!attrib) {
            continue;
        }

        switch (attrib->id) {
            case 4:
                ParseRGBStringClamped(
                    attrib->GetAttribString(), &lightVolume->colourR, &lightVolume->colourG, &lightVolume->colourB);
                break;

            case 5:
                lightVolume->innerRadius = ParseFixedString(attrib->GetAttribString());
                break;

            case 6:
                lightVolume->outerRadius = ParseFixedString(attrib->GetAttribString());
                break;

            default:
                break;
        }
    }

    hardwareLightVolumes.AddNodeTail(lightVolume);
}

bool LightAnchor::SetupLight(RadialLight* light, const DBSphere* sphere) {
    MARKFUNCTION(0x800A400C);

    if (!light || !sphere) {
        return false;
    }

    light->posX = sphere->pos.x;
    light->posY = sphere->pos.y;
    light->posZ = sphere->pos.z;
    light->radius = (u16)sphere->radius;
    light->flags &= 0xFEu;

    for (u32 i = 0; i < sphere->attribCount; i++) {
        const DBAttrib* attrib = sphere->GetAttribByIndex(i);
        if (!attrib) {
            continue;
        }

        switch (attrib->id) {
            case 4:
                ParseRGBStringClamped(attrib->GetAttribString(), &light->colourR, &light->colourG, &light->colourB);
                break;

            case 8: {
                const s32 enabled = attrib->GetAttribString() ? std::atoi(attrib->GetAttribString()) : 0;
                if (enabled != 0) {
                    light->flags |= 1u;
                }
                else {
                    light->flags &= 0xFEu;
                }
                break;
            }

            default:
                break;
        }
    }

    return true;
}

bool LightAnchor::AddRadialHardwareLight(const DBSphere* sphere, u32 index) {
    MARKFUNCTION(0x800A4194);

    if (!sphere || !radialLights || index >= radialLightCount) {
        return false;
    }

    s32 field5 = 0;
    s32 field6 = 0;
    SetupLight(&radialLights[index], sphere);

    for (u32 i = 0; i < sphere->attribCount; i++) {
        const DBAttrib* attrib = sphere->GetAttribByIndex(i);
        if (!attrib) {
            continue;
        }

        if (attrib->id == 5) {
            field5 = ParseFixedString(attrib->GetAttribString());
        }
        else if (attrib->id == 6) {
            field6 = ParseFixedString(attrib->GetAttribString());
        }
    }

    (void)field5;
    (void)field6;
    return true;
}

AmbientLight::AmbientLight() {
    MARKFUNCTION(0x800A4264);
    SetToWorldAmbient();
}

AmbientLight::~AmbientLight() {
    MARKFUNCTION(0x800A4298);
}

void AmbientLight::SetToWorldAmbient() {
    MARKFUNCTION(0x800A42CC);
    desiredColour = s_worldAmbientLightColour;
}

void AmbientLight::SetDesired(u32 colour) {
    MARKFUNCTION(0x800A42E4);
    desiredColour = colour;
}

void AmbientLight::SetPortToLight() {
    MARKFUNCTION(0x800A42EC);
    s_portAmbientLightColour = desiredColour;
}

HardwareLight::HardwareLight() {
    MARKFUNCTION(0x800A4328);
}

HardwareLight::~HardwareLight() {
    MARKFUNCTION(0x800A4350);
}

void HardwareLight::SetLight(u32 newColour, const LVector* direction) {
    MARKFUNCTION(0x800A4384);
    colour = newColour;
    if (!direction) {
        directionX = 0;
        directionY = 0;
        directionZ = 0;
        return;
    }

    directionX = direction->x;
    directionY = direction->y;
    directionZ = direction->z;
}

const HardwareLight* GetCurrentPortHardwareLight(s32 slot) {
    if (slot < 0 || slot >= 3) {
        return nullptr;
    }

    return &s_portHardwareLights[slot];
}

LightingClass::LightingClass() {
    MARKFUNCTION(0x800A2720);
    Reset();
}

LightingClass::~LightingClass() {
    MARKFUNCTION(0x800A27A4);
    // PC: the view lives in Display, which Game::~Game deletes before EnvironmentManager.
    view = nullptr;
    Reset();
}

void LightingClass::Reset() {
    MARKFUNCTION(0x800A2864);
    slotHandles[0] = 0;
    slotHandles[1] = 0;
    slotHandles[2] = 0;
    for (s32 i = 0; i < 3; i++) {
        slotLights[i] = &s_portHardwareLights[i];
        s_portHardwareLights[i] = HardwareLight();
    }
    worldAmbientColour = 0x585858;
    delete[] s_lightBlockIndices;
    s_lightBlockIndices = nullptr;
    delete[] s_lightBlockRanges;
    s_lightBlockRanges = nullptr;
    s_lightBlockRangeCount = 0;
    delete lightAnchor;
    lightAnchor = nullptr;
    s_worldAmbientLightColour = worldAmbientColour;
    s_portAmbientLightColour = s_worldAmbientLightColour;
    if (view) {
        const pddiColour ambient(
            (u8)(worldAmbientColour & 0xFF),
            (u8)((worldAmbientColour >> 8) & 0xFF),
            (u8)((worldAmbientColour >> 16) & 0xFF),
            255);
        view->SetAmbientLight(ambient);
    }
}

void LightingClass::InternalOpen(tView* newView) {
    view = newView;
}

void LightingClass::AnalyzeSphere(DBPoint* point) {
    MARKFUNCTION(0x800A2538);

    if (!point) {
        return;
    }

    u32 value = 0;
    if (point->subType == 65533) {
        if (point->FindAttribValue(3, &value)) {
            s_lightSphereCountA = value;
        }
        if (point->FindAttribValue(4, &value)) {
            s_lightSphereCountB = value;
        }
        if (point->FindAttribValue(5, &value)) {
            s_lightBlockIndexCapacity = value;
        }
        return;
    }

    if (s_lightBlockRanges) {
        return;
    }

    u32 blockCount = g_blockManager ? g_blockManager->GetNumBlocks() : 0;
    u32 inferredBlockCount = 0;
    for (DBPoint* current = point; current; current = static_cast<DBPoint*>(current->next)) {
        if (current->subType != 65534) {
            continue;
        }

        u32 blockIndex = 0;
        if (!current->FindAttribValue(5, &blockIndex)) {
            continue;
        }

        const u32 candidateCount = blockIndex + 1;
        if (candidateCount > inferredBlockCount) {
            inferredBlockCount = candidateCount;
        }
    }
    if (inferredBlockCount > blockCount) {
        blockCount = inferredBlockCount;
    }
    if (blockCount == 0) {
        return;
    }

    s_lightBlockRangeCount = blockCount;
    s_lightBlockRanges = new LightBlockRange[blockCount]();
    s_lightBlockIndices = (s_lightBlockIndexCapacity != 0) ? new u16[s_lightBlockIndexCapacity]() : nullptr;

    u16 nextOffset = 0;
    for (DBPoint* current = point; current; current = static_cast<DBPoint*>(current->next)) {
        if (current->subType != 65534) {
            continue;
        }

        u32 blockIndex = 0;
        if (!current->FindAttribValue(5, &blockIndex) || blockIndex >= blockCount) {
            continue;
        }

        u32 linkCount = 0;
        if (!current->FindAttribValue(6, &linkCount)) {
            continue;
        }

        LightBlockRange& range = s_lightBlockRanges[blockIndex];
        range.offset = nextOffset;
        range.count = (u16)linkCount;

        for (u32 i = 0; i < linkCount && nextOffset < s_lightBlockIndexCapacity; i++) {
            u32 linkValue = 0;
            if (current->FindAttribValue(i + 7, &linkValue) && s_lightBlockIndices) {
                s_lightBlockIndices[nextOffset++] = (u16)linkValue;
            }
        }
    }
}

s32 LightingClass::DoModelLighting(Thing* thing) {
    MARKFUNCTION(0x800A3110);
    FindAmbientVolumes(thing);
    return FindHardwareVolumes(thing);
}

void LightingClass::SetupLighting() {
    MARKFUNCTION(0x800A2BFC);

    lightAnchor = new LightAnchor();

    lightAnchor->SetupLightMemory(s_lightSphereCountA + s_lightSphereCountB);

    static const LVector defaultLightDirs[3] = {
        { 0, -65535, 0 },
        { 0, -65536, 0 },
        { 0, -65536, 0 },
    };

    for (s32 i = 0; i < 3; i++) {
        const u32 fallbackColour = (i == 0) ? 0x00F0F0F0 : 0x00000000;
        originalLights[i].SetLight(fallbackColour, &defaultLightDirs[i]);
    }

    if (g_database) {
        for (DBVolume* volume = g_database->GetFirstVolume(); volume; volume = static_cast<DBVolume*>(volume->next)) {
            if (volume->subType == 101) {
                lightAnchor->AddColourVolume(volume);
            }
            else if (volume->subType == 102) {
                lightAnchor->AddHardwareLightVolume(volume);
            }
            else {
                const char* name = volume->GetName();
                if (name && rcStricmp(name, "StageVolume") == 0) {
                    SetupStageAttributes(volume);
                }
            }
        }
    }

    if (g_database && lightAnchor) {
        u32 radialIndex = 0;
        for (DBSphere* sphere = g_database->GetFirstSphere(); sphere; sphere = static_cast<DBSphere*>(sphere->next)) {
            if (sphere->subType != 104) {
                continue;
            }

            lightAnchor->AddRadialHardwareLight(sphere, radialIndex);
            radialIndex++;
        }
    }
}

void LightingClass::FindAmbientVolumes(Thing* thing) {
    MARKFUNCTION(0x800A314C);

    if (!thing || !thing->model || !lightAnchor) {
        return;
    }

    Model* model = static_cast<Model*>(thing->model);
    AmbientLight* ambient = static_cast<AmbientLight*>(model->ambientLight);
    if (!ambient) {
        return;
    }

    if (!s_lightBlockRanges || !s_lightBlockIndices || !lightAnchor->radialLights
        || thing->blockNum >= s_lightBlockRangeCount) {
        return;
    }

    s32 baseR = 128;
    s32 baseG = 128;
    s32 baseB = 128;
    s32 addR = 0;
    s32 addG = 0;
    s32 addB = 0;

    for (ccMinNode* node = lightAnchor->colourVolumes.head; node; node = node->next) {
        LightColourVolume* volume = static_cast<LightColourVolume*>(node);
        if (!volume->IsInside(thing->pos)) {
            continue;
        }

        baseR = volume->colourR;
        baseG = volume->colourG;
        baseB = volume->colourB;
        addR = volume->addR;
        addG = volume->addG;
        addB = volume->addB;
        break;
    }

    const LightBlockRange& range = s_lightBlockRanges[thing->blockNum];
    s32 accumR = 0;
    s32 accumG = 0;
    s32 accumB = 0;
    s32 contributionCount = 0;

    for (u32 i = 0; i < range.count && contributionCount < 2; i++) {
        const u16 lightIndex = s_lightBlockIndices[range.offset + i];
        if (lightIndex >= lightAnchor->radialLightCount) {
            continue;
        }

        const RadialLight& light = lightAnchor->radialLights[lightIndex];
        if (((light.flags >> 2) & 1u) == 0) {
            continue;
        }

        s32 distance = AbsLightDelta(light.posX - thing->pos.x) + AbsLightDelta(light.posZ - thing->pos.z);
        if ((light.flags & 1u) != 0) {
            distance += AbsLightDelta(light.posY - thing->pos.y);
        }

        if ((s32)light.radius < distance) {
            continue;
        }

        accumR += light.colourR;
        accumG += light.colourG;
        accumB += light.colourB;
        contributionCount++;
    }

    if (contributionCount == 1) {
        baseR = accumR;
        baseG = accumG;
        baseB = accumB;
    }
    else if (contributionCount >= 2) {
        baseR = accumR >> 1;
        baseG = accumG >> 1;
        baseB = accumB >> 1;
    }

    u32 finalR = 0;
    u32 finalG = 0;
    u32 finalB = 0;
    if (thing->thingType >= 29) {
        finalR = (u32)(baseR + addR + (s32)s_worldAmbientAdd[0]);
        finalG = (u32)(baseG + addG + (s32)s_worldAmbientAdd[1]);
        finalB = (u32)(baseB + addB + (s32)s_worldAmbientAdd[2]);
        if (finalR > s_worldAmbientClamp[0]) finalR = s_worldAmbientClamp[0];
        if (finalG > s_worldAmbientClamp[1]) finalG = s_worldAmbientClamp[1];
        if (finalB > s_worldAmbientClamp[2]) finalB = s_worldAmbientClamp[2];
    }
    else {
        finalR = (u32)(baseR + (s32)s_humanoidAmbientAdd[0]);
        finalG = (u32)(baseG + (s32)s_humanoidAmbientAdd[1]);
        finalB = (u32)(baseB + (s32)s_humanoidAmbientAdd[2]);
        if (finalR > s_humanoidAmbientClamp[0]) finalR = s_humanoidAmbientClamp[0];
        if (finalG > s_humanoidAmbientClamp[1]) finalG = s_humanoidAmbientClamp[1];
        if (finalB > s_humanoidAmbientClamp[2]) finalB = s_humanoidAmbientClamp[2];
    }

    ambient->SetDesired(PackRGB(finalR, finalG, finalB));
}

void LightingClass::ComputeLightDir(LVector* direction) {
    MARKFUNCTION(0x800A34A0);

    if (!direction) {
        return;
    }

    if (!g_display || !g_display->GetCamera()) {
        return;
    }

    const Camera* camera = g_display->GetCamera();
    const LVector& cameraPos = camera->GetPosition();
    const LVector& cameraTarget = camera->GetTargetPos();
    const s32 deltaX = cameraTarget.x - cameraPos.x;
    const s32 deltaZ = cameraTarget.z - cameraPos.z;
    const s32 absX = AbsLightDelta(deltaX);
    const s32 absZ = AbsLightDelta(deltaZ);

    if ((absX >> 1) < absZ) {
        direction->z = (deltaZ <= 0) ? -30559 : 30559;
    }
    else {
        direction->z = 0;
    }

    if ((absZ >> 1) < absX) {
        direction->x = (deltaX <= 0) ? -30559 : 30559;
    }
    else {
        direction->x = 0;
    }
    direction->y = -65535;
}

s32 LightingClass::FindHardwareVolumes(Thing* thing) {
    MARKFUNCTION(0x800A3550);

    if (!thing || !thing->model || !lightAnchor) {
        return 240;
    }

    Model* model = static_cast<Model*>(thing->model);
    HardwareLight* modelLights = static_cast<HardwareLight*>(model->hwLights);
    if (!modelLights || model->hwLightCount <= 0) {
        return 240;
    }

    if (!s_lightBlockRanges || !s_lightBlockIndices || !lightAnchor->radialLights
        || thing->blockNum >= s_lightBlockRangeCount) {
        return 240;
    }

    for (s32 i = 0; i < model->hwLightCount; i++) {
        modelLights[i].active = 0;
    }

    s32 baseR = 240;
    s32 baseG = 240;
    s32 baseB = 240;
    LVector direction = { 0, -65535, 0 };

    for (ccMinNode* node = lightAnchor->hardwareLightVolumes.head; node; node = node->next) {
        HardwareLightVolume* volume = static_cast<HardwareLightVolume*>(node);
        if (!volume->IsInside(thing->pos)) {
            continue;
        }

        baseR = volume->colourR;
        baseG = volume->colourG;
        baseB = volume->colourB;
        direction.x = volume->directionX;
        direction.y = volume->directionY;
        direction.z = volume->directionZ;
        break;
    }

    s32 accumR = 0;
    s32 accumG = 0;
    s32 accumB = 0;
    s32 contributionCount = 0;
    bool useComputedDirection = false;

    const LightBlockRange& range = s_lightBlockRanges[thing->blockNum];
    for (u32 i = 0; i < range.count; i++) {
        const u16 lightIndex = s_lightBlockIndices[range.offset + i];
        if (lightIndex >= lightAnchor->radialLightCount) {
            continue;
        }

        const RadialLight& light = lightAnchor->radialLights[lightIndex];
        if (((light.flags >> 2) & 1u) != 0) {
            continue;
        }

        s32 distance = AbsLightDelta(light.posX - thing->pos.x) + AbsLightDelta(light.posZ - thing->pos.z);
        const s32 innerRadius = ((s32)light.radius >> 2) + ((s32)light.radius >> 4);
        if (innerRadius < distance) {
            if ((s32)light.radius < distance) {
                continue;
            }

            const s32 ratio = rmDiv16i(distance - innerRadius, (s32)light.radius - innerRadius);
            accumR += baseR + (s32)(((0x10000 - ratio) * (s64)((s32)light.colourR - baseR)) >> 16);
            accumG += baseG + (s32)(((0x10000 - ratio) * (s64)((s32)light.colourG - baseG)) >> 16);
            accumB += baseB + (s32)(((0x10000 - ratio) * (s64)((s32)light.colourB - baseB)) >> 16);
        }
        else {
            accumR += light.colourR;
            accumG += light.colourG;
            accumB += light.colourB;
        }

        contributionCount++;
        if (!useComputedDirection) {
            useComputedDirection = true;
            ComputeLightDir(&direction);
        }
    }

    if (contributionCount > 0) {
        if (contributionCount >= 3) {
            const s32 reciprocal = rmDiv16i(0x10000, contributionCount);
            accumR = (s32)(((s64)accumR * reciprocal) >> 16);
            accumG = (s32)(((s64)accumG * reciprocal) >> 16);
            accumB = (s32)(((s64)accumB * reciprocal) >> 16);
        }
        else {
            const s32 shift = contributionCount - 1;
            accumR >>= shift;
            accumG >>= shift;
            accumB >>= shift;
        }
    }
    else {
        accumR = baseR;
        accumG = baseG;
        accumB = baseB;
    }

    u32 finalR = (u32)accumR;
    u32 finalG = (u32)accumG;
    u32 finalB = (u32)accumB;
    ClampWithinRGBLimit(&finalR, &finalG, &finalB);

    modelLights[0].SetLight(PackRGB(finalR, finalG, finalB), &direction);
    modelLights[0].active = 1;
    return 1;
}

void LightingClass::SetupStageAttributes(const DBVolume* volume) {
    MARKFUNCTION(0x800A2E5C);

    if (!volume) {
        return;
    }

    u32 pendingR = 0;
    u32 pendingG = 0;
    u32 pendingB = 0;

    for (u32 i = 0; i < volume->attribCount; i++) {
        const DBAttrib* attrib = volume->GetAttribByIndex(i);
        if (!attrib) {
            continue;
        }

        switch (attrib->id) {
            case 3: {
                u32 r = 0;
                u32 g = 0;
                u32 b = 0;
                std::sscanf(attrib->GetAttribString(), "%u,%u,%u", &r, &g, &b);
                ClampWithinRGBLimit(&r, &g, &b);
                worldAmbientColour = (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16);
                s_worldAmbientLightColour = worldAmbientColour;
                s_portAmbientLightColour = s_worldAmbientLightColour;
                if (view) {
                    view->SetAmbientLight(pddiColour((u8)r, (u8)g, (u8)b, 255));
                }
                break;
            }

            case 4:
            case 6:
            case 8: {
                pendingR = 0;
                pendingG = 0;
                pendingB = 0;
                std::sscanf(attrib->GetAttribString(), "%u,%u,%u", &pendingR, &pendingG, &pendingB);
                ClampWithinRGBLimit(&pendingR, &pendingG, &pendingB);
                break;
            }

            case 5:
            case 7:
            case 9: {
                s32 dirX = 0;
                s32 dirY = 0;
                s32 dirZ = 0;
                std::sscanf(attrib->GetAttribString(), "%d,%d,%d", &dirX, &dirY, &dirZ);
                ClampWithinNormalLimit(&dirX, &dirY, &dirZ);

                const s32 slot = (s32)((attrib->id - 5) / 2);
                const u32 colour = (pendingR & 0xFFu) | ((pendingG & 0xFFu) << 8) | ((pendingB & 0xFFu) << 16);
                const LVector dir = { dirX, dirY, dirZ };
                if (slot >= 0 && slot < 3) {
                    originalLights[slot].SetLight(colour, &dir);
                }
                break;
            }

            default:
                break;
        }
    }
}

void LightingClass::ClampWithinRGBLimit(u32* r, u32* g, u32* b) {
    MARKFUNCTION(0x800A2D8C);

    if (*r >= 0x100u) {
        *r = 255;
    }
    if (*g >= 0x100u) {
        *g = 255;
    }
    if (*b >= 0x100u) {
        *b = 255;
    }
}

void LightingClass::ClampWithinNormalLimit(s32* x, s32* y, s32* z) {
    MARKFUNCTION(0x800A2DDC);

    if (*x < -65536) *x = -65536;
    else if (*x > 65536) *x = 65536;

    if (*y < -65536) *y = -65536;
    else if (*y > 65536) *y = 65536;

    if (*z < -65536) *z = -65536;
    else if (*z > 65536) *z = 65536;
}

u32 GetWorldAmbientLightColour() {
    return s_worldAmbientLightColour;
}

u32 GetCurrentPortAmbientLightColour() {
    return s_portAmbientLightColour;
}

void RestoreWorldAmbientLightToPort() {
    s_portAmbientLightColour = s_worldAmbientLightColour;
}

void LightingClass::SetupHLight(s32 slot, const LVector* direction, u32 colour) {
    MARKFUNCTION(0x800A2B44);

    if (slot < 0 || slot >= 3) {
        return;
    }

    s_portHardwareLights[slot].SetLight(colour, direction);
}

void LightingClass::SetHLightToOriginal(s32 slot) {
    MARKFUNCTION(0x800A2B98);

    if (slot < 0 || slot >= 3) {
        return;
    }

    s_portHardwareLights[slot] = originalLights[slot];
}

void LightingClass::AddLightToPort(s32 slot, const LVector* direction, u32 colour) {
    MARKFUNCTION(0x800A2A88);

    if (slot < 0 || slot >= 3) {
        return;
    }

    SetupHLight(slot, direction, colour);
    slotHandles[slot] = static_cast<u8>(slot);
    slotLights[slot] = &s_portHardwareLights[slot];
}

void LightingClass::RemoveLightFromPort(s32 slot) {
    MARKFUNCTION(0x800A2AEC);

    if (slot < 0 || slot >= 3) {
        return;
    }

    SetHLightToOriginal(slot);
    slotHandles[slot] = static_cast<u8>(slot);
    slotLights[slot] = &s_portHardwareLights[slot];
}

const HardwareLight* LightingClass::GetPortLight(s32 slot) const {
    if (slot < 0 || slot >= 3) {
        return nullptr;
    }

    return &s_portHardwareLights[slot];
}

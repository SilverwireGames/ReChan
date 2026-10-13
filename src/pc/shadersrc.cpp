#include "pc/shadersrc.h"

#include <cstring>
#include <string>
#include <unordered_map>

// All GLSL for both backends lives here, not in libp3d. Sources carry no
// #version line and no precision statements: each pddi backend prepends its
// own preamble and the feature defines documented in pddi/pddishadersrc.h.
//
// GL and GLES share every body. Of the twelve programs only mesh and
// shadow_depth differ between the two, and those differences are expressed
// with NOPERSP / HIGHP / HAS_* rather than a second copy of the source.

static const char* kSimpleVert = R"GLSL(
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
uniform mat4 uProj;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
)GLSL";

static const char* kSimpleFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uTint;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vUV) * uTint;
}
)GLSL";

static const char* kTiltVert = R"GLSL(
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
uniform mat4 uProj;
uniform vec4 uTiltRect;   // xy = center, zw = half width/height
uniform vec4 uTiltAngles; // x = pitch (rad), y = yaw (rad), z = roll (rad), w = focal distance
out vec2 vUV;
void main() {
    vUV = aUV;

    vec2 local = (aUV - 0.5) * 2.0 * uTiltRect.zw;
    vec3 p = vec3(local, 0.0);

    float cx = cos(uTiltAngles.x), sx = sin(uTiltAngles.x);
    p = vec3(p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx);

    float cy = cos(uTiltAngles.y), sy = sin(uTiltAngles.y);
    p = vec3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy);

    float cz = cos(uTiltAngles.z), sz = sin(uTiltAngles.z);
    p = vec3(p.x * cz - p.y * sz, p.x * sz + p.y * cz, p.z);

    float focal = max(uTiltAngles.w, 1.0);
    float perspective = focal / max(focal - p.z, 0.0001);
    vec2 screenPos = uTiltRect.xy + p.xy * perspective;

    gl_Position = uProj * vec4(screenPos, 0.0, 1.0);
}
)GLSL";

static const char* kDotFrag = R"GLSL(
in vec2 vUV;
uniform vec4 uTint;
uniform float uShapeSeed;
out vec4 FragColor;

float hash11(float v) {
    return fract(sin(v * 127.1) * 43758.5453123);
}

float angularNoise(float v) {
    float cell = floor(v);
    float blend = fract(v);
    blend = blend * blend * (3.0 - 2.0 * blend);
    return mix(hash11(cell), hash11(cell + 1.0), blend);
}

void main() {
    vec2 centered = (vUV - 0.5) * 2.0;
    float dist = length(centered);
    float angle = atan(centered.y, centered.x);

    float n1 = angularNoise(angle * 2.4 + uShapeSeed);
    float n2 = angularNoise(angle * 5.1 - uShapeSeed * 1.7 + 4.0);
    float wobble = (n1 * 0.6 + n2 * 0.4) - 0.5;
    float edge = 0.74 + wobble * 0.36;

    float alpha = 1.0 - smoothstep(edge - 0.18, edge, dist);
    if (alpha <= 0.0) discard;
    FragColor = vec4(uTint.rgb, uTint.a * alpha);
}
)GLSL";

static const char* kGlowFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uGlowParams; // x = blur radius (uv units), y = intensity
uniform vec4 uGlowMotion; // x = ring rotation speed (rad/s), y = drift radius (uv), z = drift speed (rad/s)
uniform float uTime;
out vec4 FragColor;

const int kRings = 3;
const int kDirections = 8;

void main() {
    vec2 driftedUV = vUV + vec2(cos(uTime * uGlowMotion.z), sin(uTime * uGlowMotion.z * 1.21)) * uGlowMotion.y;

    vec4 center = texture(uTex, vUV);
    vec3 color = center.rgb * center.a;
    float alpha = center.a;
    float total = 1.0;
    float rot = uTime * uGlowMotion.x;
    for (int ring = 1; ring <= kRings; ++ring) {
        float radius = uGlowParams.x * (float(ring) / float(kRings));
        float weight = 1.0 / float(ring);
        for (int dir = 0; dir < kDirections; ++dir) {
            float angle = (float(dir) / float(kDirections)) * 6.28318530718 + rot + float(ring) * 0.35;
            vec2 offset = vec2(cos(angle), sin(angle)) * radius;
            vec4 tap = texture(uTex, driftedUV + offset);
            color += tap.rgb * tap.a * weight;
            alpha += tap.a * weight;
            total += weight;
        }
    }
    color /= total;
    alpha /= total;
    FragColor = vec4(color * uGlowParams.y, alpha * uGlowParams.y);
}
)GLSL";

static const char* kGodraysFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uRayParams; // xy = light source uv, z = sample spread, w = decay per sample
uniform vec4 uRayMotion; // x = swirl twist amount (rad), y = swirl speed (rad/s), z = orbit radius (uv), w = orbit speed (rad/s)
uniform float uExposure;
uniform float uTime;
out vec4 FragColor;

const int kSampleCount = 48;

void main() {
    vec2 origin = uRayParams.xy
        + vec2(cos(uTime * uRayMotion.w), sin(uTime * uRayMotion.w * 1.37)) * uRayMotion.z;

    vec2 uv = vUV;
    vec4 source = texture(uTex, uv);
    vec3 color = source.rgb * source.a;
    float decay = 1.0;
    float stepFrac = uRayParams.z / float(kSampleCount);
    for (int i = 0; i < kSampleCount; ++i) {
        vec2 step = (origin - uv) * stepFrac;

        float twist = uRayMotion.x * sin(uTime * uRayMotion.y + float(i) * 0.22);
        float ca = cos(twist);
        float sa = sin(twist);
        step = vec2(step.x * ca - step.y * sa, step.x * sa + step.y * ca);
        uv += step;

        vec4 tap = texture(uTex, uv);
        color += tap.rgb * tap.a * decay;
        decay *= uRayParams.w;
    }
    FragColor = vec4(color * (uExposure / float(kSampleCount)), 1.0);
}
)GLSL";

static const char* kMoviesharpFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uTexel;
uniform float uSharpAmount;
out vec4 FragColor;

void main() {
    vec3 c = texture(uTex, vUV).rgb;
    vec3 n = texture(uTex, vUV + vec2(0.0, -uTexel.y)).rgb;
    vec3 s = texture(uTex, vUV + vec2(0.0,  uTexel.y)).rgb;
    vec3 e = texture(uTex, vUV + vec2( uTexel.x, 0.0)).rgb;
    vec3 w = texture(uTex, vUV + vec2(-uTexel.x, 0.0)).rgb;

    vec3 blur = (n + s + e + w) * 0.25;
    vec3 sharp = c + (c - blur) * uSharpAmount;

    FragColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
)GLSL";

static const char* kMoviedenoiseFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uTexel;
uniform float uSharpAmount;
out vec4 FragColor;

void main() {
    vec3 c = texture(uTex, vUV).rgb;

    float sigma = mix(0.02, 0.18, clamp(uSharpAmount, 0.0, 1.0));
    float invSigma2 = 1.0 / (sigma * sigma);

    vec3 result = c;
    float totalW = 1.0;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            vec2 off = vec2(float(dx), float(dy)) * uTexel.xy;
            vec3 s = texture(uTex, vUV + off).rgb;
            vec3 diff = c - s;
            float w = exp(-dot(diff, diff) * invSigma2);
            if (dx != 0 && dy != 0) w *= 0.707;
            result += s * w;
            totalW += w;
        }
    }

    FragColor = vec4(result / totalW, 1.0);
}
)GLSL";

static const char* kMovieupscaleFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uTexel;
out vec4 FragColor;

vec4 CubicWeights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return vec4(-0.5*t3 + t2 - 0.5*t,
                 1.5*t3 - 2.5*t2 + 1.0,
                -1.5*t3 + 2.0*t2 + 0.5*t,
                 0.5*t3 - 0.5*t2);
}

void main() {
    vec2 pos = vUV / uTexel.xy - 0.5;
    vec2 f = fract(pos);
    vec2 base = (floor(pos) + 0.5) * uTexel.xy;

    vec4 wx = CubicWeights(f.x);
    vec4 wy = CubicWeights(f.y);

    vec3 color = vec3(0.0);
    for (int j = 0; j < 4; j++) {
        vec3 row = vec3(0.0);
        for (int i = 0; i < 4; i++) {
            vec2 off = vec2(float(i - 1), float(j - 1)) * uTexel.xy;
            row += texture(uTex, base + off).rgb * wx[i];
        }
        color += row * wy[j];
    }
    FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
)GLSL";

static const char* kGouraudVert = R"GLSL(
layout(location=0) in vec2 aPos;
layout(location=1) in vec4 aColor;
uniform mat4 uProj;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
)GLSL";

static const char* kGouraudFrag = R"GLSL(
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
)GLSL";

static const char* kBatchVert = R"GLSL(
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform mat4 uProj;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
)GLSL";

static const char* kBatchFrag = R"GLSL(
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vUV) * vColor;
}
)GLSL";

// PSX VRAM/CLUT decode, shared verbatim by the mesh and shadow-depth fragment
// shaders (concatenated in front of each body, so both stay in step).
//
// Two implementations behind VRAM_FLOAT. Desktop GL keeps the exact integer
// path: a R16UI texture read with texelFetch and decoded with shifts/masks.
// GLES takes the float path, because the Switch emulators' shader recompilers
// do not evaluate the integer path faithfully -- artifacts there track how the
// integer maths is *phrased*, not what it computes. That path reads the same
// VRAM as RG8 (R = low byte, G = high byte) and decodes in float; every value
// involved is an exact small integer, so fp32 reproduces the integer result
// bit for bit.
static const char* kVramDecode = R"GLSL(
// Texel index within the 256x256 page, shared by both decodes.
vec2 VramTexelIndex(vec2 uv) {
    return vec2(floor(mod(uv.x + 256.0, 256.0)),
                floor(mod(uv.y + 256.0, 256.0)));
}

#ifdef VRAM_FLOAT
uniform HIGHP sampler2D uVRAM;

// PSX VRAM addressing is 10 bits of X by 9 of Y and wraps. It has to wrap here
// too: a 16bpp page at tx=15 reaches x=1215 and an 8bpp CLUT at cba x=63 reaches
// x=1263, both past the 1024-wide texture. texelFetch out of range is undefined
// in GL, and what "undefined" returns differs between drivers.
float VramWord(float x, float y) {
    vec2 rg = texelFetch(uVRAM, ivec2(int(mod(x, 1024.0)), int(mod(y, 512.0))), 0).rg;
    return floor(rg.r * 255.0 + 0.5) + floor(rg.g * 255.0 + 0.5) * 256.0;
}

float VramTexel(float tpageF, float cbaF, vec2 uv, out bool zeroTexel) {
    float tx    = mod(tpageF, 16.0);
    float ty    = mod(floor(tpageF / 16.0), 2.0);
    float depth = mod(floor(tpageF / 128.0), 4.0);

    float pageX = tx * 64.0;
    float pageY = ty * 256.0;
    float clutX = mod(cbaF, 64.0) * 16.0;
    float clutY = mod(floor(cbaF / 64.0), 512.0);

    vec2 texel = VramTexelIndex(uv);
    float px = texel.x;
    float py = texel.y;

    zeroTexel = false;
    if (depth < 0.5) {
        float word = VramWord(pageX + floor(px * 0.25), pageY + py);
        // Nibble select: divide down by 1/16/256/4096 rather than shifting.
        float lane = mod(px, 4.0);
        float scale = (lane < 0.5) ? 1.0 : (lane < 1.5) ? 16.0 : (lane < 2.5) ? 256.0 : 4096.0;
        float palIdx = mod(floor(word / scale), 16.0);
        zeroTexel = (palIdx < 0.5);
        return VramWord(clutX + palIdx, clutY);
    }
    if (depth < 1.5) {
        float word = VramWord(pageX + floor(px * 0.5), pageY + py);
        float palIdx = (mod(px, 2.0) < 0.5) ? mod(word, 256.0) : floor(word / 256.0);
        zeroTexel = (palIdx < 0.5);
        return VramWord(clutX + palIdx, clutY);
    }
    float word = VramWord(pageX + px, pageY + py);
    zeroTexel = (word < 0.5);
    return word;
}

vec3 VramColour(float clutWord) {
    return vec3(mod(clutWord, 32.0),
                mod(floor(clutWord / 32.0), 32.0),
                mod(floor(clutWord / 1024.0), 32.0)) / 31.0;
}

// 0x7C1F with the semi-transparency bit ignored: clutWord & 0x7FFF == 31775.
bool VramIsMagenta(float clutWord) {
    return abs(mod(clutWord, 32768.0) - 31775.0) < 0.5;
}
#else
uniform HIGHP usampler2D uVRAM;

// Wraps for the same reason as the float path's VramWord above.
uint VramWord(uint x, uint y) {
    return texelFetch(uVRAM, ivec2(x & 0x3FFu, y & 0x1FFu), 0).r;
}

float VramTexel(float tpageF, float cbaF, vec2 uv, out bool zeroTexel) {
    uint tpage = uint(tpageF);
    uint cba = uint(cbaF);

    uint tx = tpage & 0xFu;
    uint ty = (tpage >> 4u) & 1u;
    uint depth = (tpage >> 7u) & 3u;

    uint pageX = tx << 6u;
    uint pageY = ty << 8u;
    uint clutX = (cba & 0x3Fu) << 4u;
    uint clutY = (cba >> 6u) & 0x1FFu;

    vec2 texel = VramTexelIndex(uv);
    uint px = uint(texel.x);
    uint py = uint(texel.y);

    uint clutWord;
    zeroTexel = false;
    if (depth == 0u) {
        uint word = VramWord(pageX + (px >> 2u), pageY + py);
        uint palIdx = (word >> ((px & 3u) << 2u)) & 0xFu;
        zeroTexel = (palIdx == 0u);
        clutWord = VramWord(clutX + palIdx, clutY);
    }
    else if (depth == 1u) {
        uint word = VramWord(pageX + (px >> 1u), pageY + py);
        uint palIdx = (px & 1u) != 0u ? (word >> 8u) & 0xFFu : word & 0xFFu;
        zeroTexel = (palIdx == 0u);
        clutWord = VramWord(clutX + palIdx, clutY);
    }
    else {
        clutWord = VramWord(pageX + px, pageY + py);
        zeroTexel = (clutWord == 0u);
    }
    return float(clutWord);
}

vec3 VramColour(float clutWordF) {
    uint clutWord = uint(clutWordF);
    return vec3(float(clutWord & 0x1Fu),
                float((clutWord >> 5u) & 0x1Fu),
                float((clutWord >> 10u) & 0x1Fu)) / 31.0;
}

bool VramIsMagenta(float clutWordF) {
    return (uint(clutWordF) & 0x7FFFu) == 0x7C1Fu;
}
#endif
)GLSL";

static const char* kMeshVert = R"GLSL(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in vec2 aUV;
layout(location=3) in vec2 aTexInfo;
uniform mat4 uMVP;
NOPERSP out vec3 vColor;
NOPERSP out vec2 vUV;
flat out vec2 vTexInfo;

#ifdef HAS_SHADOW_CASCADES
uniform mat4 uWorldMatrix;
uniform mat4 uViewMatrix;
uniform vec3 uCameraPos;
out vec3 vWorldPos;
out float vViewDepth;
#endif

void main() {
    vColor = aColor;
    vUV = aUV;
    vTexInfo = aTexInfo;
#ifdef HAS_SHADOW_CASCADES
    vWorldPos = (uWorldMatrix * vec4(aPos, 1.0)).xyz;
    vec4 viewPos = uViewMatrix * vec4(vWorldPos, 1.0);
    vViewDepth = max(-viewPos.z, 0.0);
#endif
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* kMeshFrag = R"GLSL(
// Cascade shadow sampling where supported, over the VRAM decode in kVramDecode
// (prepended to this body).
//
// main() avoids early returns outside HAS_REAL_TEXTURE: Eden's and Suyu's
// shader recompilers reject that form. Desktop GL is unaffected.

NOPERSP in vec3 vColor;
NOPERSP in vec2 vUV;
flat in vec2 vTexInfo;
uniform int uHasVRAM;
uniform float uAlphaScale;
uniform int uUseZeroTexelKey;
uniform int uTexInfoOverrideEnabled;
uniform vec2 uTexInfoOverride;
out vec4 FragColor;

#ifdef HAS_REAL_TEXTURE
uniform int uRealTextureMode;
uniform sampler2D uRealTex;
uniform vec2 uRealTexOffset;
uniform vec2 uRealTexSize;
#endif

#ifdef HAS_SHADOW_CASCADES
in vec3 vWorldPos;
in float vViewDepth;
uniform int uReceiveShadows;
uniform int uShadowCascadeCount;
uniform mat4 uLightVP[3];
uniform float uCascadeSplits[3];
uniform float uCascadeBlendDistances[3];
uniform sampler2DShadow uShadowMap0;
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform int uShadowFilterQuality;
uniform float uShadowBias[3];
uniform float uShadowTexelWorldSize[3];
uniform vec3 uShadowLightDir;
uniform usampler2D uShadowIdMap0;
uniform usampler2D uShadowIdMap1;
uniform usampler2D uShadowIdMap2;
uniform uint uReceiverInstanceId;
// Debug visualization (DebugUI Shadows panel): 0=off, 1=tint receivers by
// selected cascade index, 2=force-darken every receiver fragment.
uniform int uShadowDebugMode;
int gShadowCascade = -1;

vec3 ComputeFlatNormal() {
    vec3 n = cross(dFdx(vWorldPos), dFdy(vWorldPos));
    float lenSq = dot(n, n);
    if (lenSq < 1e-8) {
        return uShadowLightDir;
    }
    n *= inversesqrt(lenSq);
    if (dot(n, uShadowLightDir) < 0.0) {
        n = -n;
    }
    return n;
}

// Rotated Poisson disk + interleaved gradient noise
const vec2 kPoissonDisk[8] = vec2[](
    vec2(-0.613392, 0.617481), vec2(0.170019, -0.040254),
    vec2(-0.299417, 0.791925), vec2(0.645680, 0.493210),
    vec2(-0.651784, 0.717887), vec2(0.421003, 0.027070),
    vec2(-0.817194, -0.271096), vec2(-0.705374, -0.668203)
);

float InterleavedGradientNoise(vec2 fragCoord) {
    return fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

ivec2 ShadowTextureSize(int cascade) {
    if (cascade == 0) return textureSize(uShadowMap0, 0);
    if (cascade == 1) return textureSize(uShadowMap1, 0);
    return textureSize(uShadowMap2, 0);
}

uint SampleShadowCasterId(int cascade, vec2 uv) {
    if (cascade == 0) return texture(uShadowIdMap0, uv).r;
    if (cascade == 1) return texture(uShadowIdMap1, uv).r;
    return texture(uShadowIdMap2, uv).r;
}

float SampleShadowLit(int cascade, vec2 uv, float compareDepth) {
    if (uReceiverInstanceId != 0u && SampleShadowCasterId(cascade, uv) == uReceiverInstanceId) {
        return 1.0;
    }
    if (cascade == 0) return texture(uShadowMap0, vec3(uv, compareDepth));
    if (cascade == 1) return texture(uShadowMap1, vec3(uv, compareDepth));
    return texture(uShadowMap2, vec3(uv, compareDepth));
}

vec2 ReceiverPlaneDepthGradient(vec2 uv, float receiverDepth) {
    vec2 uvDx = dFdx(uv);
    vec2 uvDy = dFdy(uv);
    float depthDx = dFdx(receiverDepth);
    float depthDy = dFdy(receiverDepth);
    float det = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(det) < 1e-8) {
        return vec2(0.0);
    }

    float invDet = 1.0 / det;
    return vec2((depthDx * uvDy.y - depthDy * uvDx.y) * invDet,
                (depthDy * uvDx.x - depthDx * uvDy.x) * invDet);
}

float ShadowCascadeCoverage(int cascade, vec4 lightClip) {
    vec3 ndc = lightClip.xyz / lightClip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float receiverDepth = ndc.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || receiverDepth < 0.0 || receiverDepth > 1.0) {
        return 0.0;
    }

    float borderDistance = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    float borderFadeWidth = (uShadowFilterQuality >= 2) ? 0.040 : 0.055;
    return smoothstep(0.0, borderFadeWidth, borderDistance);
}

float SampleShadowCascade(int cascade, vec4 lightClip) {
    vec3 ndc = lightClip.xyz / lightClip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float receiverDepth = ndc.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || receiverDepth < 0.0 || receiverDepth > 1.0) {
        return 1.0;
    }

    ivec2 mapSize = ShadowTextureSize(cascade);
    vec2 texel = 1.0 / vec2(max(mapSize.x, 1), max(mapSize.y, 1));
    float screenDepthSlope = abs(dFdx(receiverDepth)) + abs(dFdy(receiverDepth));
    float slopeBias = min(screenDepthSlope * 0.6, 0.00065);
    float bias = min(uShadowBias[cascade] + slopeBias, 0.0026);
    vec2 depthGradient = ReceiverPlaneDepthGradient(uv, receiverDepth);
    float diskRadiusTexels = uShadowFilterQuality == 0 ? 2.6
                            : uShadowFilterQuality == 1 ? 1.6
                            : uShadowFilterQuality == 2 ? 1.3
                            : 1.2;
    float angle = InterleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
    float s = sin(angle), c = cos(angle);
    mat2 rot = mat2(c, -s, s, c);

    float lit = 0.0;
    for (int i = 0; i < 8; i++) {
        vec2 offset = (rot * kPoissonDisk[i]) * diskRadiusTexels * texel;
        float tapPlaneBias = abs(clamp(dot(depthGradient, offset), -0.00065, 0.00065));
        float tapBias = min(bias + tapPlaneBias * 0.5, 0.0032);
        lit += SampleShadowLit(cascade, uv + offset, receiverDepth + tapBias);
    }
    return mix(0.45, 1.0, lit / 8.0);
}

float SampleCoveredCascade(int cascade, vec3 shadowNormal, out float coverage) {
    // Clamped to a narrow range so adjacent low-poly facets (which can have
    // sharply different NdotL) don't get wildly different offset magnitudes --
    // that per-facet jump in offset was the main source of self-shadow
    // banding on faceted PS1-era meshes. Grazing angles lean on the
    // constant/slope depth bias below instead of a large normal offset.
    float NdotL = max(dot(shadowNormal, uShadowLightDir), 0.15);
    float offsetScale = clamp(1.0 / NdotL, 0.6, 1.8);
    vec3 offsetPos = vWorldPos + shadowNormal * uShadowTexelWorldSize[cascade] * offsetScale;
    vec4 lightClip = uLightVP[cascade] * vec4(offsetPos, 1.0);
    coverage = ShadowCascadeCoverage(cascade, lightClip);
    if (coverage <= 0.0) {
        return 1.0;
    }
    return SampleShadowCascade(cascade, lightClip);
}

float ComputeShadowFactor() {
    if (uReceiveShadows == 0 || uShadowCascadeCount <= 0) {
        return 1.0;
    }

    float maxShadowDepth = uCascadeSplits[uShadowCascadeCount - 1];
    float fadeDistance = max(maxShadowDepth * 0.10, 1200.0);
    if (vViewDepth >= maxShadowDepth) {
        return 1.0;
    }

    vec3 shadowNormal = ComputeFlatNormal();

    for (int i = 0; i < uShadowCascadeCount; i++) {
        if (vViewDepth <= uCascadeSplits[i] || i == uShadowCascadeCount - 1) {
            gShadowCascade = i;
            if (uShadowDebugMode == 2) {
                return 0.3;
            }

            float coverage = 0.0;
            float shadow = SampleCoveredCascade(i, shadowNormal, coverage);

            // If the depth-selected cascade does not actually cover this
            // receiver, walk outward to larger cascades. Large vertical walls
            // can be close in camera depth while their shadow projection lives
            // in a broader cascade; treating the selected cascade as final
            // creates the visible hard "no shadow until I get near" cutoff.
            int fallbackCascade = i;
            float fallbackCoverage = coverage;
            float fallbackShadow = shadow;
            for (int j = i + 1; j < uShadowCascadeCount; j++) {
                float candidateCoverage = 0.0;
                float candidateShadow = SampleCoveredCascade(j, shadowNormal, candidateCoverage);
                if (candidateCoverage > fallbackCoverage) {
                    fallbackCascade = j;
                    fallbackCoverage = candidateCoverage;
                    fallbackShadow = candidateShadow;
                }
                if (candidateCoverage >= 0.999) {
                    break;
                }
            }

            if (fallbackCascade != i) {
                gShadowCascade = fallbackCascade;
                shadow = (coverage > 0.0) ? mix(fallbackShadow, shadow, coverage) : fallbackShadow;
            }

            if (i < uShadowCascadeCount - 1) {
                float blendDistance = max(uCascadeBlendDistances[i], 0.0);
                float blendStart = uCascadeSplits[i] - blendDistance;
                if (blendDistance > 0.0 && vViewDepth > blendStart) {
                    float nextCoverage = 0.0;
                    float nextShadow = SampleCoveredCascade(i + 1, shadowNormal, nextCoverage);
                    if (nextCoverage <= 0.0) {
                        nextShadow = shadow;
                    }
                    float blend = smoothstep(blendStart, uCascadeSplits[i], vViewDepth);
                    shadow = mix(shadow, nextShadow, blend);
                }
            }
            if (i == uShadowCascadeCount - 1) {
                float fadeStart = maxShadowDepth - fadeDistance;
                shadow = mix(shadow, 1.0, smoothstep(fadeStart, maxShadowDepth, vViewDepth));
            }
            return shadow;
        }
    }
    return 1.0;
}

vec3 ApplyShadowDebugTint(vec3 baseColor) {
    if (uShadowDebugMode != 1 || uReceiveShadows == 0 || gShadowCascade < 0) {
        return baseColor;
    }
    if (gShadowCascade == 0) return vec3(1.0, 0.25, 0.25);
    if (gShadowCascade == 1) return vec3(0.25, 1.0, 0.25);
    return vec3(0.25, 0.25, 1.0);
}
#else
float ComputeShadowFactor() { return 1.0; }
vec3 ApplyShadowDebugTint(vec3 colour) { return colour; }
#endif

void main() {
    float shadowFactor = ComputeShadowFactor();

    float tpageF = vTexInfo.x;
    float cbaF = vTexInfo.y;
    if (uTexInfoOverrideEnabled != 0) {
        tpageF = uTexInfoOverride.x;
        cbaF = uTexInfoOverride.y;
    }

#ifdef HAS_REAL_TEXTURE
    if (uRealTextureMode != 0) {
        vec2 puv = mod(vUV + vec2(256.0), vec2(256.0));
        vec2 ruv = (puv - uRealTexOffset) / uRealTexSize;
        vec4 texColor = texture(uRealTex, ruv);
        if (texColor.a < 0.01) discard;
        FragColor = vec4(ApplyShadowDebugTint(texColor.rgb * shadowFactor),
                         uAlphaScale * texColor.a) * vec4(vColor, 1.0);
        return;
    }
#endif

    // Untextured geometry (tpage < 0): flat per-vertex colour, PSX blob shadow etc.
    if (uHasVRAM == 0 || tpageF < 0.0) {
        FragColor = vec4(ApplyShadowDebugTint(vColor * shadowFactor), uAlphaScale);
    }
    else {
        bool zeroTexel = false;
        float clutWord = VramTexel(tpageF, cbaF, vUV, zeroTexel);

        // World/block content commonly uses magenta keying, while blended
        // effect paths use zero texel keying.
        bool keyed = (uUseZeroTexelKey != 0) ? zeroTexel : VramIsMagenta(clutWord);
        if (keyed) discard;

        vec3 texel = VramColour(clutWord);
        FragColor = vec4(ApplyShadowDebugTint(texel * shadowFactor),
                         uAlphaScale) * vec4(vColor, 1.0);
    }
}
)GLSL";

static const char* kShadowDepthVert = R"GLSL(
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;
layout(location=3) in vec2 aTexInfo;
uniform mat4 uLightMVP;
NOPERSP out vec2 vShadowUV;
flat out vec2 vShadowTexInfo;
void main() {
    vShadowUV = aUV;
    vShadowTexInfo = aTexInfo;
    gl_Position = uLightMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* kShadowDepthFrag = R"GLSL(
// Depth-only shadow-caster pass. Writes the caster instance tag and runs the
// CLUT decode purely so alpha-keyed texels discard, keeping the depth map
// consistent with the main pass.

NOPERSP in vec2 vShadowUV;
flat in vec2 vShadowTexInfo;
uniform int uHasVRAM;
uniform int uHasUV;
uniform int uHasTexInfo;
uniform int uUseZeroTexelKey;
uniform int uTexInfoOverrideEnabled;
uniform vec2 uTexInfoOverride;
uniform int uRealTextureMode;
uniform sampler2D uRealTex;
uniform vec2 uRealTexOffset;
uniform vec2 uRealTexSize;
// Instance tag for self-shadow exclusion (see uReceiverInstanceId in the main
// pass); 0 for static world geometry that should never be excluded.
uniform uint uCasterInstanceId;

#ifdef HAS_INT_ID_TARGET
layout(location=0) out uint oCasterId;
#else
// GLES packs the tag into RGBA8: an integer fragment output crashes the
// Eden/Suyu shader recompiler.
out vec4 oCasterId;
#endif

void WriteCasterId() {
#ifdef HAS_INT_ID_TARGET
    oCasterId = uCasterInstanceId;
#else
    oCasterId = vec4(
        float(uCasterInstanceId & 0xFFu) / 255.0,
        float((uCasterInstanceId >> 8u) & 0xFFu) / 255.0,
        float((uCasterInstanceId >> 16u) & 0xFFu) / 255.0,
        float((uCasterInstanceId >> 24u) & 0xFFu) / 255.0
    );
#endif
}

void main() {
    WriteCasterId();

    if (uRealTextureMode != 0 && uHasUV != 0) {
        vec2 puv = mod(vShadowUV + vec2(256.0), vec2(256.0));
        vec2 ruv = (puv - uRealTexOffset) / uRealTexSize;
        vec4 texColor = texture(uRealTex, ruv);
        if (texColor.a < 0.01) discard;
    }
    else {
        float tpageF = vShadowTexInfo.x;
        float cbaF = vShadowTexInfo.y;
        if (uTexInfoOverrideEnabled != 0) {
            tpageF = uTexInfoOverride.x;
            cbaF = uTexInfoOverride.y;
        }

        if (uHasVRAM != 0 && uHasUV != 0 && uHasTexInfo != 0 && tpageF >= 0.0) {
            bool zeroTexel = false;
            float clutWord = VramTexel(tpageF, cbaF, vShadowUV, zeroTexel);
            bool keyed = (uUseZeroTexelKey != 0) ? zeroTexel : VramIsMagenta(clutWord);
            if (keyed) discard;
        }
    }
}
)GLSL";

struct RechanShaderProgram {
    const char* name;
    const char* vertex;
    const char* fragment;
    // Prepend kVramDecode to the fragment stage.
    bool needsVramDecode;
};

static const RechanShaderProgram kPrograms[] = {
    { "simple", kSimpleVert, kSimpleFrag, false },
    { "tilt", kTiltVert, kSimpleFrag, false },
    { "dot", kSimpleVert, kDotFrag, false },
    { "glow", kSimpleVert, kGlowFrag, false },
    { "godrays", kSimpleVert, kGodraysFrag, false },
    { "moviesharp", kSimpleVert, kMoviesharpFrag, false },
    { "moviedenoise", kSimpleVert, kMoviedenoiseFrag, false },
    { "movieupscale", kSimpleVert, kMovieupscaleFrag, false },
    { "gouraud", kGouraudVert, kGouraudFrag, false },
    { "batch", kBatchVert, kBatchFrag, false },
    { "mesh", kMeshVert, kMeshFrag, true },
    { "shadow_depth", kShadowDepthVert, kShadowDepthFrag, true },
};

static const char* RechanGetShaderSource(const char* programName, pddiShaderStage stage) {
    if (!programName) {
        return nullptr;
    }

    for (const RechanShaderProgram& program : kPrograms) {
        if (std::strcmp(program.name, programName) != 0) {
            continue;
        }
        if (stage == PDDI_SHADER_STAGE_VERTEX) {
            return program.vertex;
        }
        if (!program.needsVramDecode) {
            return program.fragment;
        }
        // Cached per program so the returned pointer stays valid for the
        // caller, which only reads it during glShaderSource.
        static std::unordered_map<std::string, std::string> composed;
        std::string& source = composed[program.name];
        if (source.empty()) {
            source = std::string(kVramDecode) + program.fragment;
        }
        return source.c_str();
    }

    return nullptr;
}

void RechanInstallShaderSources() {
    pddiShaderSource = RechanGetShaderSource;
}

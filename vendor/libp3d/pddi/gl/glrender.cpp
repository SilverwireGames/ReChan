// glrender.cpp - OpenGL implementation of pddi interfaces
#include "pddi/gl/glrender.h"
#include "pddi/gl/glgamepadrumble_sdl.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// Default shader GLSL

static const char* kSimpleVert = R"(
#version 450 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
uniform mat4 uProj;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
)";

// Pseudo-3D tilt vertex shader: treats the quad as a flat plane centered at
// uTiltRect.xy with half-extents uTiltRect.zw, rotates it in 3D by
// uTiltAngles.xyz (pitch, yaw, roll) and perspective-projects it back to 2D
// using a focal distance (uTiltAngles.w). aPos is ignored -- the quad's
// position is fully rebuilt from aUV so any caller-supplied rect works.
static const char* kTiltVert = R"(
#version 450 core
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
)";

// 3D vertex-color shader with PSX VRAM palette lookup

static const char* k3DVert = R"(
#version 450 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in vec2 aUV;
layout(location=3) in vec2 aTexInfo;
uniform mat4 uMVP;
uniform mat4 uWorldMatrix;
uniform mat4 uViewMatrix;
uniform vec3 uCameraPos;
noperspective out vec3 vColor;
noperspective out vec2 vUV;
flat out vec2 vTexInfo;
out vec3 vWorldPos;
out float vViewDepth;
void main() {
    vColor = aColor;
    vUV = aUV;
    vTexInfo = aTexInfo;
    vWorldPos = (uWorldMatrix * vec4(aPos, 1.0)).xyz;
    vec4 viewPos = uViewMatrix * vec4(vWorldPos, 1.0);
    vViewDepth = max(-viewPos.z, 0.0);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

// Depth-only shader for shadow-map cascade passes (MODERN_GRAPHICS).
static const char* kShadowDepthVert = R"(
#version 450 core
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;
layout(location=3) in vec2 aTexInfo;
uniform mat4 uLightMVP;
noperspective out vec2 vShadowUV;
flat out vec2 vShadowTexInfo;
void main() {
    vShadowUV = aUV;
    vShadowTexInfo = aTexInfo;
    gl_Position = uLightMVP * vec4(aPos, 1.0);
}
)";

static const char* kShadowDepthFrag = R"(
#version 450 core
noperspective in vec2 vShadowUV;
flat in vec2 vShadowTexInfo;
uniform usampler2D uVRAM;
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
// pass shader); 0 for static world geometry that should never be excluded.
uniform uint uCasterInstanceId;
layout(location=0) out uint oCasterId;
void main() {
    oCasterId = uCasterInstanceId;
    if (uRealTextureMode != 0 && uHasUV != 0) {
        vec2 puv = mod(vShadowUV + vec2(256.0), vec2(256.0));
        vec2 ruv = (puv - uRealTexOffset) / uRealTexSize;
        vec4 texColor = texture(uRealTex, ruv);
        if (texColor.a < 0.01) discard;
        return;
    }

    float tpageF = vShadowTexInfo.x;
    float cbaF = vShadowTexInfo.y;
    if (uTexInfoOverrideEnabled != 0) {
        tpageF = uTexInfoOverride.x;
        cbaF = uTexInfoOverride.y;
    }

    if (uHasVRAM == 0 || uHasUV == 0 || uHasTexInfo == 0 || tpageF < 0.0) {
        return;
    }

    uint tpage = uint(tpageF);
    uint cba = uint(cbaF);

    uint tx = tpage & 0xFu;
    uint ty = (tpage >> 4u) & 1u;
    uint depth = (tpage >> 7u) & 3u;

    uint pageX = tx * 64u;
    uint pageY = ty * 256u;

    uint clutX = (cba & 0x3Fu) * 16u;
    uint clutY = (cba >> 6u) & 0x1FFu;

    uint px = uint(mod(vShadowUV.x + 256.0, 256.0));
    uint py = uint(mod(vShadowUV.y + 256.0, 256.0));

    uint clutWord;
    bool zeroTexel = false;
    if (depth == 0u) {
        uint wordX = pageX + px / 4u;
        uint word = texelFetch(uVRAM, ivec2(wordX, pageY + py), 0).r;
        uint palIdx = (word >> ((px % 4u) * 4u)) & 0xFu;
        zeroTexel = (palIdx == 0u);
        clutWord = texelFetch(uVRAM, ivec2(clutX + palIdx, clutY), 0).r;
    }
    else if (depth == 1u) {
        uint wordX = pageX + px / 2u;
        uint word = texelFetch(uVRAM, ivec2(wordX, pageY + py), 0).r;
        uint palIdx = (px & 1u) != 0u ? (word >> 8u) & 0xFFu : word & 0xFFu;
        zeroTexel = (palIdx == 0u);
        clutWord = texelFetch(uVRAM, ivec2(clutX + palIdx, clutY), 0).r;
    }
    else {
        clutWord = texelFetch(uVRAM, ivec2(pageX + px, pageY + py), 0).r;
        zeroTexel = (clutWord == 0u);
    }

    if (uUseZeroTexelKey != 0) {
        if (zeroTexel) discard;
    }
    else {
        if ((clutWord & 0x7FFFu) == 0x7C1Fu) discard;
    }
}
)";

static const char* k3DFrag = R"(
#version 450 core
noperspective in vec3 vColor;
noperspective in vec2 vUV;
flat in vec2 vTexInfo;
in vec3 vWorldPos;
in float vViewDepth;
uniform usampler2D uVRAM;
uniform int uHasVRAM;
uniform float uAlphaScale;
uniform int uUseZeroTexelKey;
uniform int uTexInfoOverrideEnabled;
uniform vec2 uTexInfoOverride;
uniform int uRealTextureMode;
uniform sampler2D uRealTex;
uniform vec2 uRealTexOffset;
uniform vec2 uRealTexSize;
out vec4 FragColor;

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

void main() {
    float shadowFactor = ComputeShadowFactor();
    if (uRealTextureMode != 0) {
        vec2 puv = mod(vUV + vec2(256.0), vec2(256.0));
        vec2 ruv = (puv - uRealTexOffset) / uRealTexSize;
        vec4 texColor = texture(uRealTex, ruv);
        if (texColor.a < 0.01) discard;
        FragColor = vec4(ApplyShadowDebugTint(texColor.rgb * shadowFactor), uAlphaScale * texColor.a) * vec4(vColor, 1.0);
        return;
    }

    float tpageF = vTexInfo.x;
    float cbaF = vTexInfo.y;
    if (uTexInfoOverrideEnabled != 0) {
        tpageF = uTexInfoOverride.x;
        cbaF = uTexInfoOverride.y;
    }

    if (uHasVRAM == 0 || tpageF < 0.0) {
        FragColor = vec4(ApplyShadowDebugTint(vColor * shadowFactor), uAlphaScale);
        return;
    }

    uint tpage = uint(tpageF);
    uint cba = uint(cbaF);

    uint tx = tpage & 0xFu;
    uint ty = (tpage >> 4u) & 1u;
    uint depth = (tpage >> 7u) & 3u;

    uint pageX = tx * 64u;
    uint pageY = ty * 256u;

    uint clutX = (cba & 0x3Fu) * 16u;
    uint clutY = (cba >> 6u) & 0x1FFu;

    uint px = uint(mod(vUV.x + 256.0, 256.0));
    uint py = uint(mod(vUV.y + 256.0, 256.0));

    uint clutWord;
    bool zeroTexel = false;
    if (depth == 0u) {
        uint wordX = pageX + px / 4u;
        uint word = texelFetch(uVRAM, ivec2(wordX, pageY + py), 0).r;
        uint palIdx = (word >> ((px % 4u) * 4u)) & 0xFu;
        zeroTexel = (palIdx == 0u);
        clutWord = texelFetch(uVRAM, ivec2(clutX + palIdx, clutY), 0).r;
    } 
    else if (depth == 1u) {
        uint wordX = pageX + px / 2u;
        uint word = texelFetch(uVRAM, ivec2(wordX, pageY + py), 0).r;
        uint palIdx = (px & 1u) != 0u ? (word >> 8u) & 0xFFu : word & 0xFFu;
        zeroTexel = (palIdx == 0u);
        clutWord = texelFetch(uVRAM, ivec2(clutX + palIdx, clutY), 0).r;
    } 
    else {
        clutWord = texelFetch(uVRAM, ivec2(pageX + px, pageY + py), 0).r;
        zeroTexel = (clutWord == 0u);
    }

    // World/block content in this renderer commonly uses magenta keying,
    // while blended effect paths use zero texel keying.
    if (uUseZeroTexelKey != 0) {
        if (zeroTexel) discard;
    }
    else {
        if ((clutWord & 0x7FFFu) == 0x7C1Fu) discard;
    }

    float r = float(clutWord & 0x1Fu) / 31.0;
    float g = float((clutWord >> 5u) & 0x1Fu) / 31.0;
    float b = float((clutWord >> 10u) & 0x1Fu) / 31.0;

    FragColor = vec4(ApplyShadowDebugTint(vec3(r, g, b) * shadowFactor), uAlphaScale) * vec4(vColor, 1.0);
}
)";

static const char* kSimpleFrag = R"(
#version 450 core
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uTint;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vUV) * uTint;
}
)";

static const char* kMovieDenoiseFrag = R"(
#version 450 core
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
)";

static const char* kMovieUpscaleFrag = R"(
#version 450 core
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
)";

static const char* kMovieSharpFrag = R"(
#version 450 core
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
)";

// Soft irregular dot: a textureless sprite with a noise-wobbled edge so it
// reads as a small jagged debris chip instead of a perfect circle.
// uShapeSeed offsets the noise per particle so each chip's wobble differs.
static const char* kDotFrag = R"(
#version 450 core
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
)";

// Soft halo shader: a cheap multi-ring blur of a source texture's lit
// shape, used as a glow buffer behind the rays/source art. uGlowMotion
// rotates the ring sampling pattern and drifts the sample center over time
// so the halo has a slow, living swirl instead of sitting still.
static const char* kGlowFrag = R"(
#version 450 core
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
)";

// God rays shader: radial streak accumulation toward a light-source point,
// used to generate an additive glow buffer from a source texture's alpha.
// uRayMotion twists each marching step by a slowly evolving angle (so the
// whole fan of rays swirls around the source) and orbits the light source
// itself a little, so the rays have continuous movement instead of being a
// static fan.
static const char* kGodRaysFrag = R"(
#version 450 core
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
)";


static const char* kGouraudVert = R"(
#version 450 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec4 aColor;
uniform mat4 uProj;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
)";

static const char* kGouraudFrag = R"(
#version 450 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    FragColor = vColor;
}
)";

// Batched 2D quad shader: like kSimpleVert/kSimpleFrag, but tint comes from a
// per-vertex colour attribute instead of a uniform, so a single draw call can
// contain many quads with different tints/alphas (e.g. a whole run of glyphs
// with different colours, or mixed UI rects).
static const char* kBatchVert = R"(
#version 450 core
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
)";

static const char* kBatchFrag = R"(
#version 450 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vUV) * vColor;
}
)";

static pddiTexture* s_defaultWhiteTexture = nullptr;
static s32 s_defaultWhiteTextureUsers = 0;

static pddiTexture* GetDefaultWhiteTexture() {
    if (s_defaultWhiteTexture) {
        return s_defaultWhiteTexture;
    }

    glTexture* tex = new glTexture();
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

// GL helpers

static u32 CompileGLShader(u32 type, const char* src) {
    u32 shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    int ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "GLSL compile error:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void ApplyBorderlessCaptureOverscan(GLFWwindow* window, int requestedW, int requestedH) {
    if (!window) {
        return;
    }

    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    if (!primaryMonitor) {
        return;
    }

    const GLFWvidmode* videoMode = glfwGetVideoMode(primaryMonitor);
    if (!videoMode) {
        return;
    }

    // Keep normal borderless behavior for non-monitor-sized resolutions.
    // For exact monitor-size windows, offset by 1px so Windows is less likely
    // to use independent flip paths that can produce black Alt+Print captures.
    if (requestedW != videoMode->width || requestedH != videoMode->height) {
        return;
    }

    int monitorX = 0;
    int monitorY = 0;
    glfwGetMonitorPos(primaryMonitor, &monitorX, &monitorY);

    glfwSetWindowPos(window, monitorX - 1, monitorY - 1);
    glfwSetWindowSize(window, requestedW + 2, requestedH + 2);
}

// glPrimBuffer

glPrimBuffer::glPrimBuffer(const pddiPrimBufferDesc& desc)
    : primType(desc.primType), vertexFormat(desc.vertexFormat)
    , vertexCount(desc.vertexCount), indexCount(desc.indexCount) {
    // Compute stride from vertex format
    stride = 0;
    if (vertexFormat & PDDI_V_POSITION) stride += 3 * sizeof(f32);
    if (vertexFormat & PDDI_V_COLOUR)   stride += 3 * sizeof(f32);
    if (vertexFormat & PDDI_V_UV)       stride += 2 * sizeof(f32);
    if (vertexFormat & PDDI_V_TEXINFO)  stride += 2 * sizeof(f32);

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, stride * desc.vertexCount, nullptr, GL_STATIC_DRAW);

    SetupVertexAttribs();

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, desc.indexCount * sizeof(u16), nullptr, GL_STATIC_DRAW);

    glBindVertexArray(0);
}

glPrimBuffer::~glPrimBuffer() {
    if (ebo) glDeleteBuffers(1, &ebo);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
}

void glPrimBuffer::SetVertexData(const void* data, u32 count) {
    vertexCount = count;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, stride * count, data, GL_DYNAMIC_DRAW);
}

void glPrimBuffer::SetIndices(const u16* indices, u32 count) {
    indexCount = count;
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(u16), indices, GL_STATIC_DRAW);
    glBindVertexArray(0);
}

void glPrimBuffer::SetupVertexAttribs() {
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

// glTexture

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

glTexture::glTexture() = default;

glTexture::~glTexture() {
    if (handle)
        glDeleteTextures(1, &handle);
}

void glTexture::SetData(int w, int h, int b, int a, const void* rgba) {
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

void glTexture::SetFilterMode(pddiFilterMode mode) {
    filterMode = mode;
    ApplyTextureFilterMode(handle, filterMode);
}

void glTexture::Bind(int unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, handle);
}

bool glTexture::SetRenderTargetStorage(int w, int h, pddiRenderTargetFormat format) {
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
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        // Reversed-Z: far/empty depth is 0, which should sample as fully lit.
        const GLfloat borderDepth[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderDepth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        // Replicate depth into G/B so debug-UI previews (ImGui::Image) show
        // grayscale instead of red-only; doesn't affect the .r sampling the
        // shadow shader uses.
        const GLint depthSwizzle[4] = { GL_RED, GL_RED, GL_RED, GL_ONE };
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, depthSwizzle);
        glBindTexture(GL_TEXTURE_2D, 0);
        filterMode = PDDI_FILTER_BILINEAR;
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

bool glTexture::SetIdTargetStorage(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    width = w;
    height = h;
    bpp = 32;
    alphaDepth = 0;
    if (handle) glDeleteTextures(1, &handle);
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    filterMode = PDDI_FILTER_NONE;
    return handle != 0;
}

glRenderTarget::glRenderTarget(int w, int h, pddiRenderTargetFormat targetFormat, bool withInstanceId)
    : format(targetFormat), wantsIdAttachment(withInstanceId) {
    Resize(w, h);
}

glRenderTarget::~glRenderTarget() {
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    if (texture) texture->Release();
    if (idTexture) idTexture->Release();
}

bool glRenderTarget::Resize(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    const bool needsId = wantsIdAttachment && IsDepthRenderTargetFormat(format);
    if (valid && framebuffer && texture && (!needsId || idTexture) && width == w && height == h) return true;
    valid = false;
    if (!texture) texture = new glTexture();
    if (!texture->SetRenderTargetStorage(w, h, format)) return false;
    if (needsId) {
        if (!idTexture) idTexture = new glTexture();
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
            static const GLenum kDrawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, kDrawBuffers);
            glReadBuffer(GL_NONE);
        }
        else {
            glDrawBuffer(GL_NONE);
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
        std::fprintf(stderr, "GL: texture render target incomplete (%dx%d, format=%d)\n",
                     w, h, (int)format);
        return false;
    }
    width = w;
    height = h;
    valid = true;
    return true;
}

// glShader

glShader::glShader(const char* shaderType) : type(shaderType ? shaderType : "simple") {
    for (s32 i = 0; i < kMaxTextureSlots; i++) {
        texSlots[i] = nullptr;
    }
    CreateProgram();
}

glShader::~glShader() {
    if (program)
        glDeleteProgram(program);
}

void glShader::CreateProgram() {
    const char* vertexSource = kSimpleVert;
    const char* fragmentSource = kSimpleFrag;
    if (type == "godrays") {
        fragmentSource = kGodRaysFrag;
    }
    else if (type == "glow") {
        fragmentSource = kGlowFrag;
    }
    else if (type == "tilt") {
        vertexSource = kTiltVert;
    }
    else if (type == "dot") {
        fragmentSource = kDotFrag;
    }
    else if (type == "moviesharp") {
        fragmentSource = kMovieSharpFrag;
    }
    else if (type == "moviedenoise") {
        fragmentSource = kMovieDenoiseFrag;
    }
    else if (type == "movieupscale") {
        fragmentSource = kMovieUpscaleFrag;
    }

    u32 vs = CompileGLShader(GL_VERTEX_SHADER, vertexSource);
    u32 fs = CompileGLShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs)
        return;

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    int ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "GLSL link error:\n%s\n", log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
}

void glShader::SetTexture(u32 param, pddiTexture* t) {
    s32 slot = 0;
    if (param < (u32)kMaxTextureSlots) {
        slot = (s32)param;
    }

    texSlots[slot] = t;
}
void glShader::SetInt(u32, int) {}
void glShader::SetFloat(u32, float) {}
void glShader::SetColour(u32 param, pddiColour c) {
    diffuse = c;
}

void glShader::SetInt(const char* param, int value) {
    if (param) intParams[param] = value;
}

void glShader::SetFloat(const char* param, float value) {
    if (param) floatParams[param] = value;
}

void glShader::SetVector(const char* param, float x, float y, float z, float w) {
    if (param) vectorParams[param] = { x, y, z, w };
}

void glShader::SetMatrix(const char* param, const float* matrix4x4) {
    if (!param || !matrix4x4) return;
    std::array<float, 16>& value = matrixParams[param];
    std::memcpy(value.data(), matrix4x4, sizeof(float) * value.size());
}

int glShader::FindUniform(const std::string& name) {
    const auto existing = uniformLocations.find(name);
    if (existing != uniformLocations.end()) {
        return existing->second;
    }

    const int location = glGetUniformLocation(program, name.c_str());
    uniformLocations.emplace(name, location);
    return location;
}

void glShader::PreRender() {
    glUseProgram(program);

    pddiTexture* fallback = GetDefaultWhiteTexture();
    pddiTexture* slot0 = texSlots[0] ? texSlots[0] : fallback;
    if (slot0) {
        slot0->Bind(0);
        int baseSamplerLoc = FindUniform("uTex");
        if (baseSamplerLoc >= 0) {
            glUniform1i(baseSamplerLoc, 0);
        }

        int samplerLoc = FindUniform("uTex0");
        if (samplerLoc < 0) {
            samplerLoc = FindUniform("uTex[0]");
        }
        if (samplerLoc >= 0) {
            glUniform1i(samplerLoc, 0);
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
        if (samplerLoc < 0) {
            std::snprintf(samplerName, sizeof(samplerName), "uTex[%d]", slot);
            samplerLoc = FindUniform(samplerName);
        }
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

void glShader::PostRender() {
    glUseProgram(0);
}

// glDisplay

glDisplay::glDisplay() = default;

glDisplay::~glDisplay() {
    if (imguiInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
    if (window) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

bool glDisplay::InitDisplay(const pddiDisplayInit& init) {
    if (!glfwInit()) {
        std::fprintf(stderr, "GLFW: init failed\n");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    if (init.msaa > 0) {
        glfwWindowHint(GLFW_SAMPLES, init.msaa);
    }

    GLFWmonitor* monitor = nullptr;
    int winW = init.xSize;
    int winH = init.ySize;

    if (init.fullscreen) {
        monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* vidMode = glfwGetVideoMode(monitor);
        winW = vidMode->width;
        winH = vidMode->height;
        glfwWindowHint(GLFW_RED_BITS, vidMode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, vidMode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, vidMode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, vidMode->refreshRate);
    }

    window = glfwCreateWindow(winW, winH, init.title, monitor, nullptr);
    if (!window) {
        std::fprintf(stderr, "GLFW: window creation failed\n");
        glfwTerminate();
        return false;
    }

    windowedW = init.xSize;
    windowedH = init.ySize;
    if (!init.fullscreen) {
        GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
        if (primaryMonitor) {
            int workX = 0;
            int workY = 0;
            int workW = 0;
            int workH = 0;
            glfwGetMonitorWorkarea(primaryMonitor, &workX, &workY, &workW, &workH);

            int contentW = 0;
            int contentH = 0;
            glfwGetWindowSize(window, &contentW, &contentH);

            int centeredX = workX + (workW - contentW) / 2;
            int centeredY = workY + (workH - contentH) / 2;
            if (centeredX < workX) centeredX = workX;
            if (centeredY < workY) centeredY = workY;

            glfwSetWindowPos(window, centeredX, centeredY);
        }

        glfwGetWindowPos(window, &windowedX, &windowedY);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(init.vsync ? 1 : 0);

    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);
    glfwSetWindowFocusCallback(window, WindowFocusCallback);
    glfwSetWindowCloseCallback(window, WindowCloseCallback);
    glfwSetKeyCallback(window, KeyCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPosCallback);
    glfwSetScrollCallback(window, ScrollCallback);

    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);

    if (!gladLoadGL(glfwGetProcAddress)) {
        std::fprintf(stderr, "GLAD: failed to load OpenGL\n");
        return false;
    }

    glGetIntegerv(GL_MAX_SAMPLES, &maxMsaaSamples);
    SetMSAA(init.msaa);

    std::printf("OpenGL %s on %s\n",
                reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glClearDepth(0.0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // Don't let ImGui override GLFW cursor mode
    ImGui::StyleColorsDark();

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");
    imguiInitialized = true;

    return true;
}

void glDisplay::SwapBuffers() {
    glfwSwapBuffers(window);
}

bool glDisplay::ReadBackBuffer(std::vector<unsigned char>& rgb, int& width, int& height) {
    SyncFramebufferSize();
    width = fbWidth;
    height = fbHeight;
    if (width <= 0 || height <= 0) {
        return false;
    }

    GLint prevReadFbo = 0;
    GLint prevPackAlignment = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlignment);

    std::vector<unsigned char> bottomUp((size_t)width * height * 3);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, bottomUp.data());
    const bool ok = (glGetError() == GL_NO_ERROR);

    glPixelStorei(GL_PACK_ALIGNMENT, prevPackAlignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFbo);
    if (!ok) {
        return false;
    }

    const size_t rowBytes = (size_t)width * 3;
    rgb.resize(bottomUp.size());
    for (int y = 0; y < height; ++y) {
        memcpy(rgb.data() + (size_t)y * rowBytes, bottomUp.data() + (size_t)(height - 1 - y) * rowBytes, rowBytes);
    }
    return true;
}

bool glDisplay::ShouldClose() {
    return glfwWindowShouldClose(window);
}

int glDisplay::GetWidth() {
    SyncFramebufferSize();
    return fbWidth;
}

int glDisplay::GetHeight() {
    SyncFramebufferSize();
    return fbHeight;
}

void glDisplay::PollEvents() {
    glfwPollEvents();
    SyncFramebufferSize();

    if (imguiInitialized && !imguiFrameStarted) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        imguiFrameStarted = true;
    }
}

void glDisplay::AddOverlayCallback(OverlayCallback cb) {
    overlayCallbacks.push_back(cb);
}

void glDisplay::RenderOverlay() {
    if (!imguiFrameStarted) {
        return;
    }
    for (auto& cb : overlayCallbacks) {
        cb();
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backupContext = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backupContext);
    }

    imguiFrameStarted = false;
}

bool glDisplay::IsKeyDown(int key) {
    return window && glfwGetKey(window, key) == GLFW_PRESS;
}

bool glDisplay::IsMouseButtonDown(int button) {
    return window && glfwGetMouseButton(window, button) == GLFW_PRESS;
}

void glDisplay::GetMousePosition(double& x, double& y) {
    if (window) {
        glfwGetCursorPos(window, &x, &y);
    }
    else {
        x = 0;
        y = 0;
    }
}

void glDisplay::SetIcon(int w, int h, const unsigned char* rgba) {
    if (!window || !rgba)
        return;
    GLFWimage img;
    img.width = w;
    img.height = h;
    img.pixels = const_cast<unsigned char*>(rgba);
    glfwSetWindowIcon(window, 1, &img);
}

// Video mode

int glDisplay::GetVideoModeCount() {
    int count = 0;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        glfwGetVideoModes(monitor, &count);
    }
    return count;
}

void glDisplay::GetVideoMode(int index, pddiVideoMode& mode) {
    int count = 0;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor)
        return;
    const GLFWvidmode* modes = glfwGetVideoModes(monitor, &count);
    if (index < 0 || index >= count)
        return;
    mode.width = modes[index].width;
    mode.height = modes[index].height;
    mode.refreshRate = modes[index].refreshRate;
}

void glDisplay::SetFullscreen(bool fullscreen) {
    if (!window)
        return;

    if (fullscreen == IsFullscreen())
        return;

    if (fullscreen) {
        // Save current windowed position/size
        glfwGetWindowPos(window, &windowedX, &windowedY);
        glfwGetWindowSize(window, &windowedW, &windowedH);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* vidMode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(window, monitor, 0, 0,
                             windowedW, windowedH,
                             vidMode->refreshRate);
    }
    else {
        glfwSetWindowMonitor(window, nullptr,
                             windowedX, windowedY,
                             windowedW, windowedH, 0);
    }

    SyncFramebufferSize();
}

bool glDisplay::IsFullscreen() {
    return window && glfwGetWindowMonitor(window) != nullptr;
}

void glDisplay::SetBorderless(bool enabled) {
    if (!window)
        return;

    if (borderless == enabled)
        return;

    // Borderless applies in windowed mode; keep desired state when in fullscreen.
    if (IsFullscreen()) {
        borderless = enabled;
        return;
    }

    if (enabled) {
        glfwGetWindowPos(window, &windowedX, &windowedY);
        glfwGetWindowSize(window, &windowedW, &windowedH);

        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
    }
    else {
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowSize(window, windowedW, windowedH);
        glfwSetWindowPos(window, windowedX, windowedY);
    }

    borderless = enabled;
    SyncFramebufferSize();
}

void glDisplay::SetResolution(int w, int h) {
    if (!window)
        return;

    const bool wasVisible = glfwGetWindowAttrib(window, GLFW_VISIBLE) == GLFW_TRUE;

    if (wasVisible) {
        glfwHideWindow(window);
    }

    if (IsFullscreen()) {
        GLFWmonitor* monitor = glfwGetWindowMonitor(window);
        const GLFWvidmode* vidMode = glfwGetVideoMode(monitor);

        glfwSetWindowMonitor(
            window,
            monitor,
            0,
            0,
            w,
            h,
            vidMode ? vidMode->refreshRate : GLFW_DONT_CARE
        );
    }
    else {
        glfwSetWindowSize(window, w, h);
        windowedW = w;
        windowedH = h;

        GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
        if (primaryMonitor) {
            int workX = 0;
            int workY = 0;
            int workW = 0;
            int workH = 0;

            glfwGetMonitorWorkarea(primaryMonitor, &workX, &workY, &workW, &workH);

            int centeredX = workX + (workW - w) / 2;
            int centeredY = workY + (workH - h) / 2;

            if (centeredX < workX) centeredX = workX;
            if (centeredY < workY) centeredY = workY;

            glfwSetWindowPos(window, centeredX, centeredY);

            windowedX = centeredX;
            windowedY = centeredY;
        }

        if (borderless) {
            ApplyBorderlessCaptureOverscan(window, w, h);

            glfwGetWindowPos(window, &windowedX, &windowedY);
            glfwGetWindowSize(window, &windowedW, &windowedH);
        }
    }

    PresentBlackFrame();

    if (wasVisible) {
        glfwShowWindow(window);
    }
}

void glDisplay::SetVSync(bool enabled) {
    if (window) {
        glfwSwapInterval(enabled ? 1 : 0);
    }
}

int glDisplay::ClampMSAASamples(int samples) const {
    if (samples <= 0) {
        return 0;
    }

    static const int kAllowedSamples[] = { 2, 4, 8, 16 };
    int closest = kAllowedSamples[0];
    int closestDist = (samples > closest) ? (samples - closest) : (closest - samples);

    for (u32 i = 1; i < (u32)(sizeof(kAllowedSamples) / sizeof(kAllowedSamples[0])); i++) {
        const int candidate = kAllowedSamples[i];
        const int dist = (samples > candidate) ? (samples - candidate) : (candidate - samples);
        if (dist < closestDist) {
            closest = candidate;
            closestDist = dist;
        }
    }

    if (maxMsaaSamples > 0 && closest > maxMsaaSamples) {
        int fallback = 0;
        for (u32 i = 0; i < (u32)(sizeof(kAllowedSamples) / sizeof(kAllowedSamples[0])); i++) {
            if (kAllowedSamples[i] <= maxMsaaSamples) {
                fallback = kAllowedSamples[i];
            }
        }
        closest = fallback;
    }

    return closest;
}

void glDisplay::SetMSAA(int samples) {
    msaaSamples = ClampMSAASamples(samples);
    if (msaaSamples > 0) {
        glEnable(GL_MULTISAMPLE);
    }
    else {
        glDisable(GL_MULTISAMPLE);
    }
}

void glDisplay::SetTitle(const char* title) {
    if (window) {
        glfwSetWindowTitle(window, title);
    }
}

void glDisplay::SetWindowPos(int x, int y) {
    if (window && !IsFullscreen() && !borderless) {
        glfwSetWindowPos(window, x, y);
        windowedX = x;
        windowedY = y;
    }
}

// Cursor

void glDisplay::ShowCursor(bool visible) {
    cursorVisible = visible;
    if (window) {
        glfwSetInputMode(window, GLFW_CURSOR,
                         visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
    }
}

void glDisplay::ClipCursor(bool clip) {
    cursorClipped = clip;
    UpdateCursorClip();
}

void glDisplay::UpdateCursorClip() {
    if (!window)
        return;

    if (cursorClipped && focused) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
    }
    else if (!cursorVisible) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
    else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
}

// WndProc

void glDisplay::SetWndProc(pddiWndProc proc) {
    wndProc = std::move(proc);
}

void glDisplay::QueryFramebufferSize(int& width, int& height) const {
    width = 0;
    height = 0;

    if (!window) {
        return;
    }

    glfwGetFramebufferSize(window, &width, &height);
}

void glDisplay::PresentBlackFrame() {
    if (!window) {
        return;
    }

    glfwMakeContextCurrent(window);

    SyncFramebufferSize();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbWidth, fbHeight);

    glDisable(GL_SCISSOR_TEST);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glfwSwapBuffers(window);
}

void glDisplay::SyncFramebufferSize() {
    int w = 0;
    int h = 0;
    QueryFramebufferSize(w, h);

    if (w == fbWidth && h == fbHeight) {
        return;
    }

    fbWidth = w;
    fbHeight = h;

    if (wndProc) {
        pddiWndMessage msg{};
        msg.event = PDDI_WND_RESIZE;
        msg.param1 = w;
        msg.param2 = h;
        wndProc(msg);
    }
}

void glDisplay::GetViewport(int& x, int& y, int& w, int& h) {
    SyncFramebufferSize();
    x = 0;
    y = 0;
    w = fbWidth;
    h = fbHeight;
}

// GLFW callbacks

void glDisplay::FramebufferSizeCallback(GLFWwindow* win, int w, int h) {
    auto* self = static_cast<glDisplay*>(glfwGetWindowUserPointer(win));
    if (!self)
        return;
    self->fbWidth = w;
    self->fbHeight = h;
    if (self->wndProc) {
        pddiWndMessage msg{};
        msg.event = PDDI_WND_RESIZE;
        msg.param1 = w;
        msg.param2 = h;
        self->wndProc(msg);
    }
}

void glDisplay::WindowFocusCallback(GLFWwindow* win, int isFocused) {
    auto* self = static_cast<glDisplay*>(glfwGetWindowUserPointer(win));
    if (!self)
        return;
    self->focused = (isFocused != 0);
    self->UpdateCursorClip();
    if (self->wndProc) {
        pddiWndMessage msg{};
        msg.event = PDDI_WND_FOCUS;
        msg.param1 = isFocused;
        self->wndProc(msg);
    }
}

void glDisplay::WindowCloseCallback(GLFWwindow* win) {
    auto* self = static_cast<glDisplay*>(glfwGetWindowUserPointer(win));
    if (!self)
        return;
    if (self->wndProc) {
        pddiWndMessage msg{};
        msg.event = PDDI_WND_CLOSE;
        if (self->wndProc(msg)) {
            glfwSetWindowShouldClose(win, GLFW_FALSE);
        }
    }
}

void glDisplay::KeyCallback(GLFWwindow* win, int key, int scancode, int action, int mods) {
    auto* self = static_cast<glDisplay*>(glfwGetWindowUserPointer(win));
    if (!self || !self->wndProc)
        return;
    if (action == GLFW_REPEAT)
        return;
    pddiWndMessage msg{};
    msg.event = (action == GLFW_PRESS) ? PDDI_WND_KEYDOWN : PDDI_WND_KEYUP;
    msg.param1 = key;
    msg.param2 = scancode;
    self->wndProc(msg);
}

void glDisplay::MouseButtonCallback(GLFWwindow* win, int button, int action, int mods) {
    auto* self = static_cast<glDisplay*>(glfwGetWindowUserPointer(win));
    if (!self || !self->wndProc)
        return;
    pddiWndMessage msg{};
    msg.event = PDDI_WND_MOUSEBUTTON;
    msg.param1 = button;
    msg.param2 = (action == GLFW_PRESS) ? 1 : 0;
    self->wndProc(msg);
}

void glDisplay::CursorPosCallback(GLFWwindow* win, double x, double y) {
    auto* self = static_cast<glDisplay*>(glfwGetWindowUserPointer(win));
    if (!self || !self->wndProc)
        return;
    pddiWndMessage msg{};
    msg.event = PDDI_WND_MOUSEMOVE;
    msg.fparam1 = x;
    msg.fparam2 = y;
    self->wndProc(msg);
}

void glDisplay::ScrollCallback(GLFWwindow* win, double xoff, double yoff) {
    auto* self = static_cast<glDisplay*>(glfwGetWindowUserPointer(win));
    if (!self || !self->wndProc)
        return;
    pddiWndMessage msg{};
    msg.event = PDDI_WND_SCROLL;
    msg.fparam1 = xoff;
    msg.fparam2 = yoff;
    self->wndProc(msg);
}

// glContext

glContext::glContext(glDisplay* disp)
    : display(disp) {
    GetDefaultWhiteTexture();
    s_defaultWhiteTextureUsers++;
    glGenSamplers(1, &shadowCompareSampler);
    glSamplerParameteri(shadowCompareSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(shadowCompareSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(shadowCompareSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glSamplerParameteri(shadowCompareSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glSamplerParameteri(shadowCompareSampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(shadowCompareSampler, GL_TEXTURE_COMPARE_FUNC, GL_GEQUAL);
    const GLfloat borderDepth[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    glSamplerParameterfv(shadowCompareSampler, GL_TEXTURE_BORDER_COLOR, borderDepth);
    InitQuadMesh();
    InitGouraudMesh();
    InitBatchMesh();
    Init3DShader();
    InitShadowDepthShader();
}

glContext::~glContext() {
    DestroyMSAAFramebuffer();
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
    if (shadowCompareSampler) glDeleteSamplers(1, &shadowCompareSampler);

    if (s_defaultWhiteTextureUsers > 0) {
        s_defaultWhiteTextureUsers--;
    }
    if (s_defaultWhiteTextureUsers == 0) {
        ReleaseDefaultWhiteTexture();
    }
}

void glContext::DestroyMSAAFramebuffer() {
    if (msaaDepthStencilRbo) {
        glDeleteRenderbuffers(1, &msaaDepthStencilRbo);
        msaaDepthStencilRbo = 0;
    }
    if (msaaColorRbo) {
        glDeleteRenderbuffers(1, &msaaColorRbo);
        msaaColorRbo = 0;
    }
    if (msaaFbo) {
        glDeleteFramebuffers(1, &msaaFbo);
        msaaFbo = 0;
    }
    msaaWidth = 0;
    msaaHeight = 0;
    activeMsaaSamples = 0;
}

void glContext::EnsureMSAAFramebuffer(s32 samples, s32 width, s32 height) {
    if (samples <= 0 || width <= 0 || height <= 0) {
        DestroyMSAAFramebuffer();
        return;
    }

    if (msaaFbo && activeMsaaSamples == samples && msaaWidth == width && msaaHeight == height) {
        return;
    }

    DestroyMSAAFramebuffer();

    glGenFramebuffers(1, &msaaFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo);

    glGenRenderbuffers(1, &msaaColorRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, msaaColorRbo);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaaColorRbo);

    glGenRenderbuffers(1, &msaaDepthStencilRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, msaaDepthStencilRbo);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msaaDepthStencilRbo);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "GL: MSAA framebuffer incomplete (status=0x%X)\n", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        DestroyMSAAFramebuffer();
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    msaaWidth = width;
    msaaHeight = height;
    activeMsaaSamples = samples;
}

void glContext::BeginFrame() {
    int vx, vy, vw, vh;
    display->GetViewport(vx, vy, vw, vh);

    usingMsaaFramebuffer = false;
    multisampleEnabled = true;
    resolvedForOverlay = false;
    const s32 desiredSamples = display ? display->GetMSAA() : 0;
    if (desiredSamples > 0 && vw > 0 && vh > 0) {
        EnsureMSAAFramebuffer(desiredSamples, vw, vh);
        if (msaaFbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo);
            usingMsaaFramebuffer = true;
        }
        else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }
    else {
        if (msaaFbo) {
            DestroyMSAAFramebuffer();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    UpdateMultisampleState();

    glViewport(vx, vy, vw, vh);
    glScissor(vx, vy, vw, vh);
    glEnable(GL_SCISSOR_TEST);
    stateDirty = true;
    activeRenderTarget = nullptr;
}

void glContext::EndFrame() {
    glDisable(GL_SCISSOR_TEST);

    if (usingMsaaFramebuffer && msaaFbo && !resolvedForOverlay) {
        int vx, vy, vw, vh;
        display->GetViewport(vx, vy, vw, vh);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(vx, vy, vx + vw, vy + vh,
                          vx, vy, vx + vw, vy + vh,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    glFlush();
}

void glContext::SetClearColour(pddiColour c) { clearColour = c; }

void glContext::Clear(int flags) {
    GLbitfield mask = 0;
    if (flags & PDDI_BUFFER_COLOUR) {
        glClearColor(clearColour.r / 255.0f, clearColour.g / 255.0f,
                     clearColour.b / 255.0f, clearColour.a / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & PDDI_BUFFER_DEPTH) {
        glClearDepth(0.0);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask)
        glClear(mask);
}

void glContext::SetProjectionMatrix(const Mat4& m) { projection = m; }
void glContext::SetViewMatrix(const Mat4& m) { viewMatrix = m; frameConstUniformsDirty = true; }
void glContext::SetWorldMatrix(const Mat4& m) { worldMatrix = m; }

void glContext::SetWorldMirror(bool enable) {
    if (worldMirror == enable)
        return;
    worldMirror = enable;
    stateDirty = true;
}

void glContext::SetCullMode(pddiCullMode mode) {
    if (!stateDirty && mode == cachedCullMode)
        return;

    cachedCullMode = mode;

    // PSX uses CW winding (left-handed). X-flip in projection preserves CW.
    glFrontFace(GL_CW);

    pddiCullMode effectiveMode = mode;
    if (worldMirror) {
        if (mode == PDDI_CULL_NORMAL) {
            effectiveMode = PDDI_CULL_INVERTED;
        }
        else if (mode == PDDI_CULL_INVERTED) {
            effectiveMode = PDDI_CULL_NORMAL;
        }
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

void glContext::EnableZBuffer(bool enable) {
    if (!stateDirty && enable == cachedZBuffer)
        return;

    cachedZBuffer = enable;
    if (enable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
    }
    else
        glDisable(GL_DEPTH_TEST);
}

void glContext::SetPolygonOffset(bool enable, f32 factor, f32 units) {
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
    // Force blend mode to re-evaluate on the next SetBlendMode call so the
    // BLEND_NONE case picks up the new override state.
    stateDirty = true;
}

void glContext::SetDepthClamp(bool enable) {
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

void glContext::SetBlendMode(pddiBlendMode mode) {
    if (!stateDirty && mode == cachedBlendMode)
        return;

    cachedBlendMode = mode;
    switch (mode) {
        case PDDI_BLEND_NONE:
            glDisable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glDepthMask(GL_TRUE);
            // Restore depth-test precision for opaque geometry —
            // unless a polygon offset override is active (e.g., during effect rendering),
            // in which case maintain that override so opaque effects also get forward bias.
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
            // PSX OT equivalent: blended effects inserted after geometry at the same
            // OT slot render on top.  With reversed-Z (near=1 far=0, GL_GEQUAL), a
            // positive depth offset pushes fragments toward camera so same-depth
            // effects pass the depth test against already-drawn opaque surfaces.
            glEnable(GL_POLYGON_OFFSET_FILL);
            if (polyOffsetOverride) {
                glPolygonOffset(polyOffsetFactor, polyOffsetUnits);
            }
            else {
                glPolygonOffset(1.0f, 1.0f);
            }
            break;
        case PDDI_BLEND_ADD:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            glDepthMask(GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            if (polyOffsetOverride) {
                glPolygonOffset(polyOffsetFactor, polyOffsetUnits);
            }
            else {
                glPolygonOffset(1.0f, 1.0f);
            }
            break;
        case PDDI_BLEND_SUBTRACT:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glDepthMask(GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            if (polyOffsetOverride) {
                glPolygonOffset(polyOffsetFactor, polyOffsetUnits);
            }
            else {
                glPolygonOffset(1.0f, 1.0f);
            }
            break;
        case PDDI_BLEND_PSX_QUARTER:
            glEnable(GL_BLEND);
            glBlendColor(0.0f, 0.0f, 0.0f, 0.25f);
            glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            glDepthMask(GL_FALSE);
            glEnable(GL_POLYGON_OFFSET_FILL);
            if (polyOffsetOverride) {
                glPolygonOffset(polyOffsetFactor, polyOffsetUnits);
            }
            else {
                glPolygonOffset(1.0f, 1.0f);
            }
            break;
    }
    stateDirty = false;
}

void glContext::SetScissor(int x, int y, int w, int h) {
    glScissor(x, y, w, h);
}

void glContext::SetMultisampleEnabled(bool enable) {
    multisampleEnabled = enable;
    UpdateMultisampleState();
}

void glContext::ResolveForOverlayPass() {
    if (!usingMsaaFramebuffer || !msaaFbo || resolvedForOverlay) {
        return;
    }

    int vx, vy, vw, vh;
    display->GetViewport(vx, vy, vw, vh);

    const bool hadScissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    if (hadScissor) {
        glDisable(GL_SCISSOR_TEST);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(vx, vy, vx + vw, vy + vh,
                      vx, vy, vx + vw, vy + vh,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (hadScissor) {
        glEnable(GL_SCISSOR_TEST);
    }

    usingMsaaFramebuffer = false;
    resolvedForOverlay = true;
    UpdateMultisampleState();
}

pddiRenderTarget* glContext::CreateRenderTarget(int width, int height,
                                                 pddiRenderTargetFormat format,
                                                 bool withInstanceId) {
    glRenderTarget* target = new glRenderTarget(width, height, format, withInstanceId);
    if (!target->IsValid()) {
        target->Release();
        return nullptr;
    }
    return target;
}

bool glContext::SetRenderTarget(pddiRenderTarget* target) {
    glRenderTarget* glTarget = dynamic_cast<glRenderTarget*>(target);
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

void glContext::ClearShadowCasterIdTarget() {
    if (!activeRenderTarget || !activeRenderTarget->GetIdTexture()) {
        return;
    }
    static const GLuint kZeroId[4] = { 0, 0, 0, 0 };
    glClearBufferuiv(GL_COLOR, 0, kZeroId);
}

void glContext::UpdateMultisampleState() {
    if (usingMsaaFramebuffer && multisampleEnabled) {
        glEnable(GL_MULTISAMPLE);
    }
    else {
        glDisable(GL_MULTISAMPLE);
    }
}

void glContext::DrawFilledCircle(pddiBaseShader* shader,
                                 float centerX, float centerY,
                                 float radiusX, float radiusY,
                                 float u0, float v0, float u1, float v1,
                                 int segments) {
    if (!shader)
        return;

    if (segments < 3)
        segments = 3;

    if (segments > 64)
        segments = 64;

    shader->SetMatrix("uProj", projection.Data());
    shader->PreRender();

    constexpr float PI2 = 6.28318530718f;

    // 3 vertices per segment.
    // Each vertex: x, y, u, v
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

        verts[out++] = x0;
        verts[out++] = y0;
        verts[out++] = tu0;
        verts[out++] = tv0;

        verts[out++] = x1p;
        verts[out++] = y1p;
        verts[out++] = tu1;
        verts[out++] = tv1;

        verts[out++] = x2p;
        verts[out++] = y2p;
        verts[out++] = tu2;
        verts[out++] = tv2;
    }

    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);

    const GLsizeiptr sizeBytes = (GLsizeiptr)(out * sizeof(float));
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeBytes, verts);

    glDrawArrays(GL_TRIANGLES, 0, segments * 3);

    shader->PostRender();
}

void glContext::DrawCircle(pddiBaseShader* shader,
                           float centerX, float centerY,
                           float radiusX, float radiusY,
                           float thickness,
                           float u0, float v0, float u1, float v1,
                           int segments) {
    if (!shader)
        return;

    if (segments < 3)
        segments = 3;

    if (segments > 64)
        segments = 64;

    if (thickness <= 0.0f)
        return;

    shader->SetMatrix("uProj", projection.Data());
    shader->PreRender();

    constexpr float PI2 = 6.28318530718f;

    const float innerRadiusX = std::max(0.0f, radiusX - thickness);
    const float innerRadiusY = std::max(0.0f, radiusY - thickness);

    // 2 triangles per segment, 6 vertices per segment.
    // Each vertex: x, y, u, v
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

        // Triangle 1
        verts[out++] = outerX0;
        verts[out++] = outerY0;
        verts[out++] = outerU0;
        verts[out++] = outerV0;

        verts[out++] = outerX1;
        verts[out++] = outerY1;
        verts[out++] = outerU1;
        verts[out++] = outerV1;

        verts[out++] = innerX1;
        verts[out++] = innerY1;
        verts[out++] = innerU1;
        verts[out++] = innerV1;

        // Triangle 2
        verts[out++] = outerX0;
        verts[out++] = outerY0;
        verts[out++] = outerU0;
        verts[out++] = outerV0;

        verts[out++] = innerX1;
        verts[out++] = innerY1;
        verts[out++] = innerU1;
        verts[out++] = innerV1;

        verts[out++] = innerX0;
        verts[out++] = innerY0;
        verts[out++] = innerU0;
        verts[out++] = innerV0;
    }

    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);

    const GLsizeiptr sizeBytes = (GLsizeiptr)(out * sizeof(float));
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeBytes, verts);

    glDrawArrays(GL_TRIANGLES, 0, segments * 6);

    shader->PostRender();
}

void glContext::DrawQuad(pddiBaseShader* shader,
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

void glContext::InitQuadMesh() {
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 6, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                          (void*)(sizeof(float) * 2));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void glContext::InitGouraudMesh() {
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

    u32 vs = CompileGLShader(GL_VERTEX_SHADER, kGouraudVert);
    u32 fs = CompileGLShader(GL_FRAGMENT_SHADER, kGouraudFrag);
    if (vs && fs) {
        gouraudProgram = glCreateProgram();
        glAttachShader(gouraudProgram, vs);
        glAttachShader(gouraudProgram, fs);
        glLinkProgram(gouraudProgram);
        gouraudUProjLoc = glGetUniformLocation(gouraudProgram, "uProj");
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
}

void glContext::InitBatchMesh() {
    glGenVertexArrays(1, &batchVAO);
    glGenBuffers(1, &batchVBO);
    glBindVertexArray(batchVAO);
    glBindBuffer(GL_ARRAY_BUFFER, batchVBO);
    // No fixed capacity up front - DrawQuadBatch grows/reallocates on demand.
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

    u32 vs = CompileGLShader(GL_VERTEX_SHADER, kBatchVert);
    u32 fs = CompileGLShader(GL_FRAGMENT_SHADER, kBatchFrag);
    if (vs && fs) {
        batchProgram = glCreateProgram();
        glAttachShader(batchProgram, vs);
        glAttachShader(batchProgram, fs);
        glLinkProgram(batchProgram);
        batchUProjLoc = glGetUniformLocation(batchProgram, "uProj");
        batchUTexLoc = glGetUniformLocation(batchProgram, "uTex");
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
}

void glContext::DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
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
        // Grow with headroom so a typical frame doesn't reallocate every batch.
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

void glContext::DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                                float x1, float y1, float r1, float g1, float b1, float a1,
                                float x2, float y2, float r2, float g2, float b2, float a2,
                                float x3, float y3, float r3, float g3, float b3, float a3) {
    if (!gouraudProgram) return;

    // Two triangles: (v0, v1, v2) and (v1, v3, v2)
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

void glContext::Init3DShader() {
    u32 vs = CompileGLShader(GL_VERTEX_SHADER, k3DVert);
    u32 fs = CompileGLShader(GL_FRAGMENT_SHADER, k3DFrag);

    if (!vs || !fs)
        return;

    program3D = glCreateProgram();
    glAttachShader(program3D, vs);
    glAttachShader(program3D, fs);
    glLinkProgram(program3D);

    int ok;
    glGetProgramiv(program3D, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program3D, sizeof(log), nullptr, log);
        std::fprintf(stderr, "3D shader link error:\n%s\n", log);
    }
    else {
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

    glDeleteShader(vs);
    glDeleteShader(fs);
}

void glContext::InitShadowDepthShader() {
    u32 vs = CompileGLShader(GL_VERTEX_SHADER, kShadowDepthVert);
    u32 fs = CompileGLShader(GL_FRAGMENT_SHADER, kShadowDepthFrag);

    if (!vs || !fs)
        return;

    shadowDepthProgram = glCreateProgram();
    glAttachShader(shadowDepthProgram, vs);
    glAttachShader(shadowDepthProgram, fs);
    glLinkProgram(shadowDepthProgram);

    int ok;
    glGetProgramiv(shadowDepthProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(shadowDepthProgram, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shadow depth shader link error:\n%s\n", log);
    }
    else {
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

    glDeleteShader(vs);
    glDeleteShader(fs);
}

void glContext::SetShadowCasterPass(bool enable, const Mat4& lightVP) {
    shadowCasterPassActive = enable;
    shadowCasterLightVP = lightVP;
}

void glContext::SetShadowCascades(pddiTexture* const* depthTextures, const Mat4* lightVPs,
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

void glContext::SetTexture(pddiTexture* t) {
    currentTexture = t;
}

void glContext::SetVRAMHandle(u32 h) {
    vramHandle = h;
}

void glContext::SetTexInfoOverride(bool enabled, u32 texInfoWord) {
    texInfoOverrideEnabled = enabled;
    texInfoOverrideWord = texInfoWord;
}

void glContext::SetRealTextureRect(float offsetX, float offsetY, float sizeX, float sizeY) {
    realTexOffsetX = offsetX;
    realTexOffsetY = offsetY;
    realTexSizeX = sizeX;
    realTexSizeY = sizeY;
}

u32 glContext::CreateVRAMTexture(int w, int h, const u16* data) {
    u32 tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, w, h, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_SHORT, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void glContext::DestroyVRAMTexture(u32 handle) {
    if (handle) glDeleteTextures(1, &handle);
}

void glContext::UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) {
    if (!handle) return;
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_SHORT, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void glContext::DrawPrimBuffer(pddiPrimBuffer* buffer, u32 indexOffset, u32 indexCount) {
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

        auto* glBufShadow = static_cast<glPrimBuffer*>(buffer);
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
            static_cast<glTexture*>(currentTexture)->Bind(1);
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
    glUniform1f(u3D.alphaScale, alphaScale);
    const int useZeroTexelKey = (cachedBlendMode != PDDI_BLEND_NONE) ? 1 : 0;
    glUniform1i(u3D.useZeroTexelKey, useZeroTexelKey);

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
    glUniform1i(u3D.receiveShadows, receiveShadows);
    glUniform1i(u3D.shadowCascadeCount, receiveShadows ? shadowCascadeCount : 0);
    glUniform1i(u3D.shadowFilterQuality, receiveShadows ? shadowFilterQuality : 0);
    // uShadowBias/uShadowLightDir/uLightVP/uCascade*/uShadowTexelWorldSize are 100%
    // derived from SetShadowCascades, which only runs once/frame -- re-uploading them
    // on every single mesh draw was pure waste. uReceiveShadows/uShadowCascadeCount/
    // uShadowFilterQuality/uReceiverInstanceId above and below stay per-call since
    // they (and receiveShadowsEnabled) vary per object.
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
            static_cast<glTexture*>(shadowDepthTextures[i])->Bind(2 + i);
            glBindSampler(2 + i, shadowCompareSampler);
            glUniform1i(u3D.shadowMap[i], 2 + i);
            if (shadowIdTextures[i]) {
                static_cast<glTexture*>(shadowIdTextures[i])->Bind(5 + i);
                glBindSampler(5 + i, 0);
                glUniform1i(u3D.shadowIdMap[i], 5 + i);
            }
        }
    }

    const int texInfoOverride = texInfoOverrideEnabled ? 1 : 0;
    glUniform1i(u3D.texInfoOverrideEnabled, texInfoOverride);
    if (texInfoOverride != 0) {
        const float tpage = static_cast<float>((texInfoOverrideWord >> 16) & 0xFFFFu);
        const float cba = static_cast<float>(texInfoOverrideWord & 0xFFFFu);
        glUniform2f(u3D.texInfoOverride, tpage, cba);
    }
    else {
        glUniform2f(u3D.texInfoOverride, -1.0f, 0.0f);
    }

    glUniform1i(u3D.hasVRAM, vramHandle ? 1 : 0);
    if (vramHandle) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vramHandle);
        glUniform1i(u3D.vram, 0);
    }

    const int useRealTexture = (realTextureModeEnabled && currentTexture) ? 1 : 0;
    glUniform1i(u3D.realTextureMode, useRealTexture);
    if (useRealTexture) {
        static_cast<glTexture*>(currentTexture)->Bind(1);
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

    auto* glBuf = static_cast<glPrimBuffer*>(buffer);
    glBindVertexArray(glBuf->GetVAO());
    glDrawElements(glMode, drawCount, GL_UNSIGNED_SHORT, indexPtr);
}

// glDevice

pddiDisplay* glDevice::NewDisplay() { return new glDisplay(); }
pddiRenderContext* glDevice::NewRenderContext(pddiDisplay* d) { return new glContext(static_cast<glDisplay*>(d)); }
pddiGamepad* glDevice::NewGamepad() { return new glGamepad(); }
pddiTexture* glDevice::NewTexture() { return new glTexture(); }
pddiPrimBuffer* glDevice::NewPrimBuffer(const pddiPrimBufferDesc& desc) { return new glPrimBuffer(desc); }
pddiBaseShader* glDevice::NewShader(const char* type) {
    glShader* shader = new glShader(type);
    if (!shader->IsValid()) {
        shader->Release();
        return nullptr;
    }
    return shader;
}

// glGamepad

void glGamepad::Poll() {
    connected = false;
    activeJoystickId = -1;

#if defined(P3D_USE_VENDORED_SDL2)
    if (glGamepadPollState(buttons, axes)) {
        connected = true;
        activeJoystickId = GLFW_JOYSTICK_1;
        return;
    }
#endif

    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++) {
        if (!glfwJoystickIsGamepad(jid)) {
            continue;
        }

        GLFWgamepadstate state;
        if (glfwGetGamepadState(jid, &state)) {
            connected = true;
            activeJoystickId = jid;
            for (int b = 0; b < GamepadButton::COUNT; b++) {
                buttons[b] = (state.buttons[b] == GLFW_PRESS);
            }
            for (int a = 0; a < GamepadAxis::COUNT; a++) {
                axes[a] = state.axes[a];
            }
            break;
        }
    }

    if (!connected) {
        std::memset(buttons, 0, sizeof(buttons));
        std::memset(axes, 0, sizeof(axes));
        glGamepadRumbleUpdateActive(-1, nullptr);
    }
    else {
        glGamepadRumbleUpdateActive(activeJoystickId - GLFW_JOYSTICK_1, glfwGetJoystickGUID(activeJoystickId));
    }
}

bool glGamepad::IsButtonDown(int button) const {
    if (button < 0 || button >= GamepadButton::COUNT) {
        return false;
    }
    return buttons[button];
}

float glGamepad::GetAxis(int axis) const {
    if (axis < 0 || axis >= GamepadAxis::COUNT) {
        return 0.0f;
    }
    return axes[axis];
}

bool glGamepad::SupportsVibration() const {
#if defined(P3D_USE_VENDORED_SDL2)
    (void)glGamepadRumbleUpdateActive(-1, nullptr);
    return glGamepadRumbleSupports();
#else
    const int hintIndex = (connected && activeJoystickId >= GLFW_JOYSTICK_1)
        ? (activeJoystickId - GLFW_JOYSTICK_1)
        : -1;
    const char* guid = (connected && activeJoystickId >= GLFW_JOYSTICK_1)
        ? glfwGetJoystickGUID(activeJoystickId)
        : nullptr;

    if (!glGamepadRumbleUpdateActive(hintIndex, guid)) {
        return false;
    }

    return glGamepadRumbleSupports();
#endif
}

bool glGamepad::SetVibration(float lowFrequency, float highFrequency) {
#if defined(P3D_USE_VENDORED_SDL2)
    (void)glGamepadRumbleUpdateActive(-1, nullptr);
    return glGamepadRumbleSet(lowFrequency, highFrequency);
#else
    const int hintIndex = (connected && activeJoystickId >= GLFW_JOYSTICK_1)
        ? (activeJoystickId - GLFW_JOYSTICK_1)
        : -1;
    const char* guid = (connected && activeJoystickId >= GLFW_JOYSTICK_1)
        ? glfwGetJoystickGUID(activeJoystickId)
        : nullptr;

    if (!glGamepadRumbleUpdateActive(hintIndex, guid)) {
        return false;
    }

    return glGamepadRumbleSet(lowFrequency, highFrequency);
#endif
}

// Platform factory

pddiDevice* pddiCreate() { return new glDevice(); }

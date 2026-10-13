// metalrender.mm — Metal implementation of the pddi interfaces (Objective-C++).
//
// Compiled without ARC (matching the vendored GLFW Cocoa sources), so every
// native object stored in a void* handle is explicitly retained and released.
//
// The macOS `Fixed` typedef from MacTypes.h would collide with the project's
// global `struct Fixed` (core.h). Renaming it to MacFixedType only for the
// duration of the system framework includes resolves the clash.
#include "pddi/metal/metalrender.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

#include <GLFW/glfw3.h>
#define Fixed MacFixedType
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#undef Fixed

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_metal.h>

// --- Native helpers ---------------------------------------------------------

static inline void* mtOwned(id obj) {
    return obj ? (__bridge void*)[obj retain] : nullptr;
}

static inline void* mtNew(id obj) {
    return obj ? (__bridge void*)obj : nullptr;
}

static inline id mtId(const void* handle) {
    return (__bridge id)const_cast<void*>(handle);
}

static inline void mtFree(void*& handle) {
    if (handle) {
        [mtId(handle) release];
        handle = nullptr;
    }
}

// --- Uniform layouts (must mirror the MSL structs) --------------------------

struct alignas(16) Mat4Pod { float m[16]; };

struct MtVertexUniforms {
    Mat4Pod mvp;
    Mat4Pod world;
    Mat4Pod view;
    float cameraPos[4];
    u32 stride;
    u32 posOffset;
    u32 colOffset;
    u32 uvOffset;
    u32 texInfoOffset;
    u32 hasColor;
    u32 hasUV;
    u32 hasTexInfo;
};

struct MtFragmentUniforms {
    float alphaScale;
    u32 useZeroTexelKey;
    u32 hasVRAM;
    u32 texInfoOverrideEnabled;
    float texInfoOverride[2];
    u32 realTextureMode;
    u32 pad;
    float realTexOffset[2];
    float realTexSize[2];
};

struct Mt2DUniforms {
    Mat4Pod proj;
    float tint[4];
    float flipV;
};

struct MtTiltUniforms {
    Mat4Pod proj;
    float rect[4];
    float angles[4];
};

struct MtVariantUniforms {
    float a[4];
    float b[4];
    float c[4];
    float tint[4];
    float f0;
    float f1;
    float f2;
    float f3;
    float flipV;
};

struct MtShadowUniforms {
    Mat4Pod lightVP[3];
    float cascadeSplits[3];
    float cascadeBlendDistances[3];
    float shadowTexelWorldSize[3];
    float shadowBias[3];
    float shadowLightDir[4];
    u32 receiveShadows;
    u32 shadowCascadeCount;
    u32 shadowFilterQuality;
    u32 receiverInstanceId;
    u32 shadowDebugMode;
    u32 pad0;
    u32 pad1;
    u32 pad2;
};

static constexpr u32 kOffsetNone = 0xFFFFFFFFu;

// --- Shader source (MSL, compiled at runtime via newLibraryWithSource:) ------

static const char* kMetalSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct MtVertexUniforms {
    float4x4 mvp;
    float4x4 world;
    float4x4 view;
    float4 cameraPos;
    uint stride;
    uint posOffset;
    uint colOffset;
    uint uvOffset;
    uint texInfoOffset;
    uint hasColor;
    uint hasUV;
    uint hasTexInfo;
};

struct MtFragmentUniforms {
    float alphaScale;
    uint useZeroTexelKey;
    uint hasVRAM;
    uint texInfoOverrideEnabled;
    float2 texInfoOverride;
    uint realTextureMode;
    uint pad;
    float2 realTexOffset;
    float2 realTexSize;
};

struct MtShadowUniforms {
    float4x4 lightVP[3];
    float cascadeSplits[3];
    float cascadeBlendDistances[3];
    float shadowTexelWorldSize[3];
    float shadowBias[3];
    float4 shadowLightDir;
    uint receiveShadows;
    uint shadowCascadeCount;
    uint shadowFilterQuality;
    uint receiverInstanceId;
    uint shadowDebugMode;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct MtVaryings {
    float4 position [[position]];
    float3 color [[center_no_perspective]];
    float2 uv [[center_no_perspective]];
    float2 texInfo [[flat]];
    float3 worldPos;
    float viewDepth;
};

static inline float glsl_mod(float x, float m) {
    return x - m * floor(x / m);
}

vertex MtVaryings mt_vs(uint vid [[vertex_id]],
                        const device char* vdata [[buffer(0)]],
                        constant MtVertexUniforms& u [[buffer(1)]])
{
    MtVaryings out;
    const device char* base = vdata + (ulong)vid * (ulong)u.stride;

    float3 pos = float3(0.0);
    if (u.posOffset != 0xFFFFFFFFu) {
        pos = float3(*(const device packed_float3*)(base + u.posOffset));
    }
    float3 color = float3(0.0);
    if (u.hasColor != 0u) {
        color = float3(*(const device packed_float3*)(base + u.colOffset));
    }
    float2 uv = float2(0.0);
    if (u.hasUV != 0u) {
        uv = float2(*(const device packed_float2*)(base + u.uvOffset));
    }
    float2 texInfo = float2(0.0);
    if (u.hasTexInfo != 0u) {
        texInfo = float2(*(const device packed_float2*)(base + u.texInfoOffset));
    }

    float4 world = u.world * float4(pos, 1.0);
    float4 viewPos = u.view * world;
    out.position = u.mvp * float4(pos, 1.0);
    out.color = color;
    out.uv = uv;
    out.texInfo = texInfo;
    out.worldPos = world.xyz;
    out.viewDepth = max(-viewPos.z, 0.0);
    return out;
}

// --- Shadow cascade sampling (CSM) ------------------------------------------

float3 ComputeFlatNormal(MtVaryings in, constant MtShadowUniforms& s) {
    float3 n = cross(dfdx(in.worldPos), dfdy(in.worldPos));
    float lenSq = dot(n, n);
    if (lenSq < 1e-8) {
        return s.shadowLightDir.xyz;
    }
    n *= rsqrt(lenSq);
    if (dot(n, s.shadowLightDir.xyz) < 0.0) {
        n = -n;
    }
    return n;
}

constant float2 kPoissonDisk[8] = {
    float2(-0.613392, 0.617481), float2(0.170019, -0.040254),
    float2(-0.299417, 0.791925), float2(0.645680, 0.493210),
    float2(-0.651784, 0.717887), float2(0.421003, 0.027070),
    float2(-0.817194, -0.271096), float2(-0.705374, -0.668203)
};

float InterleavedGradientNoise(float2 fragCoord) {
    return fract(52.9829189 * fract(dot(fragCoord, float2(0.06711056, 0.00583715))));
}

uint SampleShadowCasterId(int c, float2 uv,
                          texture2d<uint, access::read> i0,
                          texture2d<uint, access::read> i1,
                          texture2d<uint, access::read> i2) {
    texture2d<uint, access::read> idMap = c == 0 ? i0 : (c == 1 ? i1 : i2);
    uint2 size = uint2(idMap.get_width(), idMap.get_height());
    if (size.x == 0u || size.y == 0u) {
        return 0u;
    }
    uint2 p = min(uint2(clamp(uv, 0.0, 1.0) * float2(size)), size - 1u);
    return idMap.read(p).r;
}

float SampleShadowLit(int c, float2 uv, float compareDepth,
                      depth2d<float> m0, depth2d<float> m1, depth2d<float> m2,
                      texture2d<uint, access::read> i0,
                      texture2d<uint, access::read> i1,
                      texture2d<uint, access::read> i2,
                      sampler cmp, constant MtShadowUniforms& s) {
    if (s.receiverInstanceId != 0u &&
        SampleShadowCasterId(c, uv, i0, i1, i2) == s.receiverInstanceId) {
        return 1.0;
    }
    depth2d<float> map = c == 0 ? m0 : (c == 1 ? m1 : m2);
    return map.sample_compare(cmp, uv, compareDepth);
}

float2 ReceiverPlaneDepthGradient(float2 uv, float receiverDepth) {
    float2 uvDx = dfdx(uv);
    float2 uvDy = dfdy(uv);
    float depthDx = dfdx(receiverDepth);
    float depthDy = dfdy(receiverDepth);
    float det = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(det) < 1e-8) {
        return float2(0.0);
    }
    float invDet = 1.0 / det;
    return float2((depthDx * uvDy.y - depthDy * uvDx.y) * invDet,
                  (depthDy * uvDx.x - depthDx * uvDy.x) * invDet);
}

float ShadowCascadeCoverage(float4 lightClip, constant MtShadowUniforms& s) {
    float3 ndc = lightClip.xyz / lightClip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float receiverDepth = ndc.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        receiverDepth < 0.0 || receiverDepth > 1.0) {
        return 0.0;
    }
    float borderDistance = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    float borderFadeWidth = (s.shadowFilterQuality >= 2u) ? 0.040 : 0.055;
    return smoothstep(0.0, borderFadeWidth, borderDistance);
}

float SampleShadowCascade(int c, float4 lightClip,
                          depth2d<float> m0, depth2d<float> m1, depth2d<float> m2,
                          texture2d<uint, access::read> i0,
                          texture2d<uint, access::read> i1,
                          texture2d<uint, access::read> i2,
                          sampler cmp, constant MtShadowUniforms& s, float2 fragCoord) {
    float3 ndc = lightClip.xyz / lightClip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float receiverDepth = ndc.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        receiverDepth < 0.0 || receiverDepth > 1.0) {
        return 1.0;
    }
    depth2d<float> map = c == 0 ? m0 : (c == 1 ? m1 : m2);
    float2 texel = 1.0 / float2(max(map.get_width(), 1u), max(map.get_height(), 1u));
    float screenDepthSlope = abs(dfdx(receiverDepth)) + abs(dfdy(receiverDepth));
    float slopeBias = min(screenDepthSlope * 0.6, 0.00065);
    float bias = min(s.shadowBias[c] + slopeBias, 0.0026);
    float2 depthGradient = ReceiverPlaneDepthGradient(uv, receiverDepth);
    float diskRadiusTexels = s.shadowFilterQuality == 0u ? 2.6
                            : s.shadowFilterQuality == 1u ? 1.6
                            : s.shadowFilterQuality == 2u ? 1.3 : 1.2;
    float angle = InterleavedGradientNoise(fragCoord) * 6.2831853;
    float sn = sin(angle), cs = cos(angle);
    float2x2 rot = float2x2(float2(cs, -sn), float2(sn, cs));

    float lit = 0.0;
    for (int i = 0; i < 8; i++) {
        float2 offset = (rot * kPoissonDisk[i]) * diskRadiusTexels * texel;
        float tapPlaneBias = abs(clamp(dot(depthGradient, offset), -0.00065, 0.00065));
        float tapBias = min(bias + tapPlaneBias * 0.5, 0.0032);
        lit += SampleShadowLit(c, uv + offset, receiverDepth + tapBias,
                               m0, m1, m2, i0, i1, i2, cmp, s);
    }
    return mix(0.45, 1.0, lit / 8.0);
}

float2 SampleCoveredCascade(int c, float3 shadowNormal, MtVaryings in,
                            depth2d<float> m0, depth2d<float> m1, depth2d<float> m2,
                            texture2d<uint, access::read> i0,
                            texture2d<uint, access::read> i1,
                            texture2d<uint, access::read> i2,
                            sampler cmp, constant MtShadowUniforms& s) {
    float NdotL = max(dot(shadowNormal, s.shadowLightDir.xyz), 0.15);
    float offsetScale = clamp(1.0 / NdotL, 0.6, 1.8);
    float3 offsetPos = in.worldPos + shadowNormal * s.shadowTexelWorldSize[c] * offsetScale;
    float4 lightClip = s.lightVP[c] * float4(offsetPos, 1.0);
    float coverage = ShadowCascadeCoverage(lightClip, s);
    if (coverage <= 0.0) {
        return float2(1.0, 0.0);
    }
    float shadow = SampleShadowCascade(c, lightClip, m0, m1, m2, i0, i1, i2,
                                       cmp, s, in.position.xy);
    return float2(shadow, coverage);
}

float ComputeShadowFactor(MtVaryings in,
                          depth2d<float> m0, depth2d<float> m1, depth2d<float> m2,
                          texture2d<uint, access::read> i0,
                          texture2d<uint, access::read> i1,
                          texture2d<uint, access::read> i2,
                          sampler cmp, constant MtShadowUniforms& s) {
    if (s.receiveShadows == 0u || s.shadowCascadeCount == 0u) {
        return 1.0;
    }
    float maxShadowDepth = s.cascadeSplits[s.shadowCascadeCount - 1];
    float fadeDistance = max(maxShadowDepth * 0.10, 1200.0);
    if (in.viewDepth >= maxShadowDepth) {
        return 1.0;
    }
    float3 shadowNormal = ComputeFlatNormal(in, s);

    for (int i = 0; i < 3; i++) {
        if (i >= int(s.shadowCascadeCount)) break;
        if (in.viewDepth <= s.cascadeSplits[i] || i == int(s.shadowCascadeCount) - 1) {
            if (s.shadowDebugMode == 2u) {
                return 0.3;
            }
            float2 base = SampleCoveredCascade(i, shadowNormal, in, m0, m1, m2, i0, i1, i2, cmp, s);
            float shadow = base.x;
            float coverage = base.y;

            int fallbackCascade = i;
            float fallbackCoverage = coverage;
            float fallbackShadow = shadow;
            for (int j = i + 1; j < 3; j++) {
                if (j >= int(s.shadowCascadeCount)) break;
                float2 cand = SampleCoveredCascade(j, shadowNormal, in, m0, m1, m2, i0, i1, i2, cmp, s);
                if (cand.y > fallbackCoverage) {
                    fallbackCascade = j;
                    fallbackCoverage = cand.y;
                    fallbackShadow = cand.x;
                }
                if (cand.y >= 0.999) break;
            }
            if (fallbackCascade != i) {
                shadow = (coverage > 0.0) ? mix(fallbackShadow, shadow, coverage) : fallbackShadow;
            }

            if (i < int(s.shadowCascadeCount) - 1) {
                float blendDistance = max(s.cascadeBlendDistances[i], 0.0);
                float blendStart = s.cascadeSplits[i] - blendDistance;
                if (blendDistance > 0.0 && in.viewDepth > blendStart) {
                    float2 next = SampleCoveredCascade(i + 1, shadowNormal, in, m0, m1, m2, i0, i1, i2, cmp, s);
                    float nextShadow = (next.y <= 0.0) ? shadow : next.x;
                    float blend = smoothstep(blendStart, s.cascadeSplits[i], in.viewDepth);
                    shadow = mix(shadow, nextShadow, blend);
                }
            }
            if (i == int(s.shadowCascadeCount) - 1) {
                float fadeStart = maxShadowDepth - fadeDistance;
                shadow = mix(shadow, 1.0, smoothstep(fadeStart, maxShadowDepth, in.viewDepth));
            }
            return shadow;
        }
    }
    return 1.0;
}

float3 ApplyShadowDebugTint(float3 baseColor, constant MtShadowUniforms& s) {
    if (s.shadowDebugMode != 1u || s.receiveShadows == 0u) {
        return baseColor;
    }
    return baseColor;
}

fragment float4 mt_fs(MtVaryings in [[stage_in]],
                      constant MtFragmentUniforms& u [[buffer(0)]],
                      constant MtShadowUniforms& s [[buffer(2)]],
                      texture2d<uint, access::read> vram [[texture(0)]],
                      texture2d<float> realTex [[texture(1)]],
                      depth2d<float> shadowMap0 [[texture(2)]],
                      depth2d<float> shadowMap1 [[texture(3)]],
                      depth2d<float> shadowMap2 [[texture(4)]],
                      texture2d<uint, access::read> shadowId0 [[texture(5)]],
                      texture2d<uint, access::read> shadowId1 [[texture(6)]],
                      texture2d<uint, access::read> shadowId2 [[texture(7)]],
                      sampler texSampler [[sampler(0)]],
                      sampler shadowSampler [[sampler(1)]])
{
    float shadowFactor = ComputeShadowFactor(in, shadowMap0, shadowMap1, shadowMap2,
                                             shadowId0, shadowId1, shadowId2,
                                             shadowSampler, s);

    if (u.realTextureMode != 0u) {
        float2 puv = float2(glsl_mod(in.uv.x + 256.0, 256.0),
                            glsl_mod(in.uv.y + 256.0, 256.0));
        float2 ruv = (puv - u.realTexOffset) / u.realTexSize;
        float4 texColor = realTex.sample(texSampler, ruv);
        if (texColor.a < 0.01) {
            discard_fragment();
        }
        return float4(ApplyShadowDebugTint(texColor.rgb * shadowFactor, s),
                      u.alphaScale * texColor.a) * float4(in.color, 1.0);
    }

    float tpageF = in.texInfo.x;
    float cbaF = in.texInfo.y;
    if (u.texInfoOverrideEnabled != 0u) {
        tpageF = u.texInfoOverride.x;
        cbaF = u.texInfoOverride.y;
    }

    if (u.hasVRAM == 0u || tpageF < 0.0) {
        return float4(ApplyShadowDebugTint(in.color * shadowFactor, s), u.alphaScale);
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

    uint px = uint(glsl_mod(in.uv.x + 256.0, 256.0));
    uint py = uint(glsl_mod(in.uv.y + 256.0, 256.0));

    uint clutWord;
    bool zeroTexel = false;
    if (depth == 0u) {
        uint wordX = pageX + px / 4u;
        uint word = vram.read(uint2(wordX, pageY + py)).r;
        uint palIdx = (word >> ((px % 4u) * 4u)) & 0xFu;
        zeroTexel = (palIdx == 0u);
        clutWord = vram.read(uint2(clutX + palIdx, clutY)).r;
    } else if (depth == 1u) {
        uint wordX = pageX + px / 2u;
        uint word = vram.read(uint2(wordX, pageY + py)).r;
        uint palIdx = (px & 1u) != 0u ? (word >> 8u) & 0xFFu : word & 0xFFu;
        zeroTexel = (palIdx == 0u);
        clutWord = vram.read(uint2(clutX + palIdx, clutY)).r;
    } else {
        clutWord = vram.read(uint2(pageX + px, pageY + py)).r;
        zeroTexel = (clutWord == 0u);
    }

    if (u.useZeroTexelKey != 0u) {
        if (zeroTexel) {
            discard_fragment();
        }
    } else {
        if ((clutWord & 0x7FFFu) == 0x7C1Fu) {
            discard_fragment();
        }
    }

    float r = float(clutWord & 0x1Fu) / 31.0;
    float g = float((clutWord >> 5u) & 0x1Fu) / 31.0;
    float b = float((clutWord >> 10u) & 0x1Fu) / 31.0;

    return float4(ApplyShadowDebugTint(float3(r, g, b) * shadowFactor, s),
                  u.alphaScale) * float4(in.color, 1.0);
}

// --- Shadow caster depth pass -----------------------------------------------

struct MtShadowOut {
    float4 position [[position]];
    float2 uv;
    float2 texInfo [[flat]];
};

vertex MtShadowOut mt_sd_vs(uint vid [[vertex_id]],
                            const device char* vdata [[buffer(0)]],
                            constant MtVertexUniforms& u [[buffer(1)]])
{
    MtShadowOut out;
    const device char* base = vdata + (ulong)vid * (ulong)u.stride;
    float3 pos = float3(0.0);
    if (u.posOffset != 0xFFFFFFFFu) {
        pos = float3(*(const device packed_float3*)(base + u.posOffset));
    }
    float2 uv = float2(0.0);
    if (u.hasUV != 0u) {
        uv = float2(*(const device packed_float2*)(base + u.uvOffset));
    }
    float2 texInfo = float2(0.0);
    if (u.hasTexInfo != 0u) {
        texInfo = float2(*(const device packed_float2*)(base + u.texInfoOffset));
    }
    out.position = u.mvp * float4(pos, 1.0);
    out.uv = uv;
    out.texInfo = texInfo;
    return out;
}

fragment uint mt_sd_fs(MtShadowOut in [[stage_in]],
                       constant MtFragmentUniforms& u [[buffer(0)]],
                       constant MtShadowUniforms& s [[buffer(2)]],
                       texture2d<uint, access::read> vram [[texture(0)]],
                       texture2d<float> realTex [[texture(1)]],
                       sampler texSampler [[sampler(0)]])
{
    if (u.realTextureMode != 0u) {
        float2 puv = float2(glsl_mod(in.uv.x + 256.0, 256.0),
                            glsl_mod(in.uv.y + 256.0, 256.0));
        float2 ruv = (puv - u.realTexOffset) / u.realTexSize;
        if (realTex.sample(texSampler, ruv).a < 0.01) {
            discard_fragment();
        }
        return s.receiverInstanceId;
    }
    float tpageF = in.texInfo.x;
    float cbaF = in.texInfo.y;
    if (u.texInfoOverrideEnabled != 0u) {
        tpageF = u.texInfoOverride.x;
        cbaF = u.texInfoOverride.y;
    }
    if (u.hasVRAM == 0u || tpageF < 0.0) {
        return s.receiverInstanceId;
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
    uint px = uint(glsl_mod(in.uv.x + 256.0, 256.0));
    uint py = uint(glsl_mod(in.uv.y + 256.0, 256.0));
    uint clutWord;
    bool zeroTexel = false;
    if (depth == 0u) {
        uint word = vram.read(uint2(pageX + px / 4u, pageY + py)).r;
        uint palIdx = (word >> ((px % 4u) * 4u)) & 0xFu;
        zeroTexel = (palIdx == 0u);
        clutWord = vram.read(uint2(clutX + palIdx, clutY)).r;
    } else if (depth == 1u) {
        uint word = vram.read(uint2(pageX + px / 2u, pageY + py)).r;
        uint palIdx = (px & 1u) != 0u ? (word >> 8u) & 0xFFu : word & 0xFFu;
        zeroTexel = (palIdx == 0u);
        clutWord = vram.read(uint2(clutX + palIdx, clutY)).r;
    } else {
        clutWord = vram.read(uint2(pageX + px, pageY + py)).r;
        zeroTexel = (clutWord == 0u);
    }
    if (u.useZeroTexelKey != 0u) {
        if (zeroTexel) discard_fragment();
    } else {
        if ((clutWord & 0x7FFFu) == 0x7C1Fu) discard_fragment();
    }
    return s.receiverInstanceId;
}

// --- 2D immediate paths (quad / circles) ------------------------------------

struct Mt2DUniforms {
    float4x4 proj;
    float4 tint;
    float flipV;
};

struct Mt2DOut {
    float4 position [[position]];
    float2 uv;
};

vertex Mt2DOut mt2d_vs(uint vid [[vertex_id]],
                       const device float4* verts [[buffer(0)]],
                       constant Mt2DUniforms& u [[buffer(1)]])
{
    float4 v = verts[vid];
    Mt2DOut out;
    out.position = u.proj * float4(v.xy, 0.0, 1.0);
    out.uv = v.zw;
    return out;
}

fragment float4 mt2d_fs(Mt2DOut in [[stage_in]],
                        constant Mt2DUniforms& u [[buffer(1)]],
                        texture2d<float> tex [[texture(0)]],
                        sampler s [[sampler(0)]])
{
    float2 uv = in.uv;
    if (u.flipV != 0.0) { uv.y = 1.0 - uv.y; }
    return tex.sample(s, uv) * u.tint;
}

// Pseudo-3D tilt: rebuilds the quad from aUV and rotates it in 3D.
struct MtTiltUniforms {
    float4x4 proj;
    float4 rect;
    float4 angles;
};

vertex Mt2DOut mttilt_vs(uint vid [[vertex_id]],
                         const device float4* verts [[buffer(0)]],
                         constant MtTiltUniforms& u [[buffer(1)]])
{
    float4 v = verts[vid];
    float2 local = (v.zw - 0.5) * 2.0 * u.rect.zw;
    float3 p = float3(local, 0.0);

    float cx = cos(u.angles.x), sx = sin(u.angles.x);
    p = float3(p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx);
    float cy = cos(u.angles.y), sy = sin(u.angles.y);
    p = float3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy);
    float cz = cos(u.angles.z), sz = sin(u.angles.z);
    p = float3(p.x * cz - p.y * sz, p.x * sz + p.y * cz, p.z);

    float focal = max(u.angles.w, 1.0);
    float perspective = focal / max(focal - p.z, 0.0001);
    float2 screenPos = u.rect.xy + p.xy * perspective;

    Mt2DOut out;
    out.position = u.proj * float4(screenPos, 0.0, 1.0);
    out.uv = v.zw;
    return out;
}

// --- Gouraud-shaded quads ---------------------------------------------------

struct MtGouraudOut {
    float4 position [[position]];
    float4 color;
};

vertex MtGouraudOut mtg_vs(uint vid [[vertex_id]],
                           const device float* v [[buffer(0)]],
                           constant float4x4& proj [[buffer(1)]])
{
    MtGouraudOut out;
    out.position = proj * float4(v[vid * 6 + 0], v[vid * 6 + 1], 0.0, 1.0);
    out.color = float4(v[vid * 6 + 2], v[vid * 6 + 3], v[vid * 6 + 4], v[vid * 6 + 5]);
    return out;
}

fragment float4 mtg_fs(MtGouraudOut in [[stage_in]])
{
    return in.color;
}

// --- Batched 2D quads -------------------------------------------------------

struct MtBatchVertex {
    packed_float2 pos;
    packed_float2 uv;
    uchar4 color;
};

struct MtBatchOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};

vertex MtBatchOut mtb_vs(uint vid [[vertex_id]],
                         const device MtBatchVertex* verts [[buffer(0)]],
                         constant float4x4& proj [[buffer(1)]])
{
    MtBatchVertex v = verts[vid];
    MtBatchOut out;
    out.position = proj * float4(v.pos, 0.0, 1.0);
    out.uv = v.uv;
    out.color = float4(v.color) / 255.0;
    return out;
}

fragment float4 mtb_fs(MtBatchOut in [[stage_in]],
                       texture2d<float> tex [[texture(0)]],
                       sampler s [[sampler(0)]])
{
    return tex.sample(s, in.uv) * in.color;
}

// --- Title screen / movie effect variants -----------------------------------

struct MtVariantUniforms {
    float4 a;
    float4 b;
    float4 c;
    float4 tint;
    float f0;
    float f1;
    float f2;
    float f3;
    float flipV;
};

fragment float4 mtglow_fs(Mt2DOut in [[stage_in]],
                          constant MtVariantUniforms& u [[buffer(1)]],
                          texture2d<float> tex [[texture(0)]],
                          sampler s [[sampler(0)]])
{
    float2 uv0 = in.uv;
    if (u.flipV != 0.0) { uv0.y = 1.0 - uv0.y; }
    float2 driftedUV = uv0 + float2(cos(u.f0 * u.b.z), sin(u.f0 * u.b.z * 1.21)) * u.b.y;
    float4 center = tex.sample(s, uv0);
    float3 color = center.rgb * center.a;
    float alpha = center.a;
    float total = 1.0;
    float rot = u.f0 * u.b.x;
    for (int ring = 1; ring <= 3; ++ring) {
        float radius = u.a.x * (float(ring) / 3.0);
        float weight = 1.0 / float(ring);
        for (int dir = 0; dir < 8; ++dir) {
            float angle = (float(dir) / 8.0) * 6.28318530718 + rot + float(ring) * 0.35;
            float2 offset = float2(cos(angle), sin(angle)) * radius;
            float4 tap = tex.sample(s, driftedUV + offset);
            color += tap.rgb * tap.a * weight;
            alpha += tap.a * weight;
            total += weight;
        }
    }
    color /= total;
    alpha /= total;
    return float4(color * u.a.y, alpha * u.a.y);
}

fragment float4 mtrays_fs(Mt2DOut in [[stage_in]],
                          constant MtVariantUniforms& u [[buffer(1)]],
                          texture2d<float> tex [[texture(0)]],
                          sampler s [[sampler(0)]])
{
    const int kSampleCount = 48;
    float2 origin = u.a.xy + float2(cos(u.f1 * u.b.w), sin(u.f1 * u.b.w * 1.37)) * u.b.z;
    float2 uv = in.uv;
    if (u.flipV != 0.0) { uv.y = 1.0 - uv.y; }
    float4 source = tex.sample(s, uv);
    float3 color = source.rgb * source.a;
    float decay = 1.0;
    float stepFrac = u.a.z / float(kSampleCount);
    for (int i = 0; i < kSampleCount; ++i) {
        float2 step = (origin - uv) * stepFrac;
        float twist = u.b.x * sin(u.f1 * u.b.y + float(i) * 0.22);
        float ca = cos(twist), sa = sin(twist);
        step = float2(step.x * ca - step.y * sa, step.x * sa + step.y * ca);
        uv += step;
        float4 tap = tex.sample(s, uv);
        color += tap.rgb * tap.a * decay;
        decay *= u.a.w;
    }
    return float4(color * (u.f0 / float(kSampleCount)), 1.0);
}

float hash11(float v) {
    return fract(sin(v * 127.1) * 43758.5453123);
}

float angularNoise(float v) {
    float cell = floor(v);
    float blend = fract(v);
    blend = blend * blend * (3.0 - 2.0 * blend);
    return mix(hash11(cell), hash11(cell + 1.0), blend);
}

fragment float4 mtdot_fs(Mt2DOut in [[stage_in]],
                         constant MtVariantUniforms& u [[buffer(1)]])
{
    float2 centered = (in.uv - 0.5) * 2.0;
    float dist = length(centered);
    float angle = atan2(centered.y, centered.x);
    float n1 = angularNoise(angle * 2.4 + u.f0);
    float n2 = angularNoise(angle * 5.1 - u.f0 * 1.7 + 4.0);
    float wobble = (n1 * 0.6 + n2 * 0.4) - 0.5;
    float edge = 0.74 + wobble * 0.36;
    float alpha = 1.0 - smoothstep(edge - 0.18, edge, dist);
    if (alpha <= 0.0) {
        discard_fragment();
    }
    return float4(u.tint.rgb, u.tint.a * alpha);
}

fragment float4 mtdenoise_fs(Mt2DOut in [[stage_in]],
                             constant MtVariantUniforms& u [[buffer(1)]],
                             texture2d<float> tex [[texture(0)]],
                             sampler s [[sampler(0)]])
{
    float2 uv = in.uv;
    if (u.flipV != 0.0) { uv.y = 1.0 - uv.y; }
    float3 c = tex.sample(s, uv).rgb;
    float sigma = mix(0.02, 0.18, clamp(u.f0, 0.0, 1.0));
    float invSigma2 = 1.0 / (sigma * sigma);
    float3 result = c;
    float totalW = 1.0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            float2 off = float2(float(dx), float(dy)) * u.a.xy;
            float3 smp = tex.sample(s, uv + off).rgb;
            float3 diff = c - smp;
            float w = exp(-dot(diff, diff) * invSigma2);
            if (dx != 0 && dy != 0) w *= 0.707;
            result += smp * w;
            totalW += w;
        }
    }
    return float4(result / totalW, 1.0);
}

float4 CubicWeights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return float4(-0.5*t3 + t2 - 0.5*t,
                   1.5*t3 - 2.5*t2 + 1.0,
                  -1.5*t3 + 2.0*t2 + 0.5*t,
                   0.5*t3 - 0.5*t2);
}

fragment float4 mtupscale_fs(Mt2DOut in [[stage_in]],
                             constant MtVariantUniforms& u [[buffer(1)]],
                             texture2d<float> tex [[texture(0)]],
                             sampler s [[sampler(0)]])
{
    float2 uvBase = in.uv;
    if (u.flipV != 0.0) { uvBase.y = 1.0 - uvBase.y; }
    float2 pos = uvBase / u.a.xy - 0.5;
    float2 f = fract(pos);
    float2 base = (floor(pos) + 0.5) * u.a.xy;
    float4 wx = CubicWeights(f.x);
    float4 wy = CubicWeights(f.y);
    float3 color = float3(0.0);
    for (int j = 0; j < 4; j++) {
        float3 row = float3(0.0);
        for (int i = 0; i < 4; i++) {
            float2 off = float2(float(i - 1), float(j - 1)) * u.a.xy;
            row += tex.sample(s, base + off).rgb * wx[i];
        }
        color += row * wy[j];
    }
    return float4(clamp(color, 0.0, 1.0), 1.0);
}

fragment float4 mtsharp_fs(Mt2DOut in [[stage_in]],
                           constant MtVariantUniforms& u [[buffer(1)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler s [[sampler(0)]])
{
    float2 uv = in.uv;
    if (u.flipV != 0.0) { uv.y = 1.0 - uv.y; }
    float3 c = tex.sample(s, uv).rgb;
    float3 n = tex.sample(s, uv + float2(0.0, -u.a.y)).rgb;
    float3 so = tex.sample(s, uv + float2(0.0, u.a.y)).rgb;
    float3 e = tex.sample(s, uv + float2(u.a.x, 0.0)).rgb;
    float3 w = tex.sample(s, uv + float2(-u.a.x, 0.0)).rgb;
    float3 blur = (n + so + e + w) * 0.25;
    float3 sharp = c + (c - blur) * u.f0;
    return float4(clamp(sharp, 0.0, 1.0), 1.0);
}
)MSL";

// --- Buffers ----------------------------------------------------------------

mtPrimBuffer::mtPrimBuffer(const pddiPrimBufferDesc& desc)
    : primType(desc.primType), vertexFormat(desc.vertexFormat)
    , vertexCount(desc.vertexCount), indexCount(desc.indexCount) {
    u32 offset = 0;
    if (vertexFormat & PDDI_V_POSITION) { posOffset = offset; offset += 3 * sizeof(f32); }
    else posOffset = kOffsetNone;
    if (vertexFormat & PDDI_V_COLOUR)   { colOffset = offset; offset += 3 * sizeof(f32); }
    else colOffset = kOffsetNone;
    if (vertexFormat & PDDI_V_UV)       { uvOffset = offset; offset += 2 * sizeof(f32); }
    else uvOffset = kOffsetNone;
    if (vertexFormat & PDDI_V_TEXINFO)  { texInfoOffset = offset; offset += 2 * sizeof(f32); }
    else texInfoOffset = kOffsetNone;
    stride = offset;

    EnsureVertexBuffer(stride * std::max(vertexCount, 1u));
    EnsureIndexBuffer(sizeof(u16) * std::max(indexCount, 1u));
}

mtPrimBuffer::~mtPrimBuffer() {
    mtFree(vertexBuffer);
    mtFree(indexBuffer);
}

void mtPrimBuffer::EnsureVertexBuffer(u32 byteSize) {
    if (byteSize == 0 || (vertexBuffer && byteSize <= vertexCapacity)) {
        return;
    }
    mtFree(vertexBuffer);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;
    vertexBuffer = mtNew([device newBufferWithLength:byteSize options:MTLResourceStorageModeManaged]);
    vertexCapacity = byteSize;
}

void mtPrimBuffer::EnsureIndexBuffer(u32 byteSize) {
    if (byteSize == 0 || (indexBuffer && byteSize <= indexCapacity)) {
        return;
    }
    mtFree(indexBuffer);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;
    indexBuffer = mtNew([device newBufferWithLength:byteSize options:MTLResourceStorageModeManaged]);
    indexCapacity = byteSize;
}

void mtPrimBuffer::SetVertexData(const void* data, u32 count) {
    vertexCount = count;
    if (!data || stride == 0) return;
    const u32 byteSize = stride * count;
    EnsureVertexBuffer(byteSize);
    if (!vertexBuffer) return;
    id<MTLBuffer> buffer = mtId(vertexBuffer);
    std::memcpy([buffer contents], data, byteSize);
    [buffer didModifyRange:NSMakeRange(0, byteSize)];
}

void mtPrimBuffer::SetIndices(const u16* indices, u32 count) {
    indexCount = count;
    if (!indices || count == 0) return;
    const u32 byteSize = sizeof(u16) * count;
    EnsureIndexBuffer(byteSize);
    if (!indexBuffer) return;
    id<MTLBuffer> buffer = mtId(indexBuffer);
    std::memcpy([buffer contents], indices, byteSize);
    [buffer didModifyRange:NSMakeRange(0, byteSize)];
}

// --- Textures ---------------------------------------------------------------

mtTexture::mtTexture() = default;
mtTexture::~mtTexture() { mtFree(texture); }

void mtTexture::AdoptMetalTexture(void* tex, int w, int h, u32 kindIn) {
    mtFree(texture);
    texture = tex;
    width = w;
    height = h;
    kind = kindIn;
    isRenderTarget = true;
    bpp = 32;
    alphaDepth = 8;
}

void mtTexture::SetData(int w, int h, int bppIn, int alphaDepthIn, const void* rgba) {
    width = w;
    height = h;
    bpp = bppIn;
    alphaDepth = alphaDepthIn;

    mtFree(texture);
    if (w <= 0 || h <= 0) return;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;

    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeManaged;
    id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
    if (rgba) {
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
               mipmapLevel:0
                 withBytes:rgba
               bytesPerRow:(NSUInteger)(w * 4)];
    }
    texture = mtNew(tex);
    kind = MT_TEX_RGBA8;
    isRenderTarget = false;
}

void mtTexture::SetFilterMode(pddiFilterMode mode) { filterMode = mode; }
void mtTexture::Bind(int unit) { (void)unit; }

// --- Shaders / render targets -----------------------------------------------

mtShader::mtShader(const char* t) : type(t ? t : "simple") {}
mtShader::~mtShader() = default;

void mtShader::SetTexture(u32 param, pddiTexture* tex) {
    (void)param;
    if (tex) texture = tex;
}
void mtShader::SetInt(u32 param, int value) { (void)param; (void)value; }
void mtShader::SetFloat(u32 param, float value) { (void)param; (void)value; }
void mtShader::SetColour(u32 param, pddiColour c) { (void)param; diffuse = c; }
void mtShader::SetInt(const char* param, int value) { if (param) intParams[param] = value; }
void mtShader::SetFloat(const char* param, float value) { if (param) floatParams[param] = value; }
void mtShader::SetVector(const char* param, float x, float y, float z, float w) {
    if (param) vectorParams[param] = { x, y, z, w };
}
void mtShader::SetMatrix(const char* param, const float* matrix4x4) {
    (void)param;
    (void)matrix4x4;
}
void mtShader::PreRender() {}
void mtShader::PostRender() {}

bool mtRenderTarget::IsDepthFormat() const {
    return format == PDDI_RENDER_TARGET_DEPTH24 ||
           format == PDDI_RENDER_TARGET_DEPTH32F ||
           format == PDDI_RENDER_TARGET_DEPTH;
}

void* mtRenderTarget::GetColorMetalTexture() const {
    return texture ? texture->GetMetalTexture() : nullptr;
}
void* mtRenderTarget::GetDepthMetalTexture() const {
    return (texture && IsDepthFormat()) ? texture->GetMetalTexture() : nullptr;
}
void* mtRenderTarget::GetIdMetalTexture() const {
    return idTexture ? idTexture->GetMetalTexture() : nullptr;
}

mtRenderTarget::mtRenderTarget(int w, int h, pddiRenderTargetFormat fmt, bool withInstanceId)
    : width(w), height(h), format(fmt) {
    if (withInstanceId) {
        idTexture = new mtTexture();
    }
    CreateStorage(w, h);
}

mtRenderTarget::~mtRenderTarget() {
    delete texture;
    delete idTexture;
}

bool mtRenderTarget::CreateStorage(int w, int h) {
    if (w <= 0 || h <= 0) {
        valid = false;
        return false;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        valid = false;
        return false;
    }

    if (!texture) {
        texture = new mtTexture();
    }

    if (IsDepthFormat()) {
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:(NSUInteger)w
                                                              height:(NSUInteger)h
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        texture->AdoptMetalTexture(mtNew([device newTextureWithDescriptor:desc]), w, h, MT_TEX_DEPTH);

        if (idTexture) {
            MTLTextureDescriptor* idDesc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                                   width:(NSUInteger)w
                                                                  height:(NSUInteger)h
                                                               mipmapped:NO];
            idDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            idDesc.storageMode = MTLStorageModePrivate;
            idTexture->AdoptMetalTexture(mtNew([device newTextureWithDescriptor:idDesc]),
                                         w, h, MT_TEX_ID_UINT);
        }
    } else {
        const bool is16f = (format == PDDI_RENDER_TARGET_RGBA16F);
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(is16f ? MTLPixelFormatRGBA16Float
                                                                            : MTLPixelFormatRGBA8Unorm)
                                                               width:(NSUInteger)w
                                                              height:(NSUInteger)h
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        texture->AdoptMetalTexture(mtNew([device newTextureWithDescriptor:desc]),
                                   w, h, is16f ? MT_TEX_RGBA16F : MT_TEX_RGBA8);
    }

    width = w;
    height = h;
    valid = true;
    return true;
}

bool mtRenderTarget::Resize(int w, int h) {
    if (w == width && h == height && valid) {
        return true;
    }
    return CreateStorage(w, h);
}

// --- Device -----------------------------------------------------------------

pddiDisplay* mtDevice::NewDisplay() { return new mtDisplay(); }
pddiRenderContext* mtDevice::NewRenderContext(pddiDisplay* display) {
    return new mtContext(static_cast<mtDisplay*>(display));
}
pddiGamepad* mtDevice::NewGamepad() { return new mtGamepad(); }
pddiTexture* mtDevice::NewTexture() { return new mtTexture(); }
pddiPrimBuffer* mtDevice::NewPrimBuffer(const pddiPrimBufferDesc& desc) {
    return new mtPrimBuffer(desc);
}
pddiBaseShader* mtDevice::NewShader(const char* type) { return new mtShader(type); }

pddiDevice* pddiCreate() { return new mtDevice(); }

// --- Gamepad ----------------------------------------------------------------

mtGamepad::mtGamepad() = default;

mtGamepad::~mtGamepad() {
    if (hidDevice) {
        IOHIDDeviceClose((IOHIDDeviceRef)hidDevice, kIOHIDOptionsTypeNone);
        CFRelease((IOHIDDeviceRef)hidDevice);
    }
}

// Sony controllers (DualSense / DualShock 4) expose rumble as an IOHID output
// report. CoreHaptics is not usable from a plain executable on macOS
// ("Couldn't communicate with a helper application"), so talk HID directly,
// like SDL does.
static int HidIntProperty(IOHIDDeviceRef device, CFStringRef key) {
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    int result = 0;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &result);
    }
    return result;
}

static bool HidIsBluetooth(IOHIDDeviceRef device) {
    CFTypeRef value = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDTransportKey));
    if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
        return CFStringCompare((CFStringRef)value, CFSTR("Bluetooth"), 0) == kCFCompareEqualTo ||
               CFStringCompare((CFStringRef)value, CFSTR("BluetoothLowEnergy"), 0) == kCFCompareEqualTo;
    }
    return false;
}

bool mtGamepad::EnsureHidDevice() {
    if (hidDevice) {
        return true;
    }
    if (hidLookupFailed) {
        return false;
    }

    static IOHIDManagerRef sManager = nullptr;
    if (!sManager) {
        sManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (!sManager) {
            hidLookupFailed = true;
            return false;
        }
        IOHIDManagerSetDeviceMatching(sManager, nullptr);
        IOHIDManagerOpen(sManager, kIOHIDOptionsTypeNone);
    }

    CFSetRef devices = IOHIDManagerCopyDevices(sManager);
    if (!devices) {
        return false;
    }

    bool found = false;
    const CFIndex count = CFSetGetCount(devices);
    if (count > 0) {
        CFTypeRef* refs = (CFTypeRef*)calloc((size_t)count, sizeof(CFTypeRef));
        CFSetGetValues(devices, (const void**)refs);
        for (CFIndex i = 0; i < count && !found; i++) {
            IOHIDDeviceRef device = (IOHIDDeviceRef)refs[i];
            const int vendor = HidIntProperty(device, CFSTR(kIOHIDVendorIDKey));
            const int product = HidIntProperty(device, CFSTR(kIOHIDProductIDKey));
            // Sony: DualSense (0x0CE6), DualSense Edge (0x0DF2), DualShock 4 (0x05C4/0x09CC).
            if (vendor == 0x054C && (product == 0x0CE6 || product == 0x0DF2 ||
                                     product == 0x05C4 || product == 0x09CC)) {
                if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
                    hidDevice = (void*)CFRetain(device);
                    hidProductId = (unsigned)product;
                    vibrationSupported = true;
                    found = true;
                    // Enter the controller's "enhanced" report mode (SDL does this
                    // by sending an effects report with no enable bits set).
                    unsigned char init[48] = {};
                    init[0] = 0x02;
                    IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0x02, init, sizeof(init));
                    std::fprintf(stderr, "mtGamepad: rumble via HID active (product 0x%04X, %s)\n",
                                 hidProductId, HidIsBluetooth(device) ? "bluetooth" : "usb");
                }
            }
        }
        free(refs);
    }
    CFRelease(devices);
    return found;
}

bool mtGamepad::SupportsVibration() const {
    if (vibrationSupported) {
        return true;
    }
    return const_cast<mtGamepad*>(this)->EnsureHidDevice();
}

void mtGamepad::SendRumbleReport() {
    if (!hidDevice) {
        return;
    }
    const unsigned char strong = (unsigned char)(rumbleLow * 255.0f);
    const unsigned char weak = (unsigned char)(rumbleHigh * 255.0f);
    IOHIDDeviceRef device = (IOHIDDeviceRef)hidDevice;

    if (hidProductId == 0x0CE6 || hidProductId == 0x0DF2) {
        if (HidIsBluetooth(device)) {
            return;  // DualSense Bluetooth needs report 0x31 + CRC (not implemented)
        }
        unsigned char report[48] = {};
        report[0] = 0x02;
        report[1] = 0x03;    // enable rumble emulation + disable audio haptics
        report[2] = 0x04;    // enable lightbar colour
        report[3] = weak;    // right motor (high frequency)
        report[4] = strong;  // left motor (low frequency)
        report[45] = lightR; // lightbar R
        report[46] = lightG; // lightbar G
        report[47] = lightB; // lightbar B
        IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0x02, report, sizeof(report));
        return;
    }

    if (HidIsBluetooth(device)) {
        return;  // DualShock 4 Bluetooth needs report 0x11 + CRC (not implemented)
    }
    unsigned char report[32] = {};
    report[0] = 0x05;
    report[1] = 0x01;  // enable rumble
    report[4] = weak;
    report[5] = strong;
    IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0x05, report, sizeof(report));
}

bool mtGamepad::SetVibration(float lowFrequency, float highFrequency) {
    if (!EnsureHidDevice()) {
        return false;
    }

    float low = lowFrequency;
    float high = highFrequency;
    if (low < 0.0f) low = 0.0f;
    if (low > 1.0f) low = 1.0f;
    if (high < 0.0f) high = 0.0f;
    if (high > 1.0f) high = 1.0f;

    static float sLastLow = -1.0f, sLastHigh = -1.0f;
    if (low != sLastLow || high != sLastHigh) {
        sLastLow = low;
        sLastHigh = high;
        std::fprintf(stderr, "mtGamepad: vibration low=%.2f high=%.2f\n", low, high);
    }

    rumbleLow = low;
    rumbleHigh = high;
    SendRumbleReport();
    return true;
}

void mtGamepad::Poll() {
    EnsureHidDevice();

    // Temporary self-test: a ~1s buzz so the rumble path can be verified
    // without needing a specific in-game shock event.
    static int sTestFrames = 0;
    if (hidDevice && sTestFrames >= 0) {
        if (sTestFrames == 0) {
            SetVibration(0.7f, 0.0f);
        }
        if (++sTestFrames > 40) {
            SetVibration(0.0f, 0.0f);
            sTestFrames = -1;
        }
    }

    if (rumbleLow > 0.0f || rumbleHigh > 0.0f) {
        SendRumbleReport();
    }

    connected = false;
    std::memset(buttons, 0, sizeof(buttons));
    std::memset(axes, 0, sizeof(axes));
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++) {
        if (!glfwJoystickIsGamepad(jid)) {
            continue;
        }
        GLFWgamepadstate state;
        if (glfwGetGamepadState(jid, &state)) {
            connected = true;
            for (int b = 0; b < GamepadButton::COUNT; b++) {
                buttons[b] = (state.buttons[b] == GLFW_PRESS);
            }
            for (int a = 0; a < GamepadAxis::COUNT; a++) {
                axes[a] = state.axes[a];
            }
            break;
        }
    }
}

bool mtGamepad::IsButtonDown(int button) const {
    if (button < 0 || button >= GamepadButton::COUNT) return false;
    return buttons[button];
}

float mtGamepad::GetAxis(int axis) const {
    if (axis < 0 || axis >= GamepadAxis::COUNT) return 0.0f;
    return axes[axis];
}

// DualSense / DualShock 4 light bar via the macOS GameController framework.
// Writes are de-duplicated and rate-limited: hammering the controller's HID
// output report every frame can starve input reports, so only update when the
// colour actually changes and at most ~20 Hz.
void mtGamepad::SetLight(unsigned char r, unsigned char g, unsigned char b) {
    if (lightR == r && lightG == g && lightB == b) {
        return;
    }
    lightR = r;
    lightG = g;
    lightB = b;
    // The lightbar shares the controller's output report with the rumble
    // motors, so send one combined report.
    SendRumbleReport();
}

// --- Display ----------------------------------------------------------------

mtDisplay::mtDisplay() = default;

mtDisplay::~mtDisplay() {
    if (imguiInitialized) {
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
    mtFree(drawableTexture);
    mtFree(drawable);
    mtFree(renderEncoder);
    mtFree(commandBuffer);
    mtFree(depthTexture);
    mtFree(layer);
    mtFree(queue);
    mtFree(device);
    if (window) {
        glfwDestroyWindow(static_cast<GLFWwindow*>(window));
        window = nullptr;
        glfwTerminate();
    }
}

bool mtDisplay::InitDisplay(const pddiDisplayInit& init) {
    if (!glfwInit()) {
        std::fprintf(stderr, "mtDisplay: glfwInit failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    windowedW = init.xSize;
    windowedH = init.ySize;

    GLFWwindow* w = glfwCreateWindow(init.xSize, init.ySize,
                                     init.title ? init.title : "ReChan",
                                     nullptr, nullptr);
    if (!w) {
        std::fprintf(stderr, "mtDisplay: glfwCreateWindow failed\n");
        glfwTerminate();
        return false;
    }
    window = w;

    device = mtOwned(MTLCreateSystemDefaultDevice());
    if (!device) {
        std::fprintf(stderr, "mtDisplay: no Metal device available\n");
        return false;
    }
    queue = mtNew([mtId(device) newCommandQueue]);

    NSWindow* nswindow = glfwGetCocoaWindow(w);
    NSView* view = [nswindow contentView];
    view.wantsLayer = YES;
    CAMetalLayer* metalLayer = [CAMetalLayer layer];
    metalLayer.device = mtId(device);
    metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    metalLayer.framebufferOnly = YES;
    metalLayer.contentsScale = nswindow.backingScaleFactor;
    view.layer = metalLayer;
    layer = mtOwned(metalLayer);

    vsync = init.vsync;
    msaaSamples = init.msaa;
    fullscreen = init.fullscreen;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOther(w, true);
    imguiInitialized = ImGui_ImplMetal_Init(mtId(device));

    SyncDrawableSize();
    return true;
}

bool mtDisplay::EnsureDepthTexture() {
    if (fbWidth <= 0 || fbHeight <= 0) return false;
    if (depthTexture && depthWidth == fbWidth && depthHeight == fbHeight) return true;
    mtFree(depthTexture);
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:(NSUInteger)fbWidth
                                                          height:(NSUInteger)fbHeight
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;
    depthTexture = mtNew([mtId(device) newTextureWithDescriptor:desc]);
    depthWidth = fbWidth;
    depthHeight = fbHeight;
    return depthTexture != nullptr;
}

bool mtDisplay::SyncDrawableSize() {
    if (!window || !layer) return false;
    int width = 0, height = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(window), &width, &height);
    if (width <= 0 || height <= 0) return false;
    fbWidth = width;
    fbHeight = height;
    ((CAMetalLayer*)mtId(layer)).drawableSize = CGSizeMake(width, height);
    return EnsureDepthTexture();
}

void* mtDisplay::CreateEncoderForPass(void* colorTex, void* depthTex, void* idTex,
                                      bool clearColor, bool clearDepth, int width, int height) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (colorTex) {
        pass.colorAttachments[0].texture = mtId(colorTex);
        pass.colorAttachments[0].loadAction = clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = idTex
            ? MTLClearColorMake(0.0, 0.0, 0.0, 0.0)
            : MTLClearColorMake(clearColour.r / 255.0, clearColour.g / 255.0,
                                clearColour.b / 255.0, clearColour.a / 255.0);
    }
    if (depthTex) {
        pass.depthAttachment.texture = mtId(depthTex);
        pass.depthAttachment.loadAction = clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 0.0;
    }
    id<MTLRenderCommandEncoder> enc =
        [mtId(commandBuffer) renderCommandEncoderWithDescriptor:pass];
    if (!enc) {
        return nullptr;
    }
    MTLViewport viewport = { 0.0, 0.0, (double)width, (double)height, 0.0, 1.0 };
    [enc setViewport:viewport];
    MTLScissorRect full = { 0, 0, (NSUInteger)std::max(width, 0), (NSUInteger)std::max(height, 0) };
    [enc setScissorRect:full];
    return mtOwned(enc);
}

void mtDisplay::EndCurrentEncoder() {
    if (renderEncoder) {
        [mtId(renderEncoder) endEncoding];
        mtFree(renderEncoder);
    }
}

bool mtDisplay::BeginRenderTargetPass(mtRenderTarget* target) {
    if (!target || !target->IsValid() || !frameActive || !commandBuffer || !renderEncoder) {
        return false;
    }
    EndCurrentEncoder();

    void* colorTex = nullptr;
    void* depthTex = nullptr;
    void* idTex = nullptr;
    if (target->IsDepthFormat()) {
        depthTex = target->GetDepthMetalTexture();
        idTex = target->GetIdMetalTexture();
        colorTex = idTex;
    } else {
        colorTex = target->GetColorMetalTexture();
    }

    renderEncoder = CreateEncoderForPass(colorTex, depthTex, idTex, true, true,
                                         target->GetWidth(), target->GetHeight());
    inTargetPass = renderEncoder != nullptr;
    return inTargetPass;
}

void mtDisplay::EndRenderTargetPass() {
    if (!frameActive) {
        return;
    }
    EndCurrentEncoder();
    renderEncoder = CreateEncoderForPass(drawableTexture, depthTexture, nullptr,
                                         false, false, fbWidth, fbHeight);
    inTargetPass = false;
}

void mtDisplay::BeginFrame() {
    if (!window || frameActive) return;
    if (!SyncDrawableSize()) return;

    id<CAMetalDrawable> nextDrawable = [mtId(layer) nextDrawable];
    if (!nextDrawable) return;
    drawable = mtOwned(nextDrawable);
    drawableTexture = mtOwned(nextDrawable.texture);

    id<MTLCommandBuffer> cmd = [mtId(queue) commandBuffer];
    commandBuffer = mtOwned(cmd);
    renderEncoder = CreateEncoderForPass(drawableTexture, depthTexture, nullptr,
                                         true, true, fbWidth, fbHeight);
    frameActive = renderEncoder != nullptr;

    if (frameActive && imguiInitialized && !imguiFrameStarted) {
        MTLRenderPassDescriptor* imguiPass = [MTLRenderPassDescriptor renderPassDescriptor];
        imguiPass.colorAttachments[0].texture = mtId(drawableTexture);
        ImGui_ImplMetal_NewFrame(imguiPass);
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        imguiFrameStarted = true;
    }
}

void mtDisplay::EndFrame() {
    if (!frameActive) return;
    EndCurrentEncoder();
    inTargetPass = false;
    frameActive = false;
}

void mtDisplay::SwapBuffers() {
    if (!commandBuffer) return;
    id<MTLCommandBuffer> cmd = mtId(commandBuffer);
    if (drawable) {
        [cmd presentDrawable:mtId(drawable)];
    }
    [cmd commit];
    [cmd waitUntilCompleted];
    mtFree(drawableTexture);
    mtFree(drawable);
    mtFree(commandBuffer);
}

bool mtDisplay::ShouldClose() {
    return window ? glfwWindowShouldClose(static_cast<GLFWwindow*>(window)) != 0 : true;
}

void mtDisplay::PollEvents() { glfwPollEvents(); }

bool mtDisplay::IsKeyDown(int key) {
    if (!window) return false;
    return glfwGetKey(static_cast<GLFWwindow*>(window), key) == GLFW_PRESS;
}

bool mtDisplay::IsMouseButtonDown(int button) {
    if (!window) return false;
    return glfwGetMouseButton(static_cast<GLFWwindow*>(window), button) == GLFW_PRESS;
}

void mtDisplay::GetMousePosition(double& x, double& y) {
    x = 0.0; y = 0.0;
    if (!window) return;
    glfwGetCursorPos(static_cast<GLFWwindow*>(window), &x, &y);
}

void mtDisplay::SetIcon(int w, int h, const unsigned char* rgba) {
    if (!window || !rgba) return;
    GLFWimage image;
    image.width = w;
    image.height = h;
    image.pixels = const_cast<unsigned char*>(rgba);
    glfwSetWindowIcon(static_cast<GLFWwindow*>(window), 1, &image);
}

int mtDisplay::GetVideoModeCount() { return 1; }

void mtDisplay::GetVideoMode(int index, pddiVideoMode& mode) {
    (void)index;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor) return;
    const GLFWvidmode* vm = glfwGetVideoMode(monitor);
    if (!vm) return;
    mode.width = vm->width;
    mode.height = vm->height;
    mode.refreshRate = vm->refreshRate;
}

void mtDisplay::SetFullscreen(bool wantFullscreen) {
    if (!window || wantFullscreen == fullscreen) return;
    GLFWwindow* w = static_cast<GLFWwindow*>(window);
    if (wantFullscreen) {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* vm = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (monitor && vm) {
            glfwSetWindowMonitor(w, monitor, 0, 0, vm->width, vm->height, vm->refreshRate);
        }
    } else {
        glfwSetWindowMonitor(w, nullptr, windowedX, windowedY, windowedW, windowedH, 0);
    }
    fullscreen = wantFullscreen;
}

void mtDisplay::SetBorderless(bool wantBorderless) {
    if (!window) return;
    borderless = wantBorderless;
    glfwSetWindowAttrib(static_cast<GLFWwindow*>(window), GLFW_DECORATED,
                        wantBorderless ? GLFW_FALSE : GLFW_TRUE);
}

void mtDisplay::SetResolution(int w, int h) {
    if (!window) return;
    windowedW = w;
    windowedH = h;
    if (!fullscreen) {
        glfwSetWindowSize(static_cast<GLFWwindow*>(window), w, h);
    }
}

void mtDisplay::SetMSAA(int samples) { msaaSamples = std::max(0, samples); }

void mtDisplay::SetWindowPos(int x, int y) {
    if (!window) return;
    windowedX = x;
    windowedY = y;
    glfwSetWindowPos(static_cast<GLFWwindow*>(window), x, y);
}

void mtDisplay::SetTitle(const char* title) {
    if (!window) return;
    glfwSetWindowTitle(static_cast<GLFWwindow*>(window), title ? title : "");
}

void mtDisplay::ShowCursor(bool visible) {
    cursorVisible = visible;
    if (!window) return;
    glfwSetInputMode(static_cast<GLFWwindow*>(window), GLFW_CURSOR,
                     visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

void mtDisplay::ClipCursor(bool clip) {
    cursorClipped = clip;
    if (!window) return;
    glfwSetInputMode(static_cast<GLFWwindow*>(window), GLFW_CURSOR,
                     clip ? GLFW_CURSOR_DISABLED
                          : (cursorVisible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN));
}

void mtDisplay::AddOverlayCallback(OverlayCallback cb) { overlayCallbacks.push_back(cb); }

void mtDisplay::RenderOverlay() {
    if (!imguiFrameStarted) {
        return;
    }
    for (OverlayCallback cb : overlayCallbacks) {
        if (cb) cb();
    }

    ImGui::Render();

    if (commandBuffer && drawableTexture) {
        void* overlayEncoder = CreateEncoderForPass(drawableTexture, depthTexture, nullptr,
                                                    false, false, fbWidth, fbHeight);
        if (overlayEncoder) {
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), mtId(commandBuffer),
                                           mtId(overlayEncoder));
            [mtId(overlayEncoder) endEncoding];
            mtFree(overlayEncoder);
        }
    }
    imguiFrameStarted = false;
}

// --- Context ----------------------------------------------------------------

mtContext::mtContext(mtDisplay* disp) : display(disp) {}
mtContext::~mtContext() {
    for (auto& entry : pipelines) mtFree(entry.second);
    for (auto& entry : samplers) mtFree(entry.second);
    for (auto& entry : depthStates) mtFree(entry.second);
    for (auto& entry : vramTextures) mtFree(entry.second);
    mtFree(compareSampler);
    mtFree(dummyTexture);
    mtFree(dummyVRAM);
    mtFree(dummyDepth);
    mtFree(dummyId);
    mtFree(whiteTexture);
    for (void* buf : dynamicBuffers) mtFree(buf);
    dynamicBuffers.clear();
    mtFree(library);
}

void mtContext::EnsureLibrary() {
    if (library || !display) return;
    id<MTLDevice> device = mtId(display->GetDevice());
    if (!device) return;
    NSError* error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kMetalSource]
                                              options:nil
                                                error:&error];
    if (!lib) {
        std::fprintf(stderr, "mtContext: MSL compile error:\n%s\n",
                     error ? [[error localizedDescription] UTF8String] : "(unknown)");
        return;
    }
    library = mtNew(lib);
}

int mtContext::ActiveSurface() const {
    if (!activeRenderTarget) {
        return MT_SURF_DEFAULT;
    }
    if (activeRenderTarget->IsDepthFormat()) {
        return MT_SURF_SHADOW;
    }
    return activeRenderTarget->GetFormat() == PDDI_RENDER_TARGET_RGBA16F
        ? MT_SURF_RGBA16F : MT_SURF_RGBA8;
}

void* mtContext::GetPipelineState(int program, pddiBlendMode blend, pddiCullMode cull,
                                  bool depthTest, bool depthWrite, int surface) {
    EnsureLibrary();
    if (!library) return nullptr;

    const u64 key = (u64)program | ((u64)blend << 4) | ((u64)cull << 8) |
                    ((u64)depthTest << 11) | ((u64)depthWrite << 12) |
                    ((u64)surface << 13);
    auto it = pipelines.find(key);
    if (it != pipelines.end()) return it->second;

    const char* vsName = "mt_vs";
    const char* fsName = "mt_fs";
    switch (program) {
        case MT_PROG_QUAD2D:  vsName = "mt2d_vs"; fsName = "mt2d_fs"; break;
        case MT_PROG_GOURAUD: vsName = "mtg_vs";  fsName = "mtg_fs";  break;
        case MT_PROG_BATCH:   vsName = "mtb_vs";  fsName = "mtb_fs";  break;
        case MT_PROG_TILT:    vsName = "mttilt_vs"; fsName = "mt2d_fs"; break;
        case MT_PROG_GLOW:    vsName = "mt2d_vs"; fsName = "mtglow_fs"; break;
        case MT_PROG_GODRAYS: vsName = "mt2d_vs"; fsName = "mtrays_fs"; break;
        case MT_PROG_DOT:     vsName = "mt2d_vs"; fsName = "mtdot_fs"; break;
        case MT_PROG_MOVIEDENOISE: vsName = "mt2d_vs"; fsName = "mtdenoise_fs"; break;
        case MT_PROG_MOVIEUPSCALE: vsName = "mt2d_vs"; fsName = "mtupscale_fs"; break;
        case MT_PROG_MOVIESHARP:   vsName = "mt2d_vs"; fsName = "mtsharp_fs"; break;
        case MT_PROG_SHADOWDEPTH:  vsName = "mt_sd_vs"; fsName = "mt_sd_fs"; break;
        default: break;
    }

    MTLPixelFormat colorFormat = MTLPixelFormatBGRA8Unorm;
    MTLPixelFormat depthFormat = MTLPixelFormatDepth32Float;
    bool hasDepth = true;
    switch (surface) {
        case MT_SURF_RGBA8:   colorFormat = MTLPixelFormatRGBA8Unorm;  depthFormat = MTLPixelFormatInvalid; hasDepth = false; break;
        case MT_SURF_RGBA16F: colorFormat = MTLPixelFormatRGBA16Float; depthFormat = MTLPixelFormatInvalid; hasDepth = false; break;
        case MT_SURF_SHADOW:  colorFormat = MTLPixelFormatR32Uint;     depthFormat = MTLPixelFormatDepth32Float; hasDepth = true; break;
        default: break;
    }

    id<MTLDevice> device = mtId(display->GetDevice());
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [mtId(library) newFunctionWithName:
        [NSString stringWithUTF8String:vsName]];
    desc.fragmentFunction = [mtId(library) newFunctionWithName:
        [NSString stringWithUTF8String:fsName]];
    desc.colorAttachments[0].pixelFormat = colorFormat;
    desc.depthAttachmentPixelFormat = depthFormat;

    MTLRenderPipelineColorAttachmentDescriptor* color = desc.colorAttachments[0];
    color.writeMask = MTLColorWriteMaskAll;
    switch (blend) {
        case PDDI_BLEND_NONE:
            color.blendingEnabled = NO;
            break;
        case PDDI_BLEND_ALPHA:
            color.blendingEnabled = YES;
            color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            color.rgbBlendOperation = MTLBlendOperationAdd;
            color.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            color.alphaBlendOperation = MTLBlendOperationAdd;
            break;
        case PDDI_BLEND_ADD:
            color.blendingEnabled = YES;
            color.sourceRGBBlendFactor = MTLBlendFactorOne;
            color.destinationRGBBlendFactor = MTLBlendFactorOne;
            color.rgbBlendOperation = MTLBlendOperationAdd;
            color.sourceAlphaBlendFactor = MTLBlendFactorOne;
            color.destinationAlphaBlendFactor = MTLBlendFactorOne;
            break;
        case PDDI_BLEND_SUBTRACT:
            color.blendingEnabled = YES;
            color.sourceRGBBlendFactor = MTLBlendFactorOne;
            color.destinationRGBBlendFactor = MTLBlendFactorOne;
            color.rgbBlendOperation = MTLBlendOperationReverseSubtract;
            color.sourceAlphaBlendFactor = MTLBlendFactorOne;
            color.destinationAlphaBlendFactor = MTLBlendFactorOne;
            break;
        case PDDI_BLEND_PSX_QUARTER:
            color.blendingEnabled = YES;
            color.sourceRGBBlendFactor = MTLBlendFactorBlendAlpha;
            color.destinationRGBBlendFactor = MTLBlendFactorOne;
            color.rgbBlendOperation = MTLBlendOperationAdd;
            color.sourceAlphaBlendFactor = MTLBlendFactorBlendAlpha;
            color.destinationAlphaBlendFactor = MTLBlendFactorOne;
            break;
    }
    (void)hasDepth;

    NSError* error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:desc
                                                                             error:&error];
    [desc release];
    if (!state) {
        std::fprintf(stderr, "mtContext: pipeline %s/%s failed: %s\n", vsName, fsName,
                     error ? [[error localizedDescription] UTF8String] : "(unknown)");
        return nullptr;
    }
    void* handle = mtNew(state);
    pipelines.emplace(key, handle);
    return handle;
}

void* mtContext::GetSampler(pddiFilterMode filter) {
    const u64 key = (filter == PDDI_FILTER_NONE) ? 0 : 1;
    auto it = samplers.find(key);
    if (it != samplers.end()) return it->second;
    MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor alloc] init];
    const MTLSamplerMinMagFilter f =
        (filter == PDDI_FILTER_NONE) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    desc.minFilter = f;
    desc.magFilter = f;
    desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> state = [mtId(display->GetDevice()) newSamplerStateWithDescriptor:desc];
    [desc release];
    void* handle = mtNew(state);
    samplers.emplace(key, handle);
    return handle;
}

void* mtContext::GetDepthStencilState(bool depthTest, bool depthWrite) {
    const u64 key = (depthTest ? 1u : 0u) | (depthWrite ? 2u : 0u);
    auto it = depthStates.find(key);
    if (it != depthStates.end()) return it->second;
    MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
    desc.depthCompareFunction = depthTest ? MTLCompareFunctionGreaterEqual
                                          : MTLCompareFunctionAlways;
    desc.depthWriteEnabled = depthWrite ? YES : NO;
    id<MTLDepthStencilState> state =
        [mtId(display->GetDevice()) newDepthStencilStateWithDescriptor:desc];
    [desc release];
    if (!state) return nullptr;
    void* handle = mtNew(state);
    depthStates.emplace(key, handle);
    return handle;
}

bool mtContext::GetComparisonSampler() {
    if (compareSampler) return true;
    MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor alloc] init];
    desc.minFilter = MTLSamplerMinMagFilterLinear;
    desc.magFilter = MTLSamplerMinMagFilterLinear;
    desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    // Reversed-Z shadow maps: lit when the fragment is closer (ref >= stored).
    desc.compareFunction = MTLCompareFunctionGreaterEqual;
    id<MTLSamplerState> state = [mtId(display->GetDevice()) newSamplerStateWithDescriptor:desc];
    [desc release];
    if (!state) return false;
    compareSampler = mtNew(state);
    return true;
}

void* mtContext::GetDummyTexture() {
    if (dummyTexture) return dummyTexture;
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1 height:1 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    dummyTexture = mtNew([mtId(display->GetDevice()) newTextureWithDescriptor:desc]);
    return dummyTexture;
}

void* mtContext::GetDummyVRAMTexture() {
    if (dummyVRAM) return dummyVRAM;
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Uint
                                                           width:1 height:1 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    dummyVRAM = mtNew([mtId(display->GetDevice()) newTextureWithDescriptor:desc]);
    return dummyVRAM;
}

void* mtContext::GetDummyDepthTexture() {
    if (dummyDepth) return dummyDepth;
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:1 height:1 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    dummyDepth = mtNew([mtId(display->GetDevice()) newTextureWithDescriptor:desc]);
    return dummyDepth;
}

void* mtContext::GetDummyIdTexture() {
    if (dummyId) return dummyId;
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                                           width:1 height:1 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    dummyId = mtNew([mtId(display->GetDevice()) newTextureWithDescriptor:desc]);
    return dummyId;
}

void* mtContext::GetWhiteTexture() {
    if (whiteTexture) return whiteTexture;
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1 height:1 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeManaged;
    id<MTLTexture> tex = [mtId(display->GetDevice()) newTextureWithDescriptor:desc];
    const u8 white[4] = { 255, 255, 255, 255 };
    [tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:white bytesPerRow:4];
    whiteTexture = mtNew(tex);
    return whiteTexture;
}

void* mtContext::ResolveShaderTexture(pddiBaseShader* shader) {
    if (auto* mtS = dynamic_cast<mtShader*>(shader)) {
        if (pddiTexture* tex = mtS->GetBoundTexture()) {
            if (auto* mtT = dynamic_cast<mtTexture*>(tex)) {
                if (mtT->GetMetalTexture()) {
                    return mtT->GetMetalTexture();
                }
            }
        }
    }
    return GetWhiteTexture();
}

void* mtContext::UploadDynamic(const void* data, u32 bytes) {
    if (!display || !data || bytes == 0) return nullptr;
    // Commands are encoded now but executed by the GPU later, so every upload
    // must land in fresh memory that is not overwritten by later draws in the
    // same frame. Append into a per-frame arena instead of reusing one buffer.
    const u32 align = 256;
    const u32 aligned = (bytes + (align - 1)) & ~(align - 1);
    if (dynamicBuffers.empty() || dynamicUsed + aligned > dynamicCapacity) {
        const u32 cap = std::max(aligned, 1u << 20);
        void* buf = mtNew([mtId(display->GetDevice())
            newBufferWithLength:cap options:MTLResourceStorageModeManaged]);
        if (!buf) return nullptr;
        dynamicBuffers.push_back(buf);
        dynamicCapacity = cap;
        dynamicUsed = 0;
    }
    void* buf = dynamicBuffers.back();
    id<MTLBuffer> buffer = mtId(buf);
    char* dst = (char*)[buffer contents] + dynamicUsed;
    std::memcpy(dst, data, bytes);
    [buffer didModifyRange:NSMakeRange(dynamicUsed, bytes)];
    dynamicOffset = dynamicUsed;
    dynamicUsed += aligned;
    return buf;
}

void mtContext::ApplyEncoderState(void* encoderPtr, pddiBlendMode blend, pddiCullMode cull,
                                  bool depthTest, bool depthWrite) {
    if (!encoderPtr) return;
    id<MTLRenderCommandEncoder> enc = mtId(encoderPtr);
    if (void* ds = GetDepthStencilState(depthTest, depthWrite)) {
        [enc setDepthStencilState:mtId(ds)];
    }
    [enc setFrontFacingWinding:MTLWindingCounterClockwise];
    switch (cull) {
        case PDDI_CULL_NONE:     [enc setCullMode:MTLCullModeNone]; break;
        case PDDI_CULL_NORMAL:   [enc setCullMode:MTLCullModeBack]; break;
        case PDDI_CULL_INVERTED: [enc setCullMode:MTLCullModeFront]; break;
    }
    [enc setDepthClipMode:(depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip)];
    [enc setBlendColorRed:0.0 green:0.0 blue:0.0 alpha:0.25];
    if (polyOffsetActive) {
        [enc setDepthBias:polyOffsetUnits slopeScale:polyOffsetFactor clamp:0.0];
    } else if (blend != PDDI_BLEND_NONE) {
        [enc setDepthBias:1.0 slopeScale:1.0 clamp:0.0];
    } else {
        [enc setDepthBias:0.0 slopeScale:0.0 clamp:0.0];
    }
}

static MTLPrimitiveType ToMetalPrimitive(pddiPrimType type) {
    switch (type) {
        case PDDI_PRIM_TRIANGLES: return MTLPrimitiveTypeTriangle;
        case PDDI_PRIM_TRISTRIP:  return MTLPrimitiveTypeTriangleStrip;
        case PDDI_PRIM_LINES:     return MTLPrimitiveTypeLine;
        case PDDI_PRIM_LINESTRIP: return MTLPrimitiveTypeLineStrip;
        case PDDI_PRIM_POINTS:    return MTLPrimitiveTypePoint;
    }
    return MTLPrimitiveTypeTriangle;
}

void mtContext::BeginFrame() {
    for (void* buf : dynamicBuffers) mtFree(buf);
    dynamicBuffers.clear();
    dynamicUsed = 0;
    dynamicCapacity = 0;
    dynamicOffset = 0;
    if (display) display->BeginFrame();
}
void mtContext::EndFrame() { if (display) display->EndFrame(); }

void mtContext::SetClearColour(pddiColour c) {
    clearColour = c;
    if (display) display->SetClearColour(c);
}

void mtContext::Clear(int flags) { (void)flags; }

void mtContext::SetPolygonOffset(bool enable, f32 factor, f32 units) {
    polyOffsetActive = enable;
    polyOffsetFactor = factor;
    polyOffsetUnits = units;
}

void mtContext::SetScissor(int x, int y, int w, int h) {
    scissor[0] = x;
    scissor[1] = y;
    scissor[2] = w;
    scissor[3] = h;
    if (display && display->IsFrameActive()) {
        id<MTLRenderCommandEncoder> enc = mtId(display->GetEncoder());
        MTLScissorRect rect = { (NSUInteger)std::max(x, 0), (NSUInteger)std::max(y, 0),
                                (NSUInteger)std::max(w, 0), (NSUInteger)std::max(h, 0) };
        [enc setScissorRect:rect];
    }
}

pddiRenderTarget* mtContext::CreateRenderTarget(int width, int height,
                                                pddiRenderTargetFormat format,
                                                bool withInstanceId) {
    return new mtRenderTarget(width, height, format, withInstanceId);
}

bool mtContext::SetRenderTarget(pddiRenderTarget* target) {
    if (!display || !display->IsFrameActive()) {
        return false;
    }
    if (!target) {
        if (activeRenderTarget) {
            display->EndRenderTargetPass();
            activeRenderTarget = nullptr;
        }
        return true;
    }
    auto* mtTarget = dynamic_cast<mtRenderTarget*>(target);
    if (!mtTarget || !mtTarget->IsValid()) {
        return false;
    }
    if (activeRenderTarget) {
        display->EndRenderTargetPass();
        activeRenderTarget = nullptr;
    }
    if (!display->BeginRenderTargetPass(mtTarget)) {
        return false;
    }
    activeRenderTarget = mtTarget;
    return true;
}

void mtContext::DrawPrimBuffer(pddiPrimBuffer* buffer, u32 indexOffset, u32 indexCount) {
    if (!buffer || !display || !display->IsFrameActive()) return;
    auto* mtBuf = static_cast<mtPrimBuffer*>(buffer);
    void* vertexBuffer = mtBuf->GetVertexBuffer();
    void* indexBuffer = mtBuf->GetIndexBuffer();
    if (!vertexBuffer) return;

    const int surface = ActiveSurface();
    const bool shadowPass = shadowCasterPass;
    const bool depthTest = shadowPass ? true : zBufferEnabled;
    const bool depthWrite = shadowPass ? true : (zBufferEnabled && (blendMode == PDDI_BLEND_NONE));
    const pddiCullMode cull = shadowPass ? PDDI_CULL_NONE : cullMode;
    const pddiBlendMode blend = shadowPass ? PDDI_BLEND_NONE : blendMode;
    const int program = shadowPass ? MT_PROG_SHADOWDEPTH : MT_PROG_3D;

    void* pipeline = GetPipelineState(program, blend, cull, depthTest, depthWrite, surface);
    if (!pipeline) return;

    id<MTLRenderCommandEncoder> enc = mtId(display->GetEncoder());
    [enc setRenderPipelineState:mtId(pipeline)];
    ApplyEncoderState(display->GetEncoder(), blend, cull, depthTest, depthWrite);
    [enc setVertexBuffer:mtId(vertexBuffer) offset:0 atIndex:0];

    Mat4 mvp;
    if (shadowPass) {
        mvp = shadowCasterLightVP * worldMatrix;
    } else {
        mvp = projection * (viewMatrix * worldMatrix);
    }
    MtVertexUniforms vu{};
    std::memcpy(vu.mvp.m, mvp.Data(), sizeof(float) * 16);
    std::memcpy(vu.world.m, worldMatrix.Data(), sizeof(float) * 16);
    std::memcpy(vu.view.m, viewMatrix.Data(), sizeof(float) * 16);
    vu.cameraPos[0] = cameraWorldPos[0];
    vu.cameraPos[1] = cameraWorldPos[1];
    vu.cameraPos[2] = cameraWorldPos[2];
    vu.stride = mtBuf->GetStride();
    vu.posOffset = mtBuf->GetPosOffset();
    vu.colOffset = mtBuf->GetColOffset();
    vu.uvOffset = mtBuf->GetUVOffset();
    vu.texInfoOffset = mtBuf->GetTexInfoOffset();
    vu.hasColor = (mtBuf->GetVertexFormat() & PDDI_V_COLOUR) ? 1u : 0u;
    vu.hasUV = (mtBuf->GetVertexFormat() & PDDI_V_UV) ? 1u : 0u;
    vu.hasTexInfo = (mtBuf->GetVertexFormat() & PDDI_V_TEXINFO) ? 1u : 0u;
    [enc setVertexBytes:&vu length:sizeof(vu) atIndex:1];

    MtFragmentUniforms fu{};
    fu.alphaScale = (blendMode == PDDI_BLEND_ALPHA) ? 0.5f : 1.0f;
    fu.useZeroTexelKey = (blendMode != PDDI_BLEND_NONE) ? 1u : 0u;
    const bool usingRealTexture = realTextureMode && currentTexture &&
        static_cast<mtTexture*>(currentTexture)->GetMetalTexture();
    fu.realTextureMode = usingRealTexture ? 1u : 0u;
    fu.texInfoOverrideEnabled = texInfoOverrideEnabled ? 1u : 0u;
    if (texInfoOverrideEnabled) {
        fu.texInfoOverride[0] = float((texInfoOverrideWord >> 16) & 0xFFFFu);
        fu.texInfoOverride[1] = float(texInfoOverrideWord & 0xFFFFu);
    } else {
        fu.texInfoOverride[0] = -1.0f;
        fu.texInfoOverride[1] = 0.0f;
    }
    fu.realTexOffset[0] = realTexOffsetX;
    fu.realTexOffset[1] = realTexOffsetY;
    fu.realTexSize[0] = realTexSizeX;
    fu.realTexSize[1] = realTexSizeY;

    void* vram = ResolveVRAMTexture(vramHandle);
    fu.hasVRAM = vram ? 1u : 0u;
    [enc setFragmentTexture:(vram ? mtId(vram) : mtId(GetDummyVRAMTexture())) atIndex:0];
    [enc setFragmentTexture:(usingRealTexture
                              ? mtId(static_cast<mtTexture*>(currentTexture)->GetMetalTexture())
                              : mtId(GetDummyTexture())) atIndex:1];
    pddiFilterMode filter = usingRealTexture
        ? static_cast<mtTexture*>(currentTexture)->GetFilterMode() : PDDI_FILTER_NONE;
    [enc setFragmentSamplerState:mtId(GetSampler(filter)) atIndex:0];
    [enc setFragmentBytes:&fu length:sizeof(fu) atIndex:0];

    MtShadowUniforms su{};
    const bool shadowsActive = !shadowPass && receiveShadows && shadowCascadeCount > 0 &&
                               GetComparisonSampler();
    su.receiveShadows = shadowsActive ? 1u : 0u;
    su.shadowCascadeCount = shadowsActive ? (u32)shadowCascadeCount : 0u;
    su.shadowFilterQuality = (u32)shadowFilterQuality;
    su.receiverInstanceId = shadowReceiverInstanceId;
    su.shadowDebugMode = (u32)shadowDebugMode;
    if (shadowPass) {
        su.receiverInstanceId = shadowCasterInstanceId;
    }
    for (int i = 0; i < kShadowCascadeCount; i++) {
        std::memcpy(su.lightVP[i].m, shadowLightVP[i].Data(), sizeof(float) * 16);
        su.cascadeSplits[i] = shadowCascadeSplits[i];
        su.cascadeBlendDistances[i] = shadowCascadeBlendDistances[i];
        su.shadowTexelWorldSize[i] = shadowTexelWorldSize[i];
        su.shadowBias[i] = shadowBias[i];
    }
    su.shadowLightDir[0] = shadowLightDir[0];
    su.shadowLightDir[1] = shadowLightDir[1];
    su.shadowLightDir[2] = shadowLightDir[2];
    [enc setFragmentBytes:&su length:sizeof(su) atIndex:2];

    [enc setFragmentTexture:mtId(GetDummyDepthTexture()) atIndex:2];
    [enc setFragmentTexture:mtId(GetDummyDepthTexture()) atIndex:3];
    [enc setFragmentTexture:mtId(GetDummyDepthTexture()) atIndex:4];
    [enc setFragmentTexture:mtId(GetDummyIdTexture()) atIndex:5];
    [enc setFragmentTexture:mtId(GetDummyIdTexture()) atIndex:6];
    [enc setFragmentTexture:mtId(GetDummyIdTexture()) atIndex:7];
    [enc setFragmentSamplerState:mtId(compareSampler) atIndex:1];
    if (shadowsActive) {
        for (int i = 0; i < shadowCascadeCount && i < kShadowCascadeCount; i++) {
            if (auto* t = dynamic_cast<mtTexture*>(shadowDepthTextures[i])) {
                if (t->GetMetalTexture()) {
                    [enc setFragmentTexture:mtId(t->GetMetalTexture()) atIndex:(NSUInteger)(2 + i)];
                }
            }
            if (auto* t = dynamic_cast<mtTexture*>(shadowIdTextures[i])) {
                if (t->GetMetalTexture()) {
                    [enc setFragmentTexture:mtId(t->GetMetalTexture()) atIndex:(NSUInteger)(5 + i)];
                }
            }
        }
    }

    const u32 count = (indexCount != 0) ? indexCount : mtBuf->GetIndexCount();
    const MTLPrimitiveType prim = ToMetalPrimitive(mtBuf->GetPrimType());
    if (indexBuffer && count > 0) {
        [enc drawIndexedPrimitives:prim
                        indexCount:count
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:mtId(indexBuffer)
                 indexBufferOffset:(NSUInteger)indexOffset * sizeof(u16)];
    } else if (mtBuf->GetVertexCount() > 0) {
        [enc drawPrimitives:prim vertexStart:0 vertexCount:mtBuf->GetVertexCount()];
    }
}

static int VariantProgramForType(const char* type) {
    if (!type) return -1;
    const std::string t(type);
    if (t == "tilt") return 4;
    if (t == "glow") return 5;
    if (t == "godrays") return 6;
    if (t == "dot") return 7;
    if (t == "moviedenoise") return 8;
    if (t == "movieupscale") return 9;
    if (t == "moviesharp") return 10;
    return -1;  // simple / unknown
}

static bool IsRenderTargetTexture(pddiTexture* tex) {
    if (auto* t = dynamic_cast<mtTexture*>(tex)) {
        return t->IsRenderTarget();
    }
    return false;
}

void mtContext::BindVariantUniforms(void* encoderPtr, pddiBaseShader* shader) {
    if (!encoderPtr || !shader) return;
    auto* mtS = dynamic_cast<mtShader*>(shader);
    if (!mtS) return;
    id<MTLRenderCommandEncoder> enc = mtId(encoderPtr);

    auto vec = [&](const char* name) -> std::array<float, 4> {
        auto it = mtS->GetVectors().find(name);
        return it == mtS->GetVectors().end() ? std::array<float, 4>{0, 0, 0, 0} : it->second;
    };
    auto flt = [&](const char* name) -> float {
        auto it = mtS->GetFloats().find(name);
        return it == mtS->GetFloats().end() ? 0.0f : it->second;
    };

    MtVariantUniforms vu{};
    const std::string& t = mtS->GetType();
    if (t == "glow") {
        auto p = vec("uGlowParams");
        auto m = vec("uGlowMotion");
        std::copy(p.begin(), p.end(), vu.a);
        std::copy(m.begin(), m.end(), vu.b);
        vu.f0 = flt("uTime");
    } else if (t == "godrays") {
        auto p = vec("uRayParams");
        auto m = vec("uRayMotion");
        std::copy(p.begin(), p.end(), vu.a);
        std::copy(m.begin(), m.end(), vu.b);
        vu.f0 = flt("uExposure");
        vu.f1 = flt("uTime");
    } else if (t == "dot") {
        pddiColour c = mtS->GetDiffuse();
        vu.tint[0] = c.r / 255.0f; vu.tint[1] = c.g / 255.0f;
        vu.tint[2] = c.b / 255.0f; vu.tint[3] = c.a / 255.0f;
        vu.f0 = flt("uShapeSeed");
    } else if (t == "moviedenoise" || t == "movieupscale" || t == "moviesharp") {
        auto texel = vec("uTexel");
        std::copy(texel.begin(), texel.end(), vu.a);
        vu.f0 = flt("uSharpAmount");
    }
    vu.flipV = IsRenderTargetTexture(mtS->GetBoundTexture()) ? 1.0f : 0.0f;
    [enc setFragmentBytes:&vu length:sizeof(vu) atIndex:1];
    (void)VariantProgramForType;
}

void mtContext::DrawQuad(pddiBaseShader* shader, float x, float y, float w, float h,
                         float u0, float v0, float u1, float v1) {
    if (!display || !display->IsFrameActive()) return;
    const int program = [&]() {
        int p = VariantProgramForType(shader ? static_cast<mtShader*>(
            dynamic_cast<mtShader*>(shader))->GetType() : nullptr);
        return p >= 0 ? p : MT_PROG_QUAD2D;
    }();
    // Recompute program defensively (avoids dereferencing a non-mtShader).
    int prog = MT_PROG_QUAD2D;
    if (auto* mtS = dynamic_cast<mtShader*>(shader)) {
        int mapped = VariantProgramForType(mtS->GetType());
        if (mapped >= 0) prog = mapped;
    }
    (void)program;

    const int surface = ActiveSurface();
    const bool depthTest = surface != MT_SURF_RGBA8 && surface != MT_SURF_RGBA16F && zBufferEnabled;
    const bool depthWrite = depthTest && (blendMode == PDDI_BLEND_NONE);
    void* pipeline = GetPipelineState(prog, blendMode, PDDI_CULL_NONE, depthTest, depthWrite, surface);
    if (!pipeline) return;

    const float yTop = y, yBottom = y + h;
    const float verts[6 * 4] = {
        x,     yBottom, u0, v1,
        x + w, yBottom, u1, v1,
        x + w, yTop,    u1, v0,
        x,     yBottom, u0, v1,
        x + w, yTop,    u1, v0,
        x,     yTop,    u0, v0,
    };
    void* vb = UploadDynamic(verts, sizeof(verts));
    if (!vb) return;

    id<MTLRenderCommandEncoder> enc = mtId(display->GetEncoder());
    [enc setRenderPipelineState:mtId(pipeline)];
    ApplyEncoderState(display->GetEncoder(), blendMode, PDDI_CULL_NONE, depthTest, depthWrite);

    pddiColour c(255, 255, 255, 255);
    float flipV = 0.0f;
    if (auto* mtS = dynamic_cast<mtShader*>(shader)) {
        c = mtS->GetDiffuse();
        flipV = IsRenderTargetTexture(mtS->GetBoundTexture()) ? 1.0f : 0.0f;
    }

    if (prog == MT_PROG_TILT) {
        MtTiltUniforms tu{};
        std::memcpy(tu.proj.m, projection.Data(), sizeof(float) * 16);
        if (auto* mtS = dynamic_cast<mtShader*>(shader)) {
            auto rectIt = mtS->GetVectors().find("uTiltRect");
            auto angIt = mtS->GetVectors().find("uTiltAngles");
            if (rectIt != mtS->GetVectors().end()) std::copy(rectIt->second.begin(), rectIt->second.end(), tu.rect);
            if (angIt != mtS->GetVectors().end()) std::copy(angIt->second.begin(), angIt->second.end(), tu.angles);
        }
        [enc setVertexBuffer:mtId(vb) offset:(NSUInteger)dynamicOffset atIndex:0];
        [enc setVertexBytes:&tu length:sizeof(tu) atIndex:1];
        Mt2DUniforms u{};
        std::memcpy(u.proj.m, projection.Data(), sizeof(float) * 16);
        u.tint[0] = c.r / 255.0f; u.tint[1] = c.g / 255.0f;
        u.tint[2] = c.b / 255.0f; u.tint[3] = c.a / 255.0f;
    u.flipV = flipV;
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
    } else if (prog == MT_PROG_QUAD2D) {
        Mt2DUniforms u{};
        std::memcpy(u.proj.m, projection.Data(), sizeof(float) * 16);
        u.tint[0] = c.r / 255.0f; u.tint[1] = c.g / 255.0f;
        u.tint[2] = c.b / 255.0f; u.tint[3] = c.a / 255.0f;
    u.flipV = flipV;
        [enc setVertexBuffer:mtId(vb) offset:(NSUInteger)dynamicOffset atIndex:0];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
    } else {
        [enc setVertexBuffer:mtId(vb) offset:(NSUInteger)dynamicOffset atIndex:0];
        Mat4Pod proj{};
        std::memcpy(proj.m, projection.Data(), sizeof(float) * 16);
        [enc setVertexBytes:&proj length:sizeof(proj) atIndex:1];
        BindVariantUniforms(display->GetEncoder(), shader);
    }

    [enc setFragmentTexture:mtId(ResolveShaderTexture(shader)) atIndex:0];
    [enc setFragmentSamplerState:mtId(GetSampler(PDDI_FILTER_BILINEAR)) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

void mtContext::DrawFilledCircle(pddiBaseShader* shader, float centerX, float centerY,
                                 float radiusX, float radiusY, float u0, float v0,
                                 float u1, float v1, int segments) {
    if (!display || !display->IsFrameActive() || !shader) return;
    if (segments < 3) segments = 3;
    if (segments > 64) segments = 64;

    const float PI2 = 6.28318530718f;
    const float uvCenterX = (u0 + u1) * 0.5f;
    const float uvCenterY = (v0 + v1) * 0.5f;
    const float uvRadiusX = (u1 - u0) * 0.5f;
    const float uvRadiusY = (v1 - v0) * 0.5f;

    std::vector<float> verts;
    verts.reserve((size_t)segments * 3 * 4);
    for (int i = 0; i < segments; ++i) {
        const float a0 = ((float)i / (float)segments) * PI2;
        const float a1 = ((float)(i + 1) / (float)segments) * PI2;
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        verts.insert(verts.end(), { centerX, centerY, uvCenterX, uvCenterY });
        verts.insert(verts.end(), { centerX + c0 * radiusX, centerY + s0 * radiusY,
                                    uvCenterX + c0 * uvRadiusX, uvCenterY + s0 * uvRadiusY });
        verts.insert(verts.end(), { centerX + c1 * radiusX, centerY + s1 * radiusY,
                                    uvCenterX + c1 * uvRadiusX, uvCenterY + s1 * uvRadiusY });
    }

    const int surface = ActiveSurface();
    const bool depthTest = surface != MT_SURF_RGBA8 && surface != MT_SURF_RGBA16F && zBufferEnabled;
    const bool depthWrite = depthTest && (blendMode == PDDI_BLEND_NONE);
    void* pipeline = GetPipelineState(MT_PROG_QUAD2D, blendMode, PDDI_CULL_NONE,
                                      depthTest, depthWrite, surface);
    void* vb = UploadDynamic(verts.data(), (u32)(verts.size() * sizeof(float)));
    if (!pipeline || !vb) return;

    id<MTLRenderCommandEncoder> enc = mtId(display->GetEncoder());
    [enc setRenderPipelineState:mtId(pipeline)];
    ApplyEncoderState(display->GetEncoder(), blendMode, PDDI_CULL_NONE, depthTest, depthWrite);

    Mt2DUniforms u{};
    std::memcpy(u.proj.m, projection.Data(), sizeof(float) * 16);
    pddiColour c(255, 255, 255, 255);
    float flipV = 0.0f;
    if (auto* mtS = dynamic_cast<mtShader*>(shader)) {
        c = mtS->GetDiffuse();
        flipV = IsRenderTargetTexture(mtS->GetBoundTexture()) ? 1.0f : 0.0f;
    }
    u.tint[0] = c.r / 255.0f; u.tint[1] = c.g / 255.0f;
    u.tint[2] = c.b / 255.0f; u.tint[3] = c.a / 255.0f;
    u.flipV = flipV;

    [enc setVertexBuffer:mtId(vb) offset:(NSUInteger)dynamicOffset atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentTexture:mtId(ResolveShaderTexture(shader)) atIndex:0];
    [enc setFragmentSamplerState:mtId(GetSampler(PDDI_FILTER_BILINEAR)) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)(segments * 3)];
}

void mtContext::DrawCircle(pddiBaseShader* shader, float centerX, float centerY,
                           float radiusX, float radiusY, float thickness, float u0,
                           float v0, float u1, float v1, int segments) {
    if (!display || !display->IsFrameActive() || !shader || thickness <= 0.0f) return;
    if (segments < 3) segments = 3;
    if (segments > 64) segments = 64;

    const float PI2 = 6.28318530718f;
    const float innerRadiusX = std::max(0.0f, radiusX - thickness);
    const float innerRadiusY = std::max(0.0f, radiusY - thickness);
    const float uvCenterX = (u0 + u1) * 0.5f;
    const float uvCenterY = (v0 + v1) * 0.5f;
    const float uvRadiusX = (u1 - u0) * 0.5f;
    const float uvRadiusY = (v1 - v0) * 0.5f;

    std::vector<float> verts;
    verts.reserve((size_t)segments * 6 * 4);
    for (int i = 0; i < segments; ++i) {
        const float a0 = ((float)i / (float)segments) * PI2;
        const float a1 = ((float)(i + 1) / (float)segments) * PI2;
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        const float ox0 = centerX + c0 * radiusX,      oy0 = centerY + s0 * radiusY;
        const float ox1 = centerX + c1 * radiusX,      oy1 = centerY + s1 * radiusY;
        const float ix0 = centerX + c0 * innerRadiusX, iy0 = centerY + s0 * innerRadiusY;
        const float ix1 = centerX + c1 * innerRadiusX, iy1 = centerY + s1 * innerRadiusY;
        const float ou0 = uvCenterX + c0 * uvRadiusX,  ov0 = uvCenterY + s0 * uvRadiusY;
        const float ou1 = uvCenterX + c1 * uvRadiusX,  ov1 = uvCenterY + s1 * uvRadiusY;
        const float iu0 = uvCenterX + c0 * uvRadiusX,  iv0 = uvCenterY + s0 * uvRadiusY;
        const float iu1 = uvCenterX + c1 * uvRadiusX,  iv1 = uvCenterY + s1 * uvRadiusY;
        verts.insert(verts.end(), { ox0, oy0, ou0, ov0 });
        verts.insert(verts.end(), { ix0, iy0, iu0, iv0 });
        verts.insert(verts.end(), { ox1, oy1, ou1, ov1 });
        verts.insert(verts.end(), { ox1, oy1, ou1, ov1 });
        verts.insert(verts.end(), { ix0, iy0, iu0, iv0 });
        verts.insert(verts.end(), { ix1, iy1, iu1, iv1 });
    }

    const int surface = ActiveSurface();
    const bool depthTest = surface != MT_SURF_RGBA8 && surface != MT_SURF_RGBA16F && zBufferEnabled;
    const bool depthWrite = depthTest && (blendMode == PDDI_BLEND_NONE);
    void* pipeline = GetPipelineState(MT_PROG_QUAD2D, blendMode, PDDI_CULL_NONE,
                                      depthTest, depthWrite, surface);
    void* vb = UploadDynamic(verts.data(), (u32)(verts.size() * sizeof(float)));
    if (!pipeline || !vb) return;

    id<MTLRenderCommandEncoder> enc = mtId(display->GetEncoder());
    [enc setRenderPipelineState:mtId(pipeline)];
    ApplyEncoderState(display->GetEncoder(), blendMode, PDDI_CULL_NONE, depthTest, depthWrite);

    Mt2DUniforms u{};
    std::memcpy(u.proj.m, projection.Data(), sizeof(float) * 16);
    pddiColour c(255, 255, 255, 255);
    float flipV = 0.0f;
    if (auto* mtS = dynamic_cast<mtShader*>(shader)) {
        c = mtS->GetDiffuse();
        flipV = IsRenderTargetTexture(mtS->GetBoundTexture()) ? 1.0f : 0.0f;
    }
    u.tint[0] = c.r / 255.0f; u.tint[1] = c.g / 255.0f;
    u.tint[2] = c.b / 255.0f; u.tint[3] = c.a / 255.0f;
    u.flipV = flipV;

    [enc setVertexBuffer:mtId(vb) offset:(NSUInteger)dynamicOffset atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentTexture:mtId(ResolveShaderTexture(shader)) atIndex:0];
    [enc setFragmentSamplerState:mtId(GetSampler(PDDI_FILTER_BILINEAR)) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)(segments * 6)];
}

void mtContext::DrawQuadBatch(pddiTexture* tex, pddiBlendMode blend,
                              const pddiBatchVertex* verts, s32 vertCount) {
    if (!display || !display->IsFrameActive() || !verts || vertCount <= 0) return;
    const int surface = ActiveSurface();
    const bool depthTest = surface != MT_SURF_RGBA8 && surface != MT_SURF_RGBA16F && zBufferEnabled;
    const bool depthWrite = depthTest && (blend == PDDI_BLEND_NONE);
    void* pipeline = GetPipelineState(MT_PROG_BATCH, blend, PDDI_CULL_NONE,
                                      depthTest, depthWrite, surface);
    void* vb = UploadDynamic(verts, sizeof(pddiBatchVertex) * (u32)vertCount);
    if (!pipeline || !vb) return;

    id<MTLRenderCommandEncoder> enc = mtId(display->GetEncoder());
    [enc setRenderPipelineState:mtId(pipeline)];
    ApplyEncoderState(display->GetEncoder(), blend, PDDI_CULL_NONE, depthTest, depthWrite);

    Mat4Pod proj{};
    std::memcpy(proj.m, projection.Data(), sizeof(float) * 16);
    void* texture = GetWhiteTexture();
    pddiFilterMode filter = PDDI_FILTER_BILINEAR;
    if (auto* mtT = dynamic_cast<mtTexture*>(tex)) {
        if (mtT->GetMetalTexture()) {
            texture = mtT->GetMetalTexture();
            filter = mtT->GetFilterMode();
        }
    }

    [enc setVertexBuffer:mtId(vb) offset:(NSUInteger)dynamicOffset atIndex:0];
    [enc setVertexBytes:&proj length:sizeof(proj) atIndex:1];
    [enc setFragmentTexture:mtId(texture) atIndex:0];
    [enc setFragmentSamplerState:mtId(GetSampler(filter)) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)vertCount];
}

void mtContext::DrawGouraudQuad(float x0, float y0, float r0, float g0, float b0, float a0,
                                float x1, float y1, float r1, float g1, float b1, float a1,
                                float x2, float y2, float r2, float g2, float b2, float a2,
                                float x3, float y3, float r3, float g3, float b3, float a3) {
    if (!display || !display->IsFrameActive()) return;
    const int surface = ActiveSurface();
    const bool depthTest = surface != MT_SURF_RGBA8 && surface != MT_SURF_RGBA16F && zBufferEnabled;
    const bool depthWrite = depthTest && (blendMode == PDDI_BLEND_NONE);
    void* pipeline = GetPipelineState(MT_PROG_GOURAUD, blendMode, PDDI_CULL_NONE,
                                      depthTest, depthWrite, surface);
    if (!pipeline) return;

    const float verts[36] = {
        x0, y0, r0, g0, b0, a0,
        x1, y1, r1, g1, b1, a1,
        x2, y2, r2, g2, b2, a2,
        x1, y1, r1, g1, b1, a1,
        x3, y3, r3, g3, b3, a3,
        x2, y2, r2, g2, b2, a2,
    };
    void* vb = UploadDynamic(verts, sizeof(verts));
    if (!vb) return;

    id<MTLRenderCommandEncoder> enc = mtId(display->GetEncoder());
    [enc setRenderPipelineState:mtId(pipeline)];
    ApplyEncoderState(display->GetEncoder(), blendMode, PDDI_CULL_NONE, depthTest, depthWrite);

    Mat4Pod proj{};
    std::memcpy(proj.m, projection.Data(), sizeof(float) * 16);
    [enc setVertexBuffer:mtId(vb) offset:(NSUInteger)dynamicOffset atIndex:0];
    [enc setVertexBytes:&proj length:sizeof(proj) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

void mtContext::SetTexInfoOverride(bool enabled, u32 texInfoWord) {
    texInfoOverrideEnabled = enabled;
    texInfoOverrideWord = texInfoWord;
}

u32 mtContext::CreateVRAMTexture(int w, int h, const u16* data) {
    if (!display || w <= 0 || h <= 0) return 0;
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Uint
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeManaged;
    id<MTLTexture> tex = [mtId(display->GetDevice()) newTextureWithDescriptor:desc];
    if (data) {
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
               mipmapLevel:0
                 withBytes:data
               bytesPerRow:(NSUInteger)(w * sizeof(u16))];
    }
    const u32 handle = nextVramHandle++;
    vramTextures.emplace(handle, mtNew(tex));
    return handle;
}

void mtContext::DestroyVRAMTexture(u32 handle) {
    auto it = vramTextures.find(handle);
    if (it == vramTextures.end()) return;
    mtFree(it->second);
    vramTextures.erase(it);
}

void mtContext::UpdateVRAMTexture(u32 handle, int w, int h, const u16* data) {
    if (!data) return;
    void* tex = ResolveVRAMTexture(handle);
    if (!tex) return;
    [mtId(tex) replaceRegion:MTLRegionMake2D(0, 0, w, h)
                mipmapLevel:0
                  withBytes:data
                bytesPerRow:(NSUInteger)(w * sizeof(u16))];
}

void mtContext::UpdateVRAMTextureRect(u32 handle, int x, int y, int w, int h,
                                      int srcPitchTexels, const u16* data) {
    if (!data || w <= 0 || h <= 0 || srcPitchTexels < w) return;
    void* tex = ResolveVRAMTexture(handle);
    if (!tex) return;
    id<MTLTexture> mtl = mtId(tex);
    const int clampedX = std::max(0, x);
    const int clampedY = std::max(0, y);
    const int clampedW = std::min<int>(x + w, (int)mtl.width) - clampedX;
    const int clampedH = std::min<int>(y + h, (int)mtl.height) - clampedY;
    if (clampedW <= 0 || clampedH <= 0) return;
    // replaceRegion takes tightly packed rows, so upload one row at a time
    // instead of staging a copy of the pitched source.
    for (int row = 0; row < clampedH; ++row) {
        [mtl replaceRegion:MTLRegionMake2D(clampedX, clampedY + row, clampedW, 1)
               mipmapLevel:0
                 withBytes:data + (size_t)(row + (clampedY - y)) * srcPitchTexels
                           + (clampedX - x)
               bytesPerRow:(NSUInteger)(clampedW * sizeof(u16))];
    }
}

void* mtContext::ResolveVRAMTexture(u32 handle) {
    auto it = vramTextures.find(handle);
    return it == vramTextures.end() ? nullptr : it->second;
}

void mtContext::SetRealTextureRect(float offsetX, float offsetY, float sizeX, float sizeY) {
    realTexOffsetX = offsetX;
    realTexOffsetY = offsetY;
    realTexSizeX = (sizeX != 0.0f) ? sizeX : 1.0f;
    realTexSizeY = (sizeY != 0.0f) ? sizeY : 1.0f;
}

void mtContext::SetShadowCasterPass(bool enable, const Mat4& lightVP) {
    shadowCasterPass = enable;
    shadowCasterLightVP = lightVP;
}

void mtContext::SetShadowCascades(pddiTexture* const* depthTextures, const Mat4* lightVPs,
                                  const float* splits, const float* texelWorldSizes,
                                  pddiTexture* const* idTextures, int count) {
    shadowCascadeCount = (count < 0) ? 0 : (count > kShadowCascadeCount ? kShadowCascadeCount : count);
    shadowFilterQuality = 0;
    for (s32 i = 0; i < shadowCascadeCount; i++) {
        shadowDepthTextures[i] = depthTextures ? depthTextures[i] : nullptr;
        shadowIdTextures[i] = idTextures ? idTextures[i] : nullptr;
        shadowLightVP[i] = lightVPs[i];
        shadowCascadeSplits[i] = splits[i];
        shadowTexelWorldSize[i] = texelWorldSizes ? texelWorldSizes[i] : 0.0f;
        const float previousSplit = (i > 0) ? shadowCascadeSplits[i - 1] : 0.0f;
        const float cascadeRange = std::max(shadowCascadeSplits[i] - previousSplit, 1.0f);
        shadowCascadeBlendDistances[i] =
            std::min(std::max(cascadeRange * 0.12f, 384.0f), 1536.0f);
        if (depthTextures[i] && depthTextures[i]->GetWidth() >= 8192) {
            shadowFilterQuality = 3;
        } else if (depthTextures[i] && depthTextures[i]->GetWidth() >= 4096) {
            shadowFilterQuality = 2;
        } else if (depthTextures[i] && depthTextures[i]->GetWidth() >= 2048) {
            shadowFilterQuality = 1;
        }
    }
    for (s32 i = shadowCascadeCount; i < kShadowCascadeCount; i++) {
        shadowDepthTextures[i] = nullptr;
        shadowIdTextures[i] = nullptr;
        shadowCascadeSplits[i] = 0.0f;
        shadowCascadeBlendDistances[i] = 0.0f;
        shadowTexelWorldSize[i] = 0.0f;
    }
}

void mtContext::SetCameraWorldPos(float x, float y, float z) {
    cameraWorldPos[0] = x;
    cameraWorldPos[1] = y;
    cameraWorldPos[2] = z;
}

void mtContext::SetShadowLightDirection(float x, float y, float z) {
    shadowLightDir[0] = x;
    shadowLightDir[1] = y;
    shadowLightDir[2] = z;
}

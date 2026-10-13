#pragma once

// Shader source seam.
//
// pddi owns no GLSL. The application supplies every shader body through the
// callback below, which the backends call while building their programs. This
// keeps game-specific shading (PSX VRAM/CLUT decode, cascade shadows) out of
// the portable renderer, and mirrors the p3d::rawTextureUploader inversion
// already used for VRAM uploads.
//
// Contract for supplied sources:
//  - No "#version" line and no "precision" statements: each backend prepends
//    its own preamble, because only the backend knows its GL flavour.
//  - The preamble defines these, which shader bodies may test:
//      NOPERSP             "noperspective" where the flavour supports it,
//                          empty otherwise (GLES needs an extension for it)
//      HIGHP               "highp" on GLES, empty on desktop GL
//      HAS_SHADOW_CASCADES defined on desktop GL only
//      HAS_REAL_TEXTURE    defined on desktop GL only
//      HAS_INT_ID_TARGET   defined on desktop GL only
//      VRAM_FLOAT          defined on GLES only: the VRAM texture is RG8 and
//                          must be decoded with float maths, not an integer
//                          sampler. The backend uploads to match.
//  - Vertex attribute locations are fixed: 0 = position, 1 = colour,
//    2 = uv, 3 = the extra vec2 (PSX tpage/cba pair).
//  - Returning nullptr is a fatal configuration error, not a fallback: the
//    backend logs and fails to build that program rather than rendering black.

enum pddiShaderStage {
    PDDI_SHADER_STAGE_VERTEX,
    PDDI_SHADER_STAGE_FRAGMENT
};

// programName is the pddiBaseShader type string ("simple", "glow", "tilt", ...)
// or one of the backend-internal programs ("mesh", "shadow_depth").
using pddiShaderSourceCallback = const char* (*)(const char* programName, pddiShaderStage stage);

inline pddiShaderSourceCallback pddiShaderSource = nullptr;

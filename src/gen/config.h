// config.h - PC recompilation feature flags
// Toggle non-OG / custom PC features here.
// The goal is to keep reversed PSX code faithful; any PC-specific
// convenience features should be gated behind macros in this file.
#pragma once
// Replace the OG PSX DebugCam (pad-only) with a PC-friendly camera:
// WASD to move, LMB+drag to look, Shift for speed boost, Q/E for up/down.
// When disabled, DebugCam uses the original PSX pad-button controls.
#define IMPROVED_DEBUG_CAM 1

// Skip initial intros and goes straight into title screen
#define SKIP_INTRO 0

// Adds support to any aspect ratio, fixes ui and camera.
#define FIX_ASPECT_RATIO 1

// Director cutscene bars: 0 = PSX-like alpha fade, 1 = slide in/out using alpha as progress.
#define DIRECTOR_WIDESCREEN_SLIDE_BARS 1

// Automatic cutscene borders (letterbox/pillarbox) based on the current aspect ratio, instead of hardcoded 4:3.
#define DIRECTOR_SMART_CUTSCENE_BORDERS 1

// During real cutscenes switch the projection from Hor+ to Vert-
// This stops any display wider than 4:3 from revealing off camera content past the sides of a
// directed shot, matching exactly what the original hardware framed.
#define DIRECTOR_CUTSCENE_VERT_FOV 1

// When ESC is bound to both menu back and menu open/close,
// let it back out of submenu pages before closing the whole menu.
#define ESC_BACKS_OUT_SUBMENUS_FIRST 1

// Menu back button shape policy:
// 0 = PSX-original Triangle, 1 = Circle (Xbox B)
#define MENU_BACK_USES_CIRCLE 1

// Custom text files with localization support, parsed from game directory (requires .txt files for each language).
#define CUSTOM_TEXT 1

// Custom reimplementation of the game menus, allowing mouse support and custom settings. (Requires CUSTOM_TEXT for the text strings)
#define CUSTOM_MENU 1

// Keep simulation fixed at 30 fps while rendering at higher frame rates, with interpolation for smoothness.
#define HIGH_FPS_PLAY_PRESENTATION 1

// Mod loader support: when enabled, the game checks the ~mods/ directory for
// replacement assets (PNG textures, GLB models, WAV sounds, JSON data) before
// falling back to original PSX-format assets. Set to 0 to keep original code
// path fully intact with zero mod-related overhead.
#if defined(RC_PLATFORM_PSP)
#define MOD_LOADER 0
#else
#define MOD_LOADER 1
#endif

// Adds a persistent Cheats page and its optional gameplay changes.
#define NEW_CHEATS 1

// Credits entry on the title and main menus: a scrolling roll of the ReChan
// code contributors. PSP Edition only for now.
#if defined(RC_PLATFORM_PSP)
#define CREDITS_PAGE (CUSTOM_MENU && 1)
#else
#define CREDITS_PAGE 0
#endif

// First boot: choose the language and the HUD style (Modern/Classic) after
// the legal notice. PSP Edition only; its settings file keeps the choice.
#if defined(RC_PLATFORM_PSP)
#define FIRST_BOOT_SETUP (CUSTOM_MENU && 1)
#else
#define FIRST_BOOT_SETUP 0
#endif

// Real-texture rendering: adds a second, runtime-switchable rendering path
// (toggle via the debug UI) that samples a real full-resolution 2D texture
// per material instead of the shared PSX VRAM/CLUT atlas. Off by default at
// runtime even when compiled in. Every named texture chunk (character and
// world geometry) still gets decoded at native resolution either way, so
// toggling never changes vanilla (unmodded) visuals -- it only lifts the
// resolution/palette cap for ModLoader texture overrides.
#if defined(RC_PLATFORM_PSP)
#define REAL_TEXTURE_RENDERING 0
#else
#define REAL_TEXTURE_RENDERING 1
#endif

// Downloads a release archive and self-restarts the process
#if defined(RC_PLATFORM_NULL) || defined(RC_PLATFORM_PSP)
#define AUTO_UPDATER 0
#else
#define AUTO_UPDATER (CUSTOM_MENU && 1)
#endif

// Graphics improvements shadows and post processing.
#if defined(RC_PLATFORM_PSP)
#define MODERN_GRAPHICS 0
#else
#define MODERN_GRAPHICS 1
#endif

// Spatial audio path for positional world sfx.
#if defined(RC_PLATFORM_PSP)
#define MODERN_SPATIAL_AUDIO 0
#else
#define MODERN_SPATIAL_AUDIO 1
#endif

// Smooths .str movie playback: bilinear GPU texture filtering, bilinear
// chroma upsampling in the software MDEC decoder, and a bicubic+sharpen
// resample shader for the fullscreen draw. When disabled, movies render
// exactly as on original PSX hardware (nearest filtering, blocky chroma).
#if defined(RC_PLATFORM_PSP)
#define MODERN_MOVIE_FILTERING 0
#else
#define MODERN_MOVIE_FILTERING 1
#endif

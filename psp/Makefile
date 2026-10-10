TARGET = rechan
BUILD_MODE ?= release

PSP_EBOOT_TITLE = Re:Chain
EXTRA_TARGETS = EBOOT.PBP
# MEMSIZE=1 in PARAM.SFO: PSP-2000/3000/Go (and PPSSPP) give the game the
# extended user partition (~52 MB instead of ~24 MB); PSP-1000 keeps the small
# one. The SDK default (MEMSIZE=2) left the heap at ~17.9 MB on a PSP-3000.
PSP_LARGE_MEMORY = 1

ROOT = ..
SRC_DIRS = \
	$(ROOT)/src/ai \
	$(ROOT)/src/fe \
	$(ROOT)/src/gen \
	$(ROOT)/src/radlib \
	$(ROOT)/src/radmovie \
	$(ROOT)/src/snd \
	$(ROOT)/src/xclib \
	$(ROOT)/vendor/libp3d/p3d

SOURCES = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.cpp))
SOURCES += $(ROOT)/src/extra/cheats.cpp \
	$(ROOT)/src/extra/colorpulse.cpp \
	$(ROOT)/src/extra/customhudmgr.cpp \
	$(ROOT)/src/extra/customtext.cpp \
	$(ROOT)/src/extra/dialogpreview.cpp \
	$(ROOT)/src/extra/fecustommenumgr.cpp \
	$(ROOT)/src/extra/hudwsfix.cpp \
	$(ROOT)/src/extra/jsondataloader.cpp \
	$(ROOT)/src/extra/shadowcsm.cpp \
	$(ROOT)/src/extra/wavloader.cpp
SOURCES += $(ROOT)/src/pc/coldebug.cpp \
	$(ROOT)/src/pc/controllerlight.cpp \
	$(ROOT)/src/pc/inputaction.cpp \
	$(ROOT)/src/pc/textbackend.cpp \
	$(ROOT)/src/pc/textmgr.cpp \
	$(ROOT)/src/pc/tim.cpp
SOURCES += $(ROOT)/src/psp/psp_audio.cpp \
	$(ROOT)/src/psp/psp_crashreporter.cpp \
	$(ROOT)/src/psp/psp_debugui.cpp \
	$(ROOT)/src/psp/psp_fileio.cpp \
	$(ROOT)/src/psp/psp_funcprof.cpp \
	$(ROOT)/src/psp/psp_log.cpp \
	$(ROOT)/src/psp/psp_perf.cpp \
	$(ROOT)/src/psp/psp_psxdiscextractor.cpp \
	$(ROOT)/src/psp/psp_qa.cpp \
	$(ROOT)/src/psp/psp_settings.cpp \
	$(ROOT)/vendor/libp3d/pddi/psp/psprender.cpp \
	$(ROOT)/psp/psp_module.cpp

# These desktop implementations pull in GLFW/miniaudio/ImGui/filesystem or
# spawn worker threads. Their PSP replacements live in src/psp/.
SOURCES := $(filter-out \
	$(ROOT)/vendor/libp3d/p3d/fileio.cpp \
	$(ROOT)/src/extra/psxdiscextractor.cpp \
	$(ROOT)/src/pc/audio.cpp \
	$(ROOT)/src/pc/crashreporter.cpp \
	$(ROOT)/src/pc/debugui.cpp \
	$(ROOT)/src/pc/log.cpp \
	$(ROOT)/src/pc/miniaudio_impl.cpp \
	$(ROOT)/src/pc/settings.cpp \
	$(ROOT)/src/extra/assetexporter.cpp \
	$(ROOT)/src/extra/assetexporter_ui.cpp \
	$(ROOT)/src/extra/autoupdater.cpp \
	$(ROOT)/src/extra/gltfloader.cpp \
	$(ROOT)/src/extra/modloader.cpp \
	$(ROOT)/src/extra/parameterdata.cpp \
	$(ROOT)/src/extra/realtexture.cpp,$(SOURCES))

OBJS = $(SOURCES:.cpp=.o)

# build.mak prefixes every entry in INCDIR with -I.
INCDIR = $(ROOT)/src $(ROOT)/vendor/libp3d $(ROOT)/vendor/cgltf $(ROOT)/vendor/stb
# Same submission path as the measured benchmark/telemetry builds
# (scripts/build_psp_usb.sh): native indexed draws with list memory and
# persistent indices. All three are covered by scripts/test_psp_renderer.sh.
DEFINES = -DRC_PLATFORM_PSP -DPSP -DPSP_GE -DIMGUI_DISABLE -DPSP_SORT_OPAQUE_MATERIALS \
	-DPSP_ENABLE_NATIVE_INDEXED -DPSP_INDEXED_LIST_MEMORY -DPSP_INDEXED_PERSISTENT_INDICES

# Small-data (gp-relative) addressing. -G0 (inherited from the PSPSDK sample
# Makefiles) puts every global behind a lui+addiu/lw absolute pair. The module
# already runs with a valid $gp: PSP_MODULE_INFO publishes _gp, every thread
# here is created by this module, nothing runs in interrupt context, and the
# prebuilt newlib/libstdc++ already use gp-relative .sdata. -G8 lets globals of
# up to 8 bytes use one gp-relative access; the linker rejects any GPREL16
# reference out of range, and .sdata+.sbss stay ~6.5 KB of the 64 KB window.
# Applied to the static-ELF release EBOOT only; PRX/telemetry/diagnostic modes
# keep -G0. Override with PSP_GP_SIZE=0 (or 8) for A/B tests.
ifeq ($(BUILD_MODE)$(BUILD_PRX),release)
PSP_GP_SIZE ?= 8
endif
PSP_GP_SIZE ?= 0

# -Wdouble-promotion: the Allegrex FPU is single precision only, so every
# implicit float->double promotion becomes soft-float library calls
# (__extendsfdf2/__muldf3/...). Warning only; it flags accidental double math.
COMMON_FLAGS = $(DEFINES) -std=gnu++17 -G$(PSP_GP_SIZE) -ffunction-sections -fdata-sections \
	-fno-strict-aliasing -mno-check-zero-division -Wall -Wextra -Wdouble-promotion

CFLAGS += $(COMMON_FLAGS)
CXXFLAGS += $(COMMON_FLAGS)
ifeq ($(BUILD_MODE),debug)
CXXFLAGS += -O0 -g3 -DPSP_PERF_TELEMETRY -DPSP_TRACE_TELEMETRY
else ifeq ($(BUILD_MODE),diagnostic)
# Keep the diagnostic binary representative of the non-logging renderer:
# NDEBUG removes the broad draw probes and PSP_TRACE_TELEMETRY is deliberately
# omitted because synchronous Memory Stick writes perturb the GE scheduling.
# PSP_GU_DIAGNOSTIC enables only the bounded markers around the suspect GU
# submission path.
CXXFLAGS += -O0 -g3 -DNDEBUG -DPSP_PERF_TELEMETRY -DPSP_GU_DIAGNOSTIC
else ifeq ($(BUILD_MODE),diagnostic-opt)
# Keep the bounded GU markers, but compile the renderer at the same speed as
# the profile build.  This isolates genuine PSP/PPSSPP stalls from the cost of
# the O0 diagnostic binary.  The extra perf checkpoints are intentionally
# limited to the first few frames and are not part of shipping builds.
CXXFLAGS += -O3 -g3 -DNDEBUG -DPSP_PERF_TELEMETRY -DPSP_GU_DIAGNOSTIC -DPSP_PERF_DIAGNOSTIC
else ifeq ($(BUILD_MODE),stage-opt)
# High-level stage markers only. Unlike GU diagnostics this does not log every
# material/primitive, so it remains useful for locating a frame stall without
# changing the renderer's scheduling characteristics.
CXXFLAGS += -O3 -g3 -DNDEBUG -DPSP_PERF_TELEMETRY -DPSP_STAGE_DIAGNOSTIC
else ifeq ($(BUILD_MODE),profile)
CXXFLAGS += -O3 -DNDEBUG -DPSP_PERF_TELEMETRY
else ifeq ($(BUILD_MODE),soak)
# Manual normal boot and unbounded session; sample RAM without gameplay I/O.
CXXFLAGS += -O3 -DNDEBUG -DPSP_PERF_TELEMETRY -DPSP_PERF_SOAK
else ifeq ($(BUILD_MODE),usb-debug)
# PSPLINK: retain release optimization and normal boot, with ELF symbols.
# No synchronous trace/probe logging while measuring on real hardware.
CXXFLAGS += -O3 -g3 -DNDEBUG -DPSP_PERF_TELEMETRY -DPSP_PERF_SOAK
else ifeq ($(BUILD_MODE),shipping)
CXXFLAGS += -Os -DNDEBUG
else
CXXFLAGS += -O3 -DNDEBUG
endif

# Optional link-time optimization for A/B measurement on hardware (off by
# default): make -C psp BUILD_MODE=release PSP_LTO=1. The link step must see
# the same optimization level, so the mode's -O flag is forwarded to LDFLAGS.
# Measured with pspdev gcc 15.2: the LTO link does not apply -G, so a
# PSP_LTO=1 binary falls back to absolute addressing (.sdata stays ~1.5 KB).
PSP_LTO ?= 0
ifeq ($(PSP_LTO),1)
PSP_LTO_OPT := $(filter -O%,$(CXXFLAGS))
CXXFLAGS += -flto=auto
LDFLAGS += -flto=auto $(PSP_LTO_OPT)
endif

# Optional function profile on real hardware (off by default), for the
# automatic benchmark (PSP_AUTO_START_PHASE1 + PSP_PERF_TELEMETRY): each
# scenario's measurement window writes PROF_Sxx.TXT next to its PSP_PERF
# report (src/psp/psp_funcprof.cpp). Only the sources below are
# instrumented, and functions defined in headers (".h": inline helpers, STL)
# are left to their callers, so the PRX stays small enough for PSPLink's
# user partition (inline sites would otherwise each get enter/exit calls).
PSP_FUNC_PROFILE ?= 0
ifeq ($(PSP_FUNC_PROFILE),1)
CXXFLAGS += -DPSP_FUNC_PROFILE
# Character/animation/world/renderer path: ~150 KB of extra code. Instrumenting
# whole directories (+390 KB) leaves the PSPLink PRX heap short at the peak.
PSP_PROFILE_SOURCES ?= src/ai/humanoid.cpp src/ai/behaviour.cpp src/ai/player.cpp \
	src/ai/thing.cpp src/gen/model.cpp src/gen/world.cpp src/gen/geometry.cpp \
	src/gen/animmat.cpp src/gen/animmgr.cpp src/gen/lights.cpp src/gen/charmgr.cpp \
	src/gen/shadows.cpp src/gen/game.cpp src/gen/ai.cpp \
	src/fe/hud.cpp src/xclib/xclib.cpp src/extra/hudwsfix.cpp \
	$(patsubst $(ROOT)/%,%,$(wildcard $(ROOT)/vendor/libp3d/p3d/*.cpp)) \
	vendor/libp3d/pddi/psp/psprender.cpp
PSP_PROFILE_OBJS = $(addprefix $(ROOT)/,$(PSP_PROFILE_SOURCES:.cpp=.o))
# Tiny per-vertex/per-entry helpers are left to their callers: entry/exit
# hooks on every call would dominate (and distort) their measured cost.
# (One line: a backslash continuation would insert spaces into the list.)
PSP_PROFILE_EXCLUDED_FUNCTIONS = ToPspVertexByte,MakeGeometryColor,PackIntegralTexCoord,ConvertPsxTexelToGu5551,GetUnwrappedTime,GuCapability,LoadGumMatrix,PsxLitChannelToGuByte,ModulatePsxColourChannel,ClampLitChannelToByte,ComputeLightIntensity12,WritebackDcacheRange
$(PSP_PROFILE_OBJS): CXXFLAGS += -finstrument-functions \
	-finstrument-functions-exclude-file-list=.h,/usr/local/pspdev,psp_funcprof \
	-finstrument-functions-exclude-function-list=$(PSP_PROFILE_EXCLUDED_FUNCTIONS)
endif

# build.mak supplies the user-mode PSP runtime and core exports. Keep only
# libraries used directly by this title; adding kernel/legacy SDK libraries
# here causes duplicate newlib symbols with current PSPDev releases.
# The PSPDev VFPU GUM implementation keeps matrix setup/multiplication on
# the Allegrex vector unit. The renderer already avoids redundant uploads, so
# this changes only the implementation of the remaining matrix operations;
# the PDDI math contract and visible vertex data stay unchanged.
LIBS = -lstdc++ -lpspgum_vfpu -lpspvfpu -lpspgu -lpspaudiolib -lpspaudio -lpspaudio_driver -lpsppower -lm

EXTRA_CLEAN = $(OBJS)

include $(PSPSDK)/lib/build.mak

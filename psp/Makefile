TARGET = rechan
BUILD_MODE ?= release

PSP_EBOOT_TITLE = Re:Chain
EXTRA_TARGETS = EBOOT.PBP

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
	$(ROOT)/src/psp/psp_log.cpp \
	$(ROOT)/src/psp/psp_perf.cpp \
	$(ROOT)/src/psp/psp_psxdiscextractor.cpp \
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
DEFINES = -DRC_PLATFORM_PSP -DPSP -DPSP_GE -DIMGUI_DISABLE

COMMON_FLAGS = $(DEFINES) -std=gnu++17 -G0 -ffunction-sections -fdata-sections \
	-fno-strict-aliasing -mno-check-zero-division -Wall -Wextra

CFLAGS += $(COMMON_FLAGS)
CXXFLAGS += $(COMMON_FLAGS)
ifeq ($(BUILD_MODE),debug)
CXXFLAGS += -O0 -g3 -DPSP_PERF_TELEMETRY
else ifeq ($(BUILD_MODE),profile)
CXXFLAGS += -O3 -DNDEBUG -DPSP_PERF_TELEMETRY
else ifeq ($(BUILD_MODE),shipping)
CXXFLAGS += -Os -DNDEBUG
else
CXXFLAGS += -O3 -DNDEBUG
endif

# build.mak supplies the user-mode PSP runtime and core exports. Keep only
# libraries used directly by this title; adding kernel/legacy SDK libraries
# here causes duplicate newlib symbols with current PSPDev releases.
LIBS = -lstdc++ -lpspgum -lpspgu -lpspaudiolib -lpspaudio -lpspaudio_driver -lpsppower -lm

EXTRA_CLEAN = $(OBJS)

include $(PSPSDK)/lib/build.mak

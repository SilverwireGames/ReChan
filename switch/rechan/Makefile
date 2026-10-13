.SUFFIXES:

TOPDIR ?= $(CURDIR)
export DEVKITPRO := $(TOPDIR)/../../vendor/devkitpro
ORIGINAL_PATH := $(PATH)

include $(DEVKITPRO)/libnx/switch_rules

export PATH := $(DEVKITPRO)/devkitA64/bin;$(DEVKITPRO)/tools/bin;$(ORIGINAL_PATH)

PORTLIBS_PATH := $(DEVKITPRO)/portlibs
PORTLIBS := $(PORTLIBS_PATH)/switch

DEBUG	?=	0

ifeq ($(strip $(DEBUG)),1)
	TARGET		:=	rechan-debug
	BUILD		:=	build-debug
	OPTFLAGS	:=	-Og
	LIBP3D		:=	-lp3d-debug
else
	TARGET		:=	rechan
	BUILD		:=	build
	OPTFLAGS	:=	-O3
	LIBP3D		:=	-lp3d
endif

SOURCES		:=	../../src/ai \
			../../src/gen \
			../../src/fe \
			../../src/snd \
			../../src/xclib \
			../../src/extra \
			../../src/pc \
			../../src/radlib \
			../../src/radmovie
DATA		:=
INCLUDES	:=	../../src \
			../../vendor/libp3d \
			../../vendor/libp3d/vendor/imgui \
			../../vendor/miniaudio \
			../../vendor/cgltf

ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

DEFINES	:=	-DRC_PLATFORM_SWITCH -DRC_PLATFORM_NULL -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
ifeq ($(strip $(DEBUG)),1)
	DEFINES	+=	-DDEBUG
else
	DEFINES	+=	-DNDEBUG
endif

CFLAGS	:=	-g -Wall $(OPTFLAGS) -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:=	$(CFLAGS) -std=gnu++20

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	$(LIBP3D) -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm

LIBDIRS	:=	$(PORTLIBS) $(LIBNX) $(CURDIR)/../libp3d

APP_TITLE	:=	ReChan
APP_AUTHOR	:=	SilverwireGames
APP_VERSION	:=	1.0.2

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).lst
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

-include $(DEPENDS)

endif

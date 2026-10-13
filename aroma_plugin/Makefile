.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO to the path to devkitPro")
endif

TOPDIR ?= $(CURDIR)
ifeq ($(wildcard $(DEVKITPPC)/base_rules),)
DEVKITPPC := $(DEVKITPRO)/devkitPPC
endif
WUPS_ROOT ?= $(DEVKITPRO)/wups
ifeq ($(wildcard $(WUPS_ROOT)/share/wups_rules),)
WUPS_ROOT := $(HOME)/.local/share/devkitpro/wups
endif
include $(WUPS_ROOT)/share/wups_rules
WUT_ROOT := $(DEVKITPRO)/wut
export LD := $(CC)
TARGET := NandDumperAutoTest
BUILD := build
SOURCES := src
INCLUDES := src

CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(MACHDEP)
CFLAGS += $(INCLUDE) -D__WIIU__ -D__WUT__ -D__WUPS__

ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG -g
endif

ifeq ($(DEBUG),VERBOSE)
CFLAGS += -DDEBUG -DVERBOSE_DEBUG -g
endif

CXXFLAGS := $(CFLAGS) -std=gnu++11

LDFLAGS = -g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map) $(WUPSSPECS)
LIBS := -lwups -lwut
LIBDIRS := $(PORTLIBS) $(WUPS_ROOT) $(WUT_ROOT)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
ifneq ($(strip $(CPPFILES)),)
export LD := $(CXX)
endif
export OFILES := $(OFILES_SRC)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	-I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean $(BUILD)

all: $(BUILD)

$(BUILD):
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(TARGET).wps $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

.PHONY: all

all: $(OUTPUT).wps

$(OUTPUT).wps: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
$(OFILES_SRC): $(HFILES_BIN)

-include $(DEPENDS)

endif

PLATFORM ?= simulator
WAD ?= doom1.wad
USE_EXTERNAL_IWAD ?= 0
USE_UNSTABLE_ZONE_HEAP_SIZE ?= 0
COMPRESS_TEXTURES ?= 0

NUMWORKS_APP_DIR ?= $(CURDIR)/numworks_app

HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
SIM_PLATFORM := macos
SIM_BIN ?= $(CURDIR)/epsilon.app/Contents/MacOS/Epsilon
else
SIM_PLATFORM := linux
SIM_BIN ?= $(CURDIR)/epsilon.bin
endif

NWB_FILE := $(NUMWORKS_APP_DIR)/output/$(SIM_PLATFORM)/gbadoom.nwb
NWA_FILE := $(NUMWORKS_APP_DIR)/output/device/gbadoom.nwa
WAD_ABS := $(abspath $(WAD))
WAD_BASENAME := $(notdir $(WAD))
IWAD_EMBED_NAME := $(subst -,_,$(basename $(WAD_BASENAME)))
IWAD_C_FILE := $(CURDIR)/source/iwad/$(IWAD_EMBED_NAME).c
IWAD_INCLUDE_DEFINE := -DEMBEDDED_IWAD_INCLUDE=\"iwad/$(IWAD_EMBED_NAME).c\"
BUILD_CFG_FILE := $(NUMWORKS_APP_DIR)/output/.gbadoom_build_cfg_$(PLATFORM)
BUNDLED_PWAD := $(CURDIR)/GbaWadUtil/gbadoom.wad

# Disable reciprocal table on device by default: saves ~200 KB of FLASH rodata.
ifeq ($(PLATFORM),device)
GBADOOM_DISABLE_RECIP_TABLE ?= 1
else
GBADOOM_DISABLE_RECIP_TABLE ?= 0
endif

BUILD_CFG = $(PLATFORM)|$(USE_EXTERNAL_IWAD)|$(USE_UNSTABLE_ZONE_HEAP_SIZE)|$(GBADOOM_ENABLE_STACK_REUSE)|$(GBADOOM_ENABLE_IWAD_DEBUG)|$(GBADOOM_DISABLE_RECIP_TABLE)|$(WAD_BASENAME)|$(GBADOOM_LINK_FLAGS)|$(GBADOOM_C_OPT_FLAGS)|$(GBADOOM_CXX_OPT_FLAGS)

GBADOOM_BASE_OPT_FLAGS := -Os -g0 -DNDEBUG -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-ident
GBADOOM_C_OPT_FLAGS := $(GBADOOM_BASE_OPT_FLAGS) -ffunction-sections
GBADOOM_CXX_OPT_FLAGS := $(GBADOOM_C_OPT_FLAGS) -fno-rtti
GBADOOM_LINK_FLAGS := -Wl,--gc-sections -Wl,--build-id=none -Wl,--strip-all
GBADOOM_C_OPT_FLAGS += -DGBADOOM_DISABLE_RECIP_TABLE=$(GBADOOM_DISABLE_RECIP_TABLE)
GBADOOM_CXX_OPT_FLAGS += -DGBADOOM_DISABLE_RECIP_TABLE=$(GBADOOM_DISABLE_RECIP_TABLE)

# off by default to save size
GBADOOM_ENABLE_IWAD_DEBUG ?= 0
ifeq ($(GBADOOM_ENABLE_IWAD_DEBUG),1)
GBADOOM_C_OPT_FLAGS += -DGBADOOM_VERBOSE_IWAD_LOGS=1
GBADOOM_CXX_OPT_FLAGS += -DGBADOOM_VERBOSE_IWAD_LOGS=1
else
GBADOOM_C_OPT_FLAGS += -DGBADOOM_VERBOSE_IWAD_LOGS=0
GBADOOM_CXX_OPT_FLAGS += -DGBADOOM_VERBOSE_IWAD_LOGS=0
endif

# Optional stack slot reuse (can expose UB in legacy code, keep off by default)
GBADOOM_ENABLE_STACK_REUSE ?= 0
ifeq ($(GBADOOM_ENABLE_STACK_REUSE),1)
GBADOOM_C_OPT_FLAGS += -fstack-reuse=all
GBADOOM_CXX_OPT_FLAGS += -fstack-reuse=all
endif

.PHONY: build run clean

$(IWAD_C_FILE): $(WAD_ABS) $(CURDIR)/GbaWadUtil/gbawadutil.py $(wildcard $(BUNDLED_PWAD))
	@mkdir -p $(dir $@)
	@echo "[GBADOOM] Embedding $(WAD_BASENAME) into $(notdir $@)"
	@$(CURDIR)/GbaWadUtil/gbawadutil.py -in "$(WAD_ABS)" -cfile "$@" $(if $(filter 1,$(COMPRESS_TEXTURES)),--compress-textures)

ifeq ($(PLATFORM),device)
ifeq ($(USE_EXTERNAL_IWAD),1)
NEEDS_EMBEDDED_IWAD := 0
else
NEEDS_EMBEDDED_IWAD := 1
endif
else
NEEDS_EMBEDDED_IWAD := 1
endif

build:
	@if [ "$(NEEDS_EMBEDDED_IWAD)" = "1" ]; then \
		$(MAKE) $(IWAD_C_FILE); \
	fi
	@mkdir -p $(NUMWORKS_APP_DIR)/output
	@if [ -f "$(BUILD_CFG_FILE)" ]; then \
		old_cfg=$$(cat "$(BUILD_CFG_FILE)"); \
		new_cfg="$(BUILD_CFG)"; \
		if [ "$$old_cfg" != "$$new_cfg" ]; then \
			echo "[NUMWORKS] Config changed ($$old_cfg -> $$new_cfg), cleaning stale objects"; \
			$(MAKE) -C $(NUMWORKS_APP_DIR) PLATFORM=$(PLATFORM) clean; \
		fi; \
	fi
	@echo "$(BUILD_CFG)" > "$(BUILD_CFG_FILE)"
	@echo "[GBADOOM] EXTRA_CFLAGS: $(GBADOOM_C_OPT_FLAGS) $(IWAD_INCLUDE_DEFINE)"
	@echo "[GBADOOM] EXTRA_CXXFLAGS: $(GBADOOM_CXX_OPT_FLAGS) $(IWAD_INCLUDE_DEFINE)"
	@echo "[GBADOOM] EXTRA_LDFLAGS: $(GBADOOM_LINK_FLAGS)"
	@echo "[GBADOOM] GBADOOM_DISABLE_RECIP_TABLE: $(GBADOOM_DISABLE_RECIP_TABLE)"
ifeq ($(PLATFORM),device)
	$(MAKE) -C $(NUMWORKS_APP_DIR) PLATFORM=device EXTERNAL_DATA=$(WAD_ABS) USE_EXTERNAL_IWAD=$(USE_EXTERNAL_IWAD) USE_UNSTABLE_ZONE_HEAP_SIZE=$(USE_UNSTABLE_ZONE_HEAP_SIZE) GBADOOM_DISABLE_RECIP_TABLE=$(GBADOOM_DISABLE_RECIP_TABLE) EXTRA_CFLAGS='$(GBADOOM_C_OPT_FLAGS) $(IWAD_INCLUDE_DEFINE)' EXTRA_CXXFLAGS='$(GBADOOM_CXX_OPT_FLAGS) $(IWAD_INCLUDE_DEFINE)' EXTRA_LDFLAGS='$(GBADOOM_LINK_FLAGS)' build
else
	$(MAKE) -C $(NUMWORKS_APP_DIR) PLATFORM=simulator SIMULATOR="$(SIM_BIN)" EXTERNAL_DATA= USE_EXTERNAL_IWAD=0 USE_UNSTABLE_ZONE_HEAP_SIZE=$(USE_UNSTABLE_ZONE_HEAP_SIZE) GBADOOM_DISABLE_RECIP_TABLE=$(GBADOOM_DISABLE_RECIP_TABLE) EXTRA_CFLAGS='$(GBADOOM_C_OPT_FLAGS) $(IWAD_INCLUDE_DEFINE)' EXTRA_CXXFLAGS='$(GBADOOM_CXX_OPT_FLAGS) $(IWAD_INCLUDE_DEFINE)' EXTRA_LDFLAGS='$(GBADOOM_LINK_FLAGS)' build
endif

run: build
ifeq ($(PLATFORM),device)
ifeq ($(USE_EXTERNAL_IWAD),1)
	npx --yes -- nwlink install-nwa --external-data $(WAD_ABS) $(NWA_FILE)
else
	npx --yes -- nwlink install-nwa $(NWA_FILE)
endif
else
	$(SIM_BIN) --nwb $(NWB_FILE)
endif

clean:
	$(MAKE) -C $(NUMWORKS_APP_DIR) PLATFORM=simulator clean
	$(MAKE) -C $(NUMWORKS_APP_DIR) PLATFORM=device clean

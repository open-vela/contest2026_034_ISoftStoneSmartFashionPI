############################################################################
# Contest 2026 - Boot logo & animation for ESP32-S3 Touch AMOLED
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = watch
PRIORITY  = 100
STACKSIZE = 32768
MODULE    = $(CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT)

MAINSRC = watch_main.c

# Add include path for current directory so that
# #include "apps/launcher/launcher.h" and #include "resource/resource.h" work
CFLAGS += -I.

# Add architecture include path for ESP32S3 GPIO access
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3

# ai_agent headers for voice channel integration
CFLAGS += -I$(APPDIR)/packages/ai_agent/include
CFLAGS += -I$(APPDIR)/packages/ai_agent/src

# Vendor watch UI (app_watch) include paths
VENDOR_WATCH_DIR = ../../app_watch
CFLAGS += -I$(VENDOR_WATCH_DIR)
CFLAGS += -I$(VENDOR_WATCH_DIR)/resource
CFLAGS += -I$(VENDOR_WATCH_DIR)/resource/image/generated
CFLAGS += -I$(VENDOR_WATCH_DIR)/resource/font/generated
CFLAGS += -I$(VENDOR_WATCH_DIR)/resource/power_jpg/generated
CFLAGS += -I$(VENDOR_WATCH_DIR)/resource/power_png/generated
CFLAGS += -I$(VENDOR_WATCH_DIR)/page
CFLAGS += -I$(VENDOR_WATCH_DIR)/utils
# Force-include display compatibility header for stub functions
CFLAGS += -include $(VENDOR_WATCH_DIR)/apps/common/display_compat.h

# mbedtls (vendor watch needs it)
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3/esp-hal-3rdparty/components/mbedtls/mbedtls/include
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3/esp-hal-3rdparty/nuttx/include/mbedtls
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3/esp-hal-3rdparty/nuttx/include
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3/esp-hal-3rdparty/components/mbedtls/port/include
LIBS += -lmbedtls -lmbedcrypto -lmbedx509

# Apps source files (boot + launcher + common + settings + home_control + volume_control)
CSRCS += $(shell find ./apps/boot -name "*.c")
CSRCS += $(shell find ./apps/launcher -name "*.c")
CSRCS += $(shell find ./apps/common -name "*.c")
CSRCS += $(shell find ./apps/settings -name "*.c")
CSRCS += $(shell find ./apps/home_control -name "*.c")
CSRCS += $(shell find ./apps/volume_control -name "*.c")

# Resource manager
CSRCS += resource/resource.c

# Image data (boot logo)
CSRCS += resource/image/app/boot/isoftstone_logo.c

# Generated icon images (settings icons, wifi signal icons, etc.)
CSRCS += $(wildcard ./resource/image/generated/*.c)

# Font C array data (subset TTF embedded)
CSRCS += $(wildcard ./resource/font/generated/*.c)

# Expression GIF images embedded as C arrays (from gif1)
EXPR_GIF1_GEN_DIR := ./resource/image/Expression/gif1/generated
CSRCS += $(wildcard $(EXPR_GIF1_GEN_DIR)/*.c)

# Animation frame data (PNG assets)
POWER_PNG_GEN_DIR := ./resource/power_png/generated
CSRCS += $(wildcard $(POWER_PNG_GEN_DIR)/*.c)

# ============================================================
# Vendor watch UI sources (app_watch)
# ============================================================

# app_watch apps (exclude xiaozhi_ai to avoid conflict with ai_agent)
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/alarm -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/boot -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/calendar -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/common -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/home_control -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/launcher -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/settings -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/sos -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/sports -name "*.c")
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/stopwatch -name "*.c")
# CSRCS += $(shell find $(VENDOR_WATCH_DIR)/apps/weather -name "*.c")  # Excluded: too much DRAM (40KB+ static buffers)

# app_watch resource manager
CSRCS += $(VENDOR_WATCH_DIR)/resource/resource.c

# app_watch generated assets (images, fonts, power animations)
CSRCS += $(wildcard $(VENDOR_WATCH_DIR)/resource/image/generated/*.c)
CSRCS += $(wildcard $(VENDOR_WATCH_DIR)/resource/font/generated/*.c)
CSRCS += $(wildcard $(VENDOR_WATCH_DIR)/resource/power_jpg/generated/*.c)
CSRCS += $(wildcard $(VENDOR_WATCH_DIR)/resource/power_png/generated/*.c)

# app_watch page system (LVGL Fragment pages)
# Exclude files with missing HAL dependencies: heart_rate, _template, dialplate
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/page -name "*.c" ! -name "heart_rate.c" ! -name "_template.c" ! -name "dialplate.c")

# app_watch utility modules
CSRCS += $(shell find $(VENDOR_WATCH_DIR)/utils -name "*.c")

# app_watch demo entry point
CSRCS += $(VENDOR_WATCH_DIR)/watch_demo.c

include $(APPDIR)/Application.mk

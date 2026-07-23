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

# Apps source files (boot + launcher + common + settings + home_control)
CSRCS += $(shell find ./apps/boot -name "*.c")
CSRCS += $(shell find ./apps/launcher -name "*.c")
CSRCS += $(shell find ./apps/common -name "*.c")
CSRCS += $(shell find ./apps/settings -name "*.c")
CSRCS += $(shell find ./apps/home_control -name "*.c")

# Resource manager
CSRCS += resource/resource.c

# Image data (boot logo)
CSRCS += resource/image/app/boot/isoftstone_logo.c

# Generated icon images (settings icons, wifi signal icons, etc.)
CSRCS += $(wildcard ./resource/image/generated/*.c)

# Font C array data (subset TTF embedded)
CSRCS += $(wildcard ./resource/font/generated/*.c)

# Expression GIF images embedded as C arrays (from gif2)
EXPR_GIF2_GEN_DIR := ./resource/image/Expression/gif2/generated
CSRCS += $(wildcard $(EXPR_GIF2_GEN_DIR)/*.c)

# Animation frame data (PNG assets)
POWER_PNG_GEN_DIR := ./resource/power_png/generated
CSRCS += $(wildcard $(POWER_PNG_GEN_DIR)/*.c)

include $(APPDIR)/Application.mk

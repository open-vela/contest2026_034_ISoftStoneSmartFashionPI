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

# Apps source files (boot + launcher + common)
CSRCS += $(shell find ./apps/boot -name "*.c")
CSRCS += $(shell find ./apps/launcher -name "*.c")
CSRCS += $(shell find ./apps/common -name "*.c")

# Resource manager
CSRCS += resource/resource.c

# Image data (boot logo)
CSRCS += resource/image/app/boot/isoftstone_logo.c

# Expression GIF images are loaded from SD card at runtime,
# no C array data is compiled into firmware.

# Animation frame data (PNG assets)
POWER_PNG_GEN_DIR := ./resource/power_png/generated
CSRCS += $(wildcard $(POWER_PNG_GEN_DIR)/*.c)

include $(APPDIR)/Application.mk

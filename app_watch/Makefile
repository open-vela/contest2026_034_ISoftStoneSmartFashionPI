############################################################################
# vendor/watch/Makefile
#
#   Copyright (C) 2025 Tianji Vela. All rights reserved.
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME = watch
PRIORITY = $(CONFIG_LVX_USE_ESP32S3_WATCH_PRIORITY)
STACKSIZE = $(CONFIG_LVX_USE_ESP32S3_WATCH_STACKSIZE)
MODULE = $(CONFIG_LVX_USE_ESP32S3_WATCH)

MAINSRC = watch_main.c


CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3/esp-hal-3rdparty/components/mbedtls/mbedtls/include
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3/esp-hal-3rdparty/nuttx/include/mbedtls
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3/esp-hal-3rdparty/nuttx/include
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3/esp-hal-3rdparty/components/mbedtls/port/include

LIBS += -lmbedtls -lmbedcrypto -lmbedx509

# 只收集apps和resource目录下的源文件,排除page、utils、calendar
CSRCS += $(shell find ./apps/common -name "*.c")
CSRCS += $(shell find ./apps/boot -name "*.c")
CSRCS += $(shell find ./apps/launcher -name "*.c")
CSRCS += $(shell find ./apps/settings -name "*.c")
CSRCS += $(shell find ./apps/calendar -name "*.c")
CSRCS += $(shell find ./apps/home_control -name "*.c")
CSRCS += $(shell find ./apps/stopwatch -name "*.c")
CSRCS += $(shell find ./apps/alarm -name "*.c")
CSRCS += $(shell find ./apps/sports -name "*.c")
CSRCS += $(shell find ./apps/sos -name "*.c")
CSRCS += $(shell find ./apps/xiaozhi_ai -name "*.c")
CSRCS += $(shell find ./apps/weather -name "*.c")
# 只编译resource.c，不编译image目录下的.c文件
CSRCS += resource/resource.c

############################################################################
# Asset directories (C arrays must be generated manually via flash_esp32s3.sh -C)
############################################################################

ASSETS_GEN_DIR      := ./resource/image/generated
FONT_GEN_DIR        := ./resource/font/generated
POWER_JPG_GEN_DIR   := ./resource/power_jpg/generated
POWER_PNG_GEN_DIR   := ./resource/power_png/generated

CFLAGS += -I$(ASSETS_GEN_DIR)
CSRCS  += $(wildcard $(ASSETS_GEN_DIR)/*.c)

CFLAGS += -I$(FONT_GEN_DIR)
CSRCS  += $(wildcard $(FONT_GEN_DIR)/*.c)

CFLAGS += -I$(POWER_JPG_GEN_DIR)
CSRCS  += $(wildcard $(POWER_JPG_GEN_DIR)/*.c)

CFLAGS += -I$(POWER_PNG_GEN_DIR)
CSRCS  += $(wildcard $(POWER_PNG_GEN_DIR)/*.c)

include $(APPDIR)/Application.mk
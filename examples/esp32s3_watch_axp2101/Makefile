
############################################################################
# apps/examples/esp32s3_watch_axp2101/Makefile
############################################################################
include $(APPDIR)/Make.defs

# ESP32S3 Watch AXP2101 Application

PROGNAME  = $(CONFIG_EXAMPLES_ESP32S3_WATCH_AXP2101_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_ESP32S3_WATCH_AXP2101_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_ESP32S3_WATCH_AXP2101_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_ESP32S3_WATCH_AXP2101)

MAINSRC = esp32s3_watch_axp2101_main.c

# Add architecture include path for ESP32S3 GPIO access
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3

include $(APPDIR)/Application.mk

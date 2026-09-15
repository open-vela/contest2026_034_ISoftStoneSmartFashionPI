
############################################################################
# apps/examples/esp32s3_watch_button/Makefile
############################################################################
include $(APPDIR)/Make.defs

# ESP32S3 Watch Button Application

PROGNAME  = $(CONFIG_EXAMPLES_ESP32S3_WATCH_BUTTON_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_ESP32S3_WATCH_BUTTON_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_ESP32S3_WATCH_BUTTON_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_ESP32S3_WATCH_BUTTON)

MAINSRC = esp32s3_watch_button_main.c

# Add architecture include path for ESP32S3 GPIO access
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3

include $(APPDIR)/Application.mk

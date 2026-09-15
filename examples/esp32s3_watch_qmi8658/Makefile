
############################################################################
# apps/examples/esp32s3_watch_qmi8658/Makefile
############################################################################
include $(APPDIR)/Make.defs

# ESP32S3 Watch qmi8658 Application

PROGNAME  = $(CONFIG_EXAMPLES_ESP32S3_WATCH_QMI8658_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_ESP32S3_WATCH_QMI8658_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_ESP32S3_WATCH_QMI8658_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_ESP32S3_WATCH_QMI8658)

MAINSRC = esp32s3_watch_qmi8658_main.c

# Add architecture include path for ESP32S3 GPIO access
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3

include $(APPDIR)/Application.mk


############################################################################
# apps/examples/esp32_wifi/Makefile
############################################################################
include $(APPDIR)/Make.defs
# ESP32 WiFi 应用信息
PROGNAME  = $(CONFIG_EXAMPLES_ESP32S3_WATCH_WIFI_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_ESP32S3_WATCH_WIFI_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_ESP32S3_WATCH_WIFI_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_ESP32S3_WATCH_WIFI)
# 主源文件
MAINSRC = esp32s3_watch_wifi_main.c
CFLAGS += -I$(TOPDIR)/arch/xtensa/src/esp32s3

include $(APPDIR)/Application.mk

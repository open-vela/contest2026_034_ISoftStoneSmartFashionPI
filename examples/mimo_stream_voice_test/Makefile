############################################################################
# apps/examples/mimo_stream_voice_test/Makefile
#
# Streaming MiMo ASR + TTS voice test for ESP32-S3.
############################################################################

include $(APPDIR)/Make.defs

# MiMo streaming voice test application info
PROGNAME  = $(CONFIG_EXAMPLES_MIMO_STREAM_VOICE_TEST_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_MIMO_STREAM_VOICE_TEST_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_MIMO_STREAM_VOICE_TEST_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_MIMO_STREAM_VOICE_TEST)

# Main source
MAINSRC = mimo_stream_voice_test_main.c

# ai_agent voice/infra headers
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/src

# cJSON (for JSON parsing)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/include/netutils

# mbedtls (for base64 encode/decode)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include

ifeq ($(CONFIG_ARCH_CHIP_ESP32S3),y)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../nuttx/arch/xtensa/src/chip/esp-hal-3rdparty/nuttx/include
CFLAGS += -include mbedtls/esp_mbedtls.h
endif

include $(APPDIR)/Application.mk

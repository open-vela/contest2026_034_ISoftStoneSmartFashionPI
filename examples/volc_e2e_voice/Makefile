############################################################################
# apps/examples/volc_e2e_voice/Makefile
############################################################################
include $(APPDIR)/Make.defs

# Volcengine E2E voice client
PROGNAME  = $(CONFIG_EXAMPLES_VOLC_E2E_VOICE_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_VOLC_E2E_VOICE_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_VOLC_E2E_VOICE_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_VOLC_E2E_VOICE)

# Main source
MAINSRC = volc_e2e_voice_main.c

# Access to ai_agent public headers
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include

# Access to mbedtls headers (for TLS)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include

# ESP32-S3 specific: use esp_mbedtls wrapper
ifeq ($(CONFIG_ARCH_CHIP_ESP32S3),y)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../nuttx/arch/xtensa/src/chip/esp-hal-3rdparty/nuttx/include
CFLAGS += -include mbedtls/esp_mbedtls.h
endif

# Access to media_player headers
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../frameworks/multimedia/media/include

# Access to cJSON headers
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON

include $(APPDIR)/Application.mk

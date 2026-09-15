############################################################################
# apps/examples/mimo_voice_test/Makefile
############################################################################
include $(APPDIR)/Make.defs

# MiMo voice test application info
PROGNAME  = $(CONFIG_EXAMPLES_MIMO_VOICE_TEST_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_MIMO_VOICE_TEST_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_MIMO_VOICE_TEST_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_MIMO_VOICE_TEST)

# Main source
MAINSRC = mimo_voice_test_main.c

# Access to ai_agent public voice API headers (voice_asr.h / voice_tts.h)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include

include $(APPDIR)/Application.mk

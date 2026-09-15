/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "channels/cmd_voice.h"
#include "infra/config_store.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_asr.h"
#include "voice/voice_channel.h"
#include "voice/voice_tts.h"

#include <stdio.h>
#include <string.h>

void cmd_set_volc_key(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_volc_key <api_key>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_API_KEY, argv[1]);
    printf("Doubao voice API key saved.\n");
}

void cmd_set_volc_speaker(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_volc_speaker <speaker_id>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_SPEAKER, argv[1]);
    printf("TTS speaker set to: %s\n", argv[1]);
}

void cmd_set_volc_asr(int argc, char** argv)
{
    if (argc < 4) {
        printf("Usage: set_volc_asr <app_id> <token> <cluster>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_APPKEY, argv[1]);
    claw_config_set(AGENT_CFG_KEY_VOLC_TOKEN, argv[2]);
    claw_config_set(AGENT_CFG_KEY_VOLC_ASR_CLUSTER, argv[3]);
    printf("ASR credentials saved (app_id=%s, cluster=%s).\n",
        argv[1], argv[3]);
}

void cmd_voice_start(void)
{
    voice_channel_start();
}

void cmd_voice_stop(void)
{
    voice_channel_stop();
}

void cmd_set_voice_tts(int argc, char** argv)
{
    if (argc < 2) {
        const char* cur = voice_tts_get_backend();

        printf("Current TTS backend: %s\n",
            cur ? cur : "(none)");
        printf("Usage: set_voice_tts <backend_name>\n");
        return;
    }

    int ret = voice_tts_set_backend(argv[1]);

    if (ret == 0) {
        printf("TTS backend set to: %s\n", argv[1]);
    } else {
        printf("TTS backend '%s' not found.\n", argv[1]);
    }
}

void cmd_set_voice_asr(int argc, char** argv)
{
    if (argc < 2) {
        const char* cur = voice_asr_get_backend();

        printf("Current ASR backend: %s\n",
            cur ? cur : "(none)");
        printf("Usage: set_voice_asr <backend_name>\n");
        return;
    }

    int ret = voice_asr_set_backend(argv[1]);

    if (ret == 0) {
        printf("ASR backend set to: %s\n", argv[1]);
    } else {
        printf("ASR backend '%s' not found.\n", argv[1]);
    }
}

/* ── Volcengine E2E realtime dialogue ────────────────────────── */

void cmd_set_voice_mode(int argc, char** argv)
{
    char cur[16] = { 0 };

    if (argc < 2) {
        claw_config_get(AGENT_CFG_KEY_VOICE_MODE, cur, sizeof(cur));
        printf("Current voice mode: %s\n", cur[0] ? cur : "classic");
        printf("Active backends: asr=%s tts=%s\n",
            voice_asr_get_backend() ? voice_asr_get_backend() : "(none)",
            voice_tts_get_backend() ? voice_tts_get_backend() : "(none)");
        printf("Usage: set_voice_mode <classic|e2e>\n");
        return;
    }

    if (strcmp(argv[1], "classic") == 0) {
        int rc_a = voice_asr_set_backend(AGENT_VOICE_BACKEND_ASR_VOLC);
        int rc_t = voice_tts_set_backend(AGENT_VOICE_BACKEND_TTS_WS);
        if (rc_a != 0 || rc_t != 0) {
            printf("Failed to activate classic backends (asr=%d tts=%d).\n",
                rc_a, rc_t);
            return;
        }
        claw_config_set(AGENT_CFG_KEY_VOICE_MODE, "classic");
        printf("Voice mode: classic (volcengine ASR + WebSocket TTS)\n");
        return;
    }

    if (strcmp(argv[1], "e2e") == 0) {
        /* Set both sides atomically: a half-switched pipeline would send
         * audio over one transport and expect PCM from another. */
        int rc_a = voice_asr_set_backend(AGENT_VOICE_BACKEND_E2E);
        int rc_t = voice_tts_set_backend(AGENT_VOICE_BACKEND_E2E);
        if (rc_a != 0 || rc_t != 0) {
            voice_asr_set_backend(AGENT_VOICE_BACKEND_ASR_VOLC);
            voice_tts_set_backend(AGENT_VOICE_BACKEND_TTS_WS);
            printf("E2E backend unavailable (asr=%d tts=%d). "
                   "Enable CONFIG_AI_AGENT_VOLC_E2E.\n", rc_a, rc_t);
            return;
        }
        claw_config_set(AGENT_CFG_KEY_VOICE_MODE, "e2e");
        printf("Voice mode: e2e (single persistent WebSocket)\n");
        return;
    }

    printf("Usage: set_voice_mode <classic|e2e>\n");
}

void cmd_set_volc_e2e_speaker(int argc, char** argv)
{
    if (argc < 2) {
        char cur[80] = { 0 };
        claw_config_get(AGENT_CFG_KEY_VOLC_E2E_SPEAKER, cur, sizeof(cur));
        printf("Current E2E speaker: %s\n",
            cur[0] ? cur : AGENT_VOLC_E2E_DEFAULT_SPEAKER);
        printf("Usage: set_volc_e2e_speaker <voice_type>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_VOLC_E2E_SPEAKER, argv[1]);
    printf("E2E speaker set to: %s (reconnect on next turn)\n", argv[1]);
}

/* ── MiMo (Xiaomi) ASR/TTS configuration commands ────────────── */

void cmd_set_mimo_key(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_mimo_key <api_key>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_MIMO_API_KEY, argv[1]);
    printf("MiMo API key saved.\n");
}

void cmd_set_mimo_voice(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_mimo_voice <voice_name>\n");
        printf("  e.g. 冰糖 / 茉莉 / 苏打 / 小红\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_MIMO_VOICE, argv[1]);
    printf("MiMo TTS voice set to: %s\n", argv[1]);
}

void cmd_set_mimo_asr_lang(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_mimo_asr_lang <auto|zh|en>\n");
        return;
    }
    claw_config_set(AGENT_CFG_KEY_MIMO_ASR_LANG, argv[1]);
    printf("MiMo ASR language set to: %s\n", argv[1]);
}

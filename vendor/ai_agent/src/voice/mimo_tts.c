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

/*
 * MiMo (Xiaomi) TTS backend.
 *
 * API: POST https://api.xiaomimimo.com/v1/chat/completions
 * Auth: api-key header
 * Model: mimo-v2.5-tts
 *
 * Non-streaming: response.choices[0].message.audio.data is base64 WAV.
 * Streaming (SSE): each "data: {...}\n" line contains a JSON chunk with
 *   choices[0].delta.audio.data as base64 pcm16 (24kHz mono).
 */

#include "voice/mimo_tts.h"
#include "voice/voice_tts.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include "mbedtls/base64.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char *TAG = "mimo_tts";

/* ── Credentials ──────────────────────────────────────────────── */

static char s_api_key[64];
static char s_voice[48];

static int mimo_tts_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_voice, 0, sizeof(s_voice));

    /* Try MiMo-specific key first, fall back to shared LLM key (set_llm) */
    if (claw_config_get(AGENT_CFG_KEY_MIMO_API_KEY,
            s_api_key, sizeof(s_api_key)) != OK || s_api_key[0] == '\0') {
        claw_config_get(AGENT_CFG_KEY_API_KEY,
            s_api_key, sizeof(s_api_key));
    }

    if (claw_config_get(AGENT_CFG_KEY_MIMO_VOICE,
            s_voice, sizeof(s_voice)) != OK
        || s_voice[0] == '\0') {
        strncpy(s_voice, AGENT_MIMO_TTS_DEFAULT_VOICE,
            sizeof(s_voice) - 1);
    }

    return 0;
}

/* ── Build request JSON body ──────────────────────────────────── */

static char *build_tts_request(const char *text, const char *voice,
    int use_stream)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", AGENT_MIMO_TTS_MODEL);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", "");
    cJSON_AddItemToArray(messages, user_msg);

    cJSON *assist_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(assist_msg, "role", "assistant");
    cJSON_AddStringToObject(assist_msg, "content", text);
    cJSON_AddItemToArray(messages, assist_msg);

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio, "format",
        use_stream ? "pcm16" : "wav");
    cJSON_AddStringToObject(audio, "voice", voice);

    if (use_stream) {
        cJSON_AddBoolToObject(root, "stream", 1);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

/* ── Extract PCM from WAV data ────────────────────────────────── */

static int extract_pcm_from_wav(const unsigned char *wav, size_t wav_len,
    const unsigned char **pcm_out, size_t *pcm_len_out)
{
    if (wav_len < 44) {
        return -EPROTO;
    }

    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        syslog(LOG_ERR, "[%s] not a WAV file\n", TAG);
        return -EPROTO;
    }

    size_t off = 12;
    while (off + 8 <= wav_len) {
        const unsigned char *chunk_id = wav + off;
        uint32_t chunk_size = (uint32_t)wav[off + 4]
            | ((uint32_t)wav[off + 5] << 8)
            | ((uint32_t)wav[off + 6] << 16)
            | ((uint32_t)wav[off + 7] << 24);

        if (memcmp(chunk_id, "data", 4) == 0) {
            size_t data_off = off + 8;
            if (data_off + chunk_size > wav_len) {
                chunk_size = (uint32_t)(wav_len - data_off);
            }
            *pcm_out = wav + data_off;
            *pcm_len_out = chunk_size;
            return 0;
        }

        off += 8 + chunk_size;
    }

    syslog(LOG_ERR, "[%s] no data chunk found in WAV\n", TAG);
    return -EPROTO;
}

/* ── Parse TTS non-streaming response ─────────────────────────── */

static int parse_tts_response(const char *resp,
    unsigned char *pcm_out, size_t pcm_cap, size_t *pcm_len_out)
{
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        /* Log first 256 bytes of response to diagnose */
        syslog(LOG_ERR, "[%s] failed to parse response JSON (len=%zu): %.256s\n",
            TAG, strlen(resp), resp);
        return -EPROTO;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices)
        || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        return -EPROTO;
    }

    cJSON *first = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first, "message");
    cJSON *audio = message ? cJSON_GetObjectItem(message, "audio") : NULL;
    cJSON *data = audio ? cJSON_GetObjectItem(audio, "data") : NULL;

    if (!data || !cJSON_IsString(data)
        || !data->valuestring || data->valuestring[0] == '\0') {
        cJSON_Delete(root);
        syslog(LOG_ERR, "[%s] no audio.data in response\n", TAG);
        return -EPROTO;
    }

    const char *b64 = data->valuestring;
    size_t b64_len = strlen(b64);
    size_t wav_cap = (b64_len / 4) * 3 + 4;

    unsigned char *wav_buf = malloc(wav_cap);
    if (!wav_buf) {
        cJSON_Delete(root);
        return -ENOMEM;
    }

    size_t wav_len = 0;
    int ret = mbedtls_base64_decode(wav_buf, wav_cap, &wav_len,
        (const unsigned char *)b64, b64_len);
    cJSON_Delete(root);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] base64_decode failed: -0x%04x\n",
            TAG, -ret);
        free(wav_buf);
        return -EPROTO;
    }

    const unsigned char *pcm_ptr = NULL;
    size_t pcm_len = 0;
    ret = extract_pcm_from_wav(wav_buf, wav_len, &pcm_ptr, &pcm_len);
    if (ret != 0) {
        free(wav_buf);
        return ret;
    }

    if (pcm_len > pcm_cap) {
        syslog(LOG_WARNING,
            "[%s] PCM truncated: %zu -> %zu\n", TAG, pcm_len, pcm_cap);
        pcm_len = pcm_cap;
    }

    memcpy(pcm_out, pcm_ptr, pcm_len);
    *pcm_len_out = pcm_len;

    free(wav_buf);
    return 0;
}

/* ── Batch synthesize (non-streaming) ─────────────────────────── */

static int mimo_tts_synthesize(const char *text,
    unsigned char *pcm_out, size_t pcm_cap, size_t *pcm_len)
{
    if (!text || !pcm_out || !pcm_len) {
        return -EINVAL;
    }

    mimo_tts_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] API key not configured\n", TAG);
        return -ENOENT;
    }

    *pcm_len = 0;

    char *body = build_tts_request(text, s_voice, 0);
    if (!body) {
        return -ENOMEM;
    }

    vela_header_t hdrs[] = {
        {"api-key", s_api_key},
        {NULL, NULL}
    };

    /* 1 MB response buffer — base64 WAV for ~60 Chinese chars can exceed
     * 512 KB; truncation leaves stale data that corrupts the next pooled
     * request.  8 MB PSRAM makes this safe (freed before PCM buffer). */
    size_t resp_cap = 1024 * 1024;
    char *resp = calloc(1, resp_cap);
    if (!resp) {
        /* Fallback to 512 KB if 1 MB isn't available */
        resp_cap = 512 * 1024;
        resp = calloc(1, resp_cap);
        if (!resp) {
            free(body);
            return -ENOMEM;
        }
    }

    syslog(LOG_INFO, "[%s] TTS request: text=%zu bytes voice=%s\n",
        TAG, strlen(text), s_voice);

    size_t resp_len = 0;
    int status = vela_https_request(
        AGENT_MIMO_API_HOST, AGENT_MIMO_API_PORT,
        "POST", AGENT_MIMO_API_PATH, hdrs,
        body, strlen(body),
        resp, resp_cap, &resp_len);

    free(body);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] HTTP %d: %.256s\n", TAG, status, resp);
        free(resp);
        return -EIO;
    }

    if (resp_len == 0) {
        syslog(LOG_ERR, "[%s] empty response body\n", TAG);
        free(resp);
        return -EPROTO;
    }

    /* Detect truncation — resp_len near buffer limit means we lost data */
    if (resp_len >= resp_cap - 1) {
        syslog(LOG_ERR, "[%s] response truncated (%zu/%zu bytes), "
            "text too long for batch TTS\n", TAG, resp_len, resp_cap);
        free(resp);
        return -EFBIG;
    }

    int ret = parse_tts_response(resp, pcm_out, pcm_cap, pcm_len);
    free(resp);

    if (ret == 0) {
        syslog(LOG_INFO, "[%s] synthesized %zu PCM bytes\n",
            TAG, *pcm_len);
    }

    return ret;
}

/* ── Parse SSE stream and deliver PCM chunks ──────────────────── */

static int parse_sse_stream(char *resp, size_t resp_len,
    mimo_tts_chunk_cb cb, void *user_data)
{
    int chunks = 0;
    char *line = resp;
    char *end = resp + resp_len;

    size_t decode_cap = 16 * 1024;
    unsigned char *decode_buf = malloc(decode_cap);
    if (!decode_buf) {
        return -ENOMEM;
    }

    while (line < end) {
        while (line < end && (*line == '\r' || *line == '\n'
                              || *line == ' ')) {
            line++;
        }
        if (line >= end) {
            break;
        }

        char *eol = memchr(line, '\n', (size_t)(end - line));
        char *line_end = eol ? eol : end;

        if (line_end - line < 5 || memcmp(line, "data:", 5) != 0) {
            line = eol ? eol + 1 : end;
            continue;
        }

        char *payload = line + 5;
        while (payload < line_end && (*payload == ' ' || *payload == '\t')) {
            payload++;
        }

        size_t plen = (size_t)(line_end - payload);
        if (plen >= 6 && memcmp(payload, "[DONE]", 6) == 0) {
            break;
        }

        if (plen == 0) {
            line = eol ? eol + 1 : end;
            continue;
        }

        char save = *line_end;
        *line_end = '\0';

        cJSON *obj = cJSON_Parse(payload);
        *line_end = save;

        if (!obj) {
            line = eol ? eol + 1 : end;
            continue;
        }

        cJSON *choices = cJSON_GetObjectItem(obj, "choices");
        if (choices && cJSON_IsArray(choices)
            && cJSON_GetArraySize(choices) > 0) {
            cJSON *first = cJSON_GetArrayItem(choices, 0);
            cJSON *delta = cJSON_GetObjectItem(first, "delta");
            cJSON *audio = delta ? cJSON_GetObjectItem(delta, "audio")
                                 : NULL;
            cJSON *data = audio ? cJSON_GetObjectItem(audio, "data")
                                : NULL;

            if (data && cJSON_IsString(data)
                && data->valuestring && data->valuestring[0]) {
                const char *b64 = data->valuestring;
                size_t b64_len = strlen(b64);
                size_t decoded = 0;

                int ret = mbedtls_base64_decode(decode_buf, decode_cap,
                    &decoded, (const unsigned char *)b64, b64_len);
                if (ret == 0 && decoded > 0) {
                    cb(decode_buf, decoded, 0, user_data);
                    chunks++;
                } else if (ret != 0) {
                    syslog(LOG_WARNING,
                        "[%s] stream: base64 decode failed: -0x%04x\n",
                        TAG, -ret);
                }
            }
        }

        cJSON_Delete(obj);
        line = eol ? eol + 1 : end;
    }

    free(decode_buf);

    if (chunks > 0) {
        cb(NULL, 0, 1, user_data);
    }

    return chunks > 0 ? 0 : -EPROTO;
}

/* ── Streaming synthesize (SSE) ───────────────────────────────── */

int mimo_tts_synthesize_stream(const char *text,
    mimo_tts_chunk_cb cb, void *user_data)
{
    if (!text || !cb) {
        return -EINVAL;
    }

    mimo_tts_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] stream: API key not configured\n", TAG);
        return -ENOENT;
    }

    char *body = build_tts_request(text, s_voice, 1);
    if (!body) {
        return -ENOMEM;
    }

    vela_header_t hdrs[] = {
        {"api-key", s_api_key},
        {"Accept", "text/event-stream"},
        {NULL, NULL}
    };

    size_t resp_cap = 512 * 1024;
    char *resp = calloc(1, resp_cap);
    if (!resp) {
        free(body);
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] stream: text=%zu bytes voice=%s\n",
        TAG, strlen(text), s_voice);

    size_t resp_len = 0;
    int status = vela_https_request(
        AGENT_MIMO_API_HOST, AGENT_MIMO_API_PORT,
        "POST", AGENT_MIMO_API_PATH, hdrs,
        body, strlen(body),
        resp, resp_cap, &resp_len);

    free(body);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] stream: HTTP %d\n", TAG, status);
        free(resp);
        return -EIO;
    }

    if (resp_len == 0) {
        free(resp);
        return -EPROTO;
    }

    int ret = parse_sse_stream(resp, resp_len, cb, user_data);
    free(resp);

    syslog(LOG_INFO, "[%s] stream: done ret=%d\n", TAG, ret);
    return ret;
}

/* ── Backend ops registration ──────────────────────────────────── */

static const voice_tts_ops_t s_mimo_tts_ops = {
    .name = "mimo",
    .init = mimo_tts_init,
    .synthesize = mimo_tts_synthesize,
    .synthesize_stream = mimo_tts_synthesize_stream,
    .deinit = NULL,
};

int mimo_tts_register(void)
{
    return voice_tts_register(&s_mimo_tts_ops);
}

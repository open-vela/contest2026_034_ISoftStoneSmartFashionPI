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
 * MiMo (Xiaomi) ASR backend.
 *
 * API: POST https://api.xiaomimimo.com/v1/chat/completions
 * Auth: api-key header
 * Model: mimo-v2.5-asr
 *
 * Audio is sent as Base64-encoded WAV inside a chat message.
 * The API is non-streaming for audio input (all audio uploaded at once).
 */

#include "voice/mimo_asr.h"
#include "voice/voice_asr.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include "mbedtls/base64.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char *TAG = "mimo_asr";

/* ── Credentials ──────────────────────────────────────────────── */

static char s_api_key[64];
static char s_asr_lang[16];

static int mimo_asr_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_asr_lang, 0, sizeof(s_asr_lang));

    /* Try MiMo-specific key first, fall back to shared LLM key (set_llm) */
    if (claw_config_get(AGENT_CFG_KEY_MIMO_API_KEY,
            s_api_key, sizeof(s_api_key)) != OK || s_api_key[0] == '\0') {
        claw_config_get(AGENT_CFG_KEY_API_KEY,
            s_api_key, sizeof(s_api_key));
    }

    if (claw_config_get(AGENT_CFG_KEY_MIMO_ASR_LANG,
            s_asr_lang, sizeof(s_asr_lang)) != OK
        || s_asr_lang[0] == '\0') {
        strncpy(s_asr_lang, AGENT_MIMO_ASR_DEFAULT_LANG,
            sizeof(s_asr_lang) - 1);
    }

    return 0;
}

/* ── WAV header builder ───────────────────────────────────────── */

#define WAV_HEADER_SIZE 44

static void build_wav_header(unsigned char *hdr, size_t pcm_len)
{
    size_t file_size = WAV_HEADER_SIZE + pcm_len - 8;
    size_t data_size = pcm_len;

    memcpy(hdr + 0, "RIFF", 4);
    hdr[4] = (unsigned char)(file_size & 0xFF);
    hdr[5] = (unsigned char)((file_size >> 8) & 0xFF);
    hdr[6] = (unsigned char)((file_size >> 16) & 0xFF);
    hdr[7] = (unsigned char)((file_size >> 24) & 0xFF);
    memcpy(hdr + 8, "WAVE", 4);

    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16;
    hdr[17] = 0;
    hdr[18] = 0;
    hdr[19] = 0;
    hdr[20] = 1;
    hdr[21] = 0;
    hdr[22] = AGENT_VOICE_CHANNELS;
    hdr[23] = 0;
    hdr[24] = (unsigned char)(AGENT_VOICE_SAMPLE_RATE & 0xFF);
    hdr[25] = (unsigned char)((AGENT_VOICE_SAMPLE_RATE >> 8) & 0xFF);
    hdr[26] = 0;
    hdr[27] = 0;

    uint32_t byte_rate = AGENT_VOICE_SAMPLE_RATE
        * AGENT_VOICE_CHANNELS * (AGENT_VOICE_BITS / 8);
    hdr[28] = (unsigned char)(byte_rate & 0xFF);
    hdr[29] = (unsigned char)((byte_rate >> 8) & 0xFF);
    hdr[30] = (unsigned char)((byte_rate >> 16) & 0xFF);
    hdr[31] = (unsigned char)((byte_rate >> 24) & 0xFF);

    uint16_t block_align = AGENT_VOICE_CHANNELS * (AGENT_VOICE_BITS / 8);
    hdr[32] = (unsigned char)(block_align & 0xFF);
    hdr[33] = (unsigned char)((block_align >> 8) & 0xFF);

    hdr[34] = AGENT_VOICE_BITS;
    hdr[35] = 0;

    memcpy(hdr + 36, "data", 4);
    hdr[40] = (unsigned char)(data_size & 0xFF);
    hdr[41] = (unsigned char)((data_size >> 8) & 0xFF);
    hdr[42] = (unsigned char)((data_size >> 16) & 0xFF);
    hdr[43] = (unsigned char)((data_size >> 24) & 0xFF);
}

/* ── Build request JSON body ──────────────────────────────────── */

static char *build_asr_request(const unsigned char *pcm_data,
    size_t pcm_len, const char *lang)
{
    /* Detect whether the input already has a valid RIFF/WAVE header.
     * When called from mimo_voice_test with a recorded WAV file, we
     * can reuse the buffer in-place and skip a ~78 KB allocation. */
    bool is_wav = (pcm_len >= WAV_HEADER_SIZE
                   && memcmp(pcm_data, "RIFF", 4) == 0
                   && memcmp(pcm_data + 8, "WAVE", 4) == 0);

    const unsigned char *wav_data;
    size_t wav_len;
    unsigned char *wav_buf_alloc = NULL;

    if (is_wav)
      {
        /* Already a valid WAV — use directly, no copy needed. */
        wav_data = pcm_data;
        wav_len  = pcm_len;
      }
    else
      {
        /* Raw PCM — build a WAV header around it. */
        wav_len = WAV_HEADER_SIZE + pcm_len;
        wav_buf_alloc = malloc(wav_len);
        if (!wav_buf_alloc)
          {
            syslog(LOG_ERR, "[%s] wav buf alloc failed (%zu)\n", TAG, wav_len);
            return NULL;
          }
        build_wav_header(wav_buf_alloc, pcm_len);
        memcpy(wav_buf_alloc + WAV_HEADER_SIZE, pcm_data, pcm_len);
        wav_data = wav_buf_alloc;
      }

    /* Combine prefix + base64 output into a single allocation.
     * This avoids having b64_buf and data_url simultaneously,
     * halving the peak memory in this function (~235 KB saved). */
    static const char prefix[] = "data:audio/wav;base64,";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t b64_cap = ((wav_len + 2) / 3) * 4 + 1;
    size_t combined_cap = prefix_len + b64_cap;
    char *data_url = malloc(combined_cap);
    if (!data_url) {
        syslog(LOG_ERR, "[%s] data_url alloc failed (%zu)\n", TAG, combined_cap);
        free(wav_buf_alloc);
        return NULL;
    }

    /* Write the data-URL prefix, then base64-encode WAV directly after it. */
    memcpy(data_url, prefix, prefix_len);
    size_t b64_len = 0;
    int b64_ret = mbedtls_base64_encode(
        (unsigned char *)(data_url + prefix_len), b64_cap,
        &b64_len, wav_data, wav_len);
    free(wav_buf_alloc);   /* may be NULL if input was already WAV */
    if (b64_ret != 0) {
        syslog(LOG_ERR, "[%s] base64_encode failed: -0x%04x\n",
               TAG, -b64_ret);
        free(data_url);
        return NULL;
    }
    data_url[prefix_len + b64_len] = '\0';

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(data_url);
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", AGENT_MIMO_ASR_MODEL);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");

    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "input_audio");

    cJSON *input_audio = cJSON_AddObjectToObject(item, "input_audio");
    cJSON_AddStringToObject(input_audio, "data", data_url);

    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);

    cJSON *asr_opts = cJSON_AddObjectToObject(root, "asr_options");
    cJSON_AddStringToObject(asr_opts, "language", lang);

    char *json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    free(data_url);

    return json_str;
}

/* ── Parse ASR response ───────────────────────────────────────── */

static int parse_asr_response(const char *resp, size_t resp_len,
    char *text_out, size_t text_cap)
{
    (void)resp_len;
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        syslog(LOG_ERR, "[%s] failed to parse response JSON\n", TAG);
        return -EPROTO;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices)
        || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        syslog(LOG_ERR, "[%s] no choices in response\n", TAG);
        return -EPROTO;
    }

    cJSON *first = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first, "message");
    if (!message) {
        cJSON_Delete(root);
        return -EPROTO;
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!content || !cJSON_IsString(content)
        || !content->valuestring || content->valuestring[0] == '\0') {
        cJSON_Delete(root);
        syslog(LOG_WARNING, "[%s] empty content in response\n", TAG);
        text_out[0] = '\0';
        return -ENODATA;
    }

    strncpy(text_out, content->valuestring, text_cap - 1);
    text_out[text_cap - 1] = '\0';

    cJSON_Delete(root);
    return 0;
}

/* ── Core recognize function ───────────────────────────────────── */

static int mimo_asr_recognize_impl(const unsigned char *pcm_data,
    size_t pcm_len, char *text_out, size_t text_cap)
{
    if (!pcm_data || pcm_len == 0 || !text_out || text_cap == 0) {
        return -EINVAL;
    }

    if (pcm_len > AGENT_MIMO_ASR_MAX_AUDIO) {
        syslog(LOG_ERR, "[%s] audio too large: %zu > %zu\n",
            TAG, pcm_len, (size_t)AGENT_MIMO_ASR_MAX_AUDIO);
        return -EFBIG;
    }

    text_out[0] = '\0';
    mimo_asr_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] API key not configured\n", TAG);
        return -ENOENT;
    }

    syslog(LOG_INFO, "[%s] recognizing %zu bytes PCM\n", TAG, pcm_len);

    char *body = build_asr_request(pcm_data, pcm_len, s_asr_lang);
    if (!body) {
        syslog(LOG_ERR, "[%s] failed to build request\n", TAG);
        return -ENOMEM;
    }

    vela_header_t hdrs[] = {
        {"api-key", s_api_key},
        {NULL, NULL}
    };

    size_t resp_cap = 4096;
    char *resp = calloc(1, resp_cap);
    if (!resp) {
        free(body);
        return -ENOMEM;
    }

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

    int ret = parse_asr_response(resp, resp_len, text_out, text_cap);
    free(resp);

    if (ret == 0 && text_out[0] != '\0') {
        syslog(LOG_INFO, "[%s] recognized: %.80s\n", TAG, text_out);
    }

    return ret;
}

/* ── Streaming ASR (accumulate then batch recognize) ──────────── */

struct mimo_asr_stream {
    unsigned char *buf;
    size_t len;
    size_t cap;
};

mimo_asr_stream_t *mimo_asr_stream_open(void)
{
    mimo_asr_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] stream: API key not configured\n", TAG);
        return NULL;
    }

    mimo_asr_stream_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }

    s->cap = AGENT_MIMO_ASR_MAX_AUDIO;
    s->buf = malloc(s->cap);
    if (!s->buf) {
        syslog(LOG_ERR, "[%s] stream: alloc failed (%zu)\n",
            TAG, s->cap);
        free(s);
        return NULL;
    }

    s->len = 0;
    syslog(LOG_INFO, "[%s] stream: session opened\n", TAG);
    return s;
}

int mimo_asr_stream_send(mimo_asr_stream_t *s,
    const unsigned char *pcm, size_t len)
{
    if (!s || !pcm || len == 0) {
        return -EINVAL;
    }

    if (s->len + len > s->cap) {
        size_t avail = s->cap - s->len;
        if (avail > 0) {
            memcpy(s->buf + s->len, pcm, avail);
            s->len += avail;
        }
        syslog(LOG_WARNING,
            "[%s] stream: buffer full, truncated %zu bytes\n",
            TAG, len - avail);
        return 0;
    }

    memcpy(s->buf + s->len, pcm, len);
    s->len += len;
    return 0;
}

int mimo_asr_stream_finish(mimo_asr_stream_t *s,
    char *text_out, size_t text_cap)
{
    if (!s || !text_out || text_cap == 0) {
        if (s) {
            free(s->buf);
            free(s);
        }
        return -EINVAL;
    }

    text_out[0] = '\0';

    if (s->len == 0) {
        syslog(LOG_WARNING, "[%s] stream: no audio captured\n", TAG);
        free(s->buf);
        free(s);
        return -ENODATA;
    }

    int ret = mimo_asr_recognize_impl(s->buf, s->len, text_out, text_cap);

    free(s->buf);
    free(s);

    if (ret == 0 && text_out[0] == '\0') {
        ret = -ENODATA;
    }

    return ret;
}

void mimo_asr_stream_abort(mimo_asr_stream_t *s)
{
    if (!s) {
        return;
    }

    free(s->buf);
    free(s);
    syslog(LOG_INFO, "[%s] stream: aborted\n", TAG);
}

/* ── Backend ops registration ──────────────────────────────────── */

static int mimo_asr_recognize_ops(const unsigned char *pcm_data,
    size_t pcm_len, char *text_out, size_t text_cap)
{
    mimo_asr_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] ASR API key not configured\n", TAG);
        return -ENOENT;
    }

    return mimo_asr_recognize_impl(pcm_data, pcm_len, text_out, text_cap);
}

static const voice_asr_ops_t s_mimo_asr_ops = {
    .name = "mimo",
    .init = mimo_asr_init,
    .recognize = mimo_asr_recognize_ops,
    .deinit = NULL,
};

int mimo_asr_register(void)
{
    return voice_asr_register(&s_mimo_asr_ops);
}

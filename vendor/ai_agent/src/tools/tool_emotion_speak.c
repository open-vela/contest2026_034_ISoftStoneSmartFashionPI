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
 * tool_emotion_speak.c — Stub implementations for speak and set_face tools.
 *
 * speak:     Parse text parameter, log via syslog, return success JSON.
 *            voice_tts_speak() exists in include/voice/voice_tts.h but is a
 *            PCM synthesis function (not playback); full TTS pipeline is not
 *            wired up yet, so this remains a pure stub.
 *
 * set_face:  Parse face_id (validated against 16-value whitelist) and optional
 *            duration, log via syslog, return success JSON.
 */

#include "tools/tool_emotion.h"
#include "agent_compat.h"
#include <string.h>
#include <syslog.h>

#include "cJSON.h"

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
/* Forward declare watch expression API (flat build, symbol from watch app) */
extern int watch_expression_page_set_face(const char* face_id, int duration_ms);
#endif

/* ── speak ─────────────────────────────────────────────────────────── */

int tool_speak_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"invalid json\"}");
        return ERROR;
    }

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring[0]) {
        snprintf(output, output_size, "{\"error\":\"text required\"}");
        cJSON_Delete(root);
        return ERROR;
    }

    const char *txt = text->valuestring;
    size_t len = strlen(txt);

    /* Text length check — warn if exceeding 200 chars */
    if (len > 200) {
        syslog(LOG_WARNING, "[emotion] speak text truncated: %zu > 200\n", len);
    }

    /* Log only — TTS is deferred to dispatch_response for unified playback.
     * Playing TTS here would compete with the final LLM response TTS,
     * causing double-playback. */
    syslog(LOG_INFO, "[emotion] speak (deferred): %s\n", txt);

    /* Store last speak text so the ReAct loop can use it as final
     * response, skipping the LLM text-wrap-up round (~5s savings). */
    extern char* s_speak_text;
    free(s_speak_text);
    s_speak_text = strdup(txt);

    cJSON_Delete(root);

    /* Build output with cJSON for proper string escaping */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "text", txt);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) {
        strncpy(output, s, output_size - 1);
        output[output_size - 1] = '\0';
        free(s);
    }
    return OK;
}

/* ── set_face ──────────────────────────────────────────────────────── */

/* 16-expression whitelist (system faces listening/thinking/speaking excluded) */
static const char * const s_face_whitelist[] = {
    "happy",    "neutral",   "love",      "peaceful",
    "confused", "excited",   "sick",      "worried",
    "heartbeat","cool",      "shy",
    "sleepy",   "sleeping",  "proud",
    "standby",  "waiting"
};
#define FACE_WHITELIST_SIZE (sizeof(s_face_whitelist) / sizeof(s_face_whitelist[0]))

int tool_set_face_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"invalid json\"}");
        return ERROR;
    }

    cJSON *face_id = cJSON_GetObjectItem(root, "face_id");
    if (!cJSON_IsString(face_id) || !face_id->valuestring[0]) {
        snprintf(output, output_size, "{\"error\":\"face_id required\"}");
        cJSON_Delete(root);
        return ERROR;
    }

    /* Whitelist secondary validation */
    bool valid = false;
    for (size_t i = 0; i < FACE_WHITELIST_SIZE; i++) {
        if (strcmp(face_id->valuestring, s_face_whitelist[i]) == 0) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        snprintf(output, output_size,
            "{\"error\":\"invalid face_id, must be one of 16 whitelisted values\"}");
        cJSON_Delete(root);
        return ERROR;
    }

    /* Parse optional duration.  Default 0 = persistent: the face holds
     * until the turn ends (next listening), matching the client-side
     * emotion faces.  A duration>0 still arms the watch_pages restore
     * timer — only pass it for genuinely transient expressions. */
    cJSON *dur = cJSON_GetObjectItem(root, "duration");
    int duration = cJSON_IsNumber(dur) ? dur->valueint : 0;

    /* Call display via watch app (flat build, symbol in same firmware) */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
    int ret = watch_expression_page_set_face(face_id->valuestring, duration * 1000);
    if (ret != 0) {
        syslog(LOG_WARNING, "[emotion] set_face failed: id=%s ret=%d\n",
               face_id->valuestring, ret);
        snprintf(output, output_size,
            "{\"status\":\"error\",\"reason\":\"display not available\"}");
        cJSON_Delete(root);
        return ERROR;
    }
    syslog(LOG_INFO, "[emotion] set_face: id=%s duration=%d\n",
           face_id->valuestring, duration);
#else
    syslog(LOG_INFO, "[emotion] set_face: id=%s duration=%d (display not built)\n",
           face_id->valuestring, duration);
#endif

    cJSON_Delete(root);

    snprintf(output, output_size,
        "{\"status\":\"ok\",\"face_id\":\"%s\",\"duration\":%d}",
        face_id->valuestring, duration);
    return OK;
}

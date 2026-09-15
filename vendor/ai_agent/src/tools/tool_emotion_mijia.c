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
 * tool_emotion_mijia.c — Stub implementation for trigger_mijia_scene.
 *
 * trigger_mijia_scene:  Parses scene_id (string, required). Validates against
 *                       a whitelist (TR-001: prevent scene_id injection). In
 *                       stub mode the whitelist is empty, so all requests are
 *                       rejected by default — this is the security-conscious
 *                       behaviour for a SENSITIVE tool before IoT integration
 *                       is wired up.
 *
 * When the real implementation is added, populate s_scene_whitelist[] with
 * pre-registered scene IDs and replace the rejection with an HTTPS POST to
 * the Mijia API.
 */

#include "tools/tool_emotion.h"
#include "agent_compat.h"

#include <string.h>
#include <syslog.h>

#include "cJSON.h"

/* ── Scene ID whitelist (TR-001) ───────────────────────────────────── */

/* Pre-registered scene IDs. Empty in stub mode — all requests rejected.
 * Populate this array when Mijia IoT integration is implemented:
 *   "scene_morning", "scene_evening", "scene_sleep", "scene_relax", ... */
static const char * const s_scene_whitelist[] = {
    /* (empty — no scenes registered yet) */
};
#define SCENE_WHITELIST_SIZE (sizeof(s_scene_whitelist) / sizeof(s_scene_whitelist[0]))

/* ── trigger_mijia_scene ───────────────────────────────────────────── */

int tool_trigger_mijia_scene_execute(const char *input_json,
                                      char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"invalid json\"}");
        return ERROR;
    }

    cJSON *scene_id = cJSON_GetObjectItem(root, "scene_id");
    if (!cJSON_IsString(scene_id) || !scene_id->valuestring[0]) {
        snprintf(output, output_size, "{\"error\":\"scene_id required\"}");
        cJSON_Delete(root);
        return ERROR;
    }

    const char *sid = scene_id->valuestring;
    cJSON_Delete(root);

    /* Whitelist validation (TR-001: prevent scene_id injection) */
    bool whitelisted = false;
    for (size_t i = 0; i < SCENE_WHITELIST_SIZE; i++) {
        if (strcmp(sid, s_scene_whitelist[i]) == 0) {
            whitelisted = true;
            break;
        }
    }

    if (!whitelisted) {
        syslog(LOG_WARNING,
            "[emotion] mijia scene rejected (not in whitelist): %s\n", sid);
        snprintf(output, output_size,
            "{\"status\":\"error\",\"reason\":\"scene not in whitelist\","
            "\"scene_id\":\"%s\"}",
            sid);
        return ERROR;
    }

    /* Stub: whitelisted scenes would trigger an HTTPS POST here.
     * Not implemented yet — return a mocked success response. */
    syslog(LOG_INFO, "[emotion] mijia scene triggered (mocked): %s\n", sid);
    snprintf(output, output_size,
        "{\"status\":\"ok\",\"scene_id\":\"%s\",\"mocked\":true}", sid);
    return OK;
}

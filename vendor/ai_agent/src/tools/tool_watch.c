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

/**
 * @file tool_watch.c
 * Watch device tools — battery & volume control.
 *
 * Volume uses percentage (10-100%) as the primary interface.
 * Legacy 5-level (0-4) is still accepted for backward compat.
 */

#include "tools/tool_watch.h"
#include "agent_compat.h"

#include <string.h>
#include <syslog.h>

#include "cJSON.h"

/* ── External watch APIs (resolved at link time) ─────────────── */

extern uint8_t watch_battery_get_level(void);
extern int  watch_volume_get_level(void);
extern int  watch_volume_set_level(int level);
extern int  watch_volume_step_up(void);
extern int  watch_volume_step_down(void);
extern int  watch_volume_get_value(void);
extern int  watch_volume_set_value(int value);
extern int  watch_volume_set_percent(int percent);
extern int  watch_volume_get_percent(void);
extern int  watch_volume_step_delta(int delta);

/* ── Tool: get_watch_battery ─────────────────────────────────── */

int tool_watch_battery_execute(const char *input_json,
                                char *output, size_t output_size)
{
    (void)input_json;

    uint8_t level = watch_battery_get_level();

    snprintf(output, output_size,
        "{\"ok\":true,\"level\":%u,\"charging\":false}", level);
    syslog(LOG_INFO, "[WatchTool] battery: %u%%\n", level);
    return OK;
}

/* ── Tool: get_watch_volume ──────────────────────────────────── */

int tool_watch_volume_get_execute(const char *input_json,
                                   char *output, size_t output_size)
{
    (void)input_json;

    int percent = watch_volume_get_percent();
    if (percent < 0) {
        snprintf(output, output_size,
            "{\"error\":\"volume read failed\"}");
        return ERROR;
    }

    int value = watch_volume_get_value();

    snprintf(output, output_size,
        "{\"ok\":true,\"percent\":%d,\"value\":%d}",
        percent, value);
    syslog(LOG_INFO, "[WatchTool] volume: %d%% (hw=%d)\n",
        percent, value);
    return OK;
}

/* ── Tool: set_watch_volume ──────────────────────────────────── */

int tool_watch_volume_set_execute(const char *input_json,
                                   char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size,
            "{\"error\":\"invalid JSON\"}");
        return ERROR;
    }

    cJSON *pct_item  = cJSON_GetObjectItem(root, "percent");
    cJSON *level_item = cJSON_GetObjectItem(root, "level");
    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    int ret;

    /* ── percent: 10-100 (primary) ───────────────────────── */
    if (pct_item && cJSON_IsNumber(pct_item)) {
        int pct = (int)pct_item->valuedouble;
        if (pct < 10)  pct = 10;
        if (pct > 100) pct = 100;
        ret = watch_volume_set_percent(pct);
        cJSON_Delete(root);
        if (ret == 0) {
            snprintf(output, output_size,
                "{\"ok\":true,\"percent\":%d}", pct);
            return OK;
        }
    }
    /* ── level: 1-5 (backward compat, floored at 1) ──────── */
    else if (level_item && cJSON_IsNumber(level_item)) {
        int level = (int)level_item->valuedouble;
        if (level >= 1 && level <= 5) level = level - 1;  /* 1-5 → 0-4 */
        if (level < 1) level = 1;  /* voice robot: level 0 = mute rejected */
        if (level > 4) level = 4;
        ret = watch_volume_set_level(level);
        cJSON_Delete(root);
        const char * const labels[] = {"静音","低","中","高","最大"};
        if (ret == 0) {
            snprintf(output, output_size,
                "{\"ok\":true,\"level\":%d,\"label\":\"%s\"}",
                level, labels[level]);
            return OK;
        }
    }
    /* ── action: up/down/unmute (no mute — see below) ───── */
    else if (action_item && cJSON_IsString(action_item)) {
        const char* action = action_item->valuestring;
        cJSON_Delete(root);

        if (strcmp(action, "up") == 0) {
            ret = watch_volume_step_delta(10);
        } else if (strcmp(action, "down") == 0) {
            ret = watch_volume_step_delta(-10);
        } else if (strcmp(action, "mute") == 0) {
            /* Voice robot: speaker mute makes all replies inaudible and
             * strands the user.  Steer the LLM to the audible minimum. */
            snprintf(output, output_size,
                "{\"error\":\"mute is not supported on this voice robot; "
                "use action down or percent 10 (minimum, still audible)\"}");
            return ERROR;
        } else if (strcmp(action, "unmute") == 0) {
            ret = watch_volume_set_percent(50);
        } else {
            snprintf(output, output_size,
                "{\"error\":\"action must be up/down/unmute\"}");
            return ERROR;
        }

        if (ret >= 0) {
            snprintf(output, output_size,
                "{\"ok\":true,\"percent\":%d}", ret);
            return OK;
        }
    } else {
        cJSON_Delete(root);
        snprintf(output, output_size,
            "{\"error\":\"need 'percent' (10-100), 'level' (0-4), or 'action' (up/down/mute/unmute)\"}");
        return ERROR;
    }

    snprintf(output, output_size, "{\"error\":\"volume set failed\"}");
    return ERROR;
}

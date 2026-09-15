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
 * @file tool_home_control.c
 * Smart home control tool for AI Agent.
 *
 * Wraps the watch app's home_control module (UDP discovery + TCP command).
 * Three tools exposed to LLM / fast-path:
 *   home_control_list   — all 12 devices with on/off status
 *   home_control_status — single device query
 *   home_control_toggle — turn device on/off
 */

#include "tools/tool_home_control.h"
#include "agent_compat.h"

#include <string.h>
#include <syslog.h>
#include <time.h>

#include "cJSON.h"

/* ── External home_control APIs (resolved at link time) ──────── */

extern void watch_home_control_reset_server(void);
extern int  watch_home_control_get_device_count(void);
extern const char *watch_home_control_get_device_name(int index);
extern bool watch_home_control_get_device_status(int index);
extern int  watch_home_control_toggle_device(int index, bool turn_on);

#define HC_DEVICE_COUNT 12

/* ── Device alias table ──────────────────────────────────────── */
/* Maps user-spoken names → official device index.
 * Each entry has the canonical name, index, and a NULL-terminated
 * list of keyword aliases for fuzzy matching. */

typedef struct {
    int          index;       /* HC_DEVICE_* */
    const char  *canonical;   /* official Chinese name */
    const char * const *aliases;     /* NULL-terminated alias list */
} hc_device_entry_t;

/* Aliases include both full names and short stems for multi-device
 * patterns like "把卧室和卫生间的灯关掉" where "卧室"/"卫生间" are
 * partial refs.  Short stems are listed LAST so full-name matches
 * take priority in single-device resolution. */
static const char * const aliases_tv[]       = { "电视", "电视机", "tv", "television", NULL };
static const char * const aliases_ac[]       = { "空调", "冷气", "ac", "air conditioner", NULL };
static const char * const aliases_floor[]    = { "地暖", "地热", "floor heating", NULL };
static const char * const aliases_fresh[]    = { "新风", "新风机", "fresh air", "ventilation", NULL };
static const char * const aliases_ambient[]  = { "客厅氛围灯", "氛围灯", "ambient light", NULL };
static const char * const aliases_living[]   = { "客厅灯", "客厅的灯", "大厅灯", "living room light",
                                          "客厅", NULL };  /* stem: "客厅" → 客厅灯 */
static const char * const aliases_kitchen[]  = { "厨房灯", "厨房的灯", "kitchen light",
                                          "厨房", NULL };  /* stem: "厨房" → 厨房灯 */
static const char * const aliases_entrance[] = { "玄关灯", "门厅灯", "入口灯", "entrance light",
                                          "玄关", NULL };  /* stem: "玄关" → 玄关灯 */
static const char * const aliases_bedroom[]  = { "卧室灯", "睡房灯", "bedroom light",
                                          "卧室", "睡房", NULL };  /* stem */
static const char * const aliases_bedbg[]    = { "卧室背景灯", "bedroom backlight", NULL };
static const char * const aliases_bath[]     = { "卫生间灯", "浴室灯", "厕所灯", "bathroom light",
                                          "卫生间", "浴室", "厕所", NULL };  /* stem */
static const char * const aliases_curtain[]  = { "窗帘", "curtain", "blind", NULL };

static const hc_device_entry_t s_device_table[HC_DEVICE_COUNT] = {
    { 0,  "电视",       aliases_tv       },
    { 1,  "空调",       aliases_ac       },
    { 2,  "地暖",       aliases_floor    },
    { 3,  "新风",       aliases_fresh    },
    { 4,  "客厅氛围灯", aliases_ambient  },
    { 5,  "客厅灯",     aliases_living   },
    { 6,  "厨房灯",     aliases_kitchen  },
    { 7,  "玄关灯",     aliases_entrance },
    { 8,  "卧室灯",     aliases_bedroom  },
    { 9,  "卧室背景灯", aliases_bedbg    },
    { 10, "卫生间灯",   aliases_bath     },
    { 11, "窗帘",       aliases_curtain  },
};

/* ── Device matching ─────────────────────────────────────────── */

/**
 * Find device index by fuzzy-matching a user-provided name string.
 * Checks against canonical name first, then all aliases.
 * Returns device index (0-11) on match, -1 on no match.
 */
static int hc_find_device(const char *name)
{
    if (!name || name[0] == '\0') return -1;

    for (int i = 0; i < HC_DEVICE_COUNT; i++) {
        /* Exact match on canonical name */
        if (strcasecmp(name, s_device_table[i].canonical) == 0) {
            return s_device_table[i].index;
        }
        /* Substring match on canonical (e.g. "客厅灯" matches "客厅灯") */
        if (strcasestr(s_device_table[i].canonical, name) ||
            strcasestr(name, s_device_table[i].canonical)) {
            return s_device_table[i].index;
        }
        /* Check aliases */
        for (const char * const *a = s_device_table[i].aliases; *a; a++) {
            if (strcasecmp(name, *a) == 0 ||
                strcasestr(name, *a) ||
                strcasestr(*a, name)) {
                return s_device_table[i].index;
            }
        }
    }
    return -1;
}

/* ── Tool: home_control_list ──────────────────────────────────── */

int tool_home_control_list_execute(const char *input_json,
                                    char *output, size_t output_size)
{
    (void)input_json;

    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < HC_DEVICE_COUNT; i++) {
        cJSON *dev = cJSON_CreateObject();
        cJSON_AddStringToObject(dev, "name",
            s_device_table[i].canonical);
        cJSON_AddNumberToObject(dev, "index", i);
        cJSON_AddBoolToObject(dev, "status",
            watch_home_control_get_device_status(i));
        cJSON_AddItemToArray(arr, dev);
    }

    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (s) {
        strncpy(output, s, output_size - 1);
        output[output_size - 1] = '\0';
        free(s);
        return OK;
    }

    snprintf(output, output_size, "{\"error\":\"json encode failed\"}");
    return ERROR;
}

/* ── Tool: home_control_status ────────────────────────────────── */

int tool_home_control_status_execute(const char *input_json,
                                      char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size,
            "{\"error\":\"invalid JSON\"}");
        return ERROR;
    }

    cJSON *name_item = cJSON_GetObjectItem(root, "device_name");
    if (!name_item || !cJSON_IsString(name_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size,
            "{\"error\":\"missing device_name\"}");
        return ERROR;
    }

    int idx = hc_find_device(name_item->valuestring);
    cJSON_Delete(root);

    if (idx < 0) {
        snprintf(output, output_size,
            "{\"error\":\"device not found\",\"query\":\"%s\"}",
            name_item->valuestring);
        return ERROR;
    }

    bool status = watch_home_control_get_device_status(idx);
    snprintf(output, output_size,
        "{\"ok\":true,\"device\":\"%s\",\"status\":\"%s\"}",
        s_device_table[idx].canonical,
        status ? "on" : "off");
    return OK;
}

/* ── Tool: home_control_toggle ────────────────────────────────── */

int tool_home_control_toggle_execute(const char *input_json,
                                      char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size,
            "{\"error\":\"invalid JSON\"}");
        return ERROR;
    }

    cJSON *name_item   = cJSON_GetObjectItem(root, "device_name");
    cJSON *action_item = cJSON_GetObjectItem(root, "action");

    if (!name_item || !cJSON_IsString(name_item) ||
        !action_item || !cJSON_IsString(action_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size,
            "{\"error\":\"missing device_name or action\"}");
        return ERROR;
    }

    const char *dev_name = name_item->valuestring;
    const char *action   = action_item->valuestring;
    int idx = hc_find_device(dev_name);

    cJSON_Delete(root);

    if (idx < 0) {
        snprintf(output, output_size,
            "{\"error\":\"device not found\",\"query\":\"%s\"}",
            dev_name);
        return ERROR;
    }

    bool turn_on;
    if (strcmp(action, "on") == 0) {
        turn_on = true;
    } else if (strcmp(action, "off") == 0) {
        turn_on = false;
    } else {
        snprintf(output, output_size,
            "{\"error\":\"action must be 'on' or 'off'\"}");
        return ERROR;
    }

    int ret = watch_home_control_toggle_device(idx, turn_on);

    if (ret == 0) {
        syslog(LOG_INFO, "[HomeControl] %s → %s (ok)\n",
            s_device_table[idx].canonical,
            turn_on ? "ON" : "OFF");
        snprintf(output, output_size,
            "{\"ok\":true,\"device\":\"%s\",\"action\":\"%s\","
            "\"message\":\"%s已%s\"}",
            s_device_table[idx].canonical,
            turn_on ? "on" : "off",
            s_device_table[idx].canonical,
            turn_on ? "打开" : "关闭");
    } else {
        syslog(LOG_ERR, "[HomeControl] %s → %s failed: %d\n",
            s_device_table[idx].canonical,
            turn_on ? "ON" : "OFF", ret);
        const char *err_msg;
        switch (ret) {
        case -2: err_msg = "WiFi未连接"; break;
        case -3: err_msg = "服务器发现失败"; break;
        case -4: err_msg = "命令发送失败"; break;
        default: err_msg = "未知错误"; break;
        }
        snprintf(output, output_size,
            "{\"error\":\"%s\",\"code\":%d}", err_msg, ret);
    }

    return ret == 0 ? OK : ERROR;
}

/* ── Multi-device match entry ─────────────────────────────────── */

#define HC_MAX_MATCHES 8

typedef struct {
    int  device_index;   /* HC_DEVICE_* */
    int  pos;            /* byte offset in source text */
    int  len;            /* matched alias length (higher = better) */
    bool is_stem;        /* true if matched a short stem alias */
} hc_match_t;

/* Collect ALL non-overlapping device matches from text.
 * Greedy: sort by (is_stem ASC, len DESC), then pick disjoint spans.
 * Returns number of devices matched, fills matches[]. */
static int hc_find_all_devices(const char *text, hc_match_t *matches)
{
    hc_match_t candidates[HC_DEVICE_COUNT * 8]; /* per-device max ~8 aliases */
    int n = 0;

    /* Collect every alias hit */
    for (int i = 0; i < HC_DEVICE_COUNT; i++) {
        /* full name */
        const char *pos = strcasestr(text, s_device_table[i].canonical);
        if (pos) {
            if (n < (int)(sizeof(candidates)/sizeof(candidates[0]))) {
                candidates[n].device_index = i;
                candidates[n].pos = (int)(pos - text);
                candidates[n].len = (int)strlen(s_device_table[i].canonical);
                candidates[n].is_stem = false;
                n++;
            }
        }
        /* aliases */
        for (const char * const *a = s_device_table[i].aliases; *a; a++) {
            pos = strcasestr(text, *a);
            if (pos) {
                if (n < (int)(sizeof(candidates)/sizeof(candidates[0]))) {
                    candidates[n].device_index = i;
                    candidates[n].pos = (int)(pos - text);
                    candidates[n].len = (int)strlen(*a);
                    /* A stem is a short form that is a substring of the
                     * canonical name (e.g. "客厅" ⊂ "客厅灯").  These
                     * get lower priority when resolving overlap. */
                    candidates[n].is_stem =
                        (strlen(*a) < strlen(s_device_table[i].canonical))
                        && (strstr(s_device_table[i].canonical, *a) != NULL);
                    n++;
                }
            }
        }
    }

    if (n == 0) return 0;

    /* Sort descending: non-stem first, then by match length.
     * score = (is_stem ? 0 : 1000) + len.  Simple bubble-sort. */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int si = (candidates[i].is_stem ? 0 : 1000) + candidates[i].len;
            int sj = (candidates[j].is_stem ? 0 : 1000) + candidates[j].len;
            if (sj > si) {
                hc_match_t tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }

    /* Greedy select non-overlapping, dedupe by device_index */
    int out = 0;
    bool used_index[HC_DEVICE_COUNT] = { false };

    for (int i = 0; i < n && out < HC_MAX_MATCHES; i++) {
        if (used_index[candidates[i].device_index]) continue;

        /* Check overlap with already-selected matches */
        int c_start = candidates[i].pos;
        int c_end   = c_start + candidates[i].len;
        bool overlaps = false;
        for (int j = 0; j < out; j++) {
            int s_start = matches[j].pos;
            int s_end   = s_start + matches[j].len;
            if (!(c_end <= s_start || c_start >= s_end)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) continue;

        matches[out] = candidates[i];
        used_index[candidates[i].device_index] = true;
        out++;
    }

    return out;
}

/* ── Direct text → toggle (fast-path, supports multi-device) ── */
/* Scans user_text for action keywords + one or more device names,
 * executes all toggles, builds a combined Chinese reply.
 * Returns 0 on success (reply allocated, caller frees),
 * -1 if text doesn't match. */

int tool_home_control_fast_toggle(const char *user_text,
                                   char **reply)
{
    if (!user_text || !reply) return -1;
    *reply = NULL;

    /* ── Determine action from text ── */
    bool turn_on     = false;
    bool found_action = false;

    static const char * const kw_on[] = {
        "打开", "开", "启动", "开启", "turn on", "start",
        "打开一下", "开一下", "帮我打开", "帮我开", "开开",
        "开起来", "启动下", "打开吧", "给打开",
        "想开", "要开", "来开", "点亮", "开灯", "上电",
        "帮忙开", "给开下", NULL };
    static const char * const kw_off[] = {
        "关闭", "关", "停止", "熄", "turn off", "stop",
        "关掉", "关一下", "关了吧", "都关了", "都关掉",
        "帮我关闭", "帮我关", "关关", "关起来", "关闭下",
        "关了", "熄灯", "熄掉", "灭掉", "灭了", "关灯",
        "想关", "要关", "断电", "给关了", NULL };

    for (int i = 0; kw_on[i]; i++) {
        if (strcasestr(user_text, kw_on[i])) {
            turn_on = true;
            found_action = true;
            break;
        }
    }
    if (!found_action) {
        for (int i = 0; kw_off[i]; i++) {
            if (strcasestr(user_text, kw_off[i])) {
                turn_on = false;
                found_action = true;
                break;
            }
        }
    }
    if (!found_action) return -1;

    /* ── Safety: mixed actions → LLM fallback ── */
    /* If text contains BOTH on-keywords AND off-keywords
     * (e.g. "打开客厅灯关闭电视"), the fast-path can't reliably
     * split actions per device.  Return -1 to let LLM handle it. */
    {
        bool has_on  = false;
        bool has_off = false;
        for (int i = 0; kw_on[i]; i++) {
            if (strcasestr(user_text, kw_on[i])) { has_on = true; break; }
        }
        for (int i = 0; kw_off[i]; i++) {
            if (strcasestr(user_text, kw_off[i])) { has_off = true; break; }
        }
        if (has_on && has_off) {
            syslog(LOG_INFO,
                "[HomeControl] mixed actions detected, "
                "falling back to LLM\n");
            return -1;
        }
    }

    /* ── Find ALL devices in text ── */
    hc_match_t matches[HC_MAX_MATCHES];
    int match_count = hc_find_all_devices(user_text, matches);

    if (match_count == 0) return -1;

    /* If >1 device with different actions in one sentence (e.g.
     * "打开客厅灯关闭电视"), fall back to LLM for correctness. */
    /* For now we assume single action per command — the action
     * keyword position vs device position isn't reliable enough
     * to split actions without a real parser. */

    /* ── Execute all toggles ── */
    int ok_count = 0;
    int fail_count = 0;
    char ok_names[HC_MAX_MATCHES][64];
    char fail_names[HC_MAX_MATCHES][64];
    int  fail_codes[HC_MAX_MATCHES];

    for (int i = 0; i < match_count; i++) {
        int ret = watch_home_control_toggle_device(
            matches[i].device_index, turn_on);
        if (ret == 0) {
            strncpy(ok_names[ok_count],
                s_device_table[matches[i].device_index].canonical, 63);
            ok_names[ok_count][63] = '\0';
            ok_count++;
        } else {
            strncpy(fail_names[fail_count],
                s_device_table[matches[i].device_index].canonical, 63);
            fail_names[fail_count][63] = '\0';
            fail_codes[fail_count] = ret;
            fail_count++;
        }
    }

    syslog(LOG_INFO, "[HomeControl] fast-path multi: %d/%d ok, %d fail\n",
        ok_count, match_count, fail_count);

    /* ── Build Chinese reply ── */
    size_t buf_len = 512;
    char *r = calloc(1, buf_len);
    if (!r) return -1;

    const char *action_str = turn_on ? "打开" : "关闭";

    if (ok_count > 0 && fail_count == 0) {
        /* All succeeded: "客厅灯、卧室灯已打开" */
        int off = 0;
        for (int i = 0; i < ok_count && off < (int)buf_len - 1; i++) {
            if (i > 0 && i < ok_count - 1)
                off += snprintf(r + off, buf_len - off, "、");
            else if (i > 0 && i == ok_count - 1)
                off += snprintf(r + off, buf_len - off, "和");
            off += snprintf(r + off, buf_len - off, "%s", ok_names[i]);
        }
        snprintf(r + off, buf_len - off, "已%s", action_str);
    } else if (ok_count > 0 && fail_count > 0) {
        /* Partial success */
        int off = snprintf(r, buf_len, "部分成功：");
        for (int i = 0; i < ok_count && off < (int)buf_len - 1; i++) {
            if (i > 0) off += snprintf(r + off, buf_len - off, "、");
            off += snprintf(r + off, buf_len - off, "%s", ok_names[i]);
        }
        off += snprintf(r + off, buf_len - off, "已%s", action_str);
        for (int i = 0; i < fail_count && off < (int)buf_len - 1; i++) {
            const char *e = "失败";
            switch (fail_codes[i]) {
            case -2: e = "WiFi未连接"; break;
            case -3: e = "服务器未找到"; break;
            case -4: e = "命令失败"; break;
            }
            off += snprintf(r + off, buf_len - off, "；%s%s", fail_names[i], e);
        }
    } else {
        /* All failed */
        snprintf(r, buf_len, "%s失败，请检查WiFi和智能家居服务器", action_str);
    }

    *reply = r;
    return 0;
}

/* ── Scene-based interaction (cold/hot → confirm → control) ── */
/* 潮玩模式场景化交互（仅两个场景，其他设备不涉及）：
 *   "我好冷" → 反问"要不要打开地暖？" → 确认 → 地暖开
 *   "我好热" → 反问"要不要打开空调？" → 确认 → 空调开
 * 有状态：反问后下一轮用户文本在此消费（确认/否定/超时/转移话题）。
 * 注：睡觉/卧室灯场景已删除，可能与 skill 冲突。 */

typedef enum {
    HC_SCENE_NONE = 0,
    HC_SCENE_COLD_FLOOR,   /* 地暖 ON  (device index 2) */
    HC_SCENE_HOT_AC,       /* 空调 ON  (device index 1) */
} hc_scene_t;

static hc_scene_t s_pending_scene = HC_SCENE_NONE;
static time_t     s_pending_since = 0;

#define HC_SCENE_PENDING_TIMEOUT_SEC 30

/* Device indexes — keep in sync with s_device_table */
#define HC_DEV_AC       1
#define HC_DEV_FLOOR    2

static const char * const kw_scene_cold[] = {
    "好冷", "有点冷", "太冷", "冷死了", "冻死", NULL };
static const char * const kw_scene_hot[] = {
    "好热", "有点热", "太热", "热死了", "好烫", NULL };
/* 否定词必须先于肯定词匹配（"不要"含"要"、"不好"含"好"） */
static const char * const kw_scene_no[] = {
    "不要", "不用", "不好", "不了", "算了", "别", NULL };
static const char * const kw_scene_yes[] = {
    "要", "好的", "好啊", "行", "可以", "打开", "开", NULL };

static int scene_str_any(const char *text, const char * const *kws)
{
    for (int i = 0; kws[i]; i++) {
        if (strcasestr(text, kws[i])) return 1;
    }
    return 0;
}

int tool_home_control_scene(const char *user_text, char **reply)
{
    if (!user_text || !user_text[0] || !reply) return -1;
    *reply = NULL;

    /* ── 确认轮：有待确认场景时优先消费本轮文本 ── */
    if (s_pending_scene != HC_SCENE_NONE) {
        if (time(NULL) - s_pending_since > HC_SCENE_PENDING_TIMEOUT_SEC) {
            /* 超时放弃 */
            syslog(LOG_INFO, "[HomeControl] scene pending expired\n");
            s_pending_scene = HC_SCENE_NONE;
        } else if (scene_str_any(user_text, kw_scene_no)) {
            s_pending_scene = HC_SCENE_NONE;
            *reply = strdup("好的");
            return *reply ? 0 : -1;
        } else if (scene_str_any(user_text, kw_scene_yes)) {
            hc_scene_t scene = s_pending_scene;
            int dev = -1;
            bool turn_on = false;
            const char *confirm = NULL;
            switch (scene) {
            case HC_SCENE_COLD_FLOOR:
                dev = HC_DEV_FLOOR;
                turn_on = true;
                confirm = "地暖已打开";
                break;
            case HC_SCENE_HOT_AC:
                dev = HC_DEV_AC;
                turn_on = true;
                confirm = "空调已打开";
                break;
            default:
                s_pending_scene = HC_SCENE_NONE;
                return -1;
            }
            s_pending_scene = HC_SCENE_NONE;
            int ret = watch_home_control_toggle_device(dev, turn_on);
            syslog(LOG_INFO,
                "[HomeControl] scene confirm: %s (dev=%d on=%d ret=%d)\n",
                confirm, dev, (int)turn_on, ret);
            *reply = strdup(confirm);
            return *reply ? 0 : -1;
        } else {
            /* 用户转移话题：放弃待确认场景，走正常流程 */
            syslog(LOG_INFO,
                "[HomeControl] scene pending dropped: user said \"%s\"\n",
                user_text);
            s_pending_scene = HC_SCENE_NONE;
        }
    }

    /* ── 场景触发轮 ── */
    hc_scene_t scene = HC_SCENE_NONE;
    const char *ask = NULL;
    if (scene_str_any(user_text, kw_scene_cold)) {
        scene = HC_SCENE_COLD_FLOOR;
        ask = "要不要打开地暖？";
    } else if (scene_str_any(user_text, kw_scene_hot)) {
        scene = HC_SCENE_HOT_AC;
        ask = "要不要打开空调？";
    }
    if (scene == HC_SCENE_NONE) return -1;

    s_pending_scene = scene;
    s_pending_since = time(NULL);
    syslog(LOG_INFO,
        "[HomeControl] scene triggered: \"%s\" → \"%s\"\n",
        user_text, ask);
    *reply = strdup(ask);
    return *reply ? 0 : -1;
}

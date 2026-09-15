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
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include <inttypes.h>

#include "core/agent_loop.h"
#include "core/agent_mem.h"
#include "core/agent_trace.h"
#include "core/context_builder.h"
#include "core/emotion_keywords.h"
#include "core/message_bus.h"
#include "core/session_mgr.h"
#include "llm/llm_cache.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "tools/skill_loader.h"
#include "tools/tool_guard.h"
#include "tools/tool_registry.h"
#include "tools/tool_home_control.h"
#include "tools/tool_web_search.h"
#include "voice/voice_channel.h"
#include "voice/volc_tts.h"
#ifdef CONFIG_AI_AGENT_VOLC_E2E
#include "voice/voice_tts.h"
#include "voice/volc_e2e.h"
#endif
#include "infra/network_manager.h"
#include "agent_compat.h"
#include "agent_config.h"

/* ── External watch app APIs (resolved at link time) ─────────── */

extern uint8_t watch_battery_get_level(void);
extern int watch_volume_get_level(void);
extern int watch_volume_set_level(int level);
extern int watch_volume_step_up(void);
extern int watch_volume_step_down(void);
extern int watch_volume_set_percent(int percent);
extern int watch_volume_get_percent(void);
extern int watch_volume_step_delta(int delta);

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
extern int watch_expression_page_set_face(const char* face_id, int duration_ms);
#endif

/* Volume levels (mirrors volume_control.h) */
#define WATCH_VOLUME_LEVEL_MUTE   0
#define WATCH_VOLUME_LEVEL_MEDIUM 2

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include "cJSON.h"

static const char* TAG = "agent";

#define TOOL_OUTPUT_SIZE (8 * 1024)
#define TOOL_OUTPUT_SIZE_LARGE (16 * 1024)
#define TOOL_OUTPUT_SIZE_MIN (2 * 1024)

/* ── Forward declarations ──────────────────────────────────── */

static char* handle_slash_note(const agent_msg_t* msg);
static char* handle_slash_remind(const agent_msg_t* msg);
static bool llm_call_timed_out(uint32_t latency_ms);

/* ── Timeout message constants ─────────────────────────────── */

#define LLM_TIMEOUT_MSG \
    "请求超时，LLM 响应时间过长。请稍后重试，或尝试简化你的问题。"
#define LLM_TIMEOUT_TASK_COMPLETE_MSG \
    "任务已完成，但生成确认消息超时。"

/* ── Clock-safe elapsed time calculation ───────────────────── */

static inline uint32_t calc_elapsed_ms(const struct timeval* t0,
    const struct timeval* t1)
{
    int32_t sec_diff = (int32_t)(t1->tv_sec - t0->tv_sec);
    int32_t usec_diff = (int32_t)(t1->tv_usec - t0->tv_usec);

    /* Clock went backwards (NTP jump, manual adjustment) */
    if (sec_diff < 0) {
        syslog(LOG_WARNING, "[%s] Clock went backwards, ignoring\n", TAG);
        return 0;
    }

    /* Microsecond borrow */
    if (usec_diff < 0) {
        sec_diff--;
        usec_diff += 1000000;
    }

    if (sec_diff < 0) {
        return 0;
    }

    return (uint32_t)sec_diff * 1000 + (uint32_t)usec_diff / 1000;
}

/* ── Memory pool (pre-allocated tool output buffers) ───────── */

static agent_mem_pool_t s_tool_pool;
static bool s_pool_ready = false;

/* Build OpenAI-format assistant message with tool_calls at the top level */
static void add_assistant_message(cJSON* messages, const llm_response_t* resp)
{
    cJSON* asst_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(asst_msg, "role", "assistant");

    if (resp->text && resp->text_len > 0) {
        cJSON_AddStringToObject(asst_msg, "content", resp->text);
    } else {
        cJSON_AddNullToObject(asst_msg, "content");
    }

    /* Kimi thinking mode: echo back reasoning_content or the API returns 400 */
    if (resp->reasoning_content && resp->reasoning_content[0]) {
        cJSON_AddStringToObject(asst_msg, "reasoning_content",
            resp->reasoning_content);
    }

    if (resp->call_count > 0) {
        cJSON* tool_calls = cJSON_CreateArray();
        for (int i = 0; i < resp->call_count; i++) {
            const llm_tool_call_t* call = &resp->calls[i];
            cJSON* tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", call->id);
            cJSON_AddStringToObject(tc, "type", "function");

            cJSON* func_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(func_obj, "name", call->name);
            cJSON_AddStringToObject(func_obj, "arguments",
                call->input ? call->input : "{}");
            cJSON_AddItemToObject(tc, "function", func_obj);
            cJSON_AddItemToArray(tool_calls, tc);
        }
        cJSON_AddItemToObject(asst_msg, "tool_calls", tool_calls);
    }

    cJSON_AddItemToArray(messages, asst_msg);
}

/* Auto-inject channel/chat_id into cron_add input JSON. */
static char* inject_cron_context(const char* tool_name,
    const char* input_json, const char* channel, const char* chat_id)
{
    if (strcmp(tool_name, "cron_add") != 0) {
        return NULL;
    }
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) {
        return NULL;
    }

    cJSON* root = cJSON_Parse(input_json);
    if (!root) {
        return NULL;
    }

    cJSON_DeleteItemFromObject(root, "channel");
    cJSON_DeleteItemFromObject(root, "chat_id");
    cJSON_AddStringToObject(root, "channel", channel);
    cJSON_AddStringToObject(root, "chat_id", chat_id);

    char* patched = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return patched;
}

/* Parallel tool execution context */
typedef struct {
    const llm_tool_call_t* call;
    char* output;
    size_t output_size;
    bool from_pool;
    char channel[16];
    char chat_id[64];
} tool_task_t;

static void* tool_exec_thread(void* arg)
{
    tool_task_t* t = (tool_task_t*)arg;

    t->output[0] = '\0';
    char* patched = inject_cron_context(
        t->call->name, t->call->input, t->channel, t->chat_id);
    agent_tool_exec_streamed(t->call->name,
        patched ? patched : t->call->input,
        t->output, t->output_size);
    free(patched);
    syslog(LOG_INFO, "[%s] Tool %s result: %d bytes\n",
        TAG, t->call->name, (int)strlen(t->output));
    return NULL;
}

/* Add one role:"tool" message per tool result (OpenAI format).
 * When call_count > 1, all tools run in parallel threads.
 * Uses memory pool for parallel buffers, with heap fallback. */
static void add_tool_result_messages(cJSON* messages,
    const llm_response_t* resp, char* tool_output,
    size_t tool_output_size, const char* msg_channel,
    const char* msg_chat_id)
{
    int n = resp->call_count;

    if (n == 1) {
        const llm_tool_call_t* call = &resp->calls[0];

        syslog(LOG_INFO, "[%s] Tool call: %s args=%.500s\n", TAG,
            call->name, call->input ? call->input : "(null)");
        tool_output[0] = '\0';
        char* patched = inject_cron_context(
            call->name, call->input, msg_channel, msg_chat_id);
        agent_tool_exec_streamed(call->name,
            patched ? patched : call->input,
            tool_output, tool_output_size);
        free(patched);
        syslog(LOG_INFO, "[%s] Tool %s result: %d bytes\n", TAG,
            call->name, (int)strlen(tool_output));

        cJSON* result_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(result_msg, "role", "tool");
        cJSON_AddStringToObject(result_msg, "tool_call_id", call->id);
        cJSON_AddStringToObject(result_msg, "content", tool_output);
        cJSON_AddItemToArray(messages, result_msg);
        return;
    }

    /* Parallel path */
    tool_task_t tasks[AGENT_MAX_TOOL_CALLS];
    pthread_t threads[AGENT_MAX_TOOL_CALLS];
    size_t par_buf_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE_LARGE, TOOL_OUTPUT_SIZE_MIN);

    for (int i = 0; i < n; i++) {
        tasks[i].call = &resp->calls[i];
        strncpy(tasks[i].channel, msg_channel ? msg_channel : "",
            sizeof(tasks[i].channel) - 1);
        strncpy(tasks[i].chat_id, msg_chat_id ? msg_chat_id : "",
            sizeof(tasks[i].chat_id) - 1);

        char* buf = s_pool_ready
            ? agent_pool_acquire(&s_tool_pool)
            : NULL;
        if (buf) {
            tasks[i].output = buf;
            tasks[i].output_size = s_tool_pool.buf_size;
            tasks[i].from_pool = true;
        } else {
            tasks[i].output = calloc(1, par_buf_size);
            tasks[i].output_size = par_buf_size;
            tasks[i].from_pool = false;
        }

        if (!tasks[i].output) {
            threads[i] = 0;
            continue;
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 * 1024);
        if (pthread_create(&threads[i], &attr,
                tool_exec_thread, &tasks[i])
            != 0) {
            syslog(LOG_ERR, "[%s] Failed to spawn thread for tool %s\n",
                TAG, resp->calls[i].name);
            threads[i] = 0;
        }
        pthread_attr_destroy(&attr);
    }

    /* Join and collect results, dedup by tool_call_id */
    char seen_ids[AGENT_MAX_TOOL_CALLS][32];
    int seen_count = 0;

    for (int i = 0; i < n; i++) {
        if (threads[i]) {
            pthread_join(threads[i], NULL);
        }

        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_ids[j], resp->calls[i].id) == 0) {
                dup = true;
                break;
            }
        }

        if (dup) {
            syslog(LOG_WARNING,
                "[%s] Skipping duplicate tool_call_id: %s\n",
                TAG, resp->calls[i].id);
        } else {
            strncpy(seen_ids[seen_count++], resp->calls[i].id,
                sizeof(seen_ids[0]) - 1);

            cJSON* result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "tool");
            cJSON_AddStringToObject(result_msg, "tool_call_id",
                resp->calls[i].id);
            cJSON_AddStringToObject(result_msg, "content",
                (tasks[i].output && tasks[i].output[0])
                    ? tasks[i].output
                    : "{}");
            cJSON_AddItemToArray(messages, result_msg);
        }

        if (tasks[i].from_pool) {
            agent_pool_release(&s_tool_pool, tasks[i].output);
        } else {
            free(tasks[i].output);
        }
    }
}

/* ── Fast path counters ─────────────────────────────────── */

static int s_fast_path_count = 0;
static int s_llm_path_count = 0;

/* speak shortcut: when speak() runs in an emotion skill round,
 * skip the subsequent LLM text-wrap-up round.
 * Non-static — referenced by tool_emotion_speak.c via extern. */
bool s_emotion_tool_ran = false;
char* s_speak_text = NULL;
bool s_face_tool_ran = false;       /* set_face was called — keep emotion face */
bool s_tool_ran = false;            /* any tool actually ran this turn (E2E hybrid routing) */

/* Last text spoken by the speak tool — used to skip the LLM
 * text-wrap-up round after emotion skill execution. */

/* ── Natural language fast path (keyword → direct tool call) ── */

static bool contains_any(const char* text, const char * const * keywords)
{
    for (int i = 0; keywords[i]; i++) {
        if (strcasestr(text, keywords[i])) {
            return true;
        }
    }
    return false;
}

/* Minimal chitchat detection: obvious conversational phrases get no tools,
 * so the LLM can't call set_face/speak/read_file and force the slow agent
 * path — E2E self-chat handles these fast instead.  Conservative: only
 * short, clearly-chitchat phrases return true.
 *
 * Also the E2E direct-route table: time/weather queries are answered
 * natively (and fast) by the E2E server model, so matching phrases
 * shortcut straight to route_e2e instead of waiting for the client LLM
 * to decide on skills/tools.  The NL fast path kw_time intent, the
 * get_weather tool and the weather skill are disabled to match
 * (2026-08-17). */
static bool is_chitchat(const char* text)
{
    if (!text || text[0] == '\0') return true;

    /* Long messages are real requests, not chitchat. */
    if (strlen(text) > 30) return false;

    static const char * const kw_chitchat[] = {
        "你好", "nihao", "hello", "hi", "嗨",
        "谢谢", "thanks", "thank", "谢了",
        "好的", "ok", "行", "可以", "嗯嗯", "知道了", "明白了", "懂了",
        "再见", "拜拜", "bye",
        /* 早上好/早安(早)/晚安 是技能触发词（早晨唤醒/睡前助眠），
         * 判为闲聊会跳过 skill 匹配，故已移除。 */
        "中午好", "下午好", "晚上好", "早啊",
        "你喜欢干什么", "你爱干什么",
        "在吗", "在不在",
        "吃饭了吗", "吃了吗", "吃饭没",
        "干什么呢", "干嘛呢",
        "讲个笑话", "讲笑话", "讲个故事", "讲故事",
        "想你了", "好想你",
        "你多大了", "你几岁了",
        "辛苦了", "没关系", "没事",
        "1+1", "等于多少", "算一下",
        /* 时间/天气查询 → E2E 直连：服务端模型可直接回答且延迟低，
         * 命中即短路 route_e2e，不等客户端 LLM 判定 skill/tool。
         * 不用裸"温度"，避免误吞"空调温度"类家居控制语句。
         * "天怎么样"兜 ASR 把"天气"丢字成"天"的变体
         * （实测出现"武汉今天天怎么样"）。 */
        "几点", "什么时间", "现在时间",
        "周几", "星期几", "礼拜几", "几号", "几月几", "日期",
        "天气", "天怎么样", "气温", "温度多少", "温度是", "多少度",
        "下雨", "下雪", "what time", "what day", "weather",
        NULL };
    for (int i = 0; kw_chitchat[i]; i++)
        if (strcasestr(text, kw_chitchat[i])) return true;
    return false;
}

/* Intent table: each entry maps keywords → tool + args + output size */

typedef struct {
    const char * const * keywords;
    const char* tool_name;
    const char* tool_args;
    size_t output_size;
} nl_intent_t;

/* [DISABLED 2026-08-17] 时间查询改走 E2E 直连：is_chitchat() 表命中
 * 即 route_e2e 播服务端回复；本地 fast path 会丢弃服务端已合成音频
 * 并强制走 agent TTS 慢路径。代码保留以便回退。 */
#if 0
static const char * const kw_time[]       = { "几点了", "什么时间", "what time", "几点钟", "今天周几", "今天几号", "今天日期", "今天星期几", "现在几点", "现在时间", NULL };
#endif
/* 以下 6 个工具接口未实现，暂禁用（2026-08-14）：
 * get_battery / get_heartrate / get_steps /
 * music_pause / music_stop / music_resume
 * 电量查询仍可由下方 kw_battery_nl 扩展路径（watch_battery_get_level）命中。 */
#if 0
static const char * const kw_battery[]    = { "电量多少", "电池电量", "battery", NULL };
static const char * const kw_heartrate[]  = { "心率多少", "heart rate", "心率是多", NULL };
static const char * const kw_steps[]      = { "走了多少步", "今天走了多少", "steps", "步数", NULL };
static const char * const kw_pause[]      = { "暂停音乐", "暂停播放", "pause music", NULL };
static const char * const kw_stop[]       = { "停止播放", "停止音乐", "stop music", NULL };
static const char * const kw_resume[]     = { "继续播放", "resume", NULL };
#endif

static const nl_intent_t s_intents[] = {
#if 0 /* [DISABLED 2026-08-17] 时间查询走 E2E 直连，见 is_chitchat() */
    { kw_time,      "get_current_time", "{}",  256  },
#endif
#if 0 /* 未实现工具，暂禁用 */
    { kw_battery,   "get_battery",      "{}",  512  },
    { kw_heartrate, "get_heartrate",    "{}",  256  },
    { kw_steps,     "get_steps",        "{}",  256  },
    { kw_pause,     "music_pause",      "{}",  256  },
    { kw_stop,      "music_stop",       "{}",  256  },
    { kw_resume,    "music_resume",     "{}",  256  },
#endif
    { NULL,         NULL,               NULL,  0    },
};

static char* handle_nl_fast_path(const char* text)
{
    /* Table-driven: scan intents, first match wins */
    for (int i = 0; s_intents[i].keywords; i++) {
        if (contains_any(text, s_intents[i].keywords)) {
            char* reply = calloc(1, s_intents[i].output_size);
            if (reply) {
                tool_registry_execute(s_intents[i].tool_name,
                    s_intents[i].tool_args, reply,
                    s_intents[i].output_size);
            }
            if (reply) {
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: %s (fast=%d llm=%d)\n",
                    TAG, s_intents[i].tool_name,
                    s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }
    }

    /* ── Smart Home Control fast path ─────────────────────────── */
    /*   Intercept commands like "打开客厅灯" / "关闭空调" etc.
     *   Extract device + action, call home_control directly,
     *   return natural-language reply — bypass LLM entirely. */
    {
        char *hc_reply = NULL;
        if (tool_home_control_fast_toggle(text, &hc_reply) == 0
            && hc_reply) {
            s_fast_path_count++;
            syslog(LOG_INFO,
                "[%s] NL fast path: home_control_toggle "
                "(fast=%d llm=%d)\n",
                TAG, s_fast_path_count, s_llm_path_count);
            return hc_reply;
        }
    }

    /* ── Home Control scene interaction ────────────────────────── */
    /*   "我好冷" → "要不要打开地暖？"；"我好热" → "要不要打开空调？"。
     *   反问后下一轮确认/否定在此消费；仅这两个场景，其他设备不涉及。 */
    {
        char *scene_reply = NULL;
        if (tool_home_control_scene(text, &scene_reply) == 0
            && scene_reply) {
            s_fast_path_count++;
            syslog(LOG_INFO,
                "[%s] NL fast path: home_control_scene "
                "(fast=%d llm=%d)\n",
                TAG, s_fast_path_count, s_llm_path_count);
            return scene_reply;
        }
    }

    /* ── Battery query fast path ───────────────────────────────── */
    {
        static const char * const kw_battery_nl[] = {
            "电量", "电池", "还剩多少电", "battery",
            "还有电吗", "电量多少", "查看电量", "剩多少电",
            "还有多少电", "电量剩余", "当前电量", "现在电量",
            "看下电量", "电量还剩", "电池还有", "多少电",
            "没电了", "快没电", "充电", "电够吗", "电还够",
            "电量够", "够不够电", "余电", "剩余电量",
            "电压", "电力", "电源", NULL };
        if (contains_any(text, kw_battery_nl)) {
            uint8_t level = watch_battery_get_level();
            char* r = calloc(1, 128);
            if (r) {
                if (level <= 10)
                    snprintf(r, 128, "电量 %u%%，快没电了，请充电", level);
                else if (level <= 20)
                    snprintf(r, 128, "电量 %u%%，偏低，建议充电", level);
                else
                    snprintf(r, 128, "电量 %u%%", level);
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: battery=%u%% (fast=%d llm=%d)\n",
                    TAG, level, s_fast_path_count, s_llm_path_count);
                return r;
            }
        }
    }

    /* ── Volume control fast path ───────────────────────────────── */
    {
        static const char * const kw_vol_up[] = {
            "音量调大", "音量增大", "大声一点", "声音大点",
            "音量加", "调大音量", "volume up", "音量调高",
            "声音太小", "太小声", "大点声", "大点声音",
            "再大声", "调大点", "加大音量", "提高音量",
            "声音调大", "音量提高", "声音开大", "开大点声",
            "响一点", "再响点", "加点音量", "音量不够",
            "听不见", "听不清", "声音好小", "太轻了",
            "大一点", "大点儿", NULL };
        static const char * const kw_vol_down[] = {
            "音量调小", "音量减小", "小声一点", "声音小点",
            "音量减", "调小音量", "volume down", "音量调低",
            "声音太大", "太大声", "小点声", "小点声音",
            "再小声", "调小点", "减小音量", "降低音量",
            "声音调小", "音量降低", "声音开小", "开小点声",
            "太吵了", "吵死了", "好吵", "轻一点", "太响了",
            "震耳朵", "小一点", "小点儿", NULL };
        /* [REMOVED 2026-08-17] 语音"静音"表已删除：语音机器人把扬声器
         * 音量设 0 后，确认语与后续所有 TTS 回复均不可闻（自我失聪）；
         * 且"取消静音/解除静音/不要静音"都含"静音"子串，会先命中
         * 本表反而触发静音。"静音/闭嘴/别说话/安静"等交给 LLM 人格化
         * 应答（端侧 kw_quiet 命中时配 cool 表情）。禁麦走 BOOT 短按。 */
        static const char * const kw_vol_unmute[] = {
            "取消静音", "解除静音", "打开声音", "恢复音量",
            "unmute", "打开音量", "有声音", "出声音",
            "恢复声音", "开启声音", "不要静音", "声音回来",
            "可以说话了", NULL };
        static const char * const kw_vol_query[] = {
            "当前音量", "音量多少", "音量是", "现在音量",
            "volume level", "查看音量", "音量多大", "声音多大",
            "音量几级", "现在声音", "音量大小", "音量怎么样",
            "几级音量", NULL };

        /* Set volume to specific percent: "音量调到80%", "音量设为百分之50" */
        static const char * const kw_vol_set[] = {
            "音量调到", "音量设为", "音量设置", "调音量到",
            "把音量调到", "音量调到第", "调到", "设置音量",
            "音量调成", "音量改到", "音量变成", "调成",
            "调到百分之", "设为百分之", "音量百分之", NULL };

        /* Max/min BEFORE kw_vol_set: "调到最大" would otherwise hit the
         * numeric set branch and find no digits. */
        static const char * const kw_vol_max[] = {
            "调到最大", "调至最大", "最大声", "声音最大",
            "音量最大", NULL };
        static const char * const kw_vol_min[] = {
            "调到最小", "调至最小", "最小声", "声音最小",
            "音量最小", NULL };

        if (contains_any(text, kw_vol_max)) {
            watch_volume_set_percent(100);
            s_fast_path_count++;
            return strdup("音量已调到最大");
        }
        if (contains_any(text, kw_vol_min)) {
            watch_volume_set_percent(10);
            s_fast_path_count++;
            return strdup("音量已调到最小");
        }

        if (contains_any(text, kw_vol_set)) {
            /* Extract number — prefer percent (10-100), fallback to level (1-5) */
            int num = -1;
            const char *p = text;
            while (*p) {
                if (*p >= '0' && *p <= '9') {
                    num = (int)strtol(p, (char**)&p, 10);
                    break;
                }
                p++;
            }
            /* Detect "百分之N" pattern → percent */
            bool is_pct = (strstr(text, "百分之") != NULL ||
                           strstr(text, "%")     != NULL);
            if (num > 0) {
                if (is_pct || num > 5) {
                    /* Treat as percentage: "调到80%" or "百分之80" */
                    if (num < 10) num = 10;
                    if (num > 100) num = 100;
                    int ret = watch_volume_set_percent(num);
                    char* r = calloc(1, 128);
                    if (r) {
                        if (ret == 0) {
                            snprintf(r, 128, "音量已调到 %d%%", num);
                        } else {
                            snprintf(r, 128, "音量调节失败");
                        }
                        s_fast_path_count++;
                        return r;
                    }
                } else {
                    /* Legacy: 1-5 level mapping.  Floor at 1 — voice must
                     * not reach level 0 (= speaker mute, replies would be
                     * inaudible).  Reply reports the actual level. */
                    int level = num - 1;
                    if (level < 1) level = 1;
                    if (level > 4) level = 4;
                    int ret = watch_volume_set_level(level);
                    char* r = calloc(1, 128);
                    if (r) {
                        if (ret == 0) {
                            const char * const labels[] =
                                {"静音","低","中","高","最大"};
                            snprintf(r, 128, "音量已调到 %d 级（%s）",
                                level + 1, labels[level]);
                        } else {
                            snprintf(r, 128, "音量调节失败");
                        }
                        s_fast_path_count++;
                        return r;
                    }
                }
            }
        }

        if (contains_any(text, kw_vol_up)) {
            int new_pct = watch_volume_step_delta(10);
            char* r = calloc(1, 128);
            if (r) {
                if (new_pct >= 0) {
                    snprintf(r, 128, "音量已调大，当前 %d%%", new_pct);
                } else {
                    snprintf(r, 128, "音量调节失败");
                }
                s_fast_path_count++;
                return r;
            }
        }

        if (contains_any(text, kw_vol_down)) {
            int new_pct = watch_volume_step_delta(-10);
            char* r = calloc(1, 128);
            if (r) {
                if (new_pct >= 0) {
                    snprintf(r, 128, "音量已调小，当前 %d%%", new_pct);
                } else {
                    snprintf(r, 128, "音量调节失败");
                }
                s_fast_path_count++;
                return r;
            }
        }

        /* 仅保留取消静音：音量被外部途径（UI 等）设为 0 后的语音恢复
         * 出口——恢复到 50%，确认语可闻。语音路径不再提供静音。 */
        if (contains_any(text, kw_vol_unmute)) {
            watch_volume_set_percent(50);
            s_fast_path_count++;
            return strdup("已取消静音，恢复到 50%");
        }

        if (contains_any(text, kw_vol_query)) {
            int pct = watch_volume_get_percent();
            char* r = calloc(1, 128);
            if (r) {
                if (pct >= 0) {
                    snprintf(r, 128, "当前音量：%d%%", pct);
                } else {
                    snprintf(r, 128, "无法获取音量信息");
                }
                s_fast_path_count++;
                return r;
            }
        }
    }

    /* Skill list (special: compact format for voice) */
    static const char * const kw_skill[] = {
        "技能列表", "有什么技能", "list skill", NULL };
    if (contains_any(text, kw_skill)) {
        s_fast_path_count++;
        return strdup("我有19个技能，包括：陪伴、安抚、放松引导、睡前助眠、摇晃互动、跌倒检测、早晨唤醒、短休提醒、天气、新闻、翻译等。说具体需求就行。");
    }

    /* Fixed phrases — device-specific info, kept client-side */
    static const char * const kw_who[] = {
        "你是谁", "你叫什么", "你的名字", "who are you", NULL };
    if (contains_any(text, kw_who)) {
        s_fast_path_count++;
        return strdup("我是小潮，你的桌面陪伴机器人。");
    }

    static const char * const kw_cando[] = {
        "你能做什么", "有什么功能", "可用工具", "功能介绍", "what can you do", NULL };
    if (contains_any(text, kw_cando)) {
        s_fast_path_count++;
        return strdup("陪你聊天、播音乐、设提醒、查天气，感知触摸和摇晃。直接说需求我就做。");
    }

    /* Built-in audio effects — keyword match, extract numeric ID */
    {
        static const char * const kw_play_audio[] = {
            "播放音效", "音效", "白噪音", "白噪声", "雨声", "海浪声", "轻音乐",
            "摇篮曲", "风声", "提示音", "闹铃", NULL };
        if (contains_any(text, kw_play_audio)) {
            /* Try to extract a numeric ID from the text */
            int id = 0;
            const char *p = text;
            while (*p) {
                if (*p >= '0' && *p <= '9') {
                    id = (int)strtol(p, (char**)&p, 10);
                    break;
                }
                p++;
            }
            /* Map common keywords to IDs if no explicit number */
            if (id == 0) {
                if (strstr(text, "雨声"))       id = 12;
                else if (strstr(text, "海浪"))   id = 11;
                else if (strstr(text, "风声"))   id = 13;
                else if (strstr(text, "轻音乐")) id = 4;
                else if (strstr(text, "摇篮"))   id = 14;
                else if (strstr(text, "白噪"))   id = 14;
                else if (strstr(text, "提示音")) id = 10;
                else if (strstr(text, "闹铃"))   id = 1;
            }
            if (id >= 1 && id <= 14) {
                char args[64];
                snprintf(args, sizeof(args), "{\"audio_id\":%d}", id);
                char* reply = calloc(1, 256);
                if (reply) {
                    int rc = tool_registry_execute("play_audio", args, reply, 256);
                    s_fast_path_count++;
                    syslog(LOG_INFO, "[%s] NL fast path: play_audio id=%d rc=%d\n",
                        TAG, id, rc);
                    if (rc == OK) {
                        /* Audio is playing — no TTS reply, skip LLM */
                        syslog(LOG_INFO, "[%s] NL fast path: play_audio id=%d OK\n",
                            TAG, id);
                        reply[0] = '\0';  /* empty → continue with no TTS */
                    } else {
                        snprintf(reply, 256, "抱歉，音效文件找不到，请检查SD卡");
                    }
                    return reply;
                }
            }
        }
    }

#if 0 /* [DISABLED] 点歌 fast-path：music_search/music_play 未注册，无法执行 */
    /* Music play (special: needs keyword extraction) */
    static const char * const kw_play[] = { "播放", "play ", "放一首", NULL };
    if (contains_any(text, kw_play)) {
        const char* kw = NULL;
        const char* p = strstr(text, "播放");
        if (p) { p += strlen("播放"); while (*p == ' ') p++; if (*p) kw = p; }
        if (!kw) {
            p = strcasestr(text, "play ");
            if (p) { p += 5; while (*p == ' ') p++; if (*p) kw = p; }
        }
        if (kw) {
            int klen = 0;
            while (kw[klen] && kw[klen] != '"' && kw[klen] != '\\' && klen < 100)
                klen++;
            char input[256];
            snprintf(input, sizeof(input),
                "{\"keyword\":\"%.*s\"}", klen, kw);
            char search_result[4096];
            memset(search_result, 0, sizeof(search_result));
            tool_registry_execute("music_search", input, search_result, sizeof(search_result));

            /* Auto-play first result if search succeeded */
            char* reply = NULL;
            cJSON* sr = cJSON_Parse(search_result);
            cJSON* songs = sr ? cJSON_GetObjectItem(sr, "songs") : NULL;
            cJSON* first = songs ? cJSON_GetArrayItem(songs, 0) : NULL;
            cJSON* url = first ? cJSON_GetObjectItem(first, "url") : NULL;
            if (url && cJSON_IsString(url) && url->valuestring[0]) {
                char play_input[512];
                snprintf(play_input, sizeof(play_input),
                    "{\"url\":\"%s\"}", url->valuestring);
                char play_result[256];
                memset(play_result, 0, sizeof(play_result));
                tool_registry_execute("music_play", play_input, play_result, sizeof(play_result));

                /* Build reply with song info */
                cJSON* name = cJSON_GetObjectItem(first, "name");
                cJSON* artist = cJSON_GetObjectItem(first, "artist");
                const char* sname = (name && cJSON_IsString(name)) ? name->valuestring : "未知";
                const char* sartist = (artist && cJSON_IsString(artist)) ? artist->valuestring : "未知";

                /* Check if play actually succeeded */
                bool play_ok = (strstr(play_result, "error") == NULL && play_result[0] != '\0');
                reply = calloc(1, 512);
                if (reply) {
                    if (play_ok) {
                        snprintf(reply, 512, "正在播放: %s - %s", sname, sartist);
                    } else {
                        snprintf(reply, 512, "找到了 %s - %s，但播放失败: %s",
                            sname, sartist, play_result);
                    }
                }
            } else {
                reply = calloc(1, 4096);
                if (reply)
                    strncpy(reply, search_result, 4095);
            }
            cJSON_Delete(sr);

            if (reply) {
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: music_search (fast=%d llm=%d)\n",
                    TAG, s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }
    }
#endif

    return NULL;
}

/* ── Handle slash commands + NL fast path (bypass LLM) ─── */

static char* handle_slash_command(agent_msg_t* msg)
{
    if (!msg->content) {
        return NULL;
    }

    /* Phase 1: slash commands (highest priority) */
    if (msg->content[0] != '/') {
        return NULL; /* NL fast path handled separately after injection check */
    }

    char* reply = NULL;

    if (strncmp(msg->content, "/help", 5) == 0) {
        reply = strdup(
            "AI Agent 快捷命令：\n\n"
            "/help           显示本帮助\n"
            "/time           当前时间\n"
            "/weather 城市   实时天气\n"
            "/news [关键词]  最新新闻\n"
            "/memory         查看长期记忆\n"
            "/note 内容      记一条笔记\n"
            "/remind 秒数 内容  设提醒\n"
            "/skill          列出技能\n"
            "/translate 文本 翻译\n"
            "/daily          每日简报\n\n"
            "也可以直接用自然语言对话，我会自动调用工具。");
    } else if (strncmp(msg->content, "/time", 5) == 0) {
        reply = calloc(1, 256);
        if (reply) {
            tool_registry_execute("get_current_time", "{}", reply, 256);
        }
    } else if (strncmp(msg->content, "/weather", 8) == 0) {
        const char* arg = msg->content + 8;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = "Beijing";
        }
        char input[256];
        snprintf(input, sizeof(input),
            "{\"location\":\"%s\"}", arg);
        reply = calloc(1, 4096);
        if (reply) {
            /* get_weather 已停用注册（天气走 E2E 直连），/weather 调试
             * 命令保留：绕过注册表直接调实现。 */
            tool_get_weather_execute(input, reply, 4096);
        }
    } else if (strncmp(msg->content, "/news", 5) == 0) {
        const char* arg = msg->content + 5;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = "today";
        }
        char input[256];
        snprintf(input, sizeof(input),
            "{\"query\":\"%s\",\"top_headlines\":true}", arg);
        reply = calloc(1, 8192);
        if (reply) {
            tool_registry_execute("news_search", input, reply, 8192);
        }
    } else if (strncmp(msg->content, "/memory", 7) == 0) {
        char mem_path[256];
        snprintf(mem_path, sizeof(mem_path),
            "{\"path\":\"%s/memory/MEMORY.md\"}", AGENT_DATA_DIR);
        reply = calloc(1, 4096);
        if (reply) {
            int r = tool_registry_execute(
                "read_file", mem_path, reply, 4096);
            if (r != OK || !reply[0]) {
                snprintf(reply, 4096, "暂无长期记忆。");
            }
        }
    } else if (strncmp(msg->content, "/note ", 6) == 0) {
        reply = handle_slash_note(msg);
    } else if (strncmp(msg->content, "/remind ", 8) == 0) {
        reply = handle_slash_remind(msg);
    } else if (strncmp(msg->content, "/skill", 6) == 0) {
        char skills_buf[2048];
        size_t slen = skill_loader_build_summary(
            skills_buf, sizeof(skills_buf));
        reply = (slen > 0)
            ? strdup(skills_buf)
            : strdup("暂无已加载的技能。");
    } else if (strncmp(msg->content, "/translate ", 11) == 0) {
        /* Rewrite content to pass through LLM */
        char* nc = malloc(strlen(msg->content) + 64);
        if (nc) {
            snprintf(nc, strlen(msg->content) + 64,
                "请翻译以下内容（中英互译）：%s", msg->content + 11);
            free(msg->content);
            msg->content = nc;
        }
    } else if (strncmp(msg->content, "/daily", 6) == 0) {
        char* nc = strdup(
            "请给我一份今日简报，包括：当前时间、今日天气、"
            "最新新闻摘要。");
        if (nc) {
            free(msg->content);
            msg->content = nc;
        }
    }

    if (reply) {
        s_fast_path_count++;
    }
    return reply;
}

/* /note sub-handler */
static char* handle_slash_note(const agent_msg_t* msg)
{
    const char* note = msg->content + 6;
    time_t now = time(NULL);
    struct tm tm_now;
    time_t local_epoch = now + 8 * 3600;

    gmtime_r(&local_epoch, &tm_now);

    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", &tm_now);

    char path[256];
    snprintf(path, sizeof(path), "%s/memory/daily/%s.md",
        AGENT_DATA_DIR, date_str);

    char input[4096];
    snprintf(input, sizeof(input),
        "{\"path\":\"%s\",\"content\":\"%s %s\\n- %s %s\\n\"}",
        path, "# ", date_str, time_str, note);

    char* reply = calloc(1, 512);
    if (reply) {
        tool_registry_execute("write_file", input, reply, 512);
        snprintf(reply, 512, "已记录：%s", note);
    }
    return reply;
}

/* /remind sub-handler */
static char* handle_slash_remind(const agent_msg_t* msg)
{
    int secs = 0;
    char remind_msg[512];

    remind_msg[0] = '\0';
    if (sscanf(msg->content + 8, "%d %511[^\n]",
            &secs, remind_msg)
        < 1) {
        return NULL;
    }
    if (!remind_msg[0]) {
        strncpy(remind_msg, "提醒时间到", sizeof(remind_msg) - 1);
    }

    char input[1024];
    snprintf(input, sizeof(input),
        "{\"name\":\"remind\",\"schedule_type\":\"at\","
        "\"at_epoch\":%lld,\"message\":\"%s\","
        "\"channel\":\"%s\",\"chat_id\":\"%s\"}",
        (long long)(time(NULL) + secs), remind_msg,
        msg->channel, msg->chat_id);

    char* reply = calloc(1, 512);
    if (reply) {
        tool_registry_execute("cron_add", input, reply, 512);
        snprintf(reply, 512, "好的，%d 秒后提醒你：%s",
            secs, remind_msg);
    }
    return reply;
}

/* ── Extracted: handle vision message ──────────────────────── */

static char* handle_vision_message(agent_msg_t* msg)
{
    if (!msg->image_b64 || !msg->image_b64[0]) {
        return NULL;
    }

    syslog(LOG_INFO,
        "[%s] Vision message detected, calling llm_chat_vision\n", TAG);

    size_t vis_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE, TOOL_OUTPUT_SIZE_MIN);
    char* vision_resp = calloc(1, vis_size);

    if (!vision_resp) {
        free(msg->image_b64);
        msg->image_b64 = NULL;
        return NULL;
    }

    const char* prompt = (msg->content && msg->content[0])
        ? msg->content
        : AGENT_VISION_DEFAULT_PROMPT;
    int err = llm_chat_vision(
        prompt, msg->image_b64, NULL, vision_resp, vis_size);

    free(msg->image_b64);
    msg->image_b64 = NULL;

    if (err == OK && vision_resp[0]) {
        return vision_resp;
    }

    free(vision_resp);
    return NULL;
}

/* ── Extracted: strip leaked tool-call XML markup ─────────── */

static char* strip_tool_call_markup(char* text)
{
    if (!text) {
        return NULL;
    }

    int dirty = 0;
    if (strstr(text, "<tool_call>") || strstr(text, ":tool_call>")) {
        dirty = 1;
    }
    if (!dirty) {
        return text;
    }

    syslog(LOG_WARNING,
        "[%s] Force-finish text contains tool-call markup, stripping\n",
        TAG);

    size_t len = strlen(text);
    char* clean = calloc(1, len + 1);
    if (!clean) {
        return text;
    }

    const char* r = text;
    char* w = clean;

    while (*r) {
        const char* ts = NULL;
        const char* te = NULL;

        const char* plain = strstr(r, "<tool_call>");
        const char* ns = strstr(r, ":tool_call>");
        const char* ns_open = NULL;

        if (ns) {
            ns_open = ns;
            while (ns_open > r && *(ns_open - 1) != '<') {
                ns_open--;
            }
            if (ns_open > r) {
                ns_open--;
            } else {
                ns_open = NULL;
            }
        }

        if (plain && (!ns_open || plain <= ns_open)) {
            ts = plain;
            te = strstr(ts + 11, "</tool_call>");
            if (te) {
                te += 12;
            }
        } else if (ns_open) {
            ts = ns_open;
            size_t plen = (size_t)(ns - (ts + 1));
            if (plen > 0 && plen < 32) {
                char ctag[64];
                snprintf(ctag, sizeof(ctag), "</%.*s:tool_call>",
                    (int)plen, ts + 1);
                te = strstr(ts, ctag);
                if (te) {
                    te += strlen(ctag);
                }
            }
        }

        if (ts && te) {
            size_t prefix = (size_t)(ts - r);
            memcpy(w, r, prefix);
            w += prefix;
            r = te;
        } else {
            size_t rest = strlen(r);
            memcpy(w, r, rest);
            w += rest;
            break;
        }
    }
    *w = '\0';

    free(text);
    if (clean[0] == '\0' || strspn(clean, " \t\r\n") == strlen(clean)) {
        free(clean);
        return strdup(
            "抱歉，这个任务比较复杂，"
            "我已经收集了一些信息但未能完成全部步骤。"
            "请尝试拆分成更小的问题再问我。");
    }
    return clean;
}

/* ── Extracted: force finish when iteration limit reached ─── */

static char* force_finish_reply(const char* system_prompt,
    cJSON* messages)
{
    syslog(LOG_WARNING,
        "[%s] Tool iteration limit (%d) reached, forcing finish\n",
        TAG, AGENT_AI_AGENT_MAX_TOOL_ITER);

    cJSON* hint = cJSON_CreateObject();
    cJSON_AddStringToObject(hint, "role", "system");
    cJSON_AddStringToObject(hint, "content",
        "You have used all available tool iterations. "
        "Do NOT call any more tools. Summarize what you have "
        "learned so far and reply to the user in plain text now.");
    cJSON_AddItemToArray(messages, hint);

    llm_response_t resp;
    char* result = NULL;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int err = llm_chat_tools(system_prompt, messages, NULL, &resp);
    gettimeofday(&t1, NULL);
    uint32_t ms = calc_elapsed_ms(&t0, &t1);
    bool timed_out = llm_call_timed_out(ms);

    if (err == OK && !timed_out && resp.text && resp.text_len > 0) {
        result = strdup(resp.text);
    }
    llm_response_free(&resp);

    if (!result && timed_out) {
        result = strdup(LLM_TIMEOUT_MSG);
    }

    return strip_tool_call_markup(result);
}

/* ── Emotion hint injection ──────────────────────────────────── */
/* Scans user text for emotion keywords and returns a strong LLM hint.
 * Skill-matched messages skip this — the skill itself specifies faces.
 * Returns NULL if no keyword matches or a skill already handled it. */

#define EMOTION_HINT_MAX 3   /* at most this many triggers */

typedef struct {
    const char* face_id;
    const char* hint;      /* injected as a system message before LLM call */
    const char * const * keywords; /* NULL-terminated keyword array */
} emotion_trigger_t;

/* Keyword arrays live in emotion_keywords.h (shared by the client-side
 * face path, emotion_face_for_text, and the classic-mode hint path,
 * build_emotion_hint). */
static const emotion_trigger_t s_emotion_triggers[] = {
    { "shy",      "User is complimenting you. Call set_face shy and reply warmly.",
      kw_compliment },
    { "love",     "User seems stressed or sad. Call set_face love and comfort them.",
      kw_stressed },
    { "peaceful", "User seems tired or low-energy. Call set_face peaceful and be gentle.",
      kw_tired },
    { "proud",    "User completed a task. Call set_face proud and celebrate.",
      kw_done },
    { "happy",    "User is encouraging you. Call set_face happy and show appreciation.",
      kw_encourage },
    { "confused", "User says they didn't understand. Call set_face confused and ask them to repeat.",
      kw_confused_user },
    { "cool",     "User wants quiet companionship. Call set_face cool and stay with them quietly.",
      kw_quiet },
    { "excited",  "User is playing interactively. Call set_face excited and join in.",
      kw_playful },
};

/* Return the first emotion face_id matching the text, or NULL.  Since the
 * L1 unification this is the ONLY face source outside of skill-driven
 * set_face tool calls: apply_client_emotion_face() consults it on every
 * route (chitchat shortcut, route_e2e, route_agent, non-hybrid turns).
 * Skill turns keep their own set_face tool (guarded by s_face_tool_ran
 * in the helper below). */
static const char* emotion_face_for_text(const char* text)
{
    if (!text || !text[0]) return NULL;

    size_t n = sizeof(s_emotion_triggers) / sizeof(s_emotion_triggers[0]);
    for (size_t t = 0; t < n; t++) {
        for (int k = 0; s_emotion_triggers[t].keywords[k]; k++) {
            if (strstr(text, s_emotion_triggers[t].keywords[k])) {
                return s_emotion_triggers[t].face_id;
            }
        }
    }
    return NULL;
}

/* ── L1 unified client-side emotion face ──────────────────────
 * All non-skill expressions are decided on-device by the keyword
 * table above — the LLM no longer receives a face-selection prompt
 * section.  Skill-specified set_face tool calls are the only
 * exception: when one ran this turn (s_face_tool_ran), it owns the
 * face together with its own duration semantics.
 *
 * Called once per turn from all routing paths; applies a persistent
 * (duration=0) face and suppresses the "speaking" face during TTS. */
static void apply_client_emotion_face(const char* text)
{
    if (s_face_tool_ran) {
        return; /* this turn's LLM/skill set_face owns the face */
    }

    const char* face = emotion_face_for_text(text);
    if (!face) {
        return;
    }

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
    syslog(LOG_INFO, "[%s] emotion face: %s\n", TAG, face);
    watch_expression_page_set_face(face, 0);
    voice_channel_suppress_speaking_face(true);
#else
    (void)face;
#endif
}

/* Match user text against triggers, return the first-matching hint.
 * Each trigger key must appear ≤ EMOTION_HINT_MAX times (first-match). */
static char* build_emotion_hint(const char* text, int* out_count)
{
    if (!text || !text[0]) return NULL;

    char* hints[EMOTION_HINT_MAX];
    int   n = 0;

    size_t n_triggers = sizeof(s_emotion_triggers) / sizeof(s_emotion_triggers[0]);
    for (size_t t = 0; t < n_triggers && n < EMOTION_HINT_MAX; t++) {
        for (int k = 0; s_emotion_triggers[t].keywords[k]; k++) {
            if (strstr(text, s_emotion_triggers[t].keywords[k])) {
                hints[n++] = (char*)s_emotion_triggers[t].hint;
                break; /* one match per trigger category */
            }
        }
    }

    if (n == 0) return NULL;

    /* Build combined hint */
    size_t total = 64; /* overhead */
    for (int i = 0; i < n; i++) total += strlen(hints[i]) + 2;
    char* buf = calloc(1, total);
    if (!buf) return NULL;

    size_t off = 0;
    off += snprintf(buf + off, total - off,
        "[EMOTION HINT] ");
    for (int i = 0; i < n; i++) {
        off += snprintf(buf + off, total - off,
            "%s%s", hints[i], (i + 1 < n) ? " " : "");
    }
    *out_count = n;
    return buf;
}

/* ── Extracted: dispatch response to outbound bus ─────────── */

static void dispatch_response(const agent_msg_t* msg,
    char* final_text)
{
    if (final_text && final_text[0]) {
        session_append(msg->chat_id, "user", msg->content);
        session_append(msg->chat_id, "assistant", final_text);

        /* Voice-first: speak ALL agent responses through TTS */
        voice_channel_speak(final_text);

        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        out.content = final_text;
        if (message_bus_push_outbound(&out) != OK) {
            free(final_text);
        }
    } else {
        free(final_text);
        const char* err_text = "抱歉，我现在无法回答，请稍后再试。";
        /* Speak the error so the voice state machine
         * (PROCESSING → SPEAKING → LISTENING) is reset. Otherwise the
         * voice conversation thread blocks forever waiting to resume
         * when the LLM produced no reply (e.g. no available backend). */
        voice_channel_speak(err_text);
        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        out.content = strdup(err_text);
        if (out.content) {
            if (message_bus_push_outbound(&out) != OK) {
                free(out.content);
            }
        }
    }
}

/* ── ReAct tool-calling loop ──────────────────────────────── */

static const char * const s_working_phrases[] = {
    "正在思考中...",
    "稍等，处理中...",
    "让我查一下...",
    "正在分析...",
    "马上好...",
};
#define WORKING_PHRASE_COUNT \
    ((int)(sizeof(s_working_phrases) / sizeof(s_working_phrases[0])))

/* Send a "working" status message on the first iteration.
 * Skip for feishu (has its own typing indicator) and voice
 * (TTS synthesis of a status phrase wastes time and memory). */
static void send_working_status(const agent_msg_t* msg, int iteration)
{
    if (iteration != 0) {
        return;
    }
    if (strcmp(msg->channel, AGENT_CHAN_FEISHU) == 0
        || strcmp(msg->channel, AGENT_CHAN_VOICE) == 0
#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
        || strcmp(msg->channel, AGENT_CHAN_QUICKAPP) == 0
#endif
        || strcmp(msg->channel, AGENT_CHAN_WEIXIN) == 0) {
        return;
    }

    agent_msg_t status = { 0 };

    strncpy(status.channel, msg->channel, sizeof(status.channel) - 1);
    strncpy(status.chat_id, msg->chat_id, sizeof(status.chat_id) - 1);
    status.content = strdup(
        s_working_phrases[(unsigned)rand() % WORKING_PHRASE_COUNT]);
    if (status.content) {
        if (message_bus_push_outbound(&status) != OK) {
            free(status.content);
        }
    }
}

/* Check for duplicate tool calls. Returns true if loop should break. */
static bool check_tool_dup(const llm_response_t* resp,
    char* prev_sig, int* dup_count,
    char* prev_name, int* name_repeat)
{
    if (resp->call_count != 1) {
        prev_sig[0] = '\0';
        *dup_count = 0;
        prev_name[0] = '\0';
        *name_repeat = 0;
        return false;
    }

    /* Exact match (name + args) */
    char cur_sig[512];

    snprintf(cur_sig, sizeof(cur_sig), "%s|%.400s",
        resp->calls[0].name,
        resp->calls[0].input ? resp->calls[0].input : "");

    if (strcmp(cur_sig, prev_sig) == 0) {
        (*dup_count)++;
        if (*dup_count >= 2) {
            syslog(LOG_WARNING,
                "[%s] Duplicate tool call detected (%s), breaking\n",
                TAG, resp->calls[0].name);
            return true;
        }
    } else {
        *dup_count = 0;
    }
    strncpy(prev_sig, cur_sig, 511);
    prev_sig[511] = '\0';

    /* Name-only repeat detection */
    if (strcmp(resp->calls[0].name, prev_name) == 0) {
        (*name_repeat)++;
        if (*name_repeat >= AGENT_TOOL_NAME_REPEAT_MAX) {
            syslog(LOG_WARNING,
                "[%s] Tool name repeat limit (%s called %d times)\n",
                TAG, resp->calls[0].name, *name_repeat + 1);
            return true;
        }
    } else {
        *name_repeat = 0;
    }
    strncpy(prev_name, resp->calls[0].name, 63);
    prev_name[63] = '\0';

    return false;
}

/* Check if an LLM call exceeded the watchdog timeout.
 * Returns true when latency_ms exceeds AGENT_LLM_TIMEOUT_SEC. */
static bool llm_call_timed_out(uint32_t latency_ms)
{
    return latency_ms > (uint32_t)AGENT_LLM_TIMEOUT_SEC * 1000;
}

/* Handle TASK_COMPLETE: inject hint and do one final LLM call.
 * If the LLM call times out, *out_timed_out is set to true. */
static char* handle_task_complete(const char* sys_prompt, cJSON* messages,
    bool* out_timed_out)
{
    cJSON* hint = cJSON_CreateObject();

    cJSON_AddStringToObject(hint, "role", "system");
    cJSON_AddStringToObject(hint, "content",
        "The task has been completed successfully. "
        "Do NOT call any more tools. "
        "Reply to the user now confirming what was done.");
    cJSON_AddItemToArray(messages, hint);

    llm_response_t final_resp;
    char* result = NULL;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int err = llm_chat_tools(sys_prompt, messages, NULL, &final_resp);
    gettimeofday(&t1, NULL);
    uint32_t ms = calc_elapsed_ms(&t0, &t1);
    bool timed_out = llm_call_timed_out(ms);

    if (err == OK && !timed_out
        && final_resp.text && final_resp.text_len > 0) {
        result = strdup(final_resp.text);
    }
    llm_response_free(&final_resp);

    if (!result && timed_out) {
        if (out_timed_out) {
            *out_timed_out = true;
        }
        return strdup(LLM_TIMEOUT_TASK_COMPLETE_MSG);
    }
    return result;
}

/* MiMo 等推理模型在 thinking:disabled 未生效时，仍把 CoT 思考过程写进
 * content（例："小潮是桌面陪伴机器人…保持轻松友好的对话即可。你好！"）。
 * voice 回复要求 ≤40 字，真正的回答在结尾短句。这里把超长思考前缀裁掉，
 * 只保留结尾短句，避免思考过程被 TTS 读出来。仅当文本明显超长(>200 字节)
 * 且结尾是短句(≤80 字节)时才裁剪，正常多句回复不受影响。 */
static void strip_reasoning_prefix(char *text)
{
    size_t len;

    if (!text || !text[0]) {
        return;
    }

    len = strlen(text);
    if (len <= 200) {
        return;
    }

    for (size_t i = len; i > 0; i--) {
        char c = text[i - 1];
        if (c == '。' || c == '！' || c == '？' ||
            c == '!' || c == '?' || c == '.') {
            size_t tail = len - i;
            if (tail > 0 && tail <= 80) {
                memmove(text, text + i, tail + 1);
            }
            break;
        }
    }
}

/* Run the ReAct tool-calling loop. Returns final_text (caller owns). */
static char* run_react_loop(const char* sys_prompt, cJSON* messages,
    const char* tools_json, char* tool_output, size_t tool_size,
    const agent_msg_t* msg)
{
    char prev_sig[512];
    int dup_count;
    char prev_name[64];
    int name_repeat;
    int iteration;
    char* final_text = NULL;

    /* Clear stale speak text from previous message */
    free(s_speak_text);
    s_speak_text = NULL;
    s_emotion_tool_ran = false;
    s_face_tool_ran = false;
    s_tool_ran = false;

    prev_sig[0] = '\0';
    dup_count = 0;
    prev_name[0] = '\0';
    name_repeat = 0;
    int last_total_tokens = 0;
    bool watchdog_fired = false;

    /* Cap max_completion_tokens for TTS-bound channels.  Their system
     * prompt already forces ≤40-character replies, so a small budget
     * both prevents runaway generation and shrinks LLM server-side
     * scheduling latency (large defaults inflate TTFT).  Non-voice
     * channels fall through to the built-in default.  Cleared before
     * every return so the hint never leaks across messages. */
    bool is_short_channel =
        strcmp(msg->channel, AGENT_CHAN_VOICE) == 0
#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
        || strcmp(msg->channel, AGENT_CHAN_QUICKAPP) == 0
#endif
        ;
    llm_set_max_tokens_hint(is_short_channel
        ? AGENT_LLM_MAX_TOKENS_VOICE : 0);
    /* Voice replies are capped at ~40 chars — deep thinking on
     * reasoning models (mimo-v2.5-pro) only adds 20s+ latency. */
    llm_set_thinking_disabled_hint(is_short_channel);

    /* Router: select and apply best backend before first LLM call.
     * Estimate complexity from the last user message. */
    llm_complexity_t complexity = LLM_COMPLEXITY_SIMPLE;
    if (msg->content) {
        complexity = llm_router_estimate_complexity(
            msg->content, strlen(msg->content));
    }
    int router_idx = llm_router_select(complexity);
    if (router_idx >= 0) {
        llm_router_apply(router_idx);
    }

    /* Structured trace: begin */
    agent_trace_t trace;
    agent_trace_begin(&trace, msg->chat_id, msg->channel);
    trace.backend_idx = router_idx;

    /* Cache check: for simple queries, try cache before LLM call.
     * Proactive turns (companion / battery / fall) are exempt: their
     * prompts are static templates, so a cache hit would replay the
     * same canned reply forever — the companion feature explicitly
     * wants a fresh, varied LLM reply every turn. */
    if (msg->content && complexity == LLM_COMPLEXITY_SIMPLE
        && !(msg->flags & AGENT_MSG_FLAG_PROACTIVE)) {
        char* cached = llm_cache_get(msg->content, strlen(msg->content));
        if (cached) {
            final_text = cached;
            agent_trace_step(&trace, 0, NULL, 0, 1);
            agent_trace_end(&trace, AGENT_TRACE_OK);
            goto send_reply;
        }
    }

    for (iteration = 0; iteration < AGENT_AI_AGENT_MAX_TOOL_ITER;
         iteration++) {
        send_working_status(msg, iteration);

        llm_response_t resp;
        struct timeval tv_start, tv_end;
        gettimeofday(&tv_start, NULL);
        int err = llm_chat_tools(sys_prompt, messages, tools_json, &resp);
        gettimeofday(&tv_end, NULL);
        uint32_t latency_ms = calc_elapsed_ms(&tv_start, &tv_end);

        /* Router failover: on LLM call failure, try next backend */
        if (err != OK && router_idx >= 0) {
            syslog(LOG_WARNING,
                "[%s] LLM call failed on backend %d, trying failover\n",
                TAG, router_idx);
            llm_router_report_failure(router_idx);

            int next_idx = llm_router_select(complexity);
            if (next_idx >= 0 && next_idx != router_idx) {
                llm_router_apply(next_idx);
                router_idx = next_idx;
                trace.backend_idx = next_idx;
                llm_response_free(&resp);
                gettimeofday(&tv_start, NULL);
                err = llm_chat_tools(sys_prompt, messages,
                    tools_json, &resp);
                gettimeofday(&tv_end, NULL);
                latency_ms = calc_elapsed_ms(&tv_start, &tv_end);
            }
        }

        if (err != OK && router_idx < 0) {
            syslog(LOG_ERR, "[%s] LLM call failed with NO backend "
                "selected (router_idx=%d). No failover possible.\n",
                TAG, router_idx);
        }

        if (err != OK) {
            /* Distinguish timeout-induced failure from other errors */
            if (llm_call_timed_out(latency_ms)) {
                syslog(LOG_WARNING,
                    "[%s] LLM watchdog: call failed after %" PRIu32 " ms "
                    "(limit %ds)\n",
                    TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
                agent_trace_step(&trace, iteration, NULL,
                    latency_ms, 0);
                llm_response_free(&resp);
                final_text = strdup(LLM_TIMEOUT_MSG);
                watchdog_fired = true;
                break;
            }
            syslog(LOG_ERR, "[%s] LLM call failed (iter %d)\n",
                TAG, iteration);
            agent_trace_step(&trace, iteration, NULL, latency_ms, 0);
            llm_response_free(&resp);
            break;
        }

        /* Soft watchdog: the call completed successfully but took
         * longer than the configured limit.  Do NOT discard the valid
         * response — replacing a good answer with a canned timeout
         * message after the user already waited is the worst outcome.
         * Just warn so slow backends remain visible in logs. */
        if (llm_call_timed_out(latency_ms)) {
            syslog(LOG_WARNING,
                "[%s] LLM watchdog: call took %" PRIu32 " ms (limit %ds), "
                "accepting late response\n",
                TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
        }

        /* Report success to router */
        if (router_idx >= 0) {
            llm_router_report_success(router_idx);
            llm_router_report_latency(router_idx, latency_ms);
            if (resp.total_tokens > 0) {
                llm_router_report_tokens(router_idx,
                    resp.prompt_tokens, resp.completion_tokens);
                last_total_tokens = resp.total_tokens;
            }
        }

        syslog(LOG_INFO,
            "[%s] LLM resp: text=%zu, tool_use=%d, calls=%d\n",
            TAG, resp.text_len, resp.tool_use, resp.call_count);

        if (!resp.tool_use) {
            /* Cascade routing: if AUTO profile selected a cheap backend
             * for a SIMPLE query but the response looks inadequate,
             * retry once with PREMIUM tier. */
            bool cascade_retry = false;
            if (llm_router_get_profile() == LLM_ROUTE_AUTO
                && complexity == LLM_COMPLEXITY_SIMPLE
                && router_idx >= 0
                && resp.text != NULL) {
                bool low_quality = (strstr(resp.text, "I don't know") != NULL
                    || strstr(resp.text, "I cannot") != NULL
                    || strstr(resp.text, "无法") != NULL);
                if (low_quality) {
                    cascade_retry = true;
                }
            }

            if (cascade_retry) {
                syslog(LOG_INFO,
                    "[%s] Cascade: cheap response inadequate, "
                    "retrying with PREMIUM\n",
                    TAG);

                /* Save the cheap response as fallback before freeing */
                char* fallback_text = NULL;
                if (resp.text && resp.text_len > 0) {
                    fallback_text = strdup(resp.text);
                }
                llm_response_free(&resp);

                /* Select a PREMIUM backend without changing profile */
                int prem_idx = llm_router_select_with_profile(
                    LLM_COMPLEXITY_MEDIUM, LLM_ROUTE_PREMIUM);

                if (prem_idx >= 0 && prem_idx != router_idx) {
                    llm_router_apply(prem_idx);
                    router_idx = prem_idx;
                    trace.backend_idx = prem_idx;

                    gettimeofday(&tv_start, NULL);
                    err = llm_chat_tools(sys_prompt, messages,
                        tools_json, &resp);
                    gettimeofday(&tv_end, NULL);
                    latency_ms = calc_elapsed_ms(&tv_start, &tv_end);

                    /* Soft watchdog on cascade retry: warn only.
                     * The selection logic below already falls back to
                     * the saved cheap response when premium failed. */
                    if (llm_call_timed_out(latency_ms)) {
                        syslog(LOG_WARNING,
                            "[%s] LLM watchdog: cascade retry "
                            "took %" PRIu32 " ms\n",
                            TAG, latency_ms);
                    }

                    if (err == OK && resp.text && resp.text_len > 0) {
                        final_text = strdup(resp.text);
                        free(fallback_text);
                        fallback_text = NULL;
                    } else {
                        /* Premium failed — fall back to cheap response */
                        final_text = fallback_text;
                        fallback_text = NULL;
                    }
                    agent_trace_step(&trace, iteration, NULL,
                        latency_ms, err == OK);
                    llm_response_free(&resp);
                    break;
                }
                /* No premium backend available — use cheap response */
                final_text = fallback_text;
                fallback_text = NULL;
                agent_trace_step(&trace, iteration, NULL, latency_ms, 1);
                break;
            }

            if (resp.text && resp.text_len > 0 && !final_text) {
                final_text = strdup(resp.text);
            }
            agent_trace_step(&trace, iteration, NULL, latency_ms, 1);
            llm_response_free(&resp);

            /* If LLM returned empty text after tool calls, force a
             * final reply without tools (some models like MiMo with
             * reasoning mode return empty content after tool calls). */
            if (!final_text && iteration > 0) {
                syslog(LOG_WARNING, "[%s] Empty text after tool calls, "
                    "forcing finish reply\n", TAG);
                final_text = force_finish_reply(sys_prompt, messages);
            }
            break;
        }

        /* Only data/action tools count for E2E hybrid routing.  Emotion/
         * expression tools (set_face/speak/play_audio/stop_audio) are side
         * effects that must NOT force the slow agent path — so emotion
         * chitchat ("你真可爱" → set_face+speak) stays on E2E fast path. */
        for (int i = 0; i < resp.call_count; i++) {
            const char* n = resp.calls[i].name;
            if (strcmp(n, "set_face") == 0 || strcmp(n, "speak") == 0 ||
                strcmp(n, "play_audio") == 0 || strcmp(n, "stop_audio") == 0) {
                continue;
            }
            s_tool_ran = true;
        }

        syslog(LOG_INFO, "[%s] Tool iter %d: %d calls\n",
            TAG, iteration + 1, resp.call_count);

        /* Trace: log tool step */
        agent_trace_step(&trace, iteration,
            resp.call_count > 0 ? resp.calls[0].name : NULL,
            latency_ms, 1);

        /* Duplicate detection — break if stuck in a loop */
        bool should_break = check_tool_dup(
            &resp, prev_sig, &dup_count, prev_name, &name_repeat);

        if (should_break) {
            add_assistant_message(messages, &resp);
            add_tool_result_messages(messages, &resp, tool_output,
                tool_size, msg->channel, msg->chat_id);
            llm_response_free(&resp);
            break;
        }

        add_assistant_message(messages, &resp);
        add_tool_result_messages(messages, &resp, tool_output,
            tool_size, msg->channel, msg->chat_id);

        /* ── Emotion tool tracking (cross-round) ──────────────── */
        for (int i = 0; i < resp.call_count; i++) {
            if (strcmp(resp.calls[i].name, "set_face") == 0)
                s_face_tool_ran = true;
            if (strcmp(resp.calls[i].name, "set_face") == 0
                || strcmp(resp.calls[i].name, "play_audio") == 0)
                s_emotion_tool_ran = true;
        }
        /* ── Speak shortcut ──────────────────────────────────── */
        if (s_emotion_tool_ran && s_speak_text && s_speak_text[0]) {
            bool spoke_now = false;
            for (int i = 0; i < resp.call_count; i++)
                if (strcmp(resp.calls[i].name, "speak") == 0)
                    spoke_now = true;
            if (spoke_now) {
                final_text = s_speak_text;
                s_speak_text = NULL;
                s_emotion_tool_ran = false;
                syslog(LOG_INFO,
                    "[%s] speak shortcut: skip LLM wrap-up\n", TAG);
                agent_trace_step(&trace, iteration, "speak_shortcut",
                    0, 1);
                llm_response_free(&resp);
                break;
            }
        }


        /* Local tool shortcut: if the single tool in this round is a
         * local file op, skip the next LLM round and use the tool
         * output directly as the reply. Saves ~2s.
         * Restricted to call_count == 1: the parallel path does not
         * write into tool_output, so multi-call rounds must go through
         * the LLM to aggregate results (and tool_output would be stale
         * from a prior iteration if we allowed call_count > 1 here). */
        if (resp.call_count == 1) {
            bool all_local = true;
            for (int i = 0; i < resp.call_count; i++) {
                if (strcmp(resp.calls[i].name, "read_file") != 0
                    && strcmp(resp.calls[i].name, "write_file") != 0
                    && strcmp(resp.calls[i].name, "edit_file") != 0
                    && strcmp(resp.calls[i].name, "list_dir") != 0) {
                    all_local = false;
                    break;
                }
            }
            if (all_local && tool_output[0]) {
                /* Don't shortcut if the tool read a skill file
                 * from AGENT_SKILLS_DIR — the LLM needs to process
                 * the skill content and generate a proper user-facing
                 * response instead of dumping raw markdown to the user. */
                const char* file_op = resp.calls[0].name;
                bool is_context_op = false;
                if ((strcmp(file_op, "read_file") == 0
                     || strcmp(file_op, "write_file") == 0
                     || strcmp(file_op, "edit_file") == 0)
                    && resp.calls[0].input != NULL) {
                    cJSON *input_obj = cJSON_Parse(resp.calls[0].input);
                    if (input_obj) {
                        cJSON *path_obj = cJSON_GetObjectItem(input_obj, "path");
                        if (path_obj && cJSON_IsString(path_obj)) {
                            const char* p = path_obj->valuestring;
                            /* 技能/记忆/用户档案是 LLM 读写的上下文（读后还要写/处理，
                             * 写后还要生成正常回复），不是给用户看的最终回复，不能走
                             * shortcut 直接播报文件内容或"写入成功"。 */
                            if (strncmp(p, AGENT_SKILLS_DIR,
                                        strlen(AGENT_SKILLS_DIR)) == 0
                                || strcmp(p, AGENT_MEMORY_FILE) == 0
                                || strcmp(p, AGENT_USER_FILE) == 0) {
                                is_context_op = true;
                            }
                        }
                        cJSON_Delete(input_obj);
                    }
                }

                if (!is_context_op) {
                    syslog(LOG_INFO,
                        "[%s] Local tool shortcut: skip LLM round\n",
                        TAG);
                    final_text = strdup(tool_output);
                    llm_response_free(&resp);
                    break;
                }

                syslog(LOG_INFO,
                    "[%s] Context file op — no shortcut, LLM will process\n",
                    TAG);
            }
        }

        /* TASK_COMPLETE detection */
        if (resp.call_count == 1 && tool_output[0]
            && strstr(tool_output, "TASK_COMPLETE")) {
            syslog(LOG_INFO,
                "[%s] Tool %s returned TASK_COMPLETE\n",
                TAG, resp.calls[0].name);
            llm_response_free(&resp);
            bool task_timed_out = false;
            final_text = handle_task_complete(sys_prompt, messages,
                &task_timed_out);
            if (task_timed_out) {
                watchdog_fired = true;
            }
            break;
        }

        llm_response_free(&resp);
    }

    /* Iteration limit reached — force a summary reply */
    if (!final_text && iteration >= AGENT_AI_AGENT_MAX_TOOL_ITER) {
        final_text = force_finish_reply(sys_prompt, messages);
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (watchdog_fired) {
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (final_text) {
        agent_trace_end(&trace, AGENT_TRACE_OK);
    } else {
        agent_trace_end(&trace, AGENT_TRACE_FAIL);
    }

send_reply:
    /* 短回复通道(voice/quickapp)去掉 MiMo 混在 content 里的思考前缀，
     * 防止思考过程被 TTS 读出来。 */
    if (is_short_channel) {
        strip_reasoning_prefix(final_text);
    }

    /* Cache store: save simple query responses for future reuse.
     * Same PROACTIVE exemption as the cache check above: never cache
     * proactive replies, or companion turns would freeze after the
     * first test run. */
    if (final_text && msg->content
        && complexity == LLM_COMPLEXITY_SIMPLE
        && !(msg->flags & AGENT_MSG_FLAG_PROACTIVE)) {
        llm_cache_put(msg->content, strlen(msg->content), final_text);
        if (last_total_tokens > 0) {
            llm_cache_put_tokens(msg->content, strlen(msg->content),
                last_total_tokens);
        }
    }

    /* Reset per-message hint so it never leaks into unrelated callers
     * (e.g. llm_vision, memory summarizer). */
    llm_set_max_tokens_hint(0);
    llm_set_thinking_disabled_hint(false);

    return final_text;
}

/* ── Build messages array from session history + current msg ─ */

static cJSON* build_messages(const char* chat_id, const char* content,
    char* history_json, size_t hist_size)
{
    session_get_history_json(chat_id, history_json, hist_size,
        AGENT_AI_AGENT_MAX_HISTORY);

    cJSON* messages = cJSON_Parse(history_json);

    if (!messages) {
        messages = cJSON_CreateArray();
    }

    cJSON* user_msg = cJSON_CreateObject();

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", content);
    cJSON_AddItemToArray(messages, user_msg);
    return messages;
}

/* ── Inject session context into system prompt ───────────── */

static void inject_session_context(char* sys_prompt, size_t size,
    const char* channel, const char* chat_id)
{
    size_t len = strlen(sys_prompt);

    snprintf(sys_prompt + len, size - len,
        "\n## Current Session\n"
        "channel: %s\n"
        "chat_id: %s\n"
        "When using feishu_send_mention or feishu_chat_members, "
        "use the chat_id above unless the user specifies a "
        "different one.\n"
        "If the user message ends with "
        "[mentioned_users: name=open_id], "
        "use those open_ids when you need to @mention or remind "
        "those users. Do NOT include the [mentioned_users: ...] "
        "block in your reply text.\n",
        channel, chat_id);
}

/* ── Main agent loop task ─────────────────────────────────── */

/* Runs the TTS WebSocket TLS handshake (~1.5s) in parallel with the
 * LLM call.  Spawned from agent_loop_task so the socket lives in the
 * agent task group — the same group where voice_channel_speak() will
 * later use it (NuttX fd tables are per task group). */
static void* tts_preopen_worker(void* arg)
{
    (void)arg;
    volc_tts_ws_preopen();
    return NULL;
}

static void tts_preopen_spawn(void)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);   /* TLS handshake */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, tts_preopen_worker, NULL);
    if (rc != 0) {
        syslog(LOG_WARNING,
            "[%s] tts preopen thread failed: %d\n", TAG, rc);
    }
    pthread_attr_destroy(&attr);
}

/* ── LLM connection keep-warm ────────────────────────────────
 * The first LLM round of a voice turn pays a full TLS handshake
 * (~3 s) whenever the pooled connection went stale between turns
 * (server idle timeouts).  E2E-routed turns never touch the LLM
 * client, so the pool can sit cold for a long stretch and the next
 * tool/needing turn starts from zero.  This background thread
 * probes/re-establishes the pooled connection every 10 s so the
 * request path hits "Reusing pooled connection" instead.
 *
 * Spawned from agent_loop_start so the socket lives in the agent
 * task group — the same group where llm_chat will later use it
 * (NuttX fd tables are per task group), mirroring tts_preopen_worker.
 * Sleeping-detached thread costs 16 KB stack; the handshake itself
 * allocates on the heap, same as a normal request. */
#define LLM_KEEPWARM_INTERVAL_SEC 10

static void* llm_keepwarm_worker(void* arg)
{
    (void)arg;

    /* First sleep also gives WiFi time to come up at boot */
    while (1) {
        sleep(LLM_KEEPWARM_INTERVAL_SEC);

        if (!network_is_connected()) {
            continue;
        }

        /* No-op when a live pooled connection already exists */
        llm_preconnect();
    }
    return NULL;
}

static void llm_keepwarm_spawn(void)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);   /* TLS handshake */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, llm_keepwarm_worker, NULL);
    if (rc != 0) {
        syslog(LOG_WARNING,
            "[%s] llm keepwarm thread failed: %d\n", TAG, rc);
    }
    pthread_attr_destroy(&attr);
}

static void* agent_loop_task(void* arg)
{
    (void)arg;
    agent_mem_status_t mem_st;

    agent_mem_get_status(&mem_st);
    syslog(LOG_INFO, "[%s] Agent loop started, free heap: %zu\n",
        TAG, mem_st.free_heap);

    size_t ctx_size = agent_mem_safe_size(
        AGENT_CONTEXT_BUF_SIZE, 4 * 1024);
    size_t hist_size = agent_mem_safe_size(
        AGENT_LLM_STREAM_BUF_SIZE, 8 * 1024);
    size_t tool_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE, TOOL_OUTPUT_SIZE_MIN);

    char* sys_prompt = calloc(1, ctx_size);
    char* history_json = calloc(1, hist_size);
    char* tool_output = calloc(1, tool_size);

    if (!sys_prompt || !history_json || !tool_output) {
        syslog(LOG_ERR, "[%s] Failed to allocate agent buffers\n", TAG);
        free(sys_prompt);
        free(history_json);
        free(tool_output);
        return NULL;
    }

    syslog(LOG_INFO, "[%s] Buffers: ctx=%zu hist=%zu tool=%zu\n",
        TAG, ctx_size, hist_size, tool_size);

    /* Initialize memory pool for parallel tool outputs */
    size_t pool_buf_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE_LARGE, TOOL_OUTPUT_SIZE_MIN);
    if (agent_pool_init(&s_tool_pool, pool_buf_size,
            AGENT_MAX_TOOL_CALLS)
        == OK) {
        s_pool_ready = true;
        syslog(LOG_INFO, "[%s] Tool output pool: %d x %zu bytes\n",
            TAG, AGENT_MAX_TOOL_CALLS, pool_buf_size);
    } else {
        syslog(LOG_WARNING,
            "[%s] Pool init failed, using heap fallback\n", TAG);
    }

    char* tools_json = tool_registry_get_tools_json();

    syslog(LOG_INFO, "[%s] Tools JSON loaded: %d bytes\n",
        TAG, tools_json ? (int)strlen(tools_json) : 0);

    while (!agent_shutdown_requested()) {
        agent_msg_t msg;
        int err = message_bus_pop_inbound(&msg, UINT32_MAX);

        if (err != OK) {
            continue;
        }

        if (agent_shutdown_requested()) {
            free(msg.content);
            free(msg.image_b64);
            break;
        }

        syslog(LOG_INFO, "[%s] Processing message from %s:%s\n",
            TAG, msg.channel, msg.chat_id);

        /* Voice request: pre-open the TTS WebSocket in parallel with
         * the LLM call.  MUST happen here (agent task group) — the
         * conversation thread lives in the launcher task group and
         * NuttX fd tables are per task group, so a socket opened
         * there is unusable by voice_channel_speak() (EBADF). */
        if (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0) {
#ifdef CONFIG_AI_AGENT_VOLC_E2E
            /* The E2E backend already owns a persistent connection that
             * carries TTS, so pre-warming a second TLS context (~25KB)
             * plus a worker thread would be pure waste.  Exception:
             * proactive turns (fall help / battery reminders) force the
             * classic WS TTS — the E2E server never synthesizes audio
             * for them — so they DO need the pre-warm or the reply
             * pays a full ~1.5s TLS handshake after the LLM call. */
            const char* tts_be = voice_tts_get_backend();
            if (!tts_be || strcmp(tts_be, AGENT_VOICE_BACKEND_E2E) != 0
                || volc_e2e_classic_tts_forced())
#endif
            {
                tts_preopen_spawn();
            }
        }

        /* Check memory pressure */
        agent_mem_get_status(&mem_st);
        if (mem_st.free_heap < AGENT_MEM_RESERVE_BYTES) {
            syslog(LOG_WARNING,
                "[%s] Low memory: %zu bytes free, skipping\n",
                TAG, mem_st.free_heap);
            agent_msg_t out = { 0 };
            strncpy(out.channel, msg.channel,
                sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id,
                sizeof(out.chat_id) - 1);
            out.content = strdup("系统内存不足，请稍后再试。");
            if (out.content) {
                if (message_bus_push_outbound(&out) != OK) {
                    free(out.content);
                }
            }
            free(msg.content);
            continue;
        }

        /* Slash commands — fast path, bypass LLM */
        char* reply = handle_slash_command(&msg);

        /* Prompt injection check — before LLM */
        if (!reply && tool_guard_check_injection(msg.content)) {
            syslog(LOG_WARNING,
                "[%s] Prompt injection blocked from %s:%s\n",
                TAG, msg.channel, msg.chat_id);
            reply = strdup("I can't do that.");
            if (!reply) {
                /* Fail-closed: skip this message entirely */
                free(msg.content);
                continue;
            }
        }

        /* NL fast path — after injection check, before LLM.
         * Proactive prompts (battery reminders etc.) are exempt: their
         * keywords (电量/充电) would hijack the prompt into a fixed
         * fast-path reply and the LLM would never run. */
        syslog(LOG_INFO, "[%s] NL gate: flags=%d proactive=%d\n",
               TAG, msg.flags,
               (msg.flags & AGENT_MSG_FLAG_PROACTIVE) != 0);
        if (!reply && (msg.flags & AGENT_MSG_FLAG_PROACTIVE) == 0) {
            reply = handle_nl_fast_path(msg.content);
        }

        if (reply) {
#ifdef CONFIG_AI_AGENT_VOLC_E2E
            /* A pre-LLM reply (slash command / injection-blocked / NL
             * fast path) skips the hybrid routing below.  Resolve any
             * pending E2E state so the server self-chat is discarded
             * and this reply is spoken cleanly (via volc_tts_ws). */
            if (volc_e2e_hybrid_get() == E2E_HYBRID_PENDING) {
                char* server_text = volc_e2e_hybrid_route_agent();
                free(server_text);
                volc_e2e_note_agent_path();
            }
#endif
            /* Voice-first: speak fast-path response */
            voice_channel_speak(reply);

            /* 记忆回写：fast-path 固定回复也写进 session，两个脑共享记忆。 */
            session_append(msg.chat_id, "user", msg.content);
            session_append(msg.chat_id, "assistant", reply);

            agent_msg_t out = { 0 };
            strncpy(out.channel, msg.channel,
                sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id,
                sizeof(out.chat_id) - 1);
            out.content = reply;
            if (message_bus_push_outbound(&out) != OK) {
                free(reply);
            }
            free(msg.content);
            continue;
        }

        /* Hot-reload skills if directory changed */
        if (skill_loader_check_changed()) {
            syslog(LOG_INFO, "[%s] Skills changed, refreshing\n", TAG);
            skill_loader_refresh();
        }

        context_build_system_prompt(sys_prompt, ctx_size);
        inject_session_context(sys_prompt, ctx_size,
            msg.channel, msg.chat_id);

        /* Refresh tools JSON — Node tools are dynamic */
        free(tools_json);
        tools_json = tool_registry_get_tools_json();

        /* Obvious chitchat gets no tools so the LLM can't call
         * set_face/speak/read_file and force the slow agent path — it
         * stays on the E2E fast path.  Everything else gets tools, and
         * the routing is decided by s_tool_ran || has_skill. */
        bool chitchat = is_chitchat(msg.content);
        const char* llm_tools = chitchat ? NULL : tools_json;

        /* Pre-inject matching skill content into system prompt.
         * When user says "压力好大", stress-relief content is
         * appended so LLM sees full steps without read_file.
         * The directive header is essential: the Skills summary
         * advertises "(read with: read_file ...)" and without an
         * explicit override the LLM burns a whole extra round
         * (~10s) re-reading the skill file it already has. */
        bool has_skill = false;
        /* PROACTIVE exemption: injected system prompts (companion /
         * battery / fall) are not user speech — running them through
         * the emotion-skill trigger table caused accidental matches
         * (e.g. the companion template's "聊聊" pulled in the
         * listen-comfort skill and bent the reply style). */
        if (llm_tools && msg.content
            && !(msg.flags & AGENT_MSG_FLAG_PROACTIVE)) {
            char* skill = skill_loader_match_trigger(msg.content);
            if (skill) {
                has_skill = true;
                static const char kSkillHdr[] =
                    "\n## 已命中技能（全文已内联如下）\n"
                    "禁止再用 read_file 读任何 skill 文件；"
                    "直接在本轮按 How to use 步骤在同一个 "
                    "tool_calls 数组里一次性完成全部工具调用。\n";
                size_t prompt_len = strlen(sys_prompt);
                size_t hdr_len    = sizeof(kSkillHdr) - 1;
                size_t skill_len  = strlen(skill);
                if (prompt_len + hdr_len + skill_len + 1 < ctx_size) {
                    memcpy(sys_prompt + prompt_len, kSkillHdr, hdr_len);
                    memcpy(sys_prompt + prompt_len + hdr_len,
                           skill, skill_len + 1);
                    syslog(LOG_INFO,
                        "[%s] skill content injected "
                        "(prompt %zu→%zu bytes)\n",
                        TAG, prompt_len,
                        strlen(sys_prompt));
                }
                free(skill);
            }
        }

#ifdef CONFIG_AI_AGENT_VOLC_E2E
        /* 优化1：明显闲聊短路 LLM。E2E 服务端自聊已缓冲在 ring，
         * 立即 route_e2e 排空，省掉一整轮客户端 LLM 推理（其输出本会被
         * route_e2e 丢弃）。仅当非技能触发词才短路。 */
        if (chitchat && !has_skill
            && volc_e2e_hybrid_enabled()
            && volc_e2e_hybrid_get() == E2E_HYBRID_PENDING) {
            syslog(LOG_INFO,
                "[%s] chitchat shortcut: \"%s\" → E2E (skip LLM)\n",
                TAG, msg.content);
            volc_e2e_hybrid_route_e2e();
            volc_e2e_note_e2e_path();

            /* 客户端表情（统一入口 apply_client_emotion_face，
             * 覆盖全部路由）。 */
            apply_client_emotion_face(msg.content);
            /* 服务端文本作为兜底/出站内容；TTS 层因 hy==E2E 直接 drain
             * ring，文本仅在 drain 截断时兜底。 */
            char* server_text = volc_e2e_hybrid_get_reply();
            if (!server_text || !server_text[0]) {
                free(server_text);
                server_text = strdup("好的");
            }
            voice_channel_speak(server_text);

            /* E2E drain 已结束，此时服务端自聊文本已填充。把用户输入 +
             * 服务端文本回写 session，让两个脑共享记忆（本地，播放后，零延迟）。 */
            char* actual_reply = volc_e2e_hybrid_get_reply();
            const char* display = (actual_reply && actual_reply[0])
                ? actual_reply : server_text;
            session_append(msg.chat_id, "user", msg.content);
            session_append(msg.chat_id, "assistant", display);

            agent_msg_t out = { 0 };
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup(display);
            if (out.content) {
                if (message_bus_push_outbound(&out) != OK) {
                    free(out.content);
                }
            }
            free(actual_reply);
            free(server_text);
            free(msg.content);
            continue;
        }
#endif

        /* Emotion hint: when no skill matched, scan user text for emotion
         * keywords and inject a strong directive so the LLM calls set_face
         * reliably.  Skill messages skip this — the skill itself specifies
         * the face. */
        char* emotion_hint = NULL;
#ifdef CONFIG_AI_AGENT_VOLC_E2E
        /* In E2E mode the face is applied client-side in the route_e2e
         * branch (emotion_face_for_text), so do NOT inject a set_face
         * hint: the resulting set_face tool call would set s_tool_ran and
         * force the hybrid router onto the slow agent path. */
        bool e2e_client_face = volc_e2e_hybrid_enabled();
#else
        bool e2e_client_face = false;
#endif
        if (!e2e_client_face && !has_skill && llm_tools && msg.content) {
            int hint_count = 0;
            emotion_hint = build_emotion_hint(msg.content, &hint_count);
            if (emotion_hint) {
                syslog(LOG_INFO, "[%s] emotion hint injected (%d triggers)\n",
                    TAG, hint_count);
            }
        }

        cJSON* messages = build_messages(msg.chat_id, msg.content,
            history_json, hist_size);

        /* Emotion hint: insert as system message right before user msg.
         * build_messages appends the user msg at the end, so we detach it,
         * add the hint, then re-append the user msg. */
        if (emotion_hint) {
            int arr_size = cJSON_GetArraySize(messages);
            cJSON* user_msg = (arr_size > 0)
                ? cJSON_DetachItemFromArray(messages, arr_size - 1)
                : NULL;

            cJSON* hint_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(hint_msg, "role", "system");
            cJSON_AddStringToObject(hint_msg, "content", emotion_hint);
            cJSON_AddItemToArray(messages, hint_msg);

            if (user_msg) {
                cJSON_AddItemToArray(messages, user_msg);
            }
            free(emotion_hint);
        }

        char* final_text = NULL;

        /* Vision path */
        final_text = handle_vision_message(&msg);
        if (final_text) {
            cJSON_Delete(messages);
        } else {
            /* ReAct loop */
            s_llm_path_count++;
            final_text = run_react_loop(sys_prompt, messages,
                llm_tools, tool_output, tool_size, &msg);
            cJSON_Delete(messages);
        }

        /* If LLM called set_face, keep the emotion face
         * during TTS instead of overriding it with "speaking". */
        if (s_face_tool_ran) {
            voice_channel_suppress_speaking_face(true);
        }

#ifdef CONFIG_AI_AGENT_VOLC_E2E
        /* ── Hybrid orchestration routing ──────────────────────
         * When hybrid mode is PENDING (ASR ended, agent LLM just
         * finished, server audio buffered in E2E ring), route on
         * whether a tool ran OR a skill matched this turn:
         *   - no tool + no skill → trivial chat → route_e2e (play the
         *     server's fast self-chat audio, apply emotion face
         *     client-side)
         *   - tool used or skill matched → route_agent (agent's
         *     tool/skill-aware reply via volc_tts_ws)
         * ──────────────────────────────────────────────────── */
        if (volc_e2e_hybrid_enabled()
            && volc_e2e_hybrid_get() == E2E_HYBRID_PENDING) {
            bool used_tool = s_tool_ran || has_skill;
            if (used_tool) {
                char* agent_text = volc_e2e_hybrid_route_agent();
                free(agent_text);  /* agent's own final_text is used */
                volc_e2e_note_agent_path();
                syslog(LOG_INFO,
                    "[%s] hybrid: routing to agent (tool used)\n", TAG);
            } else {
                int rc = volc_e2e_hybrid_route_e2e();
                volc_e2e_note_e2e_path();
                if (rc == 0) {
                    syslog(LOG_INFO,
                        "[%s] hybrid: routing to E2E fast path\n", TAG);
                    /* Emotion face is applied by the unified
                     * apply_client_emotion_face() right before
                     * dispatch_response below. */
                    /* Keep agent's final_text as a fallback — the
                     * dispatch_response will speak whatever is in
                     * final_text; volc_e2e_tts will drain the ring
                     * because s_hybrid == E2E_HYBRID_E2E, so the
                     * text content is effectively ignored at the
                     * TTS layer. */
                }
            }
        }
#endif

        /* L1 统一端侧表情：未被上面 E2E 分支覆盖的路径（route_agent、
         * 非 hybrid 回退轮）同样由客户端关键词表决定情绪脸；本轮若有
         * Skill/LLM 的 set_face 已执行（s_face_tool_ran），helper 内部
         * 自动让位，保留 Skill 自己的表情与 duration 语义。
         *
         * 注入回合（低电量/充电主动提醒）除外：提示词自带「可爱」等
         * 词会被关键词表误判成 shy，造成 sick→thinking→shy 的频繁
         * 切换。注入回合只在 TTS 即将播放时切一次固定表情（sick/
         * proud），播完由 voice 层恢复 listening。 */
        if (msg.flags & AGENT_MSG_FLAG_PROACTIVE) {
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
            /* Proactive face is a WHITELIST now: charger plugged /
             * picked upright → proud, low battery → sick, everything
             * else (companion idle turns) keeps the current face —
             * TTS flips it to speaking, then back to listening.  The
             * old "else sick" fallback made companion turns flash
             * the error face (sick maps to ERROR_1.GIF).  Keywords
             * match the injected prompts in tool_emotion_motion.c and
             * watch_pages.c. */
            const char* pface = NULL;
            if (strstr(msg.content, "扶起来了")
                || strstr(msg.content, "充上电")) {
                pface = "proud";
            } else if (strstr(msg.content, "电量")) {
                pface = "sick";
            }
            if (pface) {
                syslog(LOG_INFO, "[%s] proactive face: %s\n", TAG, pface);
                watch_expression_page_set_face(pface, 0);
            }
#endif
        } else {
            apply_client_emotion_face(msg.content);
        }

        dispatch_response(&msg, final_text);

        /* Reset face-tool state so the next turn starts clean.
         * Without this, a stale s_face_tool_ran from a previous
         * set_face call (e.g. "happy" after volume adjustment)
         * keeps suppress_speaking_face active indefinitely. */
        s_face_tool_ran = false;

        /* Free image_b64 if not already freed by vision path */
        free(msg.image_b64);
        msg.image_b64 = NULL;
        free(msg.content);
    }

    /* Cleanup (unreachable in normal operation) */
    if (s_pool_ready) {
        agent_pool_destroy(&s_tool_pool);
        s_pool_ready = false;
    }
    free(tools_json);
    free(sys_prompt);
    free(history_json);
    free(tool_output);
    return NULL;
}

/* ── Public interface ─────────────────────────────────────── */

int agent_loop_init(void)
{
    syslog(LOG_INFO, "[%s] Agent loop initialized\n", TAG);
    return OK;
}

int agent_loop_start(void)
{
    int ret = agent_task_create(agent_loop_task, "agent_loop",
        AGENT_AI_AGENT_STACK, NULL, AGENT_AI_AGENT_PRIO);

    if (ret != OK) {
        syslog(LOG_ERR,
            "[%s] Failed to create agent_loop task\n", TAG);
        return ret;
    }

    /* Keep the LLM TLS pool warm from the agent task group so the
     * first round of each voice turn skips the ~3 s handshake */
    llm_keepwarm_spawn();

    return OK;
}

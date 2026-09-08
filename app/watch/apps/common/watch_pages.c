/****************************************************************************
 * apps/watch/apps/common/watch_pages.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <mqueue.h>
#include <time.h>

#include <nuttx/power/axp2101.h>

#include "watch_pages.h"
#include "../home_control/home_control.h"
#include "../volume_control/volume_control.h"

/* System alert audio (flat build, symbol from ai_agent package) */
extern void tool_system_alert_play(int alert_id, int force);

#ifdef CONFIG_AI_AGENT_EMOTION_TOY
/* ai_agent 语音管线（flat build 直接链接）：低电量/充电主动提醒用。
 * 声明不引入 ai_agent 头文件——跨 app 目录无 include 路径，与
 * tool_system_alert_play 保持一致的 extern 风格。
 * 提示词经 voice_channel_inject_prompt 注入，由 conversation_thread
 * 按完整对话回合消费（LLM 生成文案 + TTS + 状态机），不直接 speak
 * ——外部 speak 会把 LISTENING 翻成 SPEAKING 导致对话线程退出、
 * 唤醒词系统永久失效。 */
extern bool voice_channel_is_listening(void);
extern int  voice_channel_inject_prompt(const char *text);
extern bool voice_channel_is_mic_muted(void);
extern bool voice_channel_is_wake_gated(void);
extern void voice_channel_suppress_speaking_face(bool suppress);
extern bool tool_emotion_audio_is_playing(void);
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PAGE_TAG "[PAGE] "

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define PAGE_LOG(fmt, ...)  syslog(LOG_INFO, PAGE_TAG fmt, ##__VA_ARGS__)

#define MAX_PAGE_STACK_SIZE 16

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 表情页面运行时状态 */
static lv_obj_t   *s_page_root    = NULL;  /* 页面根容器 */
static lv_obj_t   *s_expr_gif     = NULL;  /* 表情GIF控件 */
static int         s_curr_index   = 0;     /* 当前表情索引 */
static int         s_expr_count   = 0;     /* 表情图片总数 */

/* set_face 去重状态：当前显示的 GIF 索引 + 是否持久（无恢复定时器）。
 * 双切链路（motion 预设 → agent PROACTIVE 重设同一张脸）每次都会
 * 让 GIF 从第 0 帧重播，观感为表情"切换两次"；watch_expression_
 * page_set_face 里据此跳过重复的持久同脸请求。restore_timer_cb
 * 恢复 excited、deinit 重建页面时同步。int 读写在 32 位平台原子，
 * 跨线程竞态的后果只是偶发多播一次，与 s_restore_duration_ms 的
 * 现有跨线程风格一致。 */
static int         s_cur_gif_index  = -1;
static bool        s_cur_persistent = false;

/* 页面栈：用于跟踪二级/三级子页面 */
static lv_obj_t **s_page_stack = NULL;
static int       s_page_stack_size = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief 等待SD卡就绪
 */
static int wait_for_sd_ready(void)
{
  int retries;
  for (retries = 0; retries < WATCH_SD_READY_RETRIES; retries++)
    {
      if (watch_resource_is_sd_ready())
        {
          return 1;
        }
      usleep(WATCH_SD_READY_DELAY_MS * 1000);
    }

  PAGE_LOG("ERROR: SD card not ready after %d retries", WATCH_SD_READY_RETRIES);
  return 0;
}

/**
 * @brief 淡入动画回调函数
 */
static void fade_in_anim_cb(void *obj, int32_t v)
{
  lv_obj_set_style_opa(obj, v, 0);
}

/**
 * @brief 切换到指定索引的表情GIF
 */
static void switch_to_expression(int index)
{
  if (s_expr_count <= 0 || s_expr_gif == NULL)
    {
      return;
    }

  /* 循环取模，确保索引在有效范围内 */
  index = index % s_expr_count;
  if (index < 0)
    {
      index += s_expr_count;
    }

  s_curr_index = index;

  const void *gif_src = watch_resource_get_img_expression(index);
  if (gif_src == NULL)
    {
      PAGE_LOG("ERROR: Failed to get expression data at index %d", index);
      return;
    }

  /* 淡出当前图片 */
  lv_obj_set_style_opa(s_expr_gif, LV_OPA_TRANSP, 0);

  /* 设置GIF嵌入式数据源 */
  lv_gif_set_src(s_expr_gif, gif_src);

  /* 重启GIF动画 */
  lv_gif_restart(s_expr_gif);

  /* 淡入新图片（使用exec_cb回调实际更新透明度） */
  lv_anim_t fade_anim;
  lv_anim_init(&fade_anim);
  lv_anim_set_var(&fade_anim, s_expr_gif);
  lv_anim_set_exec_cb(&fade_anim, fade_in_anim_cb);
  lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&fade_anim, WATCH_EXPRESSION_FADE_TIME_MS);
  lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
  lv_anim_start(&fade_anim);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *watch_expression_page_init(lv_obj_t *parent)
{
  if (s_page_root != NULL)
    {
      return s_page_root;
    }

  /* 获取表情图片总数 */
  s_expr_count = watch_resource_get_expression_count();
  if (s_expr_count <= 0)
    {
      PAGE_LOG("ERROR: No expression images registered");
      return NULL;
    }

  /* 创建页面根容器 */
  s_page_root = lv_obj_create(parent);
  lv_obj_set_size(s_page_root, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(s_page_root, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(s_page_root, 0, 0);
  lv_obj_set_style_radius(s_page_root, 0, 0);
  lv_obj_set_style_pad_all(s_page_root, 0, 0);
  lv_obj_clear_flag(s_page_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(s_page_root);

  /* 创建GIF控件 */
  s_expr_gif = lv_gif_create(s_page_root);
  lv_obj_center(s_expr_gif);

  /* 设置GIF控件背景透明。
   * 首次加载时不做淡入动画——直接显示第一帧，
   * 避免从开机动画切换到表情页时出现黑屏间隙。
   * 后续轮播切换由 switch_to_expression() 走淡入/淡出流程。 */
  lv_obj_set_style_bg_opa(s_expr_gif, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(s_expr_gif, LV_OPA_COVER, 0);

  /* 让LVGL处理一次事件循环，确保控件完全初始化 */
  lv_timer_handler();

  /* 加载第一张表情GIF，如果失败则依次尝试后续表情 */
  int i;
  for (i = 0; i < s_expr_count; i++)
    {
      s_curr_index = i;
      const void *first_gif = watch_resource_get_img_expression(s_curr_index);
      if (first_gif == NULL)
        continue;

      lv_gif_set_src(s_expr_gif, first_gif);

      int32_t w = lv_obj_get_width(s_expr_gif);
      int32_t h = lv_obj_get_height(s_expr_gif);

      if (w > 0 && h > 0)
        {
          /* 首张GIF加载成功，重启播放 */
          lv_gif_restart(s_expr_gif);
          break;
        }
    }

  return s_page_root;
}

void watch_expression_page_hide(void)
{
  if (s_page_root) {
    lv_obj_add_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
  }
}

void watch_expression_page_show(void)
{
  if (s_page_root) {
    lv_obj_clear_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
  }
}

bool watch_expression_page_is_hidden(void)
{
  if (s_page_root == NULL)
    {
      return true;  /* 未初始化视为隐藏 */
    }
  return lv_obj_has_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
}

void watch_expression_page_deinit(lv_obj_t *page_obj)
{
  /* 重置状态 */
  s_expr_gif   = NULL;
  s_curr_index = 0;
  s_expr_count = 0;
  /* 去重状态同步重置：页面重建后会加载首张 GIF，
   * 旧记录不再反映屏上内容 */
  s_cur_gif_index = -1;
  s_cur_persistent = false;

  /* 删除页面根容器（同时销毁子控件） */
  if (page_obj != NULL)
    {
      lv_obj_del(page_obj);
    }

  s_page_root = NULL;
}

int lv_watch_push_page(lv_obj_t *page)
{
  if (page == NULL)
    {
      return -1;
    }

  if (s_page_stack == NULL)
    {
      s_page_stack = calloc(MAX_PAGE_STACK_SIZE, sizeof(lv_obj_t *));
      if (s_page_stack == NULL)
        {
          return -3;
        }
    }

  if (s_page_stack_size >= MAX_PAGE_STACK_SIZE)
    {
      return -2;
    }

  s_page_stack[s_page_stack_size++] = page;
  return 0;
}

int lv_watch_pop_page(lv_obj_t *page)
{
  if (page == NULL || s_page_stack_size == 0)
    {
      return -1;
    }

  int i;
  for (i = 0; i < s_page_stack_size; i++)
    {
      if (s_page_stack[i] == page)
        {
          break;
        }
    }

  if (i >= s_page_stack_size)
    {
      return -2;
    }

  for (; i < s_page_stack_size - 1; i++)
    {
      s_page_stack[i] = s_page_stack[i + 1];
    }

  s_page_stack_size--;
  return 0;
}


/* ── set_face 集成 ──────────────────────────────────────────────── */

/* face_id → GIF索引映射表（与 ai_agent set_face 白名单对齐） */
typedef struct {
    const char *face_id;
    int         gif_index;
} face_map_t;

static const face_map_t s_face_map[] = {
    { "happy",      0  },   /* SMILE.GIF         */
    { "neutral",    1  },   /* CALM.GIF          */
    { "love",       2  },   /* CARING.GIF        */
    { "peaceful",   3  },   /* COMFORT.GIF       */
    { "confused",   4  },   /* CONFUSED.GIF      */
    { "excited",    5  },   /* ENERGY_PULSE.GIF  */
    { "sick",       6  },   /* ERROR_1.GIF       */
    { "worried",    7  },   /* FALL.GIF          */
    { "heartbeat",  8  },   /* HEART_BREATHING   */
    { "listening",  9  },   /* LISTENING.GIF     */
    { "cool",       10 },   /* MUTED.GIF         */
    { "shy",        11 },   /* SHY.GIF           */
    { "sleepy",     12 },   /* SLEEP.GIF         */
    { "sleeping",   13 },   /* SLEEP_BREATHING   */
    { "speaking",   14 },   /* SPEAKING.GIF      */
    { "standby",    15 },   /* STANDBY_1.GIF     */
    { "proud",      16 },   /* SUCCESS_1.GIF     */
    { "thinking",   17 },   /* THINKING_1.GIF    */
    { "waiting",    18 },   /* WAITING.GIF       */
};
#define FACE_MAP_SIZE (sizeof(s_face_map) / sizeof(s_face_map[0]))

/* 表情恢复定时器 */
static lv_timer_t *s_restore_timer = NULL;

/* 恢复定时器ID（用于在LVGL线程中创建） */
static int s_restore_duration_ms = 0;

static void restore_timer_cb(lv_timer_t *timer);

/* 设置表情的异步回调 (LVGL线程安全) */
static void set_face_async_cb(void *user_data)
{
    int gif_index = (int)(intptr_t)user_data;
    if (s_expr_gif == NULL || s_expr_count <= 0) return;

    /* 取消之前的恢复定时器 */
    if (s_restore_timer != NULL) {
        lv_timer_del(s_restore_timer);
        s_restore_timer = NULL;
    }

    switch_to_expression(gif_index);

    /* 在LVGL线程中创建恢复定时器（lv_timer_create非线程安全） */
    if (s_restore_duration_ms > 0) {
        s_restore_timer = lv_timer_create(restore_timer_cb,
                                           (uint32_t)s_restore_duration_ms,
                                           NULL);
        if (s_restore_timer) {
            lv_timer_set_repeat_count(s_restore_timer, 1);
        }
        s_restore_duration_ms = 0;
    }
}

/* 恢复表情的回调（不恢复轮播——回到 excited） */
static void restore_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_restore_timer = NULL;

    /* 不恢复自动轮播——回到固定的 excited */
    for (size_t i = 0; i < FACE_MAP_SIZE; i++) {
        if (strcmp(s_face_map[i].face_id, "excited") == 0) {
            switch_to_expression(s_face_map[i].gif_index);
            /* 同步去重状态：当前已是 excited（持久显示） */
            s_cur_gif_index = s_face_map[i].gif_index;
            s_cur_persistent = true;
            break;
        }
    }
}

/* [REMOVED 2026-08-17] watch_expression_page_set_default()（excited）删除：
 * 无任何调用者的死代码，且"开机默认 excited"与"每轮 listening→thinking→
 * 回复脸→listening 的状态机独占回合内表情"的设计冲突。 */

int watch_expression_page_set_face(const char* face_id, int duration_ms)
{
    if (face_id == NULL || s_expr_gif == NULL || s_expr_count <= 0) {
        return -1;
    }

    /* 查找 face_id 对应的 GIF 索引 */
    int gif_index = -1;
    for (size_t i = 0; i < FACE_MAP_SIZE; i++) {
        if (strcmp(face_id, s_face_map[i].face_id) == 0) {
            gif_index = s_face_map[i].gif_index;
            break;
        }
    }

    if (gif_index < 0) {
        PAGE_LOG("set_face: unknown face_id '%s'", face_id);
        return -1;
    }

    /* 同脸去重：当前已持久显示该表情（无恢复定时器）且本次也是
     * 持久请求 → 跳过。否则 GIF 会从第 0 帧重播，用户看到表情
     * "切换两次"——跌倒/扶正/电量提醒链路都是双切模式：motion
     * 线程（或注入方）立即预设 sick/proud，agent PROACTIVE 分支
     * 在 LLM 回合后（约 3-4 秒）再设同一张脸。
     * 带时长的请求（duration>0）永不去重：它必须刷新恢复定时器；
     * timed→persistent 的同脸请求也不去重：它必须取消定时器。 */
    if (gif_index == s_cur_gif_index && s_cur_persistent
        && duration_ms == 0) {
        PAGE_LOG("set_face: %s already showing, skip restart",
                 face_id);
        return 0;
    }

    PAGE_LOG("set_face: %s -> index %d, duration=%dms",
             face_id, gif_index, duration_ms);

    /* 记录恢复时长（LVGL线程会读取并创建定时器） */
    s_restore_duration_ms = duration_ms;
    /* 同步去重状态（async 回调消费 s_restore_duration_ms） */
    s_cur_gif_index = gif_index;
    s_cur_persistent = (duration_ms == 0);

    /* 在 LVGL 线程中执行表情切换 + 创建恢复定时器 */
    lv_async_call(set_face_async_cb, (void*)(intptr_t)gif_index);

    return 0;
}

/* ── 电池电量监控 ──────────────────────────────────────────────── */

/* 低电量告警阈值（百分比）：临时调至 80 便于实测验证，正式发布
 * 前应恢复为 20 */
#define WATCH_BATTERY_LOW_THRESHOLD   80

/* 电量周期检测间隔（毫秒） */
#define WATCH_BATTERY_CHECK_INTERVAL_MS  10000  /* 10秒检测一次 */

/* 低电量期间重复提醒间隔（秒）：低于阈值期间每 60 秒注入一次
 * 提醒提示词，直到充电或电量恢复 */
#define WATCH_BATTERY_REMIND_INTERVAL_SEC  60

/* 充电状态轮询间隔（毫秒）：插电报喜要求 ~1s 内感知 */
#define WATCH_CHARGE_CHECK_INTERVAL_MS  1000

/* 注入 E2E 模型的主动提示词。措辞避开端侧情感关键词表
 * （emotion_keywords.h），避免 apply_client_emotion_face 覆盖本模块
 * 预设的固定表情；并显式禁止工具调用，防止模型经 set_face 工具
 * 覆盖表情。 */
#define BATTERY_LOW_PROMPT_INJECT \
  "【系统】当前设备电量仅剩%u%%，请主动提醒主人给我充电，" \
  "用一句话表达，语气可爱自然，不要调用任何工具。"
#define BATTERY_CHARGED_PROMPT_INJECT \
  "【系统】主人刚给我充上电啦，请用一句话表达开心和感谢，" \
  "语气可爱，不要调用任何工具。"

/* 低电量提醒固定表情（sick=虚弱）与充电报喜固定表情（proud） */
#define BATTERY_LOW_FACE     "sick"
#define BATTERY_CHARGED_FACE "proud"

/* 电量监控定时器 */
static lv_timer_t *s_battery_timer = NULL;

/* 充电状态轮询定时器 */
static lv_timer_t *s_charge_timer = NULL;

/* 低电量提醒已播报标志：作为「充电满血报喜」的资格——仅低电量
 * 提醒后插电才报喜；插电报喜或电量恢复后清除 */
static bool s_low_power_reminded = false;

/* 上次充电状态：用于检测 0→1 跳变（插入充电器） */
static bool s_prev_charging = false;

/* 上次注入提醒的时间戳：低电量期间周期性提醒（每 60 秒一次），
 * 电量恢复后清零重计 */
static time_t s_last_remind_ts = 0;

/* 提醒回合进行中标志：注入后置 true。语音回到 LISTENING 说明
 * 播报完成，此时清标志并做待机收尾（恢复隐藏表情页） */
static bool s_remind_in_flight = false;

/* 本次提醒是否从待机唤醒的表情页：播报完成后恢复隐藏 */
static bool s_remind_woke_page = false;

/* 当前是否处于表情模式：低电量人格化提醒为表情模式专属，手表
 * 模式保持原提示音行为。由两个 watch_switch_to_* 函数维护。 */
static bool s_expression_mode = true;

/* ── 电量/充电主动提醒（注入 E2E 模型） ─────────────────────────── */

/* 提醒类型 */
enum battery_remind_kind_e
{
  BATTERY_REMIND_LOW,     /* 低电量提醒 */
  BATTERY_REMIND_CHARGED, /* 充电满血报喜 */
};

/**
 * @brief 电量/充电主动提醒（提示词注入 E2E 模型）
 *
 *  状态门（全部满足才注入，否则静默跳过、下轮检测再试）：
 *   - 语音通道 LISTENING（唤醒系统就绪且空闲，开机/对话中跳过）；
 *   - 未禁麦（短按禁麦期间不打扰，解除后下轮触发）；
 *   - 无预设音频在播（扬声器被占用时跳过）。
 *
 *  注入后由 conversation_thread 按完整对话回合消费：LLM 生成
 *  不固定文案 → TTS 播报 → 状态机自恢复，不打断任何进行中的
 *  流程。固定表情在注入前预设（sick/proud），提示词禁止工具
 *  调用与情感关键词，模型不会覆盖表情。
 *
 * @param kind 提醒类型
 * @param soc  当前电量（仅低电量提醒使用）
 */
static void battery_proactive_remind(int kind, uint8_t soc)
{
#ifdef CONFIG_AI_AGENT_EMOTION_TOY
  if (!voice_channel_is_listening())
    {
      PAGE_LOG("[BATTERY] remind deferred (voice not listening)");
      return;
    }

  if (voice_channel_is_mic_muted())
    {
      PAGE_LOG("[BATTERY] remind deferred (mic muted)");
      return;
    }

  if (tool_emotion_audio_is_playing())
    {
      PAGE_LOG("[BATTERY] remind deferred (audio playing)");
      return;
    }

  /* 低电量提醒按固定周期（60 秒）重复，充电报喜只报一次 */
  if (kind == BATTERY_REMIND_LOW
      && time(NULL) - s_last_remind_ts < WATCH_BATTERY_REMIND_INTERVAL_SEC)
    {
      return;
    }

  /* 待机场景（180s 无交互表情页已隐藏）→ 唤醒表情页播报 */
  if (watch_expression_page_is_hidden())
    {
      watch_expression_page_show();
      s_remind_woke_page = true;
      PAGE_LOG("[BATTERY] standby → show page for remind");
    }

  /* 提示词含中文（UTF-8 每字 3 字节），192 字节余量充足 */
  char prompt[192];
  if (kind == BATTERY_REMIND_LOW)
    {
      snprintf(prompt, sizeof(prompt), BATTERY_LOW_PROMPT_INJECT,
               (unsigned)soc);
      /* 表情不在此切换：保持当前脸，TTS 即将播放时由 agent 层
       * 一次性切换 sick/proud，播完恢复 listening。 */
      s_low_power_reminded = true;
    }
  else
    {
      snprintf(prompt, sizeof(prompt), "%s", BATTERY_CHARGED_PROMPT_INJECT);
      s_low_power_reminded = false;
    }

  /* 播报期间保持固定表情，不被 speaking 脸抢占（one-shot） */
  voice_channel_suppress_speaking_face(true);

  if (voice_channel_inject_prompt(prompt) == 0)
    {
      PAGE_LOG("[BATTERY] remind injected (kind=%d): %s", kind, prompt);
      s_last_remind_ts = time(NULL);
      s_remind_in_flight = true;
    }
#else
  (void)kind;
  (void)soc;
#endif
}

/**
 * @brief 获取当前电池电量百分比
 *
 *  封装 AXP2101 驱动的 axp2101_get_pmu_soc()，向应用层提供
 *  统一的电量查询接口。
 *
 * @return uint8_t 电池电量百分比（0-100），读取异常返回0
 */
uint8_t watch_battery_get_level(void)
{
  return axp2101_get_pmu_soc();
}

/**
 * @brief 电量监控定时器回调（每 10 秒）
 *
 *  1. 上一轮注入的提醒若仍在播报（语音未回到 LISTENING），只做
 *     收尾检查，本周期不再注入新提醒。
 *  2. 电量低于阈值：表情模式注入提示词（周期性重复提醒，间隔
 *     见 WATCH_BATTERY_REMIND_INTERVAL_SEC）；手表模式保留原生
 *     最高优先级提示音。
 *  3. 电量恢复：重置提醒计时与报喜资格。
 *
 *  状态门（is_listening / 未禁麦 / 无预设音频）在
 *  battery_proactive_remind 内部检查，不满足时静默跳过，
 *  下个周期再试——不打断 ASR、TTS、mic、扬声器的任何流程。
 */
static void battery_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  /* 提醒回合收尾：语音回到 LISTENING 说明播报完成。
   *  待机场景（从隐藏状态唤醒的表情页）在唤醒门仍关闭
   *  （用户未交互）时恢复隐藏，回到待机。 */
  if (s_remind_in_flight)
    {
      if (!voice_channel_is_listening())
        {
          return;  /* 回合未完成，下周期再收尾 */
        }

      if (s_remind_woke_page)
        {
          if (voice_channel_is_wake_gated())
            {
              watch_expression_page_hide();
              PAGE_LOG("[BATTERY] remind done, back to standby");
            }
          else
            {
              /* 播报后用户已唤醒交互：页面交给正常流程管理 */
              PAGE_LOG("[BATTERY] remind done, user took over");
            }
          s_remind_woke_page = false;
        }
      s_remind_in_flight = false;
      return;
    }

  uint8_t soc = watch_battery_get_level();

  /* 充电中（非放电，即插着充电器）不提醒「要充电」：刚插上充电
   * 时电量可能仍低于阈值，但正在补电，无需打扰；充满停充时电量
   * 已恢复。仅放电状态（未插电）才提醒。 */
  bool plugged_in = (axp2101_get_pmu_charge_status() != 2);

  if (soc < WATCH_BATTERY_LOW_THRESHOLD && !plugged_in)
    {
#ifdef CONFIG_AI_AGENT_EMOTION_TOY
      if (s_expression_mode)
        {
          battery_proactive_remind(BATTERY_REMIND_LOW, soc);
        }
      else
#endif
        {
          /* 非表情模式：原生硬提示音（最高优先级，自带 30s 去重） */
          tool_system_alert_play(15, 0);
        }

      syslog(LOG_WARNING,
             "[BATTERY] 电量过低: 当前电量 %u%%, 请及时充电",
             (unsigned int)soc);
    }
  else
    {
      /* 电量已恢复：重置提醒周期与报喜资格 */
      s_last_remind_ts = 0;
      s_low_power_reminded = false;
    }
}

/**
 * @brief 充电状态轮询回调（1秒）
 *
 *  检测 0→1 跳变（插入充电器）。仅当此前已播过低电量提醒
 *  （s_low_power_reminded）且处于表情模式时触发「满血报喜」：
 *  注入提示词让 E2E 模型生成不固定报喜文案。状态门不满足时
 *  静默跳过本次报喜但保留资格（下次插电仍可触发）。
 */
static void charge_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  /* 6:5 位=01 表示「正在充电」（standby=00 待机/充满停充、10 放电）。
   * 满电时插上充电器（standby）不触发报喜——与表盘图标用 VBUS 位
   * 判定不同：图标要求「插着就显示闪电」，报喜要求「真的在充」。 */
  bool charging = (axp2101_get_pmu_charge_status() == 1);

  if (charging && !s_prev_charging)
    {
      PAGE_LOG("[BATTERY] charger plugged in");
#ifdef CONFIG_AI_AGENT_EMOTION_TOY
      if (s_expression_mode && s_low_power_reminded)
        {
          /* 上一轮提醒还在播报时不叠加注入（状态门在
           * battery_proactive_remind 内也会拦截，这里提前跳过
           * 以免资格被误消耗）。 */
          if (!s_remind_in_flight)
            {
              battery_proactive_remind(BATTERY_REMIND_CHARGED, 0);
            }
        }
#endif
    }

  s_prev_charging = charging;
}

/**
 * @brief 启动电池电量周期监控
 *
 *  在 LVGL 事件循环中创建定时器：电量检测每 10 秒一次、充电
 *  状态轮询每 1 秒一次。开机阶段状态门（voice 未就绪）自动
 *  拦截至唤醒系统进入 LISTENING，无需额外延迟。重复调用安全：
 *  已存在定时器时直接返回。
 */
void watch_battery_check_start(void)
{
  if (s_battery_timer != NULL)
    {
      return;
    }

  s_battery_timer = lv_timer_create(battery_timer_cb,
                                    WATCH_BATTERY_CHECK_INTERVAL_MS,
                                    NULL);

  /* 充电状态轮询（1秒）：「低电量提醒后插电报喜」依赖它。
   * 初始值取当前状态，避免启动时已在充电被误判为「刚插入」。 */
  s_prev_charging = (axp2101_get_pmu_charge_status() == 1);
  if (s_charge_timer == NULL)
    {
      s_charge_timer = lv_timer_create(charge_timer_cb,
                                        WATCH_CHARGE_CHECK_INTERVAL_MS,
                                        NULL);
    }
}

/**
 * @brief 停止电池电量周期监控
 */
void watch_battery_check_stop(void)
{
  if (s_battery_timer != NULL)
    {
      lv_timer_del(s_battery_timer);
      s_battery_timer = NULL;
    }

  if (s_charge_timer != NULL)
    {
      lv_timer_del(s_charge_timer);
      s_charge_timer = NULL;
    }

  /* 重置提醒状态：下次 start 重新计时 */
  s_last_remind_ts = 0;
  s_remind_in_flight = false;
  s_remind_woke_page = false;
  s_low_power_reminded = false;
}

/* ── Vendor Watch UI 切换接口 ──────────────────────────────────────── */

/* 外部声明 - vendor watch UI 入口（app_watch 模块） */
extern int vw_launcher_init(lv_obj_t *parent);
extern void vw_launcher_deinit(void);
extern void vw_resource_init(void);
/* 自动熄屏开关（app_watch display_control）：表情模式永不熄屏，
 * 进入手表模式恢复自动熄屏 */
extern void display_timeout_disable(void);
extern void display_timeout_enable(void);

/**
 * @brief 切换到 vendor 手表界面
 *
 * 清理当前表情页面资源，初始化 vendor watch UI（表盘 + Fragment 页面系统）。
 * 调用后原表情页面不可恢复。
 *
 * @param parent LVGL 屏幕对象（通常传 lv_scr_act()）
 * @return 0 成功，负值失败
 */
int watch_switch_to_watch_app(lv_obj_t *parent)
{
  /* 0. 记录模式：低电量人格化语音为表情模式专属，手表模式回退
   *    原生提示音 */
  s_expression_mode = false;

  /* 1. 恢复手表模式的自动熄屏（表情模式为永不熄屏） */
  display_timeout_enable();

  /* 1. 清理现有表情页面 */
  if (s_page_root != NULL)
    {
      watch_expression_page_deinit(s_page_root);
    }

  /* 2. 清理恢复定时器 */
  if (s_restore_timer != NULL)
    {
      lv_timer_del(s_restore_timer);
      s_restore_timer = NULL;
    }

  /* 3. 初始化 vendor 资源系统（FreeType 字体等，已有重复调用保护） */
  vw_resource_init();

  /* 4. 启动 vendor 手表 UI（表盘 + 页面系统） */
  return vw_launcher_init(parent);
}

/**
 * @brief 切换到表情（潮玩）界面
 *
 * 清理 vendor 手表 UI 资源，初始化表情页面。
 * 调用后原手表页面不可恢复。
 *
 * @param parent LVGL 屏幕对象（通常传 lv_scr_act()）
 * @return 0 成功，负值失败
 */
int watch_switch_to_expression_app(lv_obj_t *parent)
{
  /* 1. 清理 vendor 手表 UI 全部资源 */
  vw_launcher_deinit();

  /* 2. 记录模式：切回表情模式，恢复人格化主动提示 */
  s_expression_mode = true;

  /* 3. 表情模式永不熄屏：挂起自动熄屏（手表模式切换过来时熄屏
   * 定时器仍在运行）。放在 vw_launcher_deinit() 之后，避免小通AI
   * 页面的删除回调 (tong_page_deleted_cb → display_timeout_enable)
   * 把禁用标志冲掉。与语音侧 180s 空闲待机互不影响。 */
  display_timeout_disable();

  /* 3. 初始化表情页面 */
  s_page_root = watch_expression_page_init(parent);
  if (s_page_root == NULL)
    {
      return -1;
    }

  return 0;
}

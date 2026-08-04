/****************************************************************************
 * apps/watch/apps/launcher/launcher.c
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
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "launcher.h"
#include "../boot/boot_logo.h"
#include "../boot/boot_animation.h"
#include "../common/watch_pages.h"
#include "../settings/settings_wifi.h"
#include "voice/voice_channel.h"

/* 调试打印开关：menuconfig 打开 CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG 后生效。
 * 默认关闭：无 USB 主机时控制台 FIFO 写满会阻塞系统。
 */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define WATCH_DBG_LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#else
#  define WATCH_DBG_LOG(fmt, ...)
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOGO_DISPLAY_TIME      1000   /* 开机logo显示时间(ms) */
#define ANIMATION_DISPLAY_TIME 9000   /* 开机动画最长显示时间(ms)，超时强制切换 */
#define ANIM_CHECK_PERIOD_MS   100    /* 动画播放完成检测周期(ms) */

/* ai_agent 自启动参数（与 packages/ai_agent Makefile 配置对齐） */
#define AGENT_TASK_PRIORITY    100
#define AGENT_TASK_STACKSIZE   32768

/****************************************************************************
 * Private Data
 ****************************************************************************/

typedef enum
{
  STATE_INIT,            /* 初始状态 */
  STATE_SHOW_LOGO,       /* 显示logo状态 */
  STATE_SHOW_ANIM,       /* 显示动画状态 */
  STATE_SHOW_CLOCK,      /* 显示手表界面状态 */
  STATE_DONE             /* 完成状态 */
} launcher_state_t;

static launcher_state_t current_state = STATE_INIT; /* 当前状态 */
static lv_obj_t *content_area = NULL;           /* 内容区域 */
static lv_obj_t *current_obj = NULL;            /* 当前显示的对象 */
static uint32_t anim_wait_ms = 0;               /* 动画已等待时间(ms) */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void state_timer_cb(lv_timer_t *timer);
static void anim_check_timer_cb(lv_timer_t *timer);

/* 空闲超时标志：voice_channel 线程设置，LVGL 定时器轮询并执行 hide */
static volatile bool g_idle_timeout_pending = false;

static void on_idle_timeout(void)
{
  g_idle_timeout_pending = true;
}

static void idle_timeout_poll_cb(lv_timer_t *timer)
{
  if (g_idle_timeout_pending) {
    g_idle_timeout_pending = false;
    watch_expression_page_hide();
  }
}

/* 唤醒词检测回调：由 voice_channel conversation_thread 调用 */
static void on_wake_detected(void)
{
  /* 在 LVGL 主线程中显示表情 */
  lv_async_call((lv_async_cb_t)watch_expression_page_show, NULL);
}

/**
 * @brief 后台拉起 ai_agent 任务（auto 模式：无 CLI，网络就绪后自动开启关键字唤醒）
 *
 * FLAT build 下 watch 与 ai_agent 同镜像，直接链接 ai_agent_main。
 * task_create 非阻塞，不影响 LVGL 主线程。
 */
static void agent_autostart(void)
{
  static bool started = false;   /* 防重入 */
  extern int ai_agent_main(int argc, char *argv[]);

  if (started)
    {
      return;
    }
  started = true;

  /* 注册唤醒词回调 + 空闲超时回调 + LVGL 轮询定时器 */
  voice_channel_set_wake_notify(on_wake_detected);
  voice_channel_set_idle_timeout_cb(on_idle_timeout);
  lv_timer_create(idle_timeout_poll_cb, 2000, NULL);

  static char *agent_argv[] = { "auto", NULL };
  int pid = task_create("ai_agent", AGENT_TASK_PRIORITY,
                        AGENT_TASK_STACKSIZE, ai_agent_main, agent_argv);
  if (pid < 0)
    {
      WATCH_DBG_LOG("[LAUNCHER] ERROR: ai_agent task_create failed: %d", pid);
    }
  else
    {
      WATCH_DBG_LOG("[LAUNCHER] ai_agent started (pid=%d, auto mode)", pid);
    }
}

/**
 * @brief 延迟唤醒启动定时器：等 WiFi + ai_agent 都就绪后再启动唤醒监听
 */
static void deferred_wake_start_timer_cb(lv_timer_t *timer)
{
    extern bool voice_channel_is_ready(void);
    extern int voice_channel_start_wake(void);
    static int attempts = 0;

    bool wifi_ok = settings_wifi_is_connected();
    bool voice_ok = voice_channel_is_ready();

    WATCH_DBG_LOG("[LAUNCHER] wake start check #%d: wifi=%d voice=%d",
                  ++attempts, wifi_ok, voice_ok);

    if (wifi_ok && voice_ok) {
        WATCH_DBG_LOG("[LAUNCHER] both ready, starting wake-word listening");
        voice_channel_start_wake();
        lv_timer_del(timer);  /* stop this timer */
        return;
    }

    /* Safety: give up after 180 attempts (180 seconds) */
    if (attempts >= 180) {
        WATCH_DBG_LOG("[LAUNCHER] wake start timeout, giving up (wifi=%d voice=%d)",
                      wifi_ok, voice_ok);
        lv_timer_del(timer);
    }
}

/**
 * @brief 切换到下一个状态
 */
static void goto_next_state(void)
{
  switch (current_state)
    {
      case STATE_INIT:
        /* 切换到显示logo状态 */
        current_state = STATE_SHOW_LOGO;
        if (current_obj != NULL)
          {
            lv_obj_del(current_obj);
          }
        current_obj = boot_logo_init(content_area);
        lv_task_handler();

        lv_timer_t *timer1 = lv_timer_create(state_timer_cb, LOGO_DISPLAY_TIME, NULL);
        lv_timer_set_repeat_count(timer1, 1);
        break;

      case STATE_SHOW_LOGO:
        /* 切换到显示动画状态 */
        current_state = STATE_SHOW_ANIM;

        /* 销毁logo并显示动画 */
        boot_logo_deinit(current_obj);
        current_obj = boot_animation_init(content_area);
        lv_task_handler();

        /* 动画播放期间后台拉起 ai_agent（初始化 config/LLM/ASR/TTS）
         * 到动画结束时 ai_agent 大概率已就绪 */
        agent_autostart();

        /* 周期性检测动画是否播完，播完立即切换（最长等待 ANIMATION_DISPLAY_TIME） */
        anim_wait_ms = 0;
        lv_timer_create(anim_check_timer_cb, ANIM_CHECK_PERIOD_MS, NULL);
        break;

      case STATE_SHOW_ANIM:
        /* 动画播放完成，切换到表情页面 */
        current_state = STATE_SHOW_CLOCK;

        /* 销毁动画 */
        boot_animation_deinit(current_obj);
        current_obj = NULL;

        /* [vendor watch UI 已禁用] 恢复到原来的表情页面 */
        /* watch_switch_to_watch_app(content_area); */
        current_obj = (lv_obj_t *)watch_expression_page_init(content_area);

        /* 启动延迟唤醒定时器：周期性检测WiFi和ai_agent是否就绪 */
        lv_timer_create(deferred_wake_start_timer_cb, 1000, NULL);

        lv_task_handler();
        break;

      case STATE_SHOW_CLOCK:
        /* 手表界面持续运行，进入完成状态 */
        current_state = STATE_DONE;
        break;

      default:
        break;
    }
}

/**
 * @brief 状态定时器回调函数
 */
static void state_timer_cb(lv_timer_t *timer)
{
  goto_next_state();
}

/**
 * @brief 动画播放完成检测定时器回调
 *
 * GIF播完最后一帧（boot_animation_is_finished）后立即切换到表情页面；
 * 超过 ANIMATION_DISPLAY_TIME 未播完则强制切换，避免一直停留在动画页。
 */
static void anim_check_timer_cb(lv_timer_t *timer)
{
  anim_wait_ms += ANIM_CHECK_PERIOD_MS;
  if (boot_animation_is_finished() || anim_wait_ms >= ANIMATION_DISPLAY_TIME)
    {
      lv_timer_del(timer);
      goto_next_state();
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief 初始化并运行开机logo → 开机动画流程
 *
 * @param parent 父容器对象
 * @return int 成功返回0，失败返回负值
 */
int launcher_init(lv_obj_t *parent)
{
  /* 创建内容区域 */
  content_area = lv_obj_create(parent);
  lv_obj_set_size(content_area, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_border_width(content_area, 0, 0);
  lv_obj_set_style_bg_color(content_area, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(content_area, 0, 0);
  lv_obj_clear_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(content_area, 0, 0);
  lv_obj_center(content_area);

  /* 启动开机流程 */
  current_state = STATE_INIT;
  goto_next_state();

  return 0;
}

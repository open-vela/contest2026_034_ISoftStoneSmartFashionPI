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
#include <syslog.h>

#include "launcher.h"
#include "../boot/boot_logo.h"
#include "../boot/boot_animation.h"
#include "../boot/watch_boot_logo.h"
#include "../boot/watch_boot_animation.h"
#include "../common/watch_pages.h"
#include "../common/ui_mode_manager.h"
#include "../volume_control/volume_control.h"
#include "apps/settings/settings_wifi.h"  /* 使用手表UI的WiFi设置头文件 */
#include "voice/voice_channel.h"

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define WATCH_DBG_LOG(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOGO_DISPLAY_TIME      5000   /* 开机logo最长显示时间(ms)，超时强制切换 */
#define ANIMATION_DISPLAY_TIME 5000   /* 开机动画最长显示时间(ms)，超时强制切换 */
#define ANIM_CHECK_PERIOD_MS   100    /* 动画播放完成检测周期(ms) */
#define LOGO_CHECK_PERIOD_MS   100    /* logo播放完成检测周期(ms) */

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
static uint32_t logo_wait_ms = 0;               /* logo已等待时间(ms) */
static ui_mode_t g_boot_ui_mode = UI_MODE_EXPRESSION; /* 本次开机的UI模式 */
static int s_wake_attempts = 0;  /* deferred_wake_start_timer 重试计数(运行时切换后可重置) */
static lv_timer_t *s_wake_timer = NULL;   /* deferred_wake_start 定时器句柄（停止语音时删除） */
static bool g_agent_started = false;      /* ai_agent 任务已拉起（防重入，停止时重置） */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void anim_check_timer_cb(lv_timer_t *timer);
static void logo_check_timer_cb(lv_timer_t *timer);

/* 空闲超时标志：voice_channel 线程设置，LVGL 定时器轮询并执行 hide */
static volatile bool g_idle_timeout_pending = false;

/* 小通AI活跃状态检查（避免对话期间黑屏） */
extern bool tong_ai_is_active(void);

static void on_idle_timeout(void)
{
  /* 小通AI页面打开时，不隐藏表情页（tong_ai 有自己的音频通路，
   * voice_channel 检测不到其语音活动，会误判为空闲） */
  if (tong_ai_is_active()) {
    return;
  }
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
  extern int ai_agent_main(int argc, char *argv[]);

  if (g_agent_started)
    {
      return;
    }
  g_agent_started = true;

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

    bool wifi_ok = settings_wifi_is_connected();
    bool voice_ok = voice_channel_is_ready();

    WATCH_DBG_LOG("[LAUNCHER] wake start check #%d: wifi=%d voice=%d",
                  ++s_wake_attempts, wifi_ok, voice_ok);

    if (wifi_ok && voice_ok) {
        WATCH_DBG_LOG("[LAUNCHER] both ready, starting wake-word listening");
        voice_channel_start_wake();
        if (timer == s_wake_timer) {
            s_wake_timer = NULL;
        }
        lv_timer_del(timer);  /* stop this timer */
        return;
    }

    /* After 10s without WiFi (10 attempts × 1s), play no-network alert once */
    if (s_wake_attempts == 10) {
        tool_system_alert_play(16, 0);
    }

    /* Safety: give up after 180 attempts (180 seconds) */
    if (s_wake_attempts >= 180) {
        WATCH_DBG_LOG("[LAUNCHER] wake start timeout, giving up (wifi=%d voice=%d)",
                      wifi_ok, voice_ok);
        if (timer == s_wake_timer) {
            s_wake_timer = NULL;
        }
        lv_timer_del(timer);
    }
}

/**
 * @brief 运行时切换到表情模式后，启动 ai_agent + 唤醒词监听
 *
 * 冷启动由 launcher 状态机负责；运行时切换（ui_mode_switch_runtime）
 * 跳过了启动流程，需由本函数补齐：拉起 ai_agent + 创建 deferred_wake_start_timer。
 * agent_autostart 内部有 static bool started 防重入，重复调用安全。
 */
void launcher_start_voice_for_expression(void)
{
  WATCH_DBG_LOG("[LAUNCHER] start voice for expression (runtime switch)");
  agent_autostart();
  s_wake_attempts = 0;
  s_wake_timer = lv_timer_create(deferred_wake_start_timer_cb, 1000, NULL);
}

/**
 * @brief 运行时切换到手表模式前，停止表情模式的后台任务
 *
 * 停止语音会话线程、断开 E2E 语音链路，并请求 ai_agent 任务退出
 *（同步等待其 teardown 完成，避免切回表情模式时新旧实例并存）。
 * 与冷启动进手表模式的行为对齐：手表模式不运行 ai_agent。
 * 由 ui_mode_switch_runtime(UI_MODE_WATCH) 调用；再次切回表情模式时
 * 由 launcher_start_voice_for_expression() 重新拉起。
 */
void launcher_stop_voice_for_watch(void)
{
  extern int voice_channel_stop(void);
  extern void volc_e2e_stop(void);
  extern void agent_request_shutdown(void);
  extern bool agent_is_running(void);

  WATCH_DBG_LOG("[LAUNCHER] stop voice+agent for watch (runtime switch)");

  /* 取消延迟唤醒定时器，避免 agent 停止后又被它拉起唤醒监听 */
  if (s_wake_timer != NULL)
    {
      lv_timer_del(s_wake_timer);
      s_wake_timer = NULL;
    }

  /* 停语音会话线程：voice_channel_stop 内部 capture_close 解除
   * read 阻塞并 join 会话线程；未运行时返回错误，忽略 */
  voice_channel_stop();

  /* 断开 E2E 语音链路（内部有锁，未建立时安全返回） */
  volc_e2e_stop();

  /* 请求 ai_agent 退出并同步等待 teardown（主循环 1s 轮询粒度，
   *  teardown 含 500ms 的线程退出等待） */
  agent_request_shutdown();
  int wait_ms = 0;
  while (agent_is_running() && wait_ms < 5000)
    {
      usleep(100 * 1000);
      wait_ms += 100;
    }
  if (agent_is_running())
    {
      WATCH_DBG_LOG("[LAUNCHER] WARN: ai_agent teardown timeout");
    }

  /* 允许切回表情模式时重新拉起 */
  g_agent_started = false;
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

        /* 尽早读取UI模式，选择对应的开机logo */
        g_boot_ui_mode = ui_mode_load();
        ui_mode_set_current(g_boot_ui_mode);
        WATCH_DBG_LOG("[LAUNCHER] Boot UI mode: %d (0=expression, 1=watch)",
                      (int)g_boot_ui_mode);

        /* 恢复上次持久化的音量（/mnt/spif/volume.json），
         * 避免重启后回退到板级默认值 */
        watch_volume_restore();

        if (current_obj != NULL)
          {
            lv_obj_del(current_obj);
          }

        if (g_boot_ui_mode == UI_MODE_WATCH)
          {
            current_obj = watch_boot_logo_init(content_area);
          }
        else
          {
            current_obj = boot_logo_init(content_area);
          }
        lv_task_handler();

        /* 周期性检测logo GIF是否播完，播完立即切换（最长等待 LOGO_DISPLAY_TIME） */
        logo_wait_ms = 0;
        lv_timer_create(logo_check_timer_cb, LOGO_CHECK_PERIOD_MS, NULL);
        break;

      case STATE_SHOW_LOGO:
        /* 切换到显示动画状态 */
        current_state = STATE_SHOW_ANIM;

        /* 销毁logo并显示动画（根据UI模式选择对应的动画） */
        if (g_boot_ui_mode == UI_MODE_WATCH)
          {
            watch_boot_logo_deinit(current_obj);
            current_obj = watch_boot_animation_init(content_area);
          }
        else
          {
            boot_logo_deinit(current_obj);
            current_obj = boot_animation_init(content_area);
          }
        lv_task_handler();

        /* 动画播放期间后台拉起 ai_agent（仅表情UI模式需要）
         * 手表UI模式不需要 ai_agent，跳过以加快开机速度 */
        if (g_boot_ui_mode == UI_MODE_EXPRESSION)
          {
            agent_autostart();
          }

        /* 周期性检测动画是否播完，播完立即切换（最长等待 ANIMATION_DISPLAY_TIME） */
        anim_wait_ms = 0;
        lv_timer_create(anim_check_timer_cb, ANIM_CHECK_PERIOD_MS, NULL);
        break;

      case STATE_SHOW_ANIM:
        /* 动画播放完成，切换到主界面 */
        current_state = STATE_SHOW_CLOCK;

        /* 销毁动画（根据UI模式选择对应的销毁函数） */
        if (g_boot_ui_mode == UI_MODE_WATCH)
          {
            watch_boot_animation_deinit(current_obj);
          }
        else
          {
            boot_animation_deinit(current_obj);
          }
        current_obj = NULL;

        /* 根据UI模式进入对应的主界面 */
        WATCH_DBG_LOG("[LAUNCHER] Entering UI mode: %d (0=expression, 1=watch)",
                      (int)g_boot_ui_mode);

        if (g_boot_ui_mode == UI_MODE_WATCH)
          {
            /* 切换到 vendor watch UI */
            watch_switch_to_watch_app(content_area);
          }
        else
          {
            /* 默认：表情页面 */
            current_obj = (lv_obj_t *)watch_expression_page_init(
              content_area);
          }

        /* 启动电池电量周期监控：开机立即检测一次，之后每60秒检测，
         * 电量低于20%时播报低电量语音（watch_battery_check_start 内部
         * 有重复调用保护） */
        watch_battery_check_start();

        /* 仅表情UI模式启动延迟唤醒定时器（手表UI不需要语音唤醒） */
        if (g_boot_ui_mode == UI_MODE_EXPRESSION)
          {
            s_wake_timer = lv_timer_create(deferred_wake_start_timer_cb, 1000, NULL);
          }

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
 * @brief 动画播放完成检测定时器回调
 *
 * GIF播完最后一帧（boot_animation_is_finished）后立即切换到表情页面；
 * 超过 ANIMATION_DISPLAY_TIME 未播完则强制切换，避免一直停留在动画页。
 */
static void anim_check_timer_cb(lv_timer_t *timer)
{
  anim_wait_ms += ANIM_CHECK_PERIOD_MS;

  /* 根据UI模式检查对应的动画是否播放完成 */
  bool finished = (g_boot_ui_mode == UI_MODE_WATCH)
    ? watch_boot_animation_is_finished()
    : boot_animation_is_finished();

  if (finished || anim_wait_ms >= ANIMATION_DISPLAY_TIME)
    {
      lv_timer_del(timer);
      goto_next_state();
    }
}

/**
 * @brief logo播放完成检测定时器回调
 *
 * GIF播完最后一帧（boot_logo_is_finished）后立即切换到动画页面；
 * 超过 LOGO_DISPLAY_TIME 未播完则强制切换，避免一直停留在logo页。
 */
static void logo_check_timer_cb(lv_timer_t *timer)
{
  logo_wait_ms += LOGO_CHECK_PERIOD_MS;

  /* 根据UI模式检查对应的logo是否播放完成 */
  bool finished = (g_boot_ui_mode == UI_MODE_WATCH)
    ? watch_boot_logo_is_finished()
    : boot_logo_is_finished();

  if (finished || logo_wait_ms >= LOGO_DISPLAY_TIME)
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

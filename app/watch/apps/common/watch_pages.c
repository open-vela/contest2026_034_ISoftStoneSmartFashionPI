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

#include <nuttx/power/axp2101.h>

#include "watch_pages.h"
#include "../home_control/home_control.h"
#include "../volume_control/volume_control.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PAGE_TAG "[PAGE] "

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#define PAGE_LOG(fmt, ...)  printf(PAGE_TAG fmt "\n", ##__VA_ARGS__)
#else
#define PAGE_LOG(fmt, ...)
#endif

#define MAX_PAGE_STACK_SIZE 16

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 表情页面运行时状态 */
static lv_obj_t   *s_page_root    = NULL;  /* 页面根容器 */
static lv_obj_t   *s_expr_gif     = NULL;  /* 表情GIF控件 */
static int         s_curr_index   = 0;     /* 当前表情索引 */
static int         s_expr_count   = 0;     /* 表情图片总数 */

/* 页面栈：用于跟踪二级/三级子页面 */
static lv_obj_t *s_page_stack[MAX_PAGE_STACK_SIZE];
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

void watch_expression_page_deinit(lv_obj_t *page_obj)
{
  /* 重置状态 */
  s_expr_gif   = NULL;
  s_curr_index = 0;
  s_expr_count = 0;

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
            break;
        }
    }
}

/* 设置默认表情（开机后显示 excited，不启动轮播） */
void watch_expression_page_set_default(void)
{
    watch_expression_page_set_face("excited", 0);
}

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

    PAGE_LOG("set_face: %s -> index %d, duration=%dms",
             face_id, gif_index, duration_ms);

    /* 记录恢复时长（LVGL线程会读取并创建定时器） */
    s_restore_duration_ms = duration_ms;

    /* 在 LVGL 线程中执行表情切换 + 创建恢复定时器 */
    lv_async_call(set_face_async_cb, (void*)(intptr_t)gif_index);

    return 0;
}

/* ── 电池电量监控 ──────────────────────────────────────────────── */

/* 低电量告警阈值（百分比） */
#define WATCH_BATTERY_LOW_THRESHOLD   20

/* 电量周期检测间隔（毫秒） */
#define WATCH_BATTERY_CHECK_INTERVAL_MS  60000  /* 60秒检测一次 */

/* 电量监控定时器 */
static lv_timer_t *s_battery_timer = NULL;

/* 低电量告警去重标志：仅在「正常→低电量」跳变时输出一次告警 */
static bool s_battery_low_warned = false;

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
 * @brief 检测电池电量并在低电量时通过 syslog 输出告警
 *
 *  当电量低于 WATCH_BATTERY_LOW_THRESHOLD（默认20%）时，通过
 *  syslog(LOG_WARNING) 输出当前电量值及「电量过低」警告信息。
 *  为避免日志刷屏，仅在状态由正常跳变为低电量时输出一次告警；
 *  电量恢复后会重置标志，下次再次低于阈值时重新告警。
 *
 * @return bool 电量过低返回 true，否则返回 false
 */
bool watch_battery_check_low_warning(void)
{
  uint8_t soc = watch_battery_get_level();

  if (soc < WATCH_BATTERY_LOW_THRESHOLD)
    {
      if (!s_battery_low_warned)
        {
          syslog(LOG_WARNING,
                 "[BATTERY] 电量过低: 当前电量 %u%%, 请及时充电",
                 (unsigned int)soc);
          s_battery_low_warned = true;
        }
      return true;
    }
  else
    {
      /* 电量已恢复，重置告警标志，便于下次低电量时再次告警 */
      s_battery_low_warned = false;
      return false;
    }
}

/**
 * @brief 电量监控定时器回调
 */
static void battery_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  watch_battery_check_low_warning();
}

/**
 * @brief 启动电池电量周期监控
 *
 *  在 LVGL 事件循环中创建定时器，按
 *  WATCH_BATTERY_CHECK_INTERVAL_MS（默认60秒）间隔周期性检测
 *  电池电量。启动时立即执行一次检测。重复调用安全：已存在
 *  定时器时直接返回，避免重复创建。
 */
void watch_battery_check_start(void)
{
  if (s_battery_timer != NULL)
    {
      return;
    }

  /* 启动时立即检测一次电量 */
  watch_battery_check_low_warning();

  s_battery_timer = lv_timer_create(battery_timer_cb,
                                     WATCH_BATTERY_CHECK_INTERVAL_MS,
                                     NULL);
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
}

/* ── Vendor Watch UI 切换接口 ──────────────────────────────────────── */

/* 外部声明 - vendor watch UI 入口（app_watch 模块） */
extern int vw_launcher_init(lv_obj_t *parent);
extern void vw_resource_init(void);

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

  /* 3. 初始化 vendor 资源系统（FreeType 字体等） */
  vw_resource_init();

  /* 4. 启动 vendor 手表 UI（表盘 + 页面系统） */
  return vw_launcher_init(parent);
}

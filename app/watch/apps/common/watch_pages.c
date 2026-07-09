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
#include <string.h>
#include <unistd.h>

#include "watch_pages.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PAGE_TAG "[PAGE] "

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
#define PAGE_LOG(fmt, ...)  printf(PAGE_TAG fmt "\n", ##__VA_ARGS__)
#else
#define PAGE_LOG(fmt, ...)
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 表情页面运行时状态 */
static lv_obj_t   *s_page_root    = NULL;  /* 页面根容器 */
static lv_obj_t   *s_expr_gif     = NULL;  /* 表情GIF控件 */
static lv_timer_t *s_switch_timer = NULL;  /* 自动轮播定时器 */
static int         s_curr_index   = 0;     /* 当前表情索引 */
static int         s_expr_count   = 0;     /* 表情图片总数 */

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

  const char *gif_path = watch_resource_get_img_expression(index);
  if (gif_path == NULL)
    {
      PAGE_LOG("ERROR: Failed to get expression path at index %d", index);
      return;
    }

  /* 淡出当前图片 */
  lv_obj_set_style_opa(s_expr_gif, LV_OPA_TRANSP, 0);

  /* 设置GIF文件源 */
  lv_gif_set_src(s_expr_gif, gif_path);

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

/**
 * @brief 自动轮播定时器回调函数
 */
static void switch_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  switch_to_expression(s_curr_index + 1);
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

  /* 等待SD卡就绪 */
  if (!wait_for_sd_ready())
    {
      PAGE_LOG("ERROR: SD card not ready, expression page init failed");
      return NULL;
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

  /* 设置GIF控件背景透明且初始不可见，避免首帧闪烁 */
  lv_obj_set_style_bg_opa(s_expr_gif, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(s_expr_gif, LV_OPA_TRANSP, 0);

  /* 让LVGL处理一次事件循环，确保控件完全初始化 */
  lv_timer_handler();

  /* 加载第一张表情GIF，如果失败则依次尝试后续表情 */
  int i;
  for (i = 0; i < s_expr_count; i++)
    {
      s_curr_index = i;
      const char *first_gif = watch_resource_get_img_expression(s_curr_index);
      if (first_gif == NULL)
        continue;

      lv_gif_set_src(s_expr_gif, first_gif);

      int32_t w = lv_obj_get_width(s_expr_gif);
      int32_t h = lv_obj_get_height(s_expr_gif);

      if (w > 0 && h > 0)
        {
          /* 首张GIF加载成功，重启并执行淡入动画 */
          lv_gif_restart(s_expr_gif);

          lv_anim_t fade_anim;
          lv_anim_init(&fade_anim);
          lv_anim_set_var(&fade_anim, s_expr_gif);
          lv_anim_set_exec_cb(&fade_anim, fade_in_anim_cb);
          lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
          lv_anim_set_time(&fade_anim, WATCH_EXPRESSION_FADE_TIME_MS);
          lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
          lv_anim_start(&fade_anim);
          break;
        }
    }

  /* 启动自动轮播定时器（每10秒切换） */
  s_switch_timer = lv_timer_create(switch_timer_cb,
                                    WATCH_EXPRESSION_SWITCH_INTERVAL_MS,
                                    NULL);

  return s_page_root;
}

void watch_expression_page_deinit(lv_obj_t *page_obj)
{
  /* 停止并删除自动轮播定时器 */
  if (s_switch_timer != NULL)
    {
      lv_timer_del(s_switch_timer);
      s_switch_timer = NULL;
    }

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

void watch_expression_page_next(void)
{
  if (s_switch_timer != NULL)
    {
      lv_timer_reset(s_switch_timer);
    }

  switch_to_expression(s_curr_index + 1);
}

int watch_expression_page_get_current_index(void)
{
  return s_curr_index;
}

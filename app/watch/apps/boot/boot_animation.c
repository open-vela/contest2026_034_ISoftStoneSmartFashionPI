/****************************************************************************
 * vendor/watch/apps/boot/boot_animation.c
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
#include <unistd.h>

#include "boot_animation.h"
#include "../../resource/resource.h"
#include "../common/watch_pages.h"

/* 调试打印开关：menuconfig 打开 CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG 后生效。
 * 默认关闭：无 USB 主机时控制台 FIFO 写满会阻塞系统。
 */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define WATCH_DBG_LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#else
#  define WATCH_DBG_LOG(fmt, ...)
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_gif_obj = NULL;      /* GIF控件 */
static bool s_anim_finished = false;    /* 动画是否已播放完成 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief GIF播放完成回调
 *
 * LVGL GIF解码器在GIF播放完最后一帧后发送LV_EVENT_READY事件，
 * 并自动暂停内部定时器，确保GIF不会重复播放。
 */
static void gif_ready_cb(lv_event_t *e)
{
  (void)e;
  s_anim_finished = true;

  /* 立即暂停 GIF，防止解码器在定时器轮询前重新开始播放 */
  if (s_gif_obj) {
    lv_gif_pause(s_gif_obj);
  }

  WATCH_DBG_LOG("[BOOT] GIF animation finished");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief 初始化并显示开机动画
 *
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的动画对象
 */
lv_obj_t *boot_animation_init(lv_obj_t *parent)
{
  /* 创建黑色背景容器 */
  lv_obj_t *bg = lv_obj_create(parent);
  lv_obj_set_size(bg, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(bg, 0, 0);
  lv_obj_set_style_radius(bg, 0, 0);
  lv_obj_set_style_pad_all(bg, 0, 0);
  lv_obj_center(bg);
  lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

  /* 创建lv_gif控件 */
  s_gif_obj = lv_gif_create(bg);
  lv_obj_center(s_gif_obj);

  /* 注册GIF播放完成事件回调，确保动画只播放一次 */
  lv_obj_add_event_cb(s_gif_obj, gif_ready_cb, LV_EVENT_READY, NULL);

  /* 设置嵌入式GIF数据源并开始播放 */
  const void *gif_src = watch_resource_get_bootlogo_gif();
  if (gif_src) {
    lv_gif_set_src(s_gif_obj, gif_src);
  } else {
    WATCH_DBG_LOG("[BOOT] boot_animation_init: bootlogo gif data is NULL");
  }

  /* 重置状态 */
  s_anim_finished = false;

  //fprintf(stderr, "[BOOT] boot_animation_init: GIF animation started\n");
  return bg;
}

/**
 * @brief 查询开机动画是否已播放完成
 *
 * @return true 表示GIF已播放完最后一帧
 */
bool boot_animation_is_finished(void)
{
  return s_anim_finished;
}

/**
 * @brief 销毁开机动画
 *
 * @param anim_obj 动画对象
 */
void boot_animation_deinit(lv_obj_t *anim_obj)
{
  /* 确保GIF停止播放 */
  if (s_gif_obj) {
    lv_gif_pause(s_gif_obj);
    s_gif_obj = NULL;
  }

  /* 删除页面容器（同时销毁所有子控件） */
  if (anim_obj) {
    lv_obj_del(anim_obj);
  }

  s_anim_finished = false;
}

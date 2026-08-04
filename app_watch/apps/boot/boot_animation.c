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


/* 序列帧数组 - 运行时填充 */
#define FRAME_COUNT 42
static const void *sequence_frames[FRAME_COUNT];
static bool frames_initialized = false;

/* 初始化序列帧数组 */
static void init_sequence_frames(void)
{
    if (frames_initialized) {
    fprintf(stderr, "[BOOT] init_sequence_frames: already initialized\n");
    return;
  }
  char frame_key[16];
  fprintf(stderr, "[BOOT] init_sequence_frames: start\n");

  for(int i = 0; i < FRAME_COUNT; i++)
  {
    snprintf(frame_key, sizeof(frame_key), "power_frame_%02d", i);
    sequence_frames[i] = vw_resource_get_img_power(frame_key);  // PNG 格式（能显示）
  }
  
  frames_initialized = true;
  fprintf(stderr, "[BOOT] init_sequence_frames: end\n");
}

/* 动画状态 */
static int current_frame = 0;
static lv_timer_t *anim_timer = NULL;
static lv_obj_t *anim_image = NULL;

/* 动画回调函数 */
static void animation_timer_cb(lv_timer_t *timer)
{
    if (current_frame < FRAME_COUNT) {
        if (sequence_frames[current_frame]) {
            lv_img_set_src(anim_image, sequence_frames[current_frame]);
        }
        current_frame++;
    } else {
        /* 动画播放完毕，停止定时器 */
        if (anim_timer) {
            lv_timer_del(anim_timer);
            anim_timer = NULL;
        }
    }
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
  /* 初始化序列帧数组 */
  init_sequence_frames();
  
  /* 创建背景 */
  lv_obj_t *bg = lv_obj_create(parent);
  lv_obj_set_size(bg, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0); /* 黑色背景 */
  lv_obj_set_style_border_width(bg, 0, 0);
  lv_obj_set_style_radius(bg, 0, 0);
  lv_obj_center(bg);
  lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

  /* 创建动画图像 */
  anim_image = lv_img_create(bg);
  if (sequence_frames[0]) {
    lv_img_set_src(anim_image, sequence_frames[0]);
  } else {
    fprintf(stderr, "[BOOT] boot_animation_init: sequence_frames[0] is NULL\n");
  }
  lv_obj_center(anim_image);

  /* 重置动画状态 */
  current_frame = 1;

  /* 创建定时器，10帧/秒，100ms每帧 (降低帧率以匹配LittleFS PNG解码速度) */
  anim_timer = lv_timer_create(animation_timer_cb, 100, NULL);

  fprintf(stderr, "[BOOT] boot_animation_init: end\n");
  return bg;
}

/**
 * @brief 销毁开机动画
 * 
 * @param anim_obj 动画对象
 */
void boot_animation_deinit(lv_obj_t *anim_obj)
{
  /* 停止动画定时器 */
  if (anim_timer) {
    lv_timer_del(anim_timer);
    anim_timer = NULL;
  }
  lv_obj_del(anim_obj);
}

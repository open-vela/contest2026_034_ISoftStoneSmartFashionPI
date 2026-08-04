/****************************************************************************
 * vendor/watch/apps/boot/boot_logo.c
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

#include "boot_logo.h"
#include "../../resource/resource.h"
#include "../common/watch_pages.h"


/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief 初始化并显示开机logo
 * 
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的logo对象
 */
lv_obj_t *boot_logo_init(lv_obj_t *parent)
{
  /* 创建背景 */
  lv_obj_t *bg = lv_obj_create(parent);
  lv_obj_set_size(bg, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0); /* 黑色背景 */
  lv_obj_set_style_border_width(bg, 0, 0);
  lv_obj_set_style_radius(bg, 0, 0);
  lv_obj_center(bg);
  lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

  /* 创建logo图像 */
  lv_obj_t *logo = lv_img_create(bg);
  const void *img_src = vw_resource_get_img("isoftstone_logo");
  if (img_src) {
    lv_img_set_src(logo, img_src);
  } else {
    fprintf(stderr, "[BOOT] boot_logo_init: img_src is NULL\n");
  }
  lv_obj_center(logo);
  return bg;
}

/**
 * @brief 销毁开机logo
 * 
 * @param logo_obj logo对象
 */
void boot_logo_deinit(lv_obj_t *logo_obj)
{
  lv_obj_del(logo_obj);
}

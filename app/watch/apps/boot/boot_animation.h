/****************************************************************************
 * vendor/watch/apps/boot/boot_animation.h
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

#ifndef __VENDOR_WATCH_APPS_LAUNCHER_BOOT_ANIMATION_H
#define __VENDOR_WATCH_APPS_LAUNCHER_BOOT_ANIMATION_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief 初始化并显示开机动画
 * 
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的动画对象
 */
lv_obj_t *boot_animation_init(lv_obj_t *parent);

/**
 * @brief 销毁开机动画
 * 
 * @param anim_obj 动画对象
 */
void boot_animation_deinit(lv_obj_t *anim_obj);

/**
 * @brief 查询开机动画是否已播放完成
 *
 * GIF解码器播完最后一帧后通过LV_EVENT_READY置位完成标志。
 *
 * @return true 表示动画已播放完成
 */
bool boot_animation_is_finished(void);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_WATCH_APPS_LAUNCHER_BOOT_ANIMATION_H */

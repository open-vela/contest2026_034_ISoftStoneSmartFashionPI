/****************************************************************************
 * vendor/watch/apps/boot/boot_logo.h
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

#ifndef __VENDOR_WATCH_APPS_LAUNCHER_BOOT_LOGO_H
#define __VENDOR_WATCH_APPS_LAUNCHER_BOOT_LOGO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief 初始化并显示开机logo
 * 
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的logo对象
 */
lv_obj_t *boot_logo_init(lv_obj_t *parent);

/**
 * @brief 查询开机logo GIF是否已播放完成
 *
 * @return true 表示GIF已播放完最后一帧
 */
bool boot_logo_is_finished(void);

/**
 * @brief 销毁开机logo
 *
 * @param logo_obj logo对象
 */
void boot_logo_deinit(lv_obj_t *logo_obj);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_WATCH_APPS_LAUNCHER_BOOT_LOGO_H */

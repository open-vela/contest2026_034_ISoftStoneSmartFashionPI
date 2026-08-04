/****************************************************************************
 * vendor/watch/apps/launcher/app_list.h
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

#ifndef __VENDOR_WATCH_APPS_LAUNCHER_APP_LIST_H
#define __VENDOR_WATCH_APPS_LAUNCHER_APP_LIST_H

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
 * @brief 创建应用列表界面
 * 
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的应用列表对象
 */
lv_obj_t *app_tile_setup(lv_obj_t *parent);

/**
 * @brief 注册应用
 *
 * @param name 应用名称
 * @param icon_src 图标资源指针
 * @param click_cb 点击回调函数
 */
void register_app(const char *name, const lv_image_dsc_t *icon_src, void (*click_cb)(lv_event_t *e));

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_WATCH_APPS_LAUNCHER_APP_LIST_H */

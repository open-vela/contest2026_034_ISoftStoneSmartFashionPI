/****************************************************************************
 * vendor/watch/apps/launcher/launcher.h
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

#ifndef __VENDOR_WATCH_APPS_LAUNCHER_LAUNCHER_H
#define __VENDOR_WATCH_APPS_LAUNCHER_LAUNCHER_H

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
 * @brief 初始化并运行主页流程
 * 
 * @param parent 父容器对象
 * @return int 成功返回0，失败返回负值
 */
int vw_launcher_init(lv_obj_t *parent);

/**
 * @brief 清理 vendor 手表 UI 全部资源（无重启切换用）
 *
 * 停止定时器、清空页面栈、删除 content_area 与表盘对象。
 * 调用后可通过 vw_launcher_init() 重新初始化。
 */
void vw_launcher_deinit(void);

/**
 * @brief 返回主页函数
 *
 * @return int 成功返回0，失败返回负值
 */
int lv_watch_back_go_home(void);

/**
 * @brief 检查当前是否在主页
 *
 * @return int 在主页返回1，否则返回0
 */
int lv_watch_is_home(void);

/**
 * @brief 将页面添加到页面栈
 * 
 * @param page 页面对象
 * @return int 成功返回0，失败返回负值
 */
int vw_watch_push_page(lv_obj_t *page);

/**
 * @brief 从页面栈中移除页面
 *
 * @param page 页面对象
 * @return int 成功返回0，失败返回负值
 */
int vw_watch_pop_page(lv_obj_t *page);

/**
 * @brief 获取内容区域对象
 *
 * @return lv_obj_t* 内容区域对象指针
 */
lv_obj_t* lv_watch_get_content_area(void);

/**
 * @brief 设置当前显示对象
 *
 * @param obj 当前显示的对象
 */
void lv_watch_set_current_obj(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_WATCH_APPS_LAUNCHER_LAUNCHER_H */

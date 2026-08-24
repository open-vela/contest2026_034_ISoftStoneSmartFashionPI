/****************************************************************************
 * apps/watch/apps/common/ui_mode_manager.h
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

#ifndef __UI_MODE_MANAGER_H
#define __UI_MODE_MANAGER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <lvgl/lvgl.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/**
 * @brief UI 模式枚举
 *
 * 设备支持两种 UI 界面，通过持久化 JSON 配置文件
 * (/mnt/spif/ui_mode.json) 管理当前模式，重启后自动加载。
 */
typedef enum
{
  UI_MODE_EXPRESSION = 0,  /* 表情UI（默认） */
  UI_MODE_WATCH      = 1,  /* 手表UI（vendor watch） */
} ui_mode_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief 从持久化配置文件加载 UI 模式
 *
 * 读取 /mnt/spif/ui_mode.json，解析 "ui_mode" 字段。
 * 文件不存在或解析失败时返回默认值 UI_MODE_EXPRESSION。
 *
 * @return ui_mode_t 当前 UI 模式
 */
ui_mode_t ui_mode_load(void);

/**
 * @brief 保存 UI 模式到持久化配置文件
 *
 * 将当前 UI 模式写入 /mnt/spif/ui_mode.json，格式：
 * {"ui_mode":0}  (0=表情, 1=手表)
 *
 * @param mode 要保存的 UI 模式
 */
void ui_mode_save(ui_mode_t mode);

/**
 * @brief 设置当前运行时的 UI 模式（由 launcher 在开机时调用）
 *
 * @param mode 当前 UI 模式
 */
void ui_mode_set_current(ui_mode_t mode);

/**
 * @brief 获取当前运行时的 UI 模式
 *
 * @return ui_mode_t 当前 UI 模式
 */
ui_mode_t ui_mode_get_current(void);

/**
 * @brief 无重启切换到指定 UI 模式
 *
 * 清理当前 UI 资源，初始化目标 UI。切换完成后写入持久化配置，
 * 保证下次开机仍进入该模式。
 *
 * @param target 目标 UI 模式
 * @param parent 目标 UI 的父容器（通常传 lv_scr_act()）
 * @return 0 成功，负值失败
 */
int ui_mode_switch_runtime(ui_mode_t target, lv_obj_t *parent);

#endif /* __UI_MODE_MANAGER_H */

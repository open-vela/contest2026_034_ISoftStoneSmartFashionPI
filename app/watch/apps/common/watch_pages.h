/****************************************************************************
 * apps/watch/apps/common/watch_pages.h
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

#ifndef WATCH_PAGES_H
#define WATCH_PAGES_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <lvgl/lvgl.h>

#include "../../resource/resource.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 屏幕尺寸 - ESP32-S3 Touch AMOLED 410x502 */
#define WATCH_SCREEN_WIDTH  410
#define WATCH_SCREEN_HEIGHT 502

/* 设置列表按钮尺寸 */
#define WATCH_BTN_WIDTH  340
#define WATCH_BTN_HEIGHT 100

/* 表情图片尺寸（GIF格式，显示居中适配） */
#define WATCH_EXPRESSION_IMG_W  300
#define WATCH_EXPRESSION_IMG_H  300

/* 表情切换动画时长（毫秒） */
#define WATCH_EXPRESSION_FADE_TIME_MS  300

/* SD卡就绪检测重试次数 */
#define WATCH_SD_READY_RETRIES  10

/* SD卡就绪检测重试间隔（毫秒） */
#define WATCH_SD_READY_DELAY_MS  500

#ifdef __cplusplus
}
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief 初始化表情展示公共页面
 *
 *  创建全屏背景和GIF控件，加载第一张表情GIF。
 *
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的页面根对象，失败返回 NULL
 */
lv_obj_t *watch_expression_page_init(lv_obj_t *parent);

/**
 * @brief 隐藏/显示表情页面（用于待机/唤醒切换）
 */
void watch_expression_page_hide(void);
void watch_expression_page_show(void);

/**
 * @brief 表情页面当前是否隐藏（待机状态）
 *
 * @return true 页面隐藏（或未初始化），false 可见
 */
bool watch_expression_page_is_hidden(void);

/**
 * @brief 销毁表情展示公共页面
 *
 *  释放页面相关资源。
 *
 * @param page_obj 页面根对象（由 watch_expression_page_init 返回）
 */
void watch_expression_page_deinit(lv_obj_t *page_obj);

/**
 * @brief 将页面对象压入页面栈
 *
 * @param page 页面对象
 * @return int 成功返回0，参数无效返回-1，栈满返回-2
 */
int lv_watch_push_page(lv_obj_t *page);

/**
 * @brief 将指定页面对象从页面栈中弹出
 *
 * @param page 页面对象
 * @return int 成功返回0，参数无效返回-1，未找到返回-2
 */
int lv_watch_pop_page(lv_obj_t *page);

/**
 * @brief 设置指定表情（供 ai_agent set_face tool / 端侧表情表调用）
 *
 *  切换到 face_id 对应的 GIF。duration_ms>0 时到期恢复 excited
 *  （仅显式传入短时表情时使用）；0=保持到下一次 set_face。
 *
 * @param face_id  表情ID（19个枚举值之一）
 * @param duration_ms 显示时长（毫秒），0=持久
 * @return 0 成功，-1 失败
 */
int watch_expression_page_set_face(const char* face_id, int duration_ms);

/****************************************************************************
 * 电池电量监控接口
 ****************************************************************************/

/**
 * @brief 获取当前电池电量百分比
 *
 *  封装 AXP2101 PMU 驱动，向应用层提供统一的电量查询接口。
 *
 * @return uint8_t 电池电量百分比（0-100），读取异常返回0
 */
uint8_t watch_battery_get_level(void);

/**
 * @brief 启动电池电量周期监控
 *
 *  在 LVGL 事件循环中创建定时器：每 10 秒检测电量、每 1 秒轮询
 *  充电状态。低电量时向 E2E 模型注入提示词触发人格化提醒
 *  （表情模式），手表模式保留原生提示音。重复调用安全。
 */
void watch_battery_check_start(void);

/**
 * @brief 停止电池电量周期监控
 */
void watch_battery_check_stop(void);

/**
 * @brief 切换到 vendor 手表界面
 *
 * 清理当前表情页面资源，初始化 vendor watch UI（表盘 + Fragment 页面系统）。
 * 调用后原表情页面不可恢复。
 *
 * @param parent LVGL 屏幕对象（通常传 lv_scr_act()）
 * @return 0 成功，负值失败
 */
int watch_switch_to_watch_app(lv_obj_t *parent);

/**
 * @brief 切换到表情（潮玩）界面
 *
 * 清理 vendor 手表 UI 资源，初始化表情页面。
 * 调用后原手表页面不可恢复。
 *
 * @param parent LVGL 屏幕对象（通常传 lv_scr_act()）
 * @return 0 成功，负值失败
 */
int watch_switch_to_expression_app(lv_obj_t *parent);

#endif /* WATCH_PAGES_H */

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

/* 表情自动轮播间隔（毫秒） */
#define WATCH_EXPRESSION_SWITCH_INTERVAL_MS  10000  /* 10秒切换一次 */

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
 * @brief 初始化表情展示公共页面（从SD卡加载GIF）
 *
 *  检测SD卡就绪后创建全屏背景和GIF控件，加载第一张表情GIF并
 *  启动自动轮播定时器（每 WATCH_EXPRESSION_SWITCH_INTERVAL_MS
 *  毫秒切换到下一张表情）。
 *
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的页面根对象，失败返回 NULL
 */
lv_obj_t *watch_expression_page_init(lv_obj_t *parent);

/**
 * @brief 销毁表情展示公共页面
 *
 *  停止自动轮播定时器并释放页面相关资源。
 *
 * @param page_obj 页面根对象（由 watch_expression_page_init 返回）
 */
void watch_expression_page_deinit(lv_obj_t *page_obj);

/**
 * @brief 手动切换到下一张表情图片
 *
 *  立即切换到下一张表情，重置轮播定时器计时。
 */
void watch_expression_page_next(void);

/**
 * @brief 获取当前表情图片索引
 *
 * @return int 当前显示的表情索引（0-based）
 */
int watch_expression_page_get_current_index(void);

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
 * @brief 设置指定表情（供 ai_agent set_face tool 调用）
 *
 *  立即停止自动轮播，切换到 face_id 对应的 GIF。
 *  如果 duration_ms > 0，到时自动恢复轮播。
 *  如果 duration_ms == 0，永久保持该表情。
 *
 * @param face_id     表情ID（happy/sad/neutral/.../sick，共18个）
 * @param duration_ms 显示时长（毫秒），0=永久
 * @return 0 成功，-1 失败（未初始化或face_id无效）
 */
int watch_expression_page_set_face(const char* face_id, int duration_ms);

#endif /* WATCH_PAGES_H */

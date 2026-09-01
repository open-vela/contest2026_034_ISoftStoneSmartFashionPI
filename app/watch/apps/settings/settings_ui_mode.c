/****************************************************************************
 * apps/watch/apps/settings/settings_ui_mode.c
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
#include <lvgl/lvgl.h>
#include <syslog.h>

#include "settings.h"
#include "../common/watch_pages.h"
#include "../common/ui_mode_manager.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define UI_MODE_LOG(fmt, ...) syslog(LOG_INFO, "[UI_MODE] " fmt, ##__VA_ARGS__)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct switch_ctx {
    ui_mode_t target;
    lv_obj_t *parent;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief 异步执行 UI 模式切换（由 lv_async_call 在事件链结束后回调）
 *
 * ui_mode_switch_runtime 涉及 LVGL 对象删除/创建和阻塞文件 I/O
 * （SPI flash littlefs 写入），不能放在 LV_EVENT_CLICKED 回调中
 * 同步执行，否则可能死锁 LVGL 线程。
 */
static void async_do_switch(void *arg)
{
    struct switch_ctx *ctx = (struct switch_ctx *)arg;

    UI_MODE_LOG("Switching to expression UI mode (runtime, async)");
    int ret = ui_mode_switch_runtime(ctx->target, ctx->parent);
    if (ret != 0)
        UI_MODE_LOG("Failed to switch to expression mode: %d", ret);

    free(ctx);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief UI模式切换按钮点击回调
 *
 * 用户在手表UI设置界面点击"切换为潮玩模式"后，保存表情UI模式
 * 到持久化配置文件并重启设备，下次开机进入表情UI界面。
 */
void settings_ui_mode_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        struct switch_ctx *ctx = malloc(sizeof(*ctx));
        if (!ctx) return;
        ctx->target = UI_MODE_EXPRESSION;
        ctx->parent = lv_scr_act();

        /* 延迟到事件回调链结束：避免在 LVGL 事件回调中同步执行
         * 对象删除、文件 I/O 等重操作 */
        lv_async_call(async_do_switch, ctx);
    }
}
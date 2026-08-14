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
#include <lvgl/lvgl.h>
#include <nuttx/power/axp2101.h>
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
      UI_MODE_LOG("Switching to expression UI mode");
      ui_mode_save(UI_MODE_EXPRESSION);
      axp2101_power_reset();  /* 重启生效 */
    }
}

/****************************************************************************
 * apps/watch/watch_main.c
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
#include <unistd.h>

#include <lvgl/lvgl.h>

/* 包含launcher模块（开机logo + 开机动画） */
#include "apps/launcher/launcher.h"
#include "apps/settings/settings_wifi.h"  /* 使用手表UI的WiFi设置 */
#include "apps/boot/watch_button.h"
#include "resource/resource.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;

    if (lv_is_initialized())
    {
      LV_LOG_ERROR("LVGL already initialized! aborting.");
      return -1;
    }

#ifdef NEED_BOARDINIT
    /* Perform board-specific driver initialization if not already done */
    boardctl(BOARDIOC_INIT, 0);
#endif

    lv_init();

    lv_nuttx_dsc_init(&info);

#ifdef CONFIG_LV_USE_NUTTX_LCD
    info.fb_path = "/dev/lcd0";
#endif

#ifdef CONFIG_INPUT_TOUCHSCREEN
    info.input_path = "/dev/input0";
#endif

    lv_nuttx_init(&info, &result);

    if (result.disp == NULL)
    {
      LV_LOG_ERROR("lv_demos initialization failure!");
      return 1;
    }

    /* 初始化资源管理（加载开机logo和动画帧数据） */
    watch_resource_init();

    /* WiFi开机自动初始化：在开机动画期间完成WiFi连接 */
    settings_wifi_auto_init();

    /* 启动物理按键监控（BOOT长按进入设置，PWR短按返回主页） */
    watch_button_monitor_init();

    /* 启动开机logo → 开机动画流程 */
    //printf("[MAIN] Calling launcher_init...\n");
    launcher_init(lv_scr_act());
    //printf("[MAIN] launcher_init completed\n");

    /* 进入LVGL事件循环 */
    //printf("[MAIN] Entering LVGL event loop\n");
    while (1)
    {
      uint32_t idle;
      idle = lv_timer_handler();

      /* Minimum sleep of 1ms */
      idle = idle ? idle : 1;
      usleep(idle * 1000);
    }

    lv_nuttx_deinit(&result);
    lv_deinit();

    return 0;
}

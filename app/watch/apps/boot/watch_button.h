/****************************************************************************
 * apps/watch/apps/boot/watch_button.h
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
 ****************************************************************************/

#ifndef __CONTEST_WATCH_APPS_BOOT_WATCH_BUTTON_H
#define __CONTEST_WATCH_APPS_BOOT_WATCH_BUTTON_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief 启动物理按键监控（使用LVGL定时器在主线程轮询）
 *
 * BOOT长按(10秒) → 打开/关闭设置界面
 * PWR短按        → 返回主页（弹出页面栈）
 *
 * 应在LVGL初始化完成后调用。
 */
void watch_button_monitor_init(void);

#endif /* __CONTEST_WATCH_APPS_BOOT_WATCH_BUTTON_H */

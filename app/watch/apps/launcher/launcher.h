/****************************************************************************
 * apps/watch/apps/launcher/launcher.h
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

#ifndef __CONTEST_WATCH_APPS_LAUNCHER_LAUNCHER_H
#define __CONTEST_WATCH_APPS_LAUNCHER_LAUNCHER_H

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
 * @brief 初始化并运行主页流程（开机logo → 开机动画）
 *
 * @param parent 父容器对象
 * @return int 成功返回0，失败返回负值
 */
int launcher_init(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_WATCH_APPS_LAUNCHER_LAUNCHER_H */

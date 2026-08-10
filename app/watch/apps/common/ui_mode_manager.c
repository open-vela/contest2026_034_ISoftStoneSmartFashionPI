/****************************************************************************
 * apps/watch/apps/common/ui_mode_manager.c
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

#include <stdio.h>
#include <string.h>
#include "ui_mode_manager.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define UI_MODE_CONFIG_FILE  "/mnt/spif/ui_mode.json"
#define UI_MODE_KEY          "\"ui_mode\":"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief 从持久化配置文件加载 UI 模式
 *
 * JSON 格式: {"ui_mode":0}  (0=表情, 1=手表)
 * 文件不存在或解析失败时返回 UI_MODE_EXPRESSION。
 */
ui_mode_t ui_mode_load(void)
{
  FILE *fp;
  char buf[64];
  int  mode_val = (int)UI_MODE_EXPRESSION;

  fp = fopen(UI_MODE_CONFIG_FILE, "r");
  if (fp == NULL)
    {
      /* 文件不存在，返回默认值 */
      return UI_MODE_EXPRESSION;
    }

  /* 读取文件内容（文件很小，一次读取即可） */
  if (fgets(buf, sizeof(buf), fp) != NULL)
    {
      /* 查找 "ui_mode": 键 */
      char *key_pos = strstr(buf, UI_MODE_KEY);
      if (key_pos != NULL)
        {
          /* 跳过键名，解析整数值 */
          key_pos += strlen(UI_MODE_KEY);
          mode_val = atoi(key_pos);
        }
    }

  fclose(fp);

  /* 确保返回值在有效范围内 */
  if (mode_val != (int)UI_MODE_EXPRESSION &&
      mode_val != (int)UI_MODE_WATCH)
    {
      mode_val = (int)UI_MODE_EXPRESSION;
    }

  return (ui_mode_t)mode_val;
}

/**
 * @brief 保存 UI 模式到持久化配置文件
 *
 * JSON 格式: {"ui_mode":0}  (0=表情, 1=手表)
 */
void ui_mode_save(ui_mode_t mode)
{
  FILE *fp;

  fp = fopen(UI_MODE_CONFIG_FILE, "w");
  if (fp == NULL)
    {
      return;
    }

  fprintf(fp, "{\"ui_mode\":%d}\n", (int)mode);
  fclose(fp);
}

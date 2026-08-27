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
 * Private Data
 ****************************************************************************/

static ui_mode_t g_current_ui_mode = UI_MODE_EXPRESSION;
static lv_obj_t *g_switch_overlay = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief 显示模式切换遮罩（全屏黑底 + 转圈 + 提示文字）
 */
static void show_switch_overlay(lv_obj_t *parent)
{
  if (g_switch_overlay != NULL)
    {
      return;
    }

  /* 全屏遮罩 */
  g_switch_overlay = lv_obj_create(parent);
  lv_obj_set_size(g_switch_overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_switch_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_switch_overlay, LV_OPA_90, 0);
  lv_obj_set_style_border_width(g_switch_overlay, 0, 0);
  lv_obj_set_style_radius(g_switch_overlay, 0, 0);
  lv_obj_clear_flag(g_switch_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(g_switch_overlay);

  /* 转圈动画 */
  lv_obj_t *spinner = lv_spinner_create(g_switch_overlay);
  lv_obj_set_size(spinner, 80, 80);
  lv_obj_center(spinner);

  /* 提示文字 */
  lv_obj_t *label = lv_label_create(g_switch_overlay);
  lv_label_set_text(label, "Switching...");
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
  lv_obj_align_to(label, spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

  /* 强制刷新，确保遮罩立即显示 */
  lv_refr_now(NULL);
}

/**
 * @brief 隐藏并删除模式切换遮罩
 */
static void hide_switch_overlay(void)
{
  if (g_switch_overlay != NULL)
    {
      lv_obj_del(g_switch_overlay);
      g_switch_overlay = NULL;
    }
}

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

/**
 * @brief 设置当前运行时的 UI 模式（由 launcher 在开机时调用）
 */
void ui_mode_set_current(ui_mode_t mode)
{
  g_current_ui_mode = mode;
}

/**
 * @brief 获取当前运行时的 UI 模式
 */
ui_mode_t ui_mode_get_current(void)
{
  return g_current_ui_mode;
}

/**
 * @brief 无重启切换到指定 UI 模式
 *
 * 清理当前 UI 资源，初始化目标 UI。切换完成后写入持久化配置，
 * 保证下次开机仍进入该模式。
 *
 * 注意：手表模式启动时跳过了 ai_agent（launcher.c agent_autostart 仅表情模式启动），
 * 运行时切换到表情模式后，由 launcher_start_voice_for_expression() 补齐
 * ai_agent 拉起 + 唤醒词监听，否则不进 listen 状态。
 */
int ui_mode_switch_runtime(ui_mode_t target, lv_obj_t *parent)
{
  if (parent == NULL)
    {
      return -1;
    }

  if (target == g_current_ui_mode)
    {
      return 0;
    }

  extern int watch_switch_to_watch_app(lv_obj_t *parent);
  extern int watch_switch_to_expression_app(lv_obj_t *parent);

  int ret = 0;

  /* 显示切换遮罩，避免耗时清理/初始化期间 UI 假死 */
  show_switch_overlay(parent);

  if (target == UI_MODE_WATCH)
    {
      /* 潮玩 → 手表：先停止表情模式的后台任务（语音会话 / E2E 链路 /
       * ai_agent），与冷启动进手表模式的行为对齐（手表模式不运行
       * ai_agent，见 launcher.c agent_autostart）；切回表情模式时由
       * launcher_start_voice_for_expression() 重新拉起 */
      extern void launcher_stop_voice_for_watch(void);
      launcher_stop_voice_for_watch();

      /* 潮玩 → 手表 */
      ret = watch_switch_to_watch_app(parent);
    }
  else
    {
      /* 手表 → 潮玩 */
      ret = watch_switch_to_expression_app(parent);
    }

  hide_switch_overlay();

  if (ret == 0)
    {
      g_current_ui_mode = target;
      ui_mode_save(target);

      /* 切到表情模式后，启动 ai_agent + 唤醒词监听
       * 手表模式启动时跳过了 ai_agent（见 launcher.c agent_autostart），
       * 运行时切换到表情需在此补齐，否则不进 listen 状态 */
      if (target == UI_MODE_EXPRESSION)
        {
          extern void launcher_start_voice_for_expression(void);
          launcher_start_voice_for_expression();
        }
    }

  return ret;
}

/****************************************************************************
 * apps/watch/apps/common/watch_pages.c
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
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "watch_pages.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PAGE_TAG "[PAGE] "

/* SD卡挂载点（与 resource.c 中的定义一致） */
#define SD_MOUNT_PATH  "/mnt/sd"

/* 表情GIF在SD卡上的目录（大写8.3格式） */
#define EXPR_SUBDIR    "/GIF"

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
#define PAGE_LOG(fmt, ...)  printf(PAGE_TAG fmt "\n", ##__VA_ARGS__)
#else
#define PAGE_LOG(fmt, ...)
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 表情页面运行时状态 */
static lv_obj_t   *s_page_root    = NULL;  /* 页面根容器 */
static lv_obj_t   *s_expr_gif     = NULL;  /* 表情GIF控件 */
static lv_timer_t *s_switch_timer = NULL;  /* 自动轮播定时器 */
static int         s_curr_index   = 0;     /* 当前表情索引 */
static int         s_expr_count   = 0;     /* 表情图片总数 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief 等待SD卡就绪
 */
static int wait_for_sd_ready(void)
{
  int retries;
  for (retries = 0; retries < WATCH_SD_READY_RETRIES; retries++)
    {
      if (watch_resource_is_sd_ready())
        {
          PAGE_LOG("SD card is ready (attempt %d)", retries + 1);
          return 1;
        }
      PAGE_LOG("Waiting for SD card... (attempt %d/%d)",
               retries + 1, WATCH_SD_READY_RETRIES);
      usleep(WATCH_SD_READY_DELAY_MS * 1000);
    }

  PAGE_LOG("SD card not ready after %d retries", WATCH_SD_READY_RETRIES);
  return 0;
}

/**
 * @brief 列出目录内容（诊断用）
 */
static void list_directory(const char *dir_path)
{
  DIR *dir = opendir(dir_path);
  if (dir == NULL)
    {
      PAGE_LOG("opendir(%s) failed (errno=%d)", dir_path, errno);
      return;
    }

  PAGE_LOG("Listing %s:", dir_path);
  struct dirent *entry;
  int count = 0;
  while ((entry = readdir(dir)) != NULL)
    {
      PAGE_LOG("  [%d] %s (type=%d)", count, entry->d_name, entry->d_type);
      count++;
    }
  closedir(dir);
  PAGE_LOG("  Total: %d entries", count);
}

/**
 * @brief 检查GIF文件在POSIX路径上是否存在且可读
 *
 *  将LVGL路径转换为POSIX路径并检查。
 *  如果打开失败，会列出SD卡根目录和GIF目录内容以辅助诊断。
 *
 * @param lvgl_path LVGL格式路径（如 "S:/GIF/SMILE.GIF"）
 * @return 1 如果文件存在且可读，0 否则
 */
static int check_gif_file_exists(const char *lvgl_path)
{
  char posix_path[256];

  /* 跳过 "S:" 前缀，拼接完整POSIX路径 */
  const char *sub_path = lvgl_path;
  if (sub_path[0] >= 'A' && sub_path[0] <= 'Z' && sub_path[1] == ':')
    {
      sub_path += 2;  /* 跳过 "S:" */
    }

  snprintf(posix_path, sizeof(posix_path), "%s%s", SD_MOUNT_PATH, sub_path);

  /* 尝试打开文件 */
  int fd = open(posix_path, O_RDONLY);
  if (fd >= 0)
    {
      /* 读取前3字节验证GIF头 */
      unsigned char header[3];
      int n = read(fd, header, 3);
      close(fd);

      if (n == 3 && header[0] == 'G' && header[1] == 'I' && header[2] == 'F')
        {
          PAGE_LOG("File OK: %s (GIF header verified)", posix_path);
          return 1;
        }
      else
        {
          PAGE_LOG("ERROR: %s exists but not a valid GIF (got %02x%02x%02x, %d bytes)",
                   posix_path, header[0], header[1], header[2], n);
          return 0;
        }
    }

  PAGE_LOG("ERROR: Cannot open %s (errno=%d)", posix_path, errno);

  /* 诊断：列出SD卡根目录和GIF目录内容 */
  list_directory(SD_MOUNT_PATH);
  list_directory(SD_MOUNT_PATH "/GIF");

  /* 尝试stat */
  struct stat st;
  if (stat(posix_path, &st) == 0)
    {
      PAGE_LOG("stat(%s) OK: size=%ld mode=0x%x", posix_path, (long)st.st_size, st.st_mode);
    }
  else
    {
      PAGE_LOG("stat(%s) failed (errno=%d)", posix_path, errno);
    }

  return 0;
}

/**
 * @brief 淡入动画回调函数
 */
static void fade_in_anim_cb(void *obj, int32_t v)
{
  lv_obj_set_style_opa(obj, v, 0);
}

/**
 * @brief 切换到指定索引的表情GIF
 */
static void switch_to_expression(int index)
{
  if (s_expr_count <= 0 || s_expr_gif == NULL)
    {
      PAGE_LOG("Cannot switch: count=%d gif=%p", s_expr_count, s_expr_gif);
      return;
    }

  /* 循环取模，确保索引在有效范围内 */
  index = index % s_expr_count;
  if (index < 0)
    {
      index += s_expr_count;
    }

  s_curr_index = index;

  const char *gif_path = watch_resource_get_img_expression(index);
  if (gif_path == NULL)
    {
      PAGE_LOG("Failed to get expression path at index %d", index);
      return;
    }

  PAGE_LOG("Loading expression %d/%d: %s", index, s_expr_count - 1, gif_path);

  /* 检查文件是否存在 */
  check_gif_file_exists(gif_path);

  /* 淡出当前图片 */
  lv_obj_set_style_opa(s_expr_gif, LV_OPA_TRANSP, 0);

  /* 设置GIF文件源 */
  lv_gif_set_src(s_expr_gif, gif_path);

  /* 重启GIF动画 */
  lv_gif_restart(s_expr_gif);

  /* 检查GIF控件尺寸（成功加载后应为300x300） */
  int32_t w = lv_obj_get_width(s_expr_gif);
  int32_t h = lv_obj_get_height(s_expr_gif);
  PAGE_LOG("GIF widget size after load: %ldx%ld", (long)w, (long)h);

  /* 淡入新图片（使用exec_cb回调实际更新透明度） */
  lv_anim_t fade_anim;
  lv_anim_init(&fade_anim);
  lv_anim_set_var(&fade_anim, s_expr_gif);
  lv_anim_set_exec_cb(&fade_anim, fade_in_anim_cb);
  lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&fade_anim, WATCH_EXPRESSION_FADE_TIME_MS);
  lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
  lv_anim_start(&fade_anim);

  PAGE_LOG("Switched to expression %d/%d", index, s_expr_count - 1);
}

/**
 * @brief 自动轮播定时器回调函数
 */
static void switch_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  switch_to_expression(s_curr_index + 1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *watch_expression_page_init(lv_obj_t *parent)
{
  if (s_page_root != NULL)
    {
      PAGE_LOG("Expression page already initialized");
      return s_page_root;
    }

  /* 等待SD卡就绪 */
  if (!wait_for_sd_ready())
    {
      PAGE_LOG("ERROR: SD card not ready, cannot init expression page");
      return NULL;
    }

  /* 获取表情图片总数 */
  s_expr_count = watch_resource_get_expression_count();
  if (s_expr_count <= 0)
    {
      PAGE_LOG("No expression images registered!");
      return NULL;
    }

  PAGE_LOG("Initializing expression page (%d images from SD card)",
           s_expr_count);

  /* 创建页面根容器 */
  s_page_root = lv_obj_create(parent);
  lv_obj_set_size(s_page_root, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(s_page_root, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(s_page_root, 0, 0);
  lv_obj_set_style_radius(s_page_root, 0, 0);
  lv_obj_set_style_pad_all(s_page_root, 0, 0);
  lv_obj_clear_flag(s_page_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(s_page_root);

  /* 创建GIF控件 */
  s_expr_gif = lv_gif_create(s_page_root);
  lv_obj_center(s_expr_gif);

  /* 让LVGL处理一次事件循环，确保控件完全初始化 */
  lv_timer_handler();

  /* 加载第一张表情GIF，如果失败则依次尝试后续表情 */
  int i;
  for (i = 0; i < s_expr_count; i++)
    {
      s_curr_index = i;
      const char *first_gif = watch_resource_get_img_expression(s_curr_index);
      if (first_gif == NULL)
        continue;

      PAGE_LOG("Loading first expression: %s", first_gif);
      check_gif_file_exists(first_gif);

      lv_gif_set_src(s_expr_gif, first_gif);
      lv_obj_set_style_opa(s_expr_gif, LV_OPA_COVER, 0);

      int32_t w = lv_obj_get_width(s_expr_gif);
      int32_t h = lv_obj_get_height(s_expr_gif);
      PAGE_LOG("GIF widget size after first load: %ldx%ld", (long)w, (long)h);

      if (w > 0 && h > 0)
        {
          PAGE_LOG("First expression loaded successfully");
          break;
        }

      PAGE_LOG("GIF %d failed to load, trying next...", i);
    }

  /* 启动自动轮播定时器（每10秒切换） */
  s_switch_timer = lv_timer_create(switch_timer_cb,
                                    WATCH_EXPRESSION_SWITCH_INTERVAL_MS,
                                    NULL);

  PAGE_LOG("Expression page initialized, auto-switch every %d ms",
           WATCH_EXPRESSION_SWITCH_INTERVAL_MS);

  return s_page_root;
}

void watch_expression_page_deinit(lv_obj_t *page_obj)
{
  /* 停止并删除自动轮播定时器 */
  if (s_switch_timer != NULL)
    {
      lv_timer_del(s_switch_timer);
      s_switch_timer = NULL;
    }

  /* 重置状态 */
  s_expr_gif   = NULL;
  s_curr_index = 0;
  s_expr_count = 0;

  /* 删除页面根容器（同时销毁子控件） */
  if (page_obj != NULL)
    {
      lv_obj_del(page_obj);
    }

  s_page_root = NULL;

  PAGE_LOG("Expression page deinitialized");
}

void watch_expression_page_next(void)
{
  if (s_switch_timer != NULL)
    {
      lv_timer_reset(s_switch_timer);
    }

  switch_to_expression(s_curr_index + 1);
}

int watch_expression_page_get_current_index(void)
{
  return s_curr_index;
}

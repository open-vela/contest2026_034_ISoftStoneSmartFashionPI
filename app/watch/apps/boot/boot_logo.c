/****************************************************************************
 * apps/watch/apps/boot/boot_logo.c
 *
 * 表情UI 开机Logo — 播放 GIF 动画
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>

#include "boot_logo.h"
#include "../../resource/resource.h"
#include "../common/watch_pages.h"

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define WATCH_DBG_LOG(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_logo_gif_obj = NULL;
static bool s_logo_finished = false;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void logo_gif_ready_cb(lv_event_t *e)
{
  (void)e;
  s_logo_finished = true;
  if (s_logo_gif_obj) {
    lv_gif_pause(s_logo_gif_obj);
  }
  WATCH_DBG_LOG("[BOOT] Logo GIF finished");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *boot_logo_init(lv_obj_t *parent)
{
  /* 创建黑色背景容器 */
  lv_obj_t *bg = lv_obj_create(parent);
  lv_obj_set_size(bg, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(bg, 0, 0);
  lv_obj_set_style_radius(bg, 0, 0);
  lv_obj_center(bg);
  lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

  /* 创建 GIF 控件播放开机 Logo */
  s_logo_gif_obj = lv_gif_create(bg);
  lv_obj_center(s_logo_gif_obj);
  lv_obj_add_event_cb(s_logo_gif_obj, logo_gif_ready_cb, LV_EVENT_READY, NULL);

  const void *gif_src = watch_resource_get_expr_logo_gif();
  if (gif_src) {
    lv_gif_set_src(s_logo_gif_obj, gif_src);
  } else {
    WATCH_DBG_LOG("[BOOT] boot_logo_init: expr logo gif data is NULL");
  }

  s_logo_finished = false;
  return bg;
}

bool boot_logo_is_finished(void)
{
  return s_logo_finished;
}

void boot_logo_deinit(lv_obj_t *logo_obj)
{
  if (s_logo_gif_obj) {
    lv_gif_pause(s_logo_gif_obj);
    s_logo_gif_obj = NULL;
  }
  if (logo_obj) {
    lv_obj_del(logo_obj);
  }
  s_logo_finished = false;
}

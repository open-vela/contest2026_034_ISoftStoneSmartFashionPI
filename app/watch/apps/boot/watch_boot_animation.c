/****************************************************************************
 * apps/watch/apps/boot/watch_boot_animation.c
 *
 * 手表UI独立开机动画实现
 *
 * 使用手表UI专用的开机动画 GIF 资源。
 * 后续修改时，只需替换此处使用的动画资源即可实现差异化。
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

#include "watch_boot_animation.h"
#include "../../resource/resource.h"
#include "../common/watch_pages.h"

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define WATCH_BOOT_LOG(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_watch_gif_obj = NULL;     /* GIF控件 */
static bool s_watch_anim_finished = false;   /* 动画是否已播放完成 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief GIF播放完成回调
 *
 * LVGL GIF解码器在GIF播放完最后一帧后发送LV_EVENT_READY事件，
 * 并自动暂停内部定时器，确保GIF不会重复播放。
 */
static void watch_gif_ready_cb(lv_event_t *e)
{
  (void)e;
  s_watch_anim_finished = true;

  /* 立即暂停 GIF，防止解码器在定时器轮询前重新开始播放 */
  if (s_watch_gif_obj) {
    lv_gif_pause(s_watch_gif_obj);
  }

  WATCH_BOOT_LOG("[WATCH_BOOT] GIF animation finished");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief 初始化并显示手表UI开机动画
 *
 * 当前使用手表UI专用的开机动画 GIF 资源。
 * 后续可替换为手表UI专属动画 GIF。
 *
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的动画对象
 */
lv_obj_t *watch_boot_animation_init(lv_obj_t *parent)
{
  WATCH_BOOT_LOG("[WATCH_BOOT] watch_boot_animation_init");

  /* 创建黑色背景容器 */
  lv_obj_t *bg = lv_obj_create(parent);
  lv_obj_set_size(bg, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(bg, 0, 0);
  lv_obj_set_style_radius(bg, 0, 0);
  lv_obj_set_style_pad_all(bg, 0, 0);
  lv_obj_center(bg);
  lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

  /* 创建lv_gif控件 */
  s_watch_gif_obj = lv_gif_create(bg);
  lv_obj_center(s_watch_gif_obj);

  /* 注册GIF播放完成事件回调，确保动画只播放一次 */
  lv_obj_add_event_cb(s_watch_gif_obj, watch_gif_ready_cb, LV_EVENT_READY, NULL);

  /* 设置嵌入式GIF数据源并开始播放 — 使用手表UI专用动画 */
  const void *gif_src = watch_resource_get_watch_anim_gif();
  if (gif_src) {
    lv_gif_set_src(s_watch_gif_obj, gif_src);
  } else {
    WATCH_BOOT_LOG("[WATCH_BOOT] watch_boot_animation_init: watch anim gif data is NULL");
  }

  /* 重置状态 */
  s_watch_anim_finished = false;

  return bg;
}

/**
 * @brief 查询手表UI开机动画是否已播放完成
 *
 * @return true 表示GIF已播放完最后一帧
 */
bool watch_boot_animation_is_finished(void)
{
  return s_watch_anim_finished;
}

/**
 * @brief 销毁手表UI开机动画
 *
 * @param anim_obj 动画对象
 */
void watch_boot_animation_deinit(lv_obj_t *anim_obj)
{
  /* 确保GIF停止播放 */
  if (s_watch_gif_obj) {
    lv_gif_pause(s_watch_gif_obj);
    s_watch_gif_obj = NULL;
  }

  /* 删除页面容器（同时销毁所有子控件） */
  if (anim_obj) {
    lv_obj_del(anim_obj);
  }

  s_watch_anim_finished = false;
}

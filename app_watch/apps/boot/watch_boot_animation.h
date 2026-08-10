/****************************************************************************
 * apps/watch/apps/boot/watch_boot_animation.h
 *
 * 手表UI独立开机动画 — 与表情UI的 boot_animation 分离，
 * 后续可替换为不同的动画资源。
 *
 ****************************************************************************/

#ifndef __APPS_WATCH_APPS_BOOT_WATCH_BOOT_ANIMATION_H
#define __APPS_WATCH_APPS_BOOT_WATCH_BOOT_ANIMATION_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * @brief 初始化并显示手表UI开机动画
 *
 * 当前使用与表情UI相同的 bootlogo GIF 资源，
 * 后续可替换为手表UI专属动画。
 *
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的动画对象
 */
lv_obj_t *watch_boot_animation_init(lv_obj_t *parent);

/**
 * @brief 销毁手表UI开机动画
 *
 * @param anim_obj 动画对象
 */
void watch_boot_animation_deinit(lv_obj_t *anim_obj);

/**
 * @brief 查询手表UI开机动画是否已播放完成
 *
 * @return true 表示动画已播放完成
 */
bool watch_boot_animation_is_finished(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_WATCH_APPS_BOOT_WATCH_BOOT_ANIMATION_H */

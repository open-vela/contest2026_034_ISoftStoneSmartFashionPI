/****************************************************************************
 * apps/watch/apps/boot/watch_boot_logo.h
 *
 * 手表UI独立开机Logo — 与表情UI的 boot_logo 分离，
 * 后续可替换为不同的 logo 资源。
 *
 ****************************************************************************/

#ifndef __APPS_WATCH_APPS_BOOT_WATCH_BOOT_LOGO_H
#define __APPS_WATCH_APPS_BOOT_WATCH_BOOT_LOGO_H

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
 * @brief 初始化并显示手表UI开机logo
 *
 * 当前使用与表情UI相同的 isoftstone_logo 资源，
 * 后续可替换为手表UI专属 logo。
 *
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的logo对象
 */
lv_obj_t *watch_boot_logo_init(lv_obj_t *parent);

/**
 * @brief 查询手表UI开机logo GIF是否已播放完成
 *
 * @return true 表示GIF已播放完最后一帧
 */
bool watch_boot_logo_is_finished(void);

/**
 * @brief 销毁手表UI开机logo
 *
 * @param logo_obj logo对象
 */
void watch_boot_logo_deinit(lv_obj_t *logo_obj);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_WATCH_APPS_BOOT_WATCH_BOOT_LOGO_H */

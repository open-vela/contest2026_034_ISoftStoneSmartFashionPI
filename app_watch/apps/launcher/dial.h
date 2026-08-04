/**
 * @file dial.h
 * 表盘模块公共接口
 */

#ifndef __DIAL_H
#define __DIAL_H

#include <nuttx/config.h>
#include <stdint.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 表盘类型枚举 */
typedef enum {
    DIAL_TYPE_DEFAULT = 0,  /* 默认数字表盘 */
    DIAL_TYPE_MOON,         /* 月亮表盘 */
    DIAL_TYPE_POINTER,      /* 指针表盘 */
    DIAL_TYPE_COUNT         /* 表盘总数 */
} dial_type_t;

/* 公共函数声明 */
lv_obj_t *dial_init(lv_obj_t *parent);
void dial_deinit(lv_obj_t *clock_obj);
void dial_switch_type(dial_type_t type);
dial_type_t dial_get_current_type(void);
const char* dial_get_type_name(dial_type_t type);

/* 获取表盘容器对象 */
lv_obj_t* dial_get_container(void);

/* 显示/隐藏表盘 */
void dial_show(void);
void dial_hide(void);

#ifdef __cplusplus
}
#endif

#endif /* __DIAL_H */
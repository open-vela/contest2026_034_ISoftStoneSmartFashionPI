#ifndef SPORTS_H
#define SPORTS_H

#include <lvgl.h>

/**
 * 运动应用点击回调
 */
void sport_app_click_callback(lv_event_t *e);

/**
 * 创建运动停止页面
 */
void sport_stop_app_create(void);

#endif // SPORTS_H

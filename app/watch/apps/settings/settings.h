/**
 * @file settings.h
 * 设置页面头文件
 *
 * 仅保留三个子模块：关于、WiFi、关机与重启。
 */

#ifndef __CONTEST_WATCH_APPS_SETTINGS_SETTINGS_H
#define __CONTEST_WATCH_APPS_SETTINGS_SETTINGS_H

#include "../common/watch_pages.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 设置项枚举 */
typedef enum
{
  SETTINGS_WIFI,
  SETTINGS_ABOUT,
  SETTINGS_REBOOT,
  SETTINGS_MAX
} settings_item_t;

/* 函数声明 */
void settings_app_click_callback(lv_event_t *e);
void settings_wifi_event_cb(lv_event_t *e);
void settings_about_event_cb(lv_event_t *e);
void settings_system_event_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif

#endif /* __CONTEST_WATCH_APPS_SETTINGS_SETTINGS_H */

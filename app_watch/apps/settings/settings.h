/**
 * @file settings.h
 * 设置页面头文件
 */

#ifndef __SETTINGS_H
#define __SETTINGS_H

#include "../common/watch_pages.h"

/* 设置项枚举 */
typedef enum {
    SETTINGS_WIFI,
    SETTINGS_BLUETOOTH,
    SETTINGS_ABOUT,
    SETTINGS_DATE,
    SETTINGS_TIME,
    SETTINGS_BATTERY,
    SETTINGS_BRIGHTNESS,
    SETTINGS_VOLUME_RING,
    SETTINGS_REBOOT,
    SETTINGS_MAX
} settings_item_t;

/* 设置项点击回调类型 */
typedef void (*settings_item_cb_t)(lv_event_t* e);

/* 设置项信息结构 */
typedef struct {
    settings_item_t id;
    const char* name;
    const char* icon_key;
    settings_item_cb_t callback;
} settings_item_info_t;


/* 函数声明 */
void settings_app_click_callback(lv_event_t *e);
void settings_wifi_event_cb(lv_event_t *e);
void settings_bluetooth_event_cb(lv_event_t *e);
void settings_dateset_event_cb(lv_event_t *e);
void settings_timeset_event_cb(lv_event_t *e);
void settings_battery_event_cb(lv_event_t *e);
void settings_display_event_cb(lv_event_t *e);
void settings_volume_ring_event_cb(lv_event_t *e);
void settings_system_event_cb(lv_event_t *e);
void settings_about_event_cb(lv_event_t *e);
void setting_charging_create(void);



#endif /* __SETTINGS_H */
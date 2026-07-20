/**
 * @file settings_wifi.h
 * WiFi设置模块头文件
 */

#ifndef __SETTINGS_WIFI_H
#define __SETTINGS_WIFI_H

#include <stdbool.h>
#include <lvgl.h>

/* 函数声明 */
void settings_wifi_event_cb(lv_event_t *e);

/* WiFi开机自动初始化：加载配置、恢复开关状态、自动连接已保存的WiFi */
void settings_wifi_auto_init(void);

/* WiFi连接状态接口 */
bool settings_wifi_is_connected(void);
const char* settings_wifi_get_connected_ssid(void);

#endif /* __SETTINGS_WIFI_H */

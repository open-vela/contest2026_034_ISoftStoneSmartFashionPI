/**
 * @file weather.h
 * 天气页面头文件
 */

#ifndef __WEATHER_H
#define __WEATHER_H

#include "../common/watch_pages.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 和风天气API配置 */
#define QWEATHER_API_KEY "7a82eeda6aa64571addb4c9a49b99747"
#define QWEATHER_BASE_URL "https://mc3wt2fka2.re.qweatherapi.com/v7"
#define QWEATHER_GEO_URL "https://mc3wt2fka2.re.qweatherapi.com/geo/v2/city/lookup"
#define QWEATHER_DEFAULT_LOCATION "101200101"  /* 武汉 */
#define QWEATHER_DEFAULT_CITY "武汉"

/* 动态定位（WiFi连接后通过IP定位更新）— 现在在 weather.c 中堆分配 */
/* extern char g_weather_location_id[32]; -- removed */
/* extern char g_weather_city[64]; -- removed */

/* 定位相关 */
typedef struct {
    char location_id[32];
    char city[64];
    char district[64];
} LocationInfo;

/* 天气数据结构体 */
typedef struct {
    char date[11];     // 日期，格式：2023-12-01
    char time[6];      // 时间，格式：12:00
    int temp;          // 温度，单位：℃
    char weather[32];  // 天气状况
    char icon[32];     // 天气图标名称
} HourlyWeather;

typedef struct {
    char date[11];     // 日期，格式：2023-12-01
    char day[16];      // 星期几
    int temp_max;      // 最高温度
    int temp_min;      // 最低温度
    char weather[32];  // 天气状况
    char icon[32];     // 天气图标名称
} DailyWeather;

typedef struct {
    char weather[32];  // 当前天气状况
    int temp;          // 当前温度
    char icon[32];     // 当前天气图标
    HourlyWeather hourly[24];  // 24小时预报
    DailyWeather daily[7];      // 7天预报
    int hourly_count;  // 实际获取的小时数
    int daily_count;   // 实际获取的天数
} WeatherData;

/* 函数声明 */
void weather_app_click_callback(lv_event_t *e);
int weather_get_location(LocationInfo *info);
int weather_get_data(WeatherData *data);
int weather_update_location(void);

#endif /* __WEATHER_H */

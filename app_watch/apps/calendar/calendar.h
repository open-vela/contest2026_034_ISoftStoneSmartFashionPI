/**
 * @file calendar.h
 * 日历页面头文件
 */

#ifndef __CALENDAR_H
#define __CALENDAR_H

#include "../common/watch_pages.h"


/* 日历事件结构 */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    const char* title;
} calendar_event_t;


void calendar_app_click_callback(lv_event_t *e);

#endif /* __CALENDAR_H */
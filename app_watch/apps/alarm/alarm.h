/**
 * @file alarm.h
 * 秒表页面头文件
 */

#ifndef __ALARM_H
#define __ALARM_H

#include "../common/watch_pages.h"

#include <stdint.h>
#include <stdbool.h>
#include "lvgl/lvgl.h"

// 宏定义
#define WATCH_MAX_ALARMS 10

// 闹钟结构体
typedef struct {
    uint8_t hour;         // 小时
    uint8_t minute;       // 分钟
    uint8_t repeat[7];    // 重复日期，0-6 对应周一到周日
    uint8_t enabled;      // 闹钟开关状态
} alarm_t;

// 闹钟操作类型
typedef enum {
    ALARM_OP_ADD,         // 添加闹钟
    ALARM_OP_EDIT         // 编辑闹钟
} alarm_op_t;


/* 函数声明 */
void alarm_app_click_callback(lv_event_t *e);

/* 系统级闹钟服务函数 */
void alarm_service_init(void);              // 初始化闹钟服务（系统启动时调用）
void alarm_service_deinit(void);            // 反初始化闹钟服务
void alarm_check_trigger(void);             // 检查闹钟触发
int64_t getSetTimer(void);                  // 获取距离下一个闹钟的秒数

/* 闹钟数据持久化函数 */
void alarm_save_to_file(void);              // 保存闹钟数据到文件
void alarm_load_from_file(void);            // 从文件加载闹钟数据

#endif /* __ALARM_H */

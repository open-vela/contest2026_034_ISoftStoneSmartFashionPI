/**
 * @file home_control.h
 * 智能家居控制模块头文件
 *
 * 通过 WiFi 与本地控制服务器通信，提供 12 路设备的开关控制。
 * 通信流程：UDP 广播发现服务器 → TCP 短连接下发命令。
 */

#ifndef __HOME_CONTROL_H
#define __HOME_CONTROL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 智能家居设备索引（0-based） */
#define HC_DEVICE_TV              0   /* 电视 */
#define HC_DEVICE_AC              1   /* 空调 */
#define HC_DEVICE_FLOOR_HEATING   2   /* 地暖 */
#define HC_DEVICE_FRESH_AIR       3   /* 新风 */
#define HC_DEVICE_LIVING_AMBIENT  4   /* 客厅氛围灯 */
#define HC_DEVICE_LIVING_LIGHT    5   /* 客厅灯 */
#define HC_DEVICE_KITCHEN_LIGHT   6   /* 厨房灯 */
#define HC_DEVICE_ENTRANCE_LIGHT  7   /* 玄关灯 */
#define HC_DEVICE_BEDROOM_LIGHT   8   /* 卧室灯 */
#define HC_DEVICE_BEDROOM_BG      9   /* 卧室背景灯 */
#define HC_DEVICE_BATHROOM_LIGHT  10  /* 卫生间灯 */
#define HC_DEVICE_CURTAIN         11  /* 窗帘 */

/**
 * @brief 重置服务器发现状态
 *
 *  当 WiFi 断开或切换网络后调用，使下一次开关操作触发重新发现。
 */
void watch_home_control_reset_server(void);

/**
 * @brief 获取智能家居设备数量
 *
 * @return int 设备总数（当前为 12）
 */
int watch_home_control_get_device_count(void);

/**
 * @brief 获取设备名称
 *
 * @param index 设备索引（0-based，可用 HC_DEVICE_* 宏）
 * @return const char* 设备名称，索引无效返回 NULL
 */
const char *watch_home_control_get_device_name(int index);

/**
 * @brief 获取设备当前开关状态
 *
 * @param index 设备索引（0-based）
 * @return true=开，false=关，索引无效返回 false
 */
bool watch_home_control_get_device_status(int index);

/**
 * @brief 控制智能家居设备开关
 *
 *  完整流程：WiFi 前置检查 → 服务器发现 → 命令下发 → 状态更新。
 *  首次调用会自动触发 UDP 广播发现控制服务器。
 *  WiFi 未连接时返回错误码 -2，不执行任何网络操作。
 *
 * @param index    设备索引（0-based，可用 HC_DEVICE_* 宏）
 * @param turn_on  true=开启，false=关闭
 * @return 0 成功，负值失败：
 *         -1 索引无效
 *         -2 WiFi 未连接
 *         -3 服务器发现失败
 *         -4 命令发送失败
 */
int watch_home_control_toggle_device(int index, bool turn_on);

#ifdef __cplusplus
}
#endif

#endif /* __HOME_CONTROL_H */

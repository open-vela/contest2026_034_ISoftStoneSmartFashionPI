/**
 * @file volume_control.h
 * 音量控制模块头文件
 *
 * 音量分 5 级，映射到 0-1000 的硬件音量值：
 *   Level 0 (mute)   → 0
 *   Level 1 (low)    → 250
 *   Level 2 (medium) → 500
 *   Level 3 (high)   → 750
 *   Level 4 (max)    → 1000
 */

#ifndef __VOLUME_CONTROL_H
#define __VOLUME_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 音量等级定义 */
#define WATCH_VOLUME_LEVEL_MUTE   0   /* 静音 */
#define WATCH_VOLUME_LEVEL_LOW    1   /* 低 */
#define WATCH_VOLUME_LEVEL_MEDIUM 2   /* 中（默认） */
#define WATCH_VOLUME_LEVEL_HIGH   3   /* 高 */
#define WATCH_VOLUME_LEVEL_MAX    4   /* 最大 */

/**
 * @brief 音量等级 (0-4) 转换为硬件音量值 (0-1000)
 *
 * @param level 音量等级 (0-4)
 * @return int 硬件音量值 (0-1000)
 */
int watch_volume_level_to_value(int level);

/**
 * @brief 硬件音量值 (0-1000) 转换为音量等级 (0-4)
 *
 * @param value 硬件音量值 (0-1000)
 * @return int 音量等级 (0-4)
 */
int watch_volume_value_to_level(int value);

/**
 * @brief 获取当前音量等级
 *
 *  从音频驱动读取当前音量值并转换为等级。
 *
 * @return int 音量等级 (0-4)，读取失败返回 -1
 */
int watch_volume_get_level(void);

/**
 * @brief 设置音量等级
 *
 * @param level 音量等级 (0-4)
 * @return 0 成功，负值失败
 */
int watch_volume_set_level(int level);

/**
 * @brief 获取当前硬件音量值
 *
 * @return int 音量值 (0-1000)，读取失败返回 -1
 */
int watch_volume_get_value(void);

/**
 * @brief 设置硬件音量值
 *
 * @param value 音量值 (0-1000)，超出范围自动截断
 * @return 0 成功，负值失败
 */
int watch_volume_set_value(int value);

/**
 * @brief 音量递增一级
 *
 * @return int 新的音量等级 (0-4)，失败返回 -1
 */
int watch_volume_step_up(void);

/**
 * @brief 音量递减一级
 *
 * @return int 新的音量等级 (0-4)，失败返回 -1
 */
int watch_volume_step_down(void);

#ifdef __cplusplus
}
#endif

#endif /* __VOLUME_CONTROL_H */

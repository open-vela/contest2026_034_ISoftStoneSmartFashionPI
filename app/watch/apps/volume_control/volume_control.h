/**
 * @file volume_control.h
 * 音量控制模块头文件
 *
 * 百分比映射到硬件音量值 (100-1000)：
 *   10% → 100,  20% → 200, ... 100% → 1000
 *
 * 五级兼容映射（保留用于 UI）:
 *   Level 0 (mute)   →   0%
 *   Level 1 (low)    →  25%
 *   Level 2 (medium) →  50%
 *   Level 3 (high)   →  75%
 *   Level 4 (max)    → 100%
 */

#ifndef __VOLUME_CONTROL_H
#define __VOLUME_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 音量等级定义 (兼容旧五级 API) */
#define WATCH_VOLUME_LEVEL_MUTE   0   /* 静音 */
#define WATCH_VOLUME_LEVEL_LOW    1   /* 低 */
#define WATCH_VOLUME_LEVEL_MEDIUM 2   /* 中（默认） */
#define WATCH_VOLUME_LEVEL_HIGH   3   /* 高 */
#define WATCH_VOLUME_LEVEL_MAX    4   /* 最大 */

/* ── 百分比 API（推荐） ─────────────────────────────────── */

/**
 * @brief 设置音量为百分比 (10-100)
 *        自动映射到硬件范围 100-1000
 *
 * @param percent 音量百分比 (10-100)
 * @return 0 成功，负值失败
 */
int watch_volume_set_percent(int percent);

/**
 * @brief 获取当前音量百分比 (10-100)
 *
 * @return int 音量百分比，读取失败返回 -1
 */
int watch_volume_get_percent(void);

/**
 * @brief 音量增加/减少 delta (默认 10%)
 *
 * @param delta 正数为增加，负数为减少，自动截断到 10-100
 * @return int 调整后的百分比，失败返回 -1
 */
int watch_volume_step_delta(int delta);

/* ── 五级 API（兼容旧代码） ─────────────────────────────── */

int watch_volume_level_to_value(int level);
int watch_volume_value_to_level(int value);
int watch_volume_get_level(void);
int watch_volume_set_level(int level);
int watch_volume_get_value(void);
int watch_volume_set_value(int value);
int watch_volume_step_up(void);
int watch_volume_step_down(void);

/**
 * @brief 开机恢复持久化音量
 *
 * 读取 /mnt/spif/volume.json 并写回硬件（由 launcher 在开机时调用）。
 * 文件不存在或内容非法时保持板级默认音量。
 *
 * @return 0 成功恢复，负值无保存值或硬件未配置
 */
int watch_volume_restore(void);

#ifdef __cplusplus
}
#endif

#endif /* __VOLUME_CONTROL_H */

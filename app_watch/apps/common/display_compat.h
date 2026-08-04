/**
 * @file display_compat.h
 * 显示超时兼容层 - 为 vendor watch 代码提供 display_set_timeout
 * 和 ft3168_display_timeout_setup 的空实现（stub）。
 *
 * vela-pai 的 lv_nuttx_touchscreen.c 中尚未实现这两个函数，
 * 此处提供 inline no-op 以避免链接错误。
 * 后续可将 openvela 的完整实现移植到 vela-pai 的 LVGL 驱动中。
 */

#ifndef DISPLAY_COMPAT_H
#define DISPLAY_COMPAT_H

static inline void display_set_timeout(int timeout) { (void)timeout; }
static inline void ft3168_display_timeout_setup(void) {}
static inline void setNull_display_timeout_timer(void) {}
static inline int getchange(void) { return 0; }
static inline void setchange(int v) { (void)v; }

#endif /* DISPLAY_COMPAT_H */

/**
 * @file display_compat.h
 * 显示超时控制接口声明。
 *
 * 实现在 display_control.c 中：
 *   - display_set_timeout / display_get_timeout: 息屏时间存取
 *   - ft3168_display_timeout_setup: 创建息屏定时器
 *   - setNull_display_timeout_timer: 停止息屏定时器
 *   - display_timeout_disable / display_timeout_enable: 暂停/恢复自动熄屏
 *   - getchange / setchange: 屏幕状态机 (0=正常, 1=亮屏中, 2=需亮屏)
 */

#ifndef DISPLAY_COMPAT_H
#define DISPLAY_COMPAT_H

void display_set_timeout(int timeout);
int  display_get_timeout(void);
void ft3168_display_timeout_setup(void);
void setNull_display_timeout_timer(void);
void display_timeout_disable(void);
void display_timeout_enable(void);
int  getchange(void);
void setchange(int v);

#endif /* DISPLAY_COMPAT_H */

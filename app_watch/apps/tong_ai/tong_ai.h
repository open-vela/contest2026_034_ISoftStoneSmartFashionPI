/**
 * @file tong_ai.h
 * 小通AI应用入口头文件
 */

#ifndef TONG_AI_H
#define TONG_AI_H

#include <lvgl.h>
#include <stdbool.h>

/**
 * 小通AI应用点击回调
 */
void tong_ai_app_click_callback(lv_event_t *e);

/**
 * 检查小通AI是否处于活跃状态（页面已打开）
 * 用于阻止空闲超时在对话期间隐藏屏幕
 * @return true 页面已打开, false 已关闭
 */
bool tong_ai_is_active(void);

#endif /* TONG_AI_H */

/**
 * @file home_control.h
 * 智能家居控制页面头文件
 */

#ifndef __HOME_CONTROL_H
#define __HOME_CONTROL_H

#include <stdbool.h>
#include "../common/watch_pages.h"

/* 语音控制返回值 */
#define HOME_CTRL_NONE       0   /* 无开关动作词，非智能家居指令 */
#define HOME_CTRL_OK         1   /* 匹配成功，指令已发送 */
#define HOME_CTRL_NET_FAIL   2   /* 匹配到设备但网络连接/发送失败 */
#define HOME_CTRL_NO_MATCH   3   /* 有开关动作词但未匹配到已知设备 */

/* SD卡提示音路径（挂载点 /mnt/sd，与 ai_agent 预置音频同目录） */
#define HOME_CTRL_FAIL_WAV       "/mnt/sd/audio/network_fail.wav"
#define HOME_CTRL_NOT_FOUND_WAV  "/mnt/sd/audio/device_not_found.wav"

/* 函数声明 */
void home_control_app_click_callback(lv_event_t *e);
void home_control_reset_server(void);

/**
 * 语音控制智能家居入口
 * 解析ASR识别的文本，匹配设备名和开关动作，发送控制指令
 * @param text  ASR识别的完整文本（如"打开客厅灯"）
 * @return HOME_CTRL_NONE      无关指令（无开关动作词），正常AI对话
 *         HOME_CTRL_OK        匹配并成功发送指令，正常AI对话
 *         HOME_CTRL_NET_FAIL  匹配到设备但网络连接/发送失败
 *         HOME_CTRL_NO_MATCH  有开关动作词但未匹配到已知设备
 */
int home_control_voice_execute(const char *text);

#endif /* __HOME_CONTROL_H */

/**
 * @file tong_ai_ws.h
 * 火山引擎端到端实时语音对话模块
 *
 * 基于火山引擎 Realtime API (S2S模型) 实现端到端语音对话，
 * 单WebSocket连接同时处理ASR+LLM+TTS。
 * 协议: wss://openspeech.bytedance.com/api/v3/realtime/dialogue
 */

#ifndef TONG_AI_WS_H
#define TONG_AI_WS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 火山引擎二进制协议常量 ========== */

#define VOLC_PROTO_VER  0x11  /* version=1, header_size=1 (x4=4B) */
#define VOLC_HDR_SIZE   4
#define VOLC_FLAG_EVENT 0x04

/* 消息类型 (byte1高4位) */
#define VOLC_MSG_FULL_REQ    0x10  /* Full-client request */
#define VOLC_MSG_AUDIO_REQ   0x20  /* Audio-only request */
#define VOLC_MSG_FULL_RESP   0x90  /* Full-server response */
#define VOLC_MSG_AUDIO_RESP  0xB0  /* Audio-only response */
#define VOLC_MSG_ERROR       0xF0  /* Error */

/* 序列化方法 */
#define VOLC_SER_RAW  0x00  /* Raw binary (audio) */
#define VOLC_SER_JSON 0x10  /* JSON */

/* 客户端事件ID */
#define EVENT_START_CONNECTION  1
#define EVENT_FINISH_CONNECTION 2
#define EVENT_START_SESSION     100
#define EVENT_FINISH_SESSION    102
#define EVENT_TASK_REQUEST      200
#define EVENT_END_ASR           400

/* 服务端事件ID */
#define EVENT_CONNECTION_STARTED  50
#define EVENT_CONNECTION_FAILED   51
#define EVENT_CONNECTION_FINISHED 52
#define EVENT_SESSION_STARTED     150
#define EVENT_SESSION_FINISHED    152
#define EVENT_SESSION_FAILED      153
#define EVENT_TTS_SENTENCE_START 350
#define EVENT_TTS_SENTENCE_END    351
#define EVENT_TTS_RESPONSE        352
#define EVENT_TTS_ENDED            359
#define EVENT_ASR_INFO            450
#define EVENT_ASR_RESPONSE        451
#define EVENT_ASR_ENDED           459
#define EVENT_CHAT_RESPONSE       550
#define EVENT_CHAT_ENDED          559
#define EVENT_DIALOG_ERROR        599

/* 音频参数 */
#define VOLC_CAPTURE_RATE     16000
#define VOLC_CAPTURE_BITS     16
#define VOLC_CAPTURE_CHANNELS 2   /* I2S硬件要求立体声 */
#define VOLC_PLAYBACK_RATE    24000
#define VOLC_PLAYBACK_BITS    16
#define VOLC_PLAYBACK_CHANNELS 1  /* 服务器发送单声道 */
#define VOLC_I2S_CHANNELS     2   /* I2S硬件要求立体声输出 */

/* 会话ID长度 (UUID格式) */
#define VOLC_SESSION_ID_LEN 36

/* ========== UI状态回调类型 ========== */

typedef enum {
    VOLC_UI_IDLE = 0,        /* 空闲 */
    VOLC_UI_CONNECTING,       /* 连接中 */
    VOLC_UI_CONNECTED,        /* 已连接，等待说话 */
    VOLC_UI_LISTENING,        /* 用户正在说话(ASR_INFO) */
    VOLC_UI_THINKING,         /* 用户说完了(ASR_ENDED)，等待AI回复 */
    VOLC_UI_SPEAKING,         /* AI正在回复(TTS播放中) */
    VOLC_UI_DISCONNECTING,    /* 正在断开 */
    VOLC_UI_ERROR             /* 错误 */
} volc_ui_state_t;

/* 回调事件类型 */
typedef enum {
    VOLC_CB_UI_STATE = 0,     /* UI状态变更 */
    VOLC_CB_USER_TEXT,        /* 用户语音识别文本 */
    VOLC_CB_AI_TEXT,          /* AI回复文本 */
    VOLC_CB_ERROR_MSG,        /* 错误消息 */
    VOLC_CB_CONNECTED,        /* 会话已建立 */
    VOLC_CB_DISCONNECTED      /* 会话已断开 */
} volc_callback_type_t;

/* 回调函数原型 */
typedef void (*volc_event_cb_t)(volc_callback_type_t type, const char *data);

/* ========== 公开API ========== */

/**
 * 初始化火山引擎语音模块(生成UUID等)
 * @return 0成功, 负值失败
 */
int volc_voice_init(void);

/**
 * 启动端到端语音会话
 * 连接TLS + WebSocket握手 + StartConnection + StartSession + 启动收发线程
 * @param cb UI事件回调函数
 * @return 0成功, 负值失败
 */
int volc_voice_start(volc_event_cb_t cb);

/**
 * 停端到端语音会话
 * 停止收发线程 + FinishSession + 断开连接
 */
void volc_voice_stop(void);

/**
 * 检查会话是否活跃
 * @return true活跃, false未活跃
 */
bool volc_voice_is_active(void);

/**
 * 中断当前TTS播放(用户打断)
 */
void volc_voice_interrupt(void);

/**
 * 播放本地WAV文件（用于提示音，如网络连接失败/设备未找到）
 * 会停止当前采集和播放，播放完WAV后自动恢复采集
 * @param path  WAV文件路径（PCM格式，24000Hz/16bit/2ch推荐）
 */
void volc_play_local_wav(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* TONG_AI_WS_H */

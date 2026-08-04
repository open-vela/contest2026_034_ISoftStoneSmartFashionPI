/**
 * @file xiaozhi_ai_ws.h
 * 小智AI WebSocket通信模块头文件
 *
 * 提供与XiaoZhi AI服务器的SSL/TLS WebSocket通信能力
 */

#ifndef XIAOZHI_AI_WS_H
#define XIAOZHI_AI_WS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WebSocket连接状态枚举
 */
typedef enum {
    WS_STATE_DISCONNECTED = 0,  /**< 未连接 */
    WS_STATE_CONNECTING,        /**< TCP连接中 */
    WS_STATE_HANDSHAKING,       /**< WebSocket握手进行中 */
    WS_STATE_CONNECTED,         /**< 已连接 */
    WS_STATE_ERROR              /**< 连接错误 */
} ws_connection_state_t;

/**
 * 初始化WebSocket通信模块
 * @return 0成功, 负值失败
 */
int xiaozhi_ai_ws_init(void);

/**
 * 清理WebSocket通信模块
 */
void xiaozhi_ai_ws_deinit(void);

/**
 * 连接到XiaoZhi AI服务器
 * @param hostname 服务器主机名
 * @param port     服务器端口
 * @param path     WebSocket路径
 * @return 0成功, 负值失败
 */
int xiaozhi_ai_ws_connect(const char *hostname, const char *port, const char *path);

/**
 * 断开WebSocket连接
 */
void xiaozhi_ai_ws_disconnect(void);

/**
 * 检查是否已连接
 * @return true已连接, false未连接
 */
bool xiaozhi_ai_ws_is_connected(void);

/**
 * 获取当前连接状态
 * @return 连接状态枚举值
 */
ws_connection_state_t xiaozhi_ai_ws_get_state(void);

/**
 * 发送JSON文本消息
 * @param json JSON字符串
 * @return 0成功, 负值失败
 */
int xiaozhi_ai_ws_send_json(const char *json);

/**
 * 接收WebSocket文本帧
 * @param buf        接收缓冲区
 * @param max_len    缓冲区最大长度
 * @param timeout_ms 超时时间(毫秒)
 * @return 接收字节数, -1错误, -2超时, 0连接关闭
 */
int xiaozhi_ai_ws_receive(char *buf, int max_len, int timeout_ms);

/**
 * 检查音频硬件是否可用
 * @return true可用, false不可用
 */
bool xiaozhi_ai_ws_is_audio_available(void);

/**
 * 获取UUID字符串
 * @return UUID字符串指针
 */
const char *xiaozhi_ai_ws_get_uuid(void);

/**
 * 获取MAC地址字符串
 * @return MAC地址字符串指针
 */
const char *xiaozhi_ai_ws_get_mac(void);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_AI_WS_H */

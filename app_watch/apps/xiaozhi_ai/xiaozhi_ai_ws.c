/**
 * @file xiaozhi_ai_ws.c
 * 小智AI WebSocket通信模块实现
 *
 * 从apps/examples/esp32s3_watch_wifi/esp32s3_websocket_test.c移植核心代码，
 * 重构为独立的、无LVGL依赖的通信模块。
 *
 * 提供基于mbedtls的SSL/TLS WebSocket客户端能力，用于连接api.tenclass.net
 */

#include "xiaozhi_ai_ws.h"

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netdb.h>

#include <sys/select.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#include "esp_mbedtls.h"

#define LOG_INFO(fmt, ...)  printf("[WS] " fmt "\n", ##__VA_ARGS__)
#define LOG_OK(fmt, ...)    printf("[WS_OK] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) printf("[WS_ERR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  printf("[WS_WARN] " fmt "\n", ##__VA_ARGS__)

#define CONNECT_TIMEOUT_SEC  10
#define MAX_HOSTNAME_LEN     64

/* WebSocket GUID for accept key computation */
static const char WEBSOCKET_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* 全局WebSocket连接状态 */
static int g_ws_sockfd = -1;
static bool g_use_ssl = true;
static ws_connection_state_t g_ws_state = WS_STATE_DISCONNECTED;
static char g_uuid[64] = {0};
static char g_mac[32] = {0};

/* SSL/TLS上下文 */
static mbedtls_ssl_context g_ssl_ctx;
static mbedtls_ssl_config g_ssl_conf;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static mbedtls_x509_crt g_cacert;

/* mbedtls时间函数 */
mbedtls_ms_time_t esp_mbedtls_ms_time(void);

mbedtls_ms_time_t esp_mbedtls_ms_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (mbedtls_ms_time_t)(ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL);
}

/* ========== 网络基础操作 ========== */

static char *get_wireless_mac_address(void)
{
    static char mac_str[32] = {0};
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return NULL;

    struct ifreq ifr;
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        close(sock);
        return NULL;
    }

    unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    close(sock);
    return mac_str;
}

static char *generate_uuid(void)
{
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }

    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 8; i++) g_uuid[i] = hex[rand() % 16];
    g_uuid[8] = '-';
    for (int i = 9; i < 13; i++) g_uuid[i] = hex[rand() % 16];
    g_uuid[13] = '-';
    g_uuid[14] = '4';
    for (int i = 15; i < 18; i++) g_uuid[i] = hex[rand() % 16];
    g_uuid[18] = '-';
    g_uuid[19] = hex[(rand() % 4) + 8];
    for (int i = 20; i < 23; i++) g_uuid[i] = hex[rand() % 16];
    g_uuid[23] = '-';
    for (int i = 24; i < 36; i++) g_uuid[i] = hex[rand() % 16];
    g_uuid[36] = '\0';

    return g_uuid;
}

/* ========== Base64 和 SHA1 (用于WebSocket握手) ========== */

static void base64_encode(const unsigned char *input, int len, char *output)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, j = 0;

    while (i < len) {
        int b1 = input[i++];
        int b2 = (i < len) ? input[i++] : 0;
        int b3 = (i < len) ? input[i++] : 0;

        output[j++] = table[(b1 >> 2) & 0x3F];
        output[j++] = table[((b1 << 4) | (b2 >> 4)) & 0x3F];
        output[j++] = (i > len + 1) ? '=' : table[((b2 << 2) | (b3 >> 6)) & 0x3F];
        output[j++] = (i > len) ? '=' : table[b3 & 0x3F];
    }
    output[j] = '\0';
}

static void sha1(const unsigned char *input, int len, unsigned char *output)
{
    unsigned int h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    for (int i = 0; i < len; i++) {
        unsigned int temp = ((h[0] << 5) | (h[0] >> 27)) + h[4] + input[i];
        temp ^= 0x5A827999;
        h[4] = h[3];
        h[3] = h[2];
        h[2] = (h[1] << 30) | (h[1] >> 2);
        h[1] = h[0];
        h[0] = temp;
    }

    for (int i = 0; i < 5; i++) {
        output[i * 4]     = (h[i] >> 24) & 0xFF;
        output[i * 4 + 1] = (h[i] >> 16) & 0xFF;
        output[i * 4 + 2] = (h[i] >> 8) & 0xFF;
        output[i * 4 + 3] = h[i] & 0xFF;
    }
}

static void compute_websocket_accept(const char *key, char *accept)
{
    char combined[128];
    snprintf(combined, sizeof(combined), "%s%s", key, WEBSOCKET_GUID);

    unsigned char sha1_hash[20];
    sha1((const unsigned char *)combined, strlen(combined), sha1_hash);

    base64_encode(sha1_hash, 20, accept);
}

/* ========== Socket I/O 超时封装 ========== */

static int ws_socket_write_timeout(int sockfd, const void *buf, size_t len, int timeout_ms)
{
    struct timeval start, now;
    gettimeofday(&start, NULL);

    while (1) {
        gettimeofday(&now, NULL);
        int elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                         (now.tv_usec - start.tv_usec) / 1000;
        if (elapsed_ms >= timeout_ms) {
            LOG_ERROR("Socket write timeout after %d ms", timeout_ms);
            return -2;
        }

        ssize_t ret = send(sockfd, buf, len, 0);
        if (ret < 0 && errno == EAGAIN) {
            usleep(10000);
            continue;
        }
        return (int)ret;
    }
}

static int ws_socket_read_timeout(int sockfd, void *buf, size_t len, int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);

    int ret = select(sockfd + 1, &read_fds, NULL, NULL, &tv);
    if (ret == 0) return -2;
    if (ret < 0) return -1;

    return (int)recv(sockfd, buf, len, 0);
}

/* ========== TCP 连接 ========== */

static int ws_connect_tcp(const char *hostname, const char *port)
{
    struct addrinfo hints, *addr_list, *cur;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    LOG_INFO("Connecting TCP to %s:%s...", hostname, port);

    int ret = getaddrinfo(hostname, port, &hints, &addr_list);
    if (ret != 0) {
        LOG_ERROR("getaddrinfo failed: %s", gai_strerror(ret));
        return -1;
    }

    int sockfd = -1;
    for (cur = addr_list; cur != NULL; cur = cur->ai_next) {
        sockfd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (sockfd < 0) continue;

        if (connect(sockfd, cur->ai_addr, cur->ai_addrlen) == 0) {
            LOG_INFO("TCP connected");
            break;
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(addr_list);

    if (sockfd < 0) {
        LOG_ERROR("TCP connect failed");
        return -1;
    }

    g_ws_sockfd = sockfd;
    return 0;
}

/* ========== mbedtls SSL/TLS ========== */

static int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int sockfd = (int)(intptr_t)ctx;
    return (int)send(sockfd, buf, len, 0);
}

static int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len)
{
    int sockfd = (int)(intptr_t)ctx;
    return (int)recv(sockfd, buf, len, 0);
}

static int ssl_init(const char *hostname)
{
    int ret;

    mbedtls_ssl_init(&g_ssl_ctx);
    mbedtls_ssl_config_init(&g_ssl_conf);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    mbedtls_x509_crt_init(&g_cacert);

    ret = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy,
                                (const unsigned char *)hostname, strlen(hostname));
    if (ret != 0) {
        LOG_ERROR("ctr_drbg_seed failed: %d", ret);
        goto error;
    }

    ret = mbedtls_ssl_config_defaults(&g_ssl_conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        LOG_ERROR("ssl_config_defaults failed: %d", ret);
        goto error;
    }

    mbedtls_ssl_conf_authmode(&g_ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&g_ssl_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);
    mbedtls_ssl_conf_ca_chain(&g_ssl_conf, &g_cacert, NULL);

    ret = mbedtls_ssl_setup(&g_ssl_ctx, &g_ssl_conf);
    if (ret != 0) {
        LOG_ERROR("ssl_setup failed: %d", ret);
        goto error;
    }

    mbedtls_ssl_set_bio(&g_ssl_ctx, (void *)(intptr_t)g_ws_sockfd,
                        mbedtls_net_send, mbedtls_net_recv, NULL);

    LOG_OK("SSL context initialized");
    return 0;

error:
    mbedtls_x509_crt_free(&g_cacert);
    mbedtls_ctr_drbg_free(&g_ctr_drbg);
    mbedtls_entropy_free(&g_entropy);
    mbedtls_ssl_config_free(&g_ssl_conf);
    mbedtls_ssl_free(&g_ssl_ctx);
    return ret;
}

static int ssl_handshake(void)
{
    int ret;
    LOG_INFO("Starting SSL handshake...");

    while (!mbedtls_ssl_is_handshake_over(&g_ssl_ctx)) {
        ret = mbedtls_ssl_handshake_step(&g_ssl_ctx);
        if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOG_ERROR("SSL handshake step failed: %d", ret);
            return ret;
        }
        usleep(10000);
    }

    LOG_OK("SSL handshake successful");
    return 0;
}

static int ssl_write(const unsigned char *buf, size_t len)
{
    if (!g_use_ssl) {
        return ws_socket_write_timeout(g_ws_sockfd, buf, len, 5000);
    }

    int ret;
    size_t written = 0;

    while (written < len) {
        ret = mbedtls_ssl_write(&g_ssl_ctx, buf + written, len - written);
        if (ret <= 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                usleep(10000);
                continue;
            }
            LOG_ERROR("SSL write failed: %d", ret);
            return ret;
        }
        written += ret;
    }

    return (int)written;
}

static int ssl_read(unsigned char *buf, size_t len, int timeout_ms)
{
    if (!g_use_ssl) {
        return ws_socket_read_timeout(g_ws_sockfd, buf, len, timeout_ms);
    }

    struct timeval start, now;
    gettimeofday(&start, NULL);

    while (1) {
        gettimeofday(&now, NULL);
        int elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                         (now.tv_usec - start.tv_usec) / 1000;
        if (elapsed_ms >= timeout_ms) return -2;

        int ret = mbedtls_ssl_read(&g_ssl_ctx, buf, len);
        if (ret <= 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                usleep(10000);
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
            return -1;
        }
        return ret;
    }
}

static void ssl_close(void)
{
    if (g_use_ssl) {
        mbedtls_x509_crt_free(&g_cacert);
        mbedtls_ctr_drbg_free(&g_ctr_drbg);
        mbedtls_entropy_free(&g_entropy);
        mbedtls_ssl_config_free(&g_ssl_conf);
        mbedtls_ssl_free(&g_ssl_ctx);
    }
}

/* ========== WebSocket 协议 ========== */

static int ws_send_handshake_request(const char *hostname, const char *path)
{
    /* 生成16字节随机数据作为WebSocket Key */
    unsigned char random_bytes[16];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int seed = (unsigned int)(tv.tv_sec ^ tv.tv_usec);

    for (int i = 0; i < 16; i++) {
        seed = seed * 1103515245 + 12345;
        random_bytes[i] = (unsigned char)(seed & 0xFF);
    }

    /* Base64编码16字节 -> 24字符 (含填充) */
    char key[25];
    const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0;

    for (int i = 0; i < 15; i += 3) {
        uint32_t octet_a = random_bytes[i];
        uint32_t octet_b = random_bytes[i + 1];
        uint32_t octet_c = random_bytes[i + 2];
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        key[j++] = base64_chars[(triple >> 18) & 0x3F];
        key[j++] = base64_chars[(triple >> 12) & 0x3F];
        key[j++] = base64_chars[(triple >> 6) & 0x3F];
        key[j++] = base64_chars[triple & 0x3F];
    }
    uint32_t last_octet = random_bytes[15];
    key[j++] = base64_chars[(last_octet >> 2) & 0x3F];
    key[j++] = base64_chars[(last_octet << 4) & 0x3F];
    key[j++] = '=';
    key[j++] = '=';
    key[j] = '\0';

    char request[1024];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer test_token\r\n"
        "Protocol-Version: 1\r\n"
        "Device-Id: %s\r\n"
        "Client-Id: %s\r\n"
        "\r\n",
        path, hostname, key, g_mac, g_uuid);

    LOG_INFO("Sending WebSocket handshake (%d bytes)...", req_len);

    int ret = ssl_write((const unsigned char *)request, req_len);
    if (ret < 0) {
        LOG_ERROR("Failed to send handshake");
        return -1;
    }

    char response[1024];
    ret = ssl_read((unsigned char *)response, sizeof(response) - 1, 10000);
    if (ret <= 0) {
        LOG_ERROR("Handshake response timeout");
        return -1;
    }

    response[ret] = '\0';

    if (strstr(response, "101 Switching Protocols")) {
        LOG_OK("WebSocket handshake successful");
        return 0;
    }

    LOG_ERROR("Handshake failed: %.*s", (ret > 100) ? 100 : ret, response);
    return -1;
}

static int ws_send_text_frame(const char *message, int msg_len)
{
    unsigned char frame[1024];
    int frame_len = 0;

    frame[0] = 0x81; /* text opcode, FIN bit set */

    if (msg_len <= 125) {
        frame[1] = msg_len;
        frame_len = 2;
    } else if (msg_len <= 65535) {
        frame[1] = 126;
        frame[2] = (msg_len >> 8) & 0xFF;
        frame[3] = msg_len & 0xFF;
        frame_len = 4;
    } else {
        LOG_ERROR("Message too long");
        return -1;
    }

    unsigned char mask[4];
    for (int i = 0; i < 4; i++) {
        mask[i] = (unsigned char)(rand() % 256);
    }

    frame[1] |= 0x80; /* mask bit */

    if (frame_len == 2) {
        frame[2] = mask[0];
        frame[3] = mask[1];
        frame[4] = mask[2];
        frame[5] = mask[3];
        frame_len = 6;
    } else {
        frame[4] = mask[0];
        frame[5] = mask[1];
        frame[6] = mask[2];
        frame[7] = mask[3];
        frame_len = 8;
    }

    memcpy(frame + frame_len, message, msg_len);
    for (int i = 0; i < msg_len; i++) {
        frame[frame_len + i] ^= mask[i % 4];
    }
    frame_len += msg_len;

    return ssl_write(frame, frame_len);
}

static int ws_receive_frame(char *buffer, int max_len, int timeout_ms)
{
    unsigned char header[2];
    int ret;

    ret = ssl_read(header, 2, timeout_ms);
    if (ret != 2) return (ret == -2) ? -2 : -1;

    int payload_len = header[1] & 0x7F;

    unsigned char mask[4] = {0};
    if (header[1] & 0x80) {
        ret = ssl_read(mask, 4, 1000);
        if (ret != 4) return -1;
    }

    int to_read = payload_len;
    if (payload_len == 126) {
        unsigned char ext[2];
        ret = ssl_read(ext, 2, 1000);
        if (ret != 2) return -1;
        to_read = (ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        unsigned char ext[8];
        ret = ssl_read(ext, 8, 1000);
        if (ret != 8) return -1;
        to_read = 0;
        for (int i = 0; i < 8; i++) {
            to_read = (to_read << 8) | ext[i];
        }
    }

    to_read = to_read < max_len - 1 ? to_read : max_len - 1;
    ret = ssl_read((unsigned char *)buffer, to_read, 5000);
    if (ret <= 0) return -1;

    if (header[1] & 0x80) {
        for (int i = 0; i < ret; i++) {
            buffer[i] ^= mask[i % 4];
        }
    }

    buffer[ret] = '\0';
    return ret;
}

/* ========== 公开 API 实现 ========== */

int xiaozhi_ai_ws_init(void)
{
    /* 获取MAC地址 */
    char *mac = get_wireless_mac_address();
    if (mac) {
        strncpy(g_mac, mac, sizeof(g_mac) - 1);
        g_mac[sizeof(g_mac) - 1] = '\0';
        LOG_INFO("MAC: %s", g_mac);
    } else {
        LOG_WARN("Failed to get MAC address");
        strncpy(g_mac, "00:00:00:00:00:00", sizeof(g_mac) - 1);
    }

    /* 生成UUID */
    generate_uuid();
    LOG_INFO("UUID: %s", g_uuid);

    g_ws_state = WS_STATE_DISCONNECTED;
    LOG_OK("WS module initialized");
    return 0;
}

void xiaozhi_ai_ws_deinit(void)
{
    xiaozhi_ai_ws_disconnect();
}

int xiaozhi_ai_ws_connect(const char *hostname, const char *port, const char *path)
{
    g_ws_state = WS_STATE_CONNECTING;

    /* TCP连接 */
    if (ws_connect_tcp(hostname, port) != 0) {
        LOG_ERROR("TCP connect failed");
        g_ws_state = WS_STATE_ERROR;
        return -1;
    }

    /* SSL初始化 */
    if (g_use_ssl) {
        if (ssl_init(hostname) != 0) {
            LOG_ERROR("SSL init failed");
            close(g_ws_sockfd);
            g_ws_sockfd = -1;
            g_ws_state = WS_STATE_ERROR;
            return -1;
        }

        if (ssl_handshake() != 0) {
            LOG_ERROR("SSL handshake failed");
            ssl_close();
            close(g_ws_sockfd);
            g_ws_sockfd = -1;
            g_ws_state = WS_STATE_ERROR;
            return -1;
        }
    }

    /* WebSocket握手 */
    g_ws_state = WS_STATE_HANDSHAKING;
    if (ws_send_handshake_request(hostname, path) != 0) {
        LOG_ERROR("WS handshake failed");
        if (g_use_ssl) ssl_close();
        close(g_ws_sockfd);
        g_ws_sockfd = -1;
        g_ws_state = WS_STATE_ERROR;
        return -1;
    }

    /* 发送hello消息 */
    g_ws_state = WS_STATE_CONNECTED;
    const char *audio_format = xiaozhi_ai_ws_is_audio_available() ?
        "opus" : "opus"; /* 仍声明opus格式，服务器可降级为纯文本 */
    char hello_msg[512];
    snprintf(hello_msg, sizeof(hello_msg),
        "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"%s\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60}}",
        audio_format);

    LOG_INFO("Sending hello: %s", hello_msg);
    ws_send_text_frame(hello_msg, strlen(hello_msg));

    LOG_OK("Connected to %s:%s%s", hostname, port, path);
    return 0;
}

void xiaozhi_ai_ws_disconnect(void)
{
    if (g_ws_sockfd >= 0 && g_ws_state == WS_STATE_CONNECTED) {
        /* 发送WebSocket关闭帧 */
        unsigned char close_frame[6] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
        if (g_use_ssl) {
            ssl_write(close_frame, 6);
        } else {
            send(g_ws_sockfd, close_frame, 6, 0);
        }
    }

    if (g_use_ssl) {
        ssl_close();
    }

    if (g_ws_sockfd >= 0) {
        close(g_ws_sockfd);
        g_ws_sockfd = -1;
    }

    g_ws_state = WS_STATE_DISCONNECTED;
    LOG_INFO("Disconnected");
}

bool xiaozhi_ai_ws_is_connected(void)
{
    return g_ws_state == WS_STATE_CONNECTED;
}

ws_connection_state_t xiaozhi_ai_ws_get_state(void)
{
    return g_ws_state;
}

int xiaozhi_ai_ws_send_json(const char *json)
{
    if (g_ws_state != WS_STATE_CONNECTED) {
        LOG_ERROR("Not connected, cannot send");
        return -1;
    }

    return ws_send_text_frame(json, strlen(json));
}

int xiaozhi_ai_ws_receive(char *buf, int max_len, int timeout_ms)
{
    if (g_ws_state != WS_STATE_CONNECTED) {
        return -1;
    }

    return ws_receive_frame(buf, max_len, timeout_ms);
}

bool xiaozhi_ai_ws_is_audio_available(void)
{
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
    return true;
#else
    return false;
#endif
}

const char *xiaozhi_ai_ws_get_uuid(void)
{
    return g_uuid;
}

const char *xiaozhi_ai_ws_get_mac(void)
{
    return g_mac;
}

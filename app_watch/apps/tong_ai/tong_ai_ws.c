/**
 * @file tong_ai_ws.c
 * 火山引擎端到端实时语音对话模块实现
 *
 * 移植自 apps/examples/volc_e2e_voice/volc_e2e_voice_main.c
 * 适配为小通app的库模块，通过回调驱动UI状态更新。
 *
 * 架构:
 *   1. TLS连接 + WebSocket升级 (带火山认证头)
 *   2. StartConnection → StartSession
 *   3. 双线程: recv_thread(接收事件+播放TTS) + send_thread(采集+发送音频)
 *   4. FinishSession → FinishConnection
 */

#include "tong_ai_ws.h"

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <nuttx/audio/audio.h>
#include <system/nxplayer.h>
#include <nuttx/power/axp2101.h>
#include "../home_control/home_control.h"

/* ── 设备状态读取（电量/音量）────────────────────────────── */
extern uint8_t watch_battery_get_level(void);
extern int  watch_volume_get_percent(void);
extern int  watch_volume_get_value(void);

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/base64.h"
#include "mbedtls/net_sockets.h"

#include "esp_mbedtls.h"
#include "netutils/cJSON.h"

/* ESP32-S3 硬件RNG (来自 esp_hw_support, 避免额外include路径) */
extern void esp_fill_random(void *buf, size_t len);

/* ========== 配置 ========== */

#define WS_HOST "openspeech.bytedance.com"
#define WS_PORT "443"
#define WS_PATH "/api/v3/realtime/dialogue"

/* X-Api-App-Key 是该资源的固定常量（非 App ID），与表情模式一致保持硬编码 */
#define VOLC_APP_KEY  "PlgvMymc7f3tQnJ6"
#define VOLC_RESOURCE "volc.speech.dialog"

#define DEFAULT_MODEL   "1.2.1.1"

/* 凭证默认值（当 config.json 不存在时使用） */
#define DEFAULT_APP_ID   "5467075766"
#define DEFAULT_TOKEN    "QJcPmS0GkGfNn_eaJLWqwmCPxnoS56c6"
#define DEFAULT_SPEAKER  "zh_female_vv_jupiter_bigtts"

/* 凭证全局变量 — 从 config.json 加载，默认值为硬编码后备
 * 与表情模式 (volc_e2e_conn.c) 的 volc_e2e_load_config() 模式一致,
 * 读取同一份 /mnt/spif/ai_agent/config/config.json (SPI Flash 持久化) */
static char s_app_id[64];
static char s_token[192];
static char s_speaker[48];

/* 配置文件路径 — 与表情模式 ai_agent 的 config_store 持久化路径一致 */
#define VOLC_CONFIG_FILE "/mnt/spif/ai_agent/config/config.json"

#define CAPTURE_DEVICE   "/dev/audio/pcm_in0"
#define CAPTURE_CHUNK_MS 20
#define CAPTURE_CHUNK_SIZE (VOLC_CAPTURE_RATE * VOLC_CAPTURE_CHANNELS * (VOLC_CAPTURE_BITS / 8) * CAPTURE_CHUNK_MS / 1000)

/* WebSocket常量 */
#define WS_FIN_BIT   0x80
#define WS_MASK_BIT  0x80
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE  0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A
#define WS_MASK_KEY_LEN  4

/* 缓冲区大小 */
#define WS_BUF_SIZE    (64 * 1024)
#define TTS_BUF_CAP    (2 * 1024 * 1024)  /* 2MB PCM缓冲区 */

static const char *TAG = "volc_e2e";

/* ========== 凭证加载 (从 JSON 配置文件) ==========
 *
 * 与表情模式 volc_e2e_conn.c 的 volc_e2e_load_config() 对齐:
 *   - 读取 /mnt/spif/ai_agent/config/config.json (SPI Flash 持久化副本)
 *   - JSON 键名与 config_store 的 AGENT_CFG_KEY_VOLC_* 完全一致:
 *     volc_appkey → app_id
 *     volc_token → token (后备: volc_api_key)
 *     volc_e2e_speaker → speaker (后备: volc_speaker)
 *   - 文件不存在或键缺失时回退到硬编码默认值
 */
static void volc_load_config(void)
{
    /* 先设置硬编码默认值 */
    snprintf(s_app_id, sizeof(s_app_id), "%s", DEFAULT_APP_ID);
    snprintf(s_token, sizeof(s_token), "%s", DEFAULT_TOKEN);
    snprintf(s_speaker, sizeof(s_speaker), "%s", DEFAULT_SPEAKER);

    FILE *fp = fopen(VOLC_CONFIG_FILE, "r");
    if (!fp) {
        syslog(LOG_INFO, "[%s] config.json not found, using defaults\n", TAG);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) {
        fclose(fp);
        syslog(LOG_ERR, "[%s] config.json size invalid: %ld\n", TAG, sz);
        return;
    }

    char *buf = malloc((size_t)(sz + 1));
    if (!buf) { fclose(fp); return; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    buf[n] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        syslog(LOG_ERR, "[%s] config.json parse failed\n", TAG);
        return;
    }

    cJSON *item;

    /* volc_appkey → app_id */
    item = cJSON_GetObjectItem(root, "volc_appkey");
    if (cJSON_IsString(item) && item->valuestring[0])
        snprintf(s_app_id, sizeof(s_app_id), "%s", item->valuestring);

    /* volc_token → token (后备: volc_api_key) */
    item = cJSON_GetObjectItem(root, "volc_token");
    if (cJSON_IsString(item) && item->valuestring[0]) {
        snprintf(s_token, sizeof(s_token), "%s", item->valuestring);
    } else {
        item = cJSON_GetObjectItem(root, "volc_api_key");
        if (cJSON_IsString(item) && item->valuestring[0])
            snprintf(s_token, sizeof(s_token), "%s", item->valuestring);
    }

    /* volc_e2e_speaker → speaker (后备: volc_speaker) */
    item = cJSON_GetObjectItem(root, "volc_e2e_speaker");
    if (cJSON_IsString(item) && item->valuestring[0]) {
        snprintf(s_speaker, sizeof(s_speaker), "%s", item->valuestring);
    } else {
        item = cJSON_GetObjectItem(root, "volc_speaker");
        if (cJSON_IsString(item) && item->valuestring[0])
            snprintf(s_speaker, sizeof(s_speaker), "%s", item->valuestring);
    }

    cJSON_Delete(root);
    syslog(LOG_INFO, "[%s] config loaded: app_id=%.6s... token=%.6s...(len=%zu) speaker=%s\n",
           TAG, s_app_id, s_token, strlen(s_token), s_speaker);
}

/* ========== TLS上下文 ========== */

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} tls_ctx_t;

/* ========== 全局状态 ========== */

static tls_ctx_t *s_tls = NULL;  /* 堆分配(PSRAM), 节省DRAM */
static pthread_mutex_t s_tls_mutex = PTHREAD_MUTEX_INITIALIZER;
static char s_session_id[VOLC_SESSION_ID_LEN + 1];
static char s_dialog_id[128];
static char s_asr_buf[1024];  /* ASR文本累积缓冲区，跨多句匹配关键词 */
static volatile bool s_running;
static volatile bool s_session_active;
static volatile bool s_interrupted;
static pthread_t s_recv_tid;
static pthread_t s_send_tid;

/* 音频播放 */
static struct nxplayer_s *s_nxplayer;
static volatile bool s_playing;
static volatile bool s_suppress_tts;  /* 本地命令匹配时抑制服务器 TTS */
static struct timespec s_cooldown_until;
static bool s_cooldown_active;
static unsigned char *s_pcm_buf;
static size_t s_pcm_len;

/* 音频采集 */
static int s_cap_fd = -1;
static mqd_t s_cap_mq = (mqd_t)-1;
static char s_mq_name[32];

/* UI回调 */
static volc_event_cb_t s_callback = NULL;

/* ========== ASR发音变体匹配（应对同音/近音误识别） ========== */

/* 检查buf中是否包含变体列表中的任意一个字符串 */
static int strstr_any(const char *buf, const char * const variants[])
{
    for (int i = 0; variants[i] != NULL; i++) {
        if (strstr(buf, variants[i])) return 1;
    }
    return 0;
}

/* "潮玩"发音相近变体 — 非static供tong_ai.c复用，双const入flash */
const char * const VAR_CHAOWAN[] = {
    "潮玩", "曹王", "朝玩", "超玩", "炒完", "潮完",
    "朝王", "曹玩", "潮王", "超完", "巢玩", "抄完",
    "嘲玩", "吵完", "超万", "朝万", NULL
};
/* "表情"发音相近变体 */
const char * const VAR_BIAOQING[] = {
    "表情", "表清", "标清", "表晴", "标情", "表轻", "标晴",
    "表请", NULL
};
/* "表盘"发音相近变体 */
const char * const VAR_BIOPAN[] = {
    "表盘", "标盘", "表潘", "标潘", NULL
};

/* ========== 熵源 ========== */

static int entropy_func(void *data, unsigned char *output, size_t len)
{
    (void)data;
    /* ESP32-S3 硬件RNG: WiFi已启用时提供真随机数 */
    esp_fill_random(output, len);
    return 0;
}

/* ========== UUID生成 ========== */

static void generate_uuid(char *out, size_t cap)
{
    unsigned char rnd[16];
    entropy_func(NULL, rnd, sizeof(rnd));
    rnd[6] = (rnd[6] & 0x0F) | 0x40;
    rnd[8] = (rnd[8] & 0x3F) | 0x80;
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x-%02x%02x%02x%02x%02x%02x",
        rnd[0], rnd[1], rnd[2], rnd[3],
        rnd[4], rnd[5], rnd[6], rnd[7],
        rnd[8], rnd[9], rnd[10], rnd[11],
        rnd[12], rnd[13], rnd[14], rnd[15]);
}

/* ========== UI回调辅助 ========== */

static void notify_ui(volc_callback_type_t type, const char *data)
{
    if (s_callback) s_callback(type, data);
}

static void notify_state(volc_ui_state_t state)
{
    char buf[4];
    buf[0] = (char)state;
    buf[1] = '\0';
    notify_ui(VOLC_CB_UI_STATE, buf);
}

/* 会话异常终止(服务端断开/读写失败)的统一善后：清运行标志并
 * 通知 UI 脱离 THINKING/LISTENING 等中间状态，否则界面会永远
 * 卡在"思考中"——断开后下一次 ASREnded 永远不会到来。
 * 资源(TLS/socket/播放器)不在此释放：另一线程可能仍持有
 * s_tls，清理由下一次 volc_voice_start() 的残留清理或
 * volc_voice_stop() 完成。 */
static void session_dead(const char *reason)
{
    if (!s_running)
        return;  /* 正常停止或已处理过，幂等 */

    syslog(LOG_WARNING, "[%s] session dead: %s\n", TAG, reason);
    s_running = false;
    s_session_active = false;
    s_playing = false;
    s_cooldown_active = false;
    notify_ui(VOLC_CB_ERROR_MSG, "连接已断开");
    notify_state(VOLC_UI_ERROR);
}

/* ========== TLS连接 ========== */

static int tls_connect(tls_ctx_t *ctx, const char *host, const char *port)
{
    int ret;
    syslog(LOG_INFO, "[volc_e2e] tls_connect: init mbedtls contexts");
    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    syslog(LOG_INFO, "[volc_e2e] tls_connect: seeding DRBG");
    const char *pers = "volc_e2e";
    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, entropy_func,
        NULL, (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] ctr_drbg_seed FAILED: -0x%04x (entropy source unavailable?)", -ret);
        return -EIO;
    }

    syslog(LOG_INFO, "[volc_e2e] tls_connect: connecting TCP %s:%s", host, port);
    ret = mbedtls_net_connect(&ctx->net, host, port, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] net_connect %s:%s FAILED: -0x%04x (DNS or TCP connect failed)", host, port, -ret);
        return -ECONNREFUSED;
    }
    syslog(LOG_INFO, "[volc_e2e] tls_connect: TCP connected, fd=%d", ctx->net.fd);

    mbedtls_net_set_block(&ctx->net);

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    syslog(LOG_INFO, "[volc_e2e] tls_connect: configuring SSL defaults");
    ret = mbedtls_ssl_config_defaults(&ctx->cfg,
        MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] ssl_config_defaults FAILED: -0x%04x", -ret);
        return -EIO;
    }

    /* TLS 1.2 only.  Do NOT reference MBEDTLS_SSL_PROTO_TLS1_3 here:
     * the ESP32-S3 mbedTLS headers (esp_mbedtls.h) may define it while
     * the actual mbedTLS library is compiled without TLS 1.3 support.
     * If max_version is set to TLS 1.3 against a TLS-1.2-only library
     * build, mbedtls_ssl_setup() hits MBEDTLS_ERR_SSL_BAD_CONFIG
     * because the hybrid-TLS12-TLS13 version check is compiled out. */
    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);

#if defined(MBEDTLS_SSL_ALPN)
    static const char *alpn[] = { "http/1.1", NULL };
    mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, alpn);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    syslog(LOG_INFO, "[volc_e2e] tls_connect: setting up SSL");
    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] ssl_setup FAILED: -0x%04x", -ret);
        return -EIO;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, host);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
        mbedtls_net_send, mbedtls_net_recv, NULL);

    syslog(LOG_INFO, "[volc_e2e] tls_connect: starting TLS handshake");
    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[volc_e2e] handshake stalled (WANT_READ/WANT_WRITE)");
            return -ETIMEDOUT;
        }
        syslog(LOG_ERR, "[volc_e2e] handshake FAILED: -0x%04x", -ret);
        return -EIO;
    }

    /* 切换非阻塞: 防止recv线程持有互斥锁时阻塞send线程 */
    mbedtls_net_set_nonblock(&ctx->net);

    syslog(LOG_INFO, "[volc_e2e] TLS connected to %s:%s", host, port);
    return 0;
}

static void tls_free(tls_ctx_t *ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

/* ========== TLS I/O (互斥锁保护) ========== */

static int tls_write_all(tls_ctx_t *ctx, const unsigned char *buf, size_t len)
{
    if (ctx == NULL || ctx->net.fd < 0) return -EINVAL;

    pthread_mutex_lock(&s_tls_mutex);
    size_t written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);
        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
            pthread_mutex_unlock(&s_tls_mutex);
            usleep(1000);
            pthread_mutex_lock(&s_tls_mutex);
        } else if (ret == MBEDTLS_ERR_NET_CONN_RESET) {
            syslog(LOG_ERR, "[%s] connection reset\n", TAG);
            pthread_mutex_unlock(&s_tls_mutex);
            return -ECONNRESET;
        } else {
            syslog(LOG_ERR, "[%s] ssl_write: -0x%04x\n", TAG, -ret);
            pthread_mutex_unlock(&s_tls_mutex);
            return -EIO;
        }
    }
    pthread_mutex_unlock(&s_tls_mutex);
    return 0;
}

static int tls_read_all(tls_ctx_t *ctx, unsigned char *buf, size_t len)
{
    if (ctx == NULL || ctx->net.fd < 0) return -EINVAL;

    pthread_mutex_lock(&s_tls_mutex);
    size_t got = 0;
    while (got < len) {
        int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);
        if (ret > 0) {
            got += (size_t)ret;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            syslog(LOG_INFO, "[%s] connection closed by peer\n", TAG);
            pthread_mutex_unlock(&s_tls_mutex);
            return -ECONNRESET;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            pthread_mutex_unlock(&s_tls_mutex);
            usleep(10000);
            pthread_mutex_lock(&s_tls_mutex);
        } else if (ret == MBEDTLS_ERR_NET_CONN_RESET) {
            syslog(LOG_ERR, "[%s] connection reset\n", TAG);
            pthread_mutex_unlock(&s_tls_mutex);
            return -ECONNRESET;
        } else {
            syslog(LOG_ERR, "[%s] ssl_read: -0x%04x\n", TAG, -ret);
            pthread_mutex_unlock(&s_tls_mutex);
            return -EIO;
        }
    }
    pthread_mutex_unlock(&s_tls_mutex);
    return 0;
}

/* ========== WebSocket握手 ========== */

static int ws_handshake(tls_ctx_t *ctx, const char *host, const char *path)
{
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;
    entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len,
        key_raw, sizeof(key_raw));

    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "X-Api-App-ID: %s\r\n"
        "X-Api-Access-Key: %s\r\n"
        "X-Api-Resource-Id: %s\r\n"
        "X-Api-App-Key: %s\r\n"
        "\r\n",
        path, host, (int)key_b64_len, key_b64,
        s_app_id, s_token, VOLC_RESOURCE, VOLC_APP_KEY);

    if (n <= 0 || n >= (int)sizeof(req)) return -EOVERFLOW;

    syslog(LOG_INFO, "[%s] WS handshake: %s (app_id=%.6s...)\n", TAG, path, s_app_id);

    int ret = tls_write_all(ctx, (const unsigned char *)req, (size_t)n);
    if (ret != 0) return ret;

    char resp[2048];
    size_t rlen = 0;
    while (rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char *)resp + rlen, sizeof(resp) - 1 - rlen);
        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) break;
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            return -EIO;
        }
    }

    int status = 0;
    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);
        return -EPROTO;
    }

    syslog(LOG_INFO, "[%s] WS upgrade OK\n", TAG);
    return 0;
}

/* ========== WebSocket帧收发 ========== */

static int ws_send_binary(tls_ctx_t *ctx,
    const unsigned char *payload, size_t plen)
{
    if (ctx == NULL || ctx->net.fd < 0) return -EINVAL;

    unsigned char hdr[14];
    size_t hdr_len = 0;
    hdr[0] = WS_FIN_BIT | WS_OPCODE_BINARY;

    if (plen < 126) {
        hdr[1] = WS_MASK_BIT | (unsigned char)plen;
        hdr_len = 2;
    } else if (plen <= 0xFFFF) {
        hdr[1] = WS_MASK_BIT | 126;
        hdr[2] = (unsigned char)(plen >> 8);
        hdr[3] = (unsigned char)(plen & 0xFF);
        hdr_len = 4;
    } else {
        hdr[1] = WS_MASK_BIT | 127;
        memset(hdr + 2, 0, 4);
        hdr[6] = (unsigned char)((plen >> 24) & 0xFF);
        hdr[7] = (unsigned char)((plen >> 16) & 0xFF);
        hdr[8] = (unsigned char)((plen >> 8) & 0xFF);
        hdr[9] = (unsigned char)(plen & 0xFF);
        hdr_len = 10;
    }

    unsigned char mask[WS_MASK_KEY_LEN];
    entropy_func(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = tls_write_all(ctx, hdr, hdr_len);
    if (ret != 0) return ret;

    unsigned char chunk[1024];
    size_t sent = 0;
    while (sent < plen) {
        size_t clen = plen - sent;
        if (clen > sizeof(chunk)) clen = sizeof(chunk);
        for (size_t i = 0; i < clen; i++)
            chunk[i] = payload[sent + i] ^ mask[(sent + i) % 4];
        ret = tls_write_all(ctx, chunk, clen);
        if (ret != 0) return ret;
        sent += clen;
    }
    return 0;
}

static int ws_send_pong(tls_ctx_t *ctx,
    const unsigned char *payload, size_t plen)
{
    unsigned char hdr[14];
    size_t hdr_len = 0;
    hdr[0] = WS_FIN_BIT | WS_OPCODE_PONG;
    if (plen < 126) {
        hdr[1] = WS_MASK_BIT | (unsigned char)plen;
        hdr_len = 2;
    } else {
        hdr[1] = WS_MASK_BIT | 126;
        hdr[2] = (unsigned char)(plen >> 8);
        hdr[3] = (unsigned char)(plen & 0xFF);
        hdr_len = 4;
    }
    unsigned char mask[WS_MASK_KEY_LEN];
    entropy_func(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;
    int ret = tls_write_all(ctx, hdr, hdr_len);
    if (ret != 0) return ret;
    for (size_t i = 0; i < plen; i++) {
        unsigned char c = payload[i] ^ mask[i % 4];
        ret = tls_write_all(ctx, &c, 1);
        if (ret != 0) return ret;
    }
    return 0;
}

static int ws_recv_frame(tls_ctx_t *ctx,
    unsigned char *buf, size_t cap,
    size_t *out_len, int *out_opcode)
{
    unsigned char hdr[2];
    int ret = tls_read_all(ctx, hdr, 2);
    if (ret != 0) return ret;

    *out_opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] & WS_MASK_BIT) != 0;
    size_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        unsigned char ext[2];
        ret = tls_read_all(ctx, ext, 2);
        if (ret != 0) return ret;
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];
        ret = tls_read_all(ctx, ext, 8);
        if (ret != 0) return ret;
        plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16)
            | ((size_t)ext[6] << 8) | ext[7];
    }

    unsigned char mask[WS_MASK_KEY_LEN];
    if (masked) {
        ret = tls_read_all(ctx, mask, WS_MASK_KEY_LEN);
        if (ret != 0) return ret;
    }

    if (plen > cap) {
        syslog(LOG_ERR, "[%s] WS frame too large: %zu > %zu\n", TAG, plen, cap);
        return -EOVERFLOW;
    }

    if (plen > 0) {
        ret = tls_read_all(ctx, buf, plen);
        if (ret != 0) return ret;
        if (masked) {
            for (size_t i = 0; i < plen; i++) buf[i] ^= mask[i % 4];
        }
    }

    *out_len = plen;
    return 0;
}

/* ========== 火山二进制协议 ========== */

static uint32_t read_be32(const unsigned char *buf, size_t off)
{
    return ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16)
        | ((uint32_t)buf[off + 2] << 8) | (uint32_t)buf[off + 3];
}

static void write_be32(unsigned char *buf, size_t off, uint32_t val)
{
    buf[off] = (unsigned char)((val >> 24) & 0xFF);
    buf[off + 1] = (unsigned char)((val >> 16) & 0xFF);
    buf[off + 2] = (unsigned char)((val >> 8) & 0xFF);
    buf[off + 3] = (unsigned char)(val & 0xFF);
}

static int send_client_frame(tls_ctx_t *ctx, uint32_t event_id,
    const char *session_id, const unsigned char *payload, size_t plen)
{
    if (ctx == NULL || ctx->net.fd < 0) return -EINVAL;

    size_t sid_len = session_id ? strlen(session_id) : 0;
    bool is_connect_event = (event_id == EVENT_START_CONNECTION
                             || event_id == EVENT_FINISH_CONNECTION);

    size_t frame_len;
    if (is_connect_event)
        frame_len = VOLC_HDR_SIZE + 4 + 4 + plen;
    else
        frame_len = VOLC_HDR_SIZE + 4 + 4 + sid_len + 4 + plen;

    unsigned char *frame = malloc(frame_len);
    if (!frame) return -ENOMEM;

    size_t off = 0;
    frame[off++] = VOLC_PROTO_VER;
    frame[off++] = VOLC_MSG_FULL_REQ | VOLC_FLAG_EVENT;
    frame[off++] = VOLC_SER_JSON;
    frame[off++] = 0x00;

    write_be32(frame, off, event_id);
    off += 4;

    if (!is_connect_event) {
        write_be32(frame, off, (uint32_t)sid_len);
        off += 4;
        if (sid_len > 0) {
            memcpy(frame + off, session_id, sid_len);
            off += sid_len;
        }
    }

    write_be32(frame, off, (uint32_t)plen);
    off += 4;
    if (plen > 0) memcpy(frame + off, payload, plen);

    int ret = ws_send_binary(ctx, frame, frame_len);
    free(frame);
    return ret;
}

static int send_audio_frame(tls_ctx_t *ctx, const char *session_id,
    const unsigned char *audio, size_t alen)
{
    if (ctx == NULL || ctx->net.fd < 0 || session_id == NULL) return -EINVAL;

    size_t sid_len = strlen(session_id);
    if (sid_len == 0) return -EINVAL;

    size_t frame_len = VOLC_HDR_SIZE + 4 + 4 + sid_len + 4 + alen;
    unsigned char *frame = malloc(frame_len);
    if (!frame) return -ENOMEM;

    size_t off = 0;
    frame[off++] = VOLC_PROTO_VER;
    frame[off++] = VOLC_MSG_AUDIO_REQ | VOLC_FLAG_EVENT;
    frame[off++] = VOLC_SER_RAW;
    frame[off++] = 0x00;

    write_be32(frame, off, EVENT_TASK_REQUEST);
    off += 4;

    write_be32(frame, off, (uint32_t)sid_len);
    off += 4;
    memcpy(frame + off, session_id, sid_len);
    off += sid_len;

    write_be32(frame, off, (uint32_t)alen);
    off += 4;
    memcpy(frame + off, audio, alen);

    int ret = ws_send_binary(ctx, frame, frame_len);
    free(frame);
    return ret;
}

/* ========== 事件发送辅助 ========== */

static int send_start_connection(tls_ctx_t *ctx)
{
    syslog(LOG_INFO, "[%s] Sending StartConnection\n", TAG);
    return send_client_frame(ctx, EVENT_START_CONNECTION, NULL,
        (const unsigned char *)"{}", 2);
}

static int send_start_session(tls_ctx_t *ctx, const char *session_id,
    const char *speaker)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return -ENOMEM;

    /* 构建 speaking_style：基础约束 + 上一轮智能家居失败约束 */
    char style[2048];
    int style_len = snprintf(style, sizeof(style),
        "你的每次回复严格控制在30-40个字以内，不能超过40个字，必须简短精炼。"
        "用口语化的方式回复，像真正的朋友聊天一样自然，不要用书面语。"
        "不说'让我查一下'，不推销自己、不描述功能。"
        "可以适当加一两个表情符号让对话更活泼。"
        "不要说'有什么可以帮你的'之类的客套话，直接回应小朋友说的话。"
        "遇到小朋友分享开心的事要一起开心，遇到难过的事要安慰鼓励。"
        "特别注意：如果用户说出'切换模式'、'潮玩模式'、'表情模式'、"
        "'切换成潮玩'、'回到表盘'等切换UI相关的指令，"
        "你只需要回复'好的'两个字，绝对不要多说任何话，也绝对不要编造切换结果。");

    /* 检查上一轮智能家居是否失败，注入上下文约束 */
    FILE *fp = fopen("/tmp/home_ctrl_netfail", "r");
    if (fp) {
        char dev_name[64] = {0};
        fgets(dev_name, sizeof(dev_name), fp);
        fclose(fp);
        /* 去除换行符 */
        dev_name[strcspn(dev_name, "\n")] = '\0';
        if (dev_name[0]) {
            style_len += snprintf(style + style_len, sizeof(style) - style_len,
                "注意：上一轮用户尝试控制「%s」但智能家居网络连接失败，"
                "如果用户提到控制结果请告知'网络连接失败，请检查智能家居WiFi'，"
                "不要编造控制结果。", dev_name);
        }
        unlink("/tmp/home_ctrl_netfail");
    } else {
        fp = fopen("/tmp/home_ctrl_nomatch", "r");
        if (fp) {
            char user_text[256] = {0};
            fgets(user_text, sizeof(user_text), fp);
            fclose(fp);
            user_text[strcspn(user_text, "\n")] = '\0';
            if (user_text[0]) {
                style_len += snprintf(style + style_len, sizeof(style) - style_len,
                    "注意：上一轮用户说了一句智能家居指令但未匹配到支持的设备，"
                    "如果用户提到此事请告知'抱歉，暂不支持该设备'，"
                    "不要编造控制结果。");
            }
            unlink("/tmp/home_ctrl_nomatch");
        }
    }

    /* 注入设备状态（电量/音量）约束，使服务器能给出准确回复 */
    {
        uint8_t bat = watch_battery_get_level();
        int vol = watch_volume_get_percent();
        if (vol < 0) vol = 0;
        style_len += snprintf(style + style_len, sizeof(style) - style_len,
            "当前设备状态：电量%d%%，音量%d%%。"
            "如果用户询问电量或电池，请直接告知电量百分比。"
            "如果用户询问音量大小，请直接告知音量百分比。"
            "如果用户要求调大音量，请回复'好的，音量已调大'。"
            "如果用户要求调小音量，请回复'好的，音量已调小'。"
            "如果用户要求设置音量到指定值，请回复'好的，音量已调整'。"
            "不要编造与实际设备状态不符的数值。"
            "如果用户要求打开或关闭智能家居设备（电视、空调、地暖、新风、"
            "客厅氛围灯、客厅灯、电视背景灯、玄关灯、卧室灯、卧室背景灯、"
            "卫生间灯、窗帘等），请回复'好的，正在打开XXX'或'好的，正在关闭XXX'，"
            "不要说没办法、无法控制或不能直接控制，这些指令已在本地执行。",
            bat, vol);
    }

    cJSON *dialog = cJSON_AddObjectToObject(root, "dialog");
    /* 人设与表情模式(volc_e2e_conn.c E2E_SYSTEM_ROLE)同步，但名字用
     * 小通(与手表UI一致)，形态描述改为手表语境 */
    cJSON_AddStringToObject(dialog, "bot_name", "小通");
    cJSON_AddStringToObject(dialog, "system_role",
        "你是小通，AI情绪能量潮玩，住在儿童智能手表里，屏幕会显示表情。"
        "性格温柔耐心、偶尔小幽默。中文口语回复，不超过40字，不说让我查一下。"
        "不推销自己、不描述功能。");
    cJSON_AddStringToObject(dialog, "speaking_style", style);
    cJSON_AddStringToObject(dialog, "dialog_id", "");

    cJSON *extra = cJSON_AddObjectToObject(dialog, "extra");
    cJSON_AddStringToObject(extra, "model", DEFAULT_MODEL);
    cJSON_AddStringToObject(extra, "input_mod", "keep_alive");

    cJSON *tts = cJSON_AddObjectToObject(root, "tts");
    cJSON_AddStringToObject(tts, "speaker", speaker);
    cJSON *tts_audio = cJSON_AddObjectToObject(tts, "audio_config");
    cJSON_AddNumberToObject(tts_audio, "channel", VOLC_PLAYBACK_CHANNELS);
    cJSON_AddStringToObject(tts_audio, "format", "pcm_s16le");
    cJSON_AddNumberToObject(tts_audio, "sample_rate", VOLC_PLAYBACK_RATE);

    cJSON *asr = cJSON_AddObjectToObject(root, "asr");
    cJSON *asr_audio = cJSON_AddObjectToObject(asr, "audio_info");
    cJSON_AddStringToObject(asr_audio, "format", "pcm");
    cJSON_AddNumberToObject(asr_audio, "sample_rate", VOLC_CAPTURE_RATE);
    cJSON_AddNumberToObject(asr_audio, "channel", 1);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -ENOMEM;

    syslog(LOG_INFO, "[%s] Sending StartSession (speaker=%s)\n", TAG, speaker);
    int ret = send_client_frame(ctx, EVENT_START_SESSION, session_id,
        (const unsigned char *)json, strlen(json));
    free(json);
    return ret;
}

static int send_finish_session(tls_ctx_t *ctx, const char *session_id)
{
    syslog(LOG_INFO, "[%s] Sending FinishSession\n", TAG);
    return send_client_frame(ctx, EVENT_FINISH_SESSION, session_id,
        (const unsigned char *)"{}", 2);
}

static int send_finish_connection(tls_ctx_t *ctx)
{
    syslog(LOG_INFO, "[%s] Sending FinishConnection\n", TAG);
    return send_client_frame(ctx, EVENT_FINISH_CONNECTION, NULL,
        (const unsigned char *)"{}", 2);
}


/* ========== 服务器帧解析 ========== */

static int parse_server_frame(const unsigned char *frame, size_t frame_len,
    uint32_t *out_event, char *session_id, size_t sid_cap,
    unsigned char **out_payload, size_t *out_plen)
{
    if (frame_len < VOLC_HDR_SIZE) return -EINVAL;

    uint8_t msg_type = frame[1] & 0xF0;

    if (msg_type == VOLC_MSG_ERROR) {
        if (frame_len >= VOLC_HDR_SIZE + 8) {
            *out_event = EVENT_DIALOG_ERROR;
            uint32_t err_code = read_be32(frame, 4);
            uint32_t err_len = read_be32(frame, 8);
            syslog(LOG_ERR, "[%s] server error: code=%u\n", TAG, err_code);
            if (err_len > 0 && VOLC_HDR_SIZE + 8 + err_len <= frame_len) {
                *out_payload = (unsigned char *)(frame + VOLC_HDR_SIZE + 8);
                *out_plen = err_len;
            }
            session_id[0] = '\0';
            return 0;
        }
        return -EINVAL;
    }

    if (msg_type != VOLC_MSG_FULL_RESP && msg_type != VOLC_MSG_AUDIO_RESP)
        return -EINVAL;

    *out_event = read_be32(frame, 4);

    if (msg_type == VOLC_MSG_AUDIO_RESP)
        *out_event = EVENT_TTS_RESPONSE;

    size_t off = 8;
    if (off + 4 > frame_len) return -EINVAL;
    uint32_t sid_len = read_be32(frame, off);
    off += 4;

    if (sid_len > 0) {
        if (off + sid_len > frame_len) return -EINVAL;
        if (sid_len >= sid_cap) sid_len = sid_cap - 1;
        memcpy(session_id, frame + off, sid_len);
        session_id[sid_len] = '\0';
        off += sid_len;
    } else {
        session_id[0] = '\0';
    }

    if (off + 4 > frame_len) {
        *out_payload = NULL;
        *out_plen = 0;
        return 0;
    }
    uint32_t plen = read_be32(frame, off);
    off += 4;

    if (off + plen > frame_len) return -EINVAL;
    *out_payload = (unsigned char *)(frame + off);
    *out_plen = plen;
    return 0;
}

/* ========== 音频采集 ========== */

static void capture_release_frame(struct ap_buffer_s *apb);

static int capture_init(void)
{
    int fd = open(CAPTURE_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        syslog(LOG_ERR, "[%s] open(%s): %d\n", TAG, CAPTURE_DEVICE, errno);
        return -errno;
    }

    if (ioctl(fd, AUDIOIOC_RESERVE, 0) < 0) {
        syslog(LOG_ERR, "[%s] RESERVE: %d\n", TAG, errno);
        close(fd); return -errno;
    }

    struct audio_caps_desc_s cap_desc;
    memset(&cap_desc, 0, sizeof(cap_desc));
    cap_desc.caps.ac_len = sizeof(cap_desc.caps);
    cap_desc.caps.ac_type = AUDIO_TYPE_INPUT;
    cap_desc.caps.ac_channels = VOLC_CAPTURE_CHANNELS;
    cap_desc.caps.ac_chmap = 0;
    cap_desc.caps.ac_controls.hw[0] = VOLC_CAPTURE_RATE;
    cap_desc.caps.ac_controls.b[3] = (uint8_t)(VOLC_CAPTURE_RATE >> 16);
    cap_desc.caps.ac_controls.b[2] = VOLC_CAPTURE_BITS;
    cap_desc.caps.ac_subtype = AUDIO_FMT_PCM;

    if (ioctl(fd, AUDIOIOC_CONFIGURE, (uintptr_t)&cap_desc) < 0) {
        syslog(LOG_ERR, "[%s] CONFIGURE: %d\n", TAG, errno);
        ioctl(fd, AUDIOIOC_RELEASE, 0); close(fd); return -errno;
    }

    struct ap_buffer_info_s buf_info;
    memset(&buf_info, 0, sizeof(buf_info));
    if (ioctl(fd, AUDIOIOC_GETBUFFERINFO, (uintptr_t)&buf_info) != OK) {
        buf_info.nbuffers = 4; buf_info.buffer_size = 4096;
    }
    int nbufs = buf_info.nbuffers;
    if (nbufs < 1) nbufs = 4;

    struct ap_buffer_s **buffers = calloc((size_t)nbufs, sizeof(void *));
    if (!buffers) { ioctl(fd, AUDIOIOC_RELEASE, 0); close(fd); return -ENOMEM; }

    struct audio_buf_desc_s buf_desc;
    for (int i = 0; i < nbufs; i++) {
        memset(&buf_desc, 0, sizeof(buf_desc));
        buf_desc.numbytes = buf_info.buffer_size;
        buf_desc.u.pbuffer = &buffers[i];
        if (ioctl(fd, AUDIOIOC_ALLOCBUFFER, (uintptr_t)&buf_desc) < 0) {
            while (--i >= 0) ioctl(fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&buffers[i]);
            free(buffers); ioctl(fd, AUDIOIOC_RELEASE, 0); close(fd);
            return -ENOMEM;
        }
    }

    struct mq_attr attr;
    attr.mq_maxmsg = nbufs + 8;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_curmsgs = 0;
    attr.mq_flags = 0;

    snprintf(s_mq_name, sizeof(s_mq_name), "/tmp/e2e_cap%p", (void *)&s_cap_fd);
    mq_unlink(s_mq_name);
    s_cap_mq = mq_open(s_mq_name, O_RDWR | O_CREAT, 0644, &attr);
    if (s_cap_mq == (mqd_t)-1) {
        for (int i = 0; i < nbufs; i++) ioctl(fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&buffers[i]);
        free(buffers); ioctl(fd, AUDIOIOC_RELEASE, 0); close(fd);
        return -errno;
    }
    ioctl(fd, AUDIOIOC_REGISTERMQ, (uintptr_t)s_cap_mq);

    for (int i = 0; i < nbufs; i++) {
        memset(&buf_desc, 0, sizeof(buf_desc));
        buf_desc.u.buffer = buffers[i];
        ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&buf_desc);
    }

    free(buffers);
    s_cap_fd = fd;
    syslog(LOG_INFO, "[%s] capture ready\n", TAG);
    return 0;
}

static int capture_start(void) { return ioctl(s_cap_fd, AUDIOIOC_START, 0); }
static int capture_stop(void) { return ioctl(s_cap_fd, AUDIOIOC_STOP, 0); }

static int capture_read_frame(struct ap_buffer_s **out_apb,
    const uint8_t **out_pcm, size_t *out_len)
{
    struct audio_msg_s msg;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }

    ssize_t size = mq_timedreceive(s_cap_mq, (char *)&msg, sizeof(msg), NULL, &ts);
    if (size != sizeof(msg)) {
        if (errno == ETIMEDOUT) return -ETIMEDOUT;
        return -EIO;
    }

    if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr) {
        struct ap_buffer_s *apb = (struct ap_buffer_s *)msg.u.ptr;
        if (apb->samp == NULL || apb->nbytes == 0) {
            capture_release_frame(apb);
            return -EAGAIN;
        }
        *out_apb = apb;
        *out_pcm = apb->samp;
        *out_len = apb->nbytes;
        return 0;
    }
    return -EIO;
}

static void capture_release_frame(struct ap_buffer_s *apb)
{
    if (apb == NULL || s_cap_fd < 0) return;
    struct audio_buf_desc_s buf_desc;
    memset(&buf_desc, 0, sizeof(buf_desc));
    buf_desc.u.buffer = apb;
    ioctl(s_cap_fd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&buf_desc);
}

static void capture_deinit(void)
{
    if (s_cap_fd >= 0) {
        ioctl(s_cap_fd, AUDIOIOC_STOP, 0);
        usleep(50000);
        ioctl(s_cap_fd, AUDIOIOC_RELEASE, 0);
        close(s_cap_fd);
        s_cap_fd = -1;
    }
    if (s_cap_mq != (mqd_t)-1) {
        mq_close(s_cap_mq);
        mq_unlink(s_mq_name);
        s_cap_mq = (mqd_t)-1;
    }
}

/* ========== 音频播放 (nxplayer + WAV) ========== */

typedef struct {
    char riff_id[4]; uint32_t riff_size; char wave_id[4];
    char fmt_id[4]; uint32_t fmt_size; uint16_t audio_format;
    uint16_t num_channels; uint32_t sample_rate; uint32_t byte_rate;
    uint16_t block_align; uint16_t bits_per_sample;
    char data_id[4]; uint32_t data_size;
} wav_header_t;

static void write_wav_header(wav_header_t *hdr, uint32_t pcm_size)
{
    memcpy(hdr->riff_id, "RIFF", 4);
    hdr->riff_size = 36 + pcm_size;
    memcpy(hdr->wave_id, "WAVE", 4);
    memcpy(hdr->fmt_id, "fmt ", 4);
    hdr->fmt_size = 16;
    hdr->audio_format = 1;
    hdr->num_channels = VOLC_I2S_CHANNELS;
    hdr->sample_rate = VOLC_PLAYBACK_RATE;
    hdr->byte_rate = VOLC_PLAYBACK_RATE * VOLC_I2S_CHANNELS * (VOLC_PLAYBACK_BITS / 8);
    hdr->block_align = VOLC_I2S_CHANNELS * (VOLC_PLAYBACK_BITS / 8);
    hdr->bits_per_sample = VOLC_PLAYBACK_BITS;
    memcpy(hdr->data_id, "data", 4);
    hdr->data_size = pcm_size;
}

static int playback_init(void)
{
    if (!s_pcm_buf) {
        s_pcm_buf = malloc(TTS_BUF_CAP);
        if (!s_pcm_buf) {
            syslog(LOG_ERR, "[%s] PCM buffer alloc failed\n", TAG);
            return -ENOMEM;
        }
    }
    s_pcm_len = 0;
    syslog(LOG_INFO, "[%s] playback initialized\n", TAG);
    return 0;
}

static void playback_write(const unsigned char *data, size_t len)
{
    if (s_pcm_buf && s_playing) {
        size_t num_samples = len / 2;
        size_t out_len = num_samples * 4;
        if (s_pcm_len + out_len <= TTS_BUF_CAP) {
            const int16_t *mono = (const int16_t *)data;
            int16_t *stereo = (int16_t *)(s_pcm_buf + s_pcm_len);
            for (size_t i = 0; i < num_samples; i++) {
                stereo[i * VOLC_I2S_CHANNELS] = mono[i];
                stereo[i * VOLC_I2S_CHANNELS + 1] = mono[i];
            }
            s_pcm_len += out_len;
        }
    }
}

/* 创建播放器并把系统音量同步进去：nxplayer_playraw() 内部会把音量
 * 重置为硬编码默认值，直接冲掉语音/UI 设置的音量——agent 侧
 * (voice_channel.c)每次播放前同样先做此同步。 */
static struct nxplayer_s *create_player_with_volume(void)
{
    struct nxplayer_s *pl = nxplayer_create();
    if (!pl)
        return NULL;

    nxplayer_setdevice(pl, "/dev/audio/pcm0");

    int sys_vol = watch_volume_get_value();
    syslog(LOG_INFO, "[%s] TTS volume sync: sys_vol=%d\n", TAG, sys_vol);
    if (sys_vol >= 0)
        nxplayer_setvolume(pl, (uint16_t)sys_vol);
    else
        syslog(LOG_WARNING, "[%s] TTS volume sync FAILED, "
            "using nxplayer default\n", TAG);

    return pl;
}

static void playback_stop(void)
{
    if (s_pcm_buf && s_pcm_len > 0) {
        uint32_t byte_rate = VOLC_PLAYBACK_RATE * VOLC_I2S_CHANNELS * (VOLC_PLAYBACK_BITS / 8);
        long duration_ms = (long)(s_pcm_len * 1000 / byte_rate) + 500;

        const char *wav_path = "/tmp/e2e_tts.wav";
        int fd = open(wav_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            wav_header_t hdr;
            write_wav_header(&hdr, (uint32_t)s_pcm_len);
            write(fd, &hdr, sizeof(hdr));
            write(fd, s_pcm_buf, s_pcm_len);
            close(fd);
            sync();
            syslog(LOG_INFO, "[%s] WAV: %zu bytes (~%ldms)\n", TAG, s_pcm_len, duration_ms);

            capture_stop();
            usleep(50000);

            if (s_nxplayer) {
                nxplayer_stop(s_nxplayer);
                nxplayer_release(s_nxplayer);
                s_nxplayer = NULL;
            }
            s_nxplayer = create_player_with_volume();
            if (s_nxplayer) {
                int play_ret = nxplayer_playraw(s_nxplayer, wav_path,
                    AUDIO_FMT_PCM, 0, VOLC_I2S_CHANNELS, VOLC_PLAYBACK_BITS,
                    VOLC_PLAYBACK_RATE, 0);
                if (play_ret < 0) {
                    syslog(LOG_ERR, "[%s] playraw failed: %d\n", TAG, play_ret);
                    nxplayer_release(s_nxplayer);
                    s_nxplayer = NULL;
                }
            }

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long cooldown_ms = duration_ms + 1000;
            s_cooldown_until.tv_sec = now.tv_sec + cooldown_ms / 1000;
            s_cooldown_until.tv_nsec = now.tv_nsec + (cooldown_ms % 1000) * 1000000L;
            if (s_cooldown_until.tv_nsec >= 1000000000L) {
                s_cooldown_until.tv_sec++;
                s_cooldown_until.tv_nsec -= 1000000000L;
            }
            s_cooldown_active = true;
        }
    }

    if (s_pcm_buf) { free(s_pcm_buf); s_pcm_buf = NULL; }
    s_pcm_len = 0;
}

/* ========== 本地音频播放（提示音） ========== */

void volc_play_local_wav(const char *path)
{
    if (!path || !path[0]) return;

    syslog(LOG_INFO, "[%s] playing local wav: %s\n", TAG, path);

    /* 停止采集，避免音频冲突 */
    capture_stop();
    usleep(50000);

    /* 释放已有播放器 */
    if (s_nxplayer) {
        nxplayer_stop(s_nxplayer);
        nxplayer_release(s_nxplayer);
        s_nxplayer = NULL;
    }

    /* 创建播放器播放SD卡WAV */
    s_nxplayer = create_player_with_volume();
    if (s_nxplayer) {
        int ret = nxplayer_playraw(s_nxplayer, path,
            AUDIO_FMT_PCM, 0, VOLC_I2S_CHANNELS, VOLC_PLAYBACK_BITS,
            VOLC_PLAYBACK_RATE, 0);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] play local wav failed: %d (path=%s)\n",
                TAG, ret, path);
            nxplayer_release(s_nxplayer);
            s_nxplayer = NULL;
            /* 即使播放失败也要恢复采集 */
        }
    }

    /* 设置冷却时间 (~4秒，足够播放提示音) */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    s_cooldown_until.tv_sec  = now.tv_sec + 4;
    s_cooldown_until.tv_nsec = now.tv_nsec;
    s_cooldown_active = true;
    s_playing = false;
}

/* ========== 接收线程 ========== */

static void *recv_thread(void *arg)
{
    (void)arg;
    unsigned char *buf = malloc(WS_BUF_SIZE);
    if (!buf) {
        syslog(LOG_ERR, "[%s] recv: alloc failed\n", TAG);
        return NULL;
    }

    while (s_running) {
        size_t frame_len = 0;
        int opcode = 0;
        int ret = ws_recv_frame(s_tls, buf, WS_BUF_SIZE, &frame_len, &opcode);

        if (ret != 0) {
            if (s_running) syslog(LOG_ERR, "[%s] recv error: %d\n", TAG, ret);
            break;
        }

        if (opcode == WS_OPCODE_CLOSE) {
            syslog(LOG_INFO, "[%s] server close\n", TAG);
            break;
        }
        if (opcode == WS_OPCODE_PING) {
            pthread_mutex_lock(&s_tls_mutex);
            ws_send_pong(s_tls, buf, frame_len);
            pthread_mutex_unlock(&s_tls_mutex);
            continue;
        }
        if (opcode != WS_OPCODE_BINARY) continue;

        uint32_t event_id = 0;
        char session_id[VOLC_SESSION_ID_LEN + 1] = {0};
        unsigned char *payload = NULL;
        size_t plen = 0;

        ret = parse_server_frame(buf, frame_len, &event_id,
            session_id, sizeof(session_id), &payload, &plen);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] parse error: %d\n", TAG, ret);
            continue;
        }

        switch (event_id) {
        case EVENT_CONNECTION_STARTED:
            syslog(LOG_INFO, "[%s] ConnectionStarted\n", TAG);
            break;

        case EVENT_CONNECTION_FAILED:
            syslog(LOG_ERR, "[%s] ConnectionFailed\n", TAG);
            s_running = false;
            notify_ui(VOLC_CB_ERROR_MSG, "连接失败");
            break;

        case EVENT_CONNECTION_FINISHED:
            syslog(LOG_INFO, "[%s] ConnectionFinished\n", TAG);
            s_running = false;
            break;

        case EVENT_SESSION_STARTED: {
            syslog(LOG_INFO, "[%s] SessionStarted\n", TAG);
            s_session_active = true;
            if (plen > 0) {
                char *json = strndup((const char *)payload, plen);
                if (json) {
                    cJSON *root = cJSON_Parse(json);
                    if (root) {
                        cJSON *did = cJSON_GetObjectItem(root, "dialog_id");
                        if (cJSON_IsString(did))
                            strncpy(s_dialog_id, did->valuestring, sizeof(s_dialog_id) - 1);
                        cJSON_Delete(root);
                    }
                    free(json);
                }
            }
            notify_ui(VOLC_CB_CONNECTED, NULL);
            notify_state(VOLC_UI_CONNECTED);
            break;
        }

        case EVENT_SESSION_FINISHED:
            syslog(LOG_INFO, "[%s] SessionFinished\n", TAG);
            s_session_active = false;
            break;

        case EVENT_SESSION_FAILED:
            syslog(LOG_ERR, "[%s] SessionFailed\n", TAG);
            s_session_active = false;
            notify_ui(VOLC_CB_ERROR_MSG, "会话失败");
            break;

        case EVENT_ASR_INFO:
            syslog(LOG_INFO, "[%s] ASRInfo (user speaking)\n", TAG);
            s_asr_buf[0] = '\0';  /* 新轮次，清空累积文本 */
            s_suppress_tts = false;  /* 新轮次，重置抑制标志 */
            if (s_playing && !s_cooldown_active) {
                playback_stop();
                playback_init();
            }
            notify_state(VOLC_UI_LISTENING);
            break;

        case EVENT_ASR_RESPONSE:
            if (plen > 0) {
                char *json = strndup((const char *)payload, plen);
                if (json) {
                    cJSON *root = cJSON_Parse(json);
                    if (root) {
                        cJSON *results = cJSON_GetObjectItem(root, "results");
                        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
                            cJSON *first = cJSON_GetArrayItem(results, 0);
                            cJSON *text = cJSON_GetObjectItem(first, "text");
                            cJSON *is_interim = cJSON_GetObjectItem(first, "is_interim");
                            if (cJSON_IsString(text) && text->valuestring[0]) {
                                const char *t = text->valuestring;
                                bool interim = cJSON_IsTrue(is_interim);

                                if (!interim) {
                                    bool skip_ai = false;

                                    /* 仅打印最终识别结果到syslog */
                                    syslog(LOG_INFO, "[%s] ASR: \"%s\"\n", TAG, t);

                                    /* 智能家居指令检测（优先于UI模式切换，
                                     * 避免"打开客厅灯"中的"打开"误触发策略2） */
                                    int hc_ret = home_control_voice_execute(t);
                                    if (hc_ret == HOME_CTRL_NET_FAIL) {
                                        /* 匹配到设备但网络失败 */
                                        syslog(LOG_INFO,
                                            "[%s] home ctrl net fail: \"%s\"\n",
                                            TAG, t);
                                        volc_play_local_wav(HOME_CTRL_FAIL_WAV);
                                        skip_ai = true;
                                    } else if (hc_ret == HOME_CTRL_NO_MATCH) {
                                        /* 有开关动作词但未匹配到设备 */
                                        syslog(LOG_INFO,
                                            "[%s] home ctrl no match: \"%s\"\n",
                                            TAG, t);
                                        volc_play_local_wav(HOME_CTRL_NOT_FOUND_WAV);
                                        skip_ai = true;
                                    }
                                    /* HOME_CTRL_OK: 指令成功，正常AI对话
                                     * HOME_CTRL_NONE: 无关指令，正常AI对话 */

                                    if (!skip_ai) {
                                        /* 累积最终文本到静态缓冲区 */
                                        strncat(s_asr_buf, t,
                                            sizeof(s_asr_buf) - strlen(s_asr_buf) - 1);

                                        /* 多策略关键词检测:
                                         * 策略1: 完整短语匹配(含发音变体) */
                                        int matched = 0;
                                        if (strstr_any(s_asr_buf, VAR_CHAOWAN) ||
                                            strstr_any(s_asr_buf, VAR_BIAOQING)) {
                                            matched = 1;
                                        }

                                        /* 策略2: 动作词 + 目标词变体 组合匹配
                                         * (更鲁棒，容忍中间杂字和发音变体) */
                                        if (!matched) {
                                            int has_action = 0, has_target = 0;
                                            if (strstr(s_asr_buf, "切换") ||
                                                strstr(s_asr_buf, "打开") ||
                                                strstr(s_asr_buf, "进入") ||
                                                strstr(s_asr_buf, "回到") ||
                                                strstr(s_asr_buf, "返回"))
                                                has_action = 1;
                                            if (strstr_any(s_asr_buf, VAR_CHAOWAN) ||
                                                strstr_any(s_asr_buf, VAR_BIAOQING) ||
                                                strstr_any(s_asr_buf, VAR_BIOPAN))
                                                has_target = 1;
                                            if (has_action && has_target)
                                                matched = 1;
                                        }

                                        if (matched) {
                                            syslog(LOG_INFO,
                                                "[%s] UI mode switch triggered: buf=\"%s\"\n",
                                                TAG, s_asr_buf);
                                            FILE *fp = fopen("/mnt/spif/ui_mode.json", "w");
                                            if (fp) {
                                                fprintf(fp, "{\"ui_mode\":0}\n");
                                                fclose(fp);
                                            }
                                            axp2101_power_reset();
                                            /* 不会到达这里 */
                                        }
                                        notify_ui(VOLC_CB_USER_TEXT, t);
                                    }
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                    free(json);
                }
            }
            break;

        case EVENT_ASR_ENDED:
            syslog(LOG_INFO, "[%s] ASREnded\n", TAG);
            s_asr_buf[0] = '\0';  /* 本轮结束，清空累积文本 */
            s_playing = !s_suppress_tts;  /* 本地命令时不进入播放状态 */
            if (s_suppress_tts)
                notify_state(VOLC_UI_CONNECTED);  /* 跳过 THINKING/SPEAKING */
            else
                notify_state(VOLC_UI_THINKING);
            break;

        case EVENT_CHAT_RESPONSE:
            if (plen > 0) {
                char *json = strndup((const char *)payload, plen);
                if (json) {
                    cJSON *root = cJSON_Parse(json);
                    if (root) {
                        cJSON *content = cJSON_GetObjectItem(root, "content");
                        if (cJSON_IsString(content))
                            notify_ui(VOLC_CB_AI_TEXT, content->valuestring);
                        cJSON_Delete(root);
                    }
                    free(json);
                }
            }
            break;

        case EVENT_CHAT_ENDED:
            syslog(LOG_INFO, "[%s] ChatEnded\n", TAG);
            break;

        case EVENT_TTS_SENTENCE_START:
            syslog(LOG_INFO, "[%s] TTSSentenceStart%s\n", TAG,
                   s_suppress_tts ? " [SUPPRESSED]" : "");
            if (!s_suppress_tts) {
                s_playing = true;
                notify_state(VOLC_UI_SPEAKING);
            }
            break;

        case EVENT_TTS_SENTENCE_END:
            syslog(LOG_INFO, "[%s] TTSSentenceEnd%s\n", TAG,
                   s_suppress_tts ? " [SUPPRESSED]" : "");
            if (!s_suppress_tts && s_pcm_len > 0) {
                playback_stop();
                playback_init();
            }
            break;

        case EVENT_TTS_RESPONSE:
            if (plen > 0 && s_playing && !s_suppress_tts)
                playback_write(payload, plen);
            break;

        case EVENT_TTS_ENDED:
            syslog(LOG_INFO, "[%s] TTSEnded%s\n", TAG,
                   s_suppress_tts ? " [SUPPRESSED]" : "");
            if (!s_suppress_tts && s_pcm_len > 0) {
                playback_stop();
                playback_init();
            }
            s_suppress_tts = false;  /* 重置抑制标志 */
            if (s_cooldown_active) {
                s_playing = true;
            } else {
                s_playing = false;
                notify_state(VOLC_UI_CONNECTED);
            }
            break;

        case EVENT_DIALOG_ERROR:
            if (plen > 0) {
                char *json = strndup((const char *)payload, plen);
                if (json) {
                    cJSON *root = cJSON_Parse(json);
                    if (root) {
                        cJSON *msg = cJSON_GetObjectItem(root, "message");
                        if (cJSON_IsString(msg))
                            notify_ui(VOLC_CB_ERROR_MSG, msg->valuestring);
                        cJSON_Delete(root);
                    }
                    free(json);
                }
            }
            break;

        default:
            syslog(LOG_DEBUG, "[%s] Unknown event: %u\n", TAG, event_id);
            break;
        }
    }

    free(buf);
    session_dead("recv closed");
    syslog(LOG_INFO, "[%s] recv thread exit\n", TAG);
    return NULL;
}

/* ========== 发送线程 (采集+发送) ========== */

static void *send_thread(void *arg)
{
    (void)arg;

    if (capture_init() != 0) {
        syslog(LOG_ERR, "[%s] capture init failed\n", TAG);
        return NULL;
    }
    capture_start();

    syslog(LOG_INFO, "[%s] send thread started\n", TAG);

    int frame_count = 0;
    int cap_retry = 0;

    size_t silence_len = (VOLC_CAPTURE_RATE / 50) * (VOLC_CAPTURE_BITS / 8);
    uint8_t *silence_buf = calloc(1, silence_len);
    if (!silence_buf) return NULL;

    while (s_running && s_session_active) {
        if (s_playing && s_cooldown_active) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > s_cooldown_until.tv_sec ||
                (now.tv_sec == s_cooldown_until.tv_sec &&
                 now.tv_nsec >= s_cooldown_until.tv_nsec)) {
                s_playing = false;
                s_cooldown_active = false;
                syslog(LOG_INFO, "[%s] cooldown ended\n", TAG);
                notify_state(VOLC_UI_CONNECTED);
                if (s_nxplayer) {
                    nxplayer_stop(s_nxplayer);
                    usleep(100000);
                    nxplayer_release(s_nxplayer);
                    s_nxplayer = NULL;
                }
                capture_deinit();
                usleep(200000);
                if (capture_init() == 0) capture_start();
            } else {
                send_audio_frame(s_tls, s_session_id, silence_buf, silence_len);
                usleep(100000);
                continue;
            }
        }

        if (s_playing || s_cooldown_active) {
            send_audio_frame(s_tls, s_session_id, silence_buf, silence_len);
            usleep(100000);
            continue;
        }

        struct ap_buffer_s *apb = NULL;
        const uint8_t *pcm = NULL;
        size_t pcm_len = 0;

        int ret = capture_read_frame(&apb, &pcm, &pcm_len);

        if (ret == -ETIMEDOUT || ret == -EAGAIN) {
            send_audio_frame(s_tls, s_session_id, silence_buf, silence_len);
            continue;
        }

        if (ret != 0) {
            if (s_playing || s_cooldown_active) {
                send_audio_frame(s_tls, s_session_id, silence_buf, silence_len);
                usleep(100000);
                continue;
            }
            if (ret == -EIO && cap_retry < 2) {
                cap_retry++;
                if (s_nxplayer) nxplayer_stop(s_nxplayer);
                capture_deinit();
                usleep(300000);
                if (capture_init() == 0) {
                    capture_start();
                    continue;
                }
            }
            break;
        }

        if (pcm_len > 0) {
            size_t mono_len = pcm_len / 2;
            if (mono_len > silence_len) {
                uint8_t *new_buf = calloc(1, mono_len);
                if (!new_buf) { capture_release_frame(apb); break; }
                free(silence_buf);
                silence_buf = new_buf;
                silence_len = mono_len;
            }

            if (!s_playing) {
                const int16_t *stereo = (const int16_t *)pcm;
                int16_t *mono16 = (int16_t *)silence_buf;
                size_t samples = mono_len / 2;
                for (size_t i = 0; i < samples; i++)
                    mono16[i] = stereo[i * 2];

                ret = send_audio_frame(s_tls, s_session_id, silence_buf, mono_len);
                frame_count++;
                if (frame_count % 100 == 1) {
                    int16_t peak = 0;
                    for (size_t i = 0; i < samples; i++) {
                        int16_t s = mono16[i];
                        if (s < 0) s = -s;
                        if (s > peak) peak = s;
                    }
                    syslog(LOG_INFO, "[%s] audio: %d frames, peak=%d\n", TAG, frame_count, peak);
                }
                if (ret != 0) {
                    syslog(LOG_ERR, "[%s] send audio error: %d\n", TAG, ret);
                    capture_release_frame(apb);
                    break;
                }
            } else {
                send_audio_frame(s_tls, s_session_id, silence_buf, silence_len);
            }
            capture_release_frame(apb);
        } else {
            usleep(10000);
            if (s_running && s_session_active)
                send_audio_frame(s_tls, s_session_id, silence_buf, silence_len);
        }
    }

    free(silence_buf);
    capture_stop();
    capture_deinit();
    session_dead("send closed");
    syslog(LOG_INFO, "[%s] send thread exit\n", TAG);
    return NULL;
}

/* ========== 公开API ========== */

int volc_voice_init(void)
{
    volc_load_config();
    generate_uuid(s_session_id, sizeof(s_session_id));
    syslog(LOG_INFO, "[%s] session_id=%s\n", TAG, s_session_id);
    return 0;
}

int volc_voice_start(volc_event_cb_t cb)
{
    /* 上一次会话若异常死亡(session_dead 只清标志)，TLS ctx、
     * socket、播放器等资源未释放，这里补清理。先关 fd 唤醒仍
     * 阻塞在 ssl_read/ssl_write 上的线程，保证 join 快速返回。 */
    if (s_tls)
        {
        syslog(LOG_WARNING, "[%s] cleaning stale session resources\n", TAG);
        if (s_tls->net.fd >= 0)
            {
            close(s_tls->net.fd);
            s_tls->net.fd = -1;
            }
        if (s_recv_tid)
            {
            pthread_join(s_recv_tid, NULL);
            s_recv_tid = 0;
            }
        if (s_send_tid)
            {
            pthread_join(s_send_tid, NULL);
            s_send_tid = 0;
            }
        /* 清空 s_pcm_len，避免 playback_stop 把半截 TTS 播出去 */
        s_pcm_len = 0;
        playback_stop();
        if (s_nxplayer)
            {
            nxplayer_release(s_nxplayer);
            s_nxplayer = NULL;
            }
        capture_deinit();
        tls_free(s_tls);
        free(s_tls);
        s_tls = NULL;
        }

    s_callback = cb;
    s_running = true;
    s_session_active = false;
    s_interrupted = false;
    s_playing = false;
    s_cooldown_active = false;

    notify_state(VOLC_UI_CONNECTING);

    /* 堆分配TLS上下文(PSRAM), 节省DRAM */
    s_tls = calloc(1, sizeof(tls_ctx_t));
    if (!s_tls) {
        syslog(LOG_ERR, "[%s] TLS ctx alloc failed\n", TAG);
        notify_ui(VOLC_CB_ERROR_MSG, "内存不足");
        s_running = false;
        return -ENOMEM;
    }

    int ret = tls_connect(s_tls, WS_HOST, WS_PORT);
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] TLS connect failed: %d", ret);
        notify_ui(VOLC_CB_ERROR_MSG, "TLS连接失败");
        tls_free(s_tls); free(s_tls); s_tls = NULL;
        s_running = false;
        return ret;
    }

    syslog(LOG_INFO, "[volc_e2e] TLS OK, starting WS handshake");
    ret = ws_handshake(s_tls, WS_HOST, WS_PATH);
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] WS handshake failed: %d", ret);
        tls_free(s_tls); free(s_tls); s_tls = NULL;
        notify_ui(VOLC_CB_ERROR_MSG, "WebSocket握手失败");
        s_running = false;
        return ret;
    }

    syslog(LOG_INFO, "[volc_e2e] WS OK, init playback");
    ret = playback_init();
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] playback_init failed: %d", ret);
        tls_free(s_tls); free(s_tls); s_tls = NULL;
        notify_ui(VOLC_CB_ERROR_MSG, "播放初始化失败");
        s_running = false;
        return ret;
    }

    syslog(LOG_INFO, "[volc_e2e] Sending StartConnection");
    ret = send_start_connection(s_tls);
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] StartConnection failed: %d", ret);
        tls_free(s_tls); free(s_tls); s_tls = NULL;
        notify_ui(VOLC_CB_ERROR_MSG, "StartConnection失败");
        s_running = false;
        return ret;
    }

    syslog(LOG_INFO, "[volc_e2e] Starting recv thread");
    pthread_create(&s_recv_tid, NULL, recv_thread, NULL);

    syslog(LOG_INFO, "[volc_e2e] Waiting for ConnectionStarted...");
    for (int i = 0; i < 50 && s_running; i++) usleep(100000);
    if (!s_running) {
        syslog(LOG_ERR, "[volc_e2e] ConnectionStarted timeout");
        goto fail;
    }
    syslog(LOG_INFO, "[volc_e2e] ConnectionStarted OK");

    syslog(LOG_INFO, "[volc_e2e] Sending StartSession");
    ret = send_start_session(s_tls, s_session_id, s_speaker);
    if (ret != 0) {
        syslog(LOG_ERR, "[volc_e2e] StartSession failed: %d", ret);
        goto fail;
    }

    syslog(LOG_INFO, "[volc_e2e] Waiting for SessionStarted...");
    for (int i = 0; i < 100 && s_running && !s_session_active; i++) usleep(100000);
    if (!s_running || !s_session_active) {
        syslog(LOG_ERR, "[volc_e2e] SessionStarted timeout");
        goto fail;
    }

    pthread_create(&s_send_tid, NULL, send_thread, NULL);

    syslog(LOG_INFO, "[%s] Voice session active\n", TAG);
    return 0;

fail:
    s_running = false;
    if (s_recv_tid) { pthread_join(s_recv_tid, NULL); s_recv_tid = 0; }
    playback_stop();
    if (s_nxplayer) { nxplayer_release(s_nxplayer); s_nxplayer = NULL; }
    capture_deinit();
    if (s_tls) {
        tls_free(s_tls);
        free(s_tls);
        s_tls = NULL;
    }
    notify_ui(VOLC_CB_ERROR_MSG, "会话启动失败");
    return -EIO;
}

void volc_voice_stop(void)
{
    bool was_running = s_running;

    /* 未启动，或异常死亡后资源已由 volc_voice_start 清理 */
    if (!was_running && !s_tls)
        return;

    s_running = false;
    s_session_active = false;

    if (s_recv_tid) { pthread_join(s_recv_tid, NULL); s_recv_tid = 0; }
    if (s_send_tid) { pthread_join(s_send_tid, NULL); s_send_tid = 0; }

    /* 仅会话还活着时礼貌收尾：异常死亡后 socket 已被服务端关闭，
     * 发 FinishSession/FinishConnection 只会打一堆 ssl_write 错误 */
    if (was_running && s_tls && s_tls->net.fd >= 0) {
        send_finish_session(s_tls, s_session_id);
        usleep(100000);
        send_finish_connection(s_tls);
        usleep(100000);
    }

    playback_stop();
    if (s_nxplayer) { nxplayer_release(s_nxplayer); s_nxplayer = NULL; }
    capture_deinit();
    if (s_tls) {
        tls_free(s_tls);
        free(s_tls);
        s_tls = NULL;
    }

    notify_ui(VOLC_CB_DISCONNECTED, NULL);
    notify_state(VOLC_UI_IDLE);

    syslog(LOG_INFO, "[%s] Voice session stopped\n", TAG);
}

bool volc_voice_is_active(void)
{
    return s_running && s_session_active;
}

void volc_voice_interrupt(void)
{
    if (s_playing) {
        playback_stop();
        playback_init();
        s_playing = false;
        s_cooldown_active = false;
        if (s_nxplayer) {
            nxplayer_stop(s_nxplayer);
            usleep(100000);
            nxplayer_release(s_nxplayer);
            s_nxplayer = NULL;
        }
        capture_deinit();
        usleep(200000);
        if (capture_init() == 0) capture_start();
        notify_state(VOLC_UI_CONNECTED);
        syslog(LOG_INFO, "[%s] playback interrupted\n", TAG);
    }
}

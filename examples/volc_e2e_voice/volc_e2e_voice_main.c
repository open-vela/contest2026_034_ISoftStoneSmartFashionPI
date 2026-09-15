/*
 * Volcengine End-to-End Realtime Voice Client
 *
 * Uses the Volcengine Realtime API (S2S model) for voice-to-voice dialogue.
 * Single WebSocket connection handles ASR + LLM + TTS simultaneously.
 *
 * Protocol: wss://openspeech.bytedance.com/api/v3/realtime/dialogue
 *
 * Flow:
 *   1. TLS connect + WebSocket upgrade
 *   2. StartConnection → ConnectionStarted
 *   3. StartSession → SessionStarted
 *   4. Loop: TaskRequest (audio) → ASRResponse + TTSResponse → play audio
 *   5. FinishSession → FinishConnection
 */

#include <errno.h>
#include <fcntl.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mqueue.h>
#include <nuttx/audio/audio.h>
#include <system/nxplayer.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

static const char* TAG = "volc_e2e";

/* ── Configuration ────────────────────────────────────────────── */

#define WS_HOST "openspeech.bytedance.com"
#define WS_PORT "443"
/* Try different API paths */
#define WS_PATH "/api/v3/realtime/dialogue"
/* Alternative paths if the above doesn't work:
 * #define WS_PATH "/v3/realtime/dialogue"
 * #define WS_PATH "/api/v2/realtime/dialogue"
 */

/* Hardcoded credentials for testing */
#define VOLC_APP_ID   "5467075766"
#define VOLC_TOKEN    "QJcPmS0GkGfNn_eaJLWqwmCPxnoS56c6"

#define APP_ID_KEY   "volc_appkey"
#define TOKEN_KEY    "volc_token"
#define API_KEY_KEY  "volc_api_key"
#define SPEAKER_KEY  "volc_speaker"
#define CLUSTER_KEY  "volc_cluster"

#define DEFAULT_SPEAKER "zh_female_vv_jupiter_bigtts"
#define DEFAULT_CLUSTER "volc.speech.dialog"
#define DEFAULT_MODEL   "1.2.1.1"  /* O2.0 version */

/* Audio parameters */
#define CAPTURE_DEVICE   "/dev/audio/pcm_in0"
#define CAPTURE_RATE     16000
#define CAPTURE_BITS     16
#define CAPTURE_CHANNELS 2  /* I2S hardware requires stereo */
#define CAPTURE_CHUNK_MS 20
#define CAPTURE_CHUNK_SIZE (CAPTURE_RATE * CAPTURE_CHANNELS * (CAPTURE_BITS / 8) * CAPTURE_CHUNK_MS / 1000)

#define PLAYBACK_RATE    24000
#define PLAYBACK_BITS    16
#define PLAYBACK_CHANNELS 1  /* server sends mono */
#define I2S_CHANNELS     2  /* I2S hardware requires stereo output */

/* Volcengine binary protocol */
#define VOLC_PROTO_VER  0x11  /* version=1, header_size=1 (x4=4B) */
#define VOLC_HDR_SIZE   4     /* Protocol header size in bytes */
#define VOLC_FLAG_EVENT 0x04  /* Flags bit: event ID field present */

/* Message types (upper 4 bits of byte 1) */
#define VOLC_MSG_FULL_REQ    0x10  /* Full-client request */
#define VOLC_MSG_AUDIO_REQ   0x20  /* Audio-only request */
#define VOLC_MSG_FULL_RESP   0x90  /* Full-server response */
#define VOLC_MSG_AUDIO_RESP  0xB0  /* Audio-only response */
#define VOLC_MSG_ERROR       0xF0  /* Error */

/* Serialization methods */
#define VOLC_SER_RAW  0x00  /* Raw binary (audio) */
#define VOLC_SER_JSON 0x10  /* JSON */

/* Client event IDs */
#define EVENT_START_CONNECTION  1
#define EVENT_FINISH_CONNECTION 2
#define EVENT_START_SESSION     100
#define EVENT_FINISH_SESSION    102
#define EVENT_TASK_REQUEST      200
#define EVENT_END_ASR           400

/* Server event IDs */
#define EVENT_CONNECTION_STARTED 50
#define EVENT_CONNECTION_FAILED  51
#define EVENT_CONNECTION_FINISHED 52
#define EVENT_SESSION_STARTED    150
#define EVENT_SESSION_FINISHED   152
#define EVENT_SESSION_FAILED     153
#define EVENT_TTS_SENTENCE_START 350
#define EVENT_TTS_SENTENCE_END   351
#define EVENT_TTS_RESPONSE       352
#define EVENT_TTS_ENDED          359
#define EVENT_ASR_INFO           450
#define EVENT_ASR_RESPONSE       451
#define EVENT_ASR_ENDED          459
#define EVENT_CHAT_RESPONSE      550
#define EVENT_CHAT_ENDED         559
#define EVENT_DIALOG_ERROR       599

/* WebSocket constants */
#define WS_FIN_BIT   0x80
#define WS_MASK_BIT  0x80
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE  0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A
#define WS_MASK_KEY_LEN  4

/* Session ID length (UUID format) */
#define SESSION_ID_LEN 36

/* Buffer sizes */
#define WS_BUF_SIZE    (64 * 1024)
#define AUDIO_BUF_SIZE (32 * 1024)

/* ── TLS context ──────────────────────────────────────────────── */

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} tls_ctx_t;

/* ── Global state ─────────────────────────────────────────────── */

static tls_ctx_t s_tls;
static pthread_mutex_t s_tls_mutex = PTHREAD_MUTEX_INITIALIZER;  /* TLS thread safety */
static char s_session_id[SESSION_ID_LEN + 1];
static char s_dialog_id[128];
static volatile bool s_running;
static volatile bool s_interrupted;   /* set by SIGINT handler */
static volatile bool s_session_active;
static pthread_t s_recv_tid;
static pthread_t s_send_tid;

/* Audio playback (nxplayer + WAV) */
static struct nxplayer_s* s_nxplayer;
static volatile bool s_playing;
static struct timespec s_cooldown_until;  /* absolute time when cooldown expires */
static bool s_cooldown_active;             /* true if waiting for playback to finish */
#define TTS_BUF_CAP (2 * 1024 * 1024)  /* 2MB buffer for PCM data */
static unsigned char* s_pcm_buf;
static size_t s_pcm_len;

/* Audio capture */
static int s_cap_fd;
static mqd_t s_cap_mq;
static char s_mq_name[32];  /* MQ name for unlink on cleanup */

/* Credentials */
static char s_app_id[64];
static char s_token[128];
static char s_speaker[64];

/* ── Entropy source ────────────────────────────────────────────── */

static int entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        fd = open("/dev/random", O_RDONLY);
    }
    if (fd < 0) {
        return -1;
    }
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, output + total, len - total);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        total += (size_t)n;
    }
    close(fd);
    return 0;
}

/* ── UUID generation ───────────────────────────────────────────── */

static void generate_uuid(char* out, size_t cap)
{
    unsigned char rnd[16];
    entropy_func(NULL, rnd, sizeof(rnd));
    rnd[6] = (rnd[6] & 0x0F) | 0x40; /* version 4 */
    rnd[8] = (rnd[8] & 0x3F) | 0x80; /* variant 1 */
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x-%02x%02x%02x%02x%02x%02x",
        rnd[0], rnd[1], rnd[2], rnd[3],
        rnd[4], rnd[5], rnd[6], rnd[7],
        rnd[8], rnd[9], rnd[10], rnd[11],
        rnd[12], rnd[13], rnd[14], rnd[15]);
}

/* ── TLS connection ────────────────────────────────────────────── */

static int tls_connect(tls_ctx_t* ctx, const char* host, const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    const char* pers = "volc_e2e";
    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, entropy_func,
        NULL, (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ctr_drbg_seed: -0x%04x\n", TAG, -ret);
        return -EIO;
    }

    ret = mbedtls_net_connect(&ctx->net, host, port,
        MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] net_connect %s:%s: -0x%04x\n",
            TAG, host, port, -ret);
        return -ECONNREFUSED;
    }

    mbedtls_net_set_block(&ctx->net);

    /* Short timeout for handshake */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ret = mbedtls_ssl_config_defaults(&ctx->cfg,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        return -EIO;
    }

    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

#if defined(MBEDTLS_SSL_ALPN)
    static const char* alpn[] = { "http/1.1", NULL };
    mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, alpn);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) {
        return -EIO;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, host);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
        mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ
            || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake stalled\n", TAG);
            return -ETIMEDOUT;
        }
        syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
        return -EIO;
    }

    /* Switch to non-blocking: prevents recv thread from holding mutex
     * during blocking reads, which would starve the send thread. */
    mbedtls_net_set_nonblock(&ctx->net);

    syslog(LOG_INFO, "[%s] TLS connected to %s:%s\n", TAG, host, port);
    return 0;
}

static void tls_free(tls_ctx_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

/* ── TLS I/O helpers ───────────────────────────────────────────── */

static int tls_write_all(tls_ctx_t* ctx, const unsigned char* buf, size_t len)
{
    if (ctx == NULL || ctx->net.fd < 0) {
        return -EINVAL;
    }

    pthread_mutex_lock(&s_tls_mutex);
    size_t written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);
        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            pthread_mutex_unlock(&s_tls_mutex);
            usleep(1000);  /* 1ms: yield to other thread */
            pthread_mutex_lock(&s_tls_mutex);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            /* TLS renegotiation needs to read; yield and retry */
            pthread_mutex_unlock(&s_tls_mutex);
            usleep(1000);
            pthread_mutex_lock(&s_tls_mutex);
        } else if (ret == MBEDTLS_ERR_NET_CONN_RESET) {
            syslog(LOG_ERR, "[%s] connection reset\n", TAG);
            pthread_mutex_unlock(&s_tls_mutex);
            return -ECONNRESET;
        } else {
            syslog(LOG_ERR, "[%s] ssl_write error: -0x%04x\n", TAG, -ret);
            pthread_mutex_unlock(&s_tls_mutex);
            return -EIO;
        }
    }
    pthread_mutex_unlock(&s_tls_mutex);
    return 0;
}

static int tls_read_all(tls_ctx_t* ctx, unsigned char* buf, size_t len)
{
    if (ctx == NULL || ctx->net.fd < 0) {
        return -EINVAL;
    }

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
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            /* Non-blocking: no data available. Release mutex, sleep, retry.
             * This gives the send thread a chance to transmit audio. */
            pthread_mutex_unlock(&s_tls_mutex);
            usleep(10000);  /* 10ms */
            pthread_mutex_lock(&s_tls_mutex);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* TLS renegotiation needs to write; yield and retry */
            pthread_mutex_unlock(&s_tls_mutex);
            usleep(1000);
            pthread_mutex_lock(&s_tls_mutex);
        } else if (ret == MBEDTLS_ERR_NET_CONN_RESET) {
            syslog(LOG_ERR, "[%s] connection reset\n", TAG);
            pthread_mutex_unlock(&s_tls_mutex);
            return -ECONNRESET;
        } else {
            syslog(LOG_ERR, "[%s] ssl_read error: -0x%04x\n", TAG, -ret);
            pthread_mutex_unlock(&s_tls_mutex);
            return -EIO;
        }
    }
    pthread_mutex_unlock(&s_tls_mutex);
    return 0;
}

/* ── WebSocket handshake ──────────────────────────────────────── */

static int ws_handshake(tls_ctx_t* ctx, const char* host, const char* path,
    const char* app_id, const char* token)
{
    /* Generate WebSocket key */
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;
    entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len,
        key_raw, sizeof(key_raw));

    /* Build HTTP upgrade request with auth headers */
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
        "X-Api-Resource-Id: volc.speech.dialog\r\n"
        "X-Api-App-Key: PlgvMymc7f3tQnJ6\r\n"
        "\r\n",
        path, host, (int)key_b64_len, key_b64,
        app_id, token);

    if (n <= 0 || n >= (int)sizeof(req)) {
        return -EOVERFLOW;
    }

    syslog(LOG_INFO, "[%s] WS handshake: %s\n", TAG, path);

    int ret = tls_write_all(ctx, (const unsigned char*)req, (size_t)n);
    if (ret != 0) {
        return ret;
    }

    /* Read HTTP 101 response */
    char resp[2048];
    size_t rlen = 0;
    while (rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)resp + rlen,
            sizeof(resp) - 1 - rlen);
        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) {
                break;
            }
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            return -EIO;
        }
    }

    int status = 0;
    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);
        syslog(LOG_ERR, "[%s] Response: %s\n", TAG, resp);
        return -EPROTO;
    }

    /* Extract X-Tt-Logid for debugging */
    char* logid = strstr(resp, "X-Tt-Logid:");
    if (logid) {
        logid += 12;
        while (*logid == ' ') logid++;
        char* end = strstr(logid, "\r\n");
        if (end) *end = '\0';
        syslog(LOG_INFO, "[%s] WS upgrade OK, logid=%s\n", TAG, logid);
    } else {
        syslog(LOG_INFO, "[%s] WS upgrade OK\n", TAG);
    }

    return 0;
}

/* ── WebSocket frame send/recv ─────────────────────────────────── */

static int ws_send_binary(tls_ctx_t* ctx,
    const unsigned char* payload, size_t plen)
{
    if (ctx == NULL || ctx->net.fd < 0) {
        return -EINVAL;
    }

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
    if (ret != 0) {
        return ret;
    }

    /* Mask and send payload */
    unsigned char chunk[1024];
    size_t sent = 0;
    while (sent < plen) {
        size_t clen = plen - sent;
        if (clen > sizeof(chunk)) clen = sizeof(chunk);
        for (size_t i = 0; i < clen; i++) {
            chunk[i] = payload[sent + i] ^ mask[(sent + i) % 4];
        }
        ret = tls_write_all(ctx, chunk, clen);
        if (ret != 0) return ret;
        sent += clen;
    }
    return 0;
}

/* Send WebSocket PONG frame (reply to server PING for keepalive).
 * Client-to-server frames MUST be masked per RFC 6455. */
static int ws_send_pong(tls_ctx_t* ctx,
    const unsigned char* payload, size_t plen)
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

    /* Mask and send payload */
    for (size_t i = 0; i < plen; i++) {
        unsigned char c = payload[i] ^ mask[i % 4];
        ret = tls_write_all(ctx, &c, 1);
        if (ret != 0) return ret;
    }
    return 0;
}

static int ws_recv_frame(tls_ctx_t* ctx,
    unsigned char* buf, size_t cap,
    size_t* out_len, int* out_opcode)
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
        syslog(LOG_ERR, "[%s] WS frame too large: %zu > %zu\n",
            TAG, plen, cap);
        return -EOVERFLOW;
    }

    if (plen > 0) {
        ret = tls_read_all(ctx, buf, plen);
        if (ret != 0) return ret;
        if (masked) {
            for (size_t i = 0; i < plen; i++) {
                buf[i] ^= mask[i % 4];
            }
        }
    }

    *out_len = plen;
    return 0;
}

/* ── Volcengine binary frame builders ──────────────────────────── */

static uint32_t read_be32(const unsigned char* buf, size_t off)
{
    return ((uint32_t)buf[off] << 24)
        | ((uint32_t)buf[off + 1] << 16)
        | ((uint32_t)buf[off + 2] << 8)
        | (uint32_t)buf[off + 3];
}

static void write_be32(unsigned char* buf, size_t off, uint32_t val)
{
    buf[off]     = (unsigned char)((val >> 24) & 0xFF);
    buf[off + 1] = (unsigned char)((val >> 16) & 0xFF);
    buf[off + 2] = (unsigned char)((val >> 8) & 0xFF);
    buf[off + 3] = (unsigned char)(val & 0xFF);
}

/* Build and send a client request frame.
 *
 * Documentation examples (decimal byte arrays):
 *   StartConnection: [17 20 16 0 0 0 0 1 0 0 0 2 123 125]
 *     = [0x11 0x14 0x10 0x00] [Event=1] [PaySize=2] "{}"
 *     → Header + Event + PayloadSize + Payload (NO SID for Connect events)
 *
 *   StartSession: [17 20 16 0 0 0 0 100 0 0 0 36 ...]
 *     = [0x11 0x14 0x10 0x00] [Event=100] [SIDSize=36] [SID] [PaySize] [Pay]
 *     → Header + Event + SIDSize + SID + PayloadSize + Payload
 *
 * byte 1 = Message Type (0b0001=Full-client) << 4 | Flags (0b0100=has-event) = 0x14
 */
static int send_client_frame(tls_ctx_t* ctx,
    uint32_t event_id,
    const char* session_id,
    const unsigned char* payload,
    size_t plen)
{
    if (ctx == NULL || ctx->net.fd < 0) {
        return -EINVAL;
    }

    size_t sid_len = session_id ? strlen(session_id) : 0;
    bool is_connect_event = (event_id == EVENT_START_CONNECTION
                             || event_id == EVENT_FINISH_CONNECTION);

    /* Frame layout:
     * Connect: [Header 4B] [Event 4B] [PaySize 4B] [Payload]
     * Session: [Header 4B] [Event 4B] [SIDSize 4B] [SID] [PaySize 4B] [Payload]
     */
    size_t frame_len;
    if (is_connect_event) {
        frame_len = VOLC_HDR_SIZE + 4 + 4 + plen;
    } else {
        frame_len = VOLC_HDR_SIZE + 4 + 4 + sid_len + 4 + plen;
    }

    unsigned char* frame = malloc(frame_len);
    if (!frame) return -ENOMEM;

    size_t off = 0;

    /* Header: protocol v1, Full-client request + has-event flag, JSON */
    frame[off++] = VOLC_PROTO_VER;
    frame[off++] = VOLC_MSG_FULL_REQ | VOLC_FLAG_EVENT;  /* 0x14 */
    frame[off++] = VOLC_SER_JSON;
    frame[off++] = 0x00;

    /* Event ID */
    write_be32(frame, off, event_id);
    off += 4;

    /* Session ID (only for Session-level events, NOT Connect events) */
    if (!is_connect_event) {
        write_be32(frame, off, (uint32_t)sid_len);
        off += 4;
        if (sid_len > 0) {
            memcpy(frame + off, session_id, sid_len);
            off += sid_len;
        }
    }

    /* Payload size + payload */
    write_be32(frame, off, (uint32_t)plen);
    off += 4;
    if (plen > 0) {
        memcpy(frame + off, payload, plen);
    }

    /* Log frame for debugging */
    syslog(LOG_DEBUG, "[%s] send frame: event=%u, len=%zu\n",
        TAG, event_id, frame_len);
    if (frame_len >= 8) {
        syslog(LOG_DEBUG, "[%s] frame[0-7]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            TAG, frame[0], frame[1], frame[2], frame[3],
            frame[4], frame[5], frame[6], frame[7]);
    }

    int ret = ws_send_binary(ctx, frame, frame_len);
    free(frame);
    return ret;
}

/* Send audio data frame (TaskRequest event = 200).
 *
 * Frame format:
 *   [Header 4B] [Event 4B] [Session ID Size 4B] [Session ID]
 *   [Payload Size 4B] [Payload]
 *
 * byte 1 = Audio-only request (0b0010) | has-event (0b0100) = 0x24
 */
static int send_audio_frame(tls_ctx_t* ctx,
    const char* session_id,
    const unsigned char* audio,
    size_t alen)
{
    if (ctx == NULL || ctx->net.fd < 0 || session_id == NULL) {
        return -EINVAL;
    }

    size_t sid_len = strlen(session_id);
    if (sid_len == 0) {
        return -EINVAL;
    }

    size_t frame_len = VOLC_HDR_SIZE + 4 + 4 + sid_len + 4 + alen;
    unsigned char* frame = malloc(frame_len);
    if (!frame) return -ENOMEM;

    size_t off = 0;

    /* Header: protocol v1, Audio-only request + has-event, Raw */
    frame[off++] = VOLC_PROTO_VER;
    frame[off++] = VOLC_MSG_AUDIO_REQ | VOLC_FLAG_EVENT;  /* 0x24 */
    frame[off++] = VOLC_SER_RAW;
    frame[off++] = 0x00;

    /* Event ID (200 = TaskRequest) */
    write_be32(frame, off, EVENT_TASK_REQUEST);
    off += 4;

    /* Session ID */
    write_be32(frame, off, (uint32_t)sid_len);
    off += 4;
    memcpy(frame + off, session_id, sid_len);
    off += sid_len;

    /* Payload size + payload */
    write_be32(frame, off, (uint32_t)alen);
    off += 4;
    memcpy(frame + off, audio, alen);

    int ret = ws_send_binary(ctx, frame, frame_len);
    free(frame);
    return ret;
}

/* ── Send event helpers ────────────────────────────────────────── */

static int send_start_connection(tls_ctx_t* ctx)
{
    syslog(LOG_INFO, "[%s] Sending StartConnection\n", TAG);
    return send_client_frame(ctx, EVENT_START_CONNECTION, NULL,
        (const unsigned char*)"{}", 2);
}

static int send_start_session(tls_ctx_t* ctx, const char* session_id,
    const char* speaker, const char* system_role)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return -ENOMEM;

    /* dialog config */
    cJSON* dialog = cJSON_AddObjectToObject(root, "dialog");
    cJSON_AddStringToObject(dialog, "bot_name", "豆包");
    if (system_role && system_role[0]) {
        cJSON_AddStringToObject(dialog, "system_role", system_role);
    }
    cJSON_AddStringToObject(dialog, "speaking_style", "你说话简洁明了，语气温和。");
    cJSON_AddStringToObject(dialog, "dialog_id", "");

    cJSON* extra = cJSON_AddObjectToObject(dialog, "extra");
    cJSON_AddStringToObject(extra, "model", DEFAULT_MODEL);

    /* tts config */
    cJSON* tts = cJSON_AddObjectToObject(root, "tts");
    cJSON_AddStringToObject(tts, "speaker", speaker);
    cJSON* tts_audio = cJSON_AddObjectToObject(tts, "audio_config");
    cJSON_AddNumberToObject(tts_audio, "channel", PLAYBACK_CHANNELS);
    cJSON_AddStringToObject(tts_audio, "format", "pcm_s16le");  /* 16-bit PCM */
    cJSON_AddNumberToObject(tts_audio, "sample_rate", PLAYBACK_RATE);

    /* asr config */
    cJSON* asr = cJSON_AddObjectToObject(root, "asr");
    cJSON* asr_audio = cJSON_AddObjectToObject(asr, "audio_info");
    cJSON_AddStringToObject(asr_audio, "format", "pcm");
    cJSON_AddNumberToObject(asr_audio, "sample_rate", CAPTURE_RATE);
    cJSON_AddNumberToObject(asr_audio, "channel", 1);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -ENOMEM;

    syslog(LOG_INFO, "[%s] Sending StartSession (speaker=%s)\n",
        TAG, speaker);
    int ret = send_client_frame(ctx, EVENT_START_SESSION, session_id,
        (const unsigned char*)json, strlen(json));
    free(json);
    return ret;
}

static int send_finish_session(tls_ctx_t* ctx, const char* session_id)
{
    syslog(LOG_INFO, "[%s] Sending FinishSession\n", TAG);
    return send_client_frame(ctx, EVENT_FINISH_SESSION, session_id,
        (const unsigned char*)"{}", 2);
}

static int send_finish_connection(tls_ctx_t* ctx)
{
    syslog(LOG_INFO, "[%s] Sending FinishConnection\n", TAG);
    return send_client_frame(ctx, EVENT_FINISH_CONNECTION, NULL,
        (const unsigned char*)"{}", 2);
}

static int send_end_asr(tls_ctx_t* ctx, const char* session_id)
{
    syslog(LOG_INFO, "[%s] Sending EndASR\n", TAG);
    return send_client_frame(ctx, EVENT_END_ASR, session_id,
        (const unsigned char*)"{}", 2);
}

/* ── Server frame parsing ──────────────────────────────────────── */

/* Parse a server response frame.
 *
 * Server response format (full-server response, 0x90):
 *   [Header 4B] [Event ID 4B] [Session ID Size 4B] [Session ID]
 *   [Payload Size 4B] [Payload]
 *
 * Server response format (audio-only response, 0xB0):
 *   [Header 4B] [Event ID 4B] [Session ID Size 4B] [Session ID]
 *   [Payload Size 4B] [Payload]
 *
 * Error response format (0xF0):
 *   [Header 4B] [Error Code 4B] [Payload Size 4B] [Payload]
 */
static int parse_server_frame(const unsigned char* frame, size_t frame_len,
    uint32_t* out_event, char* session_id, size_t sid_cap,
    unsigned char** out_payload, size_t* out_plen)
{
    if (frame_len < VOLC_HDR_SIZE) {
        syslog(LOG_ERR, "[%s] frame too short: %zu (need %d)\n",
            TAG, frame_len, VOLC_HDR_SIZE);
        return -EINVAL;
    }

    uint8_t msg_type = frame[1] & 0xF0;
    uint8_t flags = frame[1] & 0x0F;
    (void)flags;

    /* Handle error response */
    if (msg_type == VOLC_MSG_ERROR) {
        if (frame_len >= VOLC_HDR_SIZE + 8) {
            *out_event = EVENT_DIALOG_ERROR;
            uint32_t err_code = read_be32(frame, 4);
            uint32_t err_len = read_be32(frame, 8);
            syslog(LOG_ERR, "[%s] server error: code=%u, msg_len=%u\n",
                TAG, err_code, err_len);
            if (err_len > 0 && VOLC_HDR_SIZE + 8 + err_len <= frame_len) {
                *out_payload = (unsigned char*)(frame + VOLC_HDR_SIZE + 8);
                *out_plen = err_len;
                /* Try to print as string */
                char* err_msg = strndup((const char*)*out_payload, *out_plen);
                if (err_msg) {
                    syslog(LOG_ERR, "[%s] error msg: %s\n", TAG, err_msg);
                    free(err_msg);
                }
            } else {
                *out_payload = NULL;
                *out_plen = 0;
            }
            session_id[0] = '\0';
            return 0;
        }
        syslog(LOG_ERR, "[%s] error frame too short: %zu\n", TAG, frame_len);
        return -EINVAL;
    }

    /* For full-server response (0x90) and audio-only response (0xB0),
     * the structure is: [Header] [Event/Seq] [SessionID] [Payload] */
    if (msg_type != VOLC_MSG_FULL_RESP && msg_type != VOLC_MSG_AUDIO_RESP) {
        syslog(LOG_ERR, "[%s] unknown msg_type: 0x%02x (expected 0x90 or 0xB0)\n",
            TAG, msg_type);
        return -EINVAL;
    }

    /* Bytes 4-7: event_id for full-server, sequence for audio-only */
    *out_event = read_be32(frame, 4);

    /* For audio-only responses, assume TTSResponse */
    if (msg_type == VOLC_MSG_AUDIO_RESP) {
        *out_event = EVENT_TTS_RESPONSE;
    }

    /* Session ID */
    size_t off = 8;
    if (off + 4 > frame_len) {
        syslog(LOG_ERR, "[%s] no session_id_len at offset %zu\n", TAG, off);
        return -EINVAL;
    }
    uint32_t sid_len = read_be32(frame, off);
    off += 4;

    if (sid_len > 0) {
        if (off + sid_len > frame_len) {
            syslog(LOG_ERR, "[%s] session_id truncated: %u > %zu\n",
                TAG, sid_len, frame_len - off);
            return -EINVAL;
        }
        if (sid_len >= sid_cap) sid_len = sid_cap - 1;
        memcpy(session_id, frame + off, sid_len);
        session_id[sid_len] = '\0';
        off += sid_len;
    } else {
        session_id[0] = '\0';
    }

    /* Payload */
    if (off + 4 > frame_len) {
        /* No payload, that's OK */
        *out_payload = NULL;
        *out_plen = 0;
        return 0;
    }
    uint32_t plen = read_be32(frame, off);
    off += 4;

    if (off + plen > frame_len) {
        syslog(LOG_ERR, "[%s] payload truncated: %u > %zu\n",
            TAG, plen, frame_len - off);
        return -EINVAL;
    }
    *out_payload = (unsigned char*)(frame + off);
    *out_plen = plen;

    return 0;
}

/* ── Audio capture ─────────────────────────────────────────────── */

/* Forward declarations */
static void capture_release_frame(struct ap_buffer_s* apb);

static int capture_init(void)
{
    int fd = open(CAPTURE_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        syslog(LOG_ERR, "[%s] open(%s) failed: %d\n",
            TAG, CAPTURE_DEVICE, errno);
        return -errno;
    }

    /* RESERVE */
    if (ioctl(fd, AUDIOIOC_RESERVE, 0) < 0) {
        syslog(LOG_ERR, "[%s] RESERVE failed: %d\n", TAG, errno);
        close(fd);
        return -errno;
    }

    /* CONFIGURE */
    struct audio_caps_desc_s cap_desc;
    memset(&cap_desc, 0, sizeof(cap_desc));
    cap_desc.caps.ac_len      = sizeof(cap_desc.caps);
    cap_desc.caps.ac_type     = AUDIO_TYPE_INPUT;
    cap_desc.caps.ac_channels = CAPTURE_CHANNELS;
    cap_desc.caps.ac_chmap    = 0;
    cap_desc.caps.ac_controls.hw[0] = CAPTURE_RATE;
    cap_desc.caps.ac_controls.b[3]  = (uint8_t)(CAPTURE_RATE >> 16);
    cap_desc.caps.ac_controls.b[2]  = CAPTURE_BITS;
    cap_desc.caps.ac_subtype        = AUDIO_FMT_PCM;

    if (ioctl(fd, AUDIOIOC_CONFIGURE, (uintptr_t)&cap_desc) < 0) {
        syslog(LOG_ERR, "[%s] CONFIGURE failed: %d\n", TAG, errno);
        ioctl(fd, AUDIOIOC_RELEASE, 0);
        close(fd);
        return -errno;
    }

    /* GETBUFFERINFO */
    struct ap_buffer_info_s buf_info;
    memset(&buf_info, 0, sizeof(buf_info));
    if (ioctl(fd, AUDIOIOC_GETBUFFERINFO, (uintptr_t)&buf_info) != OK) {
        buf_info.nbuffers    = 4;
        buf_info.buffer_size = 4096;
    }
    int nbufs = buf_info.nbuffers;
    if (nbufs < 1) nbufs = 4;

    /* ALLOCBUFFER */
    struct ap_buffer_s** buffers = calloc((size_t)nbufs, sizeof(void*));
    if (!buffers) {
        ioctl(fd, AUDIOIOC_RELEASE, 0);
        close(fd);
        return -ENOMEM;
    }

    struct audio_buf_desc_s buf_desc;
    for (int i = 0; i < nbufs; i++) {
        memset(&buf_desc, 0, sizeof(buf_desc));
        buf_desc.numbytes  = buf_info.buffer_size;
        buf_desc.u.pbuffer = &buffers[i];
        if (ioctl(fd, AUDIOIOC_ALLOCBUFFER, (uintptr_t)&buf_desc) < 0) {
            syslog(LOG_ERR, "[%s] ALLOCBUFFER[%d] failed\n", TAG, i);
            while (--i >= 0) ioctl(fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&buffers[i]);
            free(buffers);
            ioctl(fd, AUDIOIOC_RELEASE, 0);
            close(fd);
            return -ENOMEM;
        }
    }

    /* Create MQ */
    struct mq_attr attr;
    attr.mq_maxmsg  = nbufs + 8;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_curmsgs = 0;
    attr.mq_flags   = 0;

    char mq_name[32];
    snprintf(mq_name, sizeof(mq_name), "/tmp/e2e_cap%p", (void*)&s_cap_fd);
    strncpy(s_mq_name, mq_name, sizeof(s_mq_name) - 1);
    /* Unlink any stale MQ from previous session to ensure fresh creation */
    mq_unlink(mq_name);
    mqd_t mq = mq_open(mq_name, O_RDWR | O_CREAT, 0644, &attr);
    if (mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] mq_open failed: %d\n", TAG, errno);
        for (int i = 0; i < nbufs; i++) ioctl(fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&buffers[i]);
        free(buffers);
        ioctl(fd, AUDIOIOC_RELEASE, 0);
        close(fd);
        return -errno;
    }
    ioctl(fd, AUDIOIOC_REGISTERMQ, (uintptr_t)mq);

    /* ENQUEUEBUFFER */
    for (int i = 0; i < nbufs; i++) {
        memset(&buf_desc, 0, sizeof(buf_desc));
        buf_desc.u.buffer = buffers[i];
        ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&buf_desc);
    }

    s_cap_fd = fd;
    s_cap_mq = mq;
    syslog(LOG_INFO, "[%s] capture ready: %d buffers\n", TAG, nbufs);
    return 0;
}

static int capture_start(void)
{
    return ioctl(s_cap_fd, AUDIOIOC_START, 0);
}

static int capture_stop(void)
{
    return ioctl(s_cap_fd, AUDIOIOC_STOP, 0);
}

static int capture_read_frame(struct ap_buffer_s** out_apb,
    const uint8_t** out_pcm, size_t* out_len)
{
    struct audio_msg_s msg;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000;  /* 100ms timeout - keep frames flowing */
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    ssize_t size = mq_timedreceive(s_cap_mq, (char*)&msg, sizeof(msg), NULL, &ts);
    if (size != sizeof(msg)) {
        if (errno == ETIMEDOUT) return -ETIMEDOUT;
        return -EIO;
    }

    if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr) {
        struct ap_buffer_s* apb = (struct ap_buffer_s*)msg.u.ptr;
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

static void capture_release_frame(struct ap_buffer_s* apb)
{
    if (apb == NULL || s_cap_fd < 0) {
        return;
    }
    struct audio_buf_desc_s buf_desc;
    memset(&buf_desc, 0, sizeof(buf_desc));
    buf_desc.u.buffer = apb;
    ioctl(s_cap_fd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&buf_desc);
}

static void capture_deinit(void)
{
    if (s_cap_fd >= 0) {
        ioctl(s_cap_fd, AUDIOIOC_STOP, 0);
        usleep(50000);  /* Wait for any pending buffers */
        ioctl(s_cap_fd, AUDIOIOC_RELEASE, 0);
        close(s_cap_fd);
        s_cap_fd = -1;
    }
    if (s_cap_mq != (mqd_t)-1) {
        mq_close(s_cap_mq);
        mq_unlink(s_mq_name);  /* Remove MQ from namespace */
        s_cap_mq = (mqd_t)-1;
    }
}

/* ── Audio playback (nxplayer + WAV) ────────────────────────────── */

/* WAV header structure */
typedef struct {
    char riff_id[4];       /* "RIFF" */
    uint32_t riff_size;
    char wave_id[4];       /* "WAVE" */
    char fmt_id[4];        /* "fmt " */
    uint32_t fmt_size;
    uint16_t audio_format; /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_id[4];       /* "data" */
    uint32_t data_size;
} wav_header_t;

static void write_wav_header(wav_header_t* hdr, uint32_t pcm_size)
{
    memcpy(hdr->riff_id, "RIFF", 4);
    hdr->riff_size = 36 + pcm_size;
    memcpy(hdr->wave_id, "WAVE", 4);
    memcpy(hdr->fmt_id, "fmt ", 4);
    hdr->fmt_size = 16;
    hdr->audio_format = 1;  /* PCM */
    hdr->num_channels = I2S_CHANNELS;  /* stereo for I2S */
    hdr->sample_rate = PLAYBACK_RATE;
    hdr->byte_rate = PLAYBACK_RATE * I2S_CHANNELS * (PLAYBACK_BITS / 8);
    hdr->block_align = I2S_CHANNELS * (PLAYBACK_BITS / 8);
    hdr->bits_per_sample = PLAYBACK_BITS;
    memcpy(hdr->data_id, "data", 4);
    hdr->data_size = pcm_size;
}

static int playback_init(void)
{
    /* Allocate PCM buffer (nxplayer is created per-playback in playback_stop) */
    if (!s_pcm_buf) {
        s_pcm_buf = malloc(TTS_BUF_CAP);
        if (!s_pcm_buf) {
            syslog(LOG_ERR, "[%s] PCM buffer alloc failed\n", TAG);
            return -ENOMEM;
        }
    }
    s_pcm_len = 0;

    syslog(LOG_INFO, "[%s] playback initialized (%uHz %uch(stereo) %ubit)\n",
        TAG, PLAYBACK_RATE, I2S_CHANNELS, PLAYBACK_BITS);
    return 0;
}

static void playback_write(const unsigned char* data, size_t len)
{
    if (s_pcm_buf && s_playing) {
        /* Server sends mono 16-bit PCM, convert to stereo for I2S playback */
        size_t num_samples = len / 2;  /* number of mono 16-bit samples */
        size_t out_len = num_samples * 4;  /* each mono sample → L+R = 4 bytes */

        if (s_pcm_len + out_len <= TTS_BUF_CAP) {
            const int16_t* mono = (const int16_t*)data;
            int16_t* stereo = (int16_t*)(s_pcm_buf + s_pcm_len);
            for (size_t i = 0; i < num_samples; i++) {
                stereo[i * I2S_CHANNELS] = mono[i];      /* Left channel */
                stereo[i * I2S_CHANNELS + 1] = mono[i];  /* Right channel */
            }
            s_pcm_len += out_len;
        } else {
            syslog(LOG_WARNING, "[%s] PCM buffer overflow\n", TAG);
        }
    }
}

static void playback_stop(void)
{
    if (s_pcm_buf && s_pcm_len > 0) {
        /* Calculate playback duration for cooldown */
        uint32_t byte_rate = PLAYBACK_RATE * I2S_CHANNELS * (PLAYBACK_BITS / 8);
        long duration_ms = (long)(s_pcm_len * 1000 / byte_rate) + 500; /* +500ms margin */

        /* Write WAV file */
        const char* wav_path = "/tmp/e2e_tts.wav";
        int fd = open(wav_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            wav_header_t hdr;
            write_wav_header(&hdr, (uint32_t)s_pcm_len);
            write(fd, &hdr, sizeof(hdr));
            write(fd, s_pcm_buf, s_pcm_len);
            close(fd);
            sync();
            syslog(LOG_INFO, "[%s] WAV: %zu bytes (~%ldms)\n",
                TAG, s_pcm_len + sizeof(hdr), duration_ms);

            /* Stop capture DMA to free I2S for playback */
            capture_stop();
            usleep(50000);  /* 50ms: let DMA stop and I2S settle */

            /* Create fresh nxplayer per playback (same pattern as ai_agent).
             * Reusing nxplayer across playbacks causes silent failures. */
            if (s_nxplayer) {
                nxplayer_stop(s_nxplayer);
                nxplayer_release(s_nxplayer);
                s_nxplayer = NULL;
            }
            s_nxplayer = nxplayer_create();
            if (!s_nxplayer) {
                syslog(LOG_ERR, "[%s] nxplayer_create failed\n", TAG);
            } else {
                nxplayer_setdevice(s_nxplayer, "/dev/audio/pcm0");

                syslog(LOG_INFO, "[%s] starting nxplayer playraw\n", TAG);
                int play_ret = nxplayer_playraw(s_nxplayer, wav_path,
                    AUDIO_FMT_PCM, 0, I2S_CHANNELS, PLAYBACK_BITS,
                    PLAYBACK_RATE, 0);
                syslog(LOG_INFO, "[%s] playraw returned: %d\n", TAG, play_ret);

                if (play_ret < 0) {
                    syslog(LOG_ERR, "[%s] playraw failed: %d\n", TAG, play_ret);
                    nxplayer_release(s_nxplayer);
                    s_nxplayer = NULL;
                }
            }

            /* Set cooldown: playback duration + 1s acoustic decay */
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

    if (s_pcm_buf) {
        free(s_pcm_buf);
        s_pcm_buf = NULL;
    }
    s_pcm_len = 0;
}

/* ── Receive thread ────────────────────────────────────────────── */

static void* recv_thread(void* arg)
{
    (void)arg;
    unsigned char* buf = malloc(WS_BUF_SIZE);
    if (!buf) {
        syslog(LOG_ERR, "[%s] recv: alloc failed\n", TAG);
        return NULL;
    }

    while (s_running) {
        size_t frame_len = 0;
        int opcode = 0;
        int ret = ws_recv_frame(&s_tls, buf, WS_BUF_SIZE,
            &frame_len, &opcode);

        if (ret != 0) {
            if (s_running) {
                syslog(LOG_ERR, "[%s] recv error: %d\n", TAG, ret);
            }
            break;
        }

        /* Handle control frames */
        if (opcode == WS_OPCODE_CLOSE) {
            syslog(LOG_INFO, "[%s] server close\n", TAG);
            break;
        }
        if (opcode == WS_OPCODE_PING) {
            /* Reply PONG with same payload to maintain keepalive */
            syslog(LOG_INFO, "[%s] PING received (%zu bytes), sending PONG\n",
                TAG, frame_len);
            pthread_mutex_lock(&s_tls_mutex);
            ws_send_pong(&s_tls, buf, frame_len);
            pthread_mutex_unlock(&s_tls_mutex);
            continue;
        }
        if (opcode != WS_OPCODE_BINARY) {
            continue;
        }

        /* Parse Volcengine frame */
        uint32_t event_id = 0;
        char session_id[SESSION_ID_LEN + 1] = {0};
        unsigned char* payload = NULL;
        size_t plen = 0;

        ret = parse_server_frame(buf, frame_len,
            &event_id, session_id, sizeof(session_id),
            &payload, &plen);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] parse error: %d (len=%zu)\n", TAG, ret, frame_len);
            continue;
        }

        /* Handle events */
        switch (event_id) {
        case EVENT_CONNECTION_STARTED:
            syslog(LOG_INFO, "[%s] ConnectionStarted\n", TAG);
            break;

        case EVENT_CONNECTION_FAILED:
            syslog(LOG_ERR, "[%s] ConnectionFailed\n", TAG);
            s_running = false;
            break;

        case EVENT_CONNECTION_FINISHED:
            syslog(LOG_INFO, "[%s] ConnectionFinished\n", TAG);
            s_running = false;
            break;

        case EVENT_SESSION_STARTED: {
            syslog(LOG_INFO, "[%s] SessionStarted\n", TAG);
            s_session_active = true;
            /* Parse dialog_id from payload */
            if (plen > 0) {
                char* json = strndup((const char*)payload, plen);
                if (json) {
                    cJSON* root = cJSON_Parse(json);
                    if (root) {
                        cJSON* did = cJSON_GetObjectItem(root, "dialog_id");
                        if (cJSON_IsString(did)) {
                            strncpy(s_dialog_id, did->valuestring,
                                sizeof(s_dialog_id) - 1);
                            syslog(LOG_INFO, "[%s] dialog_id=%s\n",
                                TAG, s_dialog_id);
                        }
                        cJSON_Delete(root);
                    }
                    free(json);
                }
            }
            break;
        }

        case EVENT_SESSION_FINISHED:
            syslog(LOG_INFO, "[%s] SessionFinished\n", TAG);
            s_session_active = false;
            break;

        case EVENT_SESSION_FAILED:
            syslog(LOG_ERR, "[%s] SessionFailed\n", TAG);
            s_session_active = false;
            break;

        case EVENT_ASR_INFO:
            syslog(LOG_INFO, "[%s] ASRInfo (user started speaking)\n", TAG);
            /* If cooldown active, this is TTS echo - ignore it */
            if (s_playing && !s_cooldown_active) {
                syslog(LOG_INFO, "[%s] interrupting playback\n", TAG);
                playback_stop();
                playback_init();
            }
            break;

        case EVENT_ASR_RESPONSE:
            if (plen > 0) {
                char* json = strndup((const char*)payload, plen);
                if (json) {
                    cJSON* root = cJSON_Parse(json);
                    if (root) {
                        cJSON* results = cJSON_GetObjectItem(root, "results");
                        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
                            cJSON* first = cJSON_GetArrayItem(results, 0);
                            cJSON* text = cJSON_GetObjectItem(first, "text");
                            cJSON* is_interim = cJSON_GetObjectItem(first, "is_interim");
                            if (cJSON_IsString(text)) {
                                syslog(LOG_INFO, "[%s] ASR%s: %s\n",
                                    TAG,
                                    cJSON_IsTrue(is_interim) ? "(interim)" : "",
                                    text->valuestring);
                            }
                        }
                        cJSON_Delete(root);
                    }
                    free(json);
                }
            }
            break;

        case EVENT_ASR_ENDED:
            syslog(LOG_INFO, "[%s] ASREnded (user stopped speaking)\n", TAG);
            /* Stop sending audio immediately. Server detected user stopped;
             * sending more audio may confuse server during TTS generation. */
            s_playing = true;
            break;

        case 154:  /* UsageResponse */
            if (plen > 0) {
                char* json = strndup((const char*)payload, plen);
                if (json) {
                    syslog(LOG_INFO, "[%s] Usage: %s\n", TAG, json);
                    free(json);
                }
            }
            break;

        case EVENT_CHAT_RESPONSE:
            if (plen > 0) {
                char* json = strndup((const char*)payload, plen);
                if (json) {
                    cJSON* root = cJSON_Parse(json);
                    if (root) {
                        cJSON* content = cJSON_GetObjectItem(root, "content");
                        if (cJSON_IsString(content)) {
                            syslog(LOG_INFO, "[%s] Chat: %s\n",
                                TAG, content->valuestring);
                        }
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
            syslog(LOG_INFO, "[%s] TTSSentenceStart\n", TAG);
            s_playing = true;  /* TTS audio is coming, pause audio send */
            break;

        case EVENT_TTS_SENTENCE_END:
            syslog(LOG_INFO, "[%s] TTSSentenceEnd\n", TAG);
            if (s_pcm_len > 0) {
                playback_stop();    /* play accumulated TTS audio */
                playback_init();    /* reset buffer for next sentence */
                /* Keep s_playing=true to prevent echo during playback */
            }
            break;

        case EVENT_TTS_RESPONSE:
            if (plen > 0 && s_playing) {
                playback_write(payload, plen);
            }
            break;

        case EVENT_TTS_ENDED:
            syslog(LOG_INFO, "[%s] TTSEnded\n", TAG);
            if (s_pcm_len > 0) {
                playback_stop();    /* play any remaining TTS audio */
                playback_init();    /* reset for next response */
            }
            /* Keep s_playing=true during cooldown to prevent echo pickup */
            if (s_cooldown_active) {
                s_playing = true;
            } else {
                s_playing = false;
            }
            break;

        case EVENT_DIALOG_ERROR:
            if (plen > 0) {
                char* json = strndup((const char*)payload, plen);
                if (json) {
                    cJSON* root = cJSON_Parse(json);
                    if (root) {
                        cJSON* code = cJSON_GetObjectItem(root, "status_code");
                        cJSON* msg = cJSON_GetObjectItem(root, "message");
                        syslog(LOG_ERR, "[%s] Error: %s - %s\n",
                            TAG,
                            cJSON_IsString(code) ? code->valuestring : "?",
                            cJSON_IsString(msg) ? msg->valuestring : "?");
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
    syslog(LOG_INFO, "[%s] recv thread exiting\n", TAG);
    return NULL;
}

/* ── Send thread (audio capture + send) ────────────────────────── */

/* Simple VAD: detect silence to trigger EndASR */
#define VAD_SILENCE_THRESHOLD  300
#define VAD_SILENCE_FRAMES     15  /* 15 * 20ms = 300ms silence */
#define VAD_SPEECH_MIN_FRAMES  3   /* Minimum frames to confirm speech */

static void* send_thread(void* arg)
{
    (void)arg;

    /* Initialize audio capture */
    if (capture_init() != 0) {
        syslog(LOG_ERR, "[%s] capture init failed\n", TAG);
        return NULL;
    }
    capture_start();

    syslog(LOG_INFO, "[%s] send thread started (server VAD)\n", TAG);

    int frame_count = 0;
    int silence_count = 0;
    int cap_retry = 0;

    /* Silence buffer: initially small for cooldown, resized on first capture */
    size_t silence_len = (CAPTURE_RATE / 50) * (CAPTURE_BITS / 8);
    uint8_t* silence_buf = calloc(1, silence_len);
    if (!silence_buf) {
        syslog(LOG_ERR, "[%s] silence alloc failed\n", TAG);
        return NULL;
    }

    while (s_running && s_session_active) {

        /* Check cooldown: during TTS playback, send silence without blocking on capture */
        if (s_playing && s_cooldown_active) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > s_cooldown_until.tv_sec ||
                (now.tv_sec == s_cooldown_until.tv_sec &&
                 now.tv_nsec >= s_cooldown_until.tv_nsec)) {
                s_playing = false;
                s_cooldown_active = false;
                syslog(LOG_INFO, "[%s] cooldown ended, rebuilding capture\n", TAG);
                /* Stop and release nxplayer to free audio device.
                 * nxplayer_playraw is async; the playback thread may still
                 * hold the I2S device even after audio finishes. */
                if (s_nxplayer) {
                    nxplayer_stop(s_nxplayer);
                    usleep(100000);  /* 100ms: let playback thread exit */
                    nxplayer_release(s_nxplayer);
                    s_nxplayer = NULL;
                }
                /* Full close+open rebuild: NuttX AUDIOIOC_START is ignored
                 * when device is in DRAINING/OPEN state after STOP.
                 * Close+reopen resets to PREPARED, making START work. */
                capture_deinit();
                usleep(200000);  /* 200ms: let driver fully reset */
                if (capture_init() == 0) {
                    capture_start();
                }
            } else {
                /* Send silence at ~10fps (100ms per frame, matches capture rate) */
                int ret = send_audio_frame(&s_tls, s_session_id,
                    silence_buf, silence_len);
                silence_count++;
                if (ret != 0) {
                    syslog(LOG_ERR, "[%s] send silence error: %d\n", TAG, ret);
                    break;
                }
                usleep(100000);  /* 100ms pacing */
                continue;
            }
        }

        /* Quick state check before blocking on capture: if playback just
         * started, don't block for 1s in capture_read_frame. */
        if (s_playing || s_cooldown_active) {
            send_audio_frame(&s_tls, s_session_id,
                silence_buf, silence_len);
            silence_count++;
            usleep(100000);  /* 100ms pacing */
            continue;
        }

        /* Normal capture path */
        struct ap_buffer_s* apb = NULL;
        const uint8_t* pcm = NULL;
        size_t pcm_len = 0;

        int ret = capture_read_frame(&apb, &pcm, &pcm_len);

        /* On timeout or empty read, send silence to keep server alive */
        if (ret == -ETIMEDOUT || ret == -EAGAIN) {
            send_audio_frame(&s_tls, s_session_id,
                silence_buf, silence_len);
            silence_count++;
            continue;
        }

        if (ret != 0) {
            syslog(LOG_ERR, "[%s] capture read error: %d\n", TAG, ret);
            /* If cooldown is active, capture was intentionally closed by
             * recv_thread (playback_stop). Don't rebuild here - just send
             * silence and let cooldown-end code handle capture rebuild. */
            if (s_playing || s_cooldown_active) {
                send_audio_frame(&s_tls, s_session_id,
                    silence_buf, silence_len);
                silence_count++;
                usleep(100000);  /* 100ms pacing */
                continue;
            }
            /* Retry capture rebuild: nxplayer may need more time to
             * fully release the audio device after long playback. */
            if (ret == -EIO && cap_retry < 2) {
                cap_retry++;
                syslog(LOG_INFO, "[%s] capture rebuild retry %d\n", TAG, cap_retry);
                if (s_nxplayer) {
                    nxplayer_stop(s_nxplayer);
                }
                capture_deinit();
                usleep(300000);  /* 300ms extra wait */
                if (capture_init() == 0) {
                    capture_start();
                    send_audio_frame(&s_tls, s_session_id,
                        silence_buf, silence_len);
                    silence_count++;
                    continue;
                }
            }
            break;
        }

        if (ret == 0 && pcm_len > 0) {
            /* Resize silence buffer if capture frame is larger */
            size_t mono_len = pcm_len / 2;
            if (mono_len > silence_len) {
                uint8_t* new_buf = calloc(1, mono_len);
                if (!new_buf) {
                    capture_release_frame(apb);
                    break;
                }
                free(silence_buf);
                silence_buf = new_buf;
                silence_len = mono_len;
            }

            if (!s_playing) {
                /* Convert stereo to mono (take left channel) */
                const int16_t* stereo = (const int16_t*)pcm;
                int16_t* mono16 = (int16_t*)silence_buf;
                size_t samples = mono_len / 2;
                for (size_t i = 0; i < samples; i++) {
                    mono16[i] = stereo[i * 2];
                }

                ret = send_audio_frame(&s_tls, s_session_id,
                    silence_buf, mono_len);
                frame_count++;
                if (frame_count % 100 == 1) {
                    int16_t peak = 0;
                    for (size_t i = 0; i < samples; i++) {
                        int16_t s = mono16[i];
                        if (s < 0) s = -s;
                        if (s > peak) peak = s;
                    }
                    syslog(LOG_INFO, "[%s] audio: %d frames, peak=%d\n",
                        TAG, frame_count, peak);
                }
                if (ret != 0) {
                    syslog(LOG_ERR, "[%s] send audio error: %d\n", TAG, ret);
                    capture_release_frame(apb);
                    break;
                }
            } else {
                /* s_playing true but no cooldown yet: send silence */
                send_audio_frame(&s_tls, s_session_id,
                    silence_buf, silence_len);
                silence_count++;
            }
            capture_release_frame(apb);
        } else {
            /* Capture timeout or empty: send silence to keep alive */
            if (ret != 0) {
                usleep(10000);  /* 10ms to avoid busy loop */
            }
            if (s_running && s_session_active) {
                ret = send_audio_frame(&s_tls, s_session_id,
                    silence_buf, silence_len);
                silence_count++;
                if (ret != 0) {
                    syslog(LOG_ERR, "[%s] send silence error: %d\n", TAG, ret);
                    break;
                }
            }
        }
    }

    free(silence_buf);
    syslog(LOG_INFO, "[%s] sent %d audio + %d silence frames\n",
        TAG, frame_count, silence_count);

    capture_stop();
    capture_deinit();
    syslog(LOG_INFO, "[%s] send thread exiting\n", TAG);
    return NULL;
}

/* ── Main entry point ──────────────────────────────────────────── */

static void print_usage(void)
{
    printf("Volcengine E2E Realtime Voice Client\n");
    printf("Usage: volc_e2e_voice [options]\n");
    printf("Options:\n");
    printf("  -s <speaker>  TTS speaker (default: %s)\n", DEFAULT_SPEAKER);
    printf("  -r <role>     System role prompt\n");
    printf("  -h            Show this help\n");
    printf("\nPress Ctrl+C to exit.\n");
}

/* SIGINT handler: set interrupted flag to exit main loop */
static void sigint_handler(int sig)
{
    (void)sig;
    s_interrupted = true;
    s_running = false;
}

int main(int argc, char* argv[])
{
    const char* speaker = DEFAULT_SPEAKER;
    const char* system_role = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "s:r:h")) != -1) {
        switch (opt) {
        case 's': speaker = optarg; break;
        case 'r': system_role = optarg; break;
        case 'h': print_usage(); return 0;
        default: print_usage(); return 1;
        }
    }

    syslog(LOG_INFO, "[%s] Starting E2E voice client\n", TAG);

    /* Register signal handler for graceful exit */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Use hardcoded credentials for testing */
    strncpy(s_app_id, VOLC_APP_ID, sizeof(s_app_id) - 1);
    strncpy(s_token, VOLC_TOKEN, sizeof(s_token) - 1);
    strncpy(s_speaker, speaker, sizeof(s_speaker) - 1);

    syslog(LOG_INFO, "[%s] app_id=%s speaker=%s\n",
        TAG, s_app_id, s_speaker);

    /* Generate session ID */
    generate_uuid(s_session_id, sizeof(s_session_id));
    syslog(LOG_INFO, "[%s] session_id=%s\n", TAG, s_session_id);

    /* Connect */
    int ret = tls_connect(&s_tls, WS_HOST, WS_PORT);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] TLS connect failed: %d\n", TAG, ret);
        return 1;
    }

    /* WebSocket handshake */
    ret = ws_handshake(&s_tls, WS_HOST, WS_PATH, s_app_id, s_token);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] WS handshake failed: %d\n", TAG, ret);
        tls_free(&s_tls);
        return 1;
    }

    /* Initialize playback */
    ret = playback_init();
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] playback init failed: %d\n", TAG, ret);
        tls_free(&s_tls);
        return 1;
    }

    /* Start connection */
    s_running = true;

    ret = send_start_connection(&s_tls);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] send StartConnection failed: %d\n", TAG, ret);
        goto cleanup;
    }

    /* Start receive thread */
    pthread_create(&s_recv_tid, NULL, recv_thread, NULL);

    /* Wait for ConnectionStarted (up to 5 seconds) */
    for (int i = 0; i < 50 && s_running; i++) {
        usleep(100000);  /* 100ms per poll */
    }

    /* Check if receive thread is still running */
    if (!s_running) {
        syslog(LOG_ERR, "[%s] connection failed, exiting\n", TAG);
        goto cleanup;
    }

    /* Start session */
    ret = send_start_session(&s_tls, s_session_id, s_speaker, system_role);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] send StartSession failed: %d\n", TAG, ret);
        goto cleanup;
    }

    /* Wait for SessionStarted (up to 10 seconds) */
    for (int i = 0; i < 100 && s_running && !s_session_active; i++) {
        usleep(100000);  /* 100ms per poll */
    }

    /* Check if session is active */
    if (!s_running || !s_session_active) {
        syslog(LOG_ERR, "[%s] session start failed, exiting\n", TAG);
        goto cleanup;
    }

    /* Start send thread (audio capture + send) */
    pthread_create(&s_send_tid, NULL, send_thread, NULL);

    /* Main loop: wait for user to press Ctrl+C */
    syslog(LOG_INFO, "[%s] Voice session active. Press Ctrl+C to exit.\n", TAG);

    while (s_running && s_session_active) {
        usleep(100000);

        /* Check if threads are still alive */
        if (s_recv_tid && pthread_kill(s_recv_tid, 0) != 0) {
            syslog(LOG_WARNING, "[%s] recv thread died\n", TAG);
            s_running = false;
        }
        if (s_send_tid && pthread_kill(s_send_tid, 0) != 0) {
            syslog(LOG_WARNING, "[%s] send thread died\n", TAG);
            s_running = false;
        }

        /* If connection lost (not user exit), auto-reconnect */
        if (!s_running && !s_interrupted) {
            syslog(LOG_INFO, "[%s] connection lost, reconnecting in 2s...\n", TAG);
            sleep(2);

            /* Cleanup old threads */
            s_running = false;
            s_session_active = false;
            if (s_recv_tid) { pthread_join(s_recv_tid, NULL); s_recv_tid = 0; }
            if (s_send_tid) { pthread_join(s_send_tid, NULL); s_send_tid = 0; }

            /* Cleanup old TLS and audio */
            playback_stop();
            if (s_nxplayer) { nxplayer_release(s_nxplayer); s_nxplayer = NULL; }
            capture_deinit();
            tls_free(&s_tls);

            /* Reset state */
            s_playing = false;
            s_cooldown_active = false;
            s_pcm_len = 0;

            /* Generate new session ID */
            generate_uuid(s_session_id, sizeof(s_session_id));
            syslog(LOG_INFO, "[%s] reconnect session_id=%s\n", TAG, s_session_id);

            /* Reconnect TLS */
            ret = tls_connect(&s_tls, "openspeech.bytedance.com", "443");
            if (ret != 0) {
                syslog(LOG_ERR, "[%s] reconnect TLS failed: %d\n", TAG, ret);
                break;
            }

            /* WS handshake */
            ret = ws_handshake(&s_tls, WS_HOST, WS_PATH, s_app_id, s_token);
            if (ret != 0) {
                syslog(LOG_ERR, "[%s] reconnect WS failed: %d\n", TAG, ret);
                tls_free(&s_tls);
                break;
            }

            /* Re-init playback */
            playback_init();

            /* Start connection + session */
            s_running = true;
            ret = send_start_connection(&s_tls);
            if (ret != 0) { syslog(LOG_ERR, "[%s] reconnect StartConnection failed\n", TAG); break; }

            pthread_create(&s_recv_tid, NULL, recv_thread, NULL);
            for (int i = 0; i < 50 && s_running; i++) usleep(100000);
            if (!s_running) { syslog(LOG_ERR, "[%s] reconnect ConnectionStarted failed\n", TAG); break; }

            ret = send_start_session(&s_tls, s_session_id, s_speaker, system_role);
            if (ret != 0) { syslog(LOG_ERR, "[%s] reconnect StartSession failed\n", TAG); break; }

            for (int i = 0; i < 100 && s_running && !s_session_active; i++) usleep(100000);
            if (!s_running || !s_session_active) {
                syslog(LOG_ERR, "[%s] reconnect SessionStarted failed\n", TAG); break;
            }

            /* Restart send thread */
            pthread_create(&s_send_tid, NULL, send_thread, NULL);
            syslog(LOG_INFO, "[%s] reconnected successfully\n", TAG);
        }
    }

cleanup:
    /* Cleanup */
    s_running = false;
    s_session_active = false;

    /* Wait for threads */
    if (s_recv_tid) {
        pthread_join(s_recv_tid, NULL);
    }
    if (s_send_tid) {
        pthread_join(s_send_tid, NULL);
    }

    /* Send finish events */
    if (s_tls.net.fd >= 0) {
        send_finish_session(&s_tls, s_session_id);
        usleep(100000);
        send_finish_connection(&s_tls);
        usleep(100000);
    }

    /* Cleanup resources */
    playback_stop();
    if (s_nxplayer) {
        nxplayer_release(s_nxplayer);
        s_nxplayer = NULL;
    }
    capture_deinit();
    tls_free(&s_tls);

    syslog(LOG_INFO, "[%s] E2E voice client exited\n", TAG);
    return 0;
}

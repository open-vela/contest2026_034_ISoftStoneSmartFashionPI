/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Volcengine E2E realtime dialogue connection — see volc_e2e.h for the
 * threading contract.  Ported from apps/examples/volc_e2e_voice with the
 * audio device handling stripped out (voice_channel.c owns capture and
 * playback) and the 2MB PCM staging buffer replaced by a small ring. */

#include "volc_e2e.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include "cJSON.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "core/session_mgr.h"

#include "volc_e2e.h"

static const char* TAG = "volc_e2e";

/* ── Protocol constants ───────────────────────────────────────── */

/* External watch app APIs (resolved at link time) — 设备状态注入用 */
extern uint8_t watch_battery_get_level(void);
extern int watch_volume_get_percent(void);

#define VOLC_PROTO_VER 0x11 /* version=1, header_size=1 (x4=4B) */
#define VOLC_HDR_SIZE 4
#define VOLC_FLAG_EVENT 0x04

#define VOLC_MSG_FULL_REQ 0x10
#define VOLC_MSG_AUDIO_REQ 0x20
#define VOLC_MSG_FULL_RESP 0x90
#define VOLC_MSG_AUDIO_RESP 0xB0
#define VOLC_MSG_ERROR 0xF0

#define VOLC_SER_RAW 0x00
#define VOLC_SER_JSON 0x10

/* Client events */
#define EVENT_START_CONNECTION 1
#define EVENT_FINISH_CONNECTION 2
#define EVENT_START_SESSION 100
#define EVENT_FINISH_SESSION 102
#define EVENT_TASK_REQUEST 200
#define EVENT_END_ASR 400
#define EVENT_CHAT_TTS_TEXT 500

/* Server events */
#define EVENT_CONNECTION_STARTED 50
#define EVENT_CONNECTION_FAILED 51
#define EVENT_CONNECTION_FINISHED 52
#define EVENT_SESSION_STARTED 150
#define EVENT_SESSION_FINISHED 152
#define EVENT_SESSION_FAILED 153
#define EVENT_TTS_SENTENCE_START 350
#define EVENT_TTS_SENTENCE_END 351
#define EVENT_TTS_RESPONSE 352
#define EVENT_TTS_ENDED 359
#define EVENT_ASR_INFO 450
#define EVENT_ASR_RESPONSE 451
#define EVENT_ASR_ENDED 459
#define EVENT_CHAT_RESPONSE 550
#define EVENT_CHAT_ENDED 559
#define EVENT_DIALOG_ERROR 599

/* WebSocket */
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08
#define WS_OPCODE_PING 0x09
#define WS_OPCODE_PONG 0x0A
#define WS_MASK_KEY_LEN 4

#define SESSION_ID_LEN 36

/* Buffers.  The ESP32-S3 heap is shared with the agent loop and the
 * LVGL launcher, but the uplink ring must be large enough to absorb
 * the server's TTS→listening transition delay (can be 4-6s after
 * TTSEnded before the server starts reading audio again). */
#define E2E_WS_BUF_SIZE (64 * 1024)   /* must exceed the server's
                                        * largest TTS audio frame
                                        * (observed 34868-byte frames
                                        * after a wake word; 32KB killed
                                        * the recv thread determin-
                                        * istically) */
#define E2E_AUDIO_RING_CAP (256 * 1024) /* ~8s @16k mono 16bit */
#define E2E_PCM_RING_CAP (384 * 1024)  /* ~8s @24k mono 16bit —
                                        * must accommodate the
                                        * server's full self-chat
                                        * stream plus our synthesis
                                        * while the consumer is
                                        * draining. */
#define E2E_UPLINK_CHUNK 640           /* 20ms @16k mono 16bit */
#define E2E_CTRL_MAX 12
#define E2E_ASR_TEXT_MAX 256

#define E2E_THREAD_STACK (10 * 1024)
#define E2E_RECONNECT_BACKOFF_MS 3000

/* Model version (O2.0) */
#define E2E_MODEL "1.2.1.1"
#define E2E_BOT_NAME "小潮"
/* 轻量人设：只给 E2E 服务端（O2.0 闲聊脑）身份与口语规则，不传技能/工具/记忆。 */
#define E2E_SYSTEM_ROLE \
    "你是小潮，AI情绪能量潮玩，正方体桌面陪伴机器人，屏幕显示表情。" \
    "性格温柔耐心、偶尔小幽默。中文口语回复，不超过40字，不说让我查一下。" \
    "不推销自己、不描述功能。"
#define E2E_DIALOG_CONTEXT_MAX 20

/* ── Turn state machine ───────────────────────────────────────── */

typedef enum {
    E2E_TURN_IDLE = 0,
    E2E_TURN_LISTENING,      /* uplink audio accepted */
    E2E_TURN_ASR_DONE,       /* ASREnded seen, uplink closed */
    E2E_TURN_TTS_CLAIMED,    /* ChatTTSText start frame queued */
    E2E_TURN_TTS_STREAMING,  /* our reply text queued, PCM gate open */
    E2E_TURN_DONE,           /* TTSEnded seen */
} e2e_turn_t;

/* ── Ring buffer ──────────────────────────────────────────────── */

typedef struct {
    unsigned char* buf;
    size_t cap;
    size_t head; /* read index */
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} e2e_ring_t;

static int ring_init(e2e_ring_t* r, size_t cap)
{
    r->buf = malloc(cap);
    if (!r->buf) return -ENOMEM;
    r->cap = cap;
    r->head = 0;
    r->count = 0;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    return 0;
}

static void ring_free(e2e_ring_t* r)
{
    free(r->buf);
    r->buf = NULL;
    r->cap = 0;
    r->head = 0;
    r->count = 0;
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->lock);
}

static void ring_clear(e2e_ring_t* r)
{
    if (!r->buf) return;
    pthread_mutex_lock(&r->lock);
    r->head = 0;
    r->count = 0;
    pthread_mutex_unlock(&r->lock);
}

/* Returns bytes accepted (may be less than len when full). */
static size_t ring_write(e2e_ring_t* r, const unsigned char* src, size_t len)
{
    if (!r->buf) return 0;
    pthread_mutex_lock(&r->lock);
    size_t space = r->cap - r->count;
    if (len > space) len = space;
    size_t tail = (r->head + r->count) % r->cap;
    size_t first = r->cap - tail;
    if (first > len) first = len;
    memcpy(r->buf + tail, src, first);
    if (len > first) {
        memcpy(r->buf, src + first, len - first);
    }
    r->count += len;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
    return len;
}

static size_t ring_read(e2e_ring_t* r, unsigned char* dst, size_t cap)
{
    if (!r->buf) return 0;
    pthread_mutex_lock(&r->lock);
    size_t len = r->count < cap ? r->count : cap;
    size_t first = r->cap - r->head;
    if (first > len) first = len;
    memcpy(dst, r->buf + r->head, first);
    if (len > first) {
        memcpy(dst + first, r->buf, len - first);
    }
    r->head = (r->head + len) % r->cap;
    r->count -= len;
    pthread_mutex_unlock(&r->lock);
    return len;
}

static size_t ring_count(e2e_ring_t* r)
{
    if (!r->buf) return 0;
    pthread_mutex_lock(&r->lock);
    size_t n = r->count;
    pthread_mutex_unlock(&r->lock);
    return n;
}

/* ── TLS context ──────────────────────────────────────────────── */

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} e2e_tls_t;

/* ── Module state ─────────────────────────────────────────────── */

typedef struct {
    uint32_t event;
    char* json; /* malloc'd, freed by send thread */
} e2e_ctrl_t;

static e2e_tls_t s_tls;
static pthread_mutex_t s_tls_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_tls_valid;

static char s_session_id[SESSION_ID_LEN + 1];
static char s_dialog_id[48];

static volatile bool s_running;     /* threads should keep going */
static volatile bool s_conn_up;     /* WS + StartConnection done */
static volatile bool s_session_up;  /* SessionStarted seen */
static pthread_t s_recv_tid;
static pthread_t s_send_tid;
static bool s_recv_started;
static bool s_send_started;
static pthread_mutex_t s_start_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_next_retry_ms;

static volatile e2e_turn_t s_turn = E2E_TURN_IDLE;

/* Set when volc_e2e_arm_turn() had to be postponed because a TTS
 * downlink was still being consumed (voice_channel.c pre-opens the next
 * ASR stream before playback ends).  Honoured by volc_e2e_push_audio(). */
static volatile bool s_arm_pending;

static e2e_ring_t s_audio_ring; /* uplink PCM */
static e2e_ring_t s_pcm_ring;   /* downlink TTS PCM */
static volatile bool s_pcm_eos;
static volatile bool s_pcm_abort;
static size_t s_pcm_dropped;

/* Running total of uplink PCM bytes that could not be queued because
 * the ring was full.  Reset on every turn arm; surfaced via syslog. */
static unsigned int s_uplink_drop_bytes;
/* Uplink stall self-heal: send_thread can stay alive while TCP writes
 * are too slow to drain the ring (no send error, no dialog error — the
 * existing self-heal paths never fire, observed 2min of continuous
 * "uplink ring full" with wake words dead).  First drop timestamps the
 * stall; a full write clears it; sustained drops past the heal window
 * mark the session down so the next ASR open redials. */
#define UPLINK_STALL_HEAL_MS  10000
static uint32_t s_uplink_stall_since;

/* True between the first volc_e2e_tts_text() and the matching
 * volc_e2e_tts_end().  While false, arriving TTSResponse frames
 * belong to the server's own (unsolicited) synthesis and must be
 * dropped even when s_turn == TTS_STREAMING.  Without this gate the
 * server can finish its self-chat AFTER our {end:true} and the tail
 * would be mixed into our audio ring, producing a double-broadcast. */
static volatile bool s_our_text_open;

/* True once volc_e2e_tts_text() has been called at least once for
 * this turn.  Reset on turn arm.  Combined with s_our_text_open,
 * this lets the consumer distinguish:
 *   - "we have never sent our own text" (accept server self-chat)
 *   - "we have sent our own text and closed the stream" (reject
 *     late-arriving server self-chat to avoid double broadcast). */
static volatile bool s_our_text_sent;

/* True once we have actually accepted at least one PCM frame for our
 * own ChatTTSText stream (i.e. after s_our_text_sent went true and
 * before tts_end/tts_abort).  This is the definitive signal that
 * our synthesis has started flowing; a preceding TTSEnded MUST be
 * the server's self-chat, NOT our stream.  Reset on turn arm. */
static volatile bool s_got_our_pcm;

/* True once the server has sent TTSEnded for its own self-chat (i.e.
 * in E2E_TURN_ASR_DONE before we produced text).  A subsequent
 * TTSEnded after our PCM has started flowing can then be recognized
 * as OUR stream's end rather than the server's.  Reset on turn arm. */
static volatile bool s_server_ended;

/* ── Hybrid orchestration (E2E + agent parallel) ─────────────────
 *
 * When enabled (set via volc_e2e_set_hybrid_enabled()), the per-turn
 * flow is:
 *   ASREnded          -> s_hybrid = PENDING, server audio flows into
 *                        s_pcm_ring (≈8s capacity)
 *   ChatResponse(550) -> server reply text cached in s_server_reply
 *   TTSEnded          -> mark s_pcm_eos = true but keep s_turn as
 *                        ASR_DONE so the ring stays readable
 *   agent decision    -> route_e2e()   unblocks pop_pcm for drain
 *                     OR route_agent() clears ring, returns text
 * Reset on turn arm. */
static volatile bool s_hybrid_enabled;
static volatile e2e_hybrid_t s_hybrid;

/* Server's ChatResponse text for this turn.  Used as the reply when
 * the hybrid route picks the E2E fast path (no tool calls).  Kept
 * small (128 bytes ≈ 42 Chinese chars) to conserve DRAM on
 * ESP32-S3; typical one-sentence chat replies fit easily. */
#define E2E_SERVER_REPLY_MAX 128
static char s_server_reply[E2E_SERVER_REPLY_MAX];
static pthread_mutex_t s_reply_lock = PTHREAD_MUTEX_INITIALIZER;

static e2e_ctrl_t s_ctrl[E2E_CTRL_MAX];
static int s_ctrl_head;
static int s_ctrl_count;
static pthread_mutex_t s_ctrl_lock = PTHREAD_MUTEX_INITIALIZER;

static char s_asr_text[E2E_ASR_TEXT_MAX];
static volatile bool s_asr_final;
static pthread_mutex_t s_asr_lock = PTHREAD_MUTEX_INITIALIZER;

/* Credentials / dialog config */
static char s_app_id[64];
static char s_token[192];
static char s_speaker[48];

/* ── Small helpers ────────────────────────────────────────────── */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) fd = open("/dev/random", O_RDONLY);
    if (fd < 0) return -1;

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

static void generate_uuid(char* out, size_t cap)
{
    unsigned char rnd[16];
    entropy_func(NULL, rnd, sizeof(rnd));
    rnd[6] = (rnd[6] & 0x0F) | 0x40;
    rnd[8] = (rnd[8] & 0x3F) | 0x80;
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x-%02x%02x%02x%02x%02x%02x",
        rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7],
        rnd[8], rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14],
        rnd[15]);
}

static uint32_t read_be32(const unsigned char* buf, size_t off)
{
    return ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16)
        | ((uint32_t)buf[off + 2] << 8) | (uint32_t)buf[off + 3];
}

static void write_be32(unsigned char* buf, size_t off, uint32_t val)
{
    buf[off] = (unsigned char)((val >> 24) & 0xFF);
    buf[off + 1] = (unsigned char)((val >> 16) & 0xFF);
    buf[off + 2] = (unsigned char)((val >> 8) & 0xFF);
    buf[off + 3] = (unsigned char)(val & 0xFF);
}

/* ── TLS ──────────────────────────────────────────────────────── */

static int tls_connect(e2e_tls_t* ctx, const char* host, const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    const char* pers = "volc_e2e";
    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, entropy_func, NULL,
        (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ctr_drbg_seed: -0x%04x\n", TAG, -ret);
        return -EIO;
    }

    /* Custom TCP connect with a 5-second timeout.  The original
     * mbedtls_net_connect() uses a blocking connect(2) with no
     * timeout: on NuttX the OS TCP retransmission timeout can be
     * 75–120 s, which stalls the conversation thread (and any
     * queued proactive prompt) for minutes when the E2E server is
     * unreachable after a WS frame error. */
    {
        struct addrinfo hints, *res, *rp;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        int gai = getaddrinfo(host, port, &hints, &res);
        if (gai != 0) {
            syslog(LOG_ERR, "[%s] dns %s: %s\n",
                TAG, host, gai_strerror(gai));
            return -ECONNREFUSED;
        }

        int fd = -1;
        int conn_ret = -1;
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0) continue;

            /* 5-second timeout for both TCP connect and later I/O */
            struct timeval tv5 = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv5, sizeof(tv5));
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv5, sizeof(tv5));

            conn_ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
            if (conn_ret == 0) break;

            syslog(LOG_ERR, "[%s] connect %s:%s: errno=%d\n",
                TAG, host, port, errno);
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);

        if (fd < 0) {
            syslog(LOG_ERR, "[%s] net_connect %s:%s failed\n",
                TAG, host, port);
            return -ECONNREFUSED;
        }

        ctx->net.fd = fd;
    }

    /* Socket is already blocking (default); SO_RCVTIMEO/SO_SNDTIMEO
     * are set above (5s).  The handshake loop below also catches
     * WANT_READ/WANT_WRITE as timeout indicators. */

    ret = mbedtls_ssl_config_defaults(&ctx->cfg, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return -EIO;

    /* TLS 1.2 only.  See the note in tong_ai_ws.c about why we must
     * not reference MBEDTLS_SSL_PROTO_TLS1_3 here. */
    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);

#if defined(MBEDTLS_SSL_ALPN)
    static const char * alpn[] = { "http/1.1", NULL };
    mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, alpn);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) return -EIO;

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

    /* Non-blocking from here on: the recv thread must not hold the TLS
     * mutex during an idle read, otherwise the send thread starves. */
    mbedtls_net_set_nonblock(&ctx->net);

    syslog(LOG_INFO, "[%s] TLS connected to %s:%s\n", TAG, host, port);
    return 0;
}

static void tls_free(e2e_tls_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

static int tls_write_all(e2e_tls_t* ctx, const unsigned char* buf, size_t len)
{
    if (!ctx || ctx->net.fd < 0) return -EINVAL;

    pthread_mutex_lock(&s_tls_lock);
    size_t written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);
        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE
            || ret == MBEDTLS_ERR_SSL_WANT_READ) {
            /* Release the lock so the recv thread can drain pending
             * incoming data (which may be causing WANT_READ on write)
             * and wait efficiently for the socket to become ready. */
            int fd = ctx->net.fd;
            short events = (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                         ? POLLOUT : POLLIN;
            pthread_mutex_unlock(&s_tls_lock);
            struct pollfd pfd = { .fd = fd, .events = events };
            poll(&pfd, 1, 1000);
            pthread_mutex_lock(&s_tls_lock);
        } else if (ret == MBEDTLS_ERR_NET_CONN_RESET) {
            pthread_mutex_unlock(&s_tls_lock);
            syslog(LOG_ERR, "[%s] write: connection reset\n", TAG);
            return -ECONNRESET;
        } else {
            pthread_mutex_unlock(&s_tls_lock);
            syslog(LOG_ERR, "[%s] ssl_write: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }
    pthread_mutex_unlock(&s_tls_lock);
    return 0;
}

static int tls_read_all(e2e_tls_t* ctx, unsigned char* buf, size_t len)
{
    if (!ctx || ctx->net.fd < 0) return -EINVAL;

    pthread_mutex_lock(&s_tls_lock);
    size_t got = 0;
    while (got < len) {
        if (!s_running) {
            pthread_mutex_unlock(&s_tls_lock);
            return -ECANCELED;
        }
        int ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);
        if (ret > 0) {
            got += (size_t)ret;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            pthread_mutex_unlock(&s_tls_lock);
            syslog(LOG_INFO, "[%s] closed by peer\n", TAG);
            return -ECONNRESET;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            /* Release the mutex so the send thread can transmit, and
             * wait efficiently for incoming data instead of busy-
             * polling with usleep.  This prevents the send thread
             * from being starved when both threads hit WANT_READ. */
            int fd = ctx->net.fd;
            pthread_mutex_unlock(&s_tls_lock);
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            poll(&pfd, 1, 1000);
            pthread_mutex_lock(&s_tls_lock);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            int fd = ctx->net.fd;
            pthread_mutex_unlock(&s_tls_lock);
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            poll(&pfd, 1, 1000);
            pthread_mutex_lock(&s_tls_lock);
        } else if (ret == MBEDTLS_ERR_NET_CONN_RESET) {
            pthread_mutex_unlock(&s_tls_lock);
            syslog(LOG_ERR, "[%s] read: connection reset\n", TAG);
            return -ECONNRESET;
        } else {
            pthread_mutex_unlock(&s_tls_lock);
            syslog(LOG_ERR, "[%s] ssl_read: -0x%04x\n", TAG, -ret);
            return -EIO;
        }
    }
    pthread_mutex_unlock(&s_tls_lock);
    return 0;
}

/* ── WebSocket ────────────────────────────────────────────────── */

static int ws_handshake(e2e_tls_t* ctx)
{
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len = 0;
    char connect_id[40];

    entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len,
        key_raw, sizeof(key_raw));

    /* X-Api-Connect-Id: connection UUID echoed back by the server in
     * the StartConnection response; used for trace correlation. */
    generate_uuid(connect_id, sizeof(connect_id));

    char req[768];
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
        "X-Api-Connect-Id: %s\r\n"
        "\r\n",
        AGENT_VOLC_E2E_WS_PATH, AGENT_VOLC_E2E_HOST,
        (int)key_b64_len, key_b64, s_app_id, s_token,
        AGENT_VOLC_E2E_RESOURCE, AGENT_VOLC_E2E_APP_KEY, connect_id);

    if (n <= 0 || n >= (int)sizeof(req)) return -EOVERFLOW;

    syslog(LOG_INFO, "[%s] WS upgrade: host=%s path=%s app_id=%.6s... token=%.6s...(len=%zu)\n",
        TAG, AGENT_VOLC_E2E_HOST, AGENT_VOLC_E2E_WS_PATH,
        s_app_id, s_token, strlen(s_token));

    int ret = tls_write_all(ctx, (const unsigned char*)req, (size_t)n);
    if (ret != 0) return ret;

    char resp[2048];
    size_t rlen = 0;
    uint32_t deadline = now_ms() + 10000;

    /* Read HTTP headers. */
    while (rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)resp + rlen,
            sizeof(resp) - 1 - rlen);
        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) break;
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            if ((int32_t)(now_ms() - deadline) > 0) return -ETIMEDOUT;
            usleep(10000);
        } else {
            return -EIO;
        }
    }

    /* Locate the start of the body so we can continue reading it. */
    char* body = strstr(resp, "\r\n\r\n");
    size_t body_have = body ? rlen - ((size_t)(body - resp) + 4) : 0;

    /* Parse Content-Length and drain the body into `resp`. */
    int content_len = 0;
    {
        char* cl = strstr(resp, "Content-Length:");
        if (!cl) cl = strstr(resp, "content-length:");
        if (cl) content_len = atoi(cl + 15);
    }
    while (body_have < (size_t)content_len && rlen < sizeof(resp) - 1) {
        int r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)resp + rlen,
            sizeof(resp) - 1 - rlen);
        if (r > 0) {
            rlen += (size_t)r;
            body_have += (size_t)r;
            resp[rlen] = '\0';
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            if ((int32_t)(now_ms() - deadline) > 0) break;
            usleep(10000);
        } else {
            break;
        }
    }
    body = strstr(resp, "\r\n\r\n");
    const char* body_str = body ? body + 4 : "";

    int status = 0;
    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        /* 4xx bodies from openspeech contain a machine-readable error
         * code (InvalidAccessKey / AppIDNotMatch / PermissionDenied /
         * SignatureDoesNotMatch ...).  Log it verbatim so the root
         * cause is obvious. */
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);
        syslog(LOG_ERR, "[%s] body (%d B): %.256s\n", TAG,
            content_len, body_str);
        return -EPROTO;
    }

    syslog(LOG_INFO, "[%s] WS upgrade OK\n", TAG);
    return 0;
}

static int ws_send_masked(e2e_tls_t* ctx, unsigned char opcode,
    const unsigned char* payload, size_t plen)
{
    if (!ctx || ctx->net.fd < 0) return -EINVAL;

    unsigned char hdr[14];
    size_t hdr_len = 0;

    hdr[0] = WS_FIN_BIT | opcode;

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

    unsigned char chunk[512];
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

static int ws_recv_frame(e2e_tls_t* ctx, unsigned char* buf, size_t cap,
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
        /* Oversized frame (e.g. a huge server TTS blob): drain and
         * discard it so the session survives — the next frame boundary
         * stays intact.  Killing the connection here (the old
         * behaviour) took the whole voice loop down for the reconnect
         * window.  parse_server_frame() rejects the empty result and
         * the recv loop just continues. */
        syslog(LOG_WARNING,
            "[%s] WS frame too large: %zu > %zu, skipping frame\n",
            TAG, plen, cap);
        unsigned char sink[512];
        while (plen > 0) {
            size_t n = plen > sizeof(sink) ? sizeof(sink) : plen;
            ret = tls_read_all(ctx, sink, n);
            if (ret != 0) return ret;
            plen -= n;
        }
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

/* ── Volcengine frame codec ───────────────────────────────────── */

static int send_client_frame(uint32_t event_id, const char* session_id,
    const unsigned char* payload, size_t plen)
{
    if (!s_tls_valid) return -ENOTCONN;

    size_t sid_len = session_id ? strlen(session_id) : 0;
    bool is_connect = (event_id == EVENT_START_CONNECTION
        || event_id == EVENT_FINISH_CONNECTION);

    size_t frame_len = is_connect
        ? VOLC_HDR_SIZE + 4 + 4 + plen
        : VOLC_HDR_SIZE + 4 + 4 + sid_len + 4 + plen;

    unsigned char* frame = malloc(frame_len);
    if (!frame) return -ENOMEM;

    size_t off = 0;
    frame[off++] = VOLC_PROTO_VER;
    frame[off++] = VOLC_MSG_FULL_REQ | VOLC_FLAG_EVENT;
    frame[off++] = VOLC_SER_JSON;
    frame[off++] = 0x00;

    write_be32(frame, off, event_id);
    off += 4;

    if (!is_connect) {
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

    int ret = ws_send_masked(&s_tls, WS_OPCODE_BINARY, frame, frame_len);
    free(frame);
    return ret;
}

/* Compile-time max for the audio frame buffer.
 * Layout: VOLC_HDR_SIZE(4) + event(4) + sid_len(4) + sid(36) + alen(4) + audio(640)
 * = 692 bytes.  Use 768 for alignment headroom. */
#define E2E_AUDIO_FRAME_MAX 768

static int send_audio_frame(const unsigned char* audio, size_t alen)
{
    if (!s_tls_valid || s_session_id[0] == '\0') return -ENOTCONN;

    size_t sid_len = strlen(s_session_id);
    size_t frame_len = VOLC_HDR_SIZE + 4 + 4 + sid_len + 4 + alen;

    /* Static buffer: this function is called ~50×/sec from send_thread
     * only (single writer), so no concurrent access.  Eliminates 50
     * malloc/free pairs per second of continuous speech. */
    static unsigned char frame[E2E_AUDIO_FRAME_MAX];
    if (frame_len > sizeof(frame)) return -E2BIG;

    size_t off = 0;
    frame[off++] = VOLC_PROTO_VER;
    frame[off++] = VOLC_MSG_AUDIO_REQ | VOLC_FLAG_EVENT;
    frame[off++] = VOLC_SER_RAW;
    frame[off++] = 0x00;

    write_be32(frame, off, EVENT_TASK_REQUEST);
    off += 4;
    write_be32(frame, off, (uint32_t)sid_len);
    off += 4;
    memcpy(frame + off, s_session_id, sid_len);
    off += sid_len;
    write_be32(frame, off, (uint32_t)alen);
    off += 4;
    memcpy(frame + off, audio, alen);

    return ws_send_masked(&s_tls, WS_OPCODE_BINARY, frame, frame_len);
}

static int parse_server_frame(const unsigned char* frame, size_t frame_len,
    uint32_t* out_event, unsigned char** out_payload, size_t* out_plen)
{
    if (frame_len < VOLC_HDR_SIZE) return -EINVAL;

    uint8_t msg_type = frame[1] & 0xF0;

    if (msg_type == VOLC_MSG_ERROR) {
        if (frame_len < VOLC_HDR_SIZE + 8) return -EINVAL;
        *out_event = EVENT_DIALOG_ERROR;
        uint32_t err_code = read_be32(frame, 4);
        uint32_t err_len = read_be32(frame, 8);
        syslog(LOG_ERR, "[%s] server error: code=%u\n", TAG, err_code);
        if (err_len > 0 && VOLC_HDR_SIZE + 8 + err_len <= frame_len) {
            *out_payload = (unsigned char*)(frame + VOLC_HDR_SIZE + 8);
            *out_plen = err_len;
        } else {
            *out_payload = NULL;
            *out_plen = 0;
        }
        return 0;
    }

    if (msg_type != VOLC_MSG_FULL_RESP && msg_type != VOLC_MSG_AUDIO_RESP) {
        return -EINVAL;
    }

    *out_event = read_be32(frame, 4);
    if (msg_type == VOLC_MSG_AUDIO_RESP) {
        *out_event = EVENT_TTS_RESPONSE;
    }

    size_t off = 8;
    if (off + 4 > frame_len) return -EINVAL;
    uint32_t sid_len = read_be32(frame, off);
    off += 4;
    if (sid_len > 0) {
        if (off + sid_len > frame_len) return -EINVAL;
        off += sid_len;
    }

    if (off + 4 > frame_len) {
        *out_payload = NULL;
        *out_plen = 0;
        return 0;
    }
    uint32_t plen = read_be32(frame, off);
    off += 4;
    if (off + plen > frame_len) return -EINVAL;

    *out_payload = (unsigned char*)(frame + off);
    *out_plen = plen;
    return 0;
}

/* ── Control outbox ───────────────────────────────────────────── */

/* Takes ownership of json (freed by the send thread, or here on error). */
static int ctrl_push(uint32_t event, char* json)
{
    pthread_mutex_lock(&s_ctrl_lock);
    if (s_ctrl_count >= E2E_CTRL_MAX) {
        pthread_mutex_unlock(&s_ctrl_lock);
        syslog(LOG_ERR, "[%s] ctrl outbox full, dropping event %u\n",
            TAG, event);
        free(json);
        return -ENOSPC;
    }
    int slot = (s_ctrl_head + s_ctrl_count) % E2E_CTRL_MAX;
    s_ctrl[slot].event = event;
    s_ctrl[slot].json = json;
    s_ctrl_count++;
    pthread_mutex_unlock(&s_ctrl_lock);
    return 0;
}

static bool ctrl_pop(e2e_ctrl_t* out)
{
    pthread_mutex_lock(&s_ctrl_lock);
    if (s_ctrl_count == 0) {
        pthread_mutex_unlock(&s_ctrl_lock);
        return false;
    }
    *out = s_ctrl[s_ctrl_head];
    s_ctrl[s_ctrl_head].json = NULL;
    s_ctrl_head = (s_ctrl_head + 1) % E2E_CTRL_MAX;
    s_ctrl_count--;
    pthread_mutex_unlock(&s_ctrl_lock);
    return true;
}

static void ctrl_clear(void)
{
    pthread_mutex_lock(&s_ctrl_lock);
    while (s_ctrl_count > 0) {
        free(s_ctrl[s_ctrl_head].json);
        s_ctrl[s_ctrl_head].json = NULL;
        s_ctrl_head = (s_ctrl_head + 1) % E2E_CTRL_MAX;
        s_ctrl_count--;
    }
    s_ctrl_head = 0;
    pthread_mutex_unlock(&s_ctrl_lock);
}

/* ── Event payload builders ───────────────────────────────────── */

static int send_start_connection(void)
{
    syslog(LOG_INFO, "[%s] -> StartConnection\n", TAG);
    return send_client_frame(EVENT_START_CONNECTION, NULL,
        (const unsigned char*)"{}", 2);
}

static int send_start_session(void)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return -ENOMEM;

    cJSON* dialog = cJSON_AddObjectToObject(root, "dialog");
    cJSON_AddStringToObject(dialog, "bot_name", E2E_BOT_NAME);
    /* system_role 动态注入设备状态（电量/音量），与小通
     * (tong_ai_ws.c) 的格式一致：用户查询电量/音量时，服务器回复
     * 必须与实际一致，避免两模式话术"不同步"。 */
    {
        char system_role[768];
        int bat = (int)watch_battery_get_level();
        int vol = watch_volume_get_percent();
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        snprintf(system_role, sizeof(system_role),
            E2E_SYSTEM_ROLE
            "设备状态：电量%d%%，音量%d%%。"
            "回答涉及电量或音量的内容时必须与实际一致。",
            bat, vol);
        cJSON_AddStringToObject(dialog, "system_role", system_role);
    }
    /* 固定 dialog_id：服务端按此 id 跨重连/重启保留最近 20 轮 QA 对。 */
    cJSON_AddStringToObject(dialog, "dialog_id", "voice");

    /* dialog_context：用客户端 session 历史初始化服务端上下文（闲聊 +
     * 工具/技能回合），让两个脑共享记忆。本地文件读，每次 StartSession 一次。 */
    {
        char hist[8192];
        if (session_get_history_json("voice", hist, sizeof(hist),
                                     E2E_DIALOG_CONTEXT_MAX) == OK) {
            cJSON* hist_arr = cJSON_Parse(hist);
            if (hist_arr) {
                if (cJSON_IsArray(hist_arr)) {
                    cJSON* ctx = cJSON_CreateArray();
                    int n = cJSON_GetArraySize(hist_arr);
                    if (n % 2 != 0) n--; /* 必须偶数条（QA 对） */
                    for (int i = 0; i < n; i++) {
                        cJSON* item = cJSON_GetArrayItem(hist_arr, i);
                        cJSON* r = cJSON_GetObjectItem(item, "role");
                        cJSON* c = cJSON_GetObjectItem(item, "content");
                        if (r && c && cJSON_IsString(r) && cJSON_IsString(c)) {
                            cJSON* e = cJSON_CreateObject();
                            cJSON_AddStringToObject(e, "role", r->valuestring);
                            cJSON_AddStringToObject(e, "text", c->valuestring);
                            cJSON_AddItemToArray(ctx, e);
                        }
                    }
                    if (cJSON_GetArraySize(ctx) > 0) {
                        cJSON_AddItemToObject(dialog, "dialog_context", ctx);
                    } else {
                        cJSON_Delete(ctx);
                    }
                }
                cJSON_Delete(hist_arr);
            }
        }
    }

    cJSON* extra = cJSON_AddObjectToObject(dialog, "extra");
    cJSON_AddStringToObject(extra, "model", E2E_MODEL);
    /* keep_alive: the client may stop sending audio while the mic is
     * muted (during playback) without tripping the audio stream
     * timeout, so no silence-frame filler thread is needed. */
    cJSON_AddStringToObject(extra, "input_mod", "keep_alive");

    cJSON* tts = cJSON_AddObjectToObject(root, "tts");
    cJSON_AddStringToObject(tts, "speaker", s_speaker);
    cJSON* tts_audio = cJSON_AddObjectToObject(tts, "audio_config");
    cJSON_AddNumberToObject(tts_audio, "channel", 1);
    cJSON_AddStringToObject(tts_audio, "format", "pcm_s16le");
    cJSON_AddNumberToObject(tts_audio, "sample_rate",
        AGENT_TTS_WS_SAMPLE_RATE);
    /* 与 volc_tts_ws.c 的 volume_ratio=2.0 对齐，E2E 不带时服务端
     * 默认 1.0，导致与本地 TTS 音量不一致 */
    cJSON_AddNumberToObject(tts_audio, "volume_ratio", 2.0);

    cJSON* asr = cJSON_AddObjectToObject(root, "asr");
    cJSON* asr_audio = cJSON_AddObjectToObject(asr, "audio_info");
    cJSON_AddStringToObject(asr_audio, "format", "pcm");
    cJSON_AddNumberToObject(asr_audio, "sample_rate",
        AGENT_VOICE_SAMPLE_RATE);
    cJSON_AddNumberToObject(asr_audio, "channel", 1);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -ENOMEM;

    syslog(LOG_INFO, "[%s] -> StartSession (speaker=%s)\n", TAG, s_speaker);
    int ret = send_client_frame(EVENT_START_SESSION, s_session_id,
        (const unsigned char*)json, strlen(json));
    free(json);
    return ret;
}

static int send_finish_session(void)
{
    syslog(LOG_INFO, "[%s] -> FinishSession\n", TAG);
    return send_client_frame(EVENT_FINISH_SESSION, s_session_id,
        (const unsigned char*)"{}", 2);
}

static int send_finish_connection(void)
{
    syslog(LOG_INFO, "[%s] -> FinishConnection\n", TAG);
    return send_client_frame(EVENT_FINISH_CONNECTION, NULL,
        (const unsigned char*)"{}", 2);
}

/* ChatTTSText (500) payload: {start?, content, end?} */
static char* build_chat_tts_text(bool start, const char* content, bool end)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;
    if (start) cJSON_AddBoolToObject(root, "start", 1);
    cJSON_AddStringToObject(root, "content", content ? content : "");
    if (end) cJSON_AddBoolToObject(root, "end", 1);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ── Receive thread ───────────────────────────────────────────── */

static void handle_asr_response(const unsigned char* payload, size_t plen)
{
    if (plen == 0) return;
    char* json = strndup((const char*)payload, plen);
    if (!json) return;

    cJSON* root = cJSON_Parse(json);
    if (root) {
        cJSON* results = cJSON_GetObjectItem(root, "results");
        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
            cJSON* first = cJSON_GetArrayItem(results, 0);
            cJSON* text = cJSON_GetObjectItem(first, "text");
            cJSON* interim = cJSON_GetObjectItem(first, "is_interim");
            if (cJSON_IsString(text) && text->valuestring[0]) {
                bool is_interim = cJSON_IsTrue(interim);
                syslog(LOG_INFO, "[%s] ASR%s: %s\n", TAG,
                    is_interim ? "(interim)" : "", text->valuestring);
                if (!is_interim) {
                    pthread_mutex_lock(&s_asr_lock);
                    snprintf(s_asr_text, sizeof(s_asr_text), "%s",
                        text->valuestring);
                    pthread_mutex_unlock(&s_asr_lock);
                }
            }
        }
        cJSON_Delete(root);
    }
    free(json);
}

static void handle_asr_ended(void)
{
    syslog(LOG_INFO, "[%s] ASREnded\n", TAG);

    /* Uplink is closed for this turn. */
    ring_clear(&s_audio_ring);

    if (s_turn != E2E_TURN_LISTENING) {
        /* The turn was already given up locally (capture timeout, no
         * speech) or a reply is in flight.  Whatever the server
         * synthesizes on its own is accepted as the reply unless our
         * LLM has already produced text (see recv_thread). */
        syslog(LOG_INFO, "[%s] ASREnded ignored (turn=%d)\n",
            TAG, (int)s_turn);
        return;
    }

    /* Do NOT send {start:true, content:""} here.  The server treats
     * an empty start frame as "our text for this turn is empty" and
     * closes the synthesis pipeline; any subsequent ChatTTSText with
     * real content is then silently ignored.  Instead, let the
     * server's own self-chat play through, and replace it later in
     * volc_e2e_tts_text() once our LLM produces text. */
    s_turn = E2E_TURN_ASR_DONE;

    /* Hybrid orchestration: enter PENDING so the server's self-chat
     * audio flows into the PCM ring (≈8s of capacity) while the
     * client agent LLM runs in parallel.  The agent will decide
     * later whether to drain the ring (E2E fast path) or discard
     * it (agent path with volc_tts_ws). */
    if (s_hybrid_enabled) {
        s_hybrid = E2E_HYBRID_PENDING;
        s_pcm_abort = false;
        syslog(LOG_INFO, "[%s] hybrid=PENDING (buffering server audio)\n",
            TAG);
    }

    pthread_mutex_lock(&s_asr_lock);
    s_asr_final = true;
    pthread_mutex_unlock(&s_asr_lock);
}

static void handle_dialog_error(const unsigned char* payload, size_t plen)
{
    if (plen == 0) {
        syslog(LOG_ERR, "[%s] DialogError: (empty payload)\n", TAG);
        return;
    }
    char* json = strndup((const char*)payload, plen);
    if (!json) return;

    /* Print the raw body first so we see whatever fields the server
     * actually sent (field names differ across server versions). */
    syslog(LOG_ERR, "[%s] DialogError raw: %.*s\n", TAG,
        plen > 512 ? 512 : (int)plen, json);

    cJSON* root = cJSON_Parse(json);
    if (root) {
        /* status_code / code may be string OR integer. */
        cJSON* sc_str = cJSON_GetObjectItem(root, "status_code");
        cJSON* code_str = cJSON_GetObjectItem(root, "code");
        cJSON* msg = cJSON_GetObjectItem(root, "message");
        if (!msg) msg = cJSON_GetObjectItem(root, "msg");
        syslog(LOG_ERR, "[%s] DialogError: status_code=%s code=%s msg=%s\n",
            TAG,
            sc_str ? cJSON_PrintUnformatted(sc_str) : "?",
            code_str ? cJSON_PrintUnformatted(code_str) : "?",
            cJSON_IsString(msg) ? msg->valuestring : "?");
        cJSON_Delete(root);
    }
    free(json);
}

static void* recv_thread(void* arg)
{
    (void)arg;
    unsigned char* buf = malloc(E2E_WS_BUF_SIZE);
    if (!buf) {
        syslog(LOG_ERR, "[%s] recv: alloc failed\n", TAG);
        s_running = false;
        return NULL;
    }

    while (s_running) {
        size_t frame_len = 0;
        int opcode = 0;
        int ret = ws_recv_frame(&s_tls, buf, E2E_WS_BUF_SIZE,
            &frame_len, &opcode);
        if (ret != 0) {
            if (s_running && ret != -ECANCELED) {
                syslog(LOG_ERR, "[%s] recv error: %d\n", TAG, ret);
            }
            break;
        }

        if (opcode == WS_OPCODE_CLOSE) {
            syslog(LOG_INFO, "[%s] server close frame\n", TAG);
            break;
        }
        if (opcode == WS_OPCODE_PING) {
            /* RFC 6455: client frames must be masked, PONG included. */
            ws_send_masked(&s_tls, WS_OPCODE_PONG, buf, frame_len);
            continue;
        }
        if (opcode != WS_OPCODE_BINARY) continue;

        uint32_t event_id = 0;
        unsigned char* payload = NULL;
        size_t plen = 0;
        ret = parse_server_frame(buf, frame_len, &event_id, &payload, &plen);
        if (ret != 0) continue;

        switch (event_id) {
        case EVENT_CONNECTION_STARTED:
            syslog(LOG_INFO, "[%s] ConnectionStarted\n", TAG);
            s_conn_up = true;
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
            if (plen > 0) {
                char* json = strndup((const char*)payload, plen);
                if (json) {
                    cJSON* root = cJSON_Parse(json);
                    if (root) {
                        cJSON* did = cJSON_GetObjectItem(root, "dialog_id");
                        if (cJSON_IsString(did)) {
                            snprintf(s_dialog_id, sizeof(s_dialog_id), "%s",
                                did->valuestring);
                        }
                        cJSON_Delete(root);
                    }
                    free(json);
                }
            }
            s_session_up = true;
            break;
        }

        case EVENT_SESSION_FINISHED:
        case EVENT_SESSION_FAILED:
            syslog(LOG_INFO, "[%s] session ended (event %u)\n",
                TAG, event_id);
            s_session_up = false;
            s_running = false;
            break;

        case EVENT_ASR_INFO:
            /* User started speaking. */
            break;

        case EVENT_ASR_RESPONSE:
            handle_asr_response(payload, plen);
            break;

        case EVENT_ASR_ENDED:
            handle_asr_ended();
            break;

        case EVENT_CHAT_RESPONSE:
            /* Hybrid orchestration: cache the server's chat text so
             * the E2E fast path can play it back as the agent reply
             * when no tool was called.  In plain bridge mode (hybrid
             * off) this event is still ignored. */
            if (s_hybrid == E2E_HYBRID_PENDING && plen > 0) {
                char* json = strndup((const char*)payload, plen);
                if (json) {
                    cJSON* root = cJSON_Parse(json);
                    if (root) {
                        const char* txt = NULL;
                        cJSON* tj = cJSON_GetObjectItem(root, "text");
                        if (tj && cJSON_IsString(tj)) {
                            txt = tj->valuestring;
                        } else {
                            /* Some servers nest under delta/message */
                            cJSON* dj = cJSON_GetObjectItem(root, "delta");
                            if (!dj) dj = cJSON_GetObjectItem(root, "message");
                            if (dj) {
                                cJSON* ct = cJSON_GetObjectItem(dj, "content");
                                if (ct && cJSON_IsString(ct))
                                    txt = ct->valuestring;
                            }
                        }
                        if (txt && txt[0]) {
                            pthread_mutex_lock(&s_reply_lock);
                            size_t cur = strlen(s_server_reply);
                            size_t add = strlen(txt);
                            if (cur + add + 1 < sizeof(s_server_reply)) {
                                memcpy(s_server_reply + cur, txt, add + 1);
                            }
                            pthread_mutex_unlock(&s_reply_lock);
                        }
                        cJSON_Delete(root);
                    }
                    free(json);
                }
            }
            break;

        case EVENT_CHAT_ENDED:
            /* Bridge mode: the server side LLM never drives anything.
             * ai_agent owns skills, tools, the system prompt and the
             * long/short term memory, so this event is informational. */
            break;

        case EVENT_TTS_SENTENCE_START:
        case EVENT_TTS_SENTENCE_END:
            break;

        case EVENT_TTS_RESPONSE:
            if (plen > 0) {
                /* Bridge-mode downlink gate:
                 *   - ASR_DONE (no text sent yet): accept server's
                 *     self-chat audio as the reply.
                 *   - TTS_STREAMING: accept all audio.  The server
                 *     sends its self-chat AND our ChatTTSText audio
                 *     in a single continuous stream (one TTSEnded at
                 *     the end).  Trying to drop the self-chat
                 *     segment also drops our text, which is why we
                 *     previously saw only 26KB of audio. */
                bool accept = false;
                if (!s_pcm_abort) {
                    if (s_turn == E2E_TURN_ASR_DONE
                        || s_turn == E2E_TURN_TTS_STREAMING) {
                        accept = true;
                    }
                }
                if (accept) {
                    size_t wrote = ring_write(&s_pcm_ring, payload, plen);
                    if (s_our_text_sent) s_got_our_pcm = true;
                    if (wrote < plen) {
                        /* Consumer is behind.  Wait for it to catch
                         * up, but give up after a bounded timeout so
                         * recv_thread does not stall here and miss
                         * the TTSEnded frame that signals stream
                         * completion.  Dropping the tail of one
                         * chunk is preferable to blocking the whole
                         * receive loop. */
                        size_t off = wrote;
                        uint32_t t0 = now_ms();
                        while (off < plen && s_running && !s_pcm_abort) {
                            if ((int32_t)(now_ms() - t0) >= 500) {
                                s_pcm_dropped += (plen - off);
                                break;
                            }
                            usleep(5000);
                            off += ring_write(&s_pcm_ring, payload + off,
                                plen - off);
                        }
                    }
                }
            }
            break;

        case EVENT_TTS_ENDED:
            if (s_pcm_dropped > 0) {
                syslog(LOG_WARNING, "[%s] dropped %zu bytes of server TTS\n",
                    TAG, s_pcm_dropped);
                s_pcm_dropped = 0;
            }
            /* Hybrid PENDING: server self-chat just finished.  Mark
             * EOS so pop_pcm will eventually return is_last=1 once
             * the buffered audio is drained by route_e2e().  Do NOT
             * transition s_turn here — the routing decision happens
             * later in volc_e2e_hybrid_route_{e2e,agent}(). */
            if (s_hybrid == E2E_HYBRID_PENDING) {
                syslog(LOG_INFO,
                    "[%s] TTSEnded (hybrid PENDING, buffered %zu bytes)\n",
                    TAG, ring_count(&s_pcm_ring));
                s_pcm_eos = true;
                s_server_ended = true;
                break;
            }
            if (s_turn == E2E_TURN_ASR_DONE) {
                /* Trivial-chat path: no own text expected.  Server's
                 * self-chat is the reply; transition to DONE. */
                syslog(LOG_INFO,
                    "[%s] TTSEnded (trivial-chat, ASR_DONE)\n", TAG);
                s_pcm_eos = true;
                s_turn = E2E_TURN_DONE;
            } else if (s_turn == E2E_TURN_TTS_STREAMING) {
                if (!s_our_text_sent) {
                    /* We never sent text; server self-chat is the
                     * reply.  Transition. */
                    syslog(LOG_INFO,
                        "[%s] TTSEnded (trivial-chat, streaming)\n",
                        TAG);
                    s_pcm_eos = true;
                    s_turn = E2E_TURN_DONE;
                } else if (s_got_our_pcm) {
                    /* Our audio has already been flowing; this
                     * TTSEnded closes our stream. */
                    syslog(LOG_INFO, "[%s] TTSEnded (ours)\n", TAG);
                    s_pcm_eos = true;
                    s_turn = E2E_TURN_DONE;
                } else if (s_server_ended) {
                    /* Server's self-chat ended earlier (first
                     * TTSEnded); this second TTSEnded closes our
                     * synthesis.  Our PCM may not have flowed (e.g.
                     * server didn't synthesize our text); transition
                     * so the consumer can unblock. */
                    syslog(LOG_INFO,
                        "[%s] TTSEnded (ours, no PCM)\n", TAG);
                    s_pcm_eos = true;
                    s_turn = E2E_TURN_DONE;
                } else {
                    /* First TTSEnded in STREAMING: the server's
                     * self-chat just finished.  Our audio has not
                     * yet started, so mark server_ended and keep
                     * the state.  The next PCM chunk is ours. */
                    syslog(LOG_INFO,
                        "[%s] TTSEnded (server self-chat)\n", TAG);
                    s_server_ended = true;
                }
            } else {
                syslog(LOG_INFO,
                    "[%s] TTSEnded (late, turn=%d)\n",
                    TAG, (int)s_turn);
            }
            break;

        case EVENT_DIALOG_ERROR:
            handle_dialog_error(payload, plen);
            /* The server terminated the dialog session (e.g. 45000003
             * "Abnormal silence audio" after a long mic mute) but keeps
             * the TCP/WS link open.  Without clearing the state,
             * s_session_up stays true: volc_e2e_start() then reports
             * the session alive, every uplink frame is silently
             * dropped by the server, ASR never returns and wake words
             * stop matching.  Mark the session down so the next
             * volc_e2e_start() tears the link down and reconnects. */
            syslog(LOG_WARNING,
                "[%s] dialog error — session down, will reconnect\n",
                TAG);
            s_session_up = false;
            s_running = false;
            break;

        default:
            break;
        }
    }

    free(buf);
    s_running = false;
    s_session_up = false;
    /* Unblock any TTS consumer waiting on the PCM ring. */
    s_pcm_eos = true;
    syslog(LOG_INFO, "[%s] recv thread exit\n", TAG);
    return NULL;
}

/* ── Send thread (single writer for control + uplink audio) ───── */

static void* send_thread(void* arg)
{
    (void)arg;
    unsigned char chunk[E2E_UPLINK_CHUNK];

    while (s_running) {
        bool busy = false;

        e2e_ctrl_t c;
        while (s_running && ctrl_pop(&c)) {
            const char* body = c.json ? c.json : "{}";
            int ret = send_client_frame(c.event, s_session_id,
                (const unsigned char*)body, strlen(body));
            free(c.json);
            busy = true;
            if (ret != 0) {
                syslog(LOG_ERR, "[%s] send event %u failed: %d\n",
                    TAG, c.event, ret);
                s_running = false;
                break;
            }
        }

        while (s_running && ring_count(&s_audio_ring) >= E2E_UPLINK_CHUNK) {
            size_t n = ring_read(&s_audio_ring, chunk, sizeof(chunk));
            if (n == 0) break;
            int ret = send_audio_frame(chunk, n);
            busy = true;
            if (ret != 0) {
                syslog(LOG_ERR, "[%s] send audio failed: %d\n", TAG, ret);
                s_running = false;
                break;
            }
        }

        usleep(busy ? 2000 : 10000);
    }

    syslog(LOG_INFO, "[%s] send thread exit\n", TAG);
    return NULL;
}

/* ── Configuration ────────────────────────────────────────────── */

static void cfg_copy(const char* key, char* dst, size_t cap)
{
    char buf[192];
    buf[0] = '\0';
    if (claw_config_get(key, buf, sizeof(buf)) == 0 && buf[0] != '\0') {
        snprintf(dst, cap, "%s", buf);
    }
}

int volc_e2e_load_config(void)
{
    s_app_id[0] = '\0';
    s_token[0] = '\0';

    cfg_copy(AGENT_CFG_KEY_VOLC_APPKEY, s_app_id, sizeof(s_app_id));

    /* Same fallback chain as classic volc_asr: the voice service Access
     * Token lives in volc_token.  volc_api_key is the generic console
     * key used by other services, so it is only used as a last resort.
     * This matches the credential model where set_volc_asr writes the
     * speech service token to volc_token. */
    cfg_copy(AGENT_CFG_KEY_VOLC_TOKEN, s_token, sizeof(s_token));
    if (s_token[0] == '\0')
        cfg_copy(AGENT_CFG_KEY_VOLC_API_KEY, s_token, sizeof(s_token));

    snprintf(s_speaker, sizeof(s_speaker), "%s",
        AGENT_VOLC_E2E_DEFAULT_SPEAKER);
    cfg_copy(AGENT_CFG_KEY_VOLC_SPEAKER, s_speaker, sizeof(s_speaker));
    cfg_copy(AGENT_CFG_KEY_VOLC_E2E_SPEAKER, s_speaker, sizeof(s_speaker));

    if (s_app_id[0] == '\0' || s_token[0] == '\0') {
        syslog(LOG_ERR, "[%s] missing credentials "
                        "(set_volc_asr <appid> ... / set_volc_key <key>)\n",
            TAG);
        return -ENOKEY;
    }
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────── */

/* Caller must hold s_start_lock. */
static void teardown_locked(void)
{
    s_running = false;

    if (s_recv_started) {
        pthread_join(s_recv_tid, NULL);
        s_recv_started = false;
    }
    if (s_send_started) {
        pthread_join(s_send_tid, NULL);
        s_send_started = false;
    }

    ctrl_clear();

    if (s_tls_valid) {
        tls_free(&s_tls);
        s_tls_valid = false;
    }

    s_conn_up = false;
    s_session_up = false;
    s_session_id[0] = '\0';
    s_dialog_id[0] = '\0';
    s_turn = E2E_TURN_IDLE;
    s_arm_pending = false;
    s_pcm_eos = true;
    ring_clear(&s_audio_ring);
}

static int wait_flag(volatile bool* flag, uint32_t timeout_ms)
{
    uint32_t deadline = now_ms() + timeout_ms;
    while (!*flag) {
        if (!s_running) return -ECONNRESET;
        if ((int32_t)(now_ms() - deadline) >= 0) return -ETIMEDOUT;
        usleep(20000);
    }
    return 0;
}

int volc_e2e_start(void)
{
    int ret;

    pthread_mutex_lock(&s_start_lock);

    if (s_running) {
        ret = s_session_up ? 0 : -EAGAIN;
        pthread_mutex_unlock(&s_start_lock);
        return ret;
    }

    /* Reap a link that died since the last call. */
    if (s_recv_started || s_send_started || s_tls_valid) {
        teardown_locked();
    }

    if (s_next_retry_ms != 0
        && (int32_t)(now_ms() - s_next_retry_ms) < 0) {
        pthread_mutex_unlock(&s_start_lock);
        return -EAGAIN;
    }

    ret = volc_e2e_load_config();
    if (ret != 0) goto fail;

    if (!s_audio_ring.buf) {
        ret = ring_init(&s_audio_ring, E2E_AUDIO_RING_CAP);
        if (ret != 0) goto fail;
    }
    if (!s_pcm_ring.buf) {
        ret = ring_init(&s_pcm_ring, E2E_PCM_RING_CAP);
        if (ret != 0) goto fail;
    }
    ring_clear(&s_audio_ring);
    ring_clear(&s_pcm_ring);
    s_pcm_eos = false;
    s_pcm_abort = false;
    s_pcm_dropped = 0;
    s_turn = E2E_TURN_IDLE;
    s_arm_pending = false;

    ret = tls_connect(&s_tls, AGENT_VOLC_E2E_HOST, AGENT_VOLC_E2E_PORT);
    if (ret != 0) {
        tls_free(&s_tls);
        goto fail;
    }
    s_tls_valid = true;

    ret = ws_handshake(&s_tls);
    if (ret != 0) goto fail_tls;

    /* tls_read_all() honours s_running, so raise it before the threads
     * start touching the socket. */
    s_running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, E2E_THREAD_STACK);

    if (pthread_create(&s_recv_tid, &attr, recv_thread, NULL) != 0) {
        pthread_attr_destroy(&attr);
        ret = -EAGAIN;
        goto fail_threads;
    }
    s_recv_started = true;

    if (pthread_create(&s_send_tid, &attr, send_thread, NULL) != 0) {
        pthread_attr_destroy(&attr);
        ret = -EAGAIN;
        goto fail_threads;
    }
    s_send_started = true;
    pthread_attr_destroy(&attr);

    ret = send_start_connection();
    if (ret != 0) goto fail_threads;

    ret = wait_flag(&s_conn_up, 8000);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ConnectionStarted timeout\n", TAG);
        goto fail_threads;
    }

    generate_uuid(s_session_id, sizeof(s_session_id));
    ret = send_start_session();
    if (ret != 0) goto fail_threads;

    ret = wait_flag(&s_session_up, 8000);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] SessionStarted timeout\n", TAG);
        goto fail_threads;
    }

    s_next_retry_ms = 0;
    syslog(LOG_INFO, "[%s] session up (id=%s)\n", TAG, s_session_id);
    pthread_mutex_unlock(&s_start_lock);
    return 0;

fail_threads:
    teardown_locked();
    s_next_retry_ms = now_ms() + E2E_RECONNECT_BACKOFF_MS;
    pthread_mutex_unlock(&s_start_lock);
    return ret;

fail_tls:
    s_running = false;
    tls_free(&s_tls);
    s_tls_valid = false;
fail:
    s_next_retry_ms = now_ms() + E2E_RECONNECT_BACKOFF_MS;
    pthread_mutex_unlock(&s_start_lock);
    return ret;
}

void volc_e2e_stop(void)
{
    pthread_mutex_lock(&s_start_lock);

    if (s_tls_valid && s_running) {
        if (s_session_up) send_finish_session();
        send_finish_connection();

        /* Graceful shutdown: wait (≤500ms) for the server's
         * SessionFinished / ConnectionFinished reply before tearing down.
         * Closing the socket while the server is still processing the
         * Finish* events leaves its session "dirty"; the next
         * StartSession (watch mode, same app_id) then gets dropped with a
         * WS 1002 protocol error.  The recv thread sets s_running=false on
         * those replies, so this loop exits early once they arrive. */
        for (int i = 0; i < 50 && s_running; i++) {
            usleep(10000);
        }
    }
    teardown_locked();
    s_next_retry_ms = 0;

    pthread_mutex_unlock(&s_start_lock);
}

bool volc_e2e_is_up(void)
{
    return s_running && s_session_up;
}

/* ── Per-turn control ─────────────────────────────────────────── */

static void turn_arm_now(void)
{
    /* Close a ChatTTSText stream that was claimed but never finished
     * (LLM produced nothing, speak() failed, ...) so the server is not
     * left waiting for the end frame of the previous turn. */
    if (s_turn >= E2E_TURN_TTS_CLAIMED && s_turn < E2E_TURN_DONE) {
        char* json = build_chat_tts_text(false, NULL, true);
        if (json) ctrl_push(EVENT_CHAT_TTS_TEXT, json);
    }

    ring_clear(&s_audio_ring);
    ring_clear(&s_pcm_ring);
    s_pcm_eos = false;
    s_pcm_abort = false;
    s_pcm_dropped = 0;
    s_uplink_drop_bytes = 0;
    s_uplink_stall_since = 0;
    s_our_text_open = false;
    s_our_text_sent = false;
    s_got_our_pcm = false;
    s_server_ended = false;

    /* Reset hybrid routing state for the new turn.  s_hybrid_enabled
     * is preserved (it's a config-level toggle, not per-turn). */
    s_hybrid = E2E_HYBRID_IDLE;
    pthread_mutex_lock(&s_reply_lock);
    s_server_reply[0] = '\0';
    pthread_mutex_unlock(&s_reply_lock);

    pthread_mutex_lock(&s_asr_lock);
    s_asr_text[0] = '\0';
    s_asr_final = false;
    pthread_mutex_unlock(&s_asr_lock);

    s_arm_pending = false;
    s_turn = E2E_TURN_LISTENING;
}

void volc_e2e_arm_turn(void)
{
    /* voice_channel.c pre-opens the next ASR stream ~2s before the
     * current reply finishes playing.  Resetting right here would wipe
     * the PCM the player is still draining, so postpone the reset until
     * the first uplink chunk of the new turn actually shows up. */
    if (s_turn == E2E_TURN_TTS_STREAMING) {
        s_arm_pending = true;
        return;
    }
    turn_arm_now();
}

void volc_e2e_end_turn(void)
{
    /* Stop the uplink for this turn.  When ASREnded already claimed the
     * TTS channel the turn state must survive: the reply is on its way
     * and volc_e2e_tts_text() has to continue that same stream. */
    ring_clear(&s_audio_ring);
    if (s_turn == E2E_TURN_LISTENING) {
        s_turn = E2E_TURN_DONE;
    }
}

/* ── ASR ──────────────────────────────────────────────────────── */

int volc_e2e_push_audio(const unsigned char* pcm, size_t len)
{
    if (!pcm || len == 0) return 0;
    if (!s_running || !s_session_up) return -ENOTCONN;

    if (s_turn != E2E_TURN_LISTENING) {
        if (!s_arm_pending || s_turn == E2E_TURN_TTS_STREAMING) {
            /* The normal case here is s_turn == ASR_DONE — the tail audio
             * arriving right after ASREnded.  That is expected and stays
             * silent.  Only log when the turn is stuck in an unexpected
             * state (TTS_CLAIMED / DONE), which would swallow the whole
             * utterance and present as "no response". */
            if (s_turn != E2E_TURN_ASR_DONE) {
                static uint32_t s_last_gate_drop_ms;
                uint32_t now = now_ms();
                if ((int32_t)(now - s_last_gate_drop_ms) >= 2000
                    || s_last_gate_drop_ms == 0) {
                    syslog(LOG_WARNING,
                        "[%s] push_audio dropped: turn=%d (not listening)\n",
                        TAG, (int)s_turn);
                    s_last_gate_drop_ms = now;
                }
            }
            return 0;
        }
        /* Deferred arm from a pre-opened stream: the previous reply is
         * done, this chunk belongs to the new turn. */
        turn_arm_now();
    }

    size_t wrote = ring_write(&s_audio_ring, pcm, len);
    if (wrote < len) {
        /* Uplink is behind: the oldest tail was overwritten.  Throttle
         * the log (this can spam hundreds of lines during a send_thread
         * stall) but keep the first occurrence visible. */
        static uint32_t s_last_drop_log_ms;
        size_t lost = len - wrote;
        s_uplink_drop_bytes += lost;
        uint32_t now = now_ms();
        /* Stall self-heal: sustained drops mean the server stops
         * receiving audio and wake words stop matching (server-side
         * detection).  Same recovery path as EVENT_DIALOG_ERROR:
         * mark the session down; e2e_stream_open() calls
         * volc_e2e_start() every capture cycle (~15s), which tears
         * the link down and redials. */
        if (s_uplink_stall_since == 0) {
            s_uplink_stall_since = now;
        } else if ((int32_t)(now - s_uplink_stall_since)
                   > UPLINK_STALL_HEAL_MS) {
            syslog(LOG_ERR,
                "[%s] uplink stalled %ums (dropped %u bytes) — "
                "session down, will reconnect\n",
                TAG, (unsigned)(now - s_uplink_stall_since),
                (unsigned)s_uplink_drop_bytes);
            s_uplink_stall_since = 0;
            s_session_up = false;
            s_running = false;
            return -ENOTCONN;
        }
        if ((int32_t)(now - s_last_drop_log_ms) >= 2000
            || s_last_drop_log_ms == 0) {
            syslog(LOG_WARNING,
                "[%s] uplink ring full, dropped %zu bytes "
                "(+%u total)\n",
                TAG, lost, (unsigned)s_uplink_drop_bytes);
            s_last_drop_log_ms = now;
        }
    } else if (s_uplink_stall_since != 0) {
        /* Ring drained again — transient congestion, cancel the heal. */
        s_uplink_stall_since = 0;
    }
    return 0;
}

int volc_e2e_pop_asr(char* text_out, size_t cap)
{
    if (!text_out || cap == 0) return -EINVAL;

    if (!s_asr_final) {
        return (s_running && s_session_up) ? 0 : -ENOTCONN;
    }

    pthread_mutex_lock(&s_asr_lock);
    snprintf(text_out, cap, "%s", s_asr_text);
    s_asr_final = false;
    s_asr_text[0] = '\0';
    pthread_mutex_unlock(&s_asr_lock);
    return 1;
}

/* ── TTS ──────────────────────────────────────────────────────── */

int volc_e2e_tts_begin(void)
{
    if (!s_running || !s_session_up) return -ENOTCONN;

    /* Usually already claimed by the ASREnded handler. */
    if (s_turn >= E2E_TURN_TTS_CLAIMED) return 0;

    char* json = build_chat_tts_text(true, NULL, false);
    if (!json) return -ENOMEM;
    int ret = ctrl_push(EVENT_CHAT_TTS_TEXT, json);
    if (ret == 0) s_turn = E2E_TURN_TTS_CLAIMED;
    return ret;
}

int volc_e2e_tts_text(const char* utf8)
{
    if (!utf8 || !utf8[0]) return 0;
    if (!s_running || !s_session_up) return -ENOTCONN;

    /* Send the empty claim frame {start:true, content:""} FIRST.
     * The server needs this explicit signal to know "this turn's
     * reply will be supplied by the client"; without it, the server
     * treats our subsequent text as a continuation of its own
     * self-chat and NEVER synthesizes it separately.
     *
     * Combined frames like {start:true, content:"text"} do NOT work
     * — the logs show only 1 TTSEnded and ~30KB of "our" audio that
     * is actually the tail of the server's self-chat.
     *
     * Two-frame flow ({start:true, content:""} then {content:"text"})
     * produces 2 TTSEndeds: the server synthesizes our text as a
     * separate stream. */
    int ret = volc_e2e_tts_begin();
    if (ret != 0) return ret;

    if (s_turn < E2E_TURN_TTS_STREAMING) {
        /* Opening the gate.  Do NOT clear the PCM ring here: the
         * server's self-chat audio is already flowing and must be
         * played in full before our text's audio starts.  Clearing
         * the ring mid-stream would drop the self-chat and mix the
         * tail with our audio, producing a garbled output. */
        s_pcm_eos = false;
        s_pcm_abort = false;
        s_turn = E2E_TURN_TTS_STREAMING;
    }
    s_our_text_sent = true;
    s_our_text_open = true;

    char* json = build_chat_tts_text(false, utf8, false);
    if (!json) return -ENOMEM;
    return ctrl_push(EVENT_CHAT_TTS_TEXT, json);
}

int volc_e2e_tts_end(void)
{
    if (!s_running || !s_session_up) return -ENOTCONN;
    if (s_turn < E2E_TURN_TTS_CLAIMED) return 0;

    /* Close our text stream so late-arriving TTSResponse frames from
     * the server's own self-chat are no longer accepted as ours. */
    s_our_text_open = false;

    char* json = build_chat_tts_text(false, NULL, true);
    if (!json) return -ENOMEM;
    return ctrl_push(EVENT_CHAT_TTS_TEXT, json);
}

int volc_e2e_tts_pop_pcm(unsigned char* buf, size_t cap,
    size_t* out_len, int* is_last, int timeout_ms)
{
    if (!buf || cap == 0 || !out_len || !is_last) return -EINVAL;

    *out_len = 0;
    *is_last = 0;

    if (!s_pcm_ring.buf) return -ENOTCONN;

    uint32_t deadline = now_ms() + (uint32_t)(timeout_ms > 0 ? timeout_ms : 0);

    for (;;) {
        if (s_pcm_abort) return -ECANCELED;

        size_t n = ring_read(&s_pcm_ring, buf, cap);
        if (n > 0) {
            *out_len = n;
            if (s_pcm_eos && ring_count(&s_pcm_ring) == 0) *is_last = 1;
            return 0;
        }

        if (s_pcm_eos) {
            *is_last = 1;
            return 0;
        }
        if (!s_running) return -ENOTCONN;
        if ((int32_t)(now_ms() - deadline) >= 0) return -ETIMEDOUT;
        usleep(5000);
    }
}

void volc_e2e_tts_abort(void)
{
    s_pcm_abort = true;
    s_pcm_eos = true;
    s_our_text_open = false;
    s_our_text_sent = false;
    s_got_our_pcm = false;
    s_server_ended = false;
    ring_clear(&s_pcm_ring);
    if (s_turn >= E2E_TURN_TTS_CLAIMED) s_turn = E2E_TURN_DONE;
}

/* ── Hybrid orchestration API ──────────────────────────────────── */

void volc_e2e_set_hybrid_enabled(bool on)
{
    s_hybrid_enabled = on;
    syslog(LOG_INFO, "[%s] hybrid_enabled=%d\n", TAG, (int)on);
}

bool volc_e2e_hybrid_enabled(void)
{
    return s_hybrid_enabled;
}

e2e_hybrid_t volc_e2e_hybrid_get(void)
{
    return s_hybrid;
}

char* volc_e2e_hybrid_get_reply(void)
{
    char* reply = NULL;
    pthread_mutex_lock(&s_reply_lock);
    if (s_server_reply[0]) {
        reply = strdup(s_server_reply);
    }
    pthread_mutex_unlock(&s_reply_lock);
    return reply;
}

int volc_e2e_hybrid_route_e2e(void)
{
    if (s_hybrid != E2E_HYBRID_PENDING) {
        syslog(LOG_WARNING,
            "[%s] route_e2e: not in PENDING (hybrid=%d turn=%d)\n",
            TAG, (int)s_hybrid, (int)s_turn);
        return -EAGAIN;
    }

    /* Transition to E2E fast path:
     *   - Set s_hybrid = E2E so volc_e2e_tts knows to drain the ring
     *   - Keep s_pcm_abort = false so pop_pcm can read the ring
     *   - Keep s_pcm_eos as set by TTSEnded (pop_pcm uses it to
     *     return is_last=1 after draining)
     *   - Advance s_turn to TTS_STREAMING so pop_pcm's accept gate
     *     (already open in ASR_DONE) remains open */
    s_hybrid = E2E_HYBRID_E2E;
    s_turn = E2E_TURN_TTS_STREAMING;

    size_t buffered = ring_count(&s_pcm_ring);
    syslog(LOG_INFO,
        "[%s] hybrid_route=e2e (buffered %zu bytes, eos=%d)\n",
        TAG, buffered, (int)s_pcm_eos);

    /* If no audio at all (server silent), mark abort so consumer
     * does not block on pop_pcm waiting for data that will never
     * arrive.  The reply will fall back to the error path. */
    if (buffered == 0 && s_pcm_eos) {
        syslog(LOG_WARNING,
            "[%s] hybrid_route=e2e: no server audio available\n", TAG);
        return 0;  /* Caller will pop_pcm and get is_last=1 immediately */
    }
    return 0;
}

char* volc_e2e_hybrid_route_agent(void)
{
    if (s_hybrid != E2E_HYBRID_PENDING) {
        syslog(LOG_WARNING,
            "[%s] route_agent: not in PENDING (hybrid=%d turn=%d)\n",
            TAG, (int)s_hybrid, (int)s_turn);
        return NULL;
    }

    /* Grab the server's cached reply BEFORE clearing state (the
     * mutex is needed because recv_thread may still be appending
     * from a late ChatResponse). */
    char* reply = NULL;
    pthread_mutex_lock(&s_reply_lock);
    if (s_server_reply[0]) {
        reply = strdup(s_server_reply);
    }
    pthread_mutex_unlock(&s_reply_lock);

    /* Discard the buffered server audio: the agent path will
     * synthesize the agent's own text via volc_tts_ws. */
    ring_clear(&s_pcm_ring);
    s_pcm_eos = true;
    s_pcm_abort = true;

    /* Claim and immediately close the TTS channel so the server stops
     * its self-chat NOW (only when it is still self-chatting).  Without
     * this the server keeps streaming audio we discard, and its late
     * frames / a confused turn state break the next turn (uplink ring
     * fills, server closes the connection). */
    if (s_turn < E2E_TURN_TTS_CLAIMED) {
        volc_e2e_tts_begin();
        volc_e2e_tts_end();
    }

    /* Advance s_turn so late TTSEnded frames are categorized as
     * "late" and ignored by the state machine. */
    s_turn = E2E_TURN_DONE;
    s_hybrid = E2E_HYBRID_AGENT;

    syslog(LOG_INFO,
        "[%s] hybrid_route=agent (reply_cached=%d)\n",
        TAG, reply != NULL);
    return reply;
}

/* ── Session memory sync support ─────────────────────────────── */

char* volc_e2e_get_server_reply(void)
{
    pthread_mutex_lock(&s_reply_lock);
    char* reply = s_server_reply[0] ? strdup(s_server_reply) : NULL;
    pthread_mutex_unlock(&s_reply_lock);
    return reply;
}

/* ── Drift tracking: auto-restart E2E session ────────────────── */

static int s_agent_path_streak = 0;
#define E2E_DRIFT_RESTART_THRESHOLD 3

void volc_e2e_note_agent_path(void)
{
    s_agent_path_streak++;
    if (s_agent_path_streak >= E2E_DRIFT_RESTART_THRESHOLD) {
        syslog(LOG_INFO,
            "[%s] drift restart: %d consecutive agent paths, "
            "restarting E2E session\n",
            TAG, s_agent_path_streak);

        /* FinishSession + StartSession to clear the server-side
         * conversation context (ghost replies from discarded agent
         * path turns).  The agent session (JSONL) is unaffected —
         * MiMo gets full history from session_mgr on next call. */
        if (s_session_up) {
            send_finish_session();
            usleep(100 * 1000);  /* 100ms grace for server processing */
            send_start_session();
        }
        s_agent_path_streak = 0;
    }
}

void volc_e2e_note_e2e_path(void)
{
    s_agent_path_streak = 0;
}

/* ── Classic-TTS forcing (proactive turns) ────────────────────── */

static volatile bool s_force_classic_tts = false;

void volc_e2e_set_classic_tts(bool on)
{
    s_force_classic_tts = on;
}

bool volc_e2e_classic_tts_forced(void)
{
    return s_force_classic_tts;
}

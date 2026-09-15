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

/*
 * Doubao TTS via WebSocket (V1 binary protocol).
 *
 * Protocol: wss://openspeech.bytedance.com/api/v1/tts/ws_binary
 * Flow:
 *   1. TLS connect + HTTP Upgrade to WebSocket
 *   2. Send full_client_request (JSON: app/user/audio/request)
 *   3. Receive audio_only_server_response frames (raw PCM)
 *   4. Last frame has sequence < 0
 *
 * Binary frame format: see volc_asr.c header comment.
 */

#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/volc_tts.h"
#include "voice/voice_tts.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "volc_tts_ws";

/* WebSocket endpoint — V1 binary protocol (V3 WS path unknown) */
#define TTS_WS_PATH "/api/v1/tts/ws_binary"

/* Volcengine binary protocol constants */
#define VOLC_PROTO_VER 0x11
#define VOLC_HDR_SIZE 8
#define VOLC_MSG_FULL_REQ 0x10
#define VOLC_MSG_AUDIO_RESP 0xB0 /* audio_only server response */
#define VOLC_MSG_FRONTEND 0xC0 /* frontend server response */
#define VOLC_MSG_ERROR 0xF0
#define VOLC_SER_JSON 0x10
#define VOLC_SER_JSON_GZ 0x11 /* JSON + gzip */
#define VOLC_SER_RAW 0x00

/* WebSocket constants — 64KB to accommodate large frames from
 * “Doubao TTS 2.0” voices (e.g. zh_female_vv_uranus_bigtts)
 * which can produce frames up to ~43KB per binary message). */
#define WS_BUF_SIZE (64 * 1024)
#define WS_MASK_KEY_LEN 4
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE 0x08
#define WS_OPCODE_PING 0x09
#define WS_OPCODE_PONG 0x0A
#define WS_FIN_BIT 0x80
#define WS_MASK_BIT 0x80

/* Credentials */
static char s_appid[32];
static char s_token[192];
static char s_cluster[32];
static char s_speaker[48];

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} tts_tls_ctx_t;

/* ── Entropy ─────────────────────────────────────────────────── */

static int tts_entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0) {
        return 0;
    }
    syslog(LOG_ERR, "[volc_tts] CRITICAL: No secure entropy source available\n");
    return -1;  /* Generic error - TLS handshake will fail safely */
}

/* ── TLS connect / free ──────────────────────────────────────── */

static void tts_tls_free(tts_tls_ctx_t* ctx);

/* Bounded TCP connect.  NuttX's blocking connect() waits on a
 * semaphore with an UINT_MAX timeout (net/tcp/tcp_connect.c) —
 * SO_SNDTIMEO does NOT bound it — so a SYN-blackholed path stalls
 * the caller for the full SYN retry budget (~33s) or longer, BEFORE
 * the "Handshake start" log even prints.  Field log 2026-09-08: a
 * TTS attempt sat silent for 33s then failed -111, and a later
 * synthesize never returned at all — the whole voice channel froze
 * and wake words went unanswered.  Non-blocking connect + poll()
 * bounds it (TCP_CONNECTED -> POLLOUT, refusal -> POLLERR|POLLHUP
 * with SO_ERROR set). */
#define TTS_CONNECT_TIMEOUT_MS 8000

static int tts_net_connect_bounded(mbedtls_net_context* net,
    const char* host, const char* port)
{
    struct addrinfo hints;
    struct addrinfo* addrs = NULL;
    struct addrinfo* cur;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &addrs) != 0 || addrs == NULL) {
        syslog(LOG_ERR, "[%s] dns %s failed\n", TAG, host);
        return -EHOSTUNREACH;
    }

    for (cur = addrs; cur != NULL; cur = cur->ai_next) {
        int flags;
        struct pollfd pfd;
        int pr;
        int soerr = 0;
        socklen_t slen = sizeof(soerr);

        fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (fd < 0) {
            continue;
        }

        flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        if (connect(fd, cur->ai_addr, cur->ai_addrlen) == 0) {
            fcntl(fd, F_SETFL, flags); /* back to blocking */
            break;
        }

        if (errno == EINPROGRESS) {
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;

            pr = poll(&pfd, 1, TTS_CONNECT_TIMEOUT_MS);
            if (pr > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                     &soerr, &slen) == 0 && soerr == 0) {
                fcntl(fd, F_SETFL, flags); /* back to blocking */
                break;
            }

            if (pr == 0) {
                syslog(LOG_ERR, "[%s] connect %s timeout after %dms\n",
                    TAG, host, TTS_CONNECT_TIMEOUT_MS);
            } else {
                syslog(LOG_ERR, "[%s] connect %s: SO_ERROR=%d\n",
                    TAG, host, soerr ? soerr : errno);
            }
        } else {
            syslog(LOG_ERR, "[%s] connect %s: errno=%d\n",
                TAG, host, errno);
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(addrs);

    if (fd < 0) {
        return -ECONNREFUSED;
    }

    net->fd = fd;
    return 0;
}

static int tts_tls_connect(tts_tls_ctx_t* ctx, const char* host,
    const char* port)
{
    int ret;
    struct timespec hs_t0;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, tts_entropy_func, NULL,
        (const unsigned char*)"volc_tts_ws", 11);
    if (ret != 0) {
        goto fail;
    }

    if (http_proxy_is_enabled()) {
        int tunnel_fd = proxy_open_tunnel(host, atoi(port), 30000);

        if (tunnel_fd < 0) {
            ret = -ECONNREFUSED;
            goto fail;
        }

        ctx->net.fd = tunnel_fd;
    } else {
        ret = tts_net_connect_bounded(&ctx->net, host, port);
        if (ret != 0) {
            goto fail;
        }
    }

    mbedtls_net_set_block(&ctx->net);

    if (ctx->net.fd >= 0) {
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };

        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        /* Bound send() as well.  Without SO_SNDTIMEO a full TCP send
         * buffer (WiFi stall / IOB pool exhaustion) blocks
         * mbedtls_ssl_write() forever and freezes the caller. */
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    ret = mbedtls_ssl_config_defaults(&ctx->cfg, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ret = -EIO;
        goto fail;
    }

    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
    if (ret != 0) {
        ret = -EIO;
        goto fail;
    }

    mbedtls_ssl_set_hostname(&ctx->ssl, host);
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net, mbedtls_net_send, mbedtls_net_recv,
        NULL);

    syslog(LOG_INFO, "[%s] Handshake start: %s:%s\n", TAG, host, port);

    clock_gettime(CLOCK_MONOTONIC, &hs_t0);
    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
            ret = -EIO;
            goto fail;
        }

        /* WANT_READ/WANT_WRITE here usually means the 10s socket
         * timeout fired mid-handshake.  Retry a little (transient
         * stall), but never forever — a peer that parks the handshake
         * would otherwise spin this loop indefinitely. */
        {
            struct timespec hs_t1;

            clock_gettime(CLOCK_MONOTONIC, &hs_t1);
            if (hs_t1.tv_sec - hs_t0.tv_sec >= 15) {
                syslog(LOG_ERR, "[%s] handshake stalled >15s, giving up\n",
                    TAG);
                ret = -ETIMEDOUT;
                goto fail;
            }
        }
    }

    syslog(LOG_INFO, "[%s] TLS connected (fd=%d)\n", TAG, ctx->net.fd);
    return 0;

fail:
    tts_tls_free(ctx);
    return ret;
}

static void tts_tls_free(tts_tls_ctx_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

/* ── TLS I/O helpers ─────────────────────────────────────────── */

static int tls_write_all(tts_tls_ctx_t* ctx, const unsigned char* buf,
    size_t len)
{
    size_t written = 0;
    struct timespec t0;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (written < len) {
        int ret;

        errno = 0;
        ret = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);

        if (ret > 0) {
            written += (size_t)ret;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE
            || ret == MBEDTLS_ERR_SSL_WANT_READ) {
            /* Blocking socket + SO_SNDTIMEO: WANT_WRITE means the send
             * buffer stayed full until the timeout fired.  Bound the
             * total retry budget instead of spinning forever.
             * WANT_READ can also surface here (renegotiation) and must
             * be retried, not treated as fatal. */
            struct timespec t1;

            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (t1.tv_sec - t0.tv_sec >= 6) {
                syslog(LOG_ERR, "[%s] ssl_write stalled >6s, giving up "
                    "(%zu/%zu bytes)\n", TAG, written, len);
                return -ETIMEDOUT;
            }
        } else {
            /* Keep the raw mbedtls code + errno: -0x004E =
             * NET_SEND_FAILED, and errno tells apart peer RST
             * (ECONNRESET=104) from local IOB exhaustion (ENOMEM=12)
             * and closed/stolen fd (EBADF=9). */
            syslog(LOG_ERR, "[%s] ssl_write: -0x%04x errno=%d "
                "(%zu/%zu bytes)\n",
                TAG, (unsigned)-ret, errno, written, len);
            return -EIO;
        }
    }

    return 0;
}

static int tls_read_all(tts_tls_ctx_t* ctx, unsigned char* buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        int ret;

        errno = 0;
        ret = mbedtls_ssl_read(&ctx->ssl, buf + got, len - got);

        if (ret > 0) {
            got += (size_t)ret;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -ECONNRESET;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            /* Distinguish SO_RCVTIMEO timeout from real I/O errors.
             * When the socket recv timeout fires, recv() returns -1
             * with errno EAGAIN/EWOULDBLOCK, and mbedtls maps that
             * to MBEDTLS_ERR_NET_RECV_FAILED (-0x004C).  Return a
             * distinct code so callers can treat timeout as normal
             * end-of-stream without masking real connection failures. */
            if (ret == -0x004C && (errno == EAGAIN || errno == EWOULDBLOCK
                                   || errno == ETIMEDOUT)) {
                return -ETIMEDOUT;
            }
            /* Keep the raw mbedtls code + errno: -0x004C =
             * NET_RECV_FAILED (errno 104 = peer RST, 9 = bad fd),
             * -0x7780/-0x7900 = TLS fatal alert, -0x7200 = bad record. */
            syslog(LOG_ERR, "[%s] ssl_read: -0x%04x errno=%d "
                "(%zu/%zu bytes)\n",
                TAG, (unsigned)-ret, errno, got, len);
            return -EIO;
        }
    }

    return 0;
}

/* ── WebSocket upgrade ───────────────────────────────────────── */

static int ws_upgrade(tts_tls_ctx_t* ctx, const char* host, const char* path,
    const char* token)
{
    unsigned char key_raw[16];
    unsigned char key_b64[32];
    size_t key_b64_len;

    tts_entropy_func(NULL, key_raw, sizeof(key_raw));
    mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_b64_len, key_raw,
        sizeof(key_raw));

    char req[768];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer;%s\r\n"
        "\r\n",
        path, host, (int)key_b64_len, key_b64, token);

    if (n <= 0 || n >= (int)sizeof(req)) {
        return -EOVERFLOW;
    }

    int ret = tls_write_all(ctx, (const unsigned char*)req, (size_t)n);

    if (ret != 0) {
        return ret;
    }

    char resp[1024];
    size_t rlen = 0;

    while (rlen < sizeof(resp) - 1) {
        errno = 0;
        int r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)resp + rlen,
            sizeof(resp) - 1 - rlen);

        if (r > 0) {
            rlen += (size_t)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) {
                break;
            }
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            syslog(LOG_ERR, "[%s] WS upgrade: peer closed (r=%d)\n", TAG, r);
            return -ECONNRESET;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] WS upgrade read: -0x%04x errno=%d\n",
                TAG, (unsigned)-r, errno);
            return -EIO;
        }
    }

    int status = 0;

    if (sscanf(resp, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        syslog(LOG_ERR, "[%s] WS upgrade failed: HTTP %d\n", TAG, status);
        return -EPROTO;
    }

    syslog(LOG_INFO, "[%s] WebSocket upgrade OK\n", TAG);
    return 0;
}

/* ── WebSocket frame send (client must mask) ─────────────────── */

static int ws_send_binary(tts_tls_ctx_t* ctx, const unsigned char* payload,
    size_t plen)
{
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

    tts_entropy_func(NULL, mask, WS_MASK_KEY_LEN);
    memcpy(hdr + hdr_len, mask, WS_MASK_KEY_LEN);
    hdr_len += WS_MASK_KEY_LEN;

    int ret = tls_write_all(ctx, hdr, hdr_len);

    if (ret != 0) {
        return ret;
    }

    unsigned char chunk[1024];
    size_t sent = 0;

    while (sent < plen) {
        size_t clen = plen - sent;

        if (clen > sizeof(chunk)) {
            clen = sizeof(chunk);
        }

        for (size_t i = 0; i < clen; i++) {
            chunk[i] = payload[sent + i] ^ mask[(sent + i) % 4];
        }

        ret = tls_write_all(ctx, chunk, clen);
        if (ret != 0) {
            return ret;
        }

        sent += clen;
    }

    return 0;
}

/* ── WebSocket PONG (reply to server PING) ───────────────────── */

/* An unanswered PING makes the gateway drop the connection, which is a
 * prime suspect for pre-opened connections parked while the LLM runs. */
static int ws_send_pong(tts_tls_ctx_t* ctx, const unsigned char* payload,
    size_t plen)
{
    unsigned char frame[2 + WS_MASK_KEY_LEN + 125];
    unsigned char mask[WS_MASK_KEY_LEN];

    if (plen > 125) {
        plen = 125; /* WS spec caps control frame payloads */
    }

    tts_entropy_func(NULL, mask, WS_MASK_KEY_LEN);

    frame[0] = WS_FIN_BIT | WS_OPCODE_PONG;
    frame[1] = WS_MASK_BIT | (unsigned char)plen;
    memcpy(frame + 2, mask, WS_MASK_KEY_LEN);

    for (size_t i = 0; i < plen; i++) {
        frame[2 + WS_MASK_KEY_LEN + i] = payload[i] ^ mask[i % 4];
    }

    return tls_write_all(ctx, frame, 2 + WS_MASK_KEY_LEN + plen);
}

/* ── WebSocket frame recv ────────────────────────────────────── */

static int ws_recv_frame(tts_tls_ctx_t* ctx, unsigned char* buf, size_t cap,
    size_t* out_len, int* out_opcode)
{
    unsigned char hdr[2];
    int ret = tls_read_all(ctx, hdr, 2);

    if (ret != 0) {
        return ret;
    }

    *out_opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] & WS_MASK_BIT) != 0;
    size_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        unsigned char ext[2];

        ret = tls_read_all(ctx, ext, 2);
        if (ret != 0) {
            return ret;
        }

        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];

        ret = tls_read_all(ctx, ext, 8);
        if (ret != 0) {
            return ret;
        }

        plen = ((size_t)ext[4] << 24) | ((size_t)ext[5] << 16) | ((size_t)ext[6] << 8) | ext[7];
    }

    unsigned char mask_key[WS_MASK_KEY_LEN];

    if (masked) {
        ret = tls_read_all(ctx, mask_key, WS_MASK_KEY_LEN);
        if (ret != 0) {
            return ret;
        }
    }

    if (plen > cap) {
        syslog(LOG_ERR,
            "[%s] WS frame too large: %zu (opcode=0x%x masked=%d cap=%zu)\n",
            TAG, plen, *out_opcode, masked, cap);
        return -EOVERFLOW;
    }

    if (plen > 0) {
        ret = tls_read_all(ctx, buf, plen);
        if (ret != 0) {
            return ret;
        }

        if (masked) {
            for (size_t i = 0; i < plen; i++) {
                buf[i] ^= mask_key[i % 4];
            }
        }
    }

    *out_len = plen;
    return 0;
}

/* ── Volcengine frame send ───────────────────────────────────── */

static int send_volc_frame(tts_tls_ctx_t* ctx, unsigned char msg_type,
    unsigned char serialization,
    const unsigned char* payload, size_t plen)
{
    unsigned char* frame = malloc(VOLC_HDR_SIZE + plen);

    if (!frame) {
        return -ENOMEM;
    }

    frame[0] = VOLC_PROTO_VER;
    frame[1] = msg_type;
    frame[2] = serialization;
    frame[3] = 0x00;
    frame[4] = (unsigned char)((plen >> 24) & 0xFF);
    frame[5] = (unsigned char)((plen >> 16) & 0xFF);
    frame[6] = (unsigned char)((plen >> 8) & 0xFF);
    frame[7] = (unsigned char)(plen & 0xFF);

    if (plen > 0) {
        memcpy(frame + VOLC_HDR_SIZE, payload, plen);
    }

    int ret = ws_send_binary(ctx, frame, VOLC_HDR_SIZE + plen);

    free(frame);
    return ret;
}

/* ── Build and send TTS full_client_request (V1 format) ─────── */

static int send_tts_request(tts_tls_ctx_t* ctx, const char* text)
{
    cJSON* root = cJSON_CreateObject();

    if (!root) {
        return -ENOMEM;
    }

    /* app */
    cJSON* app = cJSON_AddObjectToObject(root, "app");

    cJSON_AddStringToObject(app, "appid", s_appid);
    cJSON_AddStringToObject(app, "token", s_token);
    cJSON_AddStringToObject(app, "cluster", s_cluster);

    /* user */
    cJSON* user = cJSON_AddObjectToObject(root, "user");

    cJSON_AddStringToObject(user, "uid", "agent");

    /* audio */
    cJSON* audio = cJSON_AddObjectToObject(root, "audio");

    cJSON_AddStringToObject(audio, "voice_type", s_speaker);
    cJSON_AddStringToObject(audio, "encoding", "pcm");
    cJSON_AddNumberToObject(audio, "sample_rate", AGENT_TTS_WS_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "speed_ratio", 1.0);
    cJSON_AddNumberToObject(audio, "volume_ratio", 2.0);

    /* request */
    cJSON* req = cJSON_AddObjectToObject(root, "request");

    cJSON_AddStringToObject(req, "reqid", "agent-tts");
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "text_type", "plain");
    cJSON_AddStringToObject(req, "operation", "submit");

    char* json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (!json_str) {
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] TTS request: text=%zu bytes\n",
        TAG, strlen(text));

    int ret = send_volc_frame(ctx, VOLC_MSG_FULL_REQ, VOLC_SER_JSON,
        (const unsigned char*)json_str, strlen(json_str));
    free(json_str);
    return ret;
}

/* ── Receive audio responses ─────────────────────────────────── */

static int recv_tts_audio(tts_tls_ctx_t* ctx, volc_tts_chunk_cb cb,
    void* user_data)
{
    unsigned char* buf = malloc(WS_BUF_SIZE);

    if (!buf) {
        return -ENOMEM;
    }

    /* Recv timeout strategy:
     * - First chunk: keep the handshake timeout (10s) since TTS synthesis
     *   latency varies with text length and server load (200ms–2s typical).
     * - After first chunk: tighten to 500ms per recv.  To tolerate
     *   transient network stalls without silently truncating audio,
     *   allow up to 4 consecutive timeouts (~2s total) before
     *   declaring end-of-stream.  A single stall just retries.
     *   (Was 2: on a degraded WiFi link the tail of the audio was
     *   cut off mid-sentence — "话没说完".)
     * The initial 10s timeout was set by tts_tls_connect(), so we only
     * need to tighten it after the first audio chunk arrives. */

#define TTS_MAX_CONSECUTIVE_TIMEOUTS 4

    int chunks = 0;
    int consecutive_timeouts = 0;
    int err = 0;
    int opcode = 0;

    while (1) {
        size_t flen;
        int ret = ws_recv_frame(ctx, buf, WS_BUF_SIZE, &flen, &opcode);

        if (ret != 0) {
            /* Timeout after audio started: retry up to N times to
             * tolerate transient stalls.  Only declare EOF after
             * consecutive timeouts exceed the threshold (~2s). */
            if (chunks > 0 && ret == -ETIMEDOUT) {
                consecutive_timeouts++;
                if (consecutive_timeouts < TTS_MAX_CONSECUTIVE_TIMEOUTS) {
                    syslog(LOG_DEBUG,
                        "[%s] recv timeout %d/%d, retrying\n",
                        TAG, consecutive_timeouts,
                        TTS_MAX_CONSECUTIVE_TIMEOUTS);
                    continue;
                }
                syslog(LOG_INFO,
                    "[%s] recv ended after %d chunks "
                    "(%d consecutive timeouts)\n",
                    TAG, chunks, consecutive_timeouts);
                break;
            }

            /* Peer close after audio started is normal EOF. */
            if (chunks > 0 && ret == -ECONNRESET) {
                syslog(LOG_INFO, "[%s] recv ended after %d chunks (rc=%d)\n",
                    TAG, chunks, ret);
                break;
            }

            /* Frame overflow after audio started: the oversized frame
             * is lost, but keep the audio already received.  Treat as
             * graceful EOF so the caller gets is_last=1. */
            if (chunks > 0 && ret == -EOVERFLOW) {
                syslog(LOG_INFO,
                    "[%s] recv ended after %d chunks (overflow, keeping audio)\n",
                    TAG, chunks);
                break;
            }

            syslog(LOG_ERR, "[%s] recv error before any audio: %d (opcode=0x%x)\n",
                TAG, ret, opcode);
            err = ret;
            break;
        }

        /* Any successful frame resets the timeout counter. */
        consecutive_timeouts = 0;

        if (opcode == WS_OPCODE_CLOSE) {
            syslog(LOG_INFO, "[%s] server closed WS\n", TAG);
            break;
        }

        if (opcode == WS_OPCODE_PING) {
            syslog(LOG_INFO, "[%s] server PING (%zu bytes), replying PONG\n",
                TAG, flen);
            ws_send_pong(ctx, buf, flen);
            continue;
        }

        if (opcode == WS_OPCODE_PONG) {
            continue;
        }

        if (flen < 4) {
            syslog(LOG_DEBUG,
                "[%s] short WS frame: opcode=0x%x len=%zu\n",
                TAG, opcode, flen);
            continue;
        }

        unsigned char msg_type = buf[1] & 0xF0;
        unsigned char msg_flags = buf[1] & 0x0F;
        size_t volc_hdr_len = (size_t)(buf[0] & 0x0F) * 4;


        if (volc_hdr_len < 4 || flen < volc_hdr_len) {
            syslog(LOG_DEBUG,
                "[%s] non-volc frame: opcode=0x%x len=%zu volc_hdr_len=%zu "
                "msg_type=0x%x\n",
                TAG, opcode, flen, volc_hdr_len, msg_type);
            continue;
        }

        /* Error response */
        if (msg_type == VOLC_MSG_ERROR) {
            uint32_t code = 0;

            if (flen >= volc_hdr_len + 4) {
                code = ((uint32_t)buf[volc_hdr_len] << 24) | ((uint32_t)buf[volc_hdr_len + 1] << 16) | ((uint32_t)buf[volc_hdr_len + 2] << 8) | (uint32_t)buf[volc_hdr_len + 3];
            }

            syslog(LOG_ERR, "[%s] server error: %lu\n", TAG, (unsigned long)code);
            err = -EIO;
            break;
        }

        /* Frontend response (e.g. duration info) — signals end of audio
         * when it arrives after audio chunks have been received. */
        if (msg_type == VOLC_MSG_FRONTEND) {
            if (chunks > 0) {
                break; /* All audio delivered, frontend is the epilogue */
            }
            continue;
        }

        /* Audio-only response (0xB) */
        if (msg_type == VOLC_MSG_AUDIO_RESP) {
            /* flags: 0=ack(no audio), 1+=has audio data */
            if (msg_flags == 0) {
                continue; /* ACK, no audio data */
            }

            /* After volc header: 4-byte sequence (signed) + 4-byte payload_size */
            size_t audio_off = volc_hdr_len + 8;

            if (flen <= audio_off) {
                continue;
            }

            /* Extract sequence as signed 32-bit (big-endian).
             * Per Volcengine binary protocol: sequence < 0 means last frame. */
            int32_t seq = (int32_t)(
                ((uint32_t)buf[volc_hdr_len] << 24) |
                ((uint32_t)buf[volc_hdr_len + 1] << 16) |
                ((uint32_t)buf[volc_hdr_len + 2] << 8) |
                (uint32_t)buf[volc_hdr_len + 3]);

            unsigned char* pcm = buf + audio_off;
            size_t pcm_len = flen - audio_off;

            cb(pcm, pcm_len, 0, user_data);
            chunks++;

            /* After first chunk, tighten recv timeout so we detect
             * end-of-stream quickly (server may not send close frame). */
            if (chunks == 1 && ctx->net.fd >= 0) {
                struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
                if (setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO,
                        &tv, sizeof(tv)) < 0) {
                    syslog(LOG_WARNING,
                        "[%s] setsockopt SO_RCVTIMEO failed: %d\n",
                        TAG, errno);
                }
            }

            /* sequence < 0 = last audio frame */
            if (seq < 0) {
                break;
            }
        }
    }

    free(buf);

    if (chunks > 0 && err == 0) {
        cb(NULL, 0, 1, user_data);
    }

    syslog(LOG_INFO, "[%s] %d audio chunks delivered\n", TAG, chunks);

    if (chunks == 0 && err == 0) {
        return -EPROTO;
    }

    return err;
}

/* ── Init credentials ────────────────────────────────────────── */

static void tts_ws_init(void)
{
    memset(s_appid, 0, sizeof(s_appid));
    memset(s_token, 0, sizeof(s_token));
    memset(s_cluster, 0, sizeof(s_cluster));
    memset(s_speaker, 0, sizeof(s_speaker));

    claw_config_get(AGENT_CFG_KEY_VOLC_APPKEY, s_appid, sizeof(s_appid));
    claw_config_get(AGENT_CFG_KEY_VOLC_TOKEN, s_token, sizeof(s_token));

    /* Fallback: use volc_api_key as token if volc_token not configured */
    if (s_token[0] == '\0') {
        claw_config_get(AGENT_CFG_KEY_VOLC_API_KEY, s_token, sizeof(s_token));
    }

    /* Fallback: use a default appid if not configured */
    if (s_appid[0] == '\0') {
        strncpy(s_appid, "agent", sizeof(s_appid) - 1);
    }

    if (claw_config_get(AGENT_CFG_KEY_VOLC_CLUSTER, s_cluster,
            sizeof(s_cluster))
            != OK
        || s_cluster[0] == '\0') {
        strncpy(s_cluster, AGENT_VOICE_DEFAULT_CLUSTER, sizeof(s_cluster) - 1);
    }

    if (claw_config_get(AGENT_CFG_KEY_VOLC_SPEAKER, s_speaker,
            sizeof(s_speaker))
            != OK
        || s_speaker[0] == '\0') {
        strncpy(s_speaker, AGENT_VOICE_DEFAULT_SPEAKER, sizeof(s_speaker) - 1);
    }
}

/* ── Pre-open connection (TLS + WS upgrade) ─────────────────── */
/* Pre-establishes the TTS WebSocket connection during LLM processing
 * so that when text arrives, we can send it immediately without
 * waiting for the TLS handshake (~1.5s). */

static tts_tls_ctx_t* s_preopen_ctx;
static time_t s_preopen_time;
/* Monotonic stamp of the same event — used to log the exact idle gap
 * between "upgrade OK" and the first business frame. */
static struct timespec s_preopen_mono;
static pthread_mutex_t s_preopen_lock = PTHREAD_MUTEX_INITIALIZER;
/* True while synthesize_stream() runs on its own fresh connection.
 * Guarded by s_preopen_lock: a pre-open completing in that window
 * raced a synthesize (the LLM finished before the pre-open handshake
 * did) and must discard itself — parking it would hold two live
 * connections to the TTS host at once. */
static bool s_tts_fresh_busy = false;

/* Max age (seconds) for a pre-opened connection before it's discarded */
#define TTS_PREOPEN_MAX_AGE_SEC 30

int volc_tts_ws_preopen(void)
{
    tts_ws_init();

    if (s_appid[0] == '\0' || s_token[0] == '\0') {
        return -ENOENT;
    }

    /* Don't pre-open if one is already waiting */
    pthread_mutex_lock(&s_preopen_lock);
    if (s_preopen_ctx) {
        pthread_mutex_unlock(&s_preopen_lock);
        return 0;
    }
    pthread_mutex_unlock(&s_preopen_lock);

    tts_tls_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -ENOMEM;
    }

    int ret = tts_tls_connect(ctx, AGENT_DOUBAO_TTS_HOST, AGENT_DOUBAO_TTS_PORT);
    if (ret != 0) {
        free(ctx);
        return ret;
    }

    ret = ws_upgrade(ctx, AGENT_DOUBAO_TTS_HOST, TTS_WS_PATH, s_token);
    if (ret != 0) {
        tts_tls_free(ctx);
        free(ctx);
        return ret;
    }

    tts_tls_ctx_t* discard = NULL;

    pthread_mutex_lock(&s_preopen_lock);
    /* If another thread pre-opened while we were connecting, discard ours.
     * Also discard when a synthesize is already running on its own fresh
     * connection — parking this one next to it doubles our concurrent
     * connections to the TTS host (field log 2026-09-08: fd=8 parked
     * while fd=9 carried the request, immediately followed by server
     * error 21497431 and a stretch of refused/throttled connects). */
    discard = (s_preopen_ctx || s_tts_fresh_busy) ? ctx : NULL;
    if (!discard) {
        s_preopen_ctx = ctx;
        s_preopen_time = time(NULL);
        clock_gettime(CLOCK_MONOTONIC, &s_preopen_mono);
        syslog(LOG_INFO, "[%s] pre-opened TTS WS connection (fd=%d)\n",
            TAG, ctx->net.fd);
    } else if (!s_preopen_ctx) {
        syslog(LOG_INFO, "[%s] synthesize owns a fresh connection, "
            "discarding raced pre-open (fd=%d)\n", TAG, ctx->net.fd);
    }
    pthread_mutex_unlock(&s_preopen_lock);

    /* tts_tls_free() sends a close_notify — a network write that can
     * block on the socket timeout.  Keep it out of the lock. */
    if (discard) {
        tts_tls_free(discard);
        free(discard);
    }

    return 0;
}

void volc_tts_ws_cancel_preopen(void)
{
    tts_tls_ctx_t* ctx = NULL;

    pthread_mutex_lock(&s_preopen_lock);
    if (s_preopen_ctx) {
        ctx = s_preopen_ctx;
        s_preopen_ctx = NULL;
    }
    pthread_mutex_unlock(&s_preopen_lock);

    /* Free outside the lock: close_notify can block on the socket
     * timeout, and this lock sits on the synthesize hot path. */
    if (ctx) {
        tts_tls_free(ctx);
        free(ctx);
    }
}

/* ── Public API ──────────────────────────────────────────────── */

/* Streaming synthesize — used by voice_tts_speak_stream() */
static int volc_tts_ws_stream(const char* text,
    voice_tts_chunk_cb cb, void* user_data)
{
    return volc_tts_ws_synthesize_stream(text, cb, user_data);
}

static int volc_tts_ws_init(void)
{
    tts_ws_init();
    return (s_appid[0] && s_token[0]) ? 0 : -ENOENT;
}

/* Backend ops registration */
static const voice_tts_ops_t s_volc_tts_ws_ops = {
    .name = "volcengine_ws",
    .init = volc_tts_ws_init,
    .synthesize = NULL,  /* WS is streaming-only */
    .synthesize_stream = volc_tts_ws_stream,
    .deinit = NULL,
};

int volc_tts_ws_register(void)
{
    int rc = voice_tts_register(&s_volc_tts_ws_ops);
    syslog(LOG_INFO, "[%s] register: rc=%d\n", TAG, rc);
    return rc;
}

/* One complete TTS exchange over a brand-new connection:
 * TLS connect + WS upgrade + send request + receive audio.
 * Returns 0 on success (audio delivered via cb), negative errno otherwise. */
static int tts_ws_fresh_exchange(const char* text, volc_tts_chunk_cb cb,
    void* user_data)
{
    tts_tls_ctx_t ctx;

    int ret = tts_tls_connect(&ctx, AGENT_DOUBAO_TTS_HOST,
        AGENT_DOUBAO_TTS_PORT);
    if (ret != 0) {
        return ret;
    }

    ret = ws_upgrade(&ctx, AGENT_DOUBAO_TTS_HOST, TTS_WS_PATH, s_token);
    if (ret != 0) {
        tts_tls_free(&ctx);
        return ret;
    }

    ret = send_tts_request(&ctx, text);
    if (ret == 0) {
        ret = recv_tts_audio(&ctx, cb, user_data);
    }

    tts_tls_free(&ctx);
    return ret;
}

int volc_tts_ws_synthesize_stream(const char* text, volc_tts_chunk_cb cb,
    void* user_data)
{
    if (!text || !cb) {
        return -EINVAL;
    }

    syslog(LOG_INFO, "[%s] synthesize: text=\"%s\" (%zu bytes)\n",
        TAG, text, strlen(text));

    tts_ws_init();

    if (s_appid[0] == '\0' || s_token[0] == '\0') {
        syslog(LOG_ERR, "[%s] credentials not configured\n", TAG);
        return -ENOENT;
    }

    /* Use pre-opened connection if available and not stale */
    tts_tls_ctx_t* ctx = NULL;
    tts_tls_ctx_t* stale = NULL;
    bool used_preopen = false;
    time_t age = 0;
    struct timespec preopen_mono = { 0, 0 };

    pthread_mutex_lock(&s_preopen_lock);
    if (s_preopen_ctx) {
        age = time(NULL) - s_preopen_time;
        if (age <= TTS_PREOPEN_MAX_AGE_SEC) {
            ctx = s_preopen_ctx;
            s_preopen_ctx = NULL;
            used_preopen = true;
            preopen_mono = s_preopen_mono;
        } else {
            syslog(LOG_INFO, "[%s] pre-opened connection stale "
                "(%lds > %ds), discarding\n",
                TAG, (long)age, TTS_PREOPEN_MAX_AGE_SEC);
            stale = s_preopen_ctx;
            s_preopen_ctx = NULL;
        }
    }
    pthread_mutex_unlock(&s_preopen_lock);

    /* Free outside the lock: close_notify can block on the socket
     * timeout, and a lock held there stalls the next pre-open. */
    if (stale) {
        tts_tls_free(stale);
        free(stale);
    }

    /* 1. Fast path: try the pre-opened connection first (if we grabbed one).
     *    In practice the ws_binary server often closes the idle connection
     *    while we wait for the LLM, so this may fail — that's fine, we fall
     *    through to a fresh connection below. */
    if (used_preopen) {
        struct timespec now;
        long idle_ms;

        clock_gettime(CLOCK_MONOTONIC, &now);
        idle_ms = (now.tv_sec - preopen_mono.tv_sec) * 1000
            + (now.tv_nsec - preopen_mono.tv_nsec) / 1000000;

        syslog(LOG_INFO,
            "[%s] reusing pre-opened TTS WS connection (fd=%d idle=%ldms)\n",
            TAG, ctx->net.fd, idle_ms);

        int ret = send_tts_request(ctx, text);
        if (ret == 0) {
            ret = recv_tts_audio(ctx, cb, user_data);
        }
        tts_tls_free(ctx);
        free(ctx);

        if (ret == 0) {
            return 0;
        }
        syslog(LOG_WARNING,
            "[%s] pre-opened connection failed (%d), falling back to fresh\n",
            TAG, ret);
    }

    /* 2. Fresh connection with a small retry budget.  The server may
     *    transiently reset a just-established connection; retrying a few
     *    times prevents an occasional failure from dropping the whole
     *    response (which the user hears as silence).  All failures occur
     *    before any audio chunk is delivered, so retrying never produces
     *    duplicated audio. */
#define TTS_FRESH_MAX_ATTEMPTS 3
    int ret = -EIO;

    /* We're going fresh (slot empty, or the pre-opened connection
     * failed): flag it under the pre-open lock so a pre-open that
     * completes concurrently knows it raced us and discards itself,
     * instead of parking a second live connection next to ours. */
    pthread_mutex_lock(&s_preopen_lock);
    s_tts_fresh_busy = true;
    pthread_mutex_unlock(&s_preopen_lock);

    for (int attempt = 1; attempt <= TTS_FRESH_MAX_ATTEMPTS; attempt++) {
        ret = tts_ws_fresh_exchange(text, cb, user_data);
        if (ret == 0) {
            break;
        }
        syslog(LOG_WARNING, "[%s] fresh TTS attempt %d/%d failed (%d)\n",
            TAG, attempt, TTS_FRESH_MAX_ATTEMPTS, ret);
        /* Space the retries out: rapid reconnect bursts to the TTS
         * host are a plausible trigger for the server-side throttling
         * seen in the field, and each bounded attempt already costs
         * seconds. */
        if (attempt < TTS_FRESH_MAX_ATTEMPTS) {
            sleep(1);
        }
    }

    pthread_mutex_lock(&s_preopen_lock);
    s_tts_fresh_busy = false;
    pthread_mutex_unlock(&s_preopen_lock);

    return ret;
}

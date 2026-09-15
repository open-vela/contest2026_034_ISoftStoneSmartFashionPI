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

#include "infra/vela_tls.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"

#ifdef CONFIG_AI_AGENT_NET_RPMSG
#include "network/network_manager.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* POSIX networking */
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* mbedTLS */
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <fcntl.h>
#include <pthread.h>
#include <time.h>

static int simple_entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0) {
        return 0;
    }
    /* No fallback — cryptographic entropy is mandatory for TLS.
     * Returning an error forces the TLS handshake to fail safely
     * rather than proceeding with predictable key material. */
    syslog(LOG_ERR, "[vela_tls] CRITICAL: No secure entropy source available\n");
    return -1;  /* Generic error - TLS handshake will fail safely */
}

static const char* TAG = "vela_tls";

/* ── Chunked transfer decoding ───────────────────────────────── */

/**
 * Decode chunked transfer encoding in-place.
 * Format: <hex-size>\r
<data>\r
 ... 0\r
\r

 * Returns the decoded length.
 */
static size_t decode_chunked(char* buf, size_t len)
{
    char* src = buf;
    char* end = buf + len;
    char* dst = buf;

    while (src < end) {
        /* Find end of chunk-size line */
        char* crlf = (char*)memmem(src, (size_t)(end - src), "\r\n", 2);
        if (!crlf)
            break;

        /* Parse hex chunk size */
        char* endptr;
        long chunk_sz = strtol(src, &endptr, 16);

        /* Validate: endptr should reach the CRLF (skip spaces) */
        while (endptr < crlf && *endptr == ' ')
            endptr++;

        if (endptr != crlf || chunk_sz < 0 || chunk_sz > (long)(end - crlf - 2))
            break;  /* malformed or oversized chunk header */

        if (chunk_sz == 0)
            break;  /* final chunk */

        src = crlf + 2;  /* skip past chunk-size CRLF */

        /* Clamp to available data */
        if (src + chunk_sz > end)
            chunk_sz = (long)(end - src);

        memmove(dst, src, (size_t)chunk_sz);
        dst += chunk_sz;
        src += chunk_sz;

        /* Skip trailing CRLF after chunk data */
        if (src + 2 <= end && src[0] == '\r' && src[1] == '\n')
            src += 2;
    }

    return (size_t)(dst - buf);
}

/* ── TLS context ─────────────────────────────────────────────── */

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} tls_ctx_t;

static void tls_ctx_free(tls_ctx_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    network_release_resource();
#endif
}

/* ── Persistent connection pool (keep-alive) ─────────────────── */
/* Keeps one live TLS connection per host:port so repeated calls to
 * the same endpoint (Feishu REST, LLM API) skip the TLS handshake. */

#ifdef CONFIG_AI_AGENT_TLS_CONN_POOL_SIZE
#define CONN_POOL_SIZE CONFIG_AI_AGENT_TLS_CONN_POOL_SIZE
#else
#define CONN_POOL_SIZE 2
#endif

typedef struct {
    tls_ctx_t ctx;
    char host[128];
    char port[8];
    bool in_use; /* locked by a request */
    bool valid; /* connection is alive */
} conn_slot_t;

static conn_slot_t s_pool[CONN_POOL_SIZE];
static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* Acquire a slot for host:port.  Returns a locked, connected ctx or NULL. */
static conn_slot_t* pool_acquire(const char* host, const char* port)
{
    pthread_mutex_lock(&s_pool_lock);

    /* 1. Find an existing valid slot for this host:port */
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        conn_slot_t* s = &s_pool[i];
        if (s->valid && !s->in_use && strcmp(s->host, host) == 0 && strcmp(s->port, port) == 0) {
            s->in_use = true;
            pthread_mutex_unlock(&s_pool_lock);
            return s;
        }
    }

    /* 2. Find an empty slot */
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        if (!s_pool[i].valid && !s_pool[i].in_use) {
            s_pool[i].in_use = true;
            pthread_mutex_unlock(&s_pool_lock);
            return &s_pool[i];
        }
    }

    /* 3. Evict the first non-in-use slot */
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        if (!s_pool[i].in_use) {
            tls_ctx_free(&s_pool[i].ctx);
            s_pool[i].valid = false;
            s_pool[i].in_use = true;
            pthread_mutex_unlock(&s_pool_lock);
            return &s_pool[i];
        }
    }

    pthread_mutex_unlock(&s_pool_lock);
    return NULL; /* all slots busy — caller falls back to ephemeral */
}

/* Return a slot to the pool.  keep=true means the connection is still alive. */
static void pool_release(conn_slot_t* s, const char* host, const char* port, bool keep)
{
    pthread_mutex_lock(&s_pool_lock);
    if (keep) {
        strncpy(s->host, host, sizeof(s->host) - 1);
        strncpy(s->port, port, sizeof(s->port) - 1);
        s->valid = true;
        syslog(LOG_DEBUG, "[%s] Pool: kept connection to %s:%s\n",
            TAG, host, port);
    } else {
        tls_ctx_free(&s->ctx);
        s->valid = false;
        s->host[0] = '\0';
        syslog(LOG_DEBUG, "[%s] Pool: dropped connection to %s:%s\n",
            TAG, host, port);
    }
    s->in_use = false;
    pthread_mutex_unlock(&s_pool_lock);
}

void vela_tls_pool_cleanup(void)
{
    pthread_mutex_lock(&s_pool_lock);
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        if (s_pool[i].in_use) {
            syslog(LOG_WARNING,
                "[vela_tls] Pool slot %d still in use at shutdown, skipping\n", i);
            continue;
        }
        if (s_pool[i].valid) {
            tls_ctx_free(&s_pool[i].ctx);
            s_pool[i].valid = false;
        }
        s_pool[i].host[0] = '\0';
    }
    pthread_mutex_unlock(&s_pool_lock);
}

static int tls_ctx_connect(tls_ctx_t* ctx, const char* host, const char* port);

/* Non-blocking liveness probe on an owned (in_use) pooled slot.
 * Mirrors the drain probe in vela_https_request: a healthy idle
 * connection returns WANT_READ immediately; 0 / close_notify /
 * conn-reset means the server closed it while pooled. */
static bool slot_probe_alive(conn_slot_t* s)
{
    if (s->ctx.net.fd < 0) {
        return false;
    }

    /* Heap, not stack: avoid overflow on small threads */
    unsigned char* drain = malloc(2048);
    if (!drain) {
        return true; /* cannot tell — assume alive, let the next request probe */
    }

    int orig_flags = fcntl(s->ctx.net.fd, F_GETFL, 0);
    if (orig_flags >= 0) {
        fcntl(s->ctx.net.fd, F_SETFL, orig_flags | O_NONBLOCK);
    }

    int dr = -1;
    while ((dr = mbedtls_ssl_read(&s->ctx.ssl, drain, 2048)) > 0) {
        /* discard any leftover bytes so the next request starts clean */
    }

    if (orig_flags >= 0) {
        fcntl(s->ctx.net.fd, F_SETFL, orig_flags);
    }
    free(drain);

    return !(dr == 0
        || dr == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY
        || dr == MBEDTLS_ERR_NET_CONN_RESET);
}

/* Warm up the pool for host:port without sending a request.
 *
 * - A live slot for this host:port already exists (idle or owned by an
 *   in-flight request) → no-op, return 0.
 * - An idle-but-stale slot (server closed it between turns) → probe,
 *   then re-establish the TLS handshake into the same slot.
 * - No slot at all → connect into a FREE slot only.  Valid connections
 *   to other hosts are never evicted (unlike pool_acquire step 3) —
 *   preheat must not disturb the working set of live traffic.
 *
 * Returns 0 when a usable connection is in the pool, negative otherwise
 * (-1 = no free slot, VELA_TLS_ERR_CONNECT/-HANDSHAKE = connect failed).
 *
 * The ~3 s TLS handshake is the dominant cost of the first LLM round in
 * a voice turn; running this in the background lets the request path
 * log "Reusing pooled connection" instead of "New connection". */
int vela_tls_preconnect(const char* host, const char* port)
{
    if (!host || !port || host[0] == '\0') {
        return -1;
    }

    pthread_mutex_lock(&s_pool_lock);

    conn_slot_t* s = NULL;
    conn_slot_t* free_slot = NULL;
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        conn_slot_t* t = &s_pool[i];
        if (t->valid && strcmp(t->host, host) == 0
            && strcmp(t->port, port) == 0) {
            s = t;
            break;
        }
        if (!t->valid && !t->in_use && !free_slot) {
            free_slot = t;
        }
    }

    if (s && s->in_use) {
        /* A live request owns a connection to this host — it will be
         * released back to the pool, so the pool is already warm. */
        pthread_mutex_unlock(&s_pool_lock);
        return 0;
    }
    if (!s && !free_slot) {
        /* Pool holds only valid connections to other hosts — leave them
         * alone; the caller retries on its next cycle. */
        pthread_mutex_unlock(&s_pool_lock);
        return -1;
    }

    /* Claim the slot (existing one for this host, or a free one) */
    if (s) {
        s->in_use = true;
    } else {
        s = free_slot;
        s->in_use = true;
    }
    pthread_mutex_unlock(&s_pool_lock);

    if (s->valid) {
        /* Verify the idle connection is still alive before trusting it */
        if (slot_probe_alive(s)) {
            pool_release(s, host, port, true);
            return 0;
        }
        syslog(LOG_INFO,
            "[%s] Preconnect: pooled connection to %s:%s is stale\n",
            TAG, host, port);
        tls_ctx_free(&s->ctx);
        s->valid = false;
        /* slot stays owned (in_use) — reconnect into it below */
    }

    syslog(LOG_INFO, "[%s] Preconnect: warming %s:%s\n", TAG, host, port);
    int ret = tls_ctx_connect(&s->ctx, host, port);
    pool_release(s, host, port, ret == 0);
    return ret == 0 ? 0 : ret;
}

static int tls_ctx_connect(tls_ctx_t* ctx, const char* host, const char* port)
{
    int ret;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    /* Seed RNG directly with our robust function */
    const char* pers = "vela_tls";
    if ((ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, simple_entropy_func, NULL,
             (const unsigned char*)pers,
             strlen(pers)))
        != 0) {
        syslog(LOG_ERR, "[%s] ctr_drbg_seed ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    /* Check system time — crucial for TLS certificate validation. */
    time_t now = time(NULL);
    syslog(LOG_DEBUG, "[%s] Handshake start: Host=%s, UNIX=%ld\n", TAG, host, (long)now);

    if (now < 1704067200) { /* Jan 1 2024 */
        syslog(LOG_WARNING, "[%s] Clock too old, forcing to 2026\n", TAG);
        struct timespec ts = { .tv_sec = 1772275200, .tv_nsec = 0 };
        clock_settime(CLOCK_REALTIME, &ts);
    }

    /* TCP connect — use HTTP CONNECT proxy if configured */
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    {
        int res_ret = network_acquire_resource(network_get_connect_timeout() * 1000);
        if (res_ret != 0) {
            syslog(LOG_ERR, "[%s] Resource acquire failed: %d\n", TAG, res_ret);
            return VELA_TLS_ERR_CONNECT;
        }
    }
#endif

    if (http_proxy_is_enabled()) {
        int tunnel_fd = proxy_open_tunnel(host, atoi(port), 30000);
        if (tunnel_fd < 0) {
            syslog(LOG_ERR, "[%s] proxy tunnel to %s:%s failed\n",
                TAG, host, port);
            return VELA_TLS_ERR_CONNECT;
        }
        ctx->net.fd = tunnel_fd;
        syslog(LOG_INFO, "[%s] Using proxy tunnel fd=%d for %s:%s\n",
            TAG, tunnel_fd, host, port);
    } else {
        if ((ret = mbedtls_net_connect(&ctx->net, host, port,
                 MBEDTLS_NET_PROTO_TCP))
            != 0) {
            syslog(LOG_ERR, "[%s] net_connect %s:%s ret=0x%x\n",
                TAG, host, port, -ret);
            return VELA_TLS_ERR_CONNECT;
        }
    }

    /* Force blocking mode and set socket-level read timeout.
     * This avoids using select()/poll() inside mbedtls_net_recv_timeout
     * which returns MBEDTLS_ERR_NET_POLL_FAILED (-0x0047) on NuttX/QEMU.
     * Timeout is configured via AGENT_LLM_SOCKET_TIMEOUT_SEC in
     * agent_config.h.  LLM APIs (especially kimi with thinking mode)
     * can take over 60s for long responses. */
    mbedtls_net_set_block(&ctx->net);
    if (ctx->net.fd >= 0) {
#ifdef CONFIG_AI_AGENT_NET_RPMSG
        int read_timeout_sec = network_get_read_timeout();
        int send_timeout_sec = network_get_connect_timeout();
#else
        int read_timeout_sec = AGENT_LLM_SOCKET_TIMEOUT_SEC;
        int send_timeout_sec = AGENT_LLM_SOCKET_TIMEOUT_SEC;
#endif
        struct timeval tv = { .tv_sec = read_timeout_sec, .tv_usec = 0 };
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* Send timeout too: without it, a write to a stalled peer blocks
         * forever.  The E2E realtime uplink hit exactly this — the server
         * stopped reading, the send thread blocked on tls_write, and the
         * audio ring filled ("uplink ring full") until the server sent a
         * close frame.  A bounded send timeout lets the client detect the
         * broken connection and reconnect instead. */
        struct timeval ctv = { .tv_sec = send_timeout_sec, .tv_usec = 0 };
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
    }

    /* SSL config: client, TLS, default ciphersuites */
    if ((ret = mbedtls_ssl_config_defaults(&ctx->cfg,
             MBEDTLS_SSL_IS_CLIENT,
             MBEDTLS_SSL_TRANSPORT_STREAM,
             MBEDTLS_SSL_PRESET_DEFAULT))
        != 0) {
        syslog(LOG_ERR, "[%s] ssl_config_defaults ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    /* Pin the TLS version range to what is actually compiled in.
     * Setting max to MBEDTLS_SSL_VERSION_TLS1_3 when MBEDTLS_SSL_PROTO_TLS1_3
     * is not defined causes mbedtls_ssl_setup() to return BAD_CONFIG (-0x5e80).
     * Use TLS 1.3 as the ceiling only when the library was built with TLS 1.3
     * support; otherwise cap at TLS 1.2. */
    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

    /* Advertise ALPN so Cloudflare/OpenAI servers don't silently reject us.
     * Modern HTTPS servers send a fatal TLS alert or close the connection
     * when no ALPN extension is present in the ClientHello.
     * MBEDTLS_SSL_ALPN must be enabled in the build (it is). */
#if defined(MBEDTLS_SSL_ALPN)
    static const char * alpn_protos[] = { "http/1.1", NULL };
    if ((ret = mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, alpn_protos)) != 0) {
        syslog(LOG_WARNING, "[%s] Failed to set ALPN protocols: -0x%04x (non-fatal)\n", TAG, -ret);
    }
#endif

    /* Skip full chain verification — keeps portability without CA bundle */
    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    /* Read timeout is handled at the socket level via SO_RCVTIMEO,
     * not via mbedtls_ssl_conf_read_timeout + mbedtls_net_recv_timeout,
     * because select()/poll() inside mbedtls_net_recv_timeout fails on
     * NuttX/QEMU with MBEDTLS_ERR_NET_POLL_FAILED (-0x0047). */

    if ((ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg)) != 0) {
        syslog(LOG_ERR, "[%s] ssl_setup ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    if ((ret = mbedtls_ssl_set_hostname(&ctx->ssl, host)) != 0) {
        syslog(LOG_ERR, "[%s] ssl_set_hostname ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    /* Use blocking recv — no select()/poll().  The read
     * timeout is enforced by the SO_RCVTIMEO socket option set above. */
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
        mbedtls_net_send, mbedtls_net_recv, NULL);

    /* Handshake */
    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#if defined(MBEDTLS_ERROR_C)
            char err_buf[128];
            mbedtls_strerror(ret, err_buf, sizeof(err_buf));
            syslog(LOG_ERR, "[%s] ssl_handshake ret=-0x%04x: %s\n", TAG, -ret, err_buf);
#else
            syslog(LOG_ERR, "[%s] ssl_handshake ret=-0x%04x\n", TAG, -ret);
#endif
            /* Log if it was a fatal alert */
            if (ret == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
                syslog(LOG_ERR, "[%s] Server sent fatal alert message\n", TAG);
            }
            return VELA_TLS_ERR_HANDSHAKE;
        }
    }

    syslog(LOG_INFO, "[%s] Handshake OK: %s / %s\n", TAG, mbedtls_ssl_get_version(&ctx->ssl),
        mbedtls_ssl_get_ciphersuite(&ctx->ssl));

    return 0;
}

/* ── HTTP/1.1 framing ────────────────────────────────────────── */

static int tls_write_request(tls_ctx_t* ctx,
    const char* method, const char* host,
    const char* path,
    const vela_header_t* headers,
    const char* body, size_t body_len)
{
    /* Heap-allocate header buffer to reduce stack pressure.
     * This function is called from threads with limited stack
     * (outbound dispatch 16KB) and the TLS context already
     * consumes significant stack space. */
    char* hdr = malloc(4096);
    if (!hdr)
        return VELA_TLS_ERR_OVERFLOW;

    int pos = 0;
    int ret;

#define HDR_APPEND(fmt, ...)                                    \
    pos += snprintf(hdr + pos, 4096 - pos, fmt, ##__VA_ARGS__); \
    if (pos >= 4096) {                                          \
        free(hdr);                                              \
        return VELA_TLS_ERR_OVERFLOW;                           \
    }

    HDR_APPEND("%s %s HTTP/1.1\r\n", method, path);
    HDR_APPEND("Host: %s\r\n", host);
    HDR_APPEND("Connection: keep-alive\r\n");
    HDR_APPEND("User-Agent: agent-vela/1.0\r\n");

    if (body && body_len > 0) {
        HDR_APPEND("Content-Length: %zu\r\n", body_len);
    }

    if (headers) {
        for (const vela_header_t* h = headers; h->name != NULL; h++) {
            HDR_APPEND("%s: %s\r\n", h->name, h->value);
        }
    }

    HDR_APPEND("\r\n");
#undef HDR_APPEND

    /* Write headers */
    int written = 0;
    while (written < pos) {
        ret = mbedtls_ssl_write(&ctx->ssl,
            (const unsigned char*)(hdr + written),
            (size_t)(pos - written));
        if (ret > 0) {
            written += ret;
        } else if (ret == 0) {
            syslog(LOG_ERR, "[%s] ssl_write(hdr) returned 0 (conn closed)\n",
                TAG);
            free(hdr);
            return VELA_TLS_ERR_WRITE;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            syslog(LOG_ERR,
                "[%s] ssl_write(hdr) mbedtls_ret=-0x%04x written=%d/%d\n",
                TAG, -ret, written, pos);
            free(hdr);
            return VELA_TLS_ERR_WRITE;
        }
    }

    free(hdr);

    /* Write body */
    if (body && body_len > 0) {
        size_t bw = 0;
        while (bw < body_len) {
            ret = mbedtls_ssl_write(&ctx->ssl,
                (const unsigned char*)(body + bw),
                body_len - bw);
            if (ret > 0) {
                bw += (size_t)ret;
            } else if (ret == 0) {
                syslog(LOG_ERR,
                    "[%s] ssl_write(body) returned 0 at %zu/%zu (conn closed)\n",
                    TAG, bw, body_len);
                return VELA_TLS_ERR_WRITE;
            } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                syslog(LOG_ERR,
                    "[%s] ssl_write(body) mbedtls_ret=-0x%04x at %zu/%zu\n",
                    TAG, -ret, bw, body_len);
                return VELA_TLS_ERR_WRITE;
            }
        }
    }

    return 0;
}

/**
 * Read full HTTP/1.1 response.
 * Returns HTTP status code; writes body into resp_buf (NUL-terminated).
 * Handles Transfer-Encoding: chunked and Content-Length.
 */
#define TLS_RAW_BUF_SIZE 8192  /* 8KB: enough for HTTP headers + initial body */

/* Number of static raw buffers — at most 2, scaled to pool size.
 * Each buffer is 8KB; pool=1 uses 1 buffer, pool>=2 uses 2. */
#define TLS_RAW_BUF_COUNT (CONN_POOL_SIZE < 2 ? 1 : 2)

/* Raw buffers malloc'd on first use so the 16KB pool lives in the PSRAM
 * heap instead of internal DRAM .bss (DRAM is scarce on ESP32-S3). */
static char *s_tls_raw_buf[TLS_RAW_BUF_COUNT];
static pthread_mutex_t s_tls_raw_lock[TLS_RAW_BUF_COUNT];
static pthread_once_t s_tls_raw_once = PTHREAD_ONCE_INIT;

static void tls_raw_init_once(void)
{
    for (int i = 0; i < TLS_RAW_BUF_COUNT; i++) {
        pthread_mutex_init(&s_tls_raw_lock[i], NULL);
        s_tls_raw_buf[i] = malloc(TLS_RAW_BUF_SIZE);
    }
}

static char* tls_raw_acquire(void)
{
    pthread_once(&s_tls_raw_once, tls_raw_init_once);
    for (int i = 0; i < TLS_RAW_BUF_COUNT; i++) {
        if (pthread_mutex_trylock(&s_tls_raw_lock[i]) == 0) {
            if (s_tls_raw_buf[i] == NULL) {
                pthread_mutex_unlock(&s_tls_raw_lock[i]);
                return NULL;
            }
            return s_tls_raw_buf[i];
        }
    }
    /* All busy — block on first */
    pthread_mutex_lock(&s_tls_raw_lock[0]);
    if (s_tls_raw_buf[0] == NULL) {
        pthread_mutex_unlock(&s_tls_raw_lock[0]);
        return NULL;
    }
    return s_tls_raw_buf[0];
}

static void tls_raw_release(char* buf)
{
    for (int i = 0; i < TLS_RAW_BUF_COUNT; i++) {
        if (buf == s_tls_raw_buf[i]) {
            pthread_mutex_unlock(&s_tls_raw_lock[i]);
            return;
        }
    }
}

static int tls_read_response(tls_ctx_t* ctx, char* resp_buf, size_t resp_cap,
    size_t* out_body_len, bool* out_keep_alive)
{
    /* Use pooled PSRAM buffers to avoid internal-DRAM fragmentation */
    char* raw = tls_raw_acquire();
    if (!raw) {
        syslog(LOG_ERR, "[%s] TLS raw buffer alloc failed\n", TAG);
        return VELA_TLS_ERR_READ;
    }
    size_t raw_len = 0;
    int eof = 0;
    int ret;

    /* Read until we have the full header (double CRLF) or buffer full */
    while (!eof && raw_len < TLS_RAW_BUF_SIZE - 1) {
        ret = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)(raw + raw_len),
            TLS_RAW_BUF_SIZE - 1 - raw_len);
        if (ret > 0) {
            raw_len += (size_t)ret;
            if (memmem(raw, raw_len, "\r\n\r\n", 4))
                break;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            eof = 1;
            break;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] ssl_read (header) ret=0x%x\n", TAG, -ret);
            tls_raw_release(raw);
            return VELA_TLS_ERR_READ;
        }
    }
    raw[raw_len] = '\0';

    /* Parse status line */
    int http_status = 0;
    if (sscanf(raw, "HTTP/1.%*d %d", &http_status) != 1) {
        syslog(LOG_ERR, "[%s] Failed to parse HTTP status from: %.80s\n", TAG, raw);
        tls_raw_release(raw);
        return VELA_TLS_ERR_READ;
    }

    /* Find header/body split */
    char* body_start = (char*)memmem(raw, raw_len, "\r\n\r\n", 4);
    if (!body_start) {
        resp_buf[0] = '\0';
        tls_raw_release(raw);
        return http_status;
    }

    /* Determine keep-alive from Connection header (default true for HTTP/1.1) */
    if (out_keep_alive) {
        *out_keep_alive = true;  /* HTTP/1.1 default */
        char* conn_hdr = strcasestr(raw, "Connection:");
        if (conn_hdr && conn_hdr < body_start) {
            *out_keep_alive = (strcasestr(conn_hdr, "keep-alive") != NULL);
        }
    }
    body_start += 4; /* skip double CRLF */

    /* Extract content-length if present */
    long content_length = -1;
    {
        char* cl_hdr = strcasestr(raw, "Content-Length:");
        if (cl_hdr && cl_hdr < body_start) {
            cl_hdr += strlen("Content-Length:");
            content_length = strtol(cl_hdr, NULL, 10);
            if (content_length < 0 || content_length > 10 * 1024 * 1024) {
                content_length = -1; /* reject absurd values */
            }
        }
    }
    int chunked = 0;
    {
        char* te_hdr = strcasestr(raw, "Transfer-Encoding:");
        if (te_hdr && te_hdr < body_start) {
            chunked = (strcasestr(te_hdr, "chunked") != NULL);
        }
    }

    /* Initial fragment already in raw buffer */
    size_t initial = (size_t)(raw + raw_len - body_start);
    size_t resp_pos = 0;

    /* Copy initial fragment */
    size_t copy = initial < resp_cap - 1 ? initial : resp_cap - 1;
    memcpy(resp_buf, body_start, copy);
    resp_pos = copy;

    /* Keep reading body */
    if (!eof) {
        while (resp_pos < resp_cap - 1) {
            if (content_length >= 0 && (long)resp_pos >= content_length)
                break;
            ret = mbedtls_ssl_read(&ctx->ssl,
                (unsigned char*)(resp_buf + resp_pos),
                resp_cap - 1 - resp_pos);
            if (ret > 0) {
                resp_pos += (size_t)ret;
            } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                break;
            } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
                break;
            }
        }
    }

    resp_buf[resp_pos] = '\0';
    tls_raw_release(raw);

    /* Chunked decode */
    if (chunked) {
        resp_pos = decode_chunked(resp_buf, resp_pos);
        resp_buf[resp_pos] = '\0';
    }

    if (out_body_len)
        *out_body_len = resp_pos;

    return http_status;
}

/* ── Public API ──────────────────────────────────────────────── */

int vela_https_request(
    const char* host,
    const char* port,
    const char* method,
    const char* path,
    const vela_header_t* headers,
    const char* body,
    size_t body_len,
    char* resp_buf,
    size_t resp_cap,
    size_t* out_body_len)
{
    int ret;

    /* Try to get a pooled connection first */
    conn_slot_t* slot = pool_acquire(host, port);

    syslog(LOG_DEBUG, "[%s] https_request: host=%s slot=%p valid=%d\n",
        TAG, host, slot, slot ? slot->valid : -1);

    if (slot && slot->valid) {
        /* Drain any leftover data from previous response before reuse.
         * Without this, a partially-read response body (e.g. truncated
         * chunked data) would be misinterpreted as the next HTTP status.
         *
         * Fast path: put the socket in non-blocking mode so
         * mbedtls_ssl_read returns immediately (WANT_READ / EAGAIN)
         * on a healthy idle connection.  This avoids the ~50 ms per
         * probe SO_RCVTIMEO wait that used to add up on every reused
         * connection (LLM + TTS + ASR all hit the same host).
         *
         * Slow path: when there IS leftover data (e.g. TTS PCM was
         * truncated and the tail is still in-flight), we keep reading
         * under the 3-second total budget just like before. */
        if (slot->ctx.net.fd >= 0) {
            /* Heap, not stack: avoid overflow on 4 KB threads */
            unsigned char *drain = malloc(8192);
            if (!drain) goto pool_reconnect;
            int dr = -1;  /* -1 = no drain attempted */
            int drained_total = 0;
            time_t deadline = time(NULL) + 3;  /* 3 s total budget */

            int orig_flags = fcntl(slot->ctx.net.fd, F_GETFL, 0);
            if (orig_flags >= 0) {
                fcntl(slot->ctx.net.fd, F_SETFL,
                    orig_flags | O_NONBLOCK);
            }

            /* First read: non-blocking probe.  Returns immediately
             * with WANT_READ when there's no leftover — no 50ms wait. */
            while (time(NULL) < deadline
                   && (dr = mbedtls_ssl_read(&slot->ctx.ssl,
                           drain, 8192)) > 0) {
                drained_total += dr;
            }
            free(drain);

            /* Restore original (blocking) flags before HTTP write/read */
            if (orig_flags >= 0) {
                fcntl(slot->ctx.net.fd, F_SETFL, orig_flags);
            }

            if (drained_total > 0) {
                syslog(LOG_WARNING, "[%s] Drained %d leftover bytes "
                    "before reusing connection\n", TAG, drained_total);
            }
            /* If peer closed the connection, reconnect */
            if (dr == 0 || dr == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                goto pool_reconnect;
            }
            /* If we hit the time budget, force reconnect — leftover
             * data is still in-flight and would corrupt the next read. */
            if (time(NULL) >= deadline && drained_total > 0) {
                syslog(LOG_WARNING, "[%s] Drain budget exhausted "
                    "(%d bytes), reconnecting\n", TAG, drained_total);
                goto pool_reconnect;
            }
        } else {
            goto pool_reconnect;
        }

        /* Reuse existing connection — skip TLS handshake */
        syslog(LOG_INFO, "[%s] Reusing pooled connection to %s:%s\n", TAG, host, port);
        syslog(LOG_INFO, "[%s] Writing HTTP request (%s %s, %zu bytes body)\n",
            TAG, method, path, body_len);
        ret = tls_write_request(&slot->ctx, method, host, path, headers, body, body_len);
        if (ret == 0) {
            bool keep = false;
            syslog(LOG_INFO, "[%s] Reading HTTP response...\n", TAG);
            ret = tls_read_response(&slot->ctx, resp_buf, resp_cap, out_body_len, &keep);
            syslog(LOG_INFO, "[%s] HTTP response: status=%d body_len=%zu\n",
                TAG, ret, out_body_len ? *out_body_len : 0);
            if (ret > 0) {
                pool_release(slot, host, port, keep);
                return ret;
            }
        }
        /* Connection went stale — fall through to reconnect */
    pool_reconnect:
        syslog(LOG_INFO, "[%s] Pooled connection stale, reconnecting\n", TAG);
        tls_ctx_free(&slot->ctx);
        slot->valid = false;
    }

    /* New connection */
    if (!slot) {
        /* Pool full — use a temporary ephemeral context */
        tls_ctx_t tmp_ctx;
        if ((ret = tls_ctx_connect(&tmp_ctx, host, port)) != 0) {
            tls_ctx_free(&tmp_ctx);
            return ret;
        }
        if ((ret = tls_write_request(&tmp_ctx, method, host, path,
                 headers, body, body_len))
            != 0) {
            syslog(LOG_ERR, "[%s] Write request failed: %d\n", TAG, ret);
            tls_ctx_free(&tmp_ctx);
            return ret;
        }
        ret = tls_read_response(&tmp_ctx, resp_buf, resp_cap, out_body_len, NULL);
        tls_ctx_free(&tmp_ctx);
        return ret;
    }

    /* Connect into the slot */
    syslog(LOG_INFO, "[%s] New connection to %s:%s\n", TAG, host, port);
    if ((ret = tls_ctx_connect(&slot->ctx, host, port)) != 0) {
        syslog(LOG_ERR, "[%s] TLS connect failed: %d\n", TAG, ret);
        pool_release(slot, host, port, false);
        return ret;
    }

    syslog(LOG_INFO, "[%s] Writing HTTP request (%s %s, %zu bytes body)\n",
        TAG, method, path, body_len);
    if ((ret = tls_write_request(&slot->ctx, method, host, path,
             headers, body, body_len))
        != 0) {
        syslog(LOG_ERR, "[%s] Write request failed: %d\n", TAG, ret);
        pool_release(slot, host, port, false);
        return ret;
    }

    syslog(LOG_INFO, "[%s] Reading HTTP response...\n", TAG);
    bool keep = false;
    ret = tls_read_response(&slot->ctx, resp_buf, resp_cap, out_body_len, &keep);
    syslog(LOG_INFO, "[%s] HTTP response: status=%d body_len=%zu\n",
        TAG, ret, out_body_len ? *out_body_len : 0);
    pool_release(slot, host, port, ret > 0 && keep);
    return ret;
}

int vela_https_get(const char* host, const char* port, const char* path,
    char* resp_buf, size_t resp_cap)
{
    return vela_https_request(host, port, "GET", path, NULL, NULL, 0,
        resp_buf, resp_cap, NULL);
}

int vela_https_post_json(const char* host, const char* port, const char* path,
    const vela_header_t* extra_headers,
    const char* json_body,
    char* resp_buf, size_t resp_cap)
{
    /* Build a merged header list: Content-Type first, then caller extras */
    const int MAX_HDRS = 32;
    vela_header_t merged[MAX_HDRS];
    int n = 0;

    merged[n++] = (vela_header_t) { "Content-Type", "application/json" };

    if (extra_headers) {
        for (const vela_header_t* h = extra_headers; h->name && n < MAX_HDRS - 1; h++) {
            merged[n++] = *h;
        }
    }
    merged[n] = (vela_header_t) { NULL, NULL };

    size_t body_len = json_body ? strlen(json_body) : 0;
    return vela_https_request(host, port, "POST", path, merged,
        json_body, body_len, resp_buf, resp_cap, NULL);
}

/* ── Streaming line-by-line HTTPS POST ─────────────────────── */

/* Line buffer size for streaming reads.  Must be large enough to hold
 * the longest single line in the response (TTS JSON chunks are ~2KB). */
#define STREAM_LINE_BUF_SIZE (8 * 1024)

int vela_https_post_stream(
    const char* host,
    const char* port,
    const char* path,
    const vela_header_t* headers,
    const char* body,
    size_t body_len,
    vela_stream_line_cb line_cb,
    void* user_data,
    int timeout_sec)
{
    tls_ctx_t ctx;
    int ret;

    if (!host || !port || !path || !line_cb) {
        return VELA_TLS_ERR_CONNECT;
    }

    /* 1. TLS connect (dedicated, no pool) */
    ret = tls_ctx_connect(&ctx, host, port);
    if (ret != 0) {
        tls_ctx_free(&ctx);
        return ret;
    }

    /* Set read timeout */
    int tout = (timeout_sec > 0) ? timeout_sec : 60;
    if (ctx.net.fd >= 0) {
        struct timeval tv = { .tv_sec = tout, .tv_usec = 0 };
        setsockopt(ctx.net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* 2. Send HTTP request */
    ret = tls_write_request(&ctx, "POST", host, path, headers, body, body_len);
    if (ret != 0) {
        tls_ctx_free(&ctx);
        return ret;
    }

    syslog(LOG_INFO, "[%s] stream: request sent, reading response...\n", TAG);

    /* 3. Read HTTP response headers (up to double CRLF) */
    char hdr_buf[VELA_TLS_HDR_BUF];
    size_t hdr_len = 0;
    int eof = 0;

    while (!eof && hdr_len < sizeof(hdr_buf) - 1) {
        ret = mbedtls_ssl_read(&ctx.ssl,
            (unsigned char*)(hdr_buf + hdr_len),
            sizeof(hdr_buf) - 1 - hdr_len);
        if (ret > 0) {
            hdr_len += (size_t)ret;
            if (memmem(hdr_buf, hdr_len, "\r\n\r\n", 4))
                break;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            eof = 1;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] stream: ssl_read header ret=0x%x\n",
                TAG, -ret);
            tls_ctx_free(&ctx);
            return VELA_TLS_ERR_READ;
        }
    }
    hdr_buf[hdr_len] = '\0';

    /* Parse status line */
    int http_status = 0;
    if (sscanf(hdr_buf, "HTTP/1.%*d %d", &http_status) != 1) {
        syslog(LOG_ERR, "[%s] stream: bad status line: %.80s\n",
            TAG, hdr_buf);
        tls_ctx_free(&ctx);
        return VELA_TLS_ERR_READ;
    }

    /* Find header/body boundary */
    char* body_start = (char*)memmem(hdr_buf, hdr_len, "\r\n\r\n", 4);
    if (!body_start) {
        tls_ctx_free(&ctx);
        return http_status;
    }
    body_start += 4;

    /* Parse Content-Length and Transfer-Encoding */
    long content_length = -1;
    {
        char* cl = strcasestr(hdr_buf, "Content-Length:");
        if (cl && cl < body_start) {
            content_length = strtol(cl + 15, NULL, 10);
            if (content_length < 0 || content_length > 10 * 1024 * 1024)
                content_length = -1;
        }
    }
    int chunked = 0;
    {
        char* te = strcasestr(hdr_buf, "Transfer-Encoding:");
        if (te && te < body_start) {
            chunked = (strcasestr(te, "chunked") != NULL);
        }
    }

    /* 4. Stream body line-by-line.
     *
     * Strategy: read bytes from TLS into a line buffer.  When '\n' is
     * found, extract the line (strip \r\n), NUL-terminate, and call
     * the callback.  Leftover bytes stay in the buffer for the next read.
     *
     * For chunked responses, we read raw chunked data and decode lines
     * from the decoded stream.  To keep things simple, we buffer the
     * full chunked body first (TTS responses are < 200KB), then parse
     * lines from the decoded buffer. */

    if (chunked) {
        /* Chunked: buffer full body, decode, then deliver lines */
        char* chunk_buf = malloc(512 * 1024);
        if (!chunk_buf) {
            tls_ctx_free(&ctx);
            return VELA_TLS_ERR_OVERFLOW;
        }

        size_t initial = (size_t)(hdr_buf + hdr_len - body_start);
        size_t cpos = 0;
        if (initial > 0) {
            size_t cp = initial < 512 * 1024 - 1 ? initial : 512 * 1024 - 1;
            memcpy(chunk_buf, body_start, cp);
            cpos = cp;
        }

        while (!eof && cpos < 512 * 1024 - 1) {
            ret = mbedtls_ssl_read(&ctx.ssl,
                (unsigned char*)(chunk_buf + cpos),
                512 * 1024 - 1 - cpos);
            if (ret > 0) {
                cpos += (size_t)ret;
            } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                break;
            } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
                break;
            }
        }
        chunk_buf[cpos] = '\0';

        /* Decode chunked encoding in-place */
        size_t decoded = decode_chunked(chunk_buf, cpos);

        /* Deliver lines from decoded buffer */
        char* p = chunk_buf;
        char* end = chunk_buf + decoded;
        int abort = 0;

        while (p < end && !abort) {
            char* nl = memchr(p, '\n', (size_t)(end - p));
            size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
            /* Strip trailing \r */
            if (llen > 0 && p[llen - 1] == '\r')
                llen--;
            if (llen > 0) {
                char saved = p[llen];
                p[llen] = '\0';
                abort = line_cb(p, llen, user_data);
                p[llen] = saved;
            }
            p += llen + (nl ? 1 : 0);
        }

        free(chunk_buf);
        tls_ctx_free(&ctx);
        return http_status;
    }

    /* Non-chunked: stream lines directly as they arrive.
     *
     * We use a line buffer to accumulate partial lines across reads.
     * Each time we find '\n', we deliver the complete line to the callback. */
    char* line_buf = malloc(STREAM_LINE_BUF_SIZE);
    if (!line_buf) {
        tls_ctx_free(&ctx);
        return VELA_TLS_ERR_OVERFLOW;
    }
    size_t line_pos = 0;
    long total_read = 0;
    int abort = 0;

    /* Process initial body fragment already in hdr_buf */
    size_t init_len = (size_t)(hdr_buf + hdr_len - body_start);
    if (init_len > 0 && !eof) {
        memcpy(line_buf, body_start, init_len);
        line_pos = init_len;
        total_read += (long)init_len;
    }

    while (!abort && !eof) {
        /* Extract complete lines from line_buf */
        while (line_pos > 0) {
            char* nl = memchr(line_buf, '\n', line_pos);
            if (!nl)
                break;
            size_t llen = (size_t)(nl - line_buf);
            if (llen > 0 && line_buf[llen - 1] == '\r')
                llen--;
            if (llen > 0) {
                char saved = line_buf[llen];
                line_buf[llen] = '\0';
                abort = line_cb(line_buf, llen, user_data);
                line_buf[llen] = saved;
            }
            /* Shift remaining bytes to front */
            size_t consumed = llen + 1; /* +1 for \n */
            memmove(line_buf, line_buf + consumed, line_pos - consumed);
            line_pos -= consumed;
        }

        if (abort)
            break;

        /* Check if we've read all content */
        if (content_length >= 0 && total_read >= content_length)
            break;

        /* Read more data from TLS */
        size_t space = STREAM_LINE_BUF_SIZE - line_pos - 1;
        if (space == 0) {
            /* Line too long — deliver what we have as a partial line */
            line_buf[line_pos] = '\0';
            abort = line_cb(line_buf, line_pos, user_data);
            line_pos = 0;
            continue;
        }

        ret = mbedtls_ssl_read(&ctx.ssl,
            (unsigned char*)(line_buf + line_pos), space);
        if (ret > 0) {
            line_pos += (size_t)ret;
            total_read += ret;
            line_buf[line_pos] = '\0';
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            eof = 1;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            break;
        }
    }

    /* Deliver any remaining partial line */
    if (!abort && line_pos > 0) {
        if (line_buf[line_pos - 1] == '\r')
            line_pos--;
        if (line_pos > 0) {
            line_buf[line_pos] = '\0';
            line_cb(line_buf, line_pos, user_data);
        }
    }

    free(line_buf);
    tls_ctx_free(&ctx);

    syslog(LOG_INFO, "[%s] stream: done, status=%d, %ld bytes\n",
        TAG, http_status, total_read);
    return http_status;
}

int vela_https_head_date(const char* host, const char* port, const char* path,
    char* date_out, size_t date_cap)
{
    tls_ctx_t ctx;
    int ret;

    if ((ret = tls_ctx_connect(&ctx, host, port)) != 0) {
        tls_ctx_free(&ctx);
        return ret;
    }

    if ((ret = tls_write_request(&ctx, "HEAD", host, path, NULL, NULL, 0)) != 0) {
        tls_ctx_free(&ctx);
        return ret;
    }

    /* Read raw response until end of headers (\r\n\r\n) */
    char hdr_buf[2048];
    int total = 0;
    while (total < (int)sizeof(hdr_buf) - 1) {
        ret = mbedtls_ssl_read(&ctx.ssl,
            (unsigned char*)hdr_buf + total,
            sizeof(hdr_buf) - 1 - total);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ)
            continue;
        if (ret <= 0)
            break;
        total += ret;
        hdr_buf[total] = '\0';
        if (strstr(hdr_buf, "\r\n\r\n"))
            break;
    }
    tls_ctx_free(&ctx);

    if (total <= 0)
        return VELA_TLS_ERR_READ;

    /* Find Date: header (case-insensitive prefix search) */
    char* p = strcasestr(hdr_buf, "\r\nDate: ");
    if (!p)
        return VELA_TLS_ERR_READ;
    p += 8; /* skip \r\nDate:  */
    char* eol = strstr(p, "\r\n");
    if (!eol)
        return VELA_TLS_ERR_READ;

    size_t dlen = (size_t)(eol - p);
    if (dlen >= date_cap)
        dlen = date_cap - 1;
    memcpy(date_out, p, dlen);
    date_out[dlen] = '\0';
    return 0;
}

/* ── Plain HTTP (no TLS) POST ─────────────────────────────── */

int vela_http_post_json(const char* host, const char* port, const char* path,
    const vela_header_t* extra_headers,
    const char* json_body,
    char* resp_buf, size_t resp_cap)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0 || !res) {
        syslog(LOG_INFO, "http: getaddrinfo %s:%s failed: %d", host, port, gai);
        return VELA_TLS_ERR_CONNECT;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        syslog(LOG_INFO, "http: socket() failed: %d", errno);
        return VELA_TLS_ERR_CONNECT;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        syslog(LOG_INFO, "http: connect %s:%s failed: %d", host, port, errno);
        close(fd);
        freeaddrinfo(res);
        return VELA_TLS_ERR_CONNECT;
    }
    freeaddrinfo(res);

    /* Set read timeout */
    struct timeval tv = { .tv_sec = AGENT_LLM_SOCKET_TIMEOUT_SEC,
        .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Build HTTP request */
    size_t body_len = json_body ? strlen(json_body) : 0;
    char hdr[4096];
    int pos = 0;

#define HTTP_APPEND(fmt, ...)                                               \
    pos += snprintf(hdr + pos, (int)sizeof(hdr) - pos, fmt, ##__VA_ARGS__); \
    if (pos >= (int)sizeof(hdr)) {                                          \
        close(fd);                                                          \
        return VELA_TLS_ERR_OVERFLOW;                                       \
    }

    HTTP_APPEND("POST %s HTTP/1.1\r\n", path);
    HTTP_APPEND("Host: %s\r\n", host);
    HTTP_APPEND("Content-Type: application/json\r\n");
    HTTP_APPEND("Connection: close\r\n");
    HTTP_APPEND("User-Agent: agent/1.0\r\n");
    if (body_len > 0) {
        HTTP_APPEND("Content-Length: %zu\r\n", body_len);
    }
    if (extra_headers) {
        for (const vela_header_t* h = extra_headers; h->name; h++) {
            HTTP_APPEND("%s: %s\r\n", h->name, h->value);
        }
    }
    HTTP_APPEND("\r\n");
#undef HTTP_APPEND

    /* Send header + body */
    if (write(fd, hdr, (size_t)pos) != pos) {
        close(fd);
        return VELA_TLS_ERR_WRITE;
    }
    if (json_body && body_len > 0) {
        if (write(fd, json_body, body_len) != (ssize_t)body_len) {
            close(fd);
            return VELA_TLS_ERR_WRITE;
        }
    }

    /* Read response */
    char* raw = (char*)malloc(TLS_RAW_BUF_SIZE);
    if (!raw) {
        close(fd);
        return VELA_TLS_ERR_READ;
    }
    size_t raw_len = 0;
    int eof = 0;

    /* Read until we have the full header */
    while (!eof && raw_len < TLS_RAW_BUF_SIZE - 1) {
        ssize_t n = read(fd, raw + raw_len, TLS_RAW_BUF_SIZE - 1 - raw_len);
        if (n > 0) {
            raw_len += (size_t)n;
            raw[raw_len] = '\0';
            if (memmem(raw, raw_len, "\r\n\r\n", 4))
                break;
        } else {
            eof = 1;
        }
    }
    raw[raw_len] = '\0';

    /* Parse status */
    int http_status = 0;
    if (sscanf(raw, "HTTP/1.%*d %d", &http_status) != 1) {
        syslog(LOG_INFO, "http: bad status: %.80s", raw);
        free(raw);
        close(fd);
        return VELA_TLS_ERR_READ;
    }

    /* Find body */
    char* body_start = (char*)memmem(raw, raw_len, "\r\n\r\n", 4);
    if (!body_start) {
        resp_buf[0] = '\0';
        free(raw);
        close(fd);
        return http_status;
    }
    body_start += 4;

    /* Check content-length / chunked */
    long content_length = -1;
    {
        char* cl = strcasestr(raw, "Content-Length:");
        if (cl && cl < body_start) {
            content_length = strtol(cl + strlen("Content-Length:"), NULL, 10);
            if (content_length < 0 || content_length > 10 * 1024 * 1024) {
                content_length = -1;
            }
        }
    }
    int chunked = 0;
    {
        char* te = strcasestr(raw, "Transfer-Encoding:");
        if (te && te < body_start) {
            chunked = (strcasestr(te, "chunked") != NULL);
        }
    }

    /* Copy initial fragment */
    size_t initial = (size_t)(raw + raw_len - body_start);
    size_t resp_pos = 0;
    size_t copy = initial < resp_cap - 1 ? initial : resp_cap - 1;
    memcpy(resp_buf, body_start, copy);
    resp_pos = copy;

    /* Keep reading body */
    if (!eof) {
        while (resp_pos < resp_cap - 1) {
            if (content_length >= 0 && (long)resp_pos >= content_length)
                break;
            ssize_t n = read(fd, resp_buf + resp_pos, resp_cap - 1 - resp_pos);
            if (n <= 0)
                break;
            resp_pos += (size_t)n;
        }
    }
    resp_buf[resp_pos] = '\0';
    free(raw);
    close(fd);

    /* Chunked decode */
    if (chunked) {
        resp_pos = decode_chunked(resp_buf, resp_pos);
        resp_buf[resp_pos] = '\0';
    }

    return http_status;
}

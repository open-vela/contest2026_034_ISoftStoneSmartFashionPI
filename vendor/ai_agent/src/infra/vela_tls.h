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

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum size of HTTP response header section we buffer internally */
#define VELA_TLS_HDR_BUF  4096

/* Return codes (negative on error, HTTP status code on success) */
#define VELA_TLS_ERR_CONNECT   -1
#define VELA_TLS_ERR_HANDSHAKE -2
#define VELA_TLS_ERR_WRITE     -3
#define VELA_TLS_ERR_READ      -4
#define VELA_TLS_ERR_OVERFLOW  -5

/**
 * Extra header entry; pass an array terminated by {NULL, NULL}.
 */
typedef struct {
    const char *name;
    const char *value;
} vela_header_t;

/**
 * Perform a complete HTTPS request and collect the response body.
 *
 * @param host        Hostname (e.g. "api.anthropic.com")
 * @param port        Port string (e.g. "443")
 * @param method      "GET", "POST", etc.
 * @param path        URL path starting with '/' (e.g. "/v1/messages")
 * @param headers     NULL-terminated array of extra request headers, or NULL
 * @param body        Request body bytes, or NULL
 * @param body_len    Length of body; 0 if no body
 * @param resp_buf    Caller-supplied buffer for response body
 * @param resp_cap    Capacity of resp_buf (bytes)
 *
 * @return HTTP status code (200, 400, etc.) on success, negative VELA_TLS_ERR_* on failure
 *
 * The response body is written to resp_buf as a NUL-terminated string.
 * If the body is larger than resp_cap-1, it is silently truncated.
 */
int vela_https_request(
    const char       *host,
    const char       *port,
    const char       *method,
    const char       *path,
    const vela_header_t *headers,   /* NULL-terminated array or NULL */
    const char       *body,
    size_t            body_len,
    char             *resp_buf,
    size_t            resp_cap,
    size_t           *out_body_len  /* optional: actual body bytes written, or NULL */
);

/**
 * Convenience wrapper for GET with no extra headers or body.
 */
int vela_https_get(const char *host, const char *port, const char *path,
                   char *resp_buf, size_t resp_cap);

/**
 * Convenience wrapper for POST with Content-Type: application/json.
 */
int vela_https_post_json(const char *host, const char *port, const char *path,
                         const vela_header_t *extra_headers,
                         const char *json_body,
                         char *resp_buf, size_t resp_cap);

/**
 * Plain HTTP (no TLS) POST with Content-Type: application/json.
 * Used for internal/intranet endpoints that don't support HTTPS.
 */
int vela_http_post_json(const char *host, const char *port, const char *path,
                        const vela_header_t *extra_headers,
                        const char *json_body,
                        char *resp_buf, size_t resp_cap);

/**
 * Send a HEAD request and extract the value of the "Date:" response header.
 *
 * @param host     Hostname (e.g. "api.deepseek.com")
 * @param port     Port string (e.g. "443")
 * @param path     Request path (e.g. "/")
 * @param date_out Caller-supplied buffer to receive the Date header value
 * @param date_cap Capacity of date_out
 *
 * @return 0 on success, negative on error
 */
int vela_https_head_date(const char *host, const char *port, const char *path,
                         char *date_out, size_t date_cap);

/**
 * SSE (Server-Sent Events) streaming POST callback.
 *
 * @param event_data  Pointer to the data after "data:" prefix
 * @param data_len    Length of event_data (not NUL-terminated)
 * @param user_data   Opaque pointer passed from caller
 *
 * @return 0 to continue, non-zero to abort streaming
 */
typedef int (*vela_sse_cb_t)(const char *event_data, size_t data_len,
                             void *user_data);

/**
 * Streaming line callback for HTTPS POST responses.
 *
 * Called once for each complete line (delimited by '\n') in the response body.
 * The line is NUL-terminated and does NOT include the trailing newline.
 *
 * @param line      NUL-terminated line text
 * @param line_len  Length of line (excluding NUL terminator)
 * @param user_data Opaque pointer passed from caller
 *
 * @return 0 to continue, non-zero to abort streaming
 */
typedef int (*vela_stream_line_cb)(const char *line, size_t line_len,
                                   void *user_data);

/**
 * HTTPS POST with streaming line-by-line response.
 *
 * Establishes a dedicated TLS connection (no pool reuse), sends an HTTP POST
 * request, then reads the response body incrementally.  Each complete line
 * (delimited by '\\n') is delivered to the callback as it arrives — the
 * function does NOT wait for the full response.
 *
 * Supports both Content-Length and chunked Transfer-Encoding responses.
 *
 * Stops when: callback returns non-zero, connection closes, all data
 * indicated by Content-Length is received, or read timeout.
 *
 * @param host        Hostname
 * @param port        Port string (e.g. "443")
 * @param path        URL path
 * @param headers     NULL-terminated array of extra headers, or NULL
 * @param body        Request body bytes
 * @param body_len    Length of body
 * @param line_cb     Callback for each complete line in the response
 * @param user_data   Opaque pointer passed to callback
 * @param timeout_sec Read timeout in seconds (0 = 60s default)
 *
 * @return HTTP status code on success, negative VELA_TLS_ERR_* on failure
 */
int vela_https_post_stream(
    const char          *host,
    const char          *port,
    const char          *path,
    const vela_header_t *headers,
    const char          *body,
    size_t               body_len,
    vela_stream_line_cb  line_cb,
    void                *user_data,
    int                  timeout_sec);

/**
 * Warm up the TLS connection pool for host:port without sending a request.
 *
 * No-op when a live pooled connection for this host:port already exists
 * (idle, or owned by an in-flight request).  An idle-but-stale slot is
 * probed and re-established; otherwise the TLS handshake runs into a
 * FREE pool slot only — valid connections to other hosts are never
 * evicted.  Call this from a background thread in the SAME task group
 * that will later issue the request (NuttX fd tables are per task group).
 *
 * @param host  Hostname
 * @param port  Port string (e.g. "443")
 *
 * @return 0 when a usable connection is in the pool, negative otherwise
 */
int vela_tls_preconnect(const char* host, const char* port);

/**
 * Release all TLS connection pool resources.
 * Call during shutdown to free mbedtls contexts.
 */
void vela_tls_pool_cleanup(void);

#ifdef __cplusplus
}
#endif

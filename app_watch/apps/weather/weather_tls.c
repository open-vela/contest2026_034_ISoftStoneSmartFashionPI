#include <nuttx/config.h>
#include "netutils/webclient.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>

#ifndef MBEDTLS_ERR_NET_UNKNOWN_HOST
#define MBEDTLS_ERR_NET_UNKNOWN_HOST       -0x0042
#define MBEDTLS_ERR_NET_SOCKET_FAILED      -0x0044
#define MBEDTLS_ERR_NET_CONNECT_FAILED     -0x0046
#define MBEDTLS_ERR_NET_INVALID_CONTEXT    -0x0048
#define MBEDTLS_ERR_NET_CONN_RESET         -0x004A
#define MBEDTLS_ERR_NET_SEND_FAILED        -0x004C
#define MBEDTLS_ERR_NET_RECV_FAILED        -0x004E
#endif

typedef struct {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
} weather_tls_conn_t;

static void net_init(int *fd) { *fd = -1; }

static int net_connect_with_timeout(int *fd, const char *host, const char *port, 
                                     unsigned int timeout_sec)
{
    printf("[WeatherTLS] Resolving hostname: %s:%s\n", host, port);
    
    struct addrinfo hints, *list, *cur;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int gai_ret = getaddrinfo(host, port, &hints, &list);
    if (gai_ret != 0) {
        printf("[WeatherTLS] DNS resolution failed: gai_error=%d\n", gai_ret);
        return MBEDTLS_ERR_NET_UNKNOWN_HOST;
    }
    printf("[WeatherTLS] DNS resolution succeeded\n");

    int ret = MBEDTLS_ERR_NET_UNKNOWN_HOST;
    for (cur = list; cur; cur = cur->ai_next) {
        int s = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
        if (s < 0) { 
            ret = MBEDTLS_ERR_NET_SOCKET_FAILED; 
            printf("[WeatherTLS] Socket creation failed: errno=%d\n", errno);
            continue; 
        }

        int flags = fcntl(s, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(s, F_SETFL, flags | O_NONBLOCK);
        }

        int conn_ret = connect(s, cur->ai_addr, cur->ai_addrlen);
        if (conn_ret == 0) {
            *fd = s; 
            ret = 0; 
            printf("[WeatherTLS] Connected immediately\n");
            break;
        }

        if (conn_ret < 0 && errno == EINPROGRESS) {
            fd_set writefds;
            FD_ZERO(&writefds);
            FD_SET(s, &writefds);

            struct timeval timeout = {timeout_sec, 0};
            int sel_ret = select(s + 1, NULL, &writefds, NULL, &timeout);
            
            if (sel_ret > 0 && FD_ISSET(s, &writefds)) {
                int err = 0;
                socklen_t errlen = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen);
                
                if (err == 0) {
                    *fd = s; 
                    ret = 0;
                    printf("[WeatherTLS] Connected after select\n");
                    break;
                } else {
                    printf("[WeatherTLS] Connect failed after select: err=%d\n", err);
                }
            } else if (sel_ret == 0) {
                printf("[WeatherTLS] Connect timeout after %u seconds\n", timeout_sec);
                ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
            } else {
                printf("[WeatherTLS] Select error: errno=%d\n", errno);
                ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
            }
        } else {
            printf("[WeatherTLS] Connect failed immediately: errno=%d\n", errno);
            ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
        }
        
        close(s);
    }
    freeaddrinfo(list);
    
    if (ret != 0) {
        printf("[WeatherTLS] Connection failed, returning %d\n", ret);
    }
    return ret;
}

static int net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    int ret = (int)write(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

static int net_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    int ret = (int)read(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

static void net_free(int *fd)
{
    if (*fd >= 0) { shutdown(*fd, 2); close(*fd); }
    *fd = -1;
}

static int weather_tls_connect(void *ctx, const char *hostname,
                               const char *port,
                               unsigned int timeout_second,
                               struct webclient_tls_connection **connp)
{
    (void)ctx;
    (void)timeout_second;

    weather_tls_conn_t *tc = calloc(1, sizeof(weather_tls_conn_t));
    if (!tc) return -ENOMEM;

    int ret;

    net_init(&tc->fd);
    esp_mbedtls_ssl_init(&tc->ssl);
    esp_mbedtls_ssl_config_init(&tc->conf);
    esp_mbedtls_entropy_init(&tc->entropy);
    esp_mbedtls_ctr_drbg_init(&tc->ctr_drbg);

    ret = esp_mbedtls_ctr_drbg_seed(&tc->ctr_drbg, esp_mbedtls_entropy_func,
                                    &tc->entropy, NULL, 0);
    if (ret != 0) {
        printf("[WeatherTLS] ctr_drbg_seed failed: -0x%x, trying /dev/urandom\n", -ret);
        esp_mbedtls_entropy_free(&tc->entropy);
        esp_mbedtls_ctr_drbg_free(&tc->ctr_drbg);
        esp_mbedtls_entropy_init(&tc->entropy);
        esp_mbedtls_ctr_drbg_init(&tc->ctr_drbg);

        int urand = open("/dev/urandom", O_RDONLY);
        if (urand >= 0) {
            unsigned char seed[48];
            read(urand, seed, sizeof(seed));
            close(urand);
            ret = esp_mbedtls_ctr_drbg_seed(&tc->ctr_drbg, esp_mbedtls_entropy_func,
                                            &tc->entropy, seed, sizeof(seed));
        }
    }
    if (ret != 0) {
        printf("[WeatherTLS] ctr_drbg_seed failed: -0x%x\n", -ret);
        goto cleanup;
    }

    char port_str[8];
    if (port && port[0] != '\0')
        strncpy(port_str, port, sizeof(port_str) - 1);
    else
        strncpy(port_str, "443", sizeof(port_str) - 1);
    port_str[sizeof(port_str) - 1] = '\0';

    printf("[WeatherTLS] ===== TLS Connect Start =====\n");
    printf("[WeatherTLS] Host: %s, Port: %s, Timeout: %u\n", hostname, port_str, timeout_second);

    unsigned int conn_timeout = (timeout_second > 0) ? timeout_second : 10;
    ret = net_connect_with_timeout(&tc->fd, hostname, port_str, conn_timeout);
    if (ret != 0) {
        printf("[WeatherTLS] Connection failed: ret=%d (-0x%x)\n", ret, -ret);
        goto cleanup;
    }
    printf("[WeatherTLS] TCP connection established\n");

    ret = esp_mbedtls_ssl_config_defaults(&tc->conf,
                                          MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        printf("[WeatherTLS] ssl_config_defaults failed: -0x%x\n", -ret);
        goto cleanup;
    }

    esp_mbedtls_ssl_conf_authmode(&tc->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    esp_mbedtls_ssl_conf_rng(&tc->conf, esp_mbedtls_ctr_drbg_random,
                             &tc->ctr_drbg);

    ret = esp_mbedtls_ssl_setup(&tc->ssl, &tc->conf);
    if (ret != 0) {
        printf("[WeatherTLS] ssl_setup failed: -0x%x\n", -ret);
        goto cleanup;
    }

    ret = esp_mbedtls_ssl_set_hostname(&tc->ssl, hostname);
    if (ret != 0) {
        printf("[WeatherTLS] set_hostname failed: -0x%x\n", -ret);
        goto cleanup;
    }

    esp_mbedtls_ssl_set_bio(&tc->ssl, &tc->fd, net_send, net_recv, NULL);

    printf("[WeatherTLS] Starting TLS handshake...\n");
    int handshake_attempts = 0;
    const int max_handshake_attempts = 10;  // 增加重试次数
    
    while ((ret = esp_mbedtls_ssl_handshake(&tc->ssl)) != 0) {
        handshake_attempts++;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            printf("[WeatherTLS] Handshake waiting for read (attempt %d)\n", handshake_attempts);
            usleep(10000);  // 10ms等待
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf("[WeatherTLS] Handshake waiting for write (attempt %d)\n", handshake_attempts);
            usleep(10000);  // 10ms等待
        } else {
            printf("[WeatherTLS] Handshake failed: ret=%d (-0x%x)\n", ret, -ret);
            goto cleanup;
        }
        
        if (handshake_attempts >= max_handshake_attempts) {
            printf("[WeatherTLS] Handshake timeout after %d attempts\n", max_handshake_attempts);
            ret = -ETIMEDOUT;
            goto cleanup;
        }
    }

    printf("[WeatherTLS] TLS handshake completed successfully\n");
    printf("[WeatherTLS] ===== TLS Connect End (SUCCESS) =====\n");
    *connp = (struct webclient_tls_connection *)tc;
    return 0;

cleanup:
    esp_mbedtls_ssl_free(&tc->ssl);
    esp_mbedtls_ssl_config_free(&tc->conf);
    esp_mbedtls_ctr_drbg_free(&tc->ctr_drbg);
    esp_mbedtls_entropy_free(&tc->entropy);
    net_free(&tc->fd);
    return EIO;
}

static ssize_t weather_tls_send(void *ctx,
                                struct webclient_tls_connection *conn,
                                const void *buf, size_t len)
{
    (void)ctx;
    weather_tls_conn_t *tc = (weather_tls_conn_t *)conn;
    int ret;
    do {
        ret = esp_mbedtls_ssl_write(&tc->ssl, buf, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret < 0) return -EIO;
    return ret;
}

static ssize_t weather_tls_recv(void *ctx,
                                struct webclient_tls_connection *conn,
                                void *buf, size_t len)
{
    (void)ctx;
    weather_tls_conn_t *tc = (weather_tls_conn_t *)conn;
    int ret;
    do {
        ret = esp_mbedtls_ssl_read(&tc->ssl, buf, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if (ret < 0) return -EIO;
    return ret;
}

static int weather_tls_close(void *ctx,
                             struct webclient_tls_connection *conn)
{
    (void)ctx;
    weather_tls_conn_t *tc = (weather_tls_conn_t *)conn;
    if (!tc) return 0;
    esp_mbedtls_ssl_close_notify(&tc->ssl);
    esp_mbedtls_ssl_free(&tc->ssl);
    esp_mbedtls_ssl_config_free(&tc->conf);
    esp_mbedtls_ctr_drbg_free(&tc->ctr_drbg);
    esp_mbedtls_entropy_free(&tc->entropy);
    net_free(&tc->fd);
    free(tc);
    return 0;
}

static int weather_tls_get_poll_info(void *ctx,
                                    struct webclient_tls_connection *conn,
                                    struct webclient_poll_info *info)
{
    (void)ctx;
    weather_tls_conn_t *tc = (weather_tls_conn_t *)conn;
    info->fd = tc->fd;
    info->flags = WEBCLIENT_POLL_INFO_WANT_READ | WEBCLIENT_POLL_INFO_WANT_WRITE;
    return 0;
}

const struct webclient_tls_ops weather_webclient_tls_ops = {
    .connect = weather_tls_connect,
    .send = weather_tls_send,
    .recv = weather_tls_recv,
    .close = weather_tls_close,
    .get_poll_info = weather_tls_get_poll_info,
    .init_connection = NULL,
};


#include "weather.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include "../settings/settings_wifi.h"
#include "netutils/cJSON.h"
#include "netutils/webclient.h"
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define WEATHER_LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#  define WEATHER_LOG(fmt, ...)
#endif

/* 样式定义 */
static lv_style_t *style_btn_normal = NULL;
static lv_style_t *style_btn_selected = NULL;

/* 全局变量 */
/* 堆分配节省 96B BSS */
static char *g_weather_location_id = NULL;
static char *g_weather_city = NULL;
static LocationInfo *g_location = NULL;  /* 堆分配节省 160B BSS */
static int g_location_initialized = 0;
static lv_obj_t *weather_main_base;
static lv_obj_t *day_btn;
static lv_obj_t *week_btn;
static lv_obj_t *weather_icon;
static lv_obj_t *temp_label;
static lv_obj_t *weather_unit_label;
static lv_obj_t *hourly_container;
static lv_obj_t *daily_container;
static lv_obj_t *loading_label;
static lv_timer_t *update_timer;
static WeatherData *weather_data = NULL;  /* 堆分配，节省 2888 字节 BSS */

static WeatherData* weather_data_get(void)
{
    if (weather_data == NULL) {
        weather_data = calloc(1, sizeof(WeatherData));
    }
    return weather_data;
}
#define WD (*weather_data_get())
static int current_mode = 0;
static int g_weather_fetching = 0;
static time_t g_last_fetch_time = 0;
static time_t g_enter_time = 0;
static int g_data_shown = 0;
#define WEATHER_FETCH_INTERVAL_SEC (30 * 60)
#define WEATHER_TIMEOUT_SEC 30

static void slide_gesture_handler(lv_event_t *e);
static void start_weather_fetch(void);

/* 和风天气 icon code -> 本地图标名 映射表 */
typedef struct {
    const char *code;
    const char *icon_name;
} qweather_icon_map_t;

static const qweather_icon_map_t qweather_icon_map[] = {
    {"100", "sunny"}, // 晴
    {"101", "cloudy"}, // 多云
    {"102", "cloudytosunny"}, // 少云
    {"103", "cloudytosunny"}, // 晴间多云
    {"104", "cloudy"}, // 阴
    {"150", "sunny"}, // 晴
    {"151", "cloudy"}, // 多云
    {"152", "cloudytosunny"}, // 少云
    {"153", "cloudytosunny"}, // 晴间多云
    {"300", "thundershower"}, // 阵雨
    {"301", "thundershower"}, // 强阵雨
    {"302", "thundershower"}, // 雷阵雨
    {"303", "thundershower"}, // 强雷阵雨 
    {"304", "hail"}, // 雷阵雨伴有冰雹
    {"305", "lightrain"}, // 小雨
    {"306", "moderaterain"}, // 中雨
    {"307", "rainstorm"}, // 大雨
    {"308", "rainstorm"}, // 极端降雨
    {"309", "lightrain"}, // 毛毛雨/细雨
    {"310", "rainstorm"}, // 暴雨
    {"311", "rainstorm"}, // 大暴雨
    {"312", "rainstorm"}, // 特大暴雨
    {"313", "lightrain"}, // 冻雨
    {"314", "moderaterain"}, // 小到中雨
    {"315", "rainstorm"}, // 中到大雨
    {"316", "rainstorm"}, // 大到暴雨
    {"317", "rainstorm"}, // 暴雨到大暴雨
    {"318", "rainstorm"}, // 大暴雨到特大暴雨
    {"350", "thundershower"}, // 阵雨
    {"351", "thundershower"}, // 强阵雨
    {"399", "lightrain"}, // 雨
    {"400", "lightsnow"}, // 小雪
    {"401", "moderatesnow"}, // 中雪
    {"402", "major_snow"}, // 大雪
    {"403", "major_snow"}, // 暴雪
    {"404", "sleet"}, // 雨夹雪
    {"405", "sleet"}, // 雨雪天气
    {"406", "sleet"}, // 阵雨夹雪
    {"407", "sleet"}, // 阵雪
    {"408", "lightsnow"}, // 小到中雪
    {"409", "moderatesnow"}, // 中到大雪
    {"410", "major_snow"}, // 大到暴雪
    {"456", "sleet"}, // 阵雨夹雪
    {"457", "sleet"}, // 阵雪
    {"499", "lightsnow"}, // 雪
    {"500", "fog"}, // 薄雾
    {"501", "fog"}, // 雾
    {"502", "haze"}, // 霾
    {"503", "sandstorm"}, // 扬沙
    {"504", "sandstorm"}, // 浮尘
    {"507", "sandstorm"}, // 沙尘暴
    {"508", "sandstorm"}, // 强沙尘暴
    {"509", "fog"}, // 浓雾
    {"510", "fog"}, // 强浓雾
    {"511", "haze"}, // 中度霾
    {"512", "haze"}, // 重度霾
    {"513", "haze"}, // 严重霾
    {"514", "fog"}, // 大雾
    {"515", "fog"}, // 特强浓雾
    {"900", "sunny"}, // 热
    {"901", "cloudy"}, // 冷
    {"999", "nodata"}, // 未知
    {NULL, NULL}
};

static const char *get_weather_icon_by_code(const char *icon_code)
{
    if (!icon_code || icon_code[0] == '\0') return "nodata";
    for (int i = 0; qweather_icon_map[i].code != NULL; i++) {
        if (strcmp(qweather_icon_map[i].code, icon_code) == 0) {
            return qweather_icon_map[i].icon_name;
        }
    }
    return "nodata";
}

/* 获取天气图标资源 */
static const void *get_weather_icon_resource(const char *icon_name, int is_big)
{
    if (is_big) {
        if (strcmp(icon_name, "sunny") == 0) return vw_resource_get_img("icon_weather_big_sunny");
        if (strcmp(icon_name, "cloudy") == 0) return vw_resource_get_img("icon_weather_big_cloudy");
        if (strcmp(icon_name, "cloudytosunny") == 0) return vw_resource_get_img("icon_weather_big_cloudytosunny");
        if (strcmp(icon_name, "lightrain") == 0) return vw_resource_get_img("icon_weather_big_lightrain");
        if (strcmp(icon_name, "moderaterain") == 0) return vw_resource_get_img("icon_weather_big_moderaterain");
        if (strcmp(icon_name, "rainstorm") == 0) return vw_resource_get_img("icon_weather_big_rainstorm");
        if (strcmp(icon_name, "thundershower") == 0) return vw_resource_get_img("icon_weather_big_thundershower");
        if (strcmp(icon_name, "sleet") == 0) return vw_resource_get_img("icon_weather_big_sleet");
        if (strcmp(icon_name, "lightsnow") == 0) return vw_resource_get_img("icon_weather_big_lightsnow");
        if (strcmp(icon_name, "moderatesnow") == 0) return vw_resource_get_img("icon_weather_big_moderatesnow");
        if (strcmp(icon_name, "major_snow") == 0) return vw_resource_get_img("icon_weather_big_major_snow");
        if (strcmp(icon_name, "fog") == 0) return vw_resource_get_img("icon_weather_big_fog");
        if (strcmp(icon_name, "haze") == 0) return vw_resource_get_img("icon_weather_big_haze");
        if (strcmp(icon_name, "sandstorm") == 0) return vw_resource_get_img("icon_weather_big_sandstorm");
        if (strcmp(icon_name, "wind") == 0) return vw_resource_get_img("icon_weather_big_wind");
        if (strcmp(icon_name, "hail") == 0) return vw_resource_get_img("icon_weather_big_hail");
        return vw_resource_get_img("icon_weather_big_nodata");
    } else {
        if (strcmp(icon_name, "sunny") == 0) return vw_resource_get_img("icon_weather_small_sunny");
        if (strcmp(icon_name, "cloudy") == 0) return vw_resource_get_img("icon_weather_small_cloudy");
        if (strcmp(icon_name, "cloudytosunny") == 0) return vw_resource_get_img("icon_weather_small_cloudytosunny");
        if (strcmp(icon_name, "lightrain") == 0) return vw_resource_get_img("icon_weather_small_lightrain");
        if (strcmp(icon_name, "moderaterain") == 0) return vw_resource_get_img("icon_weather_small_moderaterain");
        if (strcmp(icon_name, "rainstorm") == 0) return vw_resource_get_img("icon_weather_small_rainstorm");
        if (strcmp(icon_name, "thundershower") == 0) return vw_resource_get_img("icon_weather_small_thundershower");
        if (strcmp(icon_name, "sleet") == 0) return vw_resource_get_img("icon_weather_small_sleet");
        if (strcmp(icon_name, "lightsnow") == 0) return vw_resource_get_img("icon_weather_small_lightsnow");
        if (strcmp(icon_name, "moderatesnow") == 0) return vw_resource_get_img("icon_weather_small_moderatesnow");
        if (strcmp(icon_name, "major_snow") == 0) return vw_resource_get_img("icon_weather_small_major_snow");
        if (strcmp(icon_name, "fog") == 0) return vw_resource_get_img("icon_weather_small_fog");
        if (strcmp(icon_name, "haze") == 0) return vw_resource_get_img("icon_weather_small_haze");
        if (strcmp(icon_name, "sandstorm") == 0) return vw_resource_get_img("icon_weather_small_sandstorm");
        if (strcmp(icon_name, "wind") == 0) return vw_resource_get_img("icon_weather_small_wind");
        if (strcmp(icon_name, "hail") == 0) return vw_resource_get_img("icon_weather_small_hail");
        return vw_resource_get_img("icon_weather_small_nodata");
    }
}

/* 网络状态监测结构体 */
typedef struct {
    int connect_attempts;           // 连接尝试次数
    int connect_failures;           // 连接失败次数
    int last_error;                 // 上次错误码
    time_t last_connect_time;       // 上次连接时间
    size_t total_bytes_received;    // 总接收字节数
    size_t total_bytes_sent;        // 总发送字节数
    int status;                     // 当前网络状态: 0=未知, 1=已连接, 2=连接中, 3=断开
} NetworkStats;

static NetworkStats *g_network_stats = NULL;  /* 堆分配节省 28B */

#define NETWORK_STATUS_UNKNOWN  0
#define NETWORK_STATUS_CONNECTED 1
#define NETWORK_STATUS_CONNECTING 2
#define NETWORK_STATUS_DISCONNECTED 3

static void __attribute__((unused)) network_stats_reset(void)
{
    if (g_network_stats == NULL) {
        g_network_stats = calloc(1, sizeof(NetworkStats));
        if (g_network_stats == NULL) return;
    }
    g_network_stats->connect_attempts = 0;
    g_network_stats->connect_failures = 0;
    g_network_stats->last_error = 0;
    g_network_stats->last_connect_time = 0;
    g_network_stats->total_bytes_received = 0;
    g_network_stats->total_bytes_sent = 0;
    g_network_stats->status = NETWORK_STATUS_UNKNOWN;
}

static void network_stats_log(void)
{
    if (g_network_stats == NULL) return;
    WEATHER_LOG("[WeatherNet] Status: %d, Attempts: %d, Failures: %d, "
           "LastErr: %d, Bytes RX/TX: %zu/%zu\n",
           g_network_stats->status,
           g_network_stats->connect_attempts,
           g_network_stats->connect_failures,
           g_network_stats->last_error,
           g_network_stats->total_bytes_received,
           g_network_stats->total_bytes_sent);
}

static void network_status_set(int status, int error)
{
    if (g_network_stats == NULL) {
        g_network_stats = calloc(1, sizeof(NetworkStats));
        if (g_network_stats == NULL) return;
    }
    g_network_stats->status = status;
    g_network_stats->last_error = error;
    if (status == NETWORK_STATUS_CONNECTED) {
        g_network_stats->last_connect_time = time(NULL);
        g_network_stats->connect_attempts++;
        WEATHER_LOG("[WeatherNet] Connection established (attempt %d)\n", 
               g_network_stats->connect_attempts);
    } else if (status == NETWORK_STATUS_DISCONNECTED) {
        g_network_stats->connect_failures++;
        WEATHER_LOG("[WeatherNet] Connection failed (attempt %d, err=%d)\n", 
               g_network_stats->connect_attempts, error);
    }
    network_stats_log();
}

/* 缓冲区配置 — 使用堆分配节省 DRAM (原静态分配 ~40KB) */
#define HTTP_RESP_BUF_SIZE 8192
#define HTTP_WGET_BUF_SIZE 4096

static char *g_http_resp_data = NULL;   /* 按需分配 */
static char *g_http_wget_buf = NULL;    /* 按需分配 */
static size_t g_http_resp_len = 0;

static void http_resp_buf_init_static(void)
{
    g_http_resp_len = 0;

    /* 按需分配 HTTP 响应缓冲区 */
    if (g_http_resp_data == NULL) {
        g_http_resp_data = malloc(HTTP_RESP_BUF_SIZE);
        if (g_http_resp_data == NULL) {
            WEATHER_LOG("[WeatherNet] Failed to alloc resp buffer\n");
            return;
        }
    }
    g_http_resp_data[0] = '\0';
}

static char* http_resp_buf_get_data(void)
{
    return g_http_resp_data;
}

static size_t http_resp_buf_get_len(void)
{
    return g_http_resp_len;
}

static int http_resp_buf_append(const char *data, size_t len)
{
    if (g_http_resp_len + len >= HTTP_RESP_BUF_SIZE) {
        WEATHER_LOG("[WeatherNet] HTTP buffer overflow! Current: %zu, Need: %zu\n", 
               g_http_resp_len, g_http_resp_len + len);
        return -ENOMEM;
    }
    memcpy(g_http_resp_data + g_http_resp_len, data, len);
    g_http_resp_len += len;
    g_http_resp_data[g_http_resp_len] = '\0';
    if (g_network_stats) g_network_stats->total_bytes_received += len;
    return 0;
}

/* webclient sink 回调：累积响应数据到静态缓冲区 */
static int http_sink_callback(FAR char **buffer, int offset,
                              int datend, FAR int *buflen, FAR void *arg)
{
    (void)arg;
    
    int chunk_len = datend - offset;
    if (chunk_len <= 0) {
        WEATHER_LOG("[WeatherNet] Empty chunk received\n");
        return 0;
    }

    WEATHER_LOG("[WeatherNet] Receiving chunk: %d bytes (offset=%d, datend=%d)\n", 
           chunk_len, offset, datend);
    
    int ret = http_resp_buf_append(*buffer + offset, chunk_len);
    if (ret != 0) {
        WEATHER_LOG("[WeatherNet] Buffer append failed: %d\n", ret);
    }
    return ret;
}

/* TLS 操作（在 weather_tls.c 中实现） */
extern const struct webclient_tls_ops weather_webclient_tls_ops;

extern int weather_gunzip(const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen);

static ssize_t maybe_gunzip(char **response, ssize_t resp_len)
{
    if (resp_len >= 2 && (unsigned char)(*response)[0] == 0x1f && (unsigned char)(*response)[1] == 0x8b) {
        /* 使用堆分配 gunzip 缓冲区（节省 16KB BSS） */
        char *gunzip_buf = malloc(HTTP_RESP_BUF_SIZE);
        if (gunzip_buf == NULL) {
            WEATHER_LOG("[Weather] gunzip malloc failed\n");
            return resp_len;
        }
        size_t glen = HTTP_RESP_BUF_SIZE;
        
        if (weather_gunzip((const uint8_t *)*response, resp_len, (uint8_t *)gunzip_buf, &glen) == 0) {
            if (glen < HTTP_RESP_BUF_SIZE) {
                memcpy(g_http_resp_data, gunzip_buf, glen);
                g_http_resp_len = glen;
                g_http_resp_data[glen] = '\0';
                WEATHER_LOG("[Weather] gunzip OK, len=%zu\n", glen);
                free(gunzip_buf);
                return (ssize_t)glen;
            } else {
                WEATHER_LOG("[Weather] gunzip buffer overflow\n");
            }
        } else {
            WEATHER_LOG("[Weather] gunzip decompression failed\n");
        }
        free(gunzip_buf);
    } else {
        WEATHER_LOG("[Weather] Not gzip compressed\n");
    }
    return resp_len;
}

/* 执行HTTP GET请求，返回响应体（静态缓冲区，无需free），返回响应长度 */
static ssize_t http_get(const char *url, char **response)
{
    WEATHER_LOG("[WeatherNet] ===== HTTP GET INIT =====\n");
    
    // 检查 WiFi 连接状态
    if (!settings_wifi_is_connected()) {
        WEATHER_LOG("[WeatherNet] WiFi NOT connected! Aborting HTTP request\n");
        network_status_set(NETWORK_STATUS_DISCONNECTED, -ENETUNREACH);
        *response = NULL;
        return -ENETUNREACH;
    }
    WEATHER_LOG("[WeatherNet] WiFi connected\n");

    // 重置网络状态统计和缓冲区
    network_status_set(NETWORK_STATUS_CONNECTING, 0);
    http_resp_buf_init_static();
    WEATHER_LOG("[WeatherNet] Buffer initialized\n");
    
    WEATHER_LOG("[WeatherNet] ===== HTTP GET START =====\n");
    WEATHER_LOG("[WeatherNet] URL: %s\n", url);
    WEATHER_LOG("[WeatherNet] Timestamp: %lu\n", (unsigned long)time(NULL));

    struct webclient_context ctx;
    webclient_set_defaults(&ctx);
    ctx.method = "GET";
    ctx.url = url;

    /* 按需分配 wget 缓冲区 */
    if (g_http_wget_buf == NULL) {
        g_http_wget_buf = malloc(HTTP_WGET_BUF_SIZE);
        if (g_http_wget_buf == NULL) {
            WEATHER_LOG("[WeatherNet] Failed to alloc wget buffer\n");
            *response = NULL;
            return -ENOMEM;
        }
    }
    ctx.buffer = g_http_wget_buf;
    ctx.buflen = HTTP_WGET_BUF_SIZE;
    ctx.sink_callback = http_sink_callback;
    ctx.sink_callback_arg = NULL;
    ctx.timeout_sec = 15;
    ctx.protocol_version = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;
    ctx.tls_ops = &weather_webclient_tls_ops;

    WEATHER_LOG("[WeatherNet] Starting webclient_perform...\n");
    int ret = webclient_perform(&ctx);
    WEATHER_LOG("[WeatherNet] webclient_perform completed, ret=%d\n", ret);

    if (ret != 0) {
        WEATHER_LOG("[WeatherNet] HTTP request FAILED: ret=%d\n", ret);
        network_status_set(NETWORK_STATUS_DISCONNECTED, ret);
        *response = NULL;
        WEATHER_LOG("[WeatherNet] ===== HTTP GET END (FAILED) =====\n");
        return -1;
    }

    WEATHER_LOG("[WeatherNet] HTTP status: %u\n", ctx.http_status);
    WEATHER_LOG("[WeatherNet] Response length: %zu bytes\n", http_resp_buf_get_len());
    
    if (ctx.http_status >= 200 && ctx.http_status < 300) {
        network_status_set(NETWORK_STATUS_CONNECTED, 0);
    } else {
        WEATHER_LOG("[WeatherNet] HTTP error status: %u\n", ctx.http_status);
        network_status_set(NETWORK_STATUS_DISCONNECTED, ctx.http_status);
    }

    *response = http_resp_buf_get_data();
    ssize_t resp_len = (ssize_t)http_resp_buf_get_len();
    
    WEATHER_LOG("[WeatherNet] ===== HTTP GET END (SUCCESS) =====\n");
    
    // 添加短延迟，确保连接完全关闭
    usleep(50000);  // 50ms
    
    return resp_len;
}

/* 获取位置信息 */
int weather_get_location(LocationInfo *info)
{
    if (!info) {
        return -1;
    }

    strncpy(info->location_id, g_weather_location_id, sizeof(info->location_id) - 1);
    strncpy(info->city, g_weather_city, sizeof(info->city) - 1);
    strncpy(info->district, "", sizeof(info->district) - 1);

    WEATHER_LOG("[Weather] Location: %s, %s\n", info->location_id, info->city);

    return 0;
}

static void debug_print_response(const char *tag, const char *data, size_t len)
{
    WEATHER_LOG("[Weather] === %s response (len=%zu) ===\n", tag, len);
    if (!data || len == 0) { WEATHER_LOG("(empty)\n"); return; }
    for (size_t i = 0; i < len; i += 64) {
        size_t chunk = len - i;
        if (chunk > 64) chunk = 64;
        for (size_t j = 0; j < chunk; j++) {
            unsigned char c = (unsigned char)data[i + j];
            if (c >= 0x20 && c < 0x7f) putchar(c);
            else WEATHER_LOG("\\x%02x", c);
        }
        putchar('\n');
    }
    WEATHER_LOG("[Weather] === end %s ===\n", tag);
}

/* 从和风天气API获取数据 */
int weather_get_data(WeatherData *data)
{
    if (!data) {
        return -1;
    }
    
    // 初始化数据
    memset(data, 0, sizeof(WeatherData));
    
    // 确保位置信息已初始化
    if (!g_location_initialized) {
        if (g_location == NULL) {
            g_location = calloc(1, sizeof(LocationInfo));
            if (g_location == NULL) return -1;
        }
        if (weather_get_location(g_location) != 0) {
            WEATHER_LOG("Failed to get location\n");
            return -1;
        }
        g_location_initialized = 1;
    }
    
    // 1. 获取实时天气
    char url[256];
    char *response = NULL;
    ssize_t resp_len;
    
    sprintf(url, "%s/weather/now?location=%s&key=%s", 
            QWEATHER_BASE_URL, g_weather_location_id, QWEATHER_API_KEY);
    
    resp_len = http_get(url, &response);
    if (resp_len <= 0) {
        WEATHER_LOG("Failed to get current weather\n");
        return -1;
    }
    
    resp_len = maybe_gunzip(&response, resp_len);
    
    cJSON *root = cJSON_Parse(response);
    if (root) {
        cJSON *code_obj = cJSON_GetObjectItem(root, "code");
        if (code_obj && cJSON_IsString(code_obj) &&
            strcmp(code_obj->valuestring, "200") == 0) {

            cJSON *now_obj = cJSON_GetObjectItem(root, "now");
            if (now_obj) {
                cJSON *temp_obj = cJSON_GetObjectItem(now_obj, "temp");
                if (temp_obj && cJSON_IsString(temp_obj)) {
                    data->temp = atoi(temp_obj->valuestring);
                }

                cJSON *text_obj = cJSON_GetObjectItem(now_obj, "text");
                if (text_obj && cJSON_IsString(text_obj)) {
                    strncpy(data->weather, text_obj->valuestring, sizeof(data->weather) - 1);
                }

                cJSON *icon_obj = cJSON_GetObjectItem(now_obj, "icon");
                if (icon_obj && cJSON_IsString(icon_obj)) {
                    strncpy(data->icon, get_weather_icon_by_code(icon_obj->valuestring),
                            sizeof(data->icon) - 1);
                } else {
                    strncpy(data->icon, "nodata", sizeof(data->icon) - 1);
                }
            }
        }

        cJSON_Delete(root);
    }
    
    // 静态缓冲区，无需free
    response = NULL;
    
    // 请求间隔延迟，避免频繁连接
    usleep(100000);  // 100ms
    
    // 2. 获取逐小时天气预报
    sprintf(url, "%s/weather/24h?location=%s&key=%s", 
            QWEATHER_BASE_URL, g_weather_location_id, QWEATHER_API_KEY);
    
    resp_len = http_get(url, &response);
    if (resp_len <= 0) {
        WEATHER_LOG("Failed to get hourly weather\n");
        return -1;
    }
    
    resp_len = maybe_gunzip(&response, resp_len);
    
    // 解析逐小时天气数据
    root = cJSON_Parse(response);
    if (root) {
        cJSON *code_obj = cJSON_GetObjectItem(root, "code");
        if (code_obj && cJSON_IsString(code_obj) && 
            strcmp(code_obj->valuestring, "200") == 0) {
            
            cJSON *hourly_obj = cJSON_GetObjectItem(root, "hourly");
            if (hourly_obj && cJSON_IsArray(hourly_obj)) {
                int count = cJSON_GetArraySize(hourly_obj);
                data->hourly_count = count > 24 ? 24 : count;
                
                for (int i = 0; i < data->hourly_count; i++) {
                    cJSON *item = cJSON_GetArrayItem(hourly_obj, i);
                    if (item) {
                        cJSON *fxtime_obj = cJSON_GetObjectItem(item, "fxTime");
                        if (fxtime_obj && cJSON_IsString(fxtime_obj)) {
                            const char *fxtime = fxtime_obj->valuestring;
                            // 提取时间部分 (HH:MM)
                            strncpy(data->hourly[i].time, fxtime + 11, 5);
                            data->hourly[i].time[5] = '\0';
                        }
                        
                        cJSON *temp_obj = cJSON_GetObjectItem(item, "temp");
                        if (temp_obj && cJSON_IsString(temp_obj)) {
                            data->hourly[i].temp = atoi(temp_obj->valuestring);
                        }
                        
                        cJSON *text_obj = cJSON_GetObjectItem(item, "text");
                        if (text_obj && cJSON_IsString(text_obj)) {
                            strncpy(data->hourly[i].weather, text_obj->valuestring,
                                    sizeof(data->hourly[i].weather) - 1);
                        }

                        cJSON *icon_obj = cJSON_GetObjectItem(item, "icon");
                        if (icon_obj && cJSON_IsString(icon_obj)) {
                            strncpy(data->hourly[i].icon,
                                    get_weather_icon_by_code(icon_obj->valuestring),
                                    sizeof(data->hourly[i].icon) - 1);
                        } else {
                            strncpy(data->hourly[i].icon, "nodata",
                                    sizeof(data->hourly[i].icon) - 1);
                        }
                    }
                }
            }
        }
        
        cJSON_Delete(root);
    }
    
    // 静态缓冲区，无需free
    response = NULL;
    
    // 请求间隔延迟
    usleep(100000);  // 100ms
    
    // 3. 获取7天天气预报
    sprintf(url, "%s/weather/7d?location=%s&key=%s", 
            QWEATHER_BASE_URL, g_weather_location_id, QWEATHER_API_KEY);
    
    resp_len = http_get(url, &response);
    if (resp_len <= 0) {
        WEATHER_LOG("Failed to get daily weather\n");
        return -1;
    }
    
    resp_len = maybe_gunzip(&response, resp_len);
    
    // 解析7天天气数据
    root = cJSON_Parse(response);
    if (root) {
        cJSON *code_obj = cJSON_GetObjectItem(root, "code");
        if (code_obj && cJSON_IsString(code_obj) && 
            strcmp(code_obj->valuestring, "200") == 0) {
            
            cJSON *daily_obj = cJSON_GetObjectItem(root, "daily");
            if (daily_obj && cJSON_IsArray(daily_obj)) {
                int count = cJSON_GetArraySize(daily_obj);
                data->daily_count = count > 7 ? 7 : count;
                
                for (int i = 0; i < data->daily_count; i++) {
                    cJSON *item = cJSON_GetArrayItem(daily_obj, i);
                    if (item) {
                        cJSON *fxdate_obj = cJSON_GetObjectItem(item, "fxDate");
                        if (fxdate_obj && cJSON_IsString(fxdate_obj)) {
                            if (i == 0) {
                                strcpy(data->daily[i].day, "今天");
                            } else if (i == 1) {
                                strcpy(data->daily[i].day, "明天");
                            } else {
                                const char *weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
                                const char *d = fxdate_obj->valuestring;
                                int y = 0, m = 0, dd = 0;
                                if (sscanf(d, "%d-%d-%d", &y, &m, &dd) == 3 && m >= 1 && m <= 12) {
                                    if (m < 3) { m += 12; y--; }
                                    int w = (dd + 2 * m + 3 * (m + 1) / 5 + y + y / 4 - y / 100 + y / 400 + 1) % 7;
                                    strcpy(data->daily[i].day, weekdays[w]);
                                } else {
                                    const char *fallback[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
                                    strcpy(data->daily[i].day, fallback[(i - 2) % 7]);
                                }
                            }
                        }
                        
                        cJSON *tempmax_obj = cJSON_GetObjectItem(item, "tempMax");
                        if (tempmax_obj && cJSON_IsString(tempmax_obj)) {
                            data->daily[i].temp_max = atoi(tempmax_obj->valuestring);
                        }
                        
                        cJSON *tempmin_obj = cJSON_GetObjectItem(item, "tempMin");
                        if (tempmin_obj && cJSON_IsString(tempmin_obj)) {
                            data->daily[i].temp_min = atoi(tempmin_obj->valuestring);
                        }
                        
                        cJSON *textday_obj = cJSON_GetObjectItem(item, "textDay");
                        if (textday_obj && cJSON_IsString(textday_obj)) {
                            strncpy(data->daily[i].weather, textday_obj->valuestring,
                                    sizeof(data->daily[i].weather) - 1);
                        }

                        cJSON *iconday_obj = cJSON_GetObjectItem(item, "iconDay");
                        if (iconday_obj && cJSON_IsString(iconday_obj)) {
                            strncpy(data->daily[i].icon,
                                    get_weather_icon_by_code(iconday_obj->valuestring),
                                    sizeof(data->daily[i].icon) - 1);
                        } else {
                            strncpy(data->daily[i].icon, "nodata",
                                    sizeof(data->daily[i].icon) - 1);
                        }
                    }
                }
            }
        }
        
        cJSON_Delete(root);
    }
    
    // 静态缓冲区，无需free
    
    WEATHER_LOG("[Weather] Weather data retrieved successfully\n");
    return 0;
}

/* 日/周切换回调 */
static void mode_switch_callback(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    
    if (btn == day_btn) {
        current_mode = 0;
        lv_obj_add_style(day_btn, style_btn_selected, 0);
        lv_obj_remove_style(day_btn, style_btn_normal, 0);
        lv_obj_add_style(week_btn, style_btn_normal, 0);
        lv_obj_remove_style(week_btn, style_btn_selected, 0);
        if (g_data_shown) {
            lv_obj_clear_flag(hourly_container, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(daily_container, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (btn == week_btn) {
        current_mode = 1;
        lv_obj_add_style(week_btn, style_btn_selected, 0);
        lv_obj_remove_style(week_btn, style_btn_normal, 0);
        lv_obj_add_style(day_btn, style_btn_normal, 0);
        lv_obj_remove_style(day_btn, style_btn_selected, 0);
        if (g_data_shown) {
            lv_obj_add_flag(hourly_container, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(daily_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 检查天气数据是否就绪并刷新UI */
static void weather_ui_refresh_timer_cb(lv_timer_t *timer)
{
    time_t now = time(NULL);

    // 检查 WiFi 连接状态
    bool wifi_connected = settings_wifi_is_connected();
    
    if (!g_weather_fetching && !g_data_shown) {
        if (!wifi_connected) {
            if (loading_label) {
                lv_label_set_text(loading_label, "未连接WiFi");
                lv_obj_clear_flag(loading_label, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
        
        if (g_last_fetch_time == 0 || (now - g_last_fetch_time) >= WEATHER_FETCH_INTERVAL_SEC) {
            start_weather_fetch();
        }
    }

    if (g_data_shown) return;

    if (g_weather_fetching) {
        if ((now - g_enter_time) >= WEATHER_TIMEOUT_SEC) {
            g_weather_fetching = 0;
            if (loading_label) {
                lv_label_set_text(loading_label, "无数据");
                lv_obj_clear_flag(loading_label, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
        if (loading_label) {
            lv_label_set_text(loading_label, "加载中..........");
            lv_obj_clear_flag(loading_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (WD.hourly_count == 0 && WD.daily_count == 0 &&
        WD.temp == 0 && WD.weather[0] == '\0') {
        if (loading_label) {
            lv_label_set_text(loading_label, "无数据");
            lv_obj_clear_flag(loading_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    g_data_shown = 1;

    if (loading_label) lv_obj_add_flag(loading_label, LV_OBJ_FLAG_HIDDEN);
    if (weather_icon) lv_obj_clear_flag(weather_icon, LV_OBJ_FLAG_HIDDEN);
    if (temp_label) lv_obj_clear_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
    if (weather_unit_label) lv_obj_clear_flag(weather_unit_label, LV_OBJ_FLAG_HIDDEN);
    if (current_mode == 0) {
        if (hourly_container) lv_obj_clear_flag(hourly_container, LV_OBJ_FLAG_HIDDEN);
        if (daily_container) lv_obj_add_flag(daily_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (hourly_container) lv_obj_add_flag(hourly_container, LV_OBJ_FLAG_HIDDEN);
        if (daily_container) lv_obj_clear_flag(daily_container, LV_OBJ_FLAG_HIDDEN);
    }

    char temp_str[10];
    sprintf(temp_str, "%d", WD.temp);
    lv_label_set_text(temp_label, temp_str);

    const void *icon_resource = get_weather_icon_resource(WD.icon, 1);
    lv_img_set_src(weather_icon, icon_resource);

    if (WD.hourly_count > 0 && hourly_container) {
        lv_obj_t *content = lv_obj_get_child(hourly_container, 0);
        WEATHER_LOG("[Weather] hourly: count=%d, content=%p\n", WD.hourly_count, content);
        if (content) lv_obj_set_width(content, WD.hourly_count * 88);
        int count = WD.hourly_count > 24 ? 24 : WD.hourly_count;
        for (int i = 0; i < count; i++) {
            lv_obj_t *item = lv_obj_get_child(content, i);
            if (!item) { WEATHER_LOG("[Weather] hourly item[%d] NULL\n", i); continue; }
            lv_obj_t *time_lbl = lv_obj_get_child(item, 0);
            lv_obj_t *icon_img = lv_obj_get_child(item, 1);
            lv_obj_t *temp_lbl = lv_obj_get_child(item, 2);
            if (time_lbl) lv_label_set_text(time_lbl, WD.hourly[i].time);
            if (icon_img) lv_img_set_src(icon_img, get_weather_icon_resource(WD.hourly[i].icon, 0));
            if (temp_lbl) {
                char t[8]; sprintf(t, "%d°C", WD.hourly[i].temp);
                lv_label_set_text(temp_lbl, t);
            }
            WEATHER_LOG("[Weather] hourly[%d]: time=%s icon=%s temp=%d\n",
                   i, WD.hourly[i].time, WD.hourly[i].icon, WD.hourly[i].temp);
        }
    } else {
        WEATHER_LOG("[Weather] hourly: NO DATA (count=%d, container=%p)\n",
               WD.hourly_count, hourly_container);
    }

    if (WD.daily_count > 0 && daily_container) {
        lv_obj_t *content = lv_obj_get_child(daily_container, 0);
        WEATHER_LOG("[Weather] daily: count=%d, content=%p\n", WD.daily_count, content);
        if (content) lv_obj_set_width(content, WD.daily_count * 88);
        int count = WD.daily_count > 7 ? 7 : WD.daily_count;
        for (int i = 0; i < count; i++) {
            lv_obj_t *item = lv_obj_get_child(content, i);
            if (!item) { WEATHER_LOG("[Weather] daily item[%d] NULL\n", i); continue; }
            lv_obj_t *day_lbl = lv_obj_get_child(item, 0);
            lv_obj_t *icon_img = lv_obj_get_child(item, 1);
            lv_obj_t *temp_lbl = lv_obj_get_child(item, 2);
            if (day_lbl) lv_label_set_text(day_lbl, WD.daily[i].day);
            if (icon_img) lv_img_set_src(icon_img, get_weather_icon_resource(WD.daily[i].icon, 0));
            if (temp_lbl) {
                char t[20]; sprintf(t, "%d/%d°C", WD.daily[i].temp_min, WD.daily[i].temp_max);
                lv_label_set_text(temp_lbl, t);
            }
            WEATHER_LOG("[Weather] daily[%d]: day=%s icon=%s hi=%d lo=%d\n",
                   i, WD.daily[i].day, WD.daily[i].icon,
                   WD.daily[i].temp_max, WD.daily[i].temp_min);
        }
    } else {
        WEATHER_LOG("[Weather] daily: NO DATA (count=%d, container=%p)\n",
               WD.daily_count, daily_container);
    }

    WEATHER_LOG("[Weather] UI refreshed: temp=%d, icon=%s, hourly=%d, daily=%d\n",
           WD.temp, WD.icon,
           WD.hourly_count, WD.daily_count);
}

/* 异步获取天气数据的线程 */
static void *weather_fetch_thread(void *arg)
{
    WEATHER_LOG("[Weather] Fetch thread started\n");
    g_weather_fetching = 1;

    int ret = weather_get_data(weather_data_get());
    if (ret == 0) {
        WEATHER_LOG("[Weather] Data fetched: temp=%d, weather=%s, icon=%s, hourly=%d, daily=%d\n",
               WD.temp, WD.weather, WD.icon,
               WD.hourly_count, WD.daily_count);
        for (int i = 0; i < WD.hourly_count && i < 3; i++) {
            WEATHER_LOG("[Weather]  hourly[%d]: time=%s temp=%d icon=%s\n",
                   i, WD.hourly[i].time, WD.hourly[i].temp, WD.hourly[i].icon);
        }
        for (int i = 0; i < WD.daily_count && i < 3; i++) {
            WEATHER_LOG("[Weather]  daily[%d]: day=%s hi=%d lo=%d icon=%s\n",
                   i, WD.daily[i].day, WD.daily[i].temp_max,
                   WD.daily[i].temp_min, WD.daily[i].icon);
        }
        g_last_fetch_time = time(NULL);
    } else {
        WEATHER_LOG("[Weather] Failed to fetch weather data\n");
        memset(weather_data_get(), 0, sizeof(WeatherData));
    }

    g_weather_fetching = 0;
    return NULL;
}

/* 启动异步天气数据获取 */
static void start_weather_fetch(void)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    
    // 设置线程栈大小为24KB，足够支持TLS握手（与 voice_channel.c CONV_THREAD_STACK 一致）
    const size_t stack_size = 24 * 1024;
    pthread_attr_setstacksize(&attr, stack_size);
    
    pthread_t tid;
    int ret = pthread_create(&tid, &attr, weather_fetch_thread, NULL);
    pthread_attr_destroy(&attr);
    
    if (ret != 0) {
        WEATHER_LOG("[Weather] Failed to create fetch thread: errno=%d\n", errno);
        return;
    }
    pthread_detach(tid);
    WEATHER_LOG("[Weather] Fetch thread created with stack size: %zu bytes\n", stack_size);
}



static void weather_app_create(void)
{
    WEATHER_LOG("weather_app_create\n");

    /* 初始化默认位置（堆分配） */
    if (g_weather_location_id == NULL) {
        g_weather_location_id = calloc(1, 32);
        g_weather_city = calloc(1, 64);
    }
    if (g_weather_location_id[0] == '\0') {
        strncpy(g_weather_location_id, QWEATHER_DEFAULT_LOCATION, 31);
    }
    if (g_weather_city[0] == '\0') {
        strncpy(g_weather_city, QWEATHER_DEFAULT_CITY, 63);
    }
    
    g_enter_time = time(NULL);
    g_data_shown = 0;
    
    // 创建屏幕
    weather_main_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(weather_main_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(weather_main_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(weather_main_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(weather_main_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(weather_main_base, LV_ALIGN_CENTER, 0, 0);
    
    // 获取真实天气数据（异步）
    memset(weather_data_get(), 0, sizeof(WeatherData));
    start_weather_fetch();

    /* 堆分配样式 */
    if (style_btn_normal == NULL) {
        style_btn_normal = calloc(1, sizeof(lv_style_t));
        style_btn_selected = calloc(1, sizeof(lv_style_t));
    }
    lv_style_init(style_btn_normal);
    lv_style_set_width(style_btn_normal, 80);
    lv_style_set_height(style_btn_normal, 60);
    lv_style_set_radius(style_btn_normal, 21);
    lv_style_set_bg_color(style_btn_normal, lv_color_hex(0x1A1A1A));
    lv_style_set_text_color(style_btn_normal, lv_color_hex(0xFFFFFF));
    lv_style_set_text_align(style_btn_normal, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_font(style_btn_normal, vw_resource_get_font(WATCH_REGULAR_FONT "_32"));
    
    lv_style_init(style_btn_selected);
    lv_style_set_width(style_btn_selected, 80);
    lv_style_set_height(style_btn_selected, 60);
    lv_style_set_radius(style_btn_selected, 21);
    lv_style_set_bg_color(style_btn_selected, lv_color_hex(0x2D47CB));
    lv_style_set_text_color(style_btn_selected, lv_color_hex(0xFFFFFF));
    lv_style_set_text_align(style_btn_selected, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_font(style_btn_selected, vw_resource_get_font(WATCH_REGULAR_FONT "_32"));
    
    // 创建日按钮
    day_btn = lv_btn_create(weather_main_base);
    lv_obj_set_pos(day_btn, 113, 40);
    lv_obj_add_style(day_btn, style_btn_selected, 0);
    lv_obj_add_event_cb(day_btn, mode_switch_callback, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_shadow_width(day_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *day_label = lv_label_create(day_btn);
    lv_label_set_text(day_label, "日");
    lv_obj_center(day_label);
    
    // 创建周按钮
    week_btn = lv_btn_create(weather_main_base);
    lv_obj_set_pos(week_btn, 217, 40);
    lv_obj_add_style(week_btn, style_btn_normal, 0);
    lv_obj_add_event_cb(week_btn, mode_switch_callback, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_shadow_width(week_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *week_label = lv_label_create(week_btn);
    lv_label_set_text(week_label, "周");
    lv_obj_center(week_label);
    
    // 创建天气图标
    weather_icon = lv_img_create(weather_main_base);
    lv_obj_set_size(weather_icon, 100, 100);
    lv_obj_set_pos(weather_icon, 67, 158);
    const void *icon_resource = get_weather_icon_resource(WD.icon, 1);
    lv_img_set_src(weather_icon, icon_resource);
    lv_obj_add_flag(weather_icon, LV_OBJ_FLAG_HIDDEN);
    
    // 创建温度标签
    temp_label = lv_label_create(weather_main_base);
    // lv_obj_set_pos(temp_label, 205, 141);
    lv_obj_set_style_text_font(temp_label, vw_resource_get_font(WATCH_REGULAR_FONT "_96"), 0);
    lv_obj_set_style_text_color(temp_label, lv_color_hex(0xFFFFFF), 0);
    // 显示天气数据
    char temp_str[10];
    sprintf(temp_str, "%d", WD.temp);
    lv_label_set_text(temp_label, temp_str);
    lv_obj_align(temp_label, LV_ALIGN_TOP_RIGHT, -85, 141);
    lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);

    weather_unit_label = lv_label_create(weather_main_base);
    lv_label_set_text(weather_unit_label, "°C");
    lv_obj_set_style_text_font(weather_unit_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(weather_unit_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align_to(weather_unit_label, temp_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -24);
    lv_obj_add_flag(weather_unit_label, LV_OBJ_FLAG_HIDDEN);
    
    // 创建天气信息标签
    // weather_label = lv_label_create(weather_main_base);
    // lv_obj_set_pos(weather_label, 205, 220);
    // lv_style_init(&style_weather_label);
    // lv_style_set_text_font(&style_weather_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"));
    // lv_style_set_text_color(&style_weather_label, lv_color_hex(0xFFFFFF));
    // lv_obj_add_style(weather_label, &style_weather_label, 0);
    
    // 创建小时天气滚动容器
    hourly_container = lv_obj_create(weather_main_base);
    lv_obj_set_size(hourly_container, 352, 122);
    lv_obj_align(hourly_container, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_set_style_bg_opa(hourly_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(hourly_container, 0, 0);
    lv_obj_set_style_margin_all(hourly_container, 0, 0);
    lv_obj_set_style_border_width(hourly_container, 0, 0);
    lv_obj_set_scroll_dir(hourly_container, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(hourly_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(hourly_container, LV_OBJ_FLAG_HIDDEN);

    // 创建小时天气内容容器
    lv_obj_t *hourly_content = lv_obj_create(hourly_container);
    lv_obj_set_size(hourly_content, 24 * 88, 122);
    lv_obj_set_pos(hourly_content, 0, 0);
    lv_obj_set_style_bg_opa(hourly_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(hourly_content, 0, 0);
    lv_obj_set_style_margin_all(hourly_content, 0, 0);
    lv_obj_set_style_border_width(hourly_content, 0, 0);
    lv_obj_clear_flag(hourly_content, LV_OBJ_FLAG_SCROLLABLE);
    
    // 创建周天气滚动容器
    daily_container = lv_obj_create(weather_main_base);
    lv_obj_set_size(daily_container, 352, 122);
    lv_obj_align(daily_container, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_set_style_bg_opa(daily_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(daily_container, 0, 0);
    lv_obj_set_style_margin_all(daily_container, 0, 0);
    lv_obj_set_style_border_width(daily_container, 0, 0);
    lv_obj_set_scroll_dir(daily_container, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(daily_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(daily_container, LV_OBJ_FLAG_HIDDEN);
    
    // 创建周天气内容容器
    lv_obj_t *daily_content = lv_obj_create(daily_container);
    lv_obj_set_size(daily_content, 7 * 88, 122);
    lv_obj_set_pos(daily_content, 0, 0);
    lv_obj_set_style_bg_opa(daily_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(daily_content, 0, 0);
    lv_obj_set_style_margin_all(daily_content, 0, 0);
    lv_obj_set_style_border_width(daily_content, 0, 0);
    lv_obj_clear_flag(daily_content, LV_OBJ_FLAG_SCROLLABLE);
    
    // 创建24小时天气子项（固定数量，数据到来后填充）
    for (int i = 0; i < 24; i++) {
        lv_obj_t *item = lv_obj_create(hourly_content);
        lv_obj_set_size(item, 88, 122);
        lv_obj_align(item, LV_ALIGN_TOP_LEFT, i * 88, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_style_margin_all(item, 0, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_scrollbar_mode(item, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t *time_label = lv_label_create(item);
        lv_obj_set_size(time_label, 64, 28);
        lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 0);
        lv_label_set_text(time_label, "--:--");
        lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(time_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
        
        lv_obj_t *hourly_icon = lv_img_create(item);
        lv_obj_set_size(hourly_icon, 50, 50);
        lv_obj_align(hourly_icon, LV_ALIGN_TOP_MID, 0, 35);
        lv_img_set_src(hourly_icon, get_weather_icon_resource("nodata", 0));
        
        lv_obj_t *temp_hourly_label = lv_label_create(item);
        lv_obj_set_size(temp_hourly_label, 64, 28);
        lv_obj_align(temp_hourly_label, LV_ALIGN_TOP_MID, 0, 93);
        lv_label_set_text(temp_hourly_label, "--°C");
        lv_obj_set_style_text_color(temp_hourly_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(temp_hourly_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_set_style_text_align(temp_hourly_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    
    // 创建7天天气子项（固定数量，数据到来后填充）
    for (int i = 0; i < 7; i++) {
        lv_obj_t *item = lv_obj_create(daily_content);
        lv_obj_set_size(item, 88, 122);
        lv_obj_align(item, LV_ALIGN_TOP_LEFT, i * 88, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_style_margin_all(item, 0, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_scrollbar_mode(item, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t *daily_day_label = lv_label_create(item);
        lv_obj_set_size(daily_day_label, 64, 28);
        lv_obj_align(daily_day_label, LV_ALIGN_TOP_MID, 0, 0);
        lv_label_set_text(daily_day_label, "--");
        lv_obj_set_style_text_color(daily_day_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(daily_day_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_set_style_text_align(daily_day_label, LV_TEXT_ALIGN_CENTER, 0);
        
        lv_obj_t *daily_icon = lv_img_create(item);
        lv_obj_set_size(daily_icon, 50, 50);
        lv_obj_align(daily_icon, LV_ALIGN_TOP_MID, 0, 35);
        lv_img_set_src(daily_icon, get_weather_icon_resource("nodata", 0));
        
        lv_obj_t *temp_daily_label = lv_label_create(item);
        lv_obj_set_size(temp_daily_label, 64, 28);
        lv_obj_align(temp_daily_label, LV_ALIGN_TOP_MID, 0, 93);
        lv_label_set_text(temp_daily_label, "--°C");
        lv_obj_set_style_text_color(temp_daily_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(temp_daily_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_set_style_text_align(temp_daily_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    
    // 创建加载标签
    loading_label = lv_label_create(weather_main_base);
    lv_label_set_text(loading_label, "加载中..........");
    lv_obj_set_style_text_color(loading_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(loading_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_center(loading_label);
    
    // 设置UI刷新定时器（每2秒检查数据就绪，每30分钟重新获取）
    update_timer = lv_timer_create(weather_ui_refresh_timer_cb, 2000, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(weather_main_base);

    // 设置手势事件
    lv_obj_add_event_cb(weather_main_base, slide_gesture_handler, LV_EVENT_ALL, NULL);
}

/**
 * 滑动手势处理 - 右滑返回
 */
 static void slide_gesture_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    static lv_point_t start_point = {0};
    static bool is_dragging = false;

    switch(code) {
        case LV_EVENT_PRESSED:
            lv_indev_get_point(lv_indev_active(), &start_point);
            is_dragging = true;
            break;
        
        case LV_EVENT_RELEASED:
            if(is_dragging) {
                lv_point_t end_point;
                lv_indev_get_point(lv_indev_active(), &end_point);
                
                int32_t delta_x = end_point.x - start_point.x;
                int32_t delta_y = end_point.y - start_point.y;

                if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                    WEATHER_LOG("right swipe, exit calendar\n");
                    if (update_timer) {
                        lv_timer_del(update_timer);
                        update_timer = NULL;
                    }
                    if(weather_main_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(weather_main_base);
                        lv_obj_del(weather_main_base);
                        weather_main_base = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

void weather_app_click_callback(lv_event_t *e)
{
    WEATHER_LOG("weather_app_click_callback\n");
    weather_app_create();
}

void weather_update_location(void)
{
    WEATHER_LOG("[Weather] Updating location via IP...\n");

    char *response = NULL;
    ssize_t resp_len;

    resp_len = http_get("http://ip-api.com/json", &response);
    if (resp_len <= 0) {
        WEATHER_LOG("[Weather] Failed to get IP location\n");
        return;
    }

    resp_len = maybe_gunzip(&response, resp_len);
    WEATHER_LOG("[Weather] IP location: %s\n", response);

    char city_en[32] = {0};
    char region_en[32] = {0};

    cJSON *root = cJSON_Parse(response);
    // 静态缓冲区，无需free
    response = NULL;

    if (!root) {
        WEATHER_LOG("[Weather] Failed to parse IP location JSON\n");
        return;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status && cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0) {
        cJSON *city_obj = cJSON_GetObjectItem(root, "city");
        cJSON *region_obj = cJSON_GetObjectItem(root, "regionName");

        if (city_obj && cJSON_IsString(city_obj))
            strncpy(city_en, city_obj->valuestring, sizeof(city_en) - 1);
        if (region_obj && cJSON_IsString(region_obj))
            strncpy(region_en, region_obj->valuestring, sizeof(region_en) - 1);
    }
    cJSON_Delete(root);

    WEATHER_LOG("[Weather] Detected: city=%s, region=%s\n", city_en, region_en);

    if (city_en[0] == '\0') {
        WEATHER_LOG("[Weather] Failed to get city from IP location\n");
        return;
    }

    char url[256];
    if (region_en[0])
        snprintf(url, sizeof(url), "%s?location=%s&adm=%s&key=%s",
                 QWEATHER_GEO_URL, city_en, region_en, QWEATHER_API_KEY);
    else
        snprintf(url, sizeof(url), "%s?location=%s&key=%s",
                 QWEATHER_GEO_URL, city_en, QWEATHER_API_KEY);

    WEATHER_LOG("[Weather] GeoAPI URL: %s\n", url);

    resp_len = http_get(url, &response);
    if (resp_len <= 0) {
        WEATHER_LOG("[Weather] GeoAPI failed, keeping default location\n");
        return;
    }

    resp_len = maybe_gunzip(&response, resp_len);

    root = cJSON_Parse(response);
    if (root) {
        cJSON *code_obj = cJSON_GetObjectItem(root, "code");
        if (code_obj && cJSON_IsString(code_obj) &&
            strcmp(code_obj->valuestring, "200") == 0) {

            cJSON *locations = cJSON_GetObjectItem(root, "location");
            if (locations && cJSON_IsArray(locations)) {
                cJSON *first = cJSON_GetArrayItem(locations, 0);
                if (first) {
                    cJSON *id_obj = cJSON_GetObjectItem(first, "id");
                    cJSON *name_obj = cJSON_GetObjectItem(first, "name");

                    if (id_obj && cJSON_IsString(id_obj)) {
                        strncpy(g_weather_location_id, id_obj->valuestring, 31);
                        g_weather_location_id[31] = '\0';
                    }
                    if (name_obj && cJSON_IsString(name_obj)) {
                        strncpy(g_weather_city, name_obj->valuestring, 63);
                        g_weather_city[63] = '\0';
                    }

                    WEATHER_LOG("[Weather] Location updated: %s (%s)\n",
                           g_weather_city, g_weather_location_id);
                }
            }
        } else {
            WEATHER_LOG("[Weather] GeoAPI returned non-200, keeping default\n");
        }
        cJSON_Delete(root);
    } else {
        WEATHER_LOG("[Weather] GeoAPI JSON parse failed\n");
    }

    // 静态缓冲区，无需free
    g_location_initialized = 0;
}

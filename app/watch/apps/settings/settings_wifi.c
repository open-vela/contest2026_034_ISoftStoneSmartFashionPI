
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <lvgl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <wireless/wapi.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <syslog.h>
#include "netutils/netlib.h"
#include "../common/watch_pages.h"
#include "../../resource/resource.h"
#include "../../resource/image/generated/lvgl_assets.h"

#include <nuttx/config.h>

/* 调试打印开关：menuconfig 打开 CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG 后生效。
 * 默认关闭：无 USB 主机时控制台 FIFO 写满会阻塞系统。
 */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define WATCH_DBG_LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#else
#  define WATCH_DBG_LOG(fmt, ...)
#endif

/* WiFi 扫描结果结构 */
typedef struct {
    char ssid[64];
    int channel;
    int rssi;
    bool has_ssid;
    bool is_encrypted;
} wifi_scan_result_t;

/* WiFi 扫描数据 */
typedef struct {
    wifi_scan_result_t results[32];
    int count;
    bool scanning;
} wifi_scan_data_t;

/* 保存的WiFi信息 */
typedef struct {
    char ssid[64];
    char password[64];
} saved_wifi_t;

/* 保存的WiFi列表 - 大型静态数据改为堆分配 */
#define MAX_SAVED_WIFI 10

/* 将大型静态数据包装在堆分配的结构体中 */
typedef struct {
    saved_wifi_t saved_wifi_list[MAX_SAVED_WIFI];
    wifi_scan_data_t wifi_scan_data;
    char selected_ssid[64];
    char wifi_password[64];
    char connected_ssid[64];
} wifi_static_data_t;

static wifi_static_data_t *wifi_sd = NULL;
static wifi_static_data_t* wifi_sd_get(void) {
    if (!wifi_sd) wifi_sd = calloc(1, sizeof(wifi_static_data_t));
    return wifi_sd;
}
#define g_saved_wifi_list (wifi_sd_get()->saved_wifi_list)
#define g_wifi_scan_data (wifi_sd_get()->wifi_scan_data)
#define g_selected_ssid (wifi_sd_get()->selected_ssid)
#define g_wifi_password (wifi_sd_get()->wifi_password)
#define g_connected_ssid (wifi_sd_get()->connected_ssid)

static int g_saved_wifi_count = 0;
static pthread_t g_scan_thread __attribute__((unused)) = 0;
static lv_obj_t *g_list_cont = NULL;  // 保存列表容器指针用于更新
static lv_timer_t *g_update_timer = NULL;  // 定时器用于检查扫描状态
static lv_timer_t *g_status_timer = NULL;  // 定时器用于检查连接状态
/* g_selected_ssid, g_wifi_password, g_connected_ssid 已移至堆分配 */
static lv_obj_t *g_password_label = NULL;  // 密码显示标签
static lv_obj_t *g_keyboard_cont = NULL;  // 键盘容器
static int g_keyboard_mode = 0;  // 0: 小写字母, 1: 大写字母, 2: 数字符号
static lv_obj_t *g_dialog_mask = NULL;  // 对话框遮罩
static bool g_wifi_enabled = false;  // WiFi 开关状态
static bool g_is_reconnecting = false;  // 是否正在重连
static time_t g_connect_time = 0;  // 连接建立时间
static int g_status_fail_count = 0;  // 连续状态检查失败次数

/* 函数前向声明 */
static void keyboard_btn_event_cb(lv_event_t *e);
static void create_keyboard_layout(lv_obj_t *keyboard_cont);
static void wifi_update_list_ui(lv_obj_t *list_cont);
static void wifi_scan_timer_cb(lv_timer_t *timer);
static void wifi_refresh_btn_cb(lv_event_t *e);
static void wifi_auto_reconnect_check(void);
static void wifi_show_detail_page(lv_event_t *e);
static int wifi_disconnect(const char *ifname);
static void wifi_status_check_timer_cb(lv_timer_t *timer);

/* 按信号强度排序WiFi扫描结果（降序） */
static void sort_wifi_by_signal_strength(void)
{
    for (int i = 0; i < g_wifi_scan_data.count - 1; i++) {
        for (int j = 0; j < g_wifi_scan_data.count - i - 1; j++) {
            if (g_wifi_scan_data.results[j].rssi < g_wifi_scan_data.results[j + 1].rssi) {
                wifi_scan_result_t temp = g_wifi_scan_data.results[j];
                g_wifi_scan_data.results[j] = g_wifi_scan_data.results[j + 1];
                g_wifi_scan_data.results[j + 1] = temp;
            }
        }
    }
}

static int get_wifi_signal_level(int rssi)
{
    if (rssi >= -50) return 3;
    if (rssi >= -65) return 2;
    return 1;
}

/* 根据信号强度和加密状态获取WiFi信号图标 */
static const char* get_wifi_signal_icon(int rssi, bool is_encrypted)
{
    int level = get_wifi_signal_level(rssi);
    
    if (is_encrypted) {
        switch (level) {
            case 3: return watch_resource_get_img("icon_wifi_signal_lock_3");
            case 2: return watch_resource_get_img("icon_wifi_signal_lock_2");
            case 1: 
            default: return watch_resource_get_img("icon_wifi_signal_lock_1");
        }
    } else {
        switch (level) {
            case 3: return watch_resource_get_img("icon_wifi_signal_3");
            case 2: return watch_resource_get_img("icon_wifi_signal_2");
            case 1:
            default: return watch_resource_get_img("icon_wifi_signal_1");
        }
    }
}

/* 查找保存的WiFi，返回索引，-1表示未找到 */
static int wifi_find_saved(const char *ssid)
{
    for (int i = 0; i < g_saved_wifi_count; i++) {
        if (strcmp(g_saved_wifi_list[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

/* WiFi配置文件路径 */
/* 单个WiFi的连接等待预算（毫秒）：DHCP 已在 wifi_connect 内同步完成，
 * 这里只覆盖鉴权/地址生效的抖动，失败时尽快切下一个 */
#define WIFI_CONNECT_WAIT_MS 6000

/* 等待关联到AP的预算（毫秒）：超时未关联则跳过DHCP，
 * 避免AP不在场时 ifconfig dhcp 阻塞空转20秒+ */
#define WIFI_ASSOC_WAIT_MS 4000

/* 自动重连最大轮数：开机初期驱动/射频未稳定或热点晚出现时，
 * 单轮全失败后重试，避免WiFi永远连不上 */
#define WIFI_RECONNECT_ROUNDS 3

#define WIFI_CONFIG_FILE "/mnt/spif/wifi_config.txt"
#define WIFI_ENABLED_FILE "/mnt/spif/wifi_enabled.txt"

/* 保存WiFi列表到文件 */
static void wifi_save_to_file(void)
{
    FILE *fp = fopen(WIFI_CONFIG_FILE, "w");
    if (!fp) {
        WATCH_DBG_LOG("[WiFi] Failed to open config file for writing");
        return;
    }
    
    for (int i = 0; i < g_saved_wifi_count; i++) {
        fprintf(fp, "%s,%s\n", g_saved_wifi_list[i].ssid, g_saved_wifi_list[i].password);
    }
    
    fclose(fp);
    WATCH_DBG_LOG("[WiFi] Saved %d WiFi to file", g_saved_wifi_count);
}

/* 从文件加载WiFi列表 */
static void wifi_load_from_file(void)
{
    FILE *fp = fopen(WIFI_CONFIG_FILE, "r");
    if (!fp) {
        WATCH_DBG_LOG("[WiFi] No config file found, starting fresh");
        return;
    }
    
    g_saved_wifi_count = 0;
    char line[256];
    
    while (fgets(line, sizeof(line), fp) && g_saved_wifi_count < MAX_SAVED_WIFI) {
        // 移除换行符
        line[strcspn(line, "\n")] = 0;
        
        // 解析SSID和密码（格式：ssid,password）
        char *comma = strchr(line, ',');
        if (comma && comma != line) {
            *comma = '\0';
            const char *ssid = line;
            const char *password = comma + 1;
            
            if (strlen(ssid) > 0 && strlen(password) > 0) {
                strncpy(g_saved_wifi_list[g_saved_wifi_count].ssid, ssid, sizeof(g_saved_wifi_list[0].ssid) - 1);
                g_saved_wifi_list[g_saved_wifi_count].ssid[sizeof(g_saved_wifi_list[0].ssid) - 1] = '\0';
                strncpy(g_saved_wifi_list[g_saved_wifi_count].password, password, sizeof(g_saved_wifi_list[0].password) - 1);
                g_saved_wifi_list[g_saved_wifi_count].password[sizeof(g_saved_wifi_list[0].password) - 1] = '\0';
                g_saved_wifi_count++;
            }
        }
    }
    
    fclose(fp);
    WATCH_DBG_LOG("[WiFi] Loaded %d WiFi from file", g_saved_wifi_count);
}

/* 保存WiFi开关状态到文件 */
static void wifi_save_enabled(bool enabled)
{
    FILE *fp = fopen(WIFI_ENABLED_FILE, "w");
    if (!fp) {
        WATCH_DBG_LOG("[WiFi] Failed to open enabled file for writing\n");
        return;
    }
    fprintf(fp, "%d\n", enabled ? 1 : 0);
    fclose(fp);
    WATCH_DBG_LOG("[WiFi] Saved enabled state: %d\n", enabled);
}

/* 从文件加载WiFi开关状态 */
static bool wifi_load_enabled(void)
{
    FILE *fp = fopen(WIFI_ENABLED_FILE, "r");
    if (!fp) {
        WATCH_DBG_LOG("[WiFi] No enabled file found, defaulting to off\n");
        return false;
    }
    int val = 0;
    if (fscanf(fp, "%d", &val) != 1) {
        val = 0;
    }
    fclose(fp);
    bool enabled = (val == 1);
    WATCH_DBG_LOG("[WiFi] Loaded enabled state: %d\n", enabled);
    return enabled;
}

/* 获取保存的WiFi密码，NULL表示未找到 */
static const char* wifi_get_saved_password(const char *ssid)
{
    int idx = wifi_find_saved(ssid);
    if (idx >= 0) {
        return g_saved_wifi_list[idx].password;
    }
    return NULL;
}

/* 保存WiFi，如果已存在则更新密码 */
static void wifi_save(const char *ssid, const char *password)
{
    int idx = wifi_find_saved(ssid);
    if (idx >= 0) {
        // 已存在，更新密码
        strncpy(g_saved_wifi_list[idx].password, password, sizeof(g_saved_wifi_list[0].password) - 1);
        g_saved_wifi_list[idx].password[sizeof(g_saved_wifi_list[0].password) - 1] = '\0';
        WATCH_DBG_LOG("[WiFi] Updated saved WiFi: %s", ssid);
    } else if (g_saved_wifi_count < MAX_SAVED_WIFI) {
        // 添加新的
        strncpy(g_saved_wifi_list[g_saved_wifi_count].ssid, ssid, sizeof(g_saved_wifi_list[0].ssid) - 1);
        g_saved_wifi_list[g_saved_wifi_count].ssid[sizeof(g_saved_wifi_list[0].ssid) - 1] = '\0';
        strncpy(g_saved_wifi_list[g_saved_wifi_count].password, password, sizeof(g_saved_wifi_list[0].password) - 1);
        g_saved_wifi_list[g_saved_wifi_count].password[sizeof(g_saved_wifi_list[0].password) - 1] = '\0';
        g_saved_wifi_count++;
        WATCH_DBG_LOG("[WiFi] Saved new WiFi: %s (total: %d)", ssid, g_saved_wifi_count);
    } else {
        WATCH_DBG_LOG("[WiFi] Saved WiFi list full, cannot save: %s", ssid);
        return;
    }
    
    // 保存到文件
    wifi_save_to_file();
}

/* 删除保存的WiFi */
static void wifi_forget(const char *ssid)
{
    int idx = wifi_find_saved(ssid);
    if (idx >= 0) {
        // 将后面的往前移
        for (int i = idx; i < g_saved_wifi_count - 1; i++) {
            memcpy(&g_saved_wifi_list[i], &g_saved_wifi_list[i + 1], sizeof(saved_wifi_t));
        }
        g_saved_wifi_count--;
        memset(&g_saved_wifi_list[g_saved_wifi_count], 0, sizeof(saved_wifi_t));
        WATCH_DBG_LOG("[WiFi] Forgot WiFi: %s (remaining: %d)", ssid, g_saved_wifi_count);
        
        // 保存到文件
        wifi_save_to_file();
    }
}

/* 获取当前连接WiFi的密码（用于重连） */
static const char* wifi_get_connected_password(void)
    __attribute__((unused));
static const char* wifi_get_connected_password(void)
{
    if (strlen(g_connected_ssid) > 0) {
        return wifi_get_saved_password(g_connected_ssid);
    }
    return NULL;
}

/* 获取WiFi连接状态接口 */
bool settings_wifi_is_connected(void)
{
    return (strlen(g_connected_ssid) > 0);
}

/* 获取已连接的WiFi SSID */
const char* settings_wifi_get_connected_ssid(void)
{
    if (strlen(g_connected_ssid) > 0) {
        return g_connected_ssid;
    }
    return NULL;
}

/* 关联状态：0=未关联，1=ESSID匹配但接口未RUNNING，2=ESSID匹配且RUNNING */
static int wifi_assoc_state(const char *ifname, const char *ssid)
{
    int state = 0;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return 0;
    }

    char current_essid[WAPI_ESSID_MAX_SIZE + 1];
    memset(current_essid, 0, sizeof(current_essid));
    if (wapi_get_essid(sock, ifname, current_essid, NULL) == 0 &&
        strlen(current_essid) > 0 &&
        strcmp(current_essid, ssid) == 0) {
        state = 1;
        uint8_t if_flags = 0;
        if (netlib_getifstatus(ifname, &if_flags) == 0 &&
            IFF_IS_RUNNING(if_flags)) {
            state = 2;
        }
    }

    close(sock);
    return state;
}

/* 执行系统命令并将stdout/stderr重定向到/dev/null，避免控制台FIFO满时阻塞。
 * 同时通过syslog记录命令执行情况，便于离线诊断。 */
static void wifi_system_cmd(const char *command)
{
    char redirected[320];
    syslog(LOG_INFO, "[WiFi] cmd: %s", command);
    snprintf(redirected, sizeof(redirected), "%s > /dev/null 2>&1", command);
    system(redirected);
}

/* WiFi 连接函数
 * wait_assoc：是否在DHCP前门控关联状态。仅当AP在场与否未知时（无有效
 * 预扫描）才开启，用于避免AP不在场时 ifconfig dhcp 阻塞空转20秒+。
 * 实测扫描后立即连接时驱动的ESSID读回/关联上报会滞后超过4秒，
 * 因此AP已确认在场时必须跳过门控，直接DHCP（驱动会自行完成关联，
 * DHCP阻塞至拿到租约，是验证过的可靠路径） */
static int wifi_connect(const char *ifname, const char *ssid,
                        const char *password, bool wait_assoc)
{
    char command[256];
    
    syslog(LOG_INFO, "[WiFi] Connecting to %s", ssid);
    
    /* 1. 启用 WiFi 接口 */
    snprintf(command, sizeof(command), "ifup %s", ifname);
    wifi_system_cmd(command);
    
    /* 2. 设置 WiFi 模式为 Managed (Station) */
    snprintf(command, sizeof(command), "wapi mode %s 2", ifname);
    wifi_system_cmd(command);
    
    /* 3. 设置 PSK 密码 */
    snprintf(command, sizeof(command), "wapi psk %s \"%s\" 2 2", ifname, password);
    wifi_system_cmd(command);
    
    /* 4. 设置 ESSID */
    snprintf(command, sizeof(command), "wapi essid %s \"%s\" 1", ifname, ssid);
    wifi_system_cmd(command);
    
    /* 5. 可选的关联门控（仅AP在场未知时） */
    if (wait_assoc) {
        int waited = 0;
        int assoc;
        while ((assoc = wifi_assoc_state(ifname, ssid)) != 2) {
            if (waited >= WIFI_ASSOC_WAIT_MS) {
                break;
            }
            usleep(200 * 1000);
            waited += 200;
        }
        if (assoc == 0) {
            syslog(LOG_INFO, "[WiFi] Association timeout for %s (%dms), skip DHCP\n",
                   ssid, waited);
            return -1;
        }
        if (assoc == 1) {
            /* 部分驱动 RUNNING 标志置位滞后/不可靠：ESSID 已匹配就继续 DHCP */
            syslog(LOG_WARNING, "[WiFi] %s: ESSID matched but not RUNNING, DHCP anyway\n",
                   ssid);
        } else {
            syslog(LOG_INFO, "[WiFi] Associated to %s in %dms\n", ssid, waited);
        }
    }
    
    /* 6. 启动 DHCP 获取 IP 地址 */
    snprintf(command, sizeof(command), "ifconfig %s dhcp", ifname);
    wifi_system_cmd(command);
    
    syslog(LOG_INFO, "[WiFi] Connection completed");
    return 0;
}

/* 更新密码显示 */
static void update_password_display(void)
{
    if (g_password_label) {
        // 显示明文密码
        lv_label_set_text(g_password_label, g_wifi_password);
    }
}

/* 创建键盘按键 */
static void create_keyboard_key(lv_obj_t *parent, const char *text, int x, int y, int w, int h)
{
    lv_obj_t *key_btn = lv_btn_create(parent);
    lv_obj_set_size(key_btn, w, h);
    lv_obj_set_style_bg_color(key_btn, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_color(key_btn, lv_color_hex(0x666666), LV_STATE_PRESSED);
    lv_obj_set_style_radius(key_btn, 5, 0);
    lv_obj_align(key_btn, LV_ALIGN_TOP_LEFT, x, y);
    
    lv_obj_t *key_label = lv_label_create(key_btn);
    lv_label_set_text(key_label, text);
    lv_obj_set_style_text_color(key_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(key_label, watch_resource_get_font("MiSans-Regular_20"), 0);
    lv_obj_align(key_label, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_add_event_cb(key_btn, keyboard_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

/* 创建键盘布局 */
static void create_keyboard_layout(lv_obj_t *keyboard_cont)
{
    lv_obj_clean(keyboard_cont);
    
    int key_w = 32;
    int key_h = 40;
    int gap = 3;
    int start_x = 5;
    int start_y = 5;
    
    if (g_keyboard_mode == 0 || g_keyboard_mode == 1) {
        // 字母键盘
        const char *row1[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
        const char *row2[] = {"A", "S", "D", "F", "G", "H", "J", "K", "L"};
        const char *row3[] = {"Z", "X", "C", "V", "B", "N", "M"};
        
        // 第一行
        for (int i = 0; i < 10; i++) {
            char text[2] = {row1[i][0], '\0'};
            if (g_keyboard_mode == 0) text[0] += 32;  // 转小写
            create_keyboard_key(keyboard_cont, text, start_x + i * (key_w + gap), start_y, key_w, key_h);
        }
        
        // 第二行（偏移）
        for (int i = 0; i < 9; i++) {
            char text[2] = {row2[i][0], '\0'};
            if (g_keyboard_mode == 0) text[0] += 32;
            create_keyboard_key(keyboard_cont, text, start_x + 18 + i * (key_w + gap), start_y + key_h + gap, key_w, key_h);
        }
        
        // 第三行（偏移更多）
        for (int i = 0; i < 7; i++) {
            char text[2] = {row3[i][0], '\0'};
            if (g_keyboard_mode == 0) text[0] += 32;
            create_keyboard_key(keyboard_cont, text, start_x + 36 + i * (key_w + gap), start_y + 2 * (key_h + gap), key_w, key_h);
        }
        
        // Shift 键
        create_keyboard_key(keyboard_cont, "⇧", start_x, start_y + 2 * (key_h + gap), 32, key_h);
        
        // 删除键
        create_keyboard_key(keyboard_cont, "Del", start_x + 8 * (key_w + gap), start_y + 2 * (key_h + gap), 50, key_h);
        
        // 第四行：数字切换、空格、连接、取消
        create_keyboard_key(keyboard_cont, "123", start_x, start_y + 3 * (key_h + gap), 45, key_h);
        create_keyboard_key(keyboard_cont, "空格", start_x + 50, start_y + 3 * (key_h + gap), 180, key_h);
        create_keyboard_key(keyboard_cont, "连接", start_x + 235, start_y + 3 * (key_h + gap), 50, key_h);
        create_keyboard_key(keyboard_cont, "取消", start_x + 290, start_y + 3 * (key_h + gap), 50, key_h);
        
    } else {
        // 数字符号键盘
        const char *row1[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
        const char *row2[] = {"-", "/", ":", ";", "(", ")", "$", "&", "@", "\""};
        const char *row3[] = {"#", "%", "*", "+", "=", "^", "!", "?", "_", "\\"};
        
        // 第一行
        for (int i = 0; i < 10; i++) {
            create_keyboard_key(keyboard_cont, row1[i], start_x + i * (key_w + gap), start_y, key_w, key_h);
        }
        
        // 第二行
        for (int i = 0; i < 10; i++) {
            create_keyboard_key(keyboard_cont, row2[i], start_x + i * (key_w + gap), start_y + key_h + gap, key_w, key_h);
        }
        
        // 第三行
        for (int i = 0; i < 10; i++) {
            create_keyboard_key(keyboard_cont, row3[i], start_x + i * (key_w + gap), start_y + 2 * (key_h + gap), key_w, key_h);
        }
        
        // 删除键
        create_keyboard_key(keyboard_cont, "Del", start_x + 8 * (key_w + gap), start_y + 2 * (key_h + gap), 50, key_h);
        
        // 第四行：字母切换、空格、连接、取消
        create_keyboard_key(keyboard_cont, "ABC", start_x, start_y + 3 * (key_h + gap), 45, key_h);
        create_keyboard_key(keyboard_cont, "空格", start_x + 50, start_y + 3 * (key_h + gap), 180, key_h);
        create_keyboard_key(keyboard_cont, "连接", start_x + 235, start_y + 3 * (key_h + gap), 50, key_h);
        create_keyboard_key(keyboard_cont, "取消", start_x + 290, start_y + 3 * (key_h + gap), 50, key_h);
    }
}

/* 键盘按钮事件处理 */
static void keyboard_btn_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *text = lv_label_get_text(lv_obj_get_child(btn, 0));
    
    if (strcmp(text, "Del") == 0) {
        // 删除最后一个字符
        int len = strlen(g_wifi_password);
        if (len > 0) {
            g_wifi_password[len - 1] = '\0';
        }
        update_password_display();
        } else if (strcmp(text, "连接") == 0) {
            // 连接 WiFi
            if (strlen(g_wifi_password) >= 8) {
                wifi_connect("wlan0", g_selected_ssid, g_wifi_password, false);
                // 保存已连接的 SSID
                strncpy(g_connected_ssid, g_selected_ssid, sizeof(g_connected_ssid) - 1);
                g_connected_ssid[sizeof(g_connected_ssid) - 1] = '\0';
                
                // 记录连接时间
                g_connect_time = time(NULL);
                
                // 保存WiFi用于自动重连
                wifi_save(g_selected_ssid, g_wifi_password);
                
                WATCH_DBG_LOG("[WiFi] Connected to: %s (saved for auto-reconnect)", g_connected_ssid);
                
                // 重置智能家居服务器信息，下次使用时重新发现
    /* home_control_reset_server removed: no home_control dependency */
                
                // 关闭对话框
                if (g_dialog_mask) {
                    lv_watch_pop_page(g_dialog_mask);
                    lv_obj_del(g_dialog_mask);
                    g_dialog_mask = NULL;
                }
                if (g_list_cont) {
                    wifi_update_list_ui(g_list_cont);
                }
            } else {
                WATCH_DBG_LOG("[WiFi] Password too short (min 8 characters)");
            }
    } else if (strcmp(text, "取消") == 0) {
        // 取消，关闭对话框
        if (g_dialog_mask) {
            lv_watch_pop_page(g_dialog_mask);
            lv_obj_del(g_dialog_mask);
            g_dialog_mask = NULL;
        }
    } else if (strcmp(text, "⇧") == 0) {
        // 切换大小写
        g_keyboard_mode = (g_keyboard_mode == 0) ? 1 : 0;
        create_keyboard_layout(g_keyboard_cont);
    } else if (strcmp(text, "123") == 0) {
        // 切换到数字符号键盘
        g_keyboard_mode = 2;
        create_keyboard_layout(g_keyboard_cont);
    } else if (strcmp(text, "ABC") == 0) {
        // 切换到字母键盘
        g_keyboard_mode = 0;
        create_keyboard_layout(g_keyboard_cont);
    } else if (strcmp(text, "空格") == 0) {
        // 添加空格
        int len = strlen(g_wifi_password);
        if (len < sizeof(g_wifi_password) - 1) {
            g_wifi_password[len] = ' ';
            g_wifi_password[len + 1] = '\0';
        }
        update_password_display();
    } else {
        // 添加字符
        int len = strlen(g_wifi_password);
        if (len < sizeof(g_wifi_password) - 1) {
            g_wifi_password[len] = text[0];
            g_wifi_password[len + 1] = '\0';
        }
        update_password_display();
    }
}

/* 对话框侧滑返回处理 */
static void dialog_slide_gesture_handler(lv_event_t *e)
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

                // 右滑超过50px且水平位移大于垂直位移时，关闭对话框
                if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                    if (g_dialog_mask) {
                        lv_watch_pop_page(g_dialog_mask);
                        lv_obj_del(g_dialog_mask);
                        g_dialog_mask = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

/* 创建密码输入对话框 */
static void create_password_dialog(const char *ssid)
{
    strncpy(g_selected_ssid, ssid, sizeof(g_selected_ssid) - 1);
    g_selected_ssid[sizeof(g_selected_ssid) - 1] = '\0';
    g_wifi_password[0] = '\0';
    g_keyboard_mode = 0;  // 默认小写字母
    
    // 创建对话框背景遮罩
    g_dialog_mask = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_dialog_mask, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(g_dialog_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_dialog_mask, LV_OPA_70, 0);
    lv_obj_align(g_dialog_mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(g_dialog_mask, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加侧滑手势处理
    lv_obj_add_event_cb(g_dialog_mask, dialog_slide_gesture_handler, LV_EVENT_ALL, NULL);

    // 将对话框遮罩压入页面栈，支持PWR短按返回主页
    lv_watch_push_page(g_dialog_mask);
    
    // 创建对话框容器
    lv_obj_t *dialog = lv_obj_create(g_dialog_mask);
    lv_obj_set_size(dialog, 380, 450);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(dialog, 10, 0);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(dialog, 15, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    /* 设置中文字体，确保所有子标签继承 */
    lv_obj_set_style_text_font(dialog, watch_resource_get_font("MiSans-Regular_20"), 0);
    
    // SSID 标题
    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text_fmt(title, "连接到: %s", ssid);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    
    // 密码输入框
    lv_obj_t *pwd_cont = lv_obj_create(dialog);
    lv_obj_set_size(pwd_cont, 350, 50);
    lv_obj_set_style_bg_color(pwd_cont, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(pwd_cont, 1, 0);
    lv_obj_set_style_border_color(pwd_cont, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(pwd_cont, 5, 0);
    lv_obj_clear_flag(pwd_cont, LV_OBJ_FLAG_SCROLLABLE);
    
    g_password_label = lv_label_create(pwd_cont);
    lv_label_set_text(g_password_label, "");
    lv_obj_set_style_text_color(g_password_label, lv_color_white(), 0);
    lv_obj_align(g_password_label, LV_ALIGN_LEFT_MID, 10, 0);
    
    // 提示文本
    lv_obj_t *hint = lv_label_create(dialog);
    lv_label_set_text(hint, "请输入WiFi密码 (至少8位)");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    
    // 键盘容器
    g_keyboard_cont = lv_obj_create(dialog);
    lv_obj_set_size(g_keyboard_cont, 350, 200);
    lv_obj_set_style_bg_color(g_keyboard_cont, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(g_keyboard_cont, 0, 0);
    lv_obj_set_style_pad_all(g_keyboard_cont, 0, 0);
    lv_obj_clear_flag(g_keyboard_cont, LV_OBJ_FLAG_SCROLLABLE);
    
    // 创建键盘布局
    create_keyboard_layout(g_keyboard_cont);
}

/* 启用 WiFi 接口 */
static int wifi_enable(const char *ifname)
{
    int sock;
    int ret;
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        WATCH_DBG_LOG("[WiFi] Failed to create socket: %d", errno);
        return -1;
    }
    
    ret = wapi_set_ifup(sock, ifname);
    if (ret < 0) {
        WATCH_DBG_LOG("[WiFi] Failed to bring up %s: %d", ifname, ret);
        close(sock);
        return -1;
    }
    
    WATCH_DBG_LOG("[WiFi] %s enabled successfully", ifname);
    close(sock);
    return 0;
}

/* 禁用 WiFi 接口 */
static int wifi_disable(const char *ifname)
{
    int sock;
    int ret;
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        WATCH_DBG_LOG("[WiFi] Failed to create socket: %d", errno);
        return -1;
    }
    
    ret = wapi_set_ifdown(sock, ifname);
    if (ret < 0) {
        WATCH_DBG_LOG("[WiFi] Failed to bring down %s: %d", ifname, ret);
        close(sock);
        return -1;
    }
    
    WATCH_DBG_LOG("[WiFi] %s disabled successfully", ifname);
    close(sock);
    return 0;
}

/* WiFi 扫描线程 */
static void *wifi_scan_thread(void *arg)
{
    int sock;
    int ret;
    const char *ifname = "wlan0";
    struct wapi_list_s aps;
    struct wapi_scan_info_s *scan_info;
    
    WATCH_DBG_LOG("[WiFi] Scan thread started");
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        WATCH_DBG_LOG("[WiFi] Failed to create socket: %d", errno);
        g_wifi_scan_data.scanning = false;
        return NULL;
    }
    
    /* 初始化扫描 */
    ret = wapi_scan_init(sock, ifname, NULL);
    if (ret < 0) {
        WATCH_DBG_LOG("[WiFi] Failed to init scan: %d", ret);
        close(sock);
        g_wifi_scan_data.scanning = false;
        return NULL;
    }
    
    /* 等待扫描完成 */
    sleep(5);
    
    /* 收集扫描结果 */
    memset(&aps, 0, sizeof(aps));
    ret = wapi_scan_coll(sock, ifname, &aps);
    if (ret < 0) {
        WATCH_DBG_LOG("[WiFi] Failed to collect scan results: %d", ret);
        close(sock);
        g_wifi_scan_data.scanning = false;
        return NULL;
    }
    
    /* 遍历扫描结果链表 */
    g_wifi_scan_data.count = 0;
    scan_info = aps.head.scan;
    
    while (scan_info != NULL && g_wifi_scan_data.count < 32) {
        if (scan_info->has_essid && strlen(scan_info->essid) > 0) {
            strncpy(g_wifi_scan_data.results[g_wifi_scan_data.count].ssid, 
                    scan_info->essid, sizeof(g_wifi_scan_data.results[0].ssid) - 1);
            g_wifi_scan_data.results[g_wifi_scan_data.count].ssid[sizeof(g_wifi_scan_data.results[0].ssid) - 1] = '\0';
            g_wifi_scan_data.results[g_wifi_scan_data.count].has_ssid = true;
            
            if (scan_info->has_rssi) {
                g_wifi_scan_data.results[g_wifi_scan_data.count].rssi = scan_info->rssi;
            }
            
            if (scan_info->has_freq) {
                g_wifi_scan_data.results[g_wifi_scan_data.count].channel = (int)scan_info->freq;
            }
            
            if (scan_info->has_encode) {
                g_wifi_scan_data.results[g_wifi_scan_data.count].is_encrypted = 
                    (scan_info->encode != WAPI_ENCODE_DISABLED && scan_info->encode != WAPI_ENCODE_OPEN);
            } else {
                g_wifi_scan_data.results[g_wifi_scan_data.count].is_encrypted = false;
            }
            
            g_wifi_scan_data.count++;
        }
        
        scan_info = scan_info->next;
    }
    
    sort_wifi_by_signal_strength();
    
    WATCH_DBG_LOG("[WiFi] Scan completed, found %d networks", g_wifi_scan_data.count);
    
    /* 释放扫描结果 */
    wapi_scan_coll_free(&aps);
    
    close(sock);
    g_wifi_scan_data.scanning = false;
    return NULL;
}

/* 启动 WiFi 扫描 */
static int wifi_start_scan(void)
{
    g_wifi_scan_data.scanning = true;
    g_wifi_scan_data.count = 0;
    
    pthread_t tid;
    if (pthread_create(&tid, NULL, wifi_scan_thread, NULL) != 0) {
        WATCH_DBG_LOG("[WiFi] Failed to create scan thread");
        g_wifi_scan_data.scanning = false;
        return -1;
    }
    pthread_detach(tid);
    
    return 0;
}

/* 刷新按钮回调 */
static void wifi_refresh_btn_cb(lv_event_t *e)
{
    WATCH_DBG_LOG("[WiFi] Refresh button clicked, starting scan...");
    
    if (g_wifi_scan_data.scanning) {
        WATCH_DBG_LOG("[WiFi] Already scanning, please wait");
        return;
    }

    /* 仅在 WiFi 未连接时重置接口，避免断开已有连接 */
    if (strlen(g_connected_ssid) == 0) {
        syslog(LOG_INFO, "[WiFi] Refresh: resetting interface (not connected)");
        wifi_disable("wlan0");
        usleep(300 * 1000);
        wifi_enable("wlan0");
        usleep(500 * 1000);
    } else {
        syslog(LOG_INFO, "[WiFi] Refresh: scanning without reset (connected to '%s')", g_connected_ssid);
    }
    
    wifi_start_scan();
    
    if (g_list_cont && !g_update_timer) {
        g_update_timer = lv_timer_create(wifi_scan_timer_cb, 500, NULL);
    }
}

/* 轮询等待连接就绪：拿到IP即成功，其次ESSID匹配且接口RUNNING。
 * 相比固定 sleep，成功时可立即返回，失败时也不会白等满预算。
 * timeout_ms：总等待预算；返回 true 表示连接成功。 */
static bool wifi_wait_connected(const char *ifname, const char *ssid,
                                int timeout_ms)
{
    const int poll_ms = 300;
    int waited = 0;
    bool ok = false;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        WATCH_DBG_LOG("[WiFi] wait: socket create failed\n");
        return false;
    }

    while (waited <= timeout_ms) {
        /* 判据1：已拿到IP（最可靠） */
        struct in_addr ip_addr;
        if (wapi_get_ip(sock, ifname, &ip_addr) == 0 && ip_addr.s_addr != 0) {
            WATCH_DBG_LOG("[WiFi] Connection verified (has IP): %s in %dms\n",
                          inet_ntoa(ip_addr), waited);
            ok = true;
            break;
        }

        /* 判据2：ESSID匹配且接口处于RUNNING */
        char current_essid[WAPI_ESSID_MAX_SIZE + 1];
        memset(current_essid, 0, sizeof(current_essid));
        if (wapi_get_essid(sock, ifname, current_essid, NULL) == 0 &&
            strlen(current_essid) > 0 &&
            strcmp(current_essid, ssid) == 0) {
            uint8_t if_flags = 0;
            if (netlib_getifstatus(ifname, &if_flags) == 0 &&
                IFF_IS_RUNNING(if_flags)) {
                WATCH_DBG_LOG("[WiFi] Connection verified (ESSID + running) in %dms\n",
                              waited);
                ok = true;
                break;
            }
        }

        usleep(poll_ms * 1000);
        waited += poll_ms;
    }

    close(sock);

    if (!ok) {
        WATCH_DBG_LOG("[WiFi] Wait timeout for %s (%dms)\n", ssid, timeout_ms);
    }

    return ok;
}

/* 把连接成功的WiFi移到保存列表首位并持久化，下次开机第一个就命中 */
static void wifi_promote_saved(int idx)
{
    if (idx <= 0 || idx >= g_saved_wifi_count) {
        return;
    }

    saved_wifi_t hit = g_saved_wifi_list[idx];
    for (int j = idx; j > 0; j--) {
        g_saved_wifi_list[j] = g_saved_wifi_list[j - 1];
    }
    g_saved_wifi_list[0] = hit;

    wifi_save_to_file();
    WATCH_DBG_LOG("[WiFi] Promoted %s to first in saved list\n", hit.ssid);
}

/* 同步预扫描：采集周边热点SSID写入 essids，返回热点数量，失败返回 -1。
 * 用 wapi_scan_stat 轮询代替固定 sleep，扫描完成即返回 */
static int wifi_scan_sync(char essids[][WAPI_ESSID_MAX_SIZE + 1], int max,
                          int timeout_ms)
{
    struct wapi_list_s aps;
    struct wapi_scan_info_s *scan_info;
    int count = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    if (wapi_scan_init(sock, "wlan0", NULL) < 0) {
        WATCH_DBG_LOG("[WiFi] Pre-scan init failed\n");
        close(sock);
        return -1;
    }

    /* 轮询扫描状态：0=结果就绪，>0=进行中，<0=出错（交给 coll 处理）。
     * 部分驱动会过早报"就绪"（实测300ms就返回残缺结果），
     * 先保证最短驻留时间再采集 */
    const int min_dwell_ms = 2500;
    int waited = 0;
    while (waited < timeout_ms) {
        usleep(300 * 1000);
        waited += 300;
        if (waited >= min_dwell_ms && wapi_scan_stat(sock, "wlan0") <= 0) {
            break;
        }
    }

    memset(&aps, 0, sizeof(aps));
    if (wapi_scan_coll(sock, "wlan0", &aps) < 0) {
        WATCH_DBG_LOG("[WiFi] Pre-scan collect failed\n");
        close(sock);
        return -1;
    }

    for (scan_info = aps.head.scan; scan_info != NULL && count < max;
         scan_info = scan_info->next) {
        if (scan_info->has_essid && strlen(scan_info->essid) > 0) {
            strncpy(essids[count], scan_info->essid, WAPI_ESSID_MAX_SIZE);
            essids[count][WAPI_ESSID_MAX_SIZE] = '\0';
            count++;
        }
    }

    wapi_scan_coll_free(&aps);
    close(sock);

    WATCH_DBG_LOG("[WiFi] Pre-scan done in %dms: %d networks\n", waited, count);
    return count;
}

/* WiFi自动重连线程：遍历所有保存的WiFi，依次尝试连接直到成功 */
static void *wifi_reconnect_thread(void *arg)
{
    WATCH_DBG_LOG("[WiFi] Reconnect thread started");
    
    // wifi_enable 已同步返回，这里只需短暂让驱动稳定
    usleep(500 * 1000);
    
    if (g_saved_wifi_count == 0) {
        WATCH_DBG_LOG("[WiFi] No saved WiFi for auto-reconnect");
        g_is_reconnecting = false;
        return NULL;
    }
    
    /* 单轮全失败后重试：开机初期驱动/射频未稳定或热点晚出现时，
     * 一次性放弃会让WiFi永远连不上 */
    for (int round = 1;
         round <= WIFI_RECONNECT_ROUNDS && strlen(g_connected_ssid) == 0
         && g_is_reconnecting;
         round++) {
        if (round > 1) {
            WATCH_DBG_LOG("[WiFi] Retry round %d/%d\n",
                          round, WIFI_RECONNECT_ROUNDS);
            wifi_disconnect("wlan0");
            usleep(300 * 1000);
            wifi_enable("wlan0");
            sleep(2);
        }

        /* 预扫描：只尝试在场的热点，避免对不在场的 SSID 白耗关联/DHCP时间。
         * 首次扫描未命中已保存WiFi时，等1秒重试一次（开机初期驱动可能未就绪）；
         * 两次都未命中则直接放弃本轮，不做盲连 */
        char scan_essids[16][WAPI_ESSID_MAX_SIZE + 1];
        int scan_count = -1;
        for (int scan_try = 0; scan_try < 2; scan_try++) {
            scan_count = wifi_scan_sync(scan_essids, 16, 5000);
            if (scan_count >= 0) {
                bool any_in_range = false;
                for (int i = 0; i < g_saved_wifi_count && !any_in_range; i++) {
                    for (int j = 0; j < scan_count; j++) {
                        if (strcmp(scan_essids[j], g_saved_wifi_list[i].ssid) == 0) {
                            any_in_range = true;
                            break;
                        }
                    }
                }
                if (any_in_range) {
                    break;  /* 扫描有效且有已保存WiFi在场 */
                }
                WATCH_DBG_LOG("[WiFi] No saved WiFi in scan (attempt %d/2)\n",
                              scan_try + 1);
                scan_count = -1;
            }
            /* 首次失败：等1秒让驱动稳定后重试；第二次仍失败则放弃本轮 */
            if (scan_try == 0) {
                WATCH_DBG_LOG("[WiFi] Pre-scan miss, retry in 1s...\n");
                sleep(1);
            } else {
                WATCH_DBG_LOG("[WiFi] Pre-scan miss after retry, skip this round\n");
            }
        }

        /* 两次扫描都未找到已保存WiFi，跳过本轮（不做盲连） */
        if (scan_count < 0) {
            continue;
        }

        // 遍历所有保存的WiFi，依次尝试连接
        for (int i = 0; i < g_saved_wifi_count && g_is_reconnecting; i++) {
            const char *ssid = g_saved_wifi_list[i].ssid;
            const char *password = g_saved_wifi_list[i].password;

            /* 预扫描有效时，跳过不在场的热点 */
            if (scan_count >= 0) {
                bool in_range = false;
                for (int j = 0; j < scan_count; j++) {
                    if (strcmp(scan_essids[j], ssid) == 0) {
                        in_range = true;
                        break;
                    }
                }
                if (!in_range) {
                    WATCH_DBG_LOG("[WiFi] %s not in scan results, skip\n", ssid);
                    continue;
                }
            }

            WATCH_DBG_LOG("[WiFi] Trying saved WiFi [%d/%d]: %s\n",
                          i + 1, g_saved_wifi_count, ssid);

            // 尝试连接：预扫描已确认AP在场，跳过关联门控直接DHCP
            // （扫描后驱动关联上报滞后，门控会误杀）
            bool connect_success = false;
            if (wifi_connect("wlan0", ssid, password, false) == 0) {
                // 轮询等待连接建立（成功即返回，最长 WIFI_CONNECT_WAIT_MS）
                WATCH_DBG_LOG("[WiFi] Waiting for connection to establish...\n");
                connect_success = wifi_wait_connected("wlan0", ssid,
                                                     WIFI_CONNECT_WAIT_MS);
            }

            if (connect_success) {
                strncpy(g_connected_ssid, ssid, sizeof(g_connected_ssid) - 1);
                g_connected_ssid[sizeof(g_connected_ssid) - 1] = '\0';
                g_connect_time = time(NULL);
                WATCH_DBG_LOG("[WiFi] Auto-reconnect success: %s\n", g_connected_ssid);

                /* 下次开机优先尝试本次命中的WiFi（注意：会重排列表，ssid 指针随之失效） */
                wifi_promote_saved(i);

                /* WiFi连接成功后，自动同步NTP时间 */
#ifdef CONFIG_NETUTILS_NTPCLIENT
                {
                    extern int ntpc_start(void);
                    WATCH_DBG_LOG("[WiFi] Starting NTP time sync...\n");
                    syslog(LOG_INFO, "[WiFi] NTP sync: starting (server: %s)\n",
                           CONFIG_NETUTILS_NTPCLIENT_SERVER);
                    int ntp_ret = ntpc_start();
                    if (ntp_ret >= 0) {
                        /* 等待NTP同步完成 */
                        sleep(3);
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        struct tm *tm_info = localtime(&ts.tv_sec);
                        char time_buf[64];
                        strftime(time_buf, sizeof(time_buf),
                                 "%Y-%m-%d %H:%M:%S", tm_info);
                        syslog(LOG_INFO,
                               "[WiFi] NTP sync OK: %s (CST-8)\n", time_buf);
                        WATCH_DBG_LOG("[WiFi] NTP sync OK: %s\n", time_buf);
                    } else {
                        syslog(LOG_WARNING,
                               "[WiFi] NTP sync failed: %d\n", ntp_ret);
                        WATCH_DBG_LOG("[WiFi] NTP sync failed: %d\n", ntp_ret);
                    }
                }
#endif
                break;
            }

            WATCH_DBG_LOG("[WiFi] Connection to %s failed, trying next...\n", ssid);

            // 断开当前连接，准备尝试下一个WiFi
            if (i < g_saved_wifi_count - 1) {
                wifi_disconnect("wlan0");
                usleep(300 * 1000);
                // 重新启用WiFi接口
                wifi_enable("wlan0");
                usleep(300 * 1000);
            }
        }
    }
    
    if (strlen(g_connected_ssid) == 0) {
        WATCH_DBG_LOG("[WiFi] All saved WiFi connection attempts failed\n");
    }
    
    g_is_reconnecting = false;
    WATCH_DBG_LOG("[WiFi] Reconnect thread finished, connected_ssid=%s", 
           strlen(g_connected_ssid) > 0 ? g_connected_ssid : "none");
    
    return NULL;
}

/* WiFi自动重连检查 */
static void wifi_auto_reconnect_check(void)
{
    WATCH_DBG_LOG("[WiFi] Checking auto-reconnect, saved_count=%d, connected_ssid=%s",
           g_saved_wifi_count,
           strlen(g_connected_ssid) > 0 ? g_connected_ssid : "none");
    
    // 如果没有保存的WiFi信息，直接返回
    if (g_saved_wifi_count == 0) {
        WATCH_DBG_LOG("[WiFi] No saved WiFi for auto-reconnect");
        return;
    }
    
    // 如果已经在连接或正在重连，直接返回
    if (strlen(g_connected_ssid) > 0) {
        WATCH_DBG_LOG("[WiFi] Already connected to: %s", g_connected_ssid);
        return;
    }
    
    if (g_is_reconnecting) {
        WATCH_DBG_LOG("[WiFi] Already reconnecting");
        return;
    }
    
    g_is_reconnecting = true;
    
    // 在单独线程中执行重连
    pthread_t tid;
    if (pthread_create(&tid, NULL, wifi_reconnect_thread, NULL) != 0) {
        WATCH_DBG_LOG("[WiFi] Failed to create reconnect thread");
        g_is_reconnecting = false;
        return;
    }
    pthread_detach(tid);
}

/* WiFi 列表项点击事件 */
static void wifi_list_item_click_cb(lv_event_t *e)
{
    lv_obj_t *item = lv_event_get_target(e);
    const char *ssid = (const char *)lv_obj_get_user_data(item);
    
    WATCH_DBG_LOG("[WiFi] Clicked on: %s", ssid);
    
    // 如果点击的是保存过的WiFi
    const char *saved_pwd = wifi_get_saved_password(ssid);
    if (saved_pwd != NULL) {
        WATCH_DBG_LOG("[WiFi] %s is a saved WiFi", ssid);
        
        // 设置重连标志，防止自动重连干扰
        g_is_reconnecting = true;
        
        // 如果当前有连接，先断开
        if (strlen(g_connected_ssid) > 0 && strcmp(g_connected_ssid, ssid) != 0) {
            WATCH_DBG_LOG("[WiFi] Disconnecting current WiFi: %s", g_connected_ssid);
            wifi_disconnect("wlan0");
            sleep(1);
        }
        
        // 连接新的WiFi
        WATCH_DBG_LOG("[WiFi] Connecting to: %s", ssid);
        wifi_connect("wlan0", ssid, saved_pwd, false);
        
        // 等待连接建立
        sleep(3);
        
        // 验证连接状态
        bool connect_success = false;
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            char current_essid[WAPI_ESSID_MAX_SIZE + 1];
            memset(current_essid, 0, sizeof(current_essid));
            
            // 检查ESSID
            if (wapi_get_essid(sock, "wlan0", current_essid, NULL) == 0 &&
                strlen(current_essid) > 0 &&
                strcmp(current_essid, ssid) == 0) {
                connect_success = true;
                WATCH_DBG_LOG("[WiFi] Connection verified by ESSID: %s", current_essid);
            } else {
                // 再检查IP地址
                struct in_addr ip_addr;
                if (wapi_get_ip(sock, "wlan0", &ip_addr) == 0 && ip_addr.s_addr != 0) {
                    connect_success = true;
                    WATCH_DBG_LOG("[WiFi] Connection verified by IP: %s", inet_ntoa(ip_addr));
                }
            }
            close(sock);
        }
        
        if (connect_success) {
            // 更新已连接的 SSID
            strncpy(g_connected_ssid, ssid, sizeof(g_connected_ssid) - 1);
            g_connected_ssid[sizeof(g_connected_ssid) - 1] = '\0';
            
            // 记录连接时间
            g_connect_time = time(NULL);
            
            WATCH_DBG_LOG("[WiFi] Connected to: %s", g_connected_ssid);
            
            // 重置智能家居服务器信息
            /* home_control_reset_server removed */
        } else {
            // 连接失败，保留WiFi信息但从当前扫描结果中移除
            WATCH_DBG_LOG("[WiFi] Connection failed, keeping saved WiFi: %s", ssid);
            
            // 从扫描结果中移除（下次扫描会重新添加）
            for (int i = 0; i < g_wifi_scan_data.count; i++) {
                if (strcmp(g_wifi_scan_data.results[i].ssid, ssid) == 0) {
                    for (int j = i; j < g_wifi_scan_data.count - 1; j++) {
                        memcpy(&g_wifi_scan_data.results[j], 
                               &g_wifi_scan_data.results[j + 1], 
                               sizeof(wifi_scan_result_t));
                    }
                    g_wifi_scan_data.count--;
                    WATCH_DBG_LOG("[WiFi] Removed from current scan results: %s", ssid);
                    break;
                }
            }
        }
        
        // 刷新 WiFi 列表 UI
        if (g_list_cont) {
            wifi_update_list_ui(g_list_cont);
        }
        
        // 清除重连标志
        g_is_reconnecting = false;
    } else {
        // 新WiFi，显示密码输入对话框
        create_password_dialog(ssid);
    }
}

/* 更新 WiFi 列表 UI */
static void wifi_update_list_ui(lv_obj_t *list_cont)
{
    if (!list_cont) {
        WATCH_DBG_LOG("[WiFi] list_cont is NULL, cannot update UI");
        return;
    }
    
    WATCH_DBG_LOG("[WiFi] Updating UI, connected_ssid=%s, scan_count=%d, scanning=%d", 
           g_connected_ssid, g_wifi_scan_data.count, g_wifi_scan_data.scanning);
    
    // 清空列表容器
    lv_obj_clean(list_cont);
    
    // 如果有已连接的 WiFi，在顶部显示
    if (strlen(g_connected_ssid) > 0) {
        WATCH_DBG_LOG("[WiFi] Adding connected WiFi to list: %s", g_connected_ssid);
        
        lv_obj_t *connected_item = lv_obj_create(list_cont);
        lv_obj_set_size(connected_item, LV_PCT(100), 50);
        lv_obj_set_style_bg_color(connected_item, lv_color_hex(0x1E3A5F), 0);
        lv_obj_set_style_border_width(connected_item, 0, 0);
        lv_obj_set_style_radius(connected_item, 8, 0);
        lv_obj_set_style_pad_all(connected_item, 5, 0);
        lv_obj_add_flag(connected_item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(connected_item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(connected_item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(connected_item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(connected_item, 8, 0);
        lv_obj_set_style_text_font(connected_item, watch_resource_get_font("MiSans-Regular_20"), 0);
        
        // 使用蓝色圆形背景+白色对勾图标
        lv_obj_t *check_bg = lv_obj_create(connected_item);
        lv_obj_set_size(check_bg, 24, 24);
        lv_obj_set_style_bg_color(check_bg, lv_color_hex(0x1E90FF), 0);
        lv_obj_set_style_radius(check_bg, 12, 0);
        lv_obj_set_style_border_width(check_bg, 0, 0);
        lv_obj_set_style_pad_all(check_bg, 0, 0);
        lv_obj_clear_flag(check_bg, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t *check_label = lv_label_create(check_bg);
        lv_label_set_text(check_label, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(check_label, lv_color_white(), 0);
        lv_obj_align(check_label, LV_ALIGN_CENTER, 0, 0);
        
        lv_obj_t *item_label = lv_label_create(connected_item);
        lv_label_set_text(item_label, g_connected_ssid);
        lv_obj_set_style_text_color(item_label, lv_color_white(), 0);
        lv_obj_set_flex_grow(item_label, 1);
        
        // 右侧信息详情图标（蓝色圆形背景+白色"i"）
        lv_obj_t *info_bg = lv_obj_create(connected_item);
        lv_obj_set_size(info_bg, 24, 24);
        lv_obj_set_style_bg_color(info_bg, lv_color_hex(0x1E90FF), 0);
        lv_obj_set_style_radius(info_bg, 12, 0);
        lv_obj_set_style_border_width(info_bg, 0, 0);
        lv_obj_set_style_pad_all(info_bg, 0, 0);
        lv_obj_clear_flag(info_bg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(info_bg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(info_bg, wifi_show_detail_page, LV_EVENT_CLICKED, NULL);
        
        lv_obj_t *info_label = lv_label_create(info_bg);
        lv_label_set_text(info_label, "i");
        lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
        lv_obj_align(info_label, LV_ALIGN_CENTER, 0, 0);
    } else {
        WATCH_DBG_LOG("[WiFi] No connected WiFi to display");
    }
    
    // 添加"可用网络"标题和刷新按钮
    lv_obj_t *title_cont = lv_obj_create(list_cont);
    lv_obj_set_size(title_cont, LV_PCT(100), 35);
    lv_obj_set_style_bg_color(title_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(title_cont, 0, 0);
    lv_obj_set_style_pad_all(title_cont, 0, 0);
    lv_obj_clear_flag(title_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(title_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_font(title_cont, watch_resource_get_font("MiSans-Regular_20"), 0);
    
    lv_obj_t *available_label = lv_label_create(title_cont);
    lv_label_set_text(available_label, "可用网络");
    lv_obj_set_style_text_color(available_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_pad_left(available_label, 15, 0);
    
    lv_obj_t *refresh_btn = lv_btn_create(title_cont);
    lv_obj_set_size(refresh_btn, 30, 30);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(refresh_btn, 15, 0);
    lv_obj_set_style_border_width(refresh_btn, 0, 0);
    lv_obj_add_event_cb(refresh_btn, wifi_refresh_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(refresh_label, lv_color_white(), 0);
    lv_obj_align(refresh_label, LV_ALIGN_CENTER, 0, 0);
    
    // 如果正在扫描，显示"扫描中..."
    if (g_wifi_scan_data.scanning) {
        lv_obj_t *scanning_label = lv_label_create(list_cont);
        lv_label_set_text(scanning_label, "扫描中...");
        lv_obj_set_style_text_color(scanning_label, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_pad_left(scanning_label, 15, 0);
        return;
    }
    
    // 如果没有扫描结果，显示提示
    if (g_wifi_scan_data.count == 0) {
        lv_obj_t *no_result_label = lv_label_create(list_cont);
        lv_label_set_text(no_result_label, "未找到WiFi网络");
        lv_obj_set_style_text_color(no_result_label, lv_color_hex(0xAAAAAA), 0);
        lv_obj_align(no_result_label, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    
    WATCH_DBG_LOG("[WiFi] Updating UI with %d networks", g_wifi_scan_data.count);
    
    // 添加真实扫描结果
    WATCH_DBG_LOG("[WiFi] Adding scan results to list");
    
    // 先添加保存的WiFi到"可用网络"列表最前面
    for (int s = 0; s < g_saved_wifi_count; s++) {
        const char *saved_ssid = g_saved_wifi_list[s].ssid;
        
        // 跳过当前已连接的WiFi
        if (strlen(g_connected_ssid) > 0 && strcmp(saved_ssid, g_connected_ssid) == 0) {
            continue;
        }
        
        // 检查该保存的WiFi是否在当前扫描结果中，并获取信号信息
        bool in_scan = false;
        int scan_idx = -1;
        for (int i = 0; i < g_wifi_scan_data.count; i++) {
            if (strcmp(g_wifi_scan_data.results[i].ssid, saved_ssid) == 0) {
                in_scan = true;
                scan_idx = i;
                break;
            }
        }
        
        // 只有扫描结果中存在才显示
        if (!in_scan) {
            continue;
        }
        
        WATCH_DBG_LOG("[WiFi] Adding saved WiFi at top of available: %s", saved_ssid);
        
        lv_obj_t *list_item = lv_obj_create(list_cont);
        lv_obj_set_size(list_item, LV_PCT(100), 50);
        lv_obj_set_style_bg_color(list_item, lv_color_hex(0x222222), 0);
        lv_obj_set_style_border_width(list_item, 0, 0);
        lv_obj_set_style_radius(list_item, 8, 0);
        lv_obj_set_style_pad_all(list_item, 5, 0);
        lv_obj_add_flag(list_item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(list_item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(list_item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(list_item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(list_item, 8, 0);
        lv_obj_set_style_text_font(list_item, watch_resource_get_font("MiSans-Regular_20"), 0);
        
        lv_obj_t *signal_img = lv_img_create(list_item);
        lv_img_set_src(signal_img, get_wifi_signal_icon(
            g_wifi_scan_data.results[scan_idx].rssi,
            g_wifi_scan_data.results[scan_idx].is_encrypted));
        
        lv_obj_t *item_label = lv_label_create(list_item);
        lv_label_set_text(item_label, saved_ssid);
        lv_obj_set_style_text_color(item_label, lv_color_white(), 0);
        lv_obj_set_flex_grow(item_label, 1);
        
        lv_obj_t *saved_label = lv_label_create(list_item);
        lv_label_set_text(saved_label, "已保存");
        lv_obj_set_style_text_color(saved_label, lv_color_hex(0x1E90FF), 0);
        
        lv_obj_set_user_data(list_item, (void *)saved_ssid);
        lv_obj_add_event_cb(list_item, wifi_list_item_click_cb, LV_EVENT_CLICKED, NULL);
    }
    
    for (int i = 0; i < g_wifi_scan_data.count && i < 32; i++) {
        // 跳过已连接的 WiFi（避免重复显示）
        if (strlen(g_connected_ssid) > 0 && 
            strcmp(g_wifi_scan_data.results[i].ssid, g_connected_ssid) == 0) {
            WATCH_DBG_LOG("[WiFi] Skipping connected: %s", g_wifi_scan_data.results[i].ssid);
            continue;
        }
        
        // 跳过已保存的WiFi（已经单独添加了）
        if (wifi_find_saved(g_wifi_scan_data.results[i].ssid) >= 0) {
            WATCH_DBG_LOG("[WiFi] Skipping saved (already added): %s", g_wifi_scan_data.results[i].ssid);
            continue;
        }
        
        lv_obj_t *list_item = lv_obj_create(list_cont);
        lv_obj_set_size(list_item, LV_PCT(100), 50);
        lv_obj_set_style_bg_color(list_item, lv_color_hex(0x222222), 0);
        lv_obj_set_style_border_width(list_item, 0, 0);
        lv_obj_set_style_radius(list_item, 8, 0);
        lv_obj_set_style_pad_all(list_item, 5, 0);
        lv_obj_add_flag(list_item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(list_item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(list_item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(list_item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(list_item, 8, 0);
        lv_obj_set_style_text_font(list_item, watch_resource_get_font("MiSans-Regular_20"), 0);
        
        lv_obj_t *signal_img = lv_img_create(list_item);
        lv_img_set_src(signal_img, get_wifi_signal_icon(
            g_wifi_scan_data.results[i].rssi,
            g_wifi_scan_data.results[i].is_encrypted));
        
        lv_obj_t *item_label = lv_label_create(list_item);
        lv_label_set_text(item_label, g_wifi_scan_data.results[i].ssid);
        lv_obj_set_style_text_color(item_label, lv_color_white(), 0);
        lv_obj_set_flex_grow(item_label, 1);
        
        // 保存 SSID 到 user_data 并添加点击事件
        lv_obj_set_user_data(list_item, (void *)g_wifi_scan_data.results[i].ssid);
        lv_obj_add_event_cb(list_item, wifi_list_item_click_cb, LV_EVENT_CLICKED, NULL);
    }
}

/* WiFi断开连接函数 */
static int wifi_disconnect(const char *ifname)
{
    char command[256];
    
    syslog(LOG_INFO, "[WiFi] Disconnecting from %s", ifname);
    
    // 断开ESSID
    snprintf(command, sizeof(command), "wapi essid %s \"\" 0", ifname);
    wifi_system_cmd(command);
    
    // 关闭接口
    snprintf(command, sizeof(command), "ifdown %s", ifname);
    wifi_system_cmd(command);
    
    syslog(LOG_INFO, "[WiFi] Disconnected");
    return 0;
}

/* WiFi详情页面 - 返回按钮回调 */
static void wifi_detail_back_cb(lv_event_t *e)
{
    lv_obj_t *detail_page = lv_event_get_user_data(e);
    if (detail_page) {
        lv_watch_pop_page(detail_page);
        lv_obj_del(detail_page);
    }
}

/* WiFi详情页面侧滑返回处理 */
static void wifi_detail_slide_handler(lv_event_t *e)
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

                // 右滑超过50px且水平位移大于垂直位移时，关闭详情页面
                if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                    lv_obj_t *detail_page = lv_event_get_target(e);
                    if (detail_page) {
                        lv_watch_pop_page(detail_page);
                        lv_obj_del(detail_page);
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

/* WiFi详情页面 - 断开连接回调 */
static void wifi_detail_disconnect_cb(lv_event_t *e)
{
    WATCH_DBG_LOG("[WiFi] Disconnecting...");
    
    // 断开WiFi连接
    wifi_disconnect("wlan0");
    
    // 清除已连接状态（但保留保存的信息用于重连）
    g_connected_ssid[0] = '\0';
    
    // 重置智能家居服务器信息
    /* home_control_reset_server removed */
    
    // 关闭详情页面
    lv_obj_t *detail_page = lv_event_get_user_data(e);
    if (detail_page) {
        lv_obj_del(detail_page);
    }
    
    // 刷新WiFi列表UI
    if (g_list_cont) {
        wifi_update_list_ui(g_list_cont);
    }
    
    WATCH_DBG_LOG("[WiFi] Disconnected, saved_count=%d, timer=%p", 
           g_saved_wifi_count, g_update_timer);
}

/* WiFi详情页面 - 忘记网络回调 */
static void wifi_detail_forget_cb(lv_event_t *e)
{
    WATCH_DBG_LOG("[WiFi] Forgetting network...");
    
    // 断开WiFi连接
    wifi_disconnect("wlan0");
    
    // 删除保存的WiFi信息
    if (strlen(g_connected_ssid) > 0) {
        wifi_forget(g_connected_ssid);
    }
    
    // 清除已连接状态
    g_connected_ssid[0] = '\0';
    
    // 重置智能家居服务器信息
    /* home_control_reset_server removed */
    
    // 关闭详情页面
    lv_obj_t *detail_page = lv_event_get_user_data(e);
    if (detail_page) {
        lv_obj_del(detail_page);
    }
    
    // 刷新WiFi列表UI
    if (g_list_cont) {
        wifi_update_list_ui(g_list_cont);
    }
}

/* 显示WiFi详情页面 */
static void wifi_show_detail_page(lv_event_t *e)
{
    (void)e;
    WATCH_DBG_LOG("[WiFi] Showing detail page");
    
    // 创建详情页面容器
    lv_obj_t *detail_page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(detail_page, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(detail_page, lv_color_hex(0x0A1628), 0);
    lv_obj_set_style_border_width(detail_page, 0, 0);
    lv_obj_set_style_radius(detail_page, 0, 0);
    lv_obj_set_style_pad_all(detail_page, 0, 0);
    lv_obj_align(detail_page, LV_ALIGN_CENTER, 0, 0);
    /* 设置中文字体 */
    lv_obj_set_style_text_font(detail_page, watch_resource_get_font("MiSans-Regular_20"), 0);
    
    // 添加侧滑返回手势处理
    lv_obj_add_event_cb(detail_page, wifi_detail_slide_handler, LV_EVENT_ALL, NULL);

    // 将详情页压入页面栈，支持PWR短按返回主页
    lv_watch_push_page(detail_page);
    
    // 顶部返回按钮和标题
    lv_obj_t *header = lv_obj_create(detail_page);
    lv_obj_set_size(header, LV_HOR_RES, 50);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0A1628), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    lv_obj_t *back_btn = lv_label_create(header);
    lv_label_set_text(back_btn, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_btn, lv_color_white(), 0);
    lv_obj_set_style_pad_left(back_btn, 15, 0);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, wifi_detail_back_cb, LV_EVENT_CLICKED, detail_page);
    
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, g_connected_ssid);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_pad_left(title, 15, 0);
    
    // 信息列表容器
    lv_obj_t *info_list = lv_obj_create(detail_page);
    lv_obj_set_size(info_list, LV_HOR_RES - 30, 280);
    lv_obj_set_style_bg_color(info_list, lv_color_hex(0x0A1628), 0);
    lv_obj_set_style_border_width(info_list, 0, 0);
    lv_obj_set_style_pad_all(info_list, 0, 0);
    lv_obj_set_pos(info_list, 15, 60);
    lv_obj_set_flex_flow(info_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(info_list, 12, 0);
    
    // 获取网络信息
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct in_addr ip_addr, netmask, gateway, dns;
    memset(&ip_addr, 0, sizeof(ip_addr));
    memset(&netmask, 0, sizeof(netmask));
    memset(&gateway, 0, sizeof(gateway));
    memset(&dns, 0, sizeof(dns));
    
    if (sock >= 0) {
        wapi_get_ip(sock, "wlan0", &ip_addr);
        wapi_get_netmask(sock, "wlan0", &netmask);
        dns = ip_addr;
        dns.s_addr = htonl((ntohl(ip_addr.s_addr) & 0xFFFFFF00) | 0x00000001);
        gateway = dns;
        close(sock);
    }
    
    char ip_str[32], mask_str[32], gw_str[32], dns_str[32];
    strcpy(ip_str, inet_ntoa(ip_addr));
    strcpy(mask_str, inet_ntoa(netmask));
    strcpy(gw_str, inet_ntoa(gateway));
    strcpy(dns_str, inet_ntoa(dns));
    
    // 添加信息项
    const char *info_labels[] = {"Wi-Fi接入方式", "IP地址", "子网掩码", "网关地址", "DNS"};
    const char *info_values[] = {"DHCP", 
                                  strlen(ip_str) > 1 ? ip_str : "获取中...",
                                  strlen(mask_str) > 1 ? mask_str : "获取中...",
                                  strlen(gw_str) > 1 ? gw_str : "获取中...",
                                  strlen(dns_str) > 1 ? dns_str : "获取中..."};
    
    for (int i = 0; i < 5; i++) {
        lv_obj_t *item = lv_obj_create(info_list);
        lv_obj_set_size(item, LV_HOR_RES - 30, 40);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x0A1628), 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        
        lv_obj_t *label_obj = lv_label_create(item);
        lv_label_set_text(label_obj, info_labels[i]);
        lv_obj_set_style_text_color(label_obj, lv_color_hex(0xAAAAAA), 0);
        
        lv_obj_t *value_obj = lv_label_create(item);
        lv_label_set_text(value_obj, info_values[i]);
        lv_obj_set_style_text_color(value_obj, lv_color_white(), 0);
    }
    
    // 底部按钮
    lv_obj_t *btn_cont = lv_obj_create(detail_page);
    lv_obj_set_size(btn_cont, LV_HOR_RES - 30, 60);
    lv_obj_set_style_bg_color(btn_cont, lv_color_hex(0x0A1628), 0);
    lv_obj_set_style_border_width(btn_cont, 0, 0);
    lv_obj_set_style_pad_all(btn_cont, 0, 0);
    lv_obj_set_pos(btn_cont, 15, 360);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // 断开连接按钮
    lv_obj_t *disconnect_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(disconnect_btn, (LV_HOR_RES - 50) / 2, 45);
    lv_obj_set_style_bg_color(disconnect_btn, lv_color_hex(0x1E90FF), 0);
    lv_obj_set_style_radius(disconnect_btn, 10, 0);
    lv_obj_add_event_cb(disconnect_btn, wifi_detail_disconnect_cb, LV_EVENT_CLICKED, detail_page);
    
    lv_obj_t *disconnect_label = lv_label_create(disconnect_btn);
    lv_label_set_text(disconnect_label, "断开连接");
    lv_obj_set_style_text_color(disconnect_label, lv_color_white(), 0);
    lv_obj_align(disconnect_label, LV_ALIGN_CENTER, 0, 0);
    
    // 忘记网络按钮
    lv_obj_t *forget_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(forget_btn, (LV_HOR_RES - 50) / 2, 45);
    lv_obj_set_style_bg_color(forget_btn, lv_color_hex(0x1E90FF), 0);
    lv_obj_set_style_radius(forget_btn, 10, 0);
    lv_obj_add_event_cb(forget_btn, wifi_detail_forget_cb, LV_EVENT_CLICKED, detail_page);
    
    lv_obj_t *forget_label = lv_label_create(forget_btn);
    lv_label_set_text(forget_label, "忘记网络");
    lv_obj_set_style_text_color(forget_label, lv_color_white(), 0);
    lv_obj_align(forget_label, LV_ALIGN_CENTER, 0, 0);
}

/* WiFi连接状态检查定时器回调 */
static void wifi_status_check_timer_cb(lv_timer_t *timer)
{
    // 如果WiFi未启用或没有连接的WiFi，不检查
    if (!g_wifi_enabled || strlen(g_connected_ssid) == 0) {
        return;
    }
    
    // 连接建立后的前10秒不检查，给网络足够时间稳定
    if (g_connect_time > 0) {
        time_t now = time(NULL);
        if (now - g_connect_time < 10) {
            return;
        }
    }
    
    // 使用netlib_getifstatus检查网络接口是否运行
    uint8_t if_flags = 0;
    int ret = netlib_getifstatus("wlan0", &if_flags);
    
    // WATCH_DBG_LOG("[WiFi] Interface status: ret=%d, flags=0x%02x, running=%d", 
    //        ret, if_flags, IFF_IS_RUNNING(if_flags));
    
    if (ret == 0 && IFF_IS_RUNNING(if_flags)) {
        // 接口正在运行
        g_status_fail_count = 0;
    } else {
        g_status_fail_count++;
        WATCH_DBG_LOG("[WiFi] Interface not running (%d)", g_status_fail_count);
        
        // 连续失败3次才认为断开
        if (g_status_fail_count >= 3) {
            WATCH_DBG_LOG("[WiFi] Connection lost after %d failures: %s\n", g_status_fail_count, g_connected_ssid);
                        
            // 清除连接状态（保留保存的WiFi信息）
            g_connected_ssid[0] = '\0';
            g_connect_time = 0;
            g_status_fail_count = 0;
                        
            // 重置智能家居服务器信息
            /* home_control_reset_server removed */
                        
            // 刷新UI
            if (g_list_cont) {
                wifi_update_list_ui(g_list_cont);
            }
                        
            // 尝试自动重连
            wifi_auto_reconnect_check();
        }
    }
}

static void wifi_scan_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    // 每次都更新 UI（显示"扫描中..."或扫描结果）
    wifi_update_list_ui(g_list_cont);

    // 如果扫描完成且不在重连中，停止定时器
    if (!g_wifi_scan_data.scanning && !g_is_reconnecting) {
        WATCH_DBG_LOG("[WiFi] Scan completed, stopping timer");
        if (g_update_timer) {
            lv_timer_del(g_update_timer);
            g_update_timer = NULL;
        }
    }
}

static void wifi_switch_event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * sw = lv_event_get_target(e);
        bool is_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
        
        lv_obj_t * list_cont = lv_obj_get_user_data(sw);
        if(list_cont) {
            if(is_on) {
                // 启用 WiFi
                WATCH_DBG_LOG("[WiFi] Enabling WiFi...");
                g_wifi_enabled = true;  // 保存开关状态
                wifi_save_enabled(true);  // 持久化开关状态
                if (wifi_enable("wlan0") == 0) {
                    // 启动扫描
                    WATCH_DBG_LOG("[WiFi] Starting scan...");
                    wifi_start_scan();
                    
                    // 保存列表容器指针
                    g_list_cont = list_cont;
                    
                    // 启动定时器检查扫描状态（每500ms检查一次）
                    if (g_update_timer) {
                        lv_timer_del(g_update_timer);
                    }
                    g_update_timer = lv_timer_create(wifi_scan_timer_cb, 500, NULL);
                    
                    // 启动定时器检查连接状态（每3秒检查一次）
                    if (g_status_timer) {
                        lv_timer_del(g_status_timer);
                    }
                    g_status_timer = lv_timer_create(wifi_status_check_timer_cb, 3000, NULL);
                    
                    // 尝试自动重连上次连接的WiFi
                    wifi_auto_reconnect_check();
                }
                
                lv_obj_clear_flag(list_cont, LV_OBJ_FLAG_HIDDEN);
            } else {
                // 禁用 WiFi
                WATCH_DBG_LOG("[WiFi] Disabling WiFi...");
                g_wifi_enabled = false;  // 保存开关状态
                wifi_save_enabled(false);  // 持久化开关状态
                wifi_disable("wlan0");
                
                // 清除已连接的 WiFi 状态（但保留保存的WiFi信息用于重连）
                g_connected_ssid[0] = '\0';
                WATCH_DBG_LOG("[WiFi] Cleared connected WiFi state (saved: %d)", 
                       g_saved_wifi_count);
                
                // 重置智能家居服务器信息
    /* home_control_reset_server removed: no home_control dependency */
                
                // 停止定时器
                if (g_update_timer) {
                    lv_timer_del(g_update_timer);
                    g_update_timer = NULL;
                }
                if (g_status_timer) {
                    lv_timer_del(g_status_timer);
                    g_status_timer = NULL;
                }
                
                lv_obj_add_flag(list_cont, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void wifi_slide_gesture_handler(lv_event_t * e)
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

                // 右滑超过50px且水平位移大于垂直位移时，执行返回操作
                if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                    lv_obj_t * cont = lv_event_get_target(e);
                    if(cont) {
                        lv_watch_pop_page(cont);
                        lv_obj_del(cont);
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

static void settings_wifi_create(lv_obj_t *parent)
{
    // 首次进入时加载保存的WiFi配置
    static bool config_loaded = false;
    if (!config_loaded) {
        wifi_load_from_file();
        config_loaded = true;
    }
    
    // 修改：参考 settings_date.c 和 settings.c，不再全屏覆盖，而是创建居中且大小合适的页面
    lv_obj_t *scr = lv_scr_act();
    
    // 创建主容器，作为WiFi设置页面
    lv_obj_t *cont = lv_obj_create(scr);
    // 修改：设置固定大小或带边距的大小，使其不超过设置页面的范围
    // 假设设置页面列表大小为 WATCH_SCREEN_WIDTH (410) x WATCH_SCREEN_HEIGHT (502)
    // 这里我们让WiFi页面与设置列表大小一致，或者稍小一点并居中
    lv_obj_set_size(cont, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT); 
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 0); // 居中对齐
    
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0); 
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    
    // WiFi 开关
    lv_obj_t *switch_cont = lv_obj_create(cont);
    lv_obj_set_size(switch_cont, LV_PCT(100), 60);
    // 修改：背景色改为深灰色，与 settings 按钮风格接近
    lv_obj_set_style_bg_color(switch_cont, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(switch_cont, 0, 0);
    lv_obj_set_flex_flow(switch_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(switch_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(switch_cont, 15, 0);
    lv_obj_set_style_pad_right(switch_cont, 15, 0);

    lv_obj_t *switch_label = lv_label_create(switch_cont);
    // 修改：标签文本改为 WLAN
    lv_label_set_text(switch_label, "WLAN");
    lv_obj_set_style_text_color(switch_label, lv_color_white(), 0);

    lv_obj_t *wifi_switch = lv_switch_create(switch_cont);
    lv_obj_set_size(wifi_switch, 50, 25);
    
    // 根据保存的状态初始化开关
    if (g_wifi_enabled) {
        lv_obj_add_state(wifi_switch, LV_STATE_CHECKED);
    }
    
    // WiFi 列表容器
    lv_obj_t *list_cont = lv_obj_create(cont);
    // 修改：宽度100%，高度自适应内容或占据剩余空间
    lv_obj_set_width(list_cont, LV_PCT(100));
    // 修改：背景色改为黑色
    lv_obj_set_style_bg_color(list_cont, lv_color_hex(0x000000), 0); 
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(list_cont, 1); // 占据剩余垂直空间
    lv_obj_set_style_pad_top(list_cont, 10, 0);
    // 根据 WiFi 开关状态决定是否隐藏
    if (!g_wifi_enabled) {
        lv_obj_add_flag(list_cont, LV_OBJ_FLAG_HIDDEN); // 初始隐藏
    }
    
    // 新增：允许列表容器垂直滚动，以显示所有WiFi项
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_cont, LV_SCROLLBAR_MODE_AUTO); // 可选：显示滚动条

    // 将 list_cont 保存到 wifi_switch 的 user_data 中，以便在回调中访问
    lv_obj_set_user_data(wifi_switch, list_cont);
    
    // 绑定开关事件
    lv_obj_add_event_cb(wifi_switch, wifi_switch_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
    
    // 如果 WiFi 已启用，先取消后台重连（用户手动操作优先）
    if (g_wifi_enabled) {
        if (g_is_reconnecting) {
            WATCH_DBG_LOG("[WiFi] Cancelling background reconnect for user scan");
            g_is_reconnecting = false;
            usleep(200 * 1000);  /* 给线程一点时间退出 */
        }

        /* 仅在 WiFi 未连接时重置接口：
         * 如果已连接（g_connected_ssid 非空），驱动处于干净状态，可直接扫描；
         * 如果未连接，重连线程可能在连接中间被取消导致驱动脏状态，
         * 需要 ifdown→ifup 循环让驱动回到干净状态。 */
        if (strlen(g_connected_ssid) == 0) {
            syslog(LOG_INFO, "[WiFi] Resetting WiFi interface for clean scan (not connected)");
            wifi_disable("wlan0");
            usleep(300 * 1000);
            wifi_enable("wlan0");
            usleep(500 * 1000);
        } else {
            syslog(LOG_INFO, "[WiFi] WiFi connected to '%s', scanning without reset", g_connected_ssid);
        }

        WATCH_DBG_LOG("[WiFi] WiFi already enabled, starting scan...");
        wifi_start_scan();
        g_list_cont = list_cont;
        if (g_update_timer) {
            lv_timer_del(g_update_timer);
        }
        g_update_timer = lv_timer_create(wifi_scan_timer_cb, 500, NULL);
        
        // 启动连接状态检查定时器
        if (g_status_timer) {
            lv_timer_del(g_status_timer);
        }
        g_status_timer = lv_timer_create(wifi_status_check_timer_cb, 3000, NULL);
    }
    
    // 添加滑动手势处理，用于返回
    lv_obj_add_event_cb(cont, wifi_slide_gesture_handler, LV_EVENT_ALL, NULL);

    // 将WiFi页面压入页面栈，支持PWR短按返回主页
    lv_watch_push_page(cont);
}

/* WiFi开机自动初始化：加载配置、恢复开关状态、自动连接已保存的WiFi */
void settings_wifi_auto_init(void)
{
    WATCH_DBG_LOG("[WiFi] Auto init starting...\n");

    /* 1. 加载已保存的WiFi列表 */
    wifi_load_from_file();

    /* 2. 加载WiFi开关状态 */
    g_wifi_enabled = wifi_load_enabled();

    WATCH_DBG_LOG("[WiFi] Auto init: enabled=%d, saved_count=%d\n",
           g_wifi_enabled, g_saved_wifi_count);

    /* 3. 如果没有保存的WiFi配置，保持关闭状态 */
    if (g_wifi_enabled && g_saved_wifi_count == 0) {
        WATCH_DBG_LOG("[WiFi] No saved WiFi config, keeping WiFi disabled\n");
        g_wifi_enabled = false;
        wifi_save_enabled(false);
        return;
    }

    /* 4. 如果WiFi未启用，不做任何操作 */
    if (!g_wifi_enabled) {
        WATCH_DBG_LOG("[WiFi] WiFi was not enabled, skipping auto-connect\n");
        return;
    }

    /* 5. 启用WiFi接口（使用system("ifup")而非wapi_set_ifup，
     *    后者在开机早期驱动未完全就绪时可能触发 StoreProhibited） */
    syslog(LOG_INFO, "[WiFi] Enabling WiFi interface for auto-connect...");
    system("ifup wlan0 > /dev/null 2>&1");
    sleep(1);  /* 等待驱动稳定 */

    /* 6. 启动连接状态检查定时器（检测断线后自动重连） */
    if (g_status_timer) {
        lv_timer_del(g_status_timer);
    }
    g_status_timer = lv_timer_create(wifi_status_check_timer_cb, 3000, NULL);

    /* 7. 在后台线程中尝试自动连接已保存的WiFi */
    wifi_auto_reconnect_check();

    WATCH_DBG_LOG("[WiFi] Auto init completed, reconnect thread started\n");
}

void settings_wifi_event_cb(lv_event_t *e) 
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        // 修改：传入 NULL 或任意对象，因为 create 函数内部已改为使用 lv_scr_act()
        // 这里为了兼容接口，仍然调用，但内部逻辑已变更
        settings_wifi_create(NULL);
    }
}


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
#include <time.h>
#include <syslog.h>
#include <fcntl.h>
#include <errno.h>
#include <nuttx/timers/rtc.h>
#include "netutils/netlib.h"
#ifdef CONFIG_NETUTILS_NTPCLIENT
#  include "netutils/ntpclient.h"
#endif
#include "../common/watch_pages.h"
#include "../../resource/resource.h"
#include "../home_control/home_control.h"
#include "../launcher/launcher.h"
#include "../../resource/image/generated/lvgl_assets.h"

#define WATCH_DBG_LOG(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

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
    wifi_scan_result_t results[16];
    int count;
    bool scanning;
} wifi_scan_data_t;

/* 保存的WiFi信息 */
typedef struct {
    char ssid[64];
    char password[64];
} saved_wifi_t;

/* 保存的WiFi列表 - 大型静态数据改为堆分配 */
#define MAX_SAVED_WIFI 5

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
static lv_timer_t *g_connect_check_timer = NULL;  // 定时器用于检查用户主动连接完成状态
/* g_selected_ssid, g_wifi_password, g_connected_ssid 已移至堆分配 */
static lv_obj_t *g_password_label = NULL;  // 密码显示标签
static lv_obj_t *g_keyboard_cont = NULL;  // 键盘容器
static int g_keyboard_mode = 0;  // 0: 小写字母, 1: 大写字母, 2: 数字符号
static lv_obj_t *g_dialog_mask = NULL;  // 对话框遮罩
static bool g_wifi_enabled = false;  // WiFi 开关状态
static bool g_is_reconnecting = false;  // 是否正在重连
static bool g_is_connecting = false;    // 是否正在连接中（用户主动点击连接）
static char g_connecting_ssid[64] = {0}; // 正在连接的SSID（用于UI显示"连接中"）
static bool g_has_pending_connect = false;  // 用户连接请求被重连阻塞，等待重连结束后执行
static volatile bool g_abort_reconnect = false;  // 通知重连线程中止
static char g_pending_ssid[64] = {0};  // 待连接的SSID
static char g_pending_pwd[64] = {0};  // 待连接的密码
static time_t g_connect_time = 0;  // 连接建立时间
static int g_status_fail_count = 0;  // 连续状态检查失败次数
static int g_reconnect_retry = 0;  // 重连重试计数
#define MAX_RECONNECT_RETRY 5  // 最多重试5次（每3s检查一次，约15s）
static int g_scan_cycle_count = 0;   // 扫描周期计数（用于递增间隔）
static time_t g_next_scan_time = 0;  // 下次扫描时间戳

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
static void wifi_start_connect_thread(const char *ssid, const char *password);
static void wifi_connect_check_timer_cb(lv_timer_t *timer);
static void wifi_show_saved_detail_dialog(const char *ssid);
static void wifi_saved_detail_connect_cb(lv_event_t *e);
static void wifi_saved_detail_forget_cb(lv_event_t *e);
static void wifi_dialog_cancel_cb(lv_event_t *e);

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
            case 3: return vw_resource_get_img("icon_wifi_signal_lock_3");
            case 2: return vw_resource_get_img("icon_wifi_signal_lock_2");
            case 1: 
            default: return vw_resource_get_img("icon_wifi_signal_lock_1");
        }
    } else {
        switch (level) {
            case 3: return vw_resource_get_img("icon_wifi_signal_3");
            case 2: return vw_resource_get_img("icon_wifi_signal_2");
            case 1:
            default: return vw_resource_get_img("icon_wifi_signal_1");
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
        WATCH_DBG_LOG("[WiFi] Failed to open enabled file for writing");
        return;
    }
    fprintf(fp, "%d\n", enabled ? 1 : 0);
    fclose(fp);
    WATCH_DBG_LOG("[WiFi] Saved enabled state: %d", enabled);
}

/* 从文件加载WiFi开关状态 */
static bool wifi_load_enabled(void)
{
    FILE *fp = fopen(WIFI_ENABLED_FILE, "r");
    if (!fp) {
        WATCH_DBG_LOG("[WiFi] No enabled file found, defaulting to off");
        return false;
    }
    int val = 0;
    if (fscanf(fp, "%d", &val) != 1) {
        val = 0;
    }
    fclose(fp);
    bool enabled = (val == 1);
    WATCH_DBG_LOG("[WiFi] Loaded enabled state: %d", enabled);
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
        g_reconnect_retry = 0;  /* 重置重连重试计数 */
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

/* WiFi 连接函数 */
static int wifi_connect(const char *ifname, const char *ssid, const char *password)
{
    char command[256];
    int ret;
    
    WATCH_DBG_LOG("[WiFi] ===== wifi_connect start: ssid='%s', pwd_len=%d =====",
           ssid, (int)strlen(password));
    
    /* 0. 断开旧关联（仅清除ESSID，不ifdown，避免ESP32驱动被重置） */
    snprintf(command, sizeof(command), "wapi essid %s \"\" 0", ifname);
    WATCH_DBG_LOG("[WiFi] CMD: %s", command);
    ret = system(command);
    WATCH_DBG_LOG("[WiFi]   ret=%d", ret);
    usleep(300000);  /* 300ms 等待驱动断开旧关联 */
    
    /* 1. 确保接口已启用（已UP时为no-op，不影响驱动状态） */
    snprintf(command, sizeof(command), "ifup %s", ifname);
    WATCH_DBG_LOG("[WiFi] CMD: %s", command);
    ret = system(command);
    WATCH_DBG_LOG("[WiFi]   ret=%d", ret);
    usleep(300000);  /* 300ms */
    
    /* 2. 设置 WiFi 模式为 Managed (Station) */
    snprintf(command, sizeof(command), "wapi mode %s 2", ifname);
    WATCH_DBG_LOG("[WiFi] CMD: %s", command);
    ret = system(command);
    WATCH_DBG_LOG("[WiFi]   ret=%d", ret);
    usleep(200000);  /* 200ms */
    
    /* 3. 设置 PSK 密码（CCMP/AES + WPA2，现代路由器默认配置） */
    snprintf(command, sizeof(command), "wapi psk %s \"%s\" 3 2", ifname, password);
    WATCH_DBG_LOG("[WiFi] CMD: wapi psk %s \"***\" 3 2 (pwd_len=%d)", ifname, (int)strlen(password));
    ret = system(command);
    WATCH_DBG_LOG("[WiFi]   ret=%d", ret);
    usleep(200000);  /* 200ms */
    
    /* 4. 设置 ESSID（触发关联） */
    snprintf(command, sizeof(command), "wapi essid %s \"%s\" 1", ifname, ssid);
    WATCH_DBG_LOG("[WiFi] CMD: %s", command);
    ret = system(command);
    WATCH_DBG_LOG("[WiFi]   ret=%d", ret);
    usleep(2000000);  /* 2s 等待驱动完成关联 */
    
    /* 5. 启动 DHCP 获取 IP 地址 */
    snprintf(command, sizeof(command), "ifconfig %s dhcp", ifname);
    WATCH_DBG_LOG("[WiFi] CMD: %s", command);
    ret = system(command);
    WATCH_DBG_LOG("[WiFi]   ret=%d", ret);
    
    WATCH_DBG_LOG("[WiFi] ===== wifi_connect commands completed =====");
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
    lv_obj_set_style_text_font(key_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
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
        /* 连接 WiFi：先保存配置，关闭对话框，再在后台线程中连接 */
        WATCH_DBG_LOG("[WiFi] '连接' clicked: ssid='%s', pwd_len=%d",
               g_selected_ssid, (int)strlen(g_wifi_password));
        if (strlen(g_wifi_password) >= 8) {
            /* 保存WiFi用于自动重连 */
            wifi_save(g_selected_ssid, g_wifi_password);
            WATCH_DBG_LOG("[WiFi] Saved for auto-reconnect: %s", g_selected_ssid);

            /* 先关闭对话框，避免连接期间界面卡死 */
            if (g_dialog_mask) {
                vw_watch_pop_page(g_dialog_mask);
                lv_obj_del_async(g_dialog_mask);
                g_dialog_mask = NULL;
            }

            /* 立即刷新列表UI，让用户看到"已保存"状态 */
            if (g_list_cont) {
                wifi_update_list_ui(g_list_cont);
            }

            /* 在后台线程中连接，避免阻塞UI */
            wifi_start_connect_thread(g_selected_ssid, g_wifi_password);
        } else {
            WATCH_DBG_LOG("[WiFi] Password too short (min 8 characters)");
        }
    } else if (strcmp(text, "取消") == 0) {
        // 取消，关闭对话框
        if (g_dialog_mask) {
            vw_watch_pop_page(g_dialog_mask);
            lv_obj_del_async(g_dialog_mask);
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
                        vw_watch_pop_page(g_dialog_mask);
                        lv_obj_del_async(g_dialog_mask);
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
    vw_watch_push_page(g_dialog_mask);
    
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
    
    // SSID 标题
    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text_fmt(title, "连接到: %s", ssid);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    
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
    lv_obj_set_style_text_font(g_password_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_align(g_password_label, LV_ALIGN_LEFT_MID, 10, 0);
    
    // 提示文本
    lv_obj_t *hint = lv_label_create(dialog);
    lv_label_set_text(hint, "请输入WiFi密码 (至少8位)");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(hint, vw_resource_get_font(WATCH_REGULAR_FONT "_16"), 0);
    
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
    
    /* 轮询等待扫描完成（wapi_scan_coll 返回 -EAGAIN 表示未就绪） */
    memset(&aps, 0, sizeof(aps));
    int scan_retries = 0;
    const int max_scan_retries = 20;  /* 20 * 500ms = 10s max */
    do {
        ret = wapi_scan_coll(sock, ifname, &aps);
        if (ret == 0 || ret != -EAGAIN) break;
        usleep(500000);  /* 500ms */
        scan_retries++;
    } while (scan_retries < max_scan_retries);

    WATCH_DBG_LOG("[WiFi] Scan collected after %d retries (ret=%d)", scan_retries, ret);

    if (ret < 0) {
        WATCH_DBG_LOG("[WiFi] Failed to collect scan results: %d", ret);
        close(sock);
        g_wifi_scan_data.scanning = false;
        return NULL;
    }
    
    /* 遍历扫描结果链表 */
    g_wifi_scan_data.count = 0;
    scan_info = aps.head.scan;
    
    while (scan_info != NULL && g_wifi_scan_data.count < (int)(sizeof(g_wifi_scan_data.results) / sizeof(g_wifi_scan_data.results[0]))) {
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
    
    wifi_start_scan();
    
    if (g_list_cont && !g_update_timer) {
        g_update_timer = lv_timer_create(wifi_scan_timer_cb, 1000, NULL);
    }
}

/* WiFi自动重连线程 — 扫描SSID匹配后连接 */
/* 策略：先扫描可用WiFi，匹配已保存列表，仅连接实际存在的SSID */
static void *wifi_reconnect_thread(void *arg)
{
    WATCH_DBG_LOG("[WiFi] Reconnect thread started");
    
    // 等待WiFi接口就绪
    sleep(1);
    
    if (g_saved_wifi_count == 0) {
        WATCH_DBG_LOG("[WiFi] No saved WiFi for auto-reconnect");
        g_is_reconnecting = false;
        return NULL;
    }
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        WATCH_DBG_LOG("[WiFi] socket() failed: %d", errno);
        g_is_reconnecting = false;
        return NULL;
    }

    /* ── 直接逐个尝试已保存 WiFi（不发起扫描，避免与 UI 扫描冲突） ── */
    int connect_order[MAX_SAVED_WIFI];
    int connect_count = 0;

    for (int i = 0; i < g_saved_wifi_count; i++) {
        connect_order[connect_count++] = i;
        WATCH_DBG_LOG("[WiFi] Will try saved WiFi [%d]: %s",
               i, g_saved_wifi_list[i].ssid);
    }

    if (connect_count == 0) {
        WATCH_DBG_LOG("[WiFi] No WiFi to connect");
        close(sock);
        g_is_reconnecting = false;
        return NULL;
    }
    
    /* ── 第三步：逐个尝试连接 ────────────────────── */
    bool connected = false;
    
    for (int idx = 0; idx < connect_count && !connected; idx++) {
        /* 用户发起新连接请求时中止重连 */
        if (g_abort_reconnect) {
            WATCH_DBG_LOG("[WiFi] Reconnect aborted by user connection request");
            break;
        }

        int i = connect_order[idx];
        const char *ssid = g_saved_wifi_list[i].ssid;
        const char *password = g_saved_wifi_list[i].password;
        
        WATCH_DBG_LOG("[WiFi] Trying [%d/%d]: %s", idx + 1, connect_count, ssid);
        
        /* 设置正在连接的SSID，让UI显示"连接中" */
        strncpy(g_connecting_ssid, ssid, sizeof(g_connecting_ssid) - 1);
        g_connecting_ssid[sizeof(g_connecting_ssid) - 1] = '\0';
        if (g_list_cont) {
            wifi_update_list_ui(g_list_cont);
        }
        
        wifi_connect("wlan0", ssid, password);
        
        /* 轮询等待连接建立（每500ms检查ESSID匹配+IP，最多20秒） */
        bool connect_success = false;
        char current_essid[WAPI_ESSID_MAX_SIZE + 1];
        enum wapi_essid_flag_e essid_flag;
        int retries = 40;  /* 40 * 500ms = 20s max */
        while (retries > 0) {
            if (g_abort_reconnect) {
                WATCH_DBG_LOG("[WiFi] Reconnect aborted during polling");
                break;
            }
            memset(current_essid, 0, sizeof(current_essid));
            if (wapi_get_essid(sock, "wlan0", current_essid, &essid_flag) == 0 &&
                strlen(current_essid) > 0 &&
                strcmp(current_essid, ssid) == 0) {
                /* ESSID匹配目标，检查IP */
                struct in_addr ip_addr;
                if (wapi_get_ip(sock, "wlan0", &ip_addr) == 0 &&
                    ip_addr.s_addr != 0) {
                    connect_success = true;
                    WATCH_DBG_LOG("[WiFi] Auto-reconnect verified: ESSID=%s, IP=%s",
                           current_essid, inet_ntoa(ip_addr));
                    break;
                }
                WATCH_DBG_LOG("[WiFi] ESSID matched but no IP yet, waiting for DHCP (%d retries left)",
                       retries);
            }
            usleep(500000);
            retries--;
        }

        /* 中止后跳出循环 */
        if (g_abort_reconnect) {
            WATCH_DBG_LOG("[WiFi] Reconnect aborted, breaking out of loop");
            break;
        }

        /* 最终兜底：只检查IP（即使ESSID验证失败） */
        if (!connect_success) {
            struct in_addr ip_addr;
            if (wapi_get_ip(sock, "wlan0", &ip_addr) == 0 && ip_addr.s_addr != 0) {
                connect_success = true;
                WATCH_DBG_LOG("[WiFi] Auto-reconnect verified by IP only: %s",
                       inet_ntoa(ip_addr));
            }
        }
        
        if (connect_success) {
            strncpy(g_connected_ssid, ssid, sizeof(g_connected_ssid) - 1);
            g_connected_ssid[sizeof(g_connected_ssid) - 1] = '\0';
            g_connecting_ssid[0] = '\0';  /* 清除"连接中"状态 */
            g_connect_time = time(NULL);  /* 记录连接时间，状态定时器10s宽限 */
            g_reconnect_retry = 0;
            WATCH_DBG_LOG("[WiFi] Auto-reconnect success: %s", g_connected_ssid);
            home_control_reset_server();
            connected = true;
        } else {
            WATCH_DBG_LOG("[WiFi] Auto-reconnect failed for: %s", ssid);
            g_connected_ssid[0] = '\0';
            g_connecting_ssid[0] = '\0';  /* 清除"连接中"状态 */
        }
    }
    

    close(sock);

    /* WiFi连接已完成，先清除重连标志让 UI 定时器能正常退出 */
    g_is_reconnecting = false;
    g_abort_reconnect = false;  /* 清除中止标志 */

    /* WiFi连接成功后，自动同步NTP时间 */
#ifdef CONFIG_NETUTILS_NTPCLIENT
    if (strlen(g_connected_ssid) > 0) {
        syslog(LOG_INFO, "[WiFi] NTP sync: starting (server: %s)\n",
               CONFIG_NETUTILS_NTPCLIENT_SERVER);
        int ntp_ret = ntpc_start();
        if (ntp_ret >= 0) {
            /* ntpc_start() 是异步的：真正的时间同步由后台 daemon 完成
             * （DNS 解析 + 多轮采样），这里轮询 ntpc_status() 直到
             * daemon 至少采到 1 个样本，确保同步真正生效。
             */
            bool synced = false;
            for (int i = 0; i < 30; i++) {
                struct ntpc_status_s st;
                memset(&st, 0, sizeof(st));
                if (ntpc_status(&st) == 0 && st.nsamples > 0) {
                    synced = true;
                    break;
                }
                sleep(1);
            }

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);

            if (synced) {
                /* 同步成功：将本地时间写回 RTC（RTC 存本地时间，
                 * clock_basetime 用 timegm 当 UTC 读入，watch_main
                 * 启动时减去 tz_offset 修正为真正 UTC）。
                 */
                struct tm local_tm;
                localtime_r(&ts.tv_sec, &local_tm);
                int fd = open("/dev/rtc0", O_RDWR);
                if (fd >= 0) {
                    struct rtc_time rtctime;
                    memset(&rtctime, 0, sizeof(rtctime));
                    rtctime.tm_sec   = local_tm.tm_sec;
                    rtctime.tm_min   = local_tm.tm_min;
                    rtctime.tm_hour  = local_tm.tm_hour;
                    rtctime.tm_mday  = local_tm.tm_mday;
                    rtctime.tm_mon   = local_tm.tm_mon;
                    rtctime.tm_year  = local_tm.tm_year;
                    rtctime.tm_wday  = local_tm.tm_wday;
                    int rtc_ret = ioctl(fd, RTC_SET_TIME,
                                        (unsigned long)&rtctime);
                    close(fd);
                    if (rtc_ret >= 0) {
                        syslog(LOG_INFO,
                               "[WiFi] NTP time written to RTC\n");
                        /* RTC_SET_TIME 可能同时更新了 CLOCK_REALTIME
                         * （把本地时间当 UTC 写入），导致 localtime 再 +8
                         * 快 8 小时。恢复 CLOCK_REALTIME 为 NTP 同步的
                         * 正确 UTC 时间。 */
                        struct timeval tv_restore;
                        tv_restore.tv_sec  = ts.tv_sec;
                        tv_restore.tv_usec = ts.tv_nsec / 1000;
                        settimeofday(&tv_restore, NULL);
                    } else {
                        syslog(LOG_WARNING,
                               "[WiFi] Failed to write RTC time, errno=%d\n",
                               errno);
                    }
                } else {
                    syslog(LOG_WARNING,
                           "[WiFi] Cannot open /dev/rtc0, errno=%d\n", errno);
                }
            }

            struct tm *tm_info = localtime(&ts.tv_sec);
            char time_buf[64];
            strftime(time_buf, sizeof(time_buf),
                     "%Y-%m-%d %H:%M:%S", tm_info);
            syslog(LOG_INFO, "[WiFi] NTP sync %s: %s\n",
                   synced ? "OK" : "TIMEOUT", time_buf);
        } else {
            syslog(LOG_WARNING, "[WiFi] NTP sync failed: %d\n", ntp_ret);
        }
    }
#endif

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
    
    // 在单独线程中执行重连（增大栈：扫描+匹配逻辑需要较多空间）
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    if (pthread_create(&tid, &attr, wifi_reconnect_thread, NULL) != 0) {
        WATCH_DBG_LOG("[WiFi] Failed to create reconnect thread");
        g_is_reconnecting = false;
        pthread_attr_destroy(&attr);
        return;
    }
    pthread_attr_destroy(&attr);
    pthread_detach(tid);
}

/* 连接参数结构体（堆分配，传给线程后由线程释放） */
typedef struct {
    char ssid[64];
    char password[64];
} wifi_connect_params_t;

/* 用户主动连接的后台线程 */
static void *wifi_connect_thread(void *arg)
{
    wifi_connect_params_t *params = (wifi_connect_params_t *)arg;

    WATCH_DBG_LOG("[WiFi] Connect thread started: %s", params->ssid);

    /* 等待正在进行的WiFi扫描完成，ESP32驱动不能同时扫描和关联 */
    int scan_wait = 0;
    while (g_wifi_scan_data.scanning && scan_wait < 20) {  /* 最多等10秒 */
        if (scan_wait == 0) {
            WATCH_DBG_LOG("[WiFi] Waiting for scan to finish before connecting...");
        }
        usleep(500000);
        scan_wait++;
    }
    WATCH_DBG_LOG("[WiFi] Scan wait done: waited=%d*500ms, scanning=%d",
           scan_wait, g_wifi_scan_data.scanning);

    /* 连接新的WiFi */
    wifi_connect("wlan0", params->ssid, params->password);

    /* 轮询等待连接建立（每500ms检查ESSID+IP，最多20秒） */
    bool connect_success = false;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    WATCH_DBG_LOG("[WiFi] Verification socket: sock=%d", sock);
    if (sock >= 0) {
        char current_essid[WAPI_ESSID_MAX_SIZE + 1];
        enum wapi_essid_flag_e essid_flag;
        int conn_retries = 40;  /* 40 * 500ms = 20s max */
        while (conn_retries > 0) {
            memset(current_essid, 0, sizeof(current_essid));

            int essid_ret = wapi_get_essid(sock, "wlan0", current_essid, &essid_flag);
            struct in_addr ip_addr;
            int ip_ret = wapi_get_ip(sock, "wlan0", &ip_addr);
            const char *ip_str = (ip_ret == 0 && ip_addr.s_addr != 0) ? inet_ntoa(ip_addr) : "0.0.0.0";

            if (essid_ret == 0 && strlen(current_essid) > 0 &&
                strcmp(current_essid, params->ssid) == 0) {
                /* ESSID匹配目标，检查IP */
                if (ip_ret == 0 && ip_addr.s_addr != 0) {
                    connect_success = true;
                    WATCH_DBG_LOG("[WiFi] SUCCESS: ESSID='%s', IP=%s (retry=%d)",
                           current_essid, ip_str, 40 - conn_retries);
                    break;
                }
                WATCH_DBG_LOG("[WiFi] ESSID OK='%s', no IP yet (retry=%d/40)",
                       current_essid, 40 - conn_retries);
            } else {
                WATCH_DBG_LOG("[WiFi] Poll: essid_ret=%d, essid='%s', ip=%s (retry=%d/40)",
                       essid_ret, essid_ret == 0 ? current_essid : "(err)", ip_str, 40 - conn_retries);
            }
            usleep(500000);
            conn_retries--;
        }

        /* 最终再检查一次IP地址（即使ESSID检查失败） */
        if (!connect_success) {
            struct in_addr ip_addr;
            if (wapi_get_ip(sock, "wlan0", &ip_addr) == 0 && ip_addr.s_addr != 0) {
                connect_success = true;
                WATCH_DBG_LOG("[WiFi] SUCCESS by IP only: %s", inet_ntoa(ip_addr));
            }
        }
        close(sock);
    }

    if (connect_success) {
        strncpy(g_connected_ssid, params->ssid, sizeof(g_connected_ssid) - 1);
        g_connected_ssid[sizeof(g_connected_ssid) - 1] = '\0';
        g_connect_time = time(NULL);
        g_reconnect_retry = 0;
        home_control_reset_server();
        WATCH_DBG_LOG("[WiFi] Connected to: %s", g_connected_ssid);
    } else {
        /* 连接失败，从扫描结果中移除（下次扫描会重新添加） */
        WATCH_DBG_LOG("[WiFi] Connection failed: %s", params->ssid);
        g_connected_ssid[0] = '\0';
        for (int i = 0; i < g_wifi_scan_data.count; i++) {
            if (strcmp(g_wifi_scan_data.results[i].ssid, params->ssid) == 0) {
                for (int j = i; j < g_wifi_scan_data.count - 1; j++) {
                    memcpy(&g_wifi_scan_data.results[j],
                           &g_wifi_scan_data.results[j + 1],
                           sizeof(wifi_scan_result_t));
                }
                g_wifi_scan_data.count--;
                break;
            }
        }
    }

    g_is_connecting = false;
    g_connecting_ssid[0] = '\0';
    free(params);

    WATCH_DBG_LOG("[WiFi] Connect thread finished");
    return NULL;
}

/* 连接完成检查定时器回调：连接线程结束后刷新UI，或重连结束后启动待连接 */
static void wifi_connect_check_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 场景1: 用户连接被重连阻塞，等待重连结束后启动 */
    if (g_has_pending_connect) {
        /* 重连仍在进行，继续等待 */
        if (g_is_reconnecting) {
            return;
        }

        /* 重连已结束，启动用户的待连接 */
        g_has_pending_connect = false;
        char ssid[64], pwd[64];
        strncpy(ssid, g_pending_ssid, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
        strncpy(pwd, g_pending_pwd, sizeof(pwd) - 1);
        pwd[sizeof(pwd) - 1] = '\0';

        /* 如果重连恰好已连接到同一SSID，无需重复连接 */
        if (strlen(g_connected_ssid) > 0 && strcmp(g_connected_ssid, ssid) == 0) {
            WATCH_DBG_LOG("[WiFi] Reconnect already connected to: %s, skipping pending", ssid);
            if (g_list_cont) {
                wifi_update_list_ui(g_list_cont);
            }
            if (g_connect_check_timer) {
                lv_timer_del(g_connect_check_timer);
                g_connect_check_timer = NULL;
            }
            return;
        }

        WATCH_DBG_LOG("[WiFi] Reconnect finished, starting pending user connection: %s", ssid);
        /* wifi_start_connect_thread 会重建此定时器，先删除当前 */
        if (g_connect_check_timer) {
            lv_timer_del(g_connect_check_timer);
            g_connect_check_timer = NULL;
        }
        wifi_start_connect_thread(ssid, pwd);
        return;
    }

    /* 场景2: 用户主动连接中，等待连接线程完成 */
    if (g_is_connecting) {
        return;
    }

    /* 连接完成，刷新WiFi列表UI */
    if (g_list_cont) {
        wifi_update_list_ui(g_list_cont);
    }

    /* 停止定时器 */
    if (g_connect_check_timer) {
        lv_timer_del(g_connect_check_timer);
        g_connect_check_timer = NULL;
    }
}

/* 启动后台线程连接WiFi（避免阻塞UI线程） */
static void wifi_start_connect_thread(const char *ssid, const char *password)
{
    WATCH_DBG_LOG("[WiFi] start_connect: ssid='%s', pwd_len=%d, is_connecting=%d, is_reconnecting=%d",
           ssid, (int)strlen(password), g_is_connecting, g_is_reconnecting);

    /* 用户已在连接中，忽略重复请求 */
    if (g_is_connecting) {
        WATCH_DBG_LOG("[WiFi] Already connecting, ignoring: %s", ssid);
        return;
    }

    /* 自动重连正在进行：排队用户请求并中止重连 */
    if (g_is_reconnecting) {
        WATCH_DBG_LOG("[WiFi] Reconnect in progress, queuing user connection: %s", ssid);
        strncpy(g_pending_ssid, ssid, sizeof(g_pending_ssid) - 1);
        g_pending_ssid[sizeof(g_pending_ssid) - 1] = '\0';
        strncpy(g_pending_pwd, password, sizeof(g_pending_pwd) - 1);
        g_pending_pwd[sizeof(g_pending_pwd) - 1] = '\0';
        g_has_pending_connect = true;
        g_abort_reconnect = true;  /* 通知重连线程中止 */

        /* 启动定时器检查重连是否结束，结束后自动启动用户连接 */
        if (!g_connect_check_timer) {
            g_connect_check_timer = lv_timer_create(wifi_connect_check_timer_cb, 500, NULL);
        }
        return;
    }

    wifi_connect_params_t *params = malloc(sizeof(wifi_connect_params_t));
    if (!params) {
        WATCH_DBG_LOG("[WiFi] malloc failed for connect params");
        return;
    }
    strncpy(params->ssid, ssid, sizeof(params->ssid) - 1);
    params->ssid[sizeof(params->ssid) - 1] = '\0';
    strncpy(params->password, password, sizeof(params->password) - 1);
    params->password[sizeof(params->password) - 1] = '\0';

    g_is_connecting = true;
    strncpy(g_connecting_ssid, ssid, sizeof(g_connecting_ssid) - 1);
    g_connecting_ssid[sizeof(g_connecting_ssid) - 1] = '\0';

    /* 立即刷新列表UI，让用户看到"连接中"状态 */
    if (g_list_cont) {
        wifi_update_list_ui(g_list_cont);
    }

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    if (pthread_create(&tid, &attr, wifi_connect_thread, params) != 0) {
        WATCH_DBG_LOG("[WiFi] Failed to create connect thread");
        g_is_connecting = false;
        free(params);
        pthread_attr_destroy(&attr);
        return;
    }
    pthread_attr_destroy(&attr);
    pthread_detach(tid);

    /* 启动定时器检查连接完成状态（每500ms检查一次） */
    if (g_connect_check_timer) {
        lv_timer_del(g_connect_check_timer);
    }
    g_connect_check_timer = lv_timer_create(wifi_connect_check_timer_cb, 500, NULL);

    WATCH_DBG_LOG("[WiFi] Connect thread started for: %s", ssid);
}

/* 对话框取消按钮回调 */
static void wifi_dialog_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (g_dialog_mask) {
        vw_watch_pop_page(g_dialog_mask);
        lv_obj_del_async(g_dialog_mask);
        g_dialog_mask = NULL;
    }
}

/* 已保存热点详情 - 连接按钮回调 */
static void wifi_saved_detail_connect_cb(lv_event_t *e)
{
    (void)e;
    const char *ssid = g_selected_ssid;
    const char *saved_pwd = wifi_get_saved_password(ssid);

    WATCH_DBG_LOG("[WiFi] Detail dialog connect: %s", ssid);

    /* 关闭对话框 */
    if (g_dialog_mask) {
        vw_watch_pop_page(g_dialog_mask);
        lv_obj_del_async(g_dialog_mask);
        g_dialog_mask = NULL;
    }

    /* 在后台线程中连接 */
    if (saved_pwd != NULL) {
        wifi_start_connect_thread(ssid, saved_pwd);
    } else {
        WATCH_DBG_LOG("[WiFi] No saved password for %s, opening password dialog", ssid);
        create_password_dialog(ssid);
    }
}

/* 已保存热点详情 - 忘记网络按钮回调 */
static void wifi_saved_detail_forget_cb(lv_event_t *e)
{
    (void)e;
    const char *ssid = g_selected_ssid;

    WATCH_DBG_LOG("[WiFi] Detail dialog forget: %s", ssid);

    wifi_forget(ssid);

    /* 关闭对话框 */
    if (g_dialog_mask) {
        vw_watch_pop_page(g_dialog_mask);
        lv_obj_del_async(g_dialog_mask);
        g_dialog_mask = NULL;
    }

    /* 刷新列表UI */
    if (g_list_cont) {
        wifi_update_list_ui(g_list_cont);
    }
}

/* 显示已保存热点详情对话框 */
static void wifi_show_saved_detail_dialog(const char *ssid)
{
    WATCH_DBG_LOG("[WiFi] Showing saved detail dialog for: %s", ssid);

    /* 保存当前选中的SSID */
    strncpy(g_selected_ssid, ssid, sizeof(g_selected_ssid) - 1);
    g_selected_ssid[sizeof(g_selected_ssid) - 1] = '\0';

    /* 从扫描结果中查找该热点的信息 */
    int rssi = 0;
    bool is_encrypted = true;
    int channel = 0;
    for (int i = 0; i < g_wifi_scan_data.count; i++) {
        if (strcmp(g_wifi_scan_data.results[i].ssid, ssid) == 0) {
            rssi = g_wifi_scan_data.results[i].rssi;
            is_encrypted = g_wifi_scan_data.results[i].is_encrypted;
            channel = g_wifi_scan_data.results[i].channel;
            break;
        }
    }
    int level = get_wifi_signal_level(rssi);
    const char *signal_desc[] = {"弱", "中", "强"};
    const char *security_desc = is_encrypted ? "WPA/WPA2" : "开放";

    /* 创建对话框背景遮罩 */
    g_dialog_mask = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_dialog_mask, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(g_dialog_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_dialog_mask, LV_OPA_70, 0);
    lv_obj_align(g_dialog_mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(g_dialog_mask, LV_OBJ_FLAG_SCROLLABLE);

    /* 添加侧滑手势处理 */
    lv_obj_add_event_cb(g_dialog_mask, dialog_slide_gesture_handler, LV_EVENT_ALL, NULL);

    /* 将对话框遮罩压入页面栈，支持PWR短按返回 */
    vw_watch_push_page(g_dialog_mask);

    /* 创建对话框容器 */
    lv_obj_t *dialog = lv_obj_create(g_dialog_mask);
    lv_obj_set_size(dialog, 380, 320);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(dialog, 10, 0);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(dialog, 20, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题：热点详情 */
    lv_obj_t *title_label = lv_label_create(dialog);
    lv_label_set_text(title_label, "WiFi热点详情");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);

    /* SSID名称 */
    lv_obj_t *ssid_label = lv_label_create(dialog);
    lv_label_set_text(ssid_label, ssid);
    lv_obj_set_style_text_color(ssid_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(ssid_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);

    /* 信号强度 + 安全性信息 */
    char info_buf[128];
    snprintf(info_buf, sizeof(info_buf), "信号: %s (%ddBm)  信道: %d", signal_desc[level - 1], rssi, channel);
    lv_obj_t *signal_label = lv_label_create(dialog);
    lv_label_set_text(signal_label, info_buf);
    lv_obj_set_style_text_color(signal_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(signal_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);

    lv_obj_t *sec_label = lv_label_create(dialog);
    lv_label_set_text(sec_label, security_desc);
    lv_obj_set_style_text_color(sec_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(sec_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);

    /* 按钮容器 */
    lv_obj_t *btn_cont = lv_obj_create(dialog);
    lv_obj_set_size(btn_cont, 340, 55);
    lv_obj_set_style_bg_opa(btn_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_cont, 0, 0);
    lv_obj_set_style_pad_all(btn_cont, 0, 0);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* 连接按钮 */
    lv_obj_t *connect_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(connect_btn, 100, 45);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x1E90FF), 0);
    lv_obj_set_style_radius(connect_btn, 10, 0);
    lv_obj_set_style_border_width(connect_btn, 0, 0);
    lv_obj_add_event_cb(connect_btn, wifi_saved_detail_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "连接");
    lv_obj_set_style_text_color(connect_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(connect_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_align(connect_label, LV_ALIGN_CENTER, 0, 0);

    /* 忘记网络按钮 */
    lv_obj_t *forget_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(forget_btn, 110, 45);
    lv_obj_set_style_bg_color(forget_btn, lv_color_hex(0xFF6B6B), 0);
    lv_obj_set_style_radius(forget_btn, 10, 0);
    lv_obj_set_style_border_width(forget_btn, 0, 0);
    lv_obj_add_event_cb(forget_btn, wifi_saved_detail_forget_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *forget_label = lv_label_create(forget_btn);
    lv_label_set_text(forget_label, "忘记网络");
    lv_obj_set_style_text_color(forget_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(forget_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_align(forget_label, LV_ALIGN_CENTER, 0, 0);

    /* 取消按钮 */
    lv_obj_t *cancel_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(cancel_btn, 100, 45);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(cancel_btn, 10, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, wifi_dialog_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(cancel_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_align(cancel_label, LV_ALIGN_CENTER, 0, 0);
}

/* WiFi 列表项点击事件 */
static void wifi_list_item_click_cb(lv_event_t *e)
{
    lv_obj_t *item = lv_event_get_target(e);
    const char *ssid = (const char *)lv_obj_get_user_data(item);

    WATCH_DBG_LOG("[WiFi] Clicked on: %s", ssid);

    /* 已保存的WiFi：显示详情对话框（含连接/忘记网络按钮） */
    const char *saved_pwd = wifi_get_saved_password(ssid);
    if (saved_pwd != NULL) {
        WATCH_DBG_LOG("[WiFi] %s is a saved WiFi, showing detail dialog", ssid);
        wifi_show_saved_detail_dialog(ssid);
    } else {
        /* 新WiFi，显示密码输入对话框 */
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
    
    /* 扫描中且无结果时跳过冗余日志（每秒定时器调用一次） */
    if (!g_wifi_scan_data.scanning || g_wifi_scan_data.count > 0) {
        WATCH_DBG_LOG("[WiFi] Updating UI, connected_ssid=%s, scan_count=%d, scanning=%d",
               g_connected_ssid, g_wifi_scan_data.count, g_wifi_scan_data.scanning);
    }

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
        lv_obj_set_style_text_font(item_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
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
        lv_obj_set_style_text_font(info_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_align(info_label, LV_ALIGN_CENTER, 0, 0);
    }
    /* else: 无已连接WiFi，不打印日志（避免扫描期间每秒刷屏） */
    
    // 添加"可用网络"标题和刷新按钮
    lv_obj_t *title_cont = lv_obj_create(list_cont);
    lv_obj_set_size(title_cont, LV_PCT(100), 35);
    lv_obj_set_style_bg_color(title_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(title_cont, 0, 0);
    lv_obj_set_style_pad_all(title_cont, 0, 0);
    lv_obj_clear_flag(title_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(title_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    lv_obj_t *available_label = lv_label_create(title_cont);
    lv_label_set_text(available_label, "可用网络");
    lv_obj_set_style_text_color(available_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(available_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
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
        lv_obj_set_style_text_font(scanning_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_set_style_pad_left(scanning_label, 15, 0);
        return;
    }
    
    // 如果没有扫描结果，显示提示
    if (g_wifi_scan_data.count == 0) {
        lv_obj_t *no_result_label = lv_label_create(list_cont);
        lv_label_set_text(no_result_label, "未找到WiFi网络");
        lv_obj_set_style_text_color(no_result_label, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(no_result_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
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
        
        lv_obj_t *signal_img = lv_img_create(list_item);
        lv_img_set_src(signal_img, get_wifi_signal_icon(
            g_wifi_scan_data.results[scan_idx].rssi,
            g_wifi_scan_data.results[scan_idx].is_encrypted));
        
        lv_obj_t *item_label = lv_label_create(list_item);
        lv_label_set_text(item_label, saved_ssid);
        lv_obj_set_style_text_color(item_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(item_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_set_flex_grow(item_label, 1);
        
        lv_obj_t *saved_label = lv_label_create(list_item);
        if ((g_is_connecting || g_is_reconnecting) && strcmp(g_connecting_ssid, saved_ssid) == 0) {
            lv_label_set_text(saved_label, "连接中");
            lv_obj_set_style_text_color(saved_label, lv_color_hex(0xFFA500), 0);  /* 橙色 */
        } else {
            lv_label_set_text(saved_label, "已保存");
            lv_obj_set_style_text_color(saved_label, lv_color_hex(0x1E90FF), 0);  /* 蓝色 */
        }
        lv_obj_set_style_text_font(saved_label, vw_resource_get_font(WATCH_REGULAR_FONT "_16"), 0);
        
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
        
        lv_obj_t *signal_img = lv_img_create(list_item);
        lv_img_set_src(signal_img, get_wifi_signal_icon(
            g_wifi_scan_data.results[i].rssi,
            g_wifi_scan_data.results[i].is_encrypted));
        
        lv_obj_t *item_label = lv_label_create(list_item);
        lv_label_set_text(item_label, g_wifi_scan_data.results[i].ssid);
        lv_obj_set_style_text_color(item_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(item_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
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
    
    WATCH_DBG_LOG("[WiFi] Disconnecting from %s", ifname);
    
    /* 断开ESSID即可解除关联，不调用ifdown以避免ESP32驱动被重置 */
    snprintf(command, sizeof(command), "wapi essid %s \"\" 0", ifname);
    system(command);
    usleep(300000);  /* 300ms 等待驱动处理断开 */
    
    WATCH_DBG_LOG("[WiFi] Disconnected");
    return 0;
}

/* WiFi断开连接后台线程（避免 system() 阻塞 UI 线程） */
static void *wifi_disconnect_thread(void *arg)
{
    (void)arg;
    WATCH_DBG_LOG("[WiFi] Disconnect thread started");
    wifi_disconnect("wlan0");
    WATCH_DBG_LOG("[WiFi] Disconnect thread finished");
    return NULL;
}

/* 启动后台线程断开WiFi */
static void wifi_start_disconnect_thread(void)
{
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    if (pthread_create(&tid, &attr, wifi_disconnect_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        WATCH_DBG_LOG("[WiFi] Failed to create disconnect thread, fallback to sync");
        wifi_disconnect("wlan0");
    }
    pthread_attr_destroy(&attr);
}

/* WiFi详情页面 - 返回按钮回调 */
static void wifi_detail_back_cb(lv_event_t *e)
{
    lv_obj_t *detail_page = lv_event_get_user_data(e);
    if (detail_page) {
        vw_watch_pop_page(detail_page);
        lv_obj_del_async(detail_page);
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
                        vw_watch_pop_page(detail_page);
                        lv_obj_del_async(detail_page);
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

    /* 清除已连接状态（但保留保存的信息用于重连） */
    g_connected_ssid[0] = '\0';

    /* 阻止状态检查定时器自动重连用户刚断开的WiFi */
    g_reconnect_retry = MAX_RECONNECT_RETRY;

    /* 重置智能家居服务器信息 */
    home_control_reset_server();

    /* 在后台线程中断开WiFi，避免 system() 阻塞UI线程 */
    wifi_start_disconnect_thread();

    /* 关闭详情页面：先从页面栈移除，再异步删除对象
     * 使用 lv_obj_del_async 避免在事件回调中同步删除
     * 当前事件目标（按钮）导致 use-after-free */
    lv_obj_t *detail_page = lv_event_get_user_data(e);
    if (detail_page) {
        vw_watch_pop_page(detail_page);
        lv_obj_del_async(detail_page);
    }

    /* 刷新WiFi列表UI */
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

    /* 先删除保存的WiFi信息（依赖 g_connected_ssid 尚未清除） */
    if (strlen(g_connected_ssid) > 0) {
        wifi_forget(g_connected_ssid);
    }

    /* 清除已连接状态 */
    g_connected_ssid[0] = '\0';

    /* 阻止状态检查定时器自动重连 */
    g_reconnect_retry = MAX_RECONNECT_RETRY;

    /* 重置智能家居服务器信息 */
    home_control_reset_server();

    /* 在后台线程中断开WiFi，避免 system() 阻塞UI线程 */
    wifi_start_disconnect_thread();

    /* 关闭详情页面：先从页面栈移除，再异步删除对象
     * 使用 lv_obj_del_async 避免在事件回调中同步删除
     * 当前事件目标（按钮）导致 use-after-free */
    lv_obj_t *detail_page = lv_event_get_user_data(e);
    if (detail_page) {
        vw_watch_pop_page(detail_page);
        lv_obj_del_async(detail_page);
    }

    /* 刷新WiFi列表UI */
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
    
    // 添加侧滑返回手势处理
    lv_obj_add_event_cb(detail_page, wifi_detail_slide_handler, LV_EVENT_ALL, NULL);

    // 将详情页压入页面栈，支持PWR短按返回主页
    vw_watch_push_page(detail_page);
    
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
    lv_obj_set_style_text_font(back_btn, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_pad_left(back_btn, 15, 0);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, wifi_detail_back_cb, LV_EVENT_CLICKED, detail_page);
    
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, g_connected_ssid);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
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
        lv_obj_set_style_text_font(label_obj, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        
        lv_obj_t *value_obj = lv_label_create(item);
        lv_label_set_text(value_obj, info_values[i]);
        lv_obj_set_style_text_color(value_obj, lv_color_white(), 0);
        lv_obj_set_style_text_font(value_obj, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
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
    lv_obj_set_style_text_font(disconnect_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
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
    lv_obj_set_style_text_font(forget_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_align(forget_label, LV_ALIGN_CENTER, 0, 0);
}

/* WiFi连接状态检查定时器回调 */
static void wifi_status_check_timer_cb(lv_timer_t *timer)
{
    // WiFi未启用时不检查
    if (!g_wifi_enabled) {
        return;
    }
    
    // WiFi已启用但未连接：触发扫描重连（开机初始连接失败后的重试机制）
    if (strlen(g_connected_ssid) == 0) {
        if (g_saved_wifi_count == 0) return;  // 没有已保存WiFi，不需要重连
        if (g_is_reconnecting) return;  // 正在重连中
        if (g_is_connecting) return;   // 用户正在主动连接中
        if (g_wifi_scan_data.scanning) return;  // 正在扫描中，等扫描完成后由扫描定时器触发重连
        if (g_reconnect_retry >= MAX_RECONNECT_RETRY) {
            WATCH_DBG_LOG("[WiFi] Max reconnect retries (%d) reached", MAX_RECONNECT_RETRY);
            return;
        }
        g_reconnect_retry++;
        WATCH_DBG_LOG("[WiFi] Not connected, triggering scan-reconnect (%d/%d)",
               g_reconnect_retry, MAX_RECONNECT_RETRY);
        wifi_auto_reconnect_check();
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
    
    // printf("[WiFi] Interface status: ret=%d, flags=0x%02x, running=%d\n", 
    //        ret, if_flags, IFF_IS_RUNNING(if_flags));
    
    if (ret == 0 && IFF_IS_RUNNING(if_flags)) {
        // 接口正在运行
        g_status_fail_count = 0;
        g_reconnect_retry = 0;  /* 接口正常，重置重试计数 */
    } else {
        g_status_fail_count++;
        WATCH_DBG_LOG("[WiFi] Interface not running (%d)", g_status_fail_count);
        
        // 连续失败3次才认为断开
        if (g_status_fail_count >= 3) {
            WATCH_DBG_LOG("[WiFi] Connection lost after %d failures: %s", g_status_fail_count, g_connected_ssid);
            
            // 清除连接状态（保留保存的WiFi信息）
            g_connected_ssid[0] = '\0';
            g_connect_time = 0;
            g_status_fail_count = 0;
            
            // 重置智能家居服务器信息
            home_control_reset_server();
            
            // 刷新UI
            if (g_list_cont) {
                wifi_update_list_ui(g_list_cont);
            }
        }
    }
}

static void wifi_scan_timer_cb(lv_timer_t *timer)
{
    /* WiFi未启用时停止处理 */
    if (!g_wifi_enabled) return;
    /* 注意：g_list_cont 可能为NULL（开机时WiFi页面未打开），仍需处理扫描和重连 */

    static bool was_scanning = false;
    static int  last_count   = -1;
    static char last_ssid[64] = {0};
    bool is_scanning = g_wifi_scan_data.scanning;

    /* 只在状态变化时打印日志，避免扫描期间每秒刷屏 */
    if (is_scanning != was_scanning) {
        WATCH_DBG_LOG("[WiFi] State changed: scanning=%d, reconnecting=%d",
               is_scanning, g_is_reconnecting);
        was_scanning = is_scanning;
    }

    /* 仅在数据变化时重建 UI，避免每秒清空+重建整列表 */
    if (g_wifi_scan_data.count != last_count ||
        strcmp(g_connected_ssid, last_ssid) != 0) {
        last_count = g_wifi_scan_data.count;
        strncpy(last_ssid, g_connected_ssid, sizeof(last_ssid) - 1);
        last_ssid[sizeof(last_ssid) - 1] = '\0';
        if (g_list_cont) {
            wifi_update_list_ui(g_list_cont);
        }

        /* 扫描完成后，若未连接且扫描结果中有已保存 WiFi，触发自动重连 */
        if (!is_scanning && strlen(g_connected_ssid) == 0 &&
            !g_is_reconnecting && !g_is_connecting && g_saved_wifi_count > 0) {
            for (int i = 0; i < g_wifi_scan_data.count; i++) {
                for (int s = 0; s < g_saved_wifi_count; s++) {
                    if (strcmp(g_wifi_scan_data.results[i].ssid,
                               g_saved_wifi_list[s].ssid) == 0) {
                        WATCH_DBG_LOG("[WiFi] Scan found saved WiFi, triggering auto-reconnect: %s",
                               g_saved_wifi_list[s].ssid);
                        wifi_auto_reconnect_check();
                        goto scan_done;
                    }
                }
            }
        }
    }
scan_done:

    /* 已连接：停止周期扫描，自删除定时器 */
    if (strlen(g_connected_ssid) > 0 && !is_scanning) {
        if (g_update_timer) {
            lv_timer_del(g_update_timer);
            g_update_timer = NULL;
        }
        was_scanning = false;
        last_count = -1;
        last_ssid[0] = '\0';
        return;
    }

    /* 正在扫描/连接/重连：等待完成，不启动新扫描 */
    if (is_scanning || g_is_connecting || g_is_reconnecting) {
        return;
    }

    /* 检查是否到了下次扫描时间 */
    time_t now = time(NULL);
    if (g_next_scan_time == 0 || now >= g_next_scan_time) {
        WATCH_DBG_LOG("[WiFi] Periodic scan starting (cycle=%d)", g_scan_cycle_count + 1);
        wifi_start_scan();

        /* 计算下次扫描间隔：前3次10s，之后20s→40s→60s递增 */
        g_scan_cycle_count++;
        int interval;
        if (g_scan_cycle_count <= 3) {
            interval = 10;
        } else if (g_scan_cycle_count == 4) {
            interval = 20;
        } else if (g_scan_cycle_count == 5) {
            interval = 40;
        } else {
            interval = 60;
        }
        g_next_scan_time = now + interval;
        WATCH_DBG_LOG("[WiFi] Next scan in %ds", interval);
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
                g_wifi_enabled = true;
                wifi_save_enabled(true);  // 持久化开关状态

                // 与 auto_init 保持一致：用 system("ifup") 而非 wapi_set_ifup，
                // 确保网络协议栈完整初始化
                system("ifup wlan0 > /dev/null 2>&1");
                sleep(1);  /* 等待 ESP32 驱动稳定 */

                // 保存列表容器指针
                g_list_cont = list_cont;

                // 不立即重连，先扫描找到可用 AP 后再触发重连
                // （ESP32 驱动需要先扫描到 AP 才能可靠关联）

                // 重置扫描周期计数和重连重试计数
                g_scan_cycle_count = 0;
                g_next_scan_time = 0;  /* 0 表示立即触发首次扫描 */
                g_reconnect_retry = 0;  /* 重置重连重试计数 */
                g_abort_reconnect = false;  /* 清除上次关闭WiFi时设置的中止标志 */
                g_wifi_scan_data.count = 0;  /* 清除旧的扫描结果，避免显示残留 */

                // 启动定时器（1s间隔检查，按周期触发扫描）
                if (g_update_timer) {
                    lv_timer_del(g_update_timer);
                }
                g_update_timer = lv_timer_create(wifi_scan_timer_cb, 1000, NULL);

                // 启动定时器检查连接状态
                if (g_status_timer) {
                    lv_timer_del(g_status_timer);
                }
                g_status_timer = lv_timer_create(wifi_status_check_timer_cb, 3000, NULL);
                
                lv_obj_clear_flag(list_cont, LV_OBJ_FLAG_HIDDEN);
            } else {
                // 禁用 WiFi
                WATCH_DBG_LOG("[WiFi] Disabling WiFi...");
                g_wifi_enabled = false;
                wifi_save_enabled(false);  // 持久化开关状态

                // 中止正在运行的重连/扫描线程，清理残留状态
                g_abort_reconnect = true;
                g_is_reconnecting = false;
                g_wifi_scan_data.scanning = false;
                g_reconnect_retry = 0;

                system("ifdown wlan0 > /dev/null 2>&1");
                
                // 清除已连接的 WiFi 状态（但保留保存的WiFi信息用于重连）
                g_connected_ssid[0] = '\0';
                g_wifi_scan_data.count = 0;  /* 清除扫描结果 */
                WATCH_DBG_LOG("[WiFi] Cleared connected WiFi state (saved: %d)", 
                       g_saved_wifi_count);
                
                // 重置智能家居服务器信息
                home_control_reset_server();
                
                // 刷新UI，清除旧的"已连接"显示
                if (g_list_cont) {
                    wifi_update_list_ui(g_list_cont);
                }
                
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
                        /* 退出前清理定时器和指针，防止 use-after-free 崩溃 */
                        if (g_update_timer) {
                            lv_timer_del(g_update_timer);
                            g_update_timer = NULL;
                        }
                        if (g_status_timer) {
                            lv_timer_del(g_status_timer);
                            g_status_timer = NULL;
                        }
                        if (g_connect_check_timer) {
                            lv_timer_del(g_connect_check_timer);
                            g_connect_check_timer = NULL;
                        }
                        g_list_cont = NULL;
                        g_is_reconnecting = false;
                        g_is_connecting = false;
                        g_has_pending_connect = false;
                        g_abort_reconnect = false;
                        vw_watch_pop_page(cont);
                        lv_obj_del_async(cont);
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
    lv_obj_set_style_pad_top(cont, 50, 0);  /* 顶部内边距，让开关下移避开弧形切割 */
    
    // WiFi 开关
    lv_obj_t *switch_cont = lv_obj_create(cont);
    lv_obj_set_size(switch_cont, LV_PCT(100), 80);
    // 修改：背景色改为深灰色，与 settings 按钮风格接近
    lv_obj_set_style_bg_color(switch_cont, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(switch_cont, 0, 0);
    lv_obj_set_flex_flow(switch_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(switch_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(switch_cont, 25, 0);
    lv_obj_set_style_pad_right(switch_cont, 25, 0);

    lv_obj_t *switch_label = lv_label_create(switch_cont);
    // 修改：标签文本改为 WLAN
    lv_label_set_text(switch_label, "WLAN");
    lv_obj_set_style_text_color(switch_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(switch_label, vw_resource_get_font(WATCH_REGULAR_FONT "_30"), 0);

    lv_obj_t *wifi_switch = lv_switch_create(switch_cont);
    lv_obj_set_size(wifi_switch, 60, 30);
    
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
    
    // 如果 WiFi 已启用，启动扫描并更新列表
    if (g_wifi_enabled) {
        WATCH_DBG_LOG("[WiFi] WiFi already enabled, starting scan...");
        wifi_start_scan();
        /* 不立即重连，等扫描完成后由扫描定时器匹配已保存WiFi再触发重连 */
        g_list_cont = list_cont;
        if (g_update_timer) {
            lv_timer_del(g_update_timer);
        }
        g_update_timer = lv_timer_create(wifi_scan_timer_cb, 1000, NULL);
        
        // 启动连接状态检查定时器
        if (g_status_timer) {
            lv_timer_del(g_status_timer);
        }
        g_status_timer = lv_timer_create(wifi_status_check_timer_cb, 3000, NULL);
    }
    
    // 添加滑动手势处理，用于返回
    lv_obj_add_event_cb(cont, wifi_slide_gesture_handler, LV_EVENT_ALL, NULL);

    // 将WiFi页面压入页面栈，支持PWR短按返回主页
    vw_watch_push_page(cont);
}

/* WiFi开机自动初始化：加载配置、恢复开关状态、自动连接已保存的WiFi */
void settings_wifi_auto_init(void)
{
    WATCH_DBG_LOG("[WiFi] Auto init starting...");

    /* 1. 加载已保存的WiFi列表 */
    wifi_load_from_file();

    /* 2. 加载WiFi开关状态 */
    g_wifi_enabled = wifi_load_enabled();

    WATCH_DBG_LOG("[WiFi] Auto init: enabled=%d, saved_count=%d",
           g_wifi_enabled, g_saved_wifi_count);

    /* 3. 如果没有保存的WiFi配置，保持关闭状态 */
    if (g_wifi_enabled && g_saved_wifi_count == 0) {
        WATCH_DBG_LOG("[WiFi] No saved WiFi config, keeping WiFi disabled");
        g_wifi_enabled = false;
        wifi_save_enabled(false);
        return;
    }

    /* 4. 如果WiFi未启用，不做任何操作 */
    if (!g_wifi_enabled) {
        WATCH_DBG_LOG("[WiFi] WiFi was not enabled, skipping auto-connect");
        return;
    }

    /* 5. 启用WiFi接口 */
    syslog(LOG_INFO, "[WiFi] Enabling WiFi interface for auto-connect...");
    system("ifup wlan0 > /dev/null 2>&1");
    sleep(1);  /* 等待驱动稳定 */

    /* 6. 启动扫描，先发现可用AP再触发重连 */
    g_scan_cycle_count = 0;
    g_next_scan_time = 0;
    wifi_start_scan();
    if (g_update_timer) {
        lv_timer_del(g_update_timer);
    }
    g_update_timer = lv_timer_create(wifi_scan_timer_cb, 1000, NULL);

    /* 7. 启动连接状态检查定时器（检测断线后自动重连） */
    if (g_status_timer) {
        lv_timer_del(g_status_timer);
    }
    g_status_timer = lv_timer_create(wifi_status_check_timer_cb, 3000, NULL);

    WATCH_DBG_LOG("[WiFi] Auto init completed, scan started");
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

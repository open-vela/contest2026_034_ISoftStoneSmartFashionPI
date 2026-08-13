/**
 * @file home_control.c
 * 智能家居控制页面
 */

#include <nuttx/config.h>

#include "home_control.h"
#include "../common/watch_pages.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include "../settings/settings_wifi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define HC_LOG(fmt, ...) printf("[HomeControl] " fmt "\n", ##__VA_ARGS__)
#else
#  define HC_LOG(fmt, ...)
#endif
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netutils/netlib.h>



/* UI对象 */
static lv_obj_t* home_control_base = NULL;
static lv_obj_t* home_control_list = NULL;
static lv_style_t btn_style;

/* 服务器信息 */
static char g_server_ip[64] = {0};
static int g_server_port = 0;
static bool g_server_discovered = false;

void home_control_reset_server(void)
{
    memset(g_server_ip, 0, sizeof(g_server_ip));
    g_server_port = 0;
    g_server_discovered = false;
    HC_LOG("Server info reset");
}

/* 设备信息结构 */
typedef struct {
    const char* name;
    const void* icon;
    bool status;
    bool has_sub_switch;
    bool sub_status;
} home_device_t;

/* 设备列表 - 运行时初始化 */
#define DEVICE_COUNT 12
static home_device_t devices[DEVICE_COUNT];
static bool devices_initialized = false;

/* 初始化设备列表 */
static void init_devices(void)
{
    if (devices_initialized) return;
    
    devices[0] = (home_device_t){"电视", vw_resource_get_img("icon_home_TV"), false, false, false};
    devices[1] = (home_device_t){"空调", vw_resource_get_img("icon_home_air"), false, false, false};
    devices[2] = (home_device_t){"地暖", vw_resource_get_img("icon_home_floor_heating"), false, false, false};
    devices[3] = (home_device_t){"新风", vw_resource_get_img("icon_home_new_trend"), false, false, false};
    devices[4] = (home_device_t){"客厅氛围灯", vw_resource_get_img("icon_home_room_ambient_light"), false, false, false};
    devices[5] = (home_device_t){"客厅灯", vw_resource_get_img("icon_home_room_light"), false, false, false};
    devices[6] = (home_device_t){"电视背景灯", vw_resource_get_img("icon_home_TV_bg_light"), false, false, false};
    devices[7] = (home_device_t){"玄关灯", vw_resource_get_img("icon_home_entrance_light"), false, false, false};
    devices[8] = (home_device_t){"卧室灯", vw_resource_get_img("icon_home_bedroom_light"), false, false, false};
    devices[9] = (home_device_t){"卧室背景灯", vw_resource_get_img("icon_home_bedroom_backlight"), false, false, false};
    devices[10] = (home_device_t){"卫生间灯", vw_resource_get_img("icon_home_bathroom_light"), false, false, false};
    devices[11] = (home_device_t){"窗帘", vw_resource_get_img("icon_home_curtain"), false, true, false};
    
    devices_initialized = true;
}

static void slide_gesture_handler(lv_event_t *e);
static void switch_event_handler(lv_event_t *e);
static void msg_box_timer_cb(lv_timer_t *timer);

#define DISCOVERY_PORT 9999
#define DISCOVERY_MAGIC "DISCOVER_REQ"
#define RESPONSE_MAGIC "DISCOVER_RESP"
#define DISCOVERY_TIMEOUT 5

static void discover_server_info(void);
static int send_control_command(int cmd);

static void discover_server_info(void)
{
    HC_LOG("Discovering server via UDP broadcast...");
    
    memset(g_server_ip, 0, sizeof(g_server_ip));
    g_server_port = 0;
    g_server_discovered = false;
    
    int sockfd;
    struct sockaddr_in broadcast_addr;
    struct sockaddr_in server_addr;
    struct sockaddr_in local_addr;
    socklen_t server_len;
    char buffer[256];
    fd_set readfds;
    struct timeval timeout;
    int ret;
    struct in_addr local_ip;
    
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        HC_LOG("Failed to create socket");
        g_server_discovered = false;
        return;
    }
    
    if (netlib_get_ipv4addr("wlan0", &local_ip) == 0 && local_ip.s_addr != 0) {
        HC_LOG("Local wlan0 IP: %s", inet_ntoa(local_ip));
        
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr = local_ip;
        local_addr.sin_port = 0;
        
        if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
            HC_LOG("Failed to bind to wlan0 interface");
        }
    } else {
        HC_LOG("Failed to get wlan0 IP address");
    }
    
    int broadcast = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
    broadcast_addr.sin_port = htons(DISCOVERY_PORT);
    
    ret = sendto(sockfd, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC), 0,
                 (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
    if (ret < 0) {
        HC_LOG("Broadcast send failed");
        close(sockfd);
        g_server_discovered = false;
        return;
    }
    
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    timeout.tv_sec = DISCOVERY_TIMEOUT;
    timeout.tv_usec = 0;
    
    ret = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
    if (ret <= 0) {
            HC_LOG("Discovery timeout or error");
        close(sockfd);
        g_server_discovered = false;
        return;
    }
    
    server_len = sizeof(server_addr);
    ret = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                   (struct sockaddr *)&server_addr, &server_len);
    if (ret < 0) {
        HC_LOG("Receive response failed");
        close(sockfd);
        g_server_discovered = false;
        return;
    }
    
    buffer[ret] = '\0';
    
    if (strncmp(buffer, RESPONSE_MAGIC, strlen(RESPONSE_MAGIC)) == 0) {
        char *ip_start = buffer + strlen(RESPONSE_MAGIC) + 1;
        char *port_start = strchr(ip_start, ' ');
        
        if (port_start != NULL) {
            *port_start = '\0';
            port_start++;
            
            strncpy(g_server_ip, ip_start, sizeof(g_server_ip) - 1);
            g_server_port = atoi(port_start);
            g_server_discovered = true;
            
            HC_LOG("Server discovered: %s:%d", g_server_ip, g_server_port);
            close(sockfd);
            return;
        }
    }
    
    HC_LOG("Invalid response format");
    close(sockfd);
    g_server_discovered = false;
}

static int send_control_command(int cmd)
{
    if (!g_server_discovered) {
            HC_LOG("Server not discovered, cannot send command");
        return -1;
    }
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        HC_LOG("Failed to create socket");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(g_server_port);
    inet_pton(AF_INET, g_server_ip, &server_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        HC_LOG("Failed to connect to %s:%d", g_server_ip, g_server_port);
        close(sock);
        return -1;
    }
    
    char cmd_str[16] = {0};
    snprintf(cmd_str, sizeof(cmd_str), "%d", cmd);
    
    int ret = send(sock, cmd_str, strlen(cmd_str), 0);
    
    if (ret < 0) {
        HC_LOG("Failed to send command %d", cmd);
    } else {
        HC_LOG("Sent command %d to %s:%d", cmd, g_server_ip, g_server_port);
    }
    
    close(sock);
    return ret;
}

/* ========== 语音控制（供小通AI调用） ========== */

/* 检查buf中是否包含变体列表中的任意一个字符串 */
static int strstr_variants(const char *buf, const char * const variants[])
{
    for (int i = 0; variants[i] != NULL; i++) {
        if (strstr(buf, variants[i])) return 1;
    }
    return 0;
}

/* 开关动作词变体 — 精简为最常用发音（加 const 移入 flash） */
static const char * const ACT_ON[]  = {"打开", "开", "开启", "启动", "打开", "达开", NULL};
static const char * const ACT_OFF[] = {"关闭", "关", "关上", "关掉", "停止", "关比", "光闭", NULL};

/* 设备关键词→设备索引映射 */
typedef struct {
    const char * const *variants; /* 发音变体列表（NULL结尾，const入flash） */
    int dev_index;
    bool has_sub;
} device_voice_map_t;

/* 各设备发音变体（精简为3~4个最常用变体以减少flash占用） */
static const char * const V_TV[]        = {"电视", "殿试", "电是", NULL};
static const char * const V_AC[]        = {"空调", "空条", "孔调", NULL};
static const char * const V_FLOOR[]     = {"地暖", "地卵", "地软", NULL};
static const char * const V_FRESH[]     = {"新风", "欣风", "新丰", NULL};
static const char * const V_AMBIENT[]   = {"氛围灯", "客厅氛围", "氛围", "芬围灯", NULL};
static const char * const V_LIVING[]    = {"客厅灯", "客厅的灯", "课厅灯", "可厅灯", NULL};
static const char * const V_TV_BG[]     = {"背景灯", "电视背景灯", "背静灯", "北京灯", NULL};
static const char * const V_ENTRANCE[]  = {"玄关灯", "玄关的灯", "旋关灯", NULL};
static const char * const V_BEDROOM[]   = {"卧室灯", "卧室的灯", "我是灯", "卧式灯", NULL};
static const char * const V_BEDROOM_BG[]= {"卧室背景灯", "我是背景灯", "卧室背静灯", NULL};
static const char * const V_BATHROOM[]  = {"卫生间灯", "卫生间", "厕所灯", "洗手间灯", "为生间", NULL};
static const char * const V_CURTAIN[]   = {"窗帘", "窗连", "创联", "窗脸", NULL};

/* 设备映射表 — 加 const 移入 flash */
static const device_voice_map_t g_device_map[] = {
    {V_TV,           0,  false},
    {V_AC,           1,  false},
    {V_FLOOR,        2,  false},
    {V_FRESH,        3,  false},
    {V_AMBIENT,      4,  false},
    {V_LIVING,       5,  false},
    {V_TV_BG,        6,  false},
    {V_ENTRANCE,     7,  false},
    {V_BEDROOM,      8,  false},
    {V_BEDROOM_BG,   9,  false},
    {V_BATHROOM,    10,  false},
    {V_CURTAIN,     11,  true },
};

int home_control_voice_execute(const char *text)
{
    if (!text || !text[0]) return HOME_CTRL_NONE;

    /* 1. 解析动作 */
    int action = -1; /* -1=未知, 0=关, 1=开 */
    if (strstr_variants(text, ACT_ON)) {
        action = 1;
    } else if (strstr_variants(text, ACT_OFF)) {
        action = 0;
    }
    if (action < 0) return HOME_CTRL_NONE;

    /* 2. 匹配设备 */
    int dev_index = -1;
    bool has_sub = false;
    for (int i = 0; i < (int)(sizeof(g_device_map) / sizeof(g_device_map[0])); i++) {
        if (strstr_variants(text, g_device_map[i].variants)) {
            dev_index = g_device_map[i].dev_index;
            has_sub = g_device_map[i].has_sub;
            break;
        }
    }
    if (dev_index < 0) {
        /* 有开关动作词但未匹配到设备：写标记文件供下一轮AI约束 */
        FILE *fp = fopen("/tmp/home_ctrl_nomatch", "w");
        if (fp) { fprintf(fp, "%s\n", text); fclose(fp); }
        return HOME_CTRL_NO_MATCH;
    }

    /* 3. 确保设备列表已初始化 */
    init_devices();

    HC_LOG("Voice cmd: %s → device[%d] %s → %s",
           text, dev_index, devices[dev_index].name,
           action ? "ON" : "OFF");

    /* 4. 发现服务器 */
    if (!g_server_discovered) {
        discover_server_info();
    }
    if (!g_server_discovered) {
        HC_LOG("Server not discovered, cannot execute voice command");
        /* 写标记文件供下一轮AI约束 */
        FILE *fp = fopen("/tmp/home_ctrl_netfail", "w");
        if (fp) { fprintf(fp, "%s\n", devices[dev_index].name); fclose(fp); }
        return HOME_CTRL_NET_FAIL;
    }

    /* 5. 计算命令码 */
    int cmd;
    if (has_sub) {
        cmd = action ? 23 : 24; /* 窗帘 */
    } else {
        cmd = action ? (dev_index * 2 + 1) : (dev_index * 2 + 2);
    }

    /* 6. 发送指令 */
    int ret = send_control_command(cmd);
    if (ret < 0) {
        HC_LOG("Send command failed for device %s", devices[dev_index].name);
        /* 写标记文件供下一轮AI约束 */
        FILE *fp = fopen("/tmp/home_ctrl_netfail", "w");
        if (fp) { fprintf(fp, "%s\n", devices[dev_index].name); fclose(fp); }
        return HOME_CTRL_NET_FAIL;
    }

    return HOME_CTRL_OK;
}

/**
 * 智能家居应用点击回调
 */

static void setup_home_control_button(lv_obj_t *btn, home_device_t *device);


static void home_control_app_create(void)
{
    HC_LOG("home_control app clicked");
    
    /* 初始化设备列表 */
    init_devices();
    
    // 创建智能家居控制页面
    home_control_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(home_control_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(home_control_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(home_control_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(home_control_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(home_control_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(home_control_base);
    lv_label_set_text(title_label, "智能家居");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    // 创建设备列表
    home_control_list = lv_list_create(home_control_base);
    lv_obj_set_size(home_control_list, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT - 60);
    lv_obj_set_style_bg_color(home_control_list, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(home_control_list, 0, LV_STATE_DEFAULT);
    lv_obj_align(home_control_list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_scrollbar_mode(home_control_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(home_control_list, LV_DIR_VER);
    lv_obj_set_flex_flow(home_control_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(home_control_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_snap_x(home_control_list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(home_control_list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_style_pad_row(home_control_list, 16, 0);
    lv_obj_set_style_pad_bottom(home_control_list, 20, LV_STATE_DEFAULT);

    lv_obj_add_flag(home_control_list, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建设备控制项
    for (int i = 0; i < DEVICE_COUNT; i++) {
        lv_obj_t *btn = lv_list_add_btn(home_control_list, devices[i].icon, devices[i].name);
        setup_home_control_button(btn, &devices[i]);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // 添加滑动手势处理
    lv_obj_add_event_cb(home_control_base, slide_gesture_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(home_control_base);

    HC_LOG("home_control: create complete");
}

static void setup_home_control_button(lv_obj_t *btn, home_device_t *device)
{
    /* 获取按钮中的图标和文本 */
    lv_obj_t *btn_img = lv_obj_get_child(btn, 0);  // 获取图标
    lv_obj_t *btn_label = lv_obj_get_child(btn, 1); // 获取文本标签
    
    /* 设置按钮样式 */
    static bool style_initialized = false;
    if (!style_initialized) {
        lv_style_init(&btn_style);
        lv_style_set_bg_opa(&btn_style, LV_OPA_100);
        lv_style_set_bg_color(&btn_style, lv_color_make(15, 15, 15));
        lv_style_set_text_color(&btn_style, lv_color_white());
        lv_style_set_text_font(&btn_style, vw_resource_get_font(WATCH_REGULAR_FONT "_32"));
        lv_style_set_border_width(&btn_style, 0);
        lv_style_set_radius(&btn_style, 32);
        lv_style_set_pad_all(&btn_style, 0);
        lv_style_set_outline_width(&btn_style, 0);
        /* 禁用变换效果，防止按钮按下时偏移 */
        lv_style_set_transform_width(&btn_style, 0);
        lv_style_set_transform_height(&btn_style, 0);
        lv_style_set_translate_x(&btn_style, 0);
        lv_style_set_translate_y(&btn_style, 0);
        style_initialized = true;
    }
    
    lv_obj_add_style(btn, &btn_style, 0);

    // 关键：为按下状态单独创建一个样式，也禁用变换
    static lv_style_t pressed_style;
    static bool pressed_style_initialized = false;
    if (!pressed_style_initialized) {
        lv_style_init(&pressed_style);
        lv_style_set_transform_width(&pressed_style, 0);
        lv_style_set_transform_height(&pressed_style, 0);
        lv_style_set_translate_x(&pressed_style, 0);
        lv_style_set_translate_y(&pressed_style, 0);
        // 可选：设置按下时的背景色稍微变化，但不变形
        lv_style_set_bg_color(&pressed_style, lv_color_make(25, 25, 25));
        pressed_style_initialized = true;
    }
    
    // 添加按下状态的样式
    lv_obj_add_style(btn, &pressed_style, LV_STATE_PRESSED);
    
    lv_obj_set_size(btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    
    /* 重置按钮的布局方式，确保我们的对齐设置生效 */
    // lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    // lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    lv_obj_set_layout(btn, LV_LAYOUT_NONE);

    /* 设置图标样式 - 64x64图标，距离按钮左侧18px，上下居中 */
    if (btn_img) {
        lv_obj_set_size(btn_img, 38, 38);
        // lv_obj_set_style_pad_left(btn, 18, 0);
        // lv_obj_align(btn_img, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_pos(btn_img, 18, (WATCH_BTN_HEIGHT - 38) / 2);
        lv_obj_clear_flag(btn_img, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 设置文本样式 - 距离图标右侧24px，上下居中 */
    if (btn_label) {
        lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
        // lv_obj_set_style_pad_left(btn_label, 0, 0);
        // lv_obj_align(btn_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_pos(btn_label, 18 + 38 + 12, (WATCH_BTN_HEIGHT - 32) / 2);
        lv_obj_clear_flag(btn_label, LV_OBJ_FLAG_SCROLLABLE);
    }
    
    /* 创建开关 */
    lv_obj_t *switch_obj = lv_switch_create(btn);
    lv_obj_set_size(switch_obj, 57, 32);
    /* 设置开关背景颜色 - 关闭状态灰色，开启状态绿色 */
    lv_obj_set_style_bg_color(switch_obj, lv_color_hex(0x4D4D4D), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(switch_obj, lv_color_hex(0x2BEA77), LV_PART_MAIN | LV_STATE_CHECKED);
    /* 设置开关指示器颜色 - 这是关键部分 */
    lv_obj_set_style_bg_color(switch_obj, lv_color_hex(0x4D4D4D), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(switch_obj, lv_color_hex(0x2BEA77), LV_PART_INDICATOR | LV_STATE_CHECKED);
    /* 设置开关滑块颜色 */
    lv_obj_set_style_bg_color(switch_obj, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(switch_obj, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_add_event_cb(switch_obj, switch_event_handler, LV_EVENT_VALUE_CHANGED, device);
    if (device->status) {
        lv_obj_add_state(switch_obj, LV_STATE_CHECKED);
    }
    lv_obj_set_pos(switch_obj, WATCH_BTN_WIDTH - 57 - 18, (WATCH_BTN_HEIGHT - 32) / 2);
    // lv_obj_set_style_pad_right(switch_obj, -36, 0);
    lv_obj_clear_flag(switch_obj, LV_OBJ_FLAG_SCROLLABLE);
    // lv_obj_align(btn_label, LV_ALIGN_CENTER, 0, 0);
    // lv_obj_align_to(switch_obj, btn, LV_ALIGN_RIGHT_MID, -36, 0);
    
    lv_obj_add_flag(btn_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(btn_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(switch_obj, LV_OBJ_FLAG_EVENT_BUBBLE);
}

/**
 * 提示框自动关闭定时器回调
 */
static void msg_box_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *obj = (lv_obj_t *)timer->user_data;
    if (obj) {
        lv_obj_del(obj);
    }
    lv_timer_del(timer);
}

/**
 * 开关事件处理
 */
static void switch_event_handler(lv_event_t *e)
{
    home_device_t *device = (home_device_t *)lv_event_get_user_data(e);
    lv_obj_t *switch_obj = lv_event_get_target(e);
    
    if (device) {
        bool is_on = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
        
        // 检查WiFi是否已连接
        if (!settings_wifi_is_connected()) {
            HC_LOG("WiFi not connected, please connect WiFi first");
            
            // 恢复开关状态
            if (is_on) {
                lv_obj_clear_state(switch_obj, LV_STATE_CHECKED);
            } else {
                lv_obj_add_state(switch_obj, LV_STATE_CHECKED);
            }
            
            // 显示提示信息
            lv_obj_t *msg_box = lv_obj_create(lv_scr_act());
            lv_obj_set_size(msg_box, 280, 80);
            lv_obj_set_style_bg_color(msg_box, lv_color_hex(0x333333), 0);
            lv_obj_set_style_border_width(msg_box, 2, 0);
            lv_obj_set_style_border_color(msg_box, lv_color_hex(0xFF5722), 0);
            lv_obj_set_style_radius(msg_box, 10, 0);
            lv_obj_align(msg_box, LV_ALIGN_CENTER, 0, 0);
            
            lv_obj_t *msg_label = lv_label_create(msg_box);
            lv_label_set_text(msg_label, "请先连接WiFi");
            lv_obj_set_style_text_color(msg_label, lv_color_white(), 0);
            lv_obj_set_style_text_font(msg_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
            lv_obj_align(msg_label, LV_ALIGN_CENTER, 0, -10);
            
            lv_obj_t *hint_label = lv_label_create(msg_box);
            lv_label_set_text(hint_label, "请前往设置连接WiFi");
            lv_obj_set_style_text_color(hint_label, lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_font(hint_label, vw_resource_get_font(WATCH_REGULAR_FONT "_16"), 0);
            lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, 15);
            
            // 3秒后自动关闭提示框
            lv_timer_create(msg_box_timer_cb, 3000, msg_box);
            
            return;
        }
        
        device->status = is_on;
        HC_LOG("Device: %s, status: %s", device->name, is_on ? "ON" : "OFF");
        
        if (!g_server_discovered) {
            HC_LOG("Server not discovered, discovering...");
            discover_server_info();
        }
        
        int cmd = is_on ? 1 : 2;
        
        if (device->has_sub_switch) {
            cmd = is_on ? 23 : 24;  // 窗帘电源：上电23，关闭电源24
        } else {
            for (int i = 0; i < DEVICE_COUNT; i++) {
                if (strcmp(device->name, devices[i].name) == 0) {
                    cmd = is_on ? (i * 2 + 1) : (i * 2 + 2);
                    break;
                }
            }
        }
        
        HC_LOG("Sending command %d for device %s", cmd, device->name);
        send_control_command(cmd);
    }
}

/**
 * 滑动手势处理
 */
static void slide_gesture_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            HC_LOG("home_control: pressed");
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            HC_LOG("home_control: released, delta_x=%d, delta_y=%d", delta_x, delta_y);

            // 右滑退出（横向移动超过50px且大于纵向移动）
            if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                HC_LOG("home_control: right swipe, exit");
                if(home_control_base != NULL) {
                    // 将页面从页面栈弹出
                    vw_watch_pop_page(home_control_base);
                    lv_obj_del(home_control_base);
                    home_control_base = NULL;
                }
            }
            break;
        }
        default:
            break;
    }
}


void home_control_app_click_callback(lv_event_t *e)
{
    home_control_app_create();
}


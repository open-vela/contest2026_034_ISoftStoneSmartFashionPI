/**
 * @file xiaozhi_ai.c
 * 小智AI应用UI层实现
 *
 * 基于LVGL实现小智AI的用户界面，包括状态管理、手势处理和WebSocket通信协调。
 * 参照参考图片的浅蓝背景、居中笑脸图标和问候语布局设计。
 */

#include "xiaozhi_ai.h"
#include "xiaozhi_ai_ws.h"
#include "../common/watch_pages.h"
#include "../launcher/launcher.h"
#include "../settings/settings_wifi.h"
#include "../../resource/resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define XIAOZHI_LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#  define XIAOZHI_LOG(fmt, ...)
#endif

/* ========== UI状态枚举 ========== */
typedef enum {
    XIAOZHI_STATE_IDLE = 0,        /**< 待命，显示问候语 */
    XIAOZHI_STATE_CONNECTING,      /**< 正在连接服务器 */
    XIAOZHI_STATE_CONNECTED,       /**< 已连接，等待交互 */
    XIAOZHI_STATE_ERROR            /**< 连接错误 */
} xiaozhi_ui_state_t;

/* ========== 静态UI对象 ========== */
static lv_obj_t *xiaozhi_main_base = NULL;
static lv_obj_t *xiaozhi_title_label = NULL;
static lv_obj_t *xiaozhi_face_container = NULL;
static lv_obj_t *xiaozhi_face_eye_left = NULL;
static lv_obj_t *xiaozhi_face_eye_right = NULL;
static lv_obj_t *xiaozhi_face_mouth = NULL;
static lv_obj_t *xiaozhi_status_label = NULL;
static lv_obj_t *xiaozhi_status_sub_label = NULL;
static lv_obj_t *xiaozhi_audio_mode_label = NULL;
static lv_obj_t *xiaozhi_action_btn = NULL;
static lv_obj_t *xiaozhi_action_btn_label = NULL;
static lv_obj_t *xiaozhi_conversation_area = NULL;
static lv_obj_t *xiaozhi_back_btn = NULL;

/* ========== 状态追踪 ========== */
static xiaozhi_ui_state_t g_xiaozhi_state = XIAOZHI_STATE_IDLE;
/* g_error_message removed - unused, saves 128B BSS */
static lv_timer_t *g_connect_timer = NULL;
static lv_timer_t *g_poll_timer = NULL;

/* ========== 前向声明 ========== */
static void slide_gesture_handler(lv_event_t *e);
static void action_btn_click_handler(lv_event_t *e);
static void back_btn_click_handler(lv_event_t *e);
static void menu_btn_click_handler(lv_event_t *e);
static void xiaozhi_update_ui_state(xiaozhi_ui_state_t state, const char *error_msg);
static void connect_task_cb(lv_timer_t *timer);
static void poll_task_cb(lv_timer_t *timer);
static void xiaozhi_add_message(const char *text, bool is_user);
static void xiaozhi_page_deleted_cb(lv_event_t *e);

/* ========== 按钮样式 ========== */
static lv_style_t btn_style;
static lv_style_t btn_pressed_style;
static bool btn_styles_initialized = false;

static void init_button_styles(void)
{
    if (btn_styles_initialized) return;

    lv_style_init(&btn_style);
    lv_style_set_bg_opa(&btn_style, LV_OPA_100);
    lv_style_set_bg_color(&btn_style, lv_color_hex(0x2D47CB));
    lv_style_set_text_color(&btn_style, lv_color_white());
    lv_style_set_border_width(&btn_style, 0);
    lv_style_set_radius(&btn_style, 32);
    lv_style_set_pad_all(&btn_style, 0);
    lv_style_set_outline_width(&btn_style, 0);
    lv_style_set_transform_width(&btn_style, 0);
    lv_style_set_transform_height(&btn_style, 0);
    lv_style_set_translate_x(&btn_style, 0);
    lv_style_set_translate_y(&btn_style, 0);

    lv_style_init(&btn_pressed_style);
    lv_style_set_transform_width(&btn_pressed_style, 0);
    lv_style_set_transform_height(&btn_pressed_style, 0);
    lv_style_set_translate_x(&btn_pressed_style, 0);
    lv_style_set_translate_y(&btn_pressed_style, 0);
    lv_style_set_bg_opa(&btn_pressed_style, LV_OPA_80);
    lv_style_set_bg_color(&btn_pressed_style, lv_color_hex(0x2D47CB));

    btn_styles_initialized = true;
}

/* ========== 正面笑脸绘制 ========== */

/**
 * @brief 更新笑脸表情
 * @param mouth_type 0=微笑, 1=中性, 2=难过
 */
static void xiaozhi_update_face_emotion(int mouth_type)
{
    if (!xiaozhi_face_mouth) return;

    static const lv_point_precise_t smile_pts[] = {
        {30, 72}, {40, 78}, {50, 82}, {60, 84}, {70, 82}, {80, 78}, {90, 72}
    };
    static const lv_point_precise_t neutral_pts[] = {
        {35, 80}, {50, 80}, {60, 80}, {70, 80}, {85, 80}
    };
    static const lv_point_precise_t frown_pts[] = {
        {30, 84}, {40, 78}, {50, 74}, {60, 72}, {70, 74}, {80, 78}, {90, 84}
    };

    const lv_point_precise_t *pts;
    uint32_t pt_count;

    switch (mouth_type) {
    case 0: /* 微笑 */
        pts = smile_pts;
        pt_count = sizeof(smile_pts) / sizeof(smile_pts[0]);
        break;
    case 1: /* 中性(连接中等候) */
        pts = neutral_pts;
        pt_count = sizeof(neutral_pts) / sizeof(neutral_pts[0]);
        break;
    case 2: /* 难过(错误) */
        pts = frown_pts;
        pt_count = sizeof(frown_pts) / sizeof(frown_pts[0]);
        break;
    default:
        pts = smile_pts;
        pt_count = sizeof(smile_pts) / sizeof(smile_pts[0]);
        break;
    }

    lv_line_set_points(xiaozhi_face_mouth, pts, pt_count);
}

/* ========== 对话区域 ========== */

static void xiaozhi_add_message(const char *text, bool is_user)
{
    if (!xiaozhi_conversation_area || !text) return;

    /* 首次收到消息时显示对话区域 */
    if (lv_obj_has_flag(xiaozhi_conversation_area, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(xiaozhi_conversation_area, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏笑脸和问候语 */
        if (xiaozhi_face_container) lv_obj_add_flag(xiaozhi_face_container, LV_OBJ_FLAG_HIDDEN);
        if (xiaozhi_status_label) lv_obj_add_flag(xiaozhi_status_label, LV_OBJ_FLAG_HIDDEN);
        if (xiaozhi_status_sub_label) lv_obj_add_flag(xiaozhi_status_sub_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *msg_container = lv_obj_create(xiaozhi_conversation_area);
    lv_obj_set_width(msg_container, WATCH_SCREEN_WIDTH - 40);
    lv_obj_set_height(msg_container, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(msg_container, 0, 0);
    lv_obj_set_style_pad_all(msg_container, 12, 0);
    lv_obj_set_style_radius(msg_container, 16, 0);
    lv_obj_clear_flag(msg_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(msg_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(msg_container, LV_OPA_COVER, 0);

    if (is_user) {
        lv_obj_set_style_bg_color(msg_container, lv_color_hex(0x2D47CB), 0);
        lv_obj_set_style_flex_main_place(msg_container, LV_FLEX_ALIGN_END, 0);
    } else {
        lv_obj_set_style_bg_color(msg_container, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_flex_main_place(msg_container, LV_FLEX_ALIGN_START, 0);
    }

    lv_obj_t *msg_label = lv_label_create(msg_container);
    lv_label_set_text(msg_label, text);
    lv_obj_set_style_text_font(msg_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    if (is_user) {
        lv_obj_set_style_text_color(msg_label, lv_color_white(), 0);
    } else {
        lv_obj_set_style_text_color(msg_label, lv_color_hex(0x333333), 0);
    }
    lv_label_set_long_mode(msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_label, WATCH_SCREEN_WIDTH - 80);

    /* 滚动到底部 */
    lv_obj_scroll_to_view(msg_container, LV_ANIM_ON);
}

/* ========== 状态更新 ========== */

static void xiaozhi_update_ui_state(xiaozhi_ui_state_t state, const char *error_msg)
{
    g_xiaozhi_state = state;

    if (!xiaozhi_main_base) return;

    switch (state) {
    case XIAOZHI_STATE_IDLE:
        if (xiaozhi_title_label)
            lv_label_set_text(xiaozhi_title_label, "待命");
        if (xiaozhi_face_container) {
            lv_obj_clear_flag(xiaozhi_face_container, LV_OBJ_FLAG_HIDDEN);
            xiaozhi_update_face_emotion(0);
        }
        if (xiaozhi_status_label) {
            lv_label_set_text(xiaozhi_status_label, "Hi! 有什么可以帮到你呢？");
            lv_obj_clear_flag(xiaozhi_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (xiaozhi_status_sub_label) {
            if (!xiaozhi_ai_ws_is_audio_available()) {
                lv_label_set_text(xiaozhi_status_sub_label, "语音功能暂不可用");
            } else {
                lv_label_set_text(xiaozhi_status_sub_label, "");
            }
        }
        if (xiaozhi_action_btn_label)
            lv_label_set_text(xiaozhi_action_btn_label, "开始对话");
        /* 隐藏对话区 */
        if (xiaozhi_conversation_area) {
            lv_obj_add_flag(xiaozhi_conversation_area, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clean(xiaozhi_conversation_area);
        }
        break;

    case XIAOZHI_STATE_CONNECTING:
        if (xiaozhi_title_label)
            lv_label_set_text(xiaozhi_title_label, "连接中...");
        if (xiaozhi_face_container) {
            lv_obj_clear_flag(xiaozhi_face_container, LV_OBJ_FLAG_HIDDEN);
            xiaozhi_update_face_emotion(1);
        }
        if (xiaozhi_status_label) {
            lv_label_set_text(xiaozhi_status_label, "正在连接服务器...");
            lv_obj_clear_flag(xiaozhi_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (xiaozhi_status_sub_label) {
            lv_label_set_text(xiaozhi_status_sub_label, "");
        }
        if (xiaozhi_action_btn_label)
            lv_label_set_text(xiaozhi_action_btn_label, "取消");
        if (xiaozhi_conversation_area)
            lv_obj_add_flag(xiaozhi_conversation_area, LV_OBJ_FLAG_HIDDEN);
        break;

    case XIAOZHI_STATE_CONNECTED:
        if (xiaozhi_title_label)
            lv_label_set_text(xiaozhi_title_label, "小智AI");
        if (xiaozhi_face_container) {
            lv_obj_clear_flag(xiaozhi_face_container, LV_OBJ_FLAG_HIDDEN);
            xiaozhi_update_face_emotion(0);
        }
        if (xiaozhi_status_label) {
            lv_label_set_text(xiaozhi_status_label, "小智已就绪");
            lv_obj_clear_flag(xiaozhi_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (xiaozhi_status_sub_label) {
            if (!xiaozhi_ai_ws_is_audio_available()) {
                lv_label_set_text(xiaozhi_status_sub_label, "[文字模式] 输入文字与小智对话");
            } else {
                lv_label_set_text(xiaozhi_status_sub_label, "请说话...");
            }
        }
        if (xiaozhi_action_btn_label)
            lv_label_set_text(xiaozhi_action_btn_label, "断开连接");
        /* 启动消息轮询定时器 */
        if (!g_poll_timer) {
            g_poll_timer = lv_timer_create(poll_task_cb, 500, NULL);
        }
        break;

    case XIAOZHI_STATE_ERROR:
        if (xiaozhi_title_label)
            lv_label_set_text(xiaozhi_title_label, "连接失败");
        if (xiaozhi_face_container) {
            lv_obj_clear_flag(xiaozhi_face_container, LV_OBJ_FLAG_HIDDEN);
            xiaozhi_update_face_emotion(2);
        }
        if (xiaozhi_status_label) {
            if (error_msg && error_msg[0]) {
                lv_label_set_text(xiaozhi_status_label, error_msg);
            } else {
                lv_label_set_text(xiaozhi_status_label, "连接失败，请重试");
            }
            lv_obj_clear_flag(xiaozhi_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (xiaozhi_status_sub_label) {
            lv_label_set_text(xiaozhi_status_sub_label, "请检查网络连接后重试");
        }
        if (xiaozhi_action_btn_label)
            lv_label_set_text(xiaozhi_action_btn_label, "重试");
        if (xiaozhi_conversation_area)
            lv_obj_add_flag(xiaozhi_conversation_area, LV_OBJ_FLAG_HIDDEN);
        break;
    }
}

/* ========== 连接任务 ========== */

static void connect_task_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    g_connect_timer = NULL;

    /* 检查WiFi连接 */
    if (!settings_wifi_is_connected()) {
        xiaozhi_update_ui_state(XIAOZHI_STATE_ERROR, "请先连接WiFi");
        return;
    }

    /* 连接服务器 */
    int ret = xiaozhi_ai_ws_connect("api.tenclass.net", "443", "/xiaozhi/v1/");

    if (ret == 0) {
        xiaozhi_update_ui_state(XIAOZHI_STATE_CONNECTED, NULL);

        /* 接收初始服务器响应 */
        char buf[1024];
        int len = xiaozhi_ai_ws_receive(buf, sizeof(buf) - 1, 3000);
        if (len > 0) {
            buf[len] = '\0';
            XIAOZHI_LOG("[XIAOZHI] Server response: %s\n", buf);

            /* 尝试提取type字段 */
            char *type_start = strstr(buf, "\"type\"");
            if (type_start) {
                char *colon = strchr(type_start, ':');
                if (colon) {
                    char *val_start = colon + 1;
                    while (*val_start == ' ' || *val_start == '"') val_start++;
                    char *val_end = val_start;
                    while (*val_end && *val_end != '"' && *val_end != ',' && *val_end != '}') val_end++;
                    int val_len = val_end - val_start;
                    if (val_len > 0 && val_len < 64) {
                        char type_val[64];
                        strncpy(type_val, val_start, val_len);
                        type_val[val_len] = '\0';

                        if (strcmp(type_val, "hello") == 0) {
                            xiaozhi_add_message("已连接到小智AI，有什么可以帮你的？", false);
                        }
                    }
                }
            }
        }
    } else {
        /* 判断具体错误 */
        if (!settings_wifi_is_connected()) {
            xiaozhi_update_ui_state(XIAOZHI_STATE_ERROR, "WiFi已断开");
        } else {
            xiaozhi_update_ui_state(XIAOZHI_STATE_ERROR, "无法连接到小智服务器");
        }
    }
}

/* ========== 消息轮询任务 ========== */

static void poll_task_cb(lv_timer_t *timer)
{
    if (g_xiaozhi_state != XIAOZHI_STATE_CONNECTED) return;
    if (!xiaozhi_ai_ws_is_connected()) {
        xiaozhi_update_ui_state(XIAOZHI_STATE_ERROR, "连接已断开");
        return;
    }

    char buf[1024];
    int len = xiaozhi_ai_ws_receive(buf, sizeof(buf) - 1, 200);

    if (len > 0) {
        buf[len] = '\0';
        XIAOZHI_LOG("[XIAOZHI] Received: %s\n", buf);

        /* 简单JSON解析：提取text字段 */
        char *text_start = strstr(buf, "\"text\"");
        if (text_start) {
            char *colon = strchr(text_start, ':');
            if (colon) {
                char *val_start = colon + 1;
                while (*val_start == ' ' || *val_start == '"') val_start++;
                char *val_end = strchr(val_start, '"');
                if (val_end) {
                    int val_len = val_end - val_start;
                    if (val_len > 0 && val_len < 512) {
                        char text_val[512];
                        strncpy(text_val, val_start, val_len);
                        text_val[val_len] = '\0';
                        xiaozhi_add_message(text_val, false);
                    }
                }
            }
        } else {
            /* 如果没有text字段，显示完整消息 */
            if (len < 256) {
                xiaozhi_add_message(buf, false);
            }
        }
    } else if (len == 0) {
        xiaozhi_update_ui_state(XIAOZHI_STATE_ERROR, "服务器已断开连接");
    }
    /* len == -1: no data available, normal */
    /* len == -2: timeout, normal */
}

/* ========== 按钮事件处理 ========== */

static void action_btn_click_handler(lv_event_t *e)
{
    switch (g_xiaozhi_state) {
    case XIAOZHI_STATE_IDLE:
        /* 开始连接 */
        xiaozhi_update_ui_state(XIAOZHI_STATE_CONNECTING, NULL);
        /* 使用定时器异步连接，避免阻塞UI线程 */
        g_connect_timer = lv_timer_create(connect_task_cb, 100, NULL);
        lv_timer_set_repeat_count(g_connect_timer, 1);
        break;

    case XIAOZHI_STATE_CONNECTING:
        /* 取消连接 */
        if (g_connect_timer) {
            lv_timer_del(g_connect_timer);
            g_connect_timer = NULL;
        }
        xiaozhi_ai_ws_disconnect();
        xiaozhi_update_ui_state(XIAOZHI_STATE_IDLE, NULL);
        break;

    case XIAOZHI_STATE_CONNECTED:
        /* 断开连接 */
        if (g_poll_timer) {
            lv_timer_del(g_poll_timer);
            g_poll_timer = NULL;
        }
        xiaozhi_ai_ws_disconnect();
        xiaozhi_update_ui_state(XIAOZHI_STATE_IDLE, NULL);
        break;

    case XIAOZHI_STATE_ERROR:
        /* 重试 */
        xiaozhi_update_ui_state(XIAOZHI_STATE_CONNECTING, NULL);
        xiaozhi_ai_ws_disconnect();
        g_connect_timer = lv_timer_create(connect_task_cb, 100, NULL);
        lv_timer_set_repeat_count(g_connect_timer, 1);
        break;
    }
}

static void back_btn_click_handler(lv_event_t *e)
{
    XIAOZHI_LOG("xiaozhi_ai: back button pressed\n");

    /* 停止所有定时器 */
    if (g_connect_timer) {
        lv_timer_del(g_connect_timer);
        g_connect_timer = NULL;
    }
    if (g_poll_timer) {
        lv_timer_del(g_poll_timer);
        g_poll_timer = NULL;
    }

    /* 断开连接 */
    xiaozhi_ai_ws_disconnect();

    /* 退出页面 */
    if (xiaozhi_main_base) {
        vw_watch_pop_page(xiaozhi_main_base);
        lv_obj_del(xiaozhi_main_base);
        xiaozhi_main_base = NULL;
    }
}

static void menu_btn_click_handler(lv_event_t *e)
{
    XIAOZHI_LOG("xiaozhi_ai: menu button pressed\n");
    /* 预留菜单功能 */
}

/* ========== 手势处理 ========== */

static void slide_gesture_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_active(), &start_point);
        break;

    case LV_EVENT_RELEASED: {
        lv_point_t end_point;
        lv_indev_get_point(lv_indev_active(), &end_point);

        int32_t delta_x = end_point.x - start_point.x;
        int32_t delta_y = end_point.y - start_point.y;

        /* 右滑退出 */
        if (delta_x > 50 && abs(delta_x) > abs(delta_y)) {
            XIAOZHI_LOG("xiaozhi_ai: right swipe, exit\n");
            back_btn_click_handler(NULL);
        }
        break;
    }

    default:
        break;
    }
}

/* ========== 页面删除回调 ========== */

static void xiaozhi_page_deleted_cb(lv_event_t *e)
{
    XIAOZHI_LOG("xiaozhi_ai: page deleted\n");

    /* 清理定时器 */
    if (g_connect_timer) {
        lv_timer_del(g_connect_timer);
        g_connect_timer = NULL;
    }
    if (g_poll_timer) {
        lv_timer_del(g_poll_timer);
        g_poll_timer = NULL;
    }

    /* 断开连接 */
    xiaozhi_ai_ws_disconnect();

    /* 清理UI指针 */
    xiaozhi_main_base = NULL;
    xiaozhi_title_label = NULL;
    xiaozhi_face_container = NULL;
    xiaozhi_face_eye_left = NULL;
    xiaozhi_face_eye_right = NULL;
    xiaozhi_face_mouth = NULL;
    xiaozhi_status_label = NULL;
    xiaozhi_status_sub_label = NULL;
    xiaozhi_audio_mode_label = NULL;
    xiaozhi_action_btn = NULL;
    xiaozhi_action_btn_label = NULL;
    xiaozhi_conversation_area = NULL;
    xiaozhi_back_btn = NULL;
}

/* ========== 页面创建 ========== */

static void xiaozhi_ai_page_create(void)
{
    XIAOZHI_LOG("xiaozhi_ai: creating page\n");

    init_button_styles();

    /* 创建主页面 */
    xiaozhi_main_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(xiaozhi_main_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(xiaozhi_main_base, LV_OBJ_FLAG_SCROLLABLE);
    /* 浅蓝背景，参照参考图片 */
    lv_obj_set_style_bg_color(xiaozhi_main_base, lv_color_hex(0xE8F4FD), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(xiaozhi_main_base, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(xiaozhi_main_base, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(xiaozhi_main_base, 0, 0);
    lv_obj_align(xiaozhi_main_base, LV_ALIGN_CENTER, 0, 0);

    /* ===== 顶部状态栏 ===== */
    lv_obj_t *top_bar = lv_obj_create(xiaozhi_main_base);
    lv_obj_set_size(top_bar, WATCH_SCREEN_WIDTH, 55);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);

    /* 返回按钮 (左侧) */
    xiaozhi_back_btn = lv_label_create(top_bar);
    lv_label_set_text(xiaozhi_back_btn, "<");
    lv_obj_set_style_text_font(xiaozhi_back_btn,
        vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(xiaozhi_back_btn, lv_color_hex(0x333333), 0);
    lv_obj_align(xiaozhi_back_btn, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_add_flag(xiaozhi_back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(xiaozhi_back_btn, back_btn_click_handler, LV_EVENT_CLICKED, NULL);

    /* 标题 (居中) */
    xiaozhi_title_label = lv_label_create(top_bar);
    lv_label_set_text(xiaozhi_title_label, "待命");
    lv_obj_set_style_text_font(xiaozhi_title_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(xiaozhi_title_label, lv_color_hex(0x333333), 0);
    lv_obj_align(xiaozhi_title_label, LV_ALIGN_CENTER, 0, 0);

    /* 菜单按钮 (右侧) */
    lv_obj_t *menu_btn = lv_label_create(top_bar);
    lv_label_set_text(menu_btn, "=");
    lv_obj_set_style_text_font(menu_btn,
        vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(menu_btn, lv_color_hex(0x333333), 0);
    lv_obj_align(menu_btn, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_add_flag(menu_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(menu_btn, menu_btn_click_handler, LV_EVENT_CLICKED, NULL);

    /* ===== 音频模式标签 ===== */
    xiaozhi_audio_mode_label = lv_label_create(xiaozhi_main_base);
    if (!xiaozhi_ai_ws_is_audio_available()) {
        lv_label_set_text(xiaozhi_audio_mode_label, "[文字模式]");
    } else {
        lv_label_set_text(xiaozhi_audio_mode_label, "[语音模式]");
    }
    lv_obj_set_style_text_font(xiaozhi_audio_mode_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_set_style_text_color(xiaozhi_audio_mode_label, lv_color_hex(0x999999), 0);
    lv_obj_align(xiaozhi_audio_mode_label, LV_ALIGN_TOP_MID, 0, 60);

    /* ===== 中间内容区 ===== */

    /* ===== 笑脸图标 (使用LVGL绘制) ===== */

    /* 容器：120x120圆形白色半透明背景 */
    xiaozhi_face_container = lv_obj_create(xiaozhi_main_base);
    lv_obj_set_size(xiaozhi_face_container, 120, 120);
    lv_obj_set_style_bg_color(xiaozhi_face_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(xiaozhi_face_container, LV_OPA_60, 0);
    lv_obj_set_style_border_width(xiaozhi_face_container, 0, 0);
    lv_obj_set_style_radius(xiaozhi_face_container, 60, 0);
    lv_obj_set_style_pad_all(xiaozhi_face_container, 0, 0);
    lv_obj_clear_flag(xiaozhi_face_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(xiaozhi_face_container, LV_ALIGN_CENTER, 0, -50);

    /* 左眼：14x14深色圆形 */
    xiaozhi_face_eye_left = lv_obj_create(xiaozhi_face_container);
    lv_obj_set_size(xiaozhi_face_eye_left, 14, 14);
    lv_obj_set_style_bg_color(xiaozhi_face_eye_left, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(xiaozhi_face_eye_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(xiaozhi_face_eye_left, 0, 0);
    lv_obj_set_style_radius(xiaozhi_face_eye_left, 7, 0);
    lv_obj_set_style_pad_all(xiaozhi_face_eye_left, 0, 0);
    lv_obj_clear_flag(xiaozhi_face_eye_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(xiaozhi_face_eye_left, 30, 40);

    /* 右眼：14x14深色圆形 */
    xiaozhi_face_eye_right = lv_obj_create(xiaozhi_face_container);
    lv_obj_set_size(xiaozhi_face_eye_right, 14, 14);
    lv_obj_set_style_bg_color(xiaozhi_face_eye_right, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(xiaozhi_face_eye_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(xiaozhi_face_eye_right, 0, 0);
    lv_obj_set_style_radius(xiaozhi_face_eye_right, 7, 0);
    lv_obj_set_style_pad_all(xiaozhi_face_eye_right, 0, 0);
    lv_obj_clear_flag(xiaozhi_face_eye_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(xiaozhi_face_eye_right, 76, 40);

    /* 嘴巴：线条对象，表情根据状态变化 */
    xiaozhi_face_mouth = lv_line_create(xiaozhi_face_container);
    lv_obj_set_style_line_width(xiaozhi_face_mouth, 3, 0);
    lv_obj_set_style_line_color(xiaozhi_face_mouth, lv_color_hex(0x555555), 0);
    lv_obj_set_style_line_rounded(xiaozhi_face_mouth, true, 0);

    /* 问候语 */
    xiaozhi_status_label = lv_label_create(xiaozhi_main_base);
    lv_label_set_text(xiaozhi_status_label, "Hi! 有什么可以帮到你呢？");
    lv_obj_set_style_text_font(xiaozhi_status_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_26"), 0);
    lv_obj_set_style_text_color(xiaozhi_status_label, lv_color_hex(0x333333), 0);
    lv_obj_set_width(xiaozhi_status_label, WATCH_SCREEN_WIDTH - 40);
    lv_obj_set_style_text_align(xiaozhi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(xiaozhi_status_label, LV_ALIGN_CENTER, 0, 50);

    /* 副标题 */
    xiaozhi_status_sub_label = lv_label_create(xiaozhi_main_base);
    if (!xiaozhi_ai_ws_is_audio_available()) {
        lv_label_set_text(xiaozhi_status_sub_label, "语音功能暂不可用");
    } else {
        lv_label_set_text(xiaozhi_status_sub_label, "");
    }
    lv_obj_set_style_text_font(xiaozhi_status_sub_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_set_style_text_color(xiaozhi_status_sub_label, lv_color_hex(0x999999), 0);
    lv_obj_set_width(xiaozhi_status_sub_label, WATCH_SCREEN_WIDTH - 40);
    lv_obj_set_style_text_align(xiaozhi_status_sub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(xiaozhi_status_sub_label, LV_ALIGN_CENTER, 0, 85);

    /* ===== 对话区域 (初始隐藏) ===== */
    xiaozhi_conversation_area = lv_obj_create(xiaozhi_main_base);
    lv_obj_set_size(xiaozhi_conversation_area, WATCH_SCREEN_WIDTH, 260);
    lv_obj_set_style_bg_opa(xiaozhi_conversation_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(xiaozhi_conversation_area, 0, 0);
    lv_obj_set_style_pad_all(xiaozhi_conversation_area, 10, 0);
    lv_obj_set_flex_flow(xiaozhi_conversation_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(xiaozhi_conversation_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(xiaozhi_conversation_area, LV_DIR_VER);
    lv_obj_set_style_pad_gap(xiaozhi_conversation_area, 8, 0);
    lv_obj_align(xiaozhi_conversation_area, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_add_flag(xiaozhi_conversation_area, LV_OBJ_FLAG_HIDDEN);

    /* ===== 底部操作按钮 ===== */
    xiaozhi_action_btn = lv_obj_create(xiaozhi_main_base);
    lv_obj_set_size(xiaozhi_action_btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(xiaozhi_action_btn, &btn_style, 0);
    lv_obj_add_style(xiaozhi_action_btn, &btn_pressed_style, LV_STATE_PRESSED);
    lv_obj_align(xiaozhi_action_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_clear_flag(xiaozhi_action_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(xiaozhi_action_btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    xiaozhi_action_btn_label = lv_label_create(xiaozhi_action_btn);
    lv_label_set_text(xiaozhi_action_btn_label, "开始对话");
    lv_obj_set_style_text_font(xiaozhi_action_btn_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(xiaozhi_action_btn_label, lv_color_white(), 0);
    lv_obj_align(xiaozhi_action_btn_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(xiaozhi_action_btn_label, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(xiaozhi_action_btn, action_btn_click_handler, LV_EVENT_CLICKED, NULL);

    /* ===== 手势和删除回调 ===== */
    lv_obj_add_event_cb(xiaozhi_main_base, slide_gesture_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(xiaozhi_main_base, xiaozhi_page_deleted_cb, LV_EVENT_DELETE, NULL);

    /* ===== 推入页面栈 ===== */
    vw_watch_push_page(xiaozhi_main_base);

    /* ===== 初始化WebSocket模块 ===== */
    xiaozhi_ai_ws_init();

    /* 设为IDLE状态 */
    g_xiaozhi_state = XIAOZHI_STATE_IDLE;
    xiaozhi_update_ui_state(XIAOZHI_STATE_IDLE, NULL);

    XIAOZHI_LOG("xiaozhi_ai: page created\n");
}

/* ========== 公开入口 ========== */

void xiaozhi_ai_app_click_callback(lv_event_t *e)
{
    xiaozhi_ai_page_create();
}

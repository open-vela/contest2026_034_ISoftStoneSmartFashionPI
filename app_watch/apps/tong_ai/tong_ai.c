/**
 * @file tong_ai.c
 * 小通AI应用UI层实现 (火山引擎端到端语音)
 *
 * 基于LVGL实现小通AI的用户界面，通过火山引擎E2E模块的回调
 * 驱动UI状态更新。保留原有页面布局(浅蓝背景、笑脸、对话区域、按钮)。
 *
 * 架构:
 *   - LVGL主线程: 按钮事件 → volc_voice_start/stop
 *   - volc start线程: 阻塞调用 volc_voice_start
 *   - volc recv_thread: 接收服务器事件 → 回调 → 设置pending标志
 *   - LVGL定时器(50ms): 处理pending标志 → 更新UI
 */

#include "tong_ai.h"
#include "tong_ai_ws.h"
#include "../common/watch_pages.h"
#include "../launcher/launcher.h"
#include "../settings/settings_wifi.h"
#include "../home_control/home_control.h"
#include "../../resource/resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <syslog.h>
#include <errno.h>
#include <stdbool.h>
#include <nuttx/power/axp2101.h>
#include <nuttx/lcd/co5300.h>

/* ── 音量控制（本地硬件操作，不抑制TTS）────────────────── */
extern int  watch_volume_set_percent(int percent);
extern int  watch_volume_get_percent(void);
extern int  watch_volume_step_delta(int delta);
extern void volume_ring_refresh_ui(void);

/* ── 亮度控制（本地硬件操作，不抑制TTS）────────────────── */
extern void display_refresh_brightness_ui(void);

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define TONG_LOG(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

/* ========== UI状态枚举 ========== */
typedef enum {
    TONG_STATE_IDLE = 0,        /**< 待命，显示问候语 */
    TONG_STATE_CONNECTING,      /**< 正在连接服务器 */
    TONG_STATE_CONNECTED,       /**< 已连接，等待说话 */
    TONG_STATE_LISTENING,       /**< 用户正在说话(服务端VAD) */
    TONG_STATE_THINKING,       /**< 用户说完了，等待AI回复 */
    TONG_STATE_SPEAKING,       /**< 正在播放AI回复 */
    TONG_STATE_ERROR            /**< 连接错误 */
} tong_ui_state_t;

/* ========== 静态UI对象 ========== */
static lv_obj_t *tong_main_base = NULL;
static lv_obj_t *tong_title_label = NULL;
static lv_obj_t *tong_face_container = NULL;
static lv_obj_t *tong_face_eye_left = NULL;
static lv_obj_t *tong_face_eye_right = NULL;
static lv_obj_t *tong_face_mouth = NULL;
static lv_obj_t *tong_status_label = NULL;
static lv_obj_t *tong_status_sub_label = NULL;
static lv_obj_t *tong_audio_mode_label = NULL;
static lv_obj_t *tong_action_btn = NULL;
static lv_obj_t *tong_action_btn_label = NULL;
static lv_obj_t *tong_back_btn = NULL;

/* ========== 状态追踪 ========== */
static tong_ui_state_t g_tong_state = TONG_STATE_IDLE;

/* ========== 线程安全的事件传递(从volc回调到LVGL定时器) ========== */
#define PENDING_STATE        0x01
#define PENDING_ERROR        0x02
#define PENDING_DISCONNECTED 0x04
#define PENDING_VOLUME_SYNC  0x08
#define PENDING_BRIGHTNESS_SYNC 0x10

static volatile uint8_t g_pending_flags = 0;
static volc_ui_state_t g_pending_volc_state = VOLC_UI_IDLE;
static char *g_pending_error_msg = NULL;
#define PENDING_ERROR_MSG_SIZE 256

/* ========== 启动线程 ========== */
static volatile bool g_starting = false;
static volatile int g_start_ret = -1;
static volatile bool g_cancel_requested = false;
static pthread_t g_start_tid;
static lv_timer_t *g_volc_timer = NULL;

/* 异常断开后自动重连(timer tick=50ms)，连续失败达上限后转手动 */
#define RECONNECT_DELAY_TICKS 60   /* 60 x 50ms = 3s */
#define RECONNECT_MAX_RETRY  3
static int g_reconnect_ticks = 0;
static int g_reconnect_count = 0;

/* ========== 前向声明 ========== */
static void slide_gesture_handler(lv_event_t *e);
static void action_btn_click_handler(lv_event_t *e);
static void back_btn_click_handler(lv_event_t *e);
static void menu_btn_click_handler(lv_event_t *e);
static void tong_update_ui_state(tong_ui_state_t state, const char *error_msg);
static void volc_timer_cb(lv_timer_t *timer);
static void tong_page_deleted_cb(lv_event_t *e);
static void volc_event_callback(volc_callback_type_t type, const char *data);
static void *voice_start_thread(void *arg);
static bool start_voice_session(void);

/* ========== 公开API ========== */

bool tong_ai_is_active(void)
{
    return tong_main_base != NULL;
}

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
static void tong_update_face_emotion(int mouth_type)
{
    if (!tong_face_mouth) return;

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

    lv_line_set_points(tong_face_mouth, pts, pt_count);
}

/* ========== 状态更新 ========== */

static void tong_update_ui_state(tong_ui_state_t state, const char *error_msg)
{
    g_tong_state = state;

    if (!tong_main_base) return;

    switch (state) {
    case TONG_STATE_IDLE:
        if (tong_title_label)
            lv_label_set_text(tong_title_label, "待命");
        if (tong_face_container) {
            lv_obj_clear_flag(tong_face_container, LV_OBJ_FLAG_HIDDEN);
            tong_update_face_emotion(0);
        }
        if (tong_status_label) {
            lv_label_set_text(tong_status_label, "Hi! 有什么可以帮到你呢？");
            lv_obj_clear_flag(tong_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (tong_status_sub_label)
            lv_label_set_text(tong_status_sub_label, "");
        if (tong_action_btn_label)
            lv_label_set_text(tong_action_btn_label, "开始对话");
        break;

    case TONG_STATE_CONNECTING:
        if (tong_title_label)
            lv_label_set_text(tong_title_label, "连接中...");
        if (tong_face_container) {
            lv_obj_clear_flag(tong_face_container, LV_OBJ_FLAG_HIDDEN);
            tong_update_face_emotion(1);
        }
        if (tong_status_label) {
            lv_label_set_text(tong_status_label, "正在连接服务器...");
            lv_obj_clear_flag(tong_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (tong_status_sub_label)
            lv_label_set_text(tong_status_sub_label, "");
        if (tong_action_btn_label)
            lv_label_set_text(tong_action_btn_label, "取消");
        break;

    case TONG_STATE_CONNECTED:
        if (tong_title_label)
            lv_label_set_text(tong_title_label, "小通AI");
        if (tong_face_container) {
            lv_obj_clear_flag(tong_face_container, LV_OBJ_FLAG_HIDDEN);
            tong_update_face_emotion(0);
        }
        if (tong_status_label) {
            lv_label_set_text(tong_status_label, "小通已就绪");
            lv_obj_clear_flag(tong_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (tong_status_sub_label)
            lv_label_set_text(tong_status_sub_label, "可以直接说话");
        if (tong_action_btn_label)
            lv_label_set_text(tong_action_btn_label, "结束对话");
        break;

    case TONG_STATE_LISTENING:
        if (tong_title_label)
            lv_label_set_text(tong_title_label, "聆听中");
        if (tong_face_container) {
            lv_obj_clear_flag(tong_face_container, LV_OBJ_FLAG_HIDDEN);
            tong_update_face_emotion(1);
        }
        if (tong_status_label) {
            lv_label_set_text(tong_status_label, "正在聆听...");
            lv_obj_clear_flag(tong_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (tong_status_sub_label)
            lv_label_set_text(tong_status_sub_label, "");
        if (tong_action_btn_label)
            lv_label_set_text(tong_action_btn_label, "结束对话");
        break;

    case TONG_STATE_THINKING:
        if (tong_title_label)
            lv_label_set_text(tong_title_label, "思考中");
        if (tong_face_container) {
            lv_obj_clear_flag(tong_face_container, LV_OBJ_FLAG_HIDDEN);
            tong_update_face_emotion(1);
        }
        if (tong_status_label) {
            lv_label_set_text(tong_status_label, "小通正在思考...");
            lv_obj_clear_flag(tong_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (tong_status_sub_label)
            lv_label_set_text(tong_status_sub_label, "");
        if (tong_action_btn_label)
            lv_label_set_text(tong_action_btn_label, "结束对话");
        break;

    case TONG_STATE_SPEAKING:
        if (tong_title_label)
            lv_label_set_text(tong_title_label, "回复中");
        if (tong_face_container) {
            lv_obj_clear_flag(tong_face_container, LV_OBJ_FLAG_HIDDEN);
            tong_update_face_emotion(0);
        }
        if (tong_status_label) {
            lv_label_set_text(tong_status_label, "小通正在回复...");
            lv_obj_clear_flag(tong_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (tong_status_sub_label)
            lv_label_set_text(tong_status_sub_label, "");
        if (tong_action_btn_label)
            lv_label_set_text(tong_action_btn_label, "打断");
        break;

    case TONG_STATE_ERROR:
        if (tong_title_label)
            lv_label_set_text(tong_title_label, "连接失败");
        if (tong_face_container) {
            lv_obj_clear_flag(tong_face_container, LV_OBJ_FLAG_HIDDEN);
            tong_update_face_emotion(2);
        }
        if (tong_status_label) {
            if (error_msg && error_msg[0]) {
                lv_label_set_text(tong_status_label, error_msg);
            } else {
                lv_label_set_text(tong_status_label, "连接失败，请重试");
            }
            lv_obj_clear_flag(tong_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (tong_status_sub_label) {
            lv_label_set_text(tong_status_sub_label, "请检查网络连接后重试");
        }
        if (tong_action_btn_label)
            lv_label_set_text(tong_action_btn_label, "重试");
        break;
    }
}

/* ========== ASR发音变体匹配（复用tong_ai_ws.c中的数组，避免DRAM重复） ========== */

extern const char * const VAR_CHAOWAN[];
extern const char * const VAR_BIAOQING[];
extern const char * const VAR_BIOPAN[];

static int strstr_any(const char *buf, const char * const variants[])
{
    for (int i = 0; variants[i] != NULL; i++) {
        if (strstr(buf, variants[i])) return 1;
    }
    return 0;
}

/* ========== 音量本地调节（静默执行，不抑制TTS）============
 * E2E模式下服务器LLM无法控制硬件音量，收到ASR文本后
 * 本地静默执行硬件操作，服务器TTS正常播放确认回复。
 * ============================================================ */

static void try_volume_adjust(const char *text)
{
    if (!text || !text[0]) return;

    /* ── 音量最大/最小 ──────────────────────────────────── */
    static const char * const vol_max_kw[] = {
        "调到最大", "调至最大", "最大声", "声音最大", "音量最大", NULL };
    static const char * const vol_min_kw[] = {
        "调到最小", "调至最小", "最小声", "声音最小", "音量最小", NULL };
    if (strstr_any(text, vol_max_kw)) {
        watch_volume_set_percent(100);
        syslog(LOG_INFO, "[TONG] vol max → 100%%\n");
        return;
    }
    if (strstr_any(text, vol_min_kw)) {
        watch_volume_set_percent(10);
        syslog(LOG_INFO, "[TONG] vol min → 10%%\n");
        return;
    }

    /* ── 音量调大 ────────────────────────────────────────── */
    static const char * const vol_up_kw[] = {
        "大声", "音量调大", "调大音量", "声音大", "加大音量",
        "提高音量", "音量增大", "声音太小", "听不见", "响一点",
        "音量加", "音量调高", "声音大点", "太小声", "大点声",
        "大点声音", "再大声", "调大点", "声音调大", "音量提高",
        "声音开大", "开大点声", "再响点", "加点音量", "音量不够",
        "听不清", "声音好小", "太轻了", "大一点", "大点儿", NULL };
    if (strstr_any(text, vol_up_kw)) {
        int pct = watch_volume_step_delta(10);
        syslog(LOG_INFO, "[TONG] vol up → %d%%\n", pct);
        return;
    }

    /* ── 音量调小 ────────────────────────────────────────── */
    static const char * const vol_down_kw[] = {
        "小声", "音量调小", "调小音量", "声音小", "减小音量",
        "降低音量", "音量减小", "声音太大", "太吵", "轻一点",
        "音量减", "音量调低", "声音小点", "太大声", "小点声",
        "小点声音", "再小声", "调小点", "声音调小", "音量降低",
        "声音开小", "开小点声", "太吵了", "吵死了", "好吵",
        "太响了", "震耳朵", "小一点", "小点儿", NULL };
    if (strstr_any(text, vol_down_kw)) {
        int pct = watch_volume_step_delta(-10);
        syslog(LOG_INFO, "[TONG] vol down → %d%%\n", pct);
        return;
    }

    /* [REMOVED 2026-08-17] 语音"静音"分支已删除：音量设 0 后服务器 TTS
     * 确认与后续回复均不可闻；且"取消静音/解除静音"含"静音"子串，会
     * 在此被抢先匹配反而触发静音。"静音"交给服务器人格化应答（可闻）。 */

    /* ── 取消静音 ────────────────────────────────────────── */
    static const char * const vol_unmute_kw[] = {
        "取消静音", "解除静音", "打开声音", "恢复音量",
        "打开音量", "有声音", "出声音", "恢复声音", "开启声音",
        "不要静音", "声音回来", "可以说话了", NULL };
    if (strstr_any(text, vol_unmute_kw)) {
        watch_volume_set_percent(50);
        syslog(LOG_INFO, "[TONG] vol unmute → 50%%\n");
        return;
    }

    /* ── 音量设为指定值 ──────────────────────────────────── */
    static const char * const vol_set_kw[] = {
        "音量调到", "音量设为", "设置音量", "音量设置",
        "调音量到", "把音量调到", "音量调到第", "调到",
        "音量调成", "音量改到", "音量变成", "调成",
        "调到百分之", "设为百分之", "音量百分之", NULL };
    if (strstr_any(text, vol_set_kw)) {
        const char *p = text;
        while (*p) {
            if (*p >= '0' && *p <= '9') {
                int num = (int)strtol(p, (char**)&p, 10);
                if (num > 0) {
                    if (num < 10) num = 10;
                    if (num > 100) num = 100;
                    watch_volume_set_percent(num);
                    syslog(LOG_INFO, "[TONG] vol set → %d%%\n", num);
                }
                return;
            }
            p++;
        }
    }
}

/* ========== 亮度本地调节（静默执行，不抑制TTS）============ */

static void try_brightness_adjust(const char *text)
{
    if (!text || !text[0]) return;

    /* ── 亮度最亮/最暗 ── */
    static const char * const bri_max_kw[] = {
        "亮度最大", "亮度调到最大", "最亮", "调到最亮", "屏幕最亮",
        "亮度调至最大", "屏幕调到最亮", NULL };
    static const char * const bri_min_kw[] = {
        "亮度最小", "亮度调到最小", "最暗", "调到最暗", "屏幕最暗",
        "亮度调至最小", "屏幕调到最暗", NULL };
    if (strstr_any(text, bri_max_kw)) {
        esp32s3_set_brightness(4);
        syslog(LOG_INFO, "[TONG] brightness max → level 4\n");
        return;
    }
    if (strstr_any(text, bri_min_kw)) {
        esp32s3_set_brightness(0);
        syslog(LOG_INFO, "[TONG] brightness min → level 0\n");
        return;
    }

    /* ── 亮度调亮 ── */
    static const char * const bri_up_kw[] = {
        "调亮", "亮一点", "太暗了", "亮度调高", "增加亮度",
        "亮度大一点", "亮一些", "再亮一点", "屏幕调亮", "亮度提高",
        "亮点", "太暗", "看不清屏幕", "屏幕太暗", "暗了", NULL };
    if (strstr_any(text, bri_up_kw)) {
        int level = esp32s3_get_brightness();
        if (level < 4) level++;
        esp32s3_set_brightness(level);
        syslog(LOG_INFO, "[TONG] brightness up → level %d\n", level);
        return;
    }

    /* ── 亮度调暗 ── */
    static const char * const bri_down_kw[] = {
        "调暗", "暗一点", "太亮了", "亮度调低", "降低亮度",
        "亮度小一点", "暗一些", "再暗一点", "屏幕调暗", "亮度降低",
        "暗点", "太亮", "屏幕太亮", "刺眼", "亮了", NULL };
    if (strstr_any(text, bri_down_kw)) {
        int level = esp32s3_get_brightness();
        if (level > 0) level--;
        esp32s3_set_brightness(level);
        syslog(LOG_INFO, "[TONG] brightness down → level %d\n", level);
        return;
    }
}

/* ========== volc事件回调 (从recv_thread/start_thread调用) ========== */

static void volc_event_callback(volc_callback_type_t type, const char *data)
{
    switch (type) {
    case VOLC_CB_UI_STATE:
        if (data) {
            g_pending_volc_state = (volc_ui_state_t)(unsigned char)data[0];
            g_pending_flags |= PENDING_STATE;
        }
        break;
    case VOLC_CB_ERROR_MSG:
        if (data && g_pending_error_msg) {
            strncpy(g_pending_error_msg, data, PENDING_ERROR_MSG_SIZE - 1);
            g_pending_error_msg[PENDING_ERROR_MSG_SIZE - 1] = '\0';
            g_pending_flags |= PENDING_ERROR;
        }
        break;
    case VOLC_CB_CONNECTED:
        /* 由 VOLC_CB_UI_STATE(VOLC_UI_CONNECTED) 处理 */
        break;
    case VOLC_CB_USER_TEXT:
        if (data) {
            syslog(LOG_INFO, "[TONG] ASR: \"%s\"\n", data);

            /* 智能家居指令检测（优先于UI模式切换） */
            int hc_ret = home_control_voice_execute(data);
            if (hc_ret == HOME_CTRL_NET_FAIL) {
                /* 匹配到设备但网络失败 */
                syslog(LOG_INFO,
                    "[TONG] home ctrl net fail: \"%s\"\n", data);
                volc_play_local_wav(HOME_CTRL_FAIL_WAV);
                break;
            } else if (hc_ret == HOME_CTRL_NO_MATCH) {
                /* 有开关动作词但未匹配到设备 */
                syslog(LOG_INFO,
                    "[TONG] home ctrl no match: \"%s\"\n", data);
                volc_play_local_wav(HOME_CTRL_NOT_FOUND_WAV);
                break;
            }
            /* HOME_CTRL_OK: 指令成功，继续正常流程
             * HOME_CTRL_NONE: 无关指令，继续UI模式切换检测 */

            int matched = 0;
            /* 策略1: 完整短语匹配（含发音变体） */
            if (strstr_any(data, VAR_CHAOWAN) ||
                strstr_any(data, VAR_BIAOQING)) {
                matched = 1;
            }
            /* 策略2: 动作词+目标词变体组合匹配 */
            if (!matched) {
                int has_action = (strstr(data, "切换") ||
                    strstr(data, "打开") ||
                    strstr(data, "进入") ||
                    strstr(data, "回到") ||
                    strstr(data, "返回"));
                int has_target = (strstr_any(data, VAR_CHAOWAN) ||
                    strstr_any(data, VAR_BIAOQING) ||
                    strstr_any(data, VAR_BIOPAN));
                if (has_action && has_target) matched = 1;
            }
            if (matched) {
                syslog(LOG_INFO, "[TONG] UI mode switch triggered: %s\n", data);
                FILE *fp = fopen("/mnt/spif/ui_mode.json", "w");
                if (fp) {
                    fprintf(fp, "{\"ui_mode\":0}\n");
                    fclose(fp);
                }
                axp2101_power_reset();
                /* 不会到达这里 */
            }

            /* 音量本地调节——静默执行硬件操作，不抑制TTS */
            try_volume_adjust(data);
            g_pending_flags |= PENDING_VOLUME_SYNC;

            /* 亮度本地调节——静默执行硬件操作，不抑制TTS */
            try_brightness_adjust(data);
            g_pending_flags |= PENDING_BRIGHTNESS_SYNC;
        }
        break;
    case VOLC_CB_DISCONNECTED:
        g_pending_flags |= PENDING_DISCONNECTED;
        break;
    }
}

/* ========== LVGL定时器 — 处理pending事件 ========== */

static void volc_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 取出所有pending标志(原子性读取) */
    uint8_t flags = g_pending_flags;
    g_pending_flags = 0;

    if (flags & PENDING_STATE) {
        tong_ui_state_t ui_state;
        switch (g_pending_volc_state) {
        case VOLC_UI_IDLE:          ui_state = TONG_STATE_IDLE; break;
        case VOLC_UI_CONNECTING:   ui_state = TONG_STATE_CONNECTING; break;
        case VOLC_UI_CONNECTED:    ui_state = TONG_STATE_CONNECTED; break;
        case VOLC_UI_LISTENING:    ui_state = TONG_STATE_LISTENING; break;
        case VOLC_UI_THINKING:     ui_state = TONG_STATE_THINKING; break;
        case VOLC_UI_SPEAKING:     ui_state = TONG_STATE_SPEAKING; break;
        case VOLC_UI_DISCONNECTING: ui_state = TONG_STATE_IDLE; break;
        case VOLC_UI_ERROR:        ui_state = TONG_STATE_ERROR; break;
        default:                   ui_state = TONG_STATE_IDLE; break;
        }
        if (g_pending_volc_state == VOLC_UI_CONNECTED)
            g_reconnect_count = 0;  /* 会话健康，重置自动重连计数 */
        tong_update_ui_state(ui_state, NULL);
    }

    if (flags & PENDING_ERROR) {
        tong_update_ui_state(TONG_STATE_ERROR, g_pending_error_msg);
        /* 会话异常死亡(服务端断开等)后延迟自动重连，免得用户必须
         * 手动恢复。连续失败达上限后停在 ERROR 等手动。 */
        if (!g_cancel_requested && g_reconnect_count < RECONNECT_MAX_RETRY)
            g_reconnect_ticks = RECONNECT_DELAY_TICKS;
    }

    if (flags & PENDING_DISCONNECTED) {
        tong_update_ui_state(TONG_STATE_IDLE, NULL);
        g_reconnect_ticks = 0;  /* 主动停止，取消自动重连 */
    }

    if (flags & PENDING_VOLUME_SYNC) {
        volume_ring_refresh_ui();
    }

    if (flags & PENDING_BRIGHTNESS_SYNC) {
        display_refresh_brightness_ui();
    }

    /* 异常断开自动重连倒计时 */
    if (g_reconnect_ticks > 0) {
        g_reconnect_ticks--;
        if (g_reconnect_ticks == 0) {
            if (!g_starting && !g_cancel_requested && !volc_voice_is_active()) {
                TONG_LOG("[TONG] auto reconnecting after abnormal disconnect\n");
                g_reconnect_count++;
                tong_update_ui_state(TONG_STATE_CONNECTING, NULL);
                start_voice_session();
            }
        }
    }

    /* 检查启动线程状态 */
    if (!g_starting && g_cancel_requested) {
        g_cancel_requested = false;
        if (g_start_ret == 0) {
            /* 启动成功但用户已取消，需要停止 */
            volc_voice_stop();
        }
        tong_update_ui_state(TONG_STATE_IDLE, NULL);
    }
}

/* ========== 启动线程 (避免阻塞LVGL) ========== */

#define START_THREAD_STACK (32 * 1024)

static void *voice_start_thread(void *arg)
{
    (void)arg;
    TONG_LOG("[TONG] voice_start_thread begin\n");
    g_start_ret = volc_voice_start(volc_event_callback);
    TONG_LOG("[TONG] voice_start_thread ret=%d\n", g_start_ret);

    if (g_cancel_requested) {
        if (g_start_ret == 0) {
            /* 启动成功但用户已取消 */
            volc_voice_stop();
        }
        /* 启动失败的清理由volc模块内部完成 */
    }
    /* 成功时：回调会通过pending标志驱动UI到CONNECTED状态 */

    g_starting = false;
    return NULL;
}

/* 创建启动线程(按钮/自动重连共用)。调用前 UI 应已切到 CONNECTING。 */
static bool start_voice_session(void)
{
    g_cancel_requested = false;
    g_starting = true;
    g_start_ret = -1;

    volc_voice_init();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, START_THREAD_STACK);
    int ret = pthread_create(&g_start_tid, &attr, voice_start_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        TONG_LOG("[TONG] start thread create failed: %d\n", ret);
        g_starting = false;
        tong_update_ui_state(TONG_STATE_ERROR, "无法启动语音服务");
        return false;
    }
    return true;
}

/* ========== 按钮事件处理 ========== */

static void action_btn_click_handler(lv_event_t *e)
{
    switch (g_tong_state) {
    case TONG_STATE_IDLE: {
        /* 开始连接 */
        if (!settings_wifi_is_connected()) {
            tong_update_ui_state(TONG_STATE_ERROR, "请先连接WiFi");
            return;
        }

        tong_update_ui_state(TONG_STATE_CONNECTING, NULL);
        start_voice_session();
        break;
    }

    case TONG_STATE_CONNECTING:
        /* 取消连接 — 设置标志，等待启动线程超时返回 */
        g_cancel_requested = true;
        if (tong_action_btn_label)
            lv_label_set_text(tong_action_btn_label, "取消中...");
        break;

    case TONG_STATE_CONNECTED:
    case TONG_STATE_LISTENING:
    case TONG_STATE_THINKING:
        /* 结束对话 */
        volc_voice_stop();
        /* volc_voice_stop会发送DISCONNECTED回调，由定时器处理UI更新 */
        break;

    case TONG_STATE_SPEAKING:
        /* 打断TTS播放 */
        volc_voice_interrupt();
        break;

    case TONG_STATE_ERROR:
        /* 重试 */
        tong_update_ui_state(TONG_STATE_CONNECTING, NULL);
        start_voice_session();
        break;
    }
}

static void back_btn_click_handler(lv_event_t *e)
{
    TONG_LOG("tong_ai: back button pressed\n");

    /* 停止语音会话 */
    if (volc_voice_is_active()) {
        volc_voice_stop();
    }

    /* 等待启动线程结束(如果还在运行) */
    if (g_starting) {
        g_cancel_requested = true;
        /* 给启动线程一些时间退出 */
        for (int i = 0; i < 100 && g_starting; i++) {
            usleep(100000);
        }
    }

    /* 停止定时器 */
    if (g_volc_timer) {
        lv_timer_del(g_volc_timer);
        g_volc_timer = NULL;
    }

    /* 退出页面 */
    if (tong_main_base) {
        vw_watch_pop_page(tong_main_base);
        lv_obj_del(tong_main_base);
        tong_main_base = NULL;
    }
}

static void menu_btn_click_handler(lv_event_t *e)
{
    TONG_LOG("tong_ai: menu button pressed\n");
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
            TONG_LOG("tong_ai: right swipe, exit\n");
            back_btn_click_handler(NULL);
        }
        break;
    }

    default:
        break;
    }
}

/* ========== 页面删除回调 ========== */

static void tong_page_deleted_cb(lv_event_t *e)
{
    TONG_LOG("tong_ai: page deleted\n");

    /* 停止语音会话(含异常死亡后未释放的残留资源清理) */
    volc_voice_stop();

    /* 清理定时器 */
    if (g_volc_timer) {
        lv_timer_del(g_volc_timer);
        g_volc_timer = NULL;
    }

    /* 重置状态 */
    g_pending_flags = 0;
    g_starting = false;
    g_cancel_requested = false;
    g_tong_state = TONG_STATE_IDLE;
    g_reconnect_ticks = 0;
    g_reconnect_count = 0;

    /* 释放pending文本缓冲区 */
    free(g_pending_error_msg);  g_pending_error_msg = NULL;

    /* 清理UI指针 */
    tong_main_base = NULL;
    tong_title_label = NULL;
    tong_face_container = NULL;
    tong_face_eye_left = NULL;
    tong_face_eye_right = NULL;
    tong_face_mouth = NULL;
    tong_status_label = NULL;
    tong_status_sub_label = NULL;
    tong_audio_mode_label = NULL;
    tong_action_btn = NULL;
    tong_action_btn_label = NULL;
    tong_back_btn = NULL;
}

/* ========== 页面创建 ========== */

static void tong_ai_page_create(void)
{
    TONG_LOG("tong_ai: creating page\n");

    init_button_styles();

    /* 创建主页面 */
    tong_main_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(tong_main_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(tong_main_base, LV_OBJ_FLAG_SCROLLABLE);
    /* 浅蓝背景 */
    lv_obj_set_style_bg_color(tong_main_base, lv_color_hex(0xE8F4FD), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(tong_main_base, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(tong_main_base, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(tong_main_base, 0, 0);
    lv_obj_align(tong_main_base, LV_ALIGN_CENTER, 0, 0);

    /* ===== 顶部状态栏 ===== */
    lv_obj_t *top_bar = lv_obj_create(tong_main_base);
    lv_obj_set_size(top_bar, WATCH_SCREEN_WIDTH, 55);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);

    /* 返回按钮 (左侧) */
    tong_back_btn = lv_label_create(top_bar);
    lv_label_set_text(tong_back_btn, "<");
    lv_obj_set_style_text_font(tong_back_btn,
        vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(tong_back_btn, lv_color_hex(0x333333), 0);
    lv_obj_align(tong_back_btn, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_add_flag(tong_back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tong_back_btn, back_btn_click_handler, LV_EVENT_CLICKED, NULL);

    /* 标题 (居中) */
    tong_title_label = lv_label_create(top_bar);
    lv_label_set_text(tong_title_label, "待命");
    lv_obj_set_style_text_font(tong_title_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(tong_title_label, lv_color_hex(0x333333), 0);
    lv_obj_align(tong_title_label, LV_ALIGN_CENTER, 0, 0);

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
    tong_audio_mode_label = lv_label_create(tong_main_base);
    lv_label_set_text(tong_audio_mode_label, "[语音模式]");
    lv_obj_set_style_text_font(tong_audio_mode_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_set_style_text_color(tong_audio_mode_label, lv_color_hex(0x999999), 0);
    lv_obj_align(tong_audio_mode_label, LV_ALIGN_TOP_MID, 0, 60);

    /* ===== 笑脸图标 ===== */
    tong_face_container = lv_obj_create(tong_main_base);
    lv_obj_set_size(tong_face_container, 120, 120);
    lv_obj_set_style_bg_color(tong_face_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(tong_face_container, LV_OPA_60, 0);
    lv_obj_set_style_border_width(tong_face_container, 0, 0);
    lv_obj_set_style_radius(tong_face_container, 60, 0);
    lv_obj_set_style_pad_all(tong_face_container, 0, 0);
    lv_obj_clear_flag(tong_face_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tong_face_container, LV_ALIGN_CENTER, 0, -50);

    /* 左眼 */
    tong_face_eye_left = lv_obj_create(tong_face_container);
    lv_obj_set_size(tong_face_eye_left, 14, 14);
    lv_obj_set_style_bg_color(tong_face_eye_left, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(tong_face_eye_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tong_face_eye_left, 0, 0);
    lv_obj_set_style_radius(tong_face_eye_left, 7, 0);
    lv_obj_set_style_pad_all(tong_face_eye_left, 0, 0);
    lv_obj_clear_flag(tong_face_eye_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tong_face_eye_left, 30, 40);

    /* 右眼 */
    tong_face_eye_right = lv_obj_create(tong_face_container);
    lv_obj_set_size(tong_face_eye_right, 14, 14);
    lv_obj_set_style_bg_color(tong_face_eye_right, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_opa(tong_face_eye_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tong_face_eye_right, 0, 0);
    lv_obj_set_style_radius(tong_face_eye_right, 7, 0);
    lv_obj_set_style_pad_all(tong_face_eye_right, 0, 0);
    lv_obj_clear_flag(tong_face_eye_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tong_face_eye_right, 76, 40);

    /* 嘴巴 */
    tong_face_mouth = lv_line_create(tong_face_container);
    lv_obj_set_style_line_width(tong_face_mouth, 3, 0);
    lv_obj_set_style_line_color(tong_face_mouth, lv_color_hex(0x555555), 0);
    lv_obj_set_style_line_rounded(tong_face_mouth, true, 0);

    /* 问候语 */
    tong_status_label = lv_label_create(tong_main_base);
    lv_label_set_text(tong_status_label, "Hi! 有什么可以帮到你呢？");
    lv_obj_set_style_text_font(tong_status_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_26"), 0);
    lv_obj_set_style_text_color(tong_status_label, lv_color_hex(0x333333), 0);
    lv_obj_set_width(tong_status_label, WATCH_SCREEN_WIDTH - 40);
    lv_obj_set_style_text_align(tong_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tong_status_label, LV_ALIGN_CENTER, 0, 50);

    /* 副标题 */
    tong_status_sub_label = lv_label_create(tong_main_base);
    lv_label_set_text(tong_status_sub_label, "");
    lv_obj_set_style_text_font(tong_status_sub_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_set_style_text_color(tong_status_sub_label, lv_color_hex(0x999999), 0);
    lv_obj_set_width(tong_status_sub_label, WATCH_SCREEN_WIDTH - 40);
    lv_obj_set_style_text_align(tong_status_sub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tong_status_sub_label, LV_ALIGN_CENTER, 0, 85);

    /* ===== 底部操作按钮 ===== */
    tong_action_btn = lv_obj_create(tong_main_base);
    lv_obj_set_size(tong_action_btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(tong_action_btn, &btn_style, 0);
    lv_obj_add_style(tong_action_btn, &btn_pressed_style, LV_STATE_PRESSED);
    lv_obj_align(tong_action_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_clear_flag(tong_action_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tong_action_btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    tong_action_btn_label = lv_label_create(tong_action_btn);
    lv_label_set_text(tong_action_btn_label, "开始对话");
    lv_obj_set_style_text_font(tong_action_btn_label,
        vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(tong_action_btn_label, lv_color_white(), 0);
    lv_obj_align(tong_action_btn_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(tong_action_btn_label, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(tong_action_btn, action_btn_click_handler, LV_EVENT_CLICKED, NULL);

    /* ===== 手势和删除回调 ===== */
    lv_obj_add_event_cb(tong_main_base, slide_gesture_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(tong_main_base, tong_page_deleted_cb, LV_EVENT_DELETE, NULL);

    /* ===== 推入页面栈 ===== */
    vw_watch_push_page(tong_main_base);

    /* ===== 启动volc事件定时器(50ms) ===== */
    g_volc_timer = lv_timer_create(volc_timer_cb, 50, NULL);

    /* 分配错误消息缓冲区到堆(PSRAM), 节省DRAM */
    if (!g_pending_error_msg)  g_pending_error_msg   = malloc(PENDING_ERROR_MSG_SIZE);

    /* 设为IDLE状态 */
    g_tong_state = TONG_STATE_IDLE;
    tong_update_ui_state(TONG_STATE_IDLE, NULL);

    TONG_LOG("tong_ai: page created\n");
}

/* ========== 公开入口 ========== */

void tong_ai_app_click_callback(lv_event_t *e)
{
    tong_ai_page_create();
}

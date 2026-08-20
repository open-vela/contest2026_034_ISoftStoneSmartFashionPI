#include "../common/watch_pages.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include "alarm.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <../../../../apps/graphics/lvgl/lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.h>
#include <nuttx/lcd/co5300.h>
#include <syslog.h>
#include "../common/watch_audio_player.h"

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define ALARM_LOG(fmt, ...) syslog(LOG_INFO, "[ALARM] " fmt, ##__VA_ARGS__)

/* 闹钟铃声循环播放定时器（每 3 秒重新播放一次） */
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
static lv_timer_t *alarm_sound_timer = NULL;
#define ALARM_SOUND_INTERVAL_MS  3000
#define ALARM_SOUND_VOLUME       800

static void alarm_sound_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    esp32s3_watch_audio_play_repeat(WATCH_AUDIO_PLAYER_ALARM_FILE,
                                     ALARM_SOUND_VOLUME);
}

static void alarm_sound_start(void)
{
    /* 立即播放一次 */
    esp32s3_watch_audio_play_repeat(WATCH_AUDIO_PLAYER_ALARM_FILE,
                                     ALARM_SOUND_VOLUME);
    /* 创建循环定时器 */
    if (alarm_sound_timer == NULL) {
        alarm_sound_timer = lv_timer_create(alarm_sound_timer_cb,
                                             ALARM_SOUND_INTERVAL_MS, NULL);
    }
}

static void alarm_sound_stop(void)
{
    if (alarm_sound_timer != NULL) {
        lv_timer_del(alarm_sound_timer);
        alarm_sound_timer = NULL;
    }
    esp32s3_watch_audio_stop();
}
#endif



// 闹钟数据文件路径
#define ALARM_DATA_FILE "/mnt/sd/alarm.dat"

// 全局变量
static lv_obj_t *alarm_list_base = NULL;
static lv_obj_t *alarm_edit_base = NULL;
static lv_obj_t *alarm_ring_base = NULL;
static lv_obj_t *hour_roller = NULL;
static lv_obj_t *minute_roller = NULL;
static lv_obj_t *day_btns[7] = {0};
static lv_obj_t *alarm_list = NULL;
static lv_obj_t *no_alarm_label = NULL;
static lv_style_t alarm_btn_style;

// 闹钟数据（全局变量，供系统级服务使用）
static alarm_t alarms[WATCH_MAX_ALARMS] = {0};
static int alarm_count = 0;
static int current_edit_index = -1;
static int current_ring_index = -1;
static alarm_op_t current_op = ALARM_OP_ADD;

// 系统级闹钟服务相关
static lv_timer_t *alarm_check_timer = NULL;  // 闹钟检测定时器
static bool alarm_service_initialized = false; // 闹钟服务是否已初始化
static time_t last_trigger_time[WATCH_MAX_ALARMS] = {0}; // 每个闹钟最近触发时间

// 延时闹钟相关
#define MAX_SNOOZE_ALARMS 10  // 最大延时闹钟数量

typedef struct {
    int alarm_index;           // 原始闹钟索引
    time_t trigger_time;       // 触发时间
    bool active;               // 是否激活
    bool is_snooze;            // 是否是延时闹钟
} snooze_alarm_t;

static snooze_alarm_t snooze_alarms[MAX_SNOOZE_ALARMS] = {0};  // 延时闹钟数组
static lv_timer_t *snooze_timer = NULL;    // 延时闹钟定时器
static bool is_current_ring_snooze = false; // 当前响铃是否是延时闹钟

// 函数声明
static void slide_gesture_list_handler(lv_event_t *e);
static void slide_gesture_edit_handler(lv_event_t *e);
static void slide_gesture_ring_handler(lv_event_t *e);
static void add_alarm_btn_event_cb(lv_event_t *e);
static void alarm_item_event_cb(lv_event_t *e);
static void confirm_btn_event_cb(lv_event_t *e);
static void cancel_btn_event_cb(lv_event_t *e);
static void day_btn_event_cb(lv_event_t *e);
static void snooze_btn_event_cb(lv_event_t *e);
static void close_btn_event_cb(lv_event_t *e);
static void update_alarm_list(lv_obj_t *container);
static char *get_repeat_text(alarm_t *alarm);
static void alarm_list_create(void);
static void alarm_edit_create(alarm_op_t op, int index);
static void alarm_ring_create(int index);
static void snooze_timer_cb(lv_timer_t *timer);
static void start_snooze_alarm(int alarm_index);
static void stop_snooze_alarm(void);
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
static void alarm_sound_start(void);
static void alarm_sound_stop(void);
#endif
/**
 * 创建闹钟列表页面
 */
static void alarm_list_create(void)
{
    // 创建闹钟列表页面
    alarm_list_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(alarm_list_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(alarm_list_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(alarm_list_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(alarm_list_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(alarm_list_base, LV_ALIGN_CENTER, 0, 0);

    // 创建设备列表
    alarm_list = lv_list_create(alarm_list_base);
    // 根据闹钟数量设置列表高度
    // 如果闹钟数量达到最大值（10个），列表铺满整个屏幕
    // 否则，列表高度为屏幕高度减去添加按钮的高度（120px）
    int list_height = (alarm_count >= WATCH_MAX_ALARMS) ? WATCH_SCREEN_HEIGHT : (WATCH_SCREEN_HEIGHT - 120);
    lv_obj_set_size(alarm_list, WATCH_SCREEN_WIDTH, list_height);
    lv_obj_set_style_bg_color(alarm_list, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(alarm_list, 0, LV_STATE_DEFAULT);
    lv_obj_align(alarm_list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scrollbar_mode(alarm_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(alarm_list, LV_DIR_VER);
    lv_obj_set_flex_flow(alarm_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(alarm_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_snap_x(alarm_list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(alarm_list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_style_pad_row(alarm_list, 10, 0);
    lv_obj_set_style_pad_top(alarm_list, 20, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(alarm_list, 20, LV_STATE_DEFAULT);
    lv_obj_add_flag(alarm_list, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建"无闹钟"标签（在列表之后创建，确保在上层显示）
    no_alarm_label = lv_label_create(alarm_list_base);
    lv_label_set_text(no_alarm_label, "无闹钟");
    lv_obj_set_style_text_font(no_alarm_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(no_alarm_label, lv_color_white(), 0);
    lv_obj_center(no_alarm_label);
    // 默认隐藏
    lv_obj_add_flag(no_alarm_label, LV_OBJ_FLAG_HIDDEN);

    // 更新闹钟列表
    update_alarm_list(alarm_list);

    // 创建添加闹钟按钮（只有闹钟数量小于10时才显示）
    if (alarm_count < WATCH_MAX_ALARMS) {
        lv_obj_t *add_btn = lv_btn_create(alarm_list_base);
        lv_obj_add_event_cb(add_btn, add_alarm_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_size(add_btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
        lv_obj_set_style_bg_color(add_btn, lv_color_hex(0x2D47CB), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(add_btn, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(add_btn, 32, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(add_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(add_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_t *add_btn_label = lv_label_create(add_btn);
        lv_label_set_text(add_btn_label, "添加闹钟");
        lv_obj_set_style_text_font(add_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
        lv_obj_set_style_text_color(add_btn_label, lv_color_white(), 0);
        lv_obj_center(add_btn_label);
        // 添加事件冒泡标志，确保滑动手势能正常工作
        lv_obj_add_flag(add_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // 添加滑动手势处理
    lv_obj_add_event_cb(alarm_list_base, slide_gesture_list_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(alarm_list_base);
}

static void alarm_switch_event_cb(lv_event_t *e)
{
    lv_obj_t *switch_obj = lv_event_get_target(e);
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    
    if (index >= 0 && index < alarm_count) {
        alarms[index].enabled = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
        ALARM_LOG("Alarm %d: %s", index, alarms[index].enabled ? "enabled" : "disabled");
    }
}

/**
 * 更新闹钟列表
 */
static void update_alarm_list(lv_obj_t *list)
{
    // 清除所有子对象
    lv_obj_clean(list);

    // 根据闹钟数量显示/隐藏"无闹钟"标签
    if (alarm_count == 0) {
        // 显示"无闹钟"标签
        if (no_alarm_label != NULL) {
            lv_obj_clear_flag(no_alarm_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    } else {
        // 隐藏"无闹钟"标签
        if (no_alarm_label != NULL) {
            lv_obj_add_flag(no_alarm_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 初始化按钮样式
    static bool style_initialized = false;
    if (!style_initialized) {
        lv_style_init(&alarm_btn_style);
        lv_style_set_bg_opa(&alarm_btn_style, LV_OPA_100);
        lv_style_set_bg_color(&alarm_btn_style, lv_color_make(26, 26, 26));
        lv_style_set_text_color(&alarm_btn_style, lv_color_white());
        lv_style_set_border_width(&alarm_btn_style, 0);
        lv_style_set_radius(&alarm_btn_style, 32);
        lv_style_set_pad_all(&alarm_btn_style, 0);
        lv_style_set_outline_width(&alarm_btn_style, 0);
        lv_style_set_transform_width(&alarm_btn_style, 0);
        lv_style_set_transform_height(&alarm_btn_style, 0);
        lv_style_set_translate_x(&alarm_btn_style, 0);
        lv_style_set_translate_y(&alarm_btn_style, 0);
        style_initialized = true;
    }

    // 按下状态样式
    static lv_style_t pressed_style;
    static bool pressed_style_initialized = false;
    if (!pressed_style_initialized) {
        lv_style_init(&pressed_style);
        lv_style_set_transform_width(&pressed_style, 0);
        lv_style_set_transform_height(&pressed_style, 0);
        lv_style_set_translate_x(&pressed_style, 0);
        lv_style_set_translate_y(&pressed_style, 0);
        lv_style_set_bg_color(&pressed_style, lv_color_make(35, 35, 35));
        pressed_style_initialized = true;
    }

    // 创建闹钟项
    for (int i = 0; i < alarm_count; i++) {
        // 创建按钮
        lv_obj_t *btn = lv_list_add_btn(list, NULL, NULL);
        lv_obj_add_style(btn, &alarm_btn_style, 0);
        lv_obj_add_style(btn, &pressed_style, LV_STATE_PRESSED);
        lv_obj_set_size(btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(btn, LV_LAYOUT_NONE);
        lv_obj_add_event_cb(btn, alarm_item_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        // 添加事件冒泡标志，确保滑动手势能正常工作
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);

        // 时间显示
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", alarms[i].hour, alarms[i].minute);
        lv_obj_t *time_label = lv_label_create(btn);
        lv_label_set_text(time_label, time_str);
        lv_obj_set_style_text_font(time_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
        lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
        lv_obj_set_pos(time_label, 18, 14);
        lv_obj_clear_flag(time_label, LV_OBJ_FLAG_SCROLLABLE);

        // 重复显示
        char *repeat_text = get_repeat_text(&alarms[i]);
        lv_obj_t *repeat_label = lv_label_create(btn);
        lv_label_set_text(repeat_label, repeat_text);
        lv_obj_set_style_text_font(repeat_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_set_style_text_color(repeat_label, lv_color_white(), 0);
        lv_obj_set_style_text_opa(repeat_label, LV_OPA_60, 0);
        lv_obj_set_pos(repeat_label, 18, 59);
        // 设置label宽度和长文本模式，超过宽度显示省略号
        // 最大宽度 = 按钮宽度 - 左边距 - 开关宽度 - 开关右边距 - 右边距 = 340 - 18 - 57 - 18 - 18 = 229
        lv_obj_set_width(repeat_label, 229);
        lv_label_set_long_mode(repeat_label, LV_LABEL_LONG_DOT);
        lv_obj_clear_flag(repeat_label, LV_OBJ_FLAG_SCROLLABLE);

        // 开关
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
        lv_obj_add_event_cb(switch_obj, alarm_switch_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        if (alarms[i].enabled) {
            lv_obj_add_state(switch_obj, LV_STATE_CHECKED);
        }
        lv_obj_set_pos(switch_obj, WATCH_BTN_WIDTH - 57 - 18, (WATCH_BTN_HEIGHT - 32) / 2);
        lv_obj_clear_flag(switch_obj, LV_OBJ_FLAG_SCROLLABLE);
        // 开关不添加事件冒泡标志，防止点击开关时触发按钮的点击事件
        lv_obj_clear_flag(switch_obj, LV_OBJ_FLAG_EVENT_BUBBLE);

        // 添加事件冒泡标志
        lv_obj_add_flag(time_label, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(repeat_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

/**
 * 获取重复文本
 */
static char *get_repeat_text(alarm_t *alarm)
{
    static char repeat_text[64];
    int count = 0;
    int workday_count = 0;
    int weekend_count = 0;

    for (int i = 0; i < 5; i++) {
        if (alarm->repeat[i]) workday_count++;
    }
    for (int i = 5; i < 7; i++) {
        if (alarm->repeat[i]) weekend_count++;
    }

    if (workday_count == 5 && weekend_count == 0) {
        strcpy(repeat_text, "工作日");
        return repeat_text;
    } else if (workday_count == 0 && weekend_count == 2) {
        strcpy(repeat_text, "休息日");
        return repeat_text;
    } else {
        repeat_text[0] = '\0';
        const char *days[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
        for (int i = 0; i < 7; i++) {
            if (alarm->repeat[i]) {
                if (count > 0) strcat(repeat_text, "、");
                strcat(repeat_text, days[i]);
                count++;
            }
        }
        if (count == 0) {
            strcpy(repeat_text, "仅一次");
        }
        return repeat_text;
    }
}

/**
 * 创建闹钟编辑页面
 */
static void alarm_edit_create(alarm_op_t op, int index)
{
    current_op = op;
    current_edit_index = index;

    // 创建闹钟编辑页面
    alarm_edit_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(alarm_edit_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(alarm_edit_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(alarm_edit_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(alarm_edit_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(alarm_edit_base, LV_ALIGN_CENTER, 0, 0);

    // 创建星期按钮
    const char *days[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (int i = 0; i < 7; i++) {
        day_btns[i] = lv_btn_create(alarm_edit_base);
        lv_obj_add_event_cb(day_btns[i], day_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_size(day_btns[i], 42, 42);
        lv_obj_set_style_bg_color(day_btns[i], lv_color_hex(0x1A1A1A), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(day_btns[i], lv_color_hex(0x2D47CB), LV_STATE_CHECKED);
        lv_obj_set_style_border_width(day_btns[i], 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(day_btns[i], 21, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(day_btns[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(day_btns[i], LV_ALIGN_TOP_LEFT, 22 + i * (42 + 8), 20);
        lv_obj_t *day_label = lv_label_create(day_btns[i]);
        lv_label_set_text(day_label, days[i]);
        lv_obj_set_style_text_font(day_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
        lv_obj_set_style_text_color(day_label, lv_color_white(), 0);
        lv_obj_center(day_label);
    }

    // 创建小时滚轮
    hour_roller = lv_roller_create(alarm_edit_base);
    char hours_buf[150] = {0};
    for (int i = 0; i <= 23; i++) {
        char hour_str[10];
        sprintf(hour_str, "%02d\n", i);
        strcat(hours_buf, hour_str);
    }
    hours_buf[strlen(hours_buf)-1] = '\0';
    
    lv_roller_set_options(hour_roller, hours_buf, LV_ROLLER_MODE_INFINITE);
    lv_obj_set_width(hour_roller, 169);
    lv_obj_align(hour_roller, LV_ALIGN_TOP_LEFT, 26, 87);
    
    // 设置滚轮样式
    lv_obj_set_style_border_color(hour_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(hour_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(hour_roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hour_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(hour_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hour_roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(hour_roller, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *alarm_roller_font = vw_resource_get_font(WATCH_REGULAR_FONT "_36");
    lv_obj_set_style_text_font(hour_roller, alarm_roller_font, 0);
    lv_obj_set_style_radius(hour_roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(hour_roller, 3);
    int32_t alarm_line_h = lv_font_get_line_height(alarm_roller_font);
    lv_obj_set_style_text_line_space(hour_roller, WATCH_BTN_EDIT_HEIGHT - alarm_line_h, LV_PART_MAIN);
    lv_obj_set_height(hour_roller, WATCH_BTN_EDIT_HEIGHT * 3);
    lv_obj_clear_flag(hour_roller, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(hour_roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建分钟滚轮
    minute_roller = lv_roller_create(alarm_edit_base);
    char mins_buf[300] = {0};
    for (int i = 0; i <= 59; i++) {
        char min_str[10];
        sprintf(min_str, "%02d\n", i);
        strcat(mins_buf, min_str);
    }
    mins_buf[strlen(mins_buf)-1] = '\0';
    
    lv_roller_set_options(minute_roller, mins_buf, LV_ROLLER_MODE_INFINITE);
    lv_obj_set_width(minute_roller, 169);
    lv_obj_align(minute_roller, LV_ALIGN_TOP_LEFT, 216, 87);
    
    // 设置滚轮样式
    lv_obj_set_style_border_color(minute_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(minute_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(minute_roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(minute_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(minute_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(minute_roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(minute_roller, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(minute_roller, alarm_roller_font, 0);
    lv_obj_set_style_radius(minute_roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(minute_roller, 3);
    lv_obj_set_style_text_line_space(minute_roller, WATCH_BTN_EDIT_HEIGHT - alarm_line_h, LV_PART_MAIN);
    lv_obj_set_height(minute_roller, WATCH_BTN_EDIT_HEIGHT * 3);
    lv_obj_clear_flag(minute_roller, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(minute_roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建确认按钮
    lv_obj_t *confirm_btn = lv_btn_create(alarm_edit_base);
    lv_obj_add_event_cb(confirm_btn, confirm_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(confirm_btn, 150, 100);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(0x2D47CB), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(confirm_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(confirm_btn, 32, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(confirm_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(confirm_btn, LV_ALIGN_TOP_LEFT, 35, 363);
    lv_obj_t *confirm_btn_label = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_btn_label, "确认");
    lv_obj_set_style_text_font(confirm_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(confirm_btn_label, lv_color_white(), 0);
    lv_obj_center(confirm_btn_label);

    // 创建取消/删除按钮
    lv_obj_t *cancel_btn = lv_btn_create(alarm_edit_base);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(cancel_btn, 150, 100);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cancel_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cancel_btn, 32, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 225, 363);
    lv_obj_t *cancel_btn_label = lv_label_create(cancel_btn);
    // 根据操作类型设置按钮文本
    if (current_op == ALARM_OP_EDIT) {
        lv_label_set_text(cancel_btn_label, "删除");
    } else {
        lv_label_set_text(cancel_btn_label, "取消");
    }
    lv_obj_set_style_text_font(cancel_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(cancel_btn_label, lv_color_white(), 0);
    lv_obj_center(cancel_btn_label);

    // 获取当前时间作为默认值
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    int default_hour = tm_info->tm_hour;
    int default_minute = tm_info->tm_min;

    lv_roller_set_selected(hour_roller, default_hour, LV_ANIM_OFF);
    lv_roller_set_selected(minute_roller, default_minute, LV_ANIM_OFF);

    // 如果是编辑模式，加载现有闹钟数据
    if (current_op == ALARM_OP_EDIT && current_edit_index >= 0 && current_edit_index < alarm_count) {
        alarm_t *alarm = &alarms[current_edit_index];
        lv_roller_set_selected(hour_roller, alarm->hour, LV_ANIM_OFF);
        lv_roller_set_selected(minute_roller, alarm->minute, LV_ANIM_OFF);
        for (int i = 0; i < 7; i++) {
            if (alarm->repeat[i]) {
                lv_obj_set_style_bg_color(day_btns[i], lv_color_hex(0x2D47CB), LV_STATE_DEFAULT);
            }
        }
    }

    // 添加滑动手势处理
    lv_obj_add_event_cb(alarm_edit_base, slide_gesture_edit_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(alarm_edit_base);
}

/**
 * 添加闹钟按钮事件回调
 */
static void add_alarm_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    alarm_edit_create(ALARM_OP_ADD, -1);
}

/**
 * 闹钟项事件回调
 */
static void alarm_item_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    int index = (int)(intptr_t)lv_event_get_user_data(e);
    alarm_edit_create(ALARM_OP_EDIT, index);
}

/**
 * 星期按钮事件回调
 */
static void day_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // int day = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    
    if (lv_obj_has_state(btn, LV_STATE_CHECKED)) {
        lv_obj_clear_state(btn, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), LV_STATE_DEFAULT);
    } else {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2D47CB), LV_STATE_DEFAULT);
    }
}

/**
 * 确认按钮事件回调
 */
static void confirm_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // 获取选中的时间
    int hour = lv_roller_get_selected(hour_roller);
    int minute = lv_roller_get_selected(minute_roller);

    // 获取选中的星期
    uint8_t repeat[7] = {0};
    for (int i = 0; i < 7; i++) {
        if (lv_obj_has_state(day_btns[i], LV_STATE_CHECKED)) {
            repeat[i] = 1;
        }
    }

    if (current_op == ALARM_OP_ADD) {
        // 添加新闹钟
        if (alarm_count < WATCH_MAX_ALARMS) {
            alarms[alarm_count].hour = hour;
            alarms[alarm_count].minute = minute;
            memcpy(alarms[alarm_count].repeat, repeat, sizeof(repeat));
            alarms[alarm_count].enabled = 1;
            alarm_count++;
            ALARM_LOG("add new alarm: %d:%d, repeat:%s,enabled:%d,alarm_count:%d",hour, minute, repeat, alarms[alarm_count-1].enabled, alarm_count);
            // 保存到文件
            alarm_save_to_file();
        }
    } else if (current_op == ALARM_OP_EDIT) {
        // 修改现有闹钟
        if (current_edit_index >= 0 && current_edit_index < alarm_count) {
            alarms[current_edit_index].hour = hour;
            alarms[current_edit_index].minute = minute;
            memcpy(alarms[current_edit_index].repeat, repeat, sizeof(repeat));
            // 保存到文件
            alarm_save_to_file();
            ALARM_LOG("modify alarm: %d:%d, repeat:%s,enabled:%d,alarm_count:%d,current_edit_index%d",hour, minute, repeat, alarms[current_edit_index].enabled, alarm_count,current_edit_index);
        }
    }

    // 退出页面
    if (alarm_edit_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(alarm_edit_base);
        lv_obj_del_async(alarm_edit_base);
        alarm_edit_base = NULL;
    }

    // 重新创建整个列表页面
    if (alarm_list_base != NULL) {
        vw_watch_pop_page(alarm_list_base);
        lv_obj_del_async(alarm_list_base);
        alarm_list_base = NULL;
        alarm_list = NULL;
        no_alarm_label = NULL;
    }
    alarm_list_create();
}

/**
 * 取消/删除按钮事件回调
 */
static void cancel_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // 如果是编辑模式，删除闹钟
    if (current_op == ALARM_OP_EDIT && current_edit_index >= 0 && current_edit_index < alarm_count) {
        // 删除闹钟：将后面的闹钟向前移动
        for (int i = current_edit_index; i < alarm_count - 1; i++) {
            alarms[i] = alarms[i + 1];
        }
        alarm_count--;
        ALARM_LOG("Alarm deleted, count: %d", alarm_count);
        // 保存到文件
        alarm_save_to_file();
    }

    // 退出页面
    if (alarm_edit_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(alarm_edit_base);
        lv_obj_del_async(alarm_edit_base);
        alarm_edit_base = NULL;
    }

    // 重新创建整个列表页面
    if (alarm_list_base != NULL) {
        vw_watch_pop_page(alarm_list_base);
        lv_obj_del_async(alarm_list_base);
        alarm_list_base = NULL;
        alarm_list = NULL;
        no_alarm_label = NULL;
    }
    alarm_list_create();
}

/**
 * 闹钟列表页面 - 滑动手势处理
 */
static void slide_gesture_list_handler(lv_event_t *e)
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
                    // 右滑：退出当前页面
                    if(alarm_list_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(alarm_list_base);
                        lv_obj_del_async(alarm_list_base);
                        alarm_list_base = NULL;
                        alarm_list = NULL;
                        no_alarm_label = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

/**
 * 闹钟编辑页面 - 滑动手势处理
 */
static void slide_gesture_edit_handler(lv_event_t *e)
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
                    // 右滑：返回到闹钟列表页面
                    if(alarm_edit_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(alarm_edit_base);
                        lv_obj_del_async(alarm_edit_base);
                        alarm_edit_base = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

/**
 * 创建闹钟响铃界面
 */
static void alarm_ring_create(int index)
{
    if (alarm_ring_base != NULL) {
        ALARM_LOG("alarm_ring_base already exists, deleting old one");
        lv_obj_del(alarm_ring_base);
        alarm_ring_base = NULL;
    }
    current_ring_index = index;

    if (getchange() == 2) {
        esp32s3_display_on();
        setchange(1);
    }
    setNull_display_timeout_timer();
    ft3168_display_timeout_setup();

    /* 启动闹钟铃声循环播放 */
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
    alarm_sound_start();
#endif

    // 创建闹钟响铃页面
    alarm_ring_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(alarm_ring_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(alarm_ring_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(alarm_ring_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(alarm_ring_base, LV_ALIGN_CENTER, 0, 0);
    // 清除可滚动标志，确保能够捕获手势事件
    lv_obj_clear_flag(alarm_ring_base, LV_OBJ_FLAG_SCROLLABLE);
    // 确保能接收手势事件
    lv_obj_clear_flag(alarm_ring_base, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(alarm_ring_base, LV_OBJ_FLAG_EVENT_BUBBLE);
    // 确保对象可点击
    // lv_obj_clear_flag(alarm_ring_base, LV_OBJ_FLAG_CLICKABLE);
    
    // 将闹钟页面移动到最前面
    lv_obj_move_foreground(alarm_ring_base);

    // 创建闹钟图标
    lv_obj_t *icon = lv_img_create(alarm_ring_base);
    lv_img_set_src(icon, vw_resource_get_img("icon_alarm_ring"));
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 114);
    // lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    // 创建时间显示
    char time_str[32];
    if (is_current_ring_snooze) {
        // 延时闹钟：显示当前系统时间
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        snprintf(time_str, sizeof(time_str), "%02d:%02d", t->tm_hour, t->tm_min);
        ALARM_LOG("Snooze alarm ring at %02d:%02d (current time)", t->tm_hour, t->tm_min);
    } else {
        // 正常闹钟：显示闹钟设置时间
        snprintf(time_str, sizeof(time_str), "%02d:%02d", alarms[index].hour, alarms[index].minute);
        ALARM_LOG("Alarm ring at %02d:%02d (alarm time)", alarms[index].hour, alarms[index].minute);
    }
    lv_obj_t *time_label = lv_label_create(alarm_ring_base);
    lv_label_set_text(time_label, time_str);
    lv_obj_set_style_text_font(time_label, vw_resource_get_font(WATCH_REGULAR_FONT "_56"), 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 238);
    // lv_obj_clear_flag(time_label, LV_OBJ_FLAG_CLICKABLE);

    // 创建延后5分钟按钮
    lv_obj_t *snooze_btn = lv_btn_create(alarm_ring_base);
    lv_obj_add_event_cb(snooze_btn, snooze_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(snooze_btn, 190, 100);
    lv_obj_set_style_bg_color(snooze_btn, lv_color_hex(0x2D47CB), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(snooze_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(snooze_btn, 32, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(snooze_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(snooze_btn, LV_ALIGN_TOP_LEFT, 35, 363);
    lv_obj_t *snooze_btn_label = lv_label_create(snooze_btn);
    lv_label_set_text(snooze_btn_label, "延后5分钟");
    lv_obj_set_style_text_font(snooze_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(snooze_btn_label, lv_color_white(), 0);
    lv_obj_center(snooze_btn_label);
    // 清除事件冒泡标志，防止事件被其他层拦截
    // lv_obj_clear_flag(snooze_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(snooze_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(snooze_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 创建关闭按钮
    lv_obj_t *close_btn = lv_btn_create(alarm_ring_base);
    lv_obj_add_event_cb(close_btn, close_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(close_btn, 110, 100);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(close_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(close_btn, 32, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 265, 363);
    lv_obj_t *close_btn_label = lv_label_create(close_btn);
    lv_label_set_text(close_btn_label, "关闭");
    lv_obj_set_style_text_font(close_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(close_btn_label, lv_color_white(), 0);
    lv_obj_center(close_btn_label);
    // 清除事件冒泡标志，防止事件被其他层拦截
    // lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 添加滑动手势处理
    lv_obj_add_event_cb(alarm_ring_base, slide_gesture_ring_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(alarm_ring_base);
}

/**
 * 延时闹钟定时器回调
 */
static void snooze_timer_cb(lv_timer_t *timer)
{
    // 检查定时器是否仍然有效（防止在回调执行期间定时器被删除）
    if (timer != snooze_timer) {
        return;
    }

    time_t now = time(NULL);

    // 遍历所有延时闹钟，检查是否应该触发
    for (int i = 0; i < MAX_SNOOZE_ALARMS; i++) {
        if (snooze_alarms[i].active && now >= snooze_alarms[i].trigger_time) {
            // 先保存闹钟索引
            int alarm_index = snooze_alarms[i].alarm_index;

            // 设置当前响铃是延时闹钟
            is_current_ring_snooze = true;

            // 清除激活状态
            snooze_alarms[i].active = false;
            snooze_alarms[i].alarm_index = -1;
            snooze_alarms[i].trigger_time = 0;
            snooze_alarms[i].is_snooze = false;

            // 触发延时闹钟
            ALARM_LOG("Snooze alarm triggered for alarm index %d", alarm_index);
            alarm_ring_create(alarm_index);

            // 只触发一个，下次定时器回调再检查其他的
            break;
        }
    }

    // 检查是否还有活跃的延时闹钟，如果没有则停止定时器
    bool has_active = false;
    for (int i = 0; i < MAX_SNOOZE_ALARMS; i++) {
        if (snooze_alarms[i].active) {
            has_active = true;
            break;
        }
    }

    if (!has_active && snooze_timer != NULL) {
        lv_timer_t *timer_to_del = snooze_timer;
        snooze_timer = NULL;
        lv_timer_del(timer_to_del);
        ALARM_LOG("No active snooze alarms, timer stopped");
    }
}

/**
 * 启动延时闹钟
 */
static void start_snooze_alarm(int alarm_index)
{
    // 先检查该闹钟是否已经有延时，如果有则更新时间
    for (int i = 0; i < MAX_SNOOZE_ALARMS; i++) {
        if (snooze_alarms[i].active && snooze_alarms[i].alarm_index == alarm_index) {
            // 更新延时时间
            snooze_alarms[i].trigger_time = time(NULL) + 300; // 5分钟后
            snooze_alarms[i].is_snooze = true;
            ALARM_LOG("Updated snooze alarm for alarm index %d, trigger in 5 minutes (slot %d)", alarm_index, i);

            // 确保定时器存在
            if (snooze_timer == NULL) {
                snooze_timer = lv_timer_create(snooze_timer_cb, 1000, NULL);
            }
            return;
        }
    }

    // 没有找到已有的延时，查找空闲槽位
    int free_slot = -1;
    for (int i = 0; i < MAX_SNOOZE_ALARMS; i++) {
        if (!snooze_alarms[i].active) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        ALARM_LOG("No free slot for snooze alarm");
        return;
    }

    // 设置新的延时闹钟
    snooze_alarms[free_slot].alarm_index = alarm_index;
    snooze_alarms[free_slot].trigger_time = time(NULL) + 300; // 5分钟后
    snooze_alarms[free_slot].active = true;
    snooze_alarms[free_slot].is_snooze = true;

    ALARM_LOG("Starting snooze alarm for alarm index %d, trigger in 5 minutes (slot %d)", alarm_index, free_slot);

    // 创建定时器，每秒检查一次（如果还没有创建）
    if (snooze_timer == NULL) {
        snooze_timer = lv_timer_create(snooze_timer_cb, 1000, NULL);
    }
}

/**
 * 停止所有延时闹钟
 */
static void stop_snooze_alarm(void)
{
    // 清除所有延时闹钟
    for (int i = 0; i < MAX_SNOOZE_ALARMS; i++) {
        snooze_alarms[i].active = false;
        snooze_alarms[i].alarm_index = -1;
        snooze_alarms[i].trigger_time = 0;
    }

    // 删除定时器（如果存在）
    if (snooze_timer != NULL) {
        lv_timer_t *timer_to_del = snooze_timer;
        snooze_timer = NULL;  // 先置空，防止重复删除
        lv_timer_del(timer_to_del);
    }

    ALARM_LOG("Stopped all snooze alarms");
}

/**
 * 延后5分钟按钮事件回调
 */
static void snooze_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
        return;
    }

     // 防止重复处理
    static bool is_processing = false;
    if (is_processing) {
        ALARM_LOG("Already processing snooze, skip");
        return;
    }
    is_processing = true;
    ALARM_LOG("Alarm snooze 5mins button click.");
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
    alarm_sound_stop();
#endif
    int ring_index = current_ring_index;
    // 退出页面
    if (alarm_ring_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(alarm_ring_base);
        lv_obj_del_async(alarm_ring_base);
        alarm_ring_base = NULL;
    }

    // 5分钟后再次响铃
    if (ring_index >= 0 && ring_index < alarm_count) {
        // 启动延时闹钟
        start_snooze_alarm(ring_index);
        ALARM_LOG("Alarm snoozed for 5 minutes");
    }

    is_processing = false;
}

/**
 * 关闭按钮事件回调
 */
static void close_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
        return;
    }
    static bool is_processing = false;
    if (is_processing) {
        ALARM_LOG("Already processing close, skip");
        return;
    }
    is_processing = true;
    ALARM_LOG("Alarm closed button click.");
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
    alarm_sound_stop();
#endif

    // 关闭闹钟
    if (current_ring_index >= 0 && current_ring_index < alarm_count) {
        // 检查是否是仅一次闹钟
        int repeat_count = 0;
        for (int i = 0; i < 7; i++) {
            if (alarms[current_ring_index].repeat[i]) {
                repeat_count++;
            }
        }
        if (repeat_count == 0) {
            // 仅一次闹钟，关闭后禁用
            alarms[current_ring_index].enabled = 0;
            // 保存到文件
            alarm_save_to_file();
            // 更新列表（仅当列表页面仍然存在时）
            if (alarm_list != NULL && alarm_list_base != NULL) {
                update_alarm_list(alarm_list);
            }
        }
        ALARM_LOG("Alarm closed");

        // 只停止当前闹钟的延时（不影响其他闹钟的延时）
        for (int i = 0; i < MAX_SNOOZE_ALARMS; i++) {
            if (snooze_alarms[i].active && snooze_alarms[i].alarm_index == current_ring_index) {
                snooze_alarms[i].active = false;
                snooze_alarms[i].alarm_index = -1;
                snooze_alarms[i].trigger_time = 0;
                snooze_alarms[i].is_snooze = false;
                ALARM_LOG("Stopped snooze for alarm index %d (slot %d)", current_ring_index, i);
                break;
            }
        }
    }

    // 退出页面
    if (alarm_ring_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(alarm_ring_base);
        lv_obj_del_async(alarm_ring_base);
        alarm_ring_base = NULL;
    }
    is_processing = false;
}

/**
 * 闹钟响铃页面 - 滑动手势处理
 */
static void slide_gesture_ring_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};
    static bool ring_is_dragging = false;

    switch(code) {
        case LV_EVENT_PRESSED:
            lv_indev_get_point(lv_indev_active(), &start_point);
            ring_is_dragging = true;
            break;
        
        case LV_EVENT_RELEASED:
            if(ring_is_dragging) {
                lv_point_t end_point;
                lv_indev_get_point(lv_indev_active(), &end_point);
                
                int32_t delta_x = end_point.x - start_point.x;
                int32_t delta_y = end_point.y - start_point.y;

                if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                    static bool is_processing = false;
                    if (is_processing) {
                        ALARM_LOG("Already processing swipe, skip");
                        ring_is_dragging = false;
                        return;
                    }
                    is_processing = true;
                    // 右滑：退出当前页面，功能类似于关闭按钮
                    if (current_ring_index >= 0 && current_ring_index < alarm_count) {
                        // 检查是否是仅一次闹钟
                        int repeat_count = 0;
                        for (int i = 0; i < 7; i++) {
                            if (alarms[current_ring_index].repeat[i]) {
                                repeat_count++;
                            }
                        }
                        if (repeat_count == 0) {
                            // 仅一次闹钟，关闭后禁用
                            alarms[current_ring_index].enabled = 0;
                            // 保存到文件
                            alarm_save_to_file();
                            // 更新列表（仅当列表页面仍然存在时）
                            if (alarm_list != NULL && alarm_list_base != NULL) {
                                update_alarm_list(alarm_list);
                            }
                        }
                        ALARM_LOG("Alarm closed via swipe");
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
                        alarm_sound_stop();
#endif

                        // 只停止当前闹钟的延时（不影响其他闹钟的延时）
                        for (int i = 0; i < MAX_SNOOZE_ALARMS; i++) {
                            if (snooze_alarms[i].active && snooze_alarms[i].alarm_index == current_ring_index) {
                                snooze_alarms[i].active = false;
                                snooze_alarms[i].alarm_index = -1;
                                snooze_alarms[i].trigger_time = 0;
                                snooze_alarms[i].is_snooze = false;
                                ALARM_LOG("Stopped snooze for alarm index %d (slot %d)", current_ring_index, i);
                                break;
                            }
                        }
                    }

                    if(alarm_ring_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(alarm_ring_base);
                        lv_obj_del_async(alarm_ring_base);
                        alarm_ring_base = NULL;
                    }
                    is_processing = false;
                }
                ring_is_dragging = false;
                start_point.x = 0;
                start_point.y = 0;
            }
            break;
            
        default:
            break;
    }
}

void alarm_app_click_callback(lv_event_t *e)
{
    ALARM_LOG("alarm_app_click_callback");
    alarm_list_create();
    // alarm_ring_create(1);
}

// 模拟闹钟响铃（用于测试）
void alarm_trigger(int index)
{
    if (index >= 0 && index < alarm_count) {
        alarm_ring_create(index);
    }
}

/**
 * 保存闹钟数据到文件
 */
void alarm_save_to_file(void)
{
    int fd = open(ALARM_DATA_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ALARM_LOG("Failed to open alarm data file for writing");
        return;
    }

    // 先写入闹钟数量
    ssize_t ret = write(fd, &alarm_count, sizeof(int));
    if (ret != sizeof(int)) {
        ALARM_LOG("Failed to write alarm count");
        close(fd);
        return;
    }

    // 写入所有闹钟数据
    ret = write(fd, alarms, sizeof(alarm_t) * alarm_count);
    if (ret != sizeof(alarm_t) * alarm_count) {
        ALARM_LOG("Failed to write alarm data");
    } else {
        ALARM_LOG("Saved %d alarms to file", alarm_count);
    }

    close(fd);
}

/**
 * 从文件加载闹钟数据
 */
void alarm_load_from_file(void)
{
    int fd = open(ALARM_DATA_FILE, O_RDONLY);
    if (fd < 0) {
        ALARM_LOG("Alarm data file not found, using default");
        alarm_count = 0;
        return;
    }

    // 读取闹钟数量
    ssize_t ret = read(fd, &alarm_count, sizeof(int));
    if (ret != sizeof(int) || alarm_count < 0 || alarm_count > WATCH_MAX_ALARMS) {
        ALARM_LOG("Invalid alarm count, using default");
        alarm_count = 0;
        close(fd);
        return;
    }

    // 读取所有闹钟数据
    ret = read(fd, alarms, sizeof(alarm_t) * alarm_count);
    if (ret != sizeof(alarm_t) * alarm_count) {
        ALARM_LOG("Failed to read alarm data");
        alarm_count = 0;
    } else {
        ALARM_LOG("Loaded %d alarms from file", alarm_count);
    }

    close(fd);
}

/**
 * 闹钟检测定时器回调
 */
static void alarm_check_timer_cb(lv_timer_t *timer)
{
    alarm_check_trigger();
}

/**
 * 检查闹钟触发
 */
void alarm_check_trigger(void)
{
    // 如果响铃界面已经存在，不再触发新的响铃
    if (alarm_ring_base != NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int current_minute = t->tm_hour * 60 + t->tm_min;
    int current_weekday = t->tm_wday; // 0=周日, 1=周一, ..., 6=周六

    for (int i = 0; i < alarm_count; i++) {
        if (!alarms[i].enabled) continue;

        int alarm_minute = alarms[i].hour * 60 + alarms[i].minute;

        // 检查时间是否匹配
        if (current_minute == alarm_minute) {
            // 检查是否在60秒内已经触发过（防止重复触发）
            if (now - last_trigger_time[i] < 60) {
                continue;
            }

            // 检查重复模式是否匹配
            bool should_trigger = false;

            // 检查是否有重复日期设置
            int repeat_count = 0;
            for (int j = 0; j < 7; j++) {
                if (alarms[i].repeat[j]) repeat_count++;
            }

            if (repeat_count == 0) {
                // 仅一次
                should_trigger = true;
                // 一次性闹钟触发后禁用
                alarms[i].enabled = 0;
                // 保存到文件
                alarm_save_to_file();
            } else {
                // 检查今天是否在重复日期中
                // tm_wday: 0=周日, 1=周一, ..., 6=周六
                // repeat: 0=周一, 1=周二, ..., 6=周日
                int repeat_index = (current_weekday == 0) ? 6 : (current_weekday - 1);
                should_trigger = alarms[i].repeat[repeat_index];
            }

            // 如果应该触发，显示响铃界面
            if (should_trigger) {
                // 记录触发时间
                last_trigger_time[i] = now;

                // 设置当前响铃不是延时闹钟
                is_current_ring_snooze = false;

                alarm_ring_create(i);
                // 只触发一个闹钟，避免同时触发多个
                break;
            }
        }
    }
}

/**
 * 获取距离下一个闹钟的秒数
 * 用于设置休眠唤醒时间
 */
int64_t getSetTimer(void)
{
    time_t now = time(NULL);
    struct tm *current_tm = localtime(&now);
    int current_wday = current_tm->tm_wday; // 0=周日, 1=周一, ..., 6=周六
    int current_hour = current_tm->tm_hour;
    int current_min = current_tm->tm_min;
    int current_sec = current_tm->tm_sec;
    int current_total_sec = current_hour * 3600 + current_min * 60 + current_sec;

    int64_t min_delta = INT64_MAX; // 初始化为最大值

    for (int i = 0; i < alarm_count; i++) {
        if (!alarms[i].enabled) continue;

        int alarm_total_sec = alarms[i].hour * 3600 + alarms[i].minute * 60;
        int delta_sec = alarm_total_sec - current_total_sec;

        // 检查重复模式
        int repeat_count = 0;
        for (int j = 0; j < 7; j++) {
            if (alarms[i].repeat[j]) repeat_count++;
        }

        bool should_trigger = false;

        if (repeat_count == 0) {
            // 一次性闹钟，只需时间匹配且未过期
            should_trigger = (delta_sec >= 0);
        } else {
            // 检查今天是否在重复日期中
            int repeat_index = (current_wday == 0) ? 6 : (current_wday - 1);
            should_trigger = alarms[i].repeat[repeat_index];
        }

        if (!should_trigger) {
            // 如果今天不触发，考虑明天
            delta_sec += 24 * 3600; // 增加一天的秒数
            // 重新检查重复模式
            if (repeat_count > 0) {
                int next_wday = (current_wday + 1) % 7;
                int next_repeat_index = (next_wday == 0) ? 6 : (next_wday - 1);
                should_trigger = alarms[i].repeat[next_repeat_index];
            } else {
                should_trigger = true; // 一次性闹钟第二天总是触发
            }
        }

        if (should_trigger && delta_sec < min_delta && delta_sec >= 0) {
            min_delta = delta_sec;
        }
    }

    return (min_delta == INT64_MAX) ? -1 : min_delta;
}

/**
 * 初始化闹钟服务（系统启动时调用）
 */
void alarm_service_init(void)
{
    if (alarm_service_initialized) {
        ALARM_LOG("Alarm service already initialized");
        return;
    }

    // 从文件加载闹钟数据
    alarm_load_from_file();

    // 创建闹钟检测定时器（每秒检测一次）
    alarm_check_timer = lv_timer_create(alarm_check_timer_cb, 1000, NULL);
    if (alarm_check_timer == NULL) {
        ALARM_LOG("Failed to create alarm check timer");
        return;
    }

    alarm_service_initialized = true;
    ALARM_LOG("Alarm service initialized");
}

/**
 * 反初始化闹钟服务
 */
void alarm_service_deinit(void)
{
    if (!alarm_service_initialized) {
        return;
    }

    // 删除定时器
    if (alarm_check_timer != NULL) {
        lv_timer_del(alarm_check_timer);
        alarm_check_timer = NULL;
    }

    // 停止延时闹钟
    stop_snooze_alarm();

    // 保存闹钟数据
    alarm_save_to_file();

    alarm_service_initialized = false;
    ALARM_LOG("Alarm service deinitialized");
}



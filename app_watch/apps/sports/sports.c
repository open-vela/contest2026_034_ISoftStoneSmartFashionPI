#include "sports.h"
#include "../common/watch_pages.h"
#include "../common/sensor_data.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define SPORTS_LOG(fmt, ...) printf("[SPORTS] " fmt "\n", ##__VA_ARGS__)
#else
#  define SPORTS_LOG(fmt, ...)
#endif


// 运动数据文件路径
#define SPORT_DATA_FILE "/mnt/sd/sport.dat"

/* UI对象 */
static lv_obj_t* sport_main_base = NULL;
static lv_obj_t* sport_go_base = NULL;
static lv_obj_t* sport_set_base = NULL;
static lv_obj_t* sport_set_edit_base = NULL;
static lv_obj_t* sport_running_base = NULL;
static lv_obj_t* sport_stop_base = NULL;
static lv_obj_t* sport_end_base = NULL;

/* 运动类型枚举 */
typedef enum {
    SPORT_TYPE_OUTDOOR_RUNNING = 0,
    SPORT_TYPE_OUTDOOR_WALKING
} sport_type_e;

/* 设置项枚举 */
typedef enum {
    SETTING_TARGET_TIME = 0,
    SETTING_TARGET_DISTANCE,
    SETTING_TARGET_STEPS
} setting_type_e;


/* 运动设置结构体 - 针对每种运动类型单独保存 */
typedef struct {
    int target_time;      // 目标时间（分钟）
    int target_distance;  // 目标距离（公里，实际存储为0.5公里的倍数，如2表示1公里）
    int target_steps;     // 目标步数
    int setting_type;     // 当前选择的设置类型：0-目标时间，1-目标距离，2-目标步数
} sport_settings_t;

/* 运动数据结构体 */
typedef struct {
    sport_settings_t running_settings;  // 跑步设置
    sport_settings_t walking_settings;  // 步行设置
    int current_time;                   // 当前时间（秒）
    float current_distance;             // 当前距离（米）
    int current_steps;                  // 当前步数
    float current_speed;                // 当前速度（米/秒）
    int sport_type;                     // 运动类型：0-户外跑步，1-户外步行
} sport_data_t;

/* 全局运动数据 */
static sport_data_t sport_data = {
    .running_settings = {
        .target_time = 15,      // 跑步默认15分钟
        .target_distance = 4,   // 跑步默认2公里 (4 * 0.5)
        .target_steps = 5000,   // 跑步默认5000步
        .setting_type = SETTING_TARGET_STEPS  // 默认使用目标步数
    },
    .walking_settings = {
        .target_time = 30,      // 步行默认30分钟
        .target_distance = 4,   // 步行默认2公里 (4 * 0.5)
        .target_steps = 6000,   // 步行默认6000步
        .setting_type = SETTING_TARGET_STEPS  // 默认使用目标步数
    },
    .current_time = 0,
    .current_distance = 0,
    .current_steps = 0,
    .current_speed = 8.0,
    .sport_type = 0
};

/* 定时器 */
static lv_timer_t* sport_timer = NULL;

/* 函数声明 */
static void slide_gesture_sport_main_handler(lv_event_t *e);
static void slide_gesture_sport_go_handler(lv_event_t *e);
static void slide_gesture_sport_set_handler(lv_event_t *e);
static void slide_gesture_sport_set_edit_handler(lv_event_t *e);
static void slide_gesture_sport_running_handler(lv_event_t *e);
static void slide_gesture_sport_stop_handler(lv_event_t *e);
static void slide_gesture_sport_end_handler(lv_event_t *e);
static void sport_type_click_handler(lv_event_t *e);
static void go_button_click_handler(lv_event_t *e);
static void setting_button_click_handler(lv_event_t *e);
static void setting_item_click_handler(lv_event_t *e);
static void confirm_button_click_handler(lv_event_t *e);
static void cancel_button_click_handler(lv_event_t *e);
static void continue_button_click_handler(lv_event_t *e);
static void finish_button_click_handler(lv_event_t *e);
static void sport_timer_handler(lv_timer_t *timer);

static void sport_running_deleted_cb(lv_event_t *e)
{
    if (sport_timer != NULL) {
        lv_timer_del(sport_timer);
        sport_timer = NULL;
    }
    sport_running_base = NULL;
    sensor_step_counter_pause();
}

static void sport_page_deleted_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    if (obj == sport_main_base) sport_main_base = NULL;
    else if (obj == sport_go_base) sport_go_base = NULL;
    else if (obj == sport_set_base) sport_set_base = NULL;
    else if (obj == sport_set_edit_base) sport_set_edit_base = NULL;
    else if (obj == sport_stop_base) {
        sport_stop_base = NULL;
    }
    else if (obj == sport_end_base) sport_end_base = NULL;
}

/* 文件读写函数 */
static int sport_save_settings(void);
static int sport_load_settings(void);

/* 运动结束页创建函数 */
static void sport_end_page_create(void);

/**
 * 创建运动选择页
 */
static void sport_app_create(void)
{
    SPORTS_LOG("sport app clicked");
    
    // 创建运动选择页面
    sport_main_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sport_main_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sport_main_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sport_main_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sport_main_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sport_main_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(sport_main_base);
    lv_label_set_text(title_label, "运动");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    // 创建设置按钮样式
    static lv_style_t btn_style;
    static bool style_initialized = false;
    if (!style_initialized) {
        lv_style_init(&btn_style);
        lv_style_set_bg_opa(&btn_style, LV_OPA_10);
        lv_style_set_bg_color(&btn_style, lv_color_hex(0xFFFFFF));
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
        lv_style_set_bg_opa(&pressed_style, LV_OPA_20);
        lv_style_set_bg_color(&pressed_style, lv_color_hex(0xFFFFFF));
        pressed_style_initialized = true;
    }

    // 创建户外跑步按钮
    lv_obj_t *running_btn = lv_obj_create(sport_main_base);
    lv_obj_set_size(running_btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(running_btn, &btn_style, 0);
    lv_obj_add_style(running_btn, &pressed_style, LV_STATE_PRESSED);
    lv_obj_align(running_btn, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_clear_flag(running_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(running_btn, LV_OBJ_FLAG_EVENT_BUBBLE);  // 添加事件冒泡标志
    
    // 添加图标
    lv_obj_t *running_img = lv_img_create(running_btn);
    lv_img_set_src(running_img, vw_resource_get_img("icon_sport_outdoor_running"));
    lv_obj_set_pos(running_img, 18, (WATCH_BTN_HEIGHT - 64) / 2);
    lv_obj_clear_flag(running_img, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加文字
    lv_obj_t *running_label = lv_label_create(running_btn);
    lv_label_set_text(running_label, "户外跑步");
    lv_obj_set_style_text_color(running_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(running_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_pos(running_label, 18 + 70 + 24, (WATCH_BTN_HEIGHT - 32) / 2);
    lv_obj_clear_flag(running_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(running_btn, sport_type_click_handler, LV_EVENT_CLICKED, (void*)SPORT_TYPE_OUTDOOR_RUNNING);
    
    // 创建户外步行按钮
    lv_obj_t *walking_btn = lv_obj_create(sport_main_base);
    lv_obj_set_size(walking_btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(walking_btn, &btn_style, 0);
    lv_obj_add_style(walking_btn, &pressed_style, LV_STATE_PRESSED);
    lv_obj_align(walking_btn, LV_ALIGN_TOP_MID, 0, 80 + WATCH_BTN_HEIGHT + 16);
    lv_obj_clear_flag(walking_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(walking_btn, LV_OBJ_FLAG_EVENT_BUBBLE);  // 添加事件冒泡标志
    
    // 添加图标
    lv_obj_t *walking_img = lv_img_create(walking_btn);
    lv_img_set_src(walking_img, vw_resource_get_img("icon_sport_outdoor_walking"));
    lv_obj_set_pos(walking_img, 18, (WATCH_BTN_HEIGHT - 64) / 2);
    lv_obj_clear_flag(walking_img, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加文字
    lv_obj_t *walking_label = lv_label_create(walking_btn);
    lv_label_set_text(walking_label, "户外步行");
    lv_obj_set_style_text_color(walking_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(walking_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_pos(walking_label, 18 + 70 + 24, (WATCH_BTN_HEIGHT - 32) / 2);
    lv_obj_clear_flag(walking_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(walking_btn, sport_type_click_handler, LV_EVENT_CLICKED, (void*)SPORT_TYPE_OUTDOOR_WALKING);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sport_main_base, slide_gesture_sport_main_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sport_main_base, sport_page_deleted_cb, LV_EVENT_DELETE, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(sport_main_base);

    SPORTS_LOG("sport: create complete");
}

/**
 * 创建运动详情页
 */
static void sport_go_create(int sport_type)
{
    // 创建运动详情页面
    sport_go_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sport_go_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sport_go_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sport_go_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sport_go_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sport_go_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(sport_go_base);
    lv_label_set_text(title_label, sport_type == SPORT_TYPE_OUTDOOR_RUNNING ? "户外跑步" : "户外步行");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 26);

    // 创建Go图标
    lv_obj_t *go_img = lv_img_create(sport_go_base);
    lv_img_set_src(go_img, vw_resource_get_img("icon_sport_go"));
    lv_obj_clear_flag(go_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(go_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(go_img, LV_OBJ_FLAG_CLICKABLE);  // 添加可点击标志
    lv_obj_align(go_img, LV_ALIGN_TOP_MID, 0, 136);

    // 创建设置按钮样式
    static lv_style_t btn_style;
    static bool style_initialized = false;
    if (!style_initialized) {
        lv_style_init(&btn_style);
        lv_style_set_bg_opa(&btn_style, LV_OPA_10);
        lv_style_set_bg_color(&btn_style, lv_color_hex(0xFFFFFF));
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
        lv_style_set_bg_opa(&pressed_style, LV_OPA_20);
        lv_style_set_bg_color(&pressed_style, lv_color_hex(0xFFFFFF));
        pressed_style_initialized = true;
    }

    // 创建设置按钮
    lv_obj_t *setting_btn = lv_obj_create(sport_go_base);
    lv_obj_set_size(setting_btn, 180, 100);
    lv_obj_add_style(setting_btn, &btn_style, 0);
    lv_obj_add_style(setting_btn, &pressed_style, LV_STATE_PRESSED);
    lv_obj_align(setting_btn, LV_ALIGN_TOP_MID, 0, 362);
    lv_obj_clear_flag(setting_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(setting_btn, LV_OBJ_FLAG_EVENT_BUBBLE);  // 添加事件冒泡标志
    
    // 添加文字
    lv_obj_t *setting_label = lv_label_create(setting_btn);
    lv_label_set_text(setting_label, "设置");
    lv_obj_set_style_text_color(setting_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(setting_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_align(setting_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(setting_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(setting_btn, setting_button_click_handler, LV_EVENT_CLICKED, NULL);

    // 添加GO按钮点击事件到Go图标
    lv_obj_add_event_cb(go_img, go_button_click_handler, LV_EVENT_CLICKED, (void*)(intptr_t)sport_type);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sport_go_base, slide_gesture_sport_go_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sport_go_base, sport_page_deleted_cb, LV_EVENT_DELETE, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(sport_go_base);

    SPORTS_LOG("sport_go: create complete");
}

/**
 * 创建运动设置页
 */
static void sport_set_create(void)
{
    // 创建运动设置页面
    sport_set_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sport_set_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sport_set_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sport_set_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sport_set_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sport_set_base, LV_ALIGN_CENTER, 0, 0);

    // 创建设置项样式
    static lv_style_t item_style;
    static bool style_initialized = false;
    if (!style_initialized) {
        lv_style_init(&item_style);
        lv_style_set_bg_opa(&item_style, LV_OPA_10);
        lv_style_set_bg_color(&item_style, lv_color_hex(0xFFFFFF));
        lv_style_set_text_color(&item_style, lv_color_white());
        lv_style_set_text_font(&item_style, vw_resource_get_font(WATCH_REGULAR_FONT "_32"));
        lv_style_set_border_width(&item_style, 0);
        lv_style_set_radius(&item_style, 32);
        lv_style_set_pad_all(&item_style, 0);
        lv_style_set_outline_width(&item_style, 0);
        /* 禁用变换效果，防止按钮按下时偏移 */
        lv_style_set_transform_width(&item_style, 0);
        lv_style_set_transform_height(&item_style, 0);
        lv_style_set_translate_x(&item_style, 0);
        lv_style_set_translate_y(&item_style, 0);
        style_initialized = true;
    }

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
        lv_style_set_bg_opa(&pressed_style, LV_OPA_20);
        lv_style_set_bg_color(&pressed_style, lv_color_hex(0xFFFFFF));
        pressed_style_initialized = true;
    }

    // 创建目标时间设置项
    lv_obj_t *time_item = lv_obj_create(sport_set_base);
    lv_obj_set_size(time_item, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(time_item, &item_style, 0);
    lv_obj_add_style(time_item, &pressed_style, LV_STATE_PRESSED);
    lv_obj_align(time_item, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_clear_flag(time_item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(time_item, LV_OBJ_FLAG_EVENT_BUBBLE);  // 添加事件冒泡标志
    
    // 添加文字
    lv_obj_t *time_label = lv_label_create(time_item);
    lv_label_set_text(time_label, "目标时间");
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(time_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_pos(time_label, 18, (WATCH_BTN_HEIGHT - 32) / 2);
    lv_obj_clear_flag(time_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加箭头图标
    lv_obj_t *time_arrow = lv_img_create(time_item);
    lv_img_set_src(time_arrow, vw_resource_get_img("icon_sport_right_arrow"));
    lv_obj_set_pos(time_arrow, WATCH_BTN_WIDTH - 18 - 44, (WATCH_BTN_HEIGHT - 44) / 2);
    // lv_obj_add_flag(time_arrow, LV_OBJ_FLAG_CLICKABLE);  // 添加可点击标志
    lv_obj_clear_flag(time_arrow, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(time_item, setting_item_click_handler, LV_EVENT_CLICKED, (void*)(intptr_t)SETTING_TARGET_TIME);

    // 创建目标距离设置项
    lv_obj_t *distance_item = lv_obj_create(sport_set_base);
    lv_obj_set_size(distance_item, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(distance_item, &item_style, 0);
    lv_obj_add_style(distance_item, &pressed_style, LV_STATE_PRESSED);
    lv_obj_align(distance_item, LV_ALIGN_TOP_MID, 0, 40 + WATCH_BTN_HEIGHT + 16);
    lv_obj_clear_flag(distance_item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(distance_item, LV_OBJ_FLAG_EVENT_BUBBLE);  // 添加事件冒泡标志
    
    // 添加文字
    lv_obj_t *distance_label = lv_label_create(distance_item);
    lv_label_set_text(distance_label, "目标距离");
    lv_obj_set_style_text_color(distance_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(distance_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_pos(distance_label, 18, (WATCH_BTN_HEIGHT - 32) / 2);
    lv_obj_clear_flag(distance_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加箭头图标
    lv_obj_t *distance_arrow = lv_img_create(distance_item);
    lv_img_set_src(distance_arrow, vw_resource_get_img("icon_sport_right_arrow"));
    lv_obj_set_pos(distance_arrow, WATCH_BTN_WIDTH - 18 - 44, (WATCH_BTN_HEIGHT - 44) / 2);
    // lv_obj_add_flag(distance_arrow, LV_OBJ_FLAG_CLICKABLE);  // 添加可点击标志
    lv_obj_clear_flag(distance_arrow, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(distance_item, setting_item_click_handler, LV_EVENT_CLICKED, (void*)(intptr_t)SETTING_TARGET_DISTANCE);

    // 创建目标步数设置项
    lv_obj_t *steps_item = lv_obj_create(sport_set_base);
    lv_obj_set_size(steps_item, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(steps_item, &item_style, 0);
    lv_obj_add_style(steps_item, &pressed_style, LV_STATE_PRESSED);
    lv_obj_align(steps_item, LV_ALIGN_TOP_MID, 0, 40 + (WATCH_BTN_HEIGHT + 16) * 2);
    lv_obj_clear_flag(steps_item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(steps_item, LV_OBJ_FLAG_EVENT_BUBBLE);  // 添加事件冒泡标志
    
    // 添加文字
    lv_obj_t *steps_label = lv_label_create(steps_item);
    lv_label_set_text(steps_label, "目标步数");
    lv_obj_set_style_text_color(steps_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(steps_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_pos(steps_label, 18, (WATCH_BTN_HEIGHT - 32) / 2);
    lv_obj_clear_flag(steps_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加箭头图标
    lv_obj_t *steps_arrow = lv_img_create(steps_item);
    lv_img_set_src(steps_arrow, vw_resource_get_img("icon_sport_right_arrow"));
    lv_obj_set_pos(steps_arrow, WATCH_BTN_WIDTH - 18 - 44, (WATCH_BTN_HEIGHT - 44) / 2);
    // lv_obj_add_flag(steps_arrow, LV_OBJ_FLAG_CLICKABLE);  // 添加可点击标志
    lv_obj_clear_flag(steps_arrow, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(steps_item, setting_item_click_handler, LV_EVENT_CLICKED, (void*)(intptr_t)SETTING_TARGET_STEPS);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sport_set_base, slide_gesture_sport_set_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sport_set_base, sport_page_deleted_cb, LV_EVENT_DELETE, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(sport_set_base);

    SPORTS_LOG("sport_set: create complete");
}

/**
 * 创建运动设置详情页
 */
static void sport_set_edit_create(int setting_type, int sport_type)
{
    // 创建运动设置详情页面
    sport_set_edit_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sport_set_edit_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sport_set_edit_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sport_set_edit_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sport_set_edit_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sport_set_edit_base, LV_ALIGN_CENTER, 0, 0);

    // 创建滚动轴
    lv_obj_t *roller = lv_roller_create(sport_set_edit_base);
    lv_obj_set_width(roller, WATCH_BTN_WIDTH);
    lv_obj_align(roller, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_set_style_border_color(roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *roller_font = vw_resource_get_font(WATCH_REGULAR_FONT "_36");
    lv_obj_set_style_text_font(roller, roller_font, 0);
    lv_obj_set_style_radius(roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(roller, 3);
    int32_t line_h = lv_font_get_line_height(roller_font);
    lv_obj_set_style_text_line_space(roller, WATCH_BTN_HEIGHT - line_h, LV_PART_MAIN);
    lv_obj_set_height(roller, WATCH_BTN_HEIGHT * 3);
    lv_obj_clear_flag(roller, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 根据设置类型和运动类型设置滚动轴内容
    char options[1024] = {0};
    int current_value = 0;
    int start_value = 0;
    int end_value = 0;
    int step = 0;
    int default_value = 0;

    // 获取当前运动类型的设置
    sport_settings_t *settings = (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) ?
                                  &sport_data.running_settings : &sport_data.walking_settings;

    switch (setting_type) {
        case SETTING_TARGET_TIME:
            if (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) {
                // 跑步: 5-30分钟,步长5分钟
                start_value = 5;
                end_value = 30;
                step = 5;
                default_value = 15;
            } else {
                // 步行: 10-60分钟,步长5分钟
                start_value = 10;
                end_value = 60;
                step = 5;
                default_value = 30;
            }
            current_value = settings->target_time;
            // 如果当前值不在范围内,使用默认值
            if (current_value < start_value || current_value > end_value) {
                current_value = default_value;
            }
            for (int i = start_value; i <= end_value; i += step) {
                if (i > start_value) strcat(options, "\n");
                char buf[32];
                sprintf(buf, "%d分钟", i);
                strcat(options, buf);
            }
            break;
        case SETTING_TARGET_DISTANCE:
            if (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) {
                // 跑步: 1-5公里,步长0.5公里
                start_value = 2;  // 2 * 0.5 = 1公里
                end_value = 10;   // 10 * 0.5 = 5公里
                step = 1;
                default_value = 4;  // 4 * 0.5 = 2公里
            } else {
                // 步行: 0.5-5公里,步长0.5公里
                start_value = 1;   // 1 * 0.5 = 0.5公里
                end_value = 10;    // 10 * 0.5 = 5公里
                step = 1;
                default_value = 4;  // 4 * 0.5 = 2公里
            }
            current_value = settings->target_distance;
            // 如果当前值不在范围内,使用默认值
            if (current_value < start_value || current_value > end_value) {
                current_value = default_value;
            }
            for (int i = start_value; i <= end_value; i += step) {
                if (i > start_value) strcat(options, "\n");
                char buf[32];
                sprintf(buf, "%.1f公里", i * 0.5f);
                strcat(options, buf);
            }
            break;
        case SETTING_TARGET_STEPS:
            if (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) {
                // 跑步: 3000-10000步,步长1000步
                start_value = 3000;
                end_value = 10000;
                step = 1000;
                default_value = 5000;
            } else {
                // 步行: 2000-15000步,步长1000步
                start_value = 2000;
                end_value = 15000;
                step = 1000;
                default_value = 6000;
            }
            current_value = settings->target_steps;
            // 如果当前值不在范围内,使用默认值
            if (current_value < start_value || current_value > end_value) {
                current_value = default_value;
            }
            for (int i = start_value; i <= end_value; i += step) {
                if (i > start_value) strcat(options, "\n");
                char buf[32];
                sprintf(buf, "%d步", i);
                strcat(options, buf);
            }
            break;
    }
    
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_INFINITE);
    
    // 设置当前值
    int index = (current_value - start_value) / step;
    lv_roller_set_selected(roller, index, LV_ANIM_OFF);


    // 创建设置按钮样式
    static lv_style_t btn_style;
    static bool style_initialized = false;
    if (!style_initialized) {
        lv_style_init(&btn_style);
        lv_style_set_bg_opa(&btn_style, LV_OPA_100);
        lv_style_set_bg_color(&btn_style, lv_color_hex(0x2D47CB));
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

    // 创建确认按钮
    lv_obj_t *confirm_btn = lv_obj_create(sport_set_edit_base);
    lv_obj_set_size(confirm_btn, 150, 80);
    lv_obj_add_style(confirm_btn, &btn_style, 0);
    lv_obj_align(confirm_btn, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_clear_flag(confirm_btn, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加文字
    lv_obj_t *confirm_label = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_label, "确认");
    lv_obj_set_style_text_color(confirm_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(confirm_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_align(confirm_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(confirm_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件 - 传递设置类型和运动类型
    int user_data = setting_type | (sport_type << 8);
    lv_obj_add_event_cb(confirm_btn, confirm_button_click_handler, LV_EVENT_CLICKED, (void*)(intptr_t)user_data);

    // 创建取消按钮
    lv_obj_t *cancel_btn = lv_obj_create(sport_set_edit_base);
    lv_obj_set_size(cancel_btn, 150, 80);
    lv_obj_add_style(cancel_btn, &btn_style, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x1A1A1A), 0);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_clear_flag(cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加文字
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(cancel_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_align(cancel_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(cancel_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(cancel_btn, cancel_button_click_handler, LV_EVENT_CLICKED, NULL);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sport_set_edit_base, slide_gesture_sport_set_edit_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sport_set_edit_base, sport_page_deleted_cb, LV_EVENT_DELETE, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(sport_set_edit_base);

    SPORTS_LOG("sport_set_edit: create complete");
}

/**
 * 创建运动页
 */
static void sport_running_create(int sport_type)
{
    // 创建运动页面
    sport_running_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sport_running_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sport_running_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sport_running_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sport_running_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sport_running_base, LV_ALIGN_CENTER, 0, 0);

    // 获取当前运动类型的设置
    sport_settings_t *settings = (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) ?
                                  &sport_data.running_settings : &sport_data.walking_settings;

    int setting_type = settings->setting_type;
    SPORTS_LOG("sport_running: sport_type=%d, setting_type=%d", sport_type, setting_type);
    SPORTS_LOG("Loaded settings: target_time=%d, target_distance=%d, target_steps=%d",
           settings->target_time, settings->target_distance, settings->target_steps);

    // 根据设置类型创建不同的UI
    // 顶部标题和显示
    lv_obj_t *top_title_label = lv_label_create(sport_running_base);
    lv_obj_t *top_label = lv_label_create(sport_running_base);
    lv_obj_t *top_img = lv_img_create(sport_running_base);

    if (setting_type == SETTING_TARGET_TIME) {
        // 目标时间模式：顶部显示距离
        lv_label_set_text(top_title_label, "距离");
        lv_label_set_text(top_label, "0.0KM");
        lv_img_set_src(top_img, vw_resource_get_img("icon_sport_distance"));
    } else {
        // 目标步数/距离模式：顶部显示用时
        lv_label_set_text(top_title_label, "用时");
        lv_label_set_text(top_label, "00:00:00");
        lv_img_set_src(top_img, vw_resource_get_img("icon_sport_use_time"));
    }

    lv_obj_set_style_text_font(top_title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(top_title_label, lv_color_white(), 0);
    lv_obj_align(top_title_label, LV_ALIGN_TOP_MID, 20, 31);

    lv_obj_set_style_text_font(top_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(top_label, lv_color_white(), 0);
    lv_obj_align(top_label, LV_ALIGN_TOP_MID, 0, 69);
    lv_obj_align_to(top_img, top_title_label, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    // 创建环形进度条
    lv_obj_t *arc = lv_arc_create(sport_running_base);
    lv_obj_set_size(arc, 230, 230);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 131);

    // 根据设置类型设置arc的范围
    int arc_max_value = 100;
    if (setting_type == SETTING_TARGET_STEPS) {
        arc_max_value = settings->target_steps;
    } else if (setting_type == SETTING_TARGET_DISTANCE) {
        arc_max_value = settings->target_distance * 500;  // 转换为米
    } else if (setting_type == SETTING_TARGET_TIME) {
        arc_max_value = settings->target_time * 60;  // 转换为秒
    }

    lv_arc_set_range(arc, 0, arc_max_value);
    lv_arc_set_value(arc, 0);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x2D47CB), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 19, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 19, LV_PART_INDICATOR);

    // 禁用arc的拖动功能和移除旋钮
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);

    // 圆弧内标题和显示
    lv_obj_t *arc_title_label = lv_label_create(sport_running_base);
    lv_obj_t *arc_value_label = lv_label_create(sport_running_base);
    lv_obj_t *target_label = lv_label_create(sport_running_base);

    if (setting_type == SETTING_TARGET_STEPS) {
        // 目标步数模式：显示步数
        lv_label_set_text(arc_title_label, "步数");
        lv_label_set_text(arc_value_label, "0");
        lv_label_set_text_fmt(target_label, "/%d", settings->target_steps);
    } else if (setting_type == SETTING_TARGET_DISTANCE) {
        // 目标距离模式：显示距离
        lv_label_set_text(arc_title_label, "距离");
        lv_label_set_text(arc_value_label, "0.0KM");
        lv_label_set_text_fmt(target_label, "/%.1fKM", settings->target_distance * 0.5f);
    } else if (setting_type == SETTING_TARGET_TIME) {
        // 目标时间模式：显示分钟
        lv_label_set_text(arc_title_label, "分钟");
        lv_label_set_text(arc_value_label, "0");
        lv_label_set_text_fmt(target_label, "/%d", settings->target_time);
    }

    lv_obj_set_style_text_font(arc_title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_set_style_text_color(arc_title_label, lv_color_white(), 0);
    lv_obj_align_to(arc_title_label, arc, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_set_style_text_font(arc_value_label, vw_resource_get_font(WATCH_REGULAR_FONT "_48"), 0);
    lv_obj_set_style_text_color(arc_value_label, lv_color_white(), 0);
    lv_obj_align_to(arc_value_label, arc, LV_ALIGN_TOP_MID, 0, 82);
    // lv_obj_center(arc_value_label);

    lv_obj_set_style_text_font(target_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(target_label, lv_color_white(), 0);
    lv_obj_align_to(target_label, arc_value_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 3);

    // 左侧图标和显示
    lv_obj_t *left_img = lv_img_create(sport_running_base);
    lv_obj_t *left_title_label = lv_label_create(sport_running_base);
    lv_obj_t *left_label = lv_label_create(sport_running_base);

    if (setting_type == SETTING_TARGET_STEPS) {
        // 目标步数模式：左侧显示距离
        lv_img_set_src(left_img, vw_resource_get_img("icon_sport_distance"));
        lv_label_set_text(left_title_label, "距离");
        lv_label_set_text(left_label, "0.0KM");
    } else {
        // 目标距离/时间模式：左侧显示步数
        lv_img_set_src(left_img, vw_resource_get_img("icon_sport_steps"));
        lv_label_set_text(left_title_label, "步数");
        lv_label_set_text(left_label, "0");
    }

    // lv_obj_set_pos(left_img, 71, 385);
    lv_obj_set_style_text_font(left_title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(left_title_label, lv_color_white(), 0);
    lv_obj_set_pos(left_title_label, 101, 379);
    lv_obj_set_style_text_font(left_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(left_label, lv_color_white(), 0);
    lv_obj_set_pos(left_label, 60, 416);
    lv_obj_align_to(left_img, left_title_label, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    // 右侧速度显示（固定）
    lv_obj_t *speed_img = lv_img_create(sport_running_base);
    lv_img_set_src(speed_img, vw_resource_get_img("icon_sport_speed"));
    // lv_obj_set_pos(speed_img, 266, 384);

    lv_obj_t *speed_title_label = lv_label_create(sport_running_base);
    lv_label_set_text(speed_title_label, "速度");
    lv_obj_set_style_text_font(speed_title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(speed_title_label, lv_color_white(), 0);
    lv_obj_set_pos(speed_title_label, 299, 379);
    lv_obj_align_to(speed_img, speed_title_label, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    lv_obj_t *speed_label = lv_label_create(sport_running_base);
    lv_label_set_text(speed_label, "0米/秒");
    lv_obj_set_style_text_font(speed_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(speed_label, lv_color_white(), 0);
    lv_obj_align(speed_label, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    // lv_obj_set_pos(speed_label, 268, 416);

    // 重置运动数据
    sport_data.current_time = 0;
    sport_data.current_distance = 0;
    sport_data.current_steps = 0;
    sport_data.sport_type = sport_type;

    // 初始化传感器并重置步数计数器
    sensor_data_init();
    sensor_step_counter_reset();
    if (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) {
        sensor_step_set_length(STEP_LENGTH_RUNNING_M);
    } else {
        sensor_step_set_length(STEP_LENGTH_WALKING_M);
    }

    // 启动定时器
    sport_timer = lv_timer_create(sport_timer_handler, 1000, NULL);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sport_running_base, slide_gesture_sport_running_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sport_running_base, sport_running_deleted_cb, LV_EVENT_DELETE, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(sport_running_base);

    SPORTS_LOG("sport_running: create complete");
}

/**
 * 创建运动停止页面
 */
void sport_stop_app_create(void)
{
    // 创建运动停止页面
    sport_stop_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sport_stop_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sport_stop_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(sport_stop_base, LV_OPA_70, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sport_stop_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sport_stop_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sport_stop_base, LV_ALIGN_CENTER, 0, 0);

    // 创建继续按钮
    lv_obj_t *continue_btn = lv_obj_create(sport_stop_base);
    lv_obj_set_size(continue_btn, 150, 100);
    lv_obj_set_style_bg_opa(continue_btn, LV_OPA_100, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(continue_btn, lv_color_hex(0x2D47CB), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(continue_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(continue_btn, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_border_width(continue_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(continue_btn, 32, LV_STATE_DEFAULT);
    lv_obj_set_pos(continue_btn, 35, 203);
    lv_obj_clear_flag(continue_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    
    // 添加文字
    lv_obj_t *continue_label = lv_label_create(continue_btn);
    lv_label_set_text(continue_label, "继续");
    lv_obj_set_style_text_color(continue_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(continue_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_align(continue_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(continue_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(continue_btn, continue_button_click_handler, LV_EVENT_CLICKED, NULL);

    // 创建结束按钮
    lv_obj_t *finish_btn = lv_obj_create(sport_stop_base);
    lv_obj_set_size(finish_btn, 150, 100);
    lv_obj_set_style_bg_opa(finish_btn, LV_OPA_100, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(finish_btn, lv_color_hex(0x1A1A1A), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(finish_btn, lv_color_white(), 0);
    lv_obj_set_style_text_font(finish_btn, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_border_width(finish_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(finish_btn, 32, LV_STATE_DEFAULT);
    lv_obj_set_pos(finish_btn, 225, 203);
    lv_obj_clear_flag(finish_btn, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加文字
    lv_obj_t *finish_label = lv_label_create(finish_btn);
    lv_label_set_text(finish_label, "结束");
    lv_obj_set_style_text_color(finish_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(finish_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_align(finish_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(finish_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加点击事件
    lv_obj_add_event_cb(finish_btn, finish_button_click_handler, LV_EVENT_CLICKED, NULL);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sport_stop_base, slide_gesture_sport_stop_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sport_stop_base, sport_page_deleted_cb, LV_EVENT_DELETE, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(sport_stop_base);

    SPORTS_LOG("sport_stop: create complete");
}

/**
 * 运动类型点击回调
 */
static void sport_type_click_handler(lv_event_t *e)
{
    int sport_type = (int)(intptr_t)lv_event_get_user_data(e);

    // 保存运动类型
    sport_data.sport_type = sport_type;

    // 加载该运动类型的设置
    sport_load_settings();

    SPORTS_LOG("sport item clicked: sport_type=%d", sport_type);
    sport_go_create(sport_type);
}

/**
 * GO按钮点击回调
 */
static void go_button_click_handler(lv_event_t *e)
{
    int sport_type = (int)(intptr_t)lv_event_get_user_data(e);
    SPORTS_LOG("sport go clicked: sport_type=%d", sport_type);

    // 保存运动类型
    sport_data.sport_type = sport_type;

    // 加载该运动类型的设置
    sport_load_settings();

    sport_running_create(sport_type);
}

/**
 * 设置按钮点击回调
 */
static void setting_button_click_handler(lv_event_t *e)
{
    sport_set_create();
}

/**
 * 设置项点击回调
 */
static void setting_item_click_handler(lv_event_t *e)
{
    int setting_type = (int)(intptr_t)lv_event_get_user_data(e);
    SPORTS_LOG("sport setting item clicked: setting_type=%d, sport_type=%d", setting_type, sport_data.sport_type);
    sport_set_edit_create(setting_type, sport_data.sport_type);
}

/**
 * 确认按钮点击回调
 */
static void confirm_button_click_handler(lv_event_t *e)
{
    int data = (int)(intptr_t)lv_event_get_user_data(e);
    int setting_type = data & 0xFF;
    int sport_type = (data >> 8) & 0xFF;

    // 获取roller当前选中的索引
    lv_obj_t *roller = lv_obj_get_child(sport_set_edit_base, 0);
    int index = lv_roller_get_selected(roller);

    // 根据运动类型获取对应的设置
    sport_settings_t *settings = (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) ?
                                  &sport_data.running_settings : &sport_data.walking_settings;

    // 保存当前选择的设置类型
    settings->setting_type = setting_type;

    // 根据设置类型和运动类型计算实际值
    int actual_value = 0;
    switch (setting_type) {
        case SETTING_TARGET_TIME:
            if (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) {
                // 跑步: 5-30分钟,步长5分钟
                actual_value = 5 + index * 5;
            } else {
                // 步行: 10-60分钟,步长5分钟
                actual_value = 10 + index * 5;
            }
            settings->target_time = actual_value;
            SPORTS_LOG("Set %s target time: %d minutes",
                   sport_type == SPORT_TYPE_OUTDOOR_RUNNING ? "running" : "walking",
                   actual_value);
            break;
        case SETTING_TARGET_DISTANCE:
            if (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) {
                // 跑步: 1-5公里,步长0.5公里 (start=2, end=10)
                actual_value = 2 + index;
            } else {
                // 步行: 0.5-5公里,步长0.5公里 (start=1, end=10)
                actual_value = 1 + index;
            }
            settings->target_distance = actual_value;  // 存储为0.5公里的倍数
            SPORTS_LOG("Set %s target distance: %.1f km",
                   sport_type == SPORT_TYPE_OUTDOOR_RUNNING ? "running" : "walking",
                   actual_value * 0.5f);
            break;
        case SETTING_TARGET_STEPS:
            if (sport_type == SPORT_TYPE_OUTDOOR_RUNNING) {
                // 跑步: 3000-10000步,步长1000步
                actual_value = 3000 + index * 1000;
            } else {
                // 步行: 2000-15000步,步长1000步
                actual_value = 2000 + index * 1000;
            }
            settings->target_steps = actual_value;
            SPORTS_LOG("Set %s target steps: %d steps",
                   sport_type == SPORT_TYPE_OUTDOOR_RUNNING ? "running" : "walking",
                   actual_value);
            break;
    }

    // 保存设置到文件
    sport_save_settings();

    if (sport_set_edit_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(sport_set_edit_base);
        lv_obj_del(sport_set_edit_base);
        sport_set_edit_base = NULL;
    }
    // sport_set_create();
}

/**
 * 取消按钮点击回调
 */
static void cancel_button_click_handler(lv_event_t *e)
{
    if (sport_set_edit_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(sport_set_edit_base);
        lv_obj_del(sport_set_edit_base);
        sport_set_edit_base = NULL;
    }
    // sport_set_create();
}

/**
 * 继续按钮点击回调
 */
static void continue_button_click_handler(lv_event_t *e)
{
    if (sport_stop_base != NULL) {
        vw_watch_pop_page(sport_stop_base);
        lv_obj_del(sport_stop_base);
        sport_stop_base = NULL;
    }
    if (sport_timer != NULL) {
        lv_timer_resume(sport_timer);
    }
    sensor_step_counter_resume();
    SPORTS_LOG("[SPORT] Resumed: timer resumed, step counter resumed");
}

/**
 * 结束按钮点击回调
 */
static void finish_button_click_handler(lv_event_t *e)
{
    if (sport_timer != NULL) {
        lv_timer_del(sport_timer);
        sport_timer = NULL;
    }
    
    if (sport_stop_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(sport_stop_base);
        lv_obj_del(sport_stop_base);
        sport_stop_base = NULL;
    }
    
    if (sport_running_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(sport_running_base);
        lv_obj_del(sport_running_base);
        sport_running_base = NULL;
    }
    sport_end_page_create();
    // sport_app_create();
}

static void format_distance(char *buf, int buf_size, float distance_m)
{
    if (distance_m < 1000.0f) {
        snprintf(buf, buf_size, "%dm", (int)distance_m);
    } else {
        snprintf(buf, buf_size, "%.1fKM", distance_m / 1000.0f);
    }
}

static void format_speed(char *buf, int buf_size, float speed_m_per_s)
{
    float km_per_h = speed_m_per_s * 3.6f;
    snprintf(buf, buf_size, "%.1fKM/H", km_per_h);
}

/**
 * 运动定时器处理
 */
static void sport_timer_handler(lv_timer_t *timer)
{
    // 更新运动数据 (step counter由sensor monitor timer 200ms更新)
    sport_data.current_time++;
    int new_steps = sensor_step_counter_get_steps();
    float new_distance = sensor_step_counter_get_distance();

    int delta_steps = 0;
    float delta_distance = 0.0f;
    if (new_steps > sport_data.current_steps) {
        delta_steps = new_steps - sport_data.current_steps;
        sport_data.current_steps = new_steps;
    }
    if (new_distance > sport_data.current_distance) {
        delta_distance = new_distance - sport_data.current_distance;
        sport_data.current_distance = new_distance;
    }

    (void)delta_steps;
    (void)delta_distance;

    // 速度 = 总距离(米) / 总时间(秒)
    if (sport_data.current_time > 0) {
        sport_data.current_speed = sport_data.current_distance / (float)sport_data.current_time;
    } else {
        sport_data.current_speed = 0.0f;
    }

    SPORTS_LOG("Timer: time=%d, steps=%d, distance=%.1f, speed=%.2f m/s",
           sport_data.current_time, sport_data.current_steps, sport_data.current_distance, sport_data.current_speed);

    // 更新UI
    if (sport_running_base != NULL) {
        // 获取当前运动类型的设置
        sport_settings_t *settings = (sport_data.sport_type == SPORT_TYPE_OUTDOOR_RUNNING) ?
                                      &sport_data.running_settings : &sport_data.walking_settings;

        int setting_type = settings->setting_type;
        SPORTS_LOG("Setting type: %d", setting_type);

        // 获取UI对象
        lv_obj_t *top_label = lv_obj_get_child(sport_running_base, 1);
        lv_obj_t *arc = lv_obj_get_child(sport_running_base, 3);
        lv_obj_t *arc_value_label = lv_obj_get_child(sport_running_base, 5);
        lv_obj_t *left_label = lv_obj_get_child(sport_running_base, 9);
        lv_obj_t *speed_label = lv_obj_get_child(sport_running_base, 12);

        // printf("Got UI objects: top_label=%p, arc=%p, arc_value_label=%p, left_label=%p, speed_label=%p\n",
        //        top_label, arc, arc_value_label, left_label, speed_label);

        if (!top_label || !arc || !arc_value_label || !left_label || !speed_label) {
            SPORTS_LOG("ERROR: Some UI objects are NULL!");
            return;
        }

        // 更新顶部显示
        if (setting_type == SETTING_TARGET_TIME) {
            char distance_str[32];
            format_distance(distance_str, sizeof(distance_str), sport_data.current_distance);
            lv_label_set_text(top_label, distance_str);
        } else {
            int hours = sport_data.current_time / 3600;
            int minutes = (sport_data.current_time % 3600) / 60;
            int seconds = sport_data.current_time % 60;
            char time_str[32];
            sprintf(time_str, "%02d:%02d:%02d", hours, minutes, seconds);
            lv_label_set_text(top_label, time_str);
        }

        // 更新圆弧内显示
        if (setting_type == SETTING_TARGET_STEPS) {
            // 目标步数模式：显示步数
            char steps_str[32];
            sprintf(steps_str, "%d", sport_data.current_steps);
            lv_label_set_text(arc_value_label, steps_str);
        } else if (setting_type == SETTING_TARGET_DISTANCE) {
            char distance_str[32];
            format_distance(distance_str, sizeof(distance_str), sport_data.current_distance);
            lv_label_set_text(arc_value_label, distance_str);
        } else if (setting_type == SETTING_TARGET_TIME) {
            // 目标时间模式：显示分钟
            int minutes = sport_data.current_time / 60;
            char minutes_str[32];
            sprintf(minutes_str, "%d", minutes);
            lv_label_set_text(arc_value_label, minutes_str);
        }
        lv_obj_align_to(arc_value_label, arc, LV_ALIGN_TOP_MID, 0, 82);

        // 更新左侧显示
        if (setting_type == SETTING_TARGET_STEPS) {
            char distance_str[32];
            format_distance(distance_str, sizeof(distance_str), sport_data.current_distance);
            lv_label_set_text(left_label, distance_str);
        } else {
            // 目标距离/时间模式：左侧显示步数
            char steps_str[32];
            sprintf(steps_str, "%d", sport_data.current_steps);
            lv_label_set_text(left_label, steps_str);
        }

        char speed_str[32];
        format_speed(speed_str, sizeof(speed_str), sport_data.current_speed);
        lv_label_set_text(speed_label, speed_str);

        // 更新环形进度条
        if (arc) {
            int current_value = 0;
            switch (setting_type) {
                case SETTING_TARGET_STEPS:
                    current_value = sport_data.current_steps;
                    break;
                case SETTING_TARGET_DISTANCE:
                    current_value = (int)sport_data.current_distance;  // 已经是米
                    break;
                case SETTING_TARGET_TIME:
                    current_value = sport_data.current_time;  // 已经是秒
                    break;
            }
            lv_arc_set_value(arc, current_value);
            SPORTS_LOG("Arc value updated to %d", current_value);
        }

        SPORTS_LOG("UI update complete");
    }
}

/**
 * 运动选择页滑动手势处理
 */
static void slide_gesture_sport_main_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            SPORTS_LOG("sport_main: pressed");
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            SPORTS_LOG("sport_main: released, delta_x=%d, delta_y=%d", delta_x, delta_y);

            // 右滑退出（横向移动超过50px且大于纵向移动）
            if (delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                SPORTS_LOG("sport_main: right swipe, exit");
                if(sport_main_base != NULL) {
                    // 将页面从页面栈弹出
                    vw_watch_pop_page(sport_main_base);
                    lv_obj_del(sport_main_base);
                    sport_main_base = NULL;
                }
            }
            break;
        }
        default:
            break;
    }
}

/**
 * 运动详情页滑动手势处理
 */
static void slide_gesture_sport_go_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            SPORTS_LOG("sport_go: pressed");
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            SPORTS_LOG("sport_go: released, delta_x=%d, delta_y=%d", delta_x, delta_y);

            // 右滑退出（横向移动超过50px且大于纵向移动）
            if (delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                SPORTS_LOG("sport_go: right swipe, exit");
                if(sport_go_base != NULL) {
                    // 将页面从页面栈弹出
                    vw_watch_pop_page(sport_go_base);
                    lv_obj_del(sport_go_base);
                    sport_go_base = NULL;
                }
                // sport_app_create();
            }
            break;
        }
        default:
            break;
    }
}

/**
 * 运动设置页滑动手势处理
 */
static void slide_gesture_sport_set_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            SPORTS_LOG("sport_set: pressed");
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            SPORTS_LOG("sport_set: released, delta_x=%d, delta_y=%d", delta_x, delta_y);

            // 右滑退出（横向移动超过50px且大于纵向移动）
            if (delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                SPORTS_LOG("sport_set: right swipe, exit, sport_data.sport_type=%d", sport_data.sport_type);
                if(sport_set_base != NULL) {
                    // 将页面从页面栈弹出
                    vw_watch_pop_page(sport_set_base);
                    lv_obj_del(sport_set_base);
                    sport_set_base = NULL;
                }
                // sport_go_create(sport_data.sport_type);
            }
            break;
        }
        default:
            break;
    }
}

/**
 * 运动设置详情页滑动手势处理
 */
static void slide_gesture_sport_set_edit_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            SPORTS_LOG("sport_set_edit: pressed");
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            SPORTS_LOG("sport_set_edit: released, delta_x=%d, delta_y=%d", delta_x, delta_y);

            // 右滑退出（横向移动超过50px且大于纵向移动）
            if (delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                SPORTS_LOG("sport_set_edit: right swipe, exit");
                if(sport_set_edit_base != NULL) {
                    // 将页面从页面栈弹出
                    vw_watch_pop_page(sport_set_edit_base);
                    lv_obj_del(sport_set_edit_base);
                    sport_set_edit_base = NULL;
                }
                // sport_set_create();
            }
            break;
        }
        default:
            break;
    }
}

/**
 * 运动页滑动手势处理
 */
static void slide_gesture_sport_running_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            SPORTS_LOG("sport_running: pressed");
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            SPORTS_LOG("sport_running: released, delta_x=%d, delta_y=%d", delta_x, delta_y);

            // 右滑退出（横向移动超过50px且大于纵向移动）
            if (delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                SPORTS_LOG("sport_running: right swipe, exit");
                if (sport_timer != NULL) {
                    lv_timer_pause(sport_timer);
                }
                sensor_step_counter_pause();
                sport_stop_app_create();
                // if(sport_running_base != NULL) {
                //     lv_obj_del(sport_running_base);
                //     sport_running_base = NULL;
                // }
                // sport_go_create(sport_data.sport_type);
            }
            break;
        }
        default:
            break;
    }
}

/**
 * 运动停止页滑动手势处理
 */
static void slide_gesture_sport_stop_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            SPORTS_LOG("sport_stop: pressed");
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            SPORTS_LOG("sport_stop: released, delta_x=%d, delta_y=%d", delta_x, delta_y);

            // 右滑退出（横向移动超过50px且大于纵向移动）
            if (delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                SPORTS_LOG("sport_stop: right swipe, exit");
                if(sport_stop_base != NULL) {
                    // 将页面从页面栈弹出
                    vw_watch_pop_page(sport_stop_base);
                    lv_obj_del(sport_stop_base);
                    sport_stop_base = NULL;
                }
                if (sport_timer != NULL) {
                    lv_timer_resume(sport_timer);
                }
                sensor_step_counter_resume();
            }
            break;
        }
        default:
            break;
    }
}

/**
 * 运动应用点击回调
 */
void sport_app_click_callback(lv_event_t *e)
{
    sport_app_create();
}

/**
 * 保存运动设置到文件
 */
static int sport_save_settings(void)
{
    int fd = open(SPORT_DATA_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        SPORTS_LOG("Failed to open sport data file for writing: %s", SPORT_DATA_FILE);
        return -1;
    }

    // 只保存设置部分,不保存运行时数据
    ssize_t bytes_written = write(fd, &sport_data.running_settings, sizeof(sport_settings_t));
    if (bytes_written != sizeof(sport_settings_t)) {
        SPORTS_LOG("Failed to write running settings");
        close(fd);
        return -1;
    }

    bytes_written = write(fd, &sport_data.walking_settings, sizeof(sport_settings_t));
    if (bytes_written != sizeof(sport_settings_t)) {
        SPORTS_LOG("Failed to write walking settings");
        close(fd);
        return -1;
    }

    close(fd);
    SPORTS_LOG("Sport settings saved successfully");
    return 0;
}

/**
 * 从文件加载运动设置
 */
static int sport_load_settings(void)
{
    int fd = open(SPORT_DATA_FILE, O_RDONLY);
    if (fd < 0) {
        SPORTS_LOG("Sport data file not found, using default settings");
        return -1;
    }

    // 读取跑步设置
    ssize_t bytes_read = read(fd, &sport_data.running_settings, sizeof(sport_settings_t));
    if (bytes_read != sizeof(sport_settings_t)) {
        SPORTS_LOG("Failed to read running settings");
        close(fd);
        return -1;
    }

    // 读取步行设置
    bytes_read = read(fd, &sport_data.walking_settings, sizeof(sport_settings_t));
    if (bytes_read != sizeof(sport_settings_t)) {
        SPORTS_LOG("Failed to read walking settings");
        close(fd);
        return -1;
    }

    close(fd);
    SPORTS_LOG("Sport settings loaded successfully");
    SPORTS_LOG("Running: time=%d, distance=%d, steps=%d",
           sport_data.running_settings.target_time,
           sport_data.running_settings.target_distance,
           sport_data.running_settings.target_steps);
    SPORTS_LOG("Walking: time=%d, distance=%d, steps=%d",
           sport_data.walking_settings.target_time,
           sport_data.walking_settings.target_distance,
           sport_data.walking_settings.target_steps);
    return 0;
}

/**
 * 创建运动结束页
 */
static void sport_end_page_create(void)
{
    // 创建运动结束页面
    sport_end_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sport_end_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sport_end_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sport_end_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sport_end_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sport_end_base, LV_ALIGN_CENTER, 0, 0);

    // 获取当前运动类型的设置
    sport_settings_t *settings = (sport_data.sport_type == SPORT_TYPE_OUTDOOR_RUNNING) ?
                                  &sport_data.running_settings : &sport_data.walking_settings;

    // 判断是否达标
    int is_achieved = 0;
    int setting_type = settings->setting_type;

    if (setting_type == SETTING_TARGET_TIME) {
        // 目标时间：比较分钟数
        int current_minutes = sport_data.current_time / 60;
        is_achieved = (current_minutes >= settings->target_time);
    } else if (setting_type == SETTING_TARGET_DISTANCE) {
        // 目标距离：比较公里数
        float current_km = sport_data.current_distance / 1000.0f;
        float target_km = settings->target_distance * 0.5f;
        is_achieved = (current_km >= target_km);
    } else {
        // 目标步数
        is_achieved = (sport_data.current_steps >= settings->target_steps);
    }

    // 创建达标/未达标背景框
    lv_obj_t *status_bg = lv_obj_create(sport_end_base);
    if (is_achieved) {
        // 达标：背景色2BEA77，尺寸163*84
        lv_obj_set_size(status_bg, 163, 84);
        lv_obj_set_style_bg_color(status_bg, lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
    } else {
        // 未达标：背景色FFA332，尺寸181*84
        lv_obj_set_size(status_bg, 181, 84);
        lv_obj_set_style_bg_color(status_bg, lv_color_hex(0xFFA332), LV_STATE_DEFAULT);
    }
    lv_obj_set_style_bg_opa(status_bg, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(status_bg, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(status_bg, 60, LV_STATE_DEFAULT);
    lv_obj_clear_flag(status_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(status_bg, LV_ALIGN_TOP_MID, 0, 57);

    // 创建达标/未达标文字
    lv_obj_t *status_label = lv_label_create(status_bg);
    lv_label_set_text(status_label, is_achieved ? "达标" : "未达标");
    lv_obj_set_style_text_font(status_label, vw_resource_get_font(WATCH_REGULAR_FONT "_44"), 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x000000), 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);

    // 创建本次运动时间数字
    lv_obj_t *time_value_label = lv_label_create(sport_end_base);
    int minutes = sport_data.current_time / 60;
    lv_label_set_text_fmt(time_value_label, "%d", minutes);
    lv_obj_set_style_text_font(time_value_label, vw_resource_get_font(WATCH_REGULAR_FONT "_64"), 0);
    lv_obj_set_style_text_color(time_value_label, lv_color_white(), 0);
    lv_obj_align(time_value_label, LV_ALIGN_TOP_MID, 0, 156);

    // 创建"分钟"文字
    lv_obj_t *time_unit_label = lv_label_create(sport_end_base);
    lv_label_set_text(time_unit_label, "分钟");
    lv_obj_set_style_text_font(time_unit_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_set_style_text_color(time_unit_label, lv_color_white(), 0);
    lv_obj_align(time_unit_label, LV_ALIGN_TOP_MID, 0, 238);
    
    // 创建"距离"文字
    lv_obj_t *distance_title_label = lv_label_create(sport_end_base);
    lv_label_set_text(distance_title_label, "距离");
    lv_obj_set_style_text_font(distance_title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(distance_title_label, lv_color_white(), 0);
    lv_obj_set_pos(distance_title_label, 131, 305);

    // 创建距离图标
    lv_obj_t *distance_icon = lv_img_create(sport_end_base);
    lv_img_set_src(distance_icon, vw_resource_get_img("icon_sport_distance"));
    lv_obj_align_to(distance_icon, distance_title_label, LV_ALIGN_OUT_LEFT_MID, -20, 0);

    // 创建距离值
    lv_obj_t *distance_value_label = lv_label_create(sport_end_base);
    char end_dist_str[32];
    format_distance(end_dist_str, sizeof(end_dist_str), sport_data.current_distance);
    lv_label_set_text(distance_value_label, end_dist_str);
    lv_obj_set_style_text_font(distance_value_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(distance_value_label, lv_color_white(), 0);
    lv_obj_set_pos(distance_value_label, 197, 297);


    // 创建"步数"文字
    lv_obj_t *steps_title_label = lv_label_create(sport_end_base);
    lv_label_set_text(steps_title_label, "步数");
    lv_obj_set_style_text_font(steps_title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(steps_title_label, lv_color_white(), 0);
    lv_obj_set_pos(steps_title_label, 132, 360);

    // 创建步数图标
    lv_obj_t *steps_icon = lv_img_create(sport_end_base);
    lv_img_set_src(steps_icon, vw_resource_get_img("icon_sport_steps"));
    lv_obj_align_to(steps_icon, steps_title_label, LV_ALIGN_OUT_LEFT_MID, -19, 0);

    // 创建步数值
    lv_obj_t *steps_value_label = lv_label_create(sport_end_base);
    lv_label_set_text_fmt(steps_value_label, "%d", sport_data.current_steps);
    lv_obj_set_style_text_font(steps_value_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(steps_value_label, lv_color_white(), 0);
    lv_obj_set_pos(steps_value_label, 198, 352);

    // 创建"速度"文字
    lv_obj_t *speed_title_label = lv_label_create(sport_end_base);
    lv_label_set_text(speed_title_label, "速度");
    lv_obj_set_style_text_font(speed_title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(speed_title_label, lv_color_white(), 0);
    lv_obj_set_pos(speed_title_label, 131, 418);

    // 创建速度图标
    lv_obj_t *speed_icon = lv_img_create(sport_end_base);
    lv_img_set_src(speed_icon, vw_resource_get_img("icon_sport_speed"));
    lv_obj_align_to(speed_icon, speed_title_label, LV_ALIGN_OUT_LEFT_MID, -19, 0);

    // 创建速度值
    lv_obj_t *speed_value_label = lv_label_create(sport_end_base);
    char end_speed_str[32];
    format_speed(end_speed_str, sizeof(end_speed_str), sport_data.current_speed);
    lv_label_set_text(speed_value_label, end_speed_str);
    lv_obj_set_style_text_font(speed_value_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(speed_value_label, lv_color_white(), 0);
    lv_obj_set_pos(speed_value_label, 200, 410);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sport_end_base, slide_gesture_sport_end_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sport_end_base, sport_page_deleted_cb, LV_EVENT_DELETE, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(sport_end_base);

    SPORTS_LOG("sport_end: create complete");
}

/**
 * 运动结束页滑动手势处理
 */
static void slide_gesture_sport_end_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            SPORTS_LOG("sport_end: pressed");
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            SPORTS_LOG("sport_end: released, delta_x=%d, delta_y=%d", delta_x, delta_y);

            // 右滑退出（横向移动超过50px且大于纵向移动）
            if (delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                SPORTS_LOG("sport_end: right swipe, exit");
                if (sport_end_base != NULL) {
                    // 将页面从页面栈弹出
                    vw_watch_pop_page(sport_end_base);
                    lv_obj_del(sport_end_base);
                    sport_end_base = NULL;
                }
            }
            break;
        }
        default:
            break;
    }
}

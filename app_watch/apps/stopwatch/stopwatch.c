/**
 * @file stopwatch.c
 * 秒表页面
 */

#include "stopwatch.h"
#include "../common/watch_pages.h"
#include "../../resource/resource.h"
#include "../launcher/launcher.h"
#include <stdio.h>
#include <time.h>

/* UI对象 */
static lv_obj_t* stopwatch_base = NULL;
static lv_obj_t* stopwatch_label = NULL;
static lv_obj_t* start_btn = NULL;
static lv_obj_t* reset_btn = NULL;
static lv_obj_t* time_container = NULL;

/* 秒表状态 */
typedef struct {
    bool is_running;
    bool is_stopped;
} stopwatch_status_t;

/* 秒表变量 */
static lv_timer_t *stopwatch_timer = NULL;
static unsigned long stopwatch_milliseconds = 0;
static unsigned long prev_time = 0;
static stopwatch_status_t status = {false, false};

/* 函数声明 */
static void slide_gesture_handler(lv_event_t *e);
static void start_stopwatch_event_cb(lv_event_t *e);
static void reset_stopwatch_event_cb(lv_event_t *e);
static void stopwatch_task(lv_timer_t *task);
static void update_stopwatch_label(void);


static void stopwatch_app_create(void)
{
    // 创建秒表页面
    stopwatch_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(stopwatch_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(stopwatch_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(stopwatch_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(stopwatch_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(stopwatch_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(stopwatch_base);
    lv_label_set_text(title_label, "秒表");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    // 创建时间显示容器
    time_container = lv_obj_create(stopwatch_base);
    lv_obj_set_size(time_container, 339, 80);
    lv_obj_set_style_bg_color(time_container, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(time_container, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(time_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(time_container, 30, LV_STATE_DEFAULT);
    lv_obj_align(time_container, LV_ALIGN_TOP_MID, 0, 171);
    lv_obj_clear_flag(time_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(time_container, LV_SCROLLBAR_MODE_OFF);

    // 创建时间显示标签
    stopwatch_label = lv_label_create(time_container);
    lv_label_set_text(stopwatch_label, "00:00:00");
    lv_obj_set_style_text_font(stopwatch_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(stopwatch_label, lv_color_white(), 0);
    lv_obj_center(stopwatch_label);

    // 创建开始按钮
    start_btn = lv_btn_create(stopwatch_base);
    lv_obj_add_event_cb(start_btn, start_stopwatch_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(start_btn, 150, 100);
    lv_obj_set_style_bg_color(start_btn, lv_color_hex(0x2D47CB), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(start_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(start_btn, 30, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(start_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(start_btn, LV_ALIGN_TOP_LEFT, 35, 363);
    lv_obj_t *start_btn_label = lv_label_create(start_btn);
    lv_label_set_text(start_btn_label, "开始");
    lv_obj_set_style_text_font(start_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(start_btn_label, lv_color_white(), 0);
    lv_obj_center(start_btn_label);

    // 创建归零按钮
    reset_btn = lv_btn_create(stopwatch_base);
    lv_obj_add_event_cb(reset_btn, reset_stopwatch_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(reset_btn, 150, 100);
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(reset_btn, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(reset_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(reset_btn, 30, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(reset_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(reset_btn, LV_ALIGN_TOP_LEFT, 225, 363);
    lv_obj_t *reset_btn_label = lv_label_create(reset_btn);
    lv_label_set_text(reset_btn_label, "归零");
    lv_obj_set_style_text_font(reset_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(reset_btn_label, lv_color_white(), 0);
    lv_obj_center(reset_btn_label);

    lv_obj_add_flag(time_container, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(start_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(reset_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    // 添加滑动手势处理
    lv_obj_add_event_cb(stopwatch_base, slide_gesture_handler, LV_EVENT_ALL, NULL);

    printf("stopwatch: create complete\n");
}

/**
 * 更新秒表显示
 */
static void update_stopwatch_label(void)
{
    // 计算时、分、秒、毫秒
    // int hours = (stopwatch_milliseconds / (1000 * 60 * 60)) % 24;
    int minutes = (stopwatch_milliseconds / (1000 * 60)) % 60;
    int seconds = (stopwatch_milliseconds / 1000) % 60;
    int milliseconds = (stopwatch_milliseconds % 1000) / 10;

    // 格式化时间字符串
    char time_str[12];
    sprintf(time_str, "%02d:%02d:%02d", minutes, seconds, milliseconds);

    // 更新标签
    lv_label_set_text(stopwatch_label, time_str);
}

/**
 * 秒表任务
 */
static void stopwatch_task(lv_timer_t *task)
{
    unsigned long now = lv_tick_get();
    unsigned long delta = now - prev_time;
    stopwatch_milliseconds += delta;
    prev_time = now;

    update_stopwatch_label();
}

/**
 * 开始/暂停按钮回调
 */
static void start_stopwatch_event_cb(lv_event_t *e)
{
    if (!status.is_running) {
        // 开始秒表
        prev_time = lv_tick_get();
        stopwatch_timer = lv_timer_create(stopwatch_task, 10, NULL);
        lv_label_set_text(lv_obj_get_child(start_btn, 0), "暂停");
        status.is_running = true;
        status.is_stopped = false;
    } else {
        // 暂停秒表
        lv_timer_del(stopwatch_timer);
        stopwatch_timer = NULL;
        lv_label_set_text(lv_obj_get_child(start_btn, 0), "开始");
        status.is_running = false;
        status.is_stopped = true;
    }
}

/**
 * 归零按钮回调
 */
static void reset_stopwatch_event_cb(lv_event_t *e)
{
    // 停止秒表
    if (stopwatch_timer) {
        lv_timer_del(stopwatch_timer);
        stopwatch_timer = NULL;
    }

    // 重置时间
    stopwatch_milliseconds = 0;
    update_stopwatch_label();

    // 重置状态
    lv_label_set_text(lv_obj_get_child(start_btn, 0), "开始");
    status.is_running = false;
    status.is_stopped = false;
}

/**
 * 滑动手势处理
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
                    // 右滑：退出当前页面
                    if(stopwatch_base != NULL) {
                        // 停止秒表
                        if (stopwatch_timer) {
                            lv_timer_del(stopwatch_timer);
                            stopwatch_timer = NULL;
                        }
                        
                        // 从页面栈中移除页面
                        vw_watch_pop_page(stopwatch_base);
                        // 删除页面
                        lv_obj_del(stopwatch_base);
                        stopwatch_base = NULL;
                        
                        // 重置状态
                        stopwatch_milliseconds = 0;
                        status.is_running = false;
                        status.is_stopped = false;
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
 * 秒表应用点击回调
 */
void stopwatch_app_click_callback(lv_event_t *e)
{
    printf("stopwatch app clicked\n");
    stopwatch_app_create();
    /* 将页面添加到页面栈 */
    if (stopwatch_base != NULL) {
        vw_watch_push_page(stopwatch_base);
    }
}
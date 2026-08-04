/**
 * @file settings_time.c
 * 设置-时间页面
 */

#include "settings.h"
#include "../common/watch_pages.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <nuttx/timers/rtc.h>

/* UI对象 */
static lv_obj_t* time_base = NULL;
static lv_obj_t* time_edit_base = NULL;
static lv_obj_t* hour_roller = NULL;
static lv_obj_t* minute_roller = NULL;

/* 选中的时间 */
static int selected_hour;
static int selected_minute;

static void slide_gesture_main_handler(lv_event_t *e);
static void slide_gesture_edit_handler(lv_event_t *e);
static void edit_btn_event_cb(lv_event_t *e);
static void confirm_btn_event_cb(lv_event_t *e);
static void cancel_btn_event_cb(lv_event_t *e);
static void set_system_time(int year, int month, int day, int hour, int minute);


static void settings_timeset_create(void)
{
    // 获取当前系统时间
    time_t current_time;
    struct tm *time_info;
    time(&current_time);
    time_info = localtime(&current_time);
    
    selected_hour = time_info->tm_hour;
    selected_minute = time_info->tm_min;
    
    // 创建时间设置页面
    time_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(time_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(time_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(time_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(time_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(time_base, LV_ALIGN_CENTER, 0, 0);

    // 创建年份月份显示
    lv_obj_t *date_label = lv_label_create(time_base);
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%d年 %d月", time_info->tm_year + 1900, time_info->tm_mon + 1);
    lv_label_set_text(date_label, date_str);
    lv_obj_set_style_text_font(date_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0, 105);

    // 创建时间显示
    lv_obj_t *time_label = lv_label_create(time_base);
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", time_info->tm_hour, time_info->tm_min);
    lv_label_set_text(time_label, time_str);
    lv_obj_set_style_text_font(time_label, vw_resource_get_font(WATCH_ALIBABA_REGULAR_FONT "_96"), 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 161);

    // 创建修改按钮
    lv_obj_t *edit_btn = lv_btn_create(time_base);
    lv_obj_add_event_cb(edit_btn, edit_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(edit_btn, 150, 100);
    lv_obj_set_style_bg_color(edit_btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(edit_btn, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(edit_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(edit_btn, 32, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(edit_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(edit_btn, LV_ALIGN_TOP_MID, 0, 313);
    lv_obj_t *edit_btn_label = lv_label_create(edit_btn);
    lv_label_set_text(edit_btn_label, "修改");
    lv_obj_set_style_text_font(edit_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(edit_btn_label, lv_color_white(), 0);
    lv_obj_center(edit_btn_label);

    // 添加滑动手势处理
    lv_obj_add_event_cb(time_base, slide_gesture_main_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(time_base);
}

static void settings_time_edit_create(void)
{
    // 创建时间编辑页面
    time_edit_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(time_edit_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(time_edit_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(time_edit_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(time_edit_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(time_edit_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(time_edit_base);
    lv_label_set_text(title_label, "修改时间");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    // 创建小时滚轮
    hour_roller = lv_roller_create(time_edit_base);
    char hours_buf[150] = {0};
    for (int i = 0; i <= 23; i++) {
        char hour_str[10];
        sprintf(hour_str, "%02d\n", i);
        strcat(hours_buf, hour_str);
    }
    hours_buf[strlen(hours_buf)-1] = '\0';
    
    lv_roller_set_options(hour_roller, hours_buf, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_selected(hour_roller, selected_hour, LV_ANIM_OFF);
    lv_obj_set_width(hour_roller, 169);
    lv_obj_align(hour_roller, LV_ALIGN_TOP_LEFT, 26, 81);
    
    // 设置滚轮样式
    lv_obj_set_style_border_color(hour_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(hour_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(hour_roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hour_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(hour_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hour_roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(hour_roller, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *hour_roller_font = vw_resource_get_font(WATCH_REGULAR_FONT "_36");
    lv_obj_set_style_text_font(hour_roller, hour_roller_font, 0);
    lv_obj_set_style_radius(hour_roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(hour_roller, 3);
    int32_t hour_line_h = lv_font_get_line_height(hour_roller_font);
    lv_obj_set_style_text_line_space(hour_roller, WATCH_BTN_EDIT_HEIGHT - hour_line_h, LV_PART_MAIN);
    lv_obj_set_height(hour_roller, WATCH_BTN_EDIT_HEIGHT * 3);
    lv_obj_clear_flag(hour_roller, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(hour_roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建分钟滚轮
    minute_roller = lv_roller_create(time_edit_base);
    char mins_buf[300] = {0};
    for (int i = 0; i <= 59; i++) {
        char min_str[10];
        sprintf(min_str, "%02d\n", i);
        strcat(mins_buf, min_str);
    }
    mins_buf[strlen(mins_buf)-1] = '\0';
    
    lv_roller_set_options(minute_roller, mins_buf, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_selected(minute_roller, selected_minute, LV_ANIM_OFF);
    lv_obj_set_width(minute_roller, 169);
    lv_obj_align(minute_roller, LV_ALIGN_TOP_LEFT, 216, 81);
    
    // 设置滚轮样式
    lv_obj_set_style_border_color(minute_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(minute_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(minute_roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(minute_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(minute_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(minute_roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(minute_roller, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *minute_roller_font = vw_resource_get_font(WATCH_REGULAR_FONT "_36");
    lv_obj_set_style_text_font(minute_roller, minute_roller_font, 0);
    lv_obj_set_style_radius(minute_roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(minute_roller, 3);
    int32_t minute_line_h = lv_font_get_line_height(minute_roller_font);
    lv_obj_set_style_text_line_space(minute_roller, WATCH_BTN_EDIT_HEIGHT - minute_line_h, LV_PART_MAIN);
    lv_obj_set_height(minute_roller, WATCH_BTN_EDIT_HEIGHT * 3);
    lv_obj_clear_flag(minute_roller, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(minute_roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建确认按钮
    lv_obj_t *confirm_btn = lv_btn_create(time_edit_base);
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

    // 创建取消按钮
    lv_obj_t *cancel_btn = lv_btn_create(time_edit_base);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(cancel_btn, 150, 100);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cancel_btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cancel_btn, 32, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 225, 363);
    lv_obj_t *cancel_btn_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_btn_label, "取消");
    lv_obj_set_style_text_font(cancel_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(cancel_btn_label, lv_color_white(), 0);
    lv_obj_center(cancel_btn_label);

    // 添加滑动手势处理
    lv_obj_add_event_cb(time_edit_base, slide_gesture_edit_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(time_edit_base);
}

/**
 * 修改按钮事件回调
 */
static void edit_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    settings_time_edit_create();
}

/**
 * 确认按钮事件回调
 */
static void confirm_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    // 获取选中的时间
    selected_hour = lv_roller_get_selected(hour_roller);
    selected_minute = lv_roller_get_selected(minute_roller);
    
    // 获取当前时间的年、月、日
    time_t current_time;
    struct tm *time_info;
    time(&current_time);
    time_info = localtime(&current_time);
    int year = time_info->tm_year + 1900;
    int month = time_info->tm_mon + 1;
    int day = time_info->tm_mday;
    
    // 设置系统时间
    set_system_time(year, month, day, selected_hour, selected_minute);

    // 更新时间显示页面
    if (time_base != NULL) {
        // 更新时间标签
        lv_obj_t *time_label = lv_obj_get_child(time_base, 1);  // 时间标签是第二个子对象
        if (time_label) {
            char time_str[32];
            snprintf(time_str, sizeof(time_str), "%02d:%02d", selected_hour, selected_minute);
            lv_label_set_text(time_label, time_str);
        }
    }
    
    // 退出页面
    if (time_edit_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(time_edit_base);
        lv_obj_del(time_edit_base);
        time_edit_base = NULL;
    }
}

/**
 * 取消按钮事件回调
 */
static void cancel_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    // 退出页面
    if (time_edit_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(time_edit_base);
        lv_obj_del(time_edit_base);
        time_edit_base = NULL;
    }
}

/**
 * 设置系统时间
 */
static void set_system_time(int year, int month, int day, int hour, int minute)
{
    struct tm time_to_set;
    memset(&time_to_set, 0, sizeof(struct tm));
    time_to_set.tm_year = year - 1900;
    time_to_set.tm_mon = month - 1;
    time_to_set.tm_mday = day;
    time_to_set.tm_hour = hour;
    time_to_set.tm_min = minute;
    time_to_set.tm_isdst = -1;
    
    // 转换为time_t并设置系统时间
    time_t new_time = mktime(&time_to_set);
    
    struct timeval new_timeval;
    new_timeval.tv_sec = new_time;
    new_timeval.tv_usec = 0;
    
    if (settimeofday(&new_timeval, NULL) == -1) {
        printf("Failed to set system time\n");
    } else {
        printf("System time set to: %d-%02d-%02d %02d:%02d\n", year, month, day, hour, minute);
    }
    
    int fd = open("/dev/rtc0", O_RDWR);
    if (fd >= 0) {
        struct tm utc_tm;
        gmtime_r(&new_time, &utc_tm);
        struct rtc_time rtctime;
        memset(&rtctime, 0, sizeof(rtctime));
        rtctime.tm_sec   = utc_tm.tm_sec;
        rtctime.tm_min   = utc_tm.tm_min;
        rtctime.tm_hour  = utc_tm.tm_hour;
        rtctime.tm_mday  = utc_tm.tm_mday;
        rtctime.tm_mon   = utc_tm.tm_mon;
        rtctime.tm_year  = utc_tm.tm_year;
        rtctime.tm_wday  = utc_tm.tm_wday;
        int rtc_ret = ioctl(fd, RTC_SET_TIME, (unsigned long)&rtctime);
        close(fd);
        if (rtc_ret >= 0)
            printf("RTC time set successfully\n");
        else
            printf("Failed to set RTC time, errno=%d\n", errno);
    } else {
        printf("Cannot open /dev/rtc0, errno=%d\n", errno);
    }
}

/**
 * 时间显示页面 - 滑动手势处理
 */
static void slide_gesture_main_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};
    static bool main_is_dragging = false;

    switch(code) {
        case LV_EVENT_PRESSED:
            lv_indev_get_point(lv_indev_active(), &start_point);
            main_is_dragging = true;
            break;
        
        case LV_EVENT_RELEASED:
            if(main_is_dragging) {
                lv_point_t end_point;
                lv_indev_get_point(lv_indev_active(), &end_point);
                
                int32_t delta_x = end_point.x - start_point.x;
                int32_t delta_y = end_point.y - start_point.y;

                if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                    // 右滑：退出当前页面
                    if(time_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(time_base);
                        lv_obj_del(time_base);
                        time_base = NULL;
                    }
                }
            }
            main_is_dragging = false;
            break;
            
        default:
            break;
    }
}

/**
 * 时间编辑页面 - 滑动手势处理
 */
static void slide_gesture_edit_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};
    static bool edit_is_dragging = false;

    switch(code) {
        case LV_EVENT_PRESSED:
            lv_indev_get_point(lv_indev_active(), &start_point);
            edit_is_dragging = true;
            break;
        
        case LV_EVENT_RELEASED:
            if(edit_is_dragging) {
                lv_point_t end_point;
                lv_indev_get_point(lv_indev_active(), &end_point);
                
                int32_t delta_x = end_point.x - start_point.x;
                int32_t delta_y = end_point.y - start_point.y;

                if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                    // 右滑：返回到时间显示页面
                    if(time_edit_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(time_edit_base);
                        lv_obj_del(time_edit_base);
                        time_edit_base = NULL;
                        // settings_timeset_event_cb(NULL);  // 重新创建时间显示页面
                    }
                }
            }
            edit_is_dragging = false;
            break;
            
        default:
            break;
    }
}

/**
 * 时间设置事件回调
 */
void settings_timeset_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        settings_timeset_create();
    }
}

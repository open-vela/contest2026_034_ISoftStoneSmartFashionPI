/**
 * @file settings_date.c
 * 设置-日期页面
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

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define SETTINGS_LOG(fmt, ...) printf("[SETTINGS] " fmt "\n", ##__VA_ARGS__)
#else
#  define SETTINGS_LOG(fmt, ...)
#endif
#include <sys/ioctl.h>
#include <errno.h>
#include <nuttx/timers/rtc.h>

/* UI对象 */
static lv_obj_t* date_base = NULL;
static lv_obj_t* year_roller = NULL;
static lv_obj_t* month_roller = NULL;
static lv_obj_t* day_roller = NULL;

/* 选中的日期 */
static int selected_year;
static int selected_month;
static int selected_day;

static void slide_gesture_handler(lv_event_t *e);
static void confirm_btn_event_cb(lv_event_t *e);
static void cancel_btn_event_cb(lv_event_t *e);
static void set_system_time(int year, int month, int day, int hour, int minute);


static void settings_dateset_create(void)
{
    // 获取当前系统时间
    time_t current_time;
    struct tm *time_info;
    time(&current_time);
    time_info = localtime(&current_time);
    
    selected_year = time_info->tm_year + 1900;
    selected_month = time_info->tm_mon + 1;
    selected_day = time_info->tm_mday;
    
    // 创建日期设置页面
    date_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(date_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(date_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(date_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(date_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(date_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(date_base);
    lv_label_set_text(title_label, "修改日期");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    // 创建年份滚轮
    year_roller = lv_roller_create(date_base);
    char years_buf[600] = {0};
    for (int i = 2000; i <= 2099; i++) {
        char year_str[10];
        sprintf(year_str, "%d\n", i);
        strcat(years_buf, year_str);
    }
    years_buf[strlen(years_buf)-1] = '\0';
    
    lv_roller_set_options(year_roller, years_buf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(year_roller, selected_year - 2000, LV_ANIM_OFF);
    lv_obj_set_width(year_roller, 139);
    lv_obj_align(year_roller, LV_ALIGN_TOP_LEFT, 26, 81);
    
    // 设置滚轮样式
    lv_obj_set_style_border_color(year_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(year_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(year_roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(year_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(year_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(year_roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(year_roller, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *date_roller_font = vw_resource_get_font(WATCH_REGULAR_FONT "_36");
    lv_obj_set_style_text_font(year_roller, date_roller_font, 0);
    lv_obj_set_style_radius(year_roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(year_roller, 3);
    int32_t date_line_h = lv_font_get_line_height(date_roller_font);
    lv_obj_set_style_text_line_space(year_roller, WATCH_BTN_EDIT_HEIGHT - date_line_h, LV_PART_MAIN);
    lv_obj_set_height(year_roller, WATCH_BTN_EDIT_HEIGHT * 3);
    lv_obj_add_flag(year_roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建月份滚轮
    month_roller = lv_roller_create(date_base);
    lv_roller_set_options(month_roller,
                        "01\n02\n03\n04\n05\n06\n"
                        "07\n08\n09\n10\n11\n12",
                        LV_ROLLER_MODE_INFINITE);
    lv_roller_set_selected(month_roller, selected_month - 1, LV_ANIM_OFF);
    lv_obj_set_width(month_roller, 90);
    lv_obj_align(month_roller, LV_ALIGN_TOP_LEFT, 26 + 139 + 21, 81);
    
    // 设置滚轮样式
    lv_obj_set_style_border_color(month_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(month_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(month_roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(month_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(month_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(month_roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(month_roller, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(month_roller, date_roller_font, 0);
    lv_obj_set_style_radius(month_roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(month_roller, 3);
    lv_obj_set_style_text_line_space(month_roller, WATCH_BTN_EDIT_HEIGHT - date_line_h, LV_PART_MAIN);
    lv_obj_set_height(month_roller, WATCH_BTN_EDIT_HEIGHT * 3);
    lv_obj_add_flag(month_roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建日期滚轮
    day_roller = lv_roller_create(date_base);
    char days_buf[200] = {0};
    for (int i = 1; i <= 31; i++) {
        char day_str[10];
        sprintf(day_str, "%02d\n", i);
        strcat(days_buf, day_str);
    }
    days_buf[strlen(days_buf)-1] = '\0';
    
    lv_roller_set_options(day_roller, days_buf, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_selected(day_roller, selected_day - 1, LV_ANIM_OFF);
    lv_obj_set_width(day_roller, 90);
    lv_obj_align(day_roller, LV_ALIGN_TOP_LEFT, 26 + 139 + 21 + 90 + 21, 81);
    
    // 设置滚轮样式
    lv_obj_set_style_border_color(day_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(day_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(day_roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(day_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(day_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(day_roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(day_roller, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(day_roller, date_roller_font, 0);
    lv_obj_set_style_radius(day_roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(day_roller, 3);
    lv_obj_set_style_text_line_space(day_roller, WATCH_BTN_EDIT_HEIGHT - date_line_h, LV_PART_MAIN);
    lv_obj_set_height(day_roller, WATCH_BTN_EDIT_HEIGHT * 3);
    lv_obj_add_flag(day_roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建确认按钮
    lv_obj_t *confirm_btn = lv_btn_create(date_base);
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
    lv_obj_t *cancel_btn = lv_btn_create(date_base);
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
    lv_obj_add_event_cb(date_base, slide_gesture_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(date_base);
}


/**
 * 确认按钮事件回调
 */
static void confirm_btn_event_cb(lv_event_t *e)
{
    // 获取选中的日期
    selected_year = lv_roller_get_selected(year_roller) + 2000;
    selected_month = lv_roller_get_selected(month_roller) + 1;
    selected_day = lv_roller_get_selected(day_roller) + 1;
    
    // 获取当前时间的小时和分钟
    time_t current_time;
    struct tm *time_info;
    time(&current_time);
    time_info = localtime(&current_time);
    int hour = time_info->tm_hour;
    int minute = time_info->tm_min;
    
    // 设置系统时间
    set_system_time(selected_year, selected_month, selected_day, hour, minute);
    
    // 退出页面
    if (date_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(date_base);
        lv_obj_del(date_base);
        date_base = NULL;
    }
}

/**
 * 取消按钮事件回调
 */
static void cancel_btn_event_cb(lv_event_t *e)
{
    // 退出页面
    if (date_base != NULL) {
        // 将页面从页面栈弹出
        vw_watch_pop_page(date_base);
        lv_obj_del(date_base);
        date_base = NULL;
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
        SETTINGS_LOG("Failed to set system time");
    } else {
        SETTINGS_LOG("System time set to: %d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
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
            SETTINGS_LOG("RTC time set successfully");
        else
            SETTINGS_LOG("Failed to set RTC time, errno=%d", errno);
    } else {
        SETTINGS_LOG("Cannot open /dev/rtc0, errno=%d", errno);
    }
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
                    if(date_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(date_base);
                        lv_obj_del(date_base);
                        date_base = NULL;
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
 * 日期设置事件回调
 */
void settings_dateset_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        settings_dateset_create();
    }
}

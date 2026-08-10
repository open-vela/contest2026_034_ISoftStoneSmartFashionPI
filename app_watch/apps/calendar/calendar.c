/**
 * @file calendar.c
 * 日历页面 - 使用LVGL原生日历控件
 */

#include "calendar.h"
#include "../common/watch_pages.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include <stdio.h>
#include <time.h>

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define CAL_LOG(fmt, ...) printf("[CALENDAR] " fmt "\n", ##__VA_ARGS__)
#else
#  define CAL_LOG(fmt, ...)
#endif

extern const lv_image_dsc_t icon_calendar_left;
extern const lv_image_dsc_t icon_calendar_right;

/* UI对象 */
static lv_obj_t* calendar_base = NULL;
static lv_obj_t* calendar = NULL;
static lv_obj_t* date_label = NULL;

static void slide_gesture_handler(lv_event_t *e);
static void calendar_event_handler(lv_event_t *e);
static void left_arrow_cb(lv_event_t *e);
static void right_arrow_cb(lv_event_t *e);

/**
 * 计算指定月份需要的行数
 * @param year 年份
 * @param month 月份 (1-12)
 * @return 需要的行数 (4-6)
 */
static int calculate_month_rows(int year, int month)
{
    // 获取本月第一天是星期几
    struct tm first_day_tm = {0};
    first_day_tm.tm_year = year - 1900;
    first_day_tm.tm_mon = month - 1;
    first_day_tm.tm_mday = 1;
    first_day_tm.tm_hour = 12;
    mktime(&first_day_tm);
    int first_day_wday = first_day_tm.tm_wday;

    // 获取本月天数
    int days_in_month;
    if (month == 2) {
        days_in_month = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28;
    } else if (month == 4 || month == 6 || month == 9 || month == 11) {
        days_in_month = 30;
    } else {
        days_in_month = 31;
    }

    // 获取本月最后一天是星期几
    struct tm last_day_tm = {0};
    last_day_tm.tm_year = year - 1900;
    last_day_tm.tm_mon = month - 1;
    last_day_tm.tm_mday = days_in_month;
    last_day_tm.tm_hour = 12;
    mktime(&last_day_tm);
    int last_day_wday = last_day_tm.tm_wday;

    // 计算需要的行数
    // 转换为周一开始的索引（周日=0 -> 6，周一=1 -> 0）
    int start_offset = (first_day_wday == 0) ? 6 : first_day_wday - 1;

    // 计算最后一天在周中的位置（周一开始）
    int last_day_offset = (last_day_wday == 0) ? 6 : last_day_wday - 1;

    // 计算总共需要的格子数
    // 从第一天的位置开始，到本月最后一天的位置
    // 需要补齐到周日（即last_day_offset=6）
    int total_cells = start_offset + days_in_month;

    // 如果最后一天不是周日，需要补齐到周日
    // 补齐的天数 = 6 - last_day_offset
    int padding_days = (last_day_offset == 6) ? 0 : (6 - last_day_offset);
    total_cells += padding_days;

    int rows_needed = (total_cells + 6) / 7;  // 向上取整

    // 限制在4-6行之间
    if (rows_needed < 4) rows_needed = 4;
    if (rows_needed > 6) rows_needed = 6;

    CAL_LOG("[calculate_month_rows] Year=%d, Month=%d, FirstDayWday=%d, LastDayWday=%d, DaysInMonth=%d, StartOffset=%d, LastDayOffset=%d, PaddingDays=%d, TotalCells=%d, RowsNeeded=%d",
           year, month, first_day_wday, last_day_wday, days_in_month, start_offset, last_day_offset, padding_days, total_cells, rows_needed);

    return rows_needed;
}

/**
 * 隐藏日历中不需要的行
 * @param calendar_btnm 日历的buttonmatrix对象
 * @param rows_needed 需要显示的行数
 */
static void hide_unused_rows(lv_obj_t *calendar_btnm, int rows_needed)
{
    if (calendar_btnm == NULL) {
        CAL_LOG("[hide_unused_rows] ERROR: calendar_btnm is NULL");
        return;
    }

    CAL_LOG("[hide_unused_rows] Start: RowsNeeded=%d", rows_needed);

    // LVGL日历固定6行，每行7个按钮
    // 前7个是星期名称，从索引7开始是日期
    // 我们需要隐藏不需要的行

    // 首先，清除所有行的HIDDEN状态
    CAL_LOG("[hide_unused_rows] Clearing HIDDEN state for all rows");
    for (int i = 7; i < 7 + 6 * 7; i++) {
        lv_buttonmatrix_clear_button_ctrl(calendar_btnm, i, LV_BUTTONMATRIX_CTRL_HIDDEN);
    }

    // 计算需要隐藏的起始索引
    int first_hidden_row = rows_needed;  // 从第几行开始隐藏（0-based）
    int first_hidden_index = 7 + first_hidden_row * 7;  // buttonmatrix中的起始索引

    CAL_LOG("[hide_unused_rows] FirstHiddenRow=%d, FirstHiddenIndex=%d, MaxIndex=%d",
           first_hidden_row, first_hidden_index, 7 + 6 * 7);

    // 隐藏不需要的行
    if (first_hidden_index < 7 + 6 * 7) {
        CAL_LOG("[hide_unused_rows] Hiding buttons from index %d to %d", first_hidden_index, 7 + 6 * 7 - 1);
        for (int i = first_hidden_index; i < 7 + 6 * 7; i++) {
            lv_buttonmatrix_set_button_ctrl(calendar_btnm, i, LV_BUTTONMATRIX_CTRL_HIDDEN);

            // 验证是否设置成功
            bool is_hidden = lv_buttonmatrix_has_button_ctrl(calendar_btnm, i, LV_BUTTONMATRIX_CTRL_HIDDEN);
            if (!is_hidden) {
                CAL_LOG("[hide_unused_rows] ERROR: Failed to hide button at index %d", i);
            }
        }
    } else {
        CAL_LOG("[hide_unused_rows] No rows to hide (all 6 rows needed)");
    }

    CAL_LOG("[hide_unused_rows] Complete");
}

/**
 * 日历事件回调 - 用户点击日期时触发
 */
static void calendar_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        if (date_label == NULL || calendar == NULL) {
            CAL_LOG("date_label is NULL");
            return;
        }

        lv_calendar_date_t date;
        if (lv_calendar_get_pressed_date(calendar, &date)) {
            CAL_LOG("Selected date: %d-%02d-%02d", date.year, date.month, date.day);

            // 更新顶部日期显示
            char date_str[32];
            snprintf(date_str, sizeof(date_str), "%04d/%02d/%02d",
                     date.year, date.month, date.day);
            lv_label_set_text(date_label, date_str);

            // 重新高亮选中的日期
            lv_obj_t *calendar_btnm = lv_calendar_get_btnmatrix(calendar);
            if (calendar_btnm != NULL) {
                // 清除所有CHECKED状态
                for (uint32_t i = 7; i < 7 + 6 * 7; i++) {
                    lv_buttonmatrix_clear_button_ctrl(calendar_btnm, i, LV_BUTTONMATRIX_CTRL_CHECKED);
                }

                // 获取当前显示的月份第一天是星期几
                struct tm tm = {0};
                tm.tm_year = date.year - 1900;
                tm.tm_mon = date.month - 1;
                tm.tm_mday = 1;
                tm.tm_hour = 12;
                mktime(&tm);
                uint8_t first_day_of_week = tm.tm_wday;
                
                // 转换为周一开始的索引（周日=0 -> 6，周一=1 -> 0）
                int start_offset;
                if (first_day_of_week == 0) {
                    start_offset = 6;
                } else {
                    start_offset = first_day_of_week - 1;
                }
                
                // 计算选中日期在日历中的索引
                uint32_t day_index = 7 + start_offset + (date.day - 1);
                
                // 检查索引是否有效且日期在当前显示月份
                const lv_calendar_date_t *showed_date = lv_calendar_get_showed_date(calendar);
                if (day_index < 7 + 6 * 7 && showed_date && 
                    date.year == showed_date->year && date.month == showed_date->month) {
                    lv_buttonmatrix_set_button_ctrl(calendar_btnm, day_index, LV_BUTTONMATRIX_CTRL_CHECKABLE);
                    lv_buttonmatrix_set_button_ctrl(calendar_btnm, day_index, LV_BUTTONMATRIX_CTRL_CHECKED);
                }
            }
        }
    }
}

/**
 * 左箭头回调 - 显示上个月
 */
static void left_arrow_cb(lv_event_t *e)
{
    CAL_LOG("left arrow clicked");

    // 检查日历对象是否有效
    if (calendar == NULL) {
        CAL_LOG("calendar object is NULL");
        return;
    }

    // 获取当前显示的日期
    const lv_calendar_date_t *showed_date = lv_calendar_get_showed_date(calendar);
    if (showed_date) {
        lv_calendar_date_t new_date = *showed_date;

        CAL_LOG("[left_arrow_cb] Current showed date: %d-%02d", new_date.year, new_date.month);

        // 切换到上个月
        new_date.month--;
        if (new_date.month == 0) {
            new_date.month = 12;
            new_date.year--;
        }
        new_date.day = 1;

        CAL_LOG("[left_arrow_cb] Switching to: %d-%02d", new_date.year, new_date.month);

        // 计算新月份需要的行数并调整日历高度
        int rows_needed = calculate_month_rows(new_date.year, new_date.month);
        int calendar_height = 60 + rows_needed * 55;

        CAL_LOG("[left_arrow_cb] Rows needed: %d, Calendar height: %d", rows_needed, calendar_height);

        // 获取当前日历的高度
        int32_t old_height = lv_obj_get_height(calendar);
        CAL_LOG("[left_arrow_cb] Old calendar height: %d", old_height);

        lv_obj_set_height(calendar, calendar_height);

        // 验证设置后的高度
        int32_t new_height = lv_obj_get_height(calendar);
        CAL_LOG("[left_arrow_cb] New calendar height after set: %d", new_height);

        // 设置新的显示日期
        lv_calendar_set_showed_date(calendar, new_date.year, new_date.month);

        // 更新顶部日期显示为当月1号
        if (date_label != NULL) {
            char date_str[32];
            snprintf(date_str, sizeof(date_str), "%04d/%02d/01", new_date.year, new_date.month);
            lv_label_set_text(date_label, date_str);
        }

        // 清除所有CHECKED状态
        lv_obj_t *calendar_btnm = lv_calendar_get_btnmatrix(calendar);
        if (calendar_btnm != NULL) {
            for (uint32_t i = 7; i < 7 + 6 * 7; i++) {
                lv_buttonmatrix_clear_button_ctrl(calendar_btnm, i, LV_BUTTONMATRIX_CTRL_CHECKED);
            }

            // 隐藏不需要的行
            hide_unused_rows(calendar_btnm, rows_needed);
        }
    }
}

/**
 * 右箭头回调 - 显示下个月
 */
static void right_arrow_cb(lv_event_t *e)
{
    CAL_LOG("right arrow clicked");

    // 检查日历对象是否有效
    if (calendar == NULL) {
        CAL_LOG("calendar object is NULL");
        return;
    }

    // 获取当前显示的日期
    const lv_calendar_date_t *showed_date = lv_calendar_get_showed_date(calendar);
    if (showed_date) {
        lv_calendar_date_t new_date = *showed_date;

        CAL_LOG("[right_arrow_cb] Current showed date: %d-%02d", new_date.year, new_date.month);

        // 切换到下个月
        new_date.month++;
        if (new_date.month == 13) {
            new_date.month = 1;
            new_date.year++;
        }
        new_date.day = 1;

        CAL_LOG("[right_arrow_cb] Switching to: %d-%02d", new_date.year, new_date.month);

        // 计算新月份需要的行数并调整日历高度
        int rows_needed = calculate_month_rows(new_date.year, new_date.month);
        int calendar_height = 60 + rows_needed * 55;

        CAL_LOG("[right_arrow_cb] Rows needed: %d, Calendar height: %d", rows_needed, calendar_height);

        // 获取当前日历的高度
        int32_t old_height = lv_obj_get_height(calendar);
        CAL_LOG("[right_arrow_cb] Old calendar height: %d", old_height);

        lv_obj_set_height(calendar, calendar_height);

        // 验证设置后的高度
        int32_t new_height = lv_obj_get_height(calendar);
        CAL_LOG("[right_arrow_cb] New calendar height after set: %d", new_height);

        // 设置新的显示日期
        lv_calendar_set_showed_date(calendar, new_date.year, new_date.month);

        // 更新顶部日期显示为当月1号
        if (date_label != NULL) {
            char date_str[32];
            snprintf(date_str, sizeof(date_str), "%04d/%02d/01", new_date.year, new_date.month);
            lv_label_set_text(date_label, date_str);
        }

        // 清除所有CHECKED状态
        lv_obj_t *calendar_btnm = lv_calendar_get_btnmatrix(calendar);
        if (calendar_btnm != NULL) {
            for (uint32_t i = 7; i < 7 + 6 * 7; i++) {
                lv_buttonmatrix_clear_button_ctrl(calendar_btnm, i, LV_BUTTONMATRIX_CTRL_CHECKED);
            }

            // 隐藏不需要的行
            hide_unused_rows(calendar_btnm, rows_needed);
        }
    }
}

/**
 * 滑动手势处理 - 右滑返回
 */
static void slide_gesture_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;
        
        case LV_EVENT_RELEASED:
        {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);
            
            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;
            
            if(delta_x > 50 && abs(delta_x) > abs(delta_y)) {
                CAL_LOG("right swipe, exit calendar");
                if(calendar_base != NULL) {
                    // 将页面从页面栈弹出
                    vw_watch_pop_page(calendar_base);
                    lv_obj_del(calendar_base);
                    calendar_base = NULL;
                }
            }
            break;
        }
        default:
            break;
    }
}

/**
 * 页面创建回调
 */
static void app_calendar_create(void)
{
    CAL_LOG("calendar: create page");

    // 获取当前日期
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    int current_year = tm_info->tm_year + 1900;
    int current_month = tm_info->tm_mon + 1;
    int current_day = tm_info->tm_mday;
    CAL_LOG("current date: %d-%02d-%02d", current_year, current_month, current_day);

    // 创建日历页面基础容器
    calendar_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(calendar_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(calendar_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(calendar_base, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(calendar_base, 0, 0);
    lv_obj_align(calendar_base, LV_ALIGN_CENTER, 0, 0);
    
    // 设置手势事件
    lv_obj_add_event_cb(calendar_base, slide_gesture_handler, LV_EVENT_ALL, NULL);

    // 创建顶部日期显示
    date_label = lv_label_create(calendar_base);
    char init_date_str[32];
    snprintf(init_date_str, sizeof(init_date_str), "%04d/%02d/%02d",
             current_year, current_month, current_day);
    lv_label_set_text(date_label, init_date_str);
    lv_obj_set_style_text_font(date_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0, 30);

    // 创建日历控件
    calendar = lv_calendar_create(calendar_base);

    // 计算本月需要多少行并设置高度
    int rows_needed = calculate_month_rows(current_year, current_month);
    int calendar_height = 60 + rows_needed * 55;  // 60是星期头高度，55是每行日期高度

    CAL_LOG("Month %d-%02d needs %d rows, height %d",
           current_year, current_month, rows_needed, calendar_height);

    lv_obj_set_size(calendar, WATCH_SCREEN_WIDTH - 20, calendar_height);
    lv_obj_align(calendar, LV_ALIGN_TOP_MID, 0, 85);
    lv_obj_set_style_bg_color(calendar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(calendar, 0, 0);
    lv_obj_set_style_pad_all(calendar, 10, 0);

    // 创建左箭头 - 在日历之后创建，确保在最上层
    lv_obj_t *left_arrow = lv_img_create(calendar_base);
    lv_img_set_src(left_arrow, vw_resource_get_img("icon_calendar_left"));
    lv_obj_add_event_cb(left_arrow, left_arrow_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(left_arrow, 40, 40);
    lv_obj_align(left_arrow, LV_ALIGN_TOP_LEFT, 55, 31);
    lv_obj_add_flag(left_arrow, LV_OBJ_FLAG_CLICKABLE);

    // 创建右箭头 - 在日历之后创建，确保在最上层
    lv_obj_t *right_arrow = lv_img_create(calendar_base);
    lv_img_set_src(right_arrow, vw_resource_get_img("icon_calendar_right"));
    lv_obj_add_event_cb(right_arrow, right_arrow_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(right_arrow, 40, 40);
    lv_obj_align(right_arrow, LV_ALIGN_TOP_LEFT, 311, 31);
    lv_obj_add_flag(right_arrow, LV_OBJ_FLAG_CLICKABLE);

    // 获取日历内部的buttonmatrix对象
    lv_obj_t *calendar_btnm = lv_calendar_get_btnmatrix(calendar);

    // 设置星期头样式 - 使用中文（周一到周日）
    static const char * day_names[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    lv_calendar_set_day_names(calendar, day_names);

    // 设置buttonmatrix的基础样式
    lv_obj_set_style_bg_opa(calendar_btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(calendar_btnm, 0, 0);
    lv_obj_set_style_pad_all(calendar_btnm, 5, 0);

    // 设置星期名称样式（前7个按钮）
    lv_obj_set_style_text_color(calendar_btnm, lv_color_hex(0x8c9db5), LV_PART_ITEMS);
    lv_obj_set_style_text_font(calendar_btnm, vw_resource_get_font(WATCH_REGULAR_FONT "_16"), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(calendar_btnm, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(calendar_btnm, 0, LV_PART_ITEMS);

    // 设置日期数字样式 - 普通日期白色，背景透明
    lv_obj_set_style_text_font(calendar_btnm, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), LV_PART_ITEMS);
    lv_obj_set_style_text_color(calendar_btnm, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(calendar_btnm, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(calendar_btnm, 0, LV_PART_ITEMS);

    // 设置非本月日期样式 - 灰色显示
    lv_obj_set_style_text_color(calendar_btnm, lv_color_hex(0x666666), LV_PART_ITEMS | LV_STATE_DISABLED);

    // 设置今日日期样式 - 蓝色圆形背景
    lv_obj_set_style_bg_color(calendar_btnm, lv_color_hex(0x2D47CB), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(calendar_btnm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(calendar_btnm, 20, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(calendar_btnm, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(calendar_btnm, 0, LV_PART_ITEMS | LV_STATE_CHECKED);

    // 设置今天日期
    lv_calendar_date_t today;
    today.year = current_year;
    today.month = current_month;
    today.day = current_day;
    lv_calendar_set_today_date(calendar, today.year, today.month, today.day);

    // 设置当前显示的月份
    lv_calendar_set_showed_date(calendar, current_year, current_month);

    // 计算今日日期在日历中的索引
    struct tm tm = {0};
    tm.tm_year = current_year - 1900;
    tm.tm_mon = current_month - 1;
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    mktime(&tm);
    uint8_t first_day_of_week = tm.tm_wday;
    int start_offset = (first_day_of_week == 0) ? 6 : first_day_of_week - 1;
    uint32_t today_index = 7 + start_offset + (current_day - 1);

    // 设置今日日期高亮，并确保非本月日期保持DISABLED状态
    if (calendar_btnm != NULL) {
        // 只清除CHECKED状态，不清除DISABLED状态
        // DISABLED状态由LVGL日历控件自动管理，用于标记非本月日期
        for (uint32_t i = 7; i < 7 + 6 * 7; i++) {
            lv_buttonmatrix_clear_button_ctrl(calendar_btnm, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }

        // 为今日日期添加CHECKED状态
        if (today_index < 7 + 6 * 7) {
            lv_buttonmatrix_set_button_ctrl(calendar_btnm, today_index, LV_BUTTONMATRIX_CTRL_CHECKABLE);
            lv_buttonmatrix_set_button_ctrl(calendar_btnm, today_index, LV_BUTTONMATRIX_CTRL_CHECKED);
            CAL_LOG("Set today highlight at index %d", today_index);
        }

        // 隐藏不需要的行
        hide_unused_rows(calendar_btnm, rows_needed);
    }

    // 添加日期点击事件
    lv_obj_add_event_cb(calendar, calendar_event_handler, LV_EVENT_VALUE_CHANGED, NULL);

    // 让日历能够接收手势冒泡
    lv_obj_add_flag(calendar, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 将页面压入页面栈
    vw_watch_push_page(calendar_base);

    CAL_LOG("calendar: create complete");
}

void calendar_app_click_callback(lv_event_t *e)
{
    CAL_LOG("calendar app clicked");
    app_calendar_create();
}
#include "settings.h"
#include "../launcher/launcher.h"
#include <stdio.h>
#include <math.h>
#include <nuttx/power/axp2101.h>
#include <syslog.h>

#define BATTERY_LOG(fmt, ...) syslog(LOG_INFO, "[BATTERY] " fmt, ##__VA_ARGS__)

/* UI对象 */
static lv_obj_t* battery_base = NULL;
static lv_obj_t* battery_arc = NULL;
static lv_obj_t* battery_label = NULL;
static lv_obj_t* charging_base = NULL;
static lv_obj_t* charging_label = NULL;
static lv_obj_t* battery_frame = NULL;
static lv_obj_t* bars[3] = {NULL};
static lv_obj_t* percentage_label = NULL;
static lv_obj_t* remaining_time_label = NULL;

/* 电量信息 */
static uint8_t battery_soc = 0;

/* 可用时间估算 */
static float estimated_discharge_current = 0.0f;
static uint8_t prev_soc_for_estimation = 0;
static int64_t prev_time_ms_for_estimation = 0;
static const float battery_full_capacity_mah = 400.0f;
static const float default_discharge_current_ma = 80.0f;

/* 函数声明 */
static void slide_gesture_handler(lv_event_t *e);
static void update_battery_info(void);
static void blink_timer_cb(lv_timer_t *timer);
static void battery_update_timer_cb(lv_timer_t *timer);

/* 定时器 */
static lv_timer_t *blink_timer = NULL;
static lv_timer_t *battery_update_timer = NULL;
static int blink_bar_index = 0;
static bool blink_state = false;  // 闪烁状态：true=高亮，false=暗淡

static void update_battery_info(void)
{
    // 获取实际电量
    battery_soc = axp2101_get_pmu_soc();
    BATTERY_LOG("Battery SOC: %d%%", battery_soc);
}

static void blink_timer_cb(lv_timer_t *timer)
{
    if (charging_base == NULL) {
        return;
    }
    
    // 实时获取电量
    battery_soc = axp2101_get_pmu_soc();
    
    // 更新百分比显示
    if (percentage_label != NULL) {
        char buf[10];
        sprintf(buf, "%d%%", battery_soc);
        lv_label_set_text(percentage_label, buf);
    }
    
    // 计算已填满的格子数 (0-3)
    // 0-33%: bars_filled=0, 34-66%: bars_filled=1, 67-99%: bars_filled=2, 100%: bars_filled=3
    uint8_t bars_filled = (battery_soc * 3) / 100;
    if (bars_filled > 3) bars_filled = 3;
    
    // 只有未满100%才闪烁
    if (bars_filled < 3) {
        // 确保闪烁索引在未填满区域内
        if (blink_bar_index < bars_filled) {
            blink_bar_index = bars_filled;
        }
        
        // 闪烁逻辑：未填满的格子逐个闪烁
        for (int i = 0; i < 3; i++) {
            if (i < bars_filled) {
                // 已填满的格子保持绿色
                lv_obj_set_style_bg_color(bars[i], lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
                lv_obj_set_style_bg_opa(bars[i], LV_OPA_100, LV_STATE_DEFAULT);
            } else {
                // 未填满的格子根据闪烁状态变色
                // blink_state == true 时，当前索引的格子高亮，其他暗淡
                // blink_state == false 时，所有未填满格子都暗淡
                if (blink_state && i == blink_bar_index) {
                    lv_obj_set_style_bg_color(bars[i], lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(bars[i], LV_OPA_100, LV_STATE_DEFAULT);
                } else {
                    lv_obj_set_style_bg_color(bars[i], lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_opa(bars[i], LV_OPA_60, LV_STATE_DEFAULT);
                }
            }
        }
        
        // 切换闪烁状态
        blink_state = !blink_state;
        
        // 如果刚刚从高亮切换到暗淡，移动到下一个要闪烁的格子
        if (!blink_state) {
            blink_bar_index++;
            if (blink_bar_index >= 3) {
                blink_bar_index = bars_filled;
            }
        }
    } else {
        // 100%时所有格子都保持绿色，不闪烁
        for (int i = 0; i < 3; i++) {
            lv_obj_set_style_bg_color(bars[i], lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(bars[i], LV_OPA_100, LV_STATE_DEFAULT);
        }
    }
}

static void battery_update_timer_cb(lv_timer_t *timer)
{
    if (battery_base == NULL) {
        return;
    }
    
    battery_soc = axp2101_get_pmu_soc();
    BATTERY_LOG("get cur battery value: %d", battery_soc);
    
    if (battery_label != NULL) {
        char buf[10];
        sprintf(buf, "%d%%", battery_soc);
        lv_label_set_text(battery_label, buf);
    }
    
    if (battery_arc != NULL) {
        lv_arc_set_value(battery_arc, battery_soc);
    }

    int64_t now_ms = (int64_t)lv_tick_get();
    if (prev_time_ms_for_estimation == 0) {
        prev_soc_for_estimation = battery_soc;
        prev_time_ms_for_estimation = now_ms;
    } else {
        int64_t elapsed_ms = now_ms - prev_time_ms_for_estimation;
        if (elapsed_ms > 5000) {
            if (prev_soc_for_estimation > battery_soc) {
                uint8_t soc_drop = prev_soc_for_estimation - battery_soc;
                float consumed_mah = (soc_drop / 100.0f) * battery_full_capacity_mah;
                float elapsed_hours = (float)elapsed_ms / 3600000.0f;
                float instant_current = consumed_mah / elapsed_hours;
                if (instant_current > 5.0f) {
                    estimated_discharge_current = instant_current;
                }
            }
            prev_soc_for_estimation = battery_soc;
            prev_time_ms_for_estimation = now_ms;
        }
    }

    if (remaining_time_label != NULL) {
        char time_buf[32];
        if (battery_soc == 0) {
            snprintf(time_buf, sizeof(time_buf), "可用时间：0h 0min");
        } else {
            float discharge_current = (estimated_discharge_current > 5.0f) ?
                                       estimated_discharge_current : default_discharge_current_ma;
            float remaining_capacity = (battery_soc / 100.0f) * battery_full_capacity_mah;
            float remaining_hours = remaining_capacity / discharge_current;
            char hmin_buf[16];
            axp2101_convert_hours_to_hmin(remaining_hours, hmin_buf, sizeof(hmin_buf));
            snprintf(time_buf, sizeof(time_buf), "可用时间：%s", hmin_buf);
        }
        lv_label_set_text(remaining_time_label, time_buf);
    }
}

void setting_charging_create(void)
{
    // 如果充电界面已存在，先销毁
    if (charging_base != NULL) {
        lv_obj_del(charging_base);
        charging_base = NULL;
    }

    // 创建充电页面
    charging_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(charging_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(charging_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(charging_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(charging_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(charging_base, LV_ALIGN_CENTER, 0, 0);

    // 创建"充电中"标题
    charging_label = lv_label_create(charging_base);
    lv_label_set_text(charging_label, "充电中");
    lv_obj_set_style_text_font(charging_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(charging_label, lv_color_white(), 0);
    lv_obj_align(charging_label, LV_ALIGN_TOP_MID, 0, 26);

    // 创建帽子形状 (41x18, 颜色2BEA77, Y=133)
    // 位于矩形框上方10px，像一个帽子盖在上面
    // 使用两个obj组合：上面半圆 + 下面矩形，实现只有上方两个圆角的效果
    
    // 创建上方半圆部分 (41x9)
    lv_obj_t *cap_top = lv_obj_create(charging_base);
    lv_obj_set_size(cap_top, 41, 18);
    lv_obj_align(cap_top, LV_ALIGN_TOP_MID, 0, 133);
    lv_obj_set_style_bg_color(cap_top, lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
    // 上方两个角设置圆角
    lv_obj_set_style_radius(cap_top, 12, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cap_top, 0, LV_STATE_DEFAULT);
    
    // 创建下方矩形部分 (41x9)，与上方半圆紧密连接
    lv_obj_t *cap_bottom = lv_obj_create(charging_base);
    lv_obj_set_size(cap_bottom, 41, 9);
    lv_obj_align_to(cap_bottom, cap_top, LV_ALIGN_OUT_BOTTOM_MID, 0, -9);
    lv_obj_set_style_bg_color(cap_bottom, lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
    // 下方两个角不设置圆角
    lv_obj_set_style_radius(cap_bottom, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cap_bottom, 0, LV_STATE_DEFAULT);

    // 创建空心矩形外框 (114x137, Y=151, 颜色2BEA77, 外边距18px, 圆角14px)
    // Y位置调整为半圆下方紧密连接
    battery_frame = lv_obj_create(charging_base);
    lv_obj_set_size(battery_frame, 114, 137);
    lv_obj_align(battery_frame, LV_ALIGN_TOP_MID, 0, 161);
    lv_obj_set_style_border_color(battery_frame, lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(battery_frame, 18, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(battery_frame, 14, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(battery_frame, LV_OPA_TRANSP, LV_STATE_DEFAULT);

    // 创建三个横条 (45x18)
    // 从下到上排列，间距25px
    int bar_y_offset[] = {3, 28, 53};  // 从下到上的位置，调整间距使三个都可见
    
    for (int i = 0; i < 3; i++) {
        bars[i] = lv_obj_create(battery_frame);
        lv_obj_set_size(bars[i], 45, 18);
        lv_obj_align(bars[i], LV_ALIGN_BOTTOM_MID, 0, -bar_y_offset[i]);
        lv_obj_set_style_radius(bars[i], 4, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(bars[i], 0, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(bars[i], 0, LV_STATE_DEFAULT);
    }

    // 更新电量显示
    update_battery_info();
    
    // 更新电量条状态
    uint8_t bars_filled = (battery_soc * 3) / 100;
    if (bars_filled > 3) bars_filled = 3;
    
    for (int i = 0; i < 3; i++) {
        if (i < bars_filled) {
            lv_obj_set_style_bg_color(bars[i], lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
        } else {
            lv_obj_set_style_bg_color(bars[i], lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(bars[i], LV_OPA_60, LV_STATE_DEFAULT);
        }
    }

    // 创建电量百分比显示 (45%, Y=348)
    percentage_label = lv_label_create(charging_base);
    char buf[10];
    sprintf(buf, "%d%%", battery_soc);
    lv_label_set_text(percentage_label, buf);
    lv_obj_set_style_text_font(percentage_label, vw_resource_get_font(WATCH_REGULAR_FONT "_48"), 0);
    lv_obj_set_style_text_color(percentage_label, lv_color_white(), 0);
    lv_obj_align(percentage_label, LV_ALIGN_TOP_MID, 0, 348);

    // 启动闪烁定时器 (1秒更新一次)
    blink_bar_index = 0;
    if (blink_timer != NULL) {
        lv_timer_del(blink_timer);
    }
    blink_timer = lv_timer_create(blink_timer_cb, 1000, NULL);

    BATTERY_LOG("charging: create complete");
}

void setting_charging_destroy(void)
{
    // 停止闪烁定时器
    if (blink_timer != NULL) {
        lv_timer_del(blink_timer);
        blink_timer = NULL;
    }

    // 删除充电界面
    if (charging_base != NULL) {
        lv_obj_del(charging_base);
        charging_base = NULL;
    }
    
    BATTERY_LOG("charging: destroy complete");
}

void setting_battery_destroy(void)
{
    // 将页面从页面栈弹出
    vw_watch_pop_page(battery_base);
    // 停止电量更新定时器
    if (battery_update_timer != NULL) {
        lv_timer_del(battery_update_timer);
        battery_update_timer = NULL;
    }

    // 删除电量页面
    if (battery_base != NULL) {
        lv_obj_del(battery_base);
        battery_base = NULL;
    }
}

static void setting_battery_create(void)
{
    // 创建电量页面
    battery_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(battery_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(battery_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(battery_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(battery_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(battery_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(battery_base);
    lv_label_set_text(title_label, "电量");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 26);

    // 获取实际电量
    update_battery_info();

    // 创建环形进度条
    battery_arc = lv_arc_create(battery_base);
    lv_obj_set_size(battery_arc, 230, 230);
    lv_obj_align(battery_arc, LV_ALIGN_TOP_MID, 0, 131);
    lv_arc_set_range(battery_arc, 0, 100);
    lv_arc_set_value(battery_arc, battery_soc);
    lv_arc_set_rotation(battery_arc, 270);
    lv_arc_set_bg_angles(battery_arc, 0, 360);
    lv_obj_set_style_arc_color(battery_arc, lv_color_hex(0x2BEA77), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(battery_arc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(battery_arc, 19, LV_PART_MAIN);
    lv_obj_set_style_arc_width(battery_arc, 19, LV_PART_INDICATOR);

    // 禁用arc的拖动功能和移除旋钮
    lv_obj_clear_flag(battery_arc, LV_OBJ_FLAG_CLICKABLE);  // 禁用点击
    lv_obj_remove_style(battery_arc, NULL, LV_PART_KNOB);  // 移除旋钮样式

    // 创建剩余标题
    lv_obj_t *remaining_title_label = lv_label_create(battery_base);
    lv_label_set_text(remaining_title_label, "剩余");
    lv_obj_set_style_text_font(remaining_title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), 0);
    lv_obj_set_style_text_color(remaining_title_label, lv_color_white(), 0);
    lv_obj_align_to(remaining_title_label, battery_arc, LV_ALIGN_TOP_MID, 0, 54);

    // 创建电量显示
    battery_label = lv_label_create(battery_arc);
    char buf[10];
    sprintf(buf, "%d%%", battery_soc);
    lv_label_set_text(battery_label, buf);
    lv_obj_set_style_text_font(battery_label, vw_resource_get_font(WATCH_REGULAR_FONT "_48"), 0);
    lv_obj_set_style_text_color(battery_label, lv_color_white(), 0);
    lv_obj_center(battery_label);

    // 启动电量更新定时器 (2秒更新一次)
    if (battery_update_timer != NULL) {
        lv_timer_del(battery_update_timer);
    }
    battery_update_timer = lv_timer_create(battery_update_timer_cb, 2000, NULL);

    estimated_discharge_current = 0.0f;
    prev_soc_for_estimation = 0;
    prev_time_ms_for_estimation = 0;

    remaining_time_label = lv_label_create(battery_base);
    {
        char time_buf[32];
        float remaining_capacity = (battery_soc / 100.0f) * battery_full_capacity_mah;
        float remaining_hours = remaining_capacity / default_discharge_current_ma;
        char hmin_buf[16];
        axp2101_convert_hours_to_hmin(remaining_hours, hmin_buf, sizeof(hmin_buf));
        snprintf(time_buf, sizeof(time_buf), "可用时间：%s", hmin_buf);
        lv_label_set_text(remaining_time_label, time_buf);
    }
    lv_obj_set_style_text_font(remaining_time_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(remaining_time_label, lv_color_white(), 0);
    lv_obj_align(remaining_time_label, LV_ALIGN_TOP_MID, 0, 403);

    // 将页面压入页面栈
    vw_watch_push_page(battery_base);
    // 添加滑动手势处理
    lv_obj_add_event_cb(battery_base, slide_gesture_handler, LV_EVENT_ALL, NULL);

    BATTERY_LOG("battery: create complete");
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
                    setting_battery_destroy();
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

void settings_battery_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        BATTERY_LOG("Settings battery button click.");
        setting_battery_create();
    }
}

bool setting_charging_is_active(void)
{
    return charging_base != NULL;
}

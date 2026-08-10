/**
 * @file settings_display.c
 * 设置-显示页面
 */

#include "settings.h"
#include "../common/watch_pages.h"
#include "../common/sensor_data.h"
#include "../common/display_compat.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include <stdio.h>
#include <nuttx/lcd/co5300.h>
#include <../../../../apps/graphics/lvgl/lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.h>

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define DISPLAY_LOG(fmt, ...) printf("[DISPLAY] " fmt "\n", ##__VA_ARGS__)
#else
#  define DISPLAY_LOG(fmt, ...)
#endif

/* UI对象 */
static lv_obj_t* display_base = NULL;
static lv_obj_t* brightness_container = NULL;
static lv_obj_t* brightness_indicator = NULL;
static lv_obj_t* timeout_base = NULL;
static lv_obj_t* timeout_roller = NULL;

/* 亮度等级 */
static int current_level = 2; // 默认中等亮度
/* 抬腕亮屏状态 */
static bool wrist_raise_enabled = false;
/* 熄屏时间 (秒) */
static int screen_timeout = 10;

static void settings_display_init_timeout(void)
{
    screen_timeout = display_get_timeout();
    if (screen_timeout < 5 || screen_timeout > 60) {
        screen_timeout = 10;
    }
}

static void slide_gesture_handler(lv_event_t *e);
static void brightness_bar_event_cb(lv_event_t *e);
static void set_brightness(int level);
static void settings_display_timeout_create(void);
static void slide_gesture_setting_display_timeout_handler(lv_event_t *e);
static void wrist_raise_switch_event_cb(lv_event_t *e);
static void timeout_item_click_handler(lv_event_t *e);
static void timeout_roller_confirm_handler(lv_event_t *e);
static void timeout_roller_cancel_handler(lv_event_t *e);


static void settings_display_create(void)
{
    settings_display_init_timeout();
    wrist_raise_load_config();
    wrist_raise_enabled = wrist_raise_get_enabled();
    // 获取当前亮度等级
    current_level = esp32s3_get_brightness();
    DISPLAY_LOG("settings get cur brightness level:%d", current_level);
    if (current_level < 0 || current_level > 4)
    {
        current_level = 2;  // 默认中等亮度
    }
    
    // 创建显示设置页面
    display_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(display_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(display_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(display_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(display_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(display_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(display_base);
    lv_label_set_text(title_label, "设置亮度");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 26);

    // 创建亮度调节容器 (横向)
    brightness_container = lv_obj_create(display_base);
    lv_obj_set_size(brightness_container, WATCH_BTN_WIDTH, 70);
    lv_obj_set_style_bg_color(brightness_container, lv_color_hex(0xFDFDFD), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(brightness_container, LV_OPA_20, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(brightness_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(brightness_container, 32, LV_STATE_DEFAULT);
    lv_obj_align(brightness_container, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_scrollbar_mode(brightness_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(brightness_container, LV_DIR_NONE);
    lv_obj_set_style_pad_all(brightness_container, 0, 0);
    lv_obj_add_event_cb(brightness_container, brightness_bar_event_cb, LV_EVENT_ALL, NULL);

    // 创建亮度指示器 (横向)
    brightness_indicator = lv_obj_create(brightness_container);
    lv_obj_set_width(brightness_indicator, WATCH_BTN_WIDTH * (current_level + 1) / 5);
    lv_obj_set_height(brightness_indicator, 70);
    lv_obj_set_style_bg_color(brightness_indicator, lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(brightness_indicator, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(brightness_indicator, 32, LV_STATE_DEFAULT);
    lv_obj_align(brightness_indicator, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(brightness_indicator, LV_OBJ_FLAG_CLICKABLE);

    // 创建熄屏时间设置项
    static lv_style_t item_style;
    static bool style_initialized = false;
    if (!style_initialized) {
        lv_style_init(&item_style);
        lv_style_set_bg_opa(&item_style, LV_OPA_10);
        lv_style_set_bg_color(&item_style, lv_color_hex(0xFFFFFF));
        lv_style_set_border_width(&item_style, 0);
        lv_style_set_radius(&item_style, 32);
        lv_style_set_pad_all(&item_style, 0);
        lv_style_set_outline_width(&item_style, 0);
        lv_style_set_transform_width(&item_style, 0);
        lv_style_set_transform_height(&item_style, 0);
        lv_style_set_translate_x(&item_style, 0);
        lv_style_set_translate_y(&item_style, 0);
        style_initialized = true;
    }

    static lv_style_t pressed_style;
    static bool pressed_style_initialized = false;
    if (!pressed_style_initialized) {
        lv_style_init(&pressed_style);
        lv_style_set_transform_width(&pressed_style, 0);
        lv_style_set_transform_height(&pressed_style, 0);
        lv_style_set_translate_x(&pressed_style, 0);
        lv_style_set_translate_y(&pressed_style, 0);
        lv_style_set_bg_opa(&pressed_style, LV_OPA_20);
        lv_style_set_bg_color(&pressed_style, lv_color_hex(0xFFFFFF));
        pressed_style_initialized = true;
    }

    // 创建抬腕亮屏按钮
    lv_obj_t *wrist_raise_item = lv_obj_create(display_base);
    lv_obj_set_size(wrist_raise_item, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(wrist_raise_item, &item_style, 0);
    lv_obj_add_style(wrist_raise_item, &pressed_style, LV_STATE_PRESSED);
    lv_obj_align(wrist_raise_item, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_clear_flag(wrist_raise_item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wrist_raise_item, LV_OBJ_FLAG_EVENT_BUBBLE);
    
    // 抬腕亮屏文字
    lv_obj_t *wrist_raise_label = lv_label_create(wrist_raise_item);
    lv_label_set_text(wrist_raise_label, "抬腕亮屏");
    lv_obj_set_style_text_color(wrist_raise_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(wrist_raise_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_pos(wrist_raise_label, 18, (WATCH_BTN_HEIGHT - 32) / 2);
    lv_obj_clear_flag(wrist_raise_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 创建抬腕亮屏开关
    lv_obj_t *wrist_switch = lv_switch_create(wrist_raise_item);
    lv_obj_set_size(wrist_switch, 57, 32);
    lv_obj_set_style_bg_color(wrist_switch, lv_color_hex(0x4D4D4D), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(wrist_switch, lv_color_hex(0x2BEA77), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(wrist_switch, lv_color_hex(0x4D4D4D), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(wrist_switch, lv_color_hex(0x2BEA77), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(wrist_switch, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(wrist_switch, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_add_event_cb(wrist_switch, wrist_raise_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (wrist_raise_enabled) {
        lv_obj_add_state(wrist_switch, LV_STATE_CHECKED);
    }
    lv_obj_set_pos(wrist_switch, WATCH_BTN_WIDTH - 57 - 18, (WATCH_BTN_HEIGHT - 32) / 2);
    lv_obj_clear_flag(wrist_switch, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *timeout_item = lv_obj_create(display_base);
    lv_obj_set_size(timeout_item, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_add_style(timeout_item, &item_style, 0);
    lv_obj_add_style(timeout_item, &pressed_style, LV_STATE_PRESSED);
    lv_obj_align(timeout_item, LV_ALIGN_TOP_MID, 0, 314);
    lv_obj_clear_flag(timeout_item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(timeout_item, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(timeout_item, timeout_item_click_handler, LV_EVENT_CLICKED, NULL);
    
    // 熄屏时间文字
    lv_obj_t *timeout_label = lv_label_create(timeout_item);
    lv_label_set_text(timeout_label, "熄屏时间");
    lv_obj_set_style_text_color(timeout_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(timeout_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_pos(timeout_label, 18, (WATCH_BTN_HEIGHT - 32) / 2);
    lv_obj_clear_flag(timeout_label, LV_OBJ_FLAG_SCROLLABLE);
    
    // 添加箭头图标
    lv_obj_t *timeout_arrow = lv_img_create(timeout_item);
    lv_img_set_src(timeout_arrow, vw_resource_get_img("icon_sport_right_arrow"));
    lv_obj_set_pos(timeout_arrow, WATCH_BTN_WIDTH - 18 - 44, (WATCH_BTN_HEIGHT - 44) / 2);
    lv_obj_clear_flag(timeout_arrow, LV_OBJ_FLAG_SCROLLABLE);

    // 添加滑动手势处理
    lv_obj_add_event_cb(display_base, slide_gesture_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(display_base);
}

/**
 * 显示设置事件回调
 */
void settings_display_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        settings_display_create();
    }
}

/**
 * 亮度调节条事件回调
 */
static void brightness_bar_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        // 获取触摸位置
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        
        // 获取容器的位置和大小
        lv_area_t container_area;
        lv_obj_get_coords(brightness_container, &container_area);
        
        // 计算容器内的相对坐标
        lv_point_t container_point;
        container_point.x = point.x - container_area.x1;
        container_point.y = point.y - container_area.y1;
        
        // 限制在容器范围内
        if (container_point.x < 0) container_point.x = 0;
        int container_width = lv_obj_get_width(brightness_container);
        if (container_point.x > container_width) container_point.x = container_width;
        
        // 计算亮度等级 (0-4)
        int new_level = (container_point.x * 5) / container_width;
        if (new_level < 0) new_level = 0;
        if (new_level > 4) new_level = 4;
        
        // 更新亮度
        if (new_level != current_level) {
            current_level = new_level;
            lv_obj_set_width(brightness_indicator, container_width * (current_level + 1) / 5);
            lv_obj_align(brightness_indicator, LV_ALIGN_LEFT_MID, 0, 0);
            set_brightness(current_level);
        }
    }
}

/**
 * 设置亮度
 */
static void set_brightness(int level)
{
    if (level >= 0 && level < 5) {
        esp32s3_set_brightness(level);
        DISPLAY_LOG("Set brightness to level %d", level);
    }
}

/**
 * 抬腕亮屏开关事件回调
 */
static void wrist_raise_switch_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    wrist_raise_enabled = lv_obj_has_state(obj, LV_STATE_CHECKED);
    DISPLAY_LOG("Wrist raise enabled: %d", wrist_raise_enabled);
    wrist_raise_set_enabled(wrist_raise_enabled);
}

/**
 * 熄屏时间设置项点击处理
 */
static void timeout_item_click_handler(lv_event_t *e)
{
    settings_display_timeout_create();
}

/**
 * 创建熄屏时间设置页面
 */
static void settings_display_timeout_create(void)
{
    // 如果页面已存在，先销毁
    if (timeout_base != NULL) {
        lv_obj_del(timeout_base);
        timeout_base = NULL;
    }

    // 创建熄屏时间设置页面
    timeout_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(timeout_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(timeout_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(timeout_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(timeout_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(timeout_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    // lv_obj_t *title_label = lv_label_create(timeout_base);
    // lv_label_set_text(title_label, "熄屏时间");
    // lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    // lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    // lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    // 创建滚轮 (5秒-60秒)
    timeout_roller = lv_roller_create(timeout_base);
    char timeout_buf[400] = {0};
    for (int i = 5; i <= 60; i += 5) {
        char timeout_str[20];
        sprintf(timeout_str, "%d秒\n", i);
        strcat(timeout_buf, timeout_str);
    }
    // 去掉最后一个换行符
    timeout_buf[strlen(timeout_buf)-1] = '\0';
    
    lv_roller_set_options(timeout_roller, timeout_buf, LV_ROLLER_MODE_INFINITE);
    // 计算当前选中的索引 (screen_timeout / 5 - 1)
    int selected_idx = (screen_timeout / 5) - 1;
    if (selected_idx < 0) selected_idx = 0;
    if (selected_idx > 11) selected_idx = 11;
    lv_roller_set_selected(timeout_roller, selected_idx, LV_ANIM_OFF);
    lv_obj_set_width(timeout_roller, WATCH_BTN_WIDTH);
    lv_obj_align(timeout_roller, LV_ALIGN_TOP_MID, 0, 36);
    
    // 设置滚轮样式
    lv_obj_set_style_border_color(timeout_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(timeout_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(timeout_roller, LV_OPA_10, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(timeout_roller, lv_color_hex(0xFFFFFF), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(timeout_roller, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(timeout_roller, lv_color_hex(0x696969), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(timeout_roller, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *timeout_roller_font = vw_resource_get_font(WATCH_REGULAR_FONT "_36");
    lv_obj_set_style_text_font(timeout_roller, timeout_roller_font, 0);
    lv_obj_set_style_radius(timeout_roller, 32, LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_roller_set_visible_row_count(timeout_roller, 3);
    int32_t timeout_line_h = lv_font_get_line_height(timeout_roller_font);
    lv_obj_set_style_text_line_space(timeout_roller, WATCH_BTN_HEIGHT - timeout_line_h, LV_PART_MAIN);
    lv_obj_set_height(timeout_roller, WATCH_BTN_HEIGHT * 3);
    lv_obj_clear_flag(timeout_roller, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(timeout_roller, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 创建确认按钮
    lv_obj_t *confirm_btn = lv_btn_create(timeout_base);
    lv_obj_add_event_cb(confirm_btn, timeout_roller_confirm_handler, LV_EVENT_CLICKED, NULL);
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
    lv_obj_t *cancel_btn = lv_btn_create(timeout_base);
    lv_obj_add_event_cb(cancel_btn, timeout_roller_cancel_handler, LV_EVENT_CLICKED, NULL);
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
    lv_obj_add_event_cb(timeout_base, slide_gesture_setting_display_timeout_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(timeout_base);
}

/**
 * 熄屏时间确认按钮事件回调
 */
static void timeout_roller_confirm_handler(lv_event_t *e)
{
    uint16_t selected = lv_roller_get_selected(timeout_roller);
    screen_timeout = (selected + 1) * 5;
    display_set_timeout(screen_timeout);
    DISPLAY_LOG("Set screen timeout to %d seconds", screen_timeout);
    
    // 退出页面
    if (timeout_base != NULL) {
        vw_watch_pop_page(timeout_base);
        lv_obj_del(timeout_base);
        timeout_base = NULL;
    }
}

static void timeout_roller_cancel_handler(lv_event_t *e)
{
    DISPLAY_LOG("Set screen cancel.");
    // 退出页面
    if (timeout_base != NULL) {
        vw_watch_pop_page(timeout_base);
        lv_obj_del(timeout_base);
        timeout_base = NULL;
    }
}


/**
 * 熄屏时间页面滑动手势处理
 */
static void slide_gesture_setting_display_timeout_handler(lv_event_t *e)
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
                    if(timeout_base != NULL) {
                        vw_watch_pop_page(timeout_base);
                        lv_obj_del(timeout_base);
                        timeout_base = NULL;
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
                    if(display_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(display_base);
                        lv_obj_del(display_base);
                        display_base = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

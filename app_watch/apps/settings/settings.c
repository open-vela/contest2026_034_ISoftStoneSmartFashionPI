/**
 * @file settings.c
 */

#include "settings.h"
#include "../common/watch_pages.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include <stdio.h>
#include <syslog.h>

#define SETTINGS_LOG(fmt, ...) syslog(LOG_INFO, "[SETTINGS] " fmt, ##__VA_ARGS__)


static lv_obj_t * settings_base = NULL;
static lv_obj_t * wifi_btn = NULL;

static lv_obj_t * time_btn = NULL;
static lv_obj_t * date_btn = NULL;
static lv_obj_t * battery_btn = NULL;
static lv_obj_t * volume_btn = NULL;
static lv_obj_t * display_btn = NULL;
static lv_obj_t * about_btn = NULL;
static lv_obj_t * system_btn = NULL;
static lv_obj_t * ui_mode_btn = NULL;

static lv_style_t style_base;
static lv_style_t btn_style;
static lv_style_t style_list;

static void settings_app_create(void);

static void slide_gesture_handler(lv_event_t *e);
static void setup_settings_button(lv_obj_t *btn);

void settings_app_click_callback(lv_event_t *e)
{
  SETTINGS_LOG("settings app clicked");
  settings_app_create();			// app界面创建
}

static void settings_app_create(void)
{
    settings_base = lv_obj_create(lv_scr_act());
    lv_style_init(&style_base);
    lv_style_set_bg_opa(&style_base, LV_OPA_COVER); // 设置背景透明
    lv_style_set_bg_color(&style_base, lv_color_hex(0x000000)); // 设置选中状态的背景颜色
    lv_style_set_border_width(&style_base, 0);
    lv_style_set_radius(&style_base, 0);
    lv_obj_add_style(settings_base, &style_base,0);
    lv_obj_clear_flag(settings_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(settings_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_align(settings_base,LV_ALIGN_CENTER, 0, 0);
    // lv_obj_set_scroll_dir(settings_base, LV_DIR_HOR);

	lv_obj_t *list = lv_list_create(settings_base);
    /* list宽度 = 屏幕宽度410，高度 = 屏幕高度502 */
    lv_obj_set_size(list, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_style_init(&style_list);
    lv_style_set_bg_opa(&style_list, LV_OPA_COVER); // 设置背景不透明
    lv_style_set_bg_color(&style_list, lv_color_hex(0x000000)); // 设置背景颜色为黑色
    lv_style_set_border_width(&style_list, 0);
    lv_style_set_radius(&style_list, 0);  /* 圆角设为0，避免边界效果 */
    lv_obj_add_style(list, &style_list, 0);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_all(list, 0, 0);
    
    /* 禁用滚动边界效果，避免灰色影印 */
    lv_obj_set_scroll_snap_x(list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_NONE);
    
    /* 计算左右padding使按钮居中：(410 - 340) / 2 = 35px */
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 16, 0);     /* 按钮间距16px */
    
    wifi_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_wifi"), "WIFI");

    time_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_time"), "时间");
    date_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_date"), "日期");
    battery_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_battery"), "电池");
    volume_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_volume_ring"), "声音");
    display_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_light"), "显示");
    ui_mode_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_ui_replace"), "潮玩模式");
    system_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_system"), "关机与重启");
    about_btn = lv_list_add_btn(list, vw_resource_get_img("icon_set_about"), "关于");
    
    setup_settings_button(wifi_btn);

    setup_settings_button(time_btn);
    setup_settings_button(date_btn);
    setup_settings_button(battery_btn);
    setup_settings_button(volume_btn);
    setup_settings_button(display_btn);
    setup_settings_button(about_btn);
    setup_settings_button(system_btn);
    setup_settings_button(ui_mode_btn);

	lv_obj_add_event_cb(wifi_btn, settings_wifi_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(time_btn, settings_timeset_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(date_btn, settings_dateset_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(battery_btn, settings_battery_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(volume_btn, settings_volume_ring_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(display_btn, settings_display_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(about_btn, settings_about_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(system_btn, settings_system_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_mode_btn, settings_ui_mode_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(settings_base, slide_gesture_handler,LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(settings_base);
}

static void setup_settings_button(lv_obj_t *btn) 
{
    /* 获取按钮中的图标和文本 */
    lv_obj_t * btn_img = lv_obj_get_child(btn, 0);  // 获取图标
    lv_obj_t * btn_label = lv_obj_get_child(btn, 1); // 获取文本标签
    
    /* 设置按钮样式 */
    static bool style_initialized = false;
    if (!style_initialized) {
        lv_style_init(&btn_style);
        lv_style_set_bg_opa(&btn_style, LV_OPA_10); 
        lv_style_set_bg_color(&btn_style, lv_color_hex(0xFFFFFF));
        lv_style_set_text_color(&btn_style, lv_color_white());
        lv_style_set_text_font(&btn_style, vw_resource_get_font(WATCH_REGULAR_FONT "_32"));
        lv_style_set_text_letter_space(&btn_style, 2); // 设置字符间距为2像素
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

    /* 为按下状态单独设置样式，确保没有变换效果 */
    static lv_style_t btn_pressed_style;
    static bool pressed_style_initialized = false;
    if (!pressed_style_initialized) {
        lv_style_init(&btn_pressed_style);
        lv_style_set_bg_opa(&btn_pressed_style, LV_OPA_10);
        lv_style_set_bg_color(&btn_pressed_style, lv_color_hex(0xFFFFFF));
        lv_style_set_text_color(&btn_pressed_style, lv_color_white());
        lv_style_set_transform_width(&btn_pressed_style, 0);
        lv_style_set_transform_height(&btn_pressed_style, 0);
        lv_style_set_translate_x(&btn_pressed_style, 0);
        lv_style_set_translate_y(&btn_pressed_style, 0);
        pressed_style_initialized = true;
    }
    
    lv_obj_add_style(btn, &btn_style, 0);
    lv_obj_add_style(btn, &btn_pressed_style, LV_STATE_PRESSED);
    
    lv_obj_set_size(btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);  // 清除滚动标志
    
    /* 重置按钮的布局方式，确保我们的对齐设置生效 */
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    /* 设置图标样式 - 64x64图标，距离按钮左侧18px，上下居中 */
    if (btn_img) {
        lv_obj_set_size(btn_img, 64, 64);  // 图标大小64x64
        lv_obj_set_style_pad_left(btn, 18, 0);  // 按钮左侧内边距18px
        lv_obj_align(btn_img, LV_ALIGN_CENTER, 0, 0);
    }

    /* 设置文本样式 - 距离图标右侧24px，上下居中 */
    if (btn_label) {
        lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
        lv_obj_set_style_pad_left(btn_label, 24, 0);  // 文本距离图标24px
        lv_obj_align(btn_label, LV_ALIGN_CENTER, 0, 0);
    }
    
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void forbift_btn_click(void)
{
    SETTINGS_LOG("forbid");
    lv_obj_clear_flag(wifi_btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_clear_flag(date_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(time_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(battery_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(display_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(volume_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(system_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(about_btn, LV_OBJ_FLAG_CLICKABLE);
}

static void unforbift_btn_click(void)
{
    SETTINGS_LOG("unforbid");
    lv_obj_add_flag(wifi_btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(date_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(time_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(battery_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(display_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(volume_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(system_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(about_btn, LV_OBJ_FLAG_CLICKABLE);
}

static void slide_gesture_handler(lv_event_t *e) 
{
    lv_event_code_t code = lv_event_get_code(e);

    static lv_point_t start_point = {0};
    static bool is_dragging = false;

    switch(code) {
        case LV_EVENT_PRESSED:
            forbift_btn_click();
            lv_indev_get_point(lv_indev_active(), &start_point);
            is_dragging = true;
            break;
        
        case LV_EVENT_RELEASED:
            if(is_dragging) {
                lv_point_t end_point;
                lv_indev_get_point(lv_indev_active(), &end_point);
                
                int32_t delta_x = end_point.x - start_point.x;
                int32_t delta_y = end_point.y - start_point.y;

                if(delta_x > 50) 
                {
                    if(abs(delta_x) > abs(delta_y))
                    {
                        if(settings_base != NULL)  // 左滑：退出当前页面（详情页或列表页）
                        {
                            // 将页面从页面栈弹出
                            vw_watch_pop_page(settings_base);
                            lv_obj_del(settings_base); //删除窗口
                            settings_base = NULL;
                        }
                    }
                } 
                else
                {
                    unforbift_btn_click();
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}


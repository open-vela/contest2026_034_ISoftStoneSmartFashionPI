#include <stdio.h>
#include <lvgl.h>
#include "settings.h"
#include "../common/watch_pages.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include <nuttx/power/axp2101.h>
#include <nuttx/arch.h>

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define SETTINGS_LOG(fmt, ...) printf("[SETTINGS] " fmt "\n", ##__VA_ARGS__)
#else
#  define SETTINGS_LOG(fmt, ...)
#endif


static lv_obj_t *system_base = NULL;
static lv_obj_t *poweroff_btn = NULL;
static lv_obj_t *reboot_btn = NULL;

static void slide_gesture_handler(lv_event_t *e);

static void poweroff_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED)
    {
        SETTINGS_LOG("Power off system");
        axp2101_power_off();
    }
}

static void reboot_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED)
    {
        SETTINGS_LOG("Reboot system");
        axp2101_power_reset();
    }
}

void settings_system_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        // 创建系统设置页面
        system_base = lv_obj_create(lv_scr_act());
        lv_obj_set_size(system_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
        lv_obj_clear_flag(system_base, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(system_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(system_base, 0, LV_STATE_DEFAULT);
        lv_obj_align(system_base, LV_ALIGN_CENTER, 0, 0);

        // 创建关机图标
        poweroff_btn = lv_img_create(system_base);
        lv_img_set_src(poweroff_btn, vw_resource_get_img("icon_set_shutdown"));
        lv_obj_add_flag(poweroff_btn, LV_OBJ_FLAG_CLICKABLE); 
        lv_obj_add_event_cb(poweroff_btn, poweroff_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_size(poweroff_btn, 100, 100);
        lv_obj_align(poweroff_btn, LV_ALIGN_TOP_LEFT, 52, 165);

        // 创建关机文字标签
        lv_obj_t *poweroff_label = lv_label_create(system_base);
        lv_label_set_text(poweroff_label, "关机");
        lv_obj_set_style_text_font(poweroff_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
        lv_obj_set_style_text_color(poweroff_label, lv_color_white(), 0);
        lv_obj_align_to(poweroff_label, poweroff_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

        // 创建重启图标
        reboot_btn = lv_img_create(system_base);
        lv_img_set_src(reboot_btn, vw_resource_get_img("icon_set_reboot"));
        lv_obj_add_flag(reboot_btn, LV_OBJ_FLAG_CLICKABLE); 
        lv_obj_add_event_cb(reboot_btn, reboot_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_size(reboot_btn, 100, 100);
        lv_obj_align(reboot_btn, LV_ALIGN_TOP_LEFT, 228, 166);
        
        // 创建重启文字标签
        lv_obj_t *reboot_label = lv_label_create(system_base);
        lv_label_set_text(reboot_label, "重启");
        lv_obj_set_style_text_font(reboot_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
        lv_obj_set_style_text_color(reboot_label, lv_color_white(), 0);
        lv_obj_align_to(reboot_label, reboot_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

        // 添加滑动手势处理
        lv_obj_add_event_cb(system_base, slide_gesture_handler, LV_EVENT_ALL, NULL);

        // 将页面压入页面栈
        vw_watch_push_page(system_base);
    }
}

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
                    if(system_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(system_base);
                        //删除窗口
                        lv_obj_del(system_base);
                        system_base = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

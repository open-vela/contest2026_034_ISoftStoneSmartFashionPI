#include "sos.h"
#include "../common/watch_pages.h"
#include "../../resource/resource.h"
#include "../launcher/launcher.h"
#include <lvgl.h>
#include <stdio.h>
#include <string.h>  // For strlen, strcat
#include <syslog.h>

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define SOS_LOG(fmt, ...) syslog(LOG_INFO, "[SOS] " fmt, ##__VA_ARGS__)


/* 全局变量 */
static char emergency_phone[12] = "";

/* UI对象 - 主页面 */
static lv_obj_t* sos_main_base = NULL;
static lv_obj_t* help_btn = NULL;
static lv_obj_t* ignore_btn = NULL;
static lv_obj_t* set_contact_btn = NULL;

/* UI对象 - 编辑页面 */
static lv_obj_t* sos_edit_base = NULL;
static lv_obj_t* phone_label = NULL;
static lv_obj_t* delete_btn = NULL;
static lv_obj_t* confirm_btn = NULL;
static lv_obj_t* cancel_btn = NULL;

/* 函数声明 */
static void sos_app_create(void);
static void sos_edit_create(void);
static void slide_gesture_sos_main_handler(lv_event_t *e);
static void slide_gesture_sos_edit_handler(lv_event_t *e);
static void help_btn_event_cb(lv_event_t *e);
static void ignore_btn_event_cb(lv_event_t *e);
static void set_contact_btn_event_cb(lv_event_t *e);
static void number_btn_event_cb(lv_event_t *e);
static void delete_btn_event_cb(lv_event_t *e);
static void confirm_btn_event_cb(lv_event_t *e);
static void cancel_btn_event_cb(lv_event_t *e);

static void sos_main_deleted_cb(lv_event_t *e)
{
    sos_main_base = NULL;
    help_btn = NULL;
    ignore_btn = NULL;
    set_contact_btn = NULL;
}

static void sos_edit_deleted_cb(lv_event_t *e)
{
    sos_edit_base = NULL;
    phone_label = NULL;
    delete_btn = NULL;
    confirm_btn = NULL;
    cancel_btn = NULL;
}

static void sos_app_create(void)
{
    // 创建SOS主页面
    sos_main_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sos_main_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sos_main_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sos_main_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sos_main_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sos_main_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(sos_main_base);
    lv_label_set_text(title_label, "SOS求助");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 26);

    // 创建求助按钮
    help_btn = lv_btn_create(sos_main_base);
    lv_obj_add_event_cb(help_btn, help_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(help_btn, 166, 238);
    lv_obj_set_style_bg_color(help_btn, lv_color_hex(0xC83A5B), LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(help_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(help_btn, 32, LV_STATE_DEFAULT);
    lv_obj_align(help_btn, LV_ALIGN_TOP_LEFT, 20, 97);
    lv_obj_clear_flag(help_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(help_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    
    // 创建求助按钮文字 - 竖排
    lv_obj_t *help_btn_label1 = lv_label_create(help_btn);
    lv_label_set_text(help_btn_label1, "求");
    lv_obj_set_style_text_font(help_btn_label1, vw_resource_get_font(WATCH_REGULAR_FONT "_40"), 0);
    lv_obj_set_style_text_color(help_btn_label1, lv_color_white(), 0);
    lv_obj_align(help_btn_label1, LV_ALIGN_CENTER, 0, -20);
    
    lv_obj_t *help_btn_label2 = lv_label_create(help_btn);
    lv_label_set_text(help_btn_label2, "助");
    lv_obj_set_style_text_font(help_btn_label2, vw_resource_get_font(WATCH_REGULAR_FONT "_40"), 0);
    lv_obj_set_style_text_color(help_btn_label2, lv_color_white(), 0);
    lv_obj_align(help_btn_label2, LV_ALIGN_CENTER, 0, 20);

    // 创建忽略按钮
    ignore_btn = lv_btn_create(sos_main_base);
    lv_obj_add_event_cb(ignore_btn, ignore_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(ignore_btn, 166, 238);
    lv_obj_set_style_bg_color(ignore_btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ignore_btn, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ignore_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ignore_btn, 32, LV_STATE_DEFAULT);
    lv_obj_align(ignore_btn, LV_ALIGN_TOP_LEFT, 195, 97);
    lv_obj_clear_flag(ignore_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(ignore_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    
    // 创建忽略按钮文字 - 竖排
    lv_obj_t *ignore_btn_label1 = lv_label_create(ignore_btn);
    lv_label_set_text(ignore_btn_label1, "忽");
    lv_obj_set_style_text_font(ignore_btn_label1, vw_resource_get_font(WATCH_REGULAR_FONT "_40"), 0);
    lv_obj_set_style_text_color(ignore_btn_label1, lv_color_white(), 0);
    lv_obj_align(ignore_btn_label1, LV_ALIGN_CENTER, 0, -20);
    
    lv_obj_t *ignore_btn_label2 = lv_label_create(ignore_btn);
    lv_label_set_text(ignore_btn_label2, "略");
    lv_obj_set_style_text_font(ignore_btn_label2, vw_resource_get_font(WATCH_REGULAR_FONT "_40"), 0);
    lv_obj_set_style_text_color(ignore_btn_label2, lv_color_white(), 0);
    lv_obj_align(ignore_btn_label2, LV_ALIGN_CENTER, 0, 20);

    // 创建设置紧急联系人按钮
    set_contact_btn = lv_btn_create(sos_main_base);
    lv_obj_add_event_cb(set_contact_btn, set_contact_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(set_contact_btn, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_set_style_bg_color(set_contact_btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(set_contact_btn, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(set_contact_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(set_contact_btn, 32, LV_STATE_DEFAULT);
    lv_obj_align(set_contact_btn, LV_ALIGN_TOP_MID, 0, 362);
    lv_obj_clear_flag(set_contact_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(set_contact_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    
    lv_obj_t *set_contact_btn_label = lv_label_create(set_contact_btn);
    lv_label_set_text(set_contact_btn_label, "设置紧急联系人");
    lv_obj_set_style_text_font(set_contact_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(set_contact_btn_label, lv_color_white(), 0);
    lv_obj_center(set_contact_btn_label);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sos_main_base, slide_gesture_sos_main_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sos_main_base, sos_main_deleted_cb, LV_EVENT_DELETE, NULL);

    SOS_LOG("SOS main page create complete");
}

static void sos_edit_create(void)
{
    // 创建SOS编辑页面
    sos_edit_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sos_edit_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(sos_edit_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sos_edit_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sos_edit_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(sos_edit_base, LV_ALIGN_CENTER, 0, 0);

    // 创建电话显示框
    lv_obj_t *phone_container = lv_obj_create(sos_edit_base);
    lv_obj_set_size(phone_container, 340, 60);
    lv_obj_set_style_bg_opa(phone_container, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(phone_container, 0, LV_STATE_DEFAULT);
    lv_obj_align(phone_container, LV_ALIGN_TOP_MID, 0, 11);
    lv_obj_clear_flag(phone_container, LV_OBJ_FLAG_SCROLLABLE);

    // 创建电话显示标签
    phone_label = lv_label_create(phone_container);
    lv_label_set_text(phone_label, emergency_phone);
    lv_obj_set_style_text_font(phone_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(phone_label, lv_color_white(), 0);
    // 设置标签从右向左对齐，新输入的数字会显示在右侧
    lv_obj_set_width(phone_label, 260);  // 设置固定宽度
    lv_label_set_long_mode(phone_label, LV_LABEL_LONG_WRAP);  // 设置长文本模式
    lv_obj_set_style_text_align(phone_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(phone_label, LV_ALIGN_RIGHT_MID, -50, 0);

    // 创建删除按钮
    delete_btn = lv_btn_create(phone_container);
    lv_obj_add_event_cb(delete_btn, delete_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(delete_btn, 44, 44);
    lv_obj_set_style_bg_opa(delete_btn, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(delete_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(delete_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(delete_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(delete_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    
    lv_obj_t *delete_img = lv_img_create(delete_btn);
    lv_img_set_src(delete_img, vw_resource_get_img("icon_sos_delete"));
    lv_obj_center(delete_img);

    // 创建数字按钮
    char number_text[2] = "0";
    lv_obj_t *number_btns[10];
    
    // 数字1-3 (正常顺序: 1, 2, 3)
    for (int i = 1; i <= 3; i++) {
        number_btns[i-1] = lv_btn_create(sos_edit_base);
        lv_obj_add_event_cb(number_btns[i-1], number_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_set_size(number_btns[i-1], 100, 64);
        lv_obj_set_style_bg_color(number_btns[i-1], lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(number_btns[i-1], LV_OPA_10, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(number_btns[i-1], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(number_btns[i-1], 32, LV_STATE_DEFAULT);
        // 正常排列: 1在左边(36), 2在中间(156), 3在右边(276)
        lv_obj_align(number_btns[i-1], LV_ALIGN_TOP_LEFT, 36 + (i-1)*120, 74);
        lv_obj_clear_flag(number_btns[i-1], LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(number_btns[i-1], LV_OBJ_FLAG_EVENT_BUBBLE);

        sprintf(number_text, "%d", i);
        lv_obj_t *number_label = lv_label_create(number_btns[i-1]);
        lv_label_set_text(number_label, number_text);
        lv_obj_set_style_text_font(number_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
        lv_obj_set_style_text_color(number_label, lv_color_white(), 0);
        lv_obj_center(number_label);
    }
    
    // 数字4-6 (正常顺序: 4, 5, 6)
    for (int i = 4; i <= 6; i++) {
        number_btns[i-1] = lv_btn_create(sos_edit_base);
        lv_obj_add_event_cb(number_btns[i-1], number_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_set_size(number_btns[i-1], 100, 64);
        lv_obj_set_style_bg_color(number_btns[i-1], lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(number_btns[i-1], LV_OPA_10, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(number_btns[i-1], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(number_btns[i-1], 32, LV_STATE_DEFAULT);
        // 正常排列: 4在左边(36), 5在中间(156), 6在右边(276)
        lv_obj_align(number_btns[i-1], LV_ALIGN_TOP_LEFT, 36 + (i-4)*120, 154);
        lv_obj_clear_flag(number_btns[i-1], LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(number_btns[i-1], LV_OBJ_FLAG_EVENT_BUBBLE);

        sprintf(number_text, "%d", i);
        lv_obj_t *number_label = lv_label_create(number_btns[i-1]);
        lv_label_set_text(number_label, number_text);
        lv_obj_set_style_text_font(number_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
        lv_obj_set_style_text_color(number_label, lv_color_white(), 0);
        lv_obj_center(number_label);
    }
    
    // 数字7-9 (正常顺序: 7, 8, 9)
    for (int i = 7; i <= 9; i++) {
        number_btns[i-1] = lv_btn_create(sos_edit_base);
        lv_obj_add_event_cb(number_btns[i-1], number_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_set_size(number_btns[i-1], 100, 64);
        lv_obj_set_style_bg_color(number_btns[i-1], lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(number_btns[i-1], LV_OPA_10, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(number_btns[i-1], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(number_btns[i-1], 32, LV_STATE_DEFAULT);
        // 正常排列: 7在左边(36), 8在中间(156), 9在右边(276)
        lv_obj_align(number_btns[i-1], LV_ALIGN_TOP_LEFT, 36 + (i-7)*120, 234);
        lv_obj_clear_flag(number_btns[i-1], LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(number_btns[i-1], LV_OBJ_FLAG_EVENT_BUBBLE);

        sprintf(number_text, "%d", i);
        lv_obj_t *number_label = lv_label_create(number_btns[i-1]);
        lv_label_set_text(number_label, number_text);
        lv_obj_set_style_text_font(number_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
        lv_obj_set_style_text_color(number_label, lv_color_white(), 0);
        lv_obj_center(number_label);
    }
    
    // 数字0
    number_btns[9] = lv_btn_create(sos_edit_base);
    lv_obj_add_event_cb(number_btns[9], number_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    lv_obj_set_size(number_btns[9], 100, 64);
    lv_obj_set_style_bg_color(number_btns[9], lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(number_btns[9], LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(number_btns[9], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(number_btns[9], 32, LV_STATE_DEFAULT);
    lv_obj_align(number_btns[9], LV_ALIGN_TOP_LEFT, 156, 314);
    lv_obj_clear_flag(number_btns[9], LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(number_btns[9], LV_OBJ_FLAG_EVENT_BUBBLE);
    
    lv_obj_t *number_label = lv_label_create(number_btns[9]);
    lv_label_set_text(number_label, "0");
    lv_obj_set_style_text_font(number_label, vw_resource_get_font(WATCH_REGULAR_FONT "_36"), 0);
    lv_obj_set_style_text_color(number_label, lv_color_white(), 0);
    lv_obj_center(number_label);

    // 创建确认按钮
    confirm_btn = lv_btn_create(sos_edit_base);
    lv_obj_add_event_cb(confirm_btn, confirm_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(confirm_btn, 150, 60);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(0x2D47CB), LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(confirm_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(confirm_btn, 32, LV_STATE_DEFAULT);
    lv_obj_align(confirm_btn, LV_ALIGN_TOP_LEFT, 40, 403);
    lv_obj_clear_flag(confirm_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(confirm_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    
    lv_obj_t *confirm_btn_label = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_btn_label, "确认");
    lv_obj_set_style_text_font(confirm_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(confirm_btn_label, lv_color_white(), 0);
    lv_obj_center(confirm_btn_label);

    // 创建取消按钮
    cancel_btn = lv_btn_create(sos_edit_base);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(cancel_btn, 150, 60);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_10, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cancel_btn, 32, LV_STATE_DEFAULT);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 230, 403);
    lv_obj_clear_flag(cancel_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    
    lv_obj_t *cancel_btn_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_btn_label, "取消");
    lv_obj_set_style_text_font(cancel_btn_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(cancel_btn_label, lv_color_white(), 0);
    lv_obj_center(cancel_btn_label);

    // 添加滑动手势处理
    lv_obj_add_event_cb(sos_edit_base, slide_gesture_sos_edit_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(sos_edit_base, sos_edit_deleted_cb, LV_EVENT_DELETE, NULL);

    SOS_LOG("SOS edit page create complete");
}

static void slide_gesture_sos_main_handler(lv_event_t *e)
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
                    if(sos_main_base != NULL) {
                        /* 从页面栈中移除 */
                        vw_watch_pop_page(sos_main_base);
                        lv_obj_del(sos_main_base);
                        sos_main_base = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

static void slide_gesture_sos_edit_handler(lv_event_t *e)
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
                    if(sos_edit_base != NULL) {
                        /* 从页面栈中移除 */
                        vw_watch_pop_page(sos_edit_base);
                        lv_obj_del(sos_edit_base);
                        sos_edit_base = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

static void help_btn_event_cb(lv_event_t *e)
{
    // 暂时不做处理
    SOS_LOG("Help button clicked");
}

static void ignore_btn_event_cb(lv_event_t *e)
{
    // 删除当前页面退出
    if(sos_main_base != NULL) {
        /* 从页面栈中移除 */
        vw_watch_pop_page(sos_main_base);
        lv_obj_del(sos_main_base);
        sos_main_base = NULL;
    }
}

static void set_contact_btn_event_cb(lv_event_t *e)
{
    // 进入数字键盘页面
    sos_edit_create();
    /* 将编辑页面添加到页面栈 */
    if (sos_edit_base != NULL) {
        vw_watch_push_page(sos_edit_base);
    }
}

static void number_btn_event_cb(lv_event_t *e)
{
    // 获取数字值
    int number = (int)(intptr_t)lv_event_get_user_data(e);
    
    // 检查电话号码长度
    if(strlen(emergency_phone) < 11) {
        // 添加数字到电话号码
        char number_str[2];
        sprintf(number_str, "%d", number);
        strcat(emergency_phone, number_str);
        
        // 更新显示
        lv_label_set_text(phone_label, emergency_phone);
    }
}

static void delete_btn_event_cb(lv_event_t *e)
{
    // 删除最后一个数字
    int len = strlen(emergency_phone);
    if(len > 0) {
        emergency_phone[len-1] = '\0';
        lv_label_set_text(phone_label, emergency_phone);
    }
}

static void confirm_btn_event_cb(lv_event_t *e)
{
    // 保存电话号码
    SOS_LOG("Emergency phone saved: %s", emergency_phone);

    // 退出编辑页面
    if(sos_edit_base != NULL) {
        /* 从页面栈中移除 */
        vw_watch_pop_page(sos_edit_base);
        lv_obj_del(sos_edit_base);
        sos_edit_base = NULL;
    }
}

static void cancel_btn_event_cb(lv_event_t *e)
{
    // 不保存，直接退出
    if(sos_edit_base != NULL) {
        /* 从页面栈中移除 */
        vw_watch_pop_page(sos_edit_base);
        lv_obj_del(sos_edit_base);
        sos_edit_base = NULL;
    }
}

void sos_app_click_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        SOS_LOG("SOS button click.");
        sos_app_create();
        /* 将主页面添加到页面栈 */
        if (sos_main_base != NULL) {
            vw_watch_push_page(sos_main_base);
        }
    }
}

void sos_app_activate(void)
{
    if (sos_main_base != NULL) {
        vw_watch_pop_page(sos_main_base);
        lv_obj_del(sos_main_base);
        sos_main_base = NULL;
    }

    sos_app_create();
    if (sos_main_base != NULL) {
        vw_watch_push_page(sos_main_base);
    }
    SOS_LOG("SOS app activated (e.g. fall detected)");
}

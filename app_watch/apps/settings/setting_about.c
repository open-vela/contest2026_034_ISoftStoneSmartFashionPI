#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <lvgl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <syslog.h>
#include <errno.h>
#include "../common/watch_pages.h"
#include "../launcher/launcher.h"

#define WATCH_DBG_LOG(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

/* 获取WiFi网卡(wlan0)的MAC地址，格式: XX:XX:XX:XX:XX:XX */
static void settings_get_wifi_mac_addr(char *buf, size_t buflen)
{
    int sock;
    struct ifreq ifr;

    if (buf == NULL || buflen < 18) {
        return;
    }

    /* 默认值，获取失败时显示 */
    snprintf(buf, buflen, "00:00:00:00:00:00");

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        WATCH_DBG_LOG("[About] Failed to create socket for MAC: %d", errno);
        return;
    }

    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
        unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
        snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        WATCH_DBG_LOG("[About] ioctl SIOCGIFHWADDR failed: %d", errno);
    }

    close(sock);
}

static void settings_about_slide_gesture_handler(lv_event_t *e);

static void settings_about_slide_gesture_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_current_target(e);

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
                    if(obj != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(obj);
                        lv_obj_del(obj);
                        obj = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

static void settings_about_create(void)
{
    lv_obj_t *about_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(about_container, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_set_scrollbar_mode(about_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(about_container, LV_DIR_VER);
    lv_obj_set_style_bg_color(about_container, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_add_event_cb(about_container, settings_about_slide_gesture_handler, LV_EVENT_ALL, NULL);
    lv_obj_center(about_container);
    lv_obj_clear_flag(about_container, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(about_container, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_pad_all(about_container, 0, 0);
    lv_obj_set_style_border_width(about_container, 0, 0);
    
    static lv_style_t cont_style;
    lv_style_init(&cont_style);
    lv_style_set_bg_color(&cont_style, lv_color_hex(0xFFFFFF));
    lv_style_set_bg_opa(&cont_style, LV_OPA_10);
    lv_style_set_radius(&cont_style, 32);
    lv_style_set_border_width(&cont_style, 0);
    
    static lv_style_t label_style;
    lv_style_init(&label_style);
    lv_style_set_text_color(&label_style, lv_color_hex(0xFFFFFF));
    lv_style_set_text_opa(&label_style, LV_OPA_20);
    
    static lv_style_t value_style;
    lv_style_init(&value_style);
    lv_style_set_text_color(&value_style, lv_color_white());
    lv_style_set_text_font(&value_style, vw_resource_get_font(WATCH_REGULAR_FONT "_32"));
    
    int y_offset = 40;
    int cont_spacing = 16;
    
    lv_obj_t *cont1 = lv_obj_create(about_container);
    lv_obj_set_size(cont1, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_align(cont1, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_add_style(cont1, &cont_style, 0);
    lv_obj_set_layout(cont1, LV_LAYOUT_NONE);
    lv_obj_set_scrollbar_mode(cont1, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(cont1, LV_DIR_NONE);
    lv_obj_add_flag(cont1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cont1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cont1, 0, 0);
    
    lv_obj_t *label1 = lv_label_create(cont1);
    lv_label_set_text(label1, "设备名称");
    lv_obj_add_style(label1, &label_style, 0);
    lv_obj_set_style_text_font(label1, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), LV_STATE_DEFAULT);
    lv_obj_set_pos(label1, 18, 14);
    
    lv_obj_t *value1 = lv_label_create(cont1);
    lv_label_set_text(value1, "TIKEY WATCH");
    lv_obj_add_style(value1, &value_style, 0);
    lv_obj_set_pos(value1, 18, 42);
    
    y_offset += WATCH_BTN_HEIGHT + cont_spacing;
    
    lv_obj_t *cont2 = lv_obj_create(about_container);
    lv_obj_set_size(cont2, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_align(cont2, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_add_style(cont2, &cont_style, 0);
    lv_obj_set_layout(cont2, LV_LAYOUT_NONE);
    lv_obj_set_scrollbar_mode(cont2, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(cont2, LV_DIR_NONE);
    lv_obj_clear_flag(cont2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cont2, 0, 0);
    
    lv_obj_t *label2 = lv_label_create(cont2);
    lv_label_set_text(label2, "系统名称");
    lv_obj_add_style(label2, &label_style, 0);
    lv_obj_set_style_text_font(label2, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), LV_STATE_DEFAULT);
    lv_obj_set_pos(label2, 18, 14);
    
    lv_obj_t *value2 = lv_label_create(cont2);
    lv_label_set_text(value2, "TIKEY OS");
    lv_obj_add_style(value2, &value_style, 0);
    lv_obj_set_pos(value2, 18, 42);
    
    y_offset += WATCH_BTN_HEIGHT + cont_spacing;
    
    lv_obj_t *cont3 = lv_obj_create(about_container);
    lv_obj_set_size(cont3, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_align(cont3, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_add_style(cont3, &cont_style, 0);
    lv_obj_set_layout(cont3, LV_LAYOUT_NONE);
    lv_obj_set_scrollbar_mode(cont3, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(cont3, LV_DIR_NONE);
    lv_obj_clear_flag(cont3, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cont3, 0, 0);
    
    lv_obj_t *label3 = lv_label_create(cont3);
    lv_label_set_text(label3, "系统版本");
    lv_obj_add_style(label3, &label_style, 0);
    lv_obj_set_style_text_font(label3, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), LV_STATE_DEFAULT);
    lv_obj_set_pos(label3, 18, 14);
    
    lv_obj_t *value3 = lv_label_create(cont3);
    lv_label_set_text(value3, "1.0");
    lv_obj_add_style(value3, &value_style, 0);
    lv_obj_set_pos(value3, 18, 42);
    
    y_offset += WATCH_BTN_HEIGHT + cont_spacing;
    
    lv_obj_t *cont4 = lv_obj_create(about_container);
    lv_obj_set_size(cont4, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_align(cont4, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_add_style(cont4, &cont_style, 0);
    lv_obj_set_layout(cont4, LV_LAYOUT_NONE);
    lv_obj_set_scrollbar_mode(cont4, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(cont4, LV_DIR_NONE);
    lv_obj_clear_flag(cont4, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cont4, 0, 0);
    
    lv_obj_t *label4 = lv_label_create(cont4);
    lv_label_set_text(label4, "软件版本");
    lv_obj_add_style(label4, &label_style, 0);
    lv_obj_set_style_text_font(label4, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), LV_STATE_DEFAULT);
    lv_obj_set_pos(label4, 18, 14);
    
    lv_obj_t *value4 = lv_label_create(cont4);
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char version_str[20];
    strftime(version_str, sizeof(version_str), "%Y%m%d", tm_info);
    lv_label_set_text(value4, version_str);
    lv_obj_add_style(value4, &value_style, 0);
    lv_obj_set_pos(value4, 18, 42);
    
    y_offset += WATCH_BTN_HEIGHT + cont_spacing;
    
    lv_obj_t *cont5 = lv_obj_create(about_container);
    lv_obj_set_size(cont5, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_align(cont5, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_add_style(cont5, &cont_style, 0);
    lv_obj_set_layout(cont5, LV_LAYOUT_NONE);
    lv_obj_set_scrollbar_mode(cont5, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(cont5, LV_DIR_NONE);
    lv_obj_clear_flag(cont5, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cont5, 0, 0);
    
    lv_obj_t *label5 = lv_label_create(cont5);
    lv_label_set_text(label5, "SN");
    lv_obj_add_style(label5, &label_style, 0);
    lv_obj_set_style_text_font(label5, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), LV_STATE_DEFAULT);
    lv_obj_set_pos(label5, 18, 14);
    
    lv_obj_t *value5 = lv_label_create(cont5);
    lv_label_set_text(value5, "SW021/0000001");
    lv_obj_add_style(value5, &value_style, 0);
    lv_obj_set_pos(value5, 18, 42);

    y_offset += WATCH_BTN_HEIGHT + cont_spacing;
    
    lv_obj_t *cont6 = lv_obj_create(about_container);
    lv_obj_set_size(cont6, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
    lv_obj_align(cont6, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_add_style(cont6, &cont_style, 0);
    lv_obj_set_layout(cont6, LV_LAYOUT_NONE);
    lv_obj_set_scrollbar_mode(cont6, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(cont6, LV_DIR_NONE);
    lv_obj_clear_flag(cont6, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cont5, 0, 0);
    
    lv_obj_t *label6 = lv_label_create(cont6);
    lv_label_set_text(label6, "MAC地址");
    lv_obj_add_style(label6, &label_style, 0);
    lv_obj_set_style_text_font(label6, vw_resource_get_font(WATCH_REGULAR_FONT "_20"), LV_STATE_DEFAULT);
    lv_obj_set_pos(label6, 18, 14);
    
    lv_obj_t *value6 = lv_label_create(cont6);
    char mac_str[18] = {0};
    settings_get_wifi_mac_addr(mac_str, sizeof(mac_str));
    lv_label_set_text(value6, mac_str);
    lv_obj_add_style(value6, &value_style, 0);
    lv_obj_set_style_text_font(value6, vw_resource_get_font(WATCH_REGULAR_FONT "_26"), LV_STATE_DEFAULT);
    lv_obj_set_pos(value6, 18, 42);

    // 将页面压入页面栈
    vw_watch_push_page(about_container);
}

void settings_about_event_cb(lv_event_t *e) 
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        settings_about_create();
    }
}



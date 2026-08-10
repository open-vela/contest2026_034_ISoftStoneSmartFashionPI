#include "settings.h"
#include "../common/watch_pages.h"
#include "../common/watch_audio_player.h"
#include "../launcher/launcher.h"
#include "../../resource/resource.h"
#include <stdio.h>
#include <arch/board/board.h>

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define VOLUME_LOG(fmt, ...) printf("[VOLUME] " fmt "\n", ##__VA_ARGS__)
#else
#  define VOLUME_LOG(fmt, ...)
#endif

/* UI对象 */
static lv_obj_t* volume_set_base = NULL;
static lv_obj_t *volume_container = NULL;
static lv_obj_t *volume_indicator = NULL;

/* 音量等级: 0-1000 映射到 5 级 */
static const int volume_levels[] = {0, 250, 500, 750, 1000};
static int current_volume_level = 2; // 默认中等音量


/* 函数声明 */
static void slide_gesture_volume_set_handler(lv_event_t *e);
static void volume_bar_event_cb(lv_event_t *e);
static void setting_volume_set_create(void);

/* 音量等级转 0-1000 */
static int volume_level_to_value(int level)
{
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    return volume_levels[level];
}

/* 0-1000 值转音量等级 */
static int volume_value_to_level(int value)
{
    if (value <= 0) return 0;
    if (value <= 250) return 1;
    if (value <= 500) return 2;
    if (value <= 750) return 3;
    return 4;
}


static void setting_volume_set_create(void)
{
    /* 先获取当前音量，同步到UI等级 */
    uint16_t cur_vol = 500;
    esp32s3_watch_audio_getvolume(&cur_vol);
    current_volume_level = volume_value_to_level(cur_vol);

    // 创建音量调节页面
    volume_set_base = lv_obj_create(lv_scr_act());
    lv_obj_set_size(volume_set_base, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_clear_flag(volume_set_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(volume_set_base, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(volume_set_base, 0, LV_STATE_DEFAULT);
    lv_obj_align(volume_set_base, LV_ALIGN_CENTER, 0, 0);

    // 创建标题
    lv_obj_t *title_label = lv_label_create(volume_set_base);
    lv_label_set_text(title_label, "音量调节");
    lv_obj_set_style_text_font(title_label, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 26);

    // 创建音量调节容器
    volume_container = lv_obj_create(volume_set_base);
    lv_obj_set_size(volume_container, WATCH_BTN_WIDTH, 70);
    lv_obj_set_style_bg_color(volume_container, lv_color_hex(0xFDFDFD), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(volume_container, LV_OPA_20, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(volume_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(volume_container, 32, LV_STATE_DEFAULT);
    lv_obj_align(volume_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_scrollbar_mode(volume_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(volume_container, LV_DIR_NONE);
    lv_obj_set_style_pad_all(volume_container, 0, 0);
    lv_obj_add_event_cb(volume_container, volume_bar_event_cb, LV_EVENT_ALL, NULL);

    // 创建音量指示器
    volume_indicator = lv_obj_create(volume_container);
    lv_obj_set_width(volume_indicator, WATCH_BTN_WIDTH * (current_volume_level + 1) / 5);
    lv_obj_set_height(volume_indicator, 70);
    lv_obj_set_style_bg_color(volume_indicator, lv_color_hex(0x2BEA77), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(volume_indicator, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(volume_indicator, 32, LV_STATE_DEFAULT);
    lv_obj_align(volume_indicator, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(volume_indicator, LV_OBJ_FLAG_CLICKABLE);

    // 添加滑动手势处理
    lv_obj_add_event_cb(volume_set_base, slide_gesture_volume_set_handler, LV_EVENT_ALL, NULL);

    // 将页面压入页面栈
    vw_watch_push_page(volume_set_base);

    // 进入页面时播放一次测试音频（音量已从驱动获取）
    esp32s3_watch_audio_play_onetime(WATCH_AUDIO_PLAYER_TEST_FILE,
                                      volume_level_to_value(current_volume_level));

    VOLUME_LOG("volume_set: create complete, vol=%d level=%d", cur_vol, current_volume_level);
}


static void volume_bar_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        // 获取触摸位置
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        
        // 获取容器的位置和大小
        lv_area_t container_area;
        lv_obj_get_coords(lv_event_get_current_target(e), &container_area);
        
        // 计算容器内的相对坐标
        lv_point_t container_point;
        container_point.x = point.x - container_area.x1;
        container_point.y = point.y - container_area.y1;
        
        // 限制在容器范围内
        if (container_point.x < 0) container_point.x = 0;
        int container_width = lv_obj_get_width(volume_container);
        if (container_point.x > container_width) container_point.x = container_width;
        
        // 计算音量等级 (0-4)
        int new_level = (container_point.x * 5) / container_width;
        if (new_level < 0) new_level = 0;
        if (new_level > 4) new_level = 4;
        
        // 更新音量
        if (new_level != current_volume_level) {
            current_volume_level = new_level;
            lv_obj_t *container = lv_event_get_current_target(e);
            lv_obj_t *indicator = lv_obj_get_child(container, 0);
            if (indicator) {
                lv_obj_set_width(indicator, container_width * (current_volume_level + 1) / 5);
                lv_obj_align(indicator, LV_ALIGN_LEFT_MID, 0, 0);
            }

            int vol_value = volume_level_to_value(current_volume_level);
            esp32s3_watch_audio_setvolume(vol_value);

            VOLUME_LOG("Set volume to level %d, value %d", current_volume_level, vol_value);
        }
    }
}

/**
 * 滑动手势处理 - 音量调节页面
 */
static void slide_gesture_volume_set_handler(lv_event_t *e)
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
                    esp32s3_watch_audio_stop();
                    if(volume_set_base != NULL) {
                        // 将页面从页面栈弹出
                        vw_watch_pop_page(volume_set_base);
                        // 删除页面
                        lv_obj_del(volume_set_base);
                        volume_set_base = NULL;
                    }
                }
            }
            is_dragging = false;
            break;
            
        default:
            break;
    }
}

void settings_volume_ring_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        VOLUME_LOG("Settings volume ring button click.");
        setting_volume_set_create();
    }
}


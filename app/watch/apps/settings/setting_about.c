/**
 * @file setting_about.c
 * 关于页面：显示设备名称、系统信息、SN、MAC地址等
 */

#include <stdio.h>
#include <time.h>
#include <lvgl/lvgl.h>

#include "settings.h"
#include "../common/watch_pages.h"

static void settings_about_slide_gesture_handler(lv_event_t *e);

/**
 * @brief 添加一个信息卡片
 */
static lv_obj_t *about_add_card(lv_obj_t *parent, lv_style_t *cont_style,
                                int y_offset,
                                const char *title, const char *value,
                                lv_style_t *label_style,
                                lv_style_t *value_style)
{
  lv_obj_t *cont = lv_obj_create(parent);
  lv_obj_set_size(cont, WATCH_BTN_WIDTH, WATCH_BTN_HEIGHT);
  lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, y_offset);
  lv_obj_add_style(cont, cont_style, 0);
  lv_obj_set_layout(cont, LV_LAYOUT_NONE);
  lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_dir(cont, LV_DIR_NONE);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(cont, 0, 0);

  lv_obj_t *title_label = lv_label_create(cont);
  lv_label_set_text(title_label, title);
  lv_obj_add_style(title_label, label_style, 0);
  lv_obj_set_style_text_font(title_label, watch_resource_get_font("MiSans-Regular_20"), 0);
  lv_obj_set_pos(title_label, 18, 14);

  lv_obj_t *value_label = lv_label_create(cont);
  lv_label_set_text(value_label, value);
  lv_obj_add_style(value_label, value_style, 0);
  lv_obj_set_style_text_font(value_label, watch_resource_get_font("MiSans-Regular_20"), 0);
  lv_obj_set_pos(value_label, 18, 42);

  return cont;
}

static void settings_about_create(void)
{
  lv_obj_t *about_container = lv_obj_create(lv_scr_act());
  lv_obj_set_size(about_container, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
  lv_obj_set_scrollbar_mode(about_container, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_dir(about_container, LV_DIR_VER);
  lv_obj_set_style_bg_color(about_container, lv_color_black(), LV_STATE_DEFAULT);
  lv_obj_add_event_cb(about_container, settings_about_slide_gesture_handler,
                      LV_EVENT_ALL, NULL);
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

  int y_offset = 40;
  const int cont_spacing = 16;

  about_add_card(about_container, &cont_style, y_offset,
                 "设备名称", "TIKEY WATCH", &label_style, &value_style);
  y_offset += WATCH_BTN_HEIGHT + cont_spacing;

  about_add_card(about_container, &cont_style, y_offset,
                 "系统名称", "TIKEY OS", &label_style, &value_style);
  y_offset += WATCH_BTN_HEIGHT + cont_spacing;

  about_add_card(about_container, &cont_style, y_offset,
                 "系统版本", "1.0", &label_style, &value_style);
  y_offset += WATCH_BTN_HEIGHT + cont_spacing;

  /* 软件版本 = 编译当日 YYYYMMDD */
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);
  char version_str[20] = "20260101";
  if (tm_info != NULL)
    {
      strftime(version_str, sizeof(version_str), "%Y%m%d", tm_info);
    }
  about_add_card(about_container, &cont_style, y_offset,
                 "软件版本", version_str, &label_style, &value_style);
  y_offset += WATCH_BTN_HEIGHT + cont_spacing;

  about_add_card(about_container, &cont_style, y_offset,
                 "SN", "SW021/0000001", &label_style, &value_style);
  y_offset += WATCH_BTN_HEIGHT + cont_spacing;

  about_add_card(about_container, &cont_style, y_offset,
                 "MAC地址", "00:00:00:00:00:00", &label_style, &value_style);

  /* 将页面压入页面栈 */
  lv_watch_push_page(about_container);
}

static void settings_about_slide_gesture_handler(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *obj = lv_event_get_current_target(e);

  static lv_point_t start_point = {0};
  static bool is_dragging = false;

  switch (code)
    {
      case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_active(), &start_point);
        is_dragging = true;
        break;

      case LV_EVENT_RELEASED:
        if (is_dragging)
          {
            lv_point_t end_point;
            lv_indev_get_point(lv_indev_active(), &end_point);

            int32_t delta_x = end_point.x - start_point.x;
            int32_t delta_y = end_point.y - start_point.y;

            if (delta_x > 50 && abs(delta_x) > abs(delta_y))
              {
                if (obj != NULL)
                  {
                    lv_watch_pop_page(obj);
                    lv_obj_del(obj);
                  }
              }
          }
        is_dragging = false;
        break;

      default:
        break;
    }
}

void settings_about_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_CLICKED)
    {
      settings_about_create();
    }
}

/**
 * @file settings_ui_mode.c
 * UI模式切换设置项
 */

#include <stdio.h>
#include <syslog.h>
#include <lvgl.h>
#include "settings.h"
#include "../common/watch_pages.h"
#include "../../resource/resource.h"
#include "../../../../app/watch/apps/common/ui_mode_manager.h"

#define UI_MODE_LOG(fmt, ...) syslog(LOG_INFO, "[UI_MODE] " fmt, ##__VA_ARGS__)

void settings_ui_mode_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        UI_MODE_LOG("Switching to expression UI mode (runtime, no reset)");
        int ret = ui_mode_switch_runtime(UI_MODE_EXPRESSION, lv_scr_act());
        if (ret != 0)
        {
            UI_MODE_LOG("Failed to switch to expression mode: %d", ret);
        }
        else
        {
            UI_MODE_LOG("Expression mode switched (runtime)");
        }
    }
}

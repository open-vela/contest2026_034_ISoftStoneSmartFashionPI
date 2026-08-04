#include <stdio.h>
#include <lvgl.h>

static void settings_bluetooth_create(void)
{

}

void settings_bluetooth_event_cb(lv_event_t *e) 
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        settings_bluetooth_create();
    }
}
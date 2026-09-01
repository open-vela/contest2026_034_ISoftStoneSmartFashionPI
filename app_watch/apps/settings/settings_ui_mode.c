/**
 * @file settings_ui_mode.c
 * UI模式切换设置项
 *
 * 注意：ui_mode_switch_runtime 涉及 LVGL 对象删除、创建、阻塞文件 I/O
 * （SPI flash littlefs 写入），在 LV_EVENT_CLICKED 回调（渲染/事件链中）
 * 同步执行会导致 LVGL 内部状态损坏或 ESP32-S3 flash 写 work 线程互锁。
 * 因此用 lv_async_call 延迟到事件回调链完全结束后执行。
 */

#include <stdio.h>
#include <syslog.h>
#include <lvgl.h>
#include "settings.h"
#include "../common/watch_pages.h"
#include "../../resource/resource.h"
#include "../../../../app/watch/apps/common/ui_mode_manager.h"

#define UI_MODE_LOG(fmt, ...) syslog(LOG_INFO, "[UI_MODE] " fmt, ##__VA_ARGS__)

struct switch_ctx {
    ui_mode_t target;
    lv_obj_t *parent;
};

static void async_do_switch(void *arg)
{
    struct switch_ctx *ctx = (struct switch_ctx *)arg;

    UI_MODE_LOG("Switching to expression UI mode (runtime, async)");
    int ret = ui_mode_switch_runtime(ctx->target, ctx->parent);
    if (ret != 0)
        UI_MODE_LOG("Failed to switch to expression mode: %d", ret);
    else
        UI_MODE_LOG("Expression mode switched (runtime)");

    free(ctx);
}

void settings_ui_mode_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        struct switch_ctx *ctx = malloc(sizeof(*ctx));
        if (!ctx) return;
        ctx->target = UI_MODE_EXPRESSION;
        ctx->parent = lv_scr_act();

        lv_async_call(async_do_switch, ctx);
    }
}
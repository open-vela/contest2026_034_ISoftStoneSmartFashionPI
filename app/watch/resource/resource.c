/**
 * @file resource.c
 * Resource manager for boot logo, boot animation, and expression GIFs.
 * Boot resources are compiled into firmware; expression GIFs are loaded
 * from SD card at runtime.
 */

/*********************
 *      INCLUDES
 *********************/

#include "resource.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "image/generated/lvgl_assets.h"
#include "power_png/generated/power_png_assets.h"
#include "image/Expression/gif2/generated/expression_gif2_assets.h"

/*********************
 *      DEFINES
 *********************/

#define ARRAY_SIZE(ARRAY) (sizeof(ARRAY) / sizeof(ARRAY[0]))

/* 调试开关 */
#define RESOURCE_DEBUG 0

#if RESOURCE_DEBUG
#define RES_LOG(fmt, ...) printf("[RESOURCE] " fmt "\n", ##__VA_ARGS__)
#else
#define RES_LOG(fmt, ...)
#endif

/**********************
 *      TYPEDEFS
 **********************/

/* 图片资源结构体 */
typedef struct {
    const char* key;
    const void* data;  /* LVGL image descriptor pointer */
} resource_img_t;

/**********************
 *  STATIC VARIABLES
 **********************/

/* 常规图片资源映射表（包含开机logo） */
#define IMG_DEF(NAME) { #NAME, &NAME },
static const resource_img_t g_img_resource_map[] = {
#include "image/img_src.inc"
};
#undef IMG_DEF

/* 开机动画PNG帧资源映射表 */
#define IMG_DEF(NAME) { #NAME + 10, &NAME ## _dsc },
static const resource_img_t g_img_power_resource_map[] = {
#include "power_png/power_img_src.inc"
};
#undef IMG_DEF

/* 表情GIF嵌入式数据（编译到固件，无需SD卡） */
static const lv_image_dsc_t * const g_expression_gifs[] = {
    &gif_face_smile,
    &gif_face_calm,
    &gif_face_caring,
    &gif_face_comfort,
    &gif_face_confused,
    &gif_face_energy_pulse,
    &gif_face_error_1,
    &gif_face_fall,
    &gif_face_heart_breathing,
    &gif_face_listening,
    &gif_face_muted,
    &gif_face_shy,
    &gif_face_sleep,
    &gif_face_sleep_breathing,
    &gif_face_speaking,
    &gif_face_standby_1,
    &gif_face_success_1,
    &gif_face_thinking_1,
    &gif_face_waiting,
};

static const int g_img_count = sizeof(g_img_resource_map) / sizeof(g_img_resource_map[0]);
static const int g_img_power_count = sizeof(g_img_power_resource_map) / sizeof(g_img_power_resource_map[0]);
static const int g_img_expression_count = sizeof(g_expression_gifs) / sizeof(g_expression_gifs[0]);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void watch_resource_init(void)
{
    /* 表情GIF已嵌入固件，无需SD卡 */
    RES_LOG("Expression GIFs embedded: %d images", g_img_expression_count);
}

const void* watch_resource_get_img(const char* key)
{
    for (int i = 0; i < g_img_count; i++) {
        if (lv_strcmp(key, g_img_resource_map[i].key) == 0) {
            return g_img_resource_map[i].data;
        }
    }
    RES_LOG("Image key '%s' not found", key);
    return LV_SYMBOL_IMAGE;
}

const void* watch_resource_get_img_power(const char* key)
{
    for (int i = 0; i < g_img_power_count; i++) {
        if (lv_strcmp(key, g_img_power_resource_map[i].key) == 0) {
            return g_img_power_resource_map[i].data;
        }
    }
    RES_LOG("Image Power key '%s' not found", key);
    return LV_SYMBOL_IMAGE;
}

const void* watch_resource_get_img_expression(int index)
{
    if (index < 0 || index >= g_img_expression_count) {
        RES_LOG("Expression index %d out of range (0-%d)", index, g_img_expression_count - 1);
        return NULL;
    }

    return g_expression_gifs[index];
}

int watch_resource_get_expression_count(void)
{
    return g_img_expression_count;
}

int watch_resource_is_sd_ready(void)
{
    /* 表情GIF已嵌入固件，始终返回就绪 */
    return 1;
}

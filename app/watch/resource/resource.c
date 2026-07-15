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
#include "font/generated/MiSans_Regular.h"

/*********************
 *      DEFINES
 *********************/

#define ARRAY_SIZE(ARRAY) (sizeof(ARRAY) / sizeof(ARRAY[0]))

/* 调试开关 */
#define RESOURCE_DEBUG 1

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

/* 字体资源结构体 */
typedef struct {
    const char* key;
    const lv_font_t* font;
} resource_font_t;

/* 内存字体数据映射 */
typedef struct {
    const char * name;
    const uint8_t * data;
    uint32_t size;
} font_data_t;

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

/* 字体资源映射表 */
#define FONT_DEF(NAME, SIZE) { "default", LV_FONT_DEFAULT },
static resource_font_t g_font_resource_map[] = {
#include "font/font.inc"
};
#undef FONT_DEF

/* 内存字体数据映射表 */
static const font_data_t g_font_data_map[] = {
    { "MiSans-Regular", MiSans_Regular_ttf_data, MiSans_Regular_ttf_size },
};

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
    RES_LOG("Expression GIFs embedded: %d images", g_img_expression_count);

    /* 初始化 FreeType 库 */
    lv_result_t ft_res = lv_freetype_init(128);
    printf("[RESOURCE] lv_freetype_init result: %d\n", (int)ft_res);
    if (ft_res != LV_RESULT_OK) {
        printf("[RESOURCE] ERROR: lv_freetype_init failed\n");
        return;
    }
    printf("[RESOURCE] font_data_map[0].name=%s size=%u\n", g_font_data_map[0].name, (unsigned)g_font_data_map[0].size);

    /* 从嵌入式 C 数组创建字体 */
    int font_index = 0;
#define FONT_DEF(NAME, SIZE)                                                                          \
    do {                                                                                              \
        const uint8_t * font_data = NULL;                                                             \
        uint32_t font_data_size = 0;                                                                  \
        for (int i = 0; i < (int)ARRAY_SIZE(g_font_data_map); i++) {                                  \
            if (strcmp(g_font_data_map[i].name, #NAME) == 0) {                                        \
                font_data = g_font_data_map[i].data;                                                  \
                font_data_size = g_font_data_map[i].size;                                             \
                break;                                                                                \
            }                                                                                         \
        }                                                                                             \
        if (font_data) {                                                                              \
            lv_fs_path_ex_t mempath;                                                                  \
            lv_fs_make_path_from_buffer(&mempath, 'M', font_data, font_data_size);                    \
            lv_font_t* font = lv_freetype_font_create((const char *)&mempath,                         \
                LV_FREETYPE_FONT_RENDER_MODE_BITMAP,                                                  \
                SIZE,                                                                                 \
                LV_FREETYPE_FONT_STYLE_NORMAL);                                                       \
            if (font) {                                                                               \
                g_font_resource_map[font_index].key = #NAME "_" #SIZE;                                \
                g_font_resource_map[font_index].font = font;                                          \
                printf("[RESOURCE] loaded font: %s_%d\n", #NAME, SIZE);                              \
                font_index++;                                                                         \
            } else {                                                                                  \
                printf("[RESOURCE] font_%s_%d create failed\n", #NAME, SIZE);                         \
            }                                                                                         \
        }                                                                                             \
    } while (0);
#include "font/font.inc"
#undef FONT_DEF
    printf("[RESOURCE] created %d fonts\n", font_index);

    /* 设置第一个成功创建的字体为全局默认字体，确保中文可显示 */
    if (font_index > 0 && g_font_resource_map[0].font != NULL) {
        lv_obj_set_style_text_font(lv_scr_act(), g_font_resource_map[0].font, 0);
        printf("[RESOURCE] set default font to: %s\n", g_font_resource_map[0].key);
    }
}

const void* watch_resource_get_img(const char* key)
{
    for (int i = 0; i < g_img_count; i++) {
        if (lv_strcmp(key, g_img_resource_map[i].key) == 0) {
            return g_img_resource_map[i].data;
        }
    }
    RES_LOG("Image key '%s' not found", key);
    return NULL;
}

const lv_font_t* watch_resource_get_font(const char* key)
{
    /* 搜索已创建的字体 */
    for (int i = 0; i < (int)ARRAY_SIZE(g_font_resource_map); i++) {
        if (g_font_resource_map[i].font != LV_FONT_DEFAULT &&
            lv_strcmp(key, g_font_resource_map[i].key) == 0) {
            return g_font_resource_map[i].font;
        }
    }

    /* 字体未预创建 — 解析 key "<name>_<size>" 并动态创建 */
    const char *last_underscore = strrchr(key, '_');
    if (last_underscore == NULL) {
        return LV_FONT_DEFAULT;
    }

    int size = atoi(last_underscore + 1);
    if (size <= 0 || size > 200) {
        return LV_FONT_DEFAULT;
    }

    /* 提取字体名 */
    size_t name_len = last_underscore - key;
    char font_name[64];
    if (name_len >= sizeof(font_name)) name_len = sizeof(font_name) - 1;
    memcpy(font_name, key, name_len);
    font_name[name_len] = '\0';

    /* 查找嵌入式字体数据 */
    const uint8_t *font_data = NULL;
    uint32_t font_data_size = 0;
    for (int i = 0; i < (int)ARRAY_SIZE(g_font_data_map); i++) {
        if (strcmp(g_font_data_map[i].name, font_name) == 0) {
            font_data = g_font_data_map[i].data;
            font_data_size = g_font_data_map[i].size;
            break;
        }
    }

    if (font_data == NULL) {
        return LV_FONT_DEFAULT;
    }

    /* 动态创建 FreeType 字体 */
    lv_fs_path_ex_t mempath;
    lv_fs_make_path_from_buffer(&mempath, 'M', font_data, font_data_size);
    lv_font_t *font = lv_freetype_font_create((const char *)&mempath,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
        size,
        LV_FREETYPE_FONT_STYLE_NORMAL);
    if (font) {
        return font;
    }

    return LV_FONT_DEFAULT;
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

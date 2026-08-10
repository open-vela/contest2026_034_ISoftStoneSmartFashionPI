/**
 * @file resource.c
 */

/*********************
 *      INCLUDES
 *********************/

#include "resource.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "image/generated/lvgl_assets.h"
#include "power_jpg/generated/power_jpg_assets.h"
#include "power_png/generated/power_png_assets.h"
#include "font/generated/MiSans_Regular.h"

/*********************
 *      DEFINES
 *********************/

#define ARRAY_SIZE(ARRAY) (sizeof(ARRAY) / sizeof(ARRAY[0]))
#define FONT_BASE_PATH "/FONT/"
#define IMG_BASE_PATH "/IMAGE/"
#define IMG_POWER_JPG_BASE_PATH "/power_jpg/"
#define IMG_POWER_BASE_PATH "/power_png/"

/* 调试开关 - 受 CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG 控制 */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
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
    const void* data;  /* C数组指针或文件路径字符串 */
} resource_img_t;

typedef struct {
    const char* key;
    const lv_font_t* font;
} resource_font_t;

typedef struct {
    const char* key;
    lv_style_t style;
} resource_style_t;

/**********************
 *  STATIC VARIABLES
 **********************/

/* 图片资源映射表 - 编译进固件的C数组（零运行时解码） */
#define IMG_DEF(NAME) { #NAME, &NAME },
static const resource_img_t g_img_resource_map[] = {
#include "image/img_src.inc"
};
#undef IMG_DEF


/* 图片资源映射表 - 从C数组加载（零文件系统依赖） */
#define IMG_DEF(NAME) { #NAME, &NAME ## _dsc },
static const resource_img_t g_img_power_jpg_resource_map[] = {
#include "power_jpg/img_jpg_src.inc"
};
#undef IMG_DEF

/* 图片资源映射表 - 从C数组加载（零文件系统依赖）
 * 生成的符号/键名带 "power_png_" 前缀（共 10 个字符），
 * 但调用方（boot_animation）使用逻辑键 "power_frame_XX" 查找，
 * 故注册时跳过前缀，保证键与调用方一致。
 */
#define IMG_DEF(NAME) { #NAME + 10, &NAME ## _dsc },
static const resource_img_t g_img_power_resource_map[] = {
#include "power_png/power_img_src.inc"
};
#undef IMG_DEF


/* 字体资源映射表 - 保持原有宏定义 */
#define FONT_DEF(NAME, SIZE) { "default", LV_FONT_DEFAULT },
static resource_font_t g_font_resource_map[] = {
#include "font/font.inc"
};
#undef FONT_DEF

/* 内存字体数据映射表 */
typedef struct {
    const char * name;
    const uint8_t * data;
    uint32_t size;
} font_data_t;

static const font_data_t g_font_data_map[] = {
    { "MiSans-Regular", MiSans_Regular_ttf_data, MiSans_Regular_ttf_size },
};



/* 图片总数 */
static const int g_img_count = sizeof(g_img_resource_map) / sizeof(g_img_resource_map[0]);

static const int g_img_power_jpg_count = sizeof(g_img_power_jpg_resource_map) / sizeof(g_img_power_jpg_resource_map[0]);

static const int g_img_power_count = sizeof(g_img_power_resource_map) / sizeof(g_img_power_resource_map[0]);


/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void vw_resource_init(void)
{
    RES_LOG("resource_init started");

    /* Initialize FreeType library before creating fonts */
    lv_result_t ft_res = lv_freetype_init(256);
    if(ft_res != LV_RESULT_OK) {
        RES_LOG("ERROR: lv_freetype_init failed");
    }

    /* create fonts from embedded C arrays */
    int font_index = 0;
#define FONT_DEF(NAME, SIZE)                                                                          \
    do {                                                                                              \
        const uint8_t * font_data = NULL;                                                             \
        uint32_t font_data_size = 0;                                                                  \
        for (int i = 0; i < ARRAY_SIZE(g_font_data_map); i++) {                                       \
            if (strcmp(g_font_data_map[i].name, #NAME) == 0) {                                        \
                font_data = g_font_data_map[i].data;                                                  \
                font_data_size = g_font_data_map[i].size;                                             \
                break;                                                                                \
            }                                                                                         \
        }                                                                                             \
        if (font_data) {                                                                              \
            lv_fs_path_ex_t mempath;                                                                  \
            lv_fs_make_path_from_buffer(&mempath, 'M', font_data, font_data_size);                    \
            lv_font_t* font = lv_freetype_font_create((const char *)&mempath,                          \
                LV_FREETYPE_FONT_RENDER_MODE_BITMAP,                                                  \
                SIZE,                                                                                 \
                LV_FREETYPE_FONT_STYLE_NORMAL);                                                       \
            if (font) {                                                                               \
                g_font_resource_map[font_index].key = #NAME "_" #SIZE;                                \
                g_font_resource_map[font_index].font = font;                                          \
                RES_LOG("loaded font: %s_%d", #NAME, SIZE);                                           \
                font_index++;                                                                         \
            } else {                                                                                  \
                RES_LOG("font_" #NAME "_" #SIZE " create failed");                                  \
            }                                                                                         \
        } else {                                                                                      \
            RES_LOG("font_" #NAME "_" #SIZE " data not found");                                     \
        }                                                                                             \
    } while (0);
#include "font/font.inc"
#undef FONT_DEF
    RES_LOG("create %d fonts", font_index);

}

const lv_font_t* vw_resource_get_font(const char* key)
{
    /* Search pre-created fonts first */
    for (int i = 0; i < ARRAY_SIZE(g_font_resource_map); i++) {
        if (lv_strcmp(key, g_font_resource_map[i].key) == 0) {
            return g_font_resource_map[i].font;
        }
    }

    /* Font not pre-created — parse key "<name>_<size>" and create dynamically.
     * FreeType can render at any size, so we can create missing sizes on demand.
     * Key format: "MiSans-Regular_23" → name="MiSans-Regular", size=23
     */
    const char *last_underscore = strrchr(key, '_');
    if (last_underscore == NULL) {
        RES_LOG("Invalid font key format: '%s'", key);
        return LV_FONT_DEFAULT;
    }

    int size = atoi(last_underscore + 1);
    if (size <= 0 || size > 200) {
        RES_LOG("Invalid font size in key: '%s'", key);
        return LV_FONT_DEFAULT;
    }

    /* Extract font name (everything before the last underscore) */
    size_t name_len = last_underscore - key;
    char font_name[64];
    if (name_len >= sizeof(font_name)) name_len = sizeof(font_name) - 1;
    memcpy(font_name, key, name_len);
    font_name[name_len] = '\0';

    /* Look up the embedded font data by name */
    const uint8_t *font_data = NULL;
    uint32_t font_data_size = 0;
    for (int i = 0; i < ARRAY_SIZE(g_font_data_map); i++) {
        if (strcmp(g_font_data_map[i].name, font_name) == 0) {
            font_data = g_font_data_map[i].data;
            font_data_size = g_font_data_map[i].size;
            break;
        }
    }

    if (font_data == NULL) {
        RES_LOG("Unknown font name '%s' (from key '%s')", font_name, key);
        return LV_FONT_DEFAULT;
    }

    /* Create the FreeType font dynamically */
    lv_fs_path_ex_t mempath;
    lv_fs_make_path_from_buffer(&mempath, 'M', font_data, font_data_size);
    lv_font_t *font = lv_freetype_font_create((const char *)&mempath,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
        size,
        LV_FREETYPE_FONT_STYLE_NORMAL);
    if (font) {
        RES_LOG("Dynamic font created: %s_%d (on demand)", font_name, size);
        return font;
    }

    RES_LOG("Failed to create dynamic font: %s_%d", font_name, size);
    return LV_FONT_DEFAULT;
}

const void* vw_resource_get_img(const char* key)
{
    for (int i = 0; i < g_img_count; i++) {
        if (lv_strcmp(key, g_img_resource_map[i].key) == 0) {
            return g_img_resource_map[i].data;
        }
    }
    RES_LOG("Image key '%s' not found", key);
    return LV_SYMBOL_IMAGE;
}

const void* vw_resource_get_power_img_jpg(const char* key)
{
    for (int i = 0; i < g_img_power_jpg_count; i++) {
        if (lv_strcmp(key, g_img_power_jpg_resource_map[i].key) == 0) {
            return g_img_power_jpg_resource_map[i].data;
        }
    }
    RES_LOG("Image Power JPG key '%s' not found", key);
    return LV_SYMBOL_IMAGE;
}

const void* vw_resource_get_img_power(const char* key)
{
    for (int i = 0; i < g_img_power_count; i++) {
        if (lv_strcmp(key, g_img_power_resource_map[i].key) == 0) {
            return g_img_power_resource_map[i].data;
        }
    }
    RES_LOG("Image Power PNG key '%s' not found", key);
    return LV_SYMBOL_IMAGE;
}

void vw_resource_debug_print_fonts(void)
{
    RES_LOG("=== Registered Fonts (%zu total) ===", ARRAY_SIZE(g_font_resource_map));
    for (int i = 0; i < ARRAY_SIZE(g_font_resource_map); i++) {
        if (g_font_resource_map[i].key) {
            RES_LOG("  [%d] key='%s' font=%p", i, 
                    g_font_resource_map[i].key, g_font_resource_map[i].font);
        }
    }
}
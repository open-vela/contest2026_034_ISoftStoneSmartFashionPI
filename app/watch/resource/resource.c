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

/*********************
 *      DEFINES
 *********************/

#define ARRAY_SIZE(ARRAY) (sizeof(ARRAY) / sizeof(ARRAY[0]))

/* LVGL POSIX FS drive letter for SD card (ASCII 83 = 'S') */
#define WATCH_SD_LVGL_DRIVE  'S'

/* SD card mount point on Vela/NuttX */
#define WATCH_SD_MOUNT_PATH  "/mnt/sd"

/* Expression GIF directory relative to SD card root
 * Uses uppercase 8.3 name to match FAT filesystem storage */
#define WATCH_EXPR_DIR       "GIF"

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

/* 表情GIF文件名列表（从SD卡加载） */
#define EXPR_DEF(NAME) NAME,
static const char* g_expression_files[] = {
#include "image/Expression/gif/expression_img_src.inc"
};
#undef EXPR_DEF

/* 表情GIF完整LVGL路径缓冲区（运行时填充） */
static char g_expression_path[128];

static const int g_img_count = sizeof(g_img_resource_map) / sizeof(g_img_resource_map[0]);
static const int g_img_power_count = sizeof(g_img_power_resource_map) / sizeof(g_img_power_resource_map[0]);
static const int g_img_expression_count = sizeof(g_expression_files) / sizeof(g_expression_files[0]);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void watch_resource_init(void)
{
    /* 检查SD卡是否已挂载 */
    if (access(WATCH_SD_MOUNT_PATH, F_OK) != 0)
      {
        RES_LOG("WARNING: SD card mount point '%s' not accessible", WATCH_SD_MOUNT_PATH);
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

const char* watch_resource_get_img_expression(int index)
{
    if (index < 0 || index >= g_img_expression_count) {
        RES_LOG("Expression index %d out of range (0-%d)", index, g_img_expression_count - 1);
        return NULL;
    }

    /* 构建LVGL文件路径: "S:/image/Expression/gif/face_smile.gif" */
    snprintf(g_expression_path, sizeof(g_expression_path),
             "%c:/%s/%s",
             WATCH_SD_LVGL_DRIVE,
             WATCH_EXPR_DIR,
             g_expression_files[index]);

    return g_expression_path;
}

int watch_resource_get_expression_count(void)
{
    return g_img_expression_count;
}

int watch_resource_is_sd_ready(void)
{
    struct stat st;
    if (stat(WATCH_SD_MOUNT_PATH, &st) == 0)
      {
        return 1;
      }
    RES_LOG("SD card not ready: %s (%d)", strerror(errno), errno);
    return 0;
}

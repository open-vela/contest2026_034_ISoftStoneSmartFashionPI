/**
 * @file resource.h
 */

#ifndef RESOURCE_H
#define RESOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include <lvgl/lvgl.h>

/*********************
 *      DEFINES
 *********************/

/* 图片模块定义 */
#define RES_MODULE_DIAL     "dial"      /* 表盘模块 */
#define RES_MODULE_ASSETS   "assets"    /* 通用资源模块 */
#define RES_MODULE_LAUNCHER   "launcher"    /* 设置资源模块 */
#define RES_MODULE_SETTINGS   "settings"    /* 设置资源模块 */
#define RES_MODULE_APP      "app"       /* 应用模块 */

/* 字体定义 - 统一在这里管理 */
#if !defined(WATCH_REGULAR_FONT) && !defined(WATCH_BOLD_FONT)
#define WATCH_REGULAR_FONT "MiSans-Regular"
#define WATCH_BOLD_FONT "MiSans-Demibold"
#define WATCH_ALIBABA_REGULAR_FONT "AlibabaPuHuiTi-3-55-Regular"
#endif

/* 常用字体大小快捷定义 */
#define FONT_SIZE_20  "20"
#define FONT_SIZE_23  "23"
#define FONT_SIZE_28  "28"
#define FONT_SIZE_30  "30"
#define FONT_SIZE_40  "40"
#define FONT_SIZE_110 "110"

/**********************
 *      TYPEDEFS
 **********************/



/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initialize the resource manager.
 */
void vw_resource_init(void);

/**
 * Get a font resource by key.
 * @param key The key associated with the desired font.
 * @return A pointer to the font resource, or NULL if not found.
 */
const lv_font_t* vw_resource_get_font(const char* key);

/**
 * Get an image resource by key.
 * @param key The key associated with the desired image.
 * @return A pointer to the image resource, or NULL if not found.
 */
const void* vw_resource_get_img(const char* key);

const void* vw_resource_get_img_png(const char* key);
const void* vw_resource_get_power_img_jpg(const char* key);
const void* vw_resource_get_img_power(const char* key);

/**
 * Get a style resource by key.
 * @param key The key associated with the desired style.
 * @return A pointer to the style resource, or NULL if not found.
 */
// lv_style_t* watch_resource_get_style(const char* key);


/**
 * Debug function: print all registered fonts
 */
void vw_resource_debug_print_fonts(void);

/**
 * Get the embedded bootlogo GIF image descriptor (legacy, kept for compatibility).
 * @return A pointer to lv_image_dsc_t containing raw GIF data.
 */
const void* watch_resource_get_bootlogo_gif(void);

/**
 * Get the expression UI boot logo GIF.
 * @return A pointer to lv_image_dsc_t containing raw GIF data.
 */
const void* watch_resource_get_expr_logo_gif(void);

/**
 * Get the expression UI boot animation GIF.
 * @return A pointer to lv_image_dsc_t containing raw GIF data.
 */
const void* watch_resource_get_expr_anim_gif(void);

/**
 * Get the watch UI boot logo GIF.
 * @return A pointer to lv_image_dsc_t containing raw GIF data.
 */
const void* watch_resource_get_watch_logo_gif(void);

/**
 * Get the watch UI boot animation GIF.
 * @return A pointer to lv_image_dsc_t containing raw GIF data.
 */
const void* watch_resource_get_watch_anim_gif(void);

/**********************
 *      MACROS
 **********************/

/* 便捷宏 - 用于表盘图片 */
#define RES_IMG_DIAL(key) vw_resource_get_img_ex(RES_MODULE_DIAL, key)
/* 便捷宏 - 用于assets图片 */
#define RES_IMG_ASSETS(key) vw_resource_get_img_ex(RES_MODULE_ASSETS, key)

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*RESOURCE_H*/
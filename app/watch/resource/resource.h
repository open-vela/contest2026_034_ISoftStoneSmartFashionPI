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

/* 字体定义 - 统一管理 */
#define WATCH_REGULAR_FONT "MiSans-Regular"

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initialize the resource manager.
 * Must be called after lv_init() and before using any resource API.
 */
void watch_resource_init(void);

/**
 * Get a font resource by key.
 * @param key The key in format "FontName_Size", e.g. "MiSans-Regular_20".
 * @return A pointer to the font, or LV_FONT_DEFAULT if not found.
 */
const lv_font_t* watch_resource_get_font(const char* key);

/**
 * Get an image resource by key.
 * @param key The key associated with the desired image.
 * @return A pointer to the image resource, or NULL if not found.
 */
const void* watch_resource_get_img(const char* key);

/**
 * Get a power PNG animation frame by key.
 * @param key The frame key (e.g. "power_frame_00").
 * @return A pointer to the image descriptor, or NULL if not found.
 */
const void* watch_resource_get_img_power(const char* key);

/**
 * Get an expression GIF image descriptor by index.
 * The returned pointer is suitable for lv_gif_set_src() directly (embedded GIF data).
 * @param index The index of the expression image (0-based).
 * @return A pointer to lv_image_dsc_t containing raw GIF data,
 *         or NULL if index is out of range.
 */
const void* watch_resource_get_img_expression(int index);

/**
 * Get the total number of registered expression images.
 * @return The count of expression images.
 */
int watch_resource_get_expression_count(void);

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

/**
 * Check if the SD card is mounted and accessible.
 * @return 1 if SD card is ready, 0 otherwise.
 */
int watch_resource_is_sd_ready(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*RESOURCE_H*/

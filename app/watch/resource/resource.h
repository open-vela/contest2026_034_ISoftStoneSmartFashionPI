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

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initialize the resource manager.
 */
void watch_resource_init(void);

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
 * Get an expression GIF file path (LVGL format) by index.
 * The returned path is suitable for lv_gif_set_src() or lv_img_set_src().
 * @param index The index of the expression image (0-based).
 * @return A LVGL file path string (e.g. "S:/image/Expression/gif/face_smile.gif"),
 *         or NULL if index is out of range.
 */
const char* watch_resource_get_img_expression(int index);

/**
 * Get the total number of registered expression images.
 * @return The count of expression images.
 */
int watch_resource_get_expression_count(void);

/**
 * Check if the SD card is mounted and accessible.
 * @return 1 if SD card is ready, 0 otherwise.
 */
int watch_resource_is_sd_ready(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*RESOURCE_H*/

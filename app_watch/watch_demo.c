/**
 * @file watch_demo.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "watch_demo.h"

#include "page/page.h"
#include "resource/resource.h"

/*********************
 *      DEFINES
 *********************/
#define ROOT_VER_RES 410
#define ROOT_HOR_RES 502

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void watch_demo_create(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_palette_main(LV_PALETTE_GREY), 0);

    /* prepare base root */
    static lv_obj_t* root;
    root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, ROOT_HOR_RES, ROOT_VER_RES);
    lv_obj_center(root);

    vw_resource_init();
    page_init();

    lv_fragment_manager_t* manager = lv_fragment_manager_create(NULL);

    /* push first page */
    lv_fragment_t* page = page_create("dialplate", NULL);
    lv_fragment_manager_push(manager, page, &root);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

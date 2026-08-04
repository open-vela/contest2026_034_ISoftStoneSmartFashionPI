/****************************************************************************
 * vendor/watch/apps/launcher/dial.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <nuttx/power/axp2101.h>

#include "dial.h"
#include "app_list.h"
#include "../common/watch_pages.h"
#include "../../resource/resource.h"
#include "launcher.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/


/* 当前表盘类型 */
static dial_type_t current_dial_type = DIAL_TYPE_DEFAULT;

static lv_timer_t *g_clock_timer = NULL;
static lv_obj_t *dial_container = NULL;

/* 默认表盘对象 */
lv_obj_t *dial_bg_img = NULL;
lv_obj_t *num_hour1_img = NULL;
lv_obj_t *num_hour2_img = NULL;
lv_obj_t *num_min1_img = NULL;
lv_obj_t *num_min2_img = NULL;
lv_obj_t *step_icon_img = NULL;
lv_obj_t *bt_icon_img = NULL;
lv_obj_t *wifi_icon_img = NULL;
lv_obj_t *bat_icon_img = NULL;
lv_obj_t *steps_label = NULL;
lv_obj_t *date_label = NULL;

/* 月亮表盘对象 */
lv_obj_t *moon_bg_img = NULL;
lv_obj_t *moon_hour1_img = NULL;
lv_obj_t *moon_hour2_img = NULL;
lv_obj_t *moon_min1_img = NULL;
lv_obj_t *moon_min2_img = NULL;
lv_obj_t *moon_bt_icon = NULL;
lv_obj_t *moon_wifi_icon = NULL;
lv_obj_t *moon_bat_icon = NULL;
lv_obj_t *moon_step_img = NULL;
lv_obj_t *moon_steps_label = NULL;
lv_obj_t *moon_date_label = NULL;
lv_obj_t *moon_week_label = NULL;

/* 指针表盘对象 */
lv_obj_t *pointer_bg_img = NULL;
lv_obj_t *pointer_tooth_bg_img = NULL;
lv_obj_t *pointer_hour_img = NULL;
lv_obj_t *pointer_min_img = NULL;
lv_obj_t *pointer_sec_img = NULL;
lv_obj_t *pointer_num_3 = NULL;
lv_obj_t *pointer_num_6 = NULL;
lv_obj_t *pointer_num_9 = NULL;
lv_obj_t *pointer_num_12 = NULL;

/* 表盘切换界面对象 */
static lv_obj_t *switch_container = NULL;
static lv_obj_t *switch_indicators[3] = {NULL};
static int switch_current_index = 0;


/* 数字图片数组 - 默认表盘 - 运行时初始化 */
#define NUMBER_COUNT 10
static const void *number_images[NUMBER_COUNT];

/* 小时数字图片数组 - 月亮表盘 */
static const void *moon_hour_images[NUMBER_COUNT];

/* 分钟数字图片数组 - 月亮表盘 */
static const void *moon_min_images[NUMBER_COUNT];

/* 电量图标数组 */
#define BATTERY_COUNT 5
static const void *battery_images[BATTERY_COUNT];

/* 表盘缩略图数组 */
#define DIAL_SWITCH_COUNT 3
static const void *dial_switch_images[DIAL_SWITCH_COUNT];

static bool dial_images_initialized = false;

/* 初始化图片数组 */
static void init_dial_images(void)
{
    if (dial_images_initialized) return;
    
    /* 默认表盘数字 */
    number_images[0] = vw_resource_get_img("icon_dial_default_0");
    number_images[1] = vw_resource_get_img("icon_dial_default_1");
    number_images[2] = vw_resource_get_img("icon_dial_default_2");
    number_images[3] = vw_resource_get_img("icon_dial_default_3");
    number_images[4] = vw_resource_get_img("icon_dial_default_4");
    number_images[5] = vw_resource_get_img("icon_dial_default_5");
    number_images[6] = vw_resource_get_img("icon_dial_default_6");
    number_images[7] = vw_resource_get_img("icon_dial_default_7");
    number_images[8] = vw_resource_get_img("icon_dial_default_8");
    number_images[9] = vw_resource_get_img("icon_dial_default_9");
    
    /* 月亮表盘小时 */
    moon_hour_images[0] = vw_resource_get_img("icon_dial_moon_hour_0");
    moon_hour_images[1] = vw_resource_get_img("icon_dial_moon_hour_1");
    moon_hour_images[2] = vw_resource_get_img("icon_dial_moon_hour_2");
    moon_hour_images[3] = vw_resource_get_img("icon_dial_moon_hour_3");
    moon_hour_images[4] = vw_resource_get_img("icon_dial_moon_hour_4");
    moon_hour_images[5] = vw_resource_get_img("icon_dial_moon_hour_5");
    moon_hour_images[6] = vw_resource_get_img("icon_dial_moon_hour_6");
    moon_hour_images[7] = vw_resource_get_img("icon_dial_moon_hour_7");
    moon_hour_images[8] = vw_resource_get_img("icon_dial_moon_hour_8");
    moon_hour_images[9] = vw_resource_get_img("icon_dial_moon_hour_9");
    
    /* 月亮表盘分钟 */
    moon_min_images[0] = vw_resource_get_img("icon_dial_moon_min_0");
    moon_min_images[1] = vw_resource_get_img("icon_dial_moon_min_1");
    moon_min_images[2] = vw_resource_get_img("icon_dial_moon_min_2");
    moon_min_images[3] = vw_resource_get_img("icon_dial_moon_min_3");
    moon_min_images[4] = vw_resource_get_img("icon_dial_moon_min_4");
    moon_min_images[5] = vw_resource_get_img("icon_dial_moon_min_5");
    moon_min_images[6] = vw_resource_get_img("icon_dial_moon_min_6");
    moon_min_images[7] = vw_resource_get_img("icon_dial_moon_min_7");
    moon_min_images[8] = vw_resource_get_img("icon_dial_moon_min_8");
    moon_min_images[9] = vw_resource_get_img("icon_dial_moon_min_9");
    
    /* 电量图标 */
    battery_images[0] = vw_resource_get_img("icon_dial_bat_0");
    battery_images[1] = vw_resource_get_img("icon_dial_bat_1");
    battery_images[2] = vw_resource_get_img("icon_dial_bat_2");
    battery_images[3] = vw_resource_get_img("icon_dial_bat_3");
    battery_images[4] = vw_resource_get_img("icon_dial_bat_4");
    
    /* 表盘缩略图 */
    dial_switch_images[0] = vw_resource_get_img("icon_dial_switch_default");
    dial_switch_images[1] = vw_resource_get_img("icon_dial_switch_moon");
    dial_switch_images[2] = vw_resource_get_img("icon_dial_switch_pointer");
    
    dial_images_initialized = true;
}
	

/* 中文星期 */
const char* chineseWeekdays[] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六"
};

/* 英文星期 */
const char* englishWeekdays[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 获取电池电量百分比 */
static uint8_t get_battery_percentage(void)
{
    return axp2101_get_pmu_soc();
}

/* 根据电量百分比获取电量图标索引 (0-4) */
static int get_battery_icon_index(uint8_t percent)
{
    if (percent >= 80) return 4;
    else if (percent >= 60) return 3;
    else if (percent >= 40) return 2;
    else if (percent >= 20) return 1;
    else return 0;
}

/* 设置数字图片 */
static void set_number_image(lv_obj_t *img_obj, int number, const void *images[])
{
    if (number >= 0 && number <= 9) {
        lv_img_set_src(img_obj, images[number]);
    }
}

/* 更新默认表盘 */
static void update_default_dial(void)
{
    time_t now;
    struct tm info;
    time(&now);
    localtime_r(&now, &info);

    int hour1 = info.tm_hour / 10;
    int hour2 = info.tm_hour % 10;
    int min1 = info.tm_min / 10;
    int min2 = info.tm_min % 10;

    set_number_image(num_hour1_img, hour1, number_images);
    set_number_image(num_hour2_img, hour2, number_images);
    set_number_image(num_min1_img, min1, number_images);
    set_number_image(num_min2_img, min2, number_images);

    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%d月%d日 %s", info.tm_mon + 1, info.tm_mday, chineseWeekdays[info.tm_wday]);
    lv_label_set_text(date_label, date_str);

    uint8_t battery_percent = get_battery_percentage();
    int battery_index = get_battery_icon_index(battery_percent);
    lv_img_set_src(bat_icon_img, battery_images[battery_index]);
}

/* 更新月亮表盘 */
static void update_moon_dial(void)
{
    time_t now;
    struct tm info;
    time(&now);
    localtime_r(&now, &info);

    int hour1 = info.tm_hour / 10;
    int hour2 = info.tm_hour % 10;
    int min1 = info.tm_min / 10;
    int min2 = info.tm_min % 10;

    set_number_image(moon_hour1_img, hour1, moon_hour_images);
    set_number_image(moon_hour2_img, hour2, moon_hour_images);
    set_number_image(moon_min1_img, min1, moon_min_images);
    set_number_image(moon_min2_img, min2, moon_min_images);

    lv_label_set_text(moon_week_label, englishWeekdays[info.tm_wday]);

    char date_str[16];
    snprintf(date_str, sizeof(date_str), "%d", info.tm_mday);
    lv_label_set_text(moon_date_label, date_str);

    uint8_t battery_percent = get_battery_percentage();
    int battery_index = get_battery_icon_index(battery_percent);
    lv_img_set_src(moon_bat_icon, battery_images[battery_index]);
}

/* 更新指针表盘 */
static void update_pointer_dial(void)
{
    time_t now;
    struct tm info;
    time(&now);
    localtime_r(&now, &info);

    /* 计算指针角度 */
    int hour = info.tm_hour % 12;
    int min = info.tm_min;
    int sec = info.tm_sec;

    /* 时针角度：每小时30度，每分钟0.5度 */
    int hour_angle = (hour * 30) + (min * 0.5);
    /* 分针角度：每分钟6度 */
    int min_angle = min * 6;
    /* 秒针角度：每秒6度 */
    int sec_angle = sec * 6;

    /* 设置指针旋转角度 */
    lv_img_set_angle(pointer_hour_img, hour_angle * 10);
    lv_img_set_angle(pointer_min_img, min_angle * 10);
    lv_img_set_angle(pointer_sec_img, sec_angle * 10);
}

/* 更新时钟回调函数 */
static void update_clock_cb(lv_timer_t *timer)
{
    switch (current_dial_type) {
        case DIAL_TYPE_DEFAULT:
            update_default_dial();
            break;
        case DIAL_TYPE_MOON:
            update_moon_dial();
            break;
        case DIAL_TYPE_POINTER:
            update_pointer_dial();
            break;
        default:
            break;
    }

    /* lv_task_handler() 已在主循环中调用,此处不需要再调用 */
}

/* 创建默认表盘 */
static void create_default_dial(lv_obj_t *parent)
{
    dial_bg_img = lv_img_create(parent);
    lv_img_set_src(dial_bg_img, vw_resource_get_img("icon_dial_default_bg"));
    lv_obj_align(dial_bg_img, LV_ALIGN_CENTER, 0, 0);

    /* 右上角电量图标Y向下偏移20px，从右向左偏移30 */
    bat_icon_img = lv_img_create(parent);
    uint8_t init_battery = get_battery_percentage();
    int init_index = get_battery_icon_index(init_battery);
    lv_img_set_src(bat_icon_img, battery_images[init_index]);
    lv_obj_align(bat_icon_img, LV_ALIGN_TOP_RIGHT, -34, 20);

    /* 蓝牙Y向下20px，根据电量64*64，基于电量向左5px */
    bt_icon_img = lv_img_create(parent);
    lv_img_set_src(bt_icon_img, vw_resource_get_img("icon_dial_bt"));
    lv_obj_align(bt_icon_img, LV_ALIGN_TOP_RIGHT, -72, 20);

    /* WIFIY向下20px，根据电量64*64，基于电量向左5px */
    wifi_icon_img = lv_img_create(parent);
    lv_img_set_src(wifi_icon_img, vw_resource_get_img("icon_dial_wifi"));
    lv_obj_align(wifi_icon_img, LV_ALIGN_TOP_RIGHT, -105, 20);

    step_icon_img = lv_img_create(parent);
    lv_img_set_src(step_icon_img, vw_resource_get_img("icon_dial_steps"));
    lv_obj_align(step_icon_img, LV_ALIGN_TOP_LEFT, 162, 44);

    /* 步数label向下偏移42px，左右居中 */
    steps_label = lv_label_create(parent);
    lv_label_set_text(steps_label, "0");
    lv_obj_set_style_text_font(steps_label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(steps_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align_to(steps_label, step_icon_img, LV_ALIGN_OUT_RIGHT_MID, 3, 0);

    /* 小时数字十位Y为123px,从右向左214px */
    num_hour1_img = lv_img_create(parent);
    lv_img_set_src(num_hour1_img, vw_resource_get_img("icon_dial_default_0"));
    lv_obj_align(num_hour1_img, LV_ALIGN_TOP_RIGHT, -189, 128);

    /* 小时个位Y：123px，X从左向右218px */
    num_hour2_img = lv_img_create(parent);
    lv_img_set_src(num_hour2_img, vw_resource_get_img("icon_dial_default_0"));
    lv_obj_align(num_hour2_img, LV_ALIGN_TOP_LEFT, 210, 128);

    /* 分钟十位Y从上向下：267px,X从右向左204px */
    num_min1_img = lv_img_create(parent);
    lv_img_set_src(num_min1_img, vw_resource_get_img("icon_dial_default_0"));
    lv_obj_align(num_min1_img, LV_ALIGN_TOP_RIGHT, -189, 272);

    /* 分钟个位Y：267px，X从左向右218px */
    num_min2_img = lv_img_create(parent);
    lv_img_set_src(num_min2_img, vw_resource_get_img("icon_dial_default_0"));
    lv_obj_set_pos(num_min2_img, 210, 272);

    /* 下方日期左右居中，Y：430 ，字号26 */
    date_label = lv_label_create(parent);
    lv_label_set_text(date_label, "1月1日 周一");
    lv_obj_set_style_text_font(date_label, vw_resource_get_font(WATCH_REGULAR_FONT "_26"), 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0, 430);
}

/* 创建月亮表盘 */
static void create_moon_dial(lv_obj_t *parent)
{
    moon_bg_img = lv_img_create(parent);
    lv_img_set_src(moon_bg_img, vw_resource_get_img("icon_dial_moon_bg"));
    lv_obj_align(moon_bg_img, LV_ALIGN_CENTER, 0, 0);

    /* 右上角电量图标Y向下偏移20px，从右向左偏移30 */
    moon_bat_icon = lv_img_create(parent);
    uint8_t init_battery = get_battery_percentage();
    int init_index = get_battery_icon_index(init_battery);
    lv_img_set_src(moon_bat_icon, battery_images[init_index]);
    lv_obj_align(moon_bat_icon, LV_ALIGN_TOP_RIGHT, -34, 20);

    /* 蓝牙Y向下20px，根据电量64*64，基于电量向左5px */
    moon_bt_icon = lv_img_create(parent);
    lv_img_set_src(moon_bt_icon, vw_resource_get_img("icon_dial_bt"));
    lv_obj_align(moon_bt_icon, LV_ALIGN_TOP_RIGHT, -72, 20);

    /* WIFIY向下20px，根据电量64*64，基于电量向左5px */
    moon_wifi_icon = lv_img_create(parent);
    lv_img_set_src(moon_wifi_icon, vw_resource_get_img("icon_dial_wifi"));
    lv_obj_align(moon_wifi_icon, LV_ALIGN_TOP_RIGHT, -105, 20);

    /* 小时数字十位Y为74px,从右向左158px */
    moon_hour1_img = lv_img_create(parent);
    lv_img_set_src(moon_hour1_img, vw_resource_get_img("icon_dial_moon_hour_0"));
    lv_obj_align(moon_hour1_img, LV_ALIGN_TOP_RIGHT, -120, 74);

    /* 小时个位Y：74px，X从左向右270px */
    moon_hour2_img = lv_img_create(parent);
    lv_img_set_src(moon_hour2_img, vw_resource_get_img("icon_dial_moon_hour_0"));
    lv_obj_set_pos(moon_hour2_img, 263, 74);

    /* 分钟十位Y从上向下：207px,X从右向左100px */
    moon_min1_img = lv_img_create(parent);
    lv_img_set_src(moon_min1_img, vw_resource_get_img("icon_dial_moon_min_0"));
    lv_obj_align(moon_min1_img, LV_ALIGN_TOP_RIGHT, -60, 207);

    /* 分钟个位Y：207px，X从左向右320px */
    moon_min2_img = lv_img_create(parent);
    lv_img_set_src(moon_min2_img, vw_resource_get_img("icon_dial_moon_min_0"));
    lv_obj_set_pos(moon_min2_img, 318, 207);

    /* FRI日期Y偏移239px，X从左向右177px，字号26 */
    moon_week_label = lv_label_create(parent);
    lv_label_set_text(moon_week_label, "FRI");
    lv_obj_set_style_text_font(moon_week_label, vw_resource_get_font(WATCH_REGULAR_FONT "_26"), 0);
    lv_obj_set_style_text_color(moon_week_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(moon_week_label, 177, 239);

    /* 下方具体日位于星期正下方Y间隔5px，字号26 */
    moon_date_label = lv_label_create(parent);
    lv_label_set_text(moon_date_label, "24");
    lv_obj_set_style_text_font(moon_date_label, vw_resource_get_font(WATCH_REGULAR_FONT "_26"), 0);
    lv_obj_set_style_text_color(moon_date_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align_to(moon_date_label, moon_week_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);

    moon_step_img = lv_img_create(parent);
    lv_img_set_src(moon_step_img, vw_resource_get_img("icon_dial_steps"));
    lv_obj_align(moon_step_img, LV_ALIGN_TOP_LEFT, 258, 368);

    /* 步数label向下Y偏移366px， X从左向右299px，字号26 */
    moon_steps_label = lv_label_create(parent);
    lv_label_set_text(moon_steps_label, "2561");
    lv_obj_set_style_text_font(moon_steps_label, vw_resource_get_font(WATCH_REGULAR_FONT "_26"), 0);
    lv_obj_set_style_text_color(moon_steps_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(moon_steps_label, 299, 366);
}

/* 创建指针表盘 */
static void create_pointer_dial(lv_obj_t *parent)
{
    pointer_tooth_bg_img = lv_img_create(parent);
    lv_img_set_src(pointer_tooth_bg_img, vw_resource_get_img("icon_dial_pointer_tooth_bg")); 
    lv_obj_align(pointer_tooth_bg_img, LV_ALIGN_CENTER, 0, 0);
    
    pointer_bg_img = lv_img_create(parent);
    lv_img_set_src(pointer_bg_img, vw_resource_get_img("icon_dial_pointer_bg"));
    lv_obj_align(pointer_bg_img, LV_ALIGN_CENTER, 0, 0);

    /* 数字 12 - 顶部，左右居中，向左10px */
    pointer_num_12 = lv_img_create(parent);
    lv_img_set_src(pointer_num_12, vw_resource_get_img("icon_dial_pointer_12"));
    lv_obj_align(pointer_num_12, LV_ALIGN_TOP_MID, 0, 20);

    /* 数字 3 - 右侧，上下居中，向左10px */
    pointer_num_3 = lv_img_create(parent);
    lv_img_set_src(pointer_num_3, vw_resource_get_img("icon_dial_pointer_3"));
    lv_obj_align(pointer_num_3, LV_ALIGN_RIGHT_MID, -20, 0);

    /* 数字 6 - 底部，向上20px，向左10px */
    pointer_num_6 = lv_img_create(parent);
    lv_img_set_src(pointer_num_6, vw_resource_get_img("icon_dial_pointer_6"));
    lv_obj_align(pointer_num_6, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* 数字 9 - 左侧，上下居中，向左10px */
    pointer_num_9 = lv_img_create(parent);
    lv_img_set_src(pointer_num_9, vw_resource_get_img("icon_dial_pointer_9"));
    lv_obj_align(pointer_num_9, LV_ALIGN_LEFT_MID, 20, 0);

    /* 时针 - 屏幕中心 (205, 251)，尾部交叉 */
    pointer_hour_img = lv_img_create(parent);
    lv_img_set_src(pointer_hour_img, vw_resource_get_img("icon_dial_pointer_hour"));
    lv_obj_set_pos(pointer_hour_img, 205 - 4, 251 - 4); /* 指针尾部位于屏幕中心，98是时针宽度，4是时针高度的一半 */
    lv_img_set_pivot(pointer_hour_img, 4, 4); /* 设置旋转中心为指针尾部中心 */

    /* 分针 - 屏幕中心 (205, 251)，尾部交叉 */
    pointer_min_img = lv_img_create(parent);
    lv_img_set_src(pointer_min_img, vw_resource_get_img("icon_dial_pointer_min"));
    lv_obj_set_pos(pointer_min_img, 205 - 4, 251 - 4); /* 指针尾部位于屏幕中心，140是分针宽度，4是分针高度的一半 */
    lv_img_set_pivot(pointer_min_img, 4, 4); /* 设置旋转中心为指针尾部中心 */

    /* 秒针 - 屏幕中心 (205, 251)，尾部交叉 */
    pointer_sec_img = lv_img_create(parent);
    lv_img_set_src(pointer_sec_img, vw_resource_get_img("icon_dial_pointer_sec"));
    lv_obj_set_pos(pointer_sec_img, 205 - 3, 251 - 9); /* 指针尾部位于屏幕中心，137是秒针宽度，3是秒针高度的一半 */
    lv_img_set_pivot(pointer_sec_img, 3, -5); /* 设置旋转中心为指针尾部中心 */
}

/* 表盘切换界面指示灯更新 */
static void update_switch_indicators(int index)
{
    for (int i = 0; i < 3; i++) {
        if (i == index) {
            lv_obj_set_size(switch_indicators[i], 12, 6);
            lv_obj_set_style_bg_color(switch_indicators[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_size(switch_indicators[i], 6, 6);
            lv_obj_set_style_bg_color(switch_indicators[i], lv_color_hex(0x808080), 0);
        }
    }
}

/* 表盘切换界面 - 缩略图点击回调 */
static void switch_dial_thumbnail_click_cb(lv_event_t *e)
{
    int *index = (int *)lv_event_get_user_data(e);

    printf("Selected dial: %d\n", *index);

    /* 更新当前表盘类型 */
    current_dial_type = (dial_type_t)*index;

    /* 从页面栈中移除切换界面 */
    if (switch_container) {
        vw_watch_pop_page(switch_container);
        lv_obj_delete(switch_container);
        switch_container = NULL;
    }

    /* 删除旧表盘 */
    lv_obj_t *old_dial = dial_get_container();
    if (old_dial) {
        lv_obj_delete(old_dial);
    }
    
    /* 清理旧表盘全局变量 */
    dial_deinit(NULL);

    /* 重新创建表盘 - 基于 content_area */
    dial_init(lv_watch_get_content_area());
}

/* 表盘切换界面 - 滑动回调 */
static void switch_dial_scroll_cb(lv_event_t *e)
{
    lv_obj_t *cont = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_SCROLL) {
        lv_coord_t x = lv_obj_get_scroll_x(cont);
        int page_width = WATCH_SCREEN_WIDTH; /* 每页宽度410px */
        int new_index = (x + page_width / 2) / page_width;
        
        if (new_index < 0) new_index = 0;
        if (new_index >= 3) new_index = 2;
        
        if (new_index != switch_current_index) {
            switch_current_index = new_index;
            update_switch_indicators(switch_current_index);
        }
    }
}

/* 创建表盘切换界面 */
static void create_dial_switch_ui(lv_obj_t *parent)
{
    switch_container = lv_obj_create(parent);
    lv_obj_set_size(switch_container, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(switch_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(switch_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(switch_container, 0, 0);
    lv_obj_set_style_radius(switch_container, 0, 0);
    lv_obj_center(switch_container);
    lv_obj_clear_flag(switch_container, LV_OBJ_FLAG_SCROLLABLE);
    
    /* 在上方26px位置添加"切换表盘"文字 */
    lv_obj_t *switch_title = lv_label_create(switch_container);
    lv_label_set_text(switch_title, "切换表盘");
    lv_obj_set_style_text_font(switch_title, vw_resource_get_font(WATCH_REGULAR_FONT "_32"), 0);
    lv_obj_set_style_text_color(switch_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(switch_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(switch_title, LV_ALIGN_TOP_MID, 0, 26);
    
    /* 创建缩略图容器 - 使用flex布局，水平排列3页 */
    lv_obj_t *thumb_container = lv_obj_create(switch_container);
    lv_obj_set_size(thumb_container, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_set_pos(thumb_container, 0, 0);
    lv_obj_set_style_bg_opa(thumb_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(thumb_container, 0, 0);
    lv_obj_set_style_pad_all(thumb_container, 0, 0);
    
    /* 设置水平滚动 */
    lv_obj_set_scroll_dir(thumb_container, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(thumb_container, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(thumb_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(thumb_container, switch_dial_scroll_cb, LV_EVENT_SCROLL, NULL);
    
    /* 创建3个页面容器，每个页面410x502 */
    static int dial_indices[3] = {0, 1, 2};
    for (int i = 0; i < 3; i++) {
        /* 创建页面容器 */
        lv_obj_t *page = lv_obj_create(thumb_container);
        lv_obj_set_size(page, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
        lv_obj_set_pos(page, i * WATCH_SCREEN_WIDTH, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        
        /* 在页面中创建缩略图，212x260居中显示 */
        lv_obj_t *thumb = lv_img_create(page);
        lv_img_set_src(thumb, dial_switch_images[i]);
        /* 缩略图居中：(410-212)/2=99, (502-260)/2=121 */
        lv_obj_set_pos(thumb, 99, 121);
        lv_obj_add_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(thumb, switch_dial_thumbnail_click_cb, LV_EVENT_CLICKED, &dial_indices[i]);
    }
    
    /* 创建指示灯 - 位于底部 */
    int indicator_y = 462;  /* 底部位置 */
    int indicator_spacing = 10;
    int total_width = 3 * 6 + 2 * indicator_spacing;
    int start_x = (WATCH_SCREEN_WIDTH - total_width) / 2;
    
    for (int i = 0; i < 3; i++) {
        switch_indicators[i] = lv_obj_create(switch_container);
        lv_obj_set_size(switch_indicators[i], 6, 6);
        lv_obj_set_pos(switch_indicators[i], start_x + i * (6 + indicator_spacing), indicator_y);
        lv_obj_set_style_bg_color(switch_indicators[i], lv_color_hex(0x808080), 0);
        lv_obj_set_style_bg_opa(switch_indicators[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(switch_indicators[i], 0, 0);
        lv_obj_set_style_radius(switch_indicators[i], 3, 0);
        lv_obj_clear_flag(switch_indicators[i], LV_OBJ_FLAG_CLICKABLE);
    }
    
    update_switch_indicators(current_dial_type);
    switch_current_index = current_dial_type;
    
    /* 滚动到当前表盘的位置 */
    lv_obj_scroll_to_x(thumb_container, current_dial_type * WATCH_SCREEN_WIDTH, LV_ANIM_OFF);
}

/* 长按事件回调 */
static void dial_long_press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_LONG_PRESSED) {
        printf("Long press detected, opening dial switch UI\n");

        /* 隐藏表盘，保持常驻内存 */
        dial_hide();

        /* 创建表盘切换界面 - 基于 lv_scr_act() 创建独立层 */
        create_dial_switch_ui(lv_scr_act());

        /* 将表盘切换页添加到页面栈 */
        if (switch_container != NULL) {
            vw_watch_push_page(switch_container);
        }
    }
}
/* 时钟界面滑动手势处理函数 */
static void clock_screen_gesture_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_point = {0};

    switch(code) {
        case LV_EVENT_PRESSED:
            lv_indev_get_point(lv_indev_active(), &start_point);
            break;

        case LV_EVENT_RELEASED:
            {
                lv_point_t end_point;
                lv_indev_get_point(lv_indev_active(), &end_point);

                int32_t delta_x = end_point.x - start_point.x;
                int32_t delta_y = end_point.y - start_point.y;

                /* 左滑切换到应用列表 */
                if(delta_x < -50 && abs(delta_x) > abs(delta_y))
                {
                    printf("Swipe left to app list\n");
                    
                    /* 隐藏表盘，保持常驻内存 */
                    dial_hide();
                    
                    /* 进入APP管理界面 - 基于 lv_scr_act() 创建独立层 */
                    lv_obj_t *app_list = app_tile_setup(lv_scr_act());
                    
                    /* 加入页面栈 */
                    if (app_list != NULL) {
                        vw_watch_push_page(app_list);
                    }
                }
            }
            break;
        default:
            break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief 初始化并显示时钟界面
 *
 * @param parent 父容器对象
 * @return lv_obj_t* 返回创建的时钟对象
 */
lv_obj_t *dial_init(lv_obj_t *parent)
{
    printf("dial_init start: parent=%p\n", parent);
    
    /* 初始化图片数组 */
    printf("[DIAL] Step 1: init_dial_images\n");
    fflush(stdout);
    init_dial_images();
    
    /* 如果已存在时钟定时器,先删除 */
    if (g_clock_timer != NULL)
    {
        printf("[DIAL] Step 2: Deleting existing clock timer\n");
        fflush(stdout);
        lv_timer_del(g_clock_timer);
        g_clock_timer = NULL;
    }
    
    /* 如果已存在表盘容器,先删除 */
    if (dial_container != NULL)
    {
        printf("[DIAL] Step 3: Deleting existing dial container\n");
        fflush(stdout);
        lv_obj_del(dial_container);
        dial_container = NULL;
    }
    
    /* 创建背景容器 */
    dial_container = lv_obj_create(parent);
    lv_obj_set_size(dial_container, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(dial_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(dial_container, 0, 0);
    lv_obj_set_style_radius(dial_container, 0, 0);
    lv_obj_center(dial_container);
    lv_obj_clear_flag(dial_container, LV_OBJ_FLAG_SCROLLABLE);

    /* 根据当前表盘类型创建对应的表盘 */
    switch (current_dial_type) {
        case DIAL_TYPE_DEFAULT:
            create_default_dial(dial_container);
            break;
        case DIAL_TYPE_MOON:
            create_moon_dial(dial_container);
            break;
        case DIAL_TYPE_POINTER:
            create_pointer_dial(dial_container);
            break;
        default:
            create_default_dial(dial_container);
            break;
    }

    /* 添加滑动手势事件 */
    lv_obj_add_event_cb(dial_container, clock_screen_gesture_handler, LV_EVENT_ALL, NULL);
    
    /* 添加长按事件 */
    lv_obj_add_event_cb(dial_container, dial_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* 创建时钟定时器 */
    g_clock_timer = lv_timer_create(update_clock_cb, 1000, NULL);

    /* 立即更新一次 */
    update_clock_cb(NULL);
    
    printf("dial_init end.\n");
    fflush(stdout);

    return dial_container;
}
/**
 * @brief 销毁时钟界面
 *
 * @param clock_obj 时钟对象
 */
void dial_deinit(lv_obj_t *clock_obj)
{
    printf("dial_deinit: clock_obj=%p\n", clock_obj);
    
    /* 删除时钟定时器 */
    if (g_clock_timer != NULL)
    {
        printf("[DIAL_DEINIT] Deleting clock timer\n");
        lv_timer_del(g_clock_timer);
        g_clock_timer = NULL;
    }
    
    /* 重置所有全局对象指针 */
    dial_bg_img = NULL;
    num_hour1_img = NULL;
    num_hour2_img = NULL;
    num_min1_img = NULL;
    num_min2_img = NULL;
    step_icon_img = NULL;
    bt_icon_img = NULL;
    wifi_icon_img = NULL;
    bat_icon_img = NULL;
    steps_label = NULL;
    date_label = NULL;
    
    moon_bg_img = NULL;
    moon_hour1_img = NULL;
    moon_hour2_img = NULL;
    moon_min1_img = NULL;
    moon_min2_img = NULL;
    moon_bt_icon = NULL;
    moon_wifi_icon = NULL;
    moon_bat_icon = NULL;
    moon_step_img = NULL;
    moon_steps_label = NULL;
    moon_date_label = NULL;
    moon_week_label = NULL;
    
    pointer_bg_img = NULL;
    pointer_tooth_bg_img = NULL;
    pointer_hour_img = NULL;
    pointer_min_img = NULL;
    pointer_sec_img = NULL;
    pointer_num_3 = NULL;
    pointer_num_6 = NULL;
    pointer_num_9 = NULL;
    pointer_num_12 = NULL;
    
    dial_container = NULL;
    
    printf("[DIAL_DEINIT] All global variables reset\n");
}

/**
 * @brief 获取表盘容器对象
 */
lv_obj_t* dial_get_container(void)
{
    return dial_container;
}

/**
 * @brief 显示表盘
 */
void dial_show(void)
{
    if (dial_container != NULL) {
        lv_obj_remove_flag(dial_container, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 隐藏表盘
 */
void dial_hide(void)
{
    if (dial_container != NULL) {
        lv_obj_add_flag(dial_container, LV_OBJ_FLAG_HIDDEN);
    }
}



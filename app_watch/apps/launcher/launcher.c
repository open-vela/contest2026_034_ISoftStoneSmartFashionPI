/****************************************************************************
 * vendor/watch/apps/launcher/launcher.c
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
#include <syslog.h>

#include "launcher.h"
#include "../boot/boot_logo.h"
#include "../boot/boot_animation.h"
#include "dial.h"
#include <../../../../apps/graphics/lvgl/lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.h>
#include "app_list.h"
#include "../alarm/alarm.h"
#include "../common/watch_pages.h"
#include "../common/sensor_data.h"

#include <nuttx/power/axp2101.h>
#include <nuttx/power/pm.h>
#include "../settings/settings.h"

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define LAUNCHER_LOG(fmt, ...) syslog(LOG_INFO, "[LAUNCHER] " fmt, ##__VA_ARGS__)

/* 充电界面操作函数声明 */
extern void setting_charging_create(void);
extern void setting_charging_destroy(void);
extern bool setting_charging_is_active(void);


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LOGO_DISPLAY_TIME      1000   /* 开机logo显示时间(ms) */
#define ANIMATION_DISPLAY_TIME 9000   /* 开机动画显示时间(ms) - 96帧×50ms+余量 */

/****************************************************************************
 * Private Data
 ****************************************************************************/

typedef enum
{
  STATE_INIT,         /* 初始状态 */
  STATE_SHOW_LOGO,    /* 显示logo状态 */
  STATE_SHOW_ANIM,    /* 显示动画状态 */
  STATE_SHOW_CLOCK,   /* 显示时钟状态 */
  STATE_DONE          /* 完成状态 */
} launcher_state_t;

/* 页面栈相关定义 */
#define MAX_PAGE_STACK_SIZE 30  /* 最大页面栈大小 */

static launcher_state_t current_state = STATE_INIT; /* 当前状态 */
static lv_obj_t *content_area = NULL;           /* 内容区域 */
static lv_obj_t *current_obj = NULL;            /* 当前显示的对象 */
static lv_obj_t *page_stack[MAX_PAGE_STACK_SIZE]; /* 页面栈 */
static int page_stack_size = 0;                /* 页面栈大小 */
static lv_timer_t *charging_timer = NULL;      /* 充电状态检测定时器 */
static void state_timer_cb(lv_timer_t *timer);
static void __attribute__((unused)) charging_monitor_cb(lv_timer_t *timer);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief 切换到下一个状态
 */
static void goto_next_state(void)
{
  switch (current_state)
    {
      case STATE_INIT:
        /* 切换到显示logo状态 */
        current_state = STATE_SHOW_LOGO;
        if (current_obj != NULL)
          {
            lv_obj_del(current_obj);
          }
        current_obj = boot_logo_init(content_area);
        lv_task_handler();
        
        lv_timer_t *timer1 = lv_timer_create(state_timer_cb, LOGO_DISPLAY_TIME, NULL);
        lv_timer_set_repeat_count(timer1, 1);
        break;

      case STATE_SHOW_LOGO:
        /* 切换到显示动画状态 */
        current_state = STATE_SHOW_ANIM;

        /* 销毁logo并显示动画 */
        boot_logo_deinit(current_obj);
        LAUNCHER_LOG("boot_animation init, start load power anim: %p", current_obj);
        current_obj = boot_animation_init(content_area);
        lv_task_handler();
        
        /* 设置动画显示时间 */
        lv_timer_t *timer2 = lv_timer_create(state_timer_cb, ANIMATION_DISPLAY_TIME, NULL);
        lv_timer_set_repeat_count(timer2, 1);
        break;

      case STATE_SHOW_ANIM:
        /* 切换到显示时钟状态 */
        current_state = STATE_SHOW_CLOCK;
        LAUNCHER_LOG("destory power anim, start dial init");

        /* 销毁动画并显示时钟 */
        boot_animation_deinit(current_obj);
        current_obj = dial_init(content_area);
        lv_task_handler();
        alarm_service_init();
        pm_stay(PM_IDLE_DOMAIN, PM_NORMAL);
        display_set_timeout(60);
        ft3168_display_timeout_setup();
        LAUNCHER_LOG("dial init complete: %p", current_obj);

        /* 启动充电状态检测定时器 (1秒检测一次) */
        if (charging_timer == NULL) {
            // charging_timer = lv_timer_create(charging_monitor_cb, 1000, NULL);
            LAUNCHER_LOG("[CHARGE] Charging monitor timer started");
        }

        /* 读取抬腕亮屏配置 */
        wrist_raise_load_config();

        /* 启动传感器监控定时器（抬腕+摔倒统一检测） */
        sensor_wrist_raise_timer_start();

        break;

      default:
        /* 其他状态不处理 */
        break;
    }
}

/**
 * @brief 充电状态监控回调函数
 * 
 * @param timer 定时器对象
 */
static void __attribute__((unused)) charging_monitor_cb(lv_timer_t *timer)
{
    uint8_t charge_status = axp2101_get_pmu_charge_status();
    LAUNCHER_LOG("[CHARGE] Charge status: %#X", charge_status);
    
    // 如果正在充电，显示充电界面
    if (charge_status == 1) {
        // 只在界面不存在时创建
        if (!setting_charging_is_active()) {
            setting_charging_create();
        }
    } else {
        // 非充电状态，销毁充电界面
        if (setting_charging_is_active()) {
            setting_charging_destroy();
        }
    }
}

/**
 * @brief 状态定时器回调函数
 * 
 * @param timer 定时器对象
 */
static void state_timer_cb(lv_timer_t *timer)
{
  goto_next_state();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

//  static lv_obj_t * dial_container = NULL;
 
/**
 * @brief 清理 vendor 手表 UI 全部资源（无重启切换用）
 *
 * 停止所有定时器、清空页面栈、删除表盘与 content_area。
 * 调用后可通过 vw_launcher_init() 重新初始化。
 */
void vw_launcher_deinit(void)
{
  LAUNCHER_LOG("vw_launcher_deinit start");

  /* 1. 停止业务定时器 */
  if (charging_timer != NULL)
    {
      lv_timer_del(charging_timer);
      charging_timer = NULL;
    }

  /* 2. 停止闹钟服务与传感器监控 */
  alarm_service_deinit();
  sensor_wrist_raise_timer_stop();
  sensor_data_deinit();

  /* 3. 删除页面栈中所有页面（反向删除，与 lv_watch_back_go_home 一致） */
  for (int i = page_stack_size - 1; i >= 0; i--)
    {
      if (page_stack[i] != NULL)
        {
          lv_obj_del_async(page_stack[i]);
          page_stack[i] = NULL;
        }
    }
  page_stack_size = 0;

  /* 4. 删除表盘（含 g_clock_timer） */
  if (dial_get_container() != NULL)
    {
      dial_deinit(dial_get_container());
    }

  /* 5. 删除当前显示对象与内容区域 */
  if (current_obj != NULL)
    {
      lv_obj_del(current_obj);
      current_obj = NULL;
    }

  if (content_area != NULL)
    {
      lv_obj_del(content_area);
      content_area = NULL;
    }

  /* 6. 重置状态机，允许重新初始化 */
  current_state = STATE_INIT;

  LAUNCHER_LOG("vw_launcher_deinit done");
}

/**
 * @brief 初始化并运行主页流程
 * 
 * @param parent 父容器对象
 * @return int 成功返回0，失败返回负值
 */
int vw_launcher_init(lv_obj_t *parent)
{
  /* 创建内容区域 */
  content_area = lv_obj_create(parent);
  lv_obj_set_size(content_area, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_border_width(content_area, 0, 0);
  lv_obj_set_style_bg_color(content_area, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(content_area, 0, 0);
  lv_obj_clear_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(content_area, 0, 0);
  lv_obj_center(content_area);

  /* 启动主页流程 */
  /* 开机logo和动画已由 NuttX launcher (app/watch/apps/launcher/launcher.c)
   * 完成，此处直接跳过开机流程，进入表盘界面 */
  current_state = STATE_SHOW_ANIM;
  goto_next_state();

  return 0;
}

/**
 * @brief 将页面添加到页面栈
 * 
 * @param page 页面对象
 * @return int 成功返回0，失败返回负值
 */
int vw_watch_push_page(lv_obj_t *page)
{
    if (page == NULL) {
        return -1;
    }

    if (page_stack_size >= MAX_PAGE_STACK_SIZE) {
        return -2;
    }
    LAUNCHER_LOG("push page to stack: %p", page);

    page_stack[page_stack_size++] = page;
    return 0;
}

/**
 * @brief 从页面栈中移除页面
 * 
 * @param page 页面对象
 * @return int 成功返回0，失败返回负值
 */
int vw_watch_pop_page(lv_obj_t *page)
{
    if (page == NULL || page_stack_size == 0) {
        return -1;
    }

    int i;
    for (i = 0; i < page_stack_size; i++) {
        if (page_stack[i] == page) {
        break;
        }
    }

    if (i >= page_stack_size) {
        return -2;
    }
    LAUNCHER_LOG("pop page from stack: %p", page);

    for (; i < page_stack_size - 1; i++) {
        page_stack[i] = page_stack[i + 1];
    }

    page_stack_size--;
    return 0;
}

/**
 * @brief 返回主页函数
 *
 * @return int 成功返回0，失败返回负值
 */
int lv_watch_back_go_home(void)
{
    LAUNCHER_LOG("[BACK_HOME] Start: returning to dial...");
    
    /* 先显示隐藏的表盘 */
    if (dial_get_container() != NULL) {
        LAUNCHER_LOG("[BACK_HOME] Showing dial");
        dial_show();
        LAUNCHER_LOG("[BACK_HOME] Dial displayed");
    } else {
        LAUNCHER_LOG("[BACK_HOME] ERROR: dial_container is NULL");
        return -1;
    }

    /* 再删除页面栈中的所有页面 */
    LAUNCHER_LOG("[BACK_HOME] Deleting %d pages from stack", page_stack_size);
    
    /* 反向删除页面栈（从栈顶开始） */
    for (int i = page_stack_size - 1; i >= 0; i--) {
        if (page_stack[i] != NULL) {
            LAUNCHER_LOG("[BACK_HOME] Deleting page_stack[%d]: %p", i, page_stack[i]);
            lv_obj_del_async(page_stack[i]);
            page_stack[i] = NULL;
        }
    }
    page_stack_size = 0;
    LAUNCHER_LOG("[BACK_HOME] All pages deleted");

    LAUNCHER_LOG("[BACK_HOME] Done");
    return 0;
}

/**
 * @brief 检查当前是否在主页（页面栈为空）
 *
 * @return int 在主页返回1，否则返回0
 */
int lv_watch_is_home(void)
{
    return (page_stack_size == 0) ? 1 : 0;
}

/**
 * @brief 获取内容区域对象
 *
 * @return lv_obj_t* 内容区域对象指针
 */
lv_obj_t* lv_watch_get_content_area(void)
{
    return content_area;
}

/**
 * @brief 设置当前显示对象
 *
 * @param obj 当前显示的对象
 */
void lv_watch_set_current_obj(lv_obj_t *obj)
{
    LAUNCHER_LOG("Setting current_obj from %p to %p", current_obj, obj);
    current_obj = obj;
}


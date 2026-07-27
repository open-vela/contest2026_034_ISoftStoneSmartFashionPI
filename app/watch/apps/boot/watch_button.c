/****************************************************************************
 * apps/watch/apps/boot/watch_button.c
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
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <time.h>

#include <lvgl/lvgl.h>
#include "esp32s3_gpio.h"

#include "watch_button.h"
#include "../settings/settings.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Button GPIO definitions */
#define BOOT_BUTTON_GPIO    0   /* Boot button: pressed = LOW (0), released = HIGH (1) */
#define PWR_BUTTON_GPIO     10  /* PWR button: active HIGH (pressed=1, released=0) */

/* Long press duration in milliseconds (10 seconds) */
#define LONG_PRESS_DURATION_MS  10000

/* Short press duration in milliseconds (100ms) */
#define SHORT_PRESS_DURATION_MS 100

/* Polling interval in milliseconds */
#define POLL_INTERVAL_MS        10

/* Debounce threshold: require N consecutive consistent readings */
#define DEBOUNCE_THRESHOLD      5

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum button_state_e
{
  BUTTON_STATE_NOT_PRESSED = 0,
  BUTTON_STATE_SHORT_PRESS = 1,
  BUTTON_STATE_LONG_PRESS  = 2
};

enum button_type_e
{
  BUTTON_TYPE_BOOT = 0,
  BUTTON_TYPE_PWR  = 1
};

struct button_context_s
{
  enum button_type_e type;
  int gpio;
  bool active_low;
  bool last_state;
  bool is_pressed;
  clock_t press_start_time;
  enum button_state_e state;
  bool debounced_pressed;
  uint8_t debounce_count;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct button_context_s g_boot_button;
static struct button_context_s g_pwr_button;
static lv_timer_t *g_button_timer = NULL;
static bool g_settings_active = false;  /* 跟踪设置界面是否打开 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool button_get_gpio_state(struct button_context_s *ctx)
{
  bool gpio_value;
  bool pressed;

  gpio_value = esp32s3_gpioread(ctx->gpio);

  if (ctx->active_low)
    {
      pressed = !gpio_value;
    }
  else
    {
      pressed = gpio_value;
    }

  return pressed;
}

static void button_ctx_init(struct button_context_s *ctx,
                            enum button_type_e type,
                            int gpio,
                            bool active_low)
{
  ctx->type = type;
  ctx->gpio = gpio;
  ctx->active_low = active_low;
  ctx->last_state = false;
  ctx->is_pressed = false;
  ctx->press_start_time = 0;
  ctx->state = BUTTON_STATE_NOT_PRESSED;
  ctx->debounced_pressed = false;
  ctx->debounce_count = 0;

  if (active_low)
    {
      esp32s3_configgpio(gpio, INPUT_PULLUP);
    }
  else
    {
      esp32s3_configgpio(gpio, INPUT_PULLDOWN);
    }
}

static enum button_state_e button_update(struct button_context_s *ctx)
{
  bool current_pressed;
  bool raw_pressed;
  clock_t current_time;
  clock_t elapsed_ms;

  current_time = clock();
  raw_pressed = button_get_gpio_state(ctx);

  /* Software debounce */
  if (raw_pressed == ctx->debounced_pressed)
    {
      ctx->debounce_count = 0;
    }
  else
    {
      ctx->debounce_count++;
      if (ctx->debounce_count >= DEBOUNCE_THRESHOLD)
        {
          ctx->debounced_pressed = raw_pressed;
          ctx->debounce_count = 0;
        }
    }

  current_pressed = ctx->debounced_pressed;

  /* Detect button press event */
  if (current_pressed && !ctx->last_state)
    {
      ctx->is_pressed = true;
      ctx->press_start_time = current_time;
      ctx->state = BUTTON_STATE_NOT_PRESSED;
    }
  /* Detect button release event */
  else if (!current_pressed && ctx->last_state)
    {
      if (ctx->is_pressed)
        {
          elapsed_ms = (current_time - ctx->press_start_time) * 1000 / CLOCKS_PER_SEC;

          if (elapsed_ms >= LONG_PRESS_DURATION_MS)
            {
              ctx->state = BUTTON_STATE_LONG_PRESS;
            }
          else if (elapsed_ms >= SHORT_PRESS_DURATION_MS)
            {
              ctx->state = BUTTON_STATE_SHORT_PRESS;
            }
          else
            {
              ctx->state = BUTTON_STATE_NOT_PRESSED;
            }
        }

      ctx->is_pressed = false;
    }
  /* Check for long press while button is still held */
  else if (current_pressed && ctx->is_pressed)
    {
      elapsed_ms = (current_time - ctx->press_start_time) * 1000 / CLOCKS_PER_SEC;

      if (elapsed_ms >= LONG_PRESS_DURATION_MS)
        {
          ctx->state = BUTTON_STATE_LONG_PRESS;
        }
    }

  ctx->last_state = current_pressed;
  return ctx->state;
}

/****************************************************************************
 * Name: handle_boot_long_press
 *
 * Description:
 *   BOOT长按处理：切换设置界面
 ****************************************************************************/

static void handle_boot_long_press(void)
{
  printf("[BTN] BOOT long press detected\n");

  if (settings_is_open())
    {
      settings_app_close();
      g_settings_active = false;
      printf("[BTN] Settings closed\n");
    }
  else
    {
      settings_app_open();
      g_settings_active = true;
      printf("[BTN] Settings opened\n");
    }
}

/****************************************************************************
 * Name: handle_pwr_short_press
 *
 * Description:
 *   PWR短按处理：返回主页（弹出页面栈）
 ****************************************************************************/

static void handle_pwr_short_press(void)
{
  printf("[BTN] PWR short press detected\n");

  /* 如果设置界面打开，先关闭设置 */
  if (g_settings_active && settings_is_open())
    {
      settings_app_close();
      g_settings_active = false;
      printf("[BTN] Settings closed by PWR press\n");
      return;
    }

  /* 其他情况：不做额外处理，页面栈由触摸手势处理 */
}

/****************************************************************************
 * Name: button_monitor_timer_cb
 *
 * Description:
 *   LVGL定时器回调：轮询按键状态并处理事件
 ****************************************************************************/

static void button_monitor_timer_cb(lv_timer_t *timer)
{
  enum button_state_e boot_state;
  enum button_state_e pwr_state;
  static enum button_state_e last_boot_state = BUTTON_STATE_NOT_PRESSED;
  static enum button_state_e last_pwr_state = BUTTON_STATE_NOT_PRESSED;

  (void)timer;

  /* Update button states */
  boot_state = button_update(&g_boot_button);
  pwr_state = button_update(&g_pwr_button);

  /* Handle BOOT button events */
  if (boot_state != last_boot_state)
    {
      if (boot_state == BUTTON_STATE_LONG_PRESS)
        {
          handle_boot_long_press();
        }
      last_boot_state = boot_state;
    }

  /* Handle PWR button events */
  if (pwr_state != last_pwr_state)
    {
      if (pwr_state == BUTTON_STATE_SHORT_PRESS)
        {
          handle_pwr_short_press();
        }
      else if (pwr_state == BUTTON_STATE_LONG_PRESS)
        {
          /* PWR长按也可以返回主页 */
          handle_pwr_short_press();
        }
      last_pwr_state = pwr_state;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void watch_button_monitor_init(void)
{
  printf("[BTN] Initializing button monitor...\n");

  /* Initialize buttons */
  button_ctx_init(&g_boot_button, BUTTON_TYPE_BOOT, BOOT_BUTTON_GPIO, true);
  button_ctx_init(&g_pwr_button, BUTTON_TYPE_PWR, PWR_BUTTON_GPIO, false);

  /* Create LVGL timer for polling (runs in main thread, safe for LVGL calls) */
  g_button_timer = lv_timer_create(button_monitor_timer_cb, POLL_INTERVAL_MS, NULL);
  if (g_button_timer == NULL)
    {
      printf("[BTN] ERROR: Failed to create button monitor timer\n");
      return;
    }

  printf("[BTN] Button monitor started (BOOT=GPIO%d, PWR=GPIO%d)\n",
         BOOT_BUTTON_GPIO, PWR_BUTTON_GPIO);
}

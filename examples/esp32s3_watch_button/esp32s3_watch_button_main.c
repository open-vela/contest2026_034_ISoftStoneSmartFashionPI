/****************************************************************************
 * apps/examples/esp32s3_watch_button/esp32s3_watch_button_main.c
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
#include <fcntl.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/audio/es7210.h>

#include "esp32s3_gpio.h"

/* Use non-blocking syslog instead of printf to avoid blocking
 * when serial console is not connected.
 */
#define btn_log(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Button GPIO definitions */
#define BOOT_BUTTON_GPIO    0   /* Boot button: pressed = LOW (0), released = HIGH (1) */
#define PWR_BUTTON_GPIO     10  /* PWR button: active HIGH (pressed=1, released=0)
                                   * GPIO10 通过 BSS138LT1G 接到 AXP2101 PWRON，
                                   * 电路上有上拉。根据实际现象，GPIO10 正常情况下为低，
                                   * 偶尔受干扰变高，所以配置为 active HIGH。 */

/* Long press duration in milliseconds (10 seconds) */
#define LONG_PRESS_DURATION_MS  10000

/* Short press duration in milliseconds (100ms) */
#define SHORT_PRESS_DURATION_MS 100

/* Polling interval in milliseconds */
#define POLL_INTERVAL_MS        10

/* Startup protection: ignore PWR long-press for first N seconds after boot */
#define STARTUP_PROTECT_MS      5000

/* Debounce threshold: require N consecutive consistent readings */
#define DEBOUNCE_THRESHOLD      5


/****************************************************************************
 * Enumeration Definitions
 ****************************************************************************/

/* Button press state enumeration */

enum button_state_e
{
  BUTTON_STATE_NOT_PRESSED = 0,  /* Button is not pressed */
  BUTTON_STATE_SHORT_PRESS = 1,  /* Button is short pressed */
  BUTTON_STATE_LONG_PRESS  = 2   /* Button is long pressed */
};

/* Button type enumeration */

enum button_type_e
{
  BUTTON_TYPE_BOOT = 0,  /* Boot button (GPIO0) */
  BUTTON_TYPE_PWR  = 1   /* PWR button (GPIO10) */
};

/****************************************************************************
 * Structure Definitions
 ****************************************************************************/

/* Button context structure */

struct button_context_s
{
  enum button_type_e type;           /* Button type */
  int gpio;                          /* GPIO pin number */
  bool active_low;                   /* True if button is active low */
  bool last_state;                   /* Last button state */
  bool is_pressed;                   /* Current pressed state */
  clock_t press_start_time;          /* Press start time */
  enum button_state_e state;         /* Current button state */
  bool debounced_pressed;            /* Debounced press state */
  uint8_t debounce_count;            /* Consecutive consistent readings */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

struct button_context_s g_boot_button;
struct button_context_s g_pwr_button;
static bool g_mic_muted = false;   //false 是禁麦， true是开麦
static bool g_boot_settings_active = false;
bool g_mic_muted_es7210_flag = true;    //false 是禁麦， true是开麦

/****************************************************************************
 * Private Functions
 ****************************************************************************/


/****************************************************************************
 * Name: button_get_gpio_state
 *
 * Description:
 *   Get the current GPIO state for a button.
 *
 * Input Parameters:
 *   ctx - Button context
 *
 * Returned Value:
 *   true if button is physically pressed, false otherwise
 *
 ****************************************************************************/

static bool button_get_gpio_state(FAR struct button_context_s *ctx)
{
  bool gpio_value;
  bool pressed;

  /* Read GPIO value */
  gpio_value = esp32s3_gpioread(ctx->gpio);

  /* For active low button (BOOT), pressed when GPIO is LOW */
  /* For active high button (PWR), pressed when GPIO is HIGH */
  if (ctx->active_low)
    {
      pressed = !gpio_value;  /* Active low: pressed when GPIO is 0 */
    }
  else
    {
      pressed = gpio_value;   /* Active high: pressed when GPIO is 1 */
    }

  /* Print GPIO value when state changes */
  if (pressed != ctx->last_state)
    {
      btn_log("[GPIO%d] Raw value: %d, Logic: %s",
             ctx->gpio,
             gpio_value ? 1 : 0,
             pressed ? "PRESSED" : "RELEASED");
    }

  return pressed;
}

/****************************************************************************
 * Name: button_init
 *
 * Description:
 *   Initialize a button context.
 *
 * Input Parameters:
 *   ctx   - Button context to initialize
 *   type  - Button type
 *   gpio  - GPIO pin number
 *   active_low - True if button is active low
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void button_init(FAR struct button_context_s *ctx,
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

  /* Configure GPIO as input with pull-up/down */

  if (active_low)
    {
      /* Active low button needs pull-up */
      esp32s3_configgpio(gpio, INPUT_PULLUP);
    }
  else
    {
      /* Active high button needs pull-down */
      esp32s3_configgpio(gpio, INPUT_PULLDOWN);
    }
}

/****************************************************************************
 * Name: button_update
 *
 * Description:
 *   Update button state based on current GPIO reading.
 *
 * Input Parameters:
 *   ctx - Button context
 *
 * Returned Value:
 *   Current button state
 *
 ****************************************************************************/

static enum button_state_e button_update(FAR struct button_context_s *ctx)
{
  bool current_pressed;
  bool raw_pressed;
  clock_t current_time;
  clock_t elapsed_ms;

  /* Get current time */
  current_time = clock();

  /* Get current raw button state */
  raw_pressed = button_get_gpio_state(ctx);

  /* Software debounce: require N consecutive consistent readings */
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

  /* Use debounced state for all logic */
  current_pressed = ctx->debounced_pressed;

  /* Detect button press event (rising edge) */
  if (current_pressed && !ctx->last_state)
    {
      /* Button just pressed */
      ctx->is_pressed = true;
      ctx->press_start_time = current_time;
      ctx->state = BUTTON_STATE_NOT_PRESSED;
    }
  /* Detect button release event (falling edge) */
  else if (!current_pressed && ctx->last_state)
    {
      /* Button just released */
      if (ctx->is_pressed)
        {
          /* Calculate press duration */
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
      /* Calculate current press duration */
      elapsed_ms = (current_time - ctx->press_start_time) * 1000 / CLOCKS_PER_SEC;

      if (elapsed_ms >= LONG_PRESS_DURATION_MS)
        {
          ctx->state = BUTTON_STATE_LONG_PRESS;
        }
    }

  /* Update last state */
  ctx->last_state = current_pressed;

  return ctx->state;
}

/****************************************************************************
 * Name: button_get_name
 *
 * Description:
 *   Get button name string.
 *
 * Input Parameters:
 *   type - Button type
 *
 * Returned Value:
 *   Button name string
 *
 ****************************************************************************/

static FAR const char *button_get_name(enum button_type_e type)
{
  switch (type)
    {
      case BUTTON_TYPE_BOOT:
        return "BOOT";
      case BUTTON_TYPE_PWR:
        return "PWR";
      default:
        return "UNKNOWN";
    }
}

/****************************************************************************
 * Name: handle_button_event
 *
 * Description:
 *   Handle button event based on state.
 *
 * Input Parameters:
 *   ctx - Button context
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void handle_button_event(FAR struct button_context_s *ctx)
{
  clock_t elapsed_ms;

  switch (ctx->state)
    {
      case BUTTON_STATE_SHORT_PRESS:
        elapsed_ms = (clock() - ctx->press_start_time) * 1000 / CLOCKS_PER_SEC;
        // btn_log("[EVENT] %s BUTTON: SHORT PRESS, GPIO: %d, Duration: %ld ms",
        //         button_get_name(ctx->type), ctx->gpio, (long)elapsed_ms);
        
        // printf("\n");
        // printf("========================================\n");
        // printf("  [EVENT] %s BUTTON: SHORT PRESS\n", button_get_name(ctx->type));
        // printf("  GPIO: %d\n", ctx->gpio);
        // printf("  Duration: %ld ms\n", (long)elapsed_ms);
        // printf("  Action: Short press action triggered\n");
        // printf("========================================\n");
        // printf("\n");

        if (ctx->type == BUTTON_TYPE_BOOT)
        {
            g_mic_muted = !g_mic_muted;
            es7210_set_mic_mute(g_mic_muted);  // 入参：true 禁麦， false 开麦
            if(g_mic_muted)
            {
              g_mic_muted_es7210_flag=false;  //禁麦flag
            }
            else
            {
              g_mic_muted_es7210_flag=true;  //开麦flag
            }
            btn_log("MIC: %s\n", g_mic_muted ? "MUTED" : "UNMUTED");
        }
        else if(ctx->type == BUTTON_TYPE_PWR)
        {

        }
        break;

      case BUTTON_STATE_LONG_PRESS:
        elapsed_ms = (clock() - ctx->press_start_time) * 1000 / CLOCKS_PER_SEC;
        // btn_log("[EVENT] %s BUTTON: LONG PRESS, GPIO: %d, Duration: %ld ms",
        //         button_get_name(ctx->type), ctx->gpio, (long)elapsed_ms);
        
        // printf("\n");
        // printf("========================================\n");
        // printf("  [EVENT] %s BUTTON: LONG PRESS\n", button_get_name(ctx->type));
        // printf("  GPIO: %d\n", ctx->gpio);
        // printf("  Duration: %ld ms\n", (long)elapsed_ms);
        // printf("  Action: Long press action triggered\n");
        // printf("========================================\n");
        // printf("\n");

        if (ctx->type == BUTTON_TYPE_BOOT)
        {
          g_boot_settings_active = !g_boot_settings_active;
          if (g_boot_settings_active)
            {
              //printf("execute settings\n");
            }
          else
            {
              //printf("destroy settings\n");
            }
        }
        else if (ctx->type == BUTTON_TYPE_PWR)
        {
        }
        break;

      case BUTTON_STATE_NOT_PRESSED:
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 *
 * Description:
 *   Main entry point for ESP32S3 watch button example.
 *
 * Input Parameters:
 *   argc - Argument count
 *   argv - Argument values
 *
 * Returned Value:
 *   0 on success, error code otherwise
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  enum button_state_e boot_state;
  enum button_state_e pwr_state;
  enum button_state_e last_boot_state = BUTTON_STATE_NOT_PRESSED;
  enum button_state_e last_pwr_state = BUTTON_STATE_NOT_PRESSED;

  btn_log("ESP32S3 Watch Button Example started");
  btn_log("BOOT button: GPIO%d (Active LOW), PWR button: GPIO%d (Active HIGH)",
          BOOT_BUTTON_GPIO, PWR_BUTTON_GPIO);

  /* Initialize buttons */
  button_init(&g_boot_button, BUTTON_TYPE_BOOT, BOOT_BUTTON_GPIO, true);
  button_init(&g_pwr_button, BUTTON_TYPE_PWR, PWR_BUTTON_GPIO, false);

  btn_log("Buttons initialized, starting monitoring");

  /* Main loop - monitor buttons */
  while (1)
    {
      bool boot_gpio_value;
      bool pwr_gpio_value;

      /* Update button states */
      boot_state = button_update(&g_boot_button);
      pwr_state = button_update(&g_pwr_button);

      /* Read current GPIO values for display */
      boot_gpio_value = esp32s3_gpioread(BOOT_BUTTON_GPIO);
      pwr_gpio_value = esp32s3_gpioread(PWR_BUTTON_GPIO);

      /* Print real-time button status when pressed (use syslog to avoid blocking) */
      if (g_boot_button.is_pressed)
        {
          clock_t elapsed = (clock() - g_boot_button.press_start_time) * 1000 / CLOCKS_PER_SEC;
          btn_log("[BOOT] GPIO%d=%d, PRESSED (%ld ms)", BOOT_BUTTON_GPIO, boot_gpio_value ? 1 : 0, (long)elapsed);
        }
      else if (g_pwr_button.is_pressed)
        {
          clock_t elapsed = (clock() - g_pwr_button.press_start_time) * 1000 / CLOCKS_PER_SEC;
          btn_log("[PWR]  GPIO%d=%d, PRESSED (%ld ms)", PWR_BUTTON_GPIO, pwr_gpio_value ? 1 : 0, (long)elapsed);
        }

      /* Handle BOOT button events */
      if (boot_state != last_boot_state)
        {
          if (boot_state != BUTTON_STATE_NOT_PRESSED)
            {
              handle_button_event(&g_boot_button);
            }
          last_boot_state = boot_state;
        }

      /* Handle PWR button events */
      if (pwr_state != last_pwr_state)
        {
          if (pwr_state != BUTTON_STATE_NOT_PRESSED)
            {
              handle_button_event(&g_pwr_button);
            }
          last_pwr_state = pwr_state;
        }

      /* Small delay to prevent CPU hogging */
      usleep(POLL_INTERVAL_MS * 1000);
    }

  return 0;
}

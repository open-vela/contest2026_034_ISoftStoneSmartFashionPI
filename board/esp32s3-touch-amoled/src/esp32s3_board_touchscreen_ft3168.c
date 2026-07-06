/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-box/src/esp32s3_board_touchsceen_ft3168.c
 *
 * SPDX-License-Identifier: Apache-2.0
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

#include <syslog.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <string.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>
#include <nuttx/signal.h>
#include <nuttx/input/touchscreen.h>

#include "esp32s3_i2c.h"
#include "esp32s3_gpio.h"
#include "hardware/esp32s3_gpio_sigmap.h"

#include "esp32s3-touch-amoled.h"

/* Use syslog instead of printf to avoid blocking when serial is disconnected */
#define ft3168_log(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* FT3168 I2C Address (7-bit). esp32s3-box.h defines 0x24/0x5d for the
 * original touch IC; FT3168 uses 0x38.
 */

#define FT3168_ADDR             0x38

/* FT3168 Registers (compatible with FT5x06) */

#define FT3168_DEVICE_MODE      0x00
#define FT3168_GESTURE_ID       0x01
#define FT3168_TD_STATUS        0x02    /* Touch point count */
#define FT3168_TOUCH1_XH        0x03    /* Touch 1 X high byte + event flag */
#define FT3168_TOUCH1_XL        0x04    /* Touch 1 X low byte */
#define FT3168_TOUCH1_YH        0x05    /* Touch 1 Y high byte + touch ID */
#define FT3168_TOUCH1_YL        0x06    /* Touch 1 Y low byte */

/* I2C Configuration */

#define FT3168_I2C_RETRY_COUNT  3       /* I2C read retry count */
#define FT3168_I2C_RETRY_DELAY  1000    /* I2C retry delay in us */

#define FT3168_ID_G_THGROUP             0x80
#define FT3168_ID_G_THPEAK              0x81
#define FT3168_ID_G_THCAL               0x82
#define FT3168_ID_G_THWATER             0x83
#define FT3168_ID_G_THTEMP              0x84
#define FT3168_ID_G_THDIFF              0x85
#define FT3168_ID_G_CTRL                0x86
#define FT3168_ID_G_TIME_ENTER_MONITOR  0x87
#define FT3168_ID_G_PERIODACTIVE        0x88
#define FT3168_ID_G_PERIODMONITOR       0x89
#define FT3168_ID_G_AUTO_CLB_MODE       0xA0
#define FT3168_ID_G_LIB_VERSION_H       0xA1
#define FT3168_ID_G_LIB_VERSION_L       0xA2
#define FT3168_ID_G_CIPHER              0xA3
#define FT3168_ID_G_MODE                0xA4
#define FT3168_ID_G_PMODE               0xA5
#define FT3168_ID_G_FIRMID              0xA6
#define FT3168_ID_G_STATE               0xA7
#define FT3168_ID_G_FT5201ID            0xA8
#define FT3168_ID_G_ERR                 0xA9

/* Touch event types (bits [7:6] of XH) */

#define FT3168_EVENT_PRESS      0x00
#define FT3168_EVENT_RELEASE    0x01
#define FT3168_EVENT_CONTACT    0x02

/* Power modes */

#define FT3168_POWER_ACTIVE     0x00
#define FT3168_POWER_MONITOR    0x01
#define FT3168_POWER_STANDBY    0x02
#define FT3168_POWER_HIBERNATE  0x03

/* Board config */

#define FT3168_CLOCK            TOUCHSCEEN_CLOCK
// #define FT3168_WORK_DELAY       CONFIG_ESP32S3_BOARD_TOUCHSCREEN_SAMPLE_DELAYS
#define FT3168_WORK_DELAY       10  /* Reduced default delay for better responsiveness */
#define FT3168_SAMPLE_CACHES    CONFIG_ESP32S3_BOARD_TOUCHSCREEN_SAMPLE_CACHES
#define FT3168_PATH             CONFIG_ESP32S3_BOARD_TOUCHSCREEN_PATH

#ifdef CONFIG_ESP32S3_BOARD_TOUCHSCREEN_WIDTH
#  define FT3168_WIDTH          CONFIG_ESP32S3_BOARD_TOUCHSCREEN_WIDTH
#else
#  define FT3168_WIDTH          410
#endif

#ifdef CONFIG_ESP32S3_BOARD_TOUCHSCREEN_HEIGHT
#  define FT3168_HEIGHT         CONFIG_ESP32S3_BOARD_TOUCHSCREEN_HEIGHT
#else
#  define FT3168_HEIGHT         502
#endif

/* Reset GPIO (if available on the board). AMOLED boards usually have one. */
#ifdef CONFIG_ESP32S3_BOARD_TOUCHSCREEN_RST
#  define FT3168_RST            CONFIG_ESP32S3_BOARD_TOUCHSCREEN_RST
#else
/* GPIO 8 is often used as RST on some ESP32-S3 touch boards; adjust as needed. */
#  define FT3168_RST            9
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ft3168_dev_s
{
  struct touch_lowerhalf_s  touch_lower;
  bool                      has_report;
  struct i2c_master_s      *i2c;
  struct work_s             work;
  spinlock_t                lock;
  uint16_t                  last_x;  /* Last touch X coordinate */
  uint16_t                  last_y;  /* Last touch Y coordinate */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ft3168_dev_s g_ft3168_dev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ft3168_read_reg(uint8_t reg, uint8_t *buf, int buflen)
{
  int ret;
  int retry = 0;
  struct i2c_msg_s msgv[2];

  msgv[0].frequency = FT3168_CLOCK;
  msgv[0].addr      = FT3168_ADDR;
  msgv[0].flags     = 0;
  msgv[0].buffer    = &reg;
  msgv[0].length    = 1;

  msgv[1].frequency = FT3168_CLOCK;
  msgv[1].addr      = FT3168_ADDR;
  msgv[1].flags     = I2C_M_READ;
  msgv[1].buffer    = buf;
  msgv[1].length    = buflen;

#if 0
  ret = I2C_TRANSFER(g_ft3168_dev.i2c, msgv, 2);
  if (ret < 0)
    {
      ft3168_log("[FT3168] I2C read reg=0x%02X len=%d failed: %d\n", reg, buflen, ret);
      return ret;
    }

  return OK;
#else 
do
    {
      ret = I2C_TRANSFER(g_ft3168_dev.i2c, msgv, 2);
      if (ret >= 0)
        {
          return OK;
        }
      nxsig_usleep(FT3168_I2C_RETRY_DELAY);
      retry++;
    } while (retry < FT3168_I2C_RETRY_COUNT);

//   ft3168_log("[FT3168] I2C read reg=0x%02X len=%d failed after %d retries: %d\n", 
//          reg, buflen, FT3168_I2C_RETRY_COUNT, ret);
  return ret;
#endif
}

static int ft3168_write_reg(uint8_t reg, uint8_t val)
{
  int ret;
  uint8_t buf[2] = { reg, val };
  struct i2c_msg_s msgv[1];

  msgv[0].frequency = FT3168_CLOCK;
  msgv[0].addr      = FT3168_ADDR;
  msgv[0].flags     = 0;
  msgv[0].buffer    = buf;
  msgv[0].length    = 2;

  ret = I2C_TRANSFER(g_ft3168_dev.i2c, msgv, 1);
  if (ret < 0)
    {
      ft3168_log("[FT3168] I2C write reg=0x%02X val=0x%02X failed: %d\n", reg, val, ret);
      return ret;
    }

  return OK;
}

static void ft3168_hw_reset(void)
{
  ft3168_log("[FT3168] HW reset start, RST gpio=%d\n", FT3168_RST);
  esp32s3_configgpio(FT3168_RST, OUTPUT);
  esp32s3_gpiowrite(FT3168_RST, false);
  nxsig_usleep(10000);  /* 10ms low */
  esp32s3_gpiowrite(FT3168_RST, true);
  nxsig_usleep(50000);  /* 50ms high, wait stable */
  ft3168_log("[FT3168] HW reset done\n");
}

/* Reference: espressif__esp_lcd_touch_ft5x06 init sequence */
static int ft3168_init_controller(void)
{
  int ret = OK;

  /* Sensitivity-tuned configuration for watch touch-to-wake:
   * - Lower THGROUP/THPEAK/THDIFF for lighter touch detection
   * - Faster PERIODACTIVE for quicker response
   */
  ret |= ft3168_write_reg(FT3168_ID_G_THGROUP,            40);
  ret |= ft3168_write_reg(FT3168_ID_G_THPEAK,             40);
  ret |= ft3168_write_reg(FT3168_ID_G_THCAL,              16);
  ret |= ft3168_write_reg(FT3168_ID_G_THWATER,            40);
  ret |= ft3168_write_reg(FT3168_ID_G_THTEMP,             10);
  ret |= ft3168_write_reg(FT3168_ID_G_THDIFF,             10);
  ret |= ft3168_write_reg(FT3168_ID_G_TIME_ENTER_MONITOR,  2);
  ret |= ft3168_write_reg(FT3168_ID_G_PERIODACTIVE,        8);
  ret |= ft3168_write_reg(FT3168_ID_G_PERIODMONITOR,      40);
  ret |= ft3168_write_reg(FT3168_ID_G_PMODE,    FT3168_POWER_ACTIVE);

  ft3168_log("[FT3168] controller init sequence done (ret=%d)\n", ret);
  return ret;
}

static void ft3168_worker(void *arg)
{
  int ret;
  struct ft3168_dev_s *dev = (struct ft3168_dev_s *)arg;
  uint8_t buf[30];
  uint8_t touch_count;
  uint16_t x, y;
  uint8_t event;
  struct touch_sample_s sample;
  struct touch_point_s *point = sample.point;
  clock_t delay = FT3168_WORK_DELAY;

  /* Read touch point count (compatible with FT5x06) */
  ret = ft3168_read_reg(FT3168_TD_STATUS, buf, 1);
  if (ret < 0)
    {
        /* If I2C read fails, use faster delay to retry quickly */
        delay = 5;
        goto exit;
    }

  touch_count = buf[0] & 0x0F;

  if (touch_count > 5)
    {
      /* Invalid count, treat as no touch */
      touch_count = 0;
    }

  if (touch_count > 0)
    {
      int readlen = 6 * touch_count;
      if (readlen > (int)sizeof(buf))
        {
          readlen = (int)sizeof(buf);
        }

      ret = ft3168_read_reg(FT3168_TOUCH1_XH, buf, readlen);
      if (ret < 0)
        {
            /* If I2C read fails, use faster delay to retry quickly */
          delay = 5;
          goto exit;
        }

      /* Parse first touch point */
      event = (buf[0] >> 6) & 0x03;
      x = ((buf[0] & 0x0F) << 8) | buf[1];
      y = ((buf[2] & 0x0F) << 8) | buf[3];

      /* Coordinate mirroring per board Kconfig */
#ifdef CONFIG_ESP32S3_BOARD_TOUCHSCREEN_X_MIRROR
      if (x < FT3168_WIDTH)
        {
          x = FT3168_WIDTH - 1 - x;
        }
#endif
#ifdef CONFIG_ESP32S3_BOARD_TOUCHSCREEN_Y_MIRROR
      if (y < FT3168_HEIGHT)
        {
          y = FT3168_HEIGHT - 1 - y;
        }
#endif

      /* Save last touch coordinates */
      dev->last_x = x;
      dev->last_y = y;

      memset(&sample, 0, sizeof(sample));
      sample.npoints = 1;

      point->x = x;
      point->y = y;
      point->pressure = 100;  /* FT3168 doesn't report pressure */
      point->flags = TOUCH_POS_VALID | TOUCH_PRESSURE_VALID;

      if (event == FT3168_EVENT_PRESS || event == FT3168_EVENT_CONTACT)
        {
          point->flags |= TOUCH_DOWN;
          dev->has_report = true;
        }
      else if (event == FT3168_EVENT_RELEASE)
        {
          point->flags |= TOUCH_UP;
          dev->has_report = false;
        }

      ft3168_log("[FT3168] touch evt=%u x=%u y=%u cnt=%u flags=0x%02X\n",
             event, x, y, touch_count, point->flags);

      touch_event(dev->touch_lower.priv, &sample);
      delay = 10;  /* Faster polling when touched */
    }
  else if (dev->has_report)
    {
      /* Send touch up event when finger lifted */
      memset(&sample, 0, sizeof(sample));
      sample.npoints = 1;
      point->x = dev->last_x;  /* Use last touch coordinates */
      point->y = dev->last_y;
      point->flags = TOUCH_POS_VALID | TOUCH_UP;
      ft3168_log("[FT3168] touch UP (release) at x=%u y=%u\n", dev->last_x, dev->last_y);
      touch_event(dev->touch_lower.priv, &sample);
      dev->has_report = false;
      delay = 5;  /* Faster polling after release to catch quick taps */
    }
  else
    {
      /* No touch, use faster delay to catch quick taps for touch-to-wake */
      delay = 5;
    }

exit:
  ret = work_queue(LPWORK, &dev->work, ft3168_worker, dev, delay);
  if (ret != 0)
    {
      ierr("ERROR: work_queue() failed: %d\n", ret);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_touchscreen_initialize(void)
{
  int ret;
  struct ft3168_dev_s *dev = &g_ft3168_dev;
  uint8_t chip_id;
  uint8_t firmid;
  uint8_t libver_h;
  uint8_t libver_l;

  ft3168_log("[FT3168] board_touchscreen_initialize start\n");

    /* Initialize state */
  dev->has_report = false;

  /* Hardware reset (ESP-IDF sequence: low 10ms -> high 10ms+50ms) */
  ft3168_hw_reset();

  /* Initialize I2C */
  dev->i2c = esp32s3_i2cbus_initialize(TOUCHSCEEN_I2C);
  if (!dev->i2c)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize I2C port %d\n", TOUCHSCEEN_I2C);
      ft3168_log("[FT3168] I2C init failed\n");
      return -ENODEV;
    }

  ft3168_log("[FT3168] I2C init ok, addr=0x%02X, clock=%d\n",
         FT3168_ADDR, FT3168_CLOCK);

  /* Read some registers for debugging */
  ret = ft3168_read_reg(FT3168_ID_G_FT5201ID, &chip_id, 1);
  if (ret < 0)
    {
      ft3168_log("[FT3168] read chip ID failed: %d\n", ret);
    }
  else
    {
      ft3168_log("[FT3168] Chip ID (reg 0xA8): 0x%02X\n", chip_id);
    }

  ft3168_read_reg(FT3168_ID_G_FIRMID, &firmid, 1);
  ft3168_read_reg(FT3168_ID_G_LIB_VERSION_H, &libver_h, 1);
  ft3168_read_reg(FT3168_ID_G_LIB_VERSION_L, &libver_l, 1);
  ft3168_log("[FT3168] FirmID=0x%02X LibVer=0x%02X%02X\n", firmid, libver_h, libver_l);

  /* Controller init sequence (same as ESP-IDF ft5x06) */
  ret = ft3168_init_controller();
  if (ret < 0)
    {
      ft3168_log("[FT3168] controller init failed: %d\n", ret);
      return ret;
    }

  nxsig_usleep(10000);
  /* Set maxpoint before register (required by LVGL) */
  dev->touch_lower.maxpoint = 1;  /* FT3168 supports single touch */

  /* Register touch device */
  ret = touch_register(&dev->touch_lower, FT3168_PATH, FT3168_SAMPLE_CACHES);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: touch_register() failed: %d\n", ret);
      ft3168_log("[FT3168] touch_register failed: %d\n", ret);
      return ret;
    }

  ft3168_log("[FT3168] touch_register ok, path=%s\n", FT3168_PATH);

  /* Start worker thread */
  ret = work_queue(LPWORK, &dev->work, ft3168_worker, dev, FT3168_WORK_DELAY);
  if (ret != 0)
    {
      syslog(LOG_ERR, "ERROR: work_queue() failed: %d\n", ret);
      ft3168_log("[FT3168] work_queue failed: %d\n", ret);
      return ret;
    }

  ft3168_log("[FT3168] worker started, delay=%d ticks\n", (int)FT3168_WORK_DELAY);
  return 0;
}

void ft3168_poll_pause(void)
{
  work_cancel(LPWORK, &g_ft3168_dev.work);
}

void ft3168_poll_resume(void)
{
  int ret = work_queue(LPWORK, &g_ft3168_dev.work, ft3168_worker, &g_ft3168_dev, FT3168_WORK_DELAY);
  if (ret != 0)
    {
      ierr("ERROR: work_queue on resume failed: %d\n", ret);
    }
}
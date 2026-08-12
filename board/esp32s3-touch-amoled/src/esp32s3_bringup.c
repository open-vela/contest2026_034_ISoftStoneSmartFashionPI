/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-touch-amoled/src/esp32s3_bringup.c
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

#include <stdio.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <syslog.h>
#include <debug.h>
#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <time.h>
#include <nuttx/fs/fs.h>

#ifdef CONFIG_ESP32S3_TIMER
#  include "esp32s3_board_tim.h"
#endif

#ifdef CONFIG_ESP32S3_WIFI
#  include "esp32s3_board_wlan.h"
#endif

#ifdef CONFIG_ESP32S3_BLE
#  include "esp32s3_ble.h"
#endif

#ifdef CONFIG_ESP32S3_WIFI_BT_COEXIST
#  include "esp32s3_wifi_adapter.h"
#endif

#ifdef CONFIG_ESP32S3_RT_TIMER
#  include "esp32s3_rt_timer.h"
#endif

#ifdef CONFIG_ESP32S3_I2C
#  include "esp32s3_i2c.h"
#endif

#ifdef CONFIG_ESP32S3_I2S
#  include "esp32s3_i2s.h"
#endif

#ifdef CONFIG_WATCHDOG
#  include "esp32s3_board_wdt.h"
#endif

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_RTC_PCF85063
#  include <nuttx/timers/pcf85063.h>
#endif

#ifdef CONFIG_ESP32S3_SPI
#  include "esp32s3_spi.h"
#endif

#ifdef CONFIG_LCD_DEV
#  include <nuttx/board.h>
#  include <nuttx/lcd/lcd_dev.h>
#endif

#ifdef CONFIG_ESP32S3_SDMMC
#include "esp32s3_board_sdmmc.h"
#endif

#ifdef CONFIG_SYSLOG_FILE
#include <nuttx/syslog/syslog.h>

/****************************************************************************
 * Name: syslog_flush_task
 *
 * Description:
 *   Periodic task that calls syslog_flush() to sync the syslog file
 *   channel's sector cache to disk.  Without this, VFAT's per-handle
 *   sector cache keeps written data in RAM that is invisible to other
 *   file handles (e.g. "cat /mnt/sd/syslog/app.log").
 ****************************************************************************/

static int syslog_flush_task(int argc, FAR char *argv[])
{
  while (1)
    {
      sleep(5);  /* Flush every 5 seconds */
      syslog_flush();
    }

  return OK;
}
#endif /* CONFIG_SYSLOG_FILE */

#ifdef CONFIG_ESP32S3_WATCH_SENSOR_QMI8658
#  include <nuttx/sensors/qmi8658.h>
#endif

#include "esp32s3_gpio.h"

#include "esp32s3-touch-amoled.h"

/* 调试打印开关：menuconfig 打开 CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG 后生效。
 * 默认关闭：无 USB 主机时控制台 FIFO 写满会阻塞系统。
 */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#  define WATCH_DBG_LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#else
#  define WATCH_DBG_LOG(fmt, ...)
#endif

#ifdef CONFIG_ESP32S3_FLASH_MODE_OCT
void esp32s3_bsp_opiflash_set_required_regs(void)
{
}
#endif

#ifdef CONFIG_ESP32S3_SDMMC
/****************************************************************************
 * Name: sdmmc_mount_task
 *
 * Description:
 *   Background task that initializes the SDMMC slot and mounts the SD card
 *   with retries.  Card probing plus the mount retry loop can take several
 *   seconds (worst case when no card is inserted).  Running this
 *   synchronously from esp32s3_bringup() delayed NSH and the watch UI,
 *   leaving the screen black for that whole time.
 *
 ****************************************************************************/

static int sdmmc_mount_task(int argc, FAR char *argv[])
{
  int ret;
  int mount_retries;
  struct stat buf;

  /* Create mount point for SD card */

  ret = mkdir("/mnt", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: Failed to create /mnt: %d\n", ret);
    }

  ret = mkdir("/mnt/sd", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: Failed to create /mnt/sd: %d\n", ret);
    }

  /* SD initialize.
   * The TF card slot is wired for SPI (MOSI=GPIO1, SCK=GPIO2, MISO=GPIO3,
   * SDCS=GPIO17) but we use native SDMMC 1-bit mode.  The card samples its
   * CS/DAT3 line at power-on to decide between SPI and SD mode; drive GPIO17
   * high so the card enters SD mode and stays there after a soft reboot.
   */

  esp32s3_configgpio(17, OUTPUT);
  esp32s3_gpiowrite(17, true);
  usleep(10000);

  syslog(LOG_INFO, "Initializing SDMMC...\n");
  ret = board_sdmmc_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SDMMC: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "SDMMC initialized successfully\n");
    }

  /* Wait for SD card to be ready and try to mount with retries.
   * Some cards take longer to become ready after power-on, so we first
   * wait for the block device to appear and then retry the mount.
   */

  syslog(LOG_INFO, "Waiting for SD card...\n");

  for (mount_retries = 0; mount_retries < 20; mount_retries++)
    {
      if (stat("/dev/mmcsd1", &buf) == 0)
        {
          ret = nx_mount("/dev/mmcsd1", "/mnt/sd", "vfat", 0, NULL);
          if (ret == OK)
            {
              syslog(LOG_INFO, "SD card mounted at /mnt/sd\n");
              break;
            }

          syslog(LOG_ERR, "ERROR: Failed to mount SD card (attempt %d): %d\n",
                 mount_retries + 1, ret);
        }
      else
        {
          syslog(LOG_INFO, "SD card block device not ready (attempt %d)\n",
                 mount_retries + 1);
        }

      usleep(200000);  /* Wait 200ms before retry */
    }

#ifdef CONFIG_SYSLOG_FILE
  /* After SD card mount, redirect syslog to a file on the SD card.
   * This keeps logs persistent across reboots.  syslog_file_channel()
   * internally handles log rotation, auto-reconnect on card removal,
   * and crash-safe flush via the interrupt buffer.
   */

  ret = mkdir("/mnt/sd/syslog", 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_ERR, "ERROR: Failed to create syslog dir: %d\n", ret);
    }

  /* Diagnostic: verify VFAT writes work by writing a test file directly.
   * This helps distinguish "syslog channel doesn't write" from
   * "SD card is read-only / VFAT write is broken".
   */

    {
      int test_fd;
      test_fd = open("/mnt/sd/syslog/.writetest", O_WRONLY | O_CREAT | O_TRUNC,
                     0644);
      if (test_fd >= 0)
        {
          ssize_t n = write(test_fd, "ok\n", 3);
          close(test_fd);
          syslog(LOG_INFO, "SD write test: fd=%d written=%d\n",
                 test_fd, (int)n);
        }
      else
        {
          syslog(LOG_ERR, "ERROR: SD write test open failed: %d\n", test_fd);
        }
    }

  FAR syslog_channel_t *file_ch;
  file_ch = syslog_file_channel("/mnt/sd/syslog/app.log");
  if (file_ch == NULL)
    {
      syslog(LOG_ERR, "ERROR: syslog_file_channel() failed\n");
    }
  else
    {
      /* Start a periodic flush task to sync the VFAT sector cache
       * to disk.  Without this, writes stay in the file's private
       * cache and are invisible to other file handles (cat, tail).
       */

      ret = task_create("syslog_flush", 50, 2048,
                        syslog_flush_task, NULL);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to start flush task: %d\n", ret);
        }

      syslog(LOG_INFO, "Syslog file channel: /mnt/sd/syslog/app.log\n");
      syslog_flush();
    }
#endif /* CONFIG_SYSLOG_FILE */

  return OK;
}
#endif /* CONFIG_ESP32S3_SDMMC */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_bringup
 *
 * Description:
 *   Perform architecture-specific initialization
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y :
 *     Called from board_late_initialize().
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=n && CONFIG_BOARDCTL=y :
 *     Called from the NSH library
 *
 ****************************************************************************/

int esp32s3_bringup(void)
{
  int ret;

#ifdef CONFIG_FS_PROCFS
  /* Mount the procfs file system */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  /* Mount the tmpfs file system */

  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount tmpfs at %s: %d\n",
             CONFIG_LIBC_TMPDIR, ret);
    }
#endif

#ifdef CONFIG_ESP32S3_TIMER
  /* Configure general purpose timers */

  ret = board_tim_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize timers: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP32S3_RT_TIMER
  ret = esp32s3_rt_timer_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize RT timer: %d\n", ret);
    }
#endif

#ifdef CONFIG_WATCHDOG
  /* Configure watchdog timer */

  ret = board_wdt_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize watchdog timer: %d\n", ret);
    }
#endif

#ifdef CONFIG_I2C_DRIVER
  /* Configure I2C peripheral interfaces */

  ret = board_i2c_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize I2C driver: %d\n", ret);
    }
#endif /*CONFIG_I2C_DRIVER*/

#ifdef CONFIG_RTC_PCF85063
  struct i2c_master_s *i2c;
  
  i2c = esp32s3_i2cbus_initialize(ESP32S3_I2C0);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to get I2C bus\n");
    }
  else
    {
      ret = pcf85063_rtc_initialize(i2c);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to initialize PCF85063 RTC: %d\n", ret);
        }
      else
        {
          clock_synchronize(NULL);
          setenv("TZ", "CST-8", 1);
          tzset();
        }
    }
#endif /*CONFIG_RTC_PCF85063*/

#ifdef CONFIG_INPUT_BUTTONS
  /* Register the BUTTON driver */
  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize button driver: %d\n", ret);
    }
#endif /*CONFIG_INPUT_BUTTONS*/


#ifdef CONFIG_AXP2101
  ret = board_axp2101_initialize();    // done
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_axp2101_initialize failed: %d\n", ret);
    }
#endif /*CONFIG_AXP2101*/


#ifdef CONFIG_ESP32S3_SPIFLASH
  ret = board_spiflash_init();  // done  soc内置1M spi flash
  if (ret)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SPI Flash\n");
    }
#endif

#ifdef CONFIG_ESP32S3_WIRELESS
#ifdef CONFIG_ESP32S3_WIFI_BT_COEXIST
  ret = esp32s3_wifi_bt_coexist_init();
  if (ret)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize Wi-Fi and BT coexist\n");
    }
#endif /*CONFIG_ESP32S3_WIFI_BT_COEXIST*/
#ifdef CONFIG_ESP32S3_BLE
  ret = esp32s3_ble_initialize();    // done
  if (ret)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize BLE\n");
    }
#endif /*CONFIG_ESP32S3_BLE*/

#ifdef CONFIG_ESP32S3_WIFI
  ret = board_wlan_init();     // done
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize wireless subsystem=%d\n",
             ret);
    }
#endif /*CONFIG_ESP32S3_WIFI*/
#endif /*CONFIG_ESP32S3_WIRELESS*/

#if defined(CONFIG_DEV_GPIO) && !defined(CONFIG_GPIO_LOWER_HALF)
  ret = esp32s3_gpio_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize GPIO Driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP32S3_WATCH_SENSOR_QMI8658
  ret = esp32s3_qmi8658_initialize();    // done
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to register QMI8658 IMU: %d\n", ret);
    }
#endif /*CONFIG_ESP32S3_WATCH_SENSOR_QMI8658*/

#ifdef CONFIG_ESP32S3_WATCH_AMOLED_CO5300
  ret = board_amoled_initialize();   // done
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize LCD.\n");
      return ret;
    }
#ifdef CONFIG_LCD_DEV
  ret = lcddev_register(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: lcddev_register() failed: %d\n", ret);
    }
#endif /*CONFIG_LCD_DEV*/
#endif /*CONFIG_ESP32S3_WATCH_AMOLED_CO5300*/

#ifdef CONFIG_ESP32S3_WATCH_TOUCHSCREEN_FT3168
  ret = board_touchscreen_initialize();    // done
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize touchscreen driver\n");
    }
#endif /*CONFIG_ESP32S3_WATCH_TOUCHSCREEN_FT3168*/

#ifdef CONFIG_ESP32S3_I2S
#  ifdef CONFIG_ESP32S3_I2S1
  struct i2c_master_s *i2c_audio;
  struct i2s_dev_s *i2s_audio;
  i2s_audio = esp32s3_i2sbus_initialize(ESP32S3_I2S1);
  if (i2s_audio == NULL)
    {
      WATCH_DBG_LOG("Failed to initialize I2S%d", ESP32S3_I2S1);
    }
   
  i2c_audio = esp32s3_i2cbus_initialize(ESP32S3_I2C0);
  if (i2c_audio == NULL)
    {
      WATCH_DBG_LOG("Failed to initialize I2C%d", ESP32S3_I2C0);
    }

#    ifdef CONFIG_AUDIO_ES8311
  /* Configure ES8311 audio on I2C0 and I2S1 */
  esp32s3_configgpio(SPEAKER_ENABLE_GPIO, OUTPUT);
  esp32s3_gpiowrite(SPEAKER_ENABLE_GPIO, true);

  //WATCH_DBG_LOG(">>> ES8311: I2C0 addr=0x%02x freq=%d I2S1 <<<", ES8311_I2C_ADDR, ES8311_I2C_FREQ);

  ret = esp32s3_es8311_initialize(i2c_audio, i2s_audio);
  if (ret != OK)
    {
      //WATCH_DBG_LOG(">>> ES8311: INIT FAILED ret=%d <<<", ret);
      syslog(LOG_ERR, "ERROR: esp32s3_es8311_initialize failed: %d\n", ret);
    }
  else
    {
      //WATCH_DBG_LOG(">>> ES8311: INIT OK <<<");
      syslog(LOG_INFO, "esp32s3_es8311_initialize successfully\n");
    }
#    endif /* CONFIG_AUDIO_ES8311 */

#   ifdef CONFIG_AUDIO_ES7210
  ret = esp32s3_es7210_initialize(i2c_audio, i2s_audio);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize ES7210: %d\n", ret);
    }
#   endif

#  endif /* CONFIG_ESP32S3_I2S1 */
#endif /* CONFIG_ESP32S3_I2S */

#ifdef CONFIG_ESP32S3_SDMMC
  /* Initialize and mount the SD card in a background task.  Card probing
   * plus the mount retry loop can take several seconds (worst case when no
   * card is inserted); doing it synchronously here blocked NSH startup and
   * the watch UI, leaving the screen black for that whole time.
   * Keep the same stack size and priority as the init task that used to run
   * this code synchronously (CONFIG_INIT_STACKSIZE / 100).
   */

  ret = task_create("sdmmc_init", 100, CONFIG_INIT_STACKSIZE,
                    sdmmc_mount_task, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to start SDMMC mount task: %d\n", ret);
    }
#endif /* CONFIG_ESP32S3_SDMMC */

  /* If we got here then perhaps not all initialization was successful, but
   * at least enough succeeded to bring-up NSH with perhaps reduced
   * capabilities.
   */

  UNUSED(ret);
  return OK;
}

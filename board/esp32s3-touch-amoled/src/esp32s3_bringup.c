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
#include <string.h>
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
#  include <nuttx/timers/watchdog.h>
#endif

#ifdef CONFIG_AXP2101
#  include <nuttx/power/axp2101.h>
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

#if defined(CONFIG_SYSLOG_FILE) && defined(CONFIG_ESP32S3_SDMMC)
/* SDMMC 命令超时计数（nuttx esp32s3_sdmmc.c 定义）。超时路径里静默
 * 自增，由本任务在 lock-free 上下文上报——否则在 FatFs 锁下打日志
 * 会自死锁，历史上已炸过一次。 */
extern volatile uint32_t g_esp32s3_sdmmc_timeout_count;
#endif

static int syslog_flush_task(int argc, FAR char *argv[])
{
  int pwr_ticks = 0;
#if defined(CONFIG_SYSLOG_FILE) && defined(CONFIG_ESP32S3_SDMMC)
  uint32_t last_sd_to = 0;
#endif

  while (1)
    {
      sleep(5);  /* Flush every 5 seconds */
      syslog_flush();

#if defined(CONFIG_SYSLOG_FILE) && defined(CONFIG_ESP32S3_SDMMC)
      /* SDMMC 命令超时计数变化 → 立即上报。这是"SD 卡命令超时→锁雪崩"
      * 冻结假说的直接证据。 */

      if (g_esp32s3_sdmmc_timeout_count != last_sd_to)
        {
          syslog(LOG_WARNING, "[PWR] SDMMC cmd timeouts: %u\n",
                 g_esp32s3_sdmmc_timeout_count);
          last_sd_to = g_esp32s3_sdmmc_timeout_count;
        }
#endif

#ifdef CONFIG_AXP2101
      /* 电源遥测每 30s：若冻结前出现 chg/vbus 振荡或掉电，即电源问题实锤。
       * chg: 0=待机(充满) 1=充电中 2=放电(未插电)；vbus: 0=未插电 1=插电。 */

      if (++pwr_ticks >= 6)
        {
          pwr_ticks = 0;
          syslog(LOG_INFO, "[PWR] soc=%u%% chg=%u vbus=%u\n",
                 axp2101_get_pmu_soc(),
                 axp2101_get_pmu_charge_status(),
                 axp2101_get_vbus_status());
        }
#endif
    }

  return OK;
}
#endif /* CONFIG_SYSLOG_FILE */

#if defined(CONFIG_WATCHDOG) && defined(CONFIG_ESP32S3_MWDT1)
/****************************************************************************
 * Name: wdt_feed_task
 *
 * Description:
 *   MWDT1 watchdog feeder: 20s timeout, feed every 5s.  The feeder only
 *   does sleep+ioctl (no FS / no locks / no malloc), so:
 *
 *   - System-level freeze (scheduler or interrupts dead)  -> feeder starves
 *     -> MWDT1 resets the chip -> next boot logs reset_reason=0x08 MWDT1.
 *   - Plain thread deadlock (other threads stuck, scheduler alive)
 *     -> feeder keeps feeding -> no reset (device stays frozen for the
 *     user to inspect).  The presence/absence of an MWDT1 reset therefore
 *     distinguishes "CPU-level freeze" from "thread-level deadlock".
 ****************************************************************************/

static int wdt_feed_task(int argc, FAR char *argv[])
{
  int fd = open("/dev/watchdog1", O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "[WDT] open /dev/watchdog1 failed: %d\n", errno);
      return OK;
    }

  if (ioctl(fd, WDIOC_SETTIMEOUT, 20000) < 0)
    {
      syslog(LOG_ERR, "[WDT] MWDT1 set timeout failed: %d\n", errno);
      close(fd);
      return OK;
    }

  if (ioctl(fd, WDIOC_START, 0) < 0)
    {
      syslog(LOG_ERR, "[WDT] MWDT1 start failed: %d\n", errno);
      close(fd);
      return OK;
    }

  syslog(LOG_INFO, "[WDT] MWDT1 armed (20s timeout, 5s feed)\n");

  while (1)
    {
      sleep(5);
      ioctl(fd, WDIOC_KEEPALIVE, 0);
    }

  return OK;
}
#endif /* CONFIG_WATCHDOG && CONFIG_ESP32S3_MWDT1 */

#ifdef CONFIG_ESP32S3_WATCH_SENSOR_QMI8658
#  include <nuttx/sensors/qmi8658.h>
#endif

#include "esp32s3_gpio.h"

#include "esp32s3_reset_reasons.h"

#include "esp32s3-touch-amoled.h"

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define WATCH_DBG_LOG(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

/* 卡logo/冻结取证：开机复位原因。RTC 寄存器保持到下次复位，bringup
 * 入口先读（console 期一条，无串口则丢），SD syslog 重定向后再补一条落盘。 */
static soc_reset_reason_t g_last_reset_reason = RESET_REASON_CHIP_POWER_ON;

static const char *reset_reason_str(soc_reset_reason_t r)
{
  switch (r)
    {
      case RESET_REASON_CHIP_POWER_ON:   return "POR/BOR/SuperWDT";
      case RESET_REASON_CORE_SW:         return "SW core (reboot/panic?)";
      case RESET_REASON_CORE_DEEP_SLEEP: return "deep sleep";
      case RESET_REASON_CORE_MWDT0:      return "MWDT0 watchdog";
      case RESET_REASON_CORE_MWDT1:      return "MWDT1 watchdog (system freeze!)";
      case RESET_REASON_CORE_RTC_WDT:    return "RTC WDT core";
      case RESET_REASON_CPU0_MWDT0:      return "MWDT0 CPU0 watchdog";
      case RESET_REASON_CPU0_SW:         return "SW CPU0 (manual reboot?)";
      case RESET_REASON_CPU0_RTC_WDT:    return "RTC WDT CPU0";
      case RESET_REASON_SYS_BROWN_OUT:   return "BrownOut (power!)";
      case RESET_REASON_SYS_RTC_WDT:     return "RTC WDT system";
      case RESET_REASON_CPU0_MWDT1:      return "MWDT1 CPU0 watchdog";
      case RESET_REASON_SYS_SUPER_WDT:   return "SuperWDT system";
      case RESET_REASON_SYS_CLK_GLITCH:  return "Clock glitch";
      case RESET_REASON_CORE_USB_UART:   return "USB UART";
      case RESET_REASON_CORE_USB_JTAG:   return "USB JTAG";
      case RESET_REASON_CORE_PWR_GLITCH: return "Power glitch!";
      default:                           return "unknown";
    }
}

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
      syslog(LOG_ERR, "ERROR: Failed to initialize SDMMC: %d (%s)\n",
             ret, strerror(-ret));
    }
  else
    {
      syslog(LOG_INFO, "SDMMC initialized successfully\n");
    }

  /* Give the card medium time to finish its power-up init sequence
   * (CMD0 -> CMD55/ACMD41 -> ready).  The block device node /dev/mmcsd1
   * exists as soon as board_sdmmc_initialize() binds the slot, but the
   * card itself may still be busy; mounting too early makes vfat read an
   * invalid boot sector and return -EINVAL.  A short settle delay here
   * avoids that spurious first-mount failure.
   */

  usleep(300000);

  /* Wait for SD card to be ready and try to mount with retries.
   * Some cards take longer to become ready after power-on, so we first
   * wait for the block device to appear and then retry the mount.
   */

  syslog(LOG_INFO, "Waiting for SD card...\n");

  int sd_mounted = 0;
  for (mount_retries = 0; mount_retries < 20; mount_retries++)
    {
      if (stat("/dev/mmcsd1", &buf) == 0)
        {
          ret = nx_mount("/dev/mmcsd1", "/mnt/sd", "vfat", 0, NULL);
          if (ret == OK)
            {
              syslog(LOG_INFO, "SD card mounted at /mnt/sd\n");
              sd_mounted = 1;
              break;
            }

          /* A single failed attempt inside the retry window is normal:
           * the card medium may still be settling.  Log at INFO so it
           * is not mistaken for an error; only an exhaustive failure
           * (all retries exhausted) is reported as ERROR below.
           */

          syslog(LOG_INFO, "SD card not ready, retrying mount "
                 "(attempt %d): %d (%s)\n",
                 mount_retries + 1, ret, strerror(-ret));
        }
      else
        {
          syslog(LOG_INFO, "SD card block device not ready (attempt %d)\n",
                 mount_retries + 1);
        }

      usleep(200000);  /* Wait 200ms before retry */
    }

  /* If the mount loop exhausted all retries without success, /mnt/sd is
   * just the empty directory created by mkdir() above.  Without this
   * diagnostic, "ls /mnt/sd" silently shows nothing and users wrongly
   * assume the card is mounted but empty (e.g. missing AUDIO folder).
   */
  if (!sd_mounted)
    {
      syslog(LOG_ERR, "WARNING: SD card NOT mounted after %d attempts; "
             "/mnt/sd is an empty placeholder dir.\n", mount_retries);
      if (stat("/dev/mmcsd1", &buf) == 0)
        {
          syslog(LOG_ERR, "  /dev/mmcsd1 exists but vfat mount failed. "
                 "Likely causes:\n"
                 "    - Card formatted as exFAT/NTFS (NuttX vfat only "
                 "supports FAT12/16/32; reformat as FAT32)\n"
                 "    - Card has an MBR partition table (mount the "
                 "partition, not the whole device)\n");
        }
      else
        {
          syslog(LOG_ERR, "  /dev/mmcsd1 not found: SDMMC init or card "
                 "detection failed (card not inserted, GPIO17/CS timing, "
                 "or wiring).\n");
        }
    }

#ifdef CONFIG_SYSLOG_FILE
  /* After SD card mount, redirect syslog to a file on the SD card.
   * This keeps logs persistent across reboots.  syslog_file_channel()
   * internally handles log rotation, auto-reconnect on card removal,
   * and crash-safe flush via the interrupt buffer.
   *
   * Only attempt this when the SD card is actually mounted; otherwise
   * /mnt/sd is an empty dir on the root FS and we would redirect syslog
   * (and write test files) into the wrong place, silently filling up
   * internal flash/RAM.
   */

  if (sd_mounted)
    {
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
          test_fd = open("/mnt/sd/syslog/.writetest",
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (test_fd >= 0)
            {
              ssize_t n = write(test_fd, "ok\n", 3);
              close(test_fd);
              syslog(LOG_INFO, "SD write test: fd=%d written=%d\n",
                     test_fd, (int)n);
            }
          else
            {
              syslog(LOG_ERR, "ERROR: SD write test open failed: %d\n",
                     test_fd);
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
              syslog(LOG_ERR, "ERROR: Failed to start flush task: %d\n",
                     ret);
            }

          syslog(LOG_INFO, "Syslog file channel: /mnt/sd/syslog/app.log\n");
          syslog_flush();

          /* Boot context 落盘：上次复位原因（冻结取证关键证据）。 */

          syslog(LOG_INFO, "Boot context: reset_reason=0x%02x (%s)\n",
                 (int)g_last_reset_reason,
                 reset_reason_str(g_last_reset_reason));
          syslog_flush();
        }
    }
  else
    {
      syslog(LOG_ERR, "Skipping syslog-to-SD redirect: SD card not mounted\n");
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

  /* 卡logo取证：此阶段 syslog 尚未重定向到SD，只走console
   * （无串口则丢）。接串口时用于区分 bringup 内卡死的设备。 */
  g_last_reset_reason = esp32s3_reset_reasons(0);
  syslog(LOG_INFO, "[BOOT] reset_reason=0x%02x (%s)\n",
         (int)g_last_reset_reason, reset_reason_str(g_last_reset_reason));
  syslog(LOG_INFO, "[BRINGUP] board bringup: start\n");

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
#ifdef CONFIG_ESP32S3_MWDT1
  else
    {
      /* MWDT1 冻结自动复位 + reset_reason 留痕（取证判据）。 */

      ret = task_create("wdt_feed", 100, 2048, wdt_feed_task, NULL);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to start wdt_feed task: %d\n", ret);
        }
    }
#endif /* CONFIG_ESP32S3_MWDT1 */
#endif

#ifdef CONFIG_I2C_DRIVER
  /* Configure I2C peripheral interfaces */

  ret = board_i2c_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize I2C driver: %d\n", ret);
    }
#endif /*CONFIG_I2C_DRIVER*/

  /* 设置东八区时区。必须放在 RTC 初始化分支之外：
   * 若 PCF85063 初始化失败而不设置 TZ，mktime()/localtime()
   * 将按 UTC 处理，手动设置时间时会把本地时间当作 UTC 写入
   * 系统时钟和 RTC，导致时间快 8 小时。
   */
#ifdef CONFIG_LIBC_LOCALTIME
  setenv("TZ", "CST-8", 1);
  tzset();
#endif

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
  syslog(LOG_INFO, "[BRINGUP] board bringup: done\n");
  return OK;
}

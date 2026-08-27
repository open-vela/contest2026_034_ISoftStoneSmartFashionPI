/**
 * @file volume_control.c
 * 音量控制模块实现
 *
 * 通过 ESP32-S3 I2S + ES8311 音频编解码器控制硬件音量。
 * 百分比 10-100% 线性映射到硬件范围 100-1000（<10% 视为静音=0）。
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <math.h>

#include <arch/board/board.h>
#include <syslog.h>

#include "volume_control.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VOL_TAG "[VOLUME] "
/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define VOL_LOG(fmt, ...) syslog(LOG_INFO, VOL_TAG fmt, ##__VA_ARGS__)

#define WATCH_VOLUME_PERCENT_MIN  10
#define WATCH_VOLUME_PERCENT_MAX 100
#define WATCH_VOLUME_HW_MIN      100   /* hardware value for 10% */
#define WATCH_VOLUME_HW_MAX     1000   /* hardware value for 100% */

/* 音量持久化 —— /mnt/spif/volume.json，格式: {"volume":<hw 0-1000>}
 * 仿照 ui_mode_manager 的 JSON 持久化：板级 g_watch_volume 每次开机
 * 都会重置为 Kconfig 默认值，需在此保存并在开机时恢复
 *（watch_volume_restore，由 launcher 调用）。 */
#define VOLUME_CONFIG_FILE  "/mnt/spif/volume.json"
#define VOLUME_CONFIG_KEY   "\"volume\":"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int s_volume_levels[5] = {0, 250, 500, 750, 1000};

/****************************************************************************
 * Private Functions — persistence
 ****************************************************************************/

/* 保存硬件音量值到 /mnt/spif/volume.json（{"volume":%d}） */
static void watch_volume_save(int value)
{
  FILE *fp = fopen(VOLUME_CONFIG_FILE, "w");
  if (fp == NULL)
    {
      return;
    }
  fprintf(fp, "{\"volume\":%d}\n", value);
  fclose(fp);
}

/* 读取持久化的硬件音量值；文件不存在或内容非法时返回 -1 */
static int watch_volume_load(void)
{
  char buf[64];
  int value = -1;

  FILE *fp = fopen(VOLUME_CONFIG_FILE, "r");
  if (fp == NULL)
    {
      return -1;
    }

  if (fgets(buf, sizeof(buf), fp) != NULL)
    {
      char *key_pos = strstr(buf, VOLUME_CONFIG_KEY);
      if (key_pos != NULL)
        {
          value = atoi(key_pos + strlen(VOLUME_CONFIG_KEY));
        }
    }
  fclose(fp);

  if (value < 0 || value > WATCH_VOLUME_HW_MAX)
    {
      value = -1;
    }
  return value;
}

/****************************************************************************
 * Public Functions — Percent API
 ****************************************************************************/

/* Square-law mapping from percent to hardware value.
 * Spreads the quiet range and compresses the loud end to match
 * human loudness perception.
 *
 *   hw = 1000 * (pct / 100)^2     (0% → 0, 100% → 1000)
 *
 *   pct   0%   10%   20%   30%   50%   70%   100%
 *   hw     0    10    40    90   250   490   1000
 *   DAC    0    85   136   166   204   229    255
 */
static int percent_to_hw(int percent)
{
  if (percent <= 0)
    {
      return 0;
    }
  if (percent >= WATCH_VOLUME_PERCENT_MAX)
    {
      return WATCH_VOLUME_HW_MAX;
    }
  /* k = 2.0: hw ∝ percent^2 */
  return (int)((int64_t)WATCH_VOLUME_HW_MAX * percent * percent /
               (100 * 100));
}

/* Inverse: sqrt of hardware proportion */
static int hw_to_percent(int value)
{
  if (value <= 0)
    {
      return 0;
    }
  if (value >= WATCH_VOLUME_HW_MAX)
    {
      return WATCH_VOLUME_PERCENT_MAX;
    }
  return (int)(100.0f * sqrtf((float)value / WATCH_VOLUME_HW_MAX));
}

int watch_volume_set_percent(int percent)
{
  int hw = percent_to_hw(percent);
  int ret = watch_volume_set_value(hw);
  VOL_LOG("Set percent %d%% → hw=%d (ret=%d)", percent, hw, ret);
  return (ret == 0) ? 0 : -1;
}

int watch_volume_get_percent(void)
{
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
  uint16_t vol = 0;
  if (esp32s3_watch_audio_getvolume(&vol) != OK)
    {
      return -1;
    }
  return hw_to_percent((int)vol);
#else
  return -1;
#endif
}

int watch_volume_step_delta(int delta)
{
  int pct = watch_volume_get_percent();
  if (pct < 0)
    {
      return -1;
    }
  pct += delta;
  if (pct < WATCH_VOLUME_PERCENT_MIN)
    {
      pct = WATCH_VOLUME_PERCENT_MIN;
    }
  if (pct > WATCH_VOLUME_PERCENT_MAX)
    {
      pct = WATCH_VOLUME_PERCENT_MAX;
    }
  watch_volume_set_percent(pct);
  return pct;
}

/****************************************************************************
 * Public Functions — Level API (backward compat)
 ****************************************************************************/

int watch_volume_level_to_value(int level)
{
  if (level < 0) level = 0;
  if (level >= 5) level = 4;
  return s_volume_levels[level];
}

int watch_volume_value_to_level(int value)
{
  if (value <= 0)   return 0;
  if (value <= 250) return 1;
  if (value <= 500) return 2;
  if (value <= 750) return 3;
  return 4;
}

int watch_volume_get_level(void)
{
  int pct = watch_volume_get_percent();
  if (pct < 0) return -1;
  if (pct <= 0)   return 0;
  if (pct <= 25)  return 1;
  if (pct <= 50)  return 2;
  if (pct <= 75)  return 3;
  return 4;
}

int watch_volume_set_level(int level)
{
  int value = watch_volume_level_to_value(level);
  return watch_volume_set_value(value);
}

int watch_volume_get_value(void)
{
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
  uint16_t vol = 0;
  if (esp32s3_watch_audio_getvolume(&vol) != OK)
    {
      return -1;
    }
  return (int)vol;
#else
  return -1;
#endif
}

int watch_volume_set_value(int value)
{
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
  if (value < 0)   value = 0;
  if (value > 1000) value = 1000;
  int ret = esp32s3_watch_audio_setvolume((uint16_t)value);
  VOL_LOG("Set volume to %d (ret=%d)", value, ret);
  if (ret == OK)
    {
      /* 持久化，重启后由 watch_volume_restore() 恢复 */
      watch_volume_save(value);
    }
  return (ret == OK) ? 0 : -1;
#else
  VOL_LOG("Audio not configured, cannot set volume");
  return -1;
#endif
}

int watch_volume_restore(void)
{
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
  int value = watch_volume_load();
  if (value < 0)
    {
      /* 无保存值，保持板级默认音量 */
      return -1;
    }
  /* 直接写硬件 + 板级缓存，不回写文件（值未变化） */
  int ret = esp32s3_watch_audio_setvolume((uint16_t)value);
  VOL_LOG("Restore volume to %d (ret=%d)", value, ret);
  return (ret == OK) ? 0 : -1;
#else
  return -1;
#endif
}

int watch_volume_step_up(void)
{
  return watch_volume_step_delta(10);
}

int watch_volume_step_down(void)
{
  return watch_volume_step_delta(-10);
}

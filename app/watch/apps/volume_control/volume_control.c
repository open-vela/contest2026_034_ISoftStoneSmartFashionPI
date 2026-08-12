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

#include "volume_control.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VOL_TAG "[VOLUME] "
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#define VOL_LOG(fmt, ...)  printf(VOL_TAG fmt "\n", ##__VA_ARGS__)
#else
#define VOL_LOG(fmt, ...)
#endif

#define WATCH_VOLUME_PERCENT_MIN  10
#define WATCH_VOLUME_PERCENT_MAX 100
#define WATCH_VOLUME_HW_MIN      100   /* hardware value for 10% */
#define WATCH_VOLUME_HW_MAX     1000   /* hardware value for 100% */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int s_volume_levels[5] = {0, 250, 500, 750, 1000};

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
  return (ret == OK) ? 0 : -1;
#else
  VOL_LOG("Audio not configured, cannot set volume");
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

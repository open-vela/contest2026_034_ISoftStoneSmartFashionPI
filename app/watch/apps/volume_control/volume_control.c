/**
 * @file volume_control.c
 * 音量控制模块实现
 *
 * 通过 ESP32-S3 I2S + ES8311 音频编解码器控制硬件音量。
 * 音量分 5 级，映射到 0-1000 的硬件音量值。
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

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

#define WATCH_VOLUME_LEVEL_COUNT 5

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int s_volume_levels[WATCH_VOLUME_LEVEL_COUNT] =
  {0, 250, 500, 750, 1000};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int watch_volume_level_to_value(int level)
{
  if (level < 0)
    {
      level = 0;
    }
  if (level >= WATCH_VOLUME_LEVEL_COUNT)
    {
      level = WATCH_VOLUME_LEVEL_COUNT - 1;
    }
  return s_volume_levels[level];
}

int watch_volume_value_to_level(int value)
{
  if (value <= 0)
    {
      return 0;
    }
  if (value <= 250)
    {
      return 1;
    }
  if (value <= 500)
    {
      return 2;
    }
  if (value <= 750)
    {
      return 3;
    }
  return 4;
}

int watch_volume_get_level(void)
{
#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)
  uint16_t vol = 0;
  if (esp32s3_watch_audio_getvolume(&vol) != OK)
    {
      return -1;
    }
  return watch_volume_value_to_level((int)vol);
#else
  return -1;
#endif
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
  if (value < 0)
    {
      value = 0;
    }
  if (value > 1000)
    {
      value = 1000;
    }
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
  int level = watch_volume_get_level();
  if (level < 0)
    {
      return -1;
    }
  if (level < WATCH_VOLUME_LEVEL_COUNT - 1)
    {
      level++;
      watch_volume_set_level(level);
    }
  return level;
}

int watch_volume_step_down(void)
{
  int level = watch_volume_get_level();
  if (level < 0)
    {
      return -1;
    }
  if (level > 0)
    {
      level--;
      watch_volume_set_level(level);
    }
  return level;
}

/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-touch-amoled/src/esp32s3_watch_audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <pthread.h>

#include <nuttx/audio/audio.h>

#include <arch/board/board.h>
#include "esp32s3-touch-amoled.h"

#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)

/****************************************************************************
 * Private Data
 ****************************************************************************/

#define AUDIO_DEVICE_PATH  "/dev/audio/pcm0"

static uint16_t g_watch_volume = CONFIG_ES8311_OUTPUT_INITVOLUME;  /* sync with es8311 reset */
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32s3_watch_audio_setvolume(uint16_t volume)
{
  struct audio_caps_desc_s cap_desc;
  int fd;
  int ret = OK;

  if (volume > 1000)
    {
      volume = 1000;
    }

  pthread_mutex_lock(&g_audio_mutex);

  g_watch_volume = volume;

  /* Open the device briefly to set volume, then close immediately.
   * Keeping a persistent fd would reserve an audio session and
   * block nxplayer (TTS) from creating its own session. */

  fd = open(AUDIO_DEVICE_PATH, O_RDWR);
  if (fd >= 0)
    {
      cap_desc.caps.ac_len       = sizeof(struct audio_caps_s);
      cap_desc.caps.ac_type      = AUDIO_TYPE_FEATURE;
      cap_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
      cap_desc.caps.ac_controls.hw[0] = volume;

      ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc);
      close(fd);
    }
  else
    {
      ret = -errno;
    }

  pthread_mutex_unlock(&g_audio_mutex);

  return (ret < 0) ? -errno : OK;
}

int esp32s3_watch_audio_getvolume(uint16_t *volume)
{
  if (volume == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_audio_mutex);
  *volume = g_watch_volume;
  pthread_mutex_unlock(&g_audio_mutex);

  return OK;
}

#endif /* CONFIG_ESP32S3_I2S && CONFIG_AUDIO_ES8311 */

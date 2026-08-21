#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <nuttx/audio/audio.h>
#include <system/nxplayer.h>
#include <arch/board/board.h>
#include "watch_audio_player.h"

#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)

static FAR struct nxplayer_s *g_watch_player = NULL;
static pthread_mutex_t g_player_mutex = PTHREAD_MUTEX_INITIALIZER;

static int watch_audio_ensure_player(void)
{
    if (g_watch_player == NULL)
    {
        g_watch_player = nxplayer_create();
        if (g_watch_player == NULL)
        {
            return -ENOMEM;
        }

        /* Explicitly set the preferred audio device.  All other working
         * audio code in this project (tong_ai_ws, volc_e2e_voice, etc.)
         * calls nxplayer_setdevice after nxplayer_create.  Without this,
         * the nxplayer falls back to an unreliable device auto-search.
         */
        int devret = nxplayer_setdevice(g_watch_player, "/dev/audio/pcm0");
        if (devret < 0)
        {
            nxplayer_release(g_watch_player);
            g_watch_player = NULL;
            return devret;
        }
    }

    return OK;
}

int esp32s3_watch_audio_play_onetime(const char *filepath, uint16_t volume)
{
    int ret;

    if (filepath == NULL)
    {
        return -EINVAL;
    }

    if (volume > 1000)
    {
        volume = 1000;
    }

    pthread_mutex_lock(&g_player_mutex);

    ret = watch_audio_ensure_player();
    if (ret < 0)
    {
        pthread_mutex_unlock(&g_player_mutex);
        return ret;
    }

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
    nxplayer_stop(g_watch_player);
#endif

#ifndef CONFIG_AUDIO_EXCLUDE_VOLUME
    nxplayer_setvolume(g_watch_player, volume);
#endif

    ret = nxplayer_playfile(g_watch_player, filepath, AUDIO_FMT_UNDEF,
                            AUDIO_FMT_UNDEF);

    pthread_mutex_unlock(&g_player_mutex);

    return ret;
}

int esp32s3_watch_audio_play_repeat(const char *filepath, uint16_t volume)
{
    int ret;

    if (filepath == NULL)
    {
        return -EINVAL;
    }

    if (volume > 1000)
    {
        volume = 1000;
    }

    pthread_mutex_lock(&g_player_mutex);

    ret = watch_audio_ensure_player();
    if (ret < 0)
    {
        pthread_mutex_unlock(&g_player_mutex);
        return ret;
    }

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
    nxplayer_stop(g_watch_player);
#endif

#ifndef CONFIG_AUDIO_EXCLUDE_VOLUME
    nxplayer_setvolume(g_watch_player, volume);
#endif

    ret = nxplayer_playfile(g_watch_player, filepath, AUDIO_FMT_UNDEF,
                            AUDIO_FMT_UNDEF);

    pthread_mutex_unlock(&g_player_mutex);

    return ret;
}

int esp32s3_watch_audio_stop(void)
{
    int ret = OK;

    pthread_mutex_lock(&g_player_mutex);

    if (g_watch_player != NULL)
    {
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
        ret = nxplayer_stop(g_watch_player);
#endif
    }

    pthread_mutex_unlock(&g_player_mutex);

    return ret;
}

/* esp32s3_watch_audio_setvolume() and esp32s3_watch_audio_getvolume()
 * are defined by the board HAL (esp32s3_watch_audio.c) — a single
 * authority that talks to /dev/audio/pcm0 via ioctl.  This file
 * does NOT redefine them; link-time resolves to the board HAL.
 */

#endif

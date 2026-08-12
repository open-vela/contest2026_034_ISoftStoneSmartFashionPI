/**
 * @file watch_audio_player.h
 * 音频播放接口
 */

#ifndef WATCH_AUDIO_PLAYER_H
#define WATCH_AUDIO_PLAYER_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <lvgl/lvgl.h>

#define WATCH_AUDIO_PLAYER_TEST_FILE "/mnt/sd/CAS.WAV"
#define WATCH_AUDIO_PLAYER_ALARM_FILE "/mnt/sd/ALARM.WAV"


#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)

int esp32s3_watch_audio_play_onetime(const char *filepath, uint16_t volume);
int esp32s3_watch_audio_play_repeat(const char *filepath, uint16_t volume);
int esp32s3_watch_audio_stop(void);
/* esp32s3_watch_audio_setvolume / getvolume — provided by board HAL
 * (esp32s3-touch-amoled/src/esp32s3_watch_audio.c),
 * declared via <arch/board/board.h> → esp32s3-touch-amoled.h */

#endif

#endif /* WATCH_AUDIO_PLAYER_H */

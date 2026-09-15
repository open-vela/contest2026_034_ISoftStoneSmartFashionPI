/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * tool_emotion_audio.c — play_audio and stop_audio via nxplayer C API.
 *
 * play_audio:  Parse audio_id (1-14), look up filename, read WAV header,
 *              create nxplayer context, set device to "pcm0",
 *              call nxplayer_playraw() with explicit params.
 *              Playback is asynchronous; the nxplayer context is reference-
 *              counted and self-frees when playback completes.
 *
 * stop_audio:  Stop current playback via nxplayer_stop() and release the
 *              reference via nxplayer_release().
 */

#include "tools/tool_emotion.h"
#include "agent_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <nuttx/audio/audio.h>
#include <system/nxplayer.h>

#include "cJSON.h"

/* ── Audio ID → SD card filename mapping (14 presets) ─────────────── */

#define AUDIO_DIR "/mnt/sd/AUDIO"

static const struct {
    int  id;
    const char *filename;   /* basename under /mnt/sd/audio/ */
    const char *label_cn;   /* Chinese label for logging */
} s_audio_map[] = {
    {  1, "alarm_morning.wav",     "闹铃-早晨"       },
    {  2, "alarm_reminder.wav",    "闹铃-提醒"       },
    {  3, "alarm_wake.wav",        "闹铃-唤醒"       },
    {  4, "light_music.wav",       "轻音乐"          },
    {  5, "sfx_complete.wav",      "完成音效"        },
    {  6, "SFX_NOD.WAV",           "互动音效-点头"    },
    {  7, "sfx_shake.wav",         "互动音效-摇动"    },
    {  8, "tone_connect.wav",      "提示音-连接"      },
    {  9, "tone_error.wav",        "提示音-错误"      },
    { 10, "tone_success.wav",      "提示音-成功"      },
    { 11, "white_noise_ocean.wav", "白噪音-海浪"      },
    { 12, "white_noise_rain.wav",  "白噪音-雨声"      },
    { 13, "white_noise_wind.wav",  "白噪音-风声"      },
    { 14, "whitenoise.wav",        "白噪音"           },
    { 15, "low_battery.wav",       "系统-低电量"      },
    { 16, "no_network.wav",        "系统-无网络"      },
    { 17, "wifi_timeout.wav",      "系统-WiFi超时"    },
};
#define AUDIO_MAP_SIZE (sizeof(s_audio_map) / sizeof(s_audio_map[0]))

/* ── Player context (mutex protected — TR-002) ────────────────────── */

static pthread_mutex_t    s_audio_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct nxplayer_s *s_player    = NULL;
static int                s_current_id = 0;

/* ── WAV header reader ──────────────────────────────────────────────── */
/* Reads a WAV file header and returns sample_rate, channels, bits.
 * Returns 0 on success, -1 on error.  Caller owns nothing. */

struct wav_params {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_offset;
    uint32_t data_size;
};

static int wav_read_header(const char *path, struct wav_params *wp)
{
    unsigned char hdr[44];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (read(fd, hdr, 44) != 44) { close(fd); return -1; }
    close(fd);

    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        return -1;

    wp->channels    = hdr[22] | (hdr[23] << 8);
    wp->sample_rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    wp->bits        = hdr[34] | (hdr[35] << 8);

    /* Find "data" chunk */
    uint32_t off = 36;
    while (off < 44) {
        if (memcmp(hdr + off, "data", 4) == 0) {
            wp->data_offset = off + 8;
            wp->data_size   = hdr[off+4] | (hdr[off+5] << 8)
                            | (hdr[off+6] << 16) | (hdr[off+7] << 24);
            return 0;
        }
        off += 8 + (hdr[off+4] | (hdr[off+5] << 8)
                  | (hdr[off+6] << 16) | (hdr[off+7] << 24));
    }
    /* No "data" chunk in first 44 bytes — use default offset */
    wp->data_offset = 44;
    /* Estimate data size from file total */
    struct stat st;
    if (stat(path, &st) == 0 && (uint32_t)st.st_size > 44)
        wp->data_size = (uint32_t)st.st_size - 44;
    else
        wp->data_size = 0;
    return 0;
}

/* ── Mono→stereo conversion ──────────────────────────────────────────
 * es8311/I2S hardware requires stereo output (ch=2) — the same root cause
 * as the earlier TTS "电音" bug.  A mono 16-bit WAV played directly becomes
 * 2x-speed / distorted, so build a temp stereo copy (L=R) and return its
 * path; otherwise return the source path unchanged.
 * Returns the channel count that will actually play (1 or 2), or -1 on a
 * hard error that still leaves `dst` as a playable copy of `src`. */

#define STEREO_TMP_PATH "/tmp/emotion_stereo.wav"

static int wav_ensure_stereo(const char *src, char *dst, size_t dst_sz,
                             const struct wav_params *wp)
{
    if (wp->channels != 1 || wp->bits != 16) {
        snprintf(dst, dst_sz, "%s", src);
        return (int)wp->channels;
    }

    FILE *fin = fopen(src, "rb");
    if (!fin) {
        snprintf(dst, dst_sz, "%s", src);
        return 1;
    }

    fseek(fin, 0, SEEK_END);
    long fsz = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (fsz <= (long)wp->data_offset) {
        fclose(fin);
        snprintf(dst, dst_sz, "%s", src);
        return 1;
    }

    size_t mono_bytes = (size_t)fsz - wp->data_offset;
    uint8_t *mono = malloc(mono_bytes);
    if (!mono) {
        fclose(fin);
        snprintf(dst, dst_sz, "%s", src);
        return 1;
    }

    if (fseek(fin, wp->data_offset, SEEK_SET) != 0 ||
        fread(mono, 1, mono_bytes, fin) != mono_bytes) {
        free(mono);
        fclose(fin);
        snprintf(dst, dst_sz, "%s", src);
        return 1;
    }
    fclose(fin);

    size_t samples = mono_bytes / 2;
    size_t stereo_bytes = samples * 4;
    uint8_t *stereo = malloc(stereo_bytes);
    if (!stereo) {
        free(mono);
        snprintf(dst, dst_sz, "%s", src);
        return 1;
    }

    const int16_t *m = (const int16_t *)mono;
    int16_t *s = (int16_t *)stereo;
    for (size_t i = 0; i < samples; i++) {
        s[i * 2]     = m[i];
        s[i * 2 + 1] = m[i];
    }
    free(mono);

    snprintf(dst, dst_sz, "%s", STEREO_TMP_PATH);
    FILE *fout = fopen(dst, "wb");
    if (!fout) {
        free(stereo);
        snprintf(dst, dst_sz, "%s", src);
        return 1;
    }

    uint32_t data_sz    = (uint32_t)stereo_bytes;
    uint32_t riff_sz    = 36 + data_sz;
    uint32_t byte_rate  = wp->sample_rate * 4;  /* 2ch × 2 bytes */
    uint16_t block_align = 4;

    fwrite("RIFF", 1, 4, fout);
    fwrite(&riff_sz, 4, 1, fout);
    fwrite("WAVE", 1, 4, fout);
    fwrite("fmt ", 1, 4, fout);
    uint32_t u32 = 16;              fwrite(&u32, 4, 1, fout);
    uint16_t u16 = 1;               fwrite(&u16, 2, 1, fout);  /* PCM */
    u16 = 2;                        fwrite(&u16, 2, 1, fout);  /* channels */
    fwrite(&wp->sample_rate, 4, 1, fout);
    fwrite(&byte_rate, 4, 1, fout);
    fwrite(&block_align, 2, 1, fout);
    u16 = 16;                       fwrite(&u16, 2, 1, fout);  /* bits */
    fwrite("data", 1, 4, fout);
    fwrite(&data_sz, 4, 1, fout);
    fwrite(stereo, 1, stereo_bytes, fout);
    fclose(fout);
    free(stereo);

    syslog(LOG_INFO, "[emotion] mono→stereo converted: %s (%u samples)\n",
           src, (unsigned)samples);
    return 2;
}

/* ── play_audio ────────────────────────────────────────────────────── */

int tool_play_audio_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"invalid json\"}");
        return ERROR;
    }

    cJSON *audio_id = cJSON_GetObjectItem(root, "audio_id");
    int id = 0;
    if (cJSON_IsNumber(audio_id)) {
        id = audio_id->valueint;
    } else if (cJSON_IsString(audio_id)) {
        /* LLM may pass numeric strings with stray quotes like "\"4\"" */
        const char *raw = audio_id->valuestring;
        char clean[16];
        size_t j = 0;
        for (size_t i = 0; raw[i] && j < sizeof(clean) - 1; i++) {
            if (raw[i] >= '0' && raw[i] <= '9')
                clean[j++] = raw[i];
        }
        clean[j] = '\0';
        id = j > 0 ? atoi(clean) : 0;
    } else {
        snprintf(output, output_size, "{\"error\":\"audio_id required\"}");
        cJSON_Delete(root);
        return ERROR;
    }

    if (id < 1 || id > (int)AUDIO_MAP_SIZE) {
        snprintf(output, output_size,
            "{\"error\":\"audio_id out of range (1-%d)\"}", (int)AUDIO_MAP_SIZE);
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON *loop_j = cJSON_GetObjectItem(root, "loop");
    bool loop = cJSON_IsBool(loop_j) ? cJSON_IsTrue(loop_j) : false;

    cJSON_Delete(root);

    const char *fname = s_audio_map[id - 1].filename;
    const char *label = s_audio_map[id - 1].label_cn;
    char filepath[128];
    snprintf(filepath, sizeof(filepath), AUDIO_DIR "/%s", fname);

    /* Mutex-protected playback state update (TR-002) */
    pthread_mutex_lock(&s_audio_mtx);

    /* Stop previous playback if active */
    if (s_player) {
        syslog(LOG_INFO, "[emotion] stopping previous audio %d for new %d\n",
               s_current_id, id);
        nxplayer_stop(s_player);
        nxplayer_release(s_player);
        s_player    = NULL;
        s_current_id = 0;
    }

    /* Create nxplayer context */
    s_player = nxplayer_create();
    if (!s_player) {
        pthread_mutex_unlock(&s_audio_mtx);
        syslog(LOG_ERR, "[emotion] nxplayer_create failed\n");
        snprintf(output, output_size,
            "{\"status\":\"error\",\"reason\":\"player init failed\"}");
        return ERROR;
    }

    /* Read WAV header for correct params — avoid nxplayer_playfile
     * auto-detect bug (double-speed / distortion on NuttX audio). */
    struct wav_params wp;
    if (wav_read_header(filepath, &wp) != 0) {
        syslog(LOG_ERR, "[emotion] bad WAV header: %s\n", filepath);
        nxplayer_release(s_player);
        s_player = NULL;
        pthread_mutex_unlock(&s_audio_mtx);
        snprintf(output, output_size,
            "{\"status\":\"error\",\"reason\":\"bad wav header\"}");
        return ERROR;
    }

    syslog(LOG_INFO, "[emotion] wav: %s = %uch %luhz %ubit data=%lu bytes\n",
           filepath, wp.channels, (unsigned long)wp.sample_rate, wp.bits,
           (unsigned long)wp.data_size);

    /* es8311/I2S requires stereo — convert mono WAVs to a temp stereo copy. */
    char play_path[128];
    int play_ch = wav_ensure_stereo(filepath, play_path, sizeof(play_path), &wp);
    if (play_ch < 0) {
        nxplayer_release(s_player);
        s_player = NULL;
        pthread_mutex_unlock(&s_audio_mtx);
        snprintf(output, output_size,
            "{\"status\":\"error\",\"reason\":\"stereo convert failed\"}");
        return ERROR;
    }

    nxplayer_setdevice(s_player, "/dev/audio/pcm0");

    /* Sync volume to the system setting (same as the TTS path).  Without
     * this nxplayer_playraw() uses its hardcoded default (400) instead of
     * the user's chosen volume. */
    {
        extern int watch_volume_get_value(void);
        int sys_vol = watch_volume_get_value();
        if (sys_vol >= 0) {
            nxplayer_setvolume(s_player, (uint16_t)sys_vol);
        }
    }

    int ret = nxplayer_playraw(s_player, play_path, AUDIO_FMT_PCM, 0,
        (uint8_t)play_ch, 16, wp.sample_rate, 0);
    if (ret != OK) {
        syslog(LOG_ERR, "[emotion] playfile %s failed: ret=%d\n", play_path, ret);
        nxplayer_release(s_player);
        s_player = NULL;
        pthread_mutex_unlock(&s_audio_mtx);
        snprintf(output, output_size,
            "{\"status\":\"error\",\"reason\":\"playback failed\"}");
        return ERROR;
    }

    /* nxplayer_playraw returns before the internal play thread sets
     * state=PLAYING(1); state is still IDLE(0) at this point.  Wait for
     * the IDLE→PLAYING transition while holding s_audio_mtx so the voice
     * channel's tool_emotion_audio_is_playing() (also takes s_audio_mtx)
     * cannot run concurrently and release the player on the stale IDLE. */
    {
        int waited_ms = 0;
        while (waited_ms < 3000 && s_player->state == 0) {
            usleep(50 * 1000);
            waited_ms += 50;
        }
        if (s_player->state == 0) {
            syslog(LOG_WARNING, "[emotion] play_audio: play thread did not start\n");
        }
    }

    s_current_id = id;
    pthread_mutex_unlock(&s_audio_mtx);

    syslog(LOG_INFO, "[emotion] play_audio: id=%d file=%s label=%s loop=%d\n",
           id, fname, label, loop);

    snprintf(output, output_size,
        "{\"status\":\"playing\",\"audio_id\":%d,\"label\":\"%s\",\"loop\":%s}",
        id, label, loop ? "true" : "false");
    return OK;
}

/* ── stop_audio ────────────────────────────────────────────────────── */

int tool_stop_audio_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    pthread_mutex_lock(&s_audio_mtx);

    int  stopped_id  = s_current_id;
    bool was_playing = (s_player != NULL);

    if (s_player) {
        syslog(LOG_INFO, "[emotion] stopping audio id=%d\n", stopped_id);
        nxplayer_stop(s_player);
        nxplayer_release(s_player);
        s_player     = NULL;
        s_current_id = 0;
    }

    pthread_mutex_unlock(&s_audio_mtx);

    if (was_playing) {
        snprintf(output, output_size,
            "{\"status\":\"stopped\",\"audio_id\":%d,\"stop_reason\":\"user_request\"}",
            stopped_id);
    } else {
        snprintf(output, output_size,
            "{\"status\":\"not_playing\",\"stop_reason\":\"no_active_playback\"}");
    }
    return OK;
}

/* ── Direct stop (no JSON parsing) — for voice_channel before TTS ─── */

void tool_emotion_audio_stop(void)
{
    pthread_mutex_lock(&s_audio_mtx);

    if (s_player) {
        syslog(LOG_INFO, "[emotion] TTS preempt: stopping audio id=%d\n",
               s_current_id);
        nxplayer_stop(s_player);
        nxplayer_release(s_player);
        s_player     = NULL;
        s_current_id = 0;
    }

    pthread_mutex_unlock(&s_audio_mtx);
}

bool tool_emotion_audio_is_playing(void)
{
    bool playing;
    pthread_mutex_lock(&s_audio_mtx);
    /* s_player stays allocated after playback ends (for reuse / stop);
     * only treat as "playing" while the nxplayer state is active. */
    playing = (s_player != NULL && s_player->state != 0);
    if (!playing && s_player != NULL) {
        /* Playback finished — release so mic capture can resume */
        syslog(LOG_INFO, "[emotion] audio finished (state=%d), releasing player id=%d\n",
               s_player->state, s_current_id);
        nxplayer_release(s_player);
        s_player     = NULL;
        s_current_id = 0;
    }
    pthread_mutex_unlock(&s_audio_mtx);
    return playing;
}

/* ── Synchronous play (blocks until the WAV finishes) ──────────────── */
/* For proactive prompt sequences (e.g. charging feedback) that play a
 * short preset first and TTS afterwards: returns only after playback
 * completed, so callers can sequence audio→TTS deterministically on
 * pcm0.  Must run on a worker thread (blocks for the WAV duration). */

int tool_emotion_audio_play_wait(int audio_id)
{
    if (audio_id < 1 || audio_id > (int)AUDIO_MAP_SIZE) {
        return -EINVAL;
    }

    const char *fname = s_audio_map[audio_id - 1].filename;
    char filepath[128];
    snprintf(filepath, sizeof(filepath), AUDIO_DIR "/%s", fname);

    /* Stop previous emotion audio so pcm0 is free */
    tool_emotion_audio_stop();

    struct wav_params wp;
    if (wav_read_header(filepath, &wp) != 0) {
        syslog(LOG_ERR, "[emotion] play_wait bad WAV header: %s\n", filepath);
        return -EIO;
    }

    struct nxplayer_s *pl = nxplayer_create();
    if (!pl) {
        syslog(LOG_ERR, "[emotion] play_wait nxplayer_create failed\n");
        return -ENOMEM;
    }
    nxplayer_setdevice(pl, "/dev/audio/pcm0");

    int ret = nxplayer_playraw(pl, filepath, AUDIO_FMT_PCM, 0,
                               wp.channels, wp.bits, wp.sample_rate, 0);
    if (ret != OK) {
        syslog(LOG_ERR, "[emotion] play_wait %s failed: ret=%d\n",
               filepath, ret);
        nxplayer_release(pl);
        return -EIO;
    }

    /* Same IDLE→PLAYING→IDLE wait as alert_play_worker: playraw returns
     * before the internal play thread sets state=PLAYING, so first wait
     * for the transition, then for playback to finish. */
    int waited_ms = 0;
    while (waited_ms < 3000 && pl->state == 0) {
        usleep(50 * 1000);
        waited_ms += 50;
    }
    while (pl->state != 0 && waited_ms < 15000) {
        usleep(200 * 1000);
        waited_ms += 200;
    }

    nxplayer_stop(pl);
    nxplayer_release(pl);

    syslog(LOG_INFO, "[emotion] play_wait: id=%d file=%s finished (%dms)\n",
           audio_id, fname, waited_ms);
    return 0;
}

/* ── System alert audio (highest priority) ──────────────────────────── */

#include "voice/voice_channel.h"

/* De-dup: track last played alert to prevent spam */
static int   s_last_alert_id  = -1;
static time_t s_last_alert_ts = 0;
#define ALERT_COOLDOWN_SEC 30  /* no repeat within 30s */

/**
 * Play a system alert audio at highest priority.
 * - Async: playback runs on a detached worker thread so callers on the
 *   LVGL thread (launcher timer, battery timer) never block
 * - Stops any active TTS (voice_channel_interrupt_tts)
 * - Stops any emotion audio
 * - Plays the WAV file via nxplayer
 * - De-duplicates: same alert_id won't replay within cooldown
 *
 * @param alert_id  Audio ID (15=low_battery, 16=no_network, 17=wifi_timeout)
 * @param force     If true, bypass de-dup (e.g. user explicitly requests)
 */

/* One alert at a time: a new alert arriving while one is playing is
 * dropped (the 30s cooldown makes this rare). */
static volatile bool s_alert_playing = false;

static void* alert_play_worker(void* arg)
{
    int alert_id = (int)(intptr_t)arg;

    const char *fname = s_audio_map[alert_id - 1].filename;
    char filepath[128];
    snprintf(filepath, sizeof(filepath), AUDIO_DIR "/%s", fname);

    /* Stop emotion audio first */
    tool_emotion_audio_stop();

    /* Interrupt active TTS so alert takes the speaker immediately */
    voice_channel_interrupt_tts();

    /* Small gap: let previous audio device release */
    usleep(100 * 1000);

    /* Read WAV header for correct params — same fix as TTS */
    struct wav_params wp;
    if (wav_read_header(filepath, &wp) != 0) {
        syslog(LOG_ERR, "[emotion] alert bad WAV header: %s\n", filepath);
        s_alert_playing = false;
        return NULL;
    }
    syslog(LOG_INFO, "[emotion] alert wav: %s = %uch %luHz %ubit\n",
           fname, wp.channels, (unsigned long)wp.sample_rate, wp.bits);

    /* Play alert via dedicated nxplayer */
    struct nxplayer_s *ap = nxplayer_create();
    if (!ap) {
        syslog(LOG_ERR, "[emotion] alert nxplayer_create failed\n");
        s_alert_playing = false;
        return NULL;
    }
    nxplayer_setdevice(ap, "/dev/audio/pcm0");

    int ret = nxplayer_playraw(ap, filepath, AUDIO_FMT_PCM, 0,
        wp.channels, wp.bits, wp.sample_rate, 0);
    if (ret != OK) {
        syslog(LOG_ERR, "[emotion] alert playraw %s failed: ret=%d\n",
            filepath, ret);
        nxplayer_release(ap);
        s_alert_playing = false;
        return NULL;
    }

    /* Wait for playback completion.  nxplayer_playraw returns before the
     * internal play thread sets state=PLAYING(1) — state is still IDLE(0)
     * at first check, so first wait for the IDLE→PLAYING transition, then
     * for the return to IDLE (playback finished). */
    int waited_ms = 0;
    #define ALERT_MAX_WAIT_MS   15000
    #define ALERT_START_WAIT_MS 3000
    while (waited_ms < ALERT_START_WAIT_MS && ap->state == 0) {
        usleep(50 * 1000);
        waited_ms += 50;
    }
    while (ap->state != 0 && waited_ms < ALERT_MAX_WAIT_MS) {
        usleep(200 * 1000);
        waited_ms += 200;
    }

    nxplayer_stop(ap);
    nxplayer_release(ap);

    syslog(LOG_INFO, "[emotion] alert %d finished (%dms)\n",
        alert_id, waited_ms);
    s_alert_playing = false;
    return NULL;
}

void tool_system_alert_play(int alert_id, int force)
{
    if (alert_id < 15 || alert_id > 17) return;

    /* De-dup: skip if same alert played recently */
    if (!force && alert_id == s_last_alert_id
        && time(NULL) - s_last_alert_ts < ALERT_COOLDOWN_SEC) {
        syslog(LOG_INFO, "[emotion] alert %d suppressed (cooldown=%lds)\n",
               alert_id, (long)(time(NULL) - s_last_alert_ts));
        return;
    }

    if (s_alert_playing) {
        syslog(LOG_INFO, "[emotion] alert %d skipped (another alert playing)\n",
               alert_id);
        return;
    }

    s_last_alert_id  = alert_id;
    s_last_alert_ts  = time(NULL);
    s_alert_playing  = true;

    syslog(LOG_INFO, "[emotion] system alert: id=%d label=%s file=%s\n",
           alert_id, s_audio_map[alert_id - 1].label_cn,
           s_audio_map[alert_id - 1].filename);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, alert_play_worker,
                       (void*)(intptr_t)alert_id) != 0) {
        syslog(LOG_ERR, "[emotion] alert worker spawn failed\n");
        s_alert_playing = false;
    }
}

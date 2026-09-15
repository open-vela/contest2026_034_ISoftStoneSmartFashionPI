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
 * tool_emotion_motion.c — IMU fall / upright detection (emotion toy).
 *
 * Algorithm v5, driven by 8 groups of measured QMI8658 data
 * (docs/fall_detection_design.md): tilt angle vs. a FIXED upright
 * reference — the dock-posture baseline b = (0, -1, 0) of design doc
 * §5.1 — gated by a quasi-static condition.  No boot calibration:
 * the v4 boot-time baseline could lock onto a wrong posture (device
 * held in hand / already lying flat at boot, doc §8.6), and every
 * restart re-learned it.  With the reference fixed, detection starts
 * on the very first frame.  Gyro magnitude is deliberately NOT
 * a trigger — spinning the toy around the gravity axis reaches 490
 * rad/s, above every measured fall impact (277~417 rad/s).
 *
 * State machine:
 *   MONITORING     quasi-static AND theta > 60° vs. the fixed reference
 *                  for 500ms -> FALLEN.
 *   FALLEN         injects a help prompt via voice_channel_inject_prompt
 *                  (PROACTIVE turn; retried while voice is not ready);
 *                  quasi-static AND theta < 30° for 3s -> upright event +
 *                  thanks prompt -> MONITORING; 30s untouched -> timeout.
 *   FALLEN_TIMEOUT silent until one quasi-static theta < 30° frame (the
 *                  toy was really picked up) re-arms MONITORING.
 *
 * Quasi-static: |a| in [0.8, 1.2] g AND |gyro - offset| < 30 rad/s.
 * The gyro zero-rate offset is HARD-CODED (doc §3.2: stable across
 * posture & temperature; a few rad/s of unit-to-unit error is noise
 * against the 30 rad/s gate).  Accel unit is g, NOT m/s^2 — a resting
 * device reads ~0.98.
 */

#include "tools/tool_emotion.h"
#include "agent_compat.h"
#include "voice/voice_channel.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <syslog.h>
#include <math.h>
#include <poll.h>
#include <errno.h>

#include "cJSON.h"

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
/* Forward declare watch expression API (flat build, symbol from watch
 * app — same pattern as tool_emotion_speak.c / agent_loop.c). */
extern int watch_expression_page_set_face(const char* face_id, int duration_ms);
#endif

#if defined(CONFIG_UORB) && defined(CONFIG_SENSORS_QMI8658_POLL)
#include <uORB/uORB.h>
#include <sensor/accel.h>
#include <sensor/gyro.h>
#define MOTION_IMU_AVAILABLE 1
#else
#define MOTION_IMU_AVAILABLE 0
#endif

/* ── Tunables (all values measured, see fall_detection_design.md 8.2) ── */

#define MOTION_SAMPLE_INTERVAL_US 10000  /* request 100Hz; the driver's
                                          * 1s batch work-queue yields a
                                          * real ~30-50Hz — tolerances
                                          * below account for this */
#define MOTION_POLL_TIMEOUT_MS    500
#define MOTION_THREAD_STACK       8192
#define MOTION_THREAD_PRIO        3      /* same as heartbeat */

/* Fixed upright reference — normalized dock-posture gravity vector
 * (design doc §5.1, measured (0.00, -0.98, -0.07)).  The mounting
 * posture is guaranteed by the 3D-printed dock, so no boot learning. */
#define REF_BASE_X  0.0f
#define REF_BASE_Y  -1.0f
#define REF_BASE_Z  0.0f
/* Gyro zero-rate offset — doc §3.2 (y ≈ +4~6, stable) + the latest
 * on-device calibration log, hard-coded since the boot calibration
 * that used to measure it is gone. */
#define REF_GYRO_OX 0.4f
#define REF_GYRO_OY 5.4f
#define REF_GYRO_OZ -0.5f

#define QUIET_ACCEL_MIN      0.8f        /* g   */
#define QUIET_ACCEL_MAX      1.2f        /* g   */
#define QUIET_GYRO_RPS       30.0f       /* rad/s */

#define FALL_ANGLE_DEG       60.0f
#define FALL_CONFIRM_MS      500
#define RECOVER_ANGLE_DEG    30.0f
#define RECOVER_CONFIRM_MS   3000
#define FALLEN_TIMEOUT_MS    30000
#define INJECT_RETRY_MS      5000
#define EVENT_EXPIRE_MS      60000       /* get_motion_event freshness */

/* Proactive prompts — same pattern as the battery reminders in
 * watch_pages.c: the LLM composes the spoken line, tools are forbidden
 * (the face is applied by the agent_loop PROACTIVE branch). */
#define FALL_PROMPT_INJECT \
    "【系统】我摔倒了，起不来啦。请主动向主人求助，" \
    "让主人把我扶起来，用一句话表达，语气自然，不要调用任何工具。"
#define UPRIGHT_PROMPT_INJECT \
    "【系统】主人刚把我扶起来了。请用一句话表达感谢和开心，" \
    "语气自然，不要调用任何工具。"

static const char *TAG = "motion";

/* Protection state flag (fall_protection_feedback tool) — gates the
 * proactive prompts only; detection and get_motion_event keep working
 * so the LLM can re-enable it.  Defined before the monitor thread
 * because motion_inject() checks it. */
static volatile bool s_fall_protected = true;

#if MOTION_IMU_AVAILABLE

typedef enum {
    MOTION_MONITORING = 0,
    MOTION_FALLEN,
    MOTION_FALLEN_TIMEOUT,
} motion_state_t;

/* Last event — written by the monitor thread, read by the agent loop
 * thread from tool_get_motion_event_execute. */
typedef struct {
    char     event[12];      /* "fall" / "upright" */
    char     direction[12];  /* left/right/forward/backward/unknown/none */
    float    angle_deg;
    uint32_t time_ms;        /* CLOCK_MONOTONIC ms */
} motion_event_t;

static volatile bool s_motion_running = false;
static motion_state_t s_state = MOTION_MONITORING;

/* Fixed upright reference & gyro zero-rate offset (v5: constants —
 * the boot calibration that used to fill these is gone). */
static const float s_base_x = REF_BASE_X, s_base_y = REF_BASE_Y,
                   s_base_z = REF_BASE_Z;
static const float s_gyro_ox = REF_GYRO_OX, s_gyro_oy = REF_GYRO_OY,
                   s_gyro_oz = REF_GYRO_OZ;

/* Shared event slot */
static pthread_mutex_t s_event_lock = PTHREAD_MUTEX_INITIALIZER;
static motion_event_t s_last_event;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* Gate + inject, reusing the battery-reminder pattern: only from the
 * LISTENING state, never muted, and keep the fixed face during TTS.
 * Returns true when the prompt was queued. */
static bool motion_inject(const char *prompt)
{
    if (!s_fall_protected)
        return false;
    /* 去掉 voice_channel_is_listening() 门控：跌倒/扶正提示必须在 voice
     * 忙（companion/proactive 回合）时也能注入。voice_channel_inject_prompt
     * 本身不检查状态，提示落入 pending_prompt，voice 回到 LISTENING 后由
     * 循环顶部立即消费并播报，保证跌倒时不仅有表情也有语音。 */
    if (voice_channel_is_mic_muted())
        return false;
    voice_channel_suppress_speaking_face(true);
    return voice_channel_inject_prompt(prompt) == OK;
}

static void motion_record_event(const char *event, const char *dir,
                                float angle)
{
    pthread_mutex_lock(&s_event_lock);
    strncpy(s_last_event.event, event, sizeof(s_last_event.event) - 1);
    s_last_event.event[sizeof(s_last_event.event) - 1] = '\0';
    strncpy(s_last_event.direction, dir, sizeof(s_last_event.direction) - 1);
    s_last_event.direction[sizeof(s_last_event.direction) - 1] = '\0';
    s_last_event.angle_deg = angle;
    s_last_event.time_ms = now_ms();
    pthread_mutex_unlock(&s_event_lock);
}

/* Direction from the resting gravity vector (design doc 8.3):
 * |x| > 0.6 -> side fall, |z| > 0.8 -> front/back fall. */
static const char *motion_direction(float ax, float az)
{
    if (fabsf(ax) > 0.6f)
        return ax > 0.0f ? "left" : "right";
    if (fabsf(az) > 0.8f)
        return az > 0.0f ? "forward" : "backward";
    return "unknown";
}

/* ── Monitor thread ─────────────────────────────────────────────── */

static void *motion_monitor_thread(void *arg)
{
    (void)arg;

    struct pollfd fds[2];
    const struct orb_metadata *meta_acc = ORB_ID(sensor_accel);
    const struct orb_metadata *meta_gyr = ORB_ID(sensor_gyro);
    struct sensor_accel acc;
    struct sensor_gyro gyr;

    /* Subscribe (retry: the sensor driver registers during boot) */
    int fd_acc = -1, fd_gyr = -1;
    for (int attempt = 0; attempt < 5 && s_motion_running; attempt++) {
        if (fd_acc < 0) {
            fd_acc = orb_subscribe_multi(meta_acc, 0);
            if (fd_acc >= 0)
                orb_set_interval(fd_acc, MOTION_SAMPLE_INTERVAL_US);
        }
        if (fd_gyr < 0) {
            fd_gyr = orb_subscribe_multi(meta_gyr, 0);
            if (fd_gyr >= 0)
                orb_set_interval(fd_gyr, MOTION_SAMPLE_INTERVAL_US);
        }
        if (fd_acc >= 0 && fd_gyr >= 0)
            break;
        syslog(LOG_WARNING,
            "[%s] sensor subscribe failed (accel=%d gyro=%d), retry %d\n",
            TAG, fd_acc, fd_gyr, attempt + 1);
        sleep(1);
    }
    if (fd_acc < 0 || fd_gyr < 0) {
        syslog(LOG_ERR, "[%s] IMU not available, monitor exits\n", TAG);
        if (fd_acc >= 0) orb_unsubscribe(fd_acc);
        if (fd_gyr >= 0) orb_unsubscribe(fd_gyr);
        s_motion_running = false;
        return NULL;
    }

    fds[0].fd = fd_acc;
    fds[0].events = POLLIN;
    fds[1].fd = fd_gyr;
    fds[1].events = POLLIN;

    /* Frame-derived quantities */
    bool have_acc = false, have_gyr = false;
    float theta = 0.0f, mag = 0.0f;
    bool quiet = false;

    /* State-machine locals (all CLOCK_MONOTONIC ms, 0 = not started) */
    uint32_t fall_start_ms = 0;     /* MONITORING: theta>60 since */
    uint32_t fallen_at_ms = 0;      /* FALLEN entry time */
    uint32_t recover_start_ms = 0;  /* FALLEN: theta<30 since */
    uint32_t next_inject_ms = 0;    /* FALLEN: next help-prompt retry */
    uint32_t next_status_ms = 0;    /* FALLEN/timeout: next telemetry line */
    bool fall_prompt_sent = false;

    syslog(LOG_INFO, "[%s] monitor started (fixed upright reference)\n", TAG);

    /* Stream watchdog.  A poll timeout leaves the state machine frozen
     * mid-switch — the 30s FALLEN timeout keeps expiring silently and
     * re-arm never evaluates.  First field test went silent for 2+
     * minutes with no way to tell "sensor stream died" (driver worker
     * stalled / HPWORK blocked) from "thread died" from "posture
     * wrong".  The stall/recover pair below splits the first two from
     * the rest; the per-state telemetry lines split the third. */
    int starve_polls = 0;
    uint32_t starve_since_ms = 0;

    while (s_motion_running) {
        int rc = poll(fds, 2, MOTION_POLL_TIMEOUT_MS);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc == 0) {
            if (starve_polls == 0)
                starve_since_ms = now_ms();
            if (++starve_polls == 6)   /* ~3s without a single frame */
                syslog(LOG_WARNING,
                    "[%s] sensor stream stalled (state=%d, %.1fs)\n",
                    TAG, (int)s_state,
                    (now_ms() - starve_since_ms) / 1000.0f);
            continue;
        }

        bool got_frame = false;
        if (fds[0].revents & POLLIN) {
            have_acc = orb_copy(meta_acc, fd_acc, &acc) == OK;
            got_frame = true;
        }
        if (fds[1].revents & POLLIN) {
            have_gyr = orb_copy(meta_gyr, fd_gyr, &gyr) == OK;
            got_frame = true;
        }
        if (got_frame) {
            if (starve_polls >= 6)
                syslog(LOG_WARNING,
                    "[%s] sensor stream recovered after %.1fs\n",
                    TAG, (now_ms() - starve_since_ms) / 1000.0f);
            starve_polls = 0;
        }
        if (!have_acc || !have_gyr)
            continue;

        uint32_t now = now_ms();

        mag = sqrtf(acc.x * acc.x + acc.y * acc.y + acc.z * acc.z);
        float wx = gyr.x - s_gyro_ox;
        float wy = gyr.y - s_gyro_oy;
        float wz = gyr.z - s_gyro_oz;
        float omega = sqrtf(wx * wx + wy * wy + wz * wz);
        quiet = (mag >= QUIET_ACCEL_MIN && mag <= QUIET_ACCEL_MAX &&
                 omega < QUIET_GYRO_RPS);

        /* Tilt angle vs. baseline (|base| == 1) */
        float dot = acc.x * s_base_x + acc.y * s_base_y + acc.z * s_base_z;
        float cosv = mag > 0.0f ? dot / mag : 1.0f;
        if (cosv > 1.0f) cosv = 1.0f;
        if (cosv < -1.0f) cosv = -1.0f;
        theta = acosf(cosv) * (180.0f / 3.14159265f);

        switch (s_state) {
        case MOTION_MONITORING:
            if (quiet && theta > FALL_ANGLE_DEG) {
                if (fall_start_ms == 0) {
                    fall_start_ms = now;
                } else if (now - fall_start_ms >= FALL_CONFIRM_MS) {
                    fallen_at_ms = now;
                    const char *dir = motion_direction(acc.x, acc.z);
                    motion_record_event("fall", dir, theta);
                    syslog(LOG_WARNING,
                        "[%s] FALL %s theta=%.0f mag=%.2f omega=%.1f\n",
                        TAG, dir, theta, mag, omega);
                    /* Distress face NOW, not after the LLM round trip
                     * (~5-10s later): the user must see the toy react
                     * the moment it tips over.  The agent's PROACTIVE
                     * branch re-applies the same face before TTS
                     * (idempotent), and voice restores "listening"
                     * after playback. */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
                    watch_expression_page_set_face("sick", 0);
#endif
                    fall_prompt_sent = false;
                    next_inject_ms = now;   /* try immediately */
                    recover_start_ms = 0;
                    fall_start_ms = 0;
                    next_status_ms = now + 5000;
                    s_state = MOTION_FALLEN;
                }
            } else {
                fall_start_ms = 0;
            }
            break;

        case MOTION_FALLEN:
            /* Telemetry while waiting to be righted: separates a
             * wrong-posture upright (theta stays >= 30) from a
             * never-quiet one (held in a moving hand).  First field
             * test showed upright recognition failing with zero
             * diagnostics — these lines pin down the posture. */
            if ((int32_t)(now - next_status_ms) >= 0) {
                syslog(LOG_INFO,
                    "[%s] recovery wait: a=(%.2f,%.2f,%.2f) theta=%.0f "
                    "quiet=%d mag=%.2f omega=%.1f\n",
                    TAG, acc.x, acc.y, acc.z, theta, quiet, mag, omega);
                next_status_ms = now + 5000;
            }

            /* Help prompt — retried while voice is not ready. */
            if (!fall_prompt_sent && (int32_t)(now - next_inject_ms) >= 0) {
                if (motion_inject(FALL_PROMPT_INJECT)) {
                    fall_prompt_sent = true;
                    syslog(LOG_INFO, "[%s] fall prompt injected\n", TAG);
                } else {
                    next_inject_ms = now + INJECT_RETRY_MS;
                }
            }

            /* Pick-up detection */
            if (quiet && theta < RECOVER_ANGLE_DEG) {
                if (recover_start_ms == 0) {
                    recover_start_ms = now;
                } else if (now - recover_start_ms >= RECOVER_CONFIRM_MS) {
                    motion_record_event("upright", "none", theta);
                    syslog(LOG_INFO, "[%s] UPRIGHT theta=%.0f\n", TAG, theta);
                    /* Happy face NOW — mirrors the FALL branch; also
                     * covers the timeout path where no thanks turn
                     * runs (fall_prompt_sent == false) and nothing
                     * else would clear the sick face. */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
                    watch_expression_page_set_face("proud", 0);
#endif
                    /* Thank the user only if the help line was spoken. */
                    if (fall_prompt_sent) {
                        if (!motion_inject(UPRIGHT_PROMPT_INJECT))
                            syslog(LOG_WARNING,
                                "[%s] upright prompt dropped (voice busy)\n", TAG);
                    }
                    recover_start_ms = 0;
                    s_state = MOTION_MONITORING;
                }
            } else {
                recover_start_ms = 0;
            }

            /* Nobody picked it up — go silent, wait for a real pick-up. */
            if (now - fallen_at_ms >= FALLEN_TIMEOUT_MS) {
                syslog(LOG_INFO,
                    "[%s] fallen timeout after %lus, going silent\n",
                    TAG, (unsigned long)((now - fallen_at_ms) / 1000));
                next_status_ms = now + 10000;
                s_state = MOTION_FALLEN_TIMEOUT;
            }
            break;

        case MOTION_FALLEN_TIMEOUT:
            /* Telemetry: shows why re-arm hasn't fired (posture still
             * >= 30 deg off the fixed reference, or never quiet). */
            if ((int32_t)(now - next_status_ms) >= 0) {
                syslog(LOG_INFO,
                    "[%s] re-arm wait: a=(%.2f,%.2f,%.2f) theta=%.0f "
                    "quiet=%d mag=%.2f omega=%.1f\n",
                    TAG, acc.x, acc.y, acc.z, theta, quiet, mag, omega);
                next_status_ms = now + 10000;
            }

            /* Re-arm only after the toy was really picked upright. */
            if (quiet && theta < RECOVER_ANGLE_DEG) {
                syslog(LOG_INFO, "[%s] re-armed\n", TAG);
                /* Timeout path: the UPRIGHT branch above never ran, so
                 * restore the listening face that the FALL branch took
                 * away — otherwise the toy stays "sick" forever. */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
                watch_expression_page_set_face("listening", 0);
#endif
                s_state = MOTION_MONITORING;
            }
            break;

        default:
            s_state = MOTION_MONITORING;
            break;
        }
    }

    orb_unsubscribe(fd_acc);
    orb_unsubscribe(fd_gyr);
    syslog(LOG_INFO, "[%s] monitor stopped (state=%d)\n", TAG, (int)s_state);
    return NULL;
}

#endif /* MOTION_IMU_AVAILABLE */

/* ── get_motion_event ──────────────────────────────────────────────── */

int tool_get_motion_event_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

#if MOTION_IMU_AVAILABLE
    bool valid = false;
    char event[12] = "";
    char direction[12] = "";
    float angle = 0.0f;

    pthread_mutex_lock(&s_event_lock);
    if (s_last_event.time_ms != 0 &&
        now_ms() - s_last_event.time_ms < EVENT_EXPIRE_MS) {
        valid = true;
        strncpy(event, s_last_event.event, sizeof(event) - 1);
        strncpy(direction, s_last_event.direction, sizeof(direction) - 1);
        angle = s_last_event.angle_deg;
    }
    pthread_mutex_unlock(&s_event_lock);

    if (valid) {
        snprintf(output, output_size,
            "{\"event\":\"%s\",\"direction\":\"%s\",\"angle\":%.0f}",
            event, direction, angle);
        return OK;
    }
#endif

    syslog(LOG_DEBUG, "[%s] get_motion_event: none\n", TAG);
    snprintf(output, output_size, "{\"event\":\"none\"}");
    return OK;
}

/* ── fall_protection_feedback ──────────────────────────────────────── */

int tool_fall_protection_feedback_execute(const char *input_json,
                                           char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"invalid json\"}");
        return ERROR;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action) || !action->valuestring[0]) {
        snprintf(output, output_size, "{\"error\":\"action required\"}");
        cJSON_Delete(root);
        return ERROR;
    }

    const char *act = action->valuestring;
    cJSON_Delete(root);

    if (strcmp(act, "enable") == 0) {
        s_fall_protected = true;
        syslog(LOG_WARNING, "[emotion] fall protection enabled\n");
        snprintf(output, output_size,
            "{\"status\":\"ok\",\"action\":\"enable\",\"protected\":true}");
        return OK;
    }

    if (strcmp(act, "disable") == 0) {
        s_fall_protected = false;
        syslog(LOG_INFO, "[emotion] fall protection disabled\n");
        snprintf(output, output_size,
            "{\"status\":\"ok\",\"action\":\"disable\",\"protected\":false}");
        return OK;
    }

    snprintf(output, output_size, "{\"error\":\"invalid action, must be enable or disable\"}");
    return ERROR;
}

/* ── Monitor lifecycle ─────────────────────────────────────────────── */

int tool_emotion_motion_start(void)
{
#if MOTION_IMU_AVAILABLE
    if (s_motion_running) {
        syslog(LOG_WARNING, "[%s] monitor already running\n", TAG);
        return OK;
    }

    /* Fresh state for a re-launched agent (runtime UI-mode switch). */
    s_state = MOTION_MONITORING;
    memset(&s_last_event, 0, sizeof(s_last_event));
    s_fall_protected = true;

    s_motion_running = true;
    if (agent_task_create(motion_monitor_thread, "motion_mon",
                          MOTION_THREAD_STACK, NULL,
                          MOTION_THREAD_PRIO) != OK) {
        s_motion_running = false;
        syslog(LOG_ERR, "[%s] failed to create monitor thread\n", TAG);
        return ERROR;
    }
    return OK;
#else
    syslog(LOG_WARNING, "[%s] IMU not available, monitor not started\n", TAG);
    return ERROR;
#endif
}

void tool_emotion_motion_stop(void)
{
#if MOTION_IMU_AVAILABLE
    if (s_motion_running) {
        s_motion_running = false;
        /* Thread exits on its next poll timeout (<= 500 ms). */
        syslog(LOG_INFO, "[%s] monitor stop requested\n", TAG);
    }
#endif
}

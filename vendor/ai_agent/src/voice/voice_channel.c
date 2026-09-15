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

#include <nuttx/config.h>
#include "voice/voice_channel.h"
#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS
#include <compexp.h>
#include <echo_canceller.h>
#include <heap_api.h>
#endif
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/audio_playback.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "voice/volc_asr.h"
#include "voice/volc_tts.h"
#ifdef CONFIG_AI_AGENT_VOLC_E2E
#include "voice/volc_e2e.h"
#endif
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
extern int watch_expression_page_set_face(const char* face_id, int duration_ms);
/* Runtime UI-mode switch (no reboot) — runs LVGL teardown/init, so it is
 * always posted to the LVGL thread via lv_async_call (see
 * voice_ui_switch_consumed).  Declared with int/void* to avoid pulling
 * lvgl headers into the voice package; ABI-compatible with the real
 * prototypes (ui_mode_t enum, lv_obj_t*, lv_async_cb_t, lv_result_t). */
extern int ui_mode_switch_runtime(int target, void* parent);
extern void* lv_display_get_default(void);
extern void* lv_display_get_screen_active(void* disp);
extern int lv_async_call(void (*async_cb)(void*), void* user_data);
#define UI_MODE_EXPRESSION 0
#define UI_MODE_WATCH      1
extern int watch_volume_get_value(void);
#endif
#ifdef CONFIG_AI_AGENT_EMOTION_TOY
#include "tools/tool_emotion.h"
#endif
#include "infra/network_manager.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <pthread.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/es7210.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <net/if.h>
#include <system/nxplayer.h>
#include <sys/ioctl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "voice";

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#define VOICE_DBG(fmt, ...)
#else
#define VOICE_DBG(fmt, ...)
#endif

/* Direct capture from I2S character driver (streaming, not file-based).
 * /dev/audio/pcm_in0 supports read() after AUDIOIOC_CONFIGURE + START. */
#define CAPTURE_DEVICE   "/dev/audio/pcm_in0"
#define ASR_CHANNELS      2    /* I2S hardware requires 2-ch */
#define ASR_SAMPLE_RATE   16000
#define ASR_BITS          16
#define ASR_CHUNK_SIZE    3200  /* 100ms @ 16kHz/16bit/mono → volc sends stereo */

/* Idle lifecycle (production values):
 * - first companion turn fires after VOICE_IDLE_TIMEOUT_SEC without
 *   speech/proactive activity
 * - subsequent turns fire every VOICE_COMPANION_INTERVAL_SEC while
 *   the idle continues
 * - standby entry waits VOICE_IDLE_TIMEOUT_SEC after the companion
 *   budget is exhausted
 * See conversation_thread ("idle lifecycle"). */
#define VOICE_IDLE_TIMEOUT_SEC 180

/* Companion interaction: number of proactive companion turns before the
 * device gives up and enters standby. */
#define VOICE_COMPANION_MAX 3

/* Idle window between companion turns 2..VOICE_COMPANION_MAX. */
#define VOICE_COMPANION_INTERVAL_SEC 60

/* Standby display duration: after this many seconds on the standby face
 * with no wake word, the launcher turns the display off (final state,
 * wake word still works). */
#define VOICE_STANDBY_TIMEOUT_SEC 180

/* Client-side energy VAD used ONLY to reset the idle timer: average
 * absolute amplitude of a 100ms mono chunk.  Speech at arm's length is
 * typically well above 800; a quiet room sits below 400.  Raise if
 * standby fires during speech, lower if it never fires in a noisy room. */
#define VOICE_IDLE_VAD_THRESH 400
static int s_backends_registered;

/* Companion prompt: short, open-ended directive — identical text each
 * turn, the LLM sees its own previous companion turns in the chat
 * history so the replies still vary (proactive turns bypass llm_cache
 * and skill trigger matching in agent_loop).  The last turn adds a
 * gentle farewell + wake hint so the user knows how to revive the toy
 * after standby (no bedtime wording — standby happens any time of
 * day).  Tool calls are forbidden: keeps the turn lightweight and
 * avoids set_face overrides racing the TTS speaking face. */
#define COMPANION_PROMPT_COMMON \
    "【系统】主人很久没说话了，请主动发起简短互动，" \
    "一两句话就好，不要调用任何工具。"
#define COMPANION_PROMPT_LAST \
    "【系统】主人很久没说话了，这是你这次主动陪伴的最后一句，" \
    "请温柔道别，并提醒主人想聊天随时喊你的名字唤醒你，" \
    "一两句话就好，不要调用任何工具。"

static const char* const COMPANION_PROMPTS[] = {
    COMPANION_PROMPT_COMMON,   /* turns 1 .. VOICE_COMPANION_MAX-1 */
    COMPANION_PROMPT_LAST,     /* final turn before standby */
};

/* ── Audio preprocessing (NS + AGC via BES ec2float) ────── */

#ifdef CONFIG_AI_AGENT_AUDIO_PREPROCESS

#define PREPROC_HEAP_SIZE (150 * 1024)
#define PREPROC_FRAME_MS 15
#define PREPROC_FRAME_SIZE \
    (AGENT_VOICE_SAMPLE_RATE / 1000 * PREPROC_FRAME_MS)

static const Ec2FloatConfig s_ns_cfg = {
    .bypass = 0,
    .hpf_enabled = 1,
    .af_enabled = 0, /* no AEC */
    .adprop_enabled = 0,
    .varistep_enabled = 0,
    .nlp_enabled = 0, /* no NLP */
    .clip_enabled = 0,
    .stsupp_enabled = 0,
    .hfsupp_enabled = 0,
    .constrain_enabled = 0,
    .ns_enabled = 1, /* noise suppression ON */
    .cng_enabled = 0,
    .blocks = 1,
    .delay = 0,
    .gamma = 0.9f,
    .echo_band_start = 300,
    .echo_band_end = 1800,
    .min_ovrd = 2,
    .target_supp = -40,
    .highfre_band_start = 4000,
    .highfre_supp = 8.f,
    .noise_supp = -15,
    .cng_type = 0,
    .cng_level = -60,
    .clip_threshold = -20.f,
    .banks = 64,
};

static const CompexpConfig s_agc_cfg = {
    .bypass = 0,
    .type = 0,
    .comp_threshold = -20.f,
    .comp_ratio = 2.f,
    .expand_threshold = -45.f,
    .expand_ratio = 0.5556f,
    .attack_time = 0.001f,
    .release_time = 0.006f,
    .makeup_gain = 6,
    .delay = 32,
    .tav = 0.2f,
};

typedef struct {
    void* heap;
    Ec2FloatState* ns;
    CompexpState* agc;
} audio_preproc_t;

static audio_preproc_t* preproc_create(void)
{
    audio_preproc_t* pp = calloc(1, sizeof(*pp));
    if (!pp) {
        return NULL;
    }

    pp->heap = malloc(PREPROC_HEAP_SIZE);
    if (!pp->heap) {
        free(pp);
        return NULL;
    }
    med_heap_init(pp->heap, PREPROC_HEAP_SIZE);

    pp->ns = ec2float_create(AGENT_VOICE_SAMPLE_RATE,
        PREPROC_FRAME_SIZE, 0, &s_ns_cfg);
    pp->agc = compexp_create(AGENT_VOICE_SAMPLE_RATE,
        PREPROC_FRAME_SIZE, &s_agc_cfg);

    syslog(LOG_INFO, "[%s] preproc: NS=%p AGC=%p heap=%dKB\n",
        TAG, (void*)pp->ns, (void*)pp->agc,
        PREPROC_HEAP_SIZE / 1024);
    return pp;
}

static void preproc_destroy(audio_preproc_t* pp)
{
    if (!pp) {
        return;
    }
    if (pp->agc) {
        compexp_destroy(pp->agc);
    }
    if (pp->ns) {
        ec2float_destroy(pp->ns);
    }
    free(pp->heap);
    free(pp);
}

static void preproc_chunk(audio_preproc_t* pp,
    unsigned char* buf, int bytes)
{
    if (!pp || (!pp->ns && !pp->agc)) {
        return;
    }

    int16_t* samples = (int16_t*)buf;
    int total = bytes / 2;
    int fsz = PREPROC_FRAME_SIZE;
    int32_t in32[PREPROC_FRAME_SIZE];
    int32_t ref32[PREPROC_FRAME_SIZE];
    int32_t out32[PREPROC_FRAME_SIZE];

    memset(ref32, 0, sizeof(ref32));

    for (int off = 0; off < total; off += fsz) {
        int n = (off + fsz <= total) ? fsz : (total - off);

        for (int i = 0; i < n; i++) {
            in32[i] = samples[off + i];
        }

        if (pp->ns) {
            ec2float_process(pp->ns, in32, ref32, n, out32);
        } else {
            memcpy(out32, in32, n * sizeof(int32_t));
        }

        if (pp->agc) {
            for (int i = 0; i < n; i++) {
                out32[i] <<= 8;
            }
            compexp_process_int24(pp->agc, out32, n);
            for (int i = 0; i < n; i++) {
                out32[i] >>= 8;
            }
        }

        for (int i = 0; i < n; i++) {
            int32_t v = out32[i];
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            samples[off + i] = (int16_t)v;
        }
    }
}

#endif /* CONFIG_AI_AGENT_AUDIO_PREPROCESS */

/* ── Conversation state machine ──────────────────────────── */

enum voice_state {
    VOICE_IDLE = 0,
    VOICE_LISTENING,        /* mic open, streaming PCM → ASR WebSocket */
    VOICE_PROCESSING,       /* ASR finished, waiting for LLM + TTS */
    VOICE_SPEAKING,         /* TTS playback in progress, mic muted */
};

static const char* state_name(enum voice_state s)
{
    switch (s) {
    case VOICE_IDLE:      return "IDLE";
    case VOICE_LISTENING: return "LISTENING";
    case VOICE_PROCESSING:return "PROCESSING";
    case VOICE_SPEAKING:  return "SPEAKING";
    default:              return "?";
    }
}

static struct {
    enum voice_state state;
    pthread_mutex_t lock;
    int cap_fd;                 /* capture device fd (/dev/audio/pcm_in0) */
    mqd_t cap_mq;               /* message queue for audio buffers */
    char cap_mq_name[32];       /* mqueue name */
    struct ap_buffer_s** cap_bufs; /* allocated audio buffers */
    int cap_nbufs;              /* number of audio buffers */
    voice_asr_stream_t* stream; /* volc ASR WebSocket stream */
    pthread_t conv_thread;      /* continuous conversation thread */
    volatile bool conv_running; /* set to false to stop conversation */
    volatile bool wake_gated;   /* true: only wake-word can open the gate */
    void (*wake_notify)(void);  /* called when wake word detected (before speak) */
    void (*idle_timeout_notify)(int event); /* idle lifecycle event (VOICE_IDLE_EVT_*) */
    volatile int tts_abort;     /* set to 1 to stop active TTS playback */
    voice_asr_stream_t* pre_stream; /* ASR stream pre-opened during TTS */
    volatile bool suppress_speaking_face; /* keep emotion face during TTS */
    volatile struct nxplayer_s* tts_player; /* active TTS player for interrupt */
    volatile bool mic_mute_pending; /* button requested mic mute/unmute */
    volatile bool mic_mute_target;  /* true=mute, false=unmute */
    char* pending_prompt;           /* proactive prompt queued for the
                                     * conversation thread (consumed at
                                     * the top of its main loop) */
    volatile uint32_t play_end_ms;  /* CLOCK_MONOTONIC ms when TTS playback
                                     * is expected to end (0 = not playing).
                                     * Set by speak(), read by conv thread. */
    audio_playback_t* tts_pb;   /* active TTS playback handle (or NULL) */
    pthread_mutex_t speak_lock; /* serialize concurrent speak calls */
} s_voice = {
    .state = VOICE_IDLE,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .speak_lock = PTHREAD_MUTEX_INITIALIZER,
    .cap_fd = -1,
    .cap_mq = (mqd_t)-1,
};

/* ── ASR worker thread ──────────────────────────────────── */

#define ASR_THREAD_STACK (24 * 1024)

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ── TTS latency tracking ───────────────────────────────── */

static struct timespec s_tts_start;
static int s_tts_first_chunk;
static struct timespec s_asr_done_ts;

/* ── TTS streaming: collect PCM chunks into memory buffer ── */

/* Collect all mono PCM chunks into a pre-allocated RAM buffer.
 * The buffer is sized to leave room for the subsequent in-place
 * mono→stereo expansion (hence cap = TTS_BUF_CAP / 2). */
typedef struct {
    unsigned char* buf;
    size_t cap;
    size_t len;
    int err;
} tts_collect_ctx_t;

static void tts_collect_cb(const unsigned char* pcm, size_t pcm_len,
    int is_last, void* ud)
{
    tts_collect_ctx_t* ctx = ud;
    if (ctx->err) return;
    if (pcm && pcm_len > 0) {
        if (!s_tts_first_chunk) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - s_tts_start.tv_sec) * 1000
                + (now.tv_nsec - s_tts_start.tv_nsec) / 1000000;
            syslog(LOG_INFO, "[%s] TTS first chunk: %ldms\n", TAG, ms);
            s_tts_first_chunk = 1;
        }
        if (ctx->len + pcm_len > ctx->cap) {
            syslog(LOG_ERR, "[%s] TTS PCM overflow: %zu+%zu>%zu\n",
                TAG, ctx->len, pcm_len, ctx->cap);
            ctx->err = -ENOSPC;
            return;
        }
        memcpy(ctx->buf + ctx->len, pcm, pcm_len);
        ctx->len += pcm_len;
    }
    (void)is_last;
}

#if 0  /* unused — batch ASR path replaced by streaming ASR */

static void* asr_and_dispatch(void* arg)
{
    unsigned char* pcm = (unsigned char*)arg;
    size_t pcm_len;

    memcpy(&pcm_len, pcm, sizeof(size_t));
    const unsigned char* audio = pcm + sizeof(size_t);

    syslog(LOG_INFO, "[%s] ASR: recognizing %zu bytes\n", TAG,
        TAG, pcm_len);

    char text[512];
    int ret = voice_asr_recognize(audio, pcm_len,
        text, sizeof(text));

    if (ret == 0 && text[0] != '\0') {
        VOICE_DBG("ASR result: %s", text);

        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_VOICE,
            sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice",
            sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    } else {
        syslog(LOG_WARNING, "[%s] ASR failed or empty: %d\n",
            TAG, ret);
    }

    free(pcm);
    return NULL;
}

#endif  /* unused — batch ASR path */


/* ── Public API ──────────────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_NET_RPMSG
static void s_voice_net_cb(net_state_t state, void* arg);
#endif

/* Reset the static backend registries.  On NuttX the ai_agent is re-launched
 * via task_create() for a runtime UI-mode switch, so these statics persist
 * and would otherwise (a) make voice_channel_is_ready() return true before
 * the new instance re-registers, and (b) overflow the backend arrays. */
void voice_channel_reset_backends(void)
{
    s_backends_registered = 0;
    voice_tts_reset();
    voice_asr_reset();
}

int voice_channel_init(void)
{
    syslog(LOG_INFO, "[%s] Voice channel initializing\n", TAG);

    /* Reset state on init — static vars may persist across process restarts
     * in NuttX depending on how the binary is loaded. */
    s_voice.state = VOICE_IDLE;
    s_voice.cap_fd = -1;
    s_voice.cap_mq = (mqd_t)-1;
    s_voice.stream = NULL;
    s_voice.conv_running = false;
    s_voice.wake_gated = false;
    s_voice.tts_abort = 0;
    s_voice.tts_pb = NULL;
    /* Flat-build task_create() relaunch keeps statics alive — free any
     * prompt left over from the previous instance. */
    free(s_voice.pending_prompt);
    s_voice.pending_prompt = NULL;

    /* Always re-register backends — static vars persist across NuttX
     * flat build task_create() re-launch. */
    voice_channel_reset_backends();

    if (!s_backends_registered) {
        int rc_http = volc_tts_register();
        int rc_ws = volc_tts_ws_register();
        volc_asr_register();
        /* Prefer WebSocket TTS (streaming, lower latency) */
        int rc_set = voice_tts_set_backend("volcengine_ws");
        voice_asr_set_backend("volcengine");
        s_backends_registered = 1;
        syslog(LOG_INFO, "[%s] TTS backends: http=%d ws=%d set=%d active=%s\n",
            TAG, rc_http, rc_ws, rc_set,
            voice_tts_get_backend() ? voice_tts_get_backend() : "none");

#ifdef CONFIG_AI_AGENT_VOLC_E2E
        /* E2E realtime dialogue: one persistent WebSocket carries both
         * ASR and TTS.  Registered unconditionally so it can be enabled
         * at runtime, but only activated when voice_mode == "e2e". */
        volc_e2e_asr_register();
        volc_e2e_tts_register();

        char mode[16] = { 0 };
        claw_config_get(AGENT_CFG_KEY_VOICE_MODE, mode, sizeof(mode));
        if (strcmp(mode, "e2e") == 0) {
            int rc_a = voice_asr_set_backend(AGENT_VOICE_BACKEND_E2E);
            int rc_t = voice_tts_set_backend(AGENT_VOICE_BACKEND_E2E);
            syslog(LOG_INFO, "[%s] voice_mode=e2e (asr=%d tts=%d)\n",
                TAG, rc_a, rc_t);
            if (rc_a != 0 || rc_t != 0) {
                /* Never leave the pipeline half switched. */
                voice_asr_set_backend("volcengine");
                voice_tts_set_backend("volcengine_ws");
                syslog(LOG_ERR, "[%s] e2e activation failed, using classic\n",
                    TAG);
            } else {
                /* Hybrid orchestration: when enabled, E2E server LLM
                 * (fast chat) and the client agent LLM (tools/skills)
                 * run in parallel.  Routing happens per-turn in
                 * agent_loop.c based on whether tools were called.
                 *
                 * Default: ON when voice_mode=e2e.  Set
                 * hybrid_mode=off to disable. */
                char hyb[16] = { 0 };
                claw_config_get(AGENT_CFG_KEY_HYBRID_MODE,
                    hyb, sizeof(hyb));
                bool hyb_on = !(strcmp(hyb, "off") == 0
                    || strcmp(hyb, "0") == 0
                    || strcmp(hyb, "false") == 0);
                volc_e2e_set_hybrid_enabled(hyb_on);
                syslog(LOG_INFO,
                    "[%s] hybrid_mode=%s (default on for e2e)\n",
                    TAG, hyb_on ? "on" : "off");
            }
        }
#endif
    }

    /* Register network state listener for WiFi-drop alerts (RPMSG mode only).
     * In direct WiFi mode, the conversation_thread polls network_is_connected(). */
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    network_register_listener(s_voice_net_cb, NULL);
#endif

    return 0;
}

/* Network state change callback: alert user if WiFi drops mid-conversation.
 * Only compiled in RPMSG mode (push-based network monitoring). */
#ifdef CONFIG_AI_AGENT_NET_RPMSG
static void s_voice_net_cb(net_state_t state, void* arg)
{
    (void)arg;
    if (state == NET_STATE_DISCONNECTED
        && s_voice.state != VOICE_IDLE
        && s_voice.state != VOICE_LISTENING) {
        /* Only alert during active conversation (PROCESSING/SPEAKING) */
        syslog(LOG_WARNING, "[%s] WiFi disconnected during conversation\n", TAG);
        tool_system_alert_play(17, 0);
    }
}
#endif

/* ── WiFi link-level drop detection (direct WiFi mode) ────────────
 * Without RPMSG there is no push callback for network state.  The
 * ASR-open failure proxy further down reacts only after 3 failed
 * handshakes (6-10s+).  IFF_RUNNING on wlan0 clears within seconds of
 * an AP drop — unlike network_is_connected(), which stays true while
 * the interface keeps its IP — giving near-real-time detection.
 * Polled (throttled) from the listening frame loop. */
static time_t s_link_chk_time = 0;    /* last check timestamp */
static bool   s_link_alerted = false; /* one alert per drop episode */

static bool wifi_link_up(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return true; /* can't tell — don't alert */
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);

    bool up = false;
    if (ioctl(sock, SIOCGIFFLAGS, (unsigned long)&ifr) == 0) {
        up = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
    }
    close(sock);
    return up;
}

bool voice_channel_is_ready(void)
{
    return s_backends_registered != 0;
}

/* ── Streaming capture: NuttX audio buffer pipeline ──────── */

/* ── Frame-level audio capture via NuttX ioctl pipeline ──── */
/* Mirrors nxrecorder's internal sequence: RESERVE→GETCAPS→CONFIGURE
 * →ALLOCBUFFER×N→REGISTERMQ→ENQUEUEBUFFER×N→START→mq_receive loop */

static int capture_open(void)
{
    /* O_RDWR required — audio driver ioctl needs write permission */
    int fd = open(CAPTURE_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        syslog(LOG_ERR, "[%s] open(%s) failed: %d\n",
            TAG, CAPTURE_DEVICE, errno);
        return -errno;
    }

    /* Step 1: RESERVE */
    if (ioctl(fd, AUDIOIOC_RESERVE, 0) < 0) {
        syslog(LOG_ERR, "[%s] RESERVE failed: %d\n", TAG, errno);
        close(fd); return -errno;
    }

    /* Step 2: GETCAPS (QUERY) → GETCAPS (INPUT) */
    struct audio_caps_s caps;
    caps.ac_len  = sizeof(caps);
    caps.ac_type = AUDIO_TYPE_QUERY;
    ioctl(fd, AUDIOIOC_GETCAPS, (uintptr_t)&caps);

    caps.ac_len  = sizeof(caps);
    caps.ac_type = AUDIO_TYPE_INPUT;
    ioctl(fd, AUDIOIOC_GETCAPS, (uintptr_t)&caps);

    /* Step 3: CONFIGURE */
    struct audio_caps_desc_s cap_desc;
    memset(&cap_desc, 0, sizeof(cap_desc));
    cap_desc.caps.ac_len      = sizeof(cap_desc.caps);
    cap_desc.caps.ac_type     = AUDIO_TYPE_INPUT;
    cap_desc.caps.ac_channels = ASR_CHANNELS;
    cap_desc.caps.ac_chmap    = 0;
    cap_desc.caps.ac_controls.hw[0] = ASR_SAMPLE_RATE;
    cap_desc.caps.ac_controls.b[3]  = (uint8_t)(ASR_SAMPLE_RATE >> 16);
    cap_desc.caps.ac_controls.b[2]  = ASR_BITS;
    cap_desc.caps.ac_subtype        = AUDIO_FMT_PCM;

    if (ioctl(fd, AUDIOIOC_CONFIGURE, (uintptr_t)&cap_desc) < 0) {
        syslog(LOG_ERR, "[%s] CONFIGURE failed: %d\n", TAG, errno);
        ioctl(fd, AUDIOIOC_RELEASE, 0);
        close(fd); return -errno;
    }

    /* Step 4: GETBUFFERINFO */
    struct ap_buffer_info_s buf_info;
    memset(&buf_info, 0, sizeof(buf_info));
    if (ioctl(fd, AUDIOIOC_GETBUFFERINFO, (uintptr_t)&buf_info) != OK) {
        buf_info.nbuffers    = CONFIG_AUDIO_NUM_BUFFERS;
        buf_info.buffer_size = 4096;
    }
    int nbufs = buf_info.nbuffers;
    if (nbufs < 1) nbufs = CONFIG_AUDIO_NUM_BUFFERS;

    syslog(LOG_INFO, "[%s] capture: %d buffers x %d bytes\n",
        TAG, nbufs, (int)buf_info.buffer_size);

    /* Step 5: ALLOCBUFFER × N */
    struct ap_buffer_s** buffers = calloc((size_t)nbufs, sizeof(void*));
    if (!buffers) { ioctl(fd, AUDIOIOC_RELEASE, 0); close(fd); return -ENOMEM; }

    struct audio_buf_desc_s buf_desc;
    for (int i = 0; i < nbufs; i++) {
        memset(&buf_desc, 0, sizeof(buf_desc));
        buf_desc.numbytes  = buf_info.buffer_size;
        buf_desc.u.pbuffer = &buffers[i];
        if (ioctl(fd, AUDIOIOC_ALLOCBUFFER, (uintptr_t)&buf_desc) < 0) {
            syslog(LOG_ERR, "[%s] ALLOCBUFFER[%d] failed: %d\n", TAG, i, errno);
            while (--i >= 0) ioctl(fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&buffers[i]);
            free(buffers); ioctl(fd, AUDIOIOC_RELEASE, 0); close(fd); return -ENOMEM;
        }
    }

    /* Step 6: Create MQ + REGISTERMQ */
    struct mq_attr attr;
    attr.mq_maxmsg  = nbufs + 8;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_curmsgs = 0;
    attr.mq_flags   = 0;

    snprintf(s_voice.cap_mq_name, sizeof(s_voice.cap_mq_name),
        "/tmp/vcap%lx", (unsigned long)((uintptr_t)&s_voice));
    mqd_t mq = mq_open(s_voice.cap_mq_name, O_RDWR | O_CREAT, 0644, &attr);
    if (mq == (mqd_t)-1) {
        syslog(LOG_ERR, "[%s] mq_open failed: %d\n", TAG, errno);
        for (int i = 0; i < nbufs; i++) ioctl(fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&buffers[i]);
        free(buffers); ioctl(fd, AUDIOIOC_RELEASE, 0); close(fd); return -errno;
    }
    ioctl(fd, AUDIOIOC_REGISTERMQ, (uintptr_t)mq);

    /* Step 7: ENQUEUEBUFFER × N (submit empty buffers before START) */
    for (int i = 0; i < nbufs; i++) {
        memset(&buf_desc, 0, sizeof(buf_desc));
        buf_desc.u.buffer = buffers[i];
        if (ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&buf_desc) < 0) {
            syslog(LOG_ERR, "[%s] ENQUEUEBUFFER[%d] failed: %d\n", TAG, i, errno);
        }
    }

    s_voice.cap_bufs  = buffers;
    s_voice.cap_nbufs = nbufs;
    s_voice.cap_mq    = mq;

    syslog(LOG_INFO, "[%s] capture ready: %d buffers enqueued\n", TAG, nbufs);
    return fd;
}

static int capture_start(int fd)
{
    return ioctl(fd, AUDIOIOC_START, 0);
}

static int capture_stop(int fd)
{
    return ioctl(fd, AUDIOIOC_STOP, 0);
}

/* Read one DMA-filled audio frame.  Blocks on mq_receive until
 * driver sends AUDIO_MSG_DEQUEUE.  Returns *pcm pointing into
 * apb->samp (no copy) and *pcm_len = apb->nbytes.
 * Caller MUST call capture_release_frame(apb) after processing. */
static int capture_read_frame(struct ap_buffer_s** out_apb,
    const uint8_t** out_pcm, size_t* out_len)
{
    struct audio_msg_s msg;
    /* 5-second timeout: if no audio frame arrives within 5s,
     * the driver is likely stuck (no buffers to fill after stop/start).
     * Return -ETIMEDOUT so the caller can recover instead of blocking
     * forever on mq_receive.
     * NOTE: mq_timedreceive interprets the absolute deadline against
     * CLOCK_REALTIME (POSIX).  Using CLOCK_MONOTONIC here made the
     * deadline lie in the past once NTP set the wall clock, so the
     * call returned ETIMEDOUT immediately whenever the queue was
     * empty — capture died after the ~4 pre-filled buffers. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;

    ssize_t size = mq_timedreceive(s_voice.cap_mq, (char*)&msg, sizeof(msg),
                                  NULL, &ts);
    if (size != sizeof(msg)) {
        if (errno == ETIMEDOUT) {
            syslog(LOG_ERR, "[%s] capture_read_frame: 5s timeout (driver stuck?)\n",
                TAG);
            return -ETIMEDOUT;
        }
        return -EIO;
    }

    if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr) {
        struct ap_buffer_s* apb = (struct ap_buffer_s*)msg.u.ptr;
        *out_apb = apb;
        *out_pcm = apb->samp;
        *out_len = apb->nbytes;
        return 0;
    }
    return -EAGAIN;
}

/* Return a processed buffer to the driver for refill.
 * Uses buf_desc.u.buffer (not raw apb pointer) per nxrecorder. */
static int capture_release_frame(struct ap_buffer_s* apb)
{
    struct audio_buf_desc_s buf_desc;
    memset(&buf_desc, 0, sizeof(buf_desc));
    buf_desc.u.buffer = apb;
    return ioctl(s_voice.cap_fd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&buf_desc);
}

/* Drain stale messages from the capture message queue and
 * re-enqueue the buffers to the driver so DMA can fill them.
 * Must be called AFTER capture_stop and BEFORE capture_start
 * to prevent mq_receive from returning old audio data and
 * to ensure the driver has buffers to fill on restart. */
static int capture_drain(void)
{
    if (s_voice.cap_mq == (mqd_t)-1) return 0;

    struct mq_attr attr;
    if (mq_getattr(s_voice.cap_mq, &attr) != 0) return 0;

    int drained = 0;
    struct audio_msg_s msg;
    struct timespec ts_zero = {0, 0};

    while (mq_timedreceive(s_voice.cap_mq, (char*)&msg, sizeof(msg),
                           NULL, &ts_zero) == sizeof(msg)) {
        if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr) {
            struct ap_buffer_s* apb = (struct ap_buffer_s*)msg.u.ptr;
            struct audio_buf_desc_s buf_desc;
            memset(&buf_desc, 0, sizeof(buf_desc));
            buf_desc.u.buffer = apb;
            ioctl(s_voice.cap_fd, AUDIOIOC_ENQUEUEBUFFER,
                  (uintptr_t)&buf_desc);
            drained++;
        }
    }

    if (drained > 0) {
        syslog(LOG_INFO, "[%s] capture_drain: %d stale buffers re-enqueued\n",
            TAG, drained);
    }
    return drained;
}

static void capture_close(int fd)
{
    if (fd < 0) return;
    ioctl(fd, AUDIOIOC_STOP, 0);
    usleep(100000);  /* wait for HPWORK to release DMA buffers */

    if (s_voice.cap_mq != (mqd_t)-1) {
        ioctl(fd, AUDIOIOC_UNREGISTERMQ, (uintptr_t)s_voice.cap_mq);
        mq_close(s_voice.cap_mq);
        mq_unlink(s_voice.cap_mq_name);
        s_voice.cap_mq = (mqd_t)-1;
    }

    if (s_voice.cap_bufs) {
        for (int i = 0; i < s_voice.cap_nbufs; i++) {
            if (s_voice.cap_bufs[i]) {
                /* AUDIOIOC_FREEBUFFER expects a struct audio_buf_desc_s*,
                 * NOT a raw ap_buffer_s**. Passing &cap_bufs[i] directly
                 * makes audio_freebuffer read u.buffer from the wrong
                 * offset (past numbytes+padding), yielding cap_bufs[i+1]
                 * or an out-of-bounds NULL, which then dereferences
                 * upper->apbs[periods-1] (NULL) → LoadProhibited crash.
                 * Build a proper descriptor, mirroring nxrecorder. */
                struct audio_buf_desc_s buf_desc;
                memset(&buf_desc, 0, sizeof(buf_desc));
                buf_desc.u.buffer = s_voice.cap_bufs[i];
                ioctl(fd, AUDIOIOC_FREEBUFFER, (uintptr_t)&buf_desc);
            }
        }
        free(s_voice.cap_bufs);
        s_voice.cap_bufs = NULL;
    }

    ioctl(fd, AUDIOIOC_RELEASE, 0);
    close(fd);
}

/* ── Wake-word matching ────────────────────────────────────── */

/* Variant table for "你好" / "openvela" wake words.
 * ASR often mis-recognizes the non-dictionary word "openvea",
 * so common mis-recognition variants are all accepted. */
static const char* const WAKE_WORDS[] = {
    "你好",
    "hello",
    "哈喽",
    "openvela",
    "openvea",
    "openvila",
    "openwela",
    "openvella",
    "欧鹏vela",
    "opengvela",
};

/* Normalize ASR text (strip spaces/punctuation, lowercase ASCII)
 * then substring-match against the wake-word variant table. */
static bool wake_word_match(const char* asr_text)
{
    char norm[512];
    size_t w = 0;
    const unsigned char* r = (const unsigned char*)asr_text;

    while (*r && w < sizeof(norm) - 1) {
        if (*r < 0x80) {
            /* Keep only alphanumerics, lowercased; drop space/punct */
            if (isalnum(*r)) {
                norm[w++] = (char)tolower(*r);
            }
            r++;
            continue;
        }
        /* Skip common CJK/full-width punctuation (3-byte UTF-8):
         * U+3000-3FFF (0xE3 ..), U+FF00-FFEF (0xEF ..),
         * U+2000-206F general punct (0xE2 0x80 ..) */
        if ((r[0] == 0xE3 || r[0] == 0xEF
             || (r[0] == 0xE2 && r[1] == 0x80))
            && r[1] != '\0' && r[2] != '\0') {
            r += 3;
            continue;
        }
        norm[w++] = (char)*r++;
    }
    norm[w] = '\0';

    for (size_t i = 0; i < sizeof(WAKE_WORDS) / sizeof(WAKE_WORDS[0]); i++) {
        if (strstr(norm, WAKE_WORDS[i]) != NULL) {
            syslog(LOG_INFO, "[%s] wake word hit: \"%s\" (norm=\"%s\")\n", TAG,
                WAKE_WORDS[i], norm);
            return true;
        }
    }
    return false;
}

/* ── UI switch keyword matching ─────────────────────────────── */

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT

static const char* const SWITCH_TO_WATCH_KEYWORDS[] = {
    "切换成手表模式", "切换为手表模式", "切换手表模式",
    "切换手表", "手表模式", "打开手表",
    "switch to watch", "watch mode",
};

static bool ui_switch_to_watch_match(const char* asr_text)
{
    char norm[512];
    size_t w = 0;
    const unsigned char* r = (const unsigned char*)asr_text;

    while (*r && w < sizeof(norm) - 1) {
        if (*r < 0x80) {
            if (isalnum(*r)) norm[w++] = (char)tolower(*r);
            r++;
            continue;
        }
        /* Skip CJK/full-width punctuation (same as wake_word_match) */
        if ((r[0] == 0xE3 || r[0] == 0xEF
             || (r[0] == 0xE2 && r[1] == 0x80))
            && r[1] != '\0' && r[2] != '\0') {
            r += 3; continue;
        }
        norm[w++] = (char)*r++;
    }
    norm[w] = '\0';

    for (size_t i = 0;
         i < sizeof(SWITCH_TO_WATCH_KEYWORDS) / sizeof(SWITCH_TO_WATCH_KEYWORDS[0]);
         i++) {
        if (strstr(norm, SWITCH_TO_WATCH_KEYWORDS[i]) != NULL) {
            syslog(LOG_INFO, "[%s] UI switch keyword: \"%s\" (norm=\"%s\")\n",
                TAG, SWITCH_TO_WATCH_KEYWORDS[i], norm);
            return true;
        }
    }
    return false;
}

/* Runs on the LVGL thread (posted via lv_async_call):
 * ui_mode_switch_runtime() tears down the current UI and builds the
 * target UI, so it must not run on this voice conversation thread.
 * This is the same no-reboot path used by the BOOT-button long-press
 * and the settings page. */
static void ui_switch_to_watch_async(void* user_data)
{
    (void)user_data;
    int ret = ui_mode_switch_runtime(UI_MODE_WATCH,
        lv_display_get_screen_active(lv_display_get_default()));
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] runtime UI switch to watch failed: %d\n",
            TAG, ret);
    }
}

/* Consume an utterance that requests switching to watch mode.
 * Returns true when the text matched a switch keyword: the capture is
 * stopped and the runtime switch is posted to the LVGL thread (no
 * reboot, no LLM/TTS handling).  Caller keeps listening. */
static bool voice_ui_switch_consumed(const char* asr_text)
{
    if (!ui_switch_to_watch_match(asr_text)) {
        return false;
    }
    VOICE_DBG("UI switch to watch: %s", asr_text);
    capture_stop(s_voice.cap_fd);

    /* Stop the conversation loop NOW.  The runtime switch is posted to the
     * LVGL thread below, which will call launcher_stop_voice_for_watch() →
     * voice_channel_stop() and join this thread.  If we keep looping we race
     * it re-opening the capture device while the audio driver is torn down —
     * that race crashes the lpwork work-queue (LoadProhibited) in the
     * i2s rx duplex path.  Exiting here lets voice_channel_stop() join
     * cleanly instead. */
    s_voice.conv_running = false;

    lv_async_call(ui_switch_to_watch_async, NULL);
    return true;
}

#endif /* CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT */

/* ── Conversation thread ───────────────────────────────────── */

#define CONV_THREAD_STACK (24 * 1024)

/* Downmix stereo → mono: extract left channel from interleaved samples.
 * I2S HW requires 2ch, but volc ASR expects 1ch @ 16kHz/16bit.
 * Returns mono byte count (= stereo bytes / 2). */
static size_t downmix_stereo_to_mono(const uint8_t* stereo, size_t stereo_len,
    uint8_t* mono, size_t mono_cap)
{
    const int16_t* src = (const int16_t*)stereo;
    int16_t* dst = (int16_t*)mono;
    size_t frames = stereo_len / (2 * sizeof(int16_t)); /* L+R pairs */
    if (frames > mono_cap / sizeof(int16_t)) frames = mono_cap / sizeof(int16_t);
    for (size_t i = 0; i < frames; i++) {
        /* Average L+R — uses both mics (ES7210 ADC1+ADC2) */
        int32_t sum = (int32_t)src[i * 2] + (int32_t)src[i * 2 + 1];
        dst[i] = (int16_t)(sum / 2);
    }
    return frames * sizeof(int16_t);
}

/* Wait for the LLM+TTS round-trip of a conversation turn to complete,
 * then resume.  Shared by ASR turns and proactive-prompt turns.
 *
 * keep_wake_gate=true restores the wake gate to CLOSED after the turn
 * (proactive reminders fired while the device was in standby must not
 * leave the gate open); false keeps the gate open (normal ASR turns).
 * A 60s safety timeout recovers to LISTENING if the agent never speaks
 * back (LLM failure, dropped bus message) so the loop never hangs. */
static void conversation_turn_finish(bool keep_wake_gate)
{
    syslog(LOG_INFO, "[%s] waiting for LLM+TTS (state=%s)\n",
        TAG, state_name(s_voice.state));
    {
        int asr_pre_tried = 0;
        time_t wait_start = time(NULL);
        while (s_voice.conv_running
               && (s_voice.state == VOICE_PROCESSING
                   || s_voice.state == VOICE_SPEAKING)) {

            /* Safety net: agent never spoke back → recover. */
            if (time(NULL) - wait_start > 60) {
                syslog(LOG_WARNING,
                    "[%s] turn timeout, forcing LISTENING\n", TAG);
                s_voice.state = VOICE_LISTENING;
                break;
            }

            /* Pre-open ASR ~2s before playback ends.  Must run
             * in THIS thread (the later consumer of pre_stream),
             * not in speak(): fds don't cross task groups. */
            if (!asr_pre_tried
                && s_voice.state == VOICE_SPEAKING
                && s_voice.pre_stream == NULL) {
                uint32_t end_ms = s_voice.play_end_ms;
                if (end_ms != 0
                    && now_ms() + 2000 >= end_ms) {
                    asr_pre_tried = 1;
                    s_voice.pre_stream =
                        voice_asr_stream_open();
                    if (s_voice.pre_stream) {
                        VOICE_DBG("pre-opened ASR stream "
                            "(~2s before playback end)");
                    }
                }
            }
            usleep(200 * 1000);
        }
    }
    syslog(LOG_INFO, "[%s] TTS complete, state=%s\n",
        TAG, state_name(s_voice.state));

    /* Wake gate: proactive turns fired from standby restore the closed
     * gate (the 180s-idle state).  Normal turns force the gate open so
     * the conversation continues; resetting here also guards against
     * spurious external re-enables (e.g. phantom button events,
     * power-management callbacks) that raced the SPEAKING→LISTENING
     * transition. */
    if (keep_wake_gate) {
        s_voice.wake_gated = true;
        syslog(LOG_INFO,
            "[%s] proactive turn done, wake gate restored\n", TAG);
    } else if (s_voice.wake_gated) {
        syslog(LOG_WARNING,
            "[%s] wake gate was re-enabled during TTS, "
            "resetting for next turn\n", TAG);
        s_voice.wake_gated = false;
    }

    /* Resume: speaker acoustic coupling gap.
     * If ASR was pre-opened during TTS, shorter gap needed. */
    if (s_voice.conv_running && s_voice.state == VOICE_LISTENING) {
        if (s_voice.pre_stream) {
            VOICE_DBG("resuming: 100ms gap (ASR pre-opened)");
            usleep(100 * 1000);
        } else {
            usleep(400 * 1000);
        }
    }
}

static void* conversation_thread(void* arg)
{
    (void)arg;
    char asr_text[512];
    #define MONO_BUF_MAX 4096  /* enough for 8192-byte stereo → 4096 mono */
    uint8_t* mono_buf = malloc(MONO_BUF_MAX);
    if (!mono_buf) { return NULL; }
    time_t last_speech_time = 0;
    /* Loudest chunk average amplitude of the last streaming cycle:
     * printed when the idle timeout fires, so the silence/noise floor
     * vs VOICE_IDLE_VAD_THRESH can be read straight off the log. */
    uint32_t idle_noise_avg = 0;
    /* Idle lifecycle stage: 0 = dialog (companion budget counting),
     * 1 = standby face (wake gate closed), 2 = screen off (final).
     * Advanced by the idle checks below; reset to 0 by a wake word or
     * by real user speech. */
    int idle_stage = 0;
    int companion_count = 0;   /* companion turns already fired */
    time_t standby_since = 0;  /* entry timestamp of the standby stage */
    syslog(LOG_INFO, "[%s] conversation started\n", TAG);

    while (s_voice.conv_running && s_voice.state == VOICE_LISTENING) {

        /* ── Proactive prompt (battery reminder etc.) ─────────────
         * Consumed HERE, in this thread: an external speak() from the
         * agent task group flips LISTENING→SPEAKING, which would make
         * this loop exit below ("state != LISTENING") and permanently
         * kill the wake-word system.  Running the turn through this
         * thread reuses the same PROCESSING→SPEAKING→LISTENING
         * round-trip as a normal ASR turn.  Mic-muted (short press)
         * is respected: the prompt stays queued until unmuted. */
        {
            char *prompt = NULL;
            bool was_gated = false;

            pthread_mutex_lock(&s_voice.lock);
            if (s_voice.pending_prompt != NULL
                && s_voice.state == VOICE_LISTENING
                && !s_voice.mic_mute_target) {
                prompt = s_voice.pending_prompt;
                s_voice.pending_prompt = NULL;
                s_voice.state = VOICE_PROCESSING;
                was_gated = s_voice.wake_gated;
                /* A proactive turn counts as activity: resets the
                 * 180s idle timer. */
                last_speech_time = time(NULL);
            }
            pthread_mutex_unlock(&s_voice.lock);

            if (prompt != NULL) {
                VOICE_DBG("proactive prompt: %s", prompt);
                /* 不在此切换表情：注入回合保持当前脸，TTS 即将播放时
                 * 由 agent 层一次性切换 sick/proud，避免多段闪烁。 */
#ifdef CONFIG_AI_AGENT_VOLC_E2E
                /* Proactive turns have no uplink audio: the E2E server
                 * never synthesizes ChatTTSText for them (pop_pcm times
                 * out silently).  Force the classic WS TTS for the whole
                 * turn; conversation_turn_finish() below resets it. */
                volc_e2e_set_classic_tts(true);
#endif
                agent_msg_t msg;
                memset(&msg, 0, sizeof(msg));
                strncpy(msg.channel, AGENT_CHAN_VOICE,
                        sizeof(msg.channel) - 1);
                strncpy(msg.chat_id, "voice", sizeof(msg.chat_id) - 1);
                /* Proactive flag: the agent loop must not let the NL
                 * fast path hijack this prompt (its keywords — 电量/充电
                 * — would be answered without ever reaching the LLM). */
                msg.flags = AGENT_MSG_FLAG_PROACTIVE;
                msg.content = prompt;  /* ownership transfers to the bus */
                syslog(LOG_INFO, "[%s] proactive push: flags=%d\n",
                       TAG, msg.flags);
                if (message_bus_push_inbound(&msg) != OK) {
                    free(prompt);
                }

                /* Round-trip: wait for LLM+TTS, then restore the wake
                 * gate to its pre-turn state (standby stays standby). */
                conversation_turn_finish(was_gated);
#ifdef CONFIG_AI_AGENT_VOLC_E2E
                volc_e2e_set_classic_tts(false);
#endif
                continue;
            }
        }

        /* Idle lifecycle: dialog → companion turns → standby → screen off.
         * While the companion budget lasts, each idle window fires one
         * proactive companion turn (LLM-generated, content varies) and
         * restarts the window.  When the budget is exhausted, the next
         * expiry closes the wake gate and shows the standby face; after
         * a further VOICE_STANDBY_TIMEOUT_SEC the display goes off.
         * First turn and post-budget standby entry wait the full
         * VOICE_IDLE_TIMEOUT_SEC (3 min); turns 2..MAX fire every
         * VOICE_COMPANION_INTERVAL_SEC (1 min) while idle continues. */
        uint32_t idle_limit = (companion_count == 0
                               || companion_count >= VOICE_COMPANION_MAX)
                              ? VOICE_IDLE_TIMEOUT_SEC
                              : VOICE_COMPANION_INTERVAL_SEC;
        if (idle_stage == 0 && !s_voice.wake_gated
            && last_speech_time > 0
            && time(NULL) - last_speech_time > idle_limit) {
            if (companion_count < VOICE_COMPANION_MAX) {
                companion_count++;
                syslog(LOG_INFO, "[%s] %lus idle, companion turn %d/%d "
                       "(noise avg=%u thresh=%d)\n",
                       TAG, (unsigned long)idle_limit, companion_count,
                       VOICE_COMPANION_MAX, idle_noise_avg,
                       VOICE_IDLE_VAD_THRESH);
                /* Next window starts now; the proactive-prompt consumer
                 * at the loop top resets last_speech_time again once the
                 * turn starts (double reset is harmless). */
                last_speech_time = time(NULL);
                voice_channel_inject_prompt(
                    (companion_count == VOICE_COMPANION_MAX)
                        ? COMPANION_PROMPTS[1]   /* last: farewell */
                        : COMPANION_PROMPTS[0]);
                if (s_voice.idle_timeout_notify) {
                    s_voice.idle_timeout_notify(VOICE_IDLE_EVT_COMPANION);
                }
            } else {
                idle_stage = 1;
                standby_since = time(NULL);
                s_voice.wake_gated = true;
                syslog(LOG_INFO, "[%s] %d companion turns exhausted, "
                       "standby face + wake gate re-enabled\n",
                       TAG, VOICE_COMPANION_MAX);
                if (s_voice.idle_timeout_notify) {
                    s_voice.idle_timeout_notify(VOICE_IDLE_EVT_STANDBY);
                }
            }
        }
        if (idle_stage == 1
            && time(NULL) - standby_since > VOICE_STANDBY_TIMEOUT_SEC) {
            idle_stage = 2;
            syslog(LOG_INFO, "[%s] %ds standby without wake word, "
                   "display off (final stage)\n",
                   TAG, VOICE_STANDBY_TIMEOUT_SEC);
            if (s_voice.idle_timeout_notify) {
                s_voice.idle_timeout_notify(VOICE_IDLE_EVT_SCREEN_OFF);
            }
        }

        /* Re-open capture device if previous iteration closed it.
         * After AUDIOIOC_STOP, the NuttX audio upper layer enters
         * DRAINING/OPEN state and silently ignores subsequent
         * AUDIOIOC_START (lower->ops->start is never called).
         * The only reliable fix is to close and re-open the device,
         * which resets the upper layer state to PREPARED. */
        /* Process deferred mic mute request from button handler.
         * I2C writes to ES7210 must happen when the audio driver is
         * idle (cap_fd == -1), otherwise LVGL-timer → I2C races with
         * audio-driver → I2C and deadlocks the bus. */
        if (s_voice.mic_mute_pending) {
            s_voice.mic_mute_pending = false;
            es7210_set_mic_mute(s_voice.mic_mute_target);
            if (s_voice.mic_mute_target) {
                /* keep_alive session: with no uplink audio the server
                 * kills the dialog after ~10 minutes ("Abnormal
                 * silence audio").  Drop the E2E link up front — the
                 * next ASR stream open will reconnect cleanly.  Must
                 * run here (voice thread), not in the LVGL button
                 * handler. */
                volc_e2e_stop();
            }
            syslog(LOG_INFO, "[%s] mic mute applied: %d\n",
                TAG, s_voice.mic_mute_target);
        }

        /* When mic is muted by user (HW) or preset audio is playing:
         * pause capture→ASR cycle.  Audio playing through the speaker
         * would be picked up by the mic (echo loop), and the HW mute
         * keeps the sleepy face. */
        if (s_voice.mic_mute_target
#ifdef CONFIG_AI_AGENT_EMOTION_TOY
            || tool_emotion_audio_is_playing()
#endif
            ) {
            sleep(1);
            continue;
        }

        if (s_voice.cap_fd < 0) {
            s_voice.cap_fd = capture_open();
            if (s_voice.cap_fd < 0) {
                syslog(LOG_ERR, "[%s] capture re-open failed: %d\n",
                    TAG, s_voice.cap_fd);
                sleep(1);
                continue;
            }
            syslog(LOG_INFO, "[%s] capture re-opened: %d buffers\n",
                TAG, s_voice.cap_nbufs);
        }

        /* 1. Start capture FIRST to avoid missing audio during ASR
         *    TLS handshake (~1-2s). Audio buffers queue up in the
         *    driver while we open the ASR stream. */
        capture_drain();
        capture_start(s_voice.cap_fd);
        VOICE_DBG("capture started, opening ASR...");

        /* Bail out early if a proactive prompt was queued during the
         * capture (re-)open phase.  Opening the ASR stream below may
         * block for several seconds on an E2E reconnect; consuming the
         * prompt NOW avoids that delay entirely. */
        if (s_voice.pending_prompt != NULL) {
            capture_stop(s_voice.cap_fd);
            capture_close(s_voice.cap_fd);
            s_voice.cap_fd = -1;
            continue;
        }

        /* Face → listening as soon as capture restarts, BEFORE the
         * ASR open below.  That open blocks for seconds on an E2E
         * reconnect (or fails repeatedly while the network is down);
         * setting the face only after a successful open (the old
         * placement, further below) left the toy stuck on "speaking"
         * for the whole retry window after a wake-word turn.
         *
         *
         * Standby / screen-off stages are exempt: the 15s re-open
         * loop must not stomp the standby face — it stays until the
         * wake word, then the wake path restores thinking → speaking
         * → listening normally.  NOTE the guard is idle_stage, NOT
         * wake_gated: boot starts wake-gated too (auto mode waits
         * for the first wake word), and the toy must still show the
         * listening face then — gating on wake_gated froze the face
         * on the boot default and the toy never showed "listening". */
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
        if (idle_stage == 0) {
            watch_expression_page_set_face("listening", 0);
        }
#endif

        /* Open ASR stream (blocking TLS + WS handshake).
         * Prefer pre_stream if available (pre-opened during TTS). */
        voice_asr_stream_t* stream = s_voice.pre_stream;
        s_voice.pre_stream = NULL;
        if (!stream) {
            stream = voice_asr_stream_open();
        }

        /* Count consecutive ASR open failures as a connectivity proxy.
         * network_is_connected() is unreliable — WiFi keeps its IP
         * even when internet is unreachable.  3 failures → alert. */
        static int s_asr_fail_count = 0;

        if (stream) {
            VOICE_DBG("ASR stream ready");
            s_asr_fail_count = 0;   /* reset on success */
        } else {
            s_asr_fail_count++;
            syslog(LOG_WARNING, "[%s] ASR open failed (%d consecutive)\n",
                TAG, s_asr_fail_count);
            if (s_asr_fail_count >= 3 && last_speech_time > 0) {
                syslog(LOG_ERR, "[%s] WiFi appears down, playing alert\n", TAG);
                tool_system_alert_play(17, 0);
                s_asr_fail_count = 0;
            }
            VOICE_DBG("ASR open failed, retry in 2s");
            capture_stop(s_voice.cap_fd);
            sleep(2);
            continue;
        }

        asr_text[0] = '\0';
        bool got_final = false;

        /* Skip the very first frame after cold boot (ADC power-on spike). */
        static bool s_first_utterance = true;

        time_t start = time(NULL);
        int frame_count = 0;
        int voice_frames = 0;   /* chunks with speech-level energy */
        uint32_t peak_avg = 0;  /* loudest chunk average amplitude */
        VOICE_DBG("capture started (drain done), listening...");

        /* Stream ALL audio to server — no client-side VAD.
         * The Volcengine ASR server does its own ML-based endpoint
         * detection and returns final results via try_recv.
         * A queued proactive prompt (fall help / battery reminder)
         * breaks the loop immediately: with nobody speaking the
         * server VAD never fires and the 15s safety timeout below
         * would otherwise delay the prompt by up to 15s. */
        while (s_voice.conv_running && s_voice.state == VOICE_LISTENING
               && !got_final && !s_voice.mic_mute_target
               && s_voice.pending_prompt == NULL) {
            struct ap_buffer_s* apb = NULL;
            const uint8_t* pcm = NULL;
            size_t pcm_len = 0;

            /* Throttled WiFi link check (every 5s).  Only alerts during
             * an active conversation (after wake word); while wake-gated
             * the device is idle and the boot-time no-network alert (16)
             * already covers the offline case. */
            time_t now = time(NULL);
            if (now - s_link_chk_time >= 5) {
                s_link_chk_time = now;
                if (wifi_link_up()) {
                    s_link_alerted = false;
                } else if (!s_voice.wake_gated && !s_link_alerted) {
                    s_link_alerted = true;
                    syslog(LOG_WARNING,
                        "[%s] WiFi link down during conversation\n", TAG);
                    tool_system_alert_play(17, 0); /* async — non-blocking */
                }
            }

            int n = capture_read_frame(&apb, &pcm, &pcm_len);
            if (n == -ETIMEDOUT) {
                VOICE_DBG("timeout after %d frames", frame_count);
                break;
            }
            if (n < 0) { VOICE_DBG("capture_read err=%d at frame %d", n, frame_count); break; }

            /* Skip ADC power-on spike on very first frame only */
            if (s_first_utterance) {
                s_first_utterance = false;
                capture_release_frame(apb);
                continue;
            }

            /* Downmix stereo → mono */
            size_t mono_len = downmix_stereo_to_mono(pcm, pcm_len,
                mono_buf, MONO_BUF_MAX);

            /* Energy VAD (idle timer only): average absolute amplitude
             * of this chunk.  Silence must NOT renew the idle timer —
             * see the reset decision after the streaming loop. */
            {
                const int16_t* s16 = (const int16_t*)mono_buf;
                size_t ns = mono_len / sizeof(int16_t);
                if (ns > 0) {
                    int64_t acc = 0;
                    for (size_t i = 0; i < ns; i++) {
                        acc += s16[i] < 0 ? -(int32_t)s16[i]
                                          :  (int32_t)s16[i];
                    }
                    uint32_t avg = (uint32_t)(acc / (int64_t)ns);
                    if (avg > peak_avg) {
                        peak_avg = avg;
                    }
                    if (avg >= VOICE_IDLE_VAD_THRESH) {
                        voice_frames++;
                    }
                }
            }

            /* Send frame to ASR — server VAD decides speech/noise */
            int sret = voice_asr_stream_send(stream, mono_buf, mono_len);
            capture_release_frame(apb);
            frame_count++;

            if (sret < 0) {
                syslog(LOG_WARNING, "[%s] stream send error: %d, "
                    "restarting stream\n", TAG, sret);
                break; /* link dead — outer loop re-opens the stream */
            }

            /* Non-blocking: server sends final when its VAD
             * detects end of utterance */
            int recv_ret = voice_asr_stream_try_recv(stream, asr_text,
                sizeof(asr_text));
            if (recv_ret == 1) {
                VOICE_DBG("server final: %s", asr_text);
                got_final = true;
            } else if (recv_ret < 0) {
                syslog(LOG_WARNING, "[%s] try_recv error: %d, "
                    "restarting stream\n", TAG, recv_ret);
                break; /* stream dead — outer loop re-opens it */
            }

            /* Safety timeout */
            if (!got_final && (time(NULL) - start) >= 15) {
                VOICE_DBG("timeout after %d frames, %lds elapsed",
                    frame_count, (long)(time(NULL) - start));
                break;
            }
        }

        VOICE_DBG("loop done: got_final=%d frames=%d",
            got_final, frame_count);

        /* Reset idle timer only on real speech energy (or a recognized
         * utterance).  The previous "frame_count > 0" reset renewed the
         * timer with silence: in E2E mode the mic streams continuously,
         * so every ~15s cycle had frames even in an empty room and the
         * idle standby could never trigger (logs showed endless
         * "capture re-opened" with no "Nds idle" line).  Energy-based
         * reset keeps the original protection — a WS drop mid-speech
         * still resets, because speech chunks carry energy — while pure
         * silence lets the timer run out. */
        idle_noise_avg = peak_avg;
        if (voice_frames > 0 || got_final) {
            last_speech_time = time(NULL);
        }

        /* Get result.  When a proactive prompt broke the streaming loop
         * above, abort the stream instead of finishing it: nobody spoke,
         * so blocking on the server's final result would only add
         * latency before the prompt is consumed at the loop top. */
        if (got_final || s_voice.pending_prompt != NULL) {
            voice_asr_stream_abort(stream);
        } else {
            int ret = voice_asr_stream_finish(stream, asr_text,
                sizeof(asr_text));
            if (ret != 0 || asr_text[0] == '\0') {
                asr_text[0] = '\0';
            }
        }

        if (!s_voice.conv_running || s_voice.state != VOICE_LISTENING) {
            break;
        }

        /* 3a. Wake gate: before wake-up, ASR text is only used for
         * keyword matching and is never pushed to the LLM. */
        if (asr_text[0] != '\0' && s_voice.wake_gated) {
            VOICE_DBG("gate ASR: %s", asr_text);
            last_speech_time = time(NULL); /* any speech resets idle timer */
            capture_stop(s_voice.cap_fd);

            bool wake_hit = wake_word_match(asr_text);
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
            /* UI switch keyword — user may say wake+switch together.
             * Consume the utterance: runtime switch is posted to the
             * LVGL thread (no reboot); skip the wake handling below,
             * keep the gate on and keep listening (same end state as
             * the button-triggered runtime switch). */
            if (wake_hit && voice_ui_switch_consumed(asr_text)) {
                wake_hit = false;
            }
#endif
            if (wake_hit) {
                VOICE_DBG("wake word detected: %s", asr_text);
                VOICE_DBG("state: LISTENING → PROCESSING (wake)");
                s_voice.state = VOICE_PROCESSING;
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
                watch_expression_page_set_face("thinking", 0);
#endif
                s_voice.wake_gated = false;
                last_speech_time = time(NULL);  /* reset idle timer */
                /* Waking from standby/screen-off restarts the whole
                 * idle lifecycle: fresh companion budget, dialog stage. */
                idle_stage = 0;
                companion_count = 0;
                if (s_voice.wake_notify) {
                    s_voice.wake_notify();
                }

                /* speak() flips PROCESSING→SPEAKING, and resets to
                 * LISTENING when playback ends (all branches). */
#ifdef CONFIG_AI_AGENT_VOLC_E2E
                /* 唤醒固定输出「我在，请讲」，不走 E2E 自聊。
                 * route_agent 丢弃服务器自聊音频、停止自聊，并把 hybrid
                 * 切到 AGENT 路径，speak() 用经典 volc_tts_ws 合成固定
                 * 文案。返回的服务器缓存回复不再使用，直接释放。 */
                if (volc_e2e_hybrid_enabled()) {
                    char *e2e_reply = volc_e2e_hybrid_route_agent();
                    free(e2e_reply);
                }
#endif
                voice_channel_speak("我在，请讲");

                /* Safety net: guarantee LISTENING terminal state */
                if (s_voice.conv_running
                    && s_voice.state != VOICE_LISTENING) {
                    s_voice.state = VOICE_LISTENING;
                }
                if (s_voice.conv_running) {
                    usleep(200 * 1000);
                }
            } else {
                VOICE_DBG("wake gate: no keyword, drop");
            }
            /* Fall through to capture_close below; outer loop
             * re-opens device + ASR stream and keeps listening. */
        }
        /* 3. Got text → LLM + TTS.
         * A UI-switch utterance is consumed by voice_ui_switch_consumed()
         * (runtime switch posted to the LVGL thread — no reboot) and
         * falls through to the no-speech else below: a harmless extra
         * capture_stop, then the outer loop re-opens capture and keeps
         * listening. */
        else if (asr_text[0] != '\0'
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
                 && !voice_ui_switch_consumed(asr_text)
#endif
                 ) {

            VOICE_DBG("ASR: %s", asr_text);

            VOICE_DBG("state: LISTENING → PROCESSING");
            s_voice.state = VOICE_PROCESSING;
            last_speech_time = time(NULL);
            /* Real user interaction: restart the companion budget so a
             * long conversation does not accumulate towards standby. */
            companion_count = 0;
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
            watch_expression_page_set_face("thinking", 0);
#endif
            capture_stop(s_voice.cap_fd);

            /* NOTE: TTS preopen moved to agent_loop (agent task group).
             * This thread is created from the launcher task's LVGL timer,
             * so it lives in a DIFFERENT task group than speak(); NuttX
             * fd tables are per task group, so a socket opened here is
             * invalid (EBADF) or aliased (-0x7180) over there. */

            agent_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            strncpy(msg.channel, AGENT_CHAN_VOICE, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, "voice", sizeof(msg.chat_id) - 1);
            msg.content = strdup(asr_text);
            if (msg.content) message_bus_push_inbound(&msg);

            /* Wait for TTS to finish, restore wake gate, resume */
            conversation_turn_finish(false);

            /* Let outer loop re-open stream + start capture */
        } else {
            /* No speech detected — stop capture for clean restart */
            VOICE_DBG("no speech, stopping capture");
            capture_stop(s_voice.cap_fd);
        }

        /* Close capture device to reset upper layer state.
         * After AUDIOIOC_STOP, audio_start() silently ignores
         * AUDIOIOC_START because the state is DRAINING/OPEN.
         * Close+reopen resets to PREPARED, making START work. */
        capture_close(s_voice.cap_fd);
        s_voice.cap_fd = -1;
    }

    capture_close(s_voice.cap_fd);
    s_voice.cap_fd = -1;

    pthread_mutex_lock(&s_voice.lock);
    s_voice.state = VOICE_IDLE;
    /* Drop any queued proactive prompt — nobody will consume it now. */
    free(s_voice.pending_prompt);
    s_voice.pending_prompt = NULL;
    pthread_mutex_unlock(&s_voice.lock);

    syslog(LOG_INFO, "[%s] conversation exit\n", TAG);
    free(mono_buf);
    return NULL;
}

/* ── voice_channel_start / stop ────────────────────────────── */

static int voice_start_common(void)
{
    pthread_mutex_lock(&s_voice.lock);

    /* Already in conversation — treat as "interrupt and restart listening" */
    if (s_voice.state == VOICE_SPEAKING || s_voice.state == VOICE_PROCESSING) {
        syslog(LOG_INFO, "[%s] interrupting TTS, restarting listen\n", TAG);
        s_voice.tts_abort = 1;
        if (s_voice.tts_pb) {
            audio_playback_stop(s_voice.tts_pb);
        }
        s_voice.state = VOICE_LISTENING;
        /* capture_start happens in conversation_thread outer loop */
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_INFO, "[%s] listening...\n", TAG);
        return 0;
    }

    if (s_voice.state != VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_WARNING, "[%s] already active (state=%d)\n",
            TAG, s_voice.state);
        return -EBUSY;
    }

    /* Open capture device */
    int fd = capture_open();
    if (fd < 0) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_ERR, "[%s] capture open failed: %d\n", TAG, fd);
        return fd;
    }
    s_voice.cap_fd = fd;

    /* capture_start deferred to conversation_thread —
     * it opens ASR stream first, then starts capture. */

    s_voice.state = VOICE_LISTENING;
    s_voice.conv_running = true;
    pthread_mutex_unlock(&s_voice.lock);

    /* Spawn conversation thread */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, CONV_THREAD_STACK);
    int ret = pthread_create(&s_voice.conv_thread, &attr,
        conversation_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        pthread_mutex_lock(&s_voice.lock);
        s_voice.state = VOICE_IDLE;
        s_voice.conv_running = false;
        capture_close(s_voice.cap_fd);
        s_voice.cap_fd = -1;
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_ERR, "[%s] pthread_create failed: %d\n", TAG, ret);
        return -ret;
    }

    syslog(LOG_INFO, "[%s] listening (voice_stop to exit)\n", TAG);
    return 0;
}

int voice_channel_start(void)
{
    /* Manual start while wake-gated: just release the gate so the
     * next utterance goes straight to the LLM (voice_start effect). */
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state == VOICE_LISTENING && s_voice.wake_gated) {
        s_voice.wake_gated = false;
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_INFO, "[%s] wake gate released, listening\n", TAG);
        return 0;
    }
    pthread_mutex_unlock(&s_voice.lock);

    s_voice.wake_gated = false;
    return voice_start_common();
}

int voice_channel_start_wake(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state != VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        syslog(LOG_WARNING, "[%s] start_wake: already active (state=%s)\n",
            TAG, state_name(s_voice.state));
        return -EBUSY;
    }
    s_voice.wake_gated = true;
    pthread_mutex_unlock(&s_voice.lock);

    int ret = voice_start_common();
    if (ret != 0) {
        s_voice.wake_gated = false;
    } else {
        VOICE_DBG("wake-word listening (say 你好 / openvela)");
    }
    return ret;
}

void voice_channel_set_wake_notify(void (*cb)(void))
{
    s_voice.wake_notify = cb;
}

void voice_channel_set_idle_event_cb(void (*cb)(int event))
{
    s_voice.idle_timeout_notify = cb;
}

bool voice_channel_is_busy(void)
{
    /* Busy = a voice conversation is actively running.  Idle listening
     * (LISTENING, wake-gated or not) is not busy — an async speak can
     * interrupt it, so proactive prompts may proceed. */
    pthread_mutex_lock(&s_voice.lock);
    bool busy = (s_voice.state == VOICE_PROCESSING
                 || s_voice.state == VOICE_SPEAKING);
    pthread_mutex_unlock(&s_voice.lock);
    return busy;
}

bool voice_channel_is_listening(void)
{
    /* LISTENING = wake-word system fully up and idle.  VOICE_IDLE
     * means the voice system has not started yet (boot) or has been
     * stopped; speaking then would race the initial capture/session
     * setup and corrupt the state machine (observed: wake stuck on
     * "thinking" face after a boot-time async speak). */
    pthread_mutex_lock(&s_voice.lock);
    bool listening = (s_voice.state == VOICE_LISTENING);
    pthread_mutex_unlock(&s_voice.lock);
    return listening;
}

int voice_channel_inject_prompt(const char* text)
{
    if (!text || text[0] == '\0') {
        return -EINVAL;
    }

    /* strdup before taking the lock — no malloc under mutex. */
    char* fresh = strdup(text);
    if (!fresh) {
        return -ENOMEM;
    }

    pthread_mutex_lock(&s_voice.lock);
    char* old = s_voice.pending_prompt;
    s_voice.pending_prompt = fresh;
    pthread_mutex_unlock(&s_voice.lock);

    /* A newer prompt replaces a not-yet-consumed one. */
    free(old);

    syslog(LOG_INFO, "[%s] proactive prompt queued: %.40s\n",
        TAG, text);
    return 0;
}

bool voice_channel_is_mic_muted(void)
{
    return s_voice.mic_mute_target;
}

bool voice_channel_is_wake_gated(void)
{
    return s_voice.wake_gated;
}

int voice_channel_stop(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state == VOICE_IDLE) {
        pthread_mutex_unlock(&s_voice.lock);
        return -EINVAL;
    }

    syslog(LOG_INFO, "[%s] stopping conversation (state=%d)\n",
        TAG, s_voice.state);

    s_voice.conv_running = false;
    s_voice.state = VOICE_IDLE;

    /* Abort TTS if active */
    s_voice.tts_abort = 1;
    if (s_voice.tts_pb) {
        audio_playback_stop(s_voice.tts_pb);
    }

    /* Close capture to unblock read() in conversation thread */
    if (s_voice.cap_fd >= 0) {
        capture_close(s_voice.cap_fd);
        s_voice.cap_fd = -1;
    }
    pthread_mutex_unlock(&s_voice.lock);

    /* Wait for conversation thread to finish */
    pthread_join(s_voice.conv_thread, NULL);

    syslog(LOG_INFO, "[%s] stopped\n", TAG);
    return 0;
}

int voice_channel_stop_with_text(char* text_out, size_t text_cap)
{
    /* Streaming mode: stop conversation, no WAV file to read.
     * Caller should use voice_channel_stop() and handle result
     * via the message bus voice channel. */
    (void)text_out;
    (void)text_cap;
    return voice_channel_stop();
}

/* Strip Markdown formatting that TTS would read aloud.
 * Removes: **bold**, *italic*, # headings, - list bullets.
 * Operates in-place, result is always <= original length. */
static void tts_strip_markdown(char* s)
{
    char* r = s; /* read pointer */
    char* w = s; /* write pointer */

    while (*r) {
        /* Skip ** (bold markers) */
        if (r[0] == '*' && r[1] == '*') {
            r += 2;
            continue;
        }

        /* Skip lone * (italic markers) \xe2\x80\x94 but keep * in
         * contexts like "3*4" (digit before and after) */
        if (r[0] == '*') {
            if ((r == s || !isdigit((unsigned char)r[-1]))
                || !isdigit((unsigned char)r[1])) {
                r++;
                continue;
            }
        }

        /* Skip # at line start (headings) */
        if (r[0] == '#' && (r == s || r[-1] == '\n')) {
            while (*r == '#' || *r == ' ') {
                r++;
            }
            continue;
        }

        /* Skip "- " at line start (list bullets) */
        if (r[0] == '-' && r[1] == ' '
            && (r == s || r[-1] == '\n')) {
            r += 2;
            continue;
        }

        *w++ = *r++;
    }

    *w = '\0';
}

int voice_channel_speak(const char* text)
{
    syslog(LOG_INFO, "[%s] speak\n", TAG);
    if (!text || text[0] == '\0') {
        syslog(LOG_ERR, "[%s] speak: empty/null text\n", TAG);
        /* Must still transition PROCESSING→LISTENING, otherwise
         * conversation_thread waits forever. */
        if (s_voice.state == VOICE_PROCESSING) {
            s_voice.state = VOICE_LISTENING;
        }
        return -EINVAL;
    }

    /* If a preset audio is already playing, don't interrupt it with TTS.
     * Audio and TTS share /dev/audio/pcm0 — audio takes priority.
     * Check BEFORE acquiring any voice lock to avoid deadlock. */
#ifdef CONFIG_AI_AGENT_EMOTION_TOY
    if (tool_emotion_audio_is_playing()) {
        syslog(LOG_INFO, "[%s] audio playing, skipping TTS\n", TAG);
        if (s_voice.state == VOICE_PROCESSING) {
            s_voice.state = VOICE_LISTENING;
        }
        return 0;
    }
#endif

    /* ── Serialize concurrent speak calls ── */
    /* If another thread is already speaking, abort its TTS playback
     * so we don't overlap media_player sessions (which causes the
     * media framework to attempt a ~1.3GB allocation and crash). */
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.tts_pb) {
        syslog(LOG_INFO, "[%s] speak: aborting previous TTS\n", TAG);
        s_voice.tts_abort = 1;
        audio_playback_stop(s_voice.tts_pb);
    }

    /* Half-duplex: mark speaker active so conversation thread pauses.
     * LISTENING → SPEAKING also interrupts an idle-listen so async speaks
     * (cron reminders, emotion prompts) actually play. */
    if (s_voice.state == VOICE_PROCESSING
        || s_voice.state == VOICE_LISTENING) {
        VOICE_DBG("state: %s → SPEAKING", state_name(s_voice.state));
        s_voice.state = VOICE_SPEAKING;
        /* speaking face deferred — set when playback actually starts */
    }
    pthread_mutex_unlock(&s_voice.lock);

    pthread_mutex_lock(&s_voice.speak_lock);

    /* Only reject TTS when mic is actively recording.
     * VOICE_PROCESSING / VOICE_SPEAKING / VOICE_IDLE are fine. */
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state == VOICE_LISTENING) {
        pthread_mutex_unlock(&s_voice.lock);
        pthread_mutex_unlock(&s_voice.speak_lock);
        syslog(LOG_INFO,
            "[%s] speak: skipped, recording active\n", TAG);
        return -EBUSY;
    }
    pthread_mutex_unlock(&s_voice.lock);

    /* Work on a mutable copy so we can strip markdown in-place */
    size_t text_len = strlen(text);
    char* clean = malloc(text_len + 1);

    if (!clean) {
        if (s_voice.state == VOICE_SPEAKING) s_voice.state = VOICE_LISTENING;
        pthread_mutex_unlock(&s_voice.speak_lock);
        return -ENOMEM;
    }

    memcpy(clean, text, text_len + 1);
    tts_strip_markdown(clean);

    /* If stripping left nothing, bail out */
    if (clean[0] == '\0') {
        free(clean);
        if (s_voice.state == VOICE_SPEAKING) s_voice.state = VOICE_LISTENING;
        pthread_mutex_unlock(&s_voice.speak_lock);
        return 0;
    }

    /* Hard cap on TTS text length.  tmpfs file data lives in the
     * dedicated FS heap (CONFIG_FS_HEAPSIZE, now 2MB); a reply of
     * ~300 bytes UTF-8 needs ~1.5MB of WAV, which fits with margin.
     * Longer output is cut at the last sentence-ending punctuation
     * so the reply still ends naturally (fallback: clause boundary,
     * then char boundary). */
    #define TTS_TEXT_MAX 300
    {
        size_t len = strlen(clean);
        if (len > TTS_TEXT_MAX) {
            size_t i = 0, last_sent = 0, last_clause = 0, last_cb = 0;
            while (i < len) {
                unsigned char c = (unsigned char)clean[i];
                size_t cl = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3
                          : (c >= 0xC0) ? 2 : 1;
                if (i + cl > TTS_TEXT_MAX) break;
                if (cl == 3) {
                    const unsigned char* p =
                        (const unsigned char*)clean + i;
                    if ((p[0]==0xE3 && p[1]==0x80 && p[2]==0x82) /* 。 */
                        || (p[0]==0xEF && p[1]==0xBC
                            && (p[2]==0x81 || p[2]==0x9F))) {   /* ！？ */
                        last_sent = i + cl;
                    } else if ((p[0]==0xEF && p[1]==0xBC
                            && (p[2]==0x8C || p[2]==0x9B))      /* ，； */
                        || (p[0]==0xE3 && p[1]==0x80
                            && p[2]==0x81)) {                   /* 、 */
                        last_clause = i + cl;
                    }
                } else if (cl == 1) {
                    if (c=='.' || c=='!' || c=='?') last_sent = i + cl;
                    else if (c==',' || c==';') last_clause = i + cl;
                }
                i += cl;
                last_cb = i;
            }
            /* Prefer a sentence end, but only if it keeps at least
             * half of the budget; an early "。" followed by a long
             * "，、"-separated list would otherwise drop most of the
             * reply.  Fall back to the last clause boundary. */
            size_t cut = last_sent;
            if (cut < TTS_TEXT_MAX / 2 && last_clause > cut) {
                cut = last_clause;
            }
            if (cut == 0) {
                cut = last_cb;
            }
            clean[cut] = '\0';
            syslog(LOG_WARNING,
                "[%s] TTS text capped: %zu→%zu bytes\n",
                TAG, len, cut);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &s_tts_start);
    s_tts_first_chunk = 0;

    if (s_asr_done_ts.tv_sec > 0) {
        long llm_ms = (s_tts_start.tv_sec - s_asr_done_ts.tv_sec) * 1000
            + (s_tts_start.tv_nsec - s_asr_done_ts.tv_nsec) / 1000000;
        syslog(LOG_INFO, "[%s] LLM latency: %ldms\n", TAG, llm_ms);
    }

    /* Streaming TTS synthesis + nxplayer playback.
     * Uses WebSocket V3 TTS for fast synthesis, then plays
     * the complete PCM via nxplayer + WAV file. */

    /* Ensure a streaming TTS backend is active (may have fallen back to
     * the HTTP one).  Named backends that provide their own streaming
     * path — volc_e2e, mimo — must not be clobbered here. */
    const char* tts_be = voice_tts_get_backend();
    if (tts_be && strcmp(tts_be, "volcengine_ws") != 0
        && strcmp(tts_be, AGENT_VOICE_BACKEND_E2E) != 0
        && strncmp(tts_be, "mimo", 4) != 0) {
        int rc = voice_tts_set_backend("volcengine_ws");
        syslog(LOG_INFO, "[%s] TTS backend switch: %s (rc=%d)\n",
            TAG, voice_tts_get_backend() ? voice_tts_get_backend() : "none", rc);
    }

    /* Guard against a half-switched pipeline (e.g. set_voice_tts mimo
     * while ASR still runs on the E2E session). */
    {
        const char* asr_be = voice_asr_get_backend();
        const char* cur_tts = voice_tts_get_backend();
        if (asr_be && cur_tts
            && (strcmp(asr_be, AGENT_VOICE_BACKEND_E2E) == 0)
                != (strcmp(cur_tts, AGENT_VOICE_BACKEND_E2E) == 0)) {
            syslog(LOG_WARNING, "[%s] mixed voice backends: asr=%s tts=%s\n",
                TAG, asr_be, cur_tts);
        }
    }

    syslog(LOG_INFO, "[%s] TTS backend: %s\n",
        TAG, voice_tts_get_backend() ? voice_tts_get_backend() : "none");

    /* Batch TTS synthesis: collect all PCM into a 2MB buffer, then
     * expand mono→stereo in-place and write a complete WAV file for
     * nxplayer playback.  (The previous streaming-to-file approach
     * caused stuttering and occasional freezes on device.) */
    #define TTS_BUF_CAP (2 * 1024 * 1024)
    unsigned char* pcm = malloc(TTS_BUF_CAP);
    if (!pcm) {
        free(clean);
        if (s_voice.state == VOICE_SPEAKING) s_voice.state = VOICE_LISTENING;
        pthread_mutex_unlock(&s_voice.speak_lock);
        return -ENOMEM;
    }

    /* cap = TTS_BUF_CAP / 2: leave room for in-place mono→stereo
     * expansion (mono samples are shifted backward into stereo frames). */
    tts_collect_ctx_t collect = {
        .buf = pcm, .cap = TTS_BUF_CAP / 2, .len = 0, .err = 0
    };

    int ret = voice_tts_speak_stream(clean, tts_collect_cb, &collect);
    free(clean);

    if (ret != 0 || collect.err != 0 || collect.len == 0) {
        syslog(LOG_ERR, "[%s] TTS failed: ret=%d err=%d len=%zu\n",
            TAG, ret, collect.err, collect.len);
        free(pcm);
        if (s_voice.state == VOICE_SPEAKING) s_voice.state = VOICE_LISTENING;
        pthread_mutex_unlock(&s_voice.speak_lock);
        return ret != 0 ? ret : (collect.err != 0 ? collect.err : -ENODATA);
    }

    size_t pcm_len = collect.len;

    /* Expand mono → stereo in-place (backward pass to avoid
     * overwriting unread source samples).  The buffer was allocated
     * at 2MB so the doubled data fits. */
    {
        int16_t* samples = (int16_t*)pcm;
        size_t mono_samples = pcm_len / sizeof(int16_t);
        for (size_t i = mono_samples; i > 0; i--) {
            int16_t s = samples[i - 1];
            int16_t* frame = (int16_t*)(pcm + (i - 1) * 2 * sizeof(int16_t));
            frame[0] = s;
            frame[1] = s;
        }
        pcm_len = mono_samples * 2 * sizeof(int16_t);
    }

    /* Write WAV to tmpfs in one shot (header + complete PCM) */
    #define TTS_WAV_PATH "/tmp/tts_out.wav"
    #define WAV_HDR_SIZE 44
    uint32_t sr = (uint32_t)AGENT_TTS_WS_SAMPLE_RATE;
    uint16_t ch = 2, bits = 16;
    uint32_t byte_rate = sr * ch * (bits / 8);
    {
        uint16_t block_align = ch * (bits / 8);
        uint32_t data_size = (uint32_t)pcm_len;
        uint32_t riff_size = data_size + 36;
        unsigned char hdr[WAV_HDR_SIZE];
        memcpy(hdr, "RIFF", 4);
        hdr[4] = riff_size & 0xff; hdr[5] = (riff_size>>8)&0xff;
        hdr[6] = (riff_size>>16)&0xff; hdr[7] = (riff_size>>24)&0xff;
        memcpy(hdr+8, "WAVE", 4);
        memcpy(hdr+12, "fmt ", 4);
        hdr[16]=16; hdr[17]=0; hdr[18]=0; hdr[19]=0;
        hdr[20]=1; hdr[21]=0;
        hdr[22]=ch; hdr[23]=0;
        hdr[24]=sr&0xff; hdr[25]=(sr>>8)&0xff;
        hdr[26]=(sr>>16)&0xff; hdr[27]=(sr>>24)&0xff;
        memcpy(hdr+28, &byte_rate, 4);
        hdr[32]=block_align; hdr[33]=0;
        hdr[34]=bits; hdr[35]=0;
        memcpy(hdr+36, "data", 4);
        hdr[40]=data_size&0xff; hdr[41]=(data_size>>8)&0xff;
        hdr[42]=(data_size>>16)&0xff; hdr[43]=(data_size>>24)&0xff;

        int fd = open(TTS_WAV_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) {
            syslog(LOG_ERR, "[%s] TTS wav open failed: errno=%d\n",
                TAG, errno);
            free(pcm);
            if (s_voice.state == VOICE_SPEAKING) s_voice.state = VOICE_LISTENING;
            pthread_mutex_unlock(&s_voice.speak_lock);
            return -ENOENT;
        }
        write(fd, hdr, WAV_HDR_SIZE);
        write(fd, pcm, pcm_len);
        close(fd);
        sync();
        free(pcm);

        syslog(LOG_INFO, "[%s] WAV written: %zu bytes "
            "(hdr=%d + pcm=%zu, sr=%lu ch=%d bits=%d)\n",
            TAG, (size_t)(WAV_HDR_SIZE + pcm_len), WAV_HDR_SIZE,
            pcm_len, (unsigned long)sr, ch, bits);
    }

    {
        FAR struct nxplayer_s *pl = nxplayer_create();
        if (!pl) {
            syslog(LOG_ERR, "[%s] nxplayer_create failed\n", TAG);
        } else {
            nxplayer_setdevice(pl, "/dev/audio/pcm0");

            /* Sync nxplayer volume to the system volume set by agent/UI.
             * Without this, nxplayer_playraw() internally resets volume
             * to its hardcoded default (400), overwriting the user's
             * choice every time TTS plays. */
            int sys_vol = watch_volume_get_value();
            syslog(LOG_INFO, "[%s] TTS volume sync: sys_vol=%d\n",
                TAG, sys_vol);
            if (sys_vol >= 0) {
                nxplayer_setvolume(pl, (uint16_t)sys_vol);
            } else {
                syslog(LOG_WARNING, "[%s] TTS volume sync FAILED, "
                    "using nxplayer default\n", TAG);
            }

            syslog(LOG_INFO, "[%s] nxplayer: starting playback "
                "(sr=%lu ch=%d bits=%d pcm=%zu bytes)\n",
                TAG, (unsigned long)sr, ch, bits, pcm_len);
            int prc = nxplayer_playraw(pl, TTS_WAV_PATH,
                AUDIO_FMT_PCM, 0, ch, bits, sr, 0);
            if (prc < 0) {
                /* Playback never started.  Do NOT send STOP:
                 * a STOP on a device that never STARTed can
                 * leave it in DRAINING/OPEN, where audio_start
                 * silently ignores future STARTs and every
                 * later reply becomes permanently silent. */
                syslog(LOG_ERR,
                    "[%s] nxplayer_playraw failed: %d\n",
                    TAG, prc);
                nxplayer_release(pl);
            } else {
#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT
                if (!s_voice.suppress_speaking_face) {
                    watch_expression_page_set_face("speaking", 0);
                }
                s_voice.suppress_speaking_face = false; /* one-shot */
#endif
                s_voice.tts_player = pl;   /* expose for interrupt_tts() */
                s_voice.tts_abort  = 0;
                uint32_t est_ms =
                    (uint32_t)(pcm_len * 1000ULL / byte_rate);
                uint32_t timeout_ms = est_ms + 15000;
                if (timeout_ms > 20000) timeout_ms = 20000;
                uint32_t waited_ms = 0;
                /* Publish estimated playback end so the conversation
                 * thread (different task group — fds don't cross) can
                 * pre-open the ASR stream itself ~2s before the end. */
                if (est_ms > 2500) {
                    s_voice.play_end_ms = now_ms() + est_ms;
                }
                while (waited_ms < timeout_ms) {
                    usleep(200 * 1000);
                    waited_ms += 200;
                    if (pl->state == 0) break;
                    /* Allow system alert to abort TTS mid-playback */
                    if (s_voice.tts_abort) {
                        syslog(LOG_INFO, "[%s] TTS aborted by system alert\n", TAG);
                        break;
                    }
                }
                s_voice.play_end_ms = 0;
                s_voice.tts_player = NULL;
                usleep(300 * 1000);
                nxplayer_stop(pl);
                nxplayer_release(pl);
            }
        }
        unlink(TTS_WAV_PATH);
    }

    /* Half-duplex: TTS done, resume listening */
    if (s_voice.state == VOICE_SPEAKING) {
        VOICE_DBG("state: SPEAKING → LISTENING");
        s_voice.state = VOICE_LISTENING;
    }

    pthread_mutex_unlock(&s_voice.speak_lock);
    return 0;
}

void voice_channel_enable_wake_gate(void)
{
    pthread_mutex_lock(&s_voice.lock);
    if (s_voice.state == VOICE_LISTENING) {
        s_voice.wake_gated = true;
        syslog(LOG_INFO, "[%s] wake gate enabled (external request)\n", TAG);
    }
    pthread_mutex_unlock(&s_voice.lock);
}

void voice_channel_suppress_speaking_face(bool suppress)
{
    s_voice.suppress_speaking_face = suppress;
}

/* Request mic mute/unmute from any thread (button handler, etc.).
 * The actual I2C write to ES7210 is deferred to the conversation_thread
 * to avoid I2C bus deadlock with the audio driver. */
void voice_channel_request_mic_mute(bool mute)
{
    s_voice.mic_mute_target  = mute;
    s_voice.mic_mute_pending = true;
}

void voice_channel_interrupt_tts(void)
{
    /* Signal the TTS playback loop to abort */
    s_voice.tts_abort = 1;

    /* If player is active, stop it immediately to unblock the loop */
    pthread_mutex_lock(&s_voice.lock);
    volatile struct nxplayer_s* pl = s_voice.tts_player;
    pthread_mutex_unlock(&s_voice.lock);

    if (pl) {
        syslog(LOG_INFO, "[%s] interrupting active TTS playback\n", TAG);
        nxplayer_stop((struct nxplayer_s*)pl);
    }
}

void voice_channel_cleanup(void)
{
    voice_channel_stop();
}

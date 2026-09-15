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

/* voice_tts backend on top of the E2E realtime dialogue session.
 *
 * The reply text produced by ai_agent's own LLM (with skills, tools,
 * system prompt and memory all applied) is injected back into the live
 * session with ChatTTSText (event 500), and the synthesized PCM is
 * handed to the caller through the usual chunk callback.  No socket is
 * touched here, so this works from the agent task group.
 *
 * When no session is up (cron reminders, emotion toy prompts, ...) the
 * classic per-utterance WebSocket TTS is used instead. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_tts.h"

#include "volc_e2e.h"
#include "volc_tts.h"

static const char* TAG = "volc_e2e_tts";

/* One ChatTTSText frame per sentence keeps the server synthesizing
 * while our text is still streaming in. */
#define E2E_TTS_SEG_MAX 180
#define E2E_TTS_SEG_MIN 12

/* Deadlines: the first chunk waits for the server to start synthesis,
 * later chunks arrive back to back. */
#define E2E_TTS_FIRST_WAIT_MS 10000
#define E2E_TTS_NEXT_WAIT_MS 5000

#define E2E_TTS_POP_BUF 4096

/* E2E downlink PCM is quieter than the local volc_tts_ws output at the
 * same volume_ratio=2.0 (voice baseline difference: jupiter vs uranus,
 * and 2.0 is already the server-side maximum).  Apply a software gain
 * before playback to match perceived loudness; tune the ratio here if
 * the mismatch changes. */
#define E2E_PCM_GAIN_NUM 3
#define E2E_PCM_GAIN_DEN 2

static void e2e_apply_pcm_gain(unsigned char* buf, size_t n)
{
    int16_t* s = (int16_t*)buf;
    size_t cnt = n / 2;

    for (size_t i = 0; i < cnt; i++) {
        int32_t v = (int32_t)s[i] * E2E_PCM_GAIN_NUM / E2E_PCM_GAIN_DEN;

        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }

        s[i] = (int16_t)v;
    }
}

/* Cap on the hybrid drain: the E2E server self-chat can run unbounded
 * (a garbled utterance once produced 73s / 3.5MB), overflowing the ~8s
 * PCM ring and the tmpfs FS heap (WAV write ENOMEM).  1.5MB of mono
 * s16le @24k ≈ 32s — covers "count to 30" (~28s) while still leaving
 * the drain bounded; the WAV consumer now streams chunks straight to
 * tmpfs as mono (no stereo doubling, no RAM buffer), so the FS heap
 * only sees the file itself.  Exceeding the cap aborts the drain so
 * the caller falls back to classic TTS with the agent's own reply. */
#define E2E_HYBRID_DRAIN_MAX (1536 * 1024)

static int utf8_clen(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static bool is_sentence_end(const char* p, int clen)
{
    if (clen == 1) {
        return strchr(".!?;\n", *p) != NULL;
    }
    static const char* const ends[] = {
        "\xe3\x80\x82", /* 。 */
        "\xef\xbc\x81", /* ！ */
        "\xef\xbc\x9f", /* ？ */
        "\xef\xbc\x9b", /* ； */
        "\xe2\x80\xa6", /* … */
        NULL,
    };
    for (int i = 0; ends[i]; i++) {
        if ((int)strlen(ends[i]) == clen && memcmp(p, ends[i], clen) == 0) {
            return true;
        }
    }
    return false;
}

/* Split text on sentence boundaries and queue each piece. */
static int queue_text(const char* text)
{
    char seg[E2E_TTS_SEG_MAX + 8];
    size_t used = 0;
    const char* p = text;

    while (*p) {
        int clen = utf8_clen((unsigned char)*p);
        if ((int)strlen(p) < clen) clen = (int)strlen(p);

        if (used + (size_t)clen > E2E_TTS_SEG_MAX) {
            seg[used] = '\0';
            int ret = volc_e2e_tts_text(seg);
            if (ret != 0) return ret;
            used = 0;
        }

        memcpy(seg + used, p, (size_t)clen);
        used += (size_t)clen;

        bool flush = is_sentence_end(p, clen) && used >= E2E_TTS_SEG_MIN;
        p += clen;

        if (flush) {
            seg[used] = '\0';
            int ret = volc_e2e_tts_text(seg);
            if (ret != 0) return ret;
            used = 0;
        }
    }

    if (used > 0) {
        seg[used] = '\0';
        int ret = volc_e2e_tts_text(seg);
        if (ret != 0) return ret;
    }
    return 0;
}

static int e2e_tts_init(void)
{
    return volc_e2e_load_config();
}

static void e2e_tts_deinit(void)
{
    /* The session is owned by the ASR side; nothing to release. */
}

/* Forward decl — hybrid fast-path helper (defined below e2e_synth_stream
 * so it can reference the same E2E_TTS_POP_BUF constant). */
static int e2e_hybrid_drain(voice_tts_chunk_cb cb, void* user_data);

static int e2e_synth_stream(const char* text, voice_tts_chunk_cb cb,
    void* user_data)
{
    if (!text || !text[0] || !cb) return -EINVAL;

    if (!volc_e2e_is_up() || volc_e2e_classic_tts_forced()) {
        /* No live turn (cron, emotion toy, tool-triggered speech), or a
         * proactive turn (no uplink audio → the server never synthesizes
         * ChatTTSText): fall back to the classic per-utterance
         * WebSocket TTS. */
        syslog(LOG_INFO, "[%s] no session or classic forced, using %s\n",
            TAG, AGENT_VOICE_BACKEND_TTS_WS);
        return volc_tts_ws_synthesize_stream(text, cb, user_data);
    }

    /* ── Hybrid orchestration routing ─────────────────────────
     * When hybrid mode is active, the ASR-ended handler has already
     * buffered the server's self-chat audio in the E2E PCM ring.
     * The agent_loop has just made the routing decision:
     *   - E2E_HYBRID_E2E   → drain the ring as our reply (fast path)
     *   - E2E_HYBRID_AGENT → ring was already cleared; synthesize
     *                        the agent's text via volc_tts_ws
     *   - other            → plain bridge mode (pre-hybrid code)
     * ────────────────────────────────────────────────────────── */
    e2e_hybrid_t hy = volc_e2e_hybrid_get();

    if (hy == E2E_HYBRID_E2E) {
        /* Fast path: server's self-chat IS our reply.  Drain the ring
         * buffer (pop_pcm honors s_pcm_eos, so we get is_last=1 once
         * the buffered audio is exhausted).  Skip all ChatTTSText
         * injection — we don't want to synthesize additional text. */
        syslog(LOG_INFO,
            "[%s] hybrid: draining E2E ring as reply\n", TAG);
        int rc = e2e_hybrid_drain(cb, user_data);
        if (rc == 0) return 0;

        /* E2E reply truncated (session drop / timeout).  Fall back to
         * classic V1 WS TTS to re-synthesize the agent's full reply
         * instead of playing a half sentence. */
        syslog(LOG_WARNING,
            "[%s] hybrid: E2E drain truncated (rc=%d), "
            "fallback to classic TTS\n", TAG, rc);
        return volc_tts_ws_synthesize_stream(text, cb, user_data);
    }

    if (hy == E2E_HYBRID_AGENT) {
        /* Agent path: the ring was already cleared by
         * volc_e2e_hybrid_route_agent().  Synthesize the agent's
         * own text via V1 WebSocket TTS.  The default speaker
         * zh_female_vv_uranus_bigtts is part of the "Doubao TTS 2.0"
         * voice family and sounds identical to the E2E voice
         * (zh_female_vv_jupiter_bigtts). */
        syslog(LOG_INFO,
            "[%s] hybrid: agent path via volc_tts_ws (V1 WS)\n", TAG);
        return volc_tts_ws_synthesize_stream(text, cb, user_data);
    }

    /* Plain bridge mode (hybrid disabled or IDLE): inject text into
     * the live E2E session. */
    int ret = queue_text(text);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] queue text failed: %d\n", TAG, ret);
        volc_e2e_tts_abort();
        return ret;
    }

    ret = volc_e2e_tts_end();
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] end frame failed: %d\n", TAG, ret);
        volc_e2e_tts_abort();
        return ret;
    }

    unsigned char* buf = malloc(E2E_TTS_POP_BUF);
    if (!buf) {
        volc_e2e_tts_abort();
        return -ENOMEM;
    }

    bool first = true;
    size_t total = 0;
    int rc = 0;

    for (;;) {
        size_t n = 0;
        int is_last = 0;
        ret = volc_e2e_tts_pop_pcm(buf, E2E_TTS_POP_BUF, &n, &is_last,
            first ? E2E_TTS_FIRST_WAIT_MS : E2E_TTS_NEXT_WAIT_MS);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] pop pcm: %d (after %zu bytes)\n",
                TAG, ret, total);
            /* Close the WAV so playback still happens for what we got. */
            cb(NULL, 0, 1, user_data);
            rc = total > 0 ? 0 : ret;
            break;
        }

        first = false;
        total += n;

        if (n > 0) {
            e2e_apply_pcm_gain(buf, n);
        }

        if (n > 0 || is_last) {
            cb(n > 0 ? buf : NULL, n, is_last, user_data);
        }
        if (is_last) break;
    }

    free(buf);
    volc_e2e_tts_abort(); /* release the turn for the next round */

    if (rc == 0) {
        syslog(LOG_INFO, "[%s] synthesized %zu bytes\n", TAG, total);
    }
    return rc;
}

/* Hybrid fast-path helper: stream the buffered E2E server audio
 * straight to the TTS callback.  Relies on s_pcm_eos being set by
 * TTSEnded (see handle EVENT_TTS_ENDED in volc_e2e_conn.c) so
 * pop_pcm returns is_last=1 once the ring is drained. */
static int e2e_hybrid_drain(voice_tts_chunk_cb cb, void* user_data)
{
    unsigned char* buf = malloc(E2E_TTS_POP_BUF);
    if (!buf) return -ENOMEM;

    /* Stream chunks straight to the callback (same as bridge mode):
     * a long reply is 30s+ / 1.5MB of mono PCM, and buffering the whole
     * reply in RAM (old realloc-grow loop) peaked at cap bytes on top
     * of the WAV copy.  On truncation the caller falls back to classic
     * TTS — a negative is_last tells the WAV consumer to rewind to a
     * clean slate so the fallback rewrites the file from the start. */
    size_t total = 0;
    bool first = true;
    int rc = 0;

    for (;;) {
        size_t n = 0;
        int is_last = 0;
        int ret = volc_e2e_tts_pop_pcm(buf, E2E_TTS_POP_BUF, &n, &is_last,
            first ? E2E_TTS_FIRST_WAIT_MS : E2E_TTS_NEXT_WAIT_MS);
        if (ret != 0) {
            /* Truncated — rewind the consumer, expose the error. */
            cb(NULL, 0, -1, user_data);
            rc = ret;
            break;
        }
        first = false;
        if (n > 0) {
            if (total + n > E2E_HYBRID_DRAIN_MAX) {
                /* Server self-chat is runaway — abandon the drain and
                 * signal the caller to fall back to classic TTS with
                 * the agent's own reply. */
                syslog(LOG_WARNING,
                    "[%s] hybrid drain exceeded cap (%zu bytes), aborting\n",
                    TAG, (size_t)E2E_HYBRID_DRAIN_MAX);
                cb(NULL, 0, -1, user_data);
                rc = -EFBIG;
                break;
            }
            e2e_apply_pcm_gain(buf, n);
            cb(buf, n, 0, user_data);
            total += n;
        }
        if (is_last) {
            /* Complete — tell the consumer to finalize. */
            cb(NULL, 0, 1, user_data);
            break;
        }
    }

    free(buf);
    syslog(LOG_INFO,
        "[%s] hybrid drained %zu bytes from E2E ring (rc=%d)\n",
        TAG, total, rc);
    /* Release the turn for the next round */
    volc_e2e_tts_abort();
    return rc;
}

static const voice_tts_ops_t s_e2e_tts_ops = {
    .name = AGENT_VOICE_BACKEND_E2E,
    .init = e2e_tts_init,
    .synthesize = NULL,
    .synthesize_stream = e2e_synth_stream,
    .deinit = e2e_tts_deinit,
};

int volc_e2e_tts_register(void)
{
    return voice_tts_register(&s_e2e_tts_ops);
}

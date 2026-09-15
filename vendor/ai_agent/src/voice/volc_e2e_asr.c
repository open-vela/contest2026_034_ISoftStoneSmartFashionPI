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

/* voice_asr backend on top of the E2E realtime dialogue session.
 *
 * Every entry point here is pure memory work: the socket belongs to the
 * threads spawned by volc_e2e_start(), which runs in the caller's task
 * group (the voice conversation thread).  See volc_e2e.h. */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_asr.h"

#include "volc_e2e.h"

static const char* TAG = "volc_e2e_asr";

/* Non-NULL sentinel handed back as the stream handle: the real state
 * lives in volc_e2e_conn.c because a single session serves the whole
 * device. */
static int s_stream_token;

static int e2e_asr_init(void)
{
    /* Credentials only; the connection is established lazily on the
     * first turn so that boot time is unaffected. */
    return volc_e2e_load_config();
}

static void e2e_asr_deinit(void)
{
    volc_e2e_stop();
}

static int e2e_stream_open(void** handle)
{
    if (!handle) return -EINVAL;
    *handle = NULL;

    int ret = volc_e2e_start();
    if (ret == -EAGAIN) {
        /* Session is coming up (or backing off after a failure); give it
         * a short grace period before giving up on this turn.  Retry
         * volc_e2e_start() every ~1s instead of just polling is_up():
         * when the first call hit the reconnect-backoff gate, nobody
         * was dialing at all and the whole grace window idled away
         * (observed: 5s wasted, then -EAGAIN, while the network had
         * long recovered). */
        for (int i = 0; i < 50 && !volc_e2e_is_up(); i++) {
            usleep(100000);
            if ((i % 10) == 9) {
                ret = volc_e2e_start();
                if (ret == 0) break;
            }
        }
        ret = volc_e2e_is_up() ? 0 : -EAGAIN;
    }
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] session unavailable: %d\n", TAG, ret);
        return ret;
    }

    volc_e2e_arm_turn();
    *handle = &s_stream_token;
    return 0;
}

static int e2e_stream_send(void* handle, const unsigned char* pcm, size_t len)
{
    if (!handle) return -EINVAL;
    return volc_e2e_push_audio(pcm, len);
}

static int e2e_stream_try_recv(void* handle, char* text_out, size_t text_cap)
{
    if (!handle) return -EINVAL;
    return volc_e2e_pop_asr(text_out, text_cap);
}

static int e2e_stream_finish(void* handle, char* text_out, size_t text_cap)
{
    if (!handle) return -EINVAL;
    if (text_out && text_cap > 0) text_out[0] = '\0';

    /* Server side VAD already closed the utterance; whatever is pending
     * is returned, otherwise the caller gets an empty string.  The
     * session itself stays connected for the next turn. */
    int ret = volc_e2e_pop_asr(text_out, text_cap);
    volc_e2e_end_turn();
    return ret > 0 ? 0 : (ret < 0 ? ret : 0);
}

static void e2e_stream_abort(void* handle)
{
    (void)handle;
    /* Not a hard abort of the session: voice_channel.c also lands here
     * after a perfectly normal turn.  Only the uplink is closed; the TTS
     * channel claimed on ASREnded must survive for the reply. */
    volc_e2e_end_turn();
}

/* Batch recognition: run the streaming path once so callers that only
 * use voice_asr_recognize() (tools, tests) still work. */
static int e2e_recognize(const unsigned char* pcm_data, size_t pcm_len,
    char* text_out, size_t text_cap)
{
    if (!pcm_data || pcm_len == 0 || !text_out || text_cap == 0) {
        return -EINVAL;
    }
    text_out[0] = '\0';

    void* h = NULL;
    int ret = e2e_stream_open(&h);
    if (ret != 0) return ret;

    const size_t chunk = 640; /* 20ms @16k mono 16bit */
    for (size_t off = 0; off < pcm_len; off += chunk) {
        size_t n = pcm_len - off;
        if (n > chunk) n = chunk;
        ret = e2e_stream_send(h, pcm_data + off, n);
        if (ret != 0) {
            e2e_stream_abort(h);
            return ret;
        }
        usleep(20000); /* pace the uplink like a live capture would */
    }

    /* Wait for the server to declare the utterance finished. */
    for (int i = 0; i < 100; i++) {
        ret = e2e_stream_try_recv(h, text_out, text_cap);
        if (ret > 0) {
            /* Batch callers do not speak through this session, so drop
             * the TTS channel the ASREnded handler just claimed. */
            volc_e2e_tts_abort();
            e2e_stream_abort(h);
            return 0;
        }
        if (ret < 0) {
            e2e_stream_abort(h);
            return ret;
        }
        usleep(100000);
    }

    e2e_stream_abort(h);
    return -ETIMEDOUT;
}

static const voice_asr_ops_t s_e2e_asr_ops = {
    .name = AGENT_VOICE_BACKEND_E2E,
    .init = e2e_asr_init,
    .recognize = e2e_recognize,
    .deinit = e2e_asr_deinit,
    .stream_open = e2e_stream_open,
    .stream_send = e2e_stream_send,
    .stream_try_recv = e2e_stream_try_recv,
    .stream_finish = e2e_stream_finish,
    .stream_abort = e2e_stream_abort,
};

int volc_e2e_asr_register(void)
{
    return voice_asr_register(&s_e2e_asr_ops);
}

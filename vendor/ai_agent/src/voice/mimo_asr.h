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

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the MiMo ASR backend with the voice_asr framework. */
int mimo_asr_register(void);

/* ── Streaming ASR interface ─────────────────────────────────── */

/* MiMo ASR uses a non-streaming HTTP API (audio uploaded in full,
 * then result returned). The streaming interface below accumulates
 * PCM chunks internally and performs batch recognition on finish().
 * This keeps the voice_channel streaming API compatible. */

typedef struct mimo_asr_stream mimo_asr_stream_t;

mimo_asr_stream_t *mimo_asr_stream_open(void);
int mimo_asr_stream_send(mimo_asr_stream_t *s,
    const unsigned char *pcm, size_t len);
int mimo_asr_stream_finish(mimo_asr_stream_t *s,
    char *text_out, size_t text_cap);
void mimo_asr_stream_abort(mimo_asr_stream_t *s);

#ifdef __cplusplus
}
#endif

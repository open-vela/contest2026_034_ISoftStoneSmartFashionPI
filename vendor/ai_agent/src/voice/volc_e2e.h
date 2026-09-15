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

/* Volcengine E2E realtime dialogue connection (bridge mode).
 *
 * A single persistent WebSocket carries both directions:
 *   uplink   : captured PCM  -> TaskRequest (200)
 *   downlink : ASR text      <- ASRResponse (451) / ASREnded (459)
 *   uplink   : reply text    -> ChatTTSText (500)
 *   downlink : TTS PCM       <- TTSResponse (352) / TTSEnded (359)
 *
 * The server-side LLM output (ChatResponse 550) is deliberately ignored:
 * replies are produced by ai_agent's own llm_chat_tools(), so skills,
 * tools, the system prompt and long/short term memory all stay in play.
 *
 * THREADING / NuttX fd ownership
 * -----------------------------
 * NuttX fd tables are per task group.  The socket is created and used
 * ONLY by the two internal threads spawned from volc_e2e_start(), so
 * volc_e2e_start() must be called from the task group that will keep
 * the session alive (the voice conversation thread).  Every other
 * function below touches memory queues only and is therefore safe to
 * call from any task group (notably voice_channel_speak(), which runs
 * in the agent task group).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Session lifecycle (socket-owning task group only) ─────────── */

/* Connect (TLS + WS upgrade + StartConnection + StartSession) and spawn
 * the recv/send threads.  Idempotent: returns 0 when already up.
 * Applies a short backoff after a failure to avoid hammering. */
int volc_e2e_start(void);

/* Tear the session and connection down and join the threads. */
void volc_e2e_stop(void);

/* True when the connection and session are both established. */
bool volc_e2e_is_up(void);

/* Load credentials from the config store.  Cheap, no I/O on network. */
int volc_e2e_load_config(void);

/* ── Per-turn control (memory only, any task group) ────────────── */

/* Arm a new listening turn: clears the ASR slot and the uplink ring.
 * When a TTS downlink is still being drained (voice_channel.c pre-opens
 * the next ASR stream ~2s before playback ends) the reset is postponed
 * until the first volc_e2e_push_audio() of the new turn. */
void volc_e2e_arm_turn(void);

/* Close the uplink for the current turn.  A TTS channel already claimed
 * by the ASREnded handler is preserved, so this is the right thing to
 * call both when a turn completed normally and when it was given up. */
void volc_e2e_end_turn(void);

/* ── ASR uplink / downlink (memory only, any task group) ───────── */

/* Queue one mono 16kHz/16bit PCM chunk for the uplink.
 * Silently drops data when the turn is not in the listening phase. */
int volc_e2e_push_audio(const unsigned char *pcm, size_t len);

/* Non-blocking probe for the final utterance.
 * Returns 1 when a final result is available (copied to text_out),
 * 0 when nothing is ready yet, negative errno on a dead link. */
int volc_e2e_pop_asr(char *text_out, size_t cap);

/* ── TTS uplink / downlink (memory only, any task group) ───────── */

/* Queue ChatTTSText {start:true,content:""}.  No-op when the channel
 * was already claimed by the ASREnded handler. */
int volc_e2e_tts_begin(void);

/* Queue ChatTTSText {content:<utf8>}.  The first call also opens the
 * PCM gate, discarding any audio the server had produced on its own. */
int volc_e2e_tts_text(const char *utf8);

/* Queue ChatTTSText {end:true}. */
int volc_e2e_tts_end(void);

/* Pop synthesized PCM (16-bit LE mono @ AGENT_TTS_WS_SAMPLE_RATE).
 * Blocks up to timeout_ms for data.
 * Returns 0 on success (*out_len may be 0 when *is_last is set),
 * -ETIMEDOUT when nothing arrived in time, other negative on error. */
int volc_e2e_tts_pop_pcm(unsigned char *buf, size_t cap,
                         size_t *out_len, int *is_last, int timeout_ms);

/* Abandon the current TTS downlink (playback interrupted). */
void volc_e2e_tts_abort(void);

/* ── Hybrid orchestration (E2E + agent parallel) ─────────────────
 *
 * When hybrid mode is enabled (voice_mode=e2e AND hybrid_mode=on):
 *   1. ASREnded enters HYBRID_PENDING; E2E server audio flows into
 *      the PCM ring buffer (≈8s of capacity @24kHz mono).
 *   2. The agent LLM runs in parallel.
 *   3. If the agent LLM made no tool calls, route_e2e() makes the
 *      buffered audio available for playback via volc_e2e_tts_pop_pcm.
 *   4. If tools were called, route_agent() discards the buffered
 *      audio; the agent's own reply is synthesized via volc_tts_ws.
 *
 * All functions below are memory-only and safe from any task group. */

/* Hybrid routing state. */
typedef enum {
    E2E_HYBRID_IDLE = 0,     /* not in hybrid flow */
    E2E_HYBRID_PENDING,      /* waiting for agent decision */
    E2E_HYBRID_E2E,          /* routed to E2E (drain ring) */
    E2E_HYBRID_AGENT,        /* routed to agent (ring cleared) */
} e2e_hybrid_t;

/* Enable/disable hybrid mode at runtime.  The default (after
 * volc_e2e_load_config) reflects the AGENT_CFG_KEY_HYBRID_MODE key;
 * this call overrides it. */
void volc_e2e_set_hybrid_enabled(bool on);
bool volc_e2e_hybrid_enabled(void);

/* Current routing state for this turn. */
e2e_hybrid_t volc_e2e_hybrid_get(void);

/* Return a strdup of the server's cached text reply (if any) without
 * changing hybrid state or clearing the ring.  Used by the chitchat
 * shortcut as the reply text / fallback.  Caller frees.  Returns NULL
 * when the server has not sent a ChatResponse yet. */
char* volc_e2e_hybrid_get_reply(void);

/* Route to E2E fast path: unblock pop_pcm so the buffered server
 * audio drains as the reply.  Returns 0 on success, -EAGAIN if
 * not in PENDING state. */
int volc_e2e_hybrid_route_e2e(void);

/* Route to agent path: clear the PCM ring and return strdup'd copy
 * of the server's text reply (may be NULL if the server didn't send
 * ChatResponse).  Caller owns the returned pointer.  Returns NULL
 * on error and logs a warning. */
char *volc_e2e_hybrid_route_agent(void);

/* Read-only access to the server's cached ChatResponse text.
 * Returns a strdup'd copy (caller frees) or NULL if empty.
 * Does NOT modify hybrid state or PCM ring — safe to call from
 * the E2E fast path to sync session memory. */
char *volc_e2e_get_server_reply(void);

/* Drift tracking: call after each routing decision.
 * note_agent_path: increments streak; auto-restarts E2E session
 *   when streak >= threshold (clears server-side ghost replies).
 * note_e2e_path: resets streak to zero (server context is in sync). */
void volc_e2e_note_agent_path(void);
void volc_e2e_note_e2e_path(void);

/* Force the next TTS synthesis through the classic per-utterance
 * WebSocket TTS (volc_tts_ws) even while the E2E session is up.
 * Used for proactive turns (voice_channel_inject_prompt): they have no
 * uplink audio and no active server turn, so ChatTTSText injection
 * into the live session is never synthesized (pop_pcm times out).
 * The caller must reset to false once the turn finishes — the turns
 * are serialized, so a plain bool is safe. */
void volc_e2e_set_classic_tts(bool on);
bool volc_e2e_classic_tts_forced(void);

/* ── Backend registration ──────────────────────────────────────── */

int volc_e2e_asr_register(void);
int volc_e2e_tts_register(void);

#ifdef __cplusplus
}
#endif

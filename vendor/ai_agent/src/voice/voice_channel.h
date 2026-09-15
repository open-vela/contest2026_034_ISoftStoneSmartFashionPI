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

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize voice channel (load config keys). */
int voice_channel_init(void);

/* Reset the static backend registries.  Call this early in ai_agent_main()
 * so voice_channel_is_ready() returns false until the new instance's
 * voice_channel_init() re-registers (fixes the stale-static bug on a
 * runtime UI-mode switch where the launcher starts wake-listening too soon). */
void voice_channel_reset_backends(void);

/* Returns true if voice_channel_init() has completed successfully. */
bool voice_channel_is_ready(void);

/* Start voice recording, ASR, then push text to agent via message_bus. */
int voice_channel_start(void);

/* Same as voice_channel_start(), but wake-gated: ASR results are only
 * matched against wake words ("你好"/"openvela" variants) and dropped
 * until a wake word is heard; then the gate opens for normal dialog. */
int voice_channel_start_wake(void);

/* Register a callback invoked when wake word is detected (before TTS).
 * Runs in conversation_thread context. Pass NULL to unregister. */
void voice_channel_set_wake_notify(void (*cb)(void));

/* Force the voice channel into wake-gated mode.  Next utterance
 * must match a wake word before anything is sent to the LLM.
 * Thread-safe, callable from any context (LVGL timer, etc.). */
void voice_channel_enable_wake_gate(void);

/* Request mic mute/unmute (safe from any thread, e.g. button handler).
 * The actual I2C write is deferred to the conversation_thread to avoid
 * I2C bus contention with the audio driver. */
void voice_channel_request_mic_mute(bool mute);

/* Suppress the "speaking" face during TTS — keeps whatever emotion
 * face was set by the LLM via set_face tool.  One-shot: auto-resets
 * after the next voice_channel_speak() call. */
void voice_channel_suppress_speaking_face(bool suppress);
/* Idle lifecycle events reported via the idle event callback.
 * Sequence in expression (watch boot) mode:
 *   COMPANION ×VOICE_COMPANION_MAX (proactive companion turns fired
 *     every idle window) → STANDBY (standby face shown, wake gate
 *     closed — only the wake word opens it) → SCREEN_OFF (display
 *     turned off, final state; wake word still wakes it back up). */
#define VOICE_IDLE_EVT_COMPANION  1  /* no UI action: TTS switches the face */
#define VOICE_IDLE_EVT_STANDBY    2  /* show the standby face */
#define VOICE_IDLE_EVT_SCREEN_OFF 3  /* turn the display off */

/* Register callback for idle lifecycle events (see VOICE_IDLE_EVT_*).
 * Called from conversation_thread; the event parameter tells the UI
 * which stage was entered. */
void voice_channel_set_idle_event_cb(void (*cb)(int event));

/* Interrupt active TTS playback (for system alerts). */
void voice_channel_interrupt_tts(void);

/* Stop an active voice session. */
int voice_channel_stop(void);

/* Stop recording and return ASR text to caller (does NOT push inbound).
 * text_out: buffer to receive ASR text, text_cap: buffer capacity.
 * Returns 0 on success, negative errno on failure.
 * If ASR returns empty text, text_out[0] is set to '\0' (not an error). */
int voice_channel_stop_with_text(char *text_out, size_t text_cap);

/* Synthesize text and play back (called from outbound dispatcher). */
int voice_channel_speak(const char *text);

/* True while a voice conversation is in progress (PROCESSING/SPEAKING).
 * Idle listening (LISTENING without active recording) is NOT busy —
 * voice_channel_speak() can interrupt it.  Used by proactive prompts
 * (low-battery / charging) to skip instead of interrupting the user. */
bool voice_channel_is_busy(void);

/* True only while the wake-word system is fully up AND idle
 * (state == LISTENING).  This is the safe window for proactive
 * prompts: VOICE_IDLE means the voice system has not started yet
 * (e.g. during boot) and speaking then would race the initial
 * capture/session setup, corrupting the state machine. */
bool voice_channel_is_listening(void);

/* Queue a proactive prompt (e.g. low-battery reminder).  The text is
 * NOT spoken directly: the conversation thread consumes it at the top
 * of its loop and runs a full PROCESSING→SPEAKING→LISTENING round-trip,
 * exactly like an ASR turn.  An external speak() from the agent task
 * group would flip LISTENING→SPEAKING and make the conversation loop
 * exit, permanently killing the wake-word system.
 * Thread-safe; a newer prompt replaces a not-yet-consumed one. */
int voice_channel_inject_prompt(const char *text);

/* True while the mic is muted by the short-press button.  The state
 * machine stays LISTENING during mute, so proactive-prompt gates must
 * check this separately. */
bool voice_channel_is_mic_muted(void);

/* True while the wake gate is closed (standby: only the wake word
 * opens it).  Proactive callers use it to decide whether the page was
 * woken from standby and should be hidden again after the turn. */
bool voice_channel_is_wake_gated(void);

#ifdef __cplusplus
}
#endif

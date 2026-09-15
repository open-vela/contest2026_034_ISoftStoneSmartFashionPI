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
#include <stdbool.h>

/* ── Emotion Toy Tool execute function declarations ──────────────────
 *  Corresponding REGISTER_TOOL calls live in tool_registry.c
 *  Implementation files: src/tools/tool_emotion_*.c
 *
 *  Hardware status: audio playback, face display and IMU fall detection
 *  are wired up; Mijia IoT remains a stub.
 * ──────────────────────────────────────────────────────────────────── */

/* speak — text-to-speech (SAFE) */
int tool_speak_execute(const char *input_json, char *output, size_t output_size);

/* set_face — set facial expression, 18-value whitelist (SAFE) */
int tool_set_face_execute(const char *input_json, char *output, size_t output_size);

/* play_audio — play preset audio by ID 1-14 via nxplayer (MODERATE, mutex protected) */
int tool_play_audio_execute(const char *input_json, char *output, size_t output_size);

/* stop_audio — stop current audio playback (SAFE) */
int tool_stop_audio_execute(const char *input_json, char *output, size_t output_size);

/* Direct stop (no JSON) — for use by voice_channel before TTS */
void tool_emotion_audio_stop(void);

/* Check if a preset audio is currently playing (thread-safe). */
bool tool_emotion_audio_is_playing(void);

/* System alert audio (highest priority).
 * alert_id: 15=low_battery, 16=no_network, 17=wifi_timeout
 * force: bypass de-dup cooldown if non-zero */
void tool_system_alert_play(int alert_id, int force);

/* get_motion_event — query latest IMU motion event (SAFE) */
int tool_get_motion_event_execute(const char *input_json, char *output, size_t output_size);

/* Motion monitor lifecycle — mounted by agent_main while the agent runs
 * (the agent process exists only in emotion-toy mode; see launcher.c).
 * Starts calibration + fall/upright state machine on the QMI8658 uORB
 * topics.  Returns OK when the monitor thread is running, ERROR when
 * no IMU is available. */
int tool_emotion_motion_start(void);
void tool_emotion_motion_stop(void);

/* fall_protection_feedback — enable/disable fall protection (MODERATE) */
int tool_fall_protection_feedback_execute(const char *input_json, char *output, size_t output_size);

/* trigger_mijia_scene — trigger Mijia smart-home scene (SENSITIVE) */
int tool_trigger_mijia_scene_execute(const char *input_json, char *output, size_t output_size);

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

/**
 * @file tool_home_control.h
 * Smart home control tool — wraps home_control device APIs.
 *
 * Provides 3 LLM-callable tools:
 *   home_control_list   — enumerate all 12 devices with status
 *   home_control_status — query single device
 *   home_control_toggle — turn device on/off
 */

#pragma once
#include <stddef.h>

int tool_home_control_list_execute(const char *input_json,
                                    char *output, size_t output_size);
int tool_home_control_status_execute(const char *input_json,
                                      char *output, size_t output_size);
int tool_home_control_toggle_execute(const char *input_json,
                                      char *output, size_t output_size);

/* Fast-path: direct text→toggle without JSON parsing.
 * Scans user_text for device name + on/off keywords,
 * calls watch_home_control_toggle_device, and fills *reply
 * with a natural-language Chinese response.
 * Returns 0 on success (reply allocated, caller frees),
 * -1 if text doesn't match any device+action. */
int tool_home_control_fast_toggle(const char *user_text,
                                   char **reply);

/* Scene-based interaction (expression toy mode):
 *   "我好冷" → ask "要不要打开地暖？" → confirm → floor ON
 *   "我好热" → ask "要不要打开空调？" → confirm → AC ON
 * Only these 2 scenes take part; other devices are not involved.
 * Stateful: the follow-up confirmation is consumed on the next call
 * (pending scene expires after HC_SCENE_PENDING_TIMEOUT_SEC).
 * Returns 0 on success (reply allocated, caller frees),
 * -1 if text doesn't trigger/confirm any scene. */
int tool_home_control_scene(const char *user_text, char **reply);

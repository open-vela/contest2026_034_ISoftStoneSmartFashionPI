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
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#pragma once

#include "agent_compat.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int context_build_system_prompt(char *buf, size_t size);
int context_build_messages(const char *history_json, const char *user_message,
                                  char *buf, size_t size);

/**
 * Invalidate the internally cached system prompt.
 * Call after any operation that changes user profile, long-term
 * memory, or daily notes but does not update the underlying file's
 * mtime (rare — mtime-based auto-invalidation covers the common case).
 */
void context_invalidate_prompt_cache(void);

#ifdef __cplusplus
}
#endif

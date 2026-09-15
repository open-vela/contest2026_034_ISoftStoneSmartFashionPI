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

#include "cJSON.h"
#include "agent_compat.h"
#include "agent_config.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int llm_proxy_init(void);
int llm_set_backend(const char* host, const char* path);
int llm_set_port(const char* port);

/**
 * Atomically set all LLM proxy fields in a single lock acquisition.
 * Used by llm_router to avoid partial updates.
 * Any NULL or empty field is skipped (not cleared).
 */
int llm_set_all(const char* host, const char* path,
    const char* port, const char* api_key,
    const char* model);

/**
 * Warm up the TLS connection pool for the configured LLM endpoint so
 * the next llm_chat / llm_chat_tools skips the ~3 s TLS handshake.
 * Cheap no-op when a live pooled connection already exists.  Call from
 * the agent task group only (NuttX fd tables are per task group).
 */
int llm_preconnect(void);

int llm_chat(const char* system_prompt, const char* messages_json,
    char* response_buf, size_t buf_size);

/* ── Tool Use ─────────────────────────────────────────────── */

typedef struct {
    char id[64];
    char name[32];
    char* input; /* heap-allocated JSON string */
    size_t input_len;
} llm_tool_call_t;

typedef struct {
    char* text;
    size_t text_len;
    char* reasoning_content; /* heap-allocated; must be echoed back in tool-call turns */
    llm_tool_call_t calls[AGENT_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;

    /* Token usage from API response */
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
} llm_response_t;

void llm_response_free(llm_response_t* resp);

int llm_chat_tools(const char* system_prompt,
    cJSON* messages,
    const char* tools_json,
    llm_response_t* resp);

/**
 * Set an upper bound on max_completion_tokens / max_tokens applied
 * to every subsequent llm_chat / llm_chat_tools request until
 * explicitly cleared with llm_set_max_tokens_hint(0).
 *
 * Voice / QuickApp channels should call this with a small value
 * (e.g. 512) because TTS output is constrained to ≤40 characters.
 * A large default (16 K) makes the LLM server schedule generation
 * more conservatively and inflates TTFT.
 *
 * Passing 0 restores the built-in default (AGENT_LLM_MAX_TOKENS_OPENAI
 * for OpenAI-compatible hosts, AGENT_LLM_MAX_TOKENS otherwise).
 */
void llm_set_max_tokens_hint(int max_tokens);

/**
 * Disable deep-thinking mode for subsequent LLM calls (per-message
 * hint, mirror of llm_set_max_tokens_hint).  Currently only takes
 * effect on MiMo hosts via the documented body param
 * "thinking": {"type": "disabled"} — reasoning models like
 * mimo-v2.5-pro then answer directly (reasoning_tokens=0) instead of
 * spending 20s+ thinking before a short voice reply.
 * Pass false to restore the model default.
 */
void llm_set_thinking_disabled_hint(bool disabled);

/** Vision chat: send text + base64 image to a vision-capable model.
 *  image_b64 is the raw base64 string (no data: prefix).
 *  mime_type: "image/jpeg", "image/png", etc. NULL defaults to "image/jpeg". */
int llm_chat_vision(const char* prompt, const char* image_b64,
    const char* mime_type,
    char* response_buf, size_t buf_size);

/** Memory-optimized vision chat: accepts raw image bytes instead of
 *  pre-encoded base64.  Performs base64 encoding and JSON construction
 *  in a single buffer to avoid multiple copies of the large image data.
 *  Caller can free raw_image immediately after this returns. */
int llm_chat_vision_raw(const char* prompt,
    const unsigned char* raw_image, size_t raw_len,
    const char* mime_type,
    char* response_buf, size_t buf_size);

/** Vision config snapshot: returns the vision-specific model/api_key/host.
 *  If vision model is not configured, falls back to the main LLM config. */
void llm_snapshot_vision_config(char* model, size_t model_sz,
    char* api_key, size_t key_sz,
    char* host, size_t host_sz);

/** Set independent vision model config.
 *  host/model/api_key: set non-NULL/non-empty to override, NULL to clear.
 *  Empty vision config falls back to main LLM config automatically. */
int llm_set_vision_model(const char* host, const char* model,
    const char* api_key);

#ifdef __cplusplus
}
#endif

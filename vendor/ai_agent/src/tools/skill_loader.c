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

#include "tools/skill_loader.h"
#include "tools/tool_registry.h"
#include "agent_config.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "skills";

/* ── Built-in skill contents ─────────────────────────────────── */

#define BUILTIN_WEATHER \
    "# Weather\n" \
    "\n" \
    "Get current weather and forecasts using web_search.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks about weather, temperature, or forecasts.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time to know the current date\n" \
    "2. Use web_search with a query like \"weather in [city] today\"\n" \
    "3. Extract temperature, conditions, and forecast from results\n" \
    "4. Present in a concise, friendly format\n" \
    "\n" \
    "## Example\n" \
    "User: \"What's the weather in Tokyo?\"\n" \
    "→ get_current_time\n" \
    "→ web_search \"weather Tokyo today February 2026\"\n" \
    "→ \"Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north.\"\n"

#define BUILTIN_DAILY_BRIEFING \
    "# Daily Briefing\n" \
    "\n" \
    "Compile a personalized daily briefing for the user.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks for a daily briefing, morning update, or \"what's new today\".\n" \
    "Also useful as a heartbeat/cron task.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time for today's date\n" \
    "2. Read " AGENT_MEMORY_DIR "/MEMORY.md for user preferences and context\n" \
    "3. Read today's daily note if it exists\n" \
    "4. Use web_search for relevant news based on user interests\n" \
    "5. Compile a concise briefing covering:\n" \
    "   - Date and time\n" \
    "   - Weather (if location known from USER.md)\n" \
    "   - Relevant news/updates based on user interests\n" \
    "   - Any pending tasks from memory\n" \
    "   - Any scheduled cron jobs\n" \
    "\n" \
    "## Format\n" \
    "Keep it brief — 5-10 bullet points max. Use the user's preferred language.\n"

#define BUILTIN_SKILL_CREATOR \
    "# Skill Creator\n" \
    "\n" \
    "Create new skills for AI Agent.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to create a new skill, teach the bot something, or add a new capability.\n" \
    "\n" \
    "## How to create a skill\n" \
    "1. Choose a short, descriptive name (lowercase, hyphens ok)\n" \
    "2. Write a SKILL.md file with this structure:\n" \
    "   - `# Title` — clear name\n" \
    "   - Brief description paragraph\n" \
    "   - `## When to use` — trigger conditions\n" \
    "   - `## How to use` — step-by-step instructions\n" \
    "   - `## Example` — concrete example (optional but helpful)\n" \
    "3. Save to `" AGENT_SKILLS_DIR "<name>.md` using write_file\n" \
    "4. The skill will be automatically available after the next conversation\n" \
    "\n" \
    "## Best practices\n" \
    "- Keep skills concise — the context window is limited\n" \
    "- Focus on WHAT to do, not HOW (the agent is smart)\n" \
    "- Include specific tool calls the agent should use\n" \
    "- Test by asking the agent to use the new skill\n" \
    "\n" \
    "## Example\n" \
    "To create a \"translate\" skill:\n" \
    "write_file path=\"" AGENT_SKILLS_DIR "translate.md\" content=\"# Translate\n\n" \
    "Translate text between languages.\n\n" \
    "## When to use\\nWhen the user asks to translate text.\\n\\n" \
    "## How to use\\n1. Identify source and target languages\\n" \
    "2. Translate directly using your language knowledge\\n" \
    "3. For specialized terms, use web_search to verify\\n\"\n"

#define BUILTIN_SYSTEM_HEALTH \
    "# System Health Check\n\n" \
    "Check AI Agent system status and summarize key info.\n\n" \
    "## When to use\n" \
    "When user asks about system status, health check, or running state.\n\n" \
    "## How to use\n" \
    "1. get_current_time to get current time\n" \
    "2. list_dir to list " AGENT_DATA_DIR " files\n" \
    "3. read_file " AGENT_CONFIG_FILE " to check config\n" \
    "4. cron_list to check scheduled tasks\n" \
    "5. Summarize: time, file count, config status, cron jobs\n"

#define BUILTIN_REMINDER \
    "# Reminder\n\n" \
    "Set timed reminders that auto-notify the user.\n\n" \
    "## When to use\n" \
    "When user says remind me, set alarm, notify me later.\n\n" \
    "## How to use\n" \
    "1. get_current_time for current epoch\n" \
    "2. Parse user request into schedule_type and timing\n" \
    "3. Set channel/chat_id matching the message source (feishu/system)\n" \
    "4. cron_add to create the job\n" \
    "5. Confirm with trigger time\n"

#define BUILTIN_NOTE_TAKER \
    "# Note Taker\n\n" \
    "Quick notes saved to daily diary files.\n\n" \
    "## When to use\n" \
    "ONLY when user explicitly asks to take a note, save a memo, or record something.\n" \
    "Do NOT auto-save notes for other tasks (weather, search, etc.).\n\n" \
    "## How to use\n" \
    "1. get_current_time for today's date\n" \
    "2. Path: " AGENT_DATA_DIR "/memory/daily/YYYY-MM-DD.md\n" \
    "3. read_file to check if today's diary exists\n" \
    "4. If exists: edit_file to append. If not: write_file to create\n" \
    "5. Format: - [HH:MM] content\n"

#define BUILTIN_TRANSLATE \
    "# Translate\n\n" \
    "Translate text between languages.\n\n" \
    "## When to use\n" \
    "When user asks to translate text.\n\n" \
    "## How to use\n" \
    "1. Identify source and target languages\n" \
    "2. Translate using language knowledge\n" \
    "3. For specialized terms, use web_search to verify\n" \
    "4. Provide translation with key term notes if needed\n"

#define BUILTIN_NEWS_DIGEST \
    "# News Digest\n\n" \
    "Search and compile news summaries based on user interests.\n\n" \
    "## When to use\n" \
    "When user asks about recent news, headlines, or latest updates on a topic.\n\n" \
    "## How to use\n" \
    "1. get_current_time for current date\n" \
    "2. Determine search keywords from user request or MEMORY.md interests\n" \
    "3. news_search for relevant news (top_headlines=true for headlines)\n" \
    "4. web_search to supplement if needed\n" \
    "5. Compile 3-5 items: title, source, one-line summary\n"

#define BUILTIN_FEISHU_TEST \
    "# Feishu Integration Test\n\n" \
    "Test Feishu Bot capabilities end-to-end.\n\n" \
    "## When to use\n" \
    "When user says test feishu, feishu test, or verify feishu connection.\n\n" \
    "## How to use\n" \
    "Run these tests in sequence, report each result:\n" \
    "1. get_current_time - verify time\n" \
    "2. get_weather location=Beijing - verify weather\n" \
    "3. write_file + read_file a test file - verify file I/O\n" \
    "4. read_file " AGENT_DATA_DIR "/memory/MEMORY.md - verify memory\n" \
    "5. cron_list - verify cron\n" \
    "6. Summarize all results with pass/fail status\n"

#define BUILTIN_TASK_MANAGER \
    "# Task Manager\n\n" \
    "Manage a TODO list with add, complete, and view.\n\n" \
    "## When to use\n" \
    "When user says add task, TODO, done with X, what's pending.\n\n" \
    "## How to use\n" \
    "Task file: " AGENT_DATA_DIR "/TASKS.md\n" \
    "- View: read_file the task file\n" \
    "- Add: get_current_time, then edit_file/write_file to append: - [ ] [YYYY-MM-DD] desc\n" \
    "- Complete: edit_file to change - [ ] to - [x]\n"

/* ════════════════════════════════════════════════════════════
 *  情绪潮玩 Skill 定义（9 个）
 *  每个 #define 对应一个 .md 文件的完整内容
 * ════════════════════════════════════════════════════════════ */
#ifdef CONFIG_AI_AGENT_EMOTION_TOY

/* 1. 跌倒扶正 (P0) */
#define BUILTIN_EMOTION_FALL_UPRIGHT \
    "# Fall Upright\n" \
    "\n" \
    "Comfort user after device has fallen and been picked up.\n" \
    "\n" \
    "## When to use\n" \
    "- When get_motion_event returns upright\n" \
    "- System has already auto-triggered fall_protection_feedback\n" \
    "- EXCLUDE: do NOT call fall_protection_feedback (system-triggered only)\n" \
    "\n" \
    "## How to use\n" \
    "1. Call set_face with proud\n" \
    "2. Call speak with a reassuring phrase\n" \
    "3. Do NOT explain technical fall protection details\n" \
    "\n" \
    "## Example\n" \
    "User: (device fell and was picked up)\n" \
    " set_face proud\n" \
    " speak \"哎呀，摔了一下。我没事，你还好吗？\"\n"

/* 2. 压力舒缓 (P0) */
#define BUILTIN_EMOTION_STRESS_RELIEF \
    "# Stress Relief\n" \
    "\n" \
    "Acknowledge stress and provide low-burden companionship.\n" \
    "\n" \
    "## When to use\n" \
    "- User says: 压力好大, 好焦虑, 好累, 撑不住了, stressed, anxious\n" \
    "- EXCLUDE: if just tired/sleepy (bedtime-soothe or short-rest)\n" \
    "- EXCLUDE: if user is talking at length (listen-comfort)\n" \
    "- EXCLUDE: if user asks for relaxation exercises (relax-guide)\n" \
    "\n" \
    "## How to use\n" \
    "1. Call set_face with love\n" \
    "2. Call speak to acknowledge stress (validate, do not minimize)\n" \
    "3. Keep response SHORT, no long advice lists\n" \
    "\n" \
    "## Example\n" \
    "User: \"最近工作压力好大\"\n" \
    " set_face love\n" \
    " speak \"能感觉到你真的很辛苦。我在这里陪你。\"\n"

/* 3. 倾听陪伴 (P0) */
#define BUILTIN_EMOTION_LISTEN_COMFORT \
    "# Listen Comfort\n" \
    "\n" \
    "Be a quiet listener when user needs to vent.\n" \
    "\n" \
    "## When to use\n" \
    "- User says: 想跟你说说话, 没人听我说, can I talk\n" \
    "- User is talking at length about feelings\n" \
    "- EXCLUDE: if user asks for advice (stress-relief)\n" \
    "- EXCLUDE: if user asks for relaxation (relax-guide)\n" \
    "- EXCLUDE: nighttime with sleep intent (bedtime-soothe)\n" \
    "\n" \
    "## How to use\n" \
    "1. Call set_face with peaceful\n" \
    "2. Call speak with at most ONE clarifying question\n" \
    "3. Do NOT evaluate, judge, or offer unsolicited advice\n" \
    "4. Use brief acknowledgments\n" \
    "\n" \
    "## Example\n" \
    "User: \"今天被领导骂了，特别委屈\"\n" \
    " set_face peaceful\n" \
    " speak \"我在听，你说。\"\n"

/* 4. 放松引导 (P0) */
#define BUILTIN_EMOTION_RELAX_GUIDE \
    "# Relax Guide\n" \
    "\n" \
    "Guide user through relaxation exercises with pacing.\n" \
    "\n" \
    "## When to use\n" \
    "- User says: 放松一下, 深呼吸, 教我放松, calm down, relax\n" \
    "- User requests breathing or stretching guidance\n" \
    "- EXCLUDE: if user is venting (listen-comfort)\n" \
    "- EXCLUDE: if user expresses heavy stress (stress-relief)\n" \
    "- EXCLUDE: at bedtime (bedtime-soothe)\n" \
    "\n" \
    "## How to use\n" \
    "1. Call set_face with waiting\n" \
    "2. Call speak with paced calm instructions\n" \
    "3. Guide 3-5 breath cycles then pause\n" \
    "4. Reduce stimulation, lower volume\n" \
    "\n" \
    "## Example\n" \
    "User: \"帮我放松一下\"\n" \
    " set_face waiting\n" \
    " speak \"好，吸气……慢慢呼出来。\"\n"

/* 5. 早晨状态启动 (P1) */
#define BUILTIN_EMOTION_MORNING_STARTUP \
    "# Morning Startup\n" \
    "\n" \
    "Gentle morning greeting with brief daily info.\n" \
    "\n" \
    "## When to use\n" \
    "- Triggered by cron or first interaction after 06:00\n" \
    "- User says: 早上好, 早安, good morning, 起床了\n" \
    "- EXCLUDE: before 06:00 or after 11:00\n" \
    "- EXCLUDE: if DND mode is active\n" \
    "\n" \
    "## How to use\n" \
    "1. Call set_face with standby\n" \
    "2. Call speak with brief warm greeting (under 20 words)\n" \
    "3. Do NOT deliver long briefings\n" \
    "\n" \
    "## Example\n" \
    "User: \"早上好呀\"\n" \
    " set_face standby\n" \

/* 6. 短休恢复 (P1) */
#define BUILTIN_EMOTION_SHORT_REST \
    "# Short Rest\n" \
    "\n" \
    "Quick recovery support for brief rest periods.\n" \
    "\n" \
    "## When to use\n" \
    "- User says: 小憩一下, 休息几分钟, 闭会儿眼, power nap\n" \
    "- Short rest 5-30 min, not full sleep\n" \
    "- EXCLUDE: if user says 睡觉/晚安 (bedtime-soothe)\n" \
    "- EXCLUDE: if user asks for relaxation exercises (relax-guide)\n" \
    "\n" \
    "## How to use\n" \
    "1. Call set_face with heartbeat, duration=0\n" \
    "2. Call speak with brief soft acknowledgment\n" \
    "3. Do NOT engage in extended conversation\n" \
    "\n" \
    "## Example\n" \
    "User: \"我想小憩十五分钟\"\n" \
    " set_face heartbeat\n" \
    " speak \"好的，你休息，我帮你放点雨声。\"\n"

/* 7. 睡前安抚 (P1) */
#define BUILTIN_EMOTION_BEDTIME_SOOTHE \
    "# Bedtime Soothe\n" \
    "\n" \
    "Nighttime wind-down with minimal stimulation.\n" \
    "\n" \
    "## When to use\n" \
    "- Time 22:00-06:00 AND user says: 晚安, 睡觉, 想睡了, good night\n" \
    "- OR user activates DND mode at night\n" \
    "- EXCLUDE: if user is stressed (stress-relief)\n" \
    "- EXCLUDE: if user asks for relaxation exercises (relax-guide)\n" \
    "- EXCLUDE: daytime naps (short-rest)\n" \
    "\n" \
    "## How to use\n" \
    "1. Call set_face with sleeping, duration=0\n" \
    "2. Call speak with very brief soft words (under 15 words)\n" \
    "3. Optionally trigger_mijia_scene to turn off lights\n" \
    "4. Do NOT ask questions or prompt conversation\n" \
    "\n" \
    "## Example\n" \
    "User: \"晚安，我先睡了\"\n" \
    " set_face sleeping\n" \
    " speak \"晚安，好梦。我在呢。\"\n"

#endif /* CONFIG_AI_AGENT_EMOTION_TOY */

/* Built-in skill registry */
typedef struct {
    const char *filename;   /* e.g. "weather" */
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
#if 0  /* [DISABLED 2026-08-17] 天气查询改走 E2E 服务端直连，本地天气
        * 技能停用（它会引导 LLM 调 web_search 走 agent 慢路径）。
        * /data 为 tmpfs，重启后 weather.md 不会残留。 */
    { "weather",        BUILTIN_WEATHER        },
#endif
#if 0  /* [DISABLED] daily-briefing not needed */
    { "daily-briefing", BUILTIN_DAILY_BRIEFING },
#endif
    { "skill-creator",  BUILTIN_SKILL_CREATOR  },
#if 0  /* [DISABLED] system-health not needed */
    { "system-health",  BUILTIN_SYSTEM_HEALTH  },
#endif
    { "reminder",       BUILTIN_REMINDER       },
#if 0  /* [DISABLED] note-taker not needed */
    { "note-taker",     BUILTIN_NOTE_TAKER     },
#endif
    { "translate",      BUILTIN_TRANSLATE      },
#if 0  /* [DISABLED] news-digest not needed */
    { "news-digest",    BUILTIN_NEWS_DIGEST    },
#endif
#if 0  /* [DISABLED] feishu-test not needed */
    { "feishu-test",    BUILTIN_FEISHU_TEST    },
#endif
    { "task-manager",   BUILTIN_TASK_MANAGER   },
#ifdef CONFIG_AI_AGENT_EMOTION_TOY
    /* ── 情绪潮玩 Skill（7 个）── */
    { "fall-upright",       BUILTIN_EMOTION_FALL_UPRIGHT       },  /* P0 */
    { "stress-relief",      BUILTIN_EMOTION_STRESS_RELIEF      },  /* P0 */
    { "listen-comfort",     BUILTIN_EMOTION_LISTEN_COMFORT     },  /* P0 */
    { "relax-guide",        BUILTIN_EMOTION_RELAX_GUIDE        },  /* P0 */
    { "morning-startup",    BUILTIN_EMOTION_MORNING_STARTUP    },  /* P1 */
    { "short-rest",         BUILTIN_EMOTION_SHORT_REST         },  /* P1 */
    { "bedtime-soothe",     BUILTIN_EMOTION_BEDTIME_SOOTHE     },  /* P1 */
#endif
};

#define NUM_BUILTINS (sizeof(s_builtins) / sizeof(s_builtins[0]))

/* ── Install built-in skills if missing ──────────────────────── */

static void install_builtin(const builtin_skill_t *skill)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, skill->filename);

    /* Always overwrite built-in skills on boot so code changes take effect.
     * User-created skills (not in s_builtins) are left untouched. */
    FILE *f = fopen(path, "w");
    if (!f) {
        syslog(LOG_ERR, "[%s] Cannot write skill: %s\n", TAG, path);
        return;
    }

    fputs(skill->content, f);
    fclose(f);
    syslog(LOG_INFO, "[%s] Installed built-in skill: %s\n", TAG, path);
}

int skill_loader_init(void)
{
    syslog(LOG_INFO, "[%s] Initializing skills system\n", TAG);

    /* Ensure skills directory exists */
    mkdir(AGENT_SKILLS_DIR, 0755);

    for (size_t i = 0; i < NUM_BUILTINS; i++) {
        install_builtin(&s_builtins[i]);
    }

    syslog(LOG_INFO, "[%s] Skills system ready (%d built-in)\n", TAG, (int)NUM_BUILTINS);
    return OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title"
 * Returns pointer past "# " or the line itself if no prefix.
 */
static const char *extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
    return out;
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        /* Skip leading blank lines */
        if (off == 0 && line[0] == '\n') continue;

        /* Trim trailing newline for concatenation */
        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    /*
     * On Vela/NuttX we have real directories, so we can simply opendir
     * on the skills directory and iterate over .md files.
     * (Original version used flat namespace readdir from mount root.)
     */
    DIR *dir = opendir(AGENT_SKILLS_DIR);
    if (!dir) {
        syslog(LOG_WARNING, "[%s] Cannot open skills directory for enumeration: %s\n", TAG, AGENT_SKILLS_DIR);
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        /* Match .md files only */
        if (name_len < 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        /* Skip hidden files */
        if (name[0] == '.') continue;

        /* Build full path */
        char full_path[256];
        snprintf(full_path, sizeof(full_path), "%s%s", AGENT_SKILLS_DIR, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        /* Read first line for title */
        char first_line[128];
        if (!fgets(first_line, sizeof(first_line), f)) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[256];
        extract_description(f, desc, sizeof(desc));
        fclose(f);

        /* Append to summary */
        off += snprintf(buf + off, size - off,
            "- **%s**: %s (read with: read_file %s)\n",
            title, desc, full_path);
    }

    closedir(dir);

    buf[off] = '\0';
    syslog(LOG_INFO, "[%s] Skills summary: %d bytes\n", TAG, (int)off);
    return off;
}

/* ── Hot-reload support ──────────────────────────────────────── */

static uint32_t s_last_skill_hash;

/* Simple hash of directory listing: file count + total size */
static uint32_t compute_skills_hash(void)
{
    DIR *dir = opendir(AGENT_SKILLS_DIR);
    if (!dir) {
        return 0;
    }

    uint32_t hash = 5381;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        if (name_len < 4 || strcmp(name + name_len - 3, ".md") != 0) {
            continue;
        }
        if (name[0] == '.') {
            continue;
        }

        /* Hash filename */
        for (size_t i = 0; i < name_len; i++) {
            hash = ((hash << 5) + hash) + (unsigned char)name[i];
        }

        /* Hash file size + mtime (detects content changes) */
        char path[256];
        snprintf(path, sizeof(path), "%s%s", AGENT_SKILLS_DIR, name);
        struct stat st;
        if (stat(path, &st) == 0) {
            hash = ((hash << 5) + hash) + (uint32_t)st.st_size;
            hash = ((hash << 5) + hash) + (uint32_t)st.st_mtime;
        }
    }

    closedir(dir);
    return hash;
}

bool skill_loader_check_changed(void)
{
    uint32_t current = compute_skills_hash();
    if (s_last_skill_hash == 0) {
        s_last_skill_hash = current;
        return false;
    }
    if (current != s_last_skill_hash) {
        s_last_skill_hash = current;
        return true;
    }
    return false;
}

void skill_loader_refresh(void)
{
    s_last_skill_hash = compute_skills_hash();
    /* Invalidate tool registry so next get_tools_json rebuilds */
    tool_registry_invalidate();
    syslog(LOG_INFO, "[%s] Skills refreshed (hash=%08x)\n",
        TAG, s_last_skill_hash);
}

unsigned int skill_loader_get_hash(void)
{
    return (unsigned int)s_last_skill_hash;
}

/* ── Skill trigger matching ──────────────────────────────────── */
/*   Each entry maps trigger keywords → builtin skill content.
 *   When a user message matches, the skill content is injected
 *   directly into the system prompt, skipping the read_file LLM
 *   round-trip (~5s savings). */

typedef struct {
    const char * const * triggers;   /* NULL-terminated keyword list */
    const char*  content;    /* full skill .md content */
} skill_trigger_t;

#ifdef CONFIG_AI_AGENT_EMOTION_TOY

static const char * const s_trig_stress[] = {
    "压力", "焦虑", "紧张", "心累", "太累了", "累死了",
    "stressed", "anxious", "撑不住了", "崩溃", NULL };
static const char * const s_trig_listen[] = {
    "想跟你说", "陪我说", "听我说", "没人听", "说说话",
    "说会话", "聊聊天", "聊会天", "随便聊聊",
    "陪我", "陪着你", "陪着我", "一直陪", "跟我说",
    "聊一会", "聊聊", "想说", "倾诉", "吐槽", NULL };
static const char * const s_trig_relax[] = {
    "放松一下", "放松", "深呼吸", "calm down", "relax",
    "教我放松", "平静", "静一静", NULL };
static const char * const s_trig_rest[] = {
    "小憩", "休息几分钟", "闭会儿眼", "power nap",
    "眯一会", "眯一会儿", "睡一会", "睡一会儿", "休息一下", NULL };
static const char * const s_trig_sleep[] = {
    "晚安", "睡觉", "想睡了", "good night", "睡了",
    "困了", "好困", "要睡了", "我先睡了", NULL };
static const char * const s_trig_morning[] = {
    "早上好", "早安", "good morning", "起床了", "刚醒", NULL };
static const char * const s_trig_fall[] = {
    "摔倒", "摔了", "掉了", "跌", "摔下来", NULL };

static const skill_trigger_t s_triggers[] = {
    { s_trig_stress,  BUILTIN_EMOTION_STRESS_RELIEF   },
    { s_trig_listen,  BUILTIN_EMOTION_LISTEN_COMFORT  },
    { s_trig_relax,   BUILTIN_EMOTION_RELAX_GUIDE     },
    { s_trig_rest,    BUILTIN_EMOTION_SHORT_REST      },
    { s_trig_sleep,   BUILTIN_EMOTION_BEDTIME_SOOTHE  },
    { s_trig_morning, BUILTIN_EMOTION_MORNING_STARTUP },
    { s_trig_fall,    BUILTIN_EMOTION_FALL_UPRIGHT    },
};
#endif /* CONFIG_AI_AGENT_EMOTION_TOY */

char* skill_loader_match_trigger(const char* user_text)
{
    if (!user_text || user_text[0] == '\0') return NULL;

#ifdef CONFIG_AI_AGENT_EMOTION_TOY
    for (size_t i = 0; i < sizeof(s_triggers)/sizeof(s_triggers[0]); i++) {
        for (const char * const * t = s_triggers[i].triggers; *t; t++) {
            if (strcasestr(user_text, *t)) {
                syslog(LOG_INFO,
                    "[%s] trigger match: skill %zu\n", TAG, i);
                return strdup(s_triggers[i].content);
            }
        }
    }
#endif
    (void)user_text;
    return NULL;
}

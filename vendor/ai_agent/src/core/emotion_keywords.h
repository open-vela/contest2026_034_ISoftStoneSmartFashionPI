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

/* Shared emotion keyword tables.
 *
 * Two call sites must agree on the user-utterance -> expression mapping:
 *   - agent_loop.c (classic ASR+LLM+TTS path): build_emotion_hint() injects
 *     a "call set_face" hint so the LLM changes the face via the tool.
 *   - agent_loop.c (E2E path): emotion_face_for_text() applies the face
 *     client-side in the route_e2e branch, without a set_face tool call
 *     (a tool call would set s_tool_ran and force the hybrid router onto
 *     the slow agent path for what is really chitchat).
 *
 * static-const so each includer gets its own flash copy — no link-time
 * symbol collisions and zero DRAM cost.
 */

#ifndef __EMOTION_KEYWORDS_H
#define __EMOTION_KEYWORDS_H

static const char * const kw_compliment[] = {
    "可爱","聪明","厉害","真棒","好贴心","好喜欢","好棒","好厉害",
    "太强","真好看","好温柔","好有趣","真好玩","你好强","好暖",
    "好能干","真聪明","真厉害","好乖","好可爱","真有趣","好聪明",
    NULL };

static const char * const kw_stressed[] = {
    "压力好大","好难受","好焦虑","不开心","好难过","好伤心","想哭",
    "好烦","崩溃","撑不住","好无助","好害怕","担心","焦虑",
    "心情不好","好糟糕","心累","好沮丧","忧郁","烦躁",
    NULL };

static const char * const kw_tired[] = {
    "好疲惫","好困","没精神","好累","想休息","没力气","好乏",
    "累死了","困死了","好疲倦","没劲儿","提不起劲","好懒",
    NULL };

static const char * const kw_done[] = {
    "搞定了","完成了","成功了","设置好了","弄好了","好了",
    NULL };

static const char * const kw_encourage[] = {
    "加油","别难过","没事的","你可以的","支持你","你最棒",
    "别灰心","会好起来的","振作","别担心",
    NULL };

static const char * const kw_confused_user[] = {
    "听不懂","没听明白","什么意思","没听清","再说一遍",
    "不知道你在说什么","不明白","什么意思啊","你说什么",
    NULL };

/* [L1 2026-08-17] 端侧表情统一后新增：原 16 场景提示词中真正由用户
 * 语句驱动、而旧表未覆盖的两类。sick/worried/standby/waiting/sleepy
 * 属系统态（报错/跌倒/待机/静音按钮），不由语句触发表维护。 */
static const char * const kw_quiet[] = {
    "不想说话","安静陪我","安静点","别说话了",
    NULL };

static const char * const kw_playful[] = {
    "举高高","转个圈","摇一摇","跳个舞","击掌",
    NULL };

#endif /* __EMOTION_KEYWORDS_H */

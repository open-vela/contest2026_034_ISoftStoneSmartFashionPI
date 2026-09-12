#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_alert_audio.py — 用火山引擎大模型 TTS 生成设备预设告警音频。

流程（零第三方依赖，仅需 Python 3.8+）：
  1. 调火山 TTS HTTP API (v1) 合成文本，voice_type 优先 E2E 同款
     zh_female_vv_jupiter_bigtts，不可用自动退回 zh_female_vv_uranus_bigtts
     （两者同族、听感一致，见 agent_config.h 注释）
  2. 完整解析返回的 WAV（真正的 RIFF chunk 遍历）
  3. 重写为标准 44 字节头 WAV（fmt 恰好 16 字节、data 紧跟 36 偏移）
     —— 这是设备端 tool_emotion_audio.c wav_read_header() 的硬性要求，
     官网/在线转换器带 LIST 元数据块的文件会数据错位播成噪声
  4. 按设备解析器逻辑复核输出，打印格式表

用法：
  export VOLC_APPID=xxx VOLC_TOKEN=xxx   # 与设备配置 volc_appkey/volc_token 相同
  python3 tools/gen_alert_audio.py           # 生成 out/*.wav
  python3 tools/gen_alert_audio.py --selftest  # 无凭证自检（本地正弦波）
  cp out/*.wav /mnt/sd/AUDIO/                # 拷贝到 SD 卡（文件名小写精确匹配）

之后设备无需重新编译即可生效。
"""

import base64
import json
import math
import os
import struct
import sys
import time
import urllib.request
import uuid

# ── 可按需修改的文案 ────────────────────────────────────────────────
ALERTS = [
    ("no_network.wav",   "咦，我连不上网络了，请帮我检查一下WiFi吧"),
    ("low_battery.wav",  "我的电量快用完了，记得带我回去充电哦"),
    ("wifi_timeout.wav", "哎呀，网络断开了，请帮我检查一下网络吧"),
]

# E2E 音色优先；API 不支持时逐个退退（同族听感一致）
VOICE_CANDIDATES = [
    "zh_female_vv_jupiter_bigtts",   # E2E 默认音色
    "zh_female_vv_uranus_bigtts",    # WS TTS 默认音色（同族）
]

API_URL = "https://openspeech.bytedance.com/api/v1/tts"
CLUSTER = "volcano_tts"              # 大模型音色集群

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")


# ── 火山 TTS HTTP API ──────────────────────────────────────────────
def tts_fetch(appid, token, text, voice, retries=3):
    """合成一段文本，返回 WAV 原始字节。"""
    body = {
        "app": {"appid": appid, "token": token, "cluster": CLUSTER},
        "user": {"uid": "gen_alert_audio"},
        "audio": {
            "voice_type": voice,
            "encoding": "wav",
            "speed_ratio": 1.0,
            "volume_ratio": 1.0,
            "pitch_ratio": 1.0,
        },
        "request": {
            "reqid": uuid.uuid4().hex,
            "text": text,
            "operation": "query",
        },
    }
    req = urllib.request.Request(
        API_URL,
        data=json.dumps(body).encode("utf-8"),
        headers={
            # 火山 v1 API 特有格式：Bearer 与 token 之间是分号不是空格
            "Authorization": "Bearer;" + token,
            "Content-Type": "application/json",
        },
        method="POST",
    )

    last_err = None
    for attempt in range(1, retries + 1):
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                result = json.loads(resp.read().decode("utf-8"))
            if result.get("code") != 3000:
                # 音色不存在/无权限等：直接失败让上层退回下一个音色
                raise RuntimeError(
                    "API code=%s message=%s" % (result.get("code"),
                                                result.get("message")))
            return base64.b64decode(result["data"])
        except Exception as e:  # noqa: BLE001 — 网络抖动统一重试
            last_err = e
            if attempt < retries:
                time.sleep(2 * attempt)
    raise RuntimeError("TTS 请求失败（%s，重试 %d 次）: %s"
                       % (voice, retries, last_err))


# ── WAV 解析（完整 chunk 遍历，兼容 fmt 18/40 字节与 LIST 块）──────
def parse_wav(wav):
    """返回 (audio_format, channels, sample_rate, bits, pcm_bytes)。"""
    if wav[0:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise ValueError("不是 RIFF/WAVE 文件")

    fmt = None
    data = None
    off = 12
    while off + 8 <= len(wav):
        cid = wav[off:off + 4]
        (size,) = struct.unpack("<I", wav[off + 4:off + 8])
        body = wav[off + 8:off + 8 + size]
        if cid == b"fmt " and fmt is None:
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data" and data is None:
            data = body
        off += 8 + size + (size & 1)   # chunk 按 2 字节对齐

    if fmt is None or data is None:
        raise ValueError("WAV 缺少 fmt/data chunk")
    audio_format, ch, rate, _, _, bits = fmt
    return audio_format, ch, rate, bits, data


def write_canonical(path, pcm, channels, rate, bits):
    """重写为设备兼容的标准 44 字节头 WAV。"""
    byte_rate = rate * channels * bits // 8
    block_align = channels * bits // 8
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                 byte_rate, block_align, bits)
    hdr += b"data" + struct.pack("<I", len(pcm))
    with open(path, "wb") as f:
        f.write(hdr + pcm)


def verify(path):
    """按设备端 wav_read_header() 的逻辑复核：data 必须在 36 偏移。"""
    with open(path, "rb") as f:
        hdr = f.read(44)
        f.seek(0, os.SEEK_END)
        total = f.tell()

    ok = True
    def chk(cond, msg):
        nonlocal ok
        if not cond:
            ok = False
            print("  [FAIL] %s" % msg)

    chk(hdr[0:4] == b"RIFF" and hdr[8:12] == b"WAVE", "RIFF/WAVE 魔数")
    chk(hdr[12:16] == b"fmt ", "fmt 块在 12 偏移")
    (fmt_size,) = struct.unpack("<I", hdr[16:20])
    chk(fmt_size == 16, "fmt 块恰为 16 字节（实际 %d）" % fmt_size)
    chk(hdr[36:40] == b"data", "data 块恰在 36 偏移（设备解析器硬性要求）")

    audio_format, ch, rate, _, _, bits = struct.unpack("<HHIIHH", hdr[20:36])
    (data_size,) = struct.unpack("<I", hdr[40:44])
    chk(audio_format == 1, "编码为 PCM（实际 %d）" % audio_format)
    chk(bits == 16, "16-bit（实际 %d）" % bits)
    chk(ch == 1, "单声道（实际 %d）" % ch)
    chk(data_size == total - 44, "data 长度与文件一致")

    dur = data_size / (rate * ch * bits / 8)
    print("  %s  %d Hz / %d ch / %d bit / %.1f s / %d 字节  %s"
          % (os.path.basename(path), rate, ch, bits, dur, data_size,
             "OK" if ok else "** 不兼容 **"))
    return ok


# ── 主流程 ─────────────────────────────────────────────────────────
def pick_voice(appid, token):
    """用第一条文案试音色，返回第一个可用的。"""
    probe = ALERTS[0][1][:8]
    for voice in VOICE_CANDIDATES:
        try:
            tts_fetch(appid, token, probe, voice, retries=1)
            print("[音色] 使用 %s" % voice)
            return voice
        except Exception as e:  # noqa: BLE001
            print("[音色] %s 不可用：%s" % (voice, e))
    raise SystemExit("所有候选音色均不可用，请检查 VOLC_APPID/VOLC_TOKEN")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    if "--selftest" in sys.argv:
        # 无凭证自检：本地正弦波验证 44 字节头生成与设备解析兼容
        rate, dur = 24000, 0.5
        pcm = b"".join(
            struct.pack("<h", int(12000 * math.sin(2 * math.pi * 440 * i / rate)))
            for i in range(int(rate * dur)))
        path = os.path.join(OUT_DIR, "_selftest.wav")
        write_canonical(path, pcm, 1, rate, 16)
        print("[自检] 写入 %s" % path)
        sys.exit(0 if verify(path) else 1)

    appid = os.environ.get("VOLC_APPID", "")
    token = os.environ.get("VOLC_TOKEN", "")
    if not appid or not token:
        raise SystemExit(
            "缺少凭证。请先：\n"
            "  export VOLC_APPID=<火山语音应用 appid>\n"
            "  export VOLC_TOKEN=<访问 token>\n"
            "（与设备上 volc_appkey / volc_token 配置项相同）")

    voice = pick_voice(appid, token)

    all_ok = True
    for fname, text in ALERTS:
        print("[合成] %s <- \"%s\"" % (fname, text))
        wav = tts_fetch(appid, token, text, voice)
        audio_format, ch, rate, bits, pcm = parse_wav(wav)
        if audio_format != 1 or bits != 16:
            raise SystemExit("API 返回非 16-bit PCM（fmt=%d bits=%d），无法处理"
                             % (audio_format, bits))
        path = os.path.join(OUT_DIR, fname)
        write_canonical(path, pcm, ch, rate, bits)
        all_ok &= verify(path)

    print()
    if all_ok:
        print("全部通过。拷贝到 SD 卡：")
        print("  cp %s/*.wav /mnt/sd/AUDIO/" % OUT_DIR)
    else:
        print("存在不兼容项，请检查上方 FAIL 原因。")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()

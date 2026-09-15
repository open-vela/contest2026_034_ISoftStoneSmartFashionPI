# Volcengine E2E Voice - 问题处理手册与使用指南

> 平台：ESP32-S3 (openvela / NuttX) 智能手表
> 更新日期：2026-08-05
> 源文件：`apps/examples/volc_e2e_voice/volc_e2e_voice_main.c`

---

## 一、使用指南

### 1.1 编译与部署

```bash
# 1. 确保 defconfig 中启用
CONFIG_EXAMPLES_VOLC_E2E_VOICE=y

# 2. 编译
cd cmake_out/esp32s3-touch-amoled_openvela
cmake --build . --target volc_e2e_voice

# 3. 刷入设备
# (通过标准 openvela 刷入流程)
```

### 1.2 运行命令

```bash
# 基本运行（使用默认 speaker）
nsh> volc_e2e_voice

# 指定 speaker
nsh> volc_e2e_voice -s zh_male_yunzhou_jupiter_bigtts

# 指定系统角色
nsh> volc_e2e_voice -r "你是一个幽默的智能助手"

# Ctrl+C 退出
```

### 1.3 预期日志流程

```
[volc_e2e] Starting E2E voice client
[volc_e2e] TLS connected to openspeech.bytedance.com:443
[volc_e2e] WS upgrade OK
[volc_e2e] ConnectionStarted
[volc_e2e] SessionStarted
[volc_e2e] Voice session active. Press Ctrl+C to exit.
[volc_e2e] capture ready: 4 buffers
[volc_e2e] send thread started (server VAD)
[volc_e2e] audio: 1 frames, peak=275         ← 开始发送音频
[volc_e2e] ASRInfo (user started speaking)    ← 服务端检测到语音
[volc_e2e] ASR: 你好                          ← 识别结果
[volc_e2e] ASREnded (user stopped speaking)   ← 用户停止说话
[volc_e2e] TTSSentenceStart                   ← TTS 开始
[volc_e2e] Chat: 你好呀...                    ← 文字回复
[volc_e2e] ChatEnded
[volc_e2e] TTSSentenceEnd
[volc_e2e] WAV: 344188 bytes (~4084ms)        ← 音频写入
[volc_e2e] playraw returned: 0                ← 播放成功
[volc_e2e] TTSEnded
[volc_e2e] cooldown ended, rebuilding capture ← 冷却结束，重建采集
[volc_e2e] audio: 101 frames, peak=216        ← 下一轮开始
```

### 1.4 核心参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 采集采样率 | 16000 Hz | 麦克风输入 |
| 采集声道 | 2ch (stereo) | I2S 硬件要求 |
| 采集位深 | 16 bit | PCM signed LE |
| 播放采样率 | 24000 Hz | TTS 输出 |
| 播放声道 | 2ch (stereo) | I2S 硬件要求 |
| 播放位深 | 16 bit | PCM signed LE |
| TTS 缓冲区 | 2MB | 最大 ~22 秒回复 |
| 静音帧速率 | ~10 fps | 100ms/帧，匹配采集速率 |
| 冷却时间 | 播放时长 + 1s | 防止回音拾取 |

---

## 二、已解决问题记录

### 问题 1：TLS Mutex 阻塞导致 DialogAudioIdleTimeoutError

| 项目 | 内容 |
|------|------|
| **现象** | recv_thread 持有 TLS mutex 期间阻塞 60s，send_thread 无法发送音频帧，服务器报 `DialogAudioIdleTimeoutError` |
| **根因** | `mbedtls_ssl_read` 在非阻塞 socket 上返回 `WANT_READ` 时，原代码无限等待且不释放 mutex |
| **修复** | 切换非阻塞 socket + `WANT_READ` 时释放 mutex 并 sleep 10ms 后重试 |
| **关键代码** | `tls_connect` 末尾调用 `mbedtls_net_set_nonblock()`；`tls_read_all` 中 WANT_READ 分支 `pthread_mutex_unlock + usleep(10000) + lock` |

### 问题 2：capture 设备重启失败 (EIO)

| 项目 | 内容 |
|------|------|
| **现象** | `capture_stop + capture_start` 无效，设备停在 DRAINING/OPEN 状态 |
| **根因** | NuttX audio 驱动的 `AUDIOIOC_START` 在 DRAINING/OPEN 状态被静默忽略 |
| **修复** | 改用 `capture_deinit + capture_init`（完全 close + open 重建），加上 `mq_unlink` 清理旧消息队列 |
| **关键代码** | cooldown 结束时调用 `capture_deinit → usleep(200ms) → capture_init → capture_start` |

### 问题 3：MQ 残留导致 capture 重建失败

| 项目 | 内容 |
|------|------|
| **现象** | capture_deinit 只 `mq_close` 未 `mq_unlink`，旧 MQ 留在命名空间，新 `mq_open` 失败 |
| **根因** | POSIX 消息队列需要 unlink 才能从命名空间移除 |
| **修复** | `capture_deinit` 和 `capture_init` 中都添加 `mq_unlink(mq_name)` |

### 问题 4：nxplayer 复用导致播放无声

| 项目 | 内容 |
|------|------|
| **现象** | `playfile` 返回 0 但没有声音输出，第一轮有声音第二轮无声音 |
| **根因** | 复用 nxplayer 实例导致内部状态异常；`nxplayer_stop` 在播放中调用会破坏状态 |
| **修复** | 参照 ai_agent 的成功模式：每次播放创建新 nxplayer + `setdevice("/dev/audio/pcm0")` + `playraw`；播放完成后 `stop + release` |
| **关键代码** | `playback_stop` 中：`nxplayer_create → setdevice → playraw`；cooldown 结束时：`nxplayer_stop → release` |

### 问题 5：capture_deinit 破坏 I2S 状态导致播放无声

| 项目 | 内容 |
|------|------|
| **现象** | 在 `playback_stop` 中调用 `capture_deinit` 后，nxplayer 无法播放 |
| **根因** | `AUDIOIOC_RELEASE + close(fd)` 把 I2S 外设置于 nxplayer 无法使用的状态 |
| **修复** | `playback_stop` 中改用 `capture_stop`（只 AUDIOIOC_STOP，停止 DMA 但不释放设备），完整重建由冷却结束时处理 |

### 问题 6：send_thread 与 recv_thread 的 capture 竞争

| 项目 | 内容 |
|------|------|
| **现象** | recv_thread 调用 `capture_deinit` 时，send_thread 的 EIO retry 又重新 `capture_init`，导致 I2S 被 send_thread 重新占用，playfile 无法播放 |
| **根因** | cooldown 期间 send_thread 的 capture 错误重试逻辑与 recv_thread 的播放逻辑冲突 |
| **修复** | cooldown 期间（`s_playing || s_cooldown_active`）send_thread 遇到 EIO 不重试，只发静音 |

### 问题 7：send_thread 1 秒阻塞导致帧发送不及时

| 项目 | 内容 |
|------|------|
| **现象** | `capture_read_frame` 的 `mq_timedreceive` 超时 1 秒，期间无帧发送给服务器，导致 `DialogAudioIdleTimeoutError` |
| **根因** | 1 秒超时太长，cooldown 结束后 send_thread 仍在阻塞，无法及时发送音频 |
| **修复** | 超时从 1 秒降到 100ms；在 `capture_read_frame` 前添加快速状态检查（`s_playing || s_cooldown_active`）；ETIMEDOUT 时发送静音帧 |

### 问题 8：WebSocket PING/PONG 保活缺失

| 项目 | 内容 |
|------|------|
| **现象** | 服务器发送 PING 帧但客户端不回复 PONG，网关因空闲超时断开连接 |
| **根因** | `ws_recv_frame` 只处理 CLOSE 和 BINARY 帧，PING(0x9) 被忽略 |
| **修复** | 新增 `ws_send_pong` 函数（带掩码的 PONG 帧），recv_thread 收到 PING 时自动回复 |

### 问题 9：静音帧发送速率过高 (50fps)

| 项目 | 内容 |
|------|------|
| **现象** | 静音帧以 50fps（20ms/帧）发送，而实际 capture 帧率仅 ~8fps（128ms/buffer），服务器可能因帧率不匹配而断开连接 |
| **根因** | 所有静音发送路径的 `usleep(20000)` 导致过高的帧发送速率 |
| **修复** | 所有静音路径改为 `usleep(100000)`（100ms/帧 ≈ 10fps），与 capture 帧率匹配 |

### 问题 10：长对话后服务器断开连接

| 项目 | 内容 |
|------|------|
| **现象** | 第二轮对话 ChatEnded 后服务器立即 RST 连接（`ssl_read error: -0x004c`），不发 TTSSentenceEnd |
| **根因** | 服务器端行为（可能的会话时长限制或 TTS 生成失败），客户端在 ASREnded 后仍发送音频帧可能干扰服务器 TTS 生成 |
| **修复** | 1) ASREnded 时立即设置 `s_playing = true` 停止发送音频；2) 添加自动重连机制（连接断开后 2 秒重建 TLS + WS + Session） |

---

## 三、已知待解决问题

### 问题 A：TTS 长回复 PCM 缓冲区溢出

| 项目 | 内容 |
|------|------|
| **现象** | 长回复（如"从 1 数到 25"）音频截断，日志出现多次 `PCM buffer overflow` |
| **根因** | `TTS_BUF_CAP = 2MB` 不够长回复的音频数据（24kHz/16bit/stereo ≈ 96KB/s，2MB ≈ 22s） |
| **临时方案** | 后续对 AI 回复做文字字数限制，避免超长回复 |
| **永久方案** | 增大 `TTS_BUF_CAP` 到 4MB，或改为流式播放（边收边播） |

### 问题 B：TTSSentenceStart 时序异常

| 项目 | 内容 |
|------|------|
| **现象** | `TTSSentenceStart` 在 Chat tokens 流到一半时才出现（而非在 TTSResponse 之前） |
| **影响** | TTS 音频帧可能在 `s_playing` 变为 true 之前就开始到达，部分音频未被 `playback_write` 接收 |
| **临时方案** | 暂不处理，后续优化 |

---

## 四、关键设计决策

### 4.1 I2S 总线共享管理

ESP32-S3 的 I2S 总线被 capture 和 playback 共享，需要半双工管理：

```
采集阶段: capture_start → 持续发送音频帧
    ↓ (ASREnded)
播放阶段: capture_stop → nxplayer playraw → 发送静音帧
    ↓ (cooldown 结束)
重建阶段: capture_deinit → capture_init → capture_start
```

**核心规则**：
- 播放时 capture DMA 必须停止（`capture_stop`）
- 不能在播放期间调用 `capture_deinit`（会破坏 I2S 状态）
- 播放完成后需等待冷却期（播放时长 + 1s）再重建 capture

### 4.2 nxplayer 生命周期

每次播放创建新实例（参照 ai_agent 的成功模式）：

```
playback_stop (TTSSentenceEnd 时调用):
  1. capture_stop()           ← 释放 I2S
  2. nxplayer_create()        ← 新实例
  3. nxplayer_setdevice("/dev/audio/pcm0")
  4. nxplayer_playraw()       ← 开始播放
  5. 设置 cooldown

cooldown 结束 (send_thread):
  1. nxplayer_stop()          ← 停止播放
  2. nxplayer_release()       ← 释放资源
  3. capture_deinit()         ← 关闭采集设备
  4. capture_init()           ← 重新打开
  5. capture_start()          ← 启动 DMA
```

### 4.3 TLS 并发安全

mbedtls_ssl_context 不是线程安全的，需要 mutex 保护：

- `s_tls_mutex`：所有 TLS 读写操作必须持有
- 非阻塞 socket：`WANT_READ/WANT_WRITE` 时释放 mutex + sleep + 重试
- recv_thread 和 send_thread 通过 mutex 串行化 TLS 操作

### 4.4 自动重连机制

连接断开后自动恢复：

```
主循环检测 s_running = false
    ↓ (非用户退出 s_interrupted = false)
等待 2 秒
    ↓
清理旧资源 (threads, TLS, capture, playback)
    ↓
生成新 session_id
    ↓
重建连接 (TLS → WS → StartConnection → StartSession)
    ↓
重启 send_thread
    ↓
继续对话
```

---

## 五、调试技巧

### 5.1 日志关键字速查

| 日志 | 含义 |
|------|------|
| `playraw returned: 0` | 播放启动成功 |
| `playraw returned: -N` | 播放启动失败 |
| `PCM buffer overflow` | TTS 音频数据超出 2MB 缓冲区 |
| `PING received` | WebSocket PING 保活正常 |
| `cooldown ended, rebuilding capture` | 冷却结束，开始重建采集 |
| `capture read error: -5` | 采集设备 EIO（通常因 capture_deinit 导致） |
| `ssl_read error: -0x004c` | 连接被服务器 RST |
| `ssl_write error: -0x004e` | 网络发送失败（连接已断） |
| `connection lost, reconnecting` | 自动重连触发 |
| `reconnected successfully` | 重连成功 |
| `DialogAudioIdleTimeoutError` | 服务器未收到足够音频帧 |
| `audio: N frames, peak=X` | 每 100 帧报告一次音频状态 |

### 5.2 常见错误码

| 错误码 | 含义 | 可能原因 |
|--------|------|----------|
| `-0x004c` | MBEDTLS_ERR_SSL_CONN_RESET | 服务器/网关 RST 连接 |
| `-0x004e` | MBEDTLS_ERR_NET_SEND_FAILED | TCP send 失败 |
| `-5` (EIO) | 通用 I/O 错误 | 设备状态异常或 capture 被 deinit |
| `-110` (ETIMEDOUT) | 超时 | mq_timedreceive 无数据 |

### 5.3 帧计数分析

程序退出时打印 `sent N audio + M silence frames`：

- **audio/silence 比例**：正常约 1:2（说话时间短于冷却时间）
- **audio 帧数异常低**：capture 可能在某阶段失败
- **silence 帧数异常高**：cooldown 过长或 send_thread 阻塞

---

## 六、依赖与配置

### 6.1 编译依赖

- mbedtls (TLS 连接)
- cJSON (JSON 解析)
- nxplayer + /dev/audio/pcm0 (音频播放)
- NuttX audio driver + /dev/audio/pcm_in0 (音频采集)

### 6.2 Kconfig 选项

```
CONFIG_EXAMPLES_VOLC_E2E_VOICE=y
```

### 6.3 硬编码凭证（测试用）

```c
#define VOLC_APP_ID   "9022462685"
#define VOLC_TOKEN     "<your_access_token>"
#define DEFAULT_SPEAKER "zh_female_vv_jupiter_bigtts"
```

### 6.4 WebSocket 端点

```
wss://openspeech.bytedance.com/api/v3/realtime/dialogue
```

---

## 七、版本历史

| 日期 | 变更 |
|------|------|
| 2026-07-24 | 初始版本：基础 E2E 语音客户端 |
| 2026-08-01 | 修复 TLS mutex 阻塞 + 非阻塞 socket |
| 2026-08-03 | 修复 capture 重建（close+open + mq_unlink） |
| 2026-08-04 | 修复 nxplayer 播放（每次新建实例 + playraw） |
| 2026-08-05 | 修复 I2S 竞争 + 添加 PING/PONG + 降低静音帧率 + 自动重连 |

# Volcengine End-to-End Realtime Voice Client

This example demonstrates the Volcengine Realtime API (S2S model) for voice-to-voice dialogue using a single WebSocket connection.

## Overview

Traditional voice interaction uses three separate services:
- ASR (Speech-to-Text)
- LLM (Language Model)
- TTS (Text-to-Speech)

This end-to-end approach combines all three into a single service, reducing latency significantly.

## Architecture

```
Traditional: Microphone → [ASR] → Text → [LLM] → Text → [TTS] → Speaker
             ~3s          ~0.3s    ~7-11s   ~0.5s    ~2-3s

End-to-End:  Microphone → [Single WebSocket] → Speaker
             Continuous bidirectional streaming
```

## Features

- **Low Latency**: Single connection for ASR + LLM + TTS
- **Voice Activity Detection**: Client-side silence detection
- **Interruption Support**: Automatic playback stop on user speech
- **Error Handling**: Automatic reconnection on failures
- **Push-to-Talk Mode**: EndASR signal on silence detection

## Configuration

Before running, configure the Volcengine credentials:

```bash
# In NuttX shell
set_config volc_appkey <your_app_id>
set_config volc_token <your_access_token>
```

## Building

Enable the example in NuttX configuration:

```
CONFIG_EXAMPLES_VOLC_E2E_VOICE=y
```

## Running

```bash
# Basic usage
volc_e2e_voice

# With custom speaker
volc_e2e_voice -s zh_male_yunzhou_jupiter_bigtts

# With system role
volc_e2e_voice -r "你是一个智能助手，说话简洁明了。"
```

## Audio Parameters

- **Input**: PCM 16kHz 16bit mono (from microphone)
- **Output**: PCM 24kHz 16bit mono (to speaker)

## Protocol Details

### WebSocket Endpoint
```
wss://openspeech.bytedance.com/api/v3/realtime/dialogue
```

### Authentication Headers
```
X-Api-App-ID: <app_id>
X-Api-Access-Key: <access_token>
X-Api-Resource-Id: volc.speech.dialog
X-Api-App-Key: PlgvMymc7f3tQnJ6
```

### Message Types
- 0x10: Full-client request (text events)
- 0x20: Audio-only request (audio data)
- 0x90: Full-server response (text events)
- 0xB0: Audio-only response (audio data)

### Client Events
- StartConnection (1): Initialize connection
- StartSession (100): Start dialogue session
- TaskRequest (200): Send audio data
- EndASR (400): Signal end of speech
- FinishSession (102): End session
- FinishConnection (2): Close connection

### Server Events
- ConnectionStarted (50): Connection established
- SessionStarted (150): Session started
- ASRInfo (450): User started speaking
- ASRResponse (451): Speech recognition result
- ASREnded (459): User stopped speaking
- ChatResponse (550): LLM response text
- TTSResponse (352): Synthesized audio data
- TTSEnded (359): Audio synthesis complete

## VAD Configuration

Client-side Voice Activity Detection parameters:

```c
#define VAD_SILENCE_THRESHOLD  300   // Peak amplitude threshold
#define VAD_SILENCE_FRAMES     15    // 15 * 20ms = 300ms silence
#define VAD_SPEECH_MIN_FRAMES  3     // Minimum frames to confirm speech
```

## Error Handling

- TLS handshake failure: Retry 3 times
- WebSocket disconnection: Automatic reconnection
- Server errors: Print error code and message
- Audio capture failure: Skip current frame

## Limitations

- Model versions: Only O2.0 (1.2.1.1) and SC2.0 (2.2.0.0)
- Language: Primarily Chinese, limited English support
- Concurrency: 60 QPM, 100K TPM
- Context length: 12K tokens (2.0 versions)
- Timeout: 10 minutes idle auto-disconnect

## Dependencies

- mbedtls (TLS)
- cJSON (JSON parsing)
- media_player (audio playback)
- NuttX audio driver (audio capture)

## License

Apache License 2.0

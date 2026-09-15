/****************************************************************************
 * apps/examples/mimo_stream_voice_test/mimo_stream_voice_test_main.c
 *
 * Streaming MiMo ASR + TTS voice pipeline test on ESP32-S3:
 *   1. Record microphone audio to SD card (WAV, 16 kHz stereo 16-bit).
 *   2. Send to MiMo ASR with streaming enabled (SSE) -> get text incrementally.
 *   3. Feed text to MiMo TTS with streaming (SSE) -> get PCM chunks incrementally.
 *   4. Accumulate PCM, save as WAV, and play back through speaker.
 *
 * Key difference from mimo_voice_test: uses SSE streaming for both ASR and TTS,
 * showing incremental results as they arrive from the server.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>
#include <system/nxplayer.h>
#include <system/nxrecorder.h>

#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "infra/vela_tls.h"
#include "infra/config_store.h"

#include "cJSON.h"
#include "mbedtls/base64.h"

/****************************************************************************
 * ai_agent bridge (private symbols)
 ****************************************************************************/

extern int mimo_asr_register(void);
extern int mimo_tts_register(void);

/****************************************************************************
 * Configuration
 ****************************************************************************/

#define TAG                    "mimo_stream"

#define WAV_HDR_SIZE           44

#define ASR_SAMPLE_RATE        16000
#define ASR_CHANNELS           2
#define ASR_BITS               16

#define TTS_SAMPLE_RATE        24000
#define TTS_CHANNELS           2
#define TTS_BITS               16

#define DEFAULT_RECORD_SECONDS 2
#define DEFAULT_LANGUAGE       "zh"
#define DEFAULT_VOICE          "\xe5\x86\xb0\xe7\xb3\x96"  /* "冰糖" */

#define DEFAULT_INPUT_WAV      "/mnt/sd/stream_input.wav"
#define DEFAULT_OUTPUT_WAV     "/mnt/sd/stream_output.wav"

#define TTS_PCM_BUF_CAP        (512 * 1024)
#define ASR_TEXT_CAP           1024

#define RECORD_DEVICE          "/dev/audio/pcm_in0"
#define PLAYBACK_DEVICE        "/dev/audio/pcm0"

#define DATA_ROOT              "/data"
#define AGENT_DATA_ROOT        "/data/ai_agent"
#define AGENT_CONFIG_JSON      "/data/ai_agent/config/config.json"

#define CFG_KEY_MIMO_API_KEY   "mimo_api_key"
#define CFG_KEY_MIMO_VOICE     "mimo_voice"
#define CFG_KEY_MIMO_ASR_LANG  "mimo_asr_lang"

/* MiMo API endpoints */
#define MIMO_API_HOST          "api.xiaomimimo.com"
#define MIMO_API_PORT          "443"
#define MIMO_API_PATH          "/v1/chat/completions"
#define MIMO_ASR_MODEL         "mimo-v2.5-asr"
#define MIMO_TTS_MODEL         "mimo-v2.5-tts"

/****************************************************************************
 * Runtime options
 ****************************************************************************/

struct test_opts_s
{
  const char *api_key;
  const char *lang;
  const char *voice;
  const char *input_wav;
  const char *output_wav;
  int         record_seconds;
  int         skip_record;
};

/****************************************************************************
 * Helpers
 ****************************************************************************/

static void usage(const char *prog)
{
  printf("Usage: %s [options]\n"
         "  -k <key>    Set MiMo API key\n"
         "  -l <lang>   ASR language: auto|zh|en (default: %s)\n"
         "  -v <voice>  TTS voice name (default: 冰糖)\n"
         "  -t <secs>   Record duration (default: %d)\n"
         "  -i <path>   Input WAV path (default: %s)\n"
         "  -o <path>   Output WAV path (default: %s)\n"
         "  -s          Skip recording\n"
         "  -h          Show this help\n",
         prog, DEFAULT_LANGUAGE, DEFAULT_RECORD_SECONDS,
         DEFAULT_INPUT_WAV, DEFAULT_OUTPUT_WAV);
}

static void write_le32(unsigned char *p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void write_le16(unsigned char *p, uint16_t v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void build_wav_header(unsigned char *hdr, uint32_t pcm_len,
                             uint32_t sr, uint16_t ch, uint16_t bits)
{
  uint32_t byte_rate   = sr * ch * (bits / 8);
  uint16_t block_align = ch * (bits / 8);
  uint32_t riff_size   = pcm_len + 36;

  memcpy(hdr + 0, "RIFF", 4);
  write_le32(hdr + 4, riff_size);
  memcpy(hdr + 8, "WAVE", 4);

  memcpy(hdr + 12, "fmt ", 4);
  write_le32(hdr + 16, 16);
  write_le16(hdr + 20, 1);
  write_le16(hdr + 22, ch);
  write_le32(hdr + 24, sr);
  write_le32(hdr + 28, byte_rate);
  write_le16(hdr + 32, block_align);
  write_le16(hdr + 34, bits);

  memcpy(hdr + 36, "data", 4);
  write_le32(hdr + 40, pcm_len);
}

/* Read entire file using fstat (avoids FAT lseek issues) */
static int read_file_all(const char *path,
                         unsigned char **out_buf, size_t *out_len)
{
  struct stat st;
  unsigned char *buf = NULL;
  size_t total = 0;
  int fd = -1;
  int retries;

  for (retries = 0; retries < 3; retries++)
    {
      if (fd >= 0) { close(fd); fd = -1; }
      if (buf) { free(buf); buf = NULL; }

      if (retries > 0)
        {
          sync();
          usleep(500 * 1000);
        }

      fd = open(path, O_RDONLY);
      if (fd < 0) { continue; }

      if (fstat(fd, &st) != 0 || st.st_size <= 0) { continue; }

      buf = malloc((size_t)st.st_size);
      if (!buf) { close(fd); return -ENOMEM; }

      total = 0;
      while (total < (size_t)st.st_size)
        {
          size_t chunk = (size_t)st.st_size - total;
          if (chunk > 4096) chunk = 4096;
          ssize_t n = read(fd, buf + total, chunk);
          if (n <= 0) break;
          total += (size_t)n;
        }
      close(fd);
      fd = -1;

      if (total == (size_t)st.st_size)
        {
          *out_buf = buf;
          *out_len = (size_t)st.st_size;
          return 0;
        }
    }

  if (fd >= 0) close(fd);
  if (buf) free(buf);
  return -EIO;
}

/* Extract PCM from WAV buffer (reserved for future use) */
static int __attribute__((unused)) wav_extract_pcm(
    const unsigned char *wav, size_t wav_len,
    const unsigned char **pcm_out, size_t *pcm_len_out)
{
  if (wav_len < WAV_HDR_SIZE || memcmp(wav, "RIFF", 4) != 0
      || memcmp(wav + 8, "WAVE", 4) != 0)
    return -EPROTO;

  size_t off = 12;
  while (off + 8 <= wav_len)
    {
      uint32_t sz = (uint32_t)wav[off + 4] | ((uint32_t)wav[off + 5] << 8)
                  | ((uint32_t)wav[off + 6] << 16) | ((uint32_t)wav[off + 7] << 24);
      if (memcmp(wav + off, "data", 4) == 0)
        {
          size_t data_off = off + 8;
          if (data_off + sz > wav_len) sz = (uint32_t)(wav_len - data_off);
          *pcm_out = wav + data_off;
          *pcm_len_out = sz;
          return 0;
        }
      off += 8 + sz;
    }
  return -EPROTO;
}

static int ensure_data_writable(void)
{
  mkdir(DATA_ROOT, 0755);

  int fd = open(DATA_ROOT "/.stream_probe", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0)
    {
      close(fd);
      unlink(DATA_ROOT "/.stream_probe");
      return 0;
    }

  if (mount(NULL, DATA_ROOT, "tmpfs", 0, NULL) != 0)
    return -EIO;

  fd = open(DATA_ROOT "/.stream_probe", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -EIO;
  close(fd);
  unlink(DATA_ROOT "/.stream_probe");
  return 0;
}

static int do_record(const char *path, int seconds)
{
  printf("[%s] recording %d s to %s ...\n", TAG, seconds, path);

  unlink(path);
  sync();

  FAR struct nxrecorder_s *rec = nxrecorder_create();
  if (!rec) return -ENOMEM;

  int ret = nxrecorder_setdevice(rec, RECORD_DEVICE);
  if (ret < 0) { nxrecorder_release(rec); return ret; }

  ret = nxrecorder_recordinternal(rec, path, AUDIO_FMT_PCM,
                                  ASR_CHANNELS, ASR_BITS, ASR_SAMPLE_RATE, 0);
  if (ret < 0) { nxrecorder_release(rec); return ret; }

  for (int i = 0; i < seconds; i++)
    {
      sleep(1);
      printf("  .. %d/%d s\n", i + 1, seconds);
    }

  usleep(500 * 1000);
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  nxrecorder_stop(rec);
#endif
  sleep(1);
  nxrecorder_release(rec);
  sleep(2);
  sync();

  printf("[%s] recording saved\n", TAG);
  return 0;
}

static int write_wav_file(const char *path, const unsigned char *pcm,
                          size_t pcm_len, uint32_t sr, uint16_t ch, uint16_t bits)
{
  unsigned char hdr[WAV_HDR_SIZE];
  build_wav_header(hdr, (uint32_t)pcm_len, sr, ch, bits);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -errno;

  write(fd, hdr, WAV_HDR_SIZE);

  size_t remain = pcm_len;
  const unsigned char *p = pcm;
  while (remain > 0)
    {
      size_t chunk = remain > 4096 ? 4096 : remain;
      ssize_t w = write(fd, p, chunk);
      if (w <= 0) { close(fd); return -EIO; }
      p += w;
      remain -= (size_t)w;
    }

  close(fd);
  return 0;
}

static int do_playback(const char *path, size_t pcm_bytes)
{
  printf("[%s] playing %s (%zu PCM bytes) ...\n", TAG, path, pcm_bytes);

  FAR struct nxplayer_s *pl = nxplayer_create();
  if (!pl) return -ENOMEM;

  int ret = nxplayer_setdevice(pl, PLAYBACK_DEVICE);
  if (ret < 0) return ret;

  ret = nxplayer_playraw(pl, path, AUDIO_FMT_PCM, 0,
                         TTS_CHANNELS, TTS_BITS, TTS_SAMPLE_RATE, 0);
  if (ret < 0) return ret;

  uint32_t byte_rate  = TTS_SAMPLE_RATE * TTS_CHANNELS * (TTS_BITS / 8);
  uint32_t est_ms     = (uint32_t)(pcm_bytes * 1000ULL / byte_rate);
  uint32_t timeout_ms = est_ms + 3000;
  uint32_t waited_ms = 0;

  while (waited_ms < timeout_ms)
    {
      usleep(200 * 1000);
      waited_ms += 200;
      if (pl->state == 0) break;
    }

  return 0;
}

/****************************************************************************
 * Streaming ASR via SSE
 ****************************************************************************/

/* Build ASR request JSON with stream:true */
static char *build_stream_asr_body(const unsigned char *wav_data,
                                   size_t wav_len, const char *lang)
{
  static const char prefix[] = "data:audio/wav;base64,";
  size_t prefix_len = sizeof(prefix) - 1;
  size_t b64_cap = ((wav_len + 2) / 3) * 4 + 1;
  size_t combined_cap = prefix_len + b64_cap;

  char *data_url = malloc(combined_cap);
  if (!data_url) return NULL;

  memcpy(data_url, prefix, prefix_len);
  size_t b64_len = 0;
  int b64_ret = mbedtls_base64_encode(
      (unsigned char *)(data_url + prefix_len), b64_cap,
      &b64_len, wav_data, wav_len);
  if (b64_ret != 0) { free(data_url); return NULL; }
  data_url[prefix_len + b64_len] = '\0';

  cJSON *root = cJSON_CreateObject();
  if (!root) { free(data_url); return NULL; }

  cJSON_AddStringToObject(root, "model", MIMO_ASR_MODEL);
  cJSON_AddBoolToObject(root, "stream", 1);  /* KEY: enable streaming */

  cJSON *messages = cJSON_AddArrayToObject(root, "messages");
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "role", "user");

  cJSON *content = cJSON_CreateArray();
  cJSON *item = cJSON_CreateObject();
  cJSON_AddStringToObject(item, "type", "input_audio");

  cJSON *input_audio = cJSON_AddObjectToObject(item, "input_audio");
  cJSON_AddStringToObject(input_audio, "data", data_url);

  cJSON_AddItemToArray(content, item);
  cJSON_AddItemToObject(msg, "content", content);
  cJSON_AddItemToArray(messages, msg);

  cJSON *asr_opts = cJSON_AddObjectToObject(root, "asr_options");
  cJSON_AddStringToObject(asr_opts, "language", lang);

  char *json_str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  free(data_url);

  return json_str;
}

/* SSE callback context for ASR */
typedef struct {
  char   *text_buf;
  size_t  text_cap;
  size_t  text_len;
  int     chunk_count;
} asr_sse_ctx_t;

/* Parse SSE event and accumulate ASR text */
static int asr_sse_callback(const char *data, size_t data_len, void *user)
{
  asr_sse_ctx_t *ctx = (asr_sse_ctx_t *)user;
  ctx->chunk_count++;

  /* Parse JSON: {"choices":[{"delta":{"content":"..."}}]} */
  char *json_copy = malloc(data_len + 1);
  if (!json_copy) return -1;
  memcpy(json_copy, data, data_len);
  json_copy[data_len] = '\0';

  cJSON *root = cJSON_Parse(json_copy);
  free(json_copy);
  if (!root) return 0;  /* Skip malformed chunks */

  cJSON *choices = cJSON_GetObjectItem(root, "choices");
  if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0)
    {
      cJSON *first = cJSON_GetArrayItem(choices, 0);
      cJSON *delta = cJSON_GetObjectItem(first, "delta");
      if (delta)
        {
          cJSON *content = cJSON_GetObjectItem(delta, "content");
          if (content && cJSON_IsString(content) && content->valuestring[0])
            {
              /* Append to text buffer */
              size_t add_len = strlen(content->valuestring);
              size_t remain = ctx->text_cap - ctx->text_len - 1;
              if (add_len > remain) add_len = remain;
              if (add_len > 0)
                {
                  memcpy(ctx->text_buf + ctx->text_len,
                         content->valuestring, add_len);
                  ctx->text_len += add_len;
                  ctx->text_buf[ctx->text_len] = '\0';

                  /* Print incremental result */
                  printf("[%s] ASR chunk %d: +\"%.40s\"\n",
                         TAG, ctx->chunk_count, content->valuestring);
                }
            }
        }
    }

  cJSON_Delete(root);
  return 0;
}

/* Run streaming ASR */
static int do_stream_asr(const unsigned char *wav_buf, size_t wav_len,
                         const char *api_key, const char *lang,
                         char *text_out, size_t text_cap)
{
  printf("[%s] building streaming ASR request (%zu bytes) ...\n", TAG, wav_len);

  char *body = build_stream_asr_body(wav_buf, wav_len, lang);
  if (!body) return -ENOMEM;

  vela_header_t hdrs[] = {
      {"api-key", api_key},
      {NULL, NULL}
  };

  asr_sse_ctx_t ctx = {
      .text_buf = text_out,
      .text_cap = text_cap,
      .text_len = 0,
      .chunk_count = 0
  };
  text_out[0] = '\0';

  /* Diagnostic buffer for raw response */
  char *diag = malloc(2048);
  if (diag) diag[0] = '\0';

  printf("[%s] sending streaming ASR request ...\n", TAG);
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  int status = vela_https_sse_post(
      MIMO_API_HOST, MIMO_API_PORT, MIMO_API_PATH,
      hdrs, body, strlen(body),
      asr_sse_callback, &ctx, 60,
      diag, diag ? 2048 : 0);

  free(body);

  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long ms = (t1.tv_sec - t0.tv_sec) * 1000
          + (t1.tv_nsec - t0.tv_nsec) / 1000000;

  printf("[%s] ASR streaming done: HTTP %d, %d chunks, %ld ms\n",
         TAG, status, ctx.chunk_count, ms);

  /* Diagnostic: if no SSE events, log raw response */
  if (ctx.chunk_count == 0 && diag && diag[0])
    {
      printf("[%s] ASR raw response (first 512): %.512s\n", TAG, diag);
    }
  free(diag);

  if (status != 200) return -EIO;
  if (text_out[0] == '\0') return -ENODATA;

  printf("[%s] ASR final: \"%s\"\n", TAG, text_out);
  return 0;
}

/****************************************************************************
 * Streaming TTS via SSE
 ****************************************************************************/

/* Build TTS request JSON with stream:true, format:pcm16 */
static char *build_stream_tts_body(const char *text, const char *voice)
{
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;

  cJSON_AddStringToObject(root, "model", MIMO_TTS_MODEL);
  cJSON_AddBoolToObject(root, "stream", 1);

  cJSON *messages = cJSON_AddArrayToObject(root, "messages");

  /* User message (optional style hint) */
  cJSON *user_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(user_msg, "role", "user");
  cJSON_AddStringToObject(user_msg, "content", "");
  cJSON_AddItemToArray(messages, user_msg);

  /* Assistant message with text to synthesize */
  cJSON *asst_msg = cJSON_CreateObject();
  cJSON_AddStringToObject(asst_msg, "role", "assistant");
  cJSON_AddStringToObject(asst_msg, "content", text);
  cJSON_AddItemToArray(messages, asst_msg);

  /* Audio config */
  cJSON *audio = cJSON_AddObjectToObject(root, "audio");
  cJSON_AddStringToObject(audio, "format", "pcm16");
  cJSON_AddStringToObject(audio, "voice", voice);

  char *json_str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json_str;
}

/* SSE callback context for TTS */
typedef struct {
  unsigned char *pcm_buf;
  size_t         pcm_cap;
  size_t         pcm_len;
  int            chunk_count;
} tts_sse_ctx_t;

/* Parse SSE event and accumulate TTS PCM */
static int tts_sse_callback(const char *data, size_t data_len, void *user)
{
  tts_sse_ctx_t *ctx = (tts_sse_ctx_t *)user;
  ctx->chunk_count++;

  /* Parse JSON: {"choices":[{"delta":{"audio":{"data":"base64..."}}}]} */
  char *json_copy = malloc(data_len + 1);
  if (!json_copy) return -1;
  memcpy(json_copy, data, data_len);
  json_copy[data_len] = '\0';

  cJSON *root = cJSON_Parse(json_copy);
  free(json_copy);
  if (!root) return 0;

  cJSON *choices = cJSON_GetObjectItem(root, "choices");
  if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0)
    {
      cJSON *first = cJSON_GetArrayItem(choices, 0);
      cJSON *delta = cJSON_GetObjectItem(first, "delta");
      if (delta)
        {
          cJSON *audio = cJSON_GetObjectItem(delta, "audio");
          if (audio)
            {
              cJSON *b64_data = cJSON_GetObjectItem(audio, "data");
              if (b64_data && cJSON_IsString(b64_data))
                {
                  /* Decode base64 PCM */
                  size_t b64_len = strlen(b64_data->valuestring);
                  size_t dec_cap = (b64_len / 4) * 3 + 4;
                  unsigned char *dec_buf = malloc(dec_cap);
                  if (dec_buf)
                    {
                      size_t dec_len = 0;
                      int ret = mbedtls_base64_decode(
                          dec_buf, dec_cap, &dec_len,
                          (const unsigned char *)b64_data->valuestring, b64_len);
                      if (ret == 0 && dec_len > 0)
                        {
                          size_t remain = ctx->pcm_cap - ctx->pcm_len;
                          if (dec_len > remain) dec_len = remain;
                          memcpy(ctx->pcm_buf + ctx->pcm_len, dec_buf, dec_len);
                          ctx->pcm_len += dec_len;

                          printf("[%s] TTS chunk %d: +%zu bytes PCM\n",
                                 TAG, ctx->chunk_count, dec_len);
                        }
                      free(dec_buf);
                    }
                }
            }
        }
    }

  cJSON_Delete(root);
  return 0;
}

/* Run streaming TTS */
static int do_stream_tts(const char *text, const char *api_key,
                         const char *voice,
                         unsigned char *pcm_out, size_t pcm_cap,
                         size_t *pcm_out_len)
{
  printf("[%s] building streaming TTS request (\"%s\") ...\n", TAG, text);

  char *body = build_stream_tts_body(text, voice);
  if (!body) return -ENOMEM;

  vela_header_t hdrs[] = {
      {"api-key", api_key},
      {NULL, NULL}
  };

  tts_sse_ctx_t ctx = {
      .pcm_buf = pcm_out,
      .pcm_cap = pcm_cap,
      .pcm_len = 0,
      .chunk_count = 0
  };

  /* Diagnostic buffer for raw response */
  char *diag = malloc(2048);
  if (diag) diag[0] = '\0';

  printf("[%s] sending streaming TTS request ...\n", TAG);
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  int status = vela_https_sse_post(
      MIMO_API_HOST, MIMO_API_PORT, MIMO_API_PATH,
      hdrs, body, strlen(body),
      tts_sse_callback, &ctx, 120,
      diag, diag ? 2048 : 0);

  free(body);

  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long ms = (t1.tv_sec - t0.tv_sec) * 1000
          + (t1.tv_nsec - t0.tv_nsec) / 1000000;

  printf("[%s] TTS streaming done: HTTP %d, %d chunks, %ld ms, %zu bytes PCM\n",
         TAG, status, ctx.chunk_count, ms, ctx.pcm_len);

  /* Diagnostic: if no SSE events, log raw response */
  if (ctx.chunk_count == 0 && diag && diag[0])
    {
      printf("[%s] TTS raw response (first 512): %.512s\n", TAG, diag);
    }
  free(diag);

  *pcm_out_len = ctx.pcm_len;

  if (status != 200) return -EIO;
  if (ctx.pcm_len == 0) return -ENODATA;

  return 0;
}

/****************************************************************************
 * Main pipeline
 ****************************************************************************/

static int run_pipeline(const struct test_opts_s *opts)
{
  int ret;

  /* Step 1: init config */
  ret = ensure_data_writable();
  if (ret < 0) return ret;

  config_store_init();

  if (opts->api_key && opts->api_key[0])
    {
      claw_config_set(CFG_KEY_MIMO_API_KEY, opts->api_key);
    }
  if (opts->lang) claw_config_set(CFG_KEY_MIMO_ASR_LANG, opts->lang);
  if (opts->voice) claw_config_set(CFG_KEY_MIMO_VOICE, opts->voice);

  /* Get API key */
  char api_key[128];
  if (claw_config_get(CFG_KEY_MIMO_API_KEY, api_key, sizeof(api_key)) != 0
      || api_key[0] == '\0')
    {
      printf("[%s] API key not configured. Pass -k <key>.\n", TAG);
      return -ENOENT;
    }

  /* Register backends (for compatibility, though we call SSE directly) */
  mimo_asr_register();
  mimo_tts_register();

  /* Step 2: record */
  if (!opts->skip_record)
    {
      ret = do_record(opts->input_wav, opts->record_seconds);
      if (ret < 0) return ret;
    }

  /* Step 3: read WAV */
  unsigned char *wav_buf = NULL;
  size_t wav_len = 0;
  ret = read_file_all(opts->input_wav, &wav_buf, &wav_len);
  if (ret < 0) return ret;
  printf("[%s] loaded %zu bytes WAV\n", TAG, wav_len);

  /* Step 4: streaming ASR */
  char text[ASR_TEXT_CAP];
  text[0] = '\0';
  printf("[%s] === Streaming ASR ===\n", TAG);
  ret = do_stream_asr(wav_buf, wav_len, api_key, opts->lang,
                      text, sizeof(text));
  free(wav_buf);
  wav_buf = NULL;

  if (ret < 0)
    {
      printf("[%s] ASR failed: %d\n", TAG, ret);
      return ret;
    }

  printf("\n==============================\n");
  printf("ASR result: %s\n", text);
  printf("==============================\n\n");

  /* Step 5: streaming TTS */
  unsigned char *pcm_out = malloc(TTS_PCM_BUF_CAP);
  if (!pcm_out) return -ENOMEM;

  size_t pcm_out_len = 0;
  printf("[%s] === Streaming TTS ===\n", TAG);
  ret = do_stream_tts(text, api_key, opts->voice,
                      pcm_out, TTS_PCM_BUF_CAP, &pcm_out_len);
  if (ret < 0 || pcm_out_len == 0)
    {
      free(pcm_out);
      return ret < 0 ? ret : -ENODATA;
    }

  /* Convert mono PCM to stereo (I2S requires 2-channel) */
  {
    int16_t *mono = (int16_t *)pcm_out;
    size_t mono_samples = pcm_out_len / sizeof(int16_t);
    size_t stereo_bytes = mono_samples * 2 * sizeof(int16_t);

    if (stereo_bytes > TTS_PCM_BUF_CAP)
      {
        free(pcm_out);
        return -ENOMEM;
      }

    for (size_t i = mono_samples; i > 0; i--)
      {
        int16_t s = mono[i - 1];
        int16_t *frame = (int16_t *)(pcm_out + (i - 1) * 2 * sizeof(int16_t));
        frame[0] = s;
        frame[1] = s;
      }
    pcm_out_len = stereo_bytes;
  }

  printf("[%s] TTS: %zu PCM bytes (~%.2fs stereo @ %dHz)\n",
         TAG, pcm_out_len,
         (double)pcm_out_len / (double)(TTS_SAMPLE_RATE * (TTS_BITS / 8) * TTS_CHANNELS),
         TTS_SAMPLE_RATE);

  /* Step 6: save and play */
  ret = write_wav_file(opts->output_wav, pcm_out, pcm_out_len,
                       TTS_SAMPLE_RATE, TTS_CHANNELS, TTS_BITS);
  free(pcm_out);
  if (ret < 0) return ret;

  printf("[%s] wrote %s\n", TAG, opts->output_wav);

  ret = do_playback(opts->output_wav, pcm_out_len);
  if (ret < 0) return ret;

  printf("[%s] pipeline OK\n", TAG);
  return 0;
}

/****************************************************************************
 * Entry point
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct test_opts_s opts;
  memset(&opts, 0, sizeof(opts));
  opts.lang           = DEFAULT_LANGUAGE;
  opts.voice          = DEFAULT_VOICE;
  opts.input_wav      = DEFAULT_INPUT_WAV;
  opts.output_wav     = DEFAULT_OUTPUT_WAV;
  opts.record_seconds = DEFAULT_RECORD_SECONDS;

  int c;
  while ((c = getopt(argc, argv, "k:l:v:t:i:o:sh")) != -1)
    {
      switch (c)
        {
          case 'k': opts.api_key = optarg;                break;
          case 'l': opts.lang = optarg;                   break;
          case 'v': opts.voice = optarg;                  break;
          case 't': opts.record_seconds = atoi(optarg);   break;
          case 'i': opts.input_wav = optarg;              break;
          case 'o': opts.output_wav = optarg;             break;
          case 's': opts.skip_record = 1;                 break;
          case 'h':
          default:
            usage(argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }

  if (opts.record_seconds <= 0 || opts.record_seconds > 60)
    {
      printf("[%s] -t must be in [1,60]\n", TAG);
      return 1;
    }

  printf("====================================\n");
  printf("MiMo Streaming Voice Test\n");
  printf("====================================\n");
  printf("  language    : %s\n", opts.lang);
  printf("  voice       : %s\n", opts.voice);
  printf("  record secs : %d\n", opts.record_seconds);
  printf("  input wav   : %s\n", opts.input_wav);
  printf("  output wav  : %s\n", opts.output_wav);
  printf("  skip record : %s\n", opts.skip_record ? "yes" : "no");
  printf("====================================\n");

  int rc = run_pipeline(&opts);
  if (rc != 0)
    {
      printf("[%s] test failed: %d\n", TAG, rc);
      return 1;
    }

  return 0;
}

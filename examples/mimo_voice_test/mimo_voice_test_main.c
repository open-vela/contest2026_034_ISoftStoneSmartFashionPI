/****************************************************************************
 * apps/examples/mimo_voice_test/mimo_voice_test_main.c
 *
 * End-to-end voice pipeline test on ESP32-S3:
 *   1. Record microphone audio to SD card (WAV, 16 kHz mono 16-bit).
 *   2. Send PCM to MiMo (Xiaomi) ASR -> get text.
 *   3. Feed text to MiMo TTS -> get PCM (24 kHz mono 16-bit).
 *   4. Save synthesized audio to SD card as WAV.
 *   5. Play the WAV back through the speaker.
 *
 * Reuses the existing MiMo ASR/TTS backends in packages/ai_agent and the
 * NuttX nxrecorder / nxplayer APIs. Compatible with the running nsh shell
 * (invoked as a builtin command).
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

/****************************************************************************
 * ai_agent bridge (private symbols not exposed via public headers)
 ****************************************************************************/

extern int mimo_asr_register(void);
extern int mimo_tts_register(void);
extern int config_store_init(void);
extern int claw_config_set(const char *key, const char *value);
extern int claw_config_get(const char *key, char *buf, size_t buf_size);

/****************************************************************************
 * Configuration
 ****************************************************************************/

#define TAG                    "mimo_test"

#define WAV_HDR_SIZE           44

#define ASR_SAMPLE_RATE        16000
#define ASR_CHANNELS           2   /* I2S bus always 2-ch; mono → distortion */
#define ASR_BITS               16

#define TTS_SAMPLE_RATE        24000
#define TTS_CHANNELS           2   /* I2S bus always 2-ch; mono → distortion */
#define TTS_BITS               16

#define DEFAULT_RECORD_SECONDS 2
#define DEFAULT_LANGUAGE       "zh"
#define DEFAULT_VOICE          "\xe5\x86\xb0\xe7\xb3\x96"  /* "冰糖" (UTF-8) */

#define DEFAULT_INPUT_WAV      "/mnt/sd/mimo_input.wav"
#define DEFAULT_OUTPUT_WAV     "/mnt/sd/mimo_output.wav"

#define TTS_PCM_BUF_CAP        (512 * 1024)  /* ~10 s @ 24 kHz mono 16-bit */
#define ASR_TEXT_CAP           1024

/* nxrecorder/nxplayer *_setdevice() call open() directly on the given
 * string, so absolute device paths are required (the nsh "device pcm_in0"
 * command internally prepends "/dev/audio/" -- the C API does not).
 */
#define RECORD_DEVICE          "/dev/audio/pcm_in0"
#define PLAYBACK_DEVICE        "/dev/audio/pcm0"

/* Persistent-data root mirrors packages/ai_agent/include/agent_config.h:
 *   AGENT_DATA_DIR = CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR (default "/data/ai_agent")
 * We only need the top-level mount point to be writable; the ai_agent
 * config_store_init() will mkdir the rest.
 */
#define DATA_ROOT              "/data"
#define AGENT_DATA_ROOT        "/data/ai_agent"
#define AGENT_CONFIG_JSON      "/data/ai_agent/config/config.json"

/* Config store keys (kept in sync with packages/ai_agent/include/agent_config.h) */
#define CFG_KEY_MIMO_API_KEY   "mimo_api_key"
#define CFG_KEY_MIMO_VOICE     "mimo_voice"
#define CFG_KEY_MIMO_ASR_LANG  "mimo_asr_lang"

/****************************************************************************
 * Runtime options
 ****************************************************************************/

struct test_opts_s
{
  const char *api_key;         /* optional; if non-NULL, saved to config */
  const char *lang;            /* ASR language (auto/zh/en) */
  const char *voice;           /* TTS voice name */
  const char *input_wav;       /* recorded WAV path */
  const char *output_wav;      /* synthesized WAV path */
  int         record_seconds;  /* microphone capture duration */
  int         skip_record;     /* use existing input_wav instead of recording */
};

/****************************************************************************
 * Helpers
 ****************************************************************************/

static void usage(const char *prog)
{
  printf("Usage: %s [options]\n"
         "  -k <key>    Set MiMo API key (persisted to config store)\n"
         "  -l <lang>   ASR language: auto | zh | en (default: %s)\n"
         "  -v <voice>  TTS voice name (default: 冰糖)\n"
         "  -t <secs>   Record duration in seconds (default: %d)\n"
         "  -i <path>   Input WAV path (default: %s)\n"
         "  -o <path>   Output WAV path (default: %s)\n"
         "  -s          Skip recording, use existing input WAV\n"
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
  write_le32(hdr + 16, 16);        /* fmt chunk size */
  write_le16(hdr + 20, 1);         /* PCM */
  write_le16(hdr + 22, ch);
  write_le32(hdr + 24, sr);
  write_le32(hdr + 28, byte_rate);
  write_le16(hdr + 32, block_align);
  write_le16(hdr + 34, bits);

  memcpy(hdr + 36, "data", 4);
  write_le32(hdr + 40, pcm_len);
}

/* Read entire file into a newly-allocated buffer.
 * On success, *out_buf points to malloc'd data (caller must free()),
 * and *out_len holds the size in bytes.
 *
 * On ESP32-S3 NuttX/FAT, lseek(SEEK_END) followed by lseek(SEEK_SET)
 * can leave the internal FAT cluster cursor stuck at EOF, causing
 * read() to return 0 even though the directory entry has the correct
 * file size.  We avoid lseek(SEEK_END) entirely and use fstat() to
 * get the file size, then read sequentially from offset 0.
 */
static int read_file_all(const char *path,
                         unsigned char **out_buf, size_t *out_len)
{
  struct stat st;
  unsigned char *buf = NULL;
  size_t total = 0;
  off_t sz = 0;
  int fd = -1;
  int ret = -EIO;
  int retries;

  for (retries = 0; retries < 3; retries++)
    {
      if (fd >= 0)
        {
          close(fd);
          fd = -1;
        }

      if (buf)
        {
          free(buf);
          buf = NULL;
        }

      if (retries > 0)
        {
          printf("[%s] retry %d reading %s after sync ...\n",
                 TAG, retries, path);
          sync();
          usleep(500 * 1000);
        }

      fd = open(path, O_RDONLY);
      if (fd < 0)
        {
          printf("[%s] open '%s' failed: %s\n", TAG, path, strerror(errno));
          ret = -errno;
          continue;
        }

      /* Use fstat() to get file size — avoid lseek(SEEK_END) which
       * corrupts the FAT cluster cursor on this platform.
       */

      if (fstat(fd, &st) != 0 || st.st_size <= 0)
        {
          printf("[%s] fstat failed or empty: %s (size=%lld)\n",
                 TAG, path, (long long)st.st_size);
          ret = -EINVAL;
          continue;
        }

      sz = st.st_size;

      /* File position is 0 on freshly-opened fd — no need to lseek. */

      buf = (unsigned char *)malloc((size_t)sz);
      if (!buf)
        {
          close(fd);
          printf("[%s] alloc %ld bytes failed\n", TAG, (long)sz);
          return -ENOMEM;
        }

      total = 0;
      while (total < (size_t)sz)
        {
          size_t chunk = (size_t)sz - total;
          if (chunk > 4096)
            {
              chunk = 4096;
            }
          ssize_t n = read(fd, buf + total, chunk);
          if (n <= 0)
            {
              printf("[%s] read() returned %zd at offset %zu/%ld (errno=%d/%s)\n",
                     TAG, n, total, (long)sz, errno, strerror(errno));
              break;
            }
          total += (size_t)n;
        }

      if (total == (size_t)sz)
        {
          close(fd);
          *out_buf = buf;
          *out_len = (size_t)sz;
          return 0;
        }

      printf("[%s] short read: %zu / %ld (attempt %d)\n",
             TAG, total, (long)sz, retries + 1);
      ret = -EIO;
    }

  if (fd >= 0)
    {
      close(fd);
    }

  if (buf)
    {
      free(buf);
    }

  printf("[%s] failed to read %s after %d attempts\n",
         TAG, path, retries);
  return ret;
}

/* Locate the 'data' chunk in a WAV buffer and return pointer/length.
 * Returns 0 on success, -EPROTO if the buffer is not a RIFF/WAVE file or
 * no 'data' chunk was found (typical when nxrecorder writes raw PCM
 * because the filename extension was not exactly ".wav", or when a
 * previous recording was truncated before the header could be patched).
 */
static int wav_extract_pcm(const unsigned char *wav, size_t wav_len,
                           const unsigned char **pcm_out, size_t *pcm_len_out)
{
  if (wav_len < WAV_HDR_SIZE
      || memcmp(wav, "RIFF", 4) != 0
      || memcmp(wav + 8, "WAVE", 4) != 0)
    {
      return -EPROTO;
    }

  size_t off = 12;
  while (off + 8 <= wav_len)
    {
      const unsigned char *id = wav + off;
      uint32_t sz = (uint32_t)wav[off + 4]
                  | ((uint32_t)wav[off + 5] << 8)
                  | ((uint32_t)wav[off + 6] << 16)
                  | ((uint32_t)wav[off + 7] << 24);
      if (memcmp(id, "data", 4) == 0)
        {
          size_t data_off = off + 8;
          if (data_off + sz > wav_len)
            {
              sz = (uint32_t)(wav_len - data_off);
            }
          *pcm_out = wav + data_off;
          *pcm_len_out = sz;
          return 0;
        }
      off += 8 + sz;
    }

  return -EPROTO;
}

/* Pretty-print first 16 bytes for on-device diagnostics. */
static void dump_file_head(const unsigned char *buf, size_t len)
{
  size_t n = len < 16 ? len : 16;
  printf("[%s]   head[%zu]:", TAG, n);
  for (size_t i = 0; i < n; i++)
    {
      printf(" %02x", buf[i]);
    }
  printf("\n");
}

/****************************************************************************
 * Ensure the /data mount point is writable
 *
 * ai_agent stores its config JSON under /data/ai_agent/config/config.json.
 * When the board does not auto-mount a writable filesystem on /data (e.g.
 * user forgot to mount littlefs, or firmware boots straight into nsh),
 * config_store_init() silently succeeds because mkdir() returns EEXIST or
 * EROFS, but subsequent open(O_WRONLY|O_CREAT) fails and claw_config_set()
 * returns ERROR. That is exactly the symptom of this test failing right
 * after "API key stored".
 *
 * As a fallback we mount a tmpfs on /data if it is not already writable.
 * Note: tmpfs is volatile - the key will be lost across reboots. For
 * persistent storage, mount a littlefs partition on /data in nsh_init or
 * rc.sysinit.
 ****************************************************************************/

static int ensure_data_writable(void)
{
  /* Probe: try to create+write+delete a sentinel file under /data. */

  mkdir(DATA_ROOT, 0755);   /* harmless if it already exists */

  int fd = open(DATA_ROOT "/.mimo_probe",
                O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0)
    {
      close(fd);
      unlink(DATA_ROOT "/.mimo_probe");
      return 0;
    }

  int probe_errno = errno;
  printf("[%s] %s is not writable (errno=%d/%s), mounting tmpfs ...\n",
         TAG, DATA_ROOT, probe_errno, strerror(probe_errno));

  /* Fallback: mount tmpfs. Requires CONFIG_FS_TMPFS=y (already y in the
   * board defconfig). Data is lost on reboot; user must re-supply -k.
   */

  if (mount(NULL, DATA_ROOT, "tmpfs", 0, NULL) != 0)
    {
      printf("[%s] mount tmpfs on %s failed: %s\n",
             TAG, DATA_ROOT, strerror(errno));
      return -EIO;
    }

  /* Re-probe after mount. */

  fd = open(DATA_ROOT "/.mimo_probe",
            O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      printf("[%s] %s still not writable after mount: %s\n",
             TAG, DATA_ROOT, strerror(errno));
      return -EIO;
    }
  close(fd);
  unlink(DATA_ROOT "/.mimo_probe");

  printf("[%s] tmpfs mounted on %s (WARNING: volatile, lost on reboot)\n",
         TAG, DATA_ROOT);
  return 0;
}

/****************************************************************************
 * MiMo backend selection
 ****************************************************************************/

static int ensure_mimo_backends(void)
{
  /* Try selecting; if the backend has not been registered yet, register
   * it and retry. Register calls are cheap when the ai_agent voice
   * framework has not been initialized in this process context.
   */

  if (voice_asr_set_backend("mimo") != 0)
    {
      mimo_asr_register();
      if (voice_asr_set_backend("mimo") != 0)
        {
          printf("[%s] failed to select MiMo ASR backend\n", TAG);
          return -1;
        }
    }

  if (voice_tts_set_backend("mimo") != 0)
    {
      mimo_tts_register();
      if (voice_tts_set_backend("mimo") != 0)
        {
          printf("[%s] failed to select MiMo TTS backend\n", TAG);
          return -1;
        }
    }

  return 0;
}

/****************************************************************************
 * Recording
 ****************************************************************************/

static int do_record(const char *path, int seconds)
{
  int ret;

  printf("[%s] recording %d s to %s ...\n", TAG, seconds, path);

  /* Remove any previous file so nxrecorder starts clean, then flush
   * the FAT to ensure the directory entry is committed before we
   * create a new file with the same name.
   */

  unlink(path);
  sync();

  FAR struct nxrecorder_s *rec = nxrecorder_create();
  if (!rec)
    {
      printf("[%s] nxrecorder_create failed\n", TAG);
      return -ENOMEM;
    }

  ret = nxrecorder_setdevice(rec, RECORD_DEVICE);
  if (ret < 0)
    {
      printf("[%s] setdevice(%s) failed: %d\n", TAG, RECORD_DEVICE, ret);
      goto out;
    }

  ret = nxrecorder_recordinternal(rec, path, AUDIO_FMT_PCM,
                                  ASR_CHANNELS, ASR_BITS,
                                  ASR_SAMPLE_RATE, 0);
  if (ret < 0)
    {
      printf("[%s] recordinternal failed: %d\n", TAG, ret);
      goto out;
    }

  /* nxrecorder runs a background worker thread. Wait for the requested
   * duration, then request stop.
   */

  for (int i = 0; i < seconds; i++)
    {
      sleep(1);
      printf("  .. %d/%d s\n", i + 1, seconds);
    }

  /* Pre-stop quiescing: let the DMA queue drain naturally. */

  usleep(500 * 1000);

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  nxrecorder_stop(rec);
#endif

  /* Post-stop settle: worker thread patches the WAV header, fsync()s the
   * file, then closes fd/mq. Give it time to finish.   */

  sleep(1);

  /* Release the nxrecorder context to free its memory (struct + internal
   * allocations).  The record_thread already released its reference; this
   * releases the initial reference from nxrecorder_create(), bringing the
   * refcount to 0 and freeing the context.
   */

  nxrecorder_release(rec);
  rec = NULL;

  /* Wait for HPWORK to finish i2s_rx_worker and free the audio DMA
   * buffers.  Without this, read() on the recorded file can fail with
   * ENOMEM because the FAT driver can't allocate sector buffers.
   */

  sleep(2);

out:
  if (rec)
    {
      nxrecorder_release(rec);
    }

  /* Flush the FAT so the recorded file is fully visible to
   * subsequent reads.
   */

  sync();
  printf("[%s] recording saved to %s\n", TAG, path);
  return ret;
}

/****************************************************************************
 * Playback
 ****************************************************************************/

static int do_playback(const char *path, size_t pcm_bytes)
{
  printf("[%s] playing %s (%zu PCM bytes) ...\n", TAG, path, pcm_bytes);

  FAR struct nxplayer_s *pl = nxplayer_create();
  if (!pl)
    {
      printf("[%s] nxplayer_create failed\n", TAG);
      return -ENOMEM;
    }

  int ret = nxplayer_setdevice(pl, PLAYBACK_DEVICE);
  if (ret < 0)
    {
      printf("[%s] setdevice(%s) failed: %d\n", TAG, PLAYBACK_DEVICE, ret);
      return ret;
    }

  ret = nxplayer_playraw(pl, path, AUDIO_FMT_PCM, 0,
                         TTS_CHANNELS, TTS_BITS,
                         TTS_SAMPLE_RATE, 0);
  if (ret < 0)
    {
      printf("[%s] playraw failed: %d\n", TAG, ret);
      return ret;
    }

  /* Estimate playback duration from PCM byte count, then poll state
   * until the player returns to IDLE or the timeout expires.
   */

  uint32_t byte_rate  = TTS_SAMPLE_RATE * TTS_CHANNELS * (TTS_BITS / 8);
  uint32_t est_ms     = (uint32_t)(pcm_bytes * 1000ULL / byte_rate);
  uint32_t timeout_ms = est_ms + 3000;

  uint32_t waited_ms = 0;
  while (waited_ms < timeout_ms)
    {
      usleep(200 * 1000);
      waited_ms += 200;

      /* NXPLAYER_STATE_IDLE == 0 in nxplayer internal enum. */

      if (pl->state == 0)
        {
          break;
        }
    }

  printf("[%s] playback finished (waited %" PRIu32 " ms, est %" PRIu32 " ms)\n",
         TAG, waited_ms, est_ms);
  return 0;
}

/****************************************************************************
 * Save PCM as WAV
 ****************************************************************************/

static int write_wav_file(const char *path, const unsigned char *pcm,
                          size_t pcm_len, uint32_t sr, uint16_t ch,
                          uint16_t bits)
{
  unsigned char hdr[WAV_HDR_SIZE];
  build_wav_header(hdr, (uint32_t)pcm_len, sr, ch, bits);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      printf("[%s] open '%s' failed: %s\n", TAG, path, strerror(errno));
      return -errno;
    }

  ssize_t w = write(fd, hdr, WAV_HDR_SIZE);
  if (w != WAV_HDR_SIZE)
    {
      close(fd);
      printf("[%s] write header failed\n", TAG);
      return -EIO;
    }

  size_t remain = pcm_len;
  const unsigned char *p = pcm;
  while (remain > 0)
    {
      size_t chunk = remain > 4096 ? 4096 : remain;
      w = write(fd, p, chunk);
      if (w <= 0)
        {
          close(fd);
          printf("[%s] write pcm failed at %zu bytes\n",
                 TAG, pcm_len - remain);
          return -EIO;
        }
      p += (size_t)w;
      remain -= (size_t)w;
    }

  close(fd);
  return 0;
}

/****************************************************************************
 * Main pipeline
 ****************************************************************************/

static int run_pipeline(const struct test_opts_s *opts)
{
  int ret;

  /* Step 1: initialize config store and persist optional overrides. */

  ret = ensure_data_writable();
  if (ret < 0)
    {
      printf("[%s] cannot use %s for config; try:\n", TAG, AGENT_DATA_ROOT);
      printf("    nsh> mkdir /data\n");
      printf("    nsh> mount -t tmpfs none /data     # volatile\n");
      printf("  or mount a littlefs partition on /data for persistence.\n");
      return ret;
    }

  config_store_init();

  if (opts->api_key && opts->api_key[0])
    {
      int sret = claw_config_set(CFG_KEY_MIMO_API_KEY, opts->api_key);
      if (sret != 0)
        {
          printf("[%s] claw_config_set failed (ret=%d, errno=%d/%s)\n",
                 TAG, sret, errno, strerror(errno));
          printf("[%s] Check that %s is on a writable FS.\n",
                 TAG, AGENT_CONFIG_JSON);
          return -EIO;
        }
      printf("[%s] API key stored to %s\n", TAG, AGENT_CONFIG_JSON);
    }

  if (opts->lang && opts->lang[0])
    {
      claw_config_set(CFG_KEY_MIMO_ASR_LANG, opts->lang);
    }

  if (opts->voice && opts->voice[0])
    {
      claw_config_set(CFG_KEY_MIMO_VOICE, opts->voice);
    }

  /* Sanity check: API key must be present now. */

  char key_probe[8];
  if (claw_config_get(CFG_KEY_MIMO_API_KEY, key_probe, sizeof(key_probe)) != 0
      || key_probe[0] == '\0')
    {
      printf("[%s] MiMo API key not configured. Pass -k <key> once.\n", TAG);
      return -ENOENT;
    }

  /* Step 2: ensure MiMo backends are active. */

  ret = ensure_mimo_backends();
  if (ret < 0)
    {
      return ret;
    }

  /* Step 3: capture microphone audio (unless -s was given). */

  if (!opts->skip_record)
    {
      ret = do_record(opts->input_wav, opts->record_seconds);
      if (ret < 0)
        {
          printf("[%s] recording failed\n", TAG);
          return ret;
        }
    }
  else
    {
      printf("[%s] skipping recording, using %s\n", TAG, opts->input_wav);
    }

  /* Step 4: read WAV and extract PCM for ASR. */

  unsigned char *wav_buf = NULL;
  size_t wav_len = 0;
  ret = read_file_all(opts->input_wav, &wav_buf, &wav_len);
  if (ret < 0)
    {
      return ret;
    }
  printf("[%s] loaded %zu bytes from %s\n", TAG, wav_len, opts->input_wav);

  /* Check if the file is a valid WAV.  We pass the entire buffer
   * (including RIFF/WAVE header) to build_asr_request(), which detects
   * the WAV header and reuses the buffer in-place, saving ~78 KB on
   * ESP32-S3.  For raw PCM the backend builds its own WAV header. */
  {
    const unsigned char *pcm_data;
    size_t pcm_len;
    int is_wav = (wav_extract_pcm(wav_buf, wav_len, &pcm_data, &pcm_len) == 0);

    if (!is_wav)
      {
        printf("[%s] no WAV header, treating as raw PCM (%d Hz, %d ch, %d-bit)\n",
               TAG, ASR_SAMPLE_RATE, ASR_CHANNELS, ASR_BITS);
        dump_file_head(wav_buf, wav_len);
        pcm_len = wav_len;
      }

    printf("[%s] PCM payload: %zu bytes (~%.2f s @ %d Hz)\n",
           TAG, pcm_len,
           (double)pcm_len / (double)(ASR_SAMPLE_RATE * (ASR_BITS / 8) * ASR_CHANNELS),
           ASR_SAMPLE_RATE);
  }

  /* Step 5: ASR — pass the full buffer (WAV header + PCM). */

  char text[ASR_TEXT_CAP];
  text[0] = '\0';
  printf("[%s] running MiMo ASR ...\n", TAG);
  ret = voice_asr_recognize(wav_buf, wav_len, text, sizeof(text));
  free(wav_buf);
  wav_buf = NULL;

  if (ret < 0)
    {
      printf("[%s] ASR failed: %d (%s)\n", TAG, ret, strerror(-ret));
      return ret;
    }
  if (text[0] == '\0')
    {
      printf("[%s] ASR returned empty text\n", TAG);
      return -ENODATA;
    }
  printf("\n==============================\n");
  printf("ASR result: %s\n", text);
  printf("==============================\n\n");

  /* Step 6: TTS. */

  unsigned char *pcm_out = (unsigned char *)malloc(TTS_PCM_BUF_CAP);
  if (!pcm_out)
    {
      printf("[%s] alloc TTS buffer failed (%d bytes)\n",
             TAG, TTS_PCM_BUF_CAP);
      return -ENOMEM;
    }

  size_t pcm_out_len = 0;
  printf("[%s] running MiMo TTS ...\n", TAG);
  ret = voice_tts_speak(text, pcm_out, TTS_PCM_BUF_CAP / 2, &pcm_out_len);
  if (ret < 0 || pcm_out_len == 0)
    {
      free(pcm_out);
      printf("[%s] TTS failed: %d\n", TAG, ret);
      return ret < 0 ? ret : -ENODATA;
    }

  /* MiMo TTS returns mono PCM.  Duplicate each sample to both channels
   * because the I2S bus / ES8311 hardware produces distorted audio when
   * configured for mono (see debug log Bug #8). */

  {
    int16_t *mono = (int16_t *)pcm_out;
    size_t mono_samples = pcm_out_len / sizeof(int16_t);
    size_t stereo_bytes = mono_samples * 2 * sizeof(int16_t);

    if (stereo_bytes > TTS_PCM_BUF_CAP)
      {
        free(pcm_out);
        printf("[%s] stereo expansion exceeds buffer (%zu > %d)\n",
               TAG, stereo_bytes, TTS_PCM_BUF_CAP);
        return -ENOMEM;
      }

    /* Convert in-place from end to avoid overwrite */
    for (size_t i = mono_samples; i > 0; i--)
      {
        int16_t s = mono[i - 1];
        int16_t *frame = (int16_t *)(pcm_out + (i - 1) * 2 * sizeof(int16_t));
        frame[0] = s;  /* left */
        frame[1] = s;  /* right (duplicate) */
      }
    pcm_out_len = stereo_bytes;
  }

  printf("[%s] TTS produced %zu PCM bytes (~%.2f s @ %d Hz, stereo)\n",
         TAG, pcm_out_len,
         (double)pcm_out_len / (double)(TTS_SAMPLE_RATE * (TTS_BITS / 8) * TTS_CHANNELS),
         TTS_SAMPLE_RATE);

  /* Step 7: save synthesized audio as WAV on SD card. */

  ret = write_wav_file(opts->output_wav, pcm_out, pcm_out_len,
                       TTS_SAMPLE_RATE, TTS_CHANNELS, TTS_BITS);
  if (ret < 0)
    {
      free(pcm_out);
      return ret;
    }
  printf("[%s] wrote %s (%zu PCM bytes)\n",
         TAG, opts->output_wav, pcm_out_len);

  free(pcm_out);

  /* Step 8: playback. Use the WAV file so nxplayer streams from disk;
   * pcm_out_len is passed only to derive an approximate wait duration.
   */

  ret = do_playback(opts->output_wav, pcm_out_len);
  if (ret < 0)
    {
      return ret;
    }

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
      printf("[%s] -t must be in [1, 60], got %d\n",
             TAG, opts.record_seconds);
      return 1;
    }

  printf("====================================\n");
  printf("MiMo Voice End-to-End Test\n");
  printf("====================================\n");
  printf("  language      : %s\n", opts.lang);
  printf("  voice         : %s\n", opts.voice);
  printf("  record secs   : %d\n", opts.record_seconds);
  printf("  input   wav   : %s\n", opts.input_wav);
  printf("  output  wav   : %s\n", opts.output_wav);
  printf("  skip record   : %s\n", opts.skip_record ? "yes" : "no");
  printf("====================================\n");

  int rc = run_pipeline(&opts);
  if (rc != 0)
    {
      printf("[%s] test failed: %d\n", TAG, rc);
      return 1;
    }

  return 0;
}

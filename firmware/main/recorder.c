#include "recorder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "storage.h"
#if CONFIG_RP_REC_FORMAT_OPUS
#include "encoder/impl/esp_opus_enc.h"
#include "ogg_opus.h"
#else
#include "wav_writer.h"
#endif

#if CONFIG_RP_MIC_DUAL
#define CHANNELS 2  // left = mic with L/R to GND, right = mic with L/R to 3V3
#else
#define CHANNELS 1
#endif

#define FRAME_SAMPLES       320                        // 20 ms per I2S read and per Opus frame
#define FRAME_BYTES         (FRAME_SAMPLES * CHANNELS * sizeof(int16_t))
#define RINGBUF_SIZE        (128 * 1024)               // ~2 s of stereo PCM, absorbs storage stalls
#define SYNC_INTERVAL_FRAMES 250                       // fsync every 5 s so a power loss loses little
#define MAX_MARKS           256
#define MIC_WARMUP_FRAMES   (RECORDER_SAMPLE_RATE / 5)  // INMP441 needs ~85 ms to start; skip 200 ms
#define LIMIT_KNEE          16384.0f                   // soft limiting starts at -6 dBFS

#define EVT_CAPTURE_RUN  BIT0
#define EVT_CAPTURE_IDLE BIT1
#define EVT_WRITER_DONE  BIT2

static const char *TAG = "recorder";

static i2s_chan_handle_t s_rx;
static RingbufHandle_t s_ringbuf;
static EventGroupHandle_t s_events;

#if CONFIG_RP_REC_FORMAT_OPUS
#define FILE_EXT        "opus"
#define OPUS_PRE_SKIP   312  // encoder lookahead (6.5 ms) in 48 kHz samples
#define OPUS_MAX_PACKET 1276
static ogg_opus_t s_ogg;
static void *s_opus;
static uint32_t s_encoded_frames;
static recorder_packet_cb_t s_packet_cb;
#else
#define FILE_EXT "wav"
static FILE *s_file;
static uint32_t s_data_bytes;
#endif
static bool s_file_open;
static char s_base_path[48];
static volatile bool s_recording;
static volatile bool s_stop_requested;
static volatile uint32_t s_frames;
static volatile uint32_t s_dropped_bytes;
static uint32_t s_write_errors;
static int32_t s_marks[MAX_MARKS];
static int s_mark_count;
static volatile int16_t s_peak;  // peak of the last 20 ms frame, all channels

// Linear below LIMIT_KNEE, then eases toward full scale, so loud bursts are
// compressed instead of hard-clipped (which sounds harsh and hurts recognition).
static inline int16_t soft_limit(float s)
{
    const float range = 32767.0f - LIMIT_KNEE;
    float mag = fabsf(s);
    if (mag > LIMIT_KNEE) {
        mag = LIMIT_KNEE + range * tanhf((mag - LIMIT_KNEE) / range);
    }
    return (int16_t)copysignf(mag, s);
}

// 2nd-order Butterworth high-pass at 80 Hz (fs = 16 kHz, RBJ biquad). Removes the
// mic's DC offset and the sub-40 Hz rumble that otherwise dominates the signal.
#define HPF_B0 0.97803028f
#define HPF_B1 (-1.95606055f)
#define HPF_A1 (-1.95557783f)
#define HPF_A2 0.95654327f

typedef struct {
    float x1, x2, y1, y2;
} hpf_state_t;

// Converts interleaved INMP441 24-bit samples (left-aligned in 32 bits) to 16-bit PCM,
// high-pass filtering each channel.
static void convert_samples(const int32_t *in, int16_t *out, size_t count, hpf_state_t *hpf)
{
    const float gain = (float)(1 << CONFIG_RP_MIC_GAIN_SHIFT) / 256.0f;
    for (size_t i = 0; i < count; i++) {
        hpf_state_t *st = &hpf[i % CHANNELS];
        float x = (float)(in[i] >> 8);
        float y = HPF_B0 * (x + st->x2) + HPF_B1 * st->x1 - HPF_A1 * st->y1 - HPF_A2 * st->y2;
        st->x2 = st->x1;
        st->x1 = x;
        st->y2 = st->y1;
        st->y1 = y;
        out[i] = soft_limit(y * gain);
    }
}

// Logs each channel's raw 24-bit range once per recording (~1 s in) so wiring faults
// are visible: a mic with no power or clock reads a constant (often 0 or -1).
static void log_raw_range(const int32_t *raw, size_t count)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        for (size_t i = ch; i < count; i += CHANNELS) {
            int32_t v = raw[i] >> 8;
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        ESP_LOGI(TAG, "Mic %s raw range [%ld, %ld]%s", ch == 0 ? "L" : "R", (long)lo, (long)hi,
                 lo == hi ? "  <- constant: no data from this mic (power/clock/SD wiring?)" : "");
    }
}

static void capture_task(void *arg)
{
    const size_t buf_samples = FRAME_SAMPLES * CHANNELS;
    int32_t *raw = heap_caps_malloc(buf_samples * sizeof(int32_t), MALLOC_CAP_INTERNAL);
    int16_t *pcm = heap_caps_malloc(buf_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    assert(raw && pcm);

    while (true) {
        xEventGroupWaitBits(s_events, EVT_CAPTURE_RUN, pdFALSE, pdTRUE, portMAX_DELAY);
        hpf_state_t hpf[CHANNELS] = {0};
        uint32_t warmup_left = MIC_WARMUP_FRAMES;
        while (xEventGroupGetBits(s_events) & EVT_CAPTURE_RUN) {
            size_t bytes_read = 0;
            if (i2s_channel_read(s_rx, raw, buf_samples * sizeof(int32_t), &bytes_read,
                                 pdMS_TO_TICKS(100)) != ESP_OK || bytes_read == 0) {
                continue;
            }
            size_t count = bytes_read / sizeof(int32_t);
            size_t frames = count / CHANNELS;
            if (warmup_left > 0) {
                // The mic outputs zeros, then a DC step, while it starts up; drop that
                // and seed the filter with the settled level to avoid a click.
                warmup_left = warmup_left > frames ? warmup_left - frames : 0;
                for (int ch = 0; ch < CHANNELS; ch++) {
                    hpf[ch].x1 = hpf[ch].x2 = (float)(raw[count - CHANNELS + ch] >> 8);
                }
                continue;
            }
            if (s_frames < RECORDER_SAMPLE_RATE && s_frames + frames >= RECORDER_SAMPLE_RATE) {
                log_raw_range(raw, count);
            }
            convert_samples(raw, pcm, count, hpf);
            int16_t peak = 0;
            for (size_t i = 0; i < count; i++) {
                int16_t a = pcm[i] < 0 ? (pcm[i] == INT16_MIN ? INT16_MAX : -pcm[i]) : pcm[i];
                peak = a > peak ? a : peak;
            }
            s_peak = peak;
            if (xRingbufferSend(s_ringbuf, pcm, count * sizeof(int16_t), 0) != pdTRUE) {
                s_dropped_bytes += count * sizeof(int16_t);
            }
            s_frames += frames;
        }
        xEventGroupSetBits(s_events, EVT_CAPTURE_IDLE);
    }
}

// Writes REC_NNNN.json next to the audio file, e.g.
// {"audio":"REC_0001.opus","sample_rate":16000,"channels":1,"duration_ms":12340,"marks_ms":[1500,8020]}
static void write_metadata(uint32_t duration_ms)
{
    char path[64];
    snprintf(path, sizeof(path), "%s.json", s_base_path);
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", path);
        return;
    }
    fprintf(f, "{\"audio\":\"%s." FILE_EXT "\",\"sample_rate\":%d,\"channels\":%d,\"duration_ms\":%lu,\"marks_ms\":[",
            recorder_file_name(), RECORDER_SAMPLE_RATE, recorder_file_channels(), (unsigned long)duration_ms);
    for (int i = 0; i < s_mark_count; i++) {
        fprintf(f, "%s%ld", i ? "," : "", (long)s_marks[i]);
    }
    fprintf(f, "]}\n");
    fclose(f);
}

#if CONFIG_RP_REC_FORMAT_OPUS

static esp_err_t sink_open(const char *path)
{
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = RECORDER_SAMPLE_RATE;
    cfg.channel = 1;
    cfg.bitrate = CONFIG_RP_OPUS_BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = CONFIG_RP_OPUS_COMPLEXITY;
    cfg.enable_vbr = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_opus) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus encoder init failed");
        return ESP_FAIL;
    }
    s_encoded_frames = 0;
    if (!ogg_opus_open(&s_ogg, path, RECORDER_SAMPLE_RATE, 1, OPUS_PRE_SKIP)) {
        esp_opus_enc_close(s_opus);
        s_opus = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Encodes one 20 ms frame. With two mics the channels are averaged: speech adds up
// coherently while uncorrelated mic noise doesn't, giving up to 3 dB better SNR.
static bool sink_write(const int16_t *pcm)
{
    static int16_t mono[FRAME_SAMPLES];
    static uint8_t packet[OPUS_MAX_PACKET];
    for (int i = 0; i < FRAME_SAMPLES; i++) {
#if CHANNELS == 2
        mono[i] = (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2);
#else
        mono[i] = pcm[i];
#endif
    }
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)mono, .len = sizeof(mono)};
    esp_audio_enc_out_frame_t out = {.buffer = packet, .len = sizeof(packet)};
    if (esp_opus_enc_process(s_opus, &in, &out) != ESP_AUDIO_ERR_OK) {
        return false;
    }
    if (s_packet_cb) {
        s_packet_cb(packet, out.encoded_bytes, s_encoded_frames);
    }
    s_encoded_frames++;
    return ogg_opus_write(&s_ogg, packet, out.encoded_bytes, FRAME_SAMPLES * 48000 / RECORDER_SAMPLE_RATE);
}

static void sink_sync(void)
{
    ogg_opus_flush(&s_ogg);
    fflush(s_ogg.f);
    fsync(fileno(s_ogg.f));
}

static void sink_close(void)
{
    ogg_opus_close(&s_ogg);
    esp_opus_enc_close(s_opus);
    s_opus = NULL;
}

#else  // WAV

static esp_err_t sink_open(const char *path)
{
    s_file = wav_open(path, RECORDER_SAMPLE_RATE, 16, CHANNELS);
    if (!s_file) {
        return ESP_FAIL;
    }
    setvbuf(s_file, NULL, _IOFBF, 16 * 1024);  // storage likes large writes
    s_data_bytes = 0;
    return ESP_OK;
}

static bool sink_write(const int16_t *pcm)
{
    bool ok = fwrite(pcm, 1, FRAME_BYTES, s_file) == FRAME_BYTES;
    s_data_bytes += FRAME_BYTES;
    return ok;
}

static void sink_sync(void)
{
    // Keep the header valid so a power loss still leaves a playable file.
    wav_update_sizes(s_file, s_data_bytes);
    fflush(s_file);
    fsync(fileno(s_file));
}

static void sink_close(void)
{
    wav_update_sizes(s_file, s_data_bytes);
    fclose(s_file);
    s_file = NULL;
}

#endif  // CONFIG_RP_REC_FORMAT_OPUS

static void writer_task(void *arg)
{
    int16_t *frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_INTERNAL);
    assert(frame);
    size_t frame_len = 0;
    uint32_t unsynced_frames = 0;

    while (true) {
        size_t n = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &n, pdMS_TO_TICKS(50), FRAME_BYTES - frame_len);
        if (item) {
            memcpy((uint8_t *)frame + frame_len, item, n);
            frame_len += n;
            vRingbufferReturnItem(s_ringbuf, item);
        }

        if (frame_len == FRAME_BYTES) {
            if (s_file_open && !sink_write(frame) && s_write_errors++ == 0) {
                ESP_LOGE(TAG, "Write failed (storage full?)");
            }
            frame_len = 0;
            if (s_file_open && ++unsynced_frames >= SYNC_INTERVAL_FRAMES) {
                sink_sync();
                unsynced_frames = 0;
            }
        }

        // Capture has stopped and the ring buffer is drained; a partial frame is dropped.
        if (!item && s_stop_requested) {
            if (s_file_open) {
                sink_close();
                s_file_open = false;
            }
            write_metadata(recorder_elapsed_ms());
            frame_len = 0;
            unsynced_frames = 0;
            s_stop_requested = false;
            xEventGroupSetBits(s_events, EVT_WRITER_DONE);
        }
    }
}

esp_err_t recorder_init(void)
{
    s_events = xEventGroupCreate();
    s_ringbuf = xRingbufferCreateWithCaps(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_events || !s_ringbuf) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_events, EVT_CAPTURE_IDLE);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = FRAME_SAMPLES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RECORDER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        CHANNELS == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_RP_MIC_SCK_GPIO,
            .ws = CONFIG_RP_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_RP_MIC_SD_GPIO,
        },
    };
#if CONFIG_RP_MIC_DUAL
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
#elif CONFIG_RP_MIC_SLOT_RIGHT
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
#else
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#endif
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s_channel_init_std_mode");

    xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 10, NULL, 1);
    // The Opus encoder needs a deep stack.
    xTaskCreatePinnedToCore(writer_task, "writer", 32 * 1024, NULL, 6, NULL, 0);
    return ESP_OK;
}

esp_err_t recorder_start(void)
{
    if (s_recording) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(storage_mount(), TAG, "SD card not available");
    ESP_RETURN_ON_ERROR(storage_next_rec_base(s_base_path, sizeof(s_base_path)), TAG, "Cannot list SD card");

    char path[64];
    snprintf(path, sizeof(path), "%s." FILE_EXT, s_base_path);
    if (sink_open(path) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot create %s", path);
        return ESP_FAIL;
    }
    s_file_open = true;

    s_frames = 0;
    s_dropped_bytes = 0;
    s_write_errors = 0;
    s_mark_count = 0;
    s_peak = 0;
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    s_recording = true;
    xEventGroupClearBits(s_events, EVT_CAPTURE_IDLE | EVT_WRITER_DONE);
    xEventGroupSetBits(s_events, EVT_CAPTURE_RUN);
    ESP_LOGI(TAG, "Recording -> %s", path);
    return ESP_OK;
}

esp_err_t recorder_stop(void)
{
    if (!s_recording) {
        return ESP_OK;
    }
    xEventGroupClearBits(s_events, EVT_CAPTURE_RUN);
    xEventGroupWaitBits(s_events, EVT_CAPTURE_IDLE, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
    i2s_channel_disable(s_rx);

    s_stop_requested = true;
    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_WRITER_DONE, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    s_recording = false;

    uint32_t ms = recorder_elapsed_ms();
    s_peak = 0;
    ESP_LOGI(TAG, "Stopped %s." FILE_EXT ": %lu.%03lu s, %d marks, dropped %lu bytes, %lu write errors",
             s_base_path, (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), s_mark_count,
             (unsigned long)s_dropped_bytes, (unsigned long)s_write_errors);
    return (bits & EVT_WRITER_DONE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool recorder_is_recording(void)
{
    return s_recording;
}

int32_t recorder_mark(void)
{
    if (!s_recording || s_mark_count >= MAX_MARKS) {
        return -1;
    }
    int32_t ms = (int32_t)recorder_elapsed_ms();
    s_marks[s_mark_count++] = ms;
    ESP_LOGI(TAG, "Mark #%d at %ld ms", s_mark_count, (long)ms);
    return ms;
}

uint32_t recorder_elapsed_ms(void)
{
    return (uint32_t)((uint64_t)s_frames * 1000 / RECORDER_SAMPLE_RATE);
}

int recorder_mark_count(void)
{
    return s_mark_count;
}

int recorder_peak_db(void)
{
    int16_t peak = s_peak;
    return peak > 0 ? (int)lroundf(20.0f * log10f(peak / 32767.0f)) : -96;
}

const char *recorder_file_name(void)
{
    const char *slash = strrchr(s_base_path, '/');
    return slash ? slash + 1 : "";
}

int recorder_file_channels(void)
{
#if CONFIG_RP_REC_FORMAT_OPUS
    return 1;
#else
    return CHANNELS;
#endif
}

void recorder_set_packet_cb(recorder_packet_cb_t cb)
{
#if CONFIG_RP_REC_FORMAT_OPUS
    s_packet_cb = cb;
#endif
}

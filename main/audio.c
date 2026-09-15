#include "audio.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "board_pins.h"
#include "mem_utils.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

static SemaphoreHandle_t s_mutex;
static i2s_chan_handle_t s_rx;
static i2s_chan_handle_t s_tx;
static uint8_t *s_record;
static size_t s_record_size;
static size_t s_record_cap;
static uint8_t *s_ring;
static size_t s_ring_cap;
static size_t s_ring_head;
static size_t s_ring_tail;
static bool s_listening;
static bool s_storing;
static bool s_playing;
static bool s_inited;
static volatile float s_last_peak;
static volatile float s_last_rms;
static volatile uint32_t s_frame_seq;
static float s_hp_x1;
static float s_hp_y1;
static float s_lp_y1;
static i2s_pdm_slot_mask_t s_pdm_slot = I2S_PDM_SLOT_LEFT;
static int s_quiet_reads;
static bool s_tried_alt_slot;
static uint32_t s_play_start_ms;
static uint32_t s_last_tx_ms;
static size_t s_play_bytes;
static bool s_logged_pcm;
static bool s_logged_tx;
static uint8_t s_play_mono[512];
static int16_t s_play_stereo[512];
static int s_volume_percent = 100;

#define PLAY_GAIN_100 1
#define LP_ALPHA 0.7f
#define FADE_SAMPLES 160
#define PRE_ROLL_MS 400
#define PRE_ROLL_BYTES ((VOICE_SAMPLE_RATE * 2 * PRE_ROLL_MS) / 1000)
/* Kconfig 可调（menuconfig → Agent Display → Voice / audio tuning）。 */
#define PDM_AGC_GAIN_MIN (CONFIG_AGENT_PDM_AGC_GAIN_MIN_X10 / 10.0f)
#define PDM_AGC_GAIN_MAX (CONFIG_AGENT_PDM_AGC_GAIN_MAX_X10 / 10.0f)
#define PDM_AGC_GAIN_INIT (CONFIG_AGENT_PDM_AGC_GAIN_INIT_X10 / 10.0f)
#define PDM_AGC_TARGET_RMS (CONFIG_AGENT_PDM_AGC_TARGET_RMS_X1000 / 1000.0f)
#define PDM_AGC_ALPHA (CONFIG_AGENT_PDM_AGC_ALPHA_X100 / 100.0f)
#define PDM_READ_TIMEOUT_MS CONFIG_AGENT_PDM_READ_TIMEOUT_MS
#define PDM_QUIET_PEAK 40
#define PDM_QUIET_SWITCH_READS 50

static float s_pdm_gain = PDM_AGC_GAIN_INIT;
static size_t s_fade_in_pos;
static bool s_fade_out_req;
static size_t s_fade_out_pos;
static uint8_t *s_preroll;
static size_t s_preroll_cap;
static size_t s_preroll_w;
static size_t s_preroll_n;

static void preroll_push(const uint8_t *data, size_t len)
{
    if (!s_preroll || s_preroll_cap == 0 || !data || len == 0) {
        return;
    }
    if (len >= s_preroll_cap) {
        memcpy(s_preroll, data + (len - s_preroll_cap), s_preroll_cap);
        s_preroll_w = 0;
        s_preroll_n = s_preroll_cap;
        return;
    }
    size_t first = s_preroll_cap - s_preroll_w;
    if (first > len) {
        first = len;
    }
    memcpy(s_preroll + s_preroll_w, data, first);
    s_preroll_w = (s_preroll_w + first) % s_preroll_cap;
    if (first < len) {
        memcpy(s_preroll, data + first, len - first);
        s_preroll_w = len - first;
    }
    s_preroll_n += len;
    if (s_preroll_n > s_preroll_cap) {
        s_preroll_n = s_preroll_cap;
    }
}

static size_t preroll_copy_out(uint8_t *dest, size_t dest_cap)
{
    if (!s_preroll || !dest || dest_cap == 0 || s_preroll_n == 0) {
        return 0;
    }
    size_t n = s_preroll_n;
    if (n > dest_cap) {
        n = dest_cap;
    }
    size_t start = (s_preroll_w + s_preroll_cap - s_preroll_n) % s_preroll_cap;
    size_t first = s_preroll_cap - start;
    if (first > n) {
        first = n;
    }
    memcpy(dest, s_preroll + start, first);
    if (first < n) {
        memcpy(dest + first, s_preroll, n - first);
    }
    return n;
}

static bool lock(TickType_t ticks)
{
    return s_mutex && xSemaphoreTake(s_mutex, ticks) == pdTRUE;
}

static void unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static size_t ring_used(void)
{
    if (s_ring_head >= s_ring_tail) {
        return s_ring_head - s_ring_tail;
    }
    return s_ring_cap - s_ring_tail + s_ring_head;
}

static size_t ring_free(void) { return s_ring_cap - ring_used() - 1; }

esp_err_t audio_init(void)
{
#if !VOICE_HARDWARE_ENABLED
    ESP_LOGI(TAG, "hardware disabled");
    return ESP_OK;
#endif
    if (s_inited) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    s_record_cap = VOICE_MAX_RECORD_BYTES;
    s_record = psram_malloc(s_record_cap);
    s_ring_cap = VOICE_PLAY_RING_BYTES;
    s_ring = psram_malloc(s_ring_cap);
    s_preroll_cap = PRE_ROLL_BYTES;
    s_preroll = psram_malloc(s_preroll_cap);
    if (!s_mutex || !s_record || !s_ring || !s_preroll) {
        free(s_record);
        s_record = NULL;
        free(s_ring);
        s_ring = NULL;
        free(s_preroll);
        s_preroll = NULL;
        if (s_mutex) {
            vSemaphoreDelete(s_mutex);
            s_mutex = NULL;
        }
        ESP_LOGE(TAG, "buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_preroll_w = 0;
    s_preroll_n = 0;

    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_cfg, NULL, &s_rx));
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .clk = PIN_PDM_CLK,
                .din = PIN_PDM_DATA,
                .invert_flags = {.clk_inv = false},
            },
    };
    pdm_cfg.slot_cfg.slot_mask = s_pdm_slot;
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg));

    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_cfg.dma_desc_num = 8;
    tx_cfg.dma_frame_num = 256;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_cfg, &s_tx, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = PIN_I2S_BCLK,
                .ws = PIN_I2S_LRC,
                .dout = PIN_I2S_DIN,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
            },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    s_inited = true;
    ESP_LOGI(TAG, "I2S ready PDM din=GPIO%d clk=GPIO%d tx din=GPIO%d ws=GPIO%d bclk=GPIO%d",
             PIN_PDM_DATA, PIN_PDM_CLK, PIN_I2S_DIN, PIN_I2S_LRC, PIN_I2S_BCLK);
    mem_report("audio");
    return ESP_OK;
}

static float lowpass(float x)
{
    s_lp_y1 = LP_ALPHA * s_lp_y1 + (1.0f - LP_ALPHA) * x;
    return s_lp_y1;
}

static float playback_fade_factor(void)
{
    if (s_fade_in_pos < FADE_SAMPLES) {
        return (float)s_fade_in_pos++ / (float)FADE_SAMPLES;
    }
    if (s_fade_out_req) {
        if (s_fade_out_pos < FADE_SAMPLES) {
            return 1.0f - (float)s_fade_out_pos++ / (float)FADE_SAMPLES;
        }
        return 0.0f;
    }
    return 1.0f;
}

static void agc_adjust(float rms)
{
    if (rms < 0.001f) {
        return;
    }
    float error = PDM_AGC_TARGET_RMS / rms;
    float gain_delta = PDM_AGC_ALPHA * (error - 1.0f);
    s_pdm_gain += gain_delta;
    if (s_pdm_gain < PDM_AGC_GAIN_MIN) {
        s_pdm_gain = PDM_AGC_GAIN_MIN;
    } else if (s_pdm_gain > PDM_AGC_GAIN_MAX) {
        s_pdm_gain = PDM_AGC_GAIN_MAX;
    }
}

static void switch_pdm_slot(void)
{
    if (!s_rx) {
        return;
    }
    i2s_channel_disable(s_rx);
    s_pdm_slot = (s_pdm_slot == I2S_PDM_SLOT_LEFT) ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;
    i2s_pdm_rx_slot_config_t slot_cfg =
        I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    slot_cfg.slot_mask = s_pdm_slot;
    esp_err_t err = i2s_channel_reconfig_pdm_rx_slot(s_rx, &slot_cfg);
    i2s_channel_enable(s_rx);
    ESP_LOGW(TAG, "PDM slot -> %s err=%s", s_pdm_slot == I2S_PDM_SLOT_LEFT ? "LEFT" : "RIGHT",
             esp_err_to_name(err));
}

static float read_peak(bool append)
{
    if (!s_listening || !s_rx) {
        return 0.0f;
    }
    int16_t sample_buf[256];
    size_t bytes_read = 0;
    if (i2s_channel_read(s_rx, sample_buf, sizeof(sample_buf), &bytes_read,
                         pdMS_TO_TICKS(PDM_READ_TIMEOUT_MS)) != ESP_OK ||
        bytes_read == 0) {
        return s_last_peak;
    }
    size_t samples = bytes_read / 2;
    int32_t mean = 0;
    for (size_t i = 0; i < samples; ++i) {
        mean += sample_buf[i];
    }
    mean /= (int32_t)samples;

    float sum_sq = 0.0f;
    float peak_n = 0.0f;
    for (size_t i = 0; i < samples; ++i) {
        int32_t centered = (int32_t)sample_buf[i] - mean;
        /* High-pass on raw, then amplify — VAD uses amplified scale (old behavior). */
        float x = (float)centered / 32768.0f;
        float y = 0.99f * (s_hp_y1 + x - s_hp_x1);
        s_hp_x1 = x;
        s_hp_y1 = y;
        y = lowpass(y);

        int32_t v = (int32_t)(y * 32768.0f * s_pdm_gain);
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        sample_buf[i] = (int16_t)v;

        float an = fabsf((float)v / 32768.0f);
        if (an > peak_n) {
            peak_n = an;
        }
        sum_sq += an * an;
    }
    s_last_rms = sqrtf(sum_sq / (float)samples);
    s_last_peak = peak_n;
    agc_adjust(s_last_rms);
    s_frame_seq++;

    if (lock(pdMS_TO_TICKS(20))) {
        if (append) {
            size_t copy = bytes_read;
            if (s_record_size + copy > s_record_cap) {
                copy = s_record_cap - s_record_size;
            }
            if (copy > 0) {
                memcpy(s_record + s_record_size, sample_buf, copy);
                s_record_size += copy;
            }
        } else {
            preroll_push((const uint8_t *)sample_buf, bytes_read);
        }
        unlock();
    }
    return peak_n;
}

esp_err_t audio_capture_listen_start(void)
{
#if !VOICE_HARDWARE_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock(pdMS_TO_TICKS(100))) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_listening) {
        s_record_size = 0;
        s_storing = false;
        s_preroll_w = 0;
        s_preroll_n = 0;
        esp_err_t err = i2s_channel_enable(s_rx);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "PDM enable failed: %s", esp_err_to_name(err));
            unlock();
            return err;
        }
        s_listening = true;
        s_quiet_reads = 0;
    }
    unlock();
    return ESP_OK;
}

void audio_capture_listen_stop(void)
{
    if (!lock(pdMS_TO_TICKS(100))) {
        return;
    }
    if (s_rx && s_listening) {
        i2s_channel_disable(s_rx);
    }
    s_listening = false;
    s_storing = false;
    unlock();
}

void audio_capture_begin_store(void)
{
    if (!lock(pdMS_TO_TICKS(100))) {
        return;
    }
    s_record_size = preroll_copy_out(s_record, s_record_cap);
    s_storing = true;
    unlock();
}

void audio_capture_end_store(void)
{
    if (!lock(pdMS_TO_TICKS(100))) {
        return;
    }
    s_storing = false;
    unlock();
}

void audio_capture_clear(void)
{
    if (!lock(pdMS_TO_TICKS(100))) {
        return;
    }
    s_record_size = 0;
    unlock();
}

float audio_capture_peak(void) { return s_last_peak; }
float audio_capture_rms(void) { return s_last_rms; }
uint32_t audio_capture_frame_seq(void) { return s_frame_seq; }

void audio_capture_reset_hp(void)
{
    s_hp_x1 = 0.0f;
    s_hp_y1 = 0.0f;
    s_lp_y1 = 0.0f;
    s_pdm_gain = PDM_AGC_GAIN_INIT;
}

bool audio_capture_is_listening(void) { return s_listening; }
const uint8_t *audio_capture_data(void) { return s_record; }
size_t audio_capture_size(void) { return s_record_size; }

void audio_capture_snapshot(const uint8_t **data, size_t *size)
{
    const uint8_t *p = NULL;
    size_t n = 0;
    if (lock(pdMS_TO_TICKS(50))) {
        p = s_record;
        n = s_record_size;
        unlock();
    }
    if (data) {
        *data = p;
    }
    if (size) {
        *size = n;
    }
}

void audio_capture_reset(void)
{
    audio_capture_listen_stop();
    s_record_size = 0;
    s_last_peak = 0.0f;
    s_last_rms = 0.0f;
    audio_capture_reset_hp();
}

bool audio_playback_start(void)
{
#if !VOICE_HARDWARE_ENABLED
    return false;
#endif
    if (!s_inited) {
        return false;
    }
    if (!lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    s_ring_head = s_ring_tail = 0;
    s_play_bytes = 0;
    s_last_tx_ms = 0;
    s_logged_pcm = false;
    s_logged_tx = false;
    s_play_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_fade_in_pos = 0;
    s_fade_out_req = false;
    s_fade_out_pos = 0;
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "tx enable %s", esp_err_to_name(err));
        unlock();
        return false;
    }
    s_playing = true;
    unlock();
    ESP_LOGI(TAG, "playback start");
    return true;
}

void audio_playback_stop(void)
{
    if (!lock(pdMS_TO_TICKS(100))) {
        return;
    }
    ESP_LOGI(TAG, "playback stop queued=%u", (unsigned)s_play_bytes);
    if (s_tx) {
        i2s_channel_disable(s_tx);
    }
    s_playing = false;
    s_ring_head = s_ring_tail = 0;
    s_fade_out_req = false;
    s_fade_out_pos = 0;
    unlock();
}

void audio_playback_begin_fadeout(void)
{
    if (!lock(pdMS_TO_TICKS(50))) {
        return;
    }
    s_fade_out_req = true;
    s_fade_out_pos = 0;
    unlock();
}

void audio_playback_cancel_fadeout(void)
{
    if (!lock(pdMS_TO_TICKS(50))) {
        return;
    }
    s_fade_out_req = false;
    s_fade_out_pos = 0;
    unlock();
}

bool audio_playback_write(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || !lock(pdMS_TO_TICKS(50))) {
        return false;
    }
    bool playing = s_playing;
    if (s_fade_out_req) {
        s_fade_out_req = false;
        s_fade_out_pos = 0;
    }
    unlock();
    if (!playing) {
        return false;
    }
    if (!s_logged_pcm && len >= 2) {
        const int16_t *src = (const int16_t *)data;
        size_t ns = len / 2;
        int32_t peak = 0;
        if (ns > 256) {
            ns = 256;
        }
        for (size_t i = 0; i < ns; ++i) {
            int32_t abs_v = src[i] < 0 ? -(int32_t)src[i] : (int32_t)src[i];
            if (abs_v > peak) {
                peak = abs_v;
            }
        }
        s_logged_pcm = true;
        ESP_LOGI(TAG, "pcm in len=%u peak=%ld", (unsigned)len, (long)peak);
    }
    size_t off = 0;
    int64_t start = (int64_t)xTaskGetTickCount();
    while (off < len) {
        /* 有界等待 200ms：旧值 2s 会把 app 任务（UI/事件循环）卡死；超时丢弃剩余并告警。 */
        if ((int64_t)xTaskGetTickCount() - start > pdMS_TO_TICKS(200)) {
            ESP_LOGW(TAG, "playback ring full 200ms, dropped %u", (unsigned)(len - off));
            return off > 0;
        }
        if (!lock(pdMS_TO_TICKS(50))) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t space = ring_free();
        size_t n = len - off;
        if (n > space) {
            n = space;
        }
        if (n == 0) {
            unlock();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        /* 环形缓冲两段 memcpy，替代逐字节拷贝。 */
        size_t first = s_ring_cap - s_ring_head;
        if (first > n) {
            first = n;
        }
        memcpy(s_ring + s_ring_head, data + off, first);
        if (n > first) {
            memcpy(s_ring, data + off + first, n - first);
        }
        s_ring_head = (s_ring_head + n) % s_ring_cap;
        unlock();
        off += n;
    }
    s_play_bytes += off;
    return true;
}

void audio_playback_service(void)
{
    if (!s_tx) {
        return;
    }
    size_t n = 0;
    if (!lock(0)) {
        return;
    }
    if (!s_playing) {
        unlock();
        return;
    }
    /* 读出端同样两段 memcpy。 */
    size_t avail = ring_used();
    size_t to_read = sizeof(s_play_mono) - n;
    if (to_read > avail) {
        to_read = avail;
    }
    size_t first = s_ring_cap - s_ring_tail;
    if (first > to_read) {
        first = to_read;
    }
    memcpy(s_play_mono + n, s_ring + s_ring_tail, first);
    if (to_read > first) {
        memcpy(s_play_mono + n + first, s_ring, to_read - first);
    }
    s_ring_tail = (s_ring_tail + to_read) % s_ring_cap;
    n += to_read;
    unlock();
    n &= ~(size_t)1;
    if (n == 0) {
        return;
    }
    size_t samples = n / 2;
    if (samples > (sizeof(s_play_stereo) / sizeof(s_play_stereo[0])) / 2) {
        samples = (sizeof(s_play_stereo) / sizeof(s_play_stereo[0])) / 2;
    }
    if (!lock(pdMS_TO_TICKS(50))) {
        return;
    }
    const int volume_percent = s_volume_percent;
    const int16_t *src = (const int16_t *)s_play_mono;
    for (size_t i = 0; i < samples; ++i) {
        float fade = playback_fade_factor();
        int32_t v = (int32_t)(((int32_t)src[i] * PLAY_GAIN_100 * volume_percent) / 100 * fade);
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        s_play_stereo[2 * i] = (int16_t)v;
        s_play_stereo[2 * i + 1] = (int16_t)v;
    }
    unlock();
    size_t written = 0;
    size_t want = samples * 4;
    esp_err_t err = i2s_channel_write(s_tx, s_play_stereo, want, &written, pdMS_TO_TICKS(80));
    s_last_tx_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tx write %s", esp_err_to_name(err));
    } else if (!s_logged_tx) {
        int32_t peak = 0;
        for (size_t i = 0; i < samples; ++i) {
            int32_t abs_v = s_play_stereo[2 * i] < 0 ? -(int32_t)s_play_stereo[2 * i]
                                                     : (int32_t)s_play_stereo[2 * i];
            if (abs_v > peak) {
                peak = abs_v;
            }
        }
        s_logged_tx = true;
        ESP_LOGI(TAG, "tx first samples=%u written=%u peak=%ld", (unsigned)samples,
                 (unsigned)written, (long)peak);
    } else if (written != want) {
        ESP_LOGW(TAG, "tx short write %u/%u", (unsigned)written, (unsigned)want);
    }
}

void audio_set_volume_percent(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    if (!lock(pdMS_TO_TICKS(50))) {
        return;
    }
    s_volume_percent = percent;
    unlock();
    ESP_LOGI(TAG, "volume %d%%", percent);
}

int audio_get_volume_percent(void)
{
    int percent = 100;
    if (lock(pdMS_TO_TICKS(50))) {
        percent = s_volume_percent;
        unlock();
    }
    return percent;
}

size_t audio_record_capacity(void) { return s_record_cap; }
size_t audio_play_ring_capacity(void) { return s_ring_cap; }
float audio_get_pdm_gain(void) { return s_pdm_gain; }

bool audio_playback_is_active(void)
{
    bool playing = false;
    if (lock(pdMS_TO_TICKS(5))) {
        playing = s_playing;
        unlock();
    }
    return playing;
}

size_t audio_playback_pending(void)
{
    if (!lock(0)) {
        return 0;
    }
    size_t used = ring_used();
    unlock();
    return used;
}

bool audio_playback_should_stop(void)
{
    if (!s_playing) {
        return true;
    }
    if (audio_playback_pending() > 0) {
        return false;
    }
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_last_tx_ms == 0) {
        return (now - s_play_start_ms) >= 400u;
    }
    return (now - s_last_tx_ms) >= 200u;
}

void audio_task_loop(void)
{
#if VOICE_HARDWARE_ENABLED
    bool blocked = false;
    if (s_listening) {
        float peak = read_peak(s_storing);
        /* 归一化峰值约对应旧 int16 阈值：0.001≈33，用于槽位探测 */
        /* peak 已是放大后归一化；旧静音探测 peak<40（放大后 int16）≈0.0012 */
        if (peak < (float)PDM_QUIET_PEAK / 32768.0f) {
            ++s_quiet_reads;
        } else {
            s_quiet_reads = 0;
        }
        if (!s_tried_alt_slot && s_quiet_reads >= PDM_QUIET_SWITCH_READS) {
            s_tried_alt_slot = true;
            if (lock(pdMS_TO_TICKS(50))) {
                switch_pdm_slot();
                unlock();
            }
            s_quiet_reads = 0;
        }
        blocked = true;
    }
    audio_playback_service();
    if (!blocked) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
#else
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
}

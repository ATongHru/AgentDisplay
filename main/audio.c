#include "audio.h"

#include <string.h>

#include "board_pins.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
static volatile size_t s_last_peak;

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
    s_record = heap_caps_malloc(s_record_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_record) {
        s_record = malloc(s_record_cap);
    }
    s_ring_cap = VOICE_PLAY_RING_BYTES;
    s_ring = heap_caps_malloc(s_ring_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        s_ring = malloc(s_ring_cap);
    }
    if (!s_mutex || !s_record || !s_ring) {
        return ESP_ERR_NO_MEM;
    }

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
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg));

    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_cfg, &s_tx, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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
    ESP_LOGI(TAG, "I2S ready PDM din=GPIO%d clk=GPIO%d", PIN_PDM_DATA, PIN_PDM_CLK);
    return ESP_OK;
}

static size_t read_peak(bool append)
{
    if (!s_listening || !s_rx) {
        return 0;
    }
    int16_t sample_buf[256];
    size_t bytes_read = 0;
    if (i2s_channel_read(s_rx, sample_buf, sizeof(sample_buf), &bytes_read, 0) != ESP_OK ||
        bytes_read == 0) {
        return 0;
    }
    if (append && lock(0)) {
        size_t copy = bytes_read;
        if (s_record_size + copy > s_record_cap) {
            copy = s_record_cap - s_record_size;
        }
        if (copy > 0) {
            memcpy(s_record + s_record_size, sample_buf, copy);
            s_record_size += copy;
        }
        unlock();
    }
    int32_t peak = 0;
    for (size_t i = 0; i < bytes_read / 2; ++i) {
        int32_t v = sample_buf[i] < 0 ? -sample_buf[i] : sample_buf[i];
        if (v > peak) {
            peak = v;
        }
    }
    return (size_t)peak;
}

bool audio_capture_listen_start(void)
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
    if (!s_listening) {
        s_record_size = 0;
        s_storing = false;
        i2s_channel_enable(s_rx);
        s_listening = true;
    }
    unlock();
    return true;
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
    s_record_size = 0;
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

size_t audio_capture_poll(void) { return s_last_peak; }
bool audio_capture_is_listening(void) { return s_listening; }
const uint8_t *audio_capture_data(void) { return s_record; }
size_t audio_capture_size(void) { return s_record_size; }

void audio_capture_reset(void)
{
    audio_capture_listen_stop();
    s_record_size = 0;
    s_last_peak = 0;
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
    i2s_channel_enable(s_tx);
    s_playing = true;
    unlock();
    return true;
}

void audio_playback_stop(void)
{
    if (!lock(pdMS_TO_TICKS(100))) {
        return;
    }
    if (s_tx) {
        i2s_channel_disable(s_tx);
    }
    s_playing = false;
    s_ring_head = s_ring_tail = 0;
    unlock();
}

bool audio_playback_write(const uint8_t *data, size_t len)
{
    if (!s_playing || !data || len == 0) {
        return false;
    }
    if (!lock(pdMS_TO_TICKS(100))) {
        return false;
    }
    if (len > ring_free()) {
        unlock();
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        s_ring[s_ring_head] = data[i];
        s_ring_head = (s_ring_head + 1) % s_ring_cap;
    }
    unlock();
    return true;
}

void audio_playback_service(void)
{
    if (!s_playing || !s_tx) {
        return;
    }
    uint8_t chunk[512];
    size_t n = 0;
    if (!lock(0)) {
        return;
    }
    while (n < sizeof(chunk) && s_ring_tail != s_ring_head) {
        chunk[n++] = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) % s_ring_cap;
    }
    unlock();
    if (n == 0) {
        return;
    }
    size_t written = 0;
    i2s_channel_write(s_tx, chunk, n, &written, 0);
}

bool audio_playback_is_active(void) { return s_playing; }

size_t audio_playback_pending(void)
{
    if (!lock(0)) {
        return 0;
    }
    size_t used = ring_used();
    unlock();
    return used;
}

void audio_task_loop(void)
{
#if VOICE_HARDWARE_ENABLED
    if (s_listening) {
        s_last_peak = read_peak(s_storing);
    }
    audio_playback_service();
#endif
}

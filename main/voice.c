#include "voice.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net_ws.h"
#include "ui.h"

static const char *TAG = "voice";

enum {
    VOICE_LISTEN = 0,
    VOICE_RECORDING,
    VOICE_UPLOADING,
    VOICE_WAIT_REPLY,
    VOICE_PLAYING,
    VOICE_ERROR,
};

#define VAD_PEAK_START 1400
#define VAD_RMS_START 280
#define VAD_PEAK_HOLD 480
#define VAD_RMS_HOLD 90
#define VAD_MIN_MS 500
#define VAD_SILENCE_MS 900
#define VAD_MAX_MS 6000
#define VAD_COOLDOWN_MS 1500
#define WAIT_REPLY_MS 25000
#define MIN_CLIP_BYTES (VOICE_SAMPLE_RATE * 2 * 2 / 5)

static int s_phase = VOICE_LISTEN;
static char s_session_id[16];
static int64_t s_record_start_us;
static int64_t s_last_voice_us;
static int64_t s_last_diag_us;
static int64_t s_cooldown_until_us;
static int64_t s_wait_start_us;
static bool s_audio_end;
static bool s_session;
static bool s_listening;
static volatile bool s_upload_pending;
static volatile bool s_upload_done;
static volatile bool s_upload_ok;
static char s_last_show[24];
static size_t s_noise_peak = 180;
static size_t s_noise_rms = 40;

static int64_t now_us(void) { return esp_timer_get_time(); }

static void show(const char *status, const char *text)
{
    if (status && strcmp(status, "IDLE") == 0 && strcmp(s_last_show, "IDLE") == 0 &&
        (!text || text[0] == '\0')) {
        return;
    }
    char json[192];
    if (text && text[0]) {
        snprintf(json, sizeof(json),
                 "{\"status\":\"%s\",\"source\":\"VOICE\",\"text\":\"%s\"}", status, text);
    } else {
        snprintf(json, sizeof(json), "{\"status\":\"%s\",\"source\":\"VOICE\"}", status);
    }
    snprintf(s_last_show, sizeof(s_last_show), "%s", status ? status : "");
    s_session = strcmp(status, "IDLE") != 0;
    ui_post_event_json(json, false);
}

static void post_voice_icons(void)
{
    ui_post_voice_link(s_listening, audio_playback_is_active());
}

static void set_listening(bool on)
{
    s_listening = on;
    post_voice_icons();
}

static void update_noise(size_t peak, size_t rms)
{
    s_noise_peak = (s_noise_peak * 31 + peak) / 32;
    s_noise_rms = (s_noise_rms * 31 + rms) / 32;
}

static size_t start_peak_th(void)
{
    size_t adaptive = s_noise_peak * 5 + 350;
    return adaptive > VAD_PEAK_START ? adaptive : VAD_PEAK_START;
}

static size_t start_rms_th(void)
{
    size_t adaptive = s_noise_rms * 4 + 80;
    return adaptive > VAD_RMS_START ? adaptive : VAD_RMS_START;
}

static size_t hold_peak_th(void)
{
    size_t adaptive = s_noise_peak * 2 + 120;
    return adaptive > VAD_PEAK_HOLD ? adaptive : VAD_PEAK_HOLD;
}

static size_t hold_rms_th(void)
{
    size_t adaptive = s_noise_rms * 2 + 30;
    return adaptive > VAD_RMS_HOLD ? adaptive : VAD_RMS_HOLD;
}

static void resume_listen(void)
{
#if VOICE_HARDWARE_ENABLED
    s_cooldown_until_us = now_us() + (int64_t)VAD_COOLDOWN_MS * 1000;
    if (audio_capture_listen_start()) {
        audio_capture_clear();
        set_listening(true);
        s_phase = VOICE_LISTEN;
        return;
    }
#endif
    set_listening(false);
    s_phase = VOICE_ERROR;
}

static void finish_turn(bool announce)
{
    if (audio_playback_is_active()) {
        audio_playback_stop();
    }
    s_audio_end = false;
    post_voice_icons();
    if (announce) {
        show("IDLE", "");
    } else {
        s_session = false;
        snprintf(s_last_show, sizeof(s_last_show), "IDLE");
    }
    resume_listen();
}

void voice_init(void)
{
    s_phase = VOICE_LISTEN;
    s_upload_pending = false;
    s_upload_done = false;
    ESP_LOGI(TAG, "always-listen VAD, PDM DATA=GPIO%d CLK=GPIO%d", PIN_PDM_DATA, PIN_PDM_CLK);
}

void voice_on_audio_chunk(const uint8_t *data, size_t len, bool end)
{
    if (data && len > 0) {
        if (s_phase != VOICE_PLAYING) {
#if VOICE_HARDWARE_ENABLED
            audio_capture_listen_stop();
#endif
            set_listening(false);
            if (!audio_playback_is_active()) {
                audio_playback_start();
            }
            post_voice_icons();
            ESP_LOGI(TAG, "play start");
        }
        s_phase = VOICE_PLAYING;
        audio_playback_write(data, len);
    }
    if (end) {
        s_audio_end = true;
        if (s_phase != VOICE_PLAYING) {
            ESP_LOGI(TAG, "server finished with no audio");
            finish_turn(false);
        }
    }
}

void voice_on_server_status(const char *status)
{
    if (!status || s_phase != VOICE_WAIT_REPLY) {
        return;
    }
    if (strcmp(status, "IDLE") == 0 || strcmp(status, "ERROR") == 0) {
        ESP_LOGI(TAG, "server status %s, resume listen", status);
        finish_turn(false);
    }
}

void voice_loop(void)
{
#if !VOICE_HARDWARE_ENABLED
    return;
#endif
    if (s_phase == VOICE_PLAYING) {
        if (s_audio_end && audio_playback_should_stop()) {
            finish_turn(false);
        }
        return;
    }

    if (s_phase == VOICE_WAIT_REPLY) {
        if (now_us() - s_wait_start_us > (int64_t)WAIT_REPLY_MS * 1000) {
            ESP_LOGW(TAG, "wait reply timeout");
            finish_turn(true);
        }
        return;
    }

    if (s_phase == VOICE_ERROR) {
        resume_listen();
        return;
    }

    if (s_phase == VOICE_UPLOADING) {
        (void)audio_capture_poll();
        if (s_upload_done) {
            s_upload_done = false;
            if (!s_upload_ok) {
                show("ERROR", "上传失败");
                s_phase = VOICE_ERROR;
                return;
            }
            audio_capture_clear();
#if VOICE_HARDWARE_ENABLED
            audio_capture_listen_stop();
#endif
            set_listening(false);
            s_wait_start_us = now_us();
            s_phase = VOICE_WAIT_REPLY;
            show("THINKING", "正在思考");
        }
        return;
    }

    if (!audio_capture_is_listening()) {
        resume_listen();
        return;
    }

    size_t peak = audio_capture_poll();
    size_t rms = audio_capture_rms();
    if (now_us() - s_last_diag_us > 2000000) {
        s_last_diag_us = now_us();
        ESP_LOGI(TAG,
                 "phase=%d listen=%d peak=%u rms=%u noise=%u/%u th=%u/%u pcm=%u ws=%d",
                 s_phase, (int)s_listening, (unsigned)peak, (unsigned)rms,
                 (unsigned)s_noise_peak, (unsigned)s_noise_rms,
                 (unsigned)start_peak_th(), (unsigned)start_rms_th(),
                 (unsigned)audio_capture_size(), (int)net_ws_ready());
    }
    if (s_phase == VOICE_LISTEN) {
        if (now_us() < s_cooldown_until_us) {
            update_noise(peak, rms);
            return;
        }
        bool speech = peak >= start_peak_th() && rms >= start_rms_th();
        if (!speech) {
            update_noise(peak, rms);
            return;
        }
        if (!net_ws_ready()) {
            return;
        }
        audio_capture_begin_store();
        s_record_start_us = now_us();
        s_last_voice_us = now_us();
        s_phase = VOICE_RECORDING;
        ESP_LOGI(TAG, "VAD start peak=%u rms=%u th=%u/%u", (unsigned)peak, (unsigned)rms,
                 (unsigned)start_peak_th(), (unsigned)start_rms_th());
        show("THINKING", "正在聆听");
        return;
    }

    if (s_phase == VOICE_RECORDING) {
        if (peak >= hold_peak_th() || rms >= hold_rms_th()) {
            s_last_voice_us = now_us();
        }
        int64_t dur_us = now_us() - s_record_start_us;
        int64_t silence_us = now_us() - s_last_voice_us;
        bool maxed = dur_us > (int64_t)VAD_MAX_MS * 1000;
        bool silenced = silence_us > (int64_t)VAD_SILENCE_MS * 1000 &&
                        dur_us > (int64_t)VAD_MIN_MS * 1000;
        if (!maxed && !silenced) {
            return;
        }
        audio_capture_end_store();
        size_t pcm_len = audio_capture_size();
        if (pcm_len < (size_t)MIN_CLIP_BYTES) {
            ESP_LOGW(TAG, "drop short clip %u bytes", (unsigned)pcm_len);
            audio_capture_clear();
            show("IDLE", "");
            s_phase = VOICE_LISTEN;
            s_cooldown_until_us = now_us() + (int64_t)VAD_COOLDOWN_MS * 1000;
            return;
        }
        s_upload_ok = false;
        s_upload_done = false;
        s_upload_pending = true;
        s_phase = VOICE_UPLOADING;
        show("THINKING", "正在上传");
    }
}

void voice_net_poll(void)
{
#if !VOICE_HARDWARE_ENABLED
    return;
#endif
    if (!s_upload_pending) {
        return;
    }
    if (!net_ws_ready()) {
        s_upload_pending = false;
        s_upload_ok = false;
        s_upload_done = true;
        return;
    }
    const uint8_t *pcm = audio_capture_data();
    size_t pcm_len = audio_capture_size();
    s_session_id[0] = '\0';
    bool ok = net_ws_send_audio_upload(pcm, pcm_len, s_session_id, sizeof(s_session_id));
    if (ok) {
        ESP_LOGI(TAG, "uploaded %u bytes session=%s", (unsigned)pcm_len, s_session_id);
    } else {
        ESP_LOGW(TAG, "upload failed len=%u", (unsigned)pcm_len);
    }
    s_upload_ok = ok;
    s_upload_pending = false;
    s_upload_done = true;
}

bool voice_session_active(void) { return s_session; }
bool voice_is_listening(void) { return s_listening; }

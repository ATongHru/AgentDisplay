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
    VOICE_PLAYING,
    VOICE_ERROR,
};

#define VAD_PEAK_START 800
#define VAD_MIN_MS 400
#define VAD_SILENCE_MS 1500
#define VAD_MAX_MS 10000

static int s_phase = VOICE_LISTEN;
static char s_session_id[16];
static int64_t s_record_start_us;
static int64_t s_last_voice_us;
static bool s_audio_end;
static bool s_session;
static bool s_listening;
static volatile bool s_upload_pending;
static volatile bool s_upload_done;
static volatile bool s_upload_ok;

static int64_t now_us(void) { return esp_timer_get_time(); }

static void show(const char *status, const char *text)
{
    char json[192];
    if (text && text[0]) {
        snprintf(json, sizeof(json),
                 "{\"status\":\"%s\",\"source\":\"VOICE\",\"text\":\"%s\"}", status, text);
    } else {
        snprintf(json, sizeof(json), "{\"status\":\"%s\",\"source\":\"VOICE\"}", status);
    }
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

static void resume_listen(void)
{
#if VOICE_HARDWARE_ENABLED
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
            show("DONE", NULL);
        }
        s_phase = VOICE_PLAYING;
        audio_playback_write(data, len);
    }
    if (end) {
        s_audio_end = true;
    }
}

void voice_loop(void)
{
#if !VOICE_HARDWARE_ENABLED
    return;
#endif
    if (s_phase == VOICE_PLAYING) {
        if (s_audio_end && audio_playback_pending() == 0) {
            audio_playback_stop();
            s_audio_end = false;
            post_voice_icons();
            show("IDLE", "");
            resume_listen();
        }
        return;
    }

    if (s_phase == VOICE_ERROR) {
        show("IDLE", "");
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
            show("IDLE", "");
            resume_listen();
        }
        return;
    }

    if (!audio_capture_is_listening()) {
        resume_listen();
        if (s_phase == VOICE_LISTEN) {
            show("IDLE", "");
        }
        return;
    }

    size_t level = audio_capture_poll();
    if (s_phase == VOICE_LISTEN) {
        if (level > VAD_PEAK_START && net_ws_ready()) {
            audio_capture_begin_store();
            s_record_start_us = now_us();
            s_last_voice_us = now_us();
            s_phase = VOICE_RECORDING;
            show("THINKING", "正在聆听");
        }
        return;
    }

    if (s_phase == VOICE_RECORDING) {
        if (level > VAD_PEAK_START) {
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
        if (pcm_len < (size_t)VOICE_SAMPLE_RATE * 2 / 5) {
            audio_capture_clear();
            show("IDLE", "");
            s_phase = VOICE_LISTEN;
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

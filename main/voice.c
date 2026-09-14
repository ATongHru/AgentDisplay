#include "voice.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "ap_prov.h"
#include "ble_prov.h"
#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net_ws.h"
#include "ui.h"

static const char *TAG = "voice";

#define WS_AUDIO_PKT_MAX 4096u
#define WS_AUDIO_QUEUE_DEPTH 16u

typedef struct {
    char session_id[16];
    uint16_t len;
    bool end;
    uint8_t data[WS_AUDIO_PKT_MAX];
} ws_audio_slot_t;

static ws_audio_slot_t *s_ws_audio_q;
static SemaphoreHandle_t s_ws_audio_mux;
static uint16_t s_ws_audio_head;
static uint16_t s_ws_audio_tail;
static uint32_t s_ws_audio_drop;

enum {
    VOICE_LISTEN = 0,
    VOICE_RECORDING,
    VOICE_UPLOADING,
    VOICE_WAIT_REPLY,
    VOICE_PLAYING,
    VOICE_ERROR,
};

/* 能量按放大后归一化（与旧 int16 VAD 同一量纲）。
 * 旧起始约 peak>=1400、rms>=280 → 归一化 0.043 / 0.0085；阈值抬高，减少环境音误触发。 */
#define VAD_RMS_START_FLOOR 0.012f
#define VAD_PEAK_START_FLOOR 0.060f
#define VAD_NOISE_MULT 3.5f
#define VAD_RMS_HOLD_FLOOR 0.003f
#define VAD_NOISE_ALPHA 0.05f
#define VAD_NOISE_INIT_FRAMES 24
#define VAD_NOISE_RMS_CAP 0.024f
#define VAD_NOISE_PEAK_CAP 0.16f
#define VAD_MIN_MS 500
#define VAD_SILENCE_MS 800
#define VAD_MAX_MS 15000
#define VAD_COOLDOWN_MS 800
#define WAIT_REPLY_MS 55000
#define MIN_CLIP_BYTES 6400
#define PLAY_END_GRACE_MS 800
#define PLAY_STALL_MS 15000

typedef struct {
    float noise_rms;
    float noise_peak;
    int noise_init_frames;
    bool in_speech;
} vad_state_t;

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
static bool s_voice_enabled = true;
static volatile bool s_upload_pending;
static volatile bool s_upload_done;
static volatile bool s_upload_ok;
static size_t s_stream_sent;
static bool s_stream_active;
static bool s_stream_failed;
static char s_last_show[24];
static vad_state_t s_vad = {.noise_rms = 0.001f, .noise_peak = 0.005f};
static uint32_t s_last_vad_seq;
static int64_t s_last_mic_ui_us;
static int64_t s_play_end_empty_us;
static int64_t s_play_stall_us;

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

static void post_voice_ui(void)
{
    bool hold = false;
    const char *overlay = "";
    if (s_phase == VOICE_RECORDING || s_phase == VOICE_UPLOADING) {
        hold = true;
        overlay = "EAR";
    } else if (s_phase == VOICE_WAIT_REPLY) {
        hold = true;
        overlay = "THINKING";
    } else if (s_phase == VOICE_PLAYING) {
        hold = true;
        overlay = "SPEAKING";
    }
    ui_post_voice_link(s_listening, audio_playback_is_active(), hold, overlay);
}

static void set_listening(bool on)
{
    s_listening = on;
    post_voice_ui();
}

static float start_rms_th(void)
{
    return fmaxf(VAD_RMS_START_FLOOR, s_vad.noise_rms * VAD_NOISE_MULT);
}

static float start_peak_th(void)
{
    return fmaxf(VAD_PEAK_START_FLOOR, s_vad.noise_peak * VAD_NOISE_MULT);
}

static float hold_rms_th(void)
{
    return fmaxf(VAD_RMS_HOLD_FLOOR, start_rms_th() * 0.5f);
}

static void update_noise_baseline(float rms, float peak)
{
    if (s_vad.in_speech) {
        return;
    }
    s_vad.noise_rms = (1.0f - VAD_NOISE_ALPHA) * s_vad.noise_rms + VAD_NOISE_ALPHA * rms;
    s_vad.noise_peak = (1.0f - VAD_NOISE_ALPHA) * s_vad.noise_peak + VAD_NOISE_ALPHA * peak;
    if (s_vad.noise_rms > VAD_NOISE_RMS_CAP) {
        s_vad.noise_rms = VAD_NOISE_RMS_CAP;
    }
    if (s_vad.noise_peak > VAD_NOISE_PEAK_CAP) {
        s_vad.noise_peak = VAD_NOISE_PEAK_CAP;
    }
}

static void reset_vad_baseline(void)
{
    s_vad.noise_init_frames = 0;
    s_vad.in_speech = false;
    s_vad.noise_rms = 0.001f;
    s_vad.noise_peak = 0.005f;
    audio_capture_reset_hp();
}

static void resume_listen(void)
{
#if VOICE_HARDWARE_ENABLED
    reset_vad_baseline();
    s_cooldown_until_us = now_us() + (int64_t)VAD_COOLDOWN_MS * 1000;
    if (!s_voice_enabled) {
        s_phase = VOICE_LISTEN;
        audio_capture_listen_stop();
        set_listening(false);
        return;
    }
    if (audio_capture_listen_start() == ESP_OK) {
        audio_capture_clear();
        s_phase = VOICE_LISTEN;
        set_listening(true);
        return;
    }
#endif
    s_phase = VOICE_ERROR;
    set_listening(false);
}

static void finish_turn(bool announce)
{
    if (audio_playback_is_active()) {
        audio_playback_stop();
    }
    s_audio_end = false;
    s_play_end_empty_us = 0;
    s_play_stall_us = 0;
    s_session = false;
    (void)announce;
    resume_listen();
}

static void drain_ws_audio_queue(void)
{
    if (!s_ws_audio_q || !s_ws_audio_mux) {
        return;
    }
    for (int burst = 0; burst < 8; ++burst) {
        ws_audio_slot_t slot;
        if (xSemaphoreTake(s_ws_audio_mux, 0) != pdTRUE) {
            return;
        }
        if (s_ws_audio_head == s_ws_audio_tail) {
            xSemaphoreGive(s_ws_audio_mux);
            return;
        }
        slot = s_ws_audio_q[s_ws_audio_tail];
        s_ws_audio_tail = (uint16_t)((s_ws_audio_tail + 1) % WS_AUDIO_QUEUE_DEPTH);
        xSemaphoreGive(s_ws_audio_mux);
        voice_on_audio_chunk(slot.session_id[0] ? slot.session_id : NULL, slot.data, slot.len,
                             slot.end);
    }
}

bool voice_enqueue_audio_chunk(const char *session_id, const uint8_t *data, size_t len, bool end)
{
    if (!s_ws_audio_q || !s_ws_audio_mux) {
        return false;
    }
    if ((!data || len == 0) && !end) {
        return true;
    }
    if (data && len > WS_AUDIO_PKT_MAX) {
        ESP_LOGW(TAG, "ws audio chunk too large %u", (unsigned)len);
        return false;
    }
    if (xSemaphoreTake(s_ws_audio_mux, pdMS_TO_TICKS(5)) != pdTRUE) {
        ++s_ws_audio_drop;
        return false;
    }
    uint16_t next = (uint16_t)((s_ws_audio_head + 1) % WS_AUDIO_QUEUE_DEPTH);
    if (next == s_ws_audio_tail) {
        xSemaphoreGive(s_ws_audio_mux);
        ++s_ws_audio_drop;
        return false;
    }
    ws_audio_slot_t *slot = &s_ws_audio_q[s_ws_audio_head];
    slot->len = 0;
    slot->end = end;
    slot->session_id[0] = '\0';
    if (session_id && session_id[0]) {
        strncpy(slot->session_id, session_id, sizeof(slot->session_id) - 1);
        slot->session_id[sizeof(slot->session_id) - 1] = '\0';
    }
    if (data && len > 0) {
        memcpy(slot->data, data, len);
        slot->len = (uint16_t)len;
    }
    s_ws_audio_head = next;
    xSemaphoreGive(s_ws_audio_mux);
    return true;
}

bool voice_net_busy(void)
{
    return s_phase == VOICE_RECORDING || s_phase == VOICE_UPLOADING || s_phase == VOICE_WAIT_REPLY ||
           s_phase == VOICE_PLAYING || audio_playback_is_active();
}

void voice_init(void)
{
    s_phase = VOICE_LISTEN;
    s_upload_pending = false;
    s_upload_done = false;
    reset_vad_baseline();
    if (!s_ws_audio_q) {
        s_ws_audio_q = heap_caps_calloc(WS_AUDIO_QUEUE_DEPTH, sizeof(ws_audio_slot_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ws_audio_q) {
            s_ws_audio_q = calloc(WS_AUDIO_QUEUE_DEPTH, sizeof(ws_audio_slot_t));
        }
        s_ws_audio_mux = xSemaphoreCreateMutex();
    }
    s_ws_audio_head = 0;
    s_ws_audio_tail = 0;
    s_ws_audio_drop = 0;
    ESP_LOGI(TAG, "always-listen VAD adaptive, PDM DATA=GPIO%d CLK=GPIO%d", PIN_PDM_DATA,
             PIN_PDM_CLK);
}

void voice_on_audio_chunk(const char *session_id, const uint8_t *data, size_t len, bool end)
{
    if (session_id && session_id[0] && s_session_id[0] &&
        (s_phase == VOICE_WAIT_REPLY || s_phase == VOICE_PLAYING) &&
        strcmp(session_id, s_session_id) != 0) {
        ESP_LOGW(TAG, "drop mismatched audio_chunk sid=%s expect=%s", session_id, s_session_id);
        return;
    }
    if (session_id && session_id[0] && s_phase != VOICE_WAIT_REPLY && s_phase != VOICE_PLAYING) {
        strncpy(s_session_id, session_id, sizeof(s_session_id) - 1);
        s_session_id[sizeof(s_session_id) - 1] = '\0';
    }

    if (data && len > 0) {
        /* New PCM always cancels a premature end/fade so streamed TTS can continue. */
        s_audio_end = false;
        audio_playback_cancel_fadeout();
        if (s_phase != VOICE_PLAYING) {
#if VOICE_HARDWARE_ENABLED
            audio_capture_listen_stop();
#endif
            s_listening = false;
            if (!audio_playback_is_active()) {
                audio_playback_start();
            }
            s_phase = VOICE_PLAYING;
            post_voice_ui();
            ESP_LOGI(TAG, "play start sid=%s", s_session_id);
        }
        s_vad.in_speech = true;
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
    if (!status) {
        return;
    }
    /* PLAYING must follow audio_chunk end, not VOICE IDLE/ERROR text frames.
     * Agent or pipeline status in the gap between streamed TTS sentences used
     * to set s_audio_end, fade the ring to silence, and cut the reply short. */
    if (s_phase == VOICE_PLAYING) {
        ESP_LOGI(TAG, "ignore server status %s during playback", status);
        return;
    }
    if (s_phase != VOICE_WAIT_REPLY) {
        return;
    }
    if (strcmp(status, "THINKING") == 0 || strcmp(status, "SPEAKING") == 0 ||
        strcmp(status, "EAR") == 0) {
        s_wait_start_us = now_us();
        return;
    }
    if (strcmp(status, "IDLE") == 0 || strcmp(status, "ERROR") == 0) {
        ESP_LOGI(TAG, "server status %s phase=%d", status, s_phase);
        finish_turn(false);
    }
}

void voice_loop(void)
{
#if !VOICE_HARDWARE_ENABLED
    return;
#endif
    if (ble_prov_active() || ap_prov_active()) {
        return;
    }
    if (!s_voice_enabled) {
        if (s_phase == VOICE_PLAYING) {
            if (s_audio_end && audio_playback_pending() == 0 && audio_playback_should_stop()) {
                finish_turn(false);
            }
            return;
        }
        if (s_phase == VOICE_RECORDING || s_phase == VOICE_UPLOADING || s_phase == VOICE_WAIT_REPLY) {
            if (audio_playback_is_active()) {
                audio_playback_stop();
            }
            s_upload_pending = false;
            s_stream_active = false;
            s_vad.in_speech = false;
            audio_capture_listen_stop();
            s_phase = VOICE_LISTEN;
            set_listening(false);
        }
        return;
    }
    if (s_phase == VOICE_PLAYING) {
        size_t pending = audio_playback_pending();
        if (pending > 0) {
            s_play_end_empty_us = 0;
            s_play_stall_us = 0;
        } else if (s_audio_end) {
            s_play_stall_us = 0;
            if (s_play_end_empty_us == 0) {
                s_play_end_empty_us = now_us();
            }
            /* Wait out streaming-TTS gaps before fading; a premature end flag
             * plus immediate fadeout used to mute the rest of the reply. */
            if (now_us() - s_play_end_empty_us > 400000) {
                audio_playback_begin_fadeout();
            }
            if (now_us() - s_play_end_empty_us > (int64_t)PLAY_END_GRACE_MS * 1000 &&
                audio_playback_should_stop()) {
                s_play_end_empty_us = 0;
                s_play_stall_us = 0;
                finish_turn(false);
            }
        } else {
            if (s_play_stall_us == 0) {
                s_play_stall_us = now_us();
            } else if (now_us() - s_play_stall_us > (int64_t)PLAY_STALL_MS * 1000) {
                ESP_LOGW(TAG, "playback stall without end flag");
                s_play_stall_us = 0;
                finish_turn(false);
            }
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
            s_wait_start_us = now_us();
            s_phase = VOICE_WAIT_REPLY;
            set_listening(false);
        }
        return;
    }

    if (!audio_capture_is_listening()) {
        resume_listen();
        return;
    }

    if (s_listening && s_voice_enabled &&
        (s_phase == VOICE_LISTEN || s_phase == VOICE_RECORDING)) {
        if (now_us() - s_last_mic_ui_us > 100000) {
            s_last_mic_ui_us = now_us();
            ui_post_mic_level(audio_capture_rms());
        }
    }

    uint32_t seq = audio_capture_frame_seq();
    if (seq == s_last_vad_seq) {
        return;
    }
    s_last_vad_seq = seq;

    float peak = audio_capture_peak();
    float rms = audio_capture_rms();

    if (s_vad.noise_init_frames < VAD_NOISE_INIT_FRAMES) {
        int n = s_vad.noise_init_frames;
        s_vad.noise_rms = (s_vad.noise_rms * (float)n + rms) / (float)(n + 1);
        s_vad.noise_peak = (s_vad.noise_peak * (float)n + peak) / (float)(n + 1);
        s_vad.noise_init_frames++;
        return;
    }

    if (now_us() - s_last_diag_us > 2000000) {
        s_last_diag_us = now_us();
        ESP_LOGI(TAG,
                 "phase=%d listen=%d rms=%.4f peak=%.4f noise=%.4f/%.4f th=%.4f/%.4f pcm=%u ws=%d",
                 s_phase, (int)s_listening, rms, peak, s_vad.noise_rms, s_vad.noise_peak,
                 start_rms_th(), start_peak_th(), (unsigned)audio_capture_size(),
                 (int)net_ws_ready());
    }

    if (s_phase == VOICE_LISTEN) {
        if (now_us() < s_cooldown_until_us) {
            update_noise_baseline(rms, peak);
            return;
        }
        bool start = (rms > start_rms_th()) && (peak > start_peak_th());
        if (!start) {
            update_noise_baseline(rms, peak);
            return;
        }
        if (!net_ws_ready()) {
            return;
        }
        audio_capture_begin_store();
        s_record_start_us = now_us();
        s_last_voice_us = now_us();
        s_vad.in_speech = true;
        s_stream_sent = 0;
        s_stream_active = false;
        s_stream_failed = false;
        s_session_id[0] = '\0';
        s_phase = VOICE_RECORDING;
        post_voice_ui();
        ui_post_mic_level(audio_capture_rms());
        ESP_LOGI(TAG, "VAD start rms=%.4f peak=%.4f th=%.4f/%.4f", rms, peak, start_rms_th(),
                 start_peak_th());
        return;
    }

    if (s_phase == VOICE_RECORDING) {
        bool hold = (rms > hold_rms_th());
        if (hold) {
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
        ESP_LOGI(TAG, "VAD end dur=%.0fms silence=%.0fms pcm=%u max=%d sil=%d",
                 (double)dur_us / 1000.0, (double)silence_us / 1000.0, (unsigned)pcm_len,
                 (int)maxed, (int)silenced);
        if (pcm_len < (size_t)MIN_CLIP_BYTES) {
            ESP_LOGW(TAG, "drop short clip %u bytes", (unsigned)pcm_len);
            if (s_stream_active) {
                (void)net_ws_send_audio_end(s_session_id, pcm_len, true);
                s_stream_active = false;
            }
            s_vad.in_speech = false;
            audio_capture_clear();
            s_phase = VOICE_LISTEN;
            post_voice_ui();
            s_cooldown_until_us = now_us() + (int64_t)VAD_COOLDOWN_MS * 1000;
            return;
        }
        s_upload_ok = false;
        s_upload_done = false;
        s_upload_pending = true;
        s_phase = VOICE_UPLOADING;
        post_voice_ui();
        /* stay on listening caption; skip upload caption */
    }
}

void voice_net_poll(void)
{
#if !VOICE_HARDWARE_ENABLED
    return;
#endif
    drain_ws_audio_queue();
    /* During RECORDING: open stream once, then push 4KB chunks while capturing. */
    if (s_phase == VOICE_RECORDING) {
        if (!net_ws_ready()) {
            return;
        }
        if (!s_stream_active && !s_stream_failed) {
            s_session_id[0] = '\0';
            if (net_ws_send_audio_stream_begin(s_session_id, sizeof(s_session_id))) {
                s_stream_active = true;
                ESP_LOGI(TAG, "stream begin session=%s", s_session_id);
            } else {
                s_stream_failed = true;
                ESP_LOGW(TAG, "stream begin failed, will bulk-upload");
            }
        }
        if (s_stream_active) {
            const uint8_t *pcm = audio_capture_data();
            size_t total = audio_capture_size();
            int burst = 0;
            while (burst < 8 && total >= s_stream_sent + (size_t)VOICE_UPLOAD_CHUNK) {
                if (!net_ws_send_audio_binary(pcm + s_stream_sent, (size_t)VOICE_UPLOAD_CHUNK)) {
                    ESP_LOGW(TAG, "stream chunk failed @%u", (unsigned)s_stream_sent);
                    s_stream_failed = true;
                    s_stream_active = false;
                    break;
                }
                s_stream_sent += (size_t)VOICE_UPLOAD_CHUNK;
                burst++;
            }
        }
        return;
    }

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
    bool ok = false;

    if (s_stream_active) {
        /* Flush remainder then audio_end. */
        if (pcm_len > s_stream_sent) {
            size_t left = pcm_len - s_stream_sent;
            if (!net_ws_send_audio_binary(pcm + s_stream_sent, left)) {
                ESP_LOGW(TAG, "stream flush failed left=%u", (unsigned)left);
                ok = false;
            } else {
                s_stream_sent = pcm_len;
                ok = net_ws_send_audio_end(s_session_id, pcm_len, false);
            }
        } else {
            ok = net_ws_send_audio_end(s_session_id, pcm_len, false);
        }
        ESP_LOGI(TAG, "stream end session=%s sent=%u ok=%d", s_session_id, (unsigned)s_stream_sent,
                 (int)ok);
        s_stream_active = false;
    } else {
        /* Fallback: one-shot upload (legacy). */
        s_session_id[0] = '\0';
        ok = net_ws_send_audio_upload(pcm, pcm_len, s_session_id, sizeof(s_session_id));
        if (ok) {
            ESP_LOGI(TAG, "uploaded %u bytes session=%s", (unsigned)pcm_len, s_session_id);
        } else {
            ESP_LOGW(TAG, "upload failed len=%u", (unsigned)pcm_len);
        }
    }

    s_upload_ok = ok;
    s_upload_pending = false;
    s_upload_done = true;
}

void voice_set_enabled(bool enabled)
{
    if (s_voice_enabled == enabled) {
        return;
    }
    s_voice_enabled = enabled;
    ESP_LOGI(TAG, "voice_enabled=%d", (int)enabled);
    ui_post_voice_enabled(enabled);
    if (!enabled) {
        if (audio_playback_is_active()) {
            audio_playback_stop();
        }
        if (s_stream_active) {
            size_t pcm_len = audio_capture_size();
            (void)net_ws_send_audio_end(s_session_id, pcm_len, true);
            ESP_LOGI(TAG, "discard stream on disable session=%s bytes=%u",
                     s_session_id, (unsigned)pcm_len);
        }
        s_upload_pending = false;
        s_stream_active = false;
        s_vad.in_speech = false;
        audio_capture_listen_stop();
        s_phase = VOICE_LISTEN;
        set_listening(false);
    } else {
        resume_listen();
    }
}

void voice_on_ws_lost(void)
{
#if VOICE_HARDWARE_ENABLED
    if (audio_playback_is_active()) {
        audio_playback_stop();
    }
    audio_capture_listen_stop();
#endif
    s_upload_pending = false;
    s_upload_done = false;
    s_stream_active = false;
    s_stream_failed = false;
    s_audio_end = false;
    s_play_end_empty_us = 0;
    s_play_stall_us = 0;
    s_vad.in_speech = false;
    s_session = false;
    s_session_id[0] = '\0';
    s_listening = false;
    s_phase = VOICE_LISTEN;
    s_last_show[0] = '\0';
    ui_post_voice_link(false, false, false, "");
}

bool voice_is_enabled(void) { return s_voice_enabled; }
bool voice_session_active(void) { return s_session; }
bool voice_is_listening(void) { return s_listening; }

void voice_fill_debug(voice_debug_t *out)
{
    if (!out) {
        return;
    }
    out->phase = s_phase;
    out->noise_rms = s_vad.noise_rms;
    out->noise_peak = s_vad.noise_peak;
    out->start_rms_th = start_rms_th();
    out->start_peak_th = start_peak_th();
    out->last_rms = audio_capture_rms();
    out->last_peak = audio_capture_peak();
}

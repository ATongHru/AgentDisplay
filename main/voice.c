#include "voice.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "ap_prov.h"
#include "cJSON.h"
#include "ble_prov.h"
#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net_ws.h"
#include "sdkconfig.h"
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
static ws_audio_slot_t *s_ws_audio_drain; /* PSRAM 暂存槽，避免 4KB 栈拷贝 */
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
/* 现场可调参数迁 Kconfig（menuconfig → Agent Display → Voice / audio tuning）。 */
#define VAD_RMS_START_FLOOR (CONFIG_AGENT_VAD_RMS_START_FLOOR_X1000 / 1000.0f)
#define VAD_PEAK_START_FLOOR (CONFIG_AGENT_VAD_PEAK_START_FLOOR_X1000 / 1000.0f)
#define VAD_NOISE_MULT (CONFIG_AGENT_VAD_NOISE_MULT_X10 / 10.0f)
#define VAD_RMS_HOLD_FLOOR (CONFIG_AGENT_VAD_RMS_HOLD_FLOOR_X1000 / 1000.0f)
#define VAD_NOISE_ALPHA (CONFIG_AGENT_VAD_NOISE_ALPHA_X100 / 100.0f)
#define VAD_NOISE_INIT_FRAMES CONFIG_AGENT_VAD_NOISE_INIT_FRAMES
#define VAD_NOISE_RMS_CAP (CONFIG_AGENT_VAD_NOISE_RMS_CAP_X1000 / 1000.0f)
#define VAD_NOISE_PEAK_CAP (CONFIG_AGENT_VAD_NOISE_PEAK_CAP_X1000 / 1000.0f)
#define VAD_MIN_MS CONFIG_AGENT_VAD_MIN_MS
#define VAD_SILENCE_MS CONFIG_AGENT_VAD_SILENCE_MS
#define VAD_MAX_MS CONFIG_AGENT_VAD_MAX_MS
#define VAD_COOLDOWN_MS CONFIG_AGENT_VAD_COOLDOWN_MS
/* session 异步握手超时（与原阻塞等待同为 2.5s，但不再阻塞 app 任务） */
#define VOICE_SESSION_WAIT_US 2500000LL
#define WAIT_REPLY_MS 55000
#define MIN_CLIP_BYTES 6400
#define PLAY_END_GRACE_MS CONFIG_AGENT_PLAY_END_GRACE_MS
#define PLAY_STALL_MS CONFIG_AGENT_PLAY_STALL_MS
#define UPLOAD_TIMEOUT_MS CONFIG_AGENT_UPLOAD_TIMEOUT_MS
#define WS_AUDIO_DRAIN_BURST 3

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
static bool s_stream_begin_pending;
static int64_t s_stream_begin_us;
static bool s_upload_meta_pending;
static int64_t s_upload_meta_us;
static size_t s_upload_sent;
static char s_last_show[24];
static vad_state_t s_vad = {.noise_rms = 0.001f, .noise_peak = 0.005f};
static uint32_t s_last_vad_seq;
static int64_t s_last_mic_ui_us;
static int64_t s_play_end_empty_us;
static int64_t s_play_stall_us;
static int64_t s_upload_start_us;

static int64_t now_us(void) { return esp_timer_get_time(); }

static void show(const char *status, const char *text)
{
    if (status && strcmp(status, "IDLE") == 0 && strcmp(s_last_show, "IDLE") == 0 &&
        (!text || text[0] == '\0')) {
        return;
    }
    /* cJSON 生成，text 含引号/反斜杠/换行也能正确转义（旧手拼会破坏 JSON 丢字幕）。 */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "status", status ? status : "");
    cJSON_AddStringToObject(root, "source", "VOICE");
    if (text && text[0]) {
        cJSON_AddStringToObject(root, "text", text);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    snprintf(s_last_show, sizeof(s_last_show), "%s", status ? status : "");
    s_session = strcmp(status, "IDLE") != 0;
    if (json) {
        ui_post_event_json(json, false); /* ui_post_event_json 内部会拷贝 */
        free(json);
    }
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

static void voice_reset_session(const char *reason)
{
    ESP_LOGW(TAG, "reset session: %s phase=%d", reason ? reason : "?", s_phase);
#if VOICE_HARDWARE_ENABLED
    if (audio_playback_is_active()) {
        audio_playback_stop();
    }
    if (s_stream_active && s_session_id[0] && net_ws_ready()) {
        size_t rec_len = 0;
        audio_capture_snapshot(NULL, &rec_len);
        (void)net_ws_send_audio_end(s_session_id, rec_len, true);
    }
    audio_capture_listen_stop();
#endif
    s_upload_pending = false;
    s_upload_done = false;
    s_upload_ok = false;
    s_upload_start_us = 0;
    s_stream_active = false;
    s_stream_failed = false;
    s_stream_begin_pending = false;
    s_upload_meta_pending = false;
    s_upload_sent = 0;
    s_stream_sent = 0;
    s_audio_end = false;
    s_play_end_empty_us = 0;
    s_play_stall_us = 0;
    s_vad.in_speech = false;
    s_session = false;
    s_session_id[0] = '\0';
    s_last_show[0] = '\0';
    audio_capture_clear();
    s_phase = VOICE_LISTEN;
    ui_post_voice_link(false, false, false, "");
    if (s_voice_enabled && !ble_prov_active() && !ap_prov_active()) {
        resume_listen();
    } else {
        set_listening(false);
    }
}

static void finalize_recording(bool maxed, bool silenced)
{
    audio_capture_end_store();
    size_t pcm_len = 0;
    audio_capture_snapshot(NULL, &pcm_len);
    int64_t dur_us = now_us() - s_record_start_us;
    int64_t silence_us = now_us() - s_last_voice_us;
    ESP_LOGI(TAG, "VAD end dur=%.0fms silence=%.0fms pcm=%u max=%d sil=%d",
             (double)dur_us / 1000.0, (double)silence_us / 1000.0, (unsigned)pcm_len, (int)maxed,
             (int)silenced);
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
    s_upload_meta_pending = false;
    s_upload_sent = 0;
    s_upload_start_us = now_us();
    s_phase = VOICE_UPLOADING;
    post_voice_ui();
}

static void drain_ws_audio_queue(void)
{
    if (!s_ws_audio_q || !s_ws_audio_mux) {
        return;
    }
    if (!s_ws_audio_drain) {
        return;
    }
    for (int burst = 0; burst < WS_AUDIO_DRAIN_BURST; ++burst) {
        /* 拷到 PSRAM 暂存槽而非栈：槽体 4KB+，app 任务栈仅 8KB。 */
        ws_audio_slot_t *slot = s_ws_audio_drain;
        if (xSemaphoreTake(s_ws_audio_mux, 0) != pdTRUE) {
            return;
        }
        if (s_ws_audio_head == s_ws_audio_tail) {
            xSemaphoreGive(s_ws_audio_mux);
            return;
        }
        *slot = s_ws_audio_q[s_ws_audio_tail];
        s_ws_audio_tail = (uint16_t)((s_ws_audio_tail + 1) % WS_AUDIO_QUEUE_DEPTH);
        xSemaphoreGive(s_ws_audio_mux);
        voice_on_audio_chunk(slot->session_id[0] ? slot->session_id : NULL, slot->data, slot->len,
                             slot->end);
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
    if (!s_ws_audio_drain) {
        s_ws_audio_drain =
            heap_caps_malloc(sizeof(ws_audio_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ws_audio_drain) {
            s_ws_audio_drain = malloc(sizeof(ws_audio_slot_t));
        }
        if (!s_ws_audio_drain) {
            ESP_LOGE(TAG, "ws audio drain slot alloc failed");
        }
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
        if (s_phase != VOICE_LISTEN) {
            voice_reset_session("prov active");
        }
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
        if (!s_upload_done && s_upload_start_us != 0 &&
            (now_us() - s_upload_start_us) > (int64_t)UPLOAD_TIMEOUT_MS * 1000) {
            ESP_LOGW(TAG, "upload timeout pending=%d", (int)s_upload_pending);
            s_upload_pending = false;
            s_upload_done = true;
            s_upload_ok = false;
        }
        if (s_upload_done) {
            s_upload_done = false;
            s_upload_start_us = 0;
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

    if (s_phase == VOICE_RECORDING) {
        int64_t dur_us = now_us() - s_record_start_us;
        if (dur_us > (int64_t)VAD_MAX_MS * 1000) {
            finalize_recording(true, false);
            return;
        }
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
        s_stream_begin_pending = false;
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
        finalize_recording(maxed, silenced);
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
            if (!s_stream_begin_pending) {
                /* 异步握手：只发元信息，session 由 net 任务回填，app 任务不再阻塞。 */
                s_session_id[0] = '\0';
                if (net_ws_audio_session_start(true, 0)) {
                    s_stream_begin_pending = true;
                    s_stream_begin_us = now_us();
                } else {
                    s_stream_failed = true;
                    ESP_LOGW(TAG, "stream begin send failed, will bulk-upload");
                }
            } else if (net_ws_audio_session_poll(s_session_id, sizeof(s_session_id))) {
                s_stream_begin_pending = false;
                s_stream_active = true;
                ESP_LOGI(TAG, "stream begin session=%s", s_session_id);
            } else if (now_us() - s_stream_begin_us > VOICE_SESSION_WAIT_US) {
                s_stream_begin_pending = false;
                s_stream_failed = true;
                ESP_LOGW(TAG, "stream begin timeout, will bulk-upload");
            }
        }
        if (s_stream_active) {
            const uint8_t *pcm = NULL;
            size_t total = 0;
            audio_capture_snapshot(&pcm, &total);
            if (!pcm) {
                return;
            }
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

    const uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    audio_capture_snapshot(&pcm, &pcm_len);
    bool ok = false;
    if (!pcm || pcm_len == 0) {
        s_upload_ok = false;
        s_upload_pending = false;
        s_upload_done = true;
        return;
    }

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
        /* 回退上传仍按 4KB 分帧。后端按 audio_len 聚合，避免 app 任务在单次
         * send_bin 中长时间阻塞，也不等待只有收齐音频后才会返回的 session。 */
        if (!s_upload_meta_pending) {
            if (net_ws_audio_session_start(false, pcm_len)) {
                s_upload_meta_pending = true;
                s_upload_meta_us = now_us();
                s_upload_sent = 0;
            } else {
                s_upload_ok = false;
                s_upload_pending = false;
                s_upload_done = true;
            }
            return;
        }
        if (s_upload_sent < pcm_len) {
            size_t part = pcm_len - s_upload_sent;
            if (part > (size_t)VOICE_UPLOAD_CHUNK) {
                part = (size_t)VOICE_UPLOAD_CHUNK;
            }
            if (!net_ws_send_audio_binary(pcm + s_upload_sent, part)) {
                /* 保持偏移不变，下一次 app loop 重试；总超时由 voice_loop 收敛。 */
                return;
            }
            s_upload_sent += part;
            return;
        }
        s_upload_meta_pending = false;
        ok = true;
        ESP_LOGI(TAG, "uploaded %u bytes in chunks", (unsigned)pcm_len);
    }

    s_upload_ok = ok;
    s_upload_pending = false;
    s_upload_done = true;
    s_upload_meta_pending = false;
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
            size_t pcm_len = 0;
            audio_capture_snapshot(NULL, &pcm_len);
            (void)net_ws_send_audio_end(s_session_id, pcm_len, true);
            ESP_LOGI(TAG, "discard stream on disable session=%s bytes=%u",
                     s_session_id, (unsigned)pcm_len);
        }
        s_upload_pending = false;
        s_upload_meta_pending = false;
        s_stream_active = false;
        s_stream_begin_pending = false;
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
    voice_reset_session("ws lost");
}

bool voice_is_enabled(void) { return s_voice_enabled; }
bool voice_session_active(void) { return s_session; }
bool voice_is_listening(void) { return s_listening; }

bool voice_is_busy(void) { return s_session; } /* 语音会话（录音/流式/播报）进行中 */

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

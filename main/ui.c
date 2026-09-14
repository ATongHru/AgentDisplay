#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "anim_loader.h"
#include "anim_size.h"
#include "board_pins.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mem_utils.h"
#include "net_ws.h"
#include "voice.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "font_cjk_16.h"
#include "font_status_icons.h"
#include "ui_status.h"
#include "ui_face.h"

static const char *TAG = "ui";

static const lv_font_t *ui_text_font(void)
{
    const lv_font_t *font = font_cjk_get();
    return font ? font : &lv_font_montserrat_14;
}
#define BAR_Y 9
#define ECHO_MAX_BYTES 256
#define CAPTION_LINES 2
#define CAPTION_TAIL_WINDOW 256
#define CAPTION_STREAM_IDLE_MS 500
#define ICON_SIZE 20
#define ICON_COLOR 0x94A3B8
#define CAPTION_TTL_MS 3000
#define CAPTION_LINE_SPACE 2
#define CAPTION_MAX_WIDTH 228
#define MIC_HINT_TTL_MS 1000

static QueueHandle_t s_queue;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *source_label;
static lv_obj_t *status_label;
static lv_obj_t *voice_label;
static lv_obj_t *mic_level_bar;
static lv_obj_t *diag_label;
static lv_obj_t *serial_icon;
static lv_obj_t *wifi_icon;
static lv_obj_t *mic_icon;
static lv_obj_t *spk_icon;
static lv_obj_t *time_label;

#define UI_EVENT_JSON_MAX 1024

static uint32_t last_event_ms;
static bool usb_link;
static bool wifi_connected;
static bool ws_link;
static bool offline_active;
static bool voice_listening;
static bool voice_feature_enabled = true;
static bool voice_playing;
static bool voice_text_visible;
static bool voice_session;
static bool s_ble_prov;
static bool s_ap_prov;
static char s_cached_face_status[24];
static char s_cached_face_source[16];

static bool prov_ui_active(void)
{
    return s_ble_prov || s_ap_prov;
}

bool ui_prov_active(void) { return prov_ui_active(); } /* 供 ui_face 查询 */

static bool caption_mode;
static bool caption_suppress_append;
static bool caption_streaming;
static bool mic_hint_active;
static uint32_t caption_until_ms;
static uint32_t last_append_ms;
static uint32_t mic_hint_until_ms;
static int wifi_rssi = -100;
static char last_time_buf[8] = "--:--";
static char current_status[24] = "IDLE";
static char current_source[16] = "BOT";
static char agent_status[24] = "IDLE";
static char agent_source[16] = "BOT";
static char current_voice_text[ECHO_MAX_BYTES + 32];
static char *s_voice_history;
static bool voice_hold;
static char voice_overlay[16];
static bool diagnostic_mode;
static uint32_t diagnostic_until_ms;
static char s_report_status[24];
static char s_report_source[16];
static bool s_report_dirty;
static portMUX_TYPE s_report_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    bool listening;
    bool playing;
    bool hold;
    char overlay[16];
    volatile bool updated;
} voice_link_pending_t;

static voice_link_pending_t s_vl_pending;
static portMUX_TYPE s_vl_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void str_upper(char *s)
{
    for (; *s; ++s) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static void json_value(const char *json, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!json || !key || out_len == 0) {
        return;
    }
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = json;
    while ((pos = strstr(pos, pattern)) != NULL) {
        if (pos > json) {
            char prev = pos[-1];
            if (prev != '{' && prev != ',') {
                pos += strlen(pattern);
                continue;
            }
        }
        const char *i = pos + strlen(pattern);
        while (*i == ' ' || *i == '\t') {
            ++i;
        }
        if (*i == '"') {
            ++i;
            size_t n = 0;
            while (*i && n + 1 < out_len) {
                if (*i == 0x5c && i[1]) {
                    char nch = i[1];
                    if (nch == 'n') {
                        out[n++] = ' ';
                    } else if (nch == '"' || nch == 0x5c) {
                        out[n++] = nch;
                    }
                    i += 2;
                    continue;
                }
                if (*i == '"') {
                    break;
                }
                out[n++] = *i++;
            }
            out[n] = '\0';
            return;
        }
        size_t n = 0;
        while (*i && *i != ',' && *i != '}' && n + 1 < out_len) {
            out[n++] = *i++;
        }
        out[n] = '\0';
        return;
    }
}

static const char *utf8_next_cp(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) {
        *cp = p[0];
        return s + 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        return s + 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
              (uint32_t)(p[2] & 0x3F);
        return s + 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        return s + 4;
    }
    *cp = p[0];
    return s + 1;
}

static lv_coord_t caption_glyph_width(const lv_font_t *font, uint32_t cp)
{
    lv_coord_t w = lv_font_get_glyph_width(font, cp, 0);
    if (w > 0) {
        return w;
    }
    w = lv_font_get_glyph_width(font, (uint32_t)' ', 0);
    if (w > 0) {
        return w;
    }
    return font && font->line_height > 0 ? (font->line_height * 3) / 4 : 12;
}

static lv_coord_t caption_max_width(void)
{
    return CAPTION_MAX_WIDTH;
}

static lv_coord_t caption_content_width(const char *text)
{
    if (!text || text[0] == '\0') {
        return 0;
    }
    const lv_font_t *font = ui_text_font();
    lv_coord_t line_w = 0;
    lv_coord_t max_w = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp = 0;
        const char *next = utf8_next_cp(p, &cp);
        if (cp == '\n') {
            if (line_w > max_w) {
                max_w = line_w;
            }
            line_w = 0;
            p = next;
            continue;
        }
        line_w += caption_glyph_width(font, cp);
        p = next;
    }
    if (line_w > max_w) {
        max_w = line_w;
    }
    return max_w;
}

/* Fold src to the last CAPTION_LINES wrapped rows so the label grows from the tail. */
static void caption_take_last_two_lines(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src || src[0] == '\0') {
        return;
    }
    const lv_font_t *font = ui_text_font();
    const lv_coord_t max_w = caption_max_width();
    char prev[128];
    char curr[128];
    size_t curr_len = 0;
    lv_coord_t x = 0;
    prev[0] = '\0';
    curr[0] = '\0';

    const char *p = src;
    while (*p) {
        uint32_t cp = 0;
        const char *next = utf8_next_cp(p, &cp);
        size_t clen = (size_t)(next - p);
        if (cp == '\r') {
            p = next;
            continue;
        }
        if (cp == '\n') {
            if (curr_len > 0) {
                memcpy(prev, curr, curr_len + 1);
                curr[0] = '\0';
                curr_len = 0;
                x = 0;
            }
            p = next;
            continue;
        }
        lv_coord_t gw = caption_glyph_width(font, cp);
        if (curr_len > 0 && x + gw > max_w) {
            memcpy(prev, curr, curr_len + 1);
            curr[0] = '\0';
            curr_len = 0;
            x = 0;
        }
        if (clen == 0 || curr_len + clen >= sizeof(curr) - 1) {
            p = next;
            continue;
        }
        memcpy(curr + curr_len, p, clen);
        curr_len += clen;
        curr[curr_len] = '\0';
        x += gw;
        p = next;
    }

    if (prev[0] && curr[0]) {
        snprintf(dst, dst_len, "%s\n%s", prev, curr);
    } else if (curr[0]) {
        snprintf(dst, dst_len, "%s", curr);
    } else {
        snprintf(dst, dst_len, "%s", prev);
    }
}

static const char *role_prefix(const char *role)
{
    (void)role;
    return "";
}

static void sync_voice_display(void)
{
    const char *src = s_voice_history;
    if (!src || src[0] == '\0') {
        current_voice_text[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    size_t window = CAPTION_TAIL_WINDOW;
    if (window > sizeof(current_voice_text) - 1) {
        window = sizeof(current_voice_text) - 1;
    }
    const char *start = src;
    if (len > window) {
        start = src + len - window;
        while (start > src && ((unsigned char)*start & 0xC0) == 0x80) {
            ++start;
        }
    }
    char tail[sizeof(current_voice_text)];
    snprintf(tail, sizeof(tail), "%s", start);
    caption_take_last_two_lines(tail, current_voice_text, sizeof(current_voice_text));
}

static void history_set(const char *text, const char *role)
{
    char line[ECHO_MAX_BYTES + 32];
    snprintf(line, sizeof(line), "%s%s", role_prefix(role), text ? text : "");
    if (!s_voice_history) {
        caption_take_last_two_lines(line, current_voice_text, sizeof(current_voice_text));
        return;
    }
    snprintf(s_voice_history, VOICE_HISTORY_BYTES, "%s", line);
    sync_voice_display();
}

static void history_append_role(const char *role, const char *text, bool reset)
{
    if (!text) {
        text = "";
    }
    if (!s_voice_history) {
        char tmp[sizeof(current_voice_text)];
        if (reset) {
            snprintf(tmp, sizeof(tmp), "%s%s", role_prefix(role), text);
        } else {
            snprintf(tmp, sizeof(tmp), "%s%s", current_voice_text, text);
        }
        caption_take_last_two_lines(tmp, current_voice_text, sizeof(current_voice_text));
        return;
    }
    if (reset) {
        snprintf(s_voice_history, VOICE_HISTORY_BYTES, "%s%s", role_prefix(role), text);
    } else {
        size_t used = strlen(s_voice_history);
        size_t room = VOICE_HISTORY_BYTES - 1 - used;
        if (room > 0) {
            strncat(s_voice_history, text, room);
        }
    }
    sync_voice_display();
}

static lv_color_t status_color(const char *status)
{
    return lv_color_hex(ui_status_info(ui_status_from_key(status))->color);
}

static void sync_voice_link_pending(void);
static void refresh_face_display(void);

static void apply_ble_prov_ui(bool on)
{
    s_ble_prov = on;
    if (on) {
        ui_face_render_bluetooth();
        if (status_label) {
            lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(status_label, "蓝牙配网");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0x60A5FA), 0);
        }
        if (source_label) {
            lv_label_set_text(source_label, "BLE");
        }
    } else if (!s_ap_prov) {
        if (source_label) {
            lv_label_set_text(source_label, agent_source[0] ? agent_source : "BOT");
        }
        s_cached_face_status[0] = 0;
        ui_face_sync_tick();
        refresh_face_display();
    }
}

static void apply_ap_prov_ui(bool on)
{
    if (on) {
        ui_face_render_ap_waiting();
        s_ap_prov = true;
        if (status_label) {
            lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(status_label, "热点配网");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFBBF24), 0);
        }
        if (source_label) {
            lv_label_set_text(source_label, "AP");
        }
    } else {
        s_ap_prov = false;
    }
    if (!on && !s_ble_prov) {
        if (source_label) {
            lv_label_set_text(source_label, agent_source[0] ? agent_source : "BOT");
        }
        s_cached_face_status[0] = 0;
        ui_face_sync_tick();
        refresh_face_display();
    }
}

static lv_coord_t caption_line_height(void)
{
    return lv_font_get_line_height(ui_text_font());
}

static void apply_voice_label_style(void)
{
    if (!voice_label) {
        return;
    }
    if (mic_hint_active && !caption_mode) {
        lv_obj_set_style_text_align(voice_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(voice_label, 0, 0);
        lv_obj_set_height(voice_label, caption_line_height());
    } else {
        lv_obj_set_style_text_align(voice_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(voice_label, CAPTION_LINE_SPACE, 0);
        lv_obj_set_height(voice_label, caption_line_height() * CAPTION_LINES + CAPTION_LINE_SPACE);
    }
}

static void fit_voice_label_width(const char *text)
{
    if (!voice_label) {
        return;
    }
    lv_coord_t w = caption_content_width(text) + 2;
    if (w < 8) {
        w = 8;
    }
    if (w > CAPTION_MAX_WIDTH) {
        w = CAPTION_MAX_WIDTH;
    }
    lv_obj_set_width(voice_label, w);
    lv_obj_align(voice_label, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void set_voice_label_text(const char *text)
{
    if (!voice_label) {
        return;
    }
    if (!text || text[0] == '\0') {
        lv_label_set_text(voice_label, "");
        lv_obj_add_flag(voice_label, LV_OBJ_FLAG_HIDDEN);
        voice_text_visible = false;
        return;
    }
    apply_voice_label_style();
    lv_obj_clear_flag(voice_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(voice_label, text);
    fit_voice_label_width(text);
    voice_text_visible = true;
}

static float s_mic_rms;
static volatile bool s_mic_pending;
static volatile float s_mic_pending_rms;

static bool mic_level_bar_wanted(void)
{
    if (!voice_feature_enabled || prov_ui_active() || !ws_link) {
        return false;
    }
    /* EAR.gif is the recording overlay; do not also require voice_listening —
     * link-state messages can briefly clear it and hide a visible bar. */
    return voice_hold && strcmp(voice_overlay, "EAR") == 0;
}

static void update_mic_level_ui(float rms)
{
    if (!mic_level_bar) {
        return;
    }
    if (rms < 0.0f) {
        rms = 0.0f;
    }
    s_mic_rms = rms;
    if (!mic_level_bar_wanted()) {
        lv_obj_add_flag(mic_level_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(mic_level_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mic_level_bar);
    /* AGC target ~0.3; map 0.25 RMS to full so speech is obvious. */
    int width = (int)(s_mic_rms * 80.0f / 0.25f);
    if (width < 2 && s_mic_rms > 0.002f) {
        width = 2;
    } else if (width > 80) {
        width = 80;
    }
    lv_bar_set_value(mic_level_bar, width, LV_ANIM_OFF);
}

static void sync_mic_level_pending(void)
{
    float rms;
    bool updated;
    portENTER_CRITICAL(&s_mux);
    updated = s_mic_pending;
    rms = s_mic_pending_rms;
    s_mic_pending = false;
    portEXIT_CRITICAL(&s_mux);
    if (updated) {
        update_mic_level_ui(rms);
    } else {
        update_mic_level_ui(s_mic_rms);
    }
}

static void build_diagnostic_text(char *out, size_t out_len)
{
    voice_debug_t vd = {0};
    voice_fill_debug(&vd);
    size_t rec_cap = audio_record_capacity();
    size_t play_cap = audio_play_ring_capacity();
    size_t rec_used = audio_capture_size();
    size_t play_used = audio_playback_pending();
    unsigned rec_pct = rec_cap ? (unsigned)(rec_used * 100 / rec_cap) : 0;
    unsigned play_pct = play_cap ? (unsigned)(play_used * 100 / play_cap) : 0;
    snprintf(out, out_len,
             "DRAM %u/%u KB\nPSRAM %u KB\nRec %u%% Play %u%%\nVAD rms %.3f/%.3f\nNoise %.3f/%.3f\nGain %.1fx",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), rec_pct, play_pct, vd.last_rms,
             vd.start_rms_th, vd.noise_rms, vd.noise_peak, (double)audio_get_pdm_gain());
}

static void apply_diagnostic_ui(bool on)
{
    diagnostic_mode = on;
    if (!diag_label) {
        return;
    }
    if (!on) {
        lv_obj_add_flag(diag_label, LV_OBJ_FLAG_HIDDEN);
        diagnostic_until_ms = 0;
        /* 诊断文本覆盖了表情区 GRAM，隐藏后强制重绘表情。 */
        ui_face_mark_dirty();
        return;
    }
    char buf[256];
    build_diagnostic_text(buf, sizeof(buf));
    lv_label_set_text(diag_label, buf);
    lv_obj_clear_flag(diag_label, LV_OBJ_FLAG_HIDDEN);
    diagnostic_until_ms = now_ms() + 12000;
}

static void set_status_label_text(const char *text, const char *status)
{
    if (!status_label) {
        return;
    }
    if ((!text || text[0] == '\0') && strcmp(status, "UNKNOWN") == 0) {
        lv_label_set_text(status_label, "");
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(status_label, text ? text : "");
    lv_obj_set_style_text_color(status_label, status_color(status), 0);
}

static void apply_source(const char *raw)
{
    if (!source_label) {
        return;
    }
    char source[16];
    snprintf(source, sizeof(source), "%s", raw ? raw : "BOT");
    str_upper(source);
    if (source[0] == '\0' || strcmp(source, "UNKNOWN") == 0) {
        strcpy(source, "BOT");
    }
    if (strcmp(source, "VOICE") == 0) {
        if (agent_source[0] && strcmp(agent_source, "VOICE") != 0) {
            lv_obj_clear_flag(source_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(source_label, agent_source);
        } else {
            lv_obj_add_flag(source_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_clear_flag(source_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(source_label, source);
}

static void queue_display_report(const char *status_key, const char *source)
{
    if (!status_key || !status_key[0]) {
        return;
    }
    const char *src = (source && source[0]) ? source : "BOT";
    portENTER_CRITICAL(&s_report_mux);
    if (!s_report_dirty && strcmp(s_report_status, status_key) == 0 &&
        strcmp(s_report_source, src) == 0) {
        portEXIT_CRITICAL(&s_report_mux);
        return;
    }
    strncpy(s_report_status, status_key, sizeof(s_report_status) - 1);
    s_report_status[sizeof(s_report_status) - 1] = '\0';
    strncpy(s_report_source, src, sizeof(s_report_source) - 1);
    s_report_source[sizeof(s_report_source) - 1] = '\0';
    s_report_dirty = true;
    portEXIT_CRITICAL(&s_report_mux);
}

static void render_status_pair(const char *status_key, const char *source)
{
    const ui_status_info_t *info = ui_status_info(ui_status_from_key(status_key));
    strncpy(current_status, info->key, sizeof(current_status) - 1);
    current_status[sizeof(current_status) - 1] = '\0';
    offline_active = (info->id == UI_ST_OFFLINE);
    if (ui_face_animation() != (size_t)info->id) {
        ui_face_set_animation((size_t)info->id);
    }
    if (source) {
        apply_source(source);
    }
    if (info->id == UI_ST_SPEAKING) {
        lv_label_set_text(status_label, "");
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_status_label_text(info->label_cn, info->key);
    }
}

static lv_opa_t wifi_opacity_for_rssi(int rssi)
{
    /* Map RSSI to icon strength (single glyph → opacity + color). */
    if (rssi >= -50) return LV_OPA_COVER;
    if (rssi >= -60) return LV_OPA_90;
    if (rssi >= -70) return LV_OPA_70;
    if (rssi >= -80) return LV_OPA_50;
    return LV_OPA_30;
}

static lv_color_t wifi_color_for_rssi(int rssi)
{
    if (rssi >= -55) return lv_color_hex(0x4ADE80); /* strong */
    if (rssi >= -70) return lv_color_hex(0xFBBF24); /* medium */
    return lv_color_hex(0xF87171); /* weak */
}

static lv_opa_t spk_opacity_for_volume(int percent, bool playing)
{
    (void)playing; /* icon follows configured volume, not playback activity */
    if (percent <= 0) {
        return LV_OPA_20;
    }
    if (percent > 100) {
        percent = 100;
    }
    /* 1..100% → OPA_55..COVER so mid volume stays visible (not near-black) */
    return (lv_opa_t)(55 + (percent * 45) / 100);
}

static lv_color_t spk_color_for_volume(int percent, bool playing)
{
    (void)playing;
    if (percent <= 0) {
        return lv_color_hex(0x4B5563); /* muted / muted */
    }
    if (percent >= 70) {
        return lv_color_hex(0xF8FAFC); /* near white */
    }
    if (percent >= 40) {
        return lv_color_hex(0xCBD5E1); /* light slate */
    }
    if (percent >= 15) {
        return lv_color_hex(0x94A3B8); /* readable mid */
    }
    return lv_color_hex(0x64748B);
}

static void bump_caption_ttl(void)
{
    caption_streaming = false;
    caption_until_ms = now_ms() + CAPTION_TTL_MS;
}

static void mark_caption_streaming(void)
{
    caption_streaming = true;
    last_append_ms = now_ms();
    caption_until_ms = 0;
}

static void layout_caption(void)
{
    if (!voice_label) {
        return;
    }
    if (caption_mode || mic_hint_active) {
        apply_voice_label_style();
        const char *shown = lv_label_get_text(voice_label);
        fit_voice_label_width(shown);
        lv_obj_clear_flag(voice_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(voice_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_caption_text(const char *text)
{
    if (!text || text[0] == '\0') {
        current_voice_text[0] = '\0';
        caption_mode = false;
        caption_until_ms = 0;
        set_voice_label_text("");
        layout_caption();
        return;
    }
    history_set(text, NULL);
    caption_suppress_append = false;
    if (!caption_mode) {
        caption_mode = true;
        layout_caption();
    }
    set_voice_label_text(current_voice_text);
    bump_caption_ttl();
}

static void arm_caption_ttl(void)
{
    if (!caption_mode || current_voice_text[0] == '\0') {
        return;
    }
    caption_streaming = false;
    caption_until_ms = now_ms() + CAPTION_TTL_MS;
}

static void expire_caption_if_needed(void)
{
    if (!caption_mode) {
        return;
    }
    /* Keep the subtitle on screen for the whole listen/think/speak turn. */
    if (voice_hold || voice_playing) {
        return;
    }
    if (caption_streaming) {
        if ((int32_t)(now_ms() - last_append_ms) >= (int32_t)CAPTION_STREAM_IDLE_MS) {
            arm_caption_ttl();
        }
        return;
    }
    if (caption_until_ms == 0) {
        return;
    }
    if ((int32_t)(now_ms() - caption_until_ms) >= 0) {
        caption_suppress_append = true;
        set_caption_text("");
    }
}

static void show_mic_hint(const char *text)
{
    mic_hint_active = true;
    mic_hint_until_ms = now_ms() + MIC_HINT_TTL_MS;
    if (!caption_mode) {
        layout_caption();
        set_voice_label_text(text);
    }
}

static void expire_mic_hint_if_needed(void)
{
    if (!mic_hint_active) {
        return;
    }
    if ((int32_t)(now_ms() - mic_hint_until_ms) < 0) {
        return;
    }
    mic_hint_active = false;
    if (!caption_mode) {
        layout_caption();
        set_voice_label_text("");
    }
}

static void enter_caption_mode(void)
{
    if (caption_mode) {
        return;
    }
    caption_mode = true;
    layout_caption();
}

static lv_obj_t *make_status_icon(lv_obj_t *parent, const char *symbol)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_width(obj, ICON_SIZE);
    lv_obj_set_style_text_font(obj, &font_status_icons_20, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(ICON_COLOR), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_label_set_text(obj, symbol);
    return obj;
}

static void layout_status_bar(void)
{
    if (!serial_icon || !wifi_icon || !mic_icon || !spk_icon || !time_label) {
        return;
    }
    lv_obj_update_layout(time_label);
    const int icon_w = ICON_SIZE;
    int time_w = (int)lv_obj_get_width(time_label);
    if (time_w < 1) {
        time_w = 56;
    }
    const int total = icon_w * 4 + time_w;
    const int gap = (LCD_W - total) / 6;
    const int leftover = LCD_W - total - gap * 6;
    int x = gap + leftover / 2;
    lv_obj_align(serial_icon, LV_ALIGN_TOP_LEFT, x, BAR_Y);
    x += icon_w + gap;
    lv_obj_align(mic_icon, LV_ALIGN_TOP_LEFT, x, BAR_Y);
    x += icon_w + gap;
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, x, BAR_Y);
    x += time_w + gap;
    lv_obj_align(spk_icon, LV_ALIGN_TOP_LEFT, x, BAR_Y);
    x += icon_w + gap;
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_LEFT, x, BAR_Y);
}

static void update_status_bar_ui(void)
{
    if (!serial_icon || !wifi_icon || !mic_icon || !spk_icon || !time_label) {
        return;
    }
    lv_label_set_text(serial_icon, UI_SYMBOL_USB);
    lv_obj_set_style_text_opa(serial_icon, usb_link ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_label_set_text(mic_icon, UI_SYMBOL_MIC);
    if (!voice_feature_enabled) {
        lv_obj_set_style_text_opa(mic_icon, LV_OPA_20, 0);
        lv_obj_set_style_text_color(mic_icon, lv_color_hex(0x4B5563), 0);
    } else {
        lv_obj_set_style_text_opa(mic_icon, voice_listening ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_text_color(mic_icon, lv_color_hex(voice_listening ? 0x38BDF8 : ICON_COLOR), 0);
    }

    const int vol = audio_get_volume_percent();
    lv_label_set_text(spk_icon, UI_SYMBOL_SPK);
    lv_obj_set_style_text_opa(spk_icon, spk_opacity_for_volume(vol, voice_playing), 0);
    lv_obj_set_style_text_color(spk_icon, spk_color_for_volume(vol, voice_playing), 0);

    lv_label_set_text(wifi_icon, UI_SYMBOL_WIFI);
    if (wifi_connected) {
        /* Strength follows RSSI whenever STA is up (WS optional). */
        lv_obj_set_style_text_opa(wifi_icon, wifi_opacity_for_rssi(wifi_rssi), 0);
        lv_obj_set_style_text_color(wifi_icon, wifi_color_for_rssi(wifi_rssi), 0);
        if (!ws_link) {
            /* Connected to AP but no backend: keep strength, slightly dimmer. */
            lv_opa_t opa = wifi_opacity_for_rssi(wifi_rssi);
            if (opa > LV_OPA_50) {
                opa = (lv_opa_t)(opa - 20);
            }
            lv_obj_set_style_text_opa(wifi_icon, opa, 0);
        }
    } else {
        lv_obj_set_style_text_opa(wifi_icon, LV_OPA_20, 0);
        lv_obj_set_style_text_color(wifi_icon, lv_color_hex(ICON_COLOR), 0);
    }
    layout_status_bar();
}

static void refresh_time_label(void)
{
    if (!time_label) {
        return;
    }
    time_t now = time(NULL);
    if (now > 1577836800) {
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        if (strncmp(buf, last_time_buf, sizeof(last_time_buf)) != 0) {
            strncpy(last_time_buf, buf, sizeof(last_time_buf) - 1);
            lv_label_set_text(time_label, buf);
            layout_status_bar();
        }
    } else {
        lv_label_set_text(time_label, "--:--");
        layout_status_bar();
    }
}

static void normalize_status(char *status)
{
    ui_status_id_t id = ui_status_from_key(status);
    snprintf(status, 24, "%s", ui_status_info(id)->key);
}

/* OFFLINE GIF is reserved for WS loss; ignore backend sessionEnd while connected. */
static void coerce_connected_status(char *status)
{
    if (ws_link && status && strcmp(status, "OFFLINE") == 0) {
        snprintf(status, 24, "IDLE");
    }
}

static bool source_is_voice(const char *source)
{
    return source && strcmp(source, "VOICE") == 0;
}

static bool voice_caption_is_content(const char *text)
{
    if (!text || text[0] == '\0') {
        return false;
    }
    if (strcmp(text, "正在识别") == 0 || strcmp(text, "正在聆听") == 0 ||
        strcmp(text, "聆听中") == 0 || strcmp(text, "回复中") == 0 ||
        strcmp(text, "正在思考") == 0 || strncmp(text, "正在回复", strlen("正在回复")) == 0 ||
        strcmp(text, "正在处理") == 0 || strcmp(text, "正在处理…") == 0) {
        return false;
    }
    for (unsigned i = 0; i < (unsigned)UI_ST_COUNT; ++i) {
        const char *label = k_ui_status[i].label_cn;
        if (label && label[0] && strcmp(text, label) == 0) {
            return false;
        }
    }
    return true;
}

static void show_voice_caption(const char *text, const char *role)
{
    if (voice_caption_is_content(text)) {
        history_set(text, role && role[0] ? role : "user");
        caption_suppress_append = false;
        if (!caption_mode) {
            caption_mode = true;
            layout_caption();
        }
        set_voice_label_text(current_voice_text);
        bump_caption_ttl();
    } else if (text && text[0]) {
        set_caption_text(text);
    }
}

static void apply_event(const char *raw_status, const char *raw_source, const char *raw_text, bool update_text)
{
    char status[24];
    char source[16];
    snprintf(status, sizeof(status), "%s", raw_status ? raw_status : "IDLE");
    snprintf(source, sizeof(source), "%s", raw_source ? raw_source : "BOT");
    normalize_status(status);
    coerce_connected_status(status);
    str_upper(source);
    if (source[0] == '\0' || strcmp(source, "UNKNOWN") == 0) {
        strcpy(source, "BOT");
    }

    if (source_is_voice(source)) {
        voice_session = true;
        if (update_text && raw_text && raw_text[0]) {
            show_voice_caption(raw_text, "assistant");
        }
        ESP_LOGI(TAG, "voice caption only status=%s", status);
        return;
    }

    strncpy(agent_status, status, sizeof(agent_status) - 1);
    agent_status[sizeof(agent_status) - 1] = '\0';
    strncpy(agent_source, source, sizeof(agent_source) - 1);
    agent_source[sizeof(agent_source) - 1] = '\0';

    if (!voice_hold || strcmp(status, "OFFLINE") == 0) {
        strncpy(current_source, source, sizeof(current_source) - 1);
        current_source[sizeof(current_source) - 1] = '\0';
        voice_session = false;
    }
    ESP_LOGI(TAG, "agent stash status=%s source=%s hold=%u overlay=%s", status, source,
             (unsigned)voice_hold, voice_overlay);
}

static void handle_text_event(const char *raw_source, const char *raw_text, const char *role)
{
    last_event_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    offline_active = false;

    char source[16];
    snprintf(source, sizeof(source), "%s", raw_source ? raw_source : "");
    str_upper(source);
    if (voice_hold && !source_is_voice(source)) {
        ESP_LOGI(TAG, "drop agent text (voice hold)");
        return;
    }
    if (source[0] && strcmp(source, "UNKNOWN") != 0 && !source_is_voice(source)) {
        strncpy(current_source, source, sizeof(current_source) - 1);
        current_source[sizeof(current_source) - 1] = '\0';
        strncpy(agent_source, source, sizeof(agent_source) - 1);
        agent_source[sizeof(agent_source) - 1] = '\0';
        apply_source(source);
        voice_session = false;
    } else if (source_is_voice(source)) {
        voice_session = true;
    } else {
        snprintf(source, sizeof(source), "%s", current_source);
    }

    if (!source_is_voice(source)) {
        return;
    }
    if (!raw_text || raw_text[0] == '\0') {
        if (voice_hold || voice_playing) {
            ESP_LOGI(TAG, "ignore empty caption during voice");
            return;
        }
        set_caption_text("");
        ESP_LOGI(TAG, "caption cleared");
        return;
    }
    show_voice_caption(raw_text, role && role[0] ? role : "assistant");
    ESP_LOGI(TAG, "caption source=%s len=%u", source, (unsigned)strlen(current_voice_text));
}

static void handle_append_event(const char *raw_source, const char *delta, bool reset, const char *role)
{
    last_event_ms = now_ms();
    offline_active = false;
    char src[16] = {0};
    if (raw_source && raw_source[0]) {
        snprintf(src, sizeof(src), "%s", raw_source);
        str_upper(src);
    }
    if (voice_hold && src[0] && !source_is_voice(src)) {
        return;
    }
    if (src[0] && !source_is_voice(src)) {
        return;
    }
    if (src[0]) {
        if (!source_is_voice(src) && strcmp(src, "UNKNOWN") != 0) {
            apply_source(src);
        }
        voice_session = source_is_voice(src);
    }
    if (reset) {
        caption_suppress_append = false;
        if (s_voice_history) {
            s_voice_history[0] = '\0';
        }
        current_voice_text[0] = '\0';
        enter_caption_mode();
        mark_caption_streaming();
    } else if (caption_suppress_append) {
        return;
    } else if (!caption_mode) {
        enter_caption_mode();
    }
    const char *use_role = role && role[0] ? role : "assistant";
    if (delta && delta[0]) {
        history_append_role(use_role, delta, reset);
        mark_caption_streaming();
    } else if (reset) {
        history_append_role(use_role, "", true);
        mark_caption_streaming();
    }
    if (current_voice_text[0] && caption_mode) {
        set_voice_label_text(current_voice_text);
    }
}

static void handle_event_json(const char *json, bool from_usb)
{
    last_event_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    offline_active = false;
    if (from_usb) {
        usb_link = true;
    }
    char type[16] = {0};
    char status[24] = {0};
    char source[16] = {0};
    char text[384] = {0};
    char role[16] = {0};
    json_value(json, "type", type, sizeof(type));
    json_value(json, "status", status, sizeof(status));
    json_value(json, "source", source, sizeof(source));
    json_value(json, "text", text, sizeof(text));
    json_value(json, "role", role, sizeof(role));
    if (strcmp(type, "append") == 0) {
        bool reset = false;
        const char *rp = strstr(json, "\"reset\"");
        if (rp) {
            rp = strchr(rp, ':');
            if (rp) {
                ++rp;
                while (*rp == ' ' || *rp == '\t') {
                    ++rp;
                }
                reset = (*rp == 't' || *rp == '1');
            }
        }
        handle_append_event(source, text, reset, role);
        return;
    }
    if (strcmp(type, "text") == 0 || status[0] == '\0') {
        handle_text_event(source, text, role);
        return;
    }
    apply_event(status, source, text, text[0] != '\0');
}


static void sync_voice_link_pending(void)
{
    voice_link_pending_t snap;
    portENTER_CRITICAL(&s_vl_mux);
    if (!s_vl_pending.updated) {
        portEXIT_CRITICAL(&s_vl_mux);
        return;
    }
    snap = s_vl_pending;
    s_vl_pending.updated = false;
    portEXIT_CRITICAL(&s_vl_mux);

    bool was_hold = voice_hold;
    bool was_playing = voice_playing;
    voice_listening = snap.listening;
    voice_playing = snap.playing;
    voice_hold = snap.hold;
    snprintf(voice_overlay, sizeof(voice_overlay), "%s", snap.overlay);
    bool was_busy = was_hold || was_playing;
    bool busy = voice_hold || voice_playing;
    if (was_hold && !voice_hold) {
        voice_session = false;
        strncpy(current_source, agent_source, sizeof(current_source) - 1);
        current_source[sizeof(current_source) - 1] = '\0';
        ESP_LOGI(TAG, "voice hold end -> agent %s %s", agent_status, agent_source);
    }
    if (was_busy && !busy) {
        arm_caption_ttl();
    }
}

static void refresh_face_display(void)
{
    if (prov_ui_active()) {
        return;
    }
    char status_key[24];
    char source[16];
    if (!ws_link) {
        snprintf(status_key, sizeof(status_key), "OFFLINE");
        snprintf(source, sizeof(source), "%s", agent_source[0] ? agent_source : "BOT");
    } else if (!voice_hold &&
               (uint32_t)(now_ms() - net_ws_last_rx_ms()) > OFFLINE_TIMEOUT_MS) {
        /* 半开连接兜底：WS 显示连着但心跳（2s）都断了 OFFLINE_TIMEOUT_MS，判 STALE。 */
        snprintf(status_key, sizeof(status_key), "STALE");
        snprintf(source, sizeof(source), "%s", agent_source[0] ? agent_source : "BOT");
    } else if (voice_hold) {
        snprintf(status_key, sizeof(status_key), "%s", voice_overlay[0] ? voice_overlay : "EAR");
        snprintf(source, sizeof(source), "%s", agent_source[0] ? agent_source : "BOT");
    } else {
        snprintf(status_key, sizeof(status_key), "%s", agent_status[0] ? agent_status : "IDLE");
        snprintf(source, sizeof(source), "%s", agent_source[0] ? agent_source : "BOT");
        coerce_connected_status(status_key);
    }
    normalize_status(status_key);
    if (strcmp(status_key, s_cached_face_status) == 0 && strcmp(source, s_cached_face_source) == 0) {
        return;
    }
    strncpy(s_cached_face_status, status_key, sizeof(s_cached_face_status) - 1);
    s_cached_face_status[sizeof(s_cached_face_status) - 1] = '\0';
    strncpy(s_cached_face_source, source, sizeof(s_cached_face_source) - 1);
    s_cached_face_source[sizeof(s_cached_face_source) - 1] = '\0';
    render_status_pair(status_key, source);
    if (ws_link) {
        queue_display_report(status_key, source);
    }
}

static int process_messages(void)
{
    int count = 0;
    ui_msg_t msg;
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
        ++count;
        if (msg.type == UI_MSG_EVENT_JSON) {
            handle_event_json(msg.json ? msg.json : "", msg.from_usb);
            free(msg.json);
        } else if (msg.type == UI_MSG_LINK_STATE) {
            usb_link = msg.usb;
            wifi_connected = msg.wifi;
            bool was_ws = ws_link;
            ws_link = msg.ws;
            voice_listening = msg.listening;
            voice_playing = msg.playing;
            wifi_rssi = msg.rssi;
            if (ws_link && !was_ws) {
                if (strcmp(agent_status, "OFFLINE") == 0) {
                    strncpy(agent_status, "IDLE", sizeof(agent_status) - 1);
                    agent_status[sizeof(agent_status) - 1] = '\0';
                }
                portENTER_CRITICAL(&s_report_mux);
                s_report_dirty = true;
                portEXIT_CRITICAL(&s_report_mux);
            }
            if (!ws_link) {
                voice_hold = false;
                voice_overlay[0] = '\0';
                voice_session = false;
                voice_playing = false;
            }
            update_status_bar_ui();
        } else if (msg.type == UI_MSG_VOICE_LINK) {
            portENTER_CRITICAL(&s_vl_mux);
            s_vl_pending.listening = msg.listening;
            s_vl_pending.playing = msg.playing;
            s_vl_pending.hold = msg.voice_hold;
            snprintf(s_vl_pending.overlay, sizeof(s_vl_pending.overlay), "%s",
                     msg.overlay[0] ? msg.overlay : "");
            s_vl_pending.updated = true;
            portEXIT_CRITICAL(&s_vl_mux);
            update_status_bar_ui();
        } else if (msg.type == UI_MSG_TIME_TICK) {
            refresh_time_label();
            update_status_bar_ui();
        } else if (msg.type == UI_MSG_BLE_PROV) {
            apply_ble_prov_ui(msg.ble_prov);
        } else if (msg.type == UI_MSG_AP_PROV) {
            apply_ap_prov_ui(msg.ap_prov);
        } else if (msg.type == UI_MSG_VOICE_ENABLED) {
            voice_feature_enabled = msg.voice_enabled;
            if (!voice_feature_enabled) {
                voice_listening = false;
            }
            show_mic_hint(voice_feature_enabled ? "麦克风开" : "麦克风关");
            update_status_bar_ui();
        } else if (msg.type == UI_MSG_VOLUME) {
            update_status_bar_ui();
        } else if (msg.type == UI_MSG_MIC_LEVEL) {
            /* Coalesced in ui_post_mic_level; keep as fallback. */
            update_mic_level_ui(msg.mic_rms);
        } else if (msg.type == UI_MSG_DIAGNOSTIC) {
            apply_diagnostic_ui(msg.diagnostic_on);
        }
    }
    return count;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x10151B), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    source_label = lv_label_create(scr);
    lv_obj_set_width(source_label, 220);
    lv_obj_set_style_text_font(source_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(source_label, lv_color_hex(0x718096), 0);
    lv_obj_set_style_text_align(source_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(source_label, "BOT");
    lv_obj_align(source_label, LV_ALIGN_TOP_MID, 0, 45);

    status_label = lv_label_create(scr);
    lv_obj_set_width(status_label, 220);
    lv_obj_set_height(status_label, 36);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(status_label, ui_text_font(), 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "空闲");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, FACE_Y + FACE_SIZE + 14);

    voice_label = lv_label_create(scr);
    lv_obj_set_width(voice_label, CAPTION_MAX_WIDTH);
    lv_label_set_long_mode(voice_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(voice_label, ui_text_font(), 0);
    lv_obj_set_style_text_color(voice_label, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_pad_all(voice_label, 0, 0);
    lv_obj_set_style_text_line_space(voice_label, CAPTION_LINE_SPACE, 0);
    lv_obj_clear_flag(voice_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(voice_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(voice_label, "");
    lv_obj_align(voice_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    mic_level_bar = lv_bar_create(scr);
    lv_obj_set_size(mic_level_bar, 88, 8);
    lv_bar_set_range(mic_level_bar, 0, 80);
    lv_obj_set_style_radius(mic_level_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(mic_level_bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(mic_level_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mic_level_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mic_level_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(mic_level_bar, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(mic_level_bar, lv_color_hex(0x4ADE80), LV_PART_INDICATOR);
    lv_obj_align(mic_level_bar, LV_ALIGN_TOP_MID, 0, FACE_Y + FACE_SIZE + 3);
    lv_obj_add_flag(mic_level_bar, LV_OBJ_FLAG_HIDDEN);

    diag_label = lv_label_create(scr);
    lv_obj_set_width(diag_label, 220);
    lv_label_set_long_mode(diag_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(diag_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(diag_label, lv_color_hex(0x4ADE80), 0);
    lv_obj_set_style_text_align(diag_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(diag_label, "");
    lv_obj_align(diag_label, LV_ALIGN_CENTER, 0, 24);
    lv_obj_add_flag(diag_label, LV_OBJ_FLAG_HIDDEN);

    serial_icon = make_status_icon(scr, UI_SYMBOL_USB);

    time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(time_label, "--:--");

    mic_icon = make_status_icon(scr, UI_SYMBOL_MIC);
    spk_icon = make_status_icon(scr, UI_SYMBOL_SPK);
    wifi_icon = make_status_icon(scr, UI_SYMBOL_WIFI);

    layout_status_bar();
}

bool ui_post(const ui_msg_t *msg)
{
    if (!s_queue || !msg) {
        return false;
    }
    if (xQueueSend(s_queue, msg, pdMS_TO_TICKS(20)) == pdTRUE) {
        return true;
    }
    /* 队列满限频告警，便于定位丢事件。 */
    static uint32_t s_drop_count;
    ++s_drop_count;
    if ((s_drop_count % 50) == 1) {
        ESP_LOGW(TAG, "ui queue full, drop type=%d total=%lu", (int)msg->type,
                 (unsigned long)s_drop_count);
    }
    return false;
}

void ui_post_event_json(const char *json, bool from_usb)
{
    /* JSON 单独分配（PSRAM 优先），队列只传指针，process_messages 消费后 free。 */
    const char *src = (json && json[0]) ? json : "{}";
    size_t len = strlen(src);
    if (len >= UI_EVENT_JSON_MAX) {
        len = UI_EVENT_JSON_MAX - 1;
    }
    char *copy = psram_malloc(len + 1);
    if (!copy) {
        ESP_LOGW(TAG, "event json alloc failed, dropped");
        return;
    }
    memcpy(copy, src, len);
    copy[len] = '\0';
    ui_msg_t msg = {.type = UI_MSG_EVENT_JSON, .from_usb = from_usb, .json = copy};
    if (!ui_post(&msg)) {
        ESP_LOGW(TAG, "event json queue full, dropped");
        free(copy);
    }
}

void ui_post_link_state(bool usb, bool wifi, bool ws, bool listening, bool playing, int rssi)
{
    ui_msg_t msg = {
        .type = UI_MSG_LINK_STATE,
        .usb = usb,
        .wifi = wifi,
        .ws = ws,
        .listening = listening,
        .playing = playing,
        .rssi = rssi,
    };
    ui_post(&msg);
}

void ui_post_voice_link(bool listening, bool playing, bool hold, const char *overlay)
{
    portENTER_CRITICAL(&s_vl_mux);
    s_vl_pending.listening = listening;
    s_vl_pending.playing = playing;
    s_vl_pending.hold = hold;
    snprintf(s_vl_pending.overlay, sizeof(s_vl_pending.overlay), "%s", overlay ? overlay : "");
    s_vl_pending.updated = true;
    portEXIT_CRITICAL(&s_vl_mux);

    ui_msg_t msg = {
        .type = UI_MSG_VOICE_LINK,
        .listening = listening,
        .playing = playing,
        .voice_hold = hold,
    };
    snprintf(msg.overlay, sizeof(msg.overlay), "%s", overlay ? overlay : "");
    ui_post(&msg);
}

void ui_post_time_tick(void)
{
    ui_msg_t msg = {.type = UI_MSG_TIME_TICK};
    ui_post(&msg);
}
void ui_post_ble_prov(bool on)
{
    ui_msg_t msg = {.type = UI_MSG_BLE_PROV, .ble_prov = on};
    ui_post(&msg);
}

void ui_post_ap_prov(bool on)
{
    ui_msg_t msg = {.type = UI_MSG_AP_PROV, .ap_prov = on};
    ui_post(&msg);
}

void ui_post_volume(int percent)
{
    ui_msg_t msg = {.type = UI_MSG_VOLUME, .volume_percent = percent};
    ui_post(&msg);
}


esp_err_t ui_init(void)
{
    s_queue = xQueueCreate(16, sizeof(ui_msg_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    s_voice_history = psram_malloc(VOICE_HISTORY_BYTES);
    if (!s_voice_history) {
        ESP_LOGW(TAG, "voice history PSRAM alloc failed, using 128B caption only");
    } else {
        s_voice_history[0] = '\0';
    }
    ui_face_init();
    build_ui();
    apply_source("BOT");
    apply_event("IDLE", "BOT", "", true);
    /* 资源缺失降级：占位脸 + 错误态文字，产测可直接识别（项26）。 */
    if (animation_count == 0) {
        ui_face_render_missing();
        set_status_label_text("无动画资源", "ERROR");
    }
    if (!font_cjk_get()) {
        set_status_label_text("FONT MISSING", "ERROR");
    }
    update_status_bar_ui();
    refresh_time_label();
    ui_face_sync_tick();
    last_event_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

static void expire_diagnostic_if_needed(void)
{
    if (!diagnostic_mode || diagnostic_until_ms == 0) {
        return;
    }
    if ((int32_t)(now_ms() - diagnostic_until_ms) >= 0) {
        apply_diagnostic_ui(false);
    }
}

uint32_t ui_loop_once(void)
{
    /* 动画推进收归 lvgl 任务：frame_index/frame_elapsed_ms 仅本任务读写，消除跨任务竞态。 */
    ui_face_tick(now_ms());
    int msgs = process_messages();
    sync_voice_link_pending();
    refresh_face_display();
    sync_mic_level_pending();
    expire_caption_if_needed();
    expire_mic_hint_if_needed();
    expire_diagnostic_if_needed();
    lv_timer_handler();
    /* 有活动保持 5ms 跟手；静帧空闲拉长到 30ms 降 Core1/SPI 空转（项34）。 */
    bool busy = ui_face_dirty_pending() || msgs > 0 || voice_listening || voice_playing ||
                prov_ui_active() || diagnostic_mode || mic_hint_active;
    ui_face_blit();
    return busy ? 5 : 30;
}

uint32_t ui_last_event_ms(void) { return last_event_ms; }
bool ui_offline_active(void) { return offline_active; }

bool ui_take_display_report(char *status, size_t status_len, char *source, size_t source_len)
{
    if (!status || status_len == 0 || !source || source_len == 0) {
        return false;
    }
    portENTER_CRITICAL(&s_report_mux);
    if (!s_report_dirty || !s_report_status[0]) {
        portEXIT_CRITICAL(&s_report_mux);
        return false;
    }
    strncpy(status, s_report_status, status_len - 1);
    status[status_len - 1] = '\0';
    strncpy(source, s_report_source, source_len - 1);
    source[source_len - 1] = '\0';
    s_report_dirty = false;
    portEXIT_CRITICAL(&s_report_mux);
    return true;
}

void ui_restore_display_report(void)
{
    portENTER_CRITICAL(&s_report_mux);
    if (s_report_status[0]) {
        s_report_dirty = true;
    }
    portEXIT_CRITICAL(&s_report_mux);
}
bool ui_voice_session_active(void) { return voice_session || voice_text_visible; }
void ui_set_ws_link(bool on) { ws_link = on; }
void ui_set_wifi(bool on, int rssi)
{
    wifi_connected = on;
    wifi_rssi = rssi;
}
void ui_set_usb(bool on) { usb_link = on; }
bool ui_get_ws_link(void) { return ws_link; }
bool ui_get_wifi(void) { return wifi_connected; }

void ui_post_voice_enabled(bool enabled)
{
    ui_msg_t msg = {.type = UI_MSG_VOICE_ENABLED, .voice_enabled = enabled};
    ui_post(&msg);
}

void ui_post_mic_level(float rms)
{
    portENTER_CRITICAL(&s_mux);
    s_mic_pending_rms = rms;
    s_mic_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

void ui_post_diagnostic_toggle(void)
{
    ui_msg_t msg = {.type = UI_MSG_DIAGNOSTIC, .diagnostic_on = true};
    ui_post(&msg);
}

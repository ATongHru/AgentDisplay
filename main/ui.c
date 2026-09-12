#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "anim_loader.h"
#include "anim_size.h"
#include "board_pins.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "font_cjk_16.h"
#include "font_status_icons.h"
#include "ui_status.h"

static const char *TAG = "ui";

static const lv_font_t *ui_text_font(void)
{
    const lv_font_t *font = font_cjk_get();
    return font ? font : &lv_font_montserrat_14;
}
#define FACE_SIZE ((int)ANIM_FACE_SIZE)
#define FACE_X ((LCD_W - FACE_SIZE) / 2)
#define FACE_Y 71
#define BAR_Y 9
#define ECHO_MAX_BYTES 120
#define ICON_SIZE 20
#define ICON_COLOR 0x94A3B8
#define CAPTION_TTL_MS 3000
#define MIC_HINT_TTL_MS 1000

static QueueHandle_t s_queue;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_frame_dirty_queued;

static lv_obj_t *source_label;
static lv_obj_t *status_label;
static lv_obj_t *voice_label;
static lv_obj_t *serial_icon;
static lv_obj_t *wifi_icon;
static lv_obj_t *mic_icon;
static lv_obj_t *spk_icon;
static lv_obj_t *time_label;

static uint8_t frame_buffer_a[ANIM_FACE_SIZE * ANIM_FACE_SIZE * 2];
static uint8_t frame_buffer_b[ANIM_FACE_SIZE * ANIM_FACE_SIZE * 2];
static uint8_t *frame_buffer_front = frame_buffer_a;

static uint32_t last_anim_tick;
static uint8_t frame_index;
static size_t animation_index;
static uint16_t frame_elapsed_ms;

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
static bool caption_mode;
static bool caption_suppress_append;
static bool mic_hint_active;
static uint32_t caption_until_ms;
static uint32_t mic_hint_until_ms;
static int wifi_rssi = -100;
static char last_time_buf[8] = "--:--";
static char current_status[24] = "IDLE";
static char current_source[16] = "BOT";
static char agent_status[24] = "IDLE";
static char agent_source[16] = "BOT";
static char current_voice_text[128];
static bool voice_hold;
static char voice_overlay[16];

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

static void truncate_utf8(char *text, size_t max_bytes)
{
    size_t len = strlen(text);
    if (len <= max_bytes) {
        return;
    }
    size_t i = max_bytes;
    while (i > 0 && ((unsigned char)text[i] & 0xC0) == 0x80) {
        --i;
    }
    text[i] = '\0';
}

static void truncate_voice(char *text)
{
    size_t len = strlen(text);
    int lines = 1;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\n') {
            ++lines;
            if (lines > 2) {
                text[i] = '\0';
                break;
            }
        }
    }
    truncate_utf8(text, ECHO_MAX_BYTES);
}

static lv_color_t status_color(const char *status)
{
    return lv_color_hex(ui_status_info(ui_status_from_key(status))->color);
}

static void fill_rgb565_bg(uint8_t *dest, size_t dest_size)
{
    const uint8_t lo = (uint8_t)(FACE_BG_RGB565 & 0xFF);
    const uint8_t hi = (uint8_t)(FACE_BG_RGB565 >> 8);
    for (size_t i = 0; i + 1 < dest_size; i += 2) {
        dest[i] = lo;
        dest[i + 1] = hi;
    }
}

static void decode_frame_rle(const frame_data_t *frame, uint8_t *dest, size_t dest_size)
{
    fill_rgb565_bg(dest, dest_size);
    size_t out = 0;
    for (size_t i = 0; i + 2 < frame->size && out + 1 < dest_size; i += 3) {
        uint8_t count = frame->data[i];
        uint8_t hi = frame->data[i + 1];
        uint8_t lo = frame->data[i + 2];
        while (count-- && out + 1 < dest_size) {
            dest[out++] = lo;
            dest[out++] = hi;
        }
    }
}


static void render_frame(void);
static void apply_voice_face_override(void);

static void put_px(uint8_t *buf, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= FACE_SIZE || y >= FACE_SIZE) {
        return;
    }
    size_t i = ((size_t)y * (size_t)FACE_SIZE + (size_t)x) * 2;
    buf[i] = (uint8_t)(color & 0xFF);
    buf[i + 1] = (uint8_t)(color >> 8);
}

static void draw_thick_line(uint8_t *buf, int x0, int y0, int x1, int y1, uint16_t color, int thick)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int ty = -thick; ty <= thick; ++ty) {
            for (int tx = -thick; tx <= thick; ++tx) {
                if (tx * tx + ty * ty <= thick * thick) {
                    put_px(buf, x0 + tx, y0 + ty, color);
                }
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Classic Bluetooth rune on face background. Color #3B82F6. */
static void render_bluetooth_face(void)
{
    fill_rgb565_bg(frame_buffer_a, sizeof(frame_buffer_a));
    frame_buffer_front = frame_buffer_a;
    const uint16_t blue = 0x3C1E; /* RGB565 #3B82F6 */
    const int cx = FACE_SIZE / 2;
    const int cy = FACE_SIZE / 2;
    const int s = FACE_SIZE / 3;
    draw_thick_line(frame_buffer_front, cx, cy - s, cx, cy + s, blue, 2);
    draw_thick_line(frame_buffer_front, cx, cy - s, cx + (s * 2) / 3, cy - s / 3, blue, 2);
    draw_thick_line(frame_buffer_front, cx + (s * 2) / 3, cy - s / 3, cx, cy, blue, 2);
    draw_thick_line(frame_buffer_front, cx, cy + s, cx + (s * 2) / 3, cy + s / 3, blue, 2);
    draw_thick_line(frame_buffer_front, cx + (s * 2) / 3, cy + s / 3, cx, cy, blue, 2);
}

static void apply_ble_prov_ui(bool on)
{
    s_ble_prov = on;
    if (on) {
        render_bluetooth_face();
        display_blit_rgb565(frame_buffer_front, FACE_X, FACE_Y, FACE_SIZE, FACE_SIZE);
        if (status_label) {
            lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(status_label, "蓝牙配网");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0x60A5FA), 0);
        }
        if (source_label) {
            lv_label_set_text(source_label, "BLE");
        }
    } else {
        if (source_label) {
            lv_label_set_text(source_label, agent_source[0] ? agent_source : "BOT");
        }
        apply_voice_face_override();
    }
}

static void blit_face_frame(void)
{
    if (!frame_buffer_front) {
        return;
    }
    if (!s_ble_prov && animation_count == 0) {
        return;
    }
    display_blit_rgb565(frame_buffer_front, FACE_X, FACE_Y, FACE_SIZE, FACE_SIZE);
}

static void render_frame(void)
{
    if (animation_count == 0 || animations[animation_index].count == 0) {
        return;
    }
    uint8_t *back = (frame_buffer_front == frame_buffer_a) ? frame_buffer_b : frame_buffer_a;
    decode_frame_rle(&animations[animation_index].frames[frame_index], back,
                     (size_t)FACE_SIZE * FACE_SIZE * 2);
    frame_buffer_front = back;
    blit_face_frame();
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
    lv_obj_clear_flag(voice_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(voice_label, text);
    voice_text_visible = true;
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
    lv_label_set_text(source_label, source);
}

static void render_status_pair(const char *status_key, const char *source)
{
    const ui_status_info_t *info = ui_status_info(ui_status_from_key(status_key));
    strncpy(current_status, info->key, sizeof(current_status) - 1);
    current_status[sizeof(current_status) - 1] = '\0';
    offline_active = (info->id == UI_ST_OFFLINE);
    if (animation_index != (size_t)info->id) {
        animation_index = (size_t)info->id;
        frame_index = 0;
        frame_elapsed_ms = 0;
        render_frame();
    }
    if (source) {
        apply_source(source);
    }
    set_status_label_text(info->label_cn, info->key);
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
    caption_until_ms = now_ms() + CAPTION_TTL_MS;
}

static void layout_caption(void)
{
    if (!voice_label) {
        return;
    }
    if (caption_mode || mic_hint_active) {
        lv_obj_set_width(voice_label, 228);
        lv_obj_set_height(voice_label, 40);
        lv_obj_align(voice_label, LV_ALIGN_BOTTOM_MID, 0, -8);
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
    snprintf(current_voice_text, sizeof(current_voice_text), "%s", text);
    truncate_voice(current_voice_text);
    caption_suppress_append = false;
    if (!caption_mode) {
        caption_mode = true;
        layout_caption();
    }
    set_voice_label_text(current_voice_text);
    bump_caption_ttl();
}

static void expire_caption_if_needed(void)
{
    if (voice_hold) {
        return;
    }
    if (!caption_mode || caption_until_ms == 0) {
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
    bump_caption_ttl();
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

static bool source_is_voice(const char *source)
{
    return source && strcmp(source, "VOICE") == 0;
}

static void apply_event(const char *raw_status, const char *raw_source, const char *raw_text, bool update_text)
{
    char status[24];
    char source[16];
    snprintf(status, sizeof(status), "%s", raw_status ? raw_status : "IDLE");
    snprintf(source, sizeof(source), "%s", raw_source ? raw_source : "BOT");
    normalize_status(status);
    str_upper(source);
    if (source[0] == '\0' || strcmp(source, "UNKNOWN") == 0) {
        strcpy(source, "BOT");
    }

    if (source_is_voice(source)) {
        voice_session = true;
        if (update_text && raw_text && raw_text[0]) {
            set_caption_text(raw_text);
        }
        ESP_LOGI(TAG, "voice caption only status=%s", status);
        return;
    }

    strncpy(agent_status, status, sizeof(agent_status) - 1);
    agent_status[sizeof(agent_status) - 1] = '\0';
    strncpy(agent_source, source, sizeof(agent_source) - 1);
    agent_source[sizeof(agent_source) - 1] = '\0';

    /* Listening / speaking own the face. Stash agent, don't render. */
    if (voice_hold && strcmp(status, "OFFLINE") != 0) {
        ESP_LOGI(TAG, "drop agent status=%s source=%s (voice hold %s)", status, source,
                 voice_overlay);
        return;
    }

    strncpy(current_source, source, sizeof(current_source) - 1);
    current_source[sizeof(current_source) - 1] = '\0';
    voice_session = false;
    render_status_pair(status, source);
    ESP_LOGI(TAG, "apply status=%s source=%s caption=%u anim=%u", status, source,
             (unsigned)strlen(current_voice_text), (unsigned)animation_index);
}

static void handle_text_event(const char *raw_source, const char *raw_text)
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

    set_caption_text(raw_text);
    ESP_LOGI(TAG, "caption source=%s len=%u", source, (unsigned)strlen(current_voice_text));
}

static void handle_append_event(const char *raw_source, const char *delta, bool reset)
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
    if (src[0]) {
        if (!source_is_voice(src) && strcmp(src, "UNKNOWN") != 0) {
            apply_source(src);
        }
        voice_session = source_is_voice(src);
    }
    if (reset) {
        caption_suppress_append = false;
        current_voice_text[0] = '\0';
        enter_caption_mode();
        bump_caption_ttl();
    } else if (caption_suppress_append) {
        return;
    } else if (!caption_mode) {
        enter_caption_mode();
    }
    if (delta && delta[0]) {
        size_t used = strlen(current_voice_text);
        size_t room = sizeof(current_voice_text) - 1 - used;
        if (room > 0) {
            strncat(current_voice_text, delta, room);
        }
        truncate_voice(current_voice_text);
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
    char text[128] = {0};
    json_value(json, "type", type, sizeof(type));
    json_value(json, "status", status, sizeof(status));
    json_value(json, "source", source, sizeof(source));
    json_value(json, "text", text, sizeof(text));
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
        handle_append_event(source, text, reset);
        return;
    }
    if (strcmp(type, "text") == 0 || status[0] == '\0') {
        handle_text_event(source, text);
        return;
    }
    apply_event(status, source, text, text[0] != '\0');
}


static void apply_voice_face_override(void)
{
    if (s_ble_prov) {
        return;
    }
    if (voice_hold) {
        const char *face = voice_overlay[0] ? voice_overlay : "EAR";
        render_status_pair(face, "VOICE");
        return;
    }
    render_status_pair(agent_status, agent_source[0] ? agent_source : "BOT");
}

static void process_messages(void)
{
    ui_msg_t msg;
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
        if (msg.type == UI_MSG_EVENT_JSON) {
            handle_event_json(msg.json, msg.from_usb);
        } else if (msg.type == UI_MSG_FRAME_DIRTY) {
            portENTER_CRITICAL(&s_mux);
            s_frame_dirty_queued = false;
            portEXIT_CRITICAL(&s_mux);
            render_frame();
        } else if (msg.type == UI_MSG_LINK_STATE) {
            usb_link = msg.usb;
            wifi_connected = msg.wifi;
            ws_link = msg.ws;
            voice_listening = msg.listening;
            voice_playing = msg.playing;
            wifi_rssi = msg.rssi;
            if (!ws_link) {
                if (strcmp(current_status, "OFFLINE") != 0) {
                    apply_event("OFFLINE", "BOT", "", false);
                }
            }
            update_status_bar_ui();
            apply_voice_face_override();
        } else if (msg.type == UI_MSG_VOICE_LINK) {
            voice_listening = msg.listening;
            voice_playing = msg.playing;
            bool was_hold = voice_hold;
            voice_hold = msg.voice_hold;
            snprintf(voice_overlay, sizeof(voice_overlay), "%s", msg.overlay[0] ? msg.overlay : "");
            update_status_bar_ui();
            apply_voice_face_override();
            if (was_hold && !voice_hold) {
                ESP_LOGI(TAG, "voice hold end, restore agent %s %s", agent_status, agent_source);
            }
        } else if (msg.type == UI_MSG_TIME_TICK) {
            refresh_time_label();
            update_status_bar_ui();
        } else if (msg.type == UI_MSG_BLE_PROV) {
            apply_ble_prov_ui(msg.ble_prov);
        } else if (msg.type == UI_MSG_VOICE_ENABLED) {
            voice_feature_enabled = msg.voice_enabled;
            if (!voice_feature_enabled) {
                voice_listening = false;
            }
            show_mic_hint(voice_feature_enabled ? "麦克风开" : "麦克风关");
            update_status_bar_ui();
            apply_voice_face_override();
        } else if (msg.type == UI_MSG_VOLUME) {
            update_status_bar_ui();
        }
    }
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
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, FACE_Y + FACE_SIZE + 6);

    voice_label = lv_label_create(scr);
    lv_obj_set_width(voice_label, 236);
    lv_obj_set_height(voice_label, 34);
    lv_label_set_long_mode(voice_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(voice_label, ui_text_font(), 0);
    lv_obj_set_style_text_color(voice_label, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_align(voice_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(voice_label, "");
    lv_obj_add_flag(voice_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(voice_label, LV_ALIGN_BOTTOM_MID, 0, -8);

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

void ui_post(const ui_msg_t *msg)
{
    if (!s_queue || !msg) {
        return;
    }
    if (msg->type == UI_MSG_FRAME_DIRTY) {
        portENTER_CRITICAL(&s_mux);
        if (s_frame_dirty_queued) {
            portEXIT_CRITICAL(&s_mux);
            return;
        }
        s_frame_dirty_queued = true;
        if (xQueueSend(s_queue, msg, 0) != pdTRUE) {
            s_frame_dirty_queued = false;
        }
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    xQueueSend(s_queue, msg, pdMS_TO_TICKS(20));
}

void ui_post_event_json(const char *json, bool from_usb)
{
    ui_msg_t msg = {.type = UI_MSG_EVENT_JSON, .from_usb = from_usb};
    strncpy(msg.json, json ? json : "", sizeof(msg.json) - 1);
    ui_post(&msg);
}

void ui_post_frame_dirty(void)
{
    ui_msg_t msg = {.type = UI_MSG_FRAME_DIRTY};
    ui_post(&msg);
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
    fill_rgb565_bg(frame_buffer_a, sizeof(frame_buffer_a));
    fill_rgb565_bg(frame_buffer_b, sizeof(frame_buffer_b));
    build_ui();
    apply_source("BOT");
    apply_event("IDLE", "BOT", "", true);
    update_status_bar_ui();
    refresh_time_label();
    last_anim_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    last_event_ms = last_anim_tick;
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

void ui_loop_once(void)
{
    process_messages();
    expire_caption_if_needed();
    expire_mic_hint_if_needed();
    lv_timer_handler();
    blit_face_frame();
}

void ui_tick_animation(uint32_t now_ms)
{
    if (s_ble_prov) {
        return;
    }
    if (animation_count == 0 || animations[animation_index].count == 0) {
        return;
    }
    uint32_t elapsed = now_ms - last_anim_tick;
    last_anim_tick = now_ms;
    frame_elapsed_ms = (uint16_t)(frame_elapsed_ms + elapsed);
    const frame_data_t *frame = &animations[animation_index].frames[frame_index];
    if (frame_elapsed_ms >= frame->duration_ms) {
        frame_elapsed_ms = (uint16_t)(frame_elapsed_ms - frame->duration_ms);
        frame_index = (uint8_t)((frame_index + 1) % animations[animation_index].count);
        ui_post_frame_dirty();
    }
}

uint32_t ui_last_event_ms(void) { return last_event_ms; }
bool ui_offline_active(void) { return offline_active; }
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

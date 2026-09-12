#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static const char *TAG = "ui";

#define FACE_SIZE ((int)ANIM_FACE_SIZE)
#define FACE_X ((LCD_W - FACE_SIZE) / 2)
#define FACE_Y 34
#define BAR_PAD 16
#define BAR_Y -8
#define BAR_GAP 8

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
static bool voice_playing;
static bool voice_text_visible;
static bool voice_session;
static int wifi_rssi = -100;
static char last_time_buf[8] = "--:--";
static char current_status[24] = "IDLE";
static char current_source[16] = "BOT";
static char current_voice_text[96];

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
    len = strlen(text);
    if (len > 48) {
        text[47] = '\0';
    }
}

static lv_color_t status_color(const char *status)
{
    if (strcmp(status, "ERROR") == 0) return lv_color_hex(0xFF4D5A);
    if (strcmp(status, "DONE") == 0) return lv_color_hex(0x45D483);
    if (strcmp(status, "CODING") == 0) return lv_color_hex(0xA78BFA);
    if (strcmp(status, "READING") == 0) return lv_color_hex(0x22D3EE);
    if (strcmp(status, "TESTING") == 0) return lv_color_hex(0x2DD4A7);
    if (strcmp(status, "WAITING") == 0) return lv_color_hex(0xFBBF24);
    if (strcmp(status, "THINKING") == 0) return lv_color_hex(0x818CF8);
    if (strcmp(status, "OFFLINE") == 0) return lv_color_hex(0x94A3B8);
    return lv_color_hex(0xE2E8F0);
}

static size_t animation_for_status(const char *status)
{
    if (strcmp(status, "THINKING") == 0) return 1;
    if (strcmp(status, "CODING") == 0) return 2;
    if (strcmp(status, "READING") == 0) return 3;
    if (strcmp(status, "TESTING") == 0) return 4;
    if (strcmp(status, "WAITING") == 0) return 5;
    if (strcmp(status, "DONE") == 0) return 6;
    if (strcmp(status, "ERROR") == 0) return 7;
    if (strcmp(status, "OFFLINE") == 0) return 8;
    if (strcmp(status, "STALE") == 0) return 9;
    if (strcmp(status, "UNKNOWN") == 0) return 10;
    return 0;
}

static const char *status_cn(const char *status)
{
    if (strcmp(status, "IDLE") == 0) return "空闲";
    if (strcmp(status, "THINKING") == 0) return "思考中";
    if (strcmp(status, "CODING") == 0) return "编码中";
    if (strcmp(status, "READING") == 0) return "读取中";
    if (strcmp(status, "TESTING") == 0) return "测试中";
    if (strcmp(status, "WAITING") == 0) return "等待中";
    if (strcmp(status, "DONE") == 0) return "已完成";
    if (strcmp(status, "ERROR") == 0) return "错误";
    if (strcmp(status, "OFFLINE") == 0) return "离线";
    if (strcmp(status, "STALE") == 0) return "已过期";
    if (strcmp(status, "UNKNOWN") == 0) return "";
    return status;
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

static void blit_face_frame(void)
{
    if (!frame_buffer_front || animation_count == 0) {
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

static void reset_animation(const char *status)
{
    animation_index = animation_for_status(status);
    frame_index = 0;
    frame_elapsed_ms = 0;
    render_frame();
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

static void apply_agent_status_label(const char *status)
{
    set_status_label_text(status_cn(status), status);
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

static lv_opa_t wifi_opacity_for_rssi(int rssi)
{
    if (rssi >= -55) return LV_OPA_COVER;
    if (rssi >= -70) return LV_OPA_80;
    if (rssi >= -85) return LV_OPA_50;
    return LV_OPA_30;
}

static void layout_status_bar(void)
{
    if (!serial_icon || !wifi_icon || !mic_icon || !spk_icon || !time_label) {
        return;
    }
    lv_obj_align(serial_icon, LV_ALIGN_BOTTOM_LEFT, BAR_PAD, BAR_Y);
    lv_obj_align(wifi_icon, LV_ALIGN_BOTTOM_RIGHT, -BAR_PAD, BAR_Y);
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_MID, 0, BAR_Y);
    lv_obj_align_to(mic_icon, time_label, LV_ALIGN_OUT_LEFT_MID, -BAR_GAP, 0);
    lv_obj_align_to(spk_icon, time_label, LV_ALIGN_OUT_RIGHT_MID, BAR_GAP, 0);
}

static void update_status_bar_ui(void)
{
    if (!serial_icon || !wifi_icon || !mic_icon || !spk_icon || !time_label) {
        return;
    }
    lv_label_set_text(serial_icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_opa(serial_icon, usb_link ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_label_set_text(mic_icon, UI_SYMBOL_MIC);
    lv_obj_set_style_text_opa(mic_icon, voice_listening ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_label_set_text(spk_icon, UI_SYMBOL_SPK);
    lv_obj_set_style_text_opa(spk_icon, voice_playing ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    if (wifi_connected) {
        lv_opa_t opa = ws_link ? wifi_opacity_for_rssi(wifi_rssi) : LV_OPA_40;
        lv_obj_set_style_text_opa(wifi_icon, opa, 0);
    } else {
        lv_obj_set_style_text_opa(wifi_icon, LV_OPA_20, 0);
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
    str_upper(status);
    if (strcmp(status, "WORKING") == 0) {
        strcpy(status, "CODING");
    }
    if (strcmp(status, "TOOL_CALL") == 0) {
        strcpy(status, "THINKING");
    }
    if (status[0] == '\0') {
        strcpy(status, "IDLE");
    }
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

    const bool status_changed = strcmp(status, current_status) != 0;
    const size_t target_animation = animation_for_status(status);
    const bool animation_mismatch = (animation_index != target_animation);
    offline_active = (strcmp(status, "OFFLINE") == 0);
    strncpy(current_status, status, sizeof(current_status) - 1);

    if (strcmp(source, "VOICE") != 0) {
        strncpy(current_source, source, sizeof(current_source) - 1);
        apply_source(source);
        current_voice_text[0] = '\0';
        set_voice_label_text("");
        voice_session = false;
    } else {
        apply_source("VOICE");
        voice_session = true;
    }

    char display_text[96];
    snprintf(display_text, sizeof(display_text), "%s", raw_text ? raw_text : "");
    if (strcmp(source, "VOICE") == 0 && display_text[0]) {
        truncate_voice(display_text);
    }

    if (status_changed || animation_mismatch) {
        reset_animation(status);
    }

    apply_agent_status_label(status);
    if (strcmp(source, "VOICE") == 0) {
        if (display_text[0] && update_text) {
            strncpy(current_voice_text, display_text, sizeof(current_voice_text) - 1);
            set_voice_label_text(display_text);
        } else if (strcmp(status, "IDLE") == 0) {
            current_voice_text[0] = '\0';
            set_voice_label_text("");
        }
    } else {
        current_voice_text[0] = '\0';
        if (strcmp(source, "CURSOR") != 0 && display_text[0] && update_text) {
            set_voice_label_text(display_text);
        } else {
            set_voice_label_text("");
        }
    }
    ESP_LOGI(TAG, "apply status=%s source=%s anim=%u", status, source, (unsigned)animation_index);
}

static void handle_event_json(const char *json, bool from_usb)
{
    last_event_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    offline_active = false;
    if (from_usb) {
        usb_link = true;
    }
    char status[24] = {0};
    char source[16] = {0};
    char text[96] = {0};
    json_value(json, "status", status, sizeof(status));
    json_value(json, "source", source, sizeof(source));
    json_value(json, "text", text, sizeof(text));
    if (status[0] == '\0') {
        return;
    }
    apply_event(status, source, text, text[0] != '\0');
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
            if (ws_link && strcmp(current_status, "OFFLINE") == 0 && !voice_session) {
                apply_event("IDLE", "BOT", "", true);
            }
            update_status_bar_ui();
        } else if (msg.type == UI_MSG_VOICE_LINK) {
            voice_listening = msg.listening;
            voice_playing = msg.playing;
            update_status_bar_ui();
        } else if (msg.type == UI_MSG_TIME_TICK) {
            refresh_time_label();
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
    lv_obj_align(source_label, LV_ALIGN_TOP_MID, 0, 8);

    status_label = lv_label_create(scr);
    lv_obj_set_width(status_label, 220);
    lv_obj_set_style_text_font(status_label, &font_cjk_16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "空闲");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, FACE_Y + FACE_SIZE + 6);

    voice_label = lv_label_create(scr);
    lv_obj_set_width(voice_label, 236);
    lv_obj_set_height(voice_label, 34);
    lv_label_set_long_mode(voice_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(voice_label, &font_cjk_16, 0);
    lv_obj_set_style_text_color(voice_label, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_align(voice_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(voice_label, "");
    lv_obj_add_flag(voice_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(voice_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    serial_icon = lv_label_create(scr);
    lv_obj_set_style_text_font(serial_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(serial_icon, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(serial_icon, LV_SYMBOL_USB);

    time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xCBD5E1), 0);
    lv_label_set_text(time_label, "--:--");

    mic_icon = lv_label_create(scr);
    lv_obj_set_style_text_font(mic_icon, &font_status_icons_20, 0);
    lv_obj_set_style_text_color(mic_icon, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(mic_icon, UI_SYMBOL_MIC);

    spk_icon = lv_label_create(scr);
    lv_obj_set_style_text_font(spk_icon, &font_status_icons_20, 0);
    lv_obj_set_style_text_color(spk_icon, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(spk_icon, UI_SYMBOL_SPK);

    wifi_icon = lv_label_create(scr);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);

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
    xQueueSend(s_queue, msg, pdMS_TO_TICKS(2));
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

void ui_post_voice_link(bool listening, bool playing)
{
    ui_msg_t msg = {.type = UI_MSG_VOICE_LINK, .listening = listening, .playing = playing};
    ui_post(&msg);
}

void ui_post_time_tick(void)
{
    ui_msg_t msg = {.type = UI_MSG_TIME_TICK};
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
    lv_timer_handler();
    blit_face_frame();
}

void ui_tick_animation(uint32_t now_ms)
{
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

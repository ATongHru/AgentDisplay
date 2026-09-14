#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_MSG_EVENT_JSON = 1,
    UI_MSG_LINK_STATE = 2,
    UI_MSG_VOICE_LINK = 3,
    UI_MSG_TIME_TICK = 4,
    UI_MSG_BLE_PROV = 5,
    UI_MSG_VOLUME = 6,
    UI_MSG_VOICE_ENABLED = 7,
    UI_MSG_MIC_LEVEL = 8,
    UI_MSG_DIAGNOSTIC = 9,
    UI_MSG_AP_PROV = 10,
} ui_msg_type_t;

typedef struct {
    ui_msg_type_t type;
    char *json; /* UI_MSG_EVENT_JSON 专用：发送方 PSRAM 分配，消费端负责 free */
    bool from_usb;
    bool usb;
    bool wifi;
    bool ws;
    bool listening;
    bool playing;
    bool voice_hold;
    char overlay[16];
    bool ble_prov;
    bool ap_prov;
    int rssi;
    int volume_percent;
    bool voice_enabled;
    float mic_rms;
    bool diagnostic_on;
} ui_msg_t;

bool ui_post(const ui_msg_t *msg);
void ui_post_event_json(const char *json, bool from_usb);
void ui_post_link_state(bool usb, bool wifi, bool ws, bool listening, bool playing, int rssi);
void ui_post_voice_link(bool listening, bool playing, bool hold, const char *overlay);
void ui_post_time_tick(void);
void ui_post_ble_prov(bool on);
void ui_post_ap_prov(bool on);
void ui_post_volume(int percent);
void ui_post_voice_enabled(bool enabled);
void ui_post_mic_level(float rms);
void ui_post_diagnostic_toggle(void);

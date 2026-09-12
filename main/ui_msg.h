#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_MSG_EVENT_JSON = 1,
    UI_MSG_FRAME_DIRTY = 2,
    UI_MSG_LINK_STATE = 3,
    UI_MSG_VOICE_LINK = 4,
    UI_MSG_TIME_TICK = 5,
    UI_MSG_BLE_PROV = 6,
    UI_MSG_VOLUME = 7,
    UI_MSG_VOICE_ENABLED = 8,
} ui_msg_type_t;

typedef struct {
    ui_msg_type_t type;
    char json[512];
    bool from_usb;
    bool usb;
    bool wifi;
    bool ws;
    bool listening;
    bool playing;
    bool voice_hold;
    char overlay[16];
    bool ble_prov;
    int rssi;
    int volume_percent;
    bool voice_enabled;
} ui_msg_t;

void ui_post(const ui_msg_t *msg);
void ui_post_event_json(const char *json, bool from_usb);
void ui_post_frame_dirty(void);
void ui_post_link_state(bool usb, bool wifi, bool ws, bool listening, bool playing, int rssi);
void ui_post_voice_link(bool listening, bool playing, bool hold, const char *overlay);
void ui_post_time_tick(void);
void ui_post_ble_prov(bool on);
void ui_post_volume(int percent);
void ui_post_voice_enabled(bool enabled);

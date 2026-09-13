#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ui_msg.h"

esp_err_t ui_init(void);
void ui_loop_once(void);
void ui_tick_animation(uint32_t now_ms);
uint32_t ui_last_event_ms(void);
bool ui_offline_active(void);
bool ui_voice_session_active(void);
void ui_set_ws_link(bool on);
void ui_set_wifi(bool on, int rssi);
void ui_set_usb(bool on);
bool ui_get_ws_link(void);
bool ui_get_wifi(void);
bool ui_take_display_report(char *status, size_t status_len, char *source, size_t source_len);
void ui_restore_display_report(void);

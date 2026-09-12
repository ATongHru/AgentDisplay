#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t net_init(void);
esp_err_t net_apply_config(void);
void net_pause_for_ble(void);
void net_resume_after_ble(void);
void net_loop(void);
bool net_wifi_ready(void);
bool net_ws_ready(void);
bool net_usb_ready(void);
int net_rssi(void);
bool net_ws_send_audio_upload(const uint8_t *pcm, size_t pcm_len, char *session_id, size_t session_id_len);
bool net_ws_send_audio_stream_begin(char *session_id, size_t session_id_len);
bool net_ws_send_audio_binary(const uint8_t *pcm, size_t pcm_len);
bool net_ws_send_audio_end(const char *session_id);

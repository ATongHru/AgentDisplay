#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t net_init(void);
void net_loop(void);
bool net_wifi_ready(void);
bool net_ws_ready(void);
int net_rssi(void);
bool net_ws_send_audio_upload(const uint8_t *pcm, size_t pcm_len, char *session_id, size_t session_id_len);

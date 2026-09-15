#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t net_init(void);
esp_err_t net_apply_config(void);
/** 配网 APPLY 后重置 AP 自动回退计时，避免 BLE 耗时超过 30s 后立即进热点。 */
void net_on_config_applied(void);
void net_pause_for_ble(void);
void net_resume_after_ble(void);
void net_pause_sta_for_ap(void);
void net_resume_sta_after_ap(void);
esp_err_t net_force_ap_prov(void);
void net_loop(void);
bool net_wifi_ready(void);
bool net_ws_ready(void);
bool net_usb_ready(void);
int net_rssi(void);
/* 异步 session 握手：start 只发元信息立即返回；poll 查询后端 session 是否就绪。 */
uint32_t net_ws_last_rx_ms(void); /* 最近一次收到任意 WS 帧的 tick(ms)，供 UI 判 STALE */
bool net_ws_audio_session_start(bool stream, size_t audio_len);
bool net_ws_audio_session_poll(char *session_id, size_t session_id_len);
bool net_ws_send_audio_binary(const uint8_t *pcm, size_t pcm_len);
bool net_ws_send_audio_end(const char *session_id, size_t total_bytes, bool discard);
bool net_ws_send_display(const char *status, const char *source);

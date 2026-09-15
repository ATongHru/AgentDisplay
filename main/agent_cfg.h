#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AGENT_CFG_SSID_MAX 32
#define AGENT_CFG_PASS_MAX 64
#define AGENT_CFG_WS_URL_MAX 128
#define AGENT_CFG_WS_TOKEN_MAX 96
#define AGENT_CFG_WS_URI_MAX 256
#define AGENT_CFG_HOST_MAX 64
#define AGENT_CFG_IP_MAX 16
#define AGENT_CFG_MAX_PROFILES 5

typedef struct {
    char ssid[AGENT_CFG_SSID_MAX];
    char pass[AGENT_CFG_PASS_MAX];
    char ws_url[AGENT_CFG_WS_URL_MAX];
    char ws_token[AGENT_CFG_WS_TOKEN_MAX];
    char ip[AGENT_CFG_IP_MAX];
    char gateway[AGENT_CFG_IP_MAX];
    char netmask[AGENT_CFG_IP_MAX];
} agent_cfg_t;

esp_err_t agent_cfg_load(void);
esp_err_t agent_cfg_save(void);
const agent_cfg_t *agent_cfg_get(void);

esp_err_t agent_cfg_set_wifi(const char *ssid, const char *pass);
esp_err_t agent_cfg_set_host(const char *host_port);
esp_err_t agent_cfg_set_token(const char *token);
esp_err_t agent_cfg_set_ip(const char *ip);
esp_err_t agent_cfg_set_gateway(const char *gateway);
esp_err_t agent_cfg_set_netmask(const char *netmask);
bool agent_cfg_has_static_ip(void);
void agent_cfg_format_host(char *out, size_t out_len);

uint8_t agent_cfg_profile_count(void);
uint8_t agent_cfg_active_index(void);
/** Returns profile pointer or NULL if idx out of range. */
const agent_cfg_t *agent_cfg_get_profile(uint8_t idx);
/** Rotate to next saved profile (round-robin). Returns true if switched. */
bool agent_cfg_next_profile(void);
/** Replace slot idx, or idx<0 / idx>=count to append. Writes NVS. */
esp_err_t agent_cfg_upsert_at(int idx, const agent_cfg_t *cfg, uint8_t *out_idx);
esp_err_t agent_cfg_delete_at(uint8_t idx);
esp_err_t agent_cfg_activate(uint8_t idx);

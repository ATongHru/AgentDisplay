#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define AGENT_CFG_SSID_MAX 32
#define AGENT_CFG_PASS_MAX 64
#define AGENT_CFG_WS_URL_MAX 128
#define AGENT_CFG_HOST_MAX 64
#define AGENT_CFG_IP_MAX 16

typedef struct {
    char ssid[AGENT_CFG_SSID_MAX];
    char pass[AGENT_CFG_PASS_MAX];
    char ws_url[AGENT_CFG_WS_URL_MAX];
    char ip[AGENT_CFG_IP_MAX];
    char gateway[AGENT_CFG_IP_MAX];
    char netmask[AGENT_CFG_IP_MAX];
} agent_cfg_t;

esp_err_t agent_cfg_load(void);
esp_err_t agent_cfg_save(void);
const agent_cfg_t *agent_cfg_get(void);

esp_err_t agent_cfg_set_wifi(const char *ssid, const char *pass);
esp_err_t agent_cfg_set_host(const char *host_port);
esp_err_t agent_cfg_set_ip(const char *ip);
esp_err_t agent_cfg_set_gateway(const char *gateway);
esp_err_t agent_cfg_set_netmask(const char *netmask);
bool agent_cfg_has_static_ip(void);
void agent_cfg_format_host(char *out, size_t out_len);

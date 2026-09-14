#include "prov_cfg.h"

#include <stdio.h>
#include <string.h>

#include "agent_cfg.h"
#include "ws_url.h"
#include "ap_prov.h"
#include "esp_log.h"
#include "net_ws.h"

static const char *TAG = "prov_cfg";

static bool json_copy_str(const cJSON *obj, const char *key, char *out, size_t out_len)
{
    out[0] = 0;
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring) {
        return false;
    }
    strncpy(out, v->valuestring, out_len - 1);
    out[out_len - 1] = 0;
    return true;
}

void prov_cfg_fill_json(cJSON *root)
{
    if (!root) {
        return;
    }
    const agent_cfg_t *cfg = agent_cfg_get();
    char host[AGENT_CFG_HOST_MAX];
    int port = 8000;
    ws_url_split_host_port(cfg->ws_url, host, sizeof(host), &port);
    cJSON_AddStringToObject(root, "ssid", cfg->ssid);
    cJSON_AddStringToObject(root, "password", cfg->pass);
    cJSON_AddStringToObject(root, "host", host);
    cJSON_AddNumberToObject(root, "port", port);
    cJSON_AddStringToObject(root, "ip", cfg->ip);
    cJSON_AddStringToObject(root, "netmask", cfg->netmask);
    cJSON_AddStringToObject(root, "gateway", cfg->gateway);
    cJSON_AddNumberToObject(root, "profile_count", agent_cfg_profile_count());
    cJSON_AddNumberToObject(root, "active", agent_cfg_active_index());
}

esp_err_t prov_cfg_load_from_json(const cJSON *root)
{
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    char ssid[AGENT_CFG_SSID_MAX];
    char pass[AGENT_CFG_PASS_MAX];
    char host[AGENT_CFG_HOST_MAX];
    if (!json_copy_str(root, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    pass[0] = 0;
    json_copy_str(root, "password", pass, sizeof(pass));
    if (!json_copy_str(root, "host", host, sizeof(host)) || !host[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    int port = 8000;
    const cJSON *pj = cJSON_GetObjectItem(root, "port");
    if (cJSON_IsNumber(pj)) {
        port = (int)pj->valuedouble;
    }
    char *colon = strrchr(host, ':');
    if (colon && colon != host && colon[1] >= '0' && colon[1] <= '9') {
        *colon = 0;
        int parsed = atoi(colon + 1);
        if (parsed > 0 && parsed <= 65535) {
            port = parsed;
        }
    }
    if (port < 1 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    char host_port[96];
    int n = snprintf(host_port, sizeof(host_port), "%s:%d", host, port);
    if (n <= 0 || n >= (int)sizeof(host_port)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (agent_cfg_set_wifi(ssid, pass) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (agent_cfg_set_host(host_port) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    char ip[AGENT_CFG_IP_MAX];
    char mask[AGENT_CFG_IP_MAX];
    char gw[AGENT_CFG_IP_MAX];
    if (json_copy_str(root, "ip", ip, sizeof(ip)) && ip[0]) {
        if (agent_cfg_set_ip(ip) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        agent_cfg_set_ip("");
    }
    if (json_copy_str(root, "netmask", mask, sizeof(mask)) && mask[0]) {
        if (agent_cfg_set_netmask(mask) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        agent_cfg_set_netmask("");
    }
    if (json_copy_str(root, "gateway", gw, sizeof(gw)) && gw[0]) {
        if (agent_cfg_set_gateway(gw) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        agent_cfg_set_gateway("");
    }
    return ESP_OK;
}

esp_err_t prov_cfg_apply_from_json(const cJSON *root)
{
    esp_err_t err = prov_cfg_load_from_json(root);
    if (err != ESP_OK) {
        return err;
    }
    return prov_cfg_apply_saved();
}

esp_err_t prov_cfg_apply_saved(void)
{
    const agent_cfg_t *cfg = agent_cfg_get();
    if (!cfg->ssid[0] || !cfg->ws_url[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = agent_cfg_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed %s", esp_err_to_name(err));
        return err;
    }
    if (ap_prov_active()) {
        ap_prov_stop_for_apply();
    }
    err = net_apply_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply failed %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "applied ssid=%s ws=%s", cfg->ssid, cfg->ws_url);
    return ESP_OK;
}

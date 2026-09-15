#include "agent_cfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "agent_cfg";
static const char *NVS_NS = "agent_cfg";

static agent_cfg_t s_profiles[AGENT_CFG_MAX_PROFILES];
static uint8_t s_count;
static uint8_t s_active;
static agent_cfg_t s_cfg;

static bool ipv4_ok(const char *s)
{
    if (!s || !s[0]) {
        return true;
    }
    unsigned a, b, c, d;
    char tail = 0;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

static bool cfg_equal(const agent_cfg_t *x, const agent_cfg_t *y)
{
    return strcmp(x->ssid, y->ssid) == 0 && strcmp(x->pass, y->pass) == 0 &&
           strcmp(x->ws_url, y->ws_url) == 0 && strcmp(x->ws_token, y->ws_token) == 0 &&
           strcmp(x->ip, y->ip) == 0 && strcmp(x->gateway, y->gateway) == 0 &&
           strcmp(x->netmask, y->netmask) == 0;
}

static void strip_url_query_token(char *url, char *token_out, size_t token_len)
{
    if (!url || !token_out || token_len == 0) {
        return;
    }
    char *q = strchr(url, '?');
    if (!q) {
        return;
    }
    *q = 0;
    if (strncmp(q + 1, "token=", 6) == 0) {
        strncpy(token_out, q + 7, token_len - 1);
        token_out[token_len - 1] = 0;
    }
}

static void sync_active_to_cfg(void)
{
    if (s_count == 0) {
        return;
    }
    if (s_active >= s_count) {
        s_active = 0;
    }
    s_cfg = s_profiles[s_active];
}
static void load_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    strncpy(s_cfg.ssid, CONFIG_AGENT_WIFI_SSID, sizeof(s_cfg.ssid) - 1);
    strncpy(s_cfg.pass, CONFIG_AGENT_WIFI_PASSWORD, sizeof(s_cfg.pass) - 1);
    strncpy(s_cfg.ws_url, CONFIG_AGENT_WS_URL, sizeof(s_cfg.ws_url) - 1);
    strncpy(s_cfg.ws_token, CONFIG_AGENT_WS_TOKEN, sizeof(s_cfg.ws_token) - 1);
    strip_url_query_token(s_cfg.ws_url, s_cfg.ws_token, sizeof(s_cfg.ws_token));
#if CONFIG_AGENT_WIFI_STATIC_IP
    strncpy(s_cfg.ip, CONFIG_AGENT_WIFI_IP, sizeof(s_cfg.ip) - 1);
    strncpy(s_cfg.gateway, CONFIG_AGENT_WIFI_GATEWAY, sizeof(s_cfg.gateway) - 1);
    strncpy(s_cfg.netmask, CONFIG_AGENT_WIFI_NETMASK, sizeof(s_cfg.netmask) - 1);
#endif
    memset(s_profiles, 0, sizeof(s_profiles));
    s_profiles[0] = s_cfg;
    s_count = 1;
    s_active = 0;
}

static esp_err_t load_one_profile(nvs_handle_t h, uint8_t idx, agent_cfg_t *out)
{
    char key[16];
    size_t len;
    memset(out, 0, sizeof(*out));
    snprintf(key, sizeof(key), "ssid%u", idx);
    len = sizeof(out->ssid);
    if (nvs_get_str(h, key, out->ssid, &len) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(key, sizeof(key), "pass%u", idx);
    len = sizeof(out->pass);
    if (nvs_get_str(h, key, out->pass, &len) != ESP_OK) {
        out->pass[0] = 0;
    }
    snprintf(key, sizeof(key), "ws%u", idx);
    len = sizeof(out->ws_url);
    if (nvs_get_str(h, key, out->ws_url, &len) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(key, sizeof(key), "tok%u", idx);
    len = sizeof(out->ws_token);
    if (nvs_get_str(h, key, out->ws_token, &len) != ESP_OK) {
        out->ws_token[0] = 0;
    }
    snprintf(key, sizeof(key), "ip%u", idx);
    len = sizeof(out->ip);
    if (nvs_get_str(h, key, out->ip, &len) != ESP_OK) {
        out->ip[0] = 0;
    }
    snprintf(key, sizeof(key), "gw%u", idx);
    len = sizeof(out->gateway);
    if (nvs_get_str(h, key, out->gateway, &len) != ESP_OK) {
        out->gateway[0] = 0;
    }
    snprintf(key, sizeof(key), "mask%u", idx);
    len = sizeof(out->netmask);
    if (nvs_get_str(h, key, out->netmask, &len) != ESP_OK) {
        out->netmask[0] = 0;
    }
    return ESP_OK;
}

static esp_err_t save_one_profile(nvs_handle_t h, uint8_t idx, const agent_cfg_t *cfg)
{
    char key[16];
    esp_err_t err;
    snprintf(key, sizeof(key), "ssid%u", idx);
    err = nvs_set_str(h, key, cfg->ssid);
    if (err != ESP_OK) return err;
    snprintf(key, sizeof(key), "pass%u", idx);
    err = nvs_set_str(h, key, cfg->pass);
    if (err != ESP_OK) return err;
    snprintf(key, sizeof(key), "ws%u", idx);
    err = nvs_set_str(h, key, cfg->ws_url);
    if (err != ESP_OK) return err;
    snprintf(key, sizeof(key), "tok%u", idx);
    err = nvs_set_str(h, key, cfg->ws_token);
    if (err != ESP_OK) return err;
    snprintf(key, sizeof(key), "ip%u", idx);
    err = nvs_set_str(h, key, cfg->ip);
    if (err != ESP_OK) return err;
    snprintf(key, sizeof(key), "gw%u", idx);
    err = nvs_set_str(h, key, cfg->gateway);
    if (err != ESP_OK) return err;
    snprintf(key, sizeof(key), "mask%u", idx);
    return nvs_set_str(h, key, cfg->netmask);
}
static void migrate_legacy(nvs_handle_t h)
{
    agent_cfg_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    size_t len = sizeof(legacy.ssid);
    bool any = false;
    if (nvs_get_str(h, "ssid", legacy.ssid, &len) == ESP_OK) any = true;
    len = sizeof(legacy.pass);
    nvs_get_str(h, "pass", legacy.pass, &len);
    len = sizeof(legacy.ws_url);
    if (nvs_get_str(h, "ws_url", legacy.ws_url, &len) == ESP_OK) any = true;
    len = sizeof(legacy.ip);
    nvs_get_str(h, "ip", legacy.ip, &len);
    len = sizeof(legacy.gateway);
    nvs_get_str(h, "gw", legacy.gateway, &len);
    len = sizeof(legacy.netmask);
    nvs_get_str(h, "mask", legacy.netmask, &len);
    if (!any || !legacy.ssid[0] || !legacy.ws_url[0]) return;
    s_profiles[0] = legacy;
    s_count = 1;
    s_active = 0;
    s_cfg = legacy;
    ESP_LOGI(TAG, "migrated legacy single profile ssid=%s", legacy.ssid);
}

esp_err_t agent_cfg_load(void)
{
    load_defaults();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no nvs cfg, using Kconfig defaults ssid=%s", s_cfg.ssid);
        return ESP_OK;
    }
    uint8_t count = 0;
    uint8_t active = 0;
    if (nvs_get_u8(h, "pcnt", &count) == ESP_OK && count > 0 && count <= AGENT_CFG_MAX_PROFILES) {
        uint8_t loaded = 0;
        for (uint8_t i = 0; i < count; ++i) {
            if (load_one_profile(h, i, &s_profiles[loaded]) == ESP_OK) loaded++;
        }
        if (loaded > 0) {
            s_count = loaded;
            if (nvs_get_u8(h, "pact", &active) == ESP_OK && active < s_count) s_active = active;
            else s_active = 0;
            sync_active_to_cfg();
        } else {
            migrate_legacy(h);
        }
    } else {
        migrate_legacy(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %u profiles active=%u ssid=%s ws=%s ip=%s", (unsigned)s_count,
             (unsigned)s_active, s_cfg.ssid, s_cfg.ws_url, s_cfg.ip[0] ? s_cfg.ip : "-");
    return ESP_OK;
}

static esp_err_t persist_profiles(void)
{
    if (s_count == 0) {
        load_defaults();
    }
    if (s_active >= s_count) {
        s_active = 0;
    }
    s_cfg = s_profiles[s_active];

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, "pcnt", s_count);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "pact", s_active);
    }
    for (uint8_t i = 0; err == ESP_OK && i < s_count; ++i) {
        err = save_one_profile(h, i, &s_profiles[i]);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ssid", s_cfg.ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", s_cfg.pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ws_url", s_cfg.ws_url);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ip", s_cfg.ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "gw", s_cfg.gateway);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "mask", s_cfg.netmask);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "saved profiles=%u active=%u ssid=%s ws=%s ip=%s", (unsigned)s_count,
             (unsigned)s_active, s_cfg.ssid, s_cfg.ws_url, s_cfg.ip[0] ? s_cfg.ip : "dhcp");
    return ESP_OK;
}

esp_err_t agent_cfg_save(void)
{
    int dup = -1;
    for (uint8_t i = 0; i < s_count; ++i) {
        if (cfg_equal(&s_profiles[i], &s_cfg)) {
            dup = (int)i;
            break;
        }
    }
    if (dup >= 0) {
        agent_cfg_t tmp = s_profiles[dup];
        for (uint8_t j = (uint8_t)dup; j + 1 < s_count; ++j) {
            s_profiles[j] = s_profiles[j + 1];
        }
        s_profiles[s_count - 1] = tmp;
        s_active = (uint8_t)(s_count - 1);
    } else if (s_count < AGENT_CFG_MAX_PROFILES) {
        s_profiles[s_count] = s_cfg;
        s_active = s_count;
        s_count++;
    } else {
        memmove(&s_profiles[0], &s_profiles[1], sizeof(agent_cfg_t) * (AGENT_CFG_MAX_PROFILES - 1));
        s_profiles[AGENT_CFG_MAX_PROFILES - 1] = s_cfg;
        s_count = AGENT_CFG_MAX_PROFILES;
        s_active = (uint8_t)(AGENT_CFG_MAX_PROFILES - 1);
    }
    return persist_profiles();
}

esp_err_t agent_cfg_upsert_at(int idx, const agent_cfg_t *cfg, uint8_t *out_idx)
{
    if (!cfg || !cfg->ssid[0] || !cfg->ws_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t slot;
    if (idx < 0 || idx >= (int)s_count) {
        if (s_count >= AGENT_CFG_MAX_PROFILES) {
            return ESP_ERR_NO_MEM;
        }
        slot = s_count;
        s_profiles[slot] = *cfg;
        s_count++;
        if (s_count == 1) {
            s_active = 0;
        }
    } else {
        slot = (uint8_t)idx;
        s_profiles[slot] = *cfg;
    }
    if (out_idx) {
        *out_idx = slot;
    }
    return persist_profiles();
}

esp_err_t agent_cfg_delete_at(uint8_t idx)
{
    if (s_count <= 1) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (idx >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = idx; i + 1 < s_count; ++i) {
        s_profiles[i] = s_profiles[i + 1];
    }
    s_count--;
    memset(&s_profiles[s_count], 0, sizeof(s_profiles[s_count]));
    if (s_active > idx) {
        s_active--;
    } else if (s_active >= s_count) {
        s_active = (uint8_t)(s_count - 1);
    }
    return persist_profiles();
}

esp_err_t agent_cfg_activate(uint8_t idx)
{
    if (idx >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    s_active = idx;
    return persist_profiles();
}

const agent_cfg_t *agent_cfg_get(void) { return &s_cfg; }
const agent_cfg_t *agent_cfg_get_profile(uint8_t idx)
{
    if (s_count == 0 || idx >= s_count) {
        return NULL;
    }
    return &s_profiles[idx];
}
uint8_t agent_cfg_profile_count(void) { return s_count ? s_count : 1; }
uint8_t agent_cfg_active_index(void) { return s_active; }

bool agent_cfg_next_profile(void)
{
    if (s_count <= 1) return false;
    s_active = (uint8_t)((s_active + 1) % s_count);
    sync_active_to_cfg();
    ESP_LOGI(TAG, "switch profile %u/%u ssid=%s ws=%s ip=%s", (unsigned)s_active,
             (unsigned)s_count, s_cfg.ssid, s_cfg.ws_url, s_cfg.ip[0] ? s_cfg.ip : "dhcp");
    return true;
}

bool agent_cfg_has_static_ip(void) { return s_cfg.ip[0] != 0; }

esp_err_t agent_cfg_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= AGENT_CFG_SSID_MAX) return ESP_ERR_INVALID_ARG;
    if (!pass || strlen(pass) >= AGENT_CFG_PASS_MAX) return ESP_ERR_INVALID_ARG;
    memset(s_cfg.ssid, 0, sizeof(s_cfg.ssid));
    memset(s_cfg.pass, 0, sizeof(s_cfg.pass));
    strncpy(s_cfg.ssid, ssid, sizeof(s_cfg.ssid) - 1);
    strncpy(s_cfg.pass, pass, sizeof(s_cfg.pass) - 1);
    return ESP_OK;
}

esp_err_t agent_cfg_set_host(const char *host_port)
{
    if (!host_port || !host_port[0]) return ESP_ERR_INVALID_ARG;
    if (strncmp(host_port, "ws://", 5) == 0 || strncmp(host_port, "wss://", 6) == 0) {
        if (strlen(host_port) >= AGENT_CFG_WS_URL_MAX) return ESP_ERR_INVALID_ARG;
        memset(s_cfg.ws_url, 0, sizeof(s_cfg.ws_url));
        strncpy(s_cfg.ws_url, host_port, sizeof(s_cfg.ws_url) - 1);
        strip_url_query_token(s_cfg.ws_url, s_cfg.ws_token, sizeof(s_cfg.ws_token));
        return ESP_OK;
    }
    if (strlen(host_port) >= AGENT_CFG_HOST_MAX) return ESP_ERR_INVALID_ARG;
    char url[AGENT_CFG_WS_URL_MAX];
    int n = snprintf(url, sizeof(url), "ws://%s/ws", host_port);
    if (n <= 0 || n >= (int)sizeof(url)) return ESP_ERR_INVALID_ARG;
    memset(s_cfg.ws_url, 0, sizeof(s_cfg.ws_url));
    strncpy(s_cfg.ws_url, url, sizeof(s_cfg.ws_url) - 1);
    return ESP_OK;
}

esp_err_t agent_cfg_set_token(const char *token)
{
    memset(s_cfg.ws_token, 0, sizeof(s_cfg.ws_token));
    if (token && token[0]) {
        if (strlen(token) >= AGENT_CFG_WS_TOKEN_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(s_cfg.ws_token, token, sizeof(s_cfg.ws_token) - 1);
    }
    return ESP_OK;
}

esp_err_t agent_cfg_set_ip(const char *ip)
{
    if (!ipv4_ok(ip)) return ESP_ERR_INVALID_ARG;
    memset(s_cfg.ip, 0, sizeof(s_cfg.ip));
    if (ip && ip[0]) strncpy(s_cfg.ip, ip, sizeof(s_cfg.ip) - 1);
    return ESP_OK;
}

esp_err_t agent_cfg_set_gateway(const char *gateway)
{
    if (!ipv4_ok(gateway)) return ESP_ERR_INVALID_ARG;
    memset(s_cfg.gateway, 0, sizeof(s_cfg.gateway));
    if (gateway && gateway[0]) strncpy(s_cfg.gateway, gateway, sizeof(s_cfg.gateway) - 1);
    return ESP_OK;
}

esp_err_t agent_cfg_set_netmask(const char *netmask)
{
    if (!ipv4_ok(netmask)) return ESP_ERR_INVALID_ARG;
    memset(s_cfg.netmask, 0, sizeof(s_cfg.netmask));
    if (netmask && netmask[0]) strncpy(s_cfg.netmask, netmask, sizeof(s_cfg.netmask) - 1);
    return ESP_OK;
}

void agent_cfg_format_host(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = 0;
    const char *u = s_cfg.ws_url;
    if (strncmp(u, "ws://", 5) == 0) u += 5;
    else if (strncmp(u, "wss://", 6) == 0) u += 6;
    size_t n = strlen(u);
    if (n >= 3 && strcmp(u + n - 3, "/ws") == 0) n -= 3;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, u, n);
    out[n] = 0;
}

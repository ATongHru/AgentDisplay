#include "agent_cfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "agent_cfg";
static const char *NVS_NS = "agent_cfg";

static agent_cfg_t s_cfg;

static bool ipv4_ok(const char *s)
{
    if (!s || !s[0]) {
        return true; /* empty allowed = clear / DHCP */
    }
    unsigned a, b, c, d;
    char tail = 0;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

static void load_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    strncpy(s_cfg.ssid, CONFIG_AGENT_WIFI_SSID, sizeof(s_cfg.ssid) - 1);
    strncpy(s_cfg.pass, CONFIG_AGENT_WIFI_PASSWORD, sizeof(s_cfg.pass) - 1);
    strncpy(s_cfg.ws_url, CONFIG_AGENT_WS_URL, sizeof(s_cfg.ws_url) - 1);
#if CONFIG_AGENT_WIFI_STATIC_IP
    strncpy(s_cfg.ip, CONFIG_AGENT_WIFI_IP, sizeof(s_cfg.ip) - 1);
    strncpy(s_cfg.gateway, CONFIG_AGENT_WIFI_GATEWAY, sizeof(s_cfg.gateway) - 1);
    strncpy(s_cfg.netmask, CONFIG_AGENT_WIFI_NETMASK, sizeof(s_cfg.netmask) - 1);
#endif
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

    size_t len = sizeof(s_cfg.ssid);
    if (nvs_get_str(h, "ssid", s_cfg.ssid, &len) != ESP_OK) {
        strncpy(s_cfg.ssid, CONFIG_AGENT_WIFI_SSID, sizeof(s_cfg.ssid) - 1);
    }
    len = sizeof(s_cfg.pass);
    if (nvs_get_str(h, "pass", s_cfg.pass, &len) != ESP_OK) {
        strncpy(s_cfg.pass, CONFIG_AGENT_WIFI_PASSWORD, sizeof(s_cfg.pass) - 1);
    }
    len = sizeof(s_cfg.ws_url);
    if (nvs_get_str(h, "ws_url", s_cfg.ws_url, &len) != ESP_OK) {
        strncpy(s_cfg.ws_url, CONFIG_AGENT_WS_URL, sizeof(s_cfg.ws_url) - 1);
    }
    len = sizeof(s_cfg.ip);
    if (nvs_get_str(h, "ip", s_cfg.ip, &len) != ESP_OK) {
#if CONFIG_AGENT_WIFI_STATIC_IP
        strncpy(s_cfg.ip, CONFIG_AGENT_WIFI_IP, sizeof(s_cfg.ip) - 1);
#else
        s_cfg.ip[0] = '\0';
#endif
    }
    len = sizeof(s_cfg.gateway);
    if (nvs_get_str(h, "gw", s_cfg.gateway, &len) != ESP_OK) {
#if CONFIG_AGENT_WIFI_STATIC_IP
        strncpy(s_cfg.gateway, CONFIG_AGENT_WIFI_GATEWAY, sizeof(s_cfg.gateway) - 1);
#else
        s_cfg.gateway[0] = '\0';
#endif
    }
    len = sizeof(s_cfg.netmask);
    if (nvs_get_str(h, "mask", s_cfg.netmask, &len) != ESP_OK) {
#if CONFIG_AGENT_WIFI_STATIC_IP
        strncpy(s_cfg.netmask, CONFIG_AGENT_WIFI_NETMASK, sizeof(s_cfg.netmask) - 1);
#else
        s_cfg.netmask[0] = '\0';
#endif
    }
    nvs_close(h);

    ESP_LOGI(TAG, "loaded ssid=%s ws=%s ip=%s gw=%s mask=%s", s_cfg.ssid, s_cfg.ws_url,
             s_cfg.ip[0] ? s_cfg.ip : "-", s_cfg.gateway[0] ? s_cfg.gateway : "-",
             s_cfg.netmask[0] ? s_cfg.netmask : "-");
    return ESP_OK;
}

esp_err_t agent_cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, "ssid", s_cfg.ssid);
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
    ESP_LOGI(TAG, "saved ssid=%s ws=%s ip=%s", s_cfg.ssid, s_cfg.ws_url,
             s_cfg.ip[0] ? s_cfg.ip : "dhcp");
    return ESP_OK;
}

const agent_cfg_t *agent_cfg_get(void)
{
    return &s_cfg;
}

bool agent_cfg_has_static_ip(void)
{
    return s_cfg.ip[0] != '\0';
}

esp_err_t agent_cfg_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= AGENT_CFG_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pass || strlen(pass) >= AGENT_CFG_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_cfg.ssid, 0, sizeof(s_cfg.ssid));
    memset(s_cfg.pass, 0, sizeof(s_cfg.pass));
    strncpy(s_cfg.ssid, ssid, sizeof(s_cfg.ssid) - 1);
    strncpy(s_cfg.pass, pass, sizeof(s_cfg.pass) - 1);
    return ESP_OK;
}

esp_err_t agent_cfg_set_host(const char *host_port)
{
    if (!host_port || !host_port[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(host_port, "ws://", 5) == 0 || strncmp(host_port, "wss://", 6) == 0) {
        if (strlen(host_port) >= AGENT_CFG_WS_URL_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        memset(s_cfg.ws_url, 0, sizeof(s_cfg.ws_url));
        strncpy(s_cfg.ws_url, host_port, sizeof(s_cfg.ws_url) - 1);
        return ESP_OK;
    }
    if (strlen(host_port) >= AGENT_CFG_HOST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[AGENT_CFG_WS_URL_MAX];
    int n = snprintf(url, sizeof(url), "ws://%s/ws", host_port);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_cfg.ws_url, 0, sizeof(s_cfg.ws_url));
    strncpy(s_cfg.ws_url, url, sizeof(s_cfg.ws_url) - 1);
    return ESP_OK;
}

esp_err_t agent_cfg_set_ip(const char *ip)
{
    if (!ipv4_ok(ip)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_cfg.ip, 0, sizeof(s_cfg.ip));
    if (ip && ip[0]) {
        strncpy(s_cfg.ip, ip, sizeof(s_cfg.ip) - 1);
    }
    return ESP_OK;
}

esp_err_t agent_cfg_set_gateway(const char *gateway)
{
    if (!ipv4_ok(gateway)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_cfg.gateway, 0, sizeof(s_cfg.gateway));
    if (gateway && gateway[0]) {
        strncpy(s_cfg.gateway, gateway, sizeof(s_cfg.gateway) - 1);
    }
    return ESP_OK;
}

esp_err_t agent_cfg_set_netmask(const char *netmask)
{
    if (!ipv4_ok(netmask)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_cfg.netmask, 0, sizeof(s_cfg.netmask));
    if (netmask && netmask[0]) {
        strncpy(s_cfg.netmask, netmask, sizeof(s_cfg.netmask) - 1);
    }
    return ESP_OK;
}

void agent_cfg_format_host(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const char *u = s_cfg.ws_url;
    if (strncmp(u, "ws://", 5) == 0) {
        u += 5;
    } else if (strncmp(u, "wss://", 6) == 0) {
        u += 6;
    }
    size_t n = strlen(u);
    if (n >= 3 && strcmp(u + n - 3, "/ws") == 0) {
        n -= 3;
    }
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, u, n);
    out[n] = '\0';
}

#include "ap_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_cfg.h"
#include "ble_prov.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_ws.h"
#include "prov_cfg.h"
#include "ui_msg.h"

static const char *TAG = "ap_prov";

#define AP_CHANNEL 1
#define AP_MAX_CONN 4
#define PROV_TIMEOUT_MS (10 * 60 * 1000)

static httpd_handle_t s_httpd;
static esp_netif_t *s_ap_netif;
static bool s_active;
static int64_t s_deadline_ms;
static char s_ap_ssid[32];
static bool s_apply_scheduled;

#define AP_APPLY_DELAY_MS 3500

static const char *PORTAL_HTML =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>AgentDisplay 配网</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#0b1018;color:#edf3fa}"
    "h1{font-size:20px;margin:0 0 8px}p{color:#8fa0b3;font-size:13px;margin:0 0 16px}"
    "label{display:block;margin:10px 0 4px;font-size:13px;color:#b8c5d6}"
    "input{width:100%;box-sizing:border-box;padding:10px;border:1px solid #2a3646;border-radius:8px;"
    "background:#121820;color:#edf3fa;font-size:15px}"
    "button{margin-top:16px;width:100%;padding:12px;border:0;border-radius:8px;background:#7c96ff;"
    "color:#fff;font-size:15px;font-weight:600}"
    "#msg{margin-top:12px;font-size:13px;min-height:18px}"
    ".ok{color:#3dcc86}.bad{color:#ff6b74}"
    ".modal{position:fixed;inset:0;background:rgba(0,0,0,.7);display:none;"
    "align-items:center;justify-content:center;padding:20px;z-index:9}"
    ".modal.show{display:flex}"
    ".mbox{background:#121820;border:1px solid #2a3646;border-radius:12px;padding:20px;max-width:320px}"
    ".mbox h2{margin:0 0 8px;font-size:18px}.mbox p{margin:0 0 16px;color:#b8c5d6;font-size:14px;line-height:1.5}"
    ".mbox button{width:100%;padding:12px;border:0;border-radius:8px;background:#3dcc86;"
    "color:#0b1018;font-size:15px;font-weight:600}"
    "</style></head><body>"
    "<h1>AgentDisplay 配网</h1>"
    "<p>填写 WiFi 与后端地址，保存后设备将自动连接。</p>"
    "<form id=\"f\">"
    "<label>WiFi 名称<input name=\"ssid\" required maxlength=\"32\" autocomplete=\"off\"></label>"
    "<label>WiFi 密码<input name=\"password\" type=\"password\" maxlength=\"64\" autocomplete=\"off\"></label>"
    "<label>后端 IP<input name=\"host\" required maxlength=\"64\" autocomplete=\"off\"></label>"
    "<label>端口<input name=\"port\" type=\"number\" min=\"1\" max=\"65535\" value=\"8000\"></label>"
    "<label>设备静态 IP（可选，留空为 DHCP）<input name=\"ip\" maxlength=\"15\"></label>"
    "<label>子网掩码<input name=\"netmask\" maxlength=\"15\" placeholder=\"255.255.255.0\"></label>"
    "<label>网关<input name=\"gateway\" maxlength=\"15\"></label>"
    "<button type=\"submit\">保存并连接</button></form><div id=\"msg\"></div>"
    "<div id=\"modal\" class=\"modal\"><div class=\"mbox\">"
    "<h2>配置保存成功</h2>"
    "<p>设备将在几秒内断开热点并连接您填写的 WiFi。请切换回原网络，等待设备上线。</p>"
    "<button id=\"okBtn\" type=\"button\">知道了</button></div></div>"
    "<script>"
    "const f=document.getElementById('f'),msg=document.getElementById('msg'),modal=document.getElementById('modal');"
    "fetch('/api/config').then(r=>r.json()).then(d=>{"
    "if(d.ssid)f.ssid.value=d.ssid;if(d.password)f.password.value=d.password;"
    "if(d.host)f.host.value=d.host;if(d.port)f.port.value=d.port;"
    "if(d.ip)f.ip.value=d.ip;if(d.netmask)f.netmask.value=d.netmask;"
    "if(d.gateway)f.gateway.value=d.gateway;}).catch(()=>{});"
    "f.onsubmit=async e=>{e.preventDefault();msg.textContent='正在保存…';msg.className='';"
    "const body={ssid:f.ssid.value.trim(),password:f.password.value,"
    "host:f.host.value.trim(),port:parseInt(f.port.value||'8000',10),"
    "ip:f.ip.value.trim(),netmask:f.netmask.value.trim(),gateway:f.gateway.value.trim()};"
    "try{const ctrl=new AbortController();const t=setTimeout(()=>ctrl.abort(),15000);"
    "const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify(body),signal:ctrl.signal});clearTimeout(t);"
    "const d=await r.json();"
    "if(d.ok){modal.classList.add('show');msg.textContent='已保存，设备正在连接…';msg.className='ok';"
    "f.querySelector('button[type=submit]').disabled=true;}"
    "else{msg.textContent=d.error||'保存失败';msg.className='bad';}}"
    "catch(err){msg.textContent='请求失败，若已保存请切换回原 WiFi 等待设备上线';msg.className='bad';}};"
    "document.getElementById('okBtn').onclick=()=>modal.classList.remove('show');"
    "</script></body></html>";

static void build_ap_ssid(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AgentDisplay-%02X%02X", mac[4], mac[5]);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, "{\"ok\":false,\"error\":\"no mem\"}");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    prov_cfg_fill_json(root);
    char *dump = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!dump) {
        return send_json(req, "{\"ok\":false,\"error\":\"no mem\"}");
    }
    esp_err_t err = send_json(req, dump);
    free(dump);
    return err;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) {
        return send_json(req, "{\"ok\":false,\"error\":\"bad body\"}");
    }
    char *buf = malloc((size_t)req->content_len + 1);
    if (!buf) {
        return send_json(req, "{\"ok\":false,\"error\":\"no mem\"}");
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            return send_json(req, "{\"ok\":false,\"error\":\"recv\"}");
        }
        received += r;
    }
    buf[received] = 0;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return send_json(req, "{\"ok\":false,\"error\":\"json\"}");
    }
    esp_err_t err = prov_cfg_load_from_json(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        const char *detail = "参数无效";
        if (err == ESP_ERR_NO_MEM) {
            detail = "配置已满";
        }
        char reply[96];
        snprintf(reply, sizeof(reply), "{\"ok\":false,\"error\":\"%s\"}", detail);
        return send_json(req, reply);
    }
    esp_err_t send_err = send_json(req, "{\"ok\":true}");
    if (send_err == ESP_OK) {
        ap_prov_schedule_apply();
    }
    return send_err;
}

static void apply_worker(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(AP_APPLY_DELAY_MS));
    esp_err_t err = prov_cfg_apply_saved();
    ESP_LOGI(TAG, "deferred apply %s", esp_err_to_name(err));
    s_apply_scheduled = false;
    vTaskDelete(NULL);
}

void ap_prov_schedule_apply(void)
{
    if (s_apply_scheduled) {
        return;
    }
    s_apply_scheduled = true;
    BaseType_t ok = xTaskCreate(apply_worker, "ap_apply", 6144, NULL, 5, NULL);
    if (ok != pdPASS) {
        s_apply_scheduled = false;
        ESP_LOGE(TAG, "apply task create failed");
    }
}

static esp_err_t start_httpd(void)
{
    if (s_httpd) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start %s", esp_err_to_name(err));
        return err;
    }
    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = handle_root};
    httpd_uri_t cfg_get = {.uri = "/api/config", .method = HTTP_GET, .handler = handle_get_config};
    httpd_uri_t cfg_post = {.uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config};
    httpd_uri_t gen204 = {.uri = "/generate_204", .method = HTTP_GET, .handler = handle_captive};
    httpd_uri_t hotspot = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_captive};
    httpd_uri_t connecttest = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = handle_captive};
    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &cfg_get);
    httpd_register_uri_handler(s_httpd, &cfg_post);
    httpd_register_uri_handler(s_httpd, &gen204);
    httpd_register_uri_handler(s_httpd, &hotspot);
    httpd_register_uri_handler(s_httpd, &connecttest);
    ESP_LOGI(TAG, "http server on http://192.168.4.1/");
    return ESP_OK;
}

static void stop_httpd(void)
{
    if (!s_httpd) {
        return;
    }
    httpd_stop(s_httpd);
    s_httpd = NULL;
}

static esp_err_t start_ap_radio(void)
{
    build_ap_ssid();
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            return ESP_FAIL;
        }
    }
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "wifi_stop %s", esp_err_to_name(err));
    }
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "AP started ssid=%s (open) url=http://192.168.4.1/", s_ap_ssid);
    return ESP_OK;
}

esp_err_t ap_prov_start(void)
{
    if (s_active) {
        return ESP_OK;
    }
    if (ble_prov_active()) {
        ESP_LOGW(TAG, "skip AP prov: BLE active");
        return ESP_ERR_INVALID_STATE;
    }
    ui_post_ap_prov(true);
    net_pause_sta_for_ap();
    esp_err_t err = start_ap_radio();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start AP failed %s", esp_err_to_name(err));
        ui_post_ap_prov(false);
        net_resume_sta_after_ap();
        return err;
    }
    err = start_httpd();
    if (err != ESP_OK) {
        esp_wifi_stop();
        ui_post_ap_prov(false);
        net_resume_sta_after_ap();
        return err;
    }
    s_active = true;
    s_apply_scheduled = false;
    s_deadline_ms = esp_timer_get_time() / 1000 + PROV_TIMEOUT_MS;
    return ESP_OK;
}

void ap_prov_stop_for_apply(void)
{
    stop_httpd();
    esp_wifi_stop();
    s_active = false;
    s_deadline_ms = 0;
    ui_post_ap_prov(false);
    net_resume_sta_after_ap();
}

void ap_prov_stop(void)
{
    if (!s_active) {
        return;
    }
    stop_httpd();
    esp_wifi_stop();
    s_active = false;
    s_deadline_ms = 0;
    ui_post_ap_prov(false);
    net_resume_sta_after_ap();
    ESP_LOGI(TAG, "AP prov stopped");
}

void ap_prov_loop(void)
{
    if (!s_active) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now >= s_deadline_ms) {
        ESP_LOGW(TAG, "AP prov timeout");
        ap_prov_stop();
    }
}

bool ap_prov_active(void)
{
    return s_active;
}

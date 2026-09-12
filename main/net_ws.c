#include "net_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "audio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "board_pins.h"
#include "sdkconfig.h"
#include "ui.h"
#include "voice.h"

static const char *TAG = "net";

#define WIFI_OK BIT0
#define WS_OK BIT1
#define SESSION_OK BIT2

static EventGroupHandle_t s_events;
static esp_websocket_client_handle_t s_ws;
static bool s_wifi;
static bool s_ws_on;
static int s_rssi = -100;
static bool s_expect_audio;
static bool s_audio_end;
static size_t s_audio_len;
static char s_session[16];
static char s_usb_line[512];
static size_t s_usb_len;

static void apply_time(int64_t epoch)
{
    if (epoch <= 1577836800) {
        return;
    }
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);
    setenv("TZ", "CST-8", 1);
    tzset();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi = false;
        s_ws_on = false;
        xEventGroupClearBits(s_events, WIFI_OK);
        ui_post_link_state(false, false, false, voice_is_listening(), audio_playback_is_active(), s_rssi);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_wifi = true;
        xEventGroupSetBits(s_events, WIFI_OK);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        esp_sntp_stop();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
    }
}

static void send_hello(void)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"type\":\"hello\",\"role\":\"device\",\"rssi\":%d}", s_rssi);
    esp_websocket_client_send_text(s_ws, buf, strlen(buf), pdMS_TO_TICKS(1000));
}

static void handle_text(const char *text, int len)
{
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(t, "ack") == 0) {
        const cJSON *st = cJSON_GetObjectItem(root, "server_time");
        if (cJSON_IsNumber(st)) {
            apply_time((int64_t)st->valuedouble);
        }
    } else if (strcmp(t, "ping") == 0) {
        const char *pong = "{\"type\":\"pong\"}";
        esp_websocket_client_send_text(s_ws, pong, strlen(pong), pdMS_TO_TICKS(500));
    } else if (strcmp(t, "status") == 0 || strcmp(t, "text") == 0) {
        char *dump = cJSON_PrintUnformatted(root);
        if (dump) {
            ui_post_event_json(dump, false);
            free(dump);
        }
        const cJSON *tm = cJSON_GetObjectItem(root, "time");
        if (cJSON_IsString(tm) && tm->valuestring) {
            apply_time(atoll(tm->valuestring));
        } else if (cJSON_IsNumber(tm)) {
            apply_time((int64_t)tm->valuedouble);
        }
    } else if (strcmp(t, "session") == 0) {
        const cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid) && sid->valuestring) {
            strncpy(s_session, sid->valuestring, sizeof(s_session) - 1);
            xEventGroupSetBits(s_events, SESSION_OK);
        }
    } else if (strcmp(t, "audio_chunk") == 0) {
        const cJSON *lenj = cJSON_GetObjectItem(root, "len");
        const cJSON *endj = cJSON_GetObjectItem(root, "end");
        s_audio_len = cJSON_IsNumber(lenj) ? (size_t)lenj->valuedouble : 0;
        s_audio_end = cJSON_IsTrue(endj);
        if (s_audio_len > 0) {
            s_expect_audio = true;
        } else {
            voice_on_audio_chunk(NULL, 0, s_audio_end);
        }
    } else if (strcmp(t, "error") == 0) {
        ESP_LOGW(TAG, "ws error frame");
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        s_ws_on = true;
        xEventGroupSetBits(s_events, WS_OK);
        send_hello();
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
        }
        ui_post_link_state(false, s_wifi, true, voice_is_listening(), audio_playback_is_active(), s_rssi);
        ESP_LOGI(TAG, "websocket connected");
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_ws_on = false;
        xEventGroupClearBits(s_events, WS_OK);
        ui_post_link_state(false, s_wifi, false, voice_is_listening(), audio_playback_is_active(), s_rssi);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        if (evt->op_code == 0x01) {
            handle_text(evt->data_ptr, evt->data_len);
        } else if (evt->op_code == 0x02) {
            if (s_expect_audio) {
                voice_on_audio_chunk((const uint8_t *)evt->data_ptr, evt->data_len, s_audio_end);
                s_expect_audio = false;
            }
        }
    }
}

static void poll_usb(void)
{
    /* Console occupies USB Serial/JTAG; USB JSON backup is unused in this build. */
    (void)s_usb_line;
    (void)s_usb_len;
}

static void start_ws(void)
{
    if (s_ws) {
        return;
    }
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_AGENT_WS_URL,
        .reconnect_timeout_ms = WS_RECONNECT_MS,
        .network_timeout_ms = 10000,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
    ESP_LOGI(TAG, "ws connecting %s", CONFIG_AGENT_WS_URL);
}

esp_err_t net_init(void)
{
    s_events = xEventGroupCreate();
    /* USB Serial/JTAG is the console; skip driver_install to avoid conflict. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
#if CONFIG_AGENT_WIFI_STATIC_IP
    esp_netif_dhcpc_stop(sta);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ipaddr_addr(CONFIG_AGENT_WIFI_IP);
    ip.gw.addr = ipaddr_addr(CONFIG_AGENT_WIFI_GATEWAY);
    ip.netmask.addr = ipaddr_addr(CONFIG_AGENT_WIFI_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(sta, &ip));
    esp_netif_dns_info_t dns = {0};
    dns.ip.u_addr.ip4.addr = ipaddr_addr(CONFIG_AGENT_WIFI_GATEWAY);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
#endif
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_AGENT_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_AGENT_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi start ssid=%s", CONFIG_AGENT_WIFI_SSID);
    return ESP_OK;
}

void net_loop(void)
{
    poll_usb();
    if (s_wifi && !s_ws) {
        start_ws();
    }
    voice_net_poll();
    if (s_wifi) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
        }
    }
}

bool net_wifi_ready(void) { return s_wifi; }
bool net_ws_ready(void) { return s_ws_on && s_ws && esp_websocket_client_is_connected(s_ws); }
int net_rssi(void) { return s_rssi; }

bool net_ws_send_audio_upload(const uint8_t *pcm, size_t pcm_len, char *session_id, size_t session_id_len)
{
    if (!net_ws_ready() || !pcm || pcm_len == 0) {
        return false;
    }
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "type", "audio_upload");
    cJSON_AddNumberToObject(meta, "sample_rate", 16000);
    cJSON_AddNumberToObject(meta, "channels", 1);
    cJSON_AddNumberToObject(meta, "bit_depth", 16);
    cJSON_AddNumberToObject(meta, "audio_len", (double)pcm_len);
    char *txt = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!txt) {
        return false;
    }
    xEventGroupClearBits(s_events, SESSION_OK);
    s_session[0] = '\0';
    esp_websocket_client_send_text(s_ws, txt, (int)strlen(txt), pdMS_TO_TICKS(2000));
    free(txt);
    esp_websocket_client_send_bin(s_ws, (const char *)pcm, (int)pcm_len, pdMS_TO_TICKS(5000));
    EventBits_t bits = xEventGroupWaitBits(s_events, SESSION_OK, pdTRUE, pdTRUE, pdMS_TO_TICKS(8000));
    if (!(bits & SESSION_OK) || s_session[0] == '\0') {
        return false;
    }
    strncpy(session_id, s_session, session_id_len - 1);
    session_id[session_id_len - 1] = '\0';
    return true;
}

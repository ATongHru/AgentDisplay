#include "net_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "agent_cfg.h"
#include "cJSON.h"
#include "audio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "soc/usb_serial_jtag_struct.h"
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
static char s_chunk_session[16];
static char s_usb_line[512];
static size_t s_usb_len;
static bool s_usb_ready;
static bool s_ble_paused;
static esp_netif_t *s_sta;
static uint32_t s_usj_sof;
static uint32_t s_usj_ok_ms;

#define USJ_HOLD_MS 400

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
        if (!s_ble_paused) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi = false;
        s_ws_on = false;
        xEventGroupClearBits(s_events, WIFI_OK);
        ui_post_link_state(net_usb_ready(), false, false, voice_is_listening(), audio_playback_is_active(), s_rssi);
        if (!s_ble_paused) {
            esp_wifi_connect();
        }
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
        const cJSON *st = cJSON_GetObjectItem(root, "status");
        const cJSON *src = cJSON_GetObjectItem(root, "source");
        if (cJSON_IsString(st) && cJSON_IsString(src) && src->valuestring &&
            strcmp(src->valuestring, "VOICE") == 0) {
            voice_on_server_status(st->valuestring);
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
            s_session[sizeof(s_session) - 1] = '\0';
            xEventGroupSetBits(s_events, SESSION_OK);
        }
        const cJSON *vol = cJSON_GetObjectItem(root, "volume_percent");
        if (cJSON_IsNumber(vol)) {
            audio_set_volume_percent((int)vol->valuedouble);
        }
    } else if (strcmp(t, "audio_chunk") == 0) {
        const cJSON *lenj = cJSON_GetObjectItem(root, "len");
        const cJSON *endj = cJSON_GetObjectItem(root, "end");
        const cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        s_chunk_session[0] = '\0';
        if (cJSON_IsString(sid) && sid->valuestring) {
            strncpy(s_chunk_session, sid->valuestring, sizeof(s_chunk_session) - 1);
            s_chunk_session[sizeof(s_chunk_session) - 1] = '\0';
        }
        s_audio_len = cJSON_IsNumber(lenj) ? (size_t)lenj->valuedouble : 0;
        s_audio_end = cJSON_IsTrue(endj);
        if (s_audio_len > 0) {
            s_expect_audio = true;
        } else {
            voice_on_audio_chunk(s_chunk_session[0] ? s_chunk_session : NULL, NULL, 0, s_audio_end);
        }
    } else if (strcmp(t, "config") == 0) {
        const cJSON *vol = cJSON_GetObjectItem(root, "volume_percent");
        if (cJSON_IsNumber(vol)) {
            audio_set_volume_percent((int)vol->valuedouble);
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
        ui_post_link_state(net_usb_ready(), s_wifi, true, voice_is_listening(), audio_playback_is_active(), s_rssi);
        ESP_LOGI(TAG, "websocket connected");
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        s_ws_on = false;
        xEventGroupClearBits(s_events, WS_OK);
        ui_post_link_state(net_usb_ready(), s_wifi, false, voice_is_listening(), audio_playback_is_active(), s_rssi);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        if (evt->op_code == 0x01) {
            handle_text(evt->data_ptr, evt->data_len);
        } else if (evt->op_code == 0x02 || evt->op_code == 0x00) {
            if (s_expect_audio && evt->data_ptr && evt->data_len > 0) {
                const bool last_piece =
                    (evt->payload_len <= 0) ||
                    (evt->payload_offset + evt->data_len >= evt->payload_len);
                voice_on_audio_chunk(s_chunk_session[0] ? s_chunk_session : NULL,
                                     (const uint8_t *)evt->data_ptr, (size_t)evt->data_len,
                                     s_audio_end && last_piece);
                if (last_piece) {
                    s_expect_audio = false;
                }
            }
        }
    }
}

static bool usj_host_present(uint32_t now_ms)
{
    uint32_t sof = USB_SERIAL_JTAG.fram_num.sof_frame_index;
    if (sof != s_usj_sof) {
        s_usj_sof = sof;
        s_usj_ok_ms = now_ms ? now_ms : 1;
        return true;
    }
    if (usb_serial_jtag_is_connected()) {
        s_usj_ok_ms = now_ms ? now_ms : 1;
        return true;
    }
    return s_usj_ok_ms != 0 && (now_ms - s_usj_ok_ms) < USJ_HOLD_MS;
}

static bool ch343_uart_present(void)
{
    /* CH343 上电后 TX 空闲为高；未供电时为高阻，板内下拉读到低。 */
    return gpio_get_level(PIN_UART0_RX) != 0;
}

static void poll_usb(void)
{
    (void)s_usb_line;
    (void)s_usb_len;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool ready = usj_host_present(now) || ch343_uart_present();
    if (ready != s_usb_ready) {
        s_usb_ready = ready;
        ESP_LOGI(TAG, "usb %s usj_sof=%u ch343=%d",
                 ready ? "up" : "down",
                 (unsigned)USB_SERIAL_JTAG.fram_num.sof_frame_index,
                 (int)ch343_uart_present());
    }
}

static char s_ws_uri[AGENT_CFG_WS_URL_MAX];

static void stop_ws(void)
{
    if (!s_ws) {
        return;
    }
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_ws_on = false;
    if (s_events) {
        xEventGroupClearBits(s_events, WS_OK | SESSION_OK);
    }
}

static void start_ws(void)
{
    if (s_ws) {
        return;
    }
    const agent_cfg_t *acfg = agent_cfg_get();
    memset(s_ws_uri, 0, sizeof(s_ws_uri));
    strncpy(s_ws_uri, acfg->ws_url, sizeof(s_ws_uri) - 1);
    esp_websocket_client_config_t cfg = {
        .uri = s_ws_uri,
        .reconnect_timeout_ms = WS_RECONNECT_MS,
        .network_timeout_ms = 10000,
        .buffer_size = 8192,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
    ESP_LOGI(TAG, "ws connecting %s", s_ws_uri);
}


static void apply_ip_from_cfg(void)
{
    if (!s_sta) {
        return;
    }
    const agent_cfg_t *acfg = agent_cfg_get();
    if (agent_cfg_has_static_ip()) {
        esp_netif_dhcpc_stop(s_sta);
        esp_netif_ip_info_t ip = {0};
        ip.ip.addr = ipaddr_addr(acfg->ip);
        const char *mask = acfg->netmask[0] ? acfg->netmask : "255.255.255.0";
        const char *gw = acfg->gateway[0] ? acfg->gateway : "0.0.0.0";
        ip.netmask.addr = ipaddr_addr(mask);
        ip.gw.addr = ipaddr_addr(gw);
        esp_err_t err = esp_netif_set_ip_info(s_sta, &ip);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_ip_info %s", esp_err_to_name(err));
        }
        if (acfg->gateway[0]) {
            esp_netif_dns_info_t dns = {0};
            dns.ip.u_addr.ip4.addr = ipaddr_addr(acfg->gateway);
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns);
        }
        ESP_LOGI(TAG, "static ip=%s mask=%s gw=%s", acfg->ip, mask, gw);
    } else {
        esp_err_t err = esp_netif_dhcpc_start(s_sta);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "dhcpc_start %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "dhcp enabled");
    }
}

esp_err_t net_init(void)
{
    s_events = xEventGroupCreate();
    gpio_config_t uart_rx = {
        .pin_bit_mask = 1ULL << PIN_UART0_RX,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&uart_rx);
    /* USB Serial/JTAG is the console; skip driver_install to avoid conflict. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    apply_ip_from_cfg();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    const agent_cfg_t *acfg = agent_cfg_get();
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, acfg->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, acfg->pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi start ssid=%s ws=%s", acfg->ssid, acfg->ws_url);
    return ESP_OK;
}


void net_pause_for_ble(void)
{
    if (s_ble_paused) {
        return;
    }
    s_ble_paused = true;
    stop_ws();
    s_wifi = false;
    if (s_events) {
        xEventGroupClearBits(s_events, WIFI_OK | WS_OK | SESSION_OK);
    }
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "wifi_stop %s", esp_err_to_name(err));
    }
    err = esp_wifi_deinit();
    ESP_LOGI(TAG, "deinit wifi for BLE: %s free_internal=%u largest=%u",
             esp_err_to_name(err),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static esp_err_t wifi_driver_reinit_and_start(void)
{
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init %s", esp_err_to_name(err));
        return err;
    }
    const agent_cfg_t *acfg = agent_cfg_get();
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, acfg->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, acfg->pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    apply_ip_from_cfg();
    return esp_wifi_start();
}

void net_resume_after_ble(void)
{
    if (!s_ble_paused) {
        return;
    }
    s_ble_paused = false;
    esp_err_t err = wifi_driver_reinit_and_start();
    ESP_LOGI(TAG, "resume wifi after BLE: %s", esp_err_to_name(err));
}

esp_err_t net_apply_config(void)
{
    const agent_cfg_t *acfg = agent_cfg_get();
    stop_ws();
    s_wifi = false;
    if (s_events) {
        xEventGroupClearBits(s_events, WIFI_OK | WS_OK | SESSION_OK);
    }

    if (s_ble_paused) {
        s_ble_paused = false;
        esp_err_t err = wifi_driver_reinit_and_start();
        ESP_LOGI(TAG, "apply wifi (from BLE pause) ssid=%s start=%s", acfg->ssid, esp_err_to_name(err));
        return err;
    }
    apply_ip_from_cfg();
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, acfg->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, acfg->pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_disconnect();
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "apply wifi ssid=%s ws=%s connect=%s", acfg->ssid, acfg->ws_url, esp_err_to_name(err));
    return err;
}

void net_loop(void)
{
    poll_usb();
    if (s_ble_paused) {
        return;
    }
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
bool net_usb_ready(void)
{
    poll_usb();
    return s_usb_ready;
}
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

bool net_ws_send_audio_stream_begin(char *session_id, size_t session_id_len)
{
    if (!net_ws_ready() || !session_id || session_id_len == 0) {
        return false;
    }
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "type", "audio_upload");
    cJSON_AddBoolToObject(meta, "stream", 1);
    cJSON_AddNumberToObject(meta, "sample_rate", 16000);
    cJSON_AddNumberToObject(meta, "channels", 1);
    cJSON_AddNumberToObject(meta, "bit_depth", 16);
    cJSON_AddNumberToObject(meta, "audio_len", 0);
    char *txt = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!txt) {
        return false;
    }
    xEventGroupClearBits(s_events, SESSION_OK);
    s_session[0] = '\0';
    esp_websocket_client_send_text(s_ws, txt, (int)strlen(txt), pdMS_TO_TICKS(2000));
    free(txt);
    EventBits_t bits = xEventGroupWaitBits(s_events, SESSION_OK, pdTRUE, pdTRUE, pdMS_TO_TICKS(8000));
    if (!(bits & SESSION_OK) || s_session[0] == '\0') {
        return false;
    }
    strncpy(session_id, s_session, session_id_len - 1);
    session_id[session_id_len - 1] = '\0';
    return true;
}

bool net_ws_send_audio_binary(const uint8_t *pcm, size_t pcm_len)
{
    if (!net_ws_ready() || !pcm || pcm_len == 0) {
        return false;
    }
    int ret = esp_websocket_client_send_bin(s_ws, (const char *)pcm, (int)pcm_len, pdMS_TO_TICKS(5000));
    return ret >= 0;
}

bool net_ws_send_audio_end(const char *session_id)
{
    if (!net_ws_ready()) {
        return false;
    }
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "type", "audio_end");
    if (session_id && session_id[0]) {
        cJSON_AddStringToObject(meta, "session_id", session_id);
    }
    char *txt = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!txt) {
        return false;
    }
    int ret = esp_websocket_client_send_text(s_ws, txt, (int)strlen(txt), pdMS_TO_TICKS(2000));
    free(txt);
    return ret >= 0;
}

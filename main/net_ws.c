#include "net_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "agent_cfg.h"
#include "ap_prov.h"
#include "ble_prov.h"
#include "cJSON.h"
#include "audio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/usb_serial_jtag_struct.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "board_pins.h"
#include "sdkconfig.h"
#include "ui.h"
#include "voice.h"
#include "ws_url.h"
#include "mem_utils.h"

static const char *TAG = "net";

#define WIFI_OK BIT0
#define WS_OK BIT1
#define SESSION_OK BIT2

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_ws_tx;

/* 文本帧队列：WS 回调只入队，net 任务消化（cJSON/NVS/UI 等重活移出回调）。 */
#define WS_TEXT_Q_LEN 8
#define WS_TEXT_MAX_BYTES 2048
#define WS_AUDIO_SEND_TIMEOUT_MS 50
typedef struct {
    char *data;
    int len;
} ws_text_msg_t;
static QueueHandle_t s_text_q;
static esp_websocket_client_handle_t s_ws;
static bool s_wifi;
static bool s_ws_on;
static uint32_t s_ws_up_ms;
static volatile uint32_t s_last_rx_ms;      /* 最近一次收到任意 WS 帧的时间（心跳保活用） */
static esp_timer_handle_t s_wifi_retry_timer;
static uint32_t s_wifi_retry_ms = WIFI_RETRY_MS; /* 指数退避当前间隔 */
static wifi_ps_type_t s_ps_mode = WIFI_PS_NONE;  /* 当前 WiFi 省电模式缓存 */
static uint32_t s_display_sent_ms;
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
static bool s_ap_paused;
static uint32_t s_boot_ms;
static bool s_ap_fallback_done;
static bool s_ws_ever_ok;
static bool s_prov_applied;
/* The client stop API waits for an in-flight connect, so this must remain
 * below the 5 s task-watchdog window. */
#define WS_CONNECT_TIMEOUT_MS 3000
#define PROFILE_WS_FAILURES_BEFORE_ROTATE 2u
static volatile uint8_t s_ws_failures;
static volatile bool s_ws_stop_requested;
#define AP_FALLBACK_MS 30000u
#define AP_FALLBACK_AFTER_PROV_MS 60000u
static esp_netif_t *s_sta;
static uint32_t s_usj_sof;
static uint32_t s_usj_ok_ms;

#define USJ_HOLD_MS 400
#define DISPLAY_QUIET_MS 400u
#define DISPLAY_MIN_GAP_MS 120u

static bool ws_send_text(const char *data, int len, int timeout_ms)
{
    if (!s_ws || !s_ws_on || !data || len <= 0) {
        return false;
    }
    if (!s_ws_tx || xSemaphoreTake(s_ws_tx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    int ret = -1;
    if (s_ws && s_ws_on && esp_websocket_client_is_connected(s_ws)) {
        ret = esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(timeout_ms));
    }
    xSemaphoreGive(s_ws_tx);
    return ret >= 0;
}

static bool ws_send_bin(const char *data, int len, int timeout_ms)
{
    if (!s_ws || !s_ws_on || !data || len <= 0) {
        return false;
    }
    if (!s_ws_tx || xSemaphoreTake(s_ws_tx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    int ret = -1;
    if (s_ws && s_ws_on && esp_websocket_client_is_connected(s_ws)) {
        ret = esp_websocket_client_send_bin(s_ws, data, len, pdMS_TO_TICKS(timeout_ms));
    }
    xSemaphoreGive(s_ws_tx);
    return ret >= 0;
}

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

/* WiFi 断线指数退避重连：WIFI_RETRY_MS 起步翻倍，封顶 60s，避免断网时无间隔猛刷 AP。 */
static void wifi_retry_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void schedule_wifi_retry(void)
{
    if (!s_wifi_retry_timer) {
        return;
    }
    esp_timer_stop(s_wifi_retry_timer);
    esp_timer_start_once(s_wifi_retry_timer, (uint64_t)s_wifi_retry_ms * 1000);
    ESP_LOGI(TAG, "wifi retry in %lu ms", (unsigned long)s_wifi_retry_ms);
    if (s_wifi_retry_ms < 60000) {
        uint32_t next_retry_ms = s_wifi_retry_ms * 2;
        s_wifi_retry_ms = next_retry_ms > 60000 ? 60000 : next_retry_ms;
    }
}

uint32_t net_ws_last_rx_ms(void) { return s_last_rx_ms; }

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* STA 重启（含配网结束）：退避清零并立即首连。 */
        s_wifi_retry_ms = WIFI_RETRY_MS;
        if (s_wifi_retry_timer) {
            esp_timer_stop(s_wifi_retry_timer);
        }
        if (!s_ble_paused && !s_ap_paused) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi = false;
        s_ws_on = false;
        xEventGroupClearBits(s_events, WIFI_OK);
        ui_post_link_state(net_usb_ready(), false, false, voice_is_listening(), audio_playback_is_active(), s_rssi);
        if (!s_ble_paused && !s_ap_paused) {
            schedule_wifi_retry();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_ms = WIFI_RETRY_MS; /* 连上即复位退避 */
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_wifi = true;
        xEventGroupSetBits(s_events, WIFI_OK);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_ws_failures = 0;
        esp_sntp_stop();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
    }
}

static void send_wifi_profiles(void)
{
    if (!s_ws) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return;
    }
    uint8_t count = agent_cfg_profile_count();
    uint8_t active = agent_cfg_active_index();
    cJSON_AddStringToObject(root, "type", "wifi_profiles");
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "active", active);
    for (uint8_t i = 0; i < count; i++) {
        const agent_cfg_t *cfg = agent_cfg_get_profile(i);
        if (!cfg) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        char host[AGENT_CFG_HOST_MAX];
        int port = 8000;
        ws_url_split_host_port(cfg->ws_url, host, sizeof(host), &port);
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "ssid", cfg->ssid);
        cJSON_AddStringToObject(item, "password", cfg->pass);
        cJSON_AddStringToObject(item, "host", host);
        cJSON_AddNumberToObject(item, "port", port);
        cJSON_AddStringToObject(item, "ip", cfg->ip);
        cJSON_AddStringToObject(item, "netmask", cfg->netmask);
        cJSON_AddStringToObject(item, "gateway", cfg->gateway);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "profiles", arr);
    char *dump = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!dump) {
        return;
    }
    ws_send_text(dump, (int)strlen(dump), 1000);
    ESP_LOGI(TAG, "sent wifi_profiles count=%u", (unsigned)count);
    free(dump);
}

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

static esp_err_t cfg_from_wifi_json(const cJSON *root, agent_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    if (!json_copy_str(root, "ssid", cfg->ssid, sizeof(cfg->ssid)) || !cfg->ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    json_copy_str(root, "password", cfg->pass, sizeof(cfg->pass));
    char host[AGENT_CFG_HOST_MAX] = {0};
    json_copy_str(root, "host", host, sizeof(host));
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
    if (!host[0] || port < 1 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(cfg->ws_url, sizeof(cfg->ws_url), "ws://%s:%d/ws", host, port);
    if (n <= 0 || n >= (int)sizeof(cfg->ws_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    json_copy_str(root, "ip", cfg->ip, sizeof(cfg->ip));
    json_copy_str(root, "netmask", cfg->netmask, sizeof(cfg->netmask));
    json_copy_str(root, "gateway", cfg->gateway, sizeof(cfg->gateway));
    json_copy_str(root, "api_token", cfg->ws_token, sizeof(cfg->ws_token));
    return ESP_OK;
}

static void send_wifi_profiles_error(const char *detail)
{
    if (!s_ws || !detail) {
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"wifi_profiles\",\"ok\":false,\"error\":\"%s\",\"count\":0,\"active\":0,\"profiles\":[]}",
             detail);
    ws_send_text(buf, (int)strlen(buf), 1000);
}

static void reply_wifi_profiles(esp_err_t err, bool apply)
{
    send_wifi_profiles();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi profile cmd failed %s", esp_err_to_name(err));
        return;
    }
    if (apply) {
        (void)net_apply_config();
    }
}

static int json_index(const cJSON *root, int fallback)
{
    const cJSON *ij = cJSON_GetObjectItem(root, "index");
    if (cJSON_IsNumber(ij)) {
        return (int)ij->valuedouble;
    }
    return fallback;
}

static void send_hello(void)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"type\":\"hello\",\"role\":\"device\",\"rssi\":%d}", s_rssi);
    ws_send_text(buf, (int)strlen(buf), 1000);
}

static void send_debug_info(void)
{
    if (!s_ws || !s_ws_on) {
        return;
    }
    voice_debug_t vd = {0};
    voice_fill_debug(&vd);
    size_t rec_cap = audio_record_capacity();
    size_t play_cap = audio_play_ring_capacity();
    size_t rec_used = audio_capture_size();
    size_t play_used = audio_playback_pending();
    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"debug\",\"dram_free\":%u,\"dram_largest\":%u,"
             "\"psram_free\":%u,\"psram_largest\":%u,"
             "\"rec_bytes\":%u,\"rec_cap\":%u,\"play_bytes\":%u,\"play_cap\":%u,"
             "\"vad_phase\":%d,\"noise_rms\":%.4f,\"noise_peak\":%.4f,"
             "\"start_rms\":%.4f,\"start_peak\":%.4f,\"last_rms\":%.4f,\"last_peak\":%.4f,"
             "\"pdm_gain\":%.2f}",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)rec_used,
             (unsigned)rec_cap, (unsigned)play_used, (unsigned)play_cap, vd.phase, vd.noise_rms,
             vd.noise_peak, vd.start_rms_th, vd.start_peak_th, vd.last_rms, vd.last_peak,
             (double)audio_get_pdm_gain());
    ws_send_text(buf, (int)strlen(buf), 1000);
    mem_report(TAG);
}

/* audio_chunk 元信息须在紧随其后的 binary 帧之前处理（不可入队延迟）。 */
static void apply_audio_chunk_meta(const cJSON *root)
{
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
        s_expect_audio = false;
        (void)voice_enqueue_audio_chunk(s_chunk_session[0] ? s_chunk_session : NULL, NULL, 0,
                                        s_audio_end);
    }
}

static bool try_handle_audio_chunk_immediate(const char *text, int len)
{
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "audio_chunk") != 0) {
        cJSON_Delete(root);
        return false;
    }
    apply_audio_chunk_meta(root);
    cJSON_Delete(root);
    return true;
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
        ws_send_text(pong, (int)strlen(pong), 100);
    } else if (strcmp(t, "status") == 0 || strcmp(t, "text") == 0 || strcmp(t, "append") == 0) {
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
            int pct = (int)vol->valuedouble;
            audio_set_volume_percent(pct);
            ui_post_volume(pct);
        }
    } else if (strcmp(t, "audio_chunk") == 0) {
        apply_audio_chunk_meta(root);
    } else if (strcmp(t, "config") == 0) {
        const cJSON *vol = cJSON_GetObjectItem(root, "volume_percent");
        if (cJSON_IsNumber(vol)) {
            int pct = (int)vol->valuedouble;
            audio_set_volume_percent(pct);
            ui_post_volume(pct);
        }
        const cJSON *ven = cJSON_GetObjectItem(root, "voice_enabled");
        if (cJSON_IsBool(ven)) {
            voice_set_enabled(cJSON_IsTrue(ven));
        }
    } else if (strcmp(t, "get_wifi_profiles") == 0) {
        send_wifi_profiles();
    } else if (strcmp(t, "wifi_profile_save") == 0) {
        agent_cfg_t cfg;
        uint8_t slot = 0;
        uint8_t prev_active = agent_cfg_active_index();
        int idx = json_index(root, -1);
        esp_err_t err = cfg_from_wifi_json(root, &cfg);
        if (err == ESP_OK) {
            err = agent_cfg_upsert_at(idx, &cfg, &slot);
        }
        if (err != ESP_OK) {
            send_wifi_profiles_error(err == ESP_ERR_NO_MEM ? "最多保存 5 条" : "保存失败");
        } else {
            reply_wifi_profiles(err, slot == prev_active);
        }
    } else if (strcmp(t, "wifi_profile_delete") == 0) {
        int idx = json_index(root, -1);
        uint8_t prev_active = agent_cfg_active_index();
        esp_err_t err = (idx < 0) ? ESP_ERR_INVALID_ARG : agent_cfg_delete_at((uint8_t)idx);
        if (err != ESP_OK) {
            send_wifi_profiles_error(err == ESP_ERR_NOT_ALLOWED ? "至少保留 1 条" : "删除失败");
        } else {
            reply_wifi_profiles(err, (uint8_t)idx == prev_active);
        }
    } else if (strcmp(t, "wifi_profile_activate") == 0) {
        int idx = json_index(root, -1);
        uint8_t prev_active = agent_cfg_active_index();
        esp_err_t err = (idx < 0) ? ESP_ERR_INVALID_ARG : agent_cfg_activate((uint8_t)idx);
        if (err != ESP_OK) {
            send_wifi_profiles_error("切换失败");
        } else {
            reply_wifi_profiles(err, (uint8_t)idx != prev_active);
        }
    } else if (strcmp(t, "debug") == 0) {
        send_debug_info();
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
        s_ws_failures = 0;
        s_ws_ever_ok = true;
        s_ws_up_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_last_rx_ms = s_ws_up_ms;
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
        s_ws_up_ms = 0;
        s_expect_audio = false;
        xEventGroupClearBits(s_events, WS_OK);
        if (!s_ws_stop_requested && s_wifi && s_ws_failures < 255u) {
            s_ws_failures++;
            ESP_LOGW(TAG, "ws failure %u/%u", (unsigned)s_ws_failures,
                     (unsigned)PROFILE_WS_FAILURES_BEFORE_ROTATE);
        }
        voice_on_ws_lost();
        ui_post_link_state(net_usb_ready(), s_wifi, false, voice_is_listening(), audio_playback_is_active(), s_rssi);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        s_last_rx_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (evt->op_code == 0x01) {
            if (!s_text_q || !evt->data_ptr || evt->data_len <= 0) {
                /* ignore */
            } else if (evt->data_len <= WS_TEXT_MAX_BYTES &&
                       try_handle_audio_chunk_immediate((const char *)evt->data_ptr, evt->data_len)) {
                /* handled synchronously */
            } else if (evt->data_len > WS_TEXT_MAX_BYTES) {
                ESP_LOGW(TAG, "text frame too large %d, drop", evt->data_len);
            } else {
                char *copy = psram_malloc((size_t)evt->data_len + 1);
                if (!copy) {
                    ESP_LOGW(TAG, "text frame alloc failed, drop");
                } else {
                    memcpy(copy, evt->data_ptr, (size_t)evt->data_len);
                    copy[evt->data_len] = 0;
                    ws_text_msg_t msg = {.data = copy, .len = evt->data_len};
                    if (xQueueSend(s_text_q, &msg, 0) != pdTRUE) {
                        free(copy);
                        ESP_LOGW(TAG, "text queue full, drop");
                    }
                }
            }
        } else if (evt->op_code == 0x02 || evt->op_code == 0x00) {
            if (!s_expect_audio && evt->data_ptr && evt->data_len > 0) {
                ESP_LOGW(TAG, "binary before audio_chunk meta, drop %d", evt->data_len);
            } else if (s_expect_audio && evt->data_ptr && evt->data_len > 0) {
                const bool last_piece =
                    (evt->payload_len <= 0) ||
                    (evt->payload_offset + evt->data_len >= evt->payload_len);
                if (!voice_enqueue_audio_chunk(s_chunk_session[0] ? s_chunk_session : NULL,
                                               (const uint8_t *)evt->data_ptr,
                                               (size_t)evt->data_len,
                                               s_audio_end && last_piece)) {
                    ESP_LOGW(TAG, "ws audio queue full, drop %u", (unsigned)evt->data_len);
                }
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

static char s_ws_uri[AGENT_CFG_WS_URI_MAX];

static void stop_ws(void)
{
    if (!s_ws) {
        return;
    }
    /* esp_websocket_client_stop waits for its task to leave connect(). Mark
     * planned teardown so its DISCONNECTED event is not counted as a failure. */
    s_ws_stop_requested = true;
    esp_err_t err = esp_websocket_client_stop(s_ws);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws stop %s", esp_err_to_name(err));
    }
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_ws_stop_requested = false;
    s_ws_on = false;
    s_expect_audio = false;
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
    ws_url_build_connect_uri(acfg->ws_url, acfg->ws_token, s_ws_uri, sizeof(s_ws_uri));
    esp_websocket_client_config_t cfg = {
        .uri = s_ws_uri,
        .reconnect_timeout_ms = WS_RECONNECT_MS,
        .network_timeout_ms = WS_CONNECT_TIMEOUT_MS,
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
    esp_timer_create(&(esp_timer_create_args_t){.callback = wifi_retry_cb, .name = "wifi_retry"},
                     &s_wifi_retry_timer);
    s_ws_tx = xSemaphoreCreateMutex();
    s_text_q = xQueueCreate(WS_TEXT_Q_LEN, sizeof(ws_text_msg_t));
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
    s_ps_mode = WIFI_PS_NONE;
    ESP_ERROR_CHECK(esp_wifi_start());
    s_boot_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_ap_fallback_done = false;
    s_ws_ever_ok = false;
    ESP_LOGI(TAG, "wifi start ssid=%s ws=%s", acfg->ssid, acfg->ws_url);
    return ESP_OK;
}

void net_pause_sta_for_ap(void)
{
    stop_ws();
    s_wifi = false;
    s_ap_paused = true;
    if (s_events) {
        xEventGroupClearBits(s_events, WIFI_OK | WS_OK | SESSION_OK);
    }
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "wifi_stop for AP %s", esp_err_to_name(err));
    }
}

void net_resume_sta_after_ap(void)
{
    s_ap_paused = false;
    apply_ip_from_cfg();
    const agent_cfg_t *acfg = agent_cfg_get();
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, acfg->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, acfg->pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resume STA set_config %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    s_ps_mode = WIFI_PS_NONE;
    err = esp_wifi_start();
    ESP_LOGI(TAG, "resume STA after AP: %s", esp_err_to_name(err));
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
    s_ps_mode = WIFI_PS_NONE;
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

void net_on_config_applied(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_boot_ms = now;
    s_prov_applied = true;
    ESP_LOGI(TAG, "config applied, AP fallback grace %us", (unsigned)(AP_FALLBACK_AFTER_PROV_MS / 1000));
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

/* 项33：录音/播放（语音会话）期间保持 PS_NONE 低时延，空闲切 MIN_MODEM 省电。
 * 仅在变化时下发，避免每 20ms 调 esp_wifi_set_ps。 */
static void wifi_ps_apply(void)
{
    bool busy = voice_is_busy() || audio_playback_is_active();
    wifi_ps_type_t want = busy ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM;
    if (want == s_ps_mode) {
        return;
    }
    if (esp_wifi_set_ps(want) == ESP_OK) {
        s_ps_mode = want;
        ESP_LOGI(TAG, "wifi ps -> %s", want == WIFI_PS_NONE ? "NONE" : "MIN_MODEM");
    }
}

void net_loop(void)
{
    /* 消化 WS 文本帧（回调内只入队，不重活）。 */
    ws_text_msg_t msg;
    while (s_text_q && xQueueReceive(s_text_q, &msg, 0) == pdTRUE) {
        handle_text(msg.data, msg.len);
        free(msg.data);
    }
    poll_usb();
    if (s_ble_paused || s_ap_paused || ap_prov_active()) {
        return;
    }
    wifi_ps_apply();
    if (s_wifi && !s_ws) {
        start_ws();
    }
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (net_ws_ready() && s_ws_up_ms != 0 && (now_ms - s_ws_up_ms) >= DISPLAY_QUIET_MS) {
        char st[24];
        char src[16];
        if (ui_take_display_report(st, sizeof(st), src, sizeof(src))) {
            if ((now_ms - s_display_sent_ms) < DISPLAY_MIN_GAP_MS) {
                ui_restore_display_report();
            } else if (!net_ws_send_display(st, src)) {
                ui_restore_display_report();
            } else {
                s_display_sent_ms = now_ms;
            }
        }
    }
    if (s_wifi) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
        }
    }

    /* Rotate only after completed WS failures. The previous time-based path
     * could tear down a client while its connect() call still blocked. */
    if (agent_cfg_profile_count() > 1 && !voice_net_busy() &&
        s_ws_failures >= PROFILE_WS_FAILURES_BEFORE_ROTATE) {
        s_ws_failures = 0;
        if (agent_cfg_next_profile()) {
            ESP_LOGW(TAG, "profile rotate after WS failures -> idx=%u",
                     (unsigned)agent_cfg_active_index());
            (void)net_apply_config();
        }
    }

    /* Auto AP only on first boot before WS has ever connected. */
    if (!net_ws_ready() && !s_ws_ever_ok && !s_ap_fallback_done && !ble_prov_active() &&
        !ap_prov_active() && !voice_net_busy()) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t grace_ms = s_prov_applied ? AP_FALLBACK_AFTER_PROV_MS : AP_FALLBACK_MS;
        if ((now - s_boot_ms) >= grace_ms) {
            s_ap_fallback_done = true;
            ESP_LOGW(TAG, "no WiFi/WS after %us, starting AP provisioning",
                     (unsigned)(grace_ms / 1000));
            (void)ap_prov_start();
        }
    }
}

esp_err_t net_force_ap_prov(void)
{
    if (ble_prov_active()) {
        ESP_LOGW(TAG, "force AP blocked: BLE active");
        return ESP_ERR_INVALID_STATE;
    }
    if (ap_prov_active()) {
        ESP_LOGI(TAG, "AP prov already active");
        return ESP_OK;
    }
    s_ap_fallback_done = true;
    ESP_LOGW(TAG, "force AP provisioning");
    return ap_prov_start();
}

bool net_wifi_ready(void) { return s_wifi; }
bool net_ws_ready(void) { return s_ws_on && s_ws && esp_websocket_client_is_connected(s_ws); }
bool net_usb_ready(void)
{
    poll_usb();
    return s_usb_ready;
}
int net_rssi(void) { return s_rssi; }

bool net_ws_send_display(const char *status, const char *source)
{
    if (!net_ws_ready() || !status || !status[0]) {
        return false;
    }
    const char *src = (source && source[0]) ? source : "BOT";
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"display\",\"status\":\"%s\",\"source\":\"%s\",\"gif\":\"%s\"}",
                     status, src, status);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return false;
    }
    return ws_send_text(buf, n, 100);
}

/* 异步 session 握手：只发 audio_upload 元信息并立即返回；
 * session_id 由 handle_text 收到后端 session 帧后置位（SESSION_OK）。
 * 调用方（voice）轮询 poll，超时自行回退，app 任务不再阻塞 2.5s。 */
static bool ws_send_audio_meta(bool stream, size_t audio_len)
{
    if (!net_ws_ready()) {
        return false;
    }
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "type", "audio_upload");
    if (stream) {
        cJSON_AddBoolToObject(meta, "stream", 1);
    }
    cJSON_AddNumberToObject(meta, "sample_rate", 16000);
    cJSON_AddNumberToObject(meta, "channels", 1);
    cJSON_AddNumberToObject(meta, "bit_depth", 16);
    cJSON_AddNumberToObject(meta, "audio_len", (double)audio_len);
    char *txt = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!txt) {
        return false;
    }
    xEventGroupClearBits(s_events, SESSION_OK);
    s_session[0] = 0;
    bool ok = ws_send_text(txt, (int)strlen(txt), 1000);
    free(txt);
    return ok;
}

bool net_ws_audio_session_start(bool stream, size_t audio_len)
{
    return ws_send_audio_meta(stream, audio_len);
}

bool net_ws_audio_session_poll(char *session_id, size_t session_id_len)
{
    if (!s_events) {
        return false;
    }
    if (!(xEventGroupGetBits(s_events) & SESSION_OK) || s_session[0] == 0) {
        return false;
    }
    if (session_id && session_id_len > 0) {
        strncpy(session_id, s_session, session_id_len - 1);
        session_id[session_id_len - 1] = 0;
    }
    return true;
}


bool net_ws_send_audio_binary(const uint8_t *pcm, size_t pcm_len)
{
    if (!net_ws_ready() || !pcm || pcm_len == 0) {
        return false;
    }
    /* app 任务调用：发送必须有很短上界，失败由 voice 状态机在后续轮次重试。 */
    return ws_send_bin((const char *)pcm, (int)pcm_len, WS_AUDIO_SEND_TIMEOUT_MS);
}

bool net_ws_send_audio_end(const char *session_id, size_t total_bytes, bool discard)
{
    if (!net_ws_ready()) {
        return false;
    }
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "type", "audio_end");
    if (session_id && session_id[0]) {
        cJSON_AddStringToObject(meta, "session_id", session_id);
    }
    cJSON_AddNumberToObject(meta, "total_bytes", (double)total_bytes);
    if (discard) {
        cJSON_AddBoolToObject(meta, "discard", 1);
    }
    char *txt = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!txt) {
        return false;
    }
    bool ok = ws_send_text(txt, (int)strlen(txt), 2000);
    free(txt);
    return ok;
}

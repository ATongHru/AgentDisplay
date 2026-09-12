#include "ble_prov.h"

#include <stdio.h>
#include <string.h>

#include "agent_cfg.h"
#include "audio.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "net_ws.h"
#include "ui_msg.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_prov";

#define DEVICE_NAME "AgentDisplay"
#define PROV_TIMEOUT_MS (5 * 60 * 1000)
#define LINE_BUF_MAX 256
#define TX_MTU 180

/* Nordic UART Service UUIDs (LE byte order) */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00,
                     0x40, 0x6e);
static const ble_uuid128_t s_chr_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00,
                     0x40, 0x6e);
static const ble_uuid128_t s_chr_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00,
                     0x40, 0x6e);

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle;
static bool s_nimble_ready;
static bool s_active;
static bool s_stop_req;
static bool s_apply_done;
static int64_t s_deadline_ms;
static int64_t s_stop_at_ms;
static char s_line[LINE_BUF_MAX];
static size_t s_line_len;
static SemaphoreHandle_t s_lock;

static int gap_event(struct ble_gap_event *event, void *arg);
static void handle_line(const char *line);

static void notify_text(const char *msg)
{
    if (!msg || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_tx_val_handle == 0) {
        return;
    }
    size_t len = strlen(msg);
    char buf[TX_MTU];
    if (len + 2 >= sizeof(buf)) {
        len = sizeof(buf) - 3;
    }
    memcpy(buf, msg, len);
    buf[len++] = '\n';
    buf[len] = '\0';

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (!om) {
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify rc=%d", rc);
    }
}

static void trim_inplace(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

static void feed_rx(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                handle_line(s_line);
                s_line_len = 0;
            }
            continue;
        }
        if (s_line_len + 1 < sizeof(s_line)) {
            s_line[s_line_len++] = c;
        } else {
            s_line_len = 0;
            notify_text("ERR line too long");
        }
    }
}

static void handle_line(const char *line_in)
{
    char line[LINE_BUF_MAX];
    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    trim_inplace(line);
    if (line[0] == '\0') {
        return;
    }
    ESP_LOGI(TAG, "cmd: %s", line);

    if (strcmp(line, "APPLY") == 0) {
        esp_err_t err = agent_cfg_save();
        if (err != ESP_OK) {
            notify_text("ERR save");
            return;
        }
        err = net_apply_config();
        if (err != ESP_OK) {
            notify_text("ERR apply");
            return;
        }
        notify_text("OK apply");
        s_apply_done = true;
        s_stop_req = true;
        s_stop_at_ms = esp_timer_get_time() / 1000 + 500;
        return;
    }

    if (strcmp(line, "GET") == 0) {
        const agent_cfg_t *cfg = agent_cfg_get();
        char host[AGENT_CFG_HOST_MAX];
        agent_cfg_format_host(host, sizeof(host));
        char reply[220];
        snprintf(reply, sizeof(reply), "OK ssid=%s host=%s ip=%s mask=%s gw=%s", cfg->ssid, host,
                 cfg->ip[0] ? cfg->ip : "-", cfg->netmask[0] ? cfg->netmask : "-",
                 cfg->gateway[0] ? cfg->gateway : "-");
        notify_text(reply);
        return;
    }


    if (strcmp(line, "LIST") == 0) {
        uint8_t count = agent_cfg_profile_count();
        uint8_t active = agent_cfg_active_index();
        char head[64];
        snprintf(head, sizeof(head), "OK LIST count=%u active=%u", (unsigned)count, (unsigned)active);
        notify_text(head);
        for (uint8_t i = 0; i < count; i++) {
            const agent_cfg_t *cfg = agent_cfg_get_profile(i);
            if (!cfg) {
                continue;
            }
            char host[AGENT_CFG_HOST_MAX];
            host[0] = 0;
            {
                const char *u = cfg->ws_url;
                if (strncmp(u, "ws://", 5) == 0) {
                    u += 5;
                } else if (strncmp(u, "wss://", 6) == 0) {
                    u += 6;
                }
                size_t n = strlen(u);
                if (n >= 3 && strcmp(u + n - 3, "/ws") == 0) {
                    n -= 3;
                }
                if (n >= sizeof(host)) {
                    n = sizeof(host) - 1;
                }
                memcpy(host, u, n);
                host[n] = 0;
            }
            char reply[280];
            snprintf(reply, sizeof(reply),
                     "OK P i=%u ssid=%s pass=%s host=%s ip=%s mask=%s gw=%s",
                     (unsigned)i, cfg->ssid, cfg->pass, host,
                     cfg->ip[0] ? cfg->ip : "-", cfg->netmask[0] ? cfg->netmask : "-",
                     cfg->gateway[0] ? cfg->gateway : "-");
            notify_text(reply);
        }
        notify_text("OK LIST_END");
        return;
    }

    if (strncmp(line, "WIFI:", 5) == 0) {
        const char *body = line + 5;
        const char *comma = strchr(body, ',');
        if (!comma || comma == body) {
            notify_text("ERR wifi format");
            return;
        }
        char ssid[AGENT_CFG_SSID_MAX];
        char pass[AGENT_CFG_PASS_MAX];
        size_t ssid_len = (size_t)(comma - body);
        if (ssid_len >= sizeof(ssid)) {
            notify_text("ERR ssid too long");
            return;
        }
        memcpy(ssid, body, ssid_len);
        ssid[ssid_len] = '\0';
        strncpy(pass, comma + 1, sizeof(pass) - 1);
        pass[sizeof(pass) - 1] = '\0';
        if (agent_cfg_set_wifi(ssid, pass) != ESP_OK) {
            notify_text("ERR wifi");
            return;
        }
        notify_text("OK wifi");
        return;
    }

    if (strncmp(line, "HOST:", 5) == 0) {
        if (agent_cfg_set_host(line + 5) != ESP_OK) {
            notify_text("ERR host");
            return;
        }
        notify_text("OK host");
        return;
    }

    if (strncmp(line, "IP:", 3) == 0) {
        if (agent_cfg_set_ip(line + 3) != ESP_OK) {
            notify_text("ERR ip");
            return;
        }
        notify_text("OK ip");
        return;
    }

    if (strncmp(line, "MASK:", 5) == 0) {
        if (agent_cfg_set_netmask(line + 5) != ESP_OK) {
            notify_text("ERR mask");
            return;
        }
        notify_text("OK mask");
        return;
    }

    if (strncmp(line, "GW:", 3) == 0) {
        if (agent_cfg_set_gateway(line + 3) != ESP_OK) {
            notify_text("ERR gw");
            return;
        }
        notify_text("OK gw");
        return;
    }


    notify_text("ERR unknown");
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                       void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t tmp[LINE_BUF_MAX];
        if (om_len > sizeof(tmp)) {
            om_len = sizeof(tmp);
        }
        int rc = ble_hs_mbuf_to_flat(ctxt->om, tmp, om_len, NULL);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        feed_rx(tmp, om_len);
        if (s_lock) {
            xSemaphoreGive(s_lock);
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_chr_rx_uuid.u,
                    .access_cb = gatt_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                },
                {
                    .uuid = &s_chr_tx_uuid.u,
                    .access_cb = gatt_access,
                    .val_handle = &s_tx_val_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                },
                {
                    0,
                },
            },
    },
    {
        0,
    },
};

static void start_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = (uint8_t)strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGW(TAG, "infer_auto rc=%d; fallback random", rc);
        own_addr_type = BLE_OWN_ADDR_RANDOM;
    }

    struct ble_gap_adv_params adv;
    memset(&adv, 0, sizeof(adv));
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    adv.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc == BLE_HS_EALREADY) {
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start rc=%d addr_type=%u", rc, own_addr_type);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s (addr_type=%u)", DEVICE_NAME, own_addr_type);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected handle=%u", s_conn_handle);
            notify_text("OK ready WIFI:/HOST:/IP:/MASK:/GW:/GET/LIST/APPLY");
        } else {
            start_advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_line_len = 0;
        if (s_active && !s_stop_req) {
            start_advertise();
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u cur=%u", event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ensure_addr rc=%d", rc);
    }
    s_nimble_ready = true;
    if (s_active) {
        start_advertise();
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ensure_nimble(void)
{
    static bool started;
    if (started) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;

    /* Classic BT unused on S3; free controller mem before host queues. */
    esp_err_t rel = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (rel != ESP_OK && rel != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mem_release %s", esp_err_to_name(rel));
    }
    /* BT controller needs contiguous internal DRAM; WiFi buffers eat it. */
    net_pause_for_ble();
    audio_capture_listen_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "heap before nimble: free=%u internal=%u largest_int=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init %s", esp_err_to_name(err));
        net_resume_after_ble();
        return err;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "name_set rc=%d", rc);
    }
    nimble_port_freertos_init(host_task);
    started = true;
    ESP_LOGI(TAG, "nimble started");
    return ESP_OK;
}

esp_err_t ble_prov_prepare(void)
{
    return ensure_nimble();
}

esp_err_t ble_prov_start(void)
{
    if (s_active) {
        ESP_LOGI(TAG, "already active");
        ui_post_ble_prov(true);
        return ESP_OK;
    }
    /* UI first — icon must appear even if controller init is slow */
    ui_post_ble_prov(true);
    esp_err_t err = ensure_nimble();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ensure_nimble failed %s", esp_err_to_name(err));
        ui_post_ble_prov(false);
        return err;
    }
    s_stop_req = false;
    s_apply_done = false;
    s_stop_at_ms = 0;
    s_line_len = 0;
    s_deadline_ms = esp_timer_get_time() / 1000 + PROV_TIMEOUT_MS;
    s_active = true;
    if (s_nimble_ready) {
        start_advertise();
    }
    ESP_LOGI(TAG, "provision window %d min", PROV_TIMEOUT_MS / 60000);
    return ESP_OK;
}

void ble_prov_stop(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    s_stop_req = true;
    ui_post_ble_prov(false);
    ble_gap_adv_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ESP_LOGI(TAG, "provision stopped%s", s_apply_done ? " after apply" : "");
    if (!s_apply_done) {
        net_resume_after_ble();
    }
}

void ble_prov_loop(void)
{
    if (!s_active) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (s_stop_req) {
        if (s_apply_done && s_stop_at_ms != 0 && now < s_stop_at_ms) {
            return;
        }
        ble_prov_stop();
        return;
    }
    if (now >= s_deadline_ms) {
        ESP_LOGW(TAG, "provision timeout");
        ble_prov_stop();
    }
}

bool ble_prov_active(void)
{
    return s_active;
}

#include "btn_boot.h"

#include "ap_prov.h"
#include "ble_prov.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

static const char *TAG = "btn_boot";

#define LONG_PRESS_MS 3000
#define DIAG_PRESS_MS 5000
#define BOOT_GRACE_MS 2000
#define POLL_MS 20
#define RELEASE_DEBOUNCE_MS 80

static int64_t s_boot_ms;
static TaskHandle_t s_task;

static volatile bool s_ble_starting;

static void ble_start_task(void *arg)
{
    (void)arg;
    esp_err_t err = ble_prov_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble_prov_start failed %s", esp_err_to_name(err));
    }
    s_ble_starting = false;
    vTaskDelete(NULL);
}

static void kick_ble_start(void)
{
    if (s_ble_starting || ble_prov_active() || ap_prov_active()) {
        ESP_LOGI(TAG, "BLE already active/starting");
        return;
    }
    s_ble_starting = true;
    ESP_LOGI(TAG, "starting BLE provision");
    BaseType_t ok = xTaskCreate(ble_start_task, "ble_start", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ble_start task create failed");
        if (ble_prov_start() != ESP_OK) {
            /* icon cleared inside ble_prov_start on failure */
        }
        s_ble_starting = false;
    }
}

static void btn_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    bool fired = false;
    bool fired_diag = false;
    int64_t press_start = 0;
    int64_t release_at = 0;
    int last_level = 1;
    int64_t last_log_ms = 0;

    /* Wait out boot grace inside the task */
    while ((esp_timer_get_time() / 1000) - s_boot_ms < BOOT_GRACE_MS) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "BOOT poll armed on GPIO%d (hold %d ms)", PIN_BOOT, LONG_PRESS_MS);

    for (;;) {
        int level = gpio_get_level(PIN_BOOT);
        int64_t now = esp_timer_get_time() / 1000;
        bool down = (level == 0);

        if (level != last_level) {
            ESP_LOGI(TAG, "GPIO%d -> %d (%s)", PIN_BOOT, level, down ? "pressed" : "released");
            last_level = level;
        }

        if (down) {
            release_at = 0;
            if (!pressed) {
                pressed = true;
                fired = false;
                fired_diag = false;
                press_start = now;
                last_log_ms = now;
                ESP_LOGI(TAG, "BOOT press start");
            } else if (!fired && (now - press_start) >= LONG_PRESS_MS) {
                fired = true;
                ESP_LOGI(TAG, "BOOT long-press %lld ms -> BLE", (long long)(now - press_start));
                ui_post_ble_prov(true);
                kick_ble_start();
            } else if (!fired_diag && (now - press_start) >= DIAG_PRESS_MS) {
                fired_diag = true;
                ESP_LOGI(TAG, "BOOT hold %lld ms -> diagnostic", (long long)(now - press_start));
                ui_post_diagnostic_toggle();
            } else if (!fired && (now - last_log_ms) >= 1000) {
                last_log_ms = now;
                ESP_LOGI(TAG, "BOOT holding %lld/%d ms", (long long)(now - press_start), LONG_PRESS_MS);
            }
        } else if (pressed) {
            /* Ignore brief bounce so a 3s hold is not reset at 2.9s. */
            if (release_at == 0) {
                release_at = now;
            } else if ((now - release_at) >= RELEASE_DEBOUNCE_MS) {
                if (!fired) {
                    ESP_LOGI(TAG, "BOOT short press ignored (%lld ms)", (long long)(now - press_start));
                }
                pressed = false;
                fired = false;
                fired_diag = false;
                press_start = 0;
                release_at = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t btn_boot_init(void)
{
    gpio_reset_pin(PIN_BOOT);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    s_boot_ms = esp_timer_get_time() / 1000;

    /* Sample once for bring-up visibility */
    ESP_LOGI(TAG, "BOOT GPIO%d init level=%d pull-up, long-press %dms", PIN_BOOT,
             gpio_get_level(PIN_BOOT), LONG_PRESS_MS);

    if (xTaskCreatePinnedToCore(btn_task, "btn_boot", 4096, NULL, 6, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "btn_boot task create failed");
        return ESP_ERR_NO_MEM;
    }
    return err;
}

void btn_boot_poll(void)
{
    /* Polling moved to dedicated btn_boot task. */
}

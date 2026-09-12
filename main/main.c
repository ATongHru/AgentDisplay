#include <stdio.h>

#include "anim_loader.h"
#include "anim_size.h"
#include "audio.h"
#include "board_pins.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_ws.h"
#include "nvs_flash.h"
#include "ui.h"
#include "voice.h"

static const char *TAG = "app";

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        ui_loop_once();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(audio_init());
    for (;;) {
        audio_task_loop();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        net_loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void app_task(void *arg)
{
    (void)arg;
    uint32_t last_time = 0;
    uint32_t last_link = 0;
    for (;;) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        ui_tick_animation(now);
        voice_loop();

        if (now - last_time >= 1000) {
            last_time = now;
            ui_post_time_tick();
        }
        if (now - last_link >= 1000) {
            last_link = now;
            ui_post_link_state(false, net_wifi_ready(), net_ws_ready(), voice_is_listening(),
                               audio_playback_is_active(), net_rssi());
        }

        if (!ui_voice_session_active() && !net_ws_ready() && !net_wifi_ready()) {
            if ((now - ui_last_event_ms()) >= OFFLINE_TIMEOUT_MS && !ui_offline_active()) {
                ui_post_event_json("{\"status\":\"OFFLINE\",\"source\":\"BOT\"}", false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "boot %s face=%u", FRAME_PLAYER_BUILD, (unsigned)ANIM_FACE_SIZE);
    if (!anim_loader_init()) {
        ESP_LOGE(TAG, "animations missing; flash firmware/data/animations.bin to 0x600000");
    }
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(net_init());
    voice_init();

    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(app_task, "app", 4096, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "tasks started");
}

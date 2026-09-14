#include <stdio.h>
#include <stdlib.h>

#include "anim_loader.h"
#include "anim_size.h"
#include "audio.h"
#include "board_pins.h"
#include "display.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "font_cjk_16.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "agent_cfg.h"
#include "btn_boot.h"
#include "ap_prov.h"
#include "ble_prov.h"
#include "net_ws.h"
#include "nvs_flash.h"
#include "ui.h"
#include "voice.h"
#include "mem_utils.h"
#include "serial_cli.h"

static const char *TAG = "app";

static void lvgl_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL); /* 订阅 TWDT：卡死 5s 自动复位 */
    for (;;) {
        uint32_t delay_ms = ui_loop_once();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(audio_init());
    esp_task_wdt_add(NULL);
    for (;;) {
        audio_task_loop(); /* 内部 I2S 读写均有 20~80ms 超时上界 */
        esp_task_wdt_reset();
    }
}

static void net_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    for (;;) {
        net_loop();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void app_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    uint32_t last_time = 0;
    uint32_t last_link = 0;
    for (;;) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        /* 动画 tick 已收归 lvgl 任务（ui_loop_once），本任务不再触碰帧状态。 */
        voice_loop();
        voice_net_poll();
        ble_prov_loop();
        ap_prov_loop();

        if (now - last_time >= 1000) {
            last_time = now;
            ui_post_time_tick();
        }
        /* 仅链路/语音状态变化时才上报，避免每 250ms 无差别占用 UI 队列；
         * rssi 连续波动，加 10dBm 滞回。 */
        if (now - last_link >= 250) {
            last_link = now;
            static bool s_inited, s_usb, s_wifi, s_ws, s_listening, s_playing;
            static int s_rssi = -1000;
            bool usb = net_usb_ready(), wifi = net_wifi_ready(), ws = net_ws_ready();
            bool listening = voice_is_listening(), playing = audio_playback_is_active();
            int rssi = net_rssi();
            if (!s_inited || usb != s_usb || wifi != s_wifi || ws != s_ws ||
                listening != s_listening || playing != s_playing ||
                (wifi && abs(rssi - s_rssi) >= 10)) {
                s_inited = true;
                s_usb = usb; s_wifi = wifi; s_ws = ws;
                s_listening = listening; s_playing = playing; s_rssi = rssi;
                ui_post_link_state(usb, wifi, ws, listening, playing, rssi);
            }
        }

        esp_task_wdt_reset();
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
        ESP_LOGE(TAG, "animations missing; flash firmware/data/animations.bin to 0x810000");
    }
    ESP_ERROR_CHECK(display_init());
    if (!font_cjk_init()) {
        ESP_LOGE(TAG, "CJK font missing; flash firmware/data/font_cjk_16.bin to 0x610000");
    }
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(agent_cfg_load());
    ESP_ERROR_CHECK(btn_boot_init());
    ESP_ERROR_CHECK(net_init());
    ESP_ERROR_CHECK(serial_cli_init());
    voice_init();
    mem_report("boot");

    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(app_task, "app", 8192, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "tasks started");
}

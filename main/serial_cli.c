#include "serial_cli.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ap_prov.h"
#include "ble_prov.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_ws.h"

static const char *TAG = "cli";

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || isspace((unsigned char)s[n - 1]))) {
        s[--n] = '\0';
    }
    char *p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

static void handle_line(char *line)
{
    trim(line);
    if (!line[0]) {
        return;
    }
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        printf("cmds: ap | ap_stop | ble_stop | help\n");
        return;
    }
    if (strcmp(line, "ap") == 0 || strcmp(line, "ap_start") == 0) {
        esp_err_t err = net_force_ap_prov();
        printf("ap prov %s\n", err == ESP_OK ? "started" : "failed");
        return;
    }
    if (strcmp(line, "ap_stop") == 0) {
        ap_prov_stop();
        printf("ap prov stopped\n");
        return;
    }
    if (strcmp(line, "ble_stop") == 0) {
        ble_prov_stop();
        printf("ble prov stopped\n");
        return;
    }
    printf("unknown cmd: %s (try help)\n", line);
}

static void cli_task(void *arg)
{
    (void)arg;
    char line[96];
    int pos = 0;
    ESP_LOGI(TAG, "ready: ap | ap_stop | ble_stop | help");
    printf("\n>>> CLI: ap | ap_stop | ble_stop | help\n");
    fflush(stdout);
    for (;;) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                handle_line(line);
                pos = 0;
            }
            continue;
        }
        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
            }
            continue;
        }
        if (pos + 1 < (int)sizeof(line)) {
            line[pos++] = (char)c;
        }
    }
}

esp_err_t serial_cli_init(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(cli_task, "cli", 4096, NULL, 2, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ble_prov_prepare(void);
esp_err_t ble_prov_start(void);
void ble_prov_stop(void);
void ble_prov_loop(void);
bool ble_prov_active(void);
/** 关闭 NimBLE 释放射频（AP 模式或 BLE 配网 APPLY 后调用）。 */
void ble_radio_shutdown(void);

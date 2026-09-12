#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ble_prov_prepare(void);
esp_err_t ble_prov_start(void);
void ble_prov_stop(void);
void ble_prov_loop(void);
bool ble_prov_active(void);

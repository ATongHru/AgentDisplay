#pragma once

#include "cJSON.h"
#include "esp_err.h"

/** Parse JSON into RAM only (no save / reconnect). */
esp_err_t prov_cfg_load_from_json(const cJSON *root);

/** Parse JSON fields and persist + apply (same semantics as BLE APPLY). */
esp_err_t prov_cfg_apply_from_json(const cJSON *root);

/** Save current RAM config to NVS and reconnect STA. Stops AP prov if active. */
esp_err_t prov_cfg_apply_saved(void);

/** Fill JSON object with current in-memory config (ssid, password, host, port, ip, ...). */
void prov_cfg_fill_json(cJSON *root);

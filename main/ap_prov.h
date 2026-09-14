#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ap_prov_start(void);
void ap_prov_stop(void);
/** Tear down AP/HTTP before STA reconnect (called from prov_cfg_apply_saved). */
void ap_prov_stop_for_apply(void);
void ap_prov_loop(void);
bool ap_prov_active(void);
/** Save + STA reconnect after HTTP response (must not run inside httpd handler). */
void ap_prov_schedule_apply(void);

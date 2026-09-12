#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t display_init(void);
void display_blit_rgb565(const uint8_t *buf, int x, int y, int w, int h);

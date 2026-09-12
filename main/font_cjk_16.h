#pragma once

#include "lvgl.h"

#include <stdbool.h>

/** Load CJK font from cjk_font flash partition (call once before UI uses it). */
bool font_cjk_init(void);

/** Runtime font; NULL if partition missing / load failed. */
const lv_font_t *font_cjk_get(void);

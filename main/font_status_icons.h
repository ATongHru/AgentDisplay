#pragma once

#include "lvgl.h"

extern const lv_font_t font_status_icons_20;

#define UI_SYMBOL_WIFI   "\xEE\x9C\x81" /* U+E701 */
#define UI_SYMBOL_MIC    "\xEE\x9C\xA0" /* U+E720 */
#define UI_SYMBOL_SPK    "\xEE\x9D\xA7" /* U+E767 */
#define UI_SYMBOL_SERIAL "\xEE\xA2\xAB" /* U+E8AB duplex arrows */

/* backward-compatible alias */
#define UI_SYMBOL_USB UI_SYMBOL_SERIAL

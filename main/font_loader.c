#include "font_cjk_16.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "font_size.h"

static const char *TAG = "font_cjk";

static lv_font_t *s_font;

static bool header_looks_valid(const uint8_t *p)
{
    /* LVGL bin font: uint32 length + "head" */
    return memcmp(p + 4, "head", 4) == 0;
}

bool font_cjk_init(void)
{
    if (s_font) {
        return true;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)FONT_CJK_PART_SUBTYPE, "cjk_font");
    if (!part) {
        /* Accept legacy partition name from older builds. */
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        (esp_partition_subtype_t)FONT_CJK_PART_SUBTYPE, "voice_font");
    }
    if (!part) {
        ESP_LOGE(TAG, "cjk_font partition not found");
        return false;
    }
    if (part->size < FONT_CJK_BIN_SIZE) {
        ESP_LOGE(TAG, "partition too small (%u < %u)", (unsigned)part->size,
                 (unsigned)FONT_CJK_BIN_SIZE);
        return false;
    }

    uint8_t hdr[8];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK || !header_looks_valid(hdr)) {
        ESP_LOGE(TAG, "no LVGL bin font at 0x%x; flash firmware/data/font_cjk_16.bin",
                 (unsigned)FONT_CJK_PART_OFFSET);
        return false;
    }

    const void *map_ptr = NULL;
    esp_partition_mmap_handle_t map_handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, FONT_CJK_BIN_SIZE, ESP_PARTITION_MMAP_DATA,
                                       &map_ptr, &map_handle);
    if (err != ESP_OK || map_ptr == NULL) {
        ESP_LOGE(TAG, "mmap failed (%s)", esp_err_to_name(err));
        return false;
    }

    /* lv_binfont copies glyph tables via lv_malloc. Needs CONFIG_LV_USE_CLIB_MALLOC so
     * this ~900KB allocation goes to ESP-IDF heap / PSRAM (builtin pool is 64KB). */
    s_font = lv_binfont_create_from_buffer((void *)map_ptr, (uint32_t)FONT_CJK_BIN_SIZE);
    esp_partition_munmap(map_handle);
    if (!s_font) {
        ESP_LOGE(TAG, "lv_binfont_create_from_buffer failed");
        return false;
    }

    ESP_LOGI(TAG, "loaded CJK font (%u bytes from flash)", (unsigned)FONT_CJK_BIN_SIZE);
    return true;
}

const lv_font_t *font_cjk_get(void)
{
    return s_font;
}

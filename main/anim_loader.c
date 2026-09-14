#include "anim_loader.h"

#include <stdlib.h>
#include <string.h>

#include "anim_size.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "mem_utils.h"

static const char *TAG = "anim";

const char FRAME_PLAYER_BUILD[] = "AGIF-FLASH-v5-rle-spi";

animation_data_t animations[16];
size_t animation_count = 0;

static const uint8_t *s_blob;
static esp_partition_mmap_handle_t s_map_handle;
static frame_data_t *s_all_frames;

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static bool parse_blob(void)
{
    if (!s_blob || memcmp(s_blob, "AGIF", 4) != 0) {
        return false;
    }
    if (u16le(s_blob + 4) != 1) {
        return false;
    }
    uint16_t anim_count = u16le(s_blob + 6);
    if (anim_count == 0 || anim_count > 16) {
        return false;
    }
    uint16_t face_w = u16le(s_blob + 8);
    uint16_t face_h = u16le(s_blob + 10);
    if (face_w != ANIM_FACE_SIZE || face_h != ANIM_FACE_SIZE) {
        ESP_LOGE(TAG, "face %ux%u != firmware %u", face_w, face_h, (unsigned)ANIM_FACE_SIZE);
        return false;
    }

    size_t total_frames = 0;
    for (uint16_t i = 0; i < anim_count; ++i) {
        const uint8_t *entry = s_blob + 12 + i * 6;
        total_frames += u16le(entry);
    }
    if (total_frames == 0) {
        return false;
    }

    s_all_frames = psram_malloc(total_frames * sizeof(frame_data_t));
    if (!s_all_frames) {
        return false;
    }

    size_t frame_cursor = 0;
    for (uint16_t i = 0; i < anim_count; ++i) {
        const uint8_t *entry = s_blob + 12 + i * 6;
        uint16_t frame_count = u16le(entry);
        uint32_t table_off = u32le(entry + 2);
        frame_data_t *frames = s_all_frames + frame_cursor;
        for (uint16_t f = 0; f < frame_count; ++f) {
            const uint8_t *row = s_blob + table_off + f * 8;
            frames[f].duration_ms = u16le(row);
            frames[f].size = u16le(row + 2);
            frames[f].data = s_blob + u32le(row + 4);
        }
        animations[i].frames = frames;
        animations[i].count = frame_count;
        frame_cursor += frame_count;
    }
    animation_count = anim_count;
    return true;
}

bool anim_loader_init(void)
{
    if (animation_count > 0) {
        return true;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x98, "animations");
    if (!part) {
        ESP_LOGE(TAG, "partition not found");
        return false;
    }
    if (part->size < ANIM_BIN_SIZE) {
        ESP_LOGE(TAG, "partition too small");
        return false;
    }

    const void *map_ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, ANIM_BIN_SIZE, ESP_PARTITION_MMAP_DATA, &map_ptr,
                                       &s_map_handle);
    if (err != ESP_OK || map_ptr == NULL) {
        ESP_LOGE(TAG, "mmap failed (%s)", esp_err_to_name(err));
        return false;
    }
    s_blob = (const uint8_t *)map_ptr;
    if (!parse_blob()) {
        ESP_LOGE(TAG, "parse failed");
        esp_partition_munmap(s_map_handle);
        s_blob = NULL;
        return false;
    }
    ESP_LOGI(TAG, "loaded %u anims, bin %u bytes", (unsigned)animation_count, (unsigned)ANIM_BIN_SIZE);
    return true;
}

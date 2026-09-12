#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Packed GIF order in animations.bin. Index == this enum. Keep in sync with
 * scripts/gen_frame_player.py names[]. */
typedef enum {
    UI_ST_IDLE = 0,
    UI_ST_THINKING,
    UI_ST_CODING,
    UI_ST_READING,
    UI_ST_TESTING,
    UI_ST_WAITING,
    UI_ST_DONE,
    UI_ST_ERROR,
    UI_ST_OFFLINE,
    UI_ST_STALE,
    UI_ST_UNKNOWN,
    UI_ST_TOOL,
    UI_ST_EAR,
    UI_ST_SPEAKING,
    UI_ST_COUNT
} ui_status_id_t;

typedef struct {
    ui_status_id_t id;
    const char *key;
    const char *label_cn;
    uint32_t color;
} ui_status_info_t;

static const ui_status_info_t k_ui_status[] = {
    {UI_ST_IDLE, "IDLE", "空闲", 0xE2E8F0},
    {UI_ST_THINKING, "THINKING", "思考中", 0x818CF8},
    {UI_ST_CODING, "CODING", "编码中", 0xA78BFA},
    {UI_ST_READING, "READING", "读取中", 0x22D3EE},
    {UI_ST_TESTING, "TESTING", "测试中", 0x2DD4A7},
    {UI_ST_WAITING, "WAITING", "等待中", 0xFBBF24},
    {UI_ST_DONE, "DONE", "已完成", 0x45D483},
    {UI_ST_ERROR, "ERROR", "错误", 0xFF4D5A},
    {UI_ST_OFFLINE, "OFFLINE", "离线", 0x94A3B8},
    {UI_ST_STALE, "STALE", "已过期", 0x94A3B8},
    {UI_ST_UNKNOWN, "UNKNOWN", "", 0xE2E8F0},
    {UI_ST_TOOL, "TOOL", "使用工具", 0xF59E0B},
    {UI_ST_EAR, "EAR", "收听中", 0x38BDF8},
    {UI_ST_SPEAKING, "SPEAKING", "播报中", 0x34D399},
};

_Static_assert(sizeof(k_ui_status) / sizeof(k_ui_status[0]) == UI_ST_COUNT,
               "ui status table must match enum / GIF pack order");

static inline const ui_status_info_t *ui_status_info(ui_status_id_t id)
{
    if ((unsigned)id >= (unsigned)UI_ST_COUNT) {
        id = UI_ST_UNKNOWN;
    }
    return &k_ui_status[id];
}

static inline ui_status_id_t ui_status_from_key(const char *raw)
{
    if (!raw || raw[0] == '\0') {
        return UI_ST_IDLE;
    }
    if (strcmp(raw, "WORKING") == 0) {
        return UI_ST_CODING;
    }
    if (strcmp(raw, "TOOL_CALL") == 0) {
        return UI_ST_TOOL;
    }
    for (unsigned i = 0; i < (unsigned)UI_ST_COUNT; ++i) {
        if (strcmp(raw, k_ui_status[i].key) == 0) {
            return (ui_status_id_t)i;
        }
    }
    return UI_ST_UNKNOWN;
}

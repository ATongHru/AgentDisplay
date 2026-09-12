#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    uint16_t duration_ms;
} frame_data_t;

typedef struct {
    const frame_data_t *frames;
    size_t count;
} animation_data_t;

extern animation_data_t animations[];
extern size_t animation_count;
extern const char FRAME_PLAYER_BUILD[];

bool anim_loader_init(void);

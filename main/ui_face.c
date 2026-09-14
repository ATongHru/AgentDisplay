#include "ui_face.h"

#include <stdlib.h>
#include <string.h>

#include "anim_loader.h"
#include "anim_size.h"
#include "board_pins.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mem_utils.h"
#include "ui.h"
#include "ui_status.h"

static const char *TAG = "ui_face";

#define FACE_FRAME_BYTES ((size_t)ANIM_FACE_SIZE * ANIM_FACE_SIZE * 2)

/* 帧双缓冲约 32KB，运行时分配（PSRAM 优先），避免挤占内部 DRAM。 */
static uint8_t *frame_buffer_a;
static uint8_t *frame_buffer_b;
static uint8_t *frame_buffer_front;

static bool s_face_dirty = true; /* 表情区内容变化才置位，blit 后清零 */
static bool s_missing_anim_face; /* 动画资源缺失：占位脸允许 blit 一次 */

static uint32_t last_anim_tick;
static uint8_t frame_index;
static size_t animation_index;
static uint16_t frame_elapsed_ms;

static void fill_rgb565_bg(uint8_t *dest, size_t dest_size)
{
    const uint8_t lo = (uint8_t)(FACE_BG_RGB565 & 0xFF);
    const uint8_t hi = (uint8_t)(FACE_BG_RGB565 >> 8);
    for (size_t i = 0; i + 1 < dest_size; i += 2) {
        dest[i] = lo;
        dest[i + 1] = hi;
    }
}

static void decode_frame_rle(const frame_data_t *frame, uint8_t *dest, size_t dest_size)
{
    fill_rgb565_bg(dest, dest_size);
    size_t out = 0;
    for (size_t i = 0; i + 2 < frame->size && out + 1 < dest_size; i += 3) {
        uint8_t count = frame->data[i];
        uint8_t hi = frame->data[i + 1];
        uint8_t lo = frame->data[i + 2];
        while (count-- && out + 1 < dest_size) {
            dest[out++] = lo;
            dest[out++] = hi;
        }
    }
}

static void put_px(uint8_t *buf, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= FACE_SIZE || y >= FACE_SIZE) {
        return;
    }
    size_t i = ((size_t)y * (size_t)FACE_SIZE + (size_t)x) * 2;
    buf[i] = (uint8_t)(color & 0xFF);
    buf[i + 1] = (uint8_t)(color >> 8);
}

static void draw_thick_line(uint8_t *buf, int x0, int y0, int x1, int y1, uint16_t color, int thick)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int ty = -thick; ty <= thick; ++ty) {
            for (int tx = -thick; tx <= thick; ++tx) {
                if (tx * tx + ty * ty <= thick * thick) {
                    put_px(buf, x0 + tx, y0 + ty, color);
                }
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void render_frame(void)
{
    if (ui_prov_active()) {
        return;
    }
    if (!frame_buffer_a || !frame_buffer_b) {
        return;
    }
    if (animation_count == 0 || animations[animation_index].count == 0) {
        return;
    }
    uint8_t *back = (frame_buffer_front == frame_buffer_a) ? frame_buffer_b : frame_buffer_a;
    decode_frame_rle(&animations[animation_index].frames[frame_index], back, FACE_FRAME_BYTES);
    frame_buffer_front = back;
    /* 标脏即可：ui_loop_once 每轮末尾统一 blit 一次，去掉解码后+循环尾的双倍传输。 */
    s_face_dirty = true;
}

void ui_face_init(void)
{
    frame_buffer_a = psram_malloc(FACE_FRAME_BYTES);
    frame_buffer_b = psram_malloc(FACE_FRAME_BYTES);
    if (frame_buffer_a && frame_buffer_b) {
        fill_rgb565_bg(frame_buffer_a, FACE_FRAME_BYTES);
        fill_rgb565_bg(frame_buffer_b, FACE_FRAME_BYTES);
        frame_buffer_front = frame_buffer_a;
    } else {
        ESP_LOGE(TAG, "face frame buffers alloc failed; face animation disabled");
    }
}

void ui_face_tick(uint32_t now_ms)
{
    if (animation_count == 0 || animations[animation_index].count == 0) {
        last_anim_tick = now_ms;
        return;
    }
    if (ui_prov_active()) {
        last_anim_tick = now_ms;
        return;
    }
    uint32_t elapsed = now_ms - last_anim_tick;
    last_anim_tick = now_ms;
    frame_elapsed_ms = (uint16_t)(frame_elapsed_ms + elapsed);
    const frame_data_t *frame = &animations[animation_index].frames[frame_index];
    if (frame_elapsed_ms >= frame->duration_ms) {
        frame_elapsed_ms = (uint16_t)(frame_elapsed_ms - frame->duration_ms);
        frame_index = (uint8_t)((frame_index + 1) % animations[animation_index].count);
        /* 已在 lvgl 任务上下文，直接渲染，无需再经队列中转。 */
        render_frame();
    }
}

void ui_face_blit(void)
{
    /* 帧未变化时每 5ms 全量刷屏纯属浪费 SPI 带宽：仅脏时传输。 */
    if (!s_face_dirty) {
        return;
    }
    if (!frame_buffer_front) {
        return;
    }
    if (!ui_prov_active() && animation_count == 0 && !s_missing_anim_face) {
        return;
    }
    s_face_dirty = false;
    display_blit_rgb565(frame_buffer_front, FACE_X, FACE_Y, FACE_SIZE, FACE_SIZE);
}

bool ui_face_dirty_pending(void) { return s_face_dirty; }

void ui_face_mark_dirty(void) { s_face_dirty = true; }

size_t ui_face_animation(void) { return animation_index; }

void ui_face_set_animation(size_t anim)
{
    animation_index = anim;
    frame_index = 0;
    frame_elapsed_ms = 0;
    render_frame();
}

void ui_face_sync_tick(void)
{
    last_anim_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    frame_elapsed_ms = 0;
}

void ui_face_render_bluetooth(void)
{
    /* Classic Bluetooth rune on face background. Color #3B82F6. */
    if (!frame_buffer_a) {
        return;
    }
    fill_rgb565_bg(frame_buffer_a, FACE_FRAME_BYTES);
    frame_buffer_front = frame_buffer_a;
    const uint16_t blue = 0x3C1E; /* RGB565 #3B82F6 */
    const int cx = FACE_SIZE / 2;
    const int cy = FACE_SIZE / 2;
    const int s = FACE_SIZE / 3;
    draw_thick_line(frame_buffer_front, cx, cy - s, cx, cy + s, blue, 2);
    draw_thick_line(frame_buffer_front, cx, cy - s, cx + (s * 2) / 3, cy - s / 3, blue, 2);
    draw_thick_line(frame_buffer_front, cx + (s * 2) / 3, cy - s / 3, cx, cy, blue, 2);
    draw_thick_line(frame_buffer_front, cx, cy + s, cx + (s * 2) / 3, cy + s / 3, blue, 2);
    draw_thick_line(frame_buffer_front, cx + (s * 2) / 3, cy + s / 3, cx, cy, blue, 2);
    /* 只标脏，统一在 ui_loop_once 末尾 blit，避免同轮多次 SPI 全量传输。 */
    s_face_dirty = true;
}

void ui_face_render_ap_waiting(void)
{
    animation_index = UI_ST_WAITING;
    frame_index = 0;
    frame_elapsed_ms = 0;
    if (frame_buffer_a && frame_buffer_b && animation_count > 0 &&
        animations[animation_index].count > 0) {
        uint8_t *back = (frame_buffer_front == frame_buffer_a) ? frame_buffer_b : frame_buffer_a;
        decode_frame_rle(&animations[animation_index].frames[frame_index], back, FACE_FRAME_BYTES);
        frame_buffer_front = back;
        s_face_dirty = true;
    }
}

void ui_face_render_missing(void)
{
    /* 动画资源缺失时的占位脸：红叉眼 + 平嘴，产测一眼可辨。 */
    s_missing_anim_face = true;
    if (!frame_buffer_front) {
        return;
    }
    fill_rgb565_bg(frame_buffer_front, FACE_FRAME_BYTES);
    const uint16_t red = 0xF800;
    draw_thick_line(frame_buffer_front, 22, 26, 38, 42, red, 2);
    draw_thick_line(frame_buffer_front, 38, 26, 22, 42, red, 2);
    draw_thick_line(frame_buffer_front, 52, 26, 68, 42, red, 2);
    draw_thick_line(frame_buffer_front, 68, 26, 52, 42, red, 2);
    draw_thick_line(frame_buffer_front, 30, 66, 60, 66, red, 2);
    s_face_dirty = true;
}

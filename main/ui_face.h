#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anim_size.h"
#include "board_pins.h"

/* 表情区几何（ui.c 排布标签也需要） */
#define FACE_SIZE ((int)ANIM_FACE_SIZE)
#define FACE_X ((LCD_W - FACE_SIZE) / 2)
#define FACE_Y 71

/* 表情帧缓冲 + RLE 动画驱动 + 直 SPI blit（绕 LVGL，90x90 表情区）。
 * 全部函数仅在 lvgl 任务上下文调用（ui_face_init 除外，app_main 初始化期调用）。 */

void ui_face_init(void);                /* 分配 PSRAM 双缓冲并填底色 */
void ui_face_tick(uint32_t now_ms);     /* 动画帧推进 */
void ui_face_blit(void);                /* 仅脏时发起 SPI 传输 */
bool ui_face_dirty_pending(void);       /* 有待刷帧（ui_loop_once busy 判定用） */
void ui_face_mark_dirty(void);          /* 强制重绘（如诊断覆盖层关闭后） */

size_t ui_face_animation(void);         /* 当前动画索引 */
void ui_face_set_animation(size_t anim);/* 切动画：重置帧计数并立即解码首帧 */
void ui_face_sync_tick(void);           /* 动画计时对齐到现在（配网退出时） */

void ui_face_render_bluetooth(void);    /* BLE 配网静态脸并标脏 */
void ui_face_render_ap_waiting(void);   /* AP 配网：切 WAITING 动画并解首帧 */
void ui_face_render_missing(void);      /* 动画资源缺失占位脸（允许 blit 一次） */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_init(void);
void audio_task_loop(void);

esp_err_t audio_capture_listen_start(void);
void audio_capture_listen_stop(void);
void audio_capture_begin_store(void);
void audio_capture_end_store(void);
void audio_capture_clear(void);
float audio_capture_peak(void);
float audio_capture_rms(void);
uint32_t audio_capture_frame_seq(void);
void audio_capture_reset_hp(void);
bool audio_capture_is_listening(void);
const uint8_t *audio_capture_data(void);
size_t audio_capture_size(void);
void audio_capture_reset(void);

bool audio_playback_start(void);
void audio_playback_stop(void);
bool audio_playback_write(const uint8_t *data, size_t len);
void audio_playback_service(void);
void audio_playback_begin_fadeout(void);
void audio_playback_cancel_fadeout(void);
bool audio_playback_is_active(void);
size_t audio_playback_pending(void);
bool audio_playback_should_stop(void);
size_t audio_record_capacity(void);
size_t audio_play_ring_capacity(void);
float audio_get_pdm_gain(void);
void audio_set_volume_percent(int percent);
int audio_get_volume_percent(void);

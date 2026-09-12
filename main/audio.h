#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_init(void);
void audio_task_loop(void);

bool audio_capture_listen_start(void);
void audio_capture_listen_stop(void);
void audio_capture_begin_store(void);
void audio_capture_end_store(void);
void audio_capture_clear(void);
size_t audio_capture_poll(void);
size_t audio_capture_rms(void);
bool audio_capture_is_listening(void);
const uint8_t *audio_capture_data(void);
size_t audio_capture_size(void);
void audio_capture_reset(void);

bool audio_playback_start(void);
void audio_playback_stop(void);
bool audio_playback_write(const uint8_t *data, size_t len);
void audio_playback_service(void);
bool audio_playback_is_active(void);
size_t audio_playback_pending(void);
bool audio_playback_should_stop(void);
void audio_set_volume_percent(int percent);
int audio_get_volume_percent(void);

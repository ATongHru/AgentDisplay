#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void voice_init(void);
void voice_loop(void);
void voice_net_poll(void);
void voice_on_audio_chunk(const char *session_id, const uint8_t *data, size_t len, bool end);
void voice_on_server_status(const char *status);
void voice_on_ws_lost(void);
void voice_set_enabled(bool enabled);
bool voice_is_enabled(void);
bool voice_session_active(void);
bool voice_is_listening(void);

typedef struct {
    int phase;
    float noise_rms;
    float noise_peak;
    float start_rms_th;
    float start_peak_th;
    float last_rms;
    float last_peak;
} voice_debug_t;

void voice_fill_debug(voice_debug_t *out);

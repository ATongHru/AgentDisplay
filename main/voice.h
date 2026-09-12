#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void voice_init(void);
void voice_loop(void);
void voice_net_poll(void);
void voice_on_audio_chunk(const uint8_t *data, size_t len, bool end);
void voice_on_server_status(const char *status);
bool voice_session_active(void);
bool voice_is_listening(void);

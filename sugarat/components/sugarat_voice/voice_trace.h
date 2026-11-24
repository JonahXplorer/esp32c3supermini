#pragma once

#include <stdint.h>

void voice_trace_init(void);
void voice_trace_begin(const char *source, int64_t wake_us);
void voice_trace_end(void);
void voice_trace_playback_reset(int64_t cleared_us, const char *reason, uint32_t reset_seq,
                                unsigned incoming_reply, unsigned last_i2s_reply,
                                unsigned pending_pcm_ms, unsigned queue_slots);

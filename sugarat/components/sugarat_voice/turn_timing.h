#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "agent_link_platform.h"

void voice_timing_set_active(bool active);
void voice_timing_mic_level(unsigned level, bool echo_suppressed);
void voice_timing_ws_event(agent_link_platform_voice_timing_event_t event, int64_t received_us);
void voice_timing_first_pcm(unsigned reply_id);
void voice_timing_reply_end(void);
void voice_timing_speaker_start(unsigned reply_id, int64_t submitted_us);

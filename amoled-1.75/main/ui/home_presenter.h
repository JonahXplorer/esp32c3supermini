#pragma once
#include "screen.h"
#include "home_screen.h"

/* UI projection only: no network, microphone or voice state mutations. */
home_snapshot_t home_presenter_snapshot(const screen_state_t *state, unsigned playback_level);

#pragma once
#include "screen.h"
#include "home_screen.h"

/* UI projection for both boards; the application owns voice and network state. */
home_snapshot_t home_presenter_snapshot(const screen_state_t *state, unsigned playback_level);

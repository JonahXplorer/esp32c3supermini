#pragma once

#include "screen.h"

void manual_control_init(bool wifi_managed);
void manual_control_update(bool visible, bool call_active, screen_action_t action, manual_control_view_t *view);

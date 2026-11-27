#pragma once

#include <stdbool.h>

// Qualify a cold start or wake before any peripherals are initialized.
void sugarat_power_start(void);

// The power-on press is ignored until release. Only a new long press can shut down.
bool sugarat_power_off_requested(void);

// Call after saving settings and stopping voice and Wi-Fi.
_Noreturn void sugarat_power_off(void);

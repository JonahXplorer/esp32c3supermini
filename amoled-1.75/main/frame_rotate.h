#pragma once
#include <stdint.h>

/* Read-only logical frame -> clockwise, panel byte-order frame. */
void amoled_frame_rotate_rgb565(const uint16_t *source, uint16_t *destination, unsigned side);
/* Update a logical subrectangle in an existing complete panel frame. */
void amoled_frame_rotate_region_rgb565(const uint16_t *source, uint16_t *destination, unsigned side,
                                       unsigned x, unsigned y, unsigned width, unsigned height);

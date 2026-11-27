#include "frame_rotate.h"

void amoled_frame_rotate_rgb565(const uint16_t *source, uint16_t *destination, unsigned side)
{
    amoled_frame_rotate_region_rgb565(source, destination, side, 0, 0, side, side);
}

void amoled_frame_rotate_region_rgb565(const uint16_t *source, uint16_t *destination, unsigned side,
                                       unsigned x, unsigned y, unsigned width, unsigned height)
{
    /* Tiling keeps both source and transposed destination cache lines hot in
     * PSRAM. A full-width strided transpose repeatedly evicts the same lines. */
    enum { TILE = 16 };
    for (unsigned top = y; top < y + height; top += TILE) {
        const unsigned bottom = top + TILE < y + height ? top + TILE : y + height;
        for (unsigned left = x; left < x + width; left += TILE) {
            const unsigned right = left + TILE < x + width ? left + TILE : x + width;
            for (unsigned sy = top; sy < bottom; ++sy) {
                const uint16_t *row = source + sy * side;
                for (unsigned sx = left; sx < right; ++sx) {
                    const uint16_t pixel = row[sx];
                    destination[sx * side + side - 1 - sy] = (uint16_t)((pixel >> 8) | (pixel << 8));
                }
            }
        }
    }
}

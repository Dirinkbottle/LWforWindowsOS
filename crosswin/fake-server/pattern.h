#ifndef CROSSWIN_FAKE_SERVER_PATTERN_H
#define CROSSWIN_FAKE_SERVER_PATTERN_H

#include <stdbool.h>
#include <stdint.h>

/* Creates a deterministic top-down BGRA8888 diagnostic pattern. */
bool cw_pattern_generate(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride);

/* Draws a clipped 21 x 21 high-contrast crosshair at a surface coordinate. */
bool cw_pattern_draw_crosshair(
    uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    int32_t center_x,
    int32_t center_y);

#endif

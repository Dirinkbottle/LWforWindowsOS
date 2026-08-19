#ifndef CROSSWIN_PIXEL_H
#define CROSSWIN_PIXEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One top-down BGRA8888 pixel in a framebuffer byte stream. */
enum { CW_BGRA8888_BYTES_PER_PIXEL = 4U };

typedef struct {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
} CwBgra8888;

/* Weston renderer surface readback is PIXMAN_a8b8g8r8. On little-endian
 * machines its byte order is R,G,B,A, while CWNP carries premultiplied
 * B,G,R,A for Windows UpdateLayeredWindow. Alpha and premultiplication are
 * intentionally preserved; only red and blue are exchanged. */
bool cw_weston_rgba_to_bgra_inplace(uint8_t *pixels, size_t byte_count);

#endif

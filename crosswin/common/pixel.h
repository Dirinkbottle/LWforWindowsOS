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
 * intentionally preserved; only red and blue are exchanged.
 *
 * Header-only on purpose: the nested Weston module can include this helper
 * without adding an outer-tree object file to its Meson target. */
static inline bool cw_weston_rgba_to_bgra_inplace(uint8_t *pixels, size_t byte_count) {
    size_t offset;

    if ((pixels == NULL && byte_count != 0U) ||
        (byte_count % CW_BGRA8888_BYTES_PER_PIXEL) != 0U) {
        return false;
    }
    for (offset = 0U; offset < byte_count; offset += CW_BGRA8888_BYTES_PER_PIXEL) {
        const uint8_t red = pixels[offset];
        pixels[offset] = pixels[offset + 2U];
        pixels[offset + 2U] = red;
    }
    return true;
}

#endif

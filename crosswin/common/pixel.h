#ifndef CROSSWIN_PIXEL_H
#define CROSSWIN_PIXEL_H

#include <stdint.h>

/* One top-down BGRA8888 pixel in a framebuffer byte stream. */
enum { CW_BGRA8888_BYTES_PER_PIXEL = 4U };

typedef struct {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
} CwBgra8888;

#endif

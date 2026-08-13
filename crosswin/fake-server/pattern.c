#include "pattern.h"

#include "../common/pixel.h"

#include <stddef.h>

static bool valid_frame(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride) {
    return pixels != NULL && width != 0U && height != 0U &&
           width <= 16384U && height <= 16384U &&
           (uint64_t)stride >= (uint64_t)width * CW_BGRA8888_BYTES_PER_PIXEL;
}

static void set_pixel(uint8_t *pixels, uint32_t stride, uint32_t x, uint32_t y,
                      uint8_t b, uint8_t g, uint8_t r) {
    size_t offset = (size_t)y * (size_t)stride + (size_t)x * CW_BGRA8888_BYTES_PER_PIXEL;

    pixels[offset] = b;
    pixels[offset + 1U] = g;
    pixels[offset + 2U] = r;
    pixels[offset + 3U] = 255U;
}

static bool in_corner(uint32_t x, uint32_t y, uint32_t width, uint32_t height, unsigned corner) {
    const uint32_t marker = 30U;

    switch (corner) {
    case 0U:
        return x < marker && y < marker;
    case 1U:
        return x + marker >= width && y < marker;
    case 2U:
        return x < marker && y + marker >= height;
    default:
        return x + marker >= width && y + marker >= height;
    }
}

bool cw_pattern_generate(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride) {
    uint32_t x;
    uint32_t y;
    uint32_t red_denominator;
    uint32_t green_denominator;

    if (!valid_frame(pixels, width, height, stride)) {
        return false;
    }
    red_denominator = width > 1U ? width - 1U : 1U;
    green_denominator = height > 1U ? height - 1U : 1U;
    for (y = 0U; y < height; ++y) {
        for (x = 0U; x < width; ++x) {
            uint8_t b = (uint8_t)(((x * 17U) ^ (y * 31U)) & 0xffU);
            uint8_t g = (uint8_t)((y * 255U) / green_denominator);
            uint8_t r = (uint8_t)((x * 255U) / red_denominator);
            bool major_grid = (x % 100U == 0U) || (y % 100U == 0U);
            bool minor_grid = (x % 25U == 0U) || (y % 25U == 0U);

            if (major_grid) {
                b = 255U;
                g = 255U;
                r = 255U;
            } else if (minor_grid) {
                b = 96U;
                g = 96U;
                r = 96U;
            }
            if (x < 3U || y < 3U || x + 3U >= width || y + 3U >= height) {
                b = 0U;
                g = 0U;
                r = 0U;
            }
            if (in_corner(x, y, width, height, 0U)) {
                b = 0U;
                g = 0U;
                r = 255U;
            } else if (in_corner(x, y, width, height, 1U)) {
                b = 0U;
                g = 255U;
                r = 0U;
            } else if (in_corner(x, y, width, height, 2U)) {
                b = 255U;
                g = 0U;
                r = 0U;
            } else if (in_corner(x, y, width, height, 3U)) {
                b = 0U;
                g = 255U;
                r = 255U;
            }
            if (x == width / 2U || y == height / 2U) {
                b = 255U;
                g = 0U;
                r = 255U;
            }
            set_pixel(pixels, stride, x, y, b, g, r);
        }
    }
    return true;
}

bool cw_pattern_draw_crosshair(
    uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    int32_t center_x,
    int32_t center_y) {
    int32_t offset;

    if (!valid_frame(pixels, width, height, stride)) {
        return false;
    }
    for (offset = -10; offset <= 10; ++offset) {
        int64_t x = (int64_t)center_x + offset;
        int64_t y = (int64_t)center_y + offset;

        if (x >= 0 && x < (int64_t)width && center_y >= 0 && center_y < (int32_t)height) {
            set_pixel(pixels, stride, (uint32_t)x, (uint32_t)center_y, 0U, 255U, 255U);
        }
        if (y >= 0 && y < (int64_t)height && center_x >= 0 && center_x < (int32_t)width) {
            set_pixel(pixels, stride, (uint32_t)center_x, (uint32_t)y, 0U, 255U, 255U);
        }
    }
    return true;
}

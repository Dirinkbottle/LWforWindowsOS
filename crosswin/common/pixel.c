#include "pixel.h"

bool cw_weston_rgba_to_bgra_inplace(uint8_t *pixels, size_t byte_count) {
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

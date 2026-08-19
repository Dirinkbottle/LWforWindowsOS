#include "../common/pixel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "pixel_test:%d: CHECK failed: %s\n", __LINE__, #expr); \
        return EXIT_FAILURE; \
    } \
} while (0)

int main(void) {
    uint8_t pixels[] = {
        0x11U, 0x22U, 0x33U, 0x44U,
        0x05U, 0x06U, 0x07U, 0x08U,
    };
    const uint8_t expected[] = {
        0x33U, 0x22U, 0x11U, 0x44U,
        0x07U, 0x06U, 0x05U, 0x08U,
    };

    CHECK(cw_weston_rgba_to_bgra_inplace(pixels, sizeof(pixels)));
    CHECK(memcmp(pixels, expected, sizeof(expected)) == 0);

    /* Critical defensive cases only: malformed lengths and a non-empty NULL
     * buffer must never be accepted by the readback boundary. */
    CHECK(!cw_weston_rgba_to_bgra_inplace(pixels, 3U));
    CHECK(!cw_weston_rgba_to_bgra_inplace(NULL, 4U));
    CHECK(cw_weston_rgba_to_bgra_inplace(NULL, 0U));

    puts("pixel_test: PASS");
    return EXIT_SUCCESS;
}

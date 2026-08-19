#ifndef CROSSWIN_STAGE7E_READBACK_H
#define CROSSWIN_STAGE7E_READBACK_H

#include "../common/pixel.h"
#include "../common/protocol.h"

#include <libweston/libweston.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Small header-only bridge for the nested weston/crosswin module.  The outer
 * repository intentionally owns the wire-format conversion, while Weston owns
 * importing and sampling wl_shm/dmabuf buffers through its active renderer. */
typedef struct {
    uint8_t *pixels;
    size_t capacity;
} CwWestonReadback;

static inline void cw_weston_readback_init(CwWestonReadback *readback) {
    if (readback != NULL) {
        readback->pixels = NULL;
        readback->capacity = 0U;
    }
}

static inline void cw_weston_readback_destroy(CwWestonReadback *readback) {
    if (readback != NULL) {
        free(readback->pixels);
        readback->pixels = NULL;
        readback->capacity = 0U;
    }
}

static inline bool cw_weston_readback_layout(int width, int height,
                                             uint32_t *stride_out,
                                             size_t *bytes_out) {
    uint64_t stride;
    uint64_t bytes;

    if (stride_out == NULL || bytes_out == NULL || width <= 0 || height <= 0 ||
        (uint32_t)width > CW_MAX_SURFACE_DIMENSION ||
        (uint32_t)height > CW_MAX_SURFACE_DIMENSION) {
        return false;
    }
    stride = (uint64_t)(uint32_t)width * CW_BGRA8888_BYTES_PER_PIXEL;
    bytes = stride * (uint32_t)height;
    if (stride > UINT32_MAX || bytes > (uint64_t)CW_MAX_PAYLOAD - 32U ||
        bytes > SIZE_MAX) {
        return false;
    }
    *stride_out = (uint32_t)stride;
    *bytes_out = (size_t)bytes;
    return true;
}

/* Read one surface-local rectangle from the renderer into tightly packed
 * premultiplied BGRA8888.  GL renderer readback is PIXMAN_a8b8g8r8 (RGBA in
 * byte-address order on little-endian), therefore only R/B are swapped.
 *
 * IMPORTANT: call this only after Weston has attached the current buffer to
 * the renderer.  CrossWin marks dmabuf commits pending, schedules repaint, and
 * performs this readback from the remote output frame callback. */
static inline bool cw_weston_readback_surface(CwWestonReadback *readback,
                                              struct weston_surface *surface,
                                              int src_x, int src_y,
                                              int width, int height,
                                              const uint8_t **pixels_out,
                                              uint32_t *stride_out,
                                              size_t *bytes_out) {
    uint32_t stride;
    size_t bytes;
    uint8_t *replacement;

    if (readback == NULL || surface == NULL || pixels_out == NULL ||
        stride_out == NULL || bytes_out == NULL || src_x < 0 || src_y < 0 ||
        !cw_weston_readback_layout(width, height, &stride, &bytes)) {
        return false;
    }
    if (readback->capacity < bytes) {
        replacement = (uint8_t *)realloc(readback->pixels, bytes);
        if (replacement == NULL) {
            return false;
        }
        readback->pixels = replacement;
        readback->capacity = bytes;
    }
    if (weston_surface_copy_content(surface, readback->pixels, bytes,
                                    src_x, src_y, width, height) < 0 ||
        !cw_weston_rgba_to_bgra_inplace(readback->pixels, bytes)) {
        return false;
    }
    *pixels_out = readback->pixels;
    *stride_out = stride;
    *bytes_out = bytes;
    return true;
}

#endif

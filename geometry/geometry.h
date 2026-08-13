#ifndef LWKVM_GEOMETRY_H
#define LWKVM_GEOMETRY_H

/*
 * Pure geometry contracts for the Linux Wayland <-> Windows KVM desktop.
 *
 * Every Rect is expressed as a half-open rectangle:
 *     [x, x + w) x [y, y + h)
 * x and y are signed global logical coordinates unless a function's name or
 * parameter documentation explicitly says output-local, surface-local, or
 * physical.  Width and height must be non-negative.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t x;
    int32_t y;
} Point;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} Rect;

/* A positive physical-pixels-per-logical-unit ratio: numerator / denominator. */
typedef struct {
    uint32_t numerator;
    uint32_t denominator;
} Scale;

typedef struct {
    Rect logical; /* Global logical desktop coordinates. */
    Scale scale;
} OutputGeometry;

typedef struct {
    Rect logical; /* Global logical desktop coordinates. */
} WindowGeometry;

/* A rectangle is valid exactly when both dimensions are non-negative. */
bool rect_is_valid(Rect rect);

/* True for a valid rectangle with zero width or zero height. */
bool rect_is_empty(Rect rect);

/* Wide half-open bounds.  These use int64_t so x + w cannot overflow. */
int64_t rect_right(Rect rect);
int64_t rect_bottom(Rect rect);

/* Tests membership using the half-open convention.  Invalid rectangles contain nothing. */
bool rect_contains_point(Rect rect, Point point);

/*
 * Returns the non-empty intersection of two valid rectangles.  On false
 * (invalid input or no non-empty overlap), *out is reset to {0, 0, 0, 0}
 * when out is non-NULL.  out itself is required.
 */
bool rect_intersect(Rect a, Rect b, Rect *out);

/*
 * Computes the one visible part of a global window on a global output.
 *
 * global_intersection is in global logical coordinates.
 * surface_source_rect is in surface-local logical coordinates: the area to
 * sample from the original, full window surface.
 * output_local_rect is in output-local logical coordinates: where that area
 * is presented on this output.
 *
 * All output parameters are required.  On false they are reset to empty
 * rectangles.  False means invalid input or no non-empty overlap.
 */
bool window_region_on_output(
    Rect window_global,
    Rect output_global,
    Rect *global_intersection,
    Rect *surface_source_rect,
    Rect *output_local_rect);

/*
 * Point-space conversions.  These require a point inside the relevant
 * half-open rectangle.  They return false for invalid geometry, an
 * out-of-bounds point, a NULL out parameter, or a result not representable by
 * Point's int32_t coordinates.  On false, *out is reset to {0, 0} when out is
 * non-NULL.
 */
bool global_to_output_local(Rect output_global, Point global, Point *out);
bool global_to_surface_local(Rect window_global, Point global, Point *out);
bool output_local_to_global(Rect output_global, Point output_local, Point *out);
bool surface_local_to_global(Rect window_global, Point surface_local, Point *out);

/*
 * Converts an output-local point to global logical coordinates without
 * requiring that the point is currently inside the output.  Capture motions
 * may legitimately be outside an HWND/output; callers that require normal
 * hit-testing should use output_local_to_global() instead.
 */
bool output_local_point_to_global(
    Rect output_global,
    Point output_local,
    Point *out);

/*
 * Converts a pointer received in output-local logical coordinates to the
 * original window surface.  A pointer is accepted only when it lies inside
 * the portion of window_global visible on remote_output_global.  This is the
 * input-routing counterpart to window_region_on_output().
 */
bool remote_pointer_to_surface_local(
    Rect window_global,
    Rect remote_output_global,
    Point remote_output_local,
    Point *surface_local);

/*
 * Converts a pointer in the presented remote fragment's local coordinate
 * system (for example, a Windows HWND client coordinate) to the original
 * surface.  (0, 0) is the top-left of the visible fragment, not the top-left
 * of the remote output.  This directly maps a visible-fragment pointer of
 * {133, 211} for a source rect starting at {220, 0} to {353, 211}.
 */
bool remote_window_pointer_to_surface_local(
    Rect window_global,
    Rect remote_output_global,
    Point remote_window_local,
    Point *surface_local);

/*
 * Maps a pointer in a presented fragment/HWND client area to the original
 * surface.  source_rect is surface-local and must have non-negative origin
 * and dimensions.  fragment_local must lie inside [0, source.w) x
 * [0, source.h); outside/captured coordinates are rejected rather than
 * clamped.  This is presentation-history-safe because it needs no current
 * global window position.
 */
bool presented_fragment_to_surface_local(
    Rect source_rect,
    Point fragment_local,
    Point *surface_local);

/* A scale is valid only when both terms are non-zero. */
bool scale_is_valid(Scale scale);

/*
 * Converts a logical point to a physical pixel point using floor(value *
 * numerator / denominator) independently per axis.  Floor gives a stable
 * physical grid boundary, including for negative global coordinates.
 */
bool logical_point_to_physical(Point logical, Scale scale, Point *physical);

/*
 * Converts a logical half-open rectangle to physical pixels.  Left/top edges
 * use floor; right/bottom use ceil.  Thus the physical rectangle never loses
 * coverage of any logical area at fractional scale.  False is returned when
 * the scale or input is invalid or the physical Rect cannot be represented.
 */
bool logical_rect_to_physical(Rect logical, Scale scale, Rect *physical);

#endif

#include "geometry.h"

#include <limits.h>
#include <stddef.h>

static void clear_rect(Rect *rect) {
    if (rect != NULL) {
        *rect = (Rect){0, 0, 0, 0};
    }
}

static void clear_point(Point *point) {
    if (point != NULL) {
        *point = (Point){0, 0};
    }
}

static bool fits_i32(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

static bool checked_add_i64(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool checked_multiply_i64_positive(int64_t value, int64_t positive, int64_t *out) {
    if (value > 0 && value > INT64_MAX / positive) {
        return false;
    }
    if (value < 0 && value < INT64_MIN / positive) {
        return false;
    }
    *out = value * positive;
    return true;
}

bool rect_is_valid(Rect rect) {
    return rect.w >= 0 && rect.h >= 0;
}

bool rect_is_empty(Rect rect) {
    return rect_is_valid(rect) && (rect.w == 0 || rect.h == 0);
}

int64_t rect_right(Rect rect) {
    return (int64_t)rect.x + (int64_t)rect.w;
}

int64_t rect_bottom(Rect rect) {
    return (int64_t)rect.y + (int64_t)rect.h;
}

bool rect_contains_point(Rect rect, Point point) {
    return rect_is_valid(rect) && !rect_is_empty(rect) &&
           (int64_t)point.x >= (int64_t)rect.x &&
           (int64_t)point.x < rect_right(rect) &&
           (int64_t)point.y >= (int64_t)rect.y &&
           (int64_t)point.y < rect_bottom(rect);
}

bool rect_intersect(Rect a, Rect b, Rect *out) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    clear_rect(out);
    if (out == NULL || !rect_is_valid(a) || !rect_is_valid(b) ||
        rect_is_empty(a) || rect_is_empty(b)) {
        return false;
    }

    left = (int64_t)a.x > (int64_t)b.x ? (int64_t)a.x : (int64_t)b.x;
    top = (int64_t)a.y > (int64_t)b.y ? (int64_t)a.y : (int64_t)b.y;
    right = rect_right(a) < rect_right(b) ? rect_right(a) : rect_right(b);
    bottom = rect_bottom(a) < rect_bottom(b) ? rect_bottom(a) : rect_bottom(b);

    if (left >= right || top >= bottom) {
        return false;
    }

    /* The intersection cannot be wider or taller than either valid input. */
    *out = (Rect){
        (int32_t)left,
        (int32_t)top,
        (int32_t)(right - left),
        (int32_t)(bottom - top),
    };
    return true;
}

bool window_region_on_output(
    Rect window_global,
    Rect output_global,
    Rect *global_intersection,
    Rect *surface_source_rect,
    Rect *output_local_rect) {
    Rect intersection;
    int64_t source_x;
    int64_t source_y;
    int64_t output_x;
    int64_t output_y;

    clear_rect(global_intersection);
    clear_rect(surface_source_rect);
    clear_rect(output_local_rect);
    if (global_intersection == NULL || surface_source_rect == NULL ||
        output_local_rect == NULL ||
        !rect_intersect(window_global, output_global, &intersection)) {
        return false;
    }

    source_x = (int64_t)intersection.x - (int64_t)window_global.x;
    source_y = (int64_t)intersection.y - (int64_t)window_global.y;
    output_x = (int64_t)intersection.x - (int64_t)output_global.x;
    output_y = (int64_t)intersection.y - (int64_t)output_global.y;

    *global_intersection = intersection;
    *surface_source_rect = (Rect){
        (int32_t)source_x,
        (int32_t)source_y,
        intersection.w,
        intersection.h,
    };
    *output_local_rect = (Rect){
        (int32_t)output_x,
        (int32_t)output_y,
        intersection.w,
        intersection.h,
    };
    return true;
}

static bool global_to_local(Rect global_rect, Point global, Point *local) {
    int64_t local_x;
    int64_t local_y;

    clear_point(local);
    if (local == NULL || !rect_contains_point(global_rect, global)) {
        return false;
    }

    local_x = (int64_t)global.x - (int64_t)global_rect.x;
    local_y = (int64_t)global.y - (int64_t)global_rect.y;
    *local = (Point){(int32_t)local_x, (int32_t)local_y};
    return true;
}

static bool local_to_global(Rect global_rect, Point local, Point *global) {
    Rect local_bounds;
    int64_t global_x;
    int64_t global_y;

    clear_point(global);
    if (global == NULL || !rect_is_valid(global_rect)) {
        return false;
    }

    local_bounds = (Rect){0, 0, global_rect.w, global_rect.h};
    if (!rect_contains_point(local_bounds, local)) {
        return false;
    }

    global_x = (int64_t)global_rect.x + (int64_t)local.x;
    global_y = (int64_t)global_rect.y + (int64_t)local.y;
    if (!fits_i32(global_x) || !fits_i32(global_y)) {
        return false;
    }

    *global = (Point){(int32_t)global_x, (int32_t)global_y};
    return true;
}

bool global_to_output_local(Rect output_global, Point global, Point *out) {
    return global_to_local(output_global, global, out);
}

bool global_to_surface_local(Rect window_global, Point global, Point *out) {
    return global_to_local(window_global, global, out);
}

bool output_local_to_global(Rect output_global, Point output_local, Point *out) {
    return local_to_global(output_global, output_local, out);
}

bool output_local_point_to_global(
    Rect output_global,
    Point output_local,
    Point *out) {
    int64_t global_x;
    int64_t global_y;

    clear_point(out);
    if (out == NULL || !rect_is_valid(output_global)) {
        return false;
    }
    global_x = (int64_t)output_global.x + (int64_t)output_local.x;
    global_y = (int64_t)output_global.y + (int64_t)output_local.y;
    if (!fits_i32(global_x) || !fits_i32(global_y)) {
        return false;
    }
    *out = (Point){(int32_t)global_x, (int32_t)global_y};
    return true;
}

bool surface_local_to_global(Rect window_global, Point surface_local, Point *out) {
    return local_to_global(window_global, surface_local, out);
}

bool remote_pointer_to_surface_local(
    Rect window_global,
    Rect remote_output_global,
    Point remote_output_local,
    Point *surface_local) {
    Rect intersection;
    Rect source;
    Rect visible_output_local;
    Point global;

    clear_point(surface_local);
    if (surface_local == NULL ||
        !window_region_on_output(window_global, remote_output_global,
                                 &intersection, &source, &visible_output_local) ||
        !rect_contains_point(visible_output_local, remote_output_local) ||
        !output_local_to_global(remote_output_global, remote_output_local, &global)) {
        return false;
    }

    return global_to_surface_local(window_global, global, surface_local);
}

bool remote_window_pointer_to_surface_local(
    Rect window_global,
    Rect remote_output_global,
    Point remote_window_local,
    Point *surface_local) {
    Rect intersection;
    Rect source;
    Rect visible_output_local;
    Rect visible_window_local;
    int64_t surface_x;
    int64_t surface_y;

    clear_point(surface_local);
    if (surface_local == NULL ||
        !window_region_on_output(window_global, remote_output_global,
                                 &intersection, &source, &visible_output_local)) {
        return false;
    }

    visible_window_local = (Rect){0, 0, source.w, source.h};
    if (!rect_contains_point(visible_window_local, remote_window_local)) {
        return false;
    }

    surface_x = (int64_t)source.x + (int64_t)remote_window_local.x;
    surface_y = (int64_t)source.y + (int64_t)remote_window_local.y;
    *surface_local = (Point){(int32_t)surface_x, (int32_t)surface_y};
    return true;
}

bool presented_fragment_to_surface_local(
    Rect source_rect,
    Point fragment_local,
    Point *surface_local) {
    Rect fragment_bounds;
    int64_t surface_x;
    int64_t surface_y;

    clear_point(surface_local);
    if (surface_local == NULL || !rect_is_valid(source_rect) ||
        source_rect.x < 0 || source_rect.y < 0) {
        return false;
    }
    fragment_bounds = (Rect){0, 0, source_rect.w, source_rect.h};
    if (!rect_contains_point(fragment_bounds, fragment_local)) {
        return false;
    }
    surface_x = (int64_t)source_rect.x + (int64_t)fragment_local.x;
    surface_y = (int64_t)source_rect.y + (int64_t)fragment_local.y;
    if (!fits_i32(surface_x) || !fits_i32(surface_y)) {
        return false;
    }
    *surface_local = (Point){(int32_t)surface_x, (int32_t)surface_y};
    return true;
}

bool scale_is_valid(Scale scale) {
    return scale.numerator != 0U && scale.denominator != 0U;
}

/*
 * Computes floor(logical * scale), and reports whether the exact result was
 * fractional.  Dividing first keeps the only direct multiplication bounded by
 * two uint32_t values, avoiding signed-overflow even for extreme scales.
 */
static bool scaled_floor(
    int64_t logical,
    Scale scale,
    int64_t *result,
    bool *was_fractional) {
    int64_t quotient;
    int64_t remainder;
    uint64_t fractional_numerator;
    int64_t whole;
    int64_t fraction;

    if (!scale_is_valid(scale) || result == NULL) {
        return false;
    }

    quotient = logical / (int64_t)scale.denominator;
    remainder = logical % (int64_t)scale.denominator;
    if (remainder < 0) {
        --quotient;
        remainder += (int64_t)scale.denominator;
    }

    if (!checked_multiply_i64_positive(
            quotient, (int64_t)scale.numerator, &whole)) {
        return false;
    }

    fractional_numerator = (uint64_t)remainder * (uint64_t)scale.numerator;
    fraction = (int64_t)(fractional_numerator / (uint64_t)scale.denominator);
    if (!checked_add_i64(whole, fraction, result)) {
        return false;
    }

    if (was_fractional != NULL) {
        *was_fractional =
            (fractional_numerator % (uint64_t)scale.denominator) != 0U;
    }
    return true;
}

static bool scaled_ceil(int64_t logical, Scale scale, int64_t *result) {
    bool was_fractional;

    if (!scaled_floor(logical, scale, result, &was_fractional)) {
        return false;
    }
    if (was_fractional && !checked_add_i64(*result, 1, result)) {
        return false;
    }
    return true;
}

bool logical_point_to_physical(Point logical, Scale scale, Point *physical) {
    int64_t physical_x;
    int64_t physical_y;

    clear_point(physical);
    if (physical == NULL ||
        !scaled_floor((int64_t)logical.x, scale, &physical_x, NULL) ||
        !scaled_floor((int64_t)logical.y, scale, &physical_y, NULL) ||
        !fits_i32(physical_x) || !fits_i32(physical_y)) {
        return false;
    }

    *physical = (Point){(int32_t)physical_x, (int32_t)physical_y};
    return true;
}

bool logical_rect_to_physical(Rect logical, Scale scale, Rect *physical) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    int64_t width;
    int64_t height;

    clear_rect(physical);
    if (physical == NULL || !rect_is_valid(logical) || !scale_is_valid(scale) ||
        !scaled_floor((int64_t)logical.x, scale, &left, NULL) ||
        !scaled_floor((int64_t)logical.y, scale, &top, NULL) ||
        !scaled_ceil(rect_right(logical), scale, &right) ||
        !scaled_ceil(rect_bottom(logical), scale, &bottom)) {
        return false;
    }

    width = right - left;
    height = bottom - top;
    if (!fits_i32(left) || !fits_i32(top) || width < 0 || height < 0 ||
        width > INT32_MAX || height > INT32_MAX) {
        return false;
    }

    *physical = (Rect){
        (int32_t)left,
        (int32_t)top,
        (int32_t)width,
        (int32_t)height,
    };
    return true;
}

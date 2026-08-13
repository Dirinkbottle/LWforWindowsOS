#include "geometry.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned long cases;

static void print_rect(const char *label, Rect rect) {
    fprintf(stderr, "%s: x=%" PRId32 " y=%" PRId32 " w=%" PRId32 " h=%" PRId32 "\n",
            label, rect.x, rect.y, rect.w, rect.h);
}

static void print_point(const char *label, Point point) {
    fprintf(stderr, "%s: x=%" PRId32 " y=%" PRId32 "\n", label, point.x, point.y);
}

static void fail(const char *function, int line, const char *condition) {
    fprintf(stderr, "FAIL: %s:%d: %s\n", function, line, condition);
    exit(EXIT_FAILURE);
}

#define CHECK(condition)                  \
    do {                                  \
        ++cases;                          \
        if (!(condition)) {               \
            fail(__func__, __LINE__, #condition); \
        }                                 \
    } while (0)

static bool rect_equal(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static bool point_equal(Point a, Point b) {
    return a.x == b.x && a.y == b.y;
}

static void expect_rect(const char *name, Rect actual, Rect expected) {
    ++cases;
    if (!rect_equal(actual, expected)) {
        fprintf(stderr, "FAIL: %s\n", name);
        print_rect("expected", expected);
        print_rect("actual", actual);
        exit(EXIT_FAILURE);
    }
}

static void expect_point(const char *name, Point actual, Point expected) {
    ++cases;
    if (!point_equal(actual, expected)) {
        fprintf(stderr, "FAIL: %s\n", name);
        print_point("expected", expected);
        print_point("actual", actual);
        exit(EXIT_FAILURE);
    }
}

static void expect_region(
    const char *name,
    Rect window,
    Rect output,
    Rect expected_global,
    Rect expected_source,
    Rect expected_output_local) {
    Rect global;
    Rect source;
    Rect output_local;

    if (!window_region_on_output(window, output, &global, &source, &output_local)) {
        fprintf(stderr, "FAIL: %s: expected visible region\n", name);
        print_rect("window", window);
        print_rect("output", output);
        exit(EXIT_FAILURE);
    }
    expect_rect("window region global", global, expected_global);
    expect_rect("window region source", source, expected_source);
    expect_rect("window region output-local", output_local, expected_output_local);
}

static void test_rectangle_basics(void) {
    Rect result = {7, 8, 9, 10};

    CHECK(rect_is_valid((Rect){0, 0, 0, 0}));
    CHECK(rect_is_valid((Rect){INT32_MIN, INT32_MAX, 1, 1}));
    CHECK(!rect_is_valid((Rect){0, 0, -1, 1}));
    CHECK(!rect_is_valid((Rect){0, 0, 1, -1}));
    CHECK(rect_is_empty((Rect){0, 0, 0, 1}));
    CHECK(rect_is_empty((Rect){0, 0, 1, 0}));
    CHECK(!rect_is_empty((Rect){0, 0, -1, 0}));
    CHECK(rect_right((Rect){INT32_MAX, 0, INT32_MAX, 1}) == 4294967294LL);
    CHECK(rect_bottom((Rect){0, INT32_MAX, 1, INT32_MAX}) == 4294967294LL);
    CHECK(rect_contains_point((Rect){0, 0, 100, 100}, (Point){0, 0}));
    CHECK(rect_contains_point((Rect){0, 0, 100, 100}, (Point){99, 99}));
    CHECK(!rect_contains_point((Rect){0, 0, 100, 100}, (Point){100, 99}));
    CHECK(!rect_contains_point((Rect){0, 0, 100, 100}, (Point){99, 100}));

    CHECK(rect_intersect((Rect){0, 0, 100, 100}, (Rect){20, 30, 10, 10}, &result));
    expect_rect("contained intersection", result, (Rect){20, 30, 10, 10});
    CHECK(rect_intersect((Rect){0, 0, 100, 100}, (Rect){50, -5, 100, 10}, &result));
    expect_rect("partial intersection", result, (Rect){50, 0, 50, 5});
    CHECK(!rect_intersect((Rect){0, 0, 10, 10}, (Rect){20, 0, 10, 10}, &result));
    expect_rect("disjoint clears result", result, (Rect){0, 0, 0, 0});
    CHECK(!rect_intersect((Rect){0, 0, 100, 100}, (Rect){100, 0, 100, 100}, &result));
    CHECK(!rect_intersect((Rect){0, 0, 100, 100}, (Rect){100, 100, 1, 1}, &result));
    CHECK(!rect_intersect((Rect){0, 0, 0, 100}, (Rect){0, 0, 100, 100}, &result));
    CHECK(rect_intersect((Rect){-20, 5, 80, 40}, (Rect){-20, 5, 80, 40}, &result));
    expect_rect("identical intersection", result, (Rect){-20, 5, 80, 40});
    CHECK(!rect_intersect((Rect){0, 0, -1, 1}, (Rect){0, 0, 1, 1}, &result));
    CHECK(!rect_intersect((Rect){0, 0, 1, 1}, (Rect){0, 0, 1, 1}, NULL));
}

static void test_window_output_regions(void) {
    const Rect linux_output = {0, 0, 1920, 1080};
    const Rect right_remote = {1920, 0, 2560, 1440};
    Rect global = {1, 1, 1, 1};
    Rect source = {1, 1, 1, 1};
    Rect output_local = {1, 1, 1, 1};

    expect_region("canonical right crossing", (Rect){1700, 300, 800, 600}, right_remote,
                  (Rect){1920, 300, 580, 600}, (Rect){220, 0, 580, 600},
                  (Rect){0, 300, 580, 600});
    expect_region("fully linux", (Rect){100, 200, 300, 400}, linux_output,
                  (Rect){100, 200, 300, 400}, (Rect){0, 0, 300, 400},
                  (Rect){100, 200, 300, 400});
    expect_region("fully remote", (Rect){2000, 200, 300, 400}, right_remote,
                  (Rect){2000, 200, 300, 400}, (Rect){0, 0, 300, 400},
                  (Rect){80, 200, 300, 400});
    expect_region("ten percent right crossing", (Rect){1830, 10, 100, 10}, right_remote,
                  (Rect){1920, 10, 10, 10}, (Rect){90, 0, 10, 10},
                  (Rect){0, 10, 10, 10});
    expect_region("half right crossing", (Rect){1870, 10, 100, 10}, right_remote,
                  (Rect){1920, 10, 50, 10}, (Rect){50, 0, 50, 10},
                  (Rect){0, 10, 50, 10});
    expect_region("one pixel right crossing", (Rect){1919, 10, 2, 10}, right_remote,
                  (Rect){1920, 10, 1, 10}, (Rect){1, 0, 1, 10},
                  (Rect){0, 10, 1, 10});
    expect_region("window larger than both outputs", (Rect){-200, 10, 5000, 100}, right_remote,
                  (Rect){1920, 10, 2560, 100}, (Rect){2120, 0, 2560, 100},
                  (Rect){0, 10, 2560, 100});

    expect_region("left remote", (Rect){-100, 100, 200, 50}, (Rect){-2560, 0, 2560, 1440},
                  (Rect){-100, 100, 100, 50}, (Rect){0, 0, 100, 50},
                  (Rect){2460, 100, 100, 50});
    expect_region("upper remote", (Rect){400, -100, 300, 200}, (Rect){0, -1440, 2560, 1440},
                  (Rect){400, -100, 300, 100}, (Rect){0, 0, 300, 100},
                  (Rect){400, 1340, 300, 100});
    expect_region("lower remote", (Rect){400, 1000, 300, 200}, (Rect){0, 1080, 2560, 1440},
                  (Rect){400, 1080, 300, 120}, (Rect){0, 80, 300, 120},
                  (Rect){400, 0, 300, 120});
    CHECK(!window_region_on_output((Rect){1900, 1000, 20, 80},
                                   (Rect){1920, 1080, 2560, 1440},
                                   &global, &source, &output_local));
    expect_rect("corner no intersection global", global, (Rect){0, 0, 0, 0});
    expect_rect("corner no intersection source", source, (Rect){0, 0, 0, 0});
    expect_rect("corner no intersection local", output_local, (Rect){0, 0, 0, 0});
    expect_region("corner actual overlap", (Rect){1900, 1000, 50, 120},
                  (Rect){1920, 1080, 2560, 1440},
                  (Rect){1920, 1080, 30, 40}, (Rect){20, 80, 30, 40},
                  (Rect){0, 0, 30, 40});
}

static void test_coordinate_transforms(void) {
    const Rect window = {1700, 300, 800, 600};
    const Rect remote = {1920, 0, 2560, 1440};
    Point point;

    CHECK(global_to_output_local(remote, (Point){2000, 10}, &point));
    expect_point("global to output-local", point, (Point){80, 10});
    CHECK(global_to_surface_local(window, (Point){2000, 310}, &point));
    expect_point("global to surface-local", point, (Point){300, 10});
    CHECK(output_local_to_global(remote, (Point){80, 10}, &point));
    expect_point("output-local to global", point, (Point){2000, 10});
    CHECK(output_local_point_to_global(remote, (Point){-12, 10}, &point));
    expect_point("unbounded output-local to global", point, (Point){1908, 10});
    CHECK(surface_local_to_global(window, (Point){300, 10}, &point));
    expect_point("surface-local to global", point, (Point){2000, 310});
    CHECK(!global_to_output_local(remote, (Point){1920, 1440}, &point));
    CHECK(!global_to_surface_local(window, (Point){2500, 300}, &point));
    CHECK(!output_local_to_global(remote, (Point){2560, 0}, &point));
    CHECK(!surface_local_to_global(window, (Point){-1, 0}, &point));
    CHECK(!global_to_output_local((Rect){0, 0, -1, 1}, (Point){0, 0}, &point));
    CHECK(presented_fragment_to_surface_local((Rect){220, 100, 580, 400},
                                              (Point){133, 211}, &point));
    expect_point("presentation fragment to surface", point, (Point){353, 311});
    CHECK(!presented_fragment_to_surface_local((Rect){220, 100, 580, 400},
                                               (Point){-10, 100}, &point));
    CHECK(!presented_fragment_to_surface_local((Rect){220, 100, 580, 400},
                                               (Point){580, 100}, &point));
    CHECK(!presented_fragment_to_surface_local((Rect){220, 100, 580, 400},
                                               (Point){100, -20}, &point));
}

static void expect_pointer(
    const char *name,
    Rect window,
    Rect remote,
    Point remote_local,
    Point expected_surface) {
    Point actual;

    if (!remote_pointer_to_surface_local(window, remote, remote_local, &actual)) {
        fprintf(stderr, "FAIL: %s: pointer unexpectedly rejected\n", name);
        print_rect("window", window);
        print_rect("remote output", remote);
        print_point("remote output-local pointer", remote_local);
        exit(EXIT_FAILURE);
    }
    expect_point(name, actual, expected_surface);
}

static void expect_window_pointer(
    const char *name,
    Rect window,
    Rect remote,
    Point remote_window_local,
    Point expected_surface) {
    Point actual;

    if (!remote_window_pointer_to_surface_local(window, remote, remote_window_local, &actual)) {
        fprintf(stderr, "FAIL: %s: client pointer unexpectedly rejected\n", name);
        print_rect("window", window);
        print_rect("remote output", remote);
        print_point("remote window-local pointer", remote_window_local);
        exit(EXIT_FAILURE);
    }
    expect_point(name, actual, expected_surface);
}

static void test_remote_pointer_conversion(void) {
    Point result;

    expect_window_pointer("canonical HWND pointer", (Rect){1700, 300, 800, 600},
                          (Rect){1920, 0, 2560, 1440}, (Point){133, 211},
                          (Point){353, 211});
    expect_pointer("canonical output-local pointer", (Rect){1700, 300, 800, 600},
                   (Rect){1920, 0, 2560, 1440}, (Point){133, 511},
                   (Point){353, 211});
    expect_window_pointer("fully remote HWND pointer", (Rect){2000, 0, 100, 100},
                          (Rect){1920, 0, 2560, 1440}, (Point){50, 20},
                   (Point){50, 20});
    expect_window_pointer("HWND pointer on left crop", (Rect){-100, 10, 300, 50},
                          (Rect){0, 0, 1920, 1080}, (Point){15, 10},
                   (Point){115, 10});
    expect_window_pointer("HWND pointer on right crop", (Rect){1800, 10, 300, 50},
                          (Rect){1920, 0, 2560, 1440}, (Point){150, 10},
                   (Point){270, 10});
    expect_window_pointer("negative global HWND pointer", (Rect){-100, 10, 300, 50},
                          (Rect){-2560, 0, 2560, 1440}, (Point){10, 10},
                   (Point){10, 10});
    expect_window_pointer("upper remote HWND pointer", (Rect){400, -100, 300, 200},
                          (Rect){0, -1440, 2560, 1440}, (Point){50, 10},
                   (Point){50, 10});
    expect_window_pointer("lower remote HWND pointer", (Rect){400, 1000, 300, 200},
                          (Rect){0, 1080, 2560, 1440}, (Point){50, 30},
                   (Point){50, 110});
    CHECK(!remote_pointer_to_surface_local((Rect){1700, 300, 800, 600},
                                           (Rect){1920, 0, 2560, 1440},
                                           (Point){700, 400}, &result));
    CHECK(!remote_pointer_to_surface_local((Rect){1700, 300, 800, 600},
                                           (Rect){1920, 0, 2560, 1440},
                                           (Point){133, 511}, NULL));
    CHECK(!remote_window_pointer_to_surface_local((Rect){1700, 300, 800, 600},
                                                  (Rect){1920, 0, 2560, 1440},
                                                  (Point){580, 0}, &result));
    CHECK(!remote_window_pointer_to_surface_local((Rect){1700, 300, 800, 600},
                                                  (Rect){1920, 0, 2560, 1440},
                                                  (Point){133, 211}, NULL));
}

static void test_scale_conversions(void) {
    Point point;
    Rect rect;

    CHECK(scale_is_valid((Scale){1, 1}));
    CHECK(scale_is_valid((Scale){5, 4}));
    CHECK(scale_is_valid((Scale){3, 2}));
    CHECK(scale_is_valid((Scale){2, 1}));
    CHECK(!scale_is_valid((Scale){0, 1}));
    CHECK(!scale_is_valid((Scale){1, 0}));
    CHECK(logical_point_to_physical((Point){4, 4}, (Scale){1, 1}, &point));
    expect_point("scale one point", point, (Point){4, 4});
    CHECK(logical_point_to_physical((Point){4, 4}, (Scale){5, 4}, &point));
    expect_point("scale five quarters point", point, (Point){5, 5});
    CHECK(logical_point_to_physical((Point){4, 4}, (Scale){3, 2}, &point));
    expect_point("scale three halves point", point, (Point){6, 6});
    CHECK(logical_point_to_physical((Point){4, 4}, (Scale){2, 1}, &point));
    expect_point("scale two point", point, (Point){8, 8});
    CHECK(logical_point_to_physical((Point){1, 1}, (Scale){3, 2}, &point));
    expect_point("point floor policy", point, (Point){1, 1});
    CHECK(logical_point_to_physical((Point){-1, -1}, (Scale){3, 2}, &point));
    expect_point("negative point floor policy", point, (Point){-2, -2});

    CHECK(logical_rect_to_physical((Rect){1, 1, 1, 1}, (Scale){1, 1}, &rect));
    expect_rect("scale one rect", rect, (Rect){1, 1, 1, 1});
    CHECK(logical_rect_to_physical((Rect){1, 1, 1, 1}, (Scale){5, 4}, &rect));
    expect_rect("five quarters preserves rect coverage", rect, (Rect){1, 1, 2, 2});
    CHECK(logical_rect_to_physical((Rect){1, 1, 1, 1}, (Scale){3, 2}, &rect));
    expect_rect("three halves preserves rect coverage", rect, (Rect){1, 1, 2, 2});
    CHECK(logical_rect_to_physical((Rect){1, 1, 1, 1}, (Scale){2, 1}, &rect));
    expect_rect("scale two rect", rect, (Rect){2, 2, 2, 2});
    CHECK(logical_rect_to_physical((Rect){-1, -1, 1, 1}, (Scale){3, 2}, &rect));
    expect_rect("negative rect coverage", rect, (Rect){-2, -2, 2, 2});
    CHECK(!logical_point_to_physical((Point){1, 1}, (Scale){1, 0}, &point));
    CHECK(!logical_rect_to_physical((Rect){0, 0, -1, 1}, (Scale){1, 1}, &rect));
    CHECK(!logical_point_to_physical((Point){INT32_MAX, 0}, (Scale){2, 1}, &point));
    CHECK(!logical_rect_to_physical((Rect){INT32_MAX, 0, 1, 1}, (Scale){2, 1}, &rect));
}

static uint32_t next_random(uint32_t *state) {
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static bool rect_is_subset(Rect inner, Rect outer) {
    return (int64_t)inner.x >= (int64_t)outer.x &&
           (int64_t)inner.y >= (int64_t)outer.y &&
           rect_right(inner) <= rect_right(outer) &&
           rect_bottom(inner) <= rect_bottom(outer);
}

static void property_failure(const char *property, Rect a, Rect b, Rect got) {
    fprintf(stderr, "FAIL: property %s\n", property);
    print_rect("A", a);
    print_rect("B", b);
    print_rect("actual", got);
    exit(EXIT_FAILURE);
}

static void test_properties(void) {
    uint32_t seed = 0xC0A55EEDU;
    unsigned index;

    for (index = 0; index < 6000U; ++index) {
        Rect a = {
            (int32_t)(next_random(&seed) % 4096U) - 2048,
            (int32_t)(next_random(&seed) % 4096U) - 2048,
            (int32_t)(next_random(&seed) % 257U),
            (int32_t)(next_random(&seed) % 257U),
        };
        Rect b = {
            (int32_t)(next_random(&seed) % 4096U) - 2048,
            (int32_t)(next_random(&seed) % 4096U) - 2048,
            (int32_t)(next_random(&seed) % 257U),
            (int32_t)(next_random(&seed) % 257U),
        };
        Rect ab;
        Rect ba;
        bool has_ab = rect_intersect(a, b, &ab);
        bool has_ba = rect_intersect(b, a, &ba);

        ++cases;
        if (has_ab != has_ba || (has_ab && !rect_equal(ab, ba))) {
            property_failure("intersection symmetry", a, b, ab);
        }
        if (has_ab) {
            ++cases;
            if (!rect_is_subset(ab, a) || !rect_is_subset(ab, b) ||
                ab.w < 0 || ab.h < 0) {
                property_failure("intersection subset/nonnegative", a, b, ab);
            }
        }

        {
            Rect global;
            Rect source;
            Rect output_local;
            bool has_region = window_region_on_output(a, b, &global, &source, &output_local);

            ++cases;
            if (has_region != has_ab) {
                property_failure("region intersection existence", a, b, global);
            }
            if (has_region) {
                ++cases;
                if (source.w != output_local.w || source.h != output_local.h ||
                    source.x < 0 || source.y < 0 || output_local.x < 0 ||
                    output_local.y < 0 ||
                    (int64_t)source.x + (int64_t)source.w > (int64_t)a.w ||
                    (int64_t)source.y + (int64_t)source.h > (int64_t)a.h ||
                    (int64_t)output_local.x + (int64_t)output_local.w > (int64_t)b.w ||
                    (int64_t)output_local.y + (int64_t)output_local.h > (int64_t)b.h) {
                    property_failure("region local invariants", a, b, global);
                }
            }
        }
    }
}

int main(void) {
    test_rectangle_basics();
    test_window_output_regions();
    test_coordinate_transforms();
    test_remote_pointer_conversion();
    test_scale_conversions();
    test_properties();

    printf("geometry tests: PASS\n");
    printf("cases: %lu\n", cases);
    return EXIT_SUCCESS;
}

#include "../fake-server/pattern.h"
#include "../fake-server/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long cases;

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

static CwFakeServerSession make_session(void) {
    CwFakeServerSession session;

    if (!cw_fake_server_session_init(&session, 1U, (Rect){1920, 0, 2560, 1440},
                                     (Rect){1700, 300, 800, 600})) {
        fail(__func__, __LINE__, "session init");
    }
    return session;
}

static CwPresentationState record_canonical_presentation(CwFakeServerSession *session) {
    CwPresentationState presentation = {
        42U,
        {220, 0, 580, 600},
        {0, 300, 580, 600},
        true,
    };

    if (!cw_fake_server_record_presentation(session, &presentation)) {
        fail(__func__, __LINE__, "record canonical presentation");
    }
    return presentation;
}

static void test_presentation_history_and_mapping(void) {
    CwFakeServerSession session = make_session();
    CwServerInputResult result;
    CwPresentationState lookup;

    record_canonical_presentation(&session);
    CHECK(cw_fake_server_lookup_presentation(&session, 42U, &lookup));
    CHECK(lookup.source_rect.x == 220 && lookup.destination_rect.y == 300);
    CHECK(cw_fake_server_map_pointer(&session, 42U, (Point){133, 211}, (Point){133, 511},
                                     &result) == CW_SERVER_INPUT_OK);
    CHECK(result.surface_mapped);
    CHECK(result.surface.x == 353 && result.surface.y == 211);
    CHECK(result.global.x == 2053 && result.global.y == 511);
    CHECK(cw_fake_server_map_pointer(&session, 42U, (Point){0, 0}, (Point){0, 300},
                                     &result) == CW_SERVER_INPUT_OK);
    CHECK(result.surface.x == 220 && result.surface.y == 0);
    CHECK(cw_fake_server_map_pointer(&session, 42U, (Point){579, 0}, (Point){579, 300},
                                     &result) == CW_SERVER_INPUT_OK);
    CHECK(result.surface.x == 799 && result.surface.y == 0);
    CHECK(cw_fake_server_map_pointer(&session, 42U, (Point){0, 599}, (Point){0, 899},
                                     &result) == CW_SERVER_INPUT_OK);
    CHECK(result.surface.x == 220 && result.surface.y == 599);
    CHECK(cw_fake_server_map_pointer(&session, 42U, (Point){579, 599}, (Point){579, 899},
                                     &result) == CW_SERVER_INPUT_OK);
    CHECK(result.surface.x == 799 && result.surface.y == 599);
    CHECK(cw_fake_server_map_pointer(&session, 42U, (Point){290, 300}, (Point){290, 600},
                                     &result) == CW_SERVER_INPUT_OK);
    CHECK(result.surface.x == 510 && result.surface.y == 300);
    CHECK(cw_fake_server_map_pointer(&session, 42U, (Point){-10, 100}, (Point){-10, 400},
                                     &result) == CW_SERVER_INPUT_OUTSIDE_PRESENTED_FRAGMENT);
    CHECK(!result.surface_mapped && result.client.x == -10 && result.global.x == 1910);
    CHECK(cw_fake_server_map_pointer(&session, 15U, (Point){1, 1}, (Point){1, 301},
                                     &result) == CW_SERVER_INPUT_STALE_PRESENTATION);

    {
        const CwPresentationState full = {
            44U, {0, 0, 800, 600}, {300, 200, 800, 600}, true,
        };
        const CwPresentationState offset = {
            45U, {100, 0, 700, 600}, {0, 200, 700, 600}, true,
        };
        const CwPresentationState cropped = {
            46U, {220, 100, 580, 400}, {0, 300, 580, 400}, true,
        };

        CHECK(cw_fake_server_record_presentation(&session, &full));
        CHECK(cw_fake_server_map_pointer(&session, 44U, (Point){133, 211}, (Point){433, 411},
                                         &result) == CW_SERVER_INPUT_OK);
        CHECK(result.surface.x == 133 && result.surface.y == 211);
        CHECK(cw_fake_server_record_presentation(&session, &offset));
        CHECK(cw_fake_server_map_pointer(&session, 45U, (Point){133, 211}, (Point){133, 411},
                                         &result) == CW_SERVER_INPUT_OK);
        CHECK(result.surface.x == 233 && result.surface.y == 211);
        CHECK(cw_fake_server_record_presentation(&session, &cropped));
        CHECK(cw_fake_server_map_pointer(&session, 46U, (Point){133, 211}, (Point){133, 511},
                                         &result) == CW_SERVER_INPUT_OK);
        CHECK(result.surface.x == 353 && result.surface.y == 311);
        CHECK(cw_fake_server_map_pointer(&session, 46U, (Point){580, 100}, (Point){580, 400},
                                         &result) == CW_SERVER_INPUT_OUTSIDE_PRESENTED_FRAGMENT);
    }

    {
        uint64_t sequence;

        for (sequence = 100U; sequence < 165U; ++sequence) {
            CwPresentationState presentation = {
                sequence,
                {0, 0, 10, 10},
                {0, 0, 10, 10},
                true,
            };
            CHECK(cw_fake_server_record_presentation(&session, &presentation));
        }
    }
    CHECK(!cw_fake_server_lookup_presentation(&session, 42U, &lookup));
    CHECK(cw_fake_server_lookup_presentation(&session, 164U, &lookup));
}

static void test_linux_owned_fake_grab(void) {
    CwFakeServerSession session = make_session();
    CwServerInputResult result;
    CwPointerMotion motion = {
        {1U, 42U, 300, 100, 300, 400, 2U},
        1U,
    };

    record_canonical_presentation(&session);
    CHECK(cw_fake_server_handle_button(&session,
                                       &(CwPointerButtonEvent){
                                           {1U, 42U, 200, 100, 200, 400, 1U},
                                           CW_POINTER_BUTTON_LEFT,
                                           CW_BUTTON_PRESSED,
                                       },
                                       &result) == CW_SERVER_INPUT_OK);
    CHECK(result.grab_began && session.grab_active);
    CHECK(cw_fake_server_handle_motion(&session, &motion, &result) == CW_SERVER_INPUT_OK);
    CHECK(result.presentation_changed);
    CHECK(session.window_global.x == 1800 && session.window_global.y == 300);
    CHECK(result.generated_presentation.sequence == 43U);
    CHECK(result.generated_presentation.source_rect.x == 120);
    CHECK(result.generated_presentation.source_rect.w == 680);
    CHECK(result.generated_presentation.destination_rect.x == 0);
    CHECK(cw_fake_server_handle_button(&session,
                                       &(CwPointerButtonEvent){
                                           {1U, 43U, 300, 100, 300, 400, 3U},
                                           CW_POINTER_BUTTON_LEFT,
                                           CW_BUTTON_RELEASED,
                                       },
                                       &result) == CW_SERVER_INPUT_OK);
    CHECK(result.grab_ended && !session.grab_active);
}

static void test_button_capture_and_disconnect(void) {
    CwFakeServerSession session = make_session();
    CwServerInputResult result;

    record_canonical_presentation(&session);
    CHECK(cw_fake_server_handle_button(&session,
                                       &(CwPointerButtonEvent){
                                           {1U, 42U, 1, 1, 1, 301, 1U},
                                           CW_POINTER_BUTTON_LEFT,
                                           CW_BUTTON_RELEASED,
                                       },
                                       &result) == CW_SERVER_INPUT_ORDER_ERROR);
    CHECK(cw_fake_server_handle_button(&session,
                                       &(CwPointerButtonEvent){
                                           {1U, 42U, 1, 1, 1, 301, 1U},
                                           CW_POINTER_BUTTON_LEFT,
                                           CW_BUTTON_PRESSED,
                                       },
                                       &result) == CW_SERVER_INPUT_OK);
    CHECK(cw_fake_server_handle_button(&session,
                                       &(CwPointerButtonEvent){
                                           {1U, 42U, 2, 2, 2, 302, 1U},
                                           CW_POINTER_BUTTON_RIGHT,
                                           CW_BUTTON_PRESSED,
                                       },
                                       &result) == CW_SERVER_INPUT_OK);
    CHECK(cw_fake_server_handle_button(&session,
                                       &(CwPointerButtonEvent){
                                           {1U, 42U, 3, 3, 3, 303, 1U},
                                           CW_POINTER_BUTTON_MIDDLE,
                                           CW_BUTTON_PRESSED,
                                       },
                                       &result) == CW_SERVER_INPUT_OK);
    CHECK(session.pressed_button_mask == 7U && session.grab_active);
    cw_fake_server_handle_capture_lost(&session);
    CHECK(session.pressed_button_mask == 0U && !session.grab_active);
    cw_fake_server_handle_disconnect(&session);
    CHECK(!cw_fake_server_lookup_presentation(&session, 42U,
                                               &(CwPresentationState){0}));
}

static void test_pattern(void) {
    uint8_t pixels[800U * 600U * 4U];
    uint8_t before[sizeof(pixels)];
    const size_t crosshair_offset = ((size_t)211U * 3200U) + ((size_t)353U * 4U);

    CHECK(cw_pattern_generate(pixels, 800U, 600U, 3200U));
    memcpy(before, pixels, sizeof(pixels));
    CHECK(cw_pattern_draw_crosshair(pixels, 800U, 600U, 3200U, 353, 211));
    CHECK(memcmp(before, pixels, sizeof(pixels)) != 0);
    CHECK(pixels[crosshair_offset] == 0U && pixels[crosshair_offset + 1U] == 255U &&
          pixels[crosshair_offset + 2U] == 255U && pixels[crosshair_offset + 3U] == 255U);
    CHECK(!cw_pattern_generate(pixels, 4U, 4U, 15U));
}

int main(void) {
    test_presentation_history_and_mapping();
    test_linux_owned_fake_grab();
    test_button_capture_and_disconnect();
    test_pattern();
    printf("session tests: PASS\n");
    printf("cases: %lu\n", cases);
    return EXIT_SUCCESS;
}

#include "session.h"

#include <limits.h>
#include <stddef.h>

static bool fits_i32(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

static uint32_t button_bit(uint32_t button) {
    return 1U << (button - 1U);
}

static bool presentation_is_valid(
    const CwFakeServerSession *session,
    const CwPresentationState *presentation) {
    int64_t source_right;
    int64_t source_bottom;

    if (session == NULL || presentation == NULL || presentation->sequence == 0U ||
        !rect_is_valid(presentation->source_rect) ||
        !rect_is_valid(presentation->destination_rect) ||
        presentation->source_rect.x < 0 || presentation->source_rect.y < 0 ||
        presentation->source_rect.w != presentation->destination_rect.w ||
        presentation->source_rect.h != presentation->destination_rect.h) {
        return false;
    }
    source_right = (int64_t)presentation->source_rect.x + presentation->source_rect.w;
    source_bottom = (int64_t)presentation->source_rect.y + presentation->source_rect.h;
    if (source_right > session->surface_bounds.w || source_bottom > session->surface_bounds.h) {
        return false;
    }
    return !presentation->visible ||
           (!rect_is_empty(presentation->source_rect) &&
            !rect_is_empty(presentation->destination_rect));
}

bool cw_fake_server_session_init(
    CwFakeServerSession *session,
    uint64_t window_id,
    Rect remote_output_global,
    Rect initial_window_global) {
    if (session == NULL || window_id == 0U || !rect_is_valid(remote_output_global) ||
        rect_is_empty(remote_output_global) || !rect_is_valid(initial_window_global) ||
        rect_is_empty(initial_window_global)) {
        return false;
    }
    *session = (CwFakeServerSession){
        window_id,
        remote_output_global,
        initial_window_global,
        {0, 0, initial_window_global.w, initial_window_global.h},
        {{0}},
        0U,
        1U,
        0U,
        false,
        {0, 0},
        {0, 0, 0, 0},
    };
    return true;
}

bool cw_fake_server_lookup_presentation(
    const CwFakeServerSession *session,
    uint64_t presentation_sequence,
    CwPresentationState *out) {
    uint64_t available;
    uint64_t index;

    if (out != NULL) {
        *out = (CwPresentationState){0, {0, 0, 0, 0}, {0, 0, 0, 0}, false};
    }
    if (session == NULL || out == NULL || presentation_sequence == 0U) {
        return false;
    }
    available = session->history_insertions < CW_PRESENTATION_HISTORY_CAPACITY ?
        session->history_insertions : CW_PRESENTATION_HISTORY_CAPACITY;
    for (index = 0U; index < available; ++index) {
        const CwPresentationState *candidate = &session->history[index];

        if (candidate->sequence == presentation_sequence) {
            *out = *candidate;
            return true;
        }
    }
    return false;
}

bool cw_fake_server_record_presentation(
    CwFakeServerSession *session,
    const CwPresentationState *presentation) {
    CwPresentationState existing;
    uint64_t slot;

    if (!presentation_is_valid(session, presentation) ||
        presentation->sequence == UINT64_MAX ||
        cw_fake_server_lookup_presentation(session, presentation->sequence, &existing)) {
        return false;
    }
    slot = session->history_insertions % CW_PRESENTATION_HISTORY_CAPACITY;
    session->history[slot] = *presentation;
    ++session->history_insertions;
    if (presentation->sequence >= session->next_presentation_sequence) {
        session->next_presentation_sequence = presentation->sequence + 1U;
    }
    return true;
}

bool cw_fake_server_recompute_presentation(
    CwFakeServerSession *session,
    CwPresentationState *out) {
    Rect global_intersection;
    Rect source;
    Rect destination;
    CwPresentationState presentation;

    if (out != NULL) {
        *out = (CwPresentationState){0, {0, 0, 0, 0}, {0, 0, 0, 0}, false};
    }
    if (session == NULL || out == NULL || session->next_presentation_sequence == 0U) {
        return false;
    }
    presentation = (CwPresentationState){
        session->next_presentation_sequence,
        {0, 0, 0, 0},
        {0, 0, 0, 0},
        false,
    };
    if (window_region_on_output(session->window_global, session->remote_output_global,
                                &global_intersection, &source, &destination)) {
        (void)global_intersection;
        presentation.source_rect = source;
        presentation.destination_rect = destination;
        presentation.visible = true;
    }
    if (!cw_fake_server_record_presentation(session, &presentation)) {
        return false;
    }
    *out = presentation;
    return true;
}

static void clear_input_result(CwServerInputResult *out, Point client, Point output) {
    if (out != NULL) {
        *out = (CwServerInputResult){
            CW_SERVER_INPUT_INVALID_COORDINATE,
            client,
            output,
            {0, 0},
            {0, 0},
            false,
            false,
            false,
            false,
            {0, {0, 0, 0, 0}, {0, 0, 0, 0}, false},
        };
    }
}

CwServerInputStatus cw_fake_server_map_pointer(
    const CwFakeServerSession *session,
    uint64_t presentation_sequence,
    Point client,
    Point output,
    CwServerInputResult *out) {
    CwPresentationState presentation;

    clear_input_result(out, client, output);
    if (session == NULL || out == NULL ||
        !output_local_point_to_global(session->remote_output_global, output, &out->global)) {
        return CW_SERVER_INPUT_INVALID_COORDINATE;
    }
    if (!cw_fake_server_lookup_presentation(session, presentation_sequence, &presentation)) {
        out->status = CW_SERVER_INPUT_STALE_PRESENTATION;
        return out->status;
    }
    if (!presentation.visible ||
        !presented_fragment_to_surface_local(presentation.source_rect, client, &out->surface)) {
        out->status = CW_SERVER_INPUT_OUTSIDE_PRESENTED_FRAGMENT;
        return out->status;
    }
    out->surface_mapped = true;
    out->status = CW_SERVER_INPUT_OK;
    return out->status;
}

static bool update_fake_grab(
    CwFakeServerSession *session,
    CwServerInputResult *out) {
    int64_t delta_x;
    int64_t delta_y;
    int64_t next_x;
    int64_t next_y;

    if (!session->grab_active) {
        return true;
    }
    delta_x = (int64_t)out->global.x - session->grab_global_pointer.x;
    delta_y = (int64_t)out->global.y - session->grab_global_pointer.y;
    next_x = (int64_t)session->grab_initial_window.x + delta_x;
    next_y = (int64_t)session->grab_initial_window.y + delta_y;
    if (!fits_i32(next_x) || !fits_i32(next_y)) {
        return false;
    }
    session->window_global.x = (int32_t)next_x;
    session->window_global.y = (int32_t)next_y;
    if (!cw_fake_server_recompute_presentation(session, &out->generated_presentation)) {
        return false;
    }
    out->presentation_changed = true;
    return true;
}

CwServerInputStatus cw_fake_server_handle_motion(
    CwFakeServerSession *session,
    const CwPointerMotion *motion,
    CwServerInputResult *out) {
    CwServerInputStatus status;

    if (motion == NULL || out == NULL || session == NULL || motion->location.window_id != session->window_id) {
        clear_input_result(out, (Point){0, 0}, (Point){0, 0});
        return CW_SERVER_INPUT_INVALID_COORDINATE;
    }
    status = cw_fake_server_map_pointer(session, motion->location.presentation_sequence,
                                        (Point){motion->location.client_x, motion->location.client_y},
                                        (Point){motion->location.output_x, motion->location.output_y}, out);
    if ((status == CW_SERVER_INPUT_OK || status == CW_SERVER_INPUT_OUTSIDE_PRESENTED_FRAGMENT) &&
        !update_fake_grab(session, out)) {
        out->status = CW_SERVER_INPUT_INVALID_COORDINATE;
        return out->status;
    }
    return status;
}

CwServerInputStatus cw_fake_server_handle_button(
    CwFakeServerSession *session,
    const CwPointerButtonEvent *button,
    CwServerInputResult *out) {
    CwServerInputStatus status;
    uint32_t bit;

    if (button == NULL || out == NULL || session == NULL ||
        button->location.window_id != session->window_id ||
        button->button < CW_POINTER_BUTTON_LEFT || button->button > CW_POINTER_BUTTON_MIDDLE ||
        (button->state != CW_BUTTON_PRESSED && button->state != CW_BUTTON_RELEASED)) {
        clear_input_result(out, (Point){0, 0}, (Point){0, 0});
        return CW_SERVER_INPUT_INVALID_COORDINATE;
    }
    status = cw_fake_server_map_pointer(session, button->location.presentation_sequence,
                                        (Point){button->location.client_x, button->location.client_y},
                                        (Point){button->location.output_x, button->location.output_y}, out);
    if (status != CW_SERVER_INPUT_OK && status != CW_SERVER_INPUT_OUTSIDE_PRESENTED_FRAGMENT) {
        return status;
    }
    bit = button_bit(button->button);
    if (button->state == CW_BUTTON_PRESSED) {
        if ((session->pressed_button_mask & bit) != 0U) {
            out->status = CW_SERVER_INPUT_ORDER_ERROR;
            return out->status;
        }
        session->pressed_button_mask |= bit;
        if (button->button == CW_POINTER_BUTTON_LEFT) {
            session->grab_active = true;
            session->grab_global_pointer = out->global;
            session->grab_initial_window = session->window_global;
            out->grab_began = true;
        }
    } else {
        if ((session->pressed_button_mask & bit) == 0U) {
            out->status = CW_SERVER_INPUT_ORDER_ERROR;
            return out->status;
        }
        session->pressed_button_mask &= ~bit;
        if (button->button == CW_POINTER_BUTTON_LEFT) {
            session->grab_active = false;
            out->grab_ended = true;
        }
    }
    return status;
}

void cw_fake_server_handle_capture_lost(CwFakeServerSession *session) {
    if (session != NULL) {
        session->pressed_button_mask = 0U;
        session->grab_active = false;
    }
}

void cw_fake_server_handle_disconnect(CwFakeServerSession *session) {
    if (session != NULL) {
        cw_fake_server_handle_capture_lost(session);
        session->history_insertions = 0U;
        session->next_presentation_sequence = 1U;
    }
}

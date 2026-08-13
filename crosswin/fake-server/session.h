#ifndef CROSSWIN_FAKE_SERVER_SESSION_H
#define CROSSWIN_FAKE_SERVER_SESSION_H

#include "../common/protocol.h"
#include "geometry.h"

#include <stdbool.h>
#include <stdint.h>

enum { CW_PRESENTATION_HISTORY_CAPACITY = 64U };

typedef struct {
    uint64_t sequence;
    Rect source_rect;      /* Surface-local logical rectangle. */
    Rect destination_rect; /* Remote-output-local logical rectangle. */
    bool visible;
} CwPresentationState;

typedef enum {
    CW_SERVER_INPUT_OK = 0,
    CW_SERVER_INPUT_STALE_PRESENTATION,
    CW_SERVER_INPUT_OUTSIDE_PRESENTED_FRAGMENT,
    CW_SERVER_INPUT_INVALID_COORDINATE,
    CW_SERVER_INPUT_ORDER_ERROR,
} CwServerInputStatus;

typedef struct {
    CwServerInputStatus status;
    Point client;
    Point output;
    Point global;
    Point surface;
    bool surface_mapped;
    bool grab_began;
    bool grab_ended;
    bool presentation_changed;
    CwPresentationState generated_presentation;
} CwServerInputResult;

typedef struct {
    uint64_t window_id;
    Rect remote_output_global;
    Rect window_global;
    Rect surface_bounds;
    CwPresentationState history[CW_PRESENTATION_HISTORY_CAPACITY];
    uint64_t history_insertions;
    uint64_t next_presentation_sequence;
    uint32_t pressed_button_mask;
    bool grab_active;
    Point grab_global_pointer;
    Rect grab_initial_window;
} CwFakeServerSession;

/* One Linux-owned window and one remote output.  Both rectangles are global logical. */
bool cw_fake_server_session_init(
    CwFakeServerSession *session,
    uint64_t window_id,
    Rect remote_output_global,
    Rect initial_window_global);

/* Adds a sent presentation to the 64-entry sequence-keyed history. */
bool cw_fake_server_record_presentation(
    CwFakeServerSession *session,
    const CwPresentationState *presentation);

bool cw_fake_server_lookup_presentation(
    const CwFakeServerSession *session,
    uint64_t presentation_sequence,
    CwPresentationState *out);

/*
 * Recomputes a new presentation from canonical Linux global geometry via
 * Stage 1.  It reserves a sequence but does not add it to history: only a
 * presentation actually transmitted to Windows may be referenced by input.
 */
bool cw_fake_server_recompute_presentation(
    CwFakeServerSession *session,
    CwPresentationState *out);

/* Maps a historical presentation's HWND client point with Stage 1 geometry only. */
CwServerInputStatus cw_fake_server_map_pointer(
    const CwFakeServerSession *session,
    uint64_t presentation_sequence,
    Point client,
    Point output,
    CwServerInputResult *out);

CwServerInputStatus cw_fake_server_handle_motion(
    CwFakeServerSession *session,
    const CwPointerMotion *motion,
    CwServerInputResult *out);
CwServerInputStatus cw_fake_server_handle_button(
    CwFakeServerSession *session,
    const CwPointerButtonEvent *button,
    CwServerInputResult *out);

/* Capture loss and TCP disconnect cancel the Linux fake grab and every pressed button. */
void cw_fake_server_handle_capture_lost(CwFakeServerSession *session);
void cw_fake_server_handle_disconnect(CwFakeServerSession *session);

#endif

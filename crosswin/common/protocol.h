#ifndef CROSSWIN_PROTOCOL_H
#define CROSSWIN_PROTOCOL_H

/*
 * CrossWin Network Protocol (CWNP), version 4.
 *
 * Every integer is serialized explicitly in little-endian byte order.  No
 * native C/C++ struct is ever used as a wire message.  TCP is a byte stream:
 * CwDecoder accepts arbitrary fragments and emits complete messages only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CW_PROTOCOL_MAGIC = 0x504E5743U, /* Wire bytes: 'C', 'W', 'N', 'P'. */
    CW_PROTOCOL_VERSION = 4U,
    CW_HEADER_SIZE = 24U,
    CW_MAX_PAYLOAD = 64U * 1024U * 1024U,
    CW_PIXEL_FORMAT_BGRA8888 = 1U,
    CW_PIXEL_FORMAT_MASK_BGRA8888 = 1U << 0,
    CW_MAX_SURFACE_DIMENSION = 16384U,
    CW_MAX_DAMAGE_RECTS = 256U,
    CW_OPTIONAL_MESSAGE_BIT = 0x8000U,
};

typedef enum {
    CW_MESSAGE_HELLO = 1,
    CW_MESSAGE_HELLO_ACK = 2,
    CW_MESSAGE_WINDOW_CREATE = 10,
    CW_MESSAGE_WINDOW_FRAME = 11,
    CW_MESSAGE_WINDOW_PRESENT = 12,
    CW_MESSAGE_WINDOW_PRESENT_ACK = 13,
    CW_MESSAGE_WINDOW_DESTROY = 14,
    CW_MESSAGE_WINDOW_DAMAGE = 15,
    CW_MESSAGE_WINDOW_FRAME_REQUEST = 16,
    CW_MESSAGE_WINDOW_RESIZE = 17,
    /* Linux canonical desktop-shell activation/stacking notification. */
    CW_MESSAGE_WINDOW_ACTIVATE = 18,
    CW_MESSAGE_POINTER_ENTER = 30,
    CW_MESSAGE_POINTER_LEAVE = 31,
    CW_MESSAGE_POINTER_MOTION = 32,
    CW_MESSAGE_POINTER_BUTTON = 33,
    CW_MESSAGE_POINTER_WHEEL = 34,
    CW_MESSAGE_POINTER_CAPTURE_LOST = 35,
} CwMessageType;

typedef enum {
    CW_POINTER_BUTTON_LEFT = 1,
    CW_POINTER_BUTTON_RIGHT = 2,
    CW_POINTER_BUTTON_MIDDLE = 3,
} CwPointerButton;

typedef enum {
    CW_BUTTON_RELEASED = 0,
    CW_BUTTON_PRESSED = 1,
} CwButtonState;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t flags;
    uint32_t payload_length;
    uint64_t sequence;
} CwHeader;

typedef struct {
    uint16_t min_version;
    uint16_t max_version;
    uint32_t pixel_format_mask;
} CwHello;

typedef struct {
    uint16_t selected_version;
    uint32_t pixel_format_mask;
} CwHelloAck;

typedef struct {
    uint64_t window_id;
    uint32_t surface_width;
    uint32_t surface_height;
    /* 0 means a top-level proxy.  A non-zero parent identifies an xdg_popup
     * owner and requires the receiver to make a non-activating owned window. */
    uint64_t parent_window_id;
} CwWindowCreate;

typedef struct {
    uint64_t window_id;
    uint64_t frame_sequence;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    const uint8_t *pixels;
    uint64_t pixel_bytes;
} CwWindowFrame;

/*
 * WINDOW_DAMAGE is one atomic content update.  Each rectangle is encoded as
 * { i32 x, i32 y, u32 width, u32 height, tightly-packed BGRA8888 pixels }.
 * There is deliberately no per-rectangle stride: every row is width * 4.
 */
typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    const uint8_t *pixels;
    uint64_t pixel_bytes;
} CwDamageRect;

typedef struct {
    uint64_t window_id;
    uint64_t frame_sequence;
    uint64_t base_frame_sequence;
    uint32_t rect_count;
    const uint8_t *rect_data;
    uint32_t rect_data_length;
} CwWindowDamage;

typedef struct {
    uint64_t window_id;
    uint64_t local_frame_sequence;
} CwWindowFrameRequest;

typedef struct {
    uint64_t window_id;
    uint32_t surface_width;
    uint32_t surface_height;
} CwWindowResize;

/* All presentation rectangles are surface-local or output-local logical px. */
typedef struct {
    uint64_t window_id;
    uint64_t presentation_sequence;
    int32_t source_x;
    int32_t source_y;
    int32_t source_w;
    int32_t source_h;
    int32_t destination_x;
    int32_t destination_y;
    int32_t destination_w;
    int32_t destination_h;
    bool visible;
} CwWindowPresent;

typedef struct {
    uint64_t window_id;
    uint64_t presentation_sequence;
} CwWindowPresentAck;

/* Sent by Linux after desktop-shell has made this logical window active.
 * The Windows side only brings its matching presentation HWND forward. */
typedef struct {
    uint64_t window_id;
} CwWindowActivate;

typedef struct {
    uint64_t window_id;
    uint64_t presentation_sequence;
    int32_t client_x;
    int32_t client_y;
    int32_t output_x;
    int32_t output_y;
    uint64_t timestamp_ms;
} CwPointerLocation;

typedef struct {
    CwPointerLocation location;
    uint32_t button_mask;
} CwPointerMotion;

typedef struct {
    CwPointerLocation location;
    uint32_t button;
    uint32_t state;
} CwPointerButtonEvent;

typedef struct {
    CwPointerLocation location;
    int32_t delta_x;
    int32_t delta_y;
} CwPointerWheel;

typedef struct {
    uint64_t window_id;
    uint64_t presentation_sequence;
    uint64_t timestamp_ms;
} CwPointerCaptureLost;

typedef enum {
    CW_DECODER_OK = 0,
    CW_DECODER_BAD_ARGUMENT,
    CW_DECODER_NO_MEMORY,
    CW_DECODER_BAD_MAGIC,
    CW_DECODER_UNSUPPORTED_VERSION,
    CW_DECODER_UNKNOWN_MANDATORY_MESSAGE,
    CW_DECODER_PAYLOAD_TOO_LARGE,
    CW_DECODER_INVALID_PAYLOAD,
    CW_DECODER_CALLBACK_REJECTED,
    CW_DECODER_TRUNCATED,
} CwDecoderError;

typedef bool (*CwMessageCallback)(
    void *context,
    const CwHeader *header,
    const uint8_t *payload);

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    bool header_ready;
    CwHeader header;
    CwDecoderError error;
} CwDecoder;

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} CwBuffer;

bool cw_message_type_is_known(uint16_t type);
bool cw_message_type_is_optional(uint16_t type);
const char *cw_message_type_name(uint16_t type);

void cw_store_u16_le(uint8_t out[2], uint16_t value);
void cw_store_u32_le(uint8_t out[4], uint32_t value);
void cw_store_u64_le(uint8_t out[8], uint64_t value);
void cw_store_i32_le(uint8_t out[4], int32_t value);
uint16_t cw_load_u16_le(const uint8_t in[2]);
uint32_t cw_load_u32_le(const uint8_t in[4]);
uint64_t cw_load_u64_le(const uint8_t in[8]);
int32_t cw_load_i32_le(const uint8_t in[4]);

bool cw_header_encode(uint8_t out[CW_HEADER_SIZE], const CwHeader *header);
bool cw_header_decode(const uint8_t in[CW_HEADER_SIZE], CwHeader *header);
bool cw_header_is_valid(const CwHeader *header, CwDecoderError *error);
bool cw_message_is_valid(const CwHeader *header, const uint8_t *payload);

void cw_decoder_init(CwDecoder *decoder);
void cw_decoder_destroy(CwDecoder *decoder);
bool cw_decoder_feed(
    CwDecoder *decoder,
    const uint8_t *bytes,
    size_t length,
    CwMessageCallback callback,
    void *context);
bool cw_decoder_finish(CwDecoder *decoder);
CwDecoderError cw_decoder_error(const CwDecoder *decoder);
const char *cw_decoder_error_string(CwDecoderError error);

void cw_buffer_init(CwBuffer *buffer);
void cw_buffer_destroy(CwBuffer *buffer);
bool cw_message_encode(
    CwBuffer *buffer,
    uint16_t type,
    uint32_t flags,
    uint64_t sequence,
    const uint8_t *payload,
    uint32_t payload_length);

bool cw_decode_hello(const uint8_t *payload, uint32_t length, CwHello *out);
bool cw_decode_hello_ack(const uint8_t *payload, uint32_t length, CwHelloAck *out);
bool cw_decode_window_create(const uint8_t *payload, uint32_t length, CwWindowCreate *out);
bool cw_decode_window_frame(const uint8_t *payload, uint32_t length, CwWindowFrame *out);
bool cw_decode_window_damage(const uint8_t *payload, uint32_t length, CwWindowDamage *out);
bool cw_window_damage_rect_at(const CwWindowDamage *damage, uint32_t index,
                              CwDamageRect *out);
bool cw_decode_window_frame_request(const uint8_t *payload, uint32_t length,
                                    CwWindowFrameRequest *out);
bool cw_decode_window_resize(const uint8_t *payload, uint32_t length,
                             CwWindowResize *out);
bool cw_decode_window_present(const uint8_t *payload, uint32_t length, CwWindowPresent *out);
bool cw_decode_window_present_ack(
    const uint8_t *payload, uint32_t length, CwWindowPresentAck *out);
bool cw_decode_window_activate(const uint8_t *payload, uint32_t length,
                               CwWindowActivate *out);
bool cw_decode_pointer_location(const uint8_t *payload, uint32_t length, CwPointerLocation *out);
bool cw_decode_pointer_motion(const uint8_t *payload, uint32_t length, CwPointerMotion *out);
bool cw_decode_pointer_button(
    const uint8_t *payload, uint32_t length, CwPointerButtonEvent *out);
bool cw_decode_pointer_wheel(const uint8_t *payload, uint32_t length, CwPointerWheel *out);
bool cw_decode_pointer_capture_lost(
    const uint8_t *payload, uint32_t length, CwPointerCaptureLost *out);

#ifdef __cplusplus
}
#endif

#endif

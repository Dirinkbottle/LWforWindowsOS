#include "protocol.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    CW_HELLO_PAYLOAD_SIZE = 8U,
    CW_HELLO_ACK_PAYLOAD_SIZE = 8U,
    CW_WINDOW_CREATE_PAYLOAD_SIZE = 24U,
    CW_WINDOW_FRAME_PREFIX_SIZE = 32U,
    CW_WINDOW_DAMAGE_PREFIX_SIZE = 32U,
    CW_WINDOW_DAMAGE_RECT_PREFIX_SIZE = 16U,
    CW_WINDOW_FRAME_REQUEST_PAYLOAD_SIZE = 16U,
    CW_WINDOW_RESIZE_PAYLOAD_SIZE = 16U,
    CW_WINDOW_PRESENT_PAYLOAD_SIZE = 56U,
    CW_WINDOW_PRESENT_ACK_PAYLOAD_SIZE = 16U,
    CW_WINDOW_DESTROY_PAYLOAD_SIZE = 8U,
    CW_WINDOW_ACTIVATE_PAYLOAD_SIZE = 8U,
    CW_OUTPUT_CONFIG_PAYLOAD_SIZE = 24U,
    CW_POINTER_LOCATION_PAYLOAD_SIZE = 40U,
    CW_POINTER_MOTION_PAYLOAD_SIZE = 48U,
    CW_POINTER_BUTTON_PAYLOAD_SIZE = 48U,
    CW_POINTER_WHEEL_PAYLOAD_SIZE = 48U,
    CW_POINTER_CAPTURE_LOST_PAYLOAD_SIZE = 24U,
};

static void set_error(CwDecoderError *out, CwDecoderError error) {
    if (out != NULL) {
        *out = error;
    }
}

static bool valid_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)x;
    (void)y;
    return w >= 0 && h >= 0;
}

static bool valid_surface(uint32_t width, uint32_t height) {
    return width != 0U && height != 0U &&
           width <= CW_MAX_SURFACE_DIMENSION &&
           height <= CW_MAX_SURFACE_DIMENSION;
}

/* Every supported Stage 7A surface must also fit into its full-frame fallback
 * message.  This keeps CREATE/RESIZE from making the receiver allocate a
 * framebuffer which CWNP can never resynchronize. */
static bool valid_frame_surface(uint32_t width, uint32_t height) {
    return valid_surface(width, height) &&
           (uint64_t)width * (uint64_t)height * 4U <=
               CW_MAX_PAYLOAD - CW_WINDOW_FRAME_PREFIX_SIZE;
}

bool cw_message_type_is_known(uint16_t type) {
    switch (type) {
    case CW_MESSAGE_HELLO:
    case CW_MESSAGE_HELLO_ACK:
    case CW_MESSAGE_WINDOW_CREATE:
    case CW_MESSAGE_WINDOW_FRAME:
    case CW_MESSAGE_WINDOW_PRESENT:
    case CW_MESSAGE_WINDOW_PRESENT_ACK:
    case CW_MESSAGE_WINDOW_DESTROY:
    case CW_MESSAGE_WINDOW_DAMAGE:
    case CW_MESSAGE_WINDOW_FRAME_REQUEST:
    case CW_MESSAGE_WINDOW_RESIZE:
    case CW_MESSAGE_WINDOW_ACTIVATE:
    case CW_MESSAGE_OUTPUT_CONFIG:
    case CW_MESSAGE_POINTER_ENTER:
    case CW_MESSAGE_POINTER_LEAVE:
    case CW_MESSAGE_POINTER_MOTION:
    case CW_MESSAGE_POINTER_BUTTON:
    case CW_MESSAGE_POINTER_WHEEL:
    case CW_MESSAGE_POINTER_CAPTURE_LOST:
        return true;
    default:
        return false;
    }
}

bool cw_message_type_is_optional(uint16_t type) {
    return (type & CW_OPTIONAL_MESSAGE_BIT) != 0U;
}

const char *cw_message_type_name(uint16_t type) {
    switch (type) {
    case CW_MESSAGE_HELLO:
        return "HELLO";
    case CW_MESSAGE_HELLO_ACK:
        return "HELLO_ACK";
    case CW_MESSAGE_WINDOW_CREATE:
        return "WINDOW_CREATE";
    case CW_MESSAGE_WINDOW_FRAME:
        return "WINDOW_FRAME";
    case CW_MESSAGE_WINDOW_PRESENT:
        return "WINDOW_PRESENT";
    case CW_MESSAGE_WINDOW_PRESENT_ACK:
        return "WINDOW_PRESENT_ACK";
    case CW_MESSAGE_WINDOW_DESTROY:
        return "WINDOW_DESTROY";
    case CW_MESSAGE_WINDOW_DAMAGE:
        return "WINDOW_DAMAGE";
    case CW_MESSAGE_WINDOW_FRAME_REQUEST:
        return "WINDOW_FRAME_REQUEST";
    case CW_MESSAGE_WINDOW_RESIZE:
        return "WINDOW_RESIZE";
    case CW_MESSAGE_WINDOW_ACTIVATE:
        return "WINDOW_ACTIVATE";
    case CW_MESSAGE_OUTPUT_CONFIG:
        return "OUTPUT_CONFIG";
    case CW_MESSAGE_POINTER_ENTER:
        return "POINTER_ENTER";
    case CW_MESSAGE_POINTER_LEAVE:
        return "POINTER_LEAVE";
    case CW_MESSAGE_POINTER_MOTION:
        return "POINTER_MOTION";
    case CW_MESSAGE_POINTER_BUTTON:
        return "POINTER_BUTTON";
    case CW_MESSAGE_POINTER_WHEEL:
        return "POINTER_WHEEL";
    case CW_MESSAGE_POINTER_CAPTURE_LOST:
        return "POINTER_CAPTURE_LOST";
    default:
        return cw_message_type_is_optional(type) ? "OPTIONAL_UNKNOWN" : "UNKNOWN";
    }
}

void cw_store_u16_le(uint8_t out[2], uint16_t value) {
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)(value >> 8);
}

void cw_store_u32_le(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
    out[2] = (uint8_t)((value >> 16) & 0xffU);
    out[3] = (uint8_t)(value >> 24);
}

void cw_store_u64_le(uint8_t out[8], uint64_t value) {
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        out[index] = (uint8_t)(value >> (index * 8U));
    }
}

void cw_store_i32_le(uint8_t out[4], int32_t value) {
    uint32_t bits;

    if (value >= 0) {
        bits = (uint32_t)value;
    } else {
        bits = UINT32_C(0x80000000) + (uint32_t)(value - INT32_MIN);
    }
    cw_store_u32_le(out, bits);
}

uint16_t cw_load_u16_le(const uint8_t in[2]) {
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

uint32_t cw_load_u32_le(const uint8_t in[4]) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

uint64_t cw_load_u64_le(const uint8_t in[8]) {
    uint64_t value = 0U;
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)in[index] << (index * 8U);
    }
    return value;
}

int32_t cw_load_i32_le(const uint8_t in[4]) {
    const uint32_t bits = cw_load_u32_le(in);

    if (bits <= INT32_MAX) {
        return (int32_t)bits;
    }
    return INT32_MIN + (int32_t)(bits - UINT32_C(0x80000000));
}

bool cw_header_encode(uint8_t out[CW_HEADER_SIZE], const CwHeader *header) {
    if (out == NULL || header == NULL || !cw_header_is_valid(header, NULL)) {
        return false;
    }
    cw_store_u32_le(out, header->magic);
    cw_store_u16_le(out + 4U, header->version);
    cw_store_u16_le(out + 6U, header->type);
    cw_store_u32_le(out + 8U, header->flags);
    cw_store_u32_le(out + 12U, header->payload_length);
    cw_store_u64_le(out + 16U, header->sequence);
    return true;
}

bool cw_header_decode(const uint8_t in[CW_HEADER_SIZE], CwHeader *header) {
    if (in == NULL || header == NULL) {
        return false;
    }
    *header = (CwHeader){
        cw_load_u32_le(in),
        cw_load_u16_le(in + 4U),
        cw_load_u16_le(in + 6U),
        cw_load_u32_le(in + 8U),
        cw_load_u32_le(in + 12U),
        cw_load_u64_le(in + 16U),
    };
    return true;
}

bool cw_header_is_valid(const CwHeader *header, CwDecoderError *error) {
    if (header == NULL) {
        set_error(error, CW_DECODER_BAD_ARGUMENT);
        return false;
    }
    if (header->magic != CW_PROTOCOL_MAGIC) {
        set_error(error, CW_DECODER_BAD_MAGIC);
        return false;
    }
    if (header->version != CW_PROTOCOL_VERSION) {
        set_error(error, CW_DECODER_UNSUPPORTED_VERSION);
        return false;
    }
    if (!cw_message_type_is_known(header->type) &&
        !cw_message_type_is_optional(header->type)) {
        set_error(error, CW_DECODER_UNKNOWN_MANDATORY_MESSAGE);
        return false;
    }
    if (header->payload_length > CW_MAX_PAYLOAD) {
        set_error(error, CW_DECODER_PAYLOAD_TOO_LARGE);
        return false;
    }
    set_error(error, CW_DECODER_OK);
    return true;
}

static bool valid_frame_payload(const uint8_t *payload, uint32_t length) {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint64_t minimum_stride;
    uint64_t pixel_bytes;
    uint64_t expected_length;

    if (payload == NULL || length < CW_WINDOW_FRAME_PREFIX_SIZE) {
        return false;
    }
    width = cw_load_u32_le(payload + 16U);
    height = cw_load_u32_le(payload + 20U);
    stride = cw_load_u32_le(payload + 24U);
    pixel_format = cw_load_u32_le(payload + 28U);
    if (cw_load_u64_le(payload + 8U) == 0U ||
        !valid_frame_surface(width, height) ||
        pixel_format != CW_PIXEL_FORMAT_BGRA8888) {
        return false;
    }
    minimum_stride = (uint64_t)width * 4U;
    if ((uint64_t)stride < minimum_stride) {
        return false;
    }
    pixel_bytes = (uint64_t)stride * (uint64_t)height;
    expected_length = (uint64_t)CW_WINDOW_FRAME_PREFIX_SIZE + pixel_bytes;
    return expected_length == (uint64_t)length && expected_length <= CW_MAX_PAYLOAD;
}

static bool damage_rect_pixel_bytes(uint32_t width, uint32_t height,
                                    uint64_t *out_bytes)
{
    uint64_t bytes;

    if (width > CW_MAX_SURFACE_DIMENSION || height > CW_MAX_SURFACE_DIMENSION) {
        return false;
    }
    bytes = (uint64_t)width * (uint64_t)height * 4U;
    if (bytes > CW_MAX_PAYLOAD) {
        return false;
    }
    *out_bytes = bytes;
    return true;
}

static bool valid_damage_payload(const uint8_t *payload, uint32_t length)
{
    uint64_t offset;
    uint32_t index;
    uint32_t rect_count;

    if (payload == NULL || length < CW_WINDOW_DAMAGE_PREFIX_SIZE ||
        cw_load_u64_le(payload + 8U) == 0U ||
        cw_load_u64_le(payload + 16U) == 0U ||
        cw_load_u64_le(payload + 8U) <= cw_load_u64_le(payload + 16U) ||
        cw_load_u32_le(payload + 28U) != 0U) {
        return false;
    }
    rect_count = cw_load_u32_le(payload + 24U);
    if (rect_count > CW_MAX_DAMAGE_RECTS) {
        return false;
    }
    offset = CW_WINDOW_DAMAGE_PREFIX_SIZE;
    for (index = 0U; index < rect_count; ++index) {
        uint32_t width;
        uint32_t height;
        uint64_t pixel_bytes;

        if (offset > length || (uint64_t)length - offset < CW_WINDOW_DAMAGE_RECT_PREFIX_SIZE) {
            return false;
        }
        width = cw_load_u32_le(payload + offset + 8U);
        height = cw_load_u32_le(payload + offset + 12U);
        if (!damage_rect_pixel_bytes(width, height, &pixel_bytes) ||
            pixel_bytes > (uint64_t)length - offset - CW_WINDOW_DAMAGE_RECT_PREFIX_SIZE) {
            return false;
        }
        offset += CW_WINDOW_DAMAGE_RECT_PREFIX_SIZE + pixel_bytes;
    }
    return offset == length;
}

static bool valid_present_payload(const uint8_t *payload, uint32_t length) {
    int32_t source_x;
    int32_t source_y;
    int32_t source_w;
    int32_t source_h;
    int32_t destination_x;
    int32_t destination_y;
    int32_t destination_w;
    int32_t destination_h;
    uint32_t visible;
    uint32_t reserved;

    if (payload == NULL || length != CW_WINDOW_PRESENT_PAYLOAD_SIZE) {
        return false;
    }
    source_x = cw_load_i32_le(payload + 16U);
    source_y = cw_load_i32_le(payload + 20U);
    source_w = cw_load_i32_le(payload + 24U);
    source_h = cw_load_i32_le(payload + 28U);
    destination_x = cw_load_i32_le(payload + 32U);
    destination_y = cw_load_i32_le(payload + 36U);
    destination_w = cw_load_i32_le(payload + 40U);
    destination_h = cw_load_i32_le(payload + 44U);
    visible = cw_load_u32_le(payload + 48U);
    reserved = cw_load_u32_le(payload + 52U);
    return valid_rect(source_x, source_y, source_w, source_h) &&
           valid_rect(destination_x, destination_y, destination_w, destination_h) &&
           source_w == destination_w && source_h == destination_h &&
           (visible == 0U || visible == 1U) && reserved == 0U;
}

static bool valid_pointer_button_payload(const uint8_t *payload, uint32_t length) {
    uint32_t button;
    uint32_t state;

    if (payload == NULL || length != CW_POINTER_BUTTON_PAYLOAD_SIZE) {
        return false;
    }
    button = cw_load_u32_le(payload + 40U);
    state = cw_load_u32_le(payload + 44U);
    return button >= CW_POINTER_BUTTON_LEFT && button <= CW_POINTER_BUTTON_MIDDLE &&
           (state == CW_BUTTON_RELEASED || state == CW_BUTTON_PRESSED);
}

bool cw_message_is_valid(const CwHeader *header, const uint8_t *payload) {
    if (!cw_header_is_valid(header, NULL)) {
        return false;
    }
    if (header->payload_length != 0U && payload == NULL) {
        return false;
    }
    if (!cw_message_type_is_known(header->type)) {
        return true; /* An unknown optional extension is deliberately opaque. */
    }
    switch (header->type) {
    case CW_MESSAGE_HELLO:
        return header->payload_length == CW_HELLO_PAYLOAD_SIZE &&
               cw_load_u16_le(payload) != 0U &&
               cw_load_u16_le(payload + 2U) >= cw_load_u16_le(payload);
    case CW_MESSAGE_HELLO_ACK:
        return header->payload_length == CW_HELLO_ACK_PAYLOAD_SIZE &&
               cw_load_u16_le(payload) == CW_PROTOCOL_VERSION &&
               cw_load_u16_le(payload + 2U) == 0U;
    case CW_MESSAGE_WINDOW_CREATE:
        return header->payload_length == CW_WINDOW_CREATE_PAYLOAD_SIZE &&
	       cw_load_u64_le(payload) != 0U &&
	       cw_load_u64_le(payload) != cw_load_u64_le(payload + 16U) &&
               valid_frame_surface(cw_load_u32_le(payload + 8U),
                                   cw_load_u32_le(payload + 12U));
    case CW_MESSAGE_WINDOW_FRAME:
        return valid_frame_payload(payload, header->payload_length);
    case CW_MESSAGE_WINDOW_DAMAGE:
        return valid_damage_payload(payload, header->payload_length);
    case CW_MESSAGE_WINDOW_FRAME_REQUEST:
        return header->payload_length == CW_WINDOW_FRAME_REQUEST_PAYLOAD_SIZE;
    case CW_MESSAGE_WINDOW_RESIZE:
        return header->payload_length == CW_WINDOW_RESIZE_PAYLOAD_SIZE &&
               valid_frame_surface(cw_load_u32_le(payload + 8U),
                                   cw_load_u32_le(payload + 12U));
    case CW_MESSAGE_WINDOW_PRESENT:
        return valid_present_payload(payload, header->payload_length);
    case CW_MESSAGE_WINDOW_PRESENT_ACK:
        return header->payload_length == CW_WINDOW_PRESENT_ACK_PAYLOAD_SIZE;
    case CW_MESSAGE_WINDOW_DESTROY:
        return header->payload_length == CW_WINDOW_DESTROY_PAYLOAD_SIZE;
    case CW_MESSAGE_WINDOW_ACTIVATE:
        return header->payload_length == CW_WINDOW_ACTIVATE_PAYLOAD_SIZE &&
               cw_load_u64_le(payload) != 0U;
    case CW_MESSAGE_OUTPUT_CONFIG:
        return header->payload_length == CW_OUTPUT_CONFIG_PAYLOAD_SIZE &&
               cw_load_u32_le(payload + 8U) != 0U &&
               cw_load_u32_le(payload + 12U) != 0U &&
               cw_load_u32_le(payload + 16U) != 0U &&
               cw_load_u32_le(payload + 20U) != 0U;
    case CW_MESSAGE_POINTER_ENTER:
    case CW_MESSAGE_POINTER_LEAVE:
        return header->payload_length == CW_POINTER_LOCATION_PAYLOAD_SIZE;
    case CW_MESSAGE_POINTER_MOTION:
        return header->payload_length == CW_POINTER_MOTION_PAYLOAD_SIZE &&
               cw_load_u32_le(payload + 44U) == 0U;
    case CW_MESSAGE_POINTER_BUTTON:
        return valid_pointer_button_payload(payload, header->payload_length);
    case CW_MESSAGE_POINTER_WHEEL:
        return header->payload_length == CW_POINTER_WHEEL_PAYLOAD_SIZE;
    case CW_MESSAGE_POINTER_CAPTURE_LOST:
        return header->payload_length == CW_POINTER_CAPTURE_LOST_PAYLOAD_SIZE;
    default:
        return false;
    }
}

static bool reserve_bytes(uint8_t **data, size_t *capacity, size_t required) {
    size_t new_capacity;
    uint8_t *new_data;

    if (required <= *capacity) {
        return true;
    }
    new_capacity = *capacity == 0U ? 64U : *capacity;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2U;
    }
    new_data = realloc(*data, new_capacity);
    if (new_data == NULL) {
        return false;
    }
    *data = new_data;
    *capacity = new_capacity;
    return true;
}

void cw_decoder_init(CwDecoder *decoder) {
    if (decoder != NULL) {
        *decoder = (CwDecoder){NULL, 0U, 0U, false, {0}, CW_DECODER_OK};
    }
}

void cw_decoder_destroy(CwDecoder *decoder) {
    if (decoder != NULL) {
        free(decoder->data);
        cw_decoder_init(decoder);
    }
}

static bool decoder_append(CwDecoder *decoder, const uint8_t *bytes, size_t length) {
    size_t required;

    if (length > SIZE_MAX - decoder->length) {
        decoder->error = CW_DECODER_NO_MEMORY;
        return false;
    }
    required = decoder->length + length;
    if (!reserve_bytes(&decoder->data, &decoder->capacity, required)) {
        decoder->error = CW_DECODER_NO_MEMORY;
        return false;
    }
    memcpy(decoder->data + decoder->length, bytes, length);
    decoder->length = required;
    return true;
}

bool cw_decoder_feed(
    CwDecoder *decoder,
    const uint8_t *bytes,
    size_t length,
    CwMessageCallback callback,
    void *context) {
    size_t offset = 0U;

    if (decoder == NULL || callback == NULL || (length != 0U && bytes == NULL)) {
        if (decoder != NULL) {
            decoder->error = CW_DECODER_BAD_ARGUMENT;
        }
        return false;
    }
    if (decoder->error != CW_DECODER_OK) {
        return false;
    }

    while (offset < length) {
        size_t required;
        size_t available;
        size_t take;

        if (!decoder->header_ready) {
            required = (size_t)CW_HEADER_SIZE - decoder->length;
            available = length - offset;
            take = required < available ? required : available;
            if (!decoder_append(decoder, bytes + offset, take)) {
                return false;
            }
            offset += take;
            if (decoder->length < CW_HEADER_SIZE) {
                continue;
            }
            if (!cw_header_decode(decoder->data, &decoder->header) ||
                !cw_header_is_valid(&decoder->header, &decoder->error)) {
                return false;
            }
            decoder->header_ready = true;
        }

        required = (size_t)CW_HEADER_SIZE + (size_t)decoder->header.payload_length;
        if (decoder->length < required) {
            available = length - offset;
            take = (required - decoder->length) < available ?
                (required - decoder->length) : available;
            if (!decoder_append(decoder, bytes + offset, take)) {
                return false;
            }
            offset += take;
        }
        if (decoder->length != required) {
            continue;
        }
        if (!cw_message_is_valid(&decoder->header, decoder->data + CW_HEADER_SIZE)) {
            decoder->error = CW_DECODER_INVALID_PAYLOAD;
            return false;
        }
        if (!callback(context, &decoder->header, decoder->data + CW_HEADER_SIZE)) {
            decoder->error = CW_DECODER_CALLBACK_REJECTED;
            return false;
        }
        decoder->length = 0U;
        decoder->header_ready = false;
    }
    return true;
}

bool cw_decoder_finish(CwDecoder *decoder) {
    if (decoder == NULL) {
        return false;
    }
    if (decoder->error != CW_DECODER_OK) {
        return false;
    }
    if (decoder->length != 0U) {
        decoder->error = CW_DECODER_TRUNCATED;
        return false;
    }
    return true;
}

CwDecoderError cw_decoder_error(const CwDecoder *decoder) {
    return decoder == NULL ? CW_DECODER_BAD_ARGUMENT : decoder->error;
}

const char *cw_decoder_error_string(CwDecoderError error) {
    switch (error) {
    case CW_DECODER_OK:
        return "ok";
    case CW_DECODER_BAD_ARGUMENT:
        return "bad argument";
    case CW_DECODER_NO_MEMORY:
        return "out of memory";
    case CW_DECODER_BAD_MAGIC:
        return "wrong magic";
    case CW_DECODER_UNSUPPORTED_VERSION:
        return "unsupported version";
    case CW_DECODER_UNKNOWN_MANDATORY_MESSAGE:
        return "unknown mandatory message";
    case CW_DECODER_PAYLOAD_TOO_LARGE:
        return "payload exceeds maximum";
    case CW_DECODER_INVALID_PAYLOAD:
        return "invalid payload";
    case CW_DECODER_CALLBACK_REJECTED:
        return "message callback rejected payload";
    case CW_DECODER_TRUNCATED:
        return "truncated TCP stream";
    default:
        return "unknown decoder error";
    }
}

void cw_buffer_init(CwBuffer *buffer) {
    if (buffer != NULL) {
        *buffer = (CwBuffer){NULL, 0U, 0U};
    }
}

void cw_buffer_destroy(CwBuffer *buffer) {
    if (buffer != NULL) {
        free(buffer->data);
        cw_buffer_init(buffer);
    }
}

bool cw_message_encode(
    CwBuffer *buffer,
    uint16_t type,
    uint32_t flags,
    uint64_t sequence,
    const uint8_t *payload,
    uint32_t payload_length) {
    CwHeader header;
    size_t total_length;

    if (buffer == NULL || (payload_length != 0U && payload == NULL) ||
        payload_length > CW_MAX_PAYLOAD) {
        return false;
    }
    header = (CwHeader){
        CW_PROTOCOL_MAGIC,
        CW_PROTOCOL_VERSION,
        type,
        flags,
        payload_length,
        sequence,
    };
    if (!cw_message_is_valid(&header, payload)) {
        return false;
    }
    total_length = (size_t)CW_HEADER_SIZE + (size_t)payload_length;
    if (!reserve_bytes(&buffer->data, &buffer->capacity, total_length)) {
        return false;
    }
    if (!cw_header_encode(buffer->data, &header)) {
        return false;
    }
    if (payload_length != 0U) {
        memcpy(buffer->data + CW_HEADER_SIZE, payload, payload_length);
    }
    buffer->length = total_length;
    return true;
}

bool cw_decode_hello(const uint8_t *payload, uint32_t length, CwHello *out) {
    if (payload == NULL || out == NULL || length != CW_HELLO_PAYLOAD_SIZE) {
        return false;
    }
    *out = (CwHello){cw_load_u16_le(payload), cw_load_u16_le(payload + 2U),
                     cw_load_u32_le(payload + 4U)};
    return true;
}

bool cw_decode_hello_ack(const uint8_t *payload, uint32_t length, CwHelloAck *out) {
    if (payload == NULL || out == NULL || length != CW_HELLO_ACK_PAYLOAD_SIZE) {
        return false;
    }
    *out = (CwHelloAck){cw_load_u16_le(payload), cw_load_u32_le(payload + 4U)};
    return true;
}

bool cw_decode_window_create(const uint8_t *payload, uint32_t length, CwWindowCreate *out) {
    if (payload == NULL || out == NULL || length != CW_WINDOW_CREATE_PAYLOAD_SIZE) {
        return false;
    }
    *out = (CwWindowCreate){cw_load_u64_le(payload), cw_load_u32_le(payload + 8U),
                            cw_load_u32_le(payload + 12U), cw_load_u64_le(payload + 16U)};
    return true;
}

bool cw_decode_window_frame(const uint8_t *payload, uint32_t length, CwWindowFrame *out) {
    if (!valid_frame_payload(payload, length) || out == NULL) {
        return false;
    }
    *out = (CwWindowFrame){
        cw_load_u64_le(payload), cw_load_u64_le(payload + 8U),
        cw_load_u32_le(payload + 16U),
        cw_load_u32_le(payload + 20U),
        cw_load_u32_le(payload + 24U),
        cw_load_u32_le(payload + 28U),
        payload + CW_WINDOW_FRAME_PREFIX_SIZE,
        (uint64_t)length - CW_WINDOW_FRAME_PREFIX_SIZE,
    };
    return true;
}

bool cw_decode_window_damage(const uint8_t *payload, uint32_t length,
                             CwWindowDamage *out)
{
    if (!valid_damage_payload(payload, length) || out == NULL) {
        return false;
    }
    *out = (CwWindowDamage){
        cw_load_u64_le(payload),
        cw_load_u64_le(payload + 8U),
        cw_load_u64_le(payload + 16U),
        cw_load_u32_le(payload + 24U),
        payload + CW_WINDOW_DAMAGE_PREFIX_SIZE,
        length - CW_WINDOW_DAMAGE_PREFIX_SIZE,
    };
    return true;
}

bool cw_window_damage_rect_at(const CwWindowDamage *damage, uint32_t index,
                              CwDamageRect *out)
{
    uint64_t offset = 0U;
    uint32_t current;

    if (damage == NULL || out == NULL || index >= damage->rect_count) {
        return false;
    }
    for (current = 0U; current <= index; ++current) {
        uint32_t width;
        uint32_t height;
        uint64_t pixel_bytes;

        if (offset > damage->rect_data_length ||
            (uint64_t)damage->rect_data_length - offset < CW_WINDOW_DAMAGE_RECT_PREFIX_SIZE) {
            return false;
        }
        width = cw_load_u32_le(damage->rect_data + offset + 8U);
        height = cw_load_u32_le(damage->rect_data + offset + 12U);
        if (!damage_rect_pixel_bytes(width, height, &pixel_bytes) ||
            pixel_bytes > (uint64_t)damage->rect_data_length - offset -
                          CW_WINDOW_DAMAGE_RECT_PREFIX_SIZE) {
            return false;
        }
        if (current == index) {
            *out = (CwDamageRect){
                cw_load_i32_le(damage->rect_data + offset),
                cw_load_i32_le(damage->rect_data + offset + 4U),
                width, height, width * 4U,
                damage->rect_data + offset + CW_WINDOW_DAMAGE_RECT_PREFIX_SIZE,
                pixel_bytes,
            };
            return true;
        }
        offset += CW_WINDOW_DAMAGE_RECT_PREFIX_SIZE + pixel_bytes;
    }
    return false;
}

bool cw_decode_window_frame_request(const uint8_t *payload, uint32_t length,
                                    CwWindowFrameRequest *out)
{
    if (payload == NULL || out == NULL || length != CW_WINDOW_FRAME_REQUEST_PAYLOAD_SIZE) {
        return false;
    }
    *out = (CwWindowFrameRequest){cw_load_u64_le(payload),
                                  cw_load_u64_le(payload + 8U)};
    return true;
}

bool cw_decode_window_resize(const uint8_t *payload, uint32_t length,
                             CwWindowResize *out)
{
    if (payload == NULL || out == NULL || length != CW_WINDOW_RESIZE_PAYLOAD_SIZE ||
        !valid_surface(cw_load_u32_le(payload + 8U), cw_load_u32_le(payload + 12U))) {
        return false;
    }
    *out = (CwWindowResize){cw_load_u64_le(payload), cw_load_u32_le(payload + 8U),
                            cw_load_u32_le(payload + 12U)};
    return true;
}

bool cw_decode_window_present(const uint8_t *payload, uint32_t length, CwWindowPresent *out) {
    if (!valid_present_payload(payload, length) || out == NULL) {
        return false;
    }
    *out = (CwWindowPresent){
        cw_load_u64_le(payload), cw_load_u64_le(payload + 8U),
        cw_load_i32_le(payload + 16U), cw_load_i32_le(payload + 20U),
        cw_load_i32_le(payload + 24U), cw_load_i32_le(payload + 28U),
        cw_load_i32_le(payload + 32U), cw_load_i32_le(payload + 36U),
        cw_load_i32_le(payload + 40U), cw_load_i32_le(payload + 44U),
        cw_load_u32_le(payload + 48U) != 0U,
    };
    return true;
}

bool cw_decode_window_present_ack(
    const uint8_t *payload, uint32_t length, CwWindowPresentAck *out) {
    if (payload == NULL || out == NULL || length != CW_WINDOW_PRESENT_ACK_PAYLOAD_SIZE) {
        return false;
    }
    *out = (CwWindowPresentAck){cw_load_u64_le(payload), cw_load_u64_le(payload + 8U)};
    return true;
}

bool cw_decode_window_activate(const uint8_t *payload, uint32_t length,
                               CwWindowActivate *out)
{
    if (payload == NULL || out == NULL || length != CW_WINDOW_ACTIVATE_PAYLOAD_SIZE ||
        cw_load_u64_le(payload) == 0U) {
        return false;
    }
    *out = (CwWindowActivate){cw_load_u64_le(payload)};
    return true;
}

bool cw_decode_output_config(const uint8_t *payload, uint32_t length,
                             CwOutputConfig *out)
{
    if (payload == NULL || out == NULL || length != CW_OUTPUT_CONFIG_PAYLOAD_SIZE ||
        cw_load_u32_le(payload + 8U) == 0U ||
        cw_load_u32_le(payload + 12U) == 0U ||
        cw_load_u32_le(payload + 16U) == 0U ||
        cw_load_u32_le(payload + 20U) == 0U) {
        return false;
    }
    *out = (CwOutputConfig){
        cw_load_i32_le(payload), cw_load_i32_le(payload + 4U),
        cw_load_u32_le(payload + 8U), cw_load_u32_le(payload + 12U),
        cw_load_u32_le(payload + 16U), cw_load_u32_le(payload + 20U),
    };
    return true;
}

bool cw_decode_pointer_location(const uint8_t *payload, uint32_t length, CwPointerLocation *out) {
    if (payload == NULL || out == NULL || length != CW_POINTER_LOCATION_PAYLOAD_SIZE) {
        return false;
    }
    *out = (CwPointerLocation){
        cw_load_u64_le(payload), cw_load_u64_le(payload + 8U),
        cw_load_i32_le(payload + 16U), cw_load_i32_le(payload + 20U),
        cw_load_i32_le(payload + 24U), cw_load_i32_le(payload + 28U),
        cw_load_u64_le(payload + 32U),
    };
    return true;
}

bool cw_decode_pointer_motion(const uint8_t *payload, uint32_t length, CwPointerMotion *out) {
    if (payload == NULL || out == NULL || length != CW_POINTER_MOTION_PAYLOAD_SIZE ||
        cw_load_u32_le(payload + 44U) != 0U ||
        !cw_decode_pointer_location(payload, CW_POINTER_LOCATION_PAYLOAD_SIZE, &out->location)) {
        return false;
    }
    out->button_mask = cw_load_u32_le(payload + 40U);
    return true;
}

bool cw_decode_pointer_button(
    const uint8_t *payload, uint32_t length, CwPointerButtonEvent *out) {
    if (!valid_pointer_button_payload(payload, length) || out == NULL ||
        !cw_decode_pointer_location(payload, CW_POINTER_LOCATION_PAYLOAD_SIZE, &out->location)) {
        return false;
    }
    out->button = cw_load_u32_le(payload + 40U);
    out->state = cw_load_u32_le(payload + 44U);
    return true;
}

bool cw_decode_pointer_wheel(const uint8_t *payload, uint32_t length, CwPointerWheel *out) {
    if (payload == NULL || out == NULL || length != CW_POINTER_WHEEL_PAYLOAD_SIZE ||
        !cw_decode_pointer_location(payload, CW_POINTER_LOCATION_PAYLOAD_SIZE, &out->location)) {
        return false;
    }
    out->delta_x = cw_load_i32_le(payload + 40U);
    out->delta_y = cw_load_i32_le(payload + 44U);
    return true;
}

bool cw_decode_pointer_capture_lost(
    const uint8_t *payload, uint32_t length, CwPointerCaptureLost *out) {
    if (payload == NULL || out == NULL || length != CW_POINTER_CAPTURE_LOST_PAYLOAD_SIZE) {
        return false;
    }
    *out = (CwPointerCaptureLost){cw_load_u64_le(payload), cw_load_u64_le(payload + 8U),
                                  cw_load_u64_le(payload + 16U)};
    return true;
}

#include "../common/protocol.h"

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

typedef struct {
    unsigned count;
    uint16_t types[8];
    uint64_t sequences[8];
} SeenMessages;

static bool collect_message(
    void *context,
    const CwHeader *header,
    const uint8_t *payload) {
    SeenMessages *seen = context;

    (void)payload;
    if (seen->count >= 8U) {
        return false;
    }
    seen->types[seen->count] = header->type;
    seen->sequences[seen->count] = header->sequence;
    ++seen->count;
    return true;
}

static void encode_hello(CwBuffer *out, uint64_t sequence) {
    uint8_t payload[8];

    cw_store_u16_le(payload, 1U);
    cw_store_u16_le(payload + 2U, 1U);
    cw_store_u32_le(payload + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
    if (!cw_message_encode(out, CW_MESSAGE_HELLO, 0U, sequence, payload, sizeof(payload))) {
        fail(__func__, __LINE__, "cw_message_encode hello");
    }
}

static void encode_test_frame(CwBuffer *out, uint64_t sequence) {
    uint8_t payload[96]; /* Prefix 24 + 3 * 6 * 4 BGRA bytes = 96. */
    unsigned index;

    memset(payload, 0, sizeof(payload));
    cw_store_u64_le(payload, 1U);
    cw_store_u32_le(payload + 8U, 3U);
    cw_store_u32_le(payload + 12U, 6U);
    cw_store_u32_le(payload + 16U, 12U);
    cw_store_u32_le(payload + 20U, CW_PIXEL_FORMAT_BGRA8888);
    for (index = 24U; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)index;
    }
    if (!cw_message_encode(out, CW_MESSAGE_WINDOW_FRAME, 0U, sequence,
                           payload, sizeof(payload))) {
        fail(__func__, __LINE__, "cw_message_encode frame");
    }
}

static void feed_fragmented(
    const uint8_t *bytes,
    size_t length,
    const size_t *fragments,
    size_t fragment_count,
    SeenMessages *seen) {
    CwDecoder decoder;
    size_t offset = 0U;
    size_t index;

    cw_decoder_init(&decoder);
    for (index = 0U; index < fragment_count; ++index) {
        CHECK(fragments[index] <= length - offset);
        CHECK(cw_decoder_feed(&decoder, bytes + offset, fragments[index],
                              collect_message, seen));
        offset += fragments[index];
    }
    CHECK(offset == length);
    CHECK(cw_decoder_finish(&decoder));
    cw_decoder_destroy(&decoder);
}

static void test_header_and_full_message(void) {
    CwBuffer hello;
    CwHeader header;
    SeenMessages seen = {0};
    size_t fragment;

    cw_buffer_init(&hello);
    encode_hello(&hello, 7U);
    CHECK(hello.length == 32U);
    CHECK(cw_header_decode(hello.data, &header));
    CHECK(header.magic == CW_PROTOCOL_MAGIC);
    CHECK(header.version == CW_PROTOCOL_VERSION);
    CHECK(header.type == CW_MESSAGE_HELLO);
    CHECK(header.sequence == 7U);
    {
        uint8_t signed_bytes[4];

        cw_store_i32_le(signed_bytes, INT32_MIN);
        CHECK(cw_load_i32_le(signed_bytes) == INT32_MIN);
        cw_store_i32_le(signed_bytes, -1);
        CHECK(cw_load_i32_le(signed_bytes) == -1);
        cw_store_i32_le(signed_bytes, INT32_MAX);
        CHECK(cw_load_i32_le(signed_bytes) == INT32_MAX);
    }
    fragment = hello.length;
    feed_fragmented(hello.data, hello.length, &fragment, 1U, &seen);
    CHECK(seen.count == 1U);
    CHECK(seen.types[0] == CW_MESSAGE_HELLO);
    CHECK(seen.sequences[0] == 7U);
    cw_buffer_destroy(&hello);
}

static void test_tcp_fragmentation(void) {
    CwBuffer frame;
    SeenMessages seen;
    size_t fragments[3];
    size_t index;

    cw_buffer_init(&frame);
    encode_test_frame(&frame, 42U);
    CHECK(frame.length == 120U);

    seen = (SeenMessages){0};
    fragments[0] = 1U;
    fragments[1] = 119U;
    feed_fragmented(frame.data, frame.length, fragments, 2U, &seen);
    CHECK(seen.count == 1U && seen.sequences[0] == 42U);

    seen = (SeenMessages){0};
    fragments[0] = 23U;
    fragments[1] = 1U;
    fragments[2] = 96U;
    feed_fragmented(frame.data, frame.length, fragments, 3U, &seen);
    CHECK(seen.count == 1U && seen.types[0] == CW_MESSAGE_WINDOW_FRAME);

    {
        CwDecoder decoder;

        cw_decoder_init(&decoder);
        seen = (SeenMessages){0};
        for (index = 0U; index < frame.length; ++index) {
            CHECK(cw_decoder_feed(&decoder, frame.data + index, 1U,
                                  collect_message, &seen));
        }
        CHECK(cw_decoder_finish(&decoder));
        CHECK(seen.count == 1U && seen.sequences[0] == 42U);
        cw_decoder_destroy(&decoder);
    }
    cw_buffer_destroy(&frame);
}

static void test_coalescing_and_mixed_stream(void) {
    CwBuffer first;
    CwBuffer second;
    CwBuffer third;
    uint8_t stream[96];
    SeenMessages seen = {0};
    CwDecoder decoder;
    size_t split;

    cw_buffer_init(&first);
    cw_buffer_init(&second);
    cw_buffer_init(&third);
    encode_hello(&first, 1U);
    encode_hello(&second, 2U);
    encode_hello(&third, 3U);
    memcpy(stream, first.data, first.length);
    memcpy(stream + first.length, second.data, second.length);
    memcpy(stream + first.length + second.length, third.data, third.length);

    cw_decoder_init(&decoder);
    CHECK(cw_decoder_feed(&decoder, stream, sizeof(stream), collect_message, &seen));
    CHECK(cw_decoder_finish(&decoder));
    CHECK(seen.count == 3U);
    CHECK(seen.sequences[0] == 1U && seen.sequences[1] == 2U && seen.sequences[2] == 3U);
    cw_decoder_destroy(&decoder);

    seen = (SeenMessages){0};
    cw_decoder_init(&decoder);
    split = first.length + 11U;
    CHECK(cw_decoder_feed(&decoder, stream, split, collect_message, &seen));
    CHECK(seen.count == 1U && seen.sequences[0] == 1U);
    CHECK(cw_decoder_feed(&decoder, stream + split, sizeof(stream) - split,
                          collect_message, &seen));
    CHECK(cw_decoder_finish(&decoder));
    CHECK(seen.count == 3U && seen.sequences[2] == 3U);
    cw_decoder_destroy(&decoder);

    cw_buffer_destroy(&first);
    cw_buffer_destroy(&second);
    cw_buffer_destroy(&third);
}

static void store_raw_header(
    uint8_t bytes[CW_HEADER_SIZE],
    uint32_t magic,
    uint16_t version,
    uint16_t type,
    uint32_t payload_length) {
    cw_store_u32_le(bytes, magic);
    cw_store_u16_le(bytes + 4U, version);
    cw_store_u16_le(bytes + 6U, type);
    cw_store_u32_le(bytes + 8U, 0U);
    cw_store_u32_le(bytes + 12U, payload_length);
    cw_store_u64_le(bytes + 16U, 9U);
}

static void expect_decode_error(
    const uint8_t *bytes,
    size_t length,
    CwDecoderError expected) {
    CwDecoder decoder;
    SeenMessages seen = {0};

    cw_decoder_init(&decoder);
    CHECK(!cw_decoder_feed(&decoder, bytes, length, collect_message, &seen));
    CHECK(cw_decoder_error(&decoder) == expected);
    cw_decoder_destroy(&decoder);
}

static void test_invalid_messages(void) {
    uint8_t header[CW_HEADER_SIZE];
    uint8_t frame[CW_HEADER_SIZE + 39U];
    CwDecoder decoder;
    SeenMessages seen = {0};

    store_raw_header(header, 0U, CW_PROTOCOL_VERSION, CW_MESSAGE_HELLO, 8U);
    expect_decode_error(header, sizeof(header), CW_DECODER_BAD_MAGIC);
    store_raw_header(header, CW_PROTOCOL_MAGIC, 2U, CW_MESSAGE_HELLO, 8U);
    expect_decode_error(header, sizeof(header), CW_DECODER_UNSUPPORTED_VERSION);
    store_raw_header(header, CW_PROTOCOL_MAGIC, CW_PROTOCOL_VERSION, 99U, 0U);
    expect_decode_error(header, sizeof(header), CW_DECODER_UNKNOWN_MANDATORY_MESSAGE);
    store_raw_header(header, CW_PROTOCOL_MAGIC, CW_PROTOCOL_VERSION, CW_MESSAGE_HELLO,
                     CW_MAX_PAYLOAD + 1U);
    expect_decode_error(header, sizeof(header), CW_DECODER_PAYLOAD_TOO_LARGE);

    cw_decoder_init(&decoder);
    store_raw_header(header, CW_PROTOCOL_MAGIC, CW_PROTOCOL_VERSION, CW_MESSAGE_HELLO, 8U);
    CHECK(cw_decoder_feed(&decoder, header, CW_HEADER_SIZE - 1U, collect_message, &seen));
    CHECK(!cw_decoder_finish(&decoder));
    CHECK(cw_decoder_error(&decoder) == CW_DECODER_TRUNCATED);
    cw_decoder_destroy(&decoder);

    memset(frame, 0, sizeof(frame));
    store_raw_header(frame, CW_PROTOCOL_MAGIC, CW_PROTOCOL_VERSION, CW_MESSAGE_WINDOW_FRAME, 24U);
    cw_store_u64_le(frame + CW_HEADER_SIZE, 1U);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 8U, 2U);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 12U, 2U);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 16U, 7U); /* Smaller than width * 4. */
    cw_store_u32_le(frame + CW_HEADER_SIZE + 20U, CW_PIXEL_FORMAT_BGRA8888);
    expect_decode_error(frame, sizeof(frame), CW_DECODER_INVALID_PAYLOAD);

    cw_store_u32_le(frame + CW_HEADER_SIZE + 8U, UINT32_MAX);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 12U, UINT32_MAX);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 16U, UINT32_MAX);
    expect_decode_error(frame, sizeof(frame), CW_DECODER_INVALID_PAYLOAD);

    store_raw_header(frame, CW_PROTOCOL_MAGIC, CW_PROTOCOL_VERSION, CW_MESSAGE_WINDOW_FRAME, 39U);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 8U, 2U);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 12U, 2U);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 16U, 8U);
    cw_store_u32_le(frame + CW_HEADER_SIZE + 20U, CW_PIXEL_FORMAT_BGRA8888);
    expect_decode_error(frame, CW_HEADER_SIZE + 39U, CW_DECODER_INVALID_PAYLOAD);
}

static void test_pointer_payloads(void) {
    uint8_t motion[48] = {0};
    uint8_t button[48] = {0};
    uint8_t wheel[48] = {0};
    CwPointerMotion decoded_motion;
    CwPointerButtonEvent decoded_button;
    CwPointerWheel decoded_wheel;

    cw_store_u64_le(motion, 1U);
    cw_store_u64_le(motion + 8U, 42U);
    cw_store_i32_le(motion + 16U, -12);
    cw_store_i32_le(motion + 20U, 211);
    cw_store_i32_le(motion + 24U, -5);
    cw_store_i32_le(motion + 28U, 511);
    cw_store_u64_le(motion + 32U, 99U);
    cw_store_u32_le(motion + 40U, 1U);
    CHECK(cw_decode_pointer_motion(motion, sizeof(motion), &decoded_motion));
    CHECK(decoded_motion.location.client_x == -12 && decoded_motion.location.output_x == -5);
    CHECK(decoded_motion.button_mask == 1U);

    memcpy(button, motion, sizeof(button));
    cw_store_u32_le(button + 40U, CW_POINTER_BUTTON_MIDDLE);
    cw_store_u32_le(button + 44U, CW_BUTTON_PRESSED);
    CHECK(cw_decode_pointer_button(button, sizeof(button), &decoded_button));
    CHECK(decoded_button.button == CW_POINTER_BUTTON_MIDDLE &&
          decoded_button.state == CW_BUTTON_PRESSED && decoded_button.location.client_x == -12);
    cw_store_u32_le(button + 40U, 0U);
    CHECK(!cw_decode_pointer_button(button, sizeof(button), &decoded_button));

    memcpy(wheel, motion, sizeof(wheel));
    cw_store_i32_le(wheel + 40U, -120);
    cw_store_i32_le(wheel + 44U, 240);
    CHECK(cw_decode_pointer_wheel(wheel, sizeof(wheel), &decoded_wheel));
    CHECK(decoded_wheel.delta_x == -120 && decoded_wheel.delta_y == 240);
}

int main(void) {
    test_header_and_full_message();
    test_tcp_fragmentation();
    test_coalescing_and_mixed_stream();
    test_invalid_messages();
    test_pointer_payloads();
    printf("protocol tests: PASS\n");
    printf("cases: %lu\n", cases);
    return EXIT_SUCCESS;
}

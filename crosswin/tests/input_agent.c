#define _POSIX_C_SOURCE 200809L

#include "../common/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    unsigned cases;
    unsigned frames;
    bool received_initial_presentation;
    bool received_drag_presentation;
    bool done;
} InputAgent;

static void fail(const char *reason) {
    fprintf(stderr, "FAIL: input-agent: %s\n", reason);
    exit(EXIT_FAILURE);
}

#define CHECK(agent, condition)             \
    do {                                    \
        ++(agent)->cases;                   \
        if (!(condition)) {                 \
            fail(#condition);               \
        }                                   \
    } while (0)

static bool send_all(int fd, const uint8_t *bytes, size_t length) {
    size_t offset = 0U;

    while (offset < length) {
        const ssize_t sent = send(fd, bytes + offset, length - offset, 0);

        if (sent > 0) {
            offset += (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool send_message(InputAgent *agent, uint16_t type, uint64_t sequence,
                         const uint8_t *payload, uint32_t length) {
    CwBuffer encoded;
    bool ok;

    cw_buffer_init(&encoded);
    ok = cw_message_encode(&encoded, type, 0U, sequence, payload, length) &&
         send_all(agent->fd, encoded.data, encoded.length);
    cw_buffer_destroy(&encoded);
    return ok;
}

static bool send_hello(InputAgent *agent) {
    uint8_t payload[8];

    cw_store_u16_le(payload, CW_PROTOCOL_VERSION);
    cw_store_u16_le(payload + 2U, CW_PROTOCOL_VERSION);
    cw_store_u32_le(payload + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
    return send_message(agent, CW_MESSAGE_HELLO, 1U, payload, sizeof(payload));
}

static void store_location(uint8_t payload[40], uint64_t presentation_sequence,
                           int32_t client_x, int32_t client_y,
                           int32_t output_x, int32_t output_y) {
    cw_store_u64_le(payload, 1U);
    cw_store_u64_le(payload + 8U, presentation_sequence);
    cw_store_i32_le(payload + 16U, client_x);
    cw_store_i32_le(payload + 20U, client_y);
    cw_store_i32_le(payload + 24U, output_x);
    cw_store_i32_le(payload + 28U, output_y);
    cw_store_u64_le(payload + 32U, 100U);
}

static bool send_button(InputAgent *agent, uint64_t wire_sequence,
                        uint64_t presentation_sequence, uint32_t state,
                        int32_t client_x, int32_t client_y,
                        int32_t output_x, int32_t output_y) {
    uint8_t payload[48] = {0};

    store_location(payload, presentation_sequence, client_x, client_y, output_x, output_y);
    cw_store_u32_le(payload + 40U, CW_POINTER_BUTTON_LEFT);
    cw_store_u32_le(payload + 44U, state);
    return send_message(agent, CW_MESSAGE_POINTER_BUTTON, wire_sequence, payload, sizeof(payload));
}

static bool send_motion(InputAgent *agent, uint64_t wire_sequence,
                        uint64_t presentation_sequence,
                        int32_t client_x, int32_t client_y,
                        int32_t output_x, int32_t output_y) {
    uint8_t payload[48] = {0};

    store_location(payload, presentation_sequence, client_x, client_y, output_x, output_y);
    cw_store_u32_le(payload + 40U, 1U);
    return send_message(agent, CW_MESSAGE_POINTER_MOTION, wire_sequence, payload, sizeof(payload));
}

static bool send_ack(InputAgent *agent, uint64_t wire_sequence, uint64_t presentation_sequence) {
    uint8_t payload[16];

    cw_store_u64_le(payload, 1U);
    cw_store_u64_le(payload + 8U, presentation_sequence);
    return send_message(agent, CW_MESSAGE_WINDOW_PRESENT_ACK, wire_sequence, payload, sizeof(payload));
}

static bool on_message(void *context, const CwHeader *header, const uint8_t *payload) {
    InputAgent *agent = context;

    switch (header->type) {
    case CW_MESSAGE_HELLO_ACK:
        CHECK(agent, header->payload_length == 8U);
        return true;
    case CW_MESSAGE_WINDOW_CREATE:
        CHECK(agent, header->payload_length == 16U);
        return true;
    case CW_MESSAGE_WINDOW_FRAME: {
        CwWindowFrame frame;

        CHECK(agent, cw_decode_window_frame(payload, header->payload_length, &frame));
        ++agent->frames;
        if (agent->frames == 2U) {
            const size_t crosshair = (size_t)211U * frame.stride + (size_t)353U * 4U;

            /* Click client=(133,211), source=(220,0) must alter surface=(353,211). */
            CHECK(agent, frame.pixels[crosshair] == 0U && frame.pixels[crosshair + 1U] == 255U &&
                         frame.pixels[crosshair + 2U] == 255U && frame.pixels[crosshair + 3U] == 255U);
            CHECK(agent, send_motion(agent, 4U, 42U, 233, 211, 233, 511));
        }
        return true;
    }
    case CW_MESSAGE_WINDOW_PRESENT: {
        CwWindowPresent present;

        CHECK(agent, cw_decode_window_present(payload, header->payload_length, &present));
        if (!agent->received_initial_presentation) {
            CHECK(agent, present.presentation_sequence == 42U && present.source_x == 220 &&
                         present.source_y == 0 && present.source_w == 580 &&
                         present.source_h == 600 && present.destination_x == 0 &&
                         present.destination_y == 300);
            agent->received_initial_presentation = true;
            CHECK(agent, send_ack(agent, 2U, 42U));
            CHECK(agent, send_button(agent, 3U, 42U, CW_BUTTON_PRESSED,
                                     133, 211, 133, 511));
            return true;
        }
        CHECK(agent, !agent->received_drag_presentation);
        CHECK(agent, present.presentation_sequence == 43U && present.source_x == 120 &&
                     present.source_y == 0 && present.source_w == 680 &&
                     present.source_h == 600 && present.destination_x == 0 &&
                     present.destination_y == 300);
        agent->received_drag_presentation = true;
        CHECK(agent, send_ack(agent, 5U, 43U));
        CHECK(agent, send_button(agent, 6U, 43U, CW_BUTTON_RELEASED,
                                 233, 211, 233, 511));
        agent->done = true;
        shutdown(agent->fd, SHUT_RDWR);
        return true;
    }
    default:
        return false;
    }
}

static int connect_to_server(uint16_t port) {
    struct sockaddr_in address;
    unsigned attempt;

    address = (struct sockaddr_in){0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        return -1;
    }
    for (attempt = 0U; attempt < 100U; ++attempt) {
        const struct timespec retry_delay = {0, 10000000L};
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd >= 0 && connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
            return fd;
        }
        if (fd >= 0) {
            close(fd);
        }
        (void)nanosleep(&retry_delay, NULL);
    }
    return -1;
}

int main(void) {
    CwDecoder decoder;
    InputAgent agent = {connect_to_server(44602U), 0U, 0U, false, false, false};
    uint8_t bytes[4096];

    if (agent.fd < 0 || !send_hello(&agent)) {
        fail("connect or hello");
    }
    cw_decoder_init(&decoder);
    while (!agent.done) {
        const ssize_t received = recv(agent.fd, bytes, sizeof(bytes), 0);

        if (received <= 0 ||
            !cw_decoder_feed(&decoder, bytes, (size_t)received, on_message, &agent)) {
            fail("receive/decode");
        }
    }
    CHECK(&agent, cw_decoder_finish(&decoder));
    cw_decoder_destroy(&decoder);
    close(agent.fd);
    printf("pointer round-trip integration: PASS\n");
    printf("cases: %u\n", agent.cases);
    return EXIT_SUCCESS;
}

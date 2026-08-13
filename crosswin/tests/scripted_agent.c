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
    unsigned presents;
    bool hello_ack;
    bool created;
    bool frame_received;
    bool destroyed;
} ScriptedAgent;

static const CwWindowPresent expected_presentations[] = {
    {1U, 1U, 0, 0, 800, 600, 300, 200, 800, 600, true},
    {1U, 2U, 100, 0, 700, 600, 0, 200, 700, 600, true},
    {1U, 3U, 220, 0, 580, 600, 0, 300, 580, 600, true},
    {1U, 4U, 400, 100, 400, 400, 200, 100, 400, 400, true},
    {1U, 5U, 0, 0, 0, 0, 0, 0, 0, 0, false},
    {1U, 6U, 0, 0, 800, 600, 300, 200, 800, 600, true},
};

static void fail(const char *reason) {
    fprintf(stderr, "FAIL: scripted-agent: %s\n", reason);
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
        ssize_t sent = send(fd, bytes + offset, length - offset, 0);

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

static bool send_simple_message(
    ScriptedAgent *agent,
    uint16_t type,
    uint64_t sequence,
    const uint8_t *payload,
    uint32_t length) {
    CwBuffer encoded;
    bool ok;

    cw_buffer_init(&encoded);
    ok = cw_message_encode(&encoded, type, 0U, sequence, payload, length) &&
         send_all(agent->fd, encoded.data, encoded.length);
    cw_buffer_destroy(&encoded);
    return ok;
}

static bool send_hello(ScriptedAgent *agent) {
    uint8_t payload[8];

    cw_store_u16_le(payload, CW_PROTOCOL_VERSION);
    cw_store_u16_le(payload + 2U, CW_PROTOCOL_VERSION);
    cw_store_u32_le(payload + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
    return send_simple_message(agent, CW_MESSAGE_HELLO, 1U, payload, sizeof(payload));
}

static bool send_present_ack(ScriptedAgent *agent, uint64_t window_id, uint64_t sequence) {
    uint8_t payload[16];

    cw_store_u64_le(payload, window_id);
    cw_store_u64_le(payload + 8U, sequence);
    return send_simple_message(agent, CW_MESSAGE_WINDOW_PRESENT_ACK, sequence + 1U,
                               payload, sizeof(payload));
}

static bool same_present(CwWindowPresent a, CwWindowPresent b) {
    return a.window_id == b.window_id && a.presentation_sequence == b.presentation_sequence &&
           a.source_x == b.source_x && a.source_y == b.source_y &&
           a.source_w == b.source_w && a.source_h == b.source_h &&
           a.destination_x == b.destination_x && a.destination_y == b.destination_y &&
           a.destination_w == b.destination_w && a.destination_h == b.destination_h &&
           a.visible == b.visible;
}

static bool on_message(void *context, const CwHeader *header, const uint8_t *payload) {
    ScriptedAgent *agent = context;

    switch (header->type) {
    case CW_MESSAGE_HELLO_ACK: {
        CwHelloAck ack;

        CHECK(agent, cw_decode_hello_ack(payload, header->payload_length, &ack));
        CHECK(agent, ack.selected_version == CW_PROTOCOL_VERSION);
        CHECK(agent, (ack.pixel_format_mask & CW_PIXEL_FORMAT_MASK_BGRA8888) != 0U);
        agent->hello_ack = true;
        return true;
    }
    case CW_MESSAGE_WINDOW_CREATE: {
        CwWindowCreate create;

        CHECK(agent, agent->hello_ack);
        CHECK(agent, cw_decode_window_create(payload, header->payload_length, &create));
        CHECK(agent, create.window_id == 1U && create.surface_width == 800U &&
                     create.surface_height == 600U);
        agent->created = true;
        return true;
    }
    case CW_MESSAGE_WINDOW_FRAME: {
        CwWindowFrame frame;

        CHECK(agent, agent->created);
        CHECK(agent, cw_decode_window_frame(payload, header->payload_length, &frame));
        CHECK(agent, frame.window_id == 1U && frame.width == 800U && frame.height == 600U &&
                     frame.stride == 3200U && frame.pixel_format == CW_PIXEL_FORMAT_BGRA8888);
        CHECK(agent, frame.pixel_bytes == 1920000U);
        /* Top-left is the intentional red corner marker in BGRA byte order. */
        CHECK(agent, frame.pixels[0] == 0U && frame.pixels[1] == 0U &&
                     frame.pixels[2] == 255U && frame.pixels[3] == 255U);
        agent->frame_received = true;
        return true;
    }
    case CW_MESSAGE_WINDOW_PRESENT: {
        CwWindowPresent present;

        CHECK(agent, agent->frame_received);
        CHECK(agent, cw_decode_window_present(payload, header->payload_length, &present));
        CHECK(agent, agent->presents < sizeof(expected_presentations) / sizeof(expected_presentations[0]));
        CHECK(agent, same_present(present, expected_presentations[agent->presents]));
        ++agent->presents;
        return send_present_ack(agent, present.window_id, present.presentation_sequence);
    }
    case CW_MESSAGE_WINDOW_DESTROY:
        CHECK(agent, header->payload_length == 8U && cw_load_u64_le(payload) == 1U);
        CHECK(agent, agent->presents == sizeof(expected_presentations) / sizeof(expected_presentations[0]));
        agent->destroyed = true;
        return true;
    default:
        return false;
    }
}

static int connect_to_server(const char *host, uint16_t port) {
    struct sockaddr_in address;
    unsigned attempt;

    address = (struct sockaddr_in){0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
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

int main(int argc, char **argv) {
    CwDecoder decoder;
    ScriptedAgent agent;
    uint8_t bytes[4096];
    uint16_t port = 44601U;

    if (argc == 2) {
        char *end = NULL;
        long value = strtol(argv[1], &end, 10);

        if (end == argv[1] || *end != '\0' || value < 1L || value > 65535L) {
            fail("invalid port");
        }
        port = (uint16_t)value;
    } else if (argc != 1) {
        fail("usage: scripted-agent [port]");
    }
    agent = (ScriptedAgent){connect_to_server("127.0.0.1", port), 0U, 0U,
                            false, false, false, false};
    if (agent.fd < 0 || !send_hello(&agent)) {
        fail("connect or hello");
    }
    cw_decoder_init(&decoder);
    while (!agent.destroyed) {
        ssize_t received = recv(agent.fd, bytes, sizeof(bytes), 0);

        if (received <= 0 ||
            !cw_decoder_feed(&decoder, bytes, (size_t)received, on_message, &agent)) {
            fail("receive/decode");
        }
    }
    CHECK(&agent, cw_decoder_finish(&decoder));
    cw_decoder_destroy(&decoder);
    close(agent.fd);
    printf("scripted presentation integration: PASS\n");
    printf("cases: %u\n", agent.cases);
    return EXIT_SUCCESS;
}

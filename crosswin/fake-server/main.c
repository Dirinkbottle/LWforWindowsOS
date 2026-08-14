#include "pattern.h"
#include "session.h"

#include "../common/pixel.h"
#include "../common/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    FAKE_WINDOW_ID = 1U,
    FAKE_SURFACE_WIDTH = 800U,
    FAKE_SURFACE_HEIGHT = 600U,
    FAKE_SURFACE_STRIDE = FAKE_SURFACE_WIDTH * CW_BGRA8888_BYTES_PER_PIXEL,
};

typedef struct {
    const char *listen_address;
    uint16_t port;
    bool script_stage2;
    bool trace_protocol;
    bool trace_present;
    bool trace_input;
} ServerOptions;

typedef struct {
    int client_fd;
    ServerOptions options;
    bool hello_received;
    bool awaiting_present_ack;
    uint64_t awaiting_presentation_sequence;
    bool deferred_presentation_pending;
    CwPresentationState deferred_presentation;
    size_t script_step;
    bool destroy_sent;
    uint64_t next_wire_sequence;
    uint8_t *framebuffer;
    CwFakeServerSession session;
} Server;

static const CwPresentationState scripted_presentations[] = {
    {1U, {0, 0, 800, 600}, {300, 200, 800, 600}, true},
    {2U, {100, 0, 700, 600}, {0, 200, 700, 600}, true},
    {3U, {220, 0, 580, 600}, {0, 300, 580, 600}, true},
    {4U, {400, 100, 400, 400}, {200, 100, 400, 400}, true},
    {5U, {0, 0, 0, 0}, {0, 0, 0, 0}, false},
    {6U, {0, 0, 800, 600}, {300, 200, 800, 600}, true},
};

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s [--listen IPv4] [--port 44600] [--script-stage2 | --interactive] "
            "[--trace-protocol] [--trace-present] [--trace-input]\n",
            program);
}

static bool parse_port(const char *text, uint16_t *out) {
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1L || value > 65535L) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool parse_options(int argc, char **argv, ServerOptions *options) {
    int index;

    *options = (ServerOptions){"0.0.0.0", 44600U, true, false, false, false};
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--listen") == 0 && index + 1 < argc) {
            options->listen_address = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (!parse_port(argv[++index], &options->port)) {
                return false;
            }
        } else if (strcmp(argv[index], "--script-stage2") == 0) {
            options->script_stage2 = true;
        } else if (strcmp(argv[index], "--interactive") == 0) {
            options->script_stage2 = false;
        } else if (strcmp(argv[index], "--trace-protocol") == 0) {
            options->trace_protocol = true;
        } else if (strcmp(argv[index], "--trace-present") == 0) {
            options->trace_present = true;
        } else if (strcmp(argv[index], "--trace-input") == 0) {
            options->trace_input = true;
        } else {
            return false;
        }
    }
    return true;
}

static bool send_all(int fd, const uint8_t *bytes, size_t length) {
    size_t offset = 0U;

    while (offset < length) {
        ssize_t sent = send(fd, bytes + offset, length - offset, 0);

        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool send_message(
    Server *server,
    uint16_t type,
    const uint8_t *payload,
    uint32_t payload_length) {
    CwBuffer encoded;
    bool ok;

    cw_buffer_init(&encoded);
    ok = cw_message_encode(&encoded, type, 0U, server->next_wire_sequence++, payload, payload_length) &&
         send_all(server->client_fd, encoded.data, encoded.length);
    if (ok && server->options.trace_protocol) {
        printf("[protocol tx] type=%s wire_seq=%" PRIu64 " payload=%" PRIu32 "\n",
               cw_message_type_name(type), server->next_wire_sequence - 1U, payload_length);
    }
    cw_buffer_destroy(&encoded);
    return ok;
}

static bool send_hello_ack(Server *server) {
    uint8_t payload[8] = {0};

    cw_store_u16_le(payload, CW_PROTOCOL_VERSION);
    cw_store_u32_le(payload + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
    return send_message(server, CW_MESSAGE_HELLO_ACK, payload, sizeof(payload));
}

static bool send_create(Server *server) {
    uint8_t payload[16];

    cw_store_u64_le(payload, server->session.window_id);
    cw_store_u32_le(payload + 8U, FAKE_SURFACE_WIDTH);
    cw_store_u32_le(payload + 12U, FAKE_SURFACE_HEIGHT);
    return send_message(server, CW_MESSAGE_WINDOW_CREATE, payload, sizeof(payload));
}

static bool send_frame(Server *server) {
    const size_t pixel_bytes = (size_t)FAKE_SURFACE_STRIDE * FAKE_SURFACE_HEIGHT;
    const size_t payload_length = 32U + pixel_bytes;
    uint8_t *payload;
    bool ok;

    payload = malloc(payload_length);
    if (payload == NULL) {
        return false;
    }
    cw_store_u64_le(payload, server->session.window_id);
    cw_store_u64_le(payload + 8U, 1U);
    cw_store_u32_le(payload + 16U, FAKE_SURFACE_WIDTH);
    cw_store_u32_le(payload + 20U, FAKE_SURFACE_HEIGHT);
    cw_store_u32_le(payload + 24U, FAKE_SURFACE_STRIDE);
    cw_store_u32_le(payload + 28U, CW_PIXEL_FORMAT_BGRA8888);
    memcpy(payload + 32U, server->framebuffer, pixel_bytes);
    ok = send_message(server, CW_MESSAGE_WINDOW_FRAME, payload, (uint32_t)payload_length);
    free(payload);
    return ok;
}

static bool send_present(Server *server, const CwPresentationState *presentation) {
    uint8_t payload[56] = {0};

    if (!cw_fake_server_record_presentation(&server->session, presentation)) {
        return false;
    }
    cw_store_u64_le(payload, server->session.window_id);
    cw_store_u64_le(payload + 8U, presentation->sequence);
    cw_store_i32_le(payload + 16U, presentation->source_rect.x);
    cw_store_i32_le(payload + 20U, presentation->source_rect.y);
    cw_store_i32_le(payload + 24U, presentation->source_rect.w);
    cw_store_i32_le(payload + 28U, presentation->source_rect.h);
    cw_store_i32_le(payload + 32U, presentation->destination_rect.x);
    cw_store_i32_le(payload + 36U, presentation->destination_rect.y);
    cw_store_i32_le(payload + 40U, presentation->destination_rect.w);
    cw_store_i32_le(payload + 44U, presentation->destination_rect.h);
    cw_store_u32_le(payload + 48U, presentation->visible ? 1U : 0U);
    if (server->options.trace_present) {
        printf("[present tx] win=%" PRIu64 " seq=%" PRIu64 " src=[%" PRId32 ",%" PRId32
               " %" PRId32 "x%" PRId32 "] dst=[%" PRId32 ",%" PRId32 " %" PRId32
               "x%" PRId32 "] visible=%u\n",
               server->session.window_id, presentation->sequence,
               presentation->source_rect.x, presentation->source_rect.y,
               presentation->source_rect.w, presentation->source_rect.h,
               presentation->destination_rect.x, presentation->destination_rect.y,
               presentation->destination_rect.w, presentation->destination_rect.h,
               presentation->visible ? 1U : 0U);
    }
    if (!send_message(server, CW_MESSAGE_WINDOW_PRESENT, payload, sizeof(payload))) {
        return false;
    }
    server->awaiting_present_ack = true;
    server->awaiting_presentation_sequence = presentation->sequence;
    return true;
}

/*
 * Preserve presentation ACK ordering on TCP.  Drag motions can arrive faster
 * than HWND presentation/ACK, so only one PRESENT may be in flight.  Linux
 * still computes every canonical state; while waiting, unsent states are
 * coalesced to the newest one.  History is deliberately populated only when
 * a PRESENT is transmitted, because only those states can produce input.
 */
static bool send_or_defer_present(
    Server *server,
    const CwPresentationState *presentation) {
    if (server->awaiting_present_ack) {
        server->deferred_presentation = *presentation;
        server->deferred_presentation_pending = true;
        if (server->options.trace_present) {
            printf("[present defer] seq=%" PRIu64 " replaces pending presentation\n",
                   presentation->sequence);
        }
        return true;
    }
    return send_present(server, presentation);
}

static bool send_destroy(Server *server) {
    uint8_t payload[8];

    cw_store_u64_le(payload, server->session.window_id);
    if (!send_message(server, CW_MESSAGE_WINDOW_DESTROY, payload, sizeof(payload))) {
        return false;
    }
    server->destroy_sent = true;
    return true;
}

static bool start_after_hello(Server *server) {
    CwPresentationState initial;

    if (!send_hello_ack(server) || !send_create(server) || !send_frame(server)) {
        return false;
    }
    if (server->options.script_stage2) {
        return send_present(server, &scripted_presentations[0]);
    }
    server->session.next_presentation_sequence = 42U;
    if (!cw_fake_server_recompute_presentation(&server->session, &initial)) {
        return false;
    }
    return send_present(server, &initial);
}

static bool handle_present_ack(Server *server, const uint8_t *payload, uint32_t length) {
    CwWindowPresentAck ack;

    if (!cw_decode_window_present_ack(payload, length, &ack) ||
        ack.window_id != server->session.window_id || !server->awaiting_present_ack ||
        ack.presentation_sequence != server->awaiting_presentation_sequence) {
        return false;
    }
    server->awaiting_present_ack = false;
    if (server->options.trace_present) {
        printf("[present ack] win=%" PRIu64 " seq=%" PRIu64 "\n",
               ack.window_id, ack.presentation_sequence);
    }
    if (server->deferred_presentation_pending) {
        CwPresentationState deferred = server->deferred_presentation;

        server->deferred_presentation_pending = false;
        return send_present(server, &deferred);
    }
    if (!server->options.script_stage2) {
        return true;
    }
    ++server->script_step;
    if (server->script_step == sizeof(scripted_presentations) / sizeof(scripted_presentations[0])) {
        printf("[script] Stage 2 presentation sequence acknowledged; sending destroy\n");
        return send_destroy(server);
    }
    return send_present(server, &scripted_presentations[server->script_step]);
}

static void trace_input_result(
    const Server *server,
    const char *event,
    uint64_t presentation_sequence,
    const CwServerInputResult *result) {
    if (!server->options.trace_input) {
        return;
    }
    printf("[input rx] win=%" PRIu64 " event=%s present=%" PRIu64
           " client=(%" PRId32 ",%" PRId32 ") output=(%" PRId32 ",%" PRId32 ")\n",
           server->session.window_id, event, presentation_sequence,
           result->client.x, result->client.y, result->output.x, result->output.y);
    if (result->status == CW_SERVER_INPUT_STALE_PRESENTATION) {
        printf("[input drop] win=%" PRIu64 " presentation=%" PRIu64
               " reason=stale-presentation\n",
               server->session.window_id, presentation_sequence);
    } else if (result->surface_mapped) {
        printf("[input map] surface=(%" PRId32 ",%" PRId32 ") global=(%" PRId32
               ",%" PRId32 ")\n",
               result->surface.x, result->surface.y, result->global.x, result->global.y);
    }
}

static bool handle_pointer_location(Server *server, const char *event,
                                    const uint8_t *payload, uint32_t length) {
    CwPointerLocation location;
    CwServerInputResult result;

    if (!cw_decode_pointer_location(payload, length, &location) ||
        location.window_id != server->session.window_id) {
        return false;
    }
    (void)cw_fake_server_map_pointer(&server->session, location.presentation_sequence,
                                     (Point){location.client_x, location.client_y},
                                     (Point){location.output_x, location.output_y}, &result);
    trace_input_result(server, event, location.presentation_sequence, &result);
    return true;
}

static bool handle_pointer_motion(Server *server, const uint8_t *payload, uint32_t length) {
    CwPointerMotion motion;
    CwServerInputResult result;

    if (!cw_decode_pointer_motion(payload, length, &motion)) {
        return false;
    }
    (void)cw_fake_server_handle_motion(&server->session, &motion, &result);
    trace_input_result(server, "motion", motion.location.presentation_sequence, &result);
    if (result.presentation_changed) {
        if (server->options.trace_input) {
            printf("[grab move] window=(%" PRId32 ",%" PRId32 ")\n",
                   server->session.window_global.x, server->session.window_global.y);
        }
        return send_or_defer_present(server, &result.generated_presentation);
    }
    return true;
}

static bool handle_pointer_button(Server *server, const uint8_t *payload, uint32_t length) {
    CwPointerButtonEvent button;
    CwServerInputResult result;

    if (!cw_decode_pointer_button(payload, length, &button)) {
        return false;
    }
    (void)cw_fake_server_handle_button(&server->session, &button, &result);
    trace_input_result(server, "button", button.location.presentation_sequence, &result);
    if (server->options.trace_input && result.grab_began) {
        printf("[grab begin] window=(%" PRId32 ",%" PRId32 ") pointer=(%" PRId32
               ",%" PRId32 ")\n",
               server->session.grab_initial_window.x, server->session.grab_initial_window.y,
               result.global.x, result.global.y);
    }
    if (server->options.trace_input && result.grab_ended) {
        printf("[grab end]\n");
    }
    if (button.state == CW_BUTTON_PRESSED && result.surface_mapped) {
        if (!cw_pattern_draw_crosshair(server->framebuffer, FAKE_SURFACE_WIDTH,
                                       FAKE_SURFACE_HEIGHT, FAKE_SURFACE_STRIDE,
                                       result.surface.x, result.surface.y)) {
            return false;
        }
        return send_frame(server);
    }
    return true;
}

static bool handle_pointer_wheel(Server *server, const uint8_t *payload, uint32_t length) {
    CwPointerWheel wheel;
    CwServerInputResult result;

    if (!cw_decode_pointer_wheel(payload, length, &wheel)) {
        return false;
    }
    (void)cw_fake_server_map_pointer(&server->session, wheel.location.presentation_sequence,
                                     (Point){wheel.location.client_x, wheel.location.client_y},
                                     (Point){wheel.location.output_x, wheel.location.output_y}, &result);
    trace_input_result(server, "wheel", wheel.location.presentation_sequence, &result);
    return true;
}

static bool on_message(void *context, const CwHeader *header, const uint8_t *payload) {
    Server *server = context;

    if (server->options.trace_protocol) {
        printf("[protocol rx] type=%s wire_seq=%" PRIu64 " payload=%" PRIu32 "\n",
               cw_message_type_name(header->type), header->sequence, header->payload_length);
    }
    if (!server->hello_received) {
        CwHello hello;

        if (header->type != CW_MESSAGE_HELLO ||
            !cw_decode_hello(payload, header->payload_length, &hello) ||
            hello.min_version > CW_PROTOCOL_VERSION || hello.max_version < CW_PROTOCOL_VERSION ||
            (hello.pixel_format_mask & CW_PIXEL_FORMAT_MASK_BGRA8888) == 0U) {
            return false;
        }
        server->hello_received = true;
        return start_after_hello(server);
    }
    switch (header->type) {
    case CW_MESSAGE_WINDOW_PRESENT_ACK:
        return handle_present_ack(server, payload, header->payload_length);
    case CW_MESSAGE_POINTER_ENTER:
        return handle_pointer_location(server, "enter", payload, header->payload_length);
    case CW_MESSAGE_POINTER_LEAVE:
        return handle_pointer_location(server, "leave", payload, header->payload_length);
    case CW_MESSAGE_POINTER_MOTION:
        return handle_pointer_motion(server, payload, header->payload_length);
    case CW_MESSAGE_POINTER_BUTTON:
        return handle_pointer_button(server, payload, header->payload_length);
    case CW_MESSAGE_POINTER_WHEEL:
        return handle_pointer_wheel(server, payload, header->payload_length);
    case CW_MESSAGE_POINTER_CAPTURE_LOST:
    {
        CwPointerCaptureLost capture_lost;

        if (!cw_decode_pointer_capture_lost(payload, header->payload_length, &capture_lost) ||
            capture_lost.window_id != server->session.window_id) {
            return false;
        }
        cw_fake_server_handle_capture_lost(&server->session);
        if (server->options.trace_input) {
            printf("[capture lost] clearing fake grab and pressed buttons\n");
        }
        return true;
    }
    default:
        return false;
    }
}

static int create_listener(const ServerOptions *options) {
    struct sockaddr_in address;
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        close(fd);
        return -1;
    }
    address = (struct sockaddr_in){0};
    address.sin_family = AF_INET;
    address.sin_port = htons(options->port);
    if (inet_pton(AF_INET, options->listen_address, &address.sin_addr) != 1 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    ServerOptions options;
    CwDecoder decoder;
    int listener;
    int client;
    uint8_t receive_buffer[8192];
    Server server;

    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    listener = create_listener(&options);
    if (listener < 0) {
        perror("listen");
        return EXIT_FAILURE;
    }
    printf("fake-server listening on %s:%u mode=%s\n", options.listen_address, options.port,
           options.script_stage2 ? "stage2-script" : "interactive-stage3");
    client = accept(listener, NULL, NULL);
    close(listener);
    if (client < 0) {
        perror("accept");
        return EXIT_FAILURE;
    }

    server = (Server){
        .client_fd = client,
        .options = options,
        .next_wire_sequence = 1U,
    };
    server.framebuffer = malloc((size_t)FAKE_SURFACE_STRIDE * FAKE_SURFACE_HEIGHT);
    if (server.framebuffer == NULL ||
        !cw_pattern_generate(server.framebuffer, FAKE_SURFACE_WIDTH, FAKE_SURFACE_HEIGHT,
                             FAKE_SURFACE_STRIDE) ||
        !cw_fake_server_session_init(&server.session, FAKE_WINDOW_ID,
                                     (Rect){1920, 0, 2560, 1440},
                                     (Rect){1700, 300, 800, 600})) {
        fprintf(stderr, "fake-server initialization failed\n");
        free(server.framebuffer);
        close(client);
        return EXIT_FAILURE;
    }

    cw_decoder_init(&decoder);
    for (;;) {
        ssize_t received = recv(client, receive_buffer, sizeof(receive_buffer), 0);

        if (received > 0) {
            if (!cw_decoder_feed(&decoder, receive_buffer, (size_t)received, on_message, &server)) {
                fprintf(stderr, "protocol error: %s\n",
                        cw_decoder_error_string(cw_decoder_error(&decoder)));
                break;
            }
            continue;
        }
        if (received == 0) {
            if (!cw_decoder_finish(&decoder)) {
                fprintf(stderr, "truncated connection: %s\n",
                        cw_decoder_error_string(cw_decoder_error(&decoder)));
            }
            break;
        }
        if (errno != EINTR) {
            if (server.destroy_sent &&
                (errno == ECONNRESET || errno == ENOTCONN)) {
                printf("[script] peer closed after WINDOW_DESTROY\n");
            } else {
                perror("recv");
            }
            break;
        }
    }
    cw_fake_server_handle_disconnect(&server.session);
    cw_decoder_destroy(&decoder);
    free(server.framebuffer);
    close(client);
    return EXIT_SUCCESS;
}

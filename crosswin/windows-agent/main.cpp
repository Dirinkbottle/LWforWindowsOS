#include "protocol.hpp"
#include "proxy_window.hpp"

#include "../common/protocol.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct AgentOptions {
    const char *host = nullptr;
    std::uint16_t port = 44600U;
    bool trace_protocol = false;
    bool trace_present = false;
    bool trace_input = false;
};

class AgentApplication {
public:
    bool initialize(HINSTANCE instance, const AgentOptions &options) {
        options_ = options;
        if (!proxy_.create(instance, &protocol_)) {
            std::fprintf(stderr, "[init] CreateWindowExW/RegisterClassW failed: Win32 error=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            return false;
        }
        if (!protocol_.connect_to(options.host, options.port, proxy_.hwnd(),
                                  &AgentApplication::protocol_callback, this)) {
            return false; /* connect_to printed the precise WinSock stage/error. */
        }
        if (!protocol_.send_hello()) {
            std::fprintf(stderr, "[init] HELLO send failed: WinSock error=%d\n", WSAGetLastError());
            return false;
        }
        return true;
    }

    int run() {
        MSG message{};

        while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
            if (message.message == CW_WM_SOCKET && message.hwnd == proxy_.hwnd()) {
                if (!protocol_.on_socket_event(message.wParam, message.lParam)) {
                    std::fprintf(stderr, "[socket] connection closed or protocol processing failed\n");
                    protocol_.close();
                    proxy_.destroy();
                    return EXIT_FAILURE;
                }
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return EXIT_SUCCESS;
    }

private:
    static bool protocol_callback(void *context, const CwHeader *header, const std::uint8_t *payload) {
        return static_cast<AgentApplication *>(context)->handle_protocol_message(*header, payload);
    }

    bool handle_protocol_message(const CwHeader &header, const std::uint8_t *payload) {
        if (options_.trace_protocol) {
            std::printf("[protocol rx] type=%s wire_seq=%llu payload=%u\n",
                        cw_message_type_name(header.type),
                        static_cast<unsigned long long>(header.sequence), header.payload_length);
        }
        switch (header.type) {
        case CW_MESSAGE_HELLO_ACK: {
            CwHelloAck ack{};
            return cw_decode_hello_ack(payload, header.payload_length, &ack) &&
                   ack.selected_version == CW_PROTOCOL_VERSION &&
                   (ack.pixel_format_mask & CW_PIXEL_FORMAT_MASK_BGRA8888) != 0U;
        }
        case CW_MESSAGE_WINDOW_CREATE: {
            CwWindowCreate create{};
            return cw_decode_window_create(payload, header.payload_length, &create) &&
                   proxy_.apply_create(create);
        }
        case CW_MESSAGE_WINDOW_FRAME: {
            CwWindowFrame frame{};
            return cw_decode_window_frame(payload, header.payload_length, &frame) &&
                   proxy_.apply_frame(frame);
        }
        case CW_MESSAGE_WINDOW_PRESENT: {
            CwWindowPresent present{};
            if (!cw_decode_window_present(payload, header.payload_length, &present)) {
                return false;
            }
            if (options_.trace_present) {
                std::printf("[present apply] win=%llu seq=%llu src=[%d,%d %dx%d] "
                            "dst=[%d,%d %dx%d] hwnd=[%d,%d %dx%d]\n",
                            static_cast<unsigned long long>(present.window_id),
                            static_cast<unsigned long long>(present.presentation_sequence),
                            present.source_x, present.source_y, present.source_w, present.source_h,
                            present.destination_x, present.destination_y,
                            present.destination_w, present.destination_h,
                            present.destination_x, present.destination_y,
                            present.destination_w, present.destination_h);
            }
            return proxy_.apply_present(present);
        }
        case CW_MESSAGE_WINDOW_DESTROY: {
            const std::uint64_t window_id = cw_load_u64_le(payload);
            if (header.payload_length != 8U || !proxy_.apply_destroy(window_id)) {
                return false;
            }
            PostQuitMessage(EXIT_SUCCESS);
            return true;
        }
        default:
            return false;
        }
    }

    AgentOptions options_{};
    AgentProtocol protocol_{};
    ProxyWindow proxy_{};
};

bool parse_port(const char *text, std::uint16_t *out) {
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);

    if (end == text || *end != '\0' || value < 1L || value > 65535L) {
        return false;
    }
    *out = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_options(int argc, char **argv, AgentOptions *options) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
            options->host = argv[++index];
        } else if (std::strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            if (!parse_port(argv[++index], &options->port)) {
                return false;
            }
        } else if (std::strcmp(argv[index], "--trace-protocol") == 0) {
            options->trace_protocol = true;
        } else if (std::strcmp(argv[index], "--trace-present") == 0) {
            options->trace_present = true;
        } else if (std::strcmp(argv[index], "--trace-input") == 0) {
            options->trace_input = true;
        } else {
            return false;
        }
    }
    return options->host != nullptr;
}

void print_usage(const char *program) {
    std::fprintf(stderr,
                 "usage: %s --host <linux-ip> [--port 44600] [--trace-protocol] "
                 "[--trace-present] [--trace-input]\n",
                 program);
}

} // namespace

int main(int argc, char **argv) {
    AgentOptions options{};
    WSADATA winsock{};
    AgentApplication app{};
    int result;

    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return EXIT_FAILURE;
    }
    if (!app.initialize(GetModuleHandleW(nullptr), options)) {
        std::fprintf(stderr, "agent initialization failed\n");
        WSACleanup();
        return EXIT_FAILURE;
    }
    result = app.run();
    WSACleanup();
    return result;
}

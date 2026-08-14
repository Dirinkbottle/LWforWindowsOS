#include "protocol.hpp"
#include "proxy_window.hpp"

#include "../common/protocol.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace {

struct AgentOptions {
    const char *host = nullptr;
    std::uint16_t port = 44600U;
    bool trace_protocol = false;
    bool trace_present = false;
    bool trace_input = false;
    bool trace_frame = false;
    bool trace_damage = false;
};

class AgentApplication {
public:
    bool initialize(HINSTANCE instance, const AgentOptions &options) {
        options_ = options;
        instance_ = instance;
        if (!create_socket_window()) {
            std::fprintf(stderr, "[init] transport window creation failed: Win32 error=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
            return false;
        }
        if (!protocol_.connect_to(options.host, options.port, socket_window_,
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
        int get_message_result;

        while ((get_message_result = GetMessageW(&message, nullptr, 0U, 0U)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return get_message_result == -1 ? EXIT_FAILURE : exit_code_;
    }

private:
    static constexpr wchar_t kTransportClassName[] = L"CrossWinTransportWindow";

    static LRESULT CALLBACK transport_window_proc(HWND hwnd, UINT message,
                                                  WPARAM wparam, LPARAM lparam) {
        AgentApplication *application = nullptr;

        if (message == WM_NCCREATE) {
            const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
            application = static_cast<AgentApplication *>(create->lpCreateParams);
            if (application == nullptr) {
                return FALSE;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(application));
            return TRUE;
        }
        application = reinterpret_cast<AgentApplication *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (application != nullptr && message == CW_WM_SOCKET) {
            if (!application->protocol_.on_socket_event(wparam, lparam)) {
                std::fprintf(stderr, "[socket] connection closed or protocol processing failed\n");
                application->protocol_.close();
                application->windows_.clear();
                application->exit_code_ = EXIT_FAILURE;
                PostQuitMessage(EXIT_FAILURE);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool create_socket_window() {
        WNDCLASSW window_class{};

        window_class.lpfnWndProc = &AgentApplication::transport_window_proc;
        window_class.hInstance = instance_;
        window_class.lpszClassName = kTransportClassName;
        if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        socket_window_ = CreateWindowExW(0U, kTransportClassName, L"", 0U,
                                         0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                         instance_, this);
        return socket_window_ != nullptr;
    }

    ProxyWindow *find_window(std::uint64_t window_id) {
        const auto it = windows_.find(window_id);
        return it == windows_.end() ? nullptr : it->second.get();
    }

    bool create_proxy(const CwWindowCreate &create) {
        HWND owner = nullptr;
        const bool is_popup = create.parent_window_id != 0U;
        auto proxy = std::make_unique<ProxyWindow>();

        if (create.window_id == 0U || find_window(create.window_id) != nullptr) {
            return false;
        }
        if (is_popup) {
            ProxyWindow *parent = find_window(create.parent_window_id);
            if (parent == nullptr || parent->hwnd() == nullptr) {
                return false;
            }
            owner = parent->hwnd();
        }
        if (!proxy->create(instance_, &protocol_, owner, is_popup, options_.trace_input,
                           options_.trace_frame, options_.trace_damage) ||
            !proxy->apply_create(create)) {
            return false;
        }
        windows_.emplace(create.window_id, std::move(proxy));
        if (options_.trace_protocol) {
            std::printf("[window create] win=%llu parent=%llu popup=%u\n",
                        static_cast<unsigned long long>(create.window_id),
                        static_cast<unsigned long long>(create.parent_window_id),
                        is_popup ? 1U : 0U);
        }
        return true;
    }

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
                   create_proxy(create);
        }
        case CW_MESSAGE_WINDOW_FRAME: {
            CwWindowFrame frame{};
            return cw_decode_window_frame(payload, header.payload_length, &frame) &&
                   find_window(frame.window_id) != nullptr &&
                   find_window(frame.window_id)->apply_frame(frame);
        }
        case CW_MESSAGE_WINDOW_DAMAGE: {
            CwWindowDamage damage{};
            return cw_decode_window_damage(payload, header.payload_length, &damage) &&
                   find_window(damage.window_id) != nullptr &&
                   find_window(damage.window_id)->apply_damage(damage);
        }
        case CW_MESSAGE_WINDOW_RESIZE: {
            CwWindowResize resize{};
            return cw_decode_window_resize(payload, header.payload_length, &resize) &&
                   find_window(resize.window_id) != nullptr &&
                   find_window(resize.window_id)->apply_resize(resize);
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
            return find_window(present.window_id) != nullptr &&
                   find_window(present.window_id)->apply_present(present);
        }
        case CW_MESSAGE_WINDOW_ACTIVATE: {
            CwWindowActivate activate{};
            ProxyWindow *window;

            if (!cw_decode_window_activate(payload, header.payload_length, &activate) ||
                (window = find_window(activate.window_id)) == nullptr ||
                !window->bring_to_front()) {
                return false;
            }
            if (options_.trace_protocol) {
                std::printf("[window activate] win=%llu\n",
                            static_cast<unsigned long long>(activate.window_id));
            }
            return true;
        }
        case CW_MESSAGE_WINDOW_DESTROY: {
            const std::uint64_t window_id = cw_load_u64_le(payload);
            const auto it = windows_.find(window_id);
            if (header.payload_length != 8U || it == windows_.end() ||
                !it->second->apply_destroy(window_id)) {
                return false;
            }
            windows_.erase(it);
            if (windows_.empty()) {
                PostQuitMessage(EXIT_SUCCESS);
            }
            return true;
        }
        default:
            return false;
        }
    }

    AgentOptions options_{};
    HINSTANCE instance_ = nullptr;
    HWND socket_window_ = nullptr;
    AgentProtocol protocol_{};
    std::unordered_map<std::uint64_t, std::unique_ptr<ProxyWindow>> windows_{};
    int exit_code_ = EXIT_SUCCESS;
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
        } else if (std::strcmp(argv[index], "--trace-frame") == 0) {
            options->trace_frame = true;
        } else if (std::strcmp(argv[index], "--trace-damage") == 0) {
            options->trace_damage = true;
        } else {
            return false;
        }
    }
    return options->host != nullptr;
}

void print_usage(const char *program) {
    std::fprintf(stderr,
                 "usage: %s --host <linux-ip> [--port 44600] [--trace-protocol] "
                 "[--trace-present] [--trace-input] [--trace-frame] [--trace-damage]\n",
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

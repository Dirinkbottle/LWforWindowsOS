#include "protocol.hpp"

#include <ws2tcpip.h>

#include <array>
#include <climits>
#include <cstdio>
#include <cstring>

AgentProtocol::AgentProtocol()
    : socket_(INVALID_SOCKET),
      decoder_{},
      callback_(nullptr),
      callback_context_(nullptr),
      transmit_queue_{},
      transmit_offset_(0U),
      next_wire_sequence_(1U) {
    cw_decoder_init(&decoder_);
}

AgentProtocol::~AgentProtocol() {
    close();
    cw_decoder_destroy(&decoder_);
}

bool AgentProtocol::connect_to(
    const char *host,
    std::uint16_t port,
    HWND notification_window,
    CwMessageCallback callback,
    void *callback_context) {
    sockaddr_in address{};

    if (host == nullptr || notification_window == nullptr || callback == nullptr || connected()) {
        std::fprintf(stderr, "[init] invalid socket setup arguments\n");
        return false;
    }
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        std::fprintf(stderr, "[init] socket() failed: WinSock error=%d\n", WSAGetLastError());
        return false;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (InetPtonA(AF_INET, host, &address.sin_addr) != 1) {
        std::fprintf(stderr, "[init] --host must be a numeric IPv4 address: %s\n", host);
        close();
        return false;
    }
    if (connect(socket_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::fprintf(stderr, "[init] TCP connect to %s:%u failed: WinSock error=%d\n",
                     host, static_cast<unsigned>(port), WSAGetLastError());
        close();
        return false;
    }
    if (WSAAsyncSelect(socket_, notification_window, CW_WM_SOCKET,
                       FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
        std::fprintf(stderr, "[init] WSAAsyncSelect failed: WinSock error=%d\n", WSAGetLastError());
        close();
        return false;
    }
    callback_ = callback;
    callback_context_ = callback_context;
    return true;
}

bool AgentProtocol::connected() const {
    return socket_ != INVALID_SOCKET;
}

void AgentProtocol::close() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    transmit_queue_.clear();
    transmit_offset_ = 0U;
}

bool AgentProtocol::queue_message(
    CwMessageType type,
    const std::uint8_t *payload,
    std::uint32_t payload_length) {
    CwBuffer encoded{};
    bool ok;

    if (!connected()) {
        return false;
    }
    cw_buffer_init(&encoded);
    ok = cw_message_encode(&encoded, static_cast<std::uint16_t>(type), 0U,
                           next_wire_sequence_++, payload, payload_length);
    if (ok) {
        transmit_queue_.insert(transmit_queue_.end(), encoded.data,
                               encoded.data + encoded.length);
        ok = flush_transmit_queue();
    }
    cw_buffer_destroy(&encoded);
    return ok;
}

bool AgentProtocol::flush_transmit_queue() {
    while (transmit_offset_ < transmit_queue_.size()) {
        const auto remaining = transmit_queue_.size() - transmit_offset_;
        const auto chunk = remaining > static_cast<std::size_t>(INT_MAX)
            ? static_cast<std::size_t>(INT_MAX) : remaining;
        const int sent = send(socket_, reinterpret_cast<const char *>(
                                  transmit_queue_.data() + transmit_offset_),
                              static_cast<int>(chunk), 0);
        if (sent > 0) {
            transmit_offset_ += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            return true;
        }
        return false;
    }
    transmit_queue_.clear();
    transmit_offset_ = 0U;
    return true;
}

bool AgentProtocol::drain_receive_queue() {
    std::array<std::uint8_t, 8192U> bytes{};

    for (;;) {
        const int received = recv(socket_, reinterpret_cast<char *>(bytes.data()),
                                  static_cast<int>(bytes.size()), 0);
        if (received > 0) {
            if (!cw_decoder_feed(&decoder_, bytes.data(), static_cast<std::size_t>(received),
                                 callback_, callback_context_)) {
                return false;
            }
            continue;
        }
        if (received == 0) {
            return cw_decoder_finish(&decoder_);
        }
        return WSAGetLastError() == WSAEWOULDBLOCK;
    }
}

bool AgentProtocol::on_socket_event(WPARAM wparam, LPARAM lparam) {
    const int event = WSAGETSELECTEVENT(lparam);
    const int error = WSAGETSELECTERROR(lparam);

    (void)wparam;
    if (error != 0) {
        return false;
    }
    switch (event) {
    case FD_READ:
        return drain_receive_queue();
    case FD_WRITE:
        return flush_transmit_queue();
    case FD_CLOSE:
        (void)drain_receive_queue();
        return false;
    default:
        return false;
    }
}

bool AgentProtocol::send_hello() {
    std::array<std::uint8_t, 8U> payload{};

    cw_store_u16_le(payload.data(), CW_PROTOCOL_VERSION);
    cw_store_u16_le(payload.data() + 2U, CW_PROTOCOL_VERSION);
    cw_store_u32_le(payload.data() + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
    return queue_message(CW_MESSAGE_HELLO, payload.data(), static_cast<std::uint32_t>(payload.size()));
}

bool AgentProtocol::send_present_ack(std::uint64_t window_id,
                                     std::uint64_t presentation_sequence) {
    std::array<std::uint8_t, 16U> payload{};

    cw_store_u64_le(payload.data(), window_id);
    cw_store_u64_le(payload.data() + 8U, presentation_sequence);
    return queue_message(CW_MESSAGE_WINDOW_PRESENT_ACK, payload.data(),
                         static_cast<std::uint32_t>(payload.size()));
}

bool AgentProtocol::send_pointer_location(CwMessageType type, const CwPointerLocation &location) {
    std::array<std::uint8_t, 40U> payload{};

    cw_store_u64_le(payload.data(), location.window_id);
    cw_store_u64_le(payload.data() + 8U, location.presentation_sequence);
    cw_store_i32_le(payload.data() + 16U, location.client_x);
    cw_store_i32_le(payload.data() + 20U, location.client_y);
    cw_store_i32_le(payload.data() + 24U, location.output_x);
    cw_store_i32_le(payload.data() + 28U, location.output_y);
    cw_store_u64_le(payload.data() + 32U, location.timestamp_ms);
    return queue_message(type, payload.data(), static_cast<std::uint32_t>(payload.size()));
}

bool AgentProtocol::send_pointer_motion(const CwPointerMotion &motion) {
    std::array<std::uint8_t, 48U> payload{};

    cw_store_u64_le(payload.data(), motion.location.window_id);
    cw_store_u64_le(payload.data() + 8U, motion.location.presentation_sequence);
    cw_store_i32_le(payload.data() + 16U, motion.location.client_x);
    cw_store_i32_le(payload.data() + 20U, motion.location.client_y);
    cw_store_i32_le(payload.data() + 24U, motion.location.output_x);
    cw_store_i32_le(payload.data() + 28U, motion.location.output_y);
    cw_store_u64_le(payload.data() + 32U, motion.location.timestamp_ms);
    cw_store_u32_le(payload.data() + 40U, motion.button_mask);
    cw_store_u32_le(payload.data() + 44U, 0U);
    return queue_message(CW_MESSAGE_POINTER_MOTION, payload.data(),
                         static_cast<std::uint32_t>(payload.size()));
}

bool AgentProtocol::send_pointer_button(const CwPointerButtonEvent &button) {
    std::array<std::uint8_t, 48U> payload{};

    cw_store_u64_le(payload.data(), button.location.window_id);
    cw_store_u64_le(payload.data() + 8U, button.location.presentation_sequence);
    cw_store_i32_le(payload.data() + 16U, button.location.client_x);
    cw_store_i32_le(payload.data() + 20U, button.location.client_y);
    cw_store_i32_le(payload.data() + 24U, button.location.output_x);
    cw_store_i32_le(payload.data() + 28U, button.location.output_y);
    cw_store_u64_le(payload.data() + 32U, button.location.timestamp_ms);
    cw_store_u32_le(payload.data() + 40U, button.button);
    cw_store_u32_le(payload.data() + 44U, button.state);
    return queue_message(CW_MESSAGE_POINTER_BUTTON, payload.data(),
                         static_cast<std::uint32_t>(payload.size()));
}

bool AgentProtocol::send_pointer_wheel(const CwPointerWheel &wheel) {
    std::array<std::uint8_t, 48U> payload{};

    cw_store_u64_le(payload.data(), wheel.location.window_id);
    cw_store_u64_le(payload.data() + 8U, wheel.location.presentation_sequence);
    cw_store_i32_le(payload.data() + 16U, wheel.location.client_x);
    cw_store_i32_le(payload.data() + 20U, wheel.location.client_y);
    cw_store_i32_le(payload.data() + 24U, wheel.location.output_x);
    cw_store_i32_le(payload.data() + 28U, wheel.location.output_y);
    cw_store_u64_le(payload.data() + 32U, wheel.location.timestamp_ms);
    cw_store_i32_le(payload.data() + 40U, wheel.delta_x);
    cw_store_i32_le(payload.data() + 44U, wheel.delta_y);
    return queue_message(CW_MESSAGE_POINTER_WHEEL, payload.data(),
                         static_cast<std::uint32_t>(payload.size()));
}

bool AgentProtocol::send_pointer_capture_lost(const CwPointerCaptureLost &capture_lost) {
    std::array<std::uint8_t, 24U> payload{};

    cw_store_u64_le(payload.data(), capture_lost.window_id);
    cw_store_u64_le(payload.data() + 8U, capture_lost.presentation_sequence);
    cw_store_u64_le(payload.data() + 16U, capture_lost.timestamp_ms);
    return queue_message(CW_MESSAGE_POINTER_CAPTURE_LOST, payload.data(),
                         static_cast<std::uint32_t>(payload.size()));
}

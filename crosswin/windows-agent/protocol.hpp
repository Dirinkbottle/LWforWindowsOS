#ifndef CROSSWIN_WINDOWS_AGENT_PROTOCOL_HPP
#define CROSSWIN_WINDOWS_AGENT_PROTOCOL_HPP

#include "../common/protocol.h"

/* Prevent Windows headers from replacing std::numeric_limits<T>::max(). */
#ifndef NOMINMAX
#define NOMINMAX
#endif

/*
 * The agent intentionally uses WSAAsyncSelect: socket notifications and HWND
 * messages are processed by one UI thread.  Recent Windows SDKs mark this
 * established WinSock API deprecated in favour of WSAEventSelect; suppress
 * only that SDK warning so /W4 /WX can remain enabled.
 */
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <vector>

constexpr UINT CW_WM_SOCKET = WM_APP + 1U;
/* Presentation is deliberately dispatched outside the socket callback so a
 * burst of drag presents collapses to the newest state before GDI renders. */
constexpr UINT CW_WM_APPLY_PENDING_PRESENTS = WM_APP + 2U;

/*
 * UI-thread-only Winsock transport.  WSAAsyncSelect makes the socket
 * nonblocking; transmit_queue_ preserves bytes across partial send() calls.
 */
class AgentProtocol {
public:
    AgentProtocol();
    ~AgentProtocol();

    AgentProtocol(const AgentProtocol &) = delete;
    AgentProtocol &operator=(const AgentProtocol &) = delete;

    bool connect_to(
        const char *host,
        std::uint16_t port,
        HWND notification_window,
        CwMessageCallback callback,
        void *callback_context);
    bool on_socket_event(WPARAM wparam, LPARAM lparam);
    void close();
    bool connected() const;

    bool send_hello();
    bool send_present_ack(std::uint64_t window_id, std::uint64_t presentation_sequence);
    bool send_frame_request(std::uint64_t window_id, std::uint64_t local_frame_sequence);
    bool send_pointer_location(CwMessageType type, const CwPointerLocation &location);
    bool send_pointer_motion(const CwPointerMotion &motion);
    bool send_pointer_button(const CwPointerButtonEvent &button);
    bool send_pointer_wheel(const CwPointerWheel &wheel);
    bool send_pointer_capture_lost(const CwPointerCaptureLost &capture_lost);
    bool send_keyboard_focus(const CwKeyboardFocus &focus);
    bool send_keyboard_key(const CwKeyboardKey &key);

private:
    bool queue_message(
        CwMessageType type,
        const std::uint8_t *payload,
        std::uint32_t payload_length);
    bool flush_transmit_queue();
    bool drain_receive_queue();

    SOCKET socket_;
    CwDecoder decoder_;
    CwMessageCallback callback_;
    void *callback_context_;
    std::vector<std::uint8_t> transmit_queue_;
    std::size_t transmit_offset_;
    std::uint64_t next_wire_sequence_;
};

#endif

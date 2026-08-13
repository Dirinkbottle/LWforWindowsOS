#ifndef CROSSWIN_WINDOWS_AGENT_PROXY_WINDOW_HPP
#define CROSSWIN_WINDOWS_AGENT_PROXY_WINDOW_HPP

#include "protocol.hpp"

#include <windows.h>

#include <cstdint>
#include <vector>

/* A single, WS_POPUP proxy HWND whose geometry is always Linux-provided. */
class ProxyWindow {
public:
    ProxyWindow();
    ~ProxyWindow();

    ProxyWindow(const ProxyWindow &) = delete;
    ProxyWindow &operator=(const ProxyWindow &) = delete;

    bool create(HINSTANCE instance, AgentProtocol *protocol, bool trace_input);
    void destroy();
    HWND hwnd() const;
    bool owns_window(std::uint64_t window_id) const;

    bool apply_create(const CwWindowCreate &create);
    bool apply_frame(const CwWindowFrame &frame);
    bool apply_present(const CwWindowPresent &present);
    bool apply_destroy(std::uint64_t window_id);

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    CwPointerLocation make_pointer_location(LPARAM lparam) const;
    CwPointerLocation current_cursor_location() const;
    void trace_location(const char *event, const CwPointerLocation &location) const;
    void send_button(UINT message, WPARAM wparam, LPARAM lparam);
    void send_wheel(WPARAM wparam, LPARAM lparam);
    void paint(HDC dc);

    HWND hwnd_;
    AgentProtocol *protocol_;
    std::uint64_t window_id_;
    std::uint32_t surface_width_;
    std::uint32_t surface_height_;
    std::uint32_t stride_;
    std::vector<std::uint8_t> framebuffer_;
    CwWindowPresent presentation_;
    std::uint32_t pressed_button_mask_;
    bool tracking_mouse_;
    bool trace_input_;
};

#endif

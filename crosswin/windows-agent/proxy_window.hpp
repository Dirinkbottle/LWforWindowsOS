#ifndef CROSSWIN_WINDOWS_AGENT_PROXY_WINDOW_HPP
#define CROSSWIN_WINDOWS_AGENT_PROXY_WINDOW_HPP

#include "protocol.hpp"

#include <windows.h>

#include <cstdint>
#include <vector>

/* One Linux-owned proxy HWND. Popups use their parent HWND as a non-activating
 * Win32 owner; every proxy is presented through per-pixel alpha. */
class ProxyWindow {
public:
    ProxyWindow();
    ~ProxyWindow();

    ProxyWindow(const ProxyWindow &) = delete;
    ProxyWindow &operator=(const ProxyWindow &) = delete;

    bool create(HINSTANCE instance, AgentProtocol *protocol, HWND owner, bool is_popup,
                bool trace_input, bool trace_frame, bool trace_damage);
    void destroy();
    HWND hwnd() const;
    bool owns_window(std::uint64_t window_id) const;

    bool apply_create(const CwWindowCreate &create);
    bool apply_resize(const CwWindowResize &resize);
    bool apply_frame(const CwWindowFrame &frame);
    bool apply_damage(const CwWindowDamage &damage);
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
    bool request_frame_resync(const char *reason);
    bool update_layered_window();
    void paint(HDC dc);

    HWND hwnd_;
    AgentProtocol *protocol_;
    bool is_popup_;
    std::uint64_t window_id_;
    std::uint32_t surface_width_;
    std::uint32_t surface_height_;
    std::uint32_t stride_;
    std::uint64_t frame_sequence_;
    std::vector<std::uint8_t> framebuffer_;
    CwWindowPresent presentation_;
    std::uint32_t pressed_button_mask_;
    bool tracking_mouse_;
    bool trace_input_;
    bool trace_frame_;
    bool trace_damage_;
};

#endif

#ifndef CROSSWIN_WINDOWS_AGENT_PROXY_WINDOW_HPP
#define CROSSWIN_WINDOWS_AGENT_PROXY_WINDOW_HPP

#include "protocol.hpp"
#include "../../geometry/geometry.h"

#include <windows.h>

#include <cstdint>
#include <unordered_set>
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
    Rect physical_destination() const;
    bool owns_window(std::uint64_t window_id) const;
    bool bring_to_front();
    bool set_output_config(const CwOutputConfig &config);

    bool apply_create(const CwWindowCreate &create);
    bool apply_resize(const CwWindowResize &resize);
    bool apply_frame(const CwWindowFrame &frame);
    bool apply_damage(const CwWindowDamage &damage);
    bool apply_present(const CwWindowPresent &present);
    bool apply_destroy(std::uint64_t window_id);

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    bool make_pointer_location(Point physical_client,
                               CwPointerLocation *location) const;
    bool current_cursor_location(CwPointerLocation *location) const;
    void trace_location(const char *event, const CwPointerLocation &location) const;
    void send_button(UINT message, WPARAM wparam, LPARAM lparam);
    void send_wheel(UINT message, WPARAM wparam, LPARAM lparam);
    void send_keyboard_focus(bool focused);
    void send_key(UINT message, WPARAM wparam, LPARAM lparam);
    void release_pressed_keys();
    bool request_frame_resync(const char *reason);
    bool ensure_present_surface(HDC screen, int width, int height);
    void destroy_present_surface();
    bool update_layered_window();
    void paint(HDC dc);

    HWND hwnd_;
    HDC present_dc_;
    HBITMAP present_bitmap_;
    HGDIOBJ present_old_bitmap_;
    void *present_bits_;
    int present_width_;
    int present_height_;
    AgentProtocol *protocol_;
    bool is_popup_;
    std::uint64_t window_id_;
    std::uint32_t surface_width_;
    std::uint32_t surface_height_;
    std::uint32_t stride_;
    std::uint64_t frame_sequence_;
    std::vector<std::uint8_t> framebuffer_;
    CwWindowPresent presentation_;
    Scale output_scale_;
    Rect physical_destination_;
    std::uint32_t pressed_button_mask_;
    std::unordered_set<std::uint32_t> pressed_keys_;
    bool keyboard_focused_;
    bool tracking_mouse_;
    bool trace_input_;
    bool trace_frame_;
    bool trace_damage_;
};

#endif

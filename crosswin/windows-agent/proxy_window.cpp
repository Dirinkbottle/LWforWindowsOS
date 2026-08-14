#include "proxy_window.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr wchar_t kProxyClassName[] = L"CrossWinProxyWindow";

std::uint32_t button_bit(std::uint32_t button) {
    return 1U << (button - 1U);
}

std::uint64_t timestamp_ms() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

/* Win32 set-1 scan codes match the Linux evdev values for the normal 101/102
 * key block. E0-prefixed keys occupy their Linux extended positions instead.
 * The wire protocol intentionally carries Linux key codes: Weston can then
 * feed its normal keyboard path and Wayland clients keep their native xkb
 * layout/repeat handling. */
bool windows_key_to_linux_evdev(WPARAM virtual_key, LPARAM lparam, std::uint32_t *out) {
    const std::uint32_t scan_code = (static_cast<std::uint32_t>(lparam) >> 16U) & 0xffU;
    const bool extended = (static_cast<std::uintptr_t>(lparam) & (UINT_PTR(1) << 24U)) != 0U;

    if (out == nullptr) {
        return false;
    }
    if (virtual_key == VK_PAUSE) {
        *out = 119U; /* KEY_PAUSE */
        return true;
    }
    if (extended) {
        switch (scan_code) {
        case 0x1cU: *out = 96U; return true;  /* KEY_KPENTER */
        case 0x1dU: *out = 97U; return true;  /* KEY_RIGHTCTRL */
        case 0x35U: *out = 98U; return true;  /* KEY_KPSLASH */
        case 0x37U: *out = 99U; return true;  /* KEY_SYSRQ */
        case 0x38U: *out = 100U; return true; /* KEY_RIGHTALT */
        case 0x47U: *out = 102U; return true; /* KEY_HOME */
        case 0x48U: *out = 103U; return true; /* KEY_UP */
        case 0x49U: *out = 104U; return true; /* KEY_PAGEUP */
        case 0x4bU: *out = 105U; return true; /* KEY_LEFT */
        case 0x4dU: *out = 106U; return true; /* KEY_RIGHT */
        case 0x4fU: *out = 107U; return true; /* KEY_END */
        case 0x50U: *out = 108U; return true; /* KEY_DOWN */
        case 0x51U: *out = 109U; return true; /* KEY_PAGEDOWN */
        case 0x52U: *out = 110U; return true; /* KEY_INSERT */
        case 0x53U: *out = 111U; return true; /* KEY_DELETE */
        case 0x5bU: *out = 125U; return true; /* KEY_LEFTMETA */
        case 0x5cU: *out = 126U; return true; /* KEY_RIGHTMETA */
        case 0x5dU: *out = 127U; return true; /* KEY_COMPOSE */
        default:
            return false;
        }
    }
    if (scan_code == 0U || scan_code > 0x58U) {
        return false;
    }
    *out = scan_code;
    return true;
}

} // namespace

ProxyWindow::ProxyWindow()
    : hwnd_(nullptr),
      protocol_(nullptr),
      is_popup_(false),
      window_id_(0U),
      surface_width_(0U),
      surface_height_(0U),
      stride_(0U),
      frame_sequence_(0U),
      framebuffer_{},
      presentation_{},
      output_scale_{1U, 1U},
      physical_destination_{},
      pressed_button_mask_(0U),
      pressed_keys_{},
      keyboard_focused_(false),
      tracking_mouse_(false),
      trace_input_(false),
      trace_frame_(false),
      trace_damage_(false) {}

ProxyWindow::~ProxyWindow() {
    destroy();
}

bool ProxyWindow::create(HINSTANCE instance, AgentProtocol *protocol, HWND owner, bool is_popup,
                         bool trace_input, bool trace_frame, bool trace_damage) {
    WNDCLASSW window_class{};

    if (protocol == nullptr || hwnd_ != nullptr) {
        return false;
    }
    window_class.lpfnWndProc = &ProxyWindow::window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kProxyClassName;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    protocol_ = protocol;
    is_popup_ = is_popup;
    trace_input_ = trace_input;
    trace_frame_ = trace_frame;
    trace_damage_ = trace_damage;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED |
                                (is_popup_ ? WS_EX_NOACTIVATE : 0U),
                            kProxyClassName, L"", WS_POPUP, 0, 0, 1, 1,
                            owner, nullptr, instance, this);
    return hwnd_ != nullptr;
}

void ProxyWindow::destroy() {
    release_pressed_keys();
    send_keyboard_focus(false);
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    framebuffer_.clear();
    window_id_ = 0U;
    frame_sequence_ = 0U;
    pressed_button_mask_ = 0U;
    pressed_keys_.clear();
    keyboard_focused_ = false;
    tracking_mouse_ = false;
    is_popup_ = false;
    physical_destination_ = Rect{};
}

HWND ProxyWindow::hwnd() const {
    return hwnd_;
}

Rect ProxyWindow::physical_destination() const {
    return physical_destination_;
}

bool ProxyWindow::owns_window(std::uint64_t window_id) const {
    return window_id_ != 0U && window_id_ == window_id;
}

bool ProxyWindow::bring_to_front() {
    if (hwnd_ == nullptr) {
        return false;
    }
    if (!IsWindowVisible(hwnd_)) {
        return true; /* A hidden logical window has no native stack position. */
    }
    /* This is deliberately HWND_TOP rather than TOPMOST/SetForegroundWindow:
     * Linux owns ordering among Crosswin windows, while ordinary Windows apps
     * may still naturally cover the whole Crosswin group. */
    return SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
}

bool ProxyWindow::set_output_config(const CwOutputConfig &config) {
    const Scale scale{config.scale_numerator, config.scale_denominator};

    if (!scale_is_valid(scale)) {
        return false;
    }
    output_scale_ = scale;
    /* OUTPUT_CONFIG is sent before CREATE on every connection. Updating an
     * already-visible proxy is nevertheless well-defined for a future output
     * reconfiguration: redraw it using the new physical coverage. */
    return !presentation_.visible || framebuffer_.empty() || update_layered_window();
}

bool ProxyWindow::apply_create(const CwWindowCreate &create) {
    if (hwnd_ == nullptr || window_id_ != 0U || create.window_id == 0U ||
        create.surface_width == 0U || create.surface_height == 0U) {
        return false;
    }
    const std::uint64_t stride = static_cast<std::uint64_t>(create.surface_width) * 4U;
    const std::uint64_t bytes = stride * create.surface_height;
    if (stride > std::numeric_limits<std::uint32_t>::max() ||
        bytes > CW_MAX_PAYLOAD - 32U ||
        bytes > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    window_id_ = create.window_id;
    surface_width_ = create.surface_width;
    surface_height_ = create.surface_height;
    stride_ = static_cast<std::uint32_t>(stride);
    framebuffer_.assign(static_cast<std::size_t>(bytes), 0U);
    frame_sequence_ = 0U;
    return true;
}

bool ProxyWindow::apply_resize(const CwWindowResize &resize) {
    CwWindowCreate create{};

    if (!owns_window(resize.window_id)) {
        return false;
    }
    create.window_id = resize.window_id;
    create.surface_width = resize.surface_width;
    create.surface_height = resize.surface_height;
    /* Reallocation has the same overflow rules as CREATE, but does not create
     * a new HWND or change any Linux-owned presentation geometry. */
    const std::uint64_t stride = static_cast<std::uint64_t>(create.surface_width) * 4U;
    const std::uint64_t bytes = stride * create.surface_height;
    if (create.surface_width == 0U || create.surface_height == 0U ||
        stride > std::numeric_limits<std::uint32_t>::max() ||
        bytes > CW_MAX_PAYLOAD - 32U ||
        bytes > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    surface_width_ = create.surface_width;
    surface_height_ = create.surface_height;
    stride_ = static_cast<std::uint32_t>(stride);
    framebuffer_.assign(static_cast<std::size_t>(bytes), 0U);
    frame_sequence_ = 0U;
    if (trace_frame_) {
        std::printf("[frame resize] win=%llu size=%ux%u\n",
                    static_cast<unsigned long long>(window_id_), surface_width_, surface_height_);
    }
    return true;
}

bool ProxyWindow::apply_frame(const CwWindowFrame &frame) {
    if (!owns_window(frame.window_id) || frame.width != surface_width_ ||
        frame.height != surface_height_ || frame.stride != stride_ ||
        frame.pixel_format != CW_PIXEL_FORMAT_BGRA8888 ||
        frame.pixel_bytes != framebuffer_.size() || frame.frame_sequence == 0U) {
        return false;
    }
    if (frame.frame_sequence < frame_sequence_) {
        if (trace_frame_) {
            std::printf("[frame stale] win=%llu frame=%llu local=%llu\n",
                        static_cast<unsigned long long>(window_id_),
                        static_cast<unsigned long long>(frame.frame_sequence),
                        static_cast<unsigned long long>(frame_sequence_));
        }
        return true;
    }
    std::memcpy(framebuffer_.data(), frame.pixels, framebuffer_.size());
    frame_sequence_ = frame.frame_sequence;
    if (trace_frame_) {
        std::printf("[frame apply] win=%llu frame=%llu full=1 bytes=%llu\n",
                    static_cast<unsigned long long>(window_id_),
                    static_cast<unsigned long long>(frame_sequence_),
                    static_cast<unsigned long long>(frame.pixel_bytes));
    }
    if (presentation_.visible && !update_layered_window()) {
        return false;
    }
    return true;
}

bool ProxyWindow::request_frame_resync(const char *reason) {
    if (trace_damage_) {
        std::printf("[damage desync] win=%llu local_frame=%llu reason=%s\n",
                    static_cast<unsigned long long>(window_id_),
                    static_cast<unsigned long long>(frame_sequence_), reason);
    }
    return protocol_ != nullptr && protocol_->send_frame_request(window_id_, frame_sequence_);
}

bool ProxyWindow::apply_damage(const CwWindowDamage &damage) {
    std::vector<CwDamageRect> rectangles;
    std::uint32_t index;

    if (!owns_window(damage.window_id)) {
        return false;
    }
    if (frame_sequence_ == 0U || damage.base_frame_sequence != frame_sequence_ ||
        damage.frame_sequence <= damage.base_frame_sequence) {
        return request_frame_resync("base-frame-mismatch");
    }
    rectangles.reserve(damage.rect_count);
    for (index = 0U; index < damage.rect_count; ++index) {
        CwDamageRect rectangle{};
        std::int64_t right;
        std::int64_t bottom;

        if (!cw_window_damage_rect_at(&damage, index, &rectangle)) {
            return request_frame_resync("malformed-rectangle");
        }
        right = static_cast<std::int64_t>(rectangle.x) + rectangle.width;
        bottom = static_cast<std::int64_t>(rectangle.y) + rectangle.height;
        if (rectangle.x < 0 || rectangle.y < 0 || right > surface_width_ ||
            bottom > surface_height_ || rectangle.stride != rectangle.width * 4U ||
            rectangle.pixel_bytes != static_cast<std::uint64_t>(rectangle.stride) * rectangle.height) {
            return request_frame_resync("rectangle-outside-surface");
        }
        rectangles.push_back(rectangle);
    }
    /* Validate every rect before altering the persistent framebuffer.  The
     * message is therefore atomic even for overlapping rectangles. */
    for (const CwDamageRect &rectangle : rectangles) {
        for (std::uint32_t row = 0U; row < rectangle.height; ++row) {
            const std::size_t destination =
                (static_cast<std::size_t>(rectangle.y) + row) * stride_ +
                static_cast<std::size_t>(rectangle.x) * 4U;
            const std::size_t source = static_cast<std::size_t>(row) * rectangle.stride;

            std::memcpy(framebuffer_.data() + destination, rectangle.pixels + source,
                        rectangle.stride);
        }
    }
    frame_sequence_ = damage.frame_sequence;
    if (trace_damage_) {
        std::printf("[damage apply] win=%llu frame=%llu base=%llu rects=%u\n",
                    static_cast<unsigned long long>(window_id_),
                    static_cast<unsigned long long>(damage.frame_sequence),
                    static_cast<unsigned long long>(damage.base_frame_sequence),
                    damage.rect_count);
    }
    if (!rectangles.empty() && presentation_.visible && !update_layered_window()) {
        return false;
    }
    return true;
}

bool ProxyWindow::apply_present(const CwWindowPresent &present) {
    Rect logical_destination{};

    if (!owns_window(present.window_id) || present.source_x < 0 || present.source_y < 0 ||
        present.source_w < 0 || present.source_h < 0 || present.destination_w != present.source_w ||
        present.destination_h != present.source_h ||
        static_cast<std::uint64_t>(present.source_x) + present.source_w > surface_width_ ||
        static_cast<std::uint64_t>(present.source_y) + present.source_h > surface_height_) {
        return false;
    }
    presentation_ = present;
    if (!present.visible) {
		physical_destination_ = Rect{};
        ShowWindow(hwnd_, SW_HIDE);
        return protocol_->send_present_ack(present.window_id, present.presentation_sequence);
    }
	logical_destination = Rect{present.destination_x, present.destination_y,
				   present.destination_w, present.destination_h};
	if (!logical_rect_to_physical(logical_destination, output_scale_,
				      &physical_destination_) ||
	    rect_is_empty(physical_destination_)) {
		return false;
	}
    if (!update_layered_window()) {
        return false;
    }
    return protocol_->send_present_ack(present.window_id, present.presentation_sequence);
}

bool ProxyWindow::update_layered_window() {
    BITMAPINFO target_info{};
    BITMAPINFO source_info{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0U, 255U, AC_SRC_ALPHA};
    POINT destination{physical_destination_.x, physical_destination_.y};
    POINT source{0, 0};
    SIZE size{physical_destination_.w, physical_destination_.h};
    HDC screen = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    void *bits = nullptr;
    bool ok = false;
    DWORD error = ERROR_SUCCESS;

    if (hwnd_ == nullptr || framebuffer_.empty() || !presentation_.visible ||
        physical_destination_.w <= 0 || physical_destination_.h <= 0 ||
        presentation_.source_w != presentation_.destination_w ||
        presentation_.source_h != presentation_.destination_h) {
        return false;
    }
    /* UpdateLayeredWindow changes pixels and geometry, but it does not set
     * WS_VISIBLE on a newly-created WS_POPUP.  The pre-Stage-7 GDI path used
     * SetWindowPos(..., SWP_SHOWWINDOW), so omitting this left every first
     * per-pixel-alpha proxy HWND hidden on the Windows desktop. */
    if (!IsWindowVisible(hwnd_) &&
        !SetWindowPos(hwnd_, HWND_TOP, physical_destination_.x,
                      physical_destination_.y, physical_destination_.w,
                      physical_destination_.h, SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        error = GetLastError();
        goto out;
    }
    target_info.bmiHeader.biSize = sizeof(target_info.bmiHeader);
    target_info.bmiHeader.biWidth = physical_destination_.w;
    target_info.bmiHeader.biHeight = -physical_destination_.h;
    target_info.bmiHeader.biPlanes = 1;
    target_info.bmiHeader.biBitCount = 32;
    target_info.bmiHeader.biCompression = BI_RGB;
    source_info.bmiHeader.biSize = sizeof(source_info.bmiHeader);
    source_info.bmiHeader.biWidth = static_cast<LONG>(surface_width_);
    source_info.bmiHeader.biHeight = -static_cast<LONG>(surface_height_);
    source_info.bmiHeader.biPlanes = 1;
    source_info.bmiHeader.biBitCount = 32;
    source_info.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(nullptr);
    if (screen == nullptr) {
        error = GetLastError();
        goto out;
    }
    memory = CreateCompatibleDC(screen);
    if (memory == nullptr) {
        error = GetLastError();
        goto out;
    }
    bitmap = CreateDIBSection(memory, &target_info, DIB_RGB_COLORS, &bits, nullptr, 0U);
    if (bitmap == nullptr || bits == nullptr) {
        error = GetLastError();
        goto out;
    }
    old_bitmap = SelectObject(memory, bitmap);
    if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
        error = GetLastError();
        goto out;
    }
    if (StretchDIBits(memory, 0, 0, physical_destination_.w, physical_destination_.h,
                      presentation_.source_x, presentation_.source_y,
                      presentation_.source_w, presentation_.source_h,
                      framebuffer_.data(), &source_info, DIB_RGB_COLORS,
                      SRCCOPY) == static_cast<int>(GDI_ERROR)) {
        error = GetLastError();
        goto out;
    }
    ok = UpdateLayeredWindow(hwnd_, screen, &destination, &size, memory, &source,
                             0U, &blend, ULW_ALPHA) != FALSE;
    if (!ok) {
        error = GetLastError();
    }
out:
    if (old_bitmap != nullptr && memory != nullptr) {
        SelectObject(memory, old_bitmap);
    }
    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    if (memory != nullptr) {
        DeleteDC(memory);
    }
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
    if (!ok && trace_frame_) {
        std::printf("[layered present failed] win=%llu error=%lu src=[%d,%d %dx%d] "
                    "logical-dst=[%d,%d %dx%d] physical-dst=[%d,%d %dx%d]\n",
                    static_cast<unsigned long long>(window_id_),
                    static_cast<unsigned long>(error), presentation_.source_x,
                    presentation_.source_y, presentation_.source_w, presentation_.source_h,
                    presentation_.destination_x, presentation_.destination_y,
                    presentation_.destination_w, presentation_.destination_h,
                    physical_destination_.x, physical_destination_.y,
                    physical_destination_.w, physical_destination_.h);
    }
    return ok;
}

bool ProxyWindow::apply_destroy(std::uint64_t window_id) {
    if (!owns_window(window_id)) {
        return false;
    }
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    release_pressed_keys();
    send_keyboard_focus(false);
    pressed_button_mask_ = 0U;
    tracking_mouse_ = false;
    framebuffer_.clear();
    window_id_ = 0U;
    surface_width_ = 0U;
    surface_height_ = 0U;
    stride_ = 0U;
    frame_sequence_ = 0U;
    presentation_ = CwWindowPresent{};
    physical_destination_ = Rect{};
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    return true;
}

bool ProxyWindow::make_pointer_location(Point physical_client,
                                        CwPointerLocation *location) const {
    const Rect logical_fragment{presentation_.destination_x, presentation_.destination_y,
                                presentation_.destination_w, presentation_.destination_h};
    Point logical_client{};
    std::int64_t output_x;
    std::int64_t output_y;

    if (location == nullptr || !presentation_.visible ||
        !physical_fragment_local_to_logical(logical_fragment, output_scale_,
                                            physical_client, &logical_client)) {
        return false;
    }
    output_x = static_cast<std::int64_t>(presentation_.destination_x) + logical_client.x;
    output_y = static_cast<std::int64_t>(presentation_.destination_y) + logical_client.y;
    if (output_x < std::numeric_limits<std::int32_t>::min() ||
        output_x > std::numeric_limits<std::int32_t>::max() ||
        output_y < std::numeric_limits<std::int32_t>::min() ||
        output_y > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    *location = CwPointerLocation{
        window_id_, presentation_.presentation_sequence,
        logical_client.x, logical_client.y,
        static_cast<std::int32_t>(output_x), static_cast<std::int32_t>(output_y),
        timestamp_ms(),
    };
    return true;
}

bool ProxyWindow::current_cursor_location(CwPointerLocation *location) const {
    POINT screen{};
    POINT client{};

    if (location == nullptr || !GetCursorPos(&screen)) {
        return false;
    }
    client = screen;
    if (!ScreenToClient(hwnd_, &client)) {
        return false;
    }
    return make_pointer_location(
        Point{static_cast<std::int32_t>(client.x), static_cast<std::int32_t>(client.y)},
        location);
}

void ProxyWindow::trace_location(const char *event, const CwPointerLocation &location) const {
    if (trace_input_) {
        std::printf("[input tx] type=%s win=%llu present=%llu client=[%d,%d] output=[%d,%d]\n",
                    event, static_cast<unsigned long long>(location.window_id),
                    static_cast<unsigned long long>(location.presentation_sequence),
                    location.client_x, location.client_y,
                    location.output_x, location.output_y);
    }
}

void ProxyWindow::send_button(UINT message, WPARAM wparam, LPARAM lparam) {
    CwPointerButtonEvent event{};
    std::uint32_t button = 0U;
    std::uint32_t state = CW_BUTTON_PRESSED;

    (void)wparam;
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        button = CW_POINTER_BUTTON_LEFT;
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        button = CW_POINTER_BUTTON_RIGHT;
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        button = CW_POINTER_BUTTON_MIDDLE;
        break;
    default:
        return;
    }
    if (message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP) {
        state = CW_BUTTON_RELEASED;
    }
    if (!make_pointer_location(
            Point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, &event.location)) {
        return;
    }
    event.button = button;
    event.state = state;
    if (state == CW_BUTTON_PRESSED) {
        pressed_button_mask_ |= button_bit(button);
        SetCapture(hwnd_);
    } else {
        pressed_button_mask_ &= ~button_bit(button);
        if (pressed_button_mask_ == 0U && GetCapture() == hwnd_) {
            ReleaseCapture();
        }
    }
    trace_location("POINTER_BUTTON", event.location);
    if (trace_input_) {
        std::printf("[input tx] button=%u state=%u\n", event.button, event.state);
    }
    (void)protocol_->send_pointer_button(event);
}

void ProxyWindow::send_wheel(UINT message, WPARAM wparam, LPARAM lparam) {
    POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    POINT client = screen;
    CwPointerWheel wheel{};

    if (!ScreenToClient(hwnd_, &client) ||
        !make_pointer_location(
            Point{static_cast<std::int32_t>(client.x), static_cast<std::int32_t>(client.y)},
            &wheel.location)) {
        return;
    }
    wheel.delta_x = message == WM_MOUSEHWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0;
    wheel.delta_y = message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0;
    trace_location("POINTER_WHEEL", wheel.location);
    if (trace_input_) {
        std::printf("[input tx] wheel=[%d,%d]\n", wheel.delta_x, wheel.delta_y);
    }
    (void)protocol_->send_pointer_wheel(wheel);
}

void ProxyWindow::send_keyboard_focus(bool focused) {
    CwKeyboardFocus focus{};

    if (is_popup_ || window_id_ == 0U || protocol_ == nullptr ||
        keyboard_focused_ == focused) {
        return;
    }
    keyboard_focused_ = focused;
    focus.window_id = window_id_;
    focus.focused = focused;
    if (trace_input_) {
        std::printf("[input tx] type=KEYBOARD_FOCUS win=%llu focused=%u\n",
                    static_cast<unsigned long long>(focus.window_id),
                    focus.focused ? 1U : 0U);
    }
    (void)protocol_->send_keyboard_focus(focus);
}

void ProxyWindow::send_key(UINT message, WPARAM wparam, LPARAM lparam) {
    CwKeyboardKey key{};
    const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;

    if (is_popup_ || !keyboard_focused_ || protocol_ == nullptr ||
        !windows_key_to_linux_evdev(wparam, lparam, &key.key)) {
        return;
    }
    key.window_id = window_id_;
    key.state = pressed ? CW_KEY_PRESSED : CW_KEY_RELEASED;
    key.timestamp_ms = timestamp_ms();
    if (pressed) {
        pressed_keys_.insert(key.key);
    } else if (pressed_keys_.erase(key.key) == 0U) {
        return; /* Do not synthesize a release without a matching press. */
    }
    if (trace_input_) {
        std::printf("[input tx] type=KEYBOARD_KEY win=%llu key=%u state=%u\n",
                    static_cast<unsigned long long>(key.window_id), key.key, key.state);
    }
    (void)protocol_->send_keyboard_key(key);
}

void ProxyWindow::release_pressed_keys() {
    if (protocol_ == nullptr || window_id_ == 0U) {
        pressed_keys_.clear();
        return;
    }
    for (const std::uint32_t key_code : pressed_keys_) {
        const CwKeyboardKey key{window_id_, key_code, CW_KEY_RELEASED, timestamp_ms()};
        if (trace_input_) {
            std::printf("[input tx] type=KEYBOARD_KEY win=%llu key=%u state=0 recovery=focus-loss\n",
                        static_cast<unsigned long long>(key.window_id), key.key);
        }
        (void)protocol_->send_keyboard_key(key);
    }
    pressed_keys_.clear();
}

void ProxyWindow::paint(HDC dc) {
    BITMAPINFO bitmap{};

    if (framebuffer_.empty() || !presentation_.visible) {
        return;
    }
    bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
    bitmap.bmiHeader.biWidth = static_cast<LONG>(surface_width_);
    bitmap.bmiHeader.biHeight = -static_cast<LONG>(surface_height_); /* Explicit top-down DIB. */
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, 0, 0, physical_destination_.w, physical_destination_.h,
                  presentation_.source_x, presentation_.source_y,
                  presentation_.source_w, presentation_.source_h,
                  framebuffer_.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT ProxyWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint_struct{};
        HDC dc = BeginPaint(hwnd_, &paint_struct);
        this->paint(dc);
        EndPaint(hwnd_, &paint_struct);
        return 0;
    }
    case WM_MOUSEMOVE: {
        CwPointerMotion motion{};
		CwPointerLocation location{};

		if (!make_pointer_location(
				Point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, &location)) {
			return 0;
		}
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0U};
            tracking_mouse_ = TrackMouseEvent(&tracking) != FALSE;
            trace_location("POINTER_ENTER", location);
            (void)protocol_->send_pointer_location(CW_MESSAGE_POINTER_ENTER, location);
        }
        motion.location = location;
        motion.button_mask = pressed_button_mask_;
        trace_location("POINTER_MOTION", motion.location);
        if (trace_input_) {
            std::printf("[input tx] button_mask=%u\n", motion.button_mask);
        }
        (void)protocol_->send_pointer_motion(motion);
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_mouse_ = false;
        {
			CwPointerLocation location{};
			if (current_cursor_location(&location)) {
				trace_location("POINTER_LEAVE", location);
				(void)protocol_->send_pointer_location(CW_MESSAGE_POINTER_LEAVE, location);
			}
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
            message == WM_MBUTTONDOWN) {
            /* A popup is deliberately non-activating; keep text input owned
             * by its top-level Linux parent in that case. */
            (void)SetFocus(is_popup_ ? GetWindow(hwnd_, GW_OWNER) : hwnd_);
        }
        send_button(message, wparam, lparam);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        send_wheel(message, wparam, lparam);
        return 0;
    case WM_SETFOCUS:
        send_keyboard_focus(true);
        return 0;
    case WM_KILLFOCUS:
        release_pressed_keys();
        send_keyboard_focus(false);
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        send_key(message, wparam, lparam);
        return 0;
    case WM_CAPTURECHANGED:
        if (pressed_button_mask_ != 0U) {
            const CwPointerCaptureLost lost{window_id_, presentation_.presentation_sequence,
                                            timestamp_ms()};
            pressed_button_mask_ = 0U;
            if (trace_input_) {
                std::printf("[input tx] type=POINTER_CAPTURE_LOST win=%llu present=%llu\n",
                            static_cast<unsigned long long>(lost.window_id),
                            static_cast<unsigned long long>(lost.presentation_sequence));
            }
            (void)protocol_->send_pointer_capture_lost(lost);
        }
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    default:
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

LRESULT CALLBACK ProxyWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    ProxyWindow *window = nullptr;

    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
        window = static_cast<ProxyWindow *>(create->lpCreateParams);
        if (window == nullptr) {
            return FALSE;
        }
        /* CreateWindowExW sends later creation messages before it returns. */
        window->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        /* WM_NCCREATE must return TRUE or CreateWindowExW aborts creation. */
        return TRUE;
    } else {
        window = reinterpret_cast<ProxyWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return window != nullptr ? window->handle_message(message, wparam, lparam)
                             : DefWindowProcW(hwnd, message, wparam, lparam);
}

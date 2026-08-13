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

} // namespace

ProxyWindow::ProxyWindow()
    : hwnd_(nullptr),
      protocol_(nullptr),
      window_id_(0U),
      surface_width_(0U),
      surface_height_(0U),
      stride_(0U),
      framebuffer_{},
      presentation_{},
      pressed_button_mask_(0U),
      tracking_mouse_(false),
      trace_input_(false) {}

ProxyWindow::~ProxyWindow() {
    destroy();
}

bool ProxyWindow::create(HINSTANCE instance, AgentProtocol *protocol, bool trace_input) {
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
    trace_input_ = trace_input;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kProxyClassName, L"",
                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    return hwnd_ != nullptr;
}

void ProxyWindow::destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    framebuffer_.clear();
    window_id_ = 0U;
    pressed_button_mask_ = 0U;
    tracking_mouse_ = false;
}

HWND ProxyWindow::hwnd() const {
    return hwnd_;
}

bool ProxyWindow::owns_window(std::uint64_t window_id) const {
    return window_id_ != 0U && window_id_ == window_id;
}

bool ProxyWindow::apply_create(const CwWindowCreate &create) {
    if (hwnd_ == nullptr || window_id_ != 0U || create.window_id == 0U ||
        create.surface_width == 0U || create.surface_height == 0U) {
        return false;
    }
    const std::uint64_t stride = static_cast<std::uint64_t>(create.surface_width) * 4U;
    const std::uint64_t bytes = stride * create.surface_height;
    if (stride > std::numeric_limits<std::uint32_t>::max() ||
        bytes > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    window_id_ = create.window_id;
    surface_width_ = create.surface_width;
    surface_height_ = create.surface_height;
    stride_ = static_cast<std::uint32_t>(stride);
    framebuffer_.assign(static_cast<std::size_t>(bytes), 0U);
    return true;
}

bool ProxyWindow::apply_frame(const CwWindowFrame &frame) {
    if (!owns_window(frame.window_id) || frame.width != surface_width_ ||
        frame.height != surface_height_ || frame.stride != stride_ ||
        frame.pixel_format != CW_PIXEL_FORMAT_BGRA8888 ||
        frame.pixel_bytes != framebuffer_.size()) {
        return false;
    }
    std::memcpy(framebuffer_.data(), frame.pixels, framebuffer_.size());
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool ProxyWindow::apply_present(const CwWindowPresent &present) {
    if (!owns_window(present.window_id) || present.source_x < 0 || present.source_y < 0 ||
        present.source_w < 0 || present.source_h < 0 || present.destination_w != present.source_w ||
        present.destination_h != present.source_h ||
        static_cast<std::uint64_t>(present.source_x) + present.source_w > surface_width_ ||
        static_cast<std::uint64_t>(present.source_y) + present.source_h > surface_height_) {
        return false;
    }
    presentation_ = present;
    if (!present.visible) {
        ShowWindow(hwnd_, SW_HIDE);
        return protocol_->send_present_ack(present.window_id, present.presentation_sequence);
    }
    SetWindowPos(hwnd_, HWND_TOP, present.destination_x, present.destination_y,
                 present.destination_w, present.destination_h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return protocol_->send_present_ack(present.window_id, present.presentation_sequence);
}

bool ProxyWindow::apply_destroy(std::uint64_t window_id) {
    if (!owns_window(window_id)) {
        return false;
    }
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    pressed_button_mask_ = 0U;
    tracking_mouse_ = false;
    framebuffer_.clear();
    window_id_ = 0U;
    surface_width_ = 0U;
    surface_height_ = 0U;
    stride_ = 0U;
    presentation_ = CwWindowPresent{};
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    return true;
}

CwPointerLocation ProxyWindow::make_pointer_location(LPARAM lparam) const {
    return CwPointerLocation{
        window_id_,
        presentation_.presentation_sequence,
        GET_X_LPARAM(lparam),
        GET_Y_LPARAM(lparam),
        presentation_.destination_x + GET_X_LPARAM(lparam),
        presentation_.destination_y + GET_Y_LPARAM(lparam),
        timestamp_ms(),
    };
}

CwPointerLocation ProxyWindow::current_cursor_location() const {
    POINT screen{};
    POINT client{};
    POINT origin{};

    GetCursorPos(&screen);
    client = screen;
    ScreenToClient(hwnd_, &client);
    ClientToScreen(hwnd_, &origin);
    return CwPointerLocation{
        window_id_,
        presentation_.presentation_sequence,
        client.x,
        client.y,
        screen.x - origin.x + presentation_.destination_x,
        screen.y - origin.y + presentation_.destination_y,
        timestamp_ms(),
    };
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
    event.location = make_pointer_location(lparam);
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

void ProxyWindow::send_wheel(WPARAM wparam, LPARAM lparam) {
    POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    POINT client = screen;
    POINT origin{0, 0};
    CwPointerWheel wheel{};

    ScreenToClient(hwnd_, &client);
    ClientToScreen(hwnd_, &origin);
    wheel.location = CwPointerLocation{
        window_id_, presentation_.presentation_sequence,
        client.x, client.y,
        screen.x - origin.x + presentation_.destination_x,
        screen.y - origin.y + presentation_.destination_y,
        timestamp_ms(),
    };
    wheel.delta_x = 0;
    wheel.delta_y = GET_WHEEL_DELTA_WPARAM(wparam);
    trace_location("POINTER_WHEEL", wheel.location);
    if (trace_input_) {
        std::printf("[input tx] wheel=[%d,%d]\n", wheel.delta_x, wheel.delta_y);
    }
    (void)protocol_->send_pointer_wheel(wheel);
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
    StretchDIBits(dc, 0, 0, presentation_.destination_w, presentation_.destination_h,
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
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0U};
            tracking_mouse_ = TrackMouseEvent(&tracking) != FALSE;
            const CwPointerLocation location = make_pointer_location(lparam);
            trace_location("POINTER_ENTER", location);
            (void)protocol_->send_pointer_location(CW_MESSAGE_POINTER_ENTER, location);
        }
        motion.location = make_pointer_location(lparam);
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
            const CwPointerLocation location = current_cursor_location();
            trace_location("POINTER_LEAVE", location);
            (void)protocol_->send_pointer_location(CW_MESSAGE_POINTER_LEAVE, location);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        send_button(message, wparam, lparam);
        return 0;
    case WM_MOUSEWHEEL:
        send_wheel(wparam, lparam);
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

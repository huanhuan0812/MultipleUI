// nativeBridge — Windows (Win32) backend: window and message handling.
// SPDX-License-Identifier: MIT
//
// See win32_platform.cpp for the DPI and threading contract. This file owns the
// HWND, the WndProc and the translation from Win32 messages to unified events.

#include "win32/win32_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <dwmapi.h>
#include <windowsx.h>

#include "nativebridge/window.h"

using namespace nativebridge;

namespace nativebridge::detail {

namespace {

/// Compute the window rectangle that yields the requested client size.
/// Uses AdjustWindowRectExForDpi when available so the result is correct on a
/// per-monitor-DPI-aware process.
RECT windowRectForClientSize(Size clientSize, DWORD style, DWORD exStyle, BOOL hasMenu, UINT dpi) {
    RECT rect{0, 0, clientSize.width, clientSize.height};
    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll"); user32 != nullptr) {
        auto adjust = reinterpret_cast<AdjustWindowRectExForDpiFn>(
            ::GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjust != nullptr) {
            (void)adjust(&rect, style, hasMenu, exStyle, dpi);
            return rect;
        }
    }
    ::AdjustWindowRectEx(&rect, style, hasMenu, exStyle);
    return rect;
}

} // namespace

const wchar_t* win32WindowClassName() noexcept { return L"nativeBridgeWindowClass"; }

bool win32RegisterWindowClass(ATOM& outAtom, std::wstring& outName) {
    static std::once_flag once;
    static ATOM atom = 0;
    std::call_once(once, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // CS_OWNDC: the renderer may create its GL context once and keep it.
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
        wc.lpfnWndProc = &win32WindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // the renderer paints the client area
        wc.lpszClassName = win32WindowClassName();
        atom = ::RegisterClassExW(&wc);
    });
    outAtom = atom;
    outName = win32WindowClassName();
    return atom != 0;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Win32Window::Win32Window(Win32Platform& platform, WindowId id, const WindowCreateInfo& info)
    : m_platform(platform), m_id(id), m_minSize(info.minSize), m_maxSize(info.maxSize),
      m_cursorShape(info.cursor), m_decorations(!hasStyle(info.style, WindowStyle::NoDecoration)),
      m_resizable(info.resizable) {
    m_style = WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    if (m_decorations) {
        m_style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        if (m_resizable) {
            m_style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        }
    } else {
        m_style |= WS_POPUP;
        if (m_resizable) {
            m_style |= WS_THICKFRAME;
        }
    }
    m_exStyle = WS_EX_APPWINDOW;
    if (hasStyle(info.style, WindowStyle::AlwaysOnTop)) {
        m_exStyle |= WS_EX_TOPMOST;
    }
    if (hasStyle(info.style, WindowStyle::ToolWindow)) {
        m_exStyle |= WS_EX_TOOLWINDOW;
    }
    if (hasStyle(info.style, WindowStyle::Transparent)) {
        m_exStyle |= WS_EX_LAYERED;
    }

    const UINT dpi = dpiForMonitorAt(POINT{0, 0});
    const RECT rect = windowRectForClientSize(info.clientSize, m_style, m_exStyle, FALSE, dpi);
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (info.hasPosition) {
        x = info.position.x;
        y = info.position.y;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    const std::wstring title = toUtf16(info.title);
    m_hwnd = ::CreateWindowExW(m_exStyle, win32WindowClassName(), title.c_str(), m_style, x, y, width, height,
                               nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (m_hwnd == nullptr) {
        log::error("win32: CreateWindowExW failed with " + std::to_string(::GetLastError()));
        return;
    }

    if (info.centered && !info.hasPosition) {
        centerOnMonitor();
    }
    if (info.parent != 0) {
        // best effort: the platform finds the parent HWND from the window id
        if (HWND parent = m_platform.hwndForWindowId(info.parent); parent != nullptr) {
            ::SetWindowLongPtrW(m_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(parent));
        }
    }

    syncScaleFactor(false);
    syncGeometry();

    if (info.visible) {
        (void)show();
    }
}

Win32Window::~Win32Window() {
    if (m_hwnd != nullptr) {
        ::SetWindowLongPtrW(m_hwnd, kWindowBackendSlot, 0);
        ::DestroyWindow(m_hwnd); // sends WM_DESTROY to handleMessage
        m_hwnd = nullptr;
    }
    if (m_customCursor != nullptr) {
        ::DestroyCursor(m_customCursor);
        m_customCursor = nullptr;
    }
    notifyDestroyed(m_platform.lifetime(), m_id);
}

// ---------------------------------------------------------------------------
// Window class registration + WndProc
// ---------------------------------------------------------------------------

/// Shared WndProc. The HWND carries the Win32Window* in GWLP_USERDATA.
LRESULT CALLBACK win32WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, kWindowBackendSlot, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* window = reinterpret_cast<Win32Window*>(::GetWindowLongPtrW(hwnd, kWindowBackendSlot));
    if (window != nullptr) {
        if (std::optional<LRESULT> result = window->handleMessage(message, wparam, lparam); result.has_value()) {
            return *result;
        }
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

std::optional<LRESULT> Win32Window::handleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CLOSE: {
            CloseRequestedEvent payload{};
            payload.cancellable = true;
            Event event = makeEvent(EventType::CloseRequested, m_id);
            event.payload = payload;
            m_platform.queue().push(std::move(event));
            return 0; // the application decides; see requestClose()
        }

        case WM_DESTROY:
            m_destroyed = true;
            return 0;

        case WM_SIZE: {
            const Size before = m_clientSize.get();
            syncGeometry();
            if (wparam == SIZE_MINIMIZED) {
                pushState(EventType::Minimized, true);
                return 0;
            }
            if (wparam == SIZE_MAXIMIZED) {
                pushState(EventType::Maximized, true);
            } else if (wparam == SIZE_RESTORED) {
                pushState(EventType::Restored, true);
            }
            if (before.width != m_clientSize.get().width || before.height != m_clientSize.get().height) {
                ResizeEvent payload{};
                payload.clientSize = m_clientSize.get();
                payload.windowSize = m_windowSize.get();
                payload.framebufferSize = framebufferSize();
                payload.inLiveResize = false;
                Event event = makeEvent(EventType::Resized, m_id);
                event.payload = payload;
                m_platform.queue().push(std::move(event));
            }
            return 0;
        }

        case WM_MOVE: {
            syncGeometry();
            MoveEvent payload{};
            payload.position = m_position.get();
            payload.globalPositionKnown = true;
            Event event = makeEvent(EventType::Moved, m_id);
            event.payload = payload;
            m_platform.queue().push(std::move(event));
            return 0;
        }

        case WM_DPICHANGED: {
            // Contract: adopt the suggested rectangle, emit ScaleFactorChanged,
            // then Resized (see include/nativebridge/event.h).
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) {
                ::SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top,
                               suggested->right - suggested->left, suggested->bottom - suggested->top,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            }
            syncScaleFactor(true);
            syncGeometry();
            ResizeEvent payload{};
            payload.clientSize = m_clientSize.get();
            payload.windowSize = m_windowSize.get();
            payload.framebufferSize = framebufferSize();
            Event event = makeEvent(EventType::Resized, m_id);
            event.payload = payload;
            m_platform.queue().push(std::move(event));
            return 0;
        }

        case WM_GETMINMAXINFO:
            applyMinMaxInfo(*reinterpret_cast<MINMAXINFO*>(lparam));
            return 0;

        case WM_SETFOCUS:
            pushFocus(true);
            return 0;

        case WM_KILLFOCUS:
            pushFocus(false);
            return 0;

        case WM_SHOWWINDOW:
            m_visible.set(wparam != FALSE);
            {
                VisibilityEvent payload{};
                payload.visible = wparam != FALSE;
                Event event = makeEvent(payload.visible ? EventType::Shown : EventType::Hidden, m_id);
                event.payload = payload;
                m_platform.queue().push(std::move(event));
            }
            return 0;

        case WM_ACTIVATE:
            // WA_INACTIVE == 0
            pushFocus(LOWORD(wparam) != 0);
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            pushKeyEvent(message, wparam, lparam);
            return 0;

        case WM_CHAR:
        case WM_SYSCHAR: {
            if (wparam >= 0x20 && wparam != 0x7F) {
                TextInputEvent payload{};
                // wParam is a UTF-16 code unit; characters outside the BMP arrive
                // as two WM_CHAR messages, so encode each unit separately.
                const wchar_t unit = static_cast<wchar_t>(wparam);
                if (unit < 0x80) {
                    payload.utf8.push_back(static_cast<char>(unit));
                } else {
                    payload.utf8 = toUtf8(std::wstring(1, unit));
                }
                if (!payload.utf8.empty()) {
                    Event event = makeEvent(EventType::TextInput, m_id);
                    event.payload = std::move(payload);
                    m_platform.queue().push(std::move(event));
                }
            }
            return 0;
        }

        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            pushMouseButton(message, wparam, lparam);
            return 0;

        case WM_MOUSEMOVE:
            pushMouseMove(wparam, lparam);
            return 0;

        case WM_MOUSELEAVE:
            m_trackingMouse = false;
            pushMouseEnter(false);
            return 0;

        case WM_MOUSEWHEEL:
            pushMouseWheel(wparam, lparam, false);
            return 0;

        case WM_MOUSEHWHEEL:
            pushMouseWheel(wparam, lparam, true);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                updateCursor();
                return TRUE;
            }
            return std::nullopt;

        case WM_ERASEBKGND:
            return TRUE; // the renderer owns the client area; avoid flicker

        case WM_SETTINGCHANGE:
        case WM_DISPLAYCHANGE:
            m_platform.broadcastDisplayChange();
            return 0;

        case WM_SYSCOMMAND:
            // Block the screensaver while a fullscreen window is focused.
            if ((wparam & 0xFFF0) == SC_SCREENSAVE && m_fullscreen.get()) {
                return 0;
            }
            return std::nullopt;

        case WM_NCHITTEST:
            return std::nullopt; // default hit testing handles resize borders

        default:
            return std::nullopt;
    }
}

} // namespace nativebridge::detail

// ---------------------------------------------------------------------------
// Input translation
// ---------------------------------------------------------------------------

void Win32Window::pushKeyEvent(UINT message, WPARAM wparam, LPARAM lparam) {
    const bool isDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    KeyEvent payload{};
    payload.key = win32KeyCode(wparam, lparam);
    payload.nativeKeyCode = static_cast<uint32_t>(wparam);
    payload.nativeScanCode = static_cast<uint32_t>((lparam >> 16) & 0xFFu);
    payload.modifiers = win32Modifiers();
    // Bit 30 of lParam is set on auto-repeat.
    payload.isRepeat = isDown && (lparam & (1LL << 30)) != 0;
    payload.action = !isDown ? KeyAction::Release : (payload.isRepeat ? KeyAction::Repeat : KeyAction::Press);

    Event event = makeEvent(isDown ? EventType::KeyDown : EventType::KeyUp, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void Win32Window::pushMouseButton(UINT message, WPARAM wparam, LPARAM lparam) {
    const bool isDown = message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN ||
                        message == WM_XBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_MBUTTONDBLCLK ||
                        message == WM_RBUTTONDBLCLK || message == WM_XBUTTONDBLCLK;

    MouseButton button = win32MouseButton(message);
    if (button == MouseButton::X1) {
        // Distinguish the two side buttons from the high word of wParam.
        button = (GET_XBUTTON_WPARAM(wparam) == XBUTTON2) ? MouseButton::X2 : MouseButton::X1;
    }

    MouseButtonEvent payload{};
    payload.button = button;
    payload.position = clientPointFromLParam(lparam);
    payload.modifiers = win32Modifiers();
    payload.clickCount = 1;
    if (message == WM_LBUTTONDBLCLK || message == WM_MBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
        message == WM_XBUTTONDBLCLK) {
        payload.clickCount = 2;
    }

    Event event = makeEvent(isDown ? EventType::MouseDown : EventType::MouseUp, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void Win32Window::pushMouseMove(WPARAM buttonState, LPARAM lparam) {
    if (!m_trackingMouse) {
        // Ask for WM_MOUSELEAVE so MouseLeft is delivered exactly once.
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = m_hwnd;
        if (::TrackMouseEvent(&track) != FALSE) {
            m_trackingMouse = true;
            pushMouseEnter(true);
        }
    }
    m_buttonState = buttonState;

    MouseMoveEvent payload{};
    payload.position = clientPointFromLParam(lparam);
    payload.delta = m_hasLastMouse
                        ? Point{payload.position.x - m_lastMouse.x, payload.position.y - m_lastMouse.y}
                        : Point{0, 0};
    payload.modifiers = win32Modifiers();
    payload.isDragging = (buttonState & MK_LBUTTON) != 0 || (buttonState & MK_RBUTTON) != 0 ||
                         (buttonState & MK_MBUTTON) != 0;
    m_lastMouse = payload.position;
    m_hasLastMouse = true;

    Event event = makeEvent(EventType::MouseMoved, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void Win32Window::pushMouseWheel(WPARAM wparam, LPARAM lparam, bool horizontal) {
    // WM_MOUSEWHEEL reports screen coordinates, so convert them to the client area.
    POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ::ScreenToClient(m_hwnd, &screen);

    MouseWheelEvent payload{};
    payload.position = Point{screen.x, screen.y};
    // GET_WHEEL_DELTA_WPARAM is a multiple of WHEEL_DELTA (120) per notch; one
    // notch is one line in nativeBridge units.
    const double notches = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<double>(WHEEL_DELTA);
    if (horizontal) {
        payload.deltaX = notches;
    } else {
        payload.deltaY = notches;
    }
    payload.precise = false;
    payload.modifiers = win32Modifiers();

    Event event = makeEvent(EventType::MouseWheel, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void Win32Window::pushMouseEnter(bool entered) {
    MouseEnterEvent payload{};
    payload.position = m_lastMouse;
    payload.entered = entered;
    Event event = makeEvent(entered ? EventType::MouseEntered : EventType::MouseLeft, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void Win32Window::pushFocus(bool focused) {
    m_focused.set(focused);
    FocusEvent payload{};
    payload.focused = focused;
    Event event = makeEvent(focused ? EventType::FocusGained : EventType::FocusLost, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void Win32Window::pushState(EventType type, bool active) {
    switch (type) {
        case EventType::Minimized:  m_minimized.set(active); break;
        case EventType::Maximized:  m_maximized.set(active); break;
        case EventType::Restored:   m_minimized.set(false); m_maximized.set(false); break;
        default: break;
    }
    StateEvent payload{};
    payload.active = active;
    Event event = makeEvent(type, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

Point Win32Window::clientPointFromLParam(LPARAM lparam) const {
    // Client coordinates are already logical in a per-monitor DPI aware process.
    return Point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
}

void Win32Window::applyMinMaxInfo(MINMAXINFO& info) const {
    const UINT dpi = dpiForWindow(m_hwnd);
    if (m_minSize.width > 0 || m_minSize.height > 0) {
        const RECT rect = windowRectForClientSize(m_minSize, m_style, m_exStyle, FALSE, dpi);
        info.ptMinTrackSize.x = rect.right - rect.left;
        info.ptMinTrackSize.y = rect.bottom - rect.top;
    }
    if (m_maxSize.width > 0 || m_maxSize.height > 0) {
        const RECT rect = windowRectForClientSize(m_maxSize, m_style, m_exStyle, FALSE, dpi);
        info.ptMaxTrackSize.x = rect.right - rect.left;
        info.ptMaxTrackSize.y = rect.bottom - rect.top;
    }
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

RECT Win32Window::desiredWindowRect(Size clientSize, DWORD style, DWORD exStyle) const {
    return windowRectForClientSize(clientSize, style, exStyle, FALSE, dpiForWindow(m_hwnd));
}

void Win32Window::syncGeometry() {
    if (m_hwnd == nullptr) {
        return;
    }
    RECT client{};
    if (::GetClientRect(m_hwnd, &client) != FALSE) {
        m_clientSize.set(Size{client.right - client.left, client.bottom - client.top});
    }
    RECT window{};
    if (::GetWindowRect(m_hwnd, &window) != FALSE) {
        m_windowSize.set(Size{window.right - window.left, window.bottom - window.top});
        // In a per-monitor DPI aware process GetWindowRect already returns
        // logical (virtual desktop) coordinates.
        m_position.set(Point{window.left, window.top});
    }
}

void Win32Window::syncScaleFactor(bool emitEvent) {
    const double before = m_scaleFactor.get();
    const double scale = scaleFactorForWindow(m_hwnd);
    m_scaleFactor.set(scale);
    if (!emitEvent || scale == before) {
        return;
    }
    ScaleFactorEvent payload{};
    payload.scaleFactor = scale;
    Event event = makeEvent(EventType::ScaleFactorChanged, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void Win32Window::centerOnMonitor() {
    HMONITOR monitor = ::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == FALSE) {
        return;
    }
    RECT window{};
    if (::GetWindowRect(m_hwnd, &window) == FALSE) {
        return;
    }
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    const int x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2;
    const int y = info.rcWork.top + ((info.rcWork.bottom - info.rcWork.top) - height) / 2;
    ::SetWindowPos(m_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32Window::updateCursor() {
    if (m_customCursor != nullptr) {
        ::SetCursor(m_customCursor);
        return;
    }
    if (win32CursorIsHidden(m_cursorShape) || !m_cursorVisible) {
        ::SetCursor(nullptr);
        return;
    }
    ::SetCursor(win32CursorForShape(m_cursorShape));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Status Win32Window::setTitle(std::string_view title) {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "setTitle: window is gone");
    }
    ::SetWindowTextW(m_hwnd, toUtf16(title).c_str());
    return okStatus();
}

Status Win32Window::resize(Size clientSize) {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "resize: window is gone");
    }
    if (clientSize.width <= 0 || clientSize.height <= 0) {
        return failStatus(Errc::InvalidArgument, "resize: client size must be positive");
    }
    const RECT rect = desiredWindowRect(clientSize, m_style, m_exStyle);
    if (::SetWindowPos(m_hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) == FALSE) {
        return failStatus(Errc::PlatformFailure, "resize: SetWindowPos failed",
                          static_cast<int64_t>(::GetLastError()));
    }
    return okStatus();
}

Status Win32Window::move(Point position) {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "move: window is gone");
    }
    if (::SetWindowPos(m_hwnd, nullptr, position.x, position.y, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) == FALSE) {
        return failStatus(Errc::PlatformFailure, "move: SetWindowPos failed",
                          static_cast<int64_t>(::GetLastError()));
    }
    return okStatus();
}

Status Win32Window::setMinSize(Size size) {
    m_minSize = size;
    if (m_hwnd != nullptr) {
        // WM_GETMINMAXINFO picks this up on the next resize.
        ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    return okStatus();
}

Status Win32Window::setMaxSize(Size size) {
    m_maxSize = size;
    if (m_hwnd != nullptr) {
        ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    return okStatus();
}

Status Win32Window::show() {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "show: window is gone");
    }
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    m_visible.set(true);
    return okStatus();
}

Status Win32Window::hide() {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "hide: window is gone");
    }
    ::ShowWindow(m_hwnd, SW_HIDE);
    m_visible.set(false);
    return okStatus();
}

Status Win32Window::minimize() {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "minimize: window is gone");
    }
    ::ShowWindow(m_hwnd, SW_MINIMIZE);
    return okStatus();
}

Status Win32Window::maximize() {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "maximize: window is gone");
    }
    ::ShowWindow(m_hwnd, SW_MAXIMIZE);
    return okStatus();
}

Status Win32Window::restore() {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "restore: window is gone");
    }
    ::ShowWindow(m_hwnd, SW_RESTORE);
    return okStatus();
}

Status Win32Window::setFullscreen(bool fullscreen) {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "setFullscreen: window is gone");
    }
    applyFullscreen(fullscreen);
    return okStatus();
}

void Win32Window::applyFullscreen(bool fullscreen) {
    if (fullscreen == m_fullscreenActive) {
        return;
    }
    if (fullscreen) {
        m_savedPlacement.length = sizeof(m_savedPlacement);
        ::GetWindowPlacement(m_hwnd, &m_savedPlacement);
        m_savedStyle = ::GetWindowLongPtrW(m_hwnd, GWL_STYLE);
        m_savedExStyle = ::GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);

        HMONITOR monitor = ::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(monitor, &info) == FALSE) {
            return;
        }
        (void)::SetWindowLongPtrW(m_hwnd, GWL_STYLE, static_cast<LONG_PTR>(WS_POPUP | WS_VISIBLE));
        (void)::SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(m_savedExStyle));
        ::SetWindowPos(m_hwnd, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                       info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                       SWP_FRAMECHANGED | SWP_NOACTIVATE);
        m_fullscreenActive = true;
    } else {
        (void)::SetWindowLongPtrW(m_hwnd, GWL_STYLE, m_savedStyle);
        (void)::SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, m_savedExStyle);
        ::SetWindowPlacement(m_hwnd, &m_savedPlacement);
        ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        m_fullscreenActive = false;
    }
    m_fullscreen.set(m_fullscreenActive);
}

Status Win32Window::setAlwaysOnTop(bool onTop) {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "setAlwaysOnTop: window is gone");
    }
    if (::SetWindowPos(m_hwnd, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) == FALSE) {
        return failStatus(Errc::PlatformFailure, "setAlwaysOnTop: SetWindowPos failed",
                          static_cast<int64_t>(::GetLastError()));
    }
    return okStatus();
}

Status Win32Window::setResizable(bool resizable) {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "setResizable: window is gone");
    }
    LONG_PTR style = ::GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    if (resizable) {
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    } else {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    m_style = static_cast<DWORD>(style);
    (void)::SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);
    ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    m_resizable = resizable;
    return okStatus();
}

Status Win32Window::setDecorations(bool decorations) {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "setDecorations: window is gone");
    }
    LONG_PTR style = ::GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    if (decorations) {
        style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        if (m_resizable) {
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        }
        style &= ~static_cast<LONG_PTR>(WS_POPUP);
    } else {
        style &= ~static_cast<LONG_PTR>(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX);
        style |= WS_POPUP;
    }
    m_style = static_cast<DWORD>(style);
    (void)::SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);
    ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    m_decorations = decorations;
    return okStatus();
}

Status Win32Window::setCursor(CursorShape shape) {
    m_cursorShape = shape;
    m_cursor.set(shape);
    updateCursor();
    return okStatus();
}

Status Win32Window::setCursorVisible(bool visible) {
    m_cursorVisible = visible;
    if (m_hwnd != nullptr) {
        // ShowCursor is a counter; keep it balanced by only calling it on a change.
        static thread_local bool s_hidden = false;
        if (!visible && !s_hidden) {
            ::ShowCursor(FALSE);
            s_hidden = true;
        } else if (visible && s_hidden) {
            ::ShowCursor(TRUE);
            s_hidden = false;
        }
        updateCursor();
    }
    return okStatus();
}

Status Win32Window::setClipRect(const std::optional<Rect>& rect) {
    m_clipRect = rect;
    return okStatus();
}

Status Win32Window::requestAttention() {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "requestAttention: window is gone");
    }
    FLASHWINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = m_hwnd;
    info.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
    info.uCount = 3;
    ::FlashWindowEx(&info);
    return okStatus();
}

Status Win32Window::requestClose() {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "requestClose: window is gone");
    }
    // Same code path as the user clicking the close button: WM_CLOSE, which the
    // application observes through CloseRequested and answers by destroying the
    // window or calling Window::requestClose() again after setting a flag.
    ::PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    return okStatus();
}

Status Win32Window::setCustomCursor(std::span<const uint8_t> bgraPixels, Size size, Point hotspot) {
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "setCustomCursor: window is gone");
    }
    const std::size_t expected = static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) * 4u;
    if (size.width <= 0 || size.height <= 0 || bgraPixels.size() < expected) {
        return failStatus(Errc::InvalidArgument, "setCustomCursor: buffer must hold width*height*4 BGRA bytes");
    }

    // A 32bpp DIB section plus an empty AND mask is the classic way to build a
    // colour cursor with alpha.
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size.width;
    header.bV5Height = -size.height; // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP colour = ::CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits,
                                        nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (colour == nullptr || bits == nullptr) {
        if (colour != nullptr) {
            ::DeleteObject(colour);
        }
        return failStatus(Errc::OutOfMemory, "setCustomCursor: CreateDIBSection failed");
    }
    std::memcpy(bits, bgraPixels.data(), expected);

    HBITMAP mask = ::CreateBitmap(size.width, size.height, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(hotspot.x);
    info.yHotspot = static_cast<DWORD>(hotspot.y);
    info.hbmMask = mask;
    info.hbmColor = colour;
    HCURSOR cursor = ::CreateIconIndirect(&info);
    ::DeleteObject(colour);
    if (mask != nullptr) {
        ::DeleteObject(mask);
    }
    if (cursor == nullptr) {
        return failStatus(Errc::PlatformFailure, "setCustomCursor: CreateIconIndirect failed");
    }
    if (m_customCursor != nullptr) {
        ::DestroyCursor(m_customCursor);
    }
    m_customCursor = cursor;
    updateCursor();
    return okStatus();
}

Status Win32Window::materializeSurface(RenderApi api, std::string_view backendName, NativeHandle& surface) {
    (void)backendName;
    if (m_hwnd == nullptr) {
        return failStatus(Errc::InvalidArgument, "materializeSurface: window is gone");
    }
    switch (api) {
        case RenderApi::Direct3D11:
        case RenderApi::Direct3D12:
        case RenderApi::OpenGL:
        case RenderApi::OpenGLES:
        case RenderApi::Vulkan:
        case RenderApi::WebGPU:
        case RenderApi::Generic:
            // Every Windows graphics API consumes the HWND itself.
            surface = NativeHandle{};
            return okStatus();
        case RenderApi::Metal:
            return failStatus(Errc::Unsupported, "materializeSurface: Metal is not available on Windows");
        default:
            surface = NativeHandle{};
            return okStatus();
    }
}

void Win32Window::onSurfaceResized() { syncGeometry(); }

void Win32Window::onDisplayConfigurationChanged() {
    // ScaleFactorChanged must precede Resized (see event.h): detect first.
    const double before = m_scaleFactor.get();
    syncScaleFactor(false);
    if (m_scaleFactor.get() != before) {
        ScaleFactorEvent payload{};
        payload.scaleFactor = m_scaleFactor.get();
        Event event = makeEvent(EventType::ScaleFactorChanged, m_id);
        event.payload = payload;
        m_platform.queue().push(std::move(event));
    }
    syncGeometry();
    ResizeEvent payload{};
    payload.clientSize = m_clientSize.get();
    payload.windowSize = m_windowSize.get();
    payload.framebufferSize = framebufferSize();
    Event event = makeEvent(EventType::Resized, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void Win32Window::onNativeDestroyed() {
    m_destroyed = true;
    m_hwnd = nullptr; // the platform is going away; do not touch the HWND again
}

} // namespace nativebridge::detail

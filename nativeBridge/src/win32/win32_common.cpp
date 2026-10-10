// nativeBridge — Windows (Win32) backend: shared implementation.
// SPDX-License-Identifier: MIT

#include "win32/win32_common.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common/backend.h"

namespace nativebridge::detail {
namespace {

/// One entry of the Win32 virtual key -> KeyCode table.
struct VkEntry {
    int     virtualKey;
    KeyCode key;
};

// KeyCode values are USB HID usage ids (see include/nativebridge/keycode.h);
// this table is the documented Win32 -> KeyCode mapping of the library.
constexpr VkEntry kVkTable[] = {
    {'A', KeyCode::A}, {'B', KeyCode::B}, {'C', KeyCode::C}, {'D', KeyCode::D},
    {'E', KeyCode::E}, {'F', KeyCode::F}, {'G', KeyCode::G}, {'H', KeyCode::H},
    {'I', KeyCode::I}, {'J', KeyCode::J}, {'K', KeyCode::K}, {'L', KeyCode::L},
    {'M', KeyCode::M}, {'N', KeyCode::N}, {'O', KeyCode::O}, {'P', KeyCode::P},
    {'Q', KeyCode::Q}, {'R', KeyCode::R}, {'S', KeyCode::S}, {'T', KeyCode::T},
    {'U', KeyCode::U}, {'V', KeyCode::V}, {'W', KeyCode::W}, {'X', KeyCode::X},
    {'Y', KeyCode::Y}, {'Z', KeyCode::Z},
    {'0', KeyCode::Digit0}, {'1', KeyCode::Digit1}, {'2', KeyCode::Digit2}, {'3', KeyCode::Digit3},
    {'4', KeyCode::Digit4}, {'5', KeyCode::Digit5}, {'6', KeyCode::Digit6}, {'7', KeyCode::Digit7},
    {'8', KeyCode::Digit8}, {'9', KeyCode::Digit9},
    {VK_RETURN, KeyCode::Enter},
    {VK_ESCAPE, KeyCode::Escape},
    {VK_BACK, KeyCode::Backspace},
    {VK_TAB, KeyCode::Tab},
    {VK_SPACE, KeyCode::Space},
    {VK_OEM_MINUS, KeyCode::Minus},
    {VK_OEM_PLUS, KeyCode::Equal},
    {VK_OEM_4, KeyCode::LeftBracket},
    {VK_OEM_6, KeyCode::RightBracket},
    {VK_OEM_5, KeyCode::Backslash},
    {VK_OEM_7, KeyCode::Apostrophe},
    {VK_OEM_3, KeyCode::Grave},
    {VK_OEM_COMMA, KeyCode::Comma},
    {VK_OEM_PERIOD, KeyCode::Period},
    {VK_OEM_2, KeyCode::Slash},
    {VK_OEM_1, KeyCode::Semicolon},
    {VK_OEM_102, KeyCode::NonUsBackslash},
    {VK_CAPITAL, KeyCode::CapsLock},
    {VK_F1, KeyCode::F1}, {VK_F2, KeyCode::F2}, {VK_F3, KeyCode::F3}, {VK_F4, KeyCode::F4},
    {VK_F5, KeyCode::F5}, {VK_F6, KeyCode::F6}, {VK_F7, KeyCode::F7}, {VK_F8, KeyCode::F8},
    {VK_F9, KeyCode::F9}, {VK_F10, KeyCode::F10}, {VK_F11, KeyCode::F11}, {VK_F12, KeyCode::F12},
    {VK_F13, KeyCode::F13}, {VK_F14, KeyCode::F14}, {VK_F15, KeyCode::F15}, {VK_F16, KeyCode::F16},
    {VK_F17, KeyCode::F17}, {VK_F18, KeyCode::F18}, {VK_F19, KeyCode::F19}, {VK_F20, KeyCode::F20},
    {VK_F21, KeyCode::F21}, {VK_F22, KeyCode::F22}, {VK_F23, KeyCode::F23}, {VK_F24, KeyCode::F24},
    {VK_SNAPSHOT, KeyCode::PrintScreen},
    {VK_SCROLL, KeyCode::ScrollLock},
    {VK_PAUSE, KeyCode::Pause},
    {VK_INSERT, KeyCode::Insert},
    {VK_HOME, KeyCode::Home},
    {VK_PRIOR, KeyCode::PageUp},
    {VK_DELETE, KeyCode::Delete},
    {VK_END, KeyCode::End},
    {VK_NEXT, KeyCode::PageDown},
    {VK_RIGHT, KeyCode::ArrowRight},
    {VK_LEFT, KeyCode::ArrowLeft},
    {VK_DOWN, KeyCode::ArrowDown},
    {VK_UP, KeyCode::ArrowUp},
    {VK_NUMLOCK, KeyCode::NumLock},
    {VK_DIVIDE, KeyCode::NumpadDivide},
    {VK_MULTIPLY, KeyCode::NumpadMultiply},
    {VK_SUBTRACT, KeyCode::NumpadSubtract},
    {VK_ADD, KeyCode::NumpadAdd},
    {VK_DECIMAL, KeyCode::NumpadDecimal},
    {VK_NUMPAD0, KeyCode::Numpad0}, {VK_NUMPAD1, KeyCode::Numpad1},
    {VK_NUMPAD2, KeyCode::Numpad2}, {VK_NUMPAD3, KeyCode::Numpad3},
    {VK_NUMPAD4, KeyCode::Numpad4}, {VK_NUMPAD5, KeyCode::Numpad5},
    {VK_NUMPAD6, KeyCode::Numpad6}, {VK_NUMPAD7, KeyCode::Numpad7},
    {VK_NUMPAD8, KeyCode::Numpad8}, {VK_NUMPAD9, KeyCode::Numpad9},
    {VK_APPS, KeyCode::Application},
    {VK_LWIN, KeyCode::LeftGui},
    {VK_RWIN, KeyCode::RightGui},
    {VK_LSHIFT, KeyCode::LeftShift},
    {VK_RSHIFT, KeyCode::RightShift},
    {VK_LCONTROL, KeyCode::LeftControl},
    {VK_RCONTROL, KeyCode::RightControl},
    {VK_LMENU, KeyCode::LeftAlt},
    {VK_RMENU, KeyCode::RightAlt},
    {VK_BROWSER_BACK, KeyCode::MediaPrevious},
    {VK_BROWSER_FORWARD, KeyCode::MediaNext},
    {VK_MEDIA_PLAY_PAUSE, KeyCode::MediaPlayPause},
    {VK_MEDIA_STOP, KeyCode::MediaStop},
    {VK_VOLUME_MUTE, KeyCode::Mute},
    {VK_VOLUME_DOWN, KeyCode::VolumeDown},
    {VK_VOLUME_UP, KeyCode::VolumeUp},
    {VK_OEM_8, KeyCode::NonUsHash},
    {VK_OEM_NEC_EQUAL, KeyCode::NumpadEqual},
    {VK_KANA, KeyCode::Lang1},
    {VK_HANJA, KeyCode::Lang2},
    {VK_CONVERT, KeyCode::International4},
    {VK_NONCONVERT, KeyCode::International5},
    {VK_HELP, KeyCode::Help},
    {VK_SELECT, KeyCode::Select},
    {VK_EXECUTE, KeyCode::Execute},
};

/// Left/right distinction for the ambiguous VK_SHIFT/VK_CONTROL/VK_MENU values.
KeyCode disambiguateModifier(WPARAM virtualKey, LPARAM keyData) {
    const UINT scancode = static_cast<UINT>((keyData >> 16) & 0xFFu);
    const bool extended = (keyData & (1LL << 24)) != 0;
    switch (virtualKey) {
        case VK_SHIFT: {
            const UINT left = ::MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
            return scancode == left ? KeyCode::LeftShift : KeyCode::RightShift;
        }
        case VK_CONTROL:
            return extended ? KeyCode::RightControl : KeyCode::LeftControl;
        case VK_MENU:
            return extended ? KeyCode::RightAlt : KeyCode::LeftAlt;
        default:
            return KeyCode::Unknown;
    }
}

} // namespace

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring toUtf16(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

void enableDpiAwareness() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll"); user32 != nullptr) {
        auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext != nullptr && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
            return;
        }
    }
    if (HMODULE shcore = ::LoadLibraryW(L"shcore.dll"); shcore != nullptr) {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto setAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(::GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setAwareness != nullptr) {
            (void)setAwareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */);
            ::FreeLibrary(shcore);
            return;
        }
        ::FreeLibrary(shcore);
    }
    ::SetProcessDPIAware();
}

uint32_t dpiForWindow(HWND window) {
    if (window != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll"); user32 != nullptr) {
            auto getDpi = reinterpret_cast<GetDpiForWindowFn>(::GetProcAddress(user32, "GetDpiForWindow"));
            if (getDpi != nullptr) {
                const UINT dpi = getDpi(window);
                if (dpi != 0) {
                    return dpi;
                }
            }
        }
    }
    return 96;
}

uint32_t dpiForMonitorAt(POINT screenPoint) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = ::LoadLibraryW(L"shcore.dll"); shcore != nullptr) {
        auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(::GetProcAddress(shcore, "GetDpiForMonitor"));
        if (getDpi != nullptr) {
            HMONITOR monitor = ::MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
            UINT dpiX = 96;
            UINT dpiY = 96;
            // MDT_EFFECTIVE_DPI == 0
            if (SUCCEEDED(getDpi(monitor, 0, &dpiX, &dpiY))) {
                ::FreeLibrary(shcore);
                return dpiX;
            }
        }
        ::FreeLibrary(shcore);
    }
    return 96;
}

double scaleFactorForWindow(HWND window) { return static_cast<double>(dpiForWindow(window)) / 96.0; }

KeyCode win32KeyCode(WPARAM virtualKey, LPARAM keyData) noexcept {
    const KeyCode modifier = disambiguateModifier(virtualKey, keyData);
    if (modifier != KeyCode::Unknown) {
        return modifier;
    }
    // Prefer the physical scancode so the mapping stays layout independent for
    // the letter/number row (a French AZERTY 'A' is still KeyCode::Q).
    const UINT scancode = static_cast<UINT>((keyData >> 16) & 0xFFu);
    if (scancode != 0) {
        const UINT mapped = ::MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
        if (mapped != 0) {
            for (const auto& entry : kVkTable) {
                if (static_cast<UINT>(entry.virtualKey) == mapped) {
                    return entry.key;
                }
            }
        }
    }
    for (const auto& entry : kVkTable) {
        if (static_cast<WPARAM>(entry.virtualKey) == virtualKey) {
            return entry.key;
        }
    }
    return KeyCode::Unknown;
}

Modifier win32Modifiers() noexcept {
    Modifier modifiers = Modifier::None;
    if (::GetKeyState(VK_SHIFT) < 0) { modifiers |= Modifier::Shift; }
    if (::GetKeyState(VK_CONTROL) < 0) { modifiers |= Modifier::Control; }
    if (::GetKeyState(VK_MENU) < 0) { modifiers |= Modifier::Alt; }
    if ((::GetKeyState(VK_LWIN) < 0) || (::GetKeyState(VK_RWIN) < 0)) { modifiers |= Modifier::Super; }
    if ((::GetKeyState(VK_CAPITAL) & 1) != 0) { modifiers |= Modifier::CapsLock; }
    if ((::GetKeyState(VK_NUMLOCK) & 1) != 0) { modifiers |= Modifier::NumLock; }
    return modifiers;
}

MouseButton win32MouseButton(UINT message) noexcept {
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            return MouseButton::Left;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
            return MouseButton::Middle;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
            return MouseButton::Right;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            return MouseButton::X1; // refined by the caller from the high word
        default:
            return MouseButton::Unknown;
    }
}

HCURSOR win32CursorForShape(CursorShape shape) {
    switch (shape) {
        case CursorShape::IBeam:           return ::LoadCursorW(nullptr, IDC_IBEAM);
        case CursorShape::Crosshair:       return ::LoadCursorW(nullptr, IDC_CROSS);
        case CursorShape::Hand:            return ::LoadCursorW(nullptr, IDC_HAND);
        case CursorShape::ResizeLeftRight: return ::LoadCursorW(nullptr, IDC_SIZEWE);
        case CursorShape::ResizeUpDown:    return ::LoadCursorW(nullptr, IDC_SIZENS);
        case CursorShape::ResizeAll:       return ::LoadCursorW(nullptr, IDC_SIZEALL);
        case CursorShape::NotAllowed:      return ::LoadCursorW(nullptr, IDC_NO);
        case CursorShape::Hidden:
        case CursorShape::Arrow:
        default:                           return ::LoadCursorW(nullptr, IDC_ARROW);
    }
}

bool win32CursorIsHidden(CursorShape shape) noexcept { return shape == CursorShape::Hidden; }

Event makeEvent(EventType type, WindowId id) {
    Event event{};
    event.type = type;
    event.window = id;
    event.timestampNs = monotonicNowNs();
    return event;
}

} // namespace nativebridge::detail

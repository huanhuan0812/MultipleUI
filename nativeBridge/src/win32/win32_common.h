// nativeBridge — Windows (Win32) backend internals: shared declarations.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API. The four .cpp files of the Win32 backend
// (win32_common.cpp, win32_window.cpp, win32_platform.cpp, win32_events.cpp)
// share these helpers. Keep this header free of anything but declarations:
// definitions live in win32_common.cpp so that they exist exactly once.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "nativebridge/event.h"
#include "nativebridge/keycode.h"
#include "nativebridge/window.h"

namespace nativebridge::detail {

class Win32Window;

// --- String conversion ------------------------------------------------------

/// UTF-16 -> UTF-8 (empty on failure).
std::string toUtf8(const std::wstring& wide);

/// UTF-8 -> UTF-16 (empty on failure).
std::wstring toUtf16(std::string_view utf8);

// --- DPI --------------------------------------------------------------------

/// Opt the process into per-monitor DPI awareness (v2 when available).
void enableDpiAwareness();

/// Effective DPI of a window's monitor; 96 when unknown.
uint32_t dpiForWindow(HWND window);

/// Effective DPI of the monitor containing a screen point; 96 when unknown.
uint32_t dpiForMonitorAt(POINT screenPoint);

/// Scale factor (dpi / 96.0) of a window's monitor.
double scaleFactorForWindow(HWND window);

// --- Input mapping ----------------------------------------------------------

/// KeyCode for a Win32 virtual key (WM_KEYDOWN's wParam) plus its scancode.
KeyCode win32KeyCode(WPARAM virtualKey, LPARAM keyData) noexcept;

/// Win32 virtual key -> Modifier mask, using the current keyboard state.
Modifier win32Modifiers() noexcept;

/// MouseButton for the WM_*BUTTON* message id (WM_LBUTTONDOWN and friends).
MouseButton win32MouseButton(UINT message) noexcept;

/// Cursor handle for a shape; the sentinel value for CursorShape::Hidden.
HCURSOR win32CursorForShape(CursorShape shape);

/// True when the shape hides the cursor (handled with ShowCursor instead).
bool win32CursorIsHidden(CursorShape shape) noexcept;

// --- Misc -------------------------------------------------------------------

/// Stamp a freshly created event with this window and the monotonic clock.
Event makeEvent(EventType type, WindowId id);

/// Window long pointer slot used to store the WindowBackend* on an HWND.
inline constexpr int kWindowBackendSlot = GWLP_USERDATA;

/// Shared WndProc for every nativeBridge window (defined in win32_window.cpp).
LRESULT CALLBACK win32WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

/// Window class name used by every nativeBridge window.
const wchar_t* win32WindowClassName() noexcept;

/// Register the nativeBridge window class once; returns false on failure.
bool win32RegisterWindowClass(ATOM& outAtom, std::wstring& outName);

} // namespace nativebridge::detail

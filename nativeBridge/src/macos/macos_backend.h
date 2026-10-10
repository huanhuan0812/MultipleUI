// nativeBridge — macOS backend entry points.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API.
//
// This header is the only interface between the Cocoa translation units
// (cocoa_window.mm, cocoa_platform.mm). It deliberately avoids including any
// AppKit header so that it can be included from plain C++ code.

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>

#include "common/backend.h"
#include "common/lifetime.h"
#include "nativebridge/geometry.h"
#include "nativebridge/system.h"
#include "nativebridge/window.h"

namespace nativebridge::detail {

class CocoaPlatform; // defined in cocoa_backend.h

// --- Factories --------------------------------------------------------------

/// Create the Cocoa platform backend. Must be called on the main thread.
[[nodiscard]] Result<std::unique_ptr<PlatformBackend>> createCocoaPlatform();

/// Create a window backend (cocoa_window.mm). `lifetime` is a weak reference to
/// the owning platform: the window uses it to deregister itself without ever
/// dereferencing a destroyed platform.
[[nodiscard]] Result<std::unique_ptr<WindowBackend>> createCocoaWindow(const WeakLifetimeToken& lifetime,
                                                                      std::thread::id uiThread,
                                                                      WindowId id,
                                                                      EventQueue* queue,
                                                                      const WindowCreateInfo& info);

/// Create the Cocoa clipboard backend (bound to NSPasteboard.generalPasteboard).
[[nodiscard]] std::unique_ptr<Clipboard> createCocoaClipboard();

// --- Cocoa space helpers (cocoa_window.mm) ---------------------------------

/// Recompute the pivot used to convert Cocoa's bottom-left space into
/// nativeBridge's top-left space. Called on startup and on every screen change.
void nativebridgeRefreshCocoaFlipReference();

/// Convert a Cocoa frame (bottom-left origin) into a logical desktop rect
/// (top-left origin). Implemented in cocoa_window.mm.
[[nodiscard]] Rect cocoaFrameToDesktopRect(double x, double y, double width, double height);

/// Same conversion for a native screen rectangle, expressed as a plain struct so
/// that this header stays free of AppKit types (pass the four frame components).
struct CocoaScreenFrame {
    double x{0.0};
    double y{0.0};
    double width{0.0};
    double height{0.0};
};

// --- Input mapping ----------------------------------------------------------

/// KeyCode for an NSEvent keyCode (hardware/ANSI virtual key code).
[[nodiscard]] KeyCode macKeyCodeFromVirtualKey(uint16_t virtualKey) noexcept;

/// KeyCode name for diagnostics.
[[nodiscard]] std::string_view macVirtualKeyName(uint16_t virtualKey) noexcept;

} // namespace nativebridge::detail

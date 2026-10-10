// nativeBridge — internal backend contracts.
// SPDX-License-Identifier: MIT
//
// THIS HEADER IS NOT PART OF THE PUBLIC API. It lives under src/common/ and
// must never be installed. It is the only place where the platform backends and
// the platform-independent common code agree on an interface.
//
// THREADING MODEL
// ---------------
// * A PlatformBackend is created on the UI thread and destroyed on the UI
//   thread. Every virtual method of PlatformBackend and WindowBackend is called
//   from the UI thread, except the "snapshot" getters and the stateful command
//   methods of WindowBackend, which are documented individually.
// * Backends are responsible for their own internal locking: the queue and the
//   snapshot cache are guarded so that any-thread getters are data race free.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nativebridge/display.h"
#include "nativebridge/event.h"
#include "nativebridge/nativebridge.h"
#include "nativebridge/window.h"

namespace nativebridge::detail {

// ---------------------------------------------------------------------------
// Small helpers shared by all backends (defined in src/common/backend.cpp).
// ---------------------------------------------------------------------------

namespace log {

/// Emit a diagnostic line to stderr, prefixed with "[nativebridge] ".
/// Enabled by NATIVEBRIDGE_ENABLE_LOGGING or a debug build.
void info(std::string_view message);
void warn(std::string_view message);
void error(std::string_view message);

} // namespace log

// ---------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------

/// Concrete SurfaceBridge handed out by Platform::surfaceBridge(). Declared
/// here (defined in src/common/platform.cpp) so that the facade and the bridge
/// can be friends of Window without exposing anything publicly.
class SurfaceBridgeImpl;

/// Immutable fallback returned for an invalid window.
class NullSurfaceBridge;

/// Owns the connection to the windowing system and creates windows.
class PlatformBackend {
public:
    virtual ~PlatformBackend() = default;

    // --- Identity ----------------------------------------------------------
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool supports(Capability capability) const noexcept = 0;
    [[nodiscard]] virtual std::string describe() const = 0;

    // --- Event loop --------------------------------------------------------
    /// Drain pending platform messages into the internal queue, apply any
    /// pending commands, and refresh cached state. Must not invoke listeners or
    /// callbacks (the public Platform does that). Returns the number of raw
    /// events collected. Runs on the UI thread.
    virtual void pump() = 0;

    /// Block until at least one raw event is available, `timeout` expired, or a
    /// pending command needs servicing. A negative timeout blocks indefinitely.
    /// Returns the number of raw events collected. Runs on the UI thread.
    virtual std::size_t wait(std::chrono::milliseconds timeout) = 0;

    /// Called by Platform after every pump/wait: take ownership of the events
    /// collected so far. Implementations must clear their queue.
    virtual std::vector<Event> takeEvents() = 0;

    /// Refresh the display snapshot (called by Platform once per pump).
    virtual void refreshDisplays() = 0;
    [[nodiscard]] virtual const DisplayList& displays() const noexcept = 0;

    // --- Factories ---------------------------------------------------------
    [[nodiscard]] virtual Result<std::unique_ptr<WindowBackend>> createWindow(const WindowCreateInfo& info) = 0;
    [[nodiscard]] virtual Clipboard& clipboard() = 0;

    /// Begin destruction of a window from the UI thread. Default implementation
    /// simply releases the handle; backends with deferred teardown override it.
    virtual void destroyWindow(WindowBackend* window) = 0;

    /// Last backend level failure (connection loss, protocol error, ...).
    [[nodiscard]] virtual std::optional<Error> fatalError() const = 0;
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

/// Factory signature used by every backend translation unit.
using BackendFactory = Result<std::unique_ptr<PlatformBackend>> (*)();

/// Backends self-register here during static initialization (before main()).
/// Thread safe. `name` must be "win32", "cocoa", "x11" or "wayland".
void registerBackend(std::string_view name, BackendFactory factory);

/// Look up a backend factory by name; nullptr when it was not compiled in.
[[nodiscard]] BackendFactory findBackend(std::string_view name);

/// Names of the compiled-in backends, in preferred order.
[[nodiscard]] std::vector<std::string> registeredBackendNames();

/// Name of the backend compiled as the Linux default ("x11", "wayland" or
/// "" when NATIVEBRIDGE_LINUX_BACKEND was not defined for this build).
[[nodiscard]] std::string_view compiledDefaultLinuxBackend();

// ---------------------------------------------------------------------------
// Shared input mapping helpers (src/common/keymap.cpp)
// ---------------------------------------------------------------------------

/// X11 keysym / evdev-keysym -> KeyCode. Shared by the X11 and Wayland
/// backends so both produce identical KeyCode values.
[[nodiscard]] KeyCode keyCodeFromKeysym(uint32_t keysym) noexcept;

/// ASCII value of a keysym when it is printable, else 0.
[[nodiscard]] uint32_t characterFromKeysym(uint32_t keysym) noexcept;

// ---------------------------------------------------------------------------
// Shared geometry helpers (src/common/backend.cpp)
// ---------------------------------------------------------------------------

[[nodiscard]] inline PixelSize logicalToPhysical(Size logical, double scale) noexcept {
    const double s = scale > 0.0 ? scale : 1.0;
    return PixelSize{static_cast<uint32_t>(static_cast<double>(logical.width) * s + 0.5),
                     static_cast<uint32_t>(static_cast<double>(logical.height) * s + 0.5)};
}

[[nodiscard]] inline Size physicalToLogical(PixelSize physical, double scale) noexcept {
    const double s = scale > 0.0 ? scale : 1.0;
    return Size{static_cast<int32_t>(static_cast<double>(physical.width) / s + 0.5),
                static_cast<int32_t>(static_cast<double>(physical.height) / s + 0.5)};
}

} // namespace nativebridge::detail

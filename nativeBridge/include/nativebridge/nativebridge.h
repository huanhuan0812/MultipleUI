// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// nativebridge.h — facade / factory entry point.
//
// THREADING MODEL (whole library)
// -------------------------------
// * One Platform instance owns one connection to the windowing system.
// * Platform and every Window created from it are affined to the thread that
//   created the Platform (the "UI thread"). Create, poll and destroy them from
//   that thread only. Moving a Window to another thread requires
//   Window::release() (unsupported on Wayland).
// * Subscriber callbacks run on the UI thread, synchronously, from inside
//   pollEvents()/waitEvents(). Callbacks must not call back into the Platform
//   re-entrantly (no nested pollEvents) and must not destroy the Window that is
//   currently being dispatched.
// * Event payloads are appended to a locked queue from the platform's own
//   message source; polling drains it, so no callback ever runs concurrently.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nativebridge/display.h"
#include "nativebridge/event.h"
#include "nativebridge/geometry.h"
#include "nativebridge/keycode.h"
#include "nativebridge/result.h"
#include "nativebridge/surface.h"
#include "nativebridge/system.h"
#include "nativebridge/window.h"

namespace nativebridge {

namespace detail {
class PlatformBackend;
} // namespace detail

/// Options for Platform::create().
struct PlatformCreateInfo {
    /// Preferred backend. Auto = follow the compile-time option + env var.
    enum class Backend : uint8_t { Auto, Win32, Cocoa, X11, Wayland };
    Backend backend{Backend::Auto};

    /// Alias kept for readability at call sites on Linux.
    static constexpr Backend LinuxAuto = Backend::Auto;

    /// Do not open a connection to the display server (only useful for tests of
    /// the pure system module).
    bool headless{false};
};


/// A Platform is the entry point of nativeBridge: it owns the connection to the
/// windowing system and is the factory for windows, surfaces and the clipboard.
///
/// Typical use:
/// ```cpp
/// auto platform = nativebridge::Platform::create().value();
/// auto window   = platform->createWindow({.title = "hello"}).value();
/// while (platform->pollEvents() > 0) { /* ... */ }
/// ```
///
/// The instance is non-copyable and non-movable; hold it in a smart pointer.
class Platform {
public:
    virtual ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    // --- Construction ------------------------------------------------------

    /// Create a Platform for the current OS.
    ///
    /// Backend selection on Linux honours, in order:
    ///   1. `info.linuxBackend` when set to something other than Auto.
    ///   2. env var NATIVEBRIDGE_LINUX_BACKEND=x11|wayland (case insensitive).
    ///   3. the compile-time option NATIVEBRIDGE_LINUX_BACKEND.
    /// In Auto mode both compiled-in backends are tried, Wayland first, and the
    /// first one that connects successfully wins. Failures are accumulated into
    /// the returned error's `message`.
    [[nodiscard]] static Result<std::unique_ptr<Platform>> create(const PlatformCreateInfo& info = {});
    [[nodiscard]] static Result<std::unique_ptr<Platform>> createDefault();

    /// INTERNAL. Do not call: Platform instances come from create().
    /// Public only so that the factory can use std::make_unique() (the library
    /// contains no raw `new`); the argument type is an internal backend that
    /// user code cannot name or obtain.
    explicit Platform(std::unique_ptr<detail::PlatformBackend> backend);

    // --- Introspection -----------------------------------------------------

    /// "win32" | "cocoa" | "x11" | "wayland"
    [[nodiscard]] std::string_view backendName() const noexcept;

    /// Debug string: backend, connected display name, available backends.
    [[nodiscard]] std::string describe() const;

    /// Backends compiled into this binary, in preferred order
    /// (e.g. {"wayland","x11"} on Linux, {"cocoa"} on macOS).
    [[nodiscard]] static std::vector<std::string> availableBackends();

    /// True when the given platform/backend capability is implemented.
    [[nodiscard]] bool supports(Capability capability) const noexcept;

    // --- Events ------------------------------------------------------------

    /// Drain pending events, invoke subscribers, return the number of events
    /// processed. Callback execution happens on the calling thread.
    /// Must be called from the UI thread.
    [[nodiscard]] std::size_t pollEvents();

    /// Like pollEvents(), but first blocks until at least one event arrives or
    /// the timeout expires. A negative timeout blocks indefinitely.
    /// Returns 0 when the timeout expired with no event.
    [[nodiscard]] std::size_t waitEvents(std::chrono::milliseconds timeout);

    /// Subscribe to *platform level* events (window == 0), e.g. DisplayChanged.
    /// Window bound events are delivered to the owning Window's listeners only.
    /// THREADING: called from the UI thread; the listener runs on the UI thread.
    [[nodiscard]] SubscriptionToken addEventListener(EventListener listener);
    [[nodiscard]] SubscriptionToken addEventListener(EventType type, EventListener listener);
    bool removeEventListener(SubscriptionToken token);

    // --- Windows -----------------------------------------------------------

    /// Create a native window. On success the native handle is already valid
    /// and the WindowCreated event has been queued for the next poll.
    /// Must be called from the UI thread.
    [[nodiscard]] Result<Window> createWindow(const WindowCreateInfo& info = {});

    // --- Displays ----------------------------------------------------------

    /// Snapshot of all monitors, refreshed on each poll.
    [[nodiscard]] const DisplayList& displays() const noexcept;

    /// Snapshot of the primary monitor; std::nullopt when there is none.
    [[nodiscard]] std::optional<DisplayInfo> primaryDisplay() const;

    /// Display whose bounds contain `point` in logical desktop space; falls back
    /// to the primary display. std::nullopt when no display is known.
    [[nodiscard]] std::optional<DisplayInfo> displayAt(Point point) const;

    /// Display the given window currently occupies (best effort; on Wayland this
    /// is the output the surface is mapped on when the compositor reports it).
    [[nodiscard]] std::optional<DisplayInfo> displayOf(const Window& window) const;

    // --- Surfaces ----------------------------------------------------------

    /// Bridge handing out native surface descriptors; no rendering is performed.
    /// The returned reference stays valid as long as the window does. Takes a
    /// mutable window because describing a Metal/OpenGL surface may have to
    /// materialize backend state for it.
    [[nodiscard]] SurfaceBridge& surfaceBridge(Window& window);
    [[nodiscard]] Result<SurfaceDescriptor> describeSurface(const Window& window, RenderApi api);

    // --- Clipboard ---------------------------------------------------------

    /// Text clipboard bound to this platform connection.
    [[nodiscard]] Clipboard& clipboard();
    [[nodiscard]] const Clipboard& clipboard() const;

    // --- Implementation detail (do not use) --------------------------------
    [[nodiscard]] detail::PlatformBackend& backend() noexcept;
    [[nodiscard]] const detail::PlatformBackend& backend() const noexcept;

private:
    /// Refresh the cached display snapshot from the backend. Called at creation
    /// and after every pump.
    void syncDisplays();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Human readable name of the backend actually used by a platform.
[[nodiscard]] std::string_view backendName(const Platform& platform) noexcept;

/// Library version as "major.minor.patch".
[[nodiscard]] constexpr std::string_view version() noexcept { return "0.1.0"; }

/// Feature list compiled into this binary, useful in diagnostics.
[[nodiscard]] std::vector<std::string> compiledFeatures();

} // namespace nativebridge

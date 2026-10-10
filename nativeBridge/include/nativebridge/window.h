// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// window.h — top level window abstraction.
//
// THREADING MODEL
// ---------------
// * A Window belongs to the thread that created it (through Platform::createWindow)
//   and must be destroyed on that thread. Its handle type is not thread safe.
// * Queries (title(), clientSize(), scaleFactor(), isFocused(), ...) read a
//   snapshot cached by the backend under its own lock and may be called from any
//   thread, but the returned values are only as fresh as the last pollEvents().
// * Mutations (setTitle(), resize(), show(), ...) submit a command to the
//   backend. When the backend owns a dedicated thread (Win32, and X11/Wayland
//   when a dedicated connection thread is used) the call blocks either until the
//   command is applied or until a bounded internal timeout, whichever comes
//   first; it never blocks forever and never runs while the event dispatch lock
//   is held by the calling thread (no re-entrancy deadlock).
// * Window is move-only: moving transfers ownership of the native window and all
//   subscriptions. Copy is deleted.
//
// TODO(pimpl): the concrete backend stores its state behind std::unique_ptr;
// the public class holds only the pointer plus a cached snapshot.
//
// WAYLAND LIMITATIONS (documented for callers, enforced by Capabilities)
// ---------------------------------------------------------------------
// * Global window position is not available: position() returns {0,0},
//   supports(Capability::WindowPosition) is false. Use xdg-foreign /
//   window-management protocols (not covered here) if you need placement.
// * Window decorations are compositor supplied. Without xdg-decoration the
//   server may draw none at all; decorationsVisible() then reports false and
//   setDecorations() is a no-op returning Unsupported.
// * Clipboard semantics depend on keyboard focus: a Wayland client may only set
//   or read the selection while one of its surfaces has focus, and reading
//   requires a data-device round trip through the compositor.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nativebridge/event.h"
#include "nativebridge/geometry.h"
#include "nativebridge/keycode.h"
#include "nativebridge/result.h"

namespace nativebridge {

class Platform;

/// Graphics API tag used by the surface bridge. Forward declared so that this
/// header stays independent of nativebridge/surface.h.
enum class RenderApi : uint8_t;

/// Opaque subscription identifier returned by Window::addEventListener() and
/// Platform::addEventListener(). Never reused within a process.
using SubscriptionToken = uint64_t;
inline constexpr SubscriptionToken kInvalidSubscription = 0;

/// Signature of an event listener. Invoked on the polling thread (see event.h).
using EventListener = std::function<void(const Event&)>;

/// A single event subscription: an event type plus the listener itself.
/// Tokens are unique per process and never reused.
struct EventSubscription {
    SubscriptionToken token{0};
    EventType         type{EventType::WindowCreated};
    EventListener     listener{};
};

/// Thread safe listener registry shared by a Platform and its Windows.
///
/// NOT PART OF THE PUBLIC API: the definition lives in src/common/dispatch.h.
/// The public header only needs the incomplete type, which keeps STL container
/// details (and their ABI) out of the installed headers.
class DispatchRegistry;

/// Opaque native handle. Deliberately a plain value so that no platform header
/// is needed on the public side; the *meaning* of `value` per platform is
/// documented in nativebridge/README (see the "Native interop" table).
struct NativeHandle {
    enum class Kind : uint8_t {
        None,
        Win32Hwnd,        ///< value = HWND
        Win32Hinstance,   ///< value = HINSTANCE
        CocoaNSWindow,    ///< value = const void* -> NSWindow*
        CocoaNSView,      ///< value = const void* -> NSView* (content view, use for CAMetalLayer)
        X11Window,        ///< value = XID (unsigned long)
        X11Display,       ///< value = const void* -> Display*
        WaylandDisplay,   ///< value = const void* -> wl_display*
        WaylandSurface,   ///< value = const void* -> wl_surface*
        WaylandEglWindow, ///< value = const void* -> wl_egl_window*
    };

    Kind     kind{Kind::None};
    uintptr_t value{0}; ///< Pointer or integer id; 0 means "not available".

    [[nodiscard]] bool valid() const noexcept { return kind != Kind::None && value != 0; }
};

/// Native handle returned by Window::nativeHandle().
struct WindowNativeHandle {
    NativeHandle window{};     ///< Top level native window (HWND / NSWindow* / XID / wl_surface*).
    NativeHandle view{};       ///< Content view/surface target for the renderer, if any.
    NativeHandle instance{};   ///< Application/connection instance (HINSTANCE, Display*, wl_display*).
    NativeHandle apiSurface{}; ///< API specific surface (wl_egl_window) once materialized, else none.
};

/// Window chrome / behavior flags.
enum class WindowStyle : uint32_t {
    Default        = 0u,
    NoDecoration   = 1u << 0u, ///< Borderless; unsupported on some Wayland compositors.
    Resizable      = 1u << 1u,
    AlwaysOnTop    = 1u << 2u,
    Transparent    = 1u << 3u,
    HighDpiAware   = 1u << 4u, ///< Opt in to per-monitor DPI scaling (default on Windows 10+).
    ToolWindow     = 1u << 5u, ///< Dock/taskbar is not shown for this window.
};

[[nodiscard]] constexpr WindowStyle operator|(WindowStyle a, WindowStyle b) noexcept {
    return static_cast<WindowStyle>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr bool hasStyle(WindowStyle set, WindowStyle bit) noexcept {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(bit)) != 0u;
}

/// Optional capabilities. Backends report what they really implement so that
/// callers can degrade gracefully instead of guessing from the platform name.
enum class Capability : uint32_t {
    WindowPosition    = 1u << 0u, ///< Global absolute positioning (absent on Wayland).
    WindowDecoration  = 1u << 1u, ///< Client-side control over decorations.
    ClientSideResize  = 1u << 2u, ///< resize()/setMinSize() honored immediately.
    AlwaysOnTop       = 1u << 3u,
    Transparency      = 1u << 4u,
    ClipboardText     = 1u << 5u,
    MultiDisplay      = 1u << 6u,
    GlobalCursor      = 1u << 7u,
    TextInput         = 1u << 8u,
    DpiChangeEvents   = 1u << 9u,
    ScreenCaptureLock = 1u << 10u, ///< Never set: capture is out of scope.
};

/// Mouse cursor shapes understood by every backend (unsupported ones fall back
/// to Arrow).
enum class CursorShape : uint8_t {
    Arrow = 0,
    IBeam,
    Crosshair,
    Hand,
    ResizeLeftRight,
    ResizeUpDown,
    ResizeAll,
    NotAllowed,
    Hidden, ///< Cursor hidden while over the window.
};

/// Options for window creation. Defaults produce a resizable 1280x720 window.
struct WindowCreateInfo {
    std::string title{"nativeBridge window"};
    Size        clientSize{1280, 720}; ///< Logical size of the client area.
    Size        minSize{0, 0};         ///< {0,0} = no constraint.
    Size        maxSize{0, 0};         ///< {0,0} = no constraint.
    Point       position{0, 0};        ///< Ignored on Wayland.
    bool        centered{true};        ///< Ignored when `position` is explicit.
    WindowStyle style{WindowStyle::Default | WindowStyle::Resizable | WindowStyle::HighDpiAware};
    bool        visible{true};
    bool        resizable{true};
    bool        hasPosition{false};    ///< Set when the caller wants an exact position.
    CursorShape cursor{CursorShape::Arrow};
    /// Optional parent window (transient child); 0 = none.
    WindowId    parent{0};
};

// ---------------------------------------------------------------------------
// Backend interface (not part of the public API contract; implemented in
// src/<platform>/). Kept in a `detail` namespace so callers do not depend on it.
// ---------------------------------------------------------------------------
namespace detail {

class SurfaceBridgeImpl;

class WindowBackend {
public:
    virtual ~WindowBackend() = default;

    [[nodiscard]] virtual WindowId id() const noexcept = 0;
    [[nodiscard]] virtual bool supports(Capability capability) const noexcept = 0;

    // --- Snapshot getters (called from any thread) --------------------------
    [[nodiscard]] virtual std::string title() const = 0;
    [[nodiscard]] virtual Size clientSize() const = 0;
    [[nodiscard]] virtual Size windowSize() const = 0;
    [[nodiscard]] virtual Point position() const = 0;
    [[nodiscard]] virtual PixelSize framebufferSize() const = 0;
    [[nodiscard]] virtual double scaleFactor() const = 0;
    [[nodiscard]] virtual bool isVisible() const = 0;
    [[nodiscard]] virtual bool isFocused() const = 0;
    [[nodiscard]] virtual bool isMinimized() const = 0;
    [[nodiscard]] virtual bool isMaximized() const = 0;
    [[nodiscard]] virtual bool isFullscreen() const = 0;
    [[nodiscard]] virtual CursorShape cursor() const = 0;
    [[nodiscard]] virtual WindowNativeHandle nativeHandle() const = 0;
    /// Stable backend identifier ("win32" | "cocoa" | "x11" | "wayland").
    /// Returns a view of a static string: never a dangling reference.
    [[nodiscard]] virtual std::string_view backendName() const noexcept = 0;

    // --- Commands ----------------------------------------------------------
    [[nodiscard]] virtual Status setTitle(std::string_view title) = 0;
    [[nodiscard]] virtual Status resize(Size clientSize) = 0;
    [[nodiscard]] virtual Status move(Point position) = 0;
    [[nodiscard]] virtual Status setMinSize(Size size) = 0;
    [[nodiscard]] virtual Status setMaxSize(Size size) = 0;
    [[nodiscard]] virtual Status show() = 0;
    [[nodiscard]] virtual Status hide() = 0;
    [[nodiscard]] virtual Status minimize() = 0;
    [[nodiscard]] virtual Status maximize() = 0;
    [[nodiscard]] virtual Status restore() = 0;
    [[nodiscard]] virtual Status setFullscreen(bool fullscreen) = 0;
    [[nodiscard]] virtual Status setAlwaysOnTop(bool onTop) = 0;
    [[nodiscard]] virtual Status setResizable(bool resizable) = 0;
    [[nodiscard]] virtual Status setDecorations(bool decorations) = 0;
    [[nodiscard]] virtual Status setCursor(CursorShape shape) = 0;
    [[nodiscard]] virtual Status setCursorVisible(bool visible) = 0;
    [[nodiscard]] virtual Status setClipRect(const std::optional<Rect>& rect) = 0;
    [[nodiscard]] virtual Status requestAttention() = 0;
    [[nodiscard]] virtual Status requestClose() = 0;
    /// Replace the cursor bitmap with raw BGRA pixels (unsupported on Wayland
    /// and X11-without-Xcursor).
    [[nodiscard]] virtual Status setCustomCursor(std::span<const uint8_t> bgraPixels,
                                                 Size size,
                                                 Point hotspot) = 0;

    /// Materialize an API specific surface for the upper layer renderer.
    /// Backends that need to allocate something (Wayland: wl_egl_window) fill in
    /// `surface`; the default implementation leaves it invalid, which the
    /// SurfaceBridge reports as "use the window handle directly".
    /// Runs on the UI thread.
    [[nodiscard]] virtual Status materializeSurface(RenderApi api,
                                                    std::string_view backendName,
                                                    NativeHandle& surface) {
        (void)api;
        (void)backendName;
        (void)surface;
        return okStatus();
    }

    /// Called by the surface bridge after a resize/scale change so backends can
    /// update derived resources.
    virtual void onSurfaceResized() {}

    /// Called by the platform when the monitor configuration changed (hotplug,
    /// resolution, DPI). Backends emit ScaleFactorChanged before Resized for
    /// windows that moved to a display with a different scale factor.
    virtual void onDisplayConfigurationChanged() {}

    /// Invoked by the platform when the window's native lifetime ends.
    virtual void onNativeDestroyed() = 0;
};

} // namespace detail

// ---------------------------------------------------------------------------

/// A top level, native window.
///
/// Obtain instances from Platform::createWindow(). Window is move-only and RAII:
/// destroying it destroys the native window (on the owning thread).
class Window {
public:
    Window() noexcept;
    ~Window();

    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// True when this object owns a live native window.
    [[nodiscard]] bool isValid() const noexcept;
    explicit operator bool() const noexcept { return isValid(); }

    [[nodiscard]] WindowId id() const noexcept;
    [[nodiscard]] std::string_view backendName() const noexcept;

    /// Query optional capability support (never throws, never fails).
    [[nodiscard]] bool supports(Capability capability) const noexcept;

    // --- State -------------------------------------------------------------
    [[nodiscard]] std::string title() const;
    [[nodiscard]] Size clientSize() const;      ///< Logical, excludes decorations.
    [[nodiscard]] Size windowSize() const;      ///< Logical, includes decorations.
    [[nodiscard]] Point position() const;       ///< Logical desktop coordinates.
    [[nodiscard]] PixelSize framebufferSize() const; ///< clientSize * scaleFactor.
    [[nodiscard]] double scaleFactor() const;   ///< 1.0 = 96 DPI / 1x.
    [[nodiscard]] bool isVisible() const;
    [[nodiscard]] bool isFocused() const;
    [[nodiscard]] bool isMinimized() const;
    [[nodiscard]] bool isMaximized() const;
    [[nodiscard]] bool isFullscreen() const;
    [[nodiscard]] CursorShape cursor() const;

    /// Native handles for interop with renderers. Never valid after destroy.
    [[nodiscard]] WindowNativeHandle nativeHandle() const;

    // --- Commands ----------------------------------------------------------
    [[nodiscard]] Status setTitle(std::string_view title);
    /// Resize the client area (logical units).
    [[nodiscard]] Status resize(Size clientSize);
    /// Move in logical desktop coordinates. Errc::Unsupported on Wayland.
    [[nodiscard]] Status move(Point position);
    [[nodiscard]] Status setMinSize(Size size);
    [[nodiscard]] Status setMaxSize(Size size);
    /// Center on the display the window currently occupies.
    [[nodiscard]] Status center();
    [[nodiscard]] Status show();
    [[nodiscard]] Status hide();
    [[nodiscard]] Status minimize();
    [[nodiscard]] Status maximize();
    [[nodiscard]] Status restore();
    [[nodiscard]] Status setFullscreen(bool fullscreen);
    [[nodiscard]] Status setAlwaysOnTop(bool onTop);
    [[nodiscard]] Status setResizable(bool resizable);
    [[nodiscard]] Status setDecorations(bool decorations);
    [[nodiscard]] Status setCursor(CursorShape shape);
    [[nodiscard]] Status setCursorVisible(bool visible);
    /// Restrict drawing to a logical client-area rect; std::nullopt clears it.
    [[nodiscard]] Status setClipRect(const std::optional<Rect>& rect);
    /// Flash the taskbar entry / bounce the dock icon.
    [[nodiscard]] Status requestAttention();
    /// Ask the window to close (as if the user pressed the close button).
    [[nodiscard]] Status requestClose();
    [[nodiscard]] Status setCustomCursor(std::span<const uint8_t> bgraPixels, Size size, Point hotspot);

    // --- Events ------------------------------------------------------------

    /// Subscribe to all events of this window.
    /// The listener is invoked on the thread that calls Platform::pollEvents()/
    /// waitEvents() (see event.h). The returned token can be passed to
    /// removeEventListener(); tokens are never reused. Returns
    /// kInvalidSubscription when the window is not valid.
    [[nodiscard]] SubscriptionToken addEventListener(EventListener listener);

    /// Subscribe to a single event type; cheaper dispatch for hot paths.
    [[nodiscard]] SubscriptionToken addEventListener(EventType type, EventListener listener);

    /// Remove a subscription; safe to call with kInvalidSubscription or a token
    /// that was already removed. Returns false when the token is unknown.
    bool removeEventListener(SubscriptionToken token);

    /// Transfer ownership of the native window to the caller: the Window stops
    /// managing the native resource and the returned object can be moved to
    /// another thread. Used by frameworks that adopt the window into their own
    /// run loop. Unsupported on Wayland (returns Unsupported).
    [[nodiscard]] Result<Window> release();

private:
    friend class Platform;
    friend class detail::WindowBackend;
    friend class detail::SurfaceBridgeImpl;

    explicit Window(std::unique_ptr<detail::WindowBackend> backend) noexcept;

    /// Internal wiring used by Platform (not part of the public contract).
    void attachDispatch(std::shared_ptr<DispatchRegistry> dispatch) noexcept;
    void attachDestructionSink(std::function<void(WindowId)> sink) noexcept;
    [[nodiscard]] Status materializeSurface(RenderApi api, std::string_view backendName);
    void refreshSurfaceSize();

    /// Backend owned by this Window; null after a successful release().
    std::unique_ptr<detail::WindowBackend> m_backend;
    /// Listener registry; shared with Platform for event routing.
    std::shared_ptr<DispatchRegistry> m_dispatch;
    /// Notifies Platform when the native window disappears. A Window is always
    /// shorter lived than its Platform (Platform owns the display connection),
    /// so invoking the sink is safe from ~Window.
    std::function<void(WindowId)> m_destructionSink;
    /// API specific surface materialized by the surface bridge (wl_egl_window).
    NativeHandle m_apiSurface{};
};

} // namespace nativebridge

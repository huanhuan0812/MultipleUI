// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// event.h — unified input / window / lifecycle events.
//
// THREADING MODEL
// ---------------
// * Events are *produced* by the platform backend. Depending on the platform
//   this may happen on a platform-owned thread (Win32 message thread, Cocoa
//   main thread, Wayland/X11 connection thread). Events are appended to an
//   internal per-Platform queue protected by a mutex.
// * Events are *consumed* on the thread that called Platform::pollEvents() or
//   Platform::waitEvents(). Subscriber callbacks registered through
//   Window::addEventListener() are invoked synchronously from that consuming
//   thread and never from anywhere else. Consequence: callbacks may touch
//   application state without extra locking, but must not block for long.
// * A callback may capture a `Window&`, but must not destroy it from inside the
//   callback (the backend is still dispatching). Use the returned
//   SubscriptionToken and unsubscribe outside the callback instead.
// * Dispatch order is guaranteed: for each poll pass, subscriptions are invoked
//   in registration order, then the poll buffer returned by pollEvents() is
//   handed to the caller.
//
// LISTENER ROUTING
// ----------------
// Events carrying a non-zero WindowId are delivered to that Window's listeners
// only. Events with WindowId == 0 (e.g. DisplayChanged) are delivered to the
// Platform listeners.
//
// COORDINATE / DPI POLICY
// -----------------------
// All coordinates delivered in events are *logical* coordinates relative to the
// client area of the window (top-left origin, +Y down), except MoveEvent, whose
// position is in logical desktop coordinates. The backend is responsible for
// converting physical device coordinates to logical ones using the scale factor
// of the output the window currently occupies; therefore
//
//     physical_pixel = logical * window.scaleFactor()
//
// always holds. On a DPI change (window dragged between displays) exactly one
// ScaleFactorEvent is emitted *before* the following ResizeEvent, so listeners
// can invalidate DPI dependent resources first.

#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "nativebridge/geometry.h"
#include "nativebridge/keycode.h"
#include "nativebridge/result.h"

namespace nativebridge {

/// Opaque, process-local window identity (never a raw platform handle).
using WindowId = uint64_t;

/// Category of an event; usable for cheap filtering before inspecting payload.
enum class EventType : uint8_t {
    // Lifecycle
    WindowCreated,   ///< Window exists and its native handle is valid.
    WindowDestroying,///< Emitted from the destructor path, before the native window dies.
    // Window state
    Resized,
    Moved,
    CloseRequested,  ///< User/system asked to close (titlebar button, WM_DELETE_WINDOW).
    FocusGained,
    FocusLost,
    Shown,
    Hidden,
    Minimized,
    Restored,
    Maximized,
    ScaleFactorChanged,
    // Input
    KeyDown,
    KeyUp,
    TextInput,
    MouseDown,
    MouseUp,
    MouseMoved,
    MouseEntered,
    MouseLeft,
    MouseWheel,
    // System
    DisplayChanged,  ///< Monitor set / geometry / DPI changed.

    /// Wildcard, usable only as a *subscription filter*: it matches every event
    /// and is never delivered as an event's own type.
    All,
};

[[nodiscard]] std::string_view toString(EventType type) noexcept;

// ---------------------------------------------------------------------------
// Event payloads. All positions/sizes are logical units (see header comment).
// ---------------------------------------------------------------------------

struct CreatedEvent {
    Size clientSize{};   ///< Logical size of the client area.
    Size windowSize{};   ///< Logical size of the whole window incl. decorations (== clientSize on Wayland).
    double scaleFactor{1.0};
};

struct DestroyingEvent {
    bool destroyedByUser{false}; ///< true when the user's close request caused it.
};

struct ResizeEvent {
    Size clientSize{};
    Size windowSize{};
    PixelSize framebufferSize{}; ///< clientSize * scaleFactor, rounded, for renderer use.
    bool inLiveResize{false};    ///< true while the user is still dragging the border.
};

struct MoveEvent {
    Point position{}; ///< Logical position of the window's top-left in desktop space.
    /// Wayland: the compositor never reports a global position, so `position`
    /// stays {0,0} and this flag is false. Treat the window as unpositioned.
    bool globalPositionKnown{true};
};

struct CloseRequestedEvent {
    bool cancellable{true}; ///< Backends that cannot veto (Wayland xdg_toplevel close) report false.
};

struct FocusEvent {
    bool focused{true};
};

struct VisibilityEvent {
    bool visible{true};
};

struct StateEvent {
    bool active{true}; ///< Maximized/Minimized/Restored: true = entered, false = left.
};

struct ScaleFactorEvent {
    double scaleFactor{1.0};
    Rect displayBounds{}; ///< Logical bounds of the display the window moved to.
};

struct KeyEvent {
    KeyCode key{KeyCode::Unknown};
    KeyAction action{KeyAction::Press};
    Modifier modifiers{Modifier::None};
    uint32_t nativeScanCode{0}; ///< Raw platform scancode (HID usage when available).
    uint32_t nativeKeyCode{0};  ///< Raw platform key (VK_* / keysym / evdev code).
    uint32_t keysym{0};         ///< X11/Wayland keysym when the backend has one, else 0.
    bool isRepeat{false};
    /// Device independent character, 0 when the key produces no text.
    /// Not affected by modifier state the backend cannot observe.
    uint32_t character{0};
};

struct TextInputEvent {
    /// UTF-8 encoded committed text (may be a multi-byte grapheme cluster).
    std::string utf8;
};

struct MouseButtonEvent {
    MouseButton button{MouseButton::Unknown};
    Point position{};        ///< Client-area logical position.
    Modifier modifiers{Modifier::None};
    uint8_t clickCount{1};   ///< 2 for double click (1 on backends without click counting).
};

struct MouseMoveEvent {
    Point position{};        ///< Client-area logical position.
    Point delta{};           ///< Movement since the previous move event, logical units.
    Modifier modifiers{Modifier::None};
    bool isDragging{false};  ///< True when at least one button is held.
};

struct MouseEnterEvent {
    Point position{};
    bool entered{true};      ///< true = entered, false = left.
};

struct MouseWheelEvent {
    Point position{};
    /// Scroll amount in *lines*; positive = content moves up/right.
    /// Trackpads report fractional values; the backend must not round.
    double deltaX{0.0};
    double deltaY{0.0};
    /// Precise scrolling (trackpad / high-resolution wheel).
    bool precise{false};
    Modifier modifiers{Modifier::None};
};

struct DisplayChangedEvent {
    bool displaysChanged{true};
};

// ---------------------------------------------------------------------------

/// Variant payload; `Event::type` names the active alternative.
using EventPayload = std::variant<std::monostate,
                                  CreatedEvent,
                                  DestroyingEvent,
                                  ResizeEvent,
                                  MoveEvent,
                                  CloseRequestedEvent,
                                  FocusEvent,
                                  VisibilityEvent,
                                  StateEvent,
                                  ScaleFactorEvent,
                                  KeyEvent,
                                  TextInputEvent,
                                  MouseButtonEvent,
                                  MouseMoveEvent,
                                  MouseEnterEvent,
                                  MouseWheelEvent,
                                  DisplayChangedEvent>;

/// One unified event.
struct Event {
    EventType    type{EventType::WindowCreated};
    WindowId     window{0};        ///< 0 when the event is not bound to a window.
    uint64_t     timestampNs{0};   ///< Monotonic nanoseconds since an unspecified epoch.
    EventPayload payload{};

    // --- Convenience accessors (never throw; null when the type does not match)

    [[nodiscard]] const CreatedEvent* asCreated() const { return std::get_if<CreatedEvent>(&payload); }
    [[nodiscard]] const DestroyingEvent* asDestroying() const { return std::get_if<DestroyingEvent>(&payload); }
    [[nodiscard]] const ResizeEvent* asResize() const { return std::get_if<ResizeEvent>(&payload); }
    [[nodiscard]] const MoveEvent* asMove() const { return std::get_if<MoveEvent>(&payload); }
    [[nodiscard]] const CloseRequestedEvent* asCloseRequested() const { return std::get_if<CloseRequestedEvent>(&payload); }
    [[nodiscard]] const FocusEvent* asFocus() const { return std::get_if<FocusEvent>(&payload); }
    [[nodiscard]] const VisibilityEvent* asVisibility() const { return std::get_if<VisibilityEvent>(&payload); }
    [[nodiscard]] const StateEvent* asState() const { return std::get_if<StateEvent>(&payload); }
    [[nodiscard]] const ScaleFactorEvent* asScaleFactor() const { return std::get_if<ScaleFactorEvent>(&payload); }
    [[nodiscard]] const KeyEvent* asKey() const { return std::get_if<KeyEvent>(&payload); }
    [[nodiscard]] const TextInputEvent* asTextInput() const { return std::get_if<TextInputEvent>(&payload); }
    [[nodiscard]] const MouseButtonEvent* asMouseButton() const { return std::get_if<MouseButtonEvent>(&payload); }
    [[nodiscard]] const MouseMoveEvent* asMouseMove() const { return std::get_if<MouseMoveEvent>(&payload); }
    [[nodiscard]] const MouseEnterEvent* asMouseEnter() const { return std::get_if<MouseEnterEvent>(&payload); }
    [[nodiscard]] const MouseWheelEvent* asMouseWheel() const { return std::get_if<MouseWheelEvent>(&payload); }
};

/// Human readable one-line rendering, e.g. "KeyDown key=A mods=Shift".
/// Intended for examples, logs and tests.
[[nodiscard]] std::string describe(const Event& event);

/// Monotonic clock helper used to stamp events (nanoseconds, not wall clock).
[[nodiscard]] uint64_t monotonicNowNs() noexcept;

} // namespace nativebridge

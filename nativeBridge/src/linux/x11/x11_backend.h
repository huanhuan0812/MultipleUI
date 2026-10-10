// nativeBridge — Linux/X11 backend internals.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API. This header is the only place where the X11
// translation units agree on an interface; everything Xlib related stays inside
// src/linux/x11/.
//
// THREADING MODEL (X11)
// ---------------------
// Xlib connections are not thread safe in practice (XInitThreads only protects
// the library's own bookkeeping, not the request stream ordering an application
// needs). nativeBridge therefore requires the whole X11 backend to be driven
// from the thread that created the Platform: window creation, every window
// command, clipboard transfer and event pumping. Commands from another thread
// fail with Err::PlatformFailure instead of interleaving requests on the wire.
//
// CLIPBOARD MODEL (X11)
// ---------------------
// X11 selections are peer-to-peer: the owner serves data on demand. Putting text
// on the clipboard therefore makes this process the CLIPBOARD selection owner
// and keeps the text in the backend, answering SelectionRequest during
// pollEvents(). Reading text means requesting UTF8_STRING from the current owner
// and waiting for SelectionNotify on the same connection, which is why
// Clipboard::getText() must also run on the polling thread.

#pragma once

// --- Xlib first ------------------------------------------------------------
// Xlib is included here (and only here, plus in X11/Xatom.h below) so that every
// X11 translation unit sees the same macro environment.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

// Xlib defines these as object-like macros, which would otherwise rewrite the
// public API (nativebridge::None -> 0L, nativebridge::Status -> int, ...).
// They are removed after the X11 headers and before the nativeBridge headers.
//
// Consequences inside this backend:
//   None    -> written as the 0 literal (null resource/atom),
//   Success -> written as the 0 literal (Xlib success code),
//   True / False / Bool -> replaced by 1 / 0 / bool.
//
// The X11 headers that need Bool (Xresource.h) are pulled in below, *before* the
// undef, so they still see the real typedef.
#include <X11/Xatom.h>
#include <X11/Xresource.h>

#undef None
#undef Success
#undef True
#undef False
#undef Bool
#undef Status
#undef StatusMask
#undef NoneMask
// X11/X.h defines CursorShape as the largest cursor size, which collides with
// nativebridge::CursorShape. The backend never needs the X11 constant.
#undef CursorShape

// --- standard library and nativeBridge -------------------------------------
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/backend.h"
#include "common/event_queue.h"
#include "common/lifetime.h"
#include "common/snapshot.h"
#include "nativebridge/display.h"
#include "nativebridge/window.h"

namespace nativebridge::detail {

class X11Window;

/// The X11 PlatformBackend implementation.
class X11Platform final : public PlatformBackend {
public:
    X11Platform();
    ~X11Platform() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "x11"; }
    [[nodiscard]] bool supports(Capability capability) const noexcept override;
    [[nodiscard]] std::string describe() const override;

    void pump() override;
    std::size_t wait(std::chrono::milliseconds timeout) override;
    [[nodiscard]] std::vector<Event> takeEvents() override { return m_queue.drain(); }

    void refreshDisplays() override;
    [[nodiscard]] const DisplayList& displays() const noexcept override { return m_displays; }

    [[nodiscard]] Result<std::unique_ptr<WindowBackend>> createWindow(const WindowCreateInfo& info) override;
    [[nodiscard]] Clipboard& clipboard() override;
    void destroyWindow(WindowBackend* window) override;
    [[nodiscard]] std::optional<Error> fatalError() const override { return m_fatal; }

    // --- Accessors used by X11Window ---------------------------------------
    [[nodiscard]] Display* display() const noexcept { return m_display; }
    [[nodiscard]] ::Window root() const noexcept { return m_root; }
    [[nodiscard]] int screen() const noexcept { return m_screen; }
    [[nodiscard]] ::Atom wmProtocols() const noexcept { return m_wmProtocols; }
    [[nodiscard]] ::Atom wmDeleteWindow() const noexcept { return m_wmDeleteWindow; }
    [[nodiscard]] ::Atom utf8String() const noexcept { return m_utf8String; }
    [[nodiscard]] ::Atom netWmName() const noexcept { return m_netWmName; }
    [[nodiscard]] ::Atom netWmState() const noexcept { return m_netWmState; }
    [[nodiscard]] ::Atom netWmStateFullscreen() const noexcept { return m_netWmStateFullscreen; }
    [[nodiscard]] ::Atom netWmStateAbove() const noexcept { return m_netWmStateAbove; }
    [[nodiscard]] ::Atom netWmStateMaximizedHorz() const noexcept { return m_netWmStateMaximizedHorz; }
    [[nodiscard]] ::Atom netWmStateMaximizedVert() const noexcept { return m_netWmStateMaximizedVert; }
    [[nodiscard]] ::Atom netWmMoveresize() const noexcept { return m_netWmMoveresize; }
    [[nodiscard]] double scaleFactor() const noexcept { return m_scale; }
    [[nodiscard]] int scrollLinesPerNotch() const noexcept { return m_scrollLinesPerNotch; }
    [[nodiscard]] EventQueue& queue() noexcept { return m_queue; }
    [[nodiscard]] WindowId nextWindowId() noexcept { return m_nextWindowId++; }
    /// Weak token for the platform's lifetime; the window backend uses it to
    /// deregister itself without keeping the platform alive.
    [[nodiscard]] WeakLifetimeToken lifetime() const noexcept { return m_lifetime; }
    [[nodiscard]] bool isUiThread() const noexcept { return std::this_thread::get_id() == m_uiThread; }
    [[nodiscard]] bool xtestAvailable() const noexcept { return m_xtestAvailable; }

    void windowDestroyed(WindowId id);
    [[nodiscard]] std::size_t processPending(); ///< Read everything already queued.

    /// Clipboard plumbing (implemented in x11_clipboard.cpp).
    [[nodiscard]] Result<std::string> clipboardGetText();
    [[nodiscard]] Status clipboardSetText(std::string_view utf8Text);
    [[nodiscard]] Status clipboardClear();
    [[nodiscard]] bool clipboardHasText();
    void handleSelectionRequest(const XSelectionRequestEvent& request);
    void handleSelectionNotify(const XSelectionEvent& notify);
    void handleSelectionClear(const XSelectionClearEvent& clear);

private:
    void rebuildDisplays();
    void applyDpiFromResources();
    void internAtoms();

    Display* m_display{nullptr};
    ::Window m_root{0};
    int      m_screen{0};
    double   m_scale{1.0};
    int      m_scrollLinesPerNotch{3};
    bool     m_xtestAvailable{false};

    ::Atom m_wmProtocols{0};
    ::Atom m_wmDeleteWindow{0};
    ::Atom m_utf8String{0};
    ::Atom m_netWmName{0};
    ::Atom m_netWmState{0};
    ::Atom m_netWmStateFullscreen{0};
    ::Atom m_netWmStateAbove{0};
    ::Atom m_netWmStateMaximizedHorz{0};
    ::Atom m_netWmStateMaximizedVert{0};
    ::Atom m_netWmMoveresize{0};
    ::Atom m_clipboard{0};
    ::Atom m_targets{0};
    ::Atom m_incr{0};

    EventQueue                 m_queue;
    DisplayList                m_displays;
    std::unique_ptr<Clipboard> m_clipboardFacade;
    LifetimeTokenPtr           m_lifetime{std::make_shared<LifetimeToken>()};
    std::optional<Error>       m_fatal;
    std::thread::id            m_uiThread{std::this_thread::get_id()};
    WindowId                   m_nextWindowId{1};
    std::mutex                 m_windowsMutex;
    std::vector<X11Window*>    m_windows;

    // Clipboard state served by this process.
    std::string m_clipboardText;
    bool        m_clipboardOwned{false};
    /// Outstanding paste request (chunked INCR transfers use the same slot).
    bool        m_pendingPaste{false};
    std::string m_pendingPasteData;
    ::Window    m_pendingPasteRequestor{0};
    ::Atom      m_pendingPasteProperty{0};
    ::Atom      m_pendingPasteTarget{0};
    ::Time      m_pendingPasteTime{0};
};

/// The X11 window backend: one X window (plus its hidden wrapper).
class X11Window final : public WindowBackend {
public:
    X11Window(X11Platform& platform, WindowId id, const WindowCreateInfo& info);
    ~X11Window() override;

    X11Window(const X11Window&) = delete;
    X11Window& operator=(const X11Window&) = delete;

    [[nodiscard]] WindowId id() const noexcept override { return m_id; }
    [[nodiscard]] bool supports(Capability capability) const noexcept override;
    [[nodiscard]] std::string_view backendName() const noexcept override { return "x11"; }

    [[nodiscard]] std::string title() const override;
    [[nodiscard]] Size clientSize() const override { return m_clientSize.get(); }
    [[nodiscard]] Size windowSize() const override { return m_windowSize.get(); }
    [[nodiscard]] Point position() const override { return m_position.get(); }
    [[nodiscard]] PixelSize framebufferSize() const override {
        return logicalToPhysical(m_clientSize.get(), m_scaleFactor.get());
    }
    [[nodiscard]] double scaleFactor() const override { return m_scaleFactor.get(); }
    [[nodiscard]] bool isVisible() const override { return m_visible.get(); }
    [[nodiscard]] bool isFocused() const override { return m_focused.get(); }
    [[nodiscard]] bool isMinimized() const override { return m_minimized.get(); }
    [[nodiscard]] bool isMaximized() const override { return m_maximized.get(); }
    [[nodiscard]] bool isFullscreen() const override { return m_fullscreen.get(); }
    [[nodiscard]] CursorShape cursor() const override { return m_cursor.get(); }
    [[nodiscard]] WindowNativeHandle nativeHandle() const override;

    [[nodiscard]] Status setTitle(std::string_view title) override;
    [[nodiscard]] Status resize(Size clientSize) override;
    [[nodiscard]] Status move(Point position) override;
    [[nodiscard]] Status setMinSize(Size size) override;
    [[nodiscard]] Status setMaxSize(Size size) override;
    [[nodiscard]] Status show() override;
    [[nodiscard]] Status hide() override;
    [[nodiscard]] Status minimize() override;
    [[nodiscard]] Status maximize() override;
    [[nodiscard]] Status restore() override;
    [[nodiscard]] Status setFullscreen(bool fullscreen) override;
    [[nodiscard]] Status setAlwaysOnTop(bool onTop) override;
    [[nodiscard]] Status setResizable(bool resizable) override;
    [[nodiscard]] Status setDecorations(bool decorations) override;
    [[nodiscard]] Status setCursor(CursorShape shape) override;
    [[nodiscard]] Status setCursorVisible(bool visible) override;
    [[nodiscard]] Status setClipRect(const std::optional<Rect>& rect) override;
    [[nodiscard]] Status requestAttention() override;
    [[nodiscard]] Status requestClose() override;
    [[nodiscard]] Status setCustomCursor(std::span<const uint8_t> bgraPixels, Size size, Point hotspot) override;
    [[nodiscard]] Status materializeSurface(RenderApi api, std::string_view backendName,
                                            NativeHandle& surface) override;
    void onSurfaceResized() override;
    void onDisplayConfigurationChanged() override;
    void onNativeDestroyed() override;

    /// Handle one XEvent addressed to this window (or to the wrapper).
    void handleEvent(XEvent& event);
    /// Refresh the cached geometry from the server.
    void syncGeometry();

    [[nodiscard]] ::Window xwindow() const noexcept { return m_window; }
    [[nodiscard]] ::Window wrapper() const noexcept { return m_wrapper; }

private:
    void selectInput();
    void createWrapper();
    void updateSizeHints();
    void syncScaleFactor(bool emitEvent);
    void applyCursor();
    void sendNetWmState(::Atom first, ::Atom second, bool enable);

    X11Platform&  m_platform;
    WindowId      m_id{0};
    ::Window      m_window{0};   ///< The window the client draws into.
    ::Window      m_wrapper{0};  ///< Decoration/positioning wrapper (managed by the WM).
    XIC           m_ic{nullptr}; ///< Input context for committed text.
    bool          m_ownWindow{false};
    double        m_scale{1.0};
    Size          m_minSize{0, 0};
    Size          m_maxSize{0, 0};
    CursorShape   m_cursorShape{CursorShape::Arrow};
    bool          m_cursorVisible{true};
    bool          m_decorations{true};
    bool          m_resizable{true};
    bool          m_fullscreenActive{false};
    bool          m_maximizedActive{false};
    Point         m_lastMouse{};
    bool          m_hasLastMouse{false};
    bool          m_trackingMouse{false};
    std::optional<Rect> m_clipRect{};
    ::Cursor      m_customCursor{0};
    ::Cursor      m_currentCursor{0};

    Snapshot<Size>        m_clientSize{Size{0, 0}};
    Snapshot<Size>        m_windowSize{Size{0, 0}};
    Snapshot<Point>       m_position{Point{0, 0}};
    Snapshot<double>      m_scaleFactor{1.0};
    Snapshot<bool>        m_visible{false};
    Snapshot<bool>        m_focused{false};
    Snapshot<bool>        m_minimized{false};
    Snapshot<bool>        m_maximized{false};
    Snapshot<bool>        m_fullscreen{false};
    Snapshot<CursorShape> m_cursor{CursorShape::Arrow};
};

} // namespace nativebridge::detail

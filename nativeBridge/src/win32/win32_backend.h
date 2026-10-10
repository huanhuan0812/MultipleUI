// nativeBridge — Windows (Win32) backend internals.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API.
//
// Win32Platform and Win32Window are declared here so that the window
// implementation can reach the platform's event queue and display snapshot
// without either translation unit reaching into the other's internals.

#pragma once

#include "win32/win32_common.h"

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

class Win32Window;

/// The Win32 PlatformBackend implementation.
///
/// THREADING: created on the UI thread; every method except the any-thread
/// snapshot getters must be called from that thread. The window registry is
/// guarded because ~Win32Window may run on a foreign thread.
class Win32Platform final : public PlatformBackend {
public:
    Win32Platform();
    ~Win32Platform() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "win32"; }
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

    /// Live windows, for broadcast messages (WM_SETTINGCHANGE, WM_DISPLAYCHANGE).
    void broadcastDisplayChange();
    void refreshScaleFactors();

    [[nodiscard]] EventQueue& queue() noexcept { return m_queue; }
    [[nodiscard]] WindowId nextWindowId() noexcept { return m_nextWindowId++; }
    [[nodiscard]] WeakLifetimeToken lifetime() const noexcept { return m_lifetime; }
    [[nodiscard]] bool isUiThread() const noexcept { return std::this_thread::get_id() == m_uiThread; }

    void windowDestroyed(WindowId id);

    /// Native handle of a live window, or nullptr (used for transient parents).
    [[nodiscard]] HWND hwndForWindowId(WindowId id) const;

private:
    void registerWindowClass();
    void rebuildDisplays();

    EventQueue                    m_queue;
    DisplayList                   m_displays;
    std::unique_ptr<Clipboard>    m_clipboard;
    LifetimeTokenPtr              m_lifetime{std::make_shared<LifetimeToken>()};
    std::optional<Error>          m_fatal;
    std::thread::id               m_uiThread{std::this_thread::get_id()};
    WindowId                      m_nextWindowId{1};
    ATOM                          m_windowClass{0};
    std::wstring                  m_className;
    std::mutex                    m_windowsMutex;
    std::vector<Win32Window*>     m_windows;
    std::vector<Win32Window*>     m_pendingWindows; ///< Created but not yet registered.
    bool                          m_classRegistered{false};
};

/// The Win32 window backend. One HWND per instance.
class Win32Window final : public WindowBackend {
public:
    Win32Window(Win32Platform& platform, WindowId id, const WindowCreateInfo& info);
    ~Win32Window() override;

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    // --- WindowBackend: identity -------------------------------------------
    [[nodiscard]] WindowId id() const noexcept override { return m_id; }
    [[nodiscard]] bool supports(Capability capability) const noexcept override;
    [[nodiscard]] std::string_view backendName() const noexcept override { return "win32"; }

    // --- Snapshot getters (any thread) --------------------------------------
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

    // --- Commands -----------------------------------------------------------
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

    /// WndProc trampoline target; returns the value for DefWindowProc fallback.
    std::optional<LRESULT> handleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    // --- Platform callbacks -------------------------------------------------
    void syncGeometry();
    void syncScaleFactor(bool emitEvent);
    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }

private:
    void pushKeyEvent(UINT message, WPARAM wparam, LPARAM lparam);
    void pushMouseButton(UINT message, WPARAM wparam, LPARAM lparam);
    void pushMouseMove(LPARAM lparam);
    void pushMouseWheel(WPARAM wparam, LPARAM lparam, bool horizontal);
    void pushMouseEnter(bool entered);
    void pushFocus(bool focused);
    void pushState(EventType type, bool active);
    void applyMinMaxInfo(MINMAXINFO& info) const;
    [[nodiscard]] Point clientPointFromLParam(LPARAM lparam) const;
    [[nodiscard]] RECT desiredWindowRect(Size clientSize, DWORD style, DWORD exStyle) const;
    void applyFullscreen(bool fullscreen);
    void updateCursor();
    void centerOnMonitor();
    void centerOnMonitor();

    Win32Platform&  m_platform;
    WindowId        m_id{0};
    HWND            m_hwnd{nullptr};
    DWORD           m_style{0};
    DWORD           m_exStyle{0};
    Size            m_minSize{0, 0};
    Size            m_maxSize{0, 0};
    CursorShape     m_cursorShape{CursorShape::Arrow};
    bool            m_cursorVisible{true};
    bool            m_decorations{true};
    bool            m_resizable{true};
    bool            m_destroyed{false};
    bool            m_trackingMouse{false};
    Point           m_lastMouse{};
    bool            m_hasLastMouse{false};
    WPARAM          m_buttonState{0}; ///< MK_* flags of the last WM_MOUSEMOVE
    bool            m_fullscreenActive{false};
    WINDOWPLACEMENT m_savedPlacement{};
    LONG_PTR        m_savedStyle{0};
    LONG_PTR        m_savedExStyle{0};
    HCURSOR         m_customCursor{nullptr};
    std::optional<Rect> m_clipRect{};

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

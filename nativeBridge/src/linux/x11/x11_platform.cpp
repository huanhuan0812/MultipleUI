// nativeBridge — Linux/X11 backend: platform, event pump, displays, clipboard.
// SPDX-License-Identifier: MIT
//
// SCALE / DPI STRATEGY (X11 has no per-monitor DPI concept)
// ---------------------------------------------------------
// X11 exposes no truthful per-monitor scale factor, and the server works in
// physical pixels. nativeBridge therefore:
//   1. reads the X resource Xft.dpi (falling back to screen DPI, then 96) and
//      derives a single scale factor for the whole session,
//   2. keeps all *public* geometry in logical units and converts at the Xlib
//      boundary: xlib_pixels = logical * scale,
//   3. reports that same scale factor for every display, so a window crossing
//      monitors never produces a ScaleFactorChanged event (Capability::
//      DpiChangeEvents is still advertised because the property can change at
//      run time, e.g. the user switches from 96 to 192 dpi).
//
// EVENT LOOP
// ----------
// Non-blocking: XPending/QLength are drained first, then XNextEvent on what is
// known to be queued. wait() uses select() on the connection file descriptor,
// which is the only blocking primitive that also lets a timeout be honoured.

#include "linux/x11/x11_backend.h"

#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/select.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/backends.h"
#include "nativebridge/system.h"

using namespace nativebridge;

namespace nativebridge::detail {
namespace {

/// X server screens -> DisplayInfo. X11 shares one scale factor for all of them.
DisplayList queryDisplays(Display* display, int defaultScreen, double scale) {
    DisplayList displays;
    const int screenCount = ScreenCount(display);
    for (int index = 0; index < screenCount; ++index) {
        DisplayInfo info{};
        info.id = static_cast<DisplayId>(index + 1);
        char* name = DisplayString(display);
        info.name = (name != nullptr) ? std::string(name) : std::string(":0");
        if (screenCount > 1) {
            info.name += "." + std::to_string(index);
        }
        info.isPrimary = index == defaultScreen;
        const int width = DisplayWidth(display, index);
        const int height = DisplayHeight(display, index);
        // X11 has no work-area protocol without a WM; the full screen is reported
        // and _NET_WORKAREA (when a compliant WM is running) overrides it below.
        info.bounds = Rect{0, 0, static_cast<int32_t>(std::lround(width / scale)),
                          static_cast<int32_t>(std::lround(height / scale))};
        info.workArea = info.bounds;
        info.scaleFactor = scale;
        info.pixelSize = PixelSize{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        info.bitDepth = static_cast<uint32_t>(DisplayPlanes(display, index));
        info.refreshRateHz = 0; // XRandR would be required; out of scope by design.
        displays.push_back(std::move(info));
    }
    return displays;
}

/// Read _NET_WORKAREA from the root window when a WM provides it.
void applyWorkArea(Display* display, ::Window root, Atom netWorkarea, DisplayList& displays) {
    if (netWorkarea == 0 || displays.empty()) {
        return;
    }
    Atom actualType = 0;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    if (::XGetWindowProperty(display, root, netWorkarea, 0, 4, 0, XA_CARDINAL, &actualType, &actualFormat,
                             &itemCount, &bytesAfter, &data) == 0 &&
        data != nullptr && itemCount >= 4 && actualFormat == 32) {
        const auto* values = reinterpret_cast<const unsigned long*>(data);
        const double scale = displays.front().scaleFactor;
        Rect work{static_cast<int32_t>(std::lround(values[0] / scale)),
                  static_cast<int32_t>(std::lround(values[1] / scale)),
                  static_cast<int32_t>(std::lround(values[2] / scale)),
                  static_cast<int32_t>(std::lround(values[3] / scale))};
        if (work.width > 0 && work.height > 0) {
            displays.front().workArea = work;
        }
    }
    if (data != nullptr) {
        ::XFree(data);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Platform lifecycle
// ---------------------------------------------------------------------------

X11Platform::X11Platform() {
    if (!isUiThread()) {
        m_fatal = Error{Errc::PlatformFailure, "x11 backend: the Platform must be created on its UI thread"};
        return;
    }
    m_lifetime->setDestroyCallback([this](WindowId id) { windowDestroyed(id); });

    // XInitThreads makes Xlib's internal bookkeeping safe; nativeBridge still
    // serialises every request on the UI thread (see x11_backend.h).
    (void)::XInitThreads();

    m_display = ::XOpenDisplay(nullptr);
    if (m_display == nullptr) {
        const char* name = std::getenv("DISPLAY");
        m_fatal = Error{Errc::ConnectionFailed,
                        std::string("XOpenDisplay failed for DISPLAY=") + (name != nullptr ? name : "(unset)")};
        return;
    }
    m_screen = DefaultScreen(m_display);
    m_root = RootWindow(m_display, m_screen);
    applyDpiFromResources();
    internAtoms();
    rebuildDisplays();

    // XTEST is only needed for WarpPointer style features and is optional.
    m_xtestAvailable = false;
}

X11Platform::~X11Platform() {
    m_lifetime->invalidate();
    std::vector<X11Window*> windows;
    {
        std::lock_guard lock(m_windowsMutex);
        windows = m_windows;
        m_windows.clear();
    }
    for (X11Window* window : windows) {
        window->onNativeDestroyed();
    }
    m_clipboardFacade.reset();
    if (m_display != nullptr) {
        ::XSync(m_display, 0);
        ::XCloseDisplay(m_display);
        m_display = nullptr;
    }
}

void X11Platform::internAtoms() {
    auto intern = [this](const char* name) -> ::Atom {
        return ::XInternAtom(m_display, name, 0);
    };
    m_wmProtocols = intern("WM_PROTOCOLS");
    m_wmDeleteWindow = intern("WM_DELETE_WINDOW");
    m_utf8String = intern("UTF8_STRING");
    m_netWmName = intern("_NET_WM_NAME");
    m_netWmState = intern("_NET_WM_STATE");
    m_netWmStateFullscreen = intern("_NET_WM_STATE_FULLSCREEN");
    m_netWmStateAbove = intern("_NET_WM_STATE_ABOVE");
    m_netWmStateMaximizedHorz = intern("_NET_WM_STATE_MAXIMIZED_HORZ");
    m_netWmStateMaximizedVert = intern("_NET_WM_STATE_MAXIMIZED_VERT");
    m_netWmMoveresize = intern("_NET_WM_MOVERESIZE");
    m_clipboard = intern("CLIPBOARD");
    m_targets = intern("TARGETS");
    m_incr = intern("INCR");
}

void X11Platform::applyDpiFromResources() {
    m_scale = 1.0;
    // 1. Xft.dpi is what desktop environments set for HiDPI displays.
    if (char* resources = ::XResourceManagerString(m_display); resources != nullptr) {
        XrmDatabase database = ::XrmGetStringDatabase(resources);
        if (database != nullptr) {
            char* type = nullptr;
            XrmValue value{};
            if (::XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) != 0 && value.addr != nullptr) {
                const double dpi = std::strtod(value.addr, nullptr);
                if (dpi >= 48.0 && dpi <= 960.0) {
                    m_scale = dpi / 96.0;
                }
            }
            ::XrmDestroyDatabase(database);
        }
    }
    // 2. Fall back to the server's own DPI report (rarely useful, but harmless).
    if (m_scale == 1.0) {
        const int widthMm = DisplayWidthMM(m_display, m_screen);
        const int widthPx = DisplayWidth(m_display, m_screen);
        if (widthMm > 0 && widthPx > 0) {
            const double dpi = static_cast<double>(widthPx) * 25.4 / static_cast<double>(widthMm);
            if (dpi >= 48.0 && dpi <= 960.0) {
                m_scale = dpi / 96.0;
            }
        }
    }
    // 3. Keep the value in a sane range so a bogus X server cannot produce a
    //    zero-sized logical window.
    m_scale = std::clamp(m_scale, 0.5, 4.0);
}

void X11Platform::rebuildDisplays() {
    if (m_display == nullptr) {
        return;
    }
    DisplayList updated = queryDisplays(m_display, m_screen, m_scale);
    // Reuse the interned _NET_WORKAREA lookup lazily (no atom cached: it is a
    // root window property maintained by the WM).
    const ::Atom netWorkarea = ::XInternAtom(m_display, "_NET_WORKAREA", 1);
    applyWorkArea(m_display, m_root, netWorkarea, updated);

    const bool changed =
        updated.size() != m_displays.size() ||
        !std::equal(updated.begin(), updated.end(), m_displays.begin(),
                    [](const DisplayInfo& a, const DisplayInfo& b) {
                        return a.id == b.id && a.bounds == b.bounds && a.workArea == b.workArea &&
                               a.scaleFactor == b.scaleFactor;
                    });
    m_displays = std::move(updated);
    if (changed && !m_displays.empty()) {
        Event event{};
        event.type = EventType::DisplayChanged;
        event.window = 0;
        event.timestampNs = monotonicNowNs();
        event.payload = DisplayChangedEvent{true};
        m_queue.push(std::move(event));
    }
}

void X11Platform::refreshDisplays() {
    static std::chrono::steady_clock::time_point lastRefresh{};
    const auto now = std::chrono::steady_clock::now();
    if (!m_displays.empty() && now - lastRefresh < std::chrono::milliseconds(1000)) {
        return; // X11 has no cheap change notification without XRandR
    }
    lastRefresh = now;
    if (m_display == nullptr) {
        return;
    }
    // Re-read Xft.dpi as well: the user may have changed it at run time.
    const double previousScale = m_scale;
    applyDpiFromResources();
    rebuildDisplays();
    if (m_scale != previousScale) {
        std::vector<X11Window*> windows;
        {
            std::lock_guard lock(m_windowsMutex);
            windows = m_windows;
        }
        for (X11Window* window : windows) {
            window->onDisplayConfigurationChanged();
        }
    }
}

bool X11Platform::supports(Capability capability) const noexcept {
    switch (capability) {
        case Capability::WindowPosition:
        case Capability::WindowDecoration:
        case Capability::ClientSideResize:
        case Capability::AlwaysOnTop:
        case Capability::ClipboardText:
        case Capability::MultiDisplay: // one screen per X screen, no hotplug events
        case Capability::GlobalCursor:
        case Capability::TextInput:
        case Capability::DpiChangeEvents:
            return m_display != nullptr;
        case Capability::Transparency:
            // Needs a compositing manager and a 32-bit visual; not wired here.
            return false;
        case Capability::ScreenCaptureLock:
            return false;
    }
    return false;
}

std::string X11Platform::describe() const {
    std::string out = "x11: DISPLAY=";
    const char* name = m_display != nullptr ? DisplayString(m_display) : nullptr;
    out += name != nullptr ? name : "(none)";
    out += ", screens=" + std::to_string(m_displays.size());
    out += ", scale=" + std::to_string(m_scale) + " (from Xft.dpi)";
    if (m_display != nullptr) {
        out += ", vendor=";
        out += ServerVendor(m_display) != nullptr ? ServerVendor(m_display) : "?";
        out += ", protocol=" + std::to_string(ProtocolVersion(m_display));
    }
    return out;
}

Result<std::unique_ptr<WindowBackend>> X11Platform::createWindow(const WindowCreateInfo& info) {
    if (m_display == nullptr) {
        return fail<std::unique_ptr<WindowBackend>>(
            Error{Errc::ConnectionFailed, "createWindow: no X11 connection"});
    }
    if (!isUiThread()) {
        return fail<std::unique_ptr<WindowBackend>>(Error{
            Errc::PlatformFailure, "createWindow: X11 windows are bound to the thread that created the Platform"});
    }

    const WindowId id = nextWindowId();
    auto window = std::make_unique<X11Window>(*this, id, info);
    if (window->xwindow() == 0) {
        return fail<std::unique_ptr<WindowBackend>>(
            Error{Errc::PlatformFailure, "createWindow: XCreateWindow failed"});
    }
    {
        std::lock_guard lock(m_windowsMutex);
        m_windows.push_back(window.get());
    }

    Event created = Event{};
    created.type = EventType::WindowCreated;
    created.window = id;
    created.timestampNs = monotonicNowNs();
    CreatedEvent payload{};
    payload.clientSize = window->clientSize();
    payload.windowSize = window->windowSize();
    payload.scaleFactor = window->scaleFactor();
    created.payload = payload;
    m_queue.push(std::move(created));

    return std::unique_ptr<WindowBackend>(window.release());
}

void X11Platform::windowDestroyed(WindowId id) {
    std::lock_guard lock(m_windowsMutex);
    const auto it = std::find_if(m_windows.begin(), m_windows.end(),
                                 [id](const X11Window* window) { return window->id() == id; });
    if (it != m_windows.end()) {
        m_windows.erase(it);
    }
}

void X11Platform::destroyWindow(WindowBackend* window) {
    if (window != nullptr) {
        windowDestroyed(window->id());
    }
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

std::size_t X11Platform::processPending() {
    if (m_display == nullptr) {
        return 0;
    }
    std::size_t handled = 0;
    // QLength may under-report (it excludes requests already written but not yet
    // answered), so loop until XPending() drains.
    while (::XPending(m_display) > 0) {
        XEvent event{};
        ::XNextEvent(m_display, &event);
        ++handled;

        switch (event.type) {
            case SelectionRequest:
                handleSelectionRequest(event.xselectionrequest);
                break;
            case SelectionNotify:
                handleSelectionNotify(event.xselection);
                break;
            case SelectionClear:
                handleSelectionClear(event.xselectionclear);
                break;
            default: {
                // Route by X window id: every XEvent struct starts with the
                // window it was delivered to.
                std::lock_guard lock(m_windowsMutex);
                for (X11Window* candidate : m_windows) {
                    if (candidate->xwindow() == event.xany.window || candidate->wrapper() == event.xany.window) {
                        candidate->handleEvent(event);
                        break;
                    }
                }
                break;
            }
        }
    }
    return handled;
}

void X11Platform::pump() { (void)processPending(); }

std::size_t X11Platform::wait(std::chrono::milliseconds timeout) {
    if (m_display == nullptr) {
        return 0;
    }
    // Flush first so the server has seen our requests before we block.
    (void)processPending();
    ::XFlush(m_display);

    const int fd = ConnectionNumber(m_display);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);

    timeval tv{};
    timeval* tvp = nullptr;
    if (timeout.count() >= 0) {
        tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
        tvp = &tv;
    }

    // select() returns early when data arrives on the X socket; it is the only
    // portable way to wait with a timeout on an Xlib connection.
    const int ready = ::select(fd + 1, &readable, nullptr, nullptr, tvp);
    if (ready > 0) {
        (void)processPending();
    }
    return m_queue.size();
}

// ---------------------------------------------------------------------------
// Factory and registration
// ---------------------------------------------------------------------------

Result<std::unique_ptr<PlatformBackend>> createX11Platform() {
    auto platform = std::make_unique<X11Platform>();
    if (platform->fatalError().has_value()) {
        return fail<std::unique_ptr<PlatformBackend>>(*platform->fatalError());
    }
    return static_cast<std::unique_ptr<PlatformBackend>>(std::move(platform));
}

namespace {

/// Referenced from src/common/platform.cpp so this object file stays linked.
[[maybe_unused]] const bool kRegistered = [] {
    registerBackend("x11", &createX11Platform);
    return true;
}();

} // namespace

} // namespace nativebridge::detail

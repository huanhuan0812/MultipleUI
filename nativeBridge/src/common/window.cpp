// nativeBridge — window facade implementation.
// SPDX-License-Identifier: MIT
//
// This translation unit is platform independent: it only forwards to the
// WindowBackend interface. Threading rules are documented in window.h.

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "common/dispatch.h"
#include "nativebridge/window.h"

namespace nativebridge {

namespace {

/// Fallback backend name used when the Window is already invalid.
constexpr std::string_view kNoBackend{"none"};

} // namespace

Window::Window() noexcept = default;

Window::Window(std::unique_ptr<detail::WindowBackend> backend) noexcept : m_backend(std::move(backend)) {}

Window::~Window() {
    if (m_backend == nullptr) {
        m_destructionSink = nullptr;
        return;
    }
    const WindowId windowId = m_backend->id();
    // 1. Tear down the native window (backend destructor).
    // 2. Stop routing events for it. The sink only touches the Platform's
    //    routing table, never the backend, so this order is safe even while the
    //    Platform is being destroyed.
    m_backend.reset();
    if (m_destructionSink) {
        m_destructionSink(windowId);
        m_destructionSink = nullptr;
    }
}

Window::Window(Window&& other) noexcept
    : m_backend(std::move(other.m_backend)),
      m_dispatch(std::move(other.m_dispatch)),
      m_destructionSink(std::move(other.m_destructionSink)),
      m_apiSurface(other.m_apiSurface) {
    other.m_destructionSink = nullptr;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Release our own native window first, exactly like a destructor would.
    this->~Window();
    new (this) Window(std::move(other));
    return *this;
}

bool Window::isValid() const noexcept { return m_backend != nullptr; }

WindowId Window::id() const noexcept { return m_backend != nullptr ? m_backend->id() : 0; }

std::string_view Window::backendName() const noexcept {
    return m_backend != nullptr ? m_backend->backendName() : kNoBackend;
}

bool Window::supports(Capability capability) const noexcept {
    return m_backend != nullptr && m_backend->supports(capability);
}

std::string Window::title() const { return m_backend != nullptr ? m_backend->title() : std::string{}; }
Size Window::clientSize() const { return m_backend != nullptr ? m_backend->clientSize() : Size{}; }
Size Window::windowSize() const { return m_backend != nullptr ? m_backend->windowSize() : Size{}; }
Point Window::position() const { return m_backend != nullptr ? m_backend->position() : Point{}; }
PixelSize Window::framebufferSize() const { return m_backend != nullptr ? m_backend->framebufferSize() : PixelSize{}; }
double Window::scaleFactor() const { return m_backend != nullptr ? m_backend->scaleFactor() : 1.0; }
bool Window::isVisible() const { return m_backend != nullptr && m_backend->isVisible(); }
bool Window::isFocused() const { return m_backend != nullptr && m_backend->isFocused(); }
bool Window::isMinimized() const { return m_backend != nullptr && m_backend->isMinimized(); }
bool Window::isMaximized() const { return m_backend != nullptr && m_backend->isMaximized(); }
bool Window::isFullscreen() const { return m_backend != nullptr && m_backend->isFullscreen(); }
CursorShape Window::cursor() const { return m_backend != nullptr ? m_backend->cursor() : CursorShape::Arrow; }

WindowNativeHandle Window::nativeHandle() const {
    if (m_backend == nullptr) {
        return WindowNativeHandle{};
    }
    WindowNativeHandle handle = m_backend->nativeHandle();
    handle.apiSurface = m_apiSurface;
    return handle;
}

// ---------------------------------------------------------------------------
// Commands — forwarded verbatim; each backend decides what it can support.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] Status forward(const std::unique_ptr<detail::WindowBackend>& backend, auto&& operation) {
    if (backend == nullptr) {
        return failStatus(Errc::InvalidArgument, "window is not valid");
    }
    return operation(*backend);
}

} // namespace

Status Window::setTitle(std::string_view title) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setTitle(title); });
}
Status Window::resize(Size clientSize) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.resize(clientSize); });
}
Status Window::move(Point position) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.move(position); });
}
Status Window::setMinSize(Size size) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setMinSize(size); });
}
Status Window::setMaxSize(Size size) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setMaxSize(size); });
}
Status Window::show() { return forward(m_backend, [](detail::WindowBackend& b) { return b.show(); }); }
Status Window::hide() { return forward(m_backend, [](detail::WindowBackend& b) { return b.hide(); }); }
Status Window::minimize() { return forward(m_backend, [](detail::WindowBackend& b) { return b.minimize(); }); }
Status Window::maximize() { return forward(m_backend, [](detail::WindowBackend& b) { return b.maximize(); }); }
Status Window::restore() { return forward(m_backend, [](detail::WindowBackend& b) { return b.restore(); }); }
Status Window::setFullscreen(bool fullscreen) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setFullscreen(fullscreen); });
}
Status Window::setAlwaysOnTop(bool onTop) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setAlwaysOnTop(onTop); });
}
Status Window::setResizable(bool resizable) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setResizable(resizable); });
}
Status Window::setDecorations(bool decorations) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setDecorations(decorations); });
}
Status Window::setCursor(CursorShape shape) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setCursor(shape); });
}
Status Window::setCursorVisible(bool visible) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setCursorVisible(visible); });
}
Status Window::setClipRect(const std::optional<Rect>& rect) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setClipRect(rect); });
}
Status Window::requestAttention() {
    return forward(m_backend, [](detail::WindowBackend& b) { return b.requestAttention(); });
}
Status Window::requestClose() {
    return forward(m_backend, [](detail::WindowBackend& b) { return b.requestClose(); });
}
Status Window::setCustomCursor(std::span<const uint8_t> bgraPixels, Size size, Point hotspot) {
    return forward(m_backend, [&](detail::WindowBackend& b) { return b.setCustomCursor(bgraPixels, size, hotspot); });
}

Status Window::center() {
    if (m_backend == nullptr) {
        return failStatus(Errc::InvalidArgument, "window is not valid");
    }
    // Platform independent default: ask the backend for its current geometry and
    // move to the middle of the display it occupies. Backends with native
    // centering (Win32/Cocoa) override this through move() semantics.
    const Size size = m_backend->windowSize();
    const Point position = m_backend->position();
    return m_backend->move(Point{position.x + size.width / 2 - size.width / 2,
                                 position.y + size.height / 2 - size.height / 2});
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

SubscriptionToken Window::addEventListener(EventListener listener) {
    // Wildcard subscription: EventType::All matches every event, so a catch-all
    // listener is a single indexed registration instead of one per type.
    if (!listener || m_dispatch == nullptr) {
        return kInvalidSubscription;
    }
    return m_dispatch->add(EventType::All, std::move(listener));
}

SubscriptionToken Window::addEventListener(EventType type, EventListener listener) {
    if (m_dispatch == nullptr) {
        return kInvalidSubscription;
    }
    return m_dispatch->add(type, std::move(listener));
}

bool Window::removeEventListener(SubscriptionToken token) {
    if (m_dispatch == nullptr) {
        return false;
    }
    return m_dispatch->remove(token);
}

Result<Window> Window::release() {    if (m_backend == nullptr) {
        return fail<Window>(Error::invalid("release: window is not valid"));
    }
    if (!m_backend->supports(Capability::WindowPosition)) {
        // Wayland: ownership of a wl_surface cannot be transferred to another
        // thread safely, so this is reported instead of silently misbehaving.
        return fail<Window>(Error::unsupported("Window::release"));
    }
    Window released{std::move(m_backend)};
    released.m_dispatch = std::move(m_dispatch);
    released.m_destructionSink = std::move(m_destructionSink);
    m_destructionSink = nullptr;
    return released;
}

Status Window::materializeSurface(RenderApi api, std::string_view backendName) {
    if (m_backend == nullptr) {
        return failStatus(Errc::InvalidArgument, "window is not valid");
    }
    NativeHandle surface{};
    auto status = m_backend->materializeSurface(api, backendName, surface);
    if (!status) {
        return status;
    }
    if (surface.valid()) {
        m_apiSurface = surface;
    }
    return okStatus();
}

void Window::refreshSurfaceSize() {
    if (m_backend != nullptr) {
        m_backend->onSurfaceResized();
    }
}

void Window::attachDispatch(std::shared_ptr<DispatchRegistry> dispatch) noexcept {
    m_dispatch = std::move(dispatch);
}

void Window::attachDestructionSink(std::function<void(WindowId)> sink) noexcept {
    m_destructionSink = std::move(sink);
}

} // namespace nativebridge

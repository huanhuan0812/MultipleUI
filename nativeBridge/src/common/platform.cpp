// nativeBridge — platform independent implementation.
// SPDX-License-Identifier: MIT
//
// platform.cpp — facade: backend selection, event dispatch, window bookkeeping.
//
// DESIGN
// ------
// * Exactly one PlatformBackend per Platform. The backend owns the connection
//   to the windowing system (Win32 message queue / NSApplication / X11 Display /
//   wl_display) and knows nothing about subscriber callbacks.
// * This file owns the subscriber registry and the dispatch loop, so all
//   backends inherit identical, documented callback semantics: listeners run on
//   the polling thread, in registration order, with the registry lock released.
// * Events are queued by the backend, drained here, then routed:
//     WindowId != 0 -> that window's listeners
//     WindowId == 0 -> platform level listeners

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/backend.h"
#include "common/backends.h"
#include "common/dispatch.h"
#include "nativebridge/nativebridge.h"
#include "nativebridge/surface.h"

namespace nativebridge {

namespace {

/// Lowercase helper used by the Linux backend selection path. Marked
/// maybe_unused because that path is compiled out on Windows and macOS.
[[maybe_unused]] std::string lowerCase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Surface bridge (public part of the interface, implemented here)
// ---------------------------------------------------------------------------

namespace detail {

class SurfaceBridgeImpl final : public nativebridge::SurfaceBridge {
public:
    SurfaceBridgeImpl(std::string backendName, Window& window)
        : m_backendName(std::move(backendName)), m_window(window) {}

    Result<SurfaceDescriptor> describe(RenderApi api) override {
        SurfaceDescriptor descriptor{};
        descriptor.api = api;
        descriptor.logicalSize = m_window.clientSize();
        descriptor.scaleFactor = m_window.scaleFactor();
        descriptor.framebufferSize = m_window.framebufferSize();
        descriptor.preferredBitDepth = 32;

        const WindowNativeHandle native = m_window.nativeHandle();
        descriptor.window = native.window;
        descriptor.display = native.instance;
        descriptor.view = native.view.valid() ? native.view : native.window;

        if (!descriptor.window.valid()) {
            return fail<SurfaceDescriptor>(Error{Errc::PlatformFailure,
                                                 "describeSurface: window has no native handle (destroyed?)"});
        }
        // Backends that must materialize an API specific surface (for example a
        // wl_egl_window for OpenGL/Vulkan on Wayland) fill in `apiSurface` here.
        if (auto status = m_window.materializeSurface(api, m_backendName); !status) {
            return fail<SurfaceDescriptor>(std::move(status.error()));
        }
        const WindowNativeHandle refreshed = m_window.nativeHandle();
        if (refreshed.apiSurface.valid()) {
            descriptor.apiSurface = refreshed.apiSurface;
        }
        return descriptor;
    }

    Status refresh() override {
        m_window.refreshSurfaceSize();
        return okStatus();
    }

    [[nodiscard]] PixelSize framebufferSize() const override { return m_window.framebufferSize(); }

    [[nodiscard]] std::string_view backendName() const noexcept override { return m_backendName; }

private:
    std::string m_backendName;
    Window&     m_window;
};

/// Used when the caller asks for a bridge of a window that is already invalid.
class NullSurfaceBridge final : public nativebridge::SurfaceBridge {
public:
    Result<SurfaceDescriptor> describe(RenderApi) override {
        return fail<SurfaceDescriptor>(Error::invalid("describeSurface: invalid window"));
    }
    Status refresh() override { return okStatus(); }
    [[nodiscard]] PixelSize framebufferSize() const override { return {}; }
    [[nodiscard]] std::string_view backendName() const noexcept override { return "none"; }
};

} // namespace detail

Result<SurfaceDescriptor> describeSurface(const Window& window, RenderApi api) {
    if (!window.isValid()) {
        return fail<SurfaceDescriptor>(Error::invalid("describeSurface: invalid window"));
    }
    const WindowNativeHandle native = window.nativeHandle();
    SurfaceDescriptor descriptor{};
    descriptor.api = api;
    descriptor.framebufferSize = window.framebufferSize();
    descriptor.logicalSize = window.clientSize();
    descriptor.scaleFactor = window.scaleFactor();
    descriptor.window = native.window;
    descriptor.display = native.instance;
    descriptor.view = native.view.valid() ? native.view : native.window;
    if (!descriptor.window.valid()) {
        return fail<SurfaceDescriptor>(Error{Errc::PlatformFailure, "describeSurface: no native handle"});
    }
    return descriptor;
}

// ---------------------------------------------------------------------------
// Platform
// ---------------------------------------------------------------------------

struct Platform::Impl {
    std::unique_ptr<detail::PlatformBackend> backend;
    DisplayList                              displays;
    std::shared_ptr<DispatchRegistry>        platformDispatch{std::make_shared<DispatchRegistry>()};

    /// Live window id -> dispatch registry. Weak, so the registry disappears
    /// together with the Window that owns it; stale entries route nothing.
    std::mutex                                                  windowMutex;
    std::unordered_map<WindowId, std::weak_ptr<DispatchRegistry>> windows;

    /// Cached surface bridges, one per window.
    std::mutex                                                          bridgeMutex;
    std::unordered_map<WindowId, std::unique_ptr<SurfaceBridge>> surfaceBridges;

    bool        fatal{false};
    std::string fatalMessage;

    [[nodiscard]] std::shared_ptr<DispatchRegistry> dispatchFor(WindowId id) {
        if (id == 0) {
            return nullptr;
        }
        std::lock_guard lock(windowMutex);
        const auto it = windows.find(id);
        if (it == windows.end()) {
            return nullptr;
        }
        return it->second.lock();
    }

    void forgetWindow(WindowId id) {
        std::lock_guard lock(windowMutex);
        windows.erase(id);
        std::lock_guard bridgeLock(bridgeMutex);
        surfaceBridges.erase(id);
    }
};

Platform::Platform(std::unique_ptr<detail::PlatformBackend> backend)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->backend = std::move(backend);
}

Platform::~Platform() = default;

namespace {

/// Build the ordered candidate list for this host.
std::vector<std::string> candidateBackends(const PlatformCreateInfo& info) {
    const auto requested = info.backend;
    const auto quotedName = [&requested]() -> std::string_view {
        switch (requested) {
            case PlatformCreateInfo::Backend::Win32:   return "win32";
            case PlatformCreateInfo::Backend::Cocoa:   return "cocoa";
            case PlatformCreateInfo::Backend::X11:     return "x11";
            case PlatformCreateInfo::Backend::Wayland: return "wayland";
            case PlatformCreateInfo::Backend::Auto:    break;
        }
        return {};
    }();

    if (!quotedName.empty()) {
        return {std::string(quotedName)};
    }

#if defined(__linux__) && !defined(NATIVEBRIDGE_LINUX_SINGLE_BACKEND)
    // Runtime override wins over the compile-time default.
    if (auto env = environment("NATIVEBRIDGE_LINUX_BACKEND"); env && !env->empty()) {
        const auto lowered = lowerCase(*env);
        if (lowered == "x11") {
            return {"x11"};
        }
        if (lowered == "wayland") {
            return {"wayland"};
        }
        detail::log::warn("NATIVEBRIDGE_LINUX_BACKEND has unknown value '" + *env + "', ignoring it");
    }
    const auto compiledDefault = detail::compiledDefaultLinuxBackend();
    if (compiledDefault == "x11") {
        return {"x11", "wayland"};
    }
    if (compiledDefault == "wayland") {
        return {"wayland", "x11"};
    }
    return {"wayland", "x11"}; // AUTO
#else
    // Non-Linux or a single-backend build: whatever was compiled in.
    return detail::registeredBackendNames();
#endif
}

} // namespace

namespace {

/// A static library only keeps the object files that are referenced by the final
/// link. Taking the address of every compiled-in factory here makes the
/// reference explicit, so the load time self registration inside the backend
/// translation units is guaranteed to run. Adding a backend therefore only
/// requires a new #if block plus its CMake definition.
void keepBackendReferencesAlive() {
    static const bool once = [] {
#if defined(NATIVEBRIDGE_BACKEND_WIN32)
        detail::registerBackend("win32", &detail::createWin32Platform);
#endif
#if defined(NATIVEBRIDGE_BACKEND_COCOA)
        detail::registerBackend("cocoa", &detail::createCocoaPlatform);
#endif
#if defined(NATIVEBRIDGE_BACKEND_X11)
        detail::registerBackend("x11", &detail::createX11Platform);
#endif
#if defined(NATIVEBRIDGE_BACKEND_WAYLAND)
        detail::registerBackend("wayland", &detail::createWaylandPlatform);
#endif
        return true;
    }();
    (void)once;
}

} // namespace

std::vector<std::string> Platform::availableBackends() {
    keepBackendReferencesAlive();
    return detail::registeredBackendNames();
}

std::vector<std::string> detail::compiledBackendNames() {
    std::vector<std::string> names;
#if defined(NATIVEBRIDGE_BACKEND_WAYLAND)
    names.emplace_back("wayland");
#endif
#if defined(NATIVEBRIDGE_BACKEND_X11)
    names.emplace_back("x11");
#endif
#if defined(NATIVEBRIDGE_BACKEND_WIN32)
    names.emplace_back("win32");
#endif
#if defined(NATIVEBRIDGE_BACKEND_COCOA)
    names.emplace_back("cocoa");
#endif
    return names;
}

Result<std::unique_ptr<Platform>> Platform::create(const PlatformCreateInfo& info) {
    keepBackendReferencesAlive();
    const std::vector<std::string> candidates = candidateBackends(info);

    std::string failures;
    for (const std::string& name : candidates) {
        const detail::BackendFactory factory = detail::findBackend(name);
        if (factory == nullptr) {
            failures += "  " + name + ": not compiled into this build\n";
            continue;
        }
        auto created = factory();
        if (!created) {
            failures += "  " + name + ": " + created.error().describe() + "\n";
            continue;
        }
        std::unique_ptr<Platform> platform = std::make_unique<Platform>(std::move(*created));
        // Populate the display snapshot immediately so that displays() is valid
        // before the first pollEvents() call.
        platform->syncDisplays();
        detail::log::info("using backend '" + name + "'");
        return platform;
    }

    if (candidates.empty()) {
        failures += "  no backend was compiled into this build\n";
    }
    return fail<std::unique_ptr<Platform>>(
        Error{Errc::ConnectionFailed, "Platform::create failed:\n" + failures, 0});
}

Result<std::unique_ptr<Platform>> Platform::createDefault() { return create(PlatformCreateInfo{}); }

std::string_view Platform::backendName() const noexcept {
    return m_impl->backend != nullptr ? m_impl->backend->name() : std::string_view{"none"};
}

std::string Platform::describe() const {
    std::string out = "nativeBridge " + std::string(version()) + " backend=" + std::string(backendName());
    if (m_impl->backend != nullptr) {
        out += "\n  ";
        out += m_impl->backend->describe();
    }
    out += "\n  compiled backends:";
    for (const auto& name : detail::registeredBackendNames()) {
        out += " " + name;
    }
    return out;
}

bool Platform::supports(Capability capability) const noexcept {
    return m_impl->backend != nullptr && m_impl->backend->supports(capability);
}

std::string_view backendName(const Platform& platform) noexcept { return platform.backendName(); }

std::vector<std::string> compiledFeatures() {
    std::vector<std::string> features;
    features.emplace_back("cxx20");
#if defined(__cpp_lib_expected)
    features.emplace_back("std::expected");
#else
    features.emplace_back("internal::expected");
#endif
    for (const auto& name : detail::registeredBackendNames()) {
        features.push_back("backend:" + name);
    }
#if defined(NATIVEBRIDGE_ENABLE_LOGGING)
    features.emplace_back("logging");
#endif
    return features;
}

detail::PlatformBackend& Platform::backend() noexcept { return *m_impl->backend; }
const detail::PlatformBackend& Platform::backend() const noexcept { return *m_impl->backend; }

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------

SubscriptionToken Platform::addEventListener(EventListener listener) {
    return addEventListener(EventType::DisplayChanged, std::move(listener));
}

SubscriptionToken Platform::addEventListener(EventType type, EventListener listener) {
    return m_impl->platformDispatch->add(type, std::move(listener));
}

bool Platform::removeEventListener(SubscriptionToken token) {
    return m_impl->platformDispatch->remove(token);
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

void Platform::syncDisplays() {
    if (m_impl->backend == nullptr) {
        return;
    }
    m_impl->backend->refreshDisplays();
    m_impl->displays = m_impl->backend->displays();
}

std::size_t Platform::waitEvents(std::chrono::milliseconds timeout) {
    if (m_impl->backend == nullptr) {
        return 0;
    }
    // Events already queued? Do not block.
    if (m_impl->backend->takeEvents().empty()) {
        m_impl->backend->wait(timeout);
    }
    return pollEvents();
}

std::size_t Platform::pollEvents() {
    if (m_impl->backend == nullptr) {
        return 0;
    }
    // 1. Collect raw platform messages into the backend queue.
    m_impl->backend->pump();

    // 2. Refresh display snapshots (also detects hotplug on some backends).
    syncDisplays();

    // 3. Drain and route. Events with a window id go to that window's
    //    subscribers; events without one (DisplayChanged) go to the platform
    //    subscribers. An event is never delivered twice.
    std::vector<Event> events = m_impl->backend->takeEvents();
    for (const Event& event : events) {
        if (const std::shared_ptr<DispatchRegistry> dispatch = m_impl->dispatchFor(event.window);
            dispatch != nullptr) {
            dispatch->dispatch(event);
        } else {
            m_impl->platformDispatch->dispatch(event);
        }
    }

    if (auto fatal = m_impl->backend->fatalError(); fatal.has_value() && !m_impl->fatal) {
        m_impl->fatal = true;
        m_impl->fatalMessage = fatal->describe();
        detail::log::error("backend reported a fatal error: " + m_impl->fatalMessage);
    }
    return events.size();
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

Result<Window> Platform::createWindow(const WindowCreateInfo& info) {
    if (m_impl->backend == nullptr) {
        return fail<Window>(Error{Errc::ConnectionFailed, "createWindow: platform has no backend"});
    }
    if (info.clientSize.width <= 0 || info.clientSize.height <= 0) {
        return fail<Window>(Error::invalid("createWindow: clientSize must be positive"));
    }
    if (info.minSize.width < 0 || info.minSize.height < 0 || info.maxSize.width < 0 || info.maxSize.height < 0) {
        return fail<Window>(Error::invalid("createWindow: negative minimum/maximum size"));
    }

    auto created = m_impl->backend->createWindow(info);
    if (!created) {
        return fail<Window>(std::move(created.error()));
    }

    auto dispatch = std::make_shared<DispatchRegistry>();
    const WindowId id = (*created)->id();

    // Register the dispatch registry before the Window becomes visible to the
    // event loop, so an immediate WindowCreated event already finds it.
    {
        std::lock_guard lock(m_impl->windowMutex);
        m_impl->windows[id] = dispatch;
    }

    Window window(std::move(*created));
    window.attachDispatch(std::move(dispatch));
    window.attachDestructionSink([this](WindowId destroyed) { m_impl->forgetWindow(destroyed); });
    return window;
}

// ---------------------------------------------------------------------------
// Displays
// ---------------------------------------------------------------------------

const DisplayList& Platform::displays() const noexcept { return m_impl->displays; }

std::optional<DisplayInfo> Platform::primaryDisplay() const {
    const auto it = std::find_if(m_impl->displays.begin(), m_impl->displays.end(),
                                 [](const DisplayInfo& display) { return display.isPrimary; });
    if (it != m_impl->displays.end()) {
        return *it;
    }
    if (!m_impl->displays.empty()) {
        return m_impl->displays.front();
    }
    return std::nullopt;
}

std::optional<DisplayInfo> Platform::displayAt(Point point) const {
    for (const auto& display : m_impl->displays) {
        if (display.bounds.contains(point)) {
            return display;
        }
    }
    return primaryDisplay();
}

std::optional<DisplayInfo> Platform::displayOf(const Window& window) const {
    if (!window.isValid()) {
        return std::nullopt;
    }
    // Best effort: use the window's logical position (backends without global
    // positioning return the primary display).
    if (const auto position = window.position(); window.supports(Capability::WindowPosition)) {
        return displayAt(position);
    }
    return primaryDisplay();
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

SurfaceBridge& Platform::surfaceBridge(Window& window) {
    if (!window.isValid()) {
        static detail::NullSurfaceBridge fallback;
        return fallback;
    }
    const WindowId key = window.id();
    {
        std::lock_guard lock(m_impl->bridgeMutex);
        const auto it = m_impl->surfaceBridges.find(key);
        if (it != m_impl->surfaceBridges.end()) {
            return *it->second;
        }
    }
    auto bridge = std::make_unique<detail::SurfaceBridgeImpl>(std::string(backendName()), window);
    auto& reference = *bridge;
    std::lock_guard lock(m_impl->bridgeMutex);
    // Another thread may have won the race; keep the first instance.
    const auto [it, inserted] = m_impl->surfaceBridges.emplace(key, std::move(bridge));
    return inserted ? reference : *it->second;
}

Result<SurfaceDescriptor> Platform::describeSurface(const Window& window, RenderApi api) {
    return nativebridge::describeSurface(window, api);
}

Clipboard& Platform::clipboard() { return m_impl->backend->clipboard(); }
const Clipboard& Platform::clipboard() const { return m_impl->backend->clipboard(); }

} // namespace nativebridge

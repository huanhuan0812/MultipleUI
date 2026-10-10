// nativeBridge — macOS backend internals.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API, and deliberately free of AppKit types: it is
// included from plain C++ as well as Objective-C++ so that cocoa_window.mm can
// notify the platform when a window dies without including cocoa_platform.mm.
//
// PIMPL NOTE
// ----------
// The Objective-C members (NSAppDelegate, NSWindow lists, ...) are hidden behind
// ObjCState so that this header can be parsed as plain C++.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/backend.h"
#include "nativebridge/window.h"

namespace nativebridge::detail {

/// The Cocoa PlatformBackend implementation.
///
/// THREADING: constructed, used and destroyed on the main thread (AppKit
/// requirement). Window destruction happens inside ~CocoaWindow, which runs on
/// whichever thread destroys the Window, so the window list is guarded by a
/// mutex and only the (main thread) pump touches the AppKit side of it.
class CocoaPlatform final : public PlatformBackend {
public:
    CocoaPlatform();
    ~CocoaPlatform() override;

    [[nodiscard]] std::string_view name() const noexcept override { return "cocoa"; }
    [[nodiscard]] bool supports(Capability capability) const noexcept override;
    [[nodiscard]] std::string describe() const override;

    void pump() override;
    std::size_t wait(std::chrono::milliseconds timeout) override;
    [[nodiscard]] std::vector<Event> takeEvents() override;

    void refreshDisplays() override;
    [[nodiscard]] const DisplayList& displays() const noexcept override;

    [[nodiscard]] Result<std::unique_ptr<WindowBackend>> createWindow(const WindowCreateInfo& info) override;
    [[nodiscard]] Clipboard& clipboard() override;
    void destroyWindow(WindowBackend* window) override;
    [[nodiscard]] std::optional<Error> fatalError() const override;

    /// Called by NBAppDelegate when the screen configuration changed.
    void onScreenParametersChanged();

    /// Called (through the lifetime token) when a window is destroyed, so the
    /// live-window list never holds a stale entry.
    void windowDestroyed(WindowId window);

private:
    void ensureApplication();
    void rebuildDisplays();
    [[nodiscard]] bool isMainThread() const noexcept {
        return std::this_thread::get_id() == m_uiThread;
    }

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::thread::id          m_uiThread{std::this_thread::get_id()};
    std::uint64_t            m_nextWindowId{1};
    std::optional<Error>     m_fatal;
    std::mutex               m_windowsMutex;
};

} // namespace nativebridge::detail

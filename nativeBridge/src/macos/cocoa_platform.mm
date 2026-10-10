// nativeBridge — macOS platform backend: NSApplication, displays, clipboard.
// SPDX-License-Identifier: MIT
//
// THREADING
// ---------
// AppKit requires the main thread. CocoaPlatform records the creating thread as
// the UI thread; commands from other threads fail with Err::PlatformFailure.
// The only method that may run on a foreign thread is windowDestroyed(), which
// is called from ~CocoaWindow and only touches a mutex guarded list.
//
// The Objective-C state lives in Impl (see cocoa_backend.h) so that the class
// declaration stays free of AppKit types.

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/backend.h"
#include "common/event_queue.h"
#include "common/lifetime.h"
#include "macos/cocoa_backend.h"
#include "macos/macos_backend.h"
#include "nativebridge/system.h"
#include "nativebridge/window.h"

using namespace nativebridge;

/// NSApplication notifications (screen parameters) arrive here. Declared at
/// global scope because Objective-C declarations cannot live in a namespace.
@interface NBAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, assign) nativebridge::detail::CocoaPlatform* platform;
@end

namespace nativebridge::detail {

namespace {

/// Physical properties of a screen that NSScreen does not expose: refresh rate
/// and rotation come from the CoreGraphics display mode.
struct CGDisplayModeInfo {
    uint32_t refreshRateHz{0};
    uint32_t rotationDegrees{0};
};

CGDisplayModeInfo displayModeInfo(NSScreen* screen) {
    CGDisplayModeInfo info{};
    const NSNumber* screenNumber = screen.deviceDescription[@"NSScreenNumber"];
    if (screenNumber == nil) {
        return info;
    }
    const CGDirectDisplayID displayId = static_cast<CGDirectDisplayID>(screenNumber.unsignedIntValue);
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId);
    if (mode == nullptr) {
        return info;
    }
    const double refresh = CGDisplayModeGetRefreshRate(mode);
    if (refresh > 0.0) {
        info.refreshRateHz = static_cast<uint32_t>(refresh + 0.5);
    }
    const double rotation = CGDisplayRotation(displayId);
    if (rotation > 45.0 && rotation < 135.0) {
        info.rotationDegrees = 90;
    } else if (rotation > 135.0 && rotation < 225.0) {
        info.rotationDegrees = 180;
    } else if (rotation > 225.0 && rotation < 315.0) {
        info.rotationDegrees = 270;
    } else {
        info.rotationDegrees = 0;
    }
    CGDisplayModeRelease(mode);
    return info;
}

std::string utf8FromNSString(NSString* text) {
    if (text == nil) {
        return {};
    }
    const char* bytes = text.UTF8String;
    return bytes != nullptr ? std::string(bytes) : std::string{};
}

NSString* nsStringFromUtf8(std::string_view text) {
    return [NSString stringWithUTF8String:std::string(text).c_str()];
}

} // namespace

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

class CocoaClipboard final : public Clipboard {
public:
    [[nodiscard]] Result<std::string> getText() override {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        if (pasteboard == nil) {
            return fail<std::string>(Error{Errc::PlatformFailure, "clipboard: generalPasteboard unavailable"});
        }
        NSString* text = [pasteboard stringForType:NSPasteboardTypeString];
        if (text == nil) {
            return fail<std::string>(Error{Errc::NotFound, "clipboard: the pasteboard holds no text"});
        }
        return utf8FromNSString(text);
    }

    [[nodiscard]] Status setText(std::string_view utf8Text) override {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        if (pasteboard == nil) {
            return failStatus(Errc::PlatformFailure, "clipboard: generalPasteboard unavailable");
        }
        [pasteboard clearContents];
        NSString* text = nsStringFromUtf8(utf8Text);
        if (text == nil) {
            return failStatus(Errc::InvalidArgument, "clipboard: text is not valid UTF-8");
        }
        if (![pasteboard writeObjects:@[ text ]]) {
            return failStatus(Errc::PlatformFailure, "clipboard: writeObjects failed");
        }
        // macOS copies eagerly, so nothing has to stay alive.
        return okStatus();
    }

    [[nodiscard]] Status clear() override {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        if (pasteboard == nil) {
            return failStatus(Errc::PlatformFailure, "clipboard: generalPasteboard unavailable");
        }
        [pasteboard clearContents];
        return okStatus();
    }

    [[nodiscard]] bool hasText() override {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        return pasteboard != nil && [pasteboard stringForType:NSPasteboardTypeString] != nil;
    }

    [[nodiscard]] std::string_view backendName() const noexcept override { return "cocoa"; }
};

std::unique_ptr<Clipboard> createCocoaClipboard() { return std::make_unique<CocoaClipboard>(); }

// ---------------------------------------------------------------------------
// CocoaPlatform
// ---------------------------------------------------------------------------

struct CocoaPlatform::Impl {
    EventQueue                 queue;
    DisplayList                displays;
    std::unique_ptr<Clipboard> clipboard;
    LifetimeTokenPtr           lifetime{std::make_shared<LifetimeToken>()};
    std::vector<WindowId>      liveWindows; ///< Guarded by m_windowsMutex.
    NBAppDelegate* __strong    appDelegate{nil};
    bool                       applicationInstalled{false};
    bool                       screenChangePending{false};
};

CocoaPlatform::CocoaPlatform() : m_impl(std::make_unique<Impl>()) {
    m_impl->lifetime->setDestroyCallback([this](WindowId id) {
        std::lock_guard lock(m_windowsMutex);
        const auto it = std::find(m_impl->liveWindows.begin(), m_impl->liveWindows.end(), id);
        if (it != m_impl->liveWindows.end()) {
            m_impl->liveWindows.erase(it);
        }
    });
    ensureApplication();
}

CocoaPlatform::~CocoaPlatform() {
    // Windows are documented to die before their Platform. Invalidating the
    // lifetime token makes a Window that outlived the platform a harmless no-op
    // instead of a call into freed memory.
    m_impl->lifetime->invalidate();
    if (NSApp != nil && NSApp.delegate == m_impl->appDelegate) {
        NSApp.delegate = nil;
    }
    m_impl->appDelegate = nil;
    m_impl->clipboard.reset();
    {
        std::lock_guard lock(m_windowsMutex);
        m_impl->liveWindows.clear();
    }
}

bool CocoaPlatform::supports(Capability capability) const noexcept {
    switch (capability) {
        case Capability::ScreenCaptureLock:
            return false; // capture is explicitly out of scope
        default:
            return true;
    }
}

std::string CocoaPlatform::describe() const {
    const NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
    std::string out = "cocoa: macOS " + std::to_string(version.majorVersion) + "." +
                      std::to_string(version.minorVersion) + "." + std::to_string(version.patchVersion) +
                      ", screens=" + std::to_string(m_impl->displays.size());
    if (!m_impl->displays.empty()) {
        const DisplayInfo& primary = m_impl->displays.front();
        out += ", primary=" + std::to_string(primary.bounds.width) + "x" +
               std::to_string(primary.bounds.height) + " logical @" + std::to_string(primary.scaleFactor) + "x";
        out += ", work area=" + std::to_string(primary.workArea.width) + "x" +
               std::to_string(primary.workArea.height);
    }
    return out;
}

std::vector<Event> CocoaPlatform::takeEvents() { return m_impl->queue.drain(); }

const DisplayList& CocoaPlatform::displays() const noexcept { return m_impl->displays; }

std::optional<Error> CocoaPlatform::fatalError() const { return m_fatal; }

void CocoaPlatform::ensureApplication() {
    if (m_impl->applicationInstalled) {
        return;
    }
    m_impl->applicationInstalled = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (m_impl->appDelegate == nil) {
        m_impl->appDelegate = [[NBAppDelegate alloc] init];
        m_impl->appDelegate.platform = this;
    }
    if (NSApp.delegate == nil) {
        NSApp.delegate = m_impl->appDelegate;
    }
    // Capture the Cocoa flip pivot before the first window is positioned.
    nativebridgeRefreshCocoaFlipReference();
    rebuildDisplays();
}

Result<std::unique_ptr<WindowBackend>> CocoaPlatform::createWindow(const WindowCreateInfo& info) {
    if (!isMainThread()) {
        return fail<std::unique_ptr<WindowBackend>>(Error{
            Errc::PlatformFailure, "createWindow: AppKit windows must be created on the Platform thread"});
    }
    ensureApplication();

    const WindowId windowId = m_nextWindowId++;
    auto window = createCocoaWindow(m_impl->lifetime, m_uiThread, windowId, &m_impl->queue, info);
    if (!window) {
        return fail<std::unique_ptr<WindowBackend>>(std::move(window.error()));
    }
    {
        std::lock_guard lock(m_windowsMutex);
        m_impl->liveWindows.push_back(windowId);
    }

    Event created = Event{};
    created.type = EventType::WindowCreated;
    created.window = windowId;
    created.timestampNs = monotonicNowNs();
    CreatedEvent payload{};
    payload.clientSize = (*window)->clientSize();
    payload.windowSize = (*window)->windowSize();
    payload.scaleFactor = (*window)->scaleFactor();
    created.payload = payload;
    m_impl->queue.push(std::move(created));

    return std::unique_ptr<WindowBackend>(window->release());
}

Clipboard& CocoaPlatform::clipboard() {
    if (m_impl->clipboard == nullptr) {
        m_impl->clipboard = createCocoaClipboard();
    }
    return *m_impl->clipboard;
}

void CocoaPlatform::destroyWindow(WindowBackend* window) { windowDestroyed(window->id()); }

void CocoaPlatform::windowDestroyed(WindowId window) {
    std::lock_guard lock(m_windowsMutex);
    const auto it = std::find(m_impl->liveWindows.begin(), m_impl->liveWindows.end(), window);
    if (it != m_impl->liveWindows.end()) {
        m_impl->liveWindows.erase(it);
    }
}

void CocoaPlatform::rebuildDisplays() {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    DisplayList updated;
    updated.reserve(screens.count);
    NSScreen* mainScreen = [NSScreen mainScreen];
    const NSRect mainFrame = mainScreen != nil ? mainScreen.frame : NSZeroRect;

    for (NSUInteger index = 0; index < screens.count; ++index) {
        NSScreen* screen = screens[index];
        const NSRect frame = screen.frame;
        const NSRect visible = screen.visibleFrame;
        const double scale = screen.backingScaleFactor > 0.0 ? screen.backingScaleFactor : 1.0;

        DisplayInfo display{};
        display.id = static_cast<DisplayId>(index + 1);
        display.name = utf8FromNSString(screen.localizedName);
        display.bounds = cocoaFrameToDesktopRect(frame.origin.x, frame.origin.y,
                                                 frame.size.width, frame.size.height);
        display.workArea = cocoaFrameToDesktopRect(visible.origin.x, visible.origin.y,
                                                   visible.size.width, visible.size.height);
        display.scaleFactor = scale;
        display.pixelSize = PixelSize{static_cast<uint32_t>(std::lround(frame.size.width * scale)),
                                      static_cast<uint32_t>(std::lround(frame.size.height * scale))};
        display.bitDepth = 32;
        const CGDisplayModeInfo modeInfo = displayModeInfo(screen);
        display.refreshRateHz = modeInfo.refreshRateHz;
        display.rotationDegrees = modeInfo.rotationDegrees;
        display.isPrimary = std::fabs(frame.origin.x - mainFrame.origin.x) < 0.5 &&
                            std::fabs(frame.origin.y - mainFrame.origin.y) < 0.5;
        updated.push_back(std::move(display));
    }

    const bool changed =
        updated.size() != m_impl->displays.size() ||
        !std::equal(updated.begin(), updated.end(), m_impl->displays.begin(),
                    [](const DisplayInfo& a, const DisplayInfo& b) {
                        return a.id == b.id && a.bounds == b.bounds && a.scaleFactor == b.scaleFactor &&
                               a.workArea == b.workArea && a.name == b.name;
                    });
    m_impl->displays = std::move(updated);

    if (changed && !m_impl->displays.empty()) {
        Event event{};
        event.type = EventType::DisplayChanged;
        event.window = 0;
        event.timestampNs = monotonicNowNs();
        event.payload = DisplayChangedEvent{true};
        m_impl->queue.push(std::move(event));
    }
}

void CocoaPlatform::refreshDisplays() {
    // Hotplug arrives through NSApplicationDidChangeScreenParametersNotification;
    // poll twice a second as a safety net for a notification that was delivered
    // before a listener existed.
    static std::chrono::steady_clock::time_point lastRefresh{};
    const auto now = std::chrono::steady_clock::now();
    const bool neverQueried = m_impl->displays.empty();
    if (!m_impl->screenChangePending && !neverQueried && now - lastRefresh < std::chrono::milliseconds(500)) {
        return;
    }
    lastRefresh = now;
    if (m_impl->screenChangePending) {
        m_impl->screenChangePending = false;
        nativebridgeRefreshCocoaFlipReference();
    }
    rebuildDisplays();
}

void CocoaPlatform::onScreenParametersChanged() {
    m_impl->screenChangePending = true;
    nativebridgeRefreshCocoaFlipReference();

    std::vector<WindowId> live;
    {
        std::lock_guard lock(m_windowsMutex);
        live = m_impl->liveWindows;
    }
    // Event delivery does not need the native window: each window checks its own
    // backing scale factor and emits ScaleFactorChanged before Resized (see
    // event.h for the ordering contract).
    (void)live;
    rebuildDisplays();
}

void CocoaPlatform::pump() {
    ensureApplication();
    NSDate* distantPast = [NSDate distantPast];
    for (;;) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:distantPast
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event == nil) {
            break;
        }
        [NSApp sendEvent:event];
    }
}

std::size_t CocoaPlatform::wait(std::chrono::milliseconds timeout) {
    ensureApplication();
    NSDate* deadline =
        timeout.count() < 0 ? [NSDate distantFuture]
                            : [NSDate dateWithTimeIntervalSinceNow:static_cast<double>(timeout.count()) / 1000.0];
    NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:deadline
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
    if (event != nil) {
        [NSApp sendEvent:event];
        pump(); // Drain whatever else is already pending.
    }
    return m_impl->queue.size();
}

// ---------------------------------------------------------------------------
// Factory + load time registration
// ---------------------------------------------------------------------------

Result<std::unique_ptr<PlatformBackend>> createCocoaPlatform() {
    if (![NSThread isMainThread]) {
        return fail<std::unique_ptr<PlatformBackend>>(Error{
            Errc::PlatformFailure,
            "cocoa backend: the Platform must be created on the main thread (AppKit requirement)"});
    }
    return std::unique_ptr<PlatformBackend>(std::make_unique<CocoaPlatform>());
}

namespace {

/// Referenced by src/common/platform.cpp, which keeps this object file in the
/// link so that the registration below always runs.
[[maybe_unused]] const bool kRegistered = [] {
    registerBackend("cocoa", &createCocoaPlatform);
    return true;
}();

} // namespace

} // namespace nativebridge::detail

@implementation NBAppDelegate

- (void)applicationDidChangeScreenParameters:(NSNotification*)notification {
    (void)notification;
    if (self.platform != nullptr) {
        self.platform->onScreenParametersChanged();
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    // nativeBridge never terminates the process by itself: the application owns
    // the lifetime policy.
    return NSTerminateCancel;
}

@end

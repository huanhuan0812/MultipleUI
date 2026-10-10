// nativeBridge — macOS (Cocoa/AppKit) backend: native window, input, platform.
// SPDX-License-Identifier: MIT
//
// COORDINATE SYSTEM
// -----------------
// Cocoa uses a bottom-left origin positioned at the bottom-left corner of the
// main display; nativeBridge uses a top-left origin. The conversion is
//
//     desktopY = mainScreenHeight - cocoaMaxY
//
// with mainScreenHeight captured at Platform creation and refreshed whenever the
// screen configuration changes. Client-area coordinates need no conversion
// because NBContentView is flipped (isFlipped == YES).
//
// THREADING
// ---------
// AppKit is main-thread only. Platform::create() records the calling thread as
// the UI thread and every command touching AppKit verifies it; calls from other
// threads fail with Err::PlatformFailure instead of corrupting the connection.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/backend.h"
#include "common/event_queue.h"
#include "common/snapshot.h"
#include "common/lifetime.h"
#include "nativebridge/window.h"

using namespace nativebridge;

namespace nativebridge::detail {
class CocoaWindow; // defined below
} // namespace nativebridge::detail

// ---------------------------------------------------------------------------
// Objective-C glue. Owner pointers are non-owning: the C++ window owns the
// Objective-C objects, never the other way round.
// ---------------------------------------------------------------------------

@interface NBContentView : NSView
@property(nonatomic, assign) nativebridge::detail::CocoaWindow* owner;
@end

@interface NBWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) nativebridge::detail::CocoaWindow* owner;
@end

namespace nativebridge::detail {
namespace {

struct MacKeyEntry {
    uint16_t virtualKey;
    KeyCode  key;
};

// kVK_* values from <HIToolbox/Events.h>, repeated literally to avoid a Carbon
// dependency. Virtual codes follow physical ANSI positions, which is exactly the
// layout independence nativeBridge wants.
constexpr MacKeyEntry kMacKeyTable[] = {
    {0, KeyCode::A},            {1, KeyCode::S},             {2, KeyCode::D},
    {3, KeyCode::F},            {4, KeyCode::H},             {5, KeyCode::G},
    {6, KeyCode::Z},            {7, KeyCode::X},             {8, KeyCode::C},
    {9, KeyCode::V},            {11, KeyCode::B},            {12, KeyCode::Q},
    {13, KeyCode::W},           {14, KeyCode::E},            {15, KeyCode::R},
    {16, KeyCode::Y},           {17, KeyCode::T},            {18, KeyCode::Digit1},
    {19, KeyCode::Digit2},      {20, KeyCode::Digit3},       {21, KeyCode::Digit4},
    {22, KeyCode::Digit6},      {23, KeyCode::Digit5},       {24, KeyCode::Equal},
    {25, KeyCode::Digit9},      {26, KeyCode::Digit7},       {27, KeyCode::Minus},
    {28, KeyCode::Digit8},      {29, KeyCode::Digit0},       {30, KeyCode::RightBracket},
    {31, KeyCode::O},           {32, KeyCode::U},            {33, KeyCode::LeftBracket},
    {34, KeyCode::I},           {35, KeyCode::P},            {36, KeyCode::Enter},
    {37, KeyCode::L},           {38, KeyCode::J},            {39, KeyCode::Apostrophe},
    {40, KeyCode::K},           {41, KeyCode::Semicolon},    {42, KeyCode::Backslash},
    {43, KeyCode::Comma},       {44, KeyCode::Slash},        {45, KeyCode::N},
    {46, KeyCode::M},           {47, KeyCode::Period},       {48, KeyCode::Tab},
    {49, KeyCode::Space},       {50, KeyCode::Grave},        {51, KeyCode::Backspace},
    {53, KeyCode::Escape},      {54, KeyCode::RightGui},     {55, KeyCode::LeftGui},
    {56, KeyCode::LeftShift},   {57, KeyCode::CapsLock},     {58, KeyCode::LeftAlt},
    {59, KeyCode::LeftControl}, {60, KeyCode::RightShift},   {61, KeyCode::RightAlt},
    {62, KeyCode::RightControl},{63, KeyCode::Application},  /* Fn */
    {64, KeyCode::F17},         {65, KeyCode::NumpadDecimal},{67, KeyCode::NumpadMultiply},
    {69, KeyCode::NumpadAdd},   {71, KeyCode::NumLock},      {75, KeyCode::NumpadDivide},
    {76, KeyCode::NumpadEnter}, {78, KeyCode::NumpadSubtract},{79, KeyCode::F18},
    {80, KeyCode::F19},         {81, KeyCode::NumpadEqual},  {82, KeyCode::Numpad0},
    {83, KeyCode::Numpad1},     {84, KeyCode::Numpad2},      {85, KeyCode::Numpad3},
    {86, KeyCode::Numpad4},     {87, KeyCode::Numpad5},      {88, KeyCode::Numpad6},
    {89, KeyCode::Numpad7},     {90, KeyCode::F20},          {91, KeyCode::Numpad8},
    {92, KeyCode::Numpad9},     {96, KeyCode::F5},           {97, KeyCode::F6},
    {98, KeyCode::F7},          {99, KeyCode::F3},           {100, KeyCode::F8},
    {101, KeyCode::F9},         {103, KeyCode::F11},         {105, KeyCode::F13},
    {106, KeyCode::F16},        {107, KeyCode::F14},         {109, KeyCode::F10},
    {111, KeyCode::F12},        {114, KeyCode::Insert},      {115, KeyCode::Home},
    {116, KeyCode::PageUp},     {117, KeyCode::Delete},      {118, KeyCode::F4},
    {119, KeyCode::End},        {120, KeyCode::F2},          {121, KeyCode::PageDown},
    {122, KeyCode::F1},         {123, KeyCode::ArrowLeft},   {124, KeyCode::ArrowRight},
    {125, KeyCode::ArrowDown},  {126, KeyCode::ArrowUp},
};

Modifier modifiersFromFlags(NSEventModifierFlags flags) noexcept {
    Modifier modifiers = Modifier::None;
    if ((flags & NSEventModifierFlagShift) != 0) { modifiers |= Modifier::Shift; }
    if ((flags & NSEventModifierFlagControl) != 0) { modifiers |= Modifier::Control; }
    if ((flags & NSEventModifierFlagOption) != 0) { modifiers |= Modifier::Alt; }
    if ((flags & NSEventModifierFlagCommand) != 0) { modifiers |= Modifier::Super; }
    if ((flags & NSEventModifierFlagCapsLock) != 0) { modifiers |= Modifier::CapsLock; }
    if ((flags & NSEventModifierFlagNumericPad) != 0) { modifiers |= Modifier::NumLock; }
    return modifiers;
}

std::string utf8FromString(NSString* text) {
    if (text == nil || text.length == 0) {
        return {};
    }
    const char* bytes = text.UTF8String;
    return bytes != nullptr ? std::string(bytes) : std::string{};
}

uint32_t firstScalar(NSString* text) {
    if (text == nil || text.length == 0) {
        return 0;
    }
    return static_cast<uint32_t>([text characterAtIndex:0]);
}

/// Height of the main display frame; the pivot of the Cocoa -> nativeBridge flip.
double gMainScreenHeight = 0.0;

void refreshCocoaFlipReference() {
    NSScreen* mainScreen = [NSScreen mainScreen];
    gMainScreenHeight = mainScreen != nil ? static_cast<double>(mainScreen.frame.size.height) : 0.0;
}

Rect cocoaFrameToDesktop(NSRect cocoaFrame) {
    return Rect{static_cast<int32_t>(std::lround(cocoaFrame.origin.x)),
                static_cast<int32_t>(std::lround(gMainScreenHeight - cocoaFrame.origin.y - cocoaFrame.size.height)),
                static_cast<int32_t>(std::lround(cocoaFrame.size.width)),
                static_cast<int32_t>(std::lround(cocoaFrame.size.height))};
}

NSRect desktopRectToCocoaFrame(Rect rect) {
    return NSMakeRect(static_cast<CGFloat>(rect.x),
                      static_cast<CGFloat>(gMainScreenHeight - static_cast<double>(rect.y) - rect.height),
                      static_cast<CGFloat>(rect.width),
                      static_cast<CGFloat>(rect.height));
}

NSCursor* cursorForShape(CursorShape shape) {
    switch (shape) {
        case CursorShape::IBeam:           return [NSCursor IBeamCursor];
        case CursorShape::Crosshair:       return [NSCursor crosshairCursor];
        case CursorShape::Hand:            return [NSCursor pointingHandCursor];
        case CursorShape::ResizeLeftRight: return [NSCursor resizeLeftRightCursor];
        case CursorShape::ResizeUpDown:    return [NSCursor resizeUpDownCursor];
        case CursorShape::ResizeAll:       return [NSCursor openHandCursor];
        case CursorShape::NotAllowed:      return [NSCursor operationNotAllowedCursor];
        case CursorShape::Hidden:          return [NSCursor arrowCursor];
        case CursorShape::Arrow:
        default:                           return [NSCursor arrowCursor];
    }
}

Event makeEvent(EventType type, WindowId id) {
    Event event{};
    event.type = type;
    event.window = id;
    event.timestampNs = monotonicNowNs();
    return event;
}

Status requireMainThread(std::thread::id uiThread, std::string_view operation) {
    if (std::this_thread::get_id() != uiThread) {
        return failStatus(Errc::PlatformFailure,
                          std::string(operation) +
                              ": AppKit must be used from the thread that created the Platform");
    }
    return okStatus();
}

} // namespace

KeyCode macKeyCodeFromVirtualKey(uint16_t virtualKey) noexcept {
    for (const auto& entry : kMacKeyTable) {
        if (entry.virtualKey == virtualKey) {
            return entry.key;
        }
    }
    return KeyCode::Unknown;
}

std::string_view macVirtualKeyName(uint16_t virtualKey) noexcept {
    return toString(macKeyCodeFromVirtualKey(virtualKey));
}

// ---------------------------------------------------------------------------
// CocoaWindow (translation unit local: other files only use the factories)
// ---------------------------------------------------------------------------

class CocoaWindow final : public WindowBackend {
public:
    CocoaWindow(const WeakLifetimeToken& lifetime, std::thread::id uiThread, WindowId id, EventQueue* queue,
                const WindowCreateInfo& info);

    ~CocoaWindow() override;

    CocoaWindow(const CocoaWindow&) = delete;
    CocoaWindow& operator=(const CocoaWindow&) = delete;

    [[nodiscard]] WindowId id() const noexcept override { return m_id; }
    [[nodiscard]] bool supports(Capability capability) const noexcept override;
    [[nodiscard]] std::string_view backendName() const noexcept override { return "cocoa"; }

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

    // --- Called from the Objective-C view/delegate (main thread) -----------
    void handleKeyEvent(NSEvent* event, bool isDown);
    void handleFlagsChanged(NSEvent* event);
    void handleMouseButton(NSEvent* event, bool isDown);
    void handleMouseMoved(NSEvent* event);
    void handleMouseEntered(bool entered);
    void handleScroll(NSEvent* event);
    void handleResized();
    void handleMoved();
    void handleFocusChanged(bool focused);
    void handleVisibilityChanged(bool visible);
    void handleMiniaturized(bool minimized);
    void handleZoomed(bool maximized);
    void handleFullscreen(bool fullscreen);
    void handleCloseRequested();
    void handleDrag(NSEvent* event);
    /// Emits ScaleFactorChanged and then Resized when the window's backing scale
    /// changed because it moved to another display. The scale event must precede
    /// the resize event (see include/nativebridge/event.h).
    void handleBackingScaleChanged();

    void syncGeometry();
    void syncScaleFactor();
    [[nodiscard]] double currentScale() const noexcept { return m_scale; }

private:
    void syncTrackingArea();
    void pushMouseMove(Point position, NSEvent* event);

    WeakLifetimeToken    m_lifetime; ///< Weak: expires with the owning Platform.
    std::thread::id      m_uiThread;
    WindowId             m_id{0};
    EventQueue*          m_queue{nullptr};
    double               m_scale{1.0};
    Size                 m_minSize{0, 0};
    Size                 m_maxSize{0, 0};
    CursorShape          m_cursorShape{CursorShape::Arrow};
    bool                 m_cursorVisible{true};
    bool                 m_decorations{true};
    bool                 m_resizable{true};
    bool                 m_dragging{false};
    Point                m_lastMouse{};
    bool                 m_hasLastMouse{false};
    std::optional<Rect>  m_clipRect{};
    bool                 m_destroyed{false};

    NSWindow* __strong         m_window{nil};
    NBContentView* __strong    m_view{nil};
    NBWindowDelegate* __strong m_delegate{nil};
    NSTrackingArea* __strong   m_trackingArea{nil};
    NSCursor* __strong         m_customCursor{nil};

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

CocoaWindow::CocoaWindow(const WeakLifetimeToken& lifetime, std::thread::id uiThread, WindowId id,
                         EventQueue* queue, const WindowCreateInfo& info)
    : m_lifetime(lifetime), m_uiThread(uiThread), m_id(id), m_queue(queue), m_minSize(info.minSize),
      m_maxSize(info.maxSize),
      m_cursorShape(info.cursor),
      m_decorations(!hasStyle(info.style, WindowStyle::NoDecoration)),
      m_resizable(info.resizable) {
    NSWindowStyleMask mask = NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (m_decorations) {
        mask |= NSWindowStyleMaskTitled;
    }
    if (m_resizable) {
        mask |= NSWindowStyleMaskResizable;
    }

    const NSRect contentRect = NSMakeRect(0.0, 0.0, static_cast<CGFloat>(info.clientSize.width),
                                          static_cast<CGFloat>(info.clientSize.height));
    m_window = [[NSWindow alloc] initWithContentRect:contentRect
                                           styleMask:mask
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    if (m_window == nil) {
        log::error("cocoa: NSWindow allocation failed");
        return;
    }
    m_window.releasedWhenClosed = NO;
    m_window.title = [NSString stringWithUTF8String:std::string(info.title).c_str()];
    m_window.acceptsMouseMovedEvents = YES;

    if (hasStyle(info.style, WindowStyle::AlwaysOnTop)) {
        m_window.level = NSFloatingWindowLevel;
    }
    if (hasStyle(info.style, WindowStyle::Transparent)) {
        m_window.opaque = NO;
        m_window.backgroundColor = [NSColor clearColor];
    }
    if (info.parent != 0) {
        m_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    }

    m_view = [[NBContentView alloc] initWithFrame:contentRect];
    m_view.owner = this;
    m_view.wantsLayer = YES;
    m_window.contentView = m_view;

    m_delegate = [[NBWindowDelegate alloc] init];
    m_delegate.owner = this;
    m_window.delegate = m_delegate;

    if (m_minSize.width > 0 || m_minSize.height > 0) {
        m_window.contentMinSize = NSMakeSize(static_cast<CGFloat>(m_minSize.width),
                                             static_cast<CGFloat>(m_minSize.height));
    }
    if (m_maxSize.width > 0 || m_maxSize.height > 0) {
        m_window.contentMaxSize = NSMakeSize(static_cast<CGFloat>(m_maxSize.width),
                                             static_cast<CGFloat>(m_maxSize.height));
    }

    if (info.hasPosition) {
        const Rect wanted{info.position.x, info.position.y, info.clientSize.width, info.clientSize.height};
        [m_window setFrame:desktopRectToCocoaFrame(wanted) display:NO];
    } else if (info.centered) {
        [m_window center];
    }

    syncScaleFactor();
    syncGeometry();
    syncTrackingArea();

    if (info.visible) {
        [m_window makeKeyAndOrderFront:nil];
        m_visible.set(true);
    }
}

CocoaWindow::~CocoaWindow() {
    if (m_window != nil) {
        m_window.delegate = nil;
        m_view.owner = nullptr;
        m_delegate.owner = nullptr;
        [m_window orderOut:nil];
        [m_window close];
    }
    m_window = nil;
    m_view = nil;
    m_delegate = nil;
    m_trackingArea = nil;
    m_customCursor = nil;
    // Deregister before the object dies. notifyDestroyed() silently does
    // nothing when the Platform was destroyed first, which is exactly the
    // behaviour we need (no dangling pointer, no call into freed memory).
    notifyDestroyed(m_lifetime, m_id);
}

bool CocoaWindow::supports(Capability capability) const noexcept {
    switch (capability) {
        case Capability::ScreenCaptureLock:
            return false;
        default:
            return true;
    }
}

std::string CocoaWindow::title() const {
    if (m_window == nil) {
        return {};
    }
    return utf8FromString(m_window.title);
}

WindowNativeHandle CocoaWindow::nativeHandle() const {
    WindowNativeHandle handle{};
    if (m_window != nil) {
        handle.window = NativeHandle{NativeHandle::Kind::CocoaNSWindow,
                                     reinterpret_cast<uintptr_t>((__bridge void*)m_window)};
    }
    if (m_view != nil) {
        handle.view = NativeHandle{NativeHandle::Kind::CocoaNSView,
                                   reinterpret_cast<uintptr_t>((__bridge void*)m_view)};
    }
    return handle;
}

void CocoaWindow::syncGeometry() {
    if (m_window == nil) {
        return;
    }
    const NSRect contentRect = [NSWindow contentRectForFrameRect:m_window.frame
                                                    styleMask:m_window.styleMask];
    const NSRect frameRect = m_window.frame;
    m_clientSize.set(Size{static_cast<int32_t>(std::lround(contentRect.size.width)),
                          static_cast<int32_t>(std::lround(contentRect.size.height))});
    m_windowSize.set(Size{static_cast<int32_t>(std::lround(frameRect.size.width)),
                          static_cast<int32_t>(std::lround(frameRect.size.height))});
    m_position.set(cocoaFrameToDesktop(frameRect).origin());
}

void CocoaWindow::syncScaleFactor() {
    if (m_window == nil) {
        return;
    }
    const double scale = m_window.backingScaleFactor > 0.0 ? m_window.backingScaleFactor : 1.0;
    m_scale = scale;
    m_scaleFactor.set(scale);
    m_cursor.set(m_cursorShape);
}

void CocoaWindow::syncTrackingArea() {
    if (m_view == nil) {
        return;
    }
    if (m_trackingArea != nil) {
        [m_view removeTrackingArea:m_trackingArea];
        m_trackingArea = nil;
    }
    m_trackingArea = [[NSTrackingArea alloc] initWithRect:m_view.bounds
                                                 options:(NSTrackingMouseEnteredAndExited |
                                                          NSTrackingMouseMoved |
                                                          NSTrackingActiveInKeyWindow |
                                                          NSTrackingInVisibleRect)
                                                   owner:m_view
                                                userInfo:nil];
    [m_view addTrackingArea:m_trackingArea];
}

// --- Input events ----------------------------------------------------------

void CocoaWindow::handleKeyEvent(NSEvent* event, bool isDown) {
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    KeyEvent payload{};
    payload.key = macKeyCodeFromVirtualKey(event.keyCode);
    payload.nativeKeyCode = event.keyCode;
    payload.nativeScanCode = event.keyCode;
    payload.modifiers = modifiersFromFlags(event.modifierFlags);
    payload.isRepeat = event.isARepeat;
    payload.action = !isDown ? KeyAction::Release : (event.isARepeat ? KeyAction::Repeat : KeyAction::Press);
    payload.character = isDown ? firstScalar(event.characters) : 0;

    Event keyEvent = makeEvent(isDown ? EventType::KeyDown : EventType::KeyUp, m_id);
    keyEvent.payload = payload;
    m_queue->push(std::move(keyEvent));

    if (isDown && !event.isARepeat) {
        NSString* characters = event.characters;
        if (characters != nil && characters.length > 0) {
            const unichar first = [characters characterAtIndex:0];
            const bool isControl = first < 0x20 || first == 0x7F;
            if (!isControl) {
                TextInputEvent text{};
                text.utf8 = utf8FromString(characters);
                if (!text.utf8.empty()) {
                    Event textEvent = makeEvent(EventType::TextInput, m_id);
                    textEvent.payload = std::move(text);
                    m_queue->push(std::move(textEvent));
                }
            }
        }
    }
}

void CocoaWindow::handleFlagsChanged(NSEvent* event) {
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    // Modifier keys generate neither keyDown nor keyUp; synthesise the action
    // from the resulting modifier state.
    const Modifier modifiers = modifiersFromFlags(event.modifierFlags);
    const KeyCode key = macKeyCodeFromVirtualKey(event.keyCode);
    const bool pressed = [&] {
        switch (key) {
            case KeyCode::LeftShift:
            case KeyCode::RightShift:   return hasModifier(modifiers, Modifier::Shift);
            case KeyCode::LeftControl:
            case KeyCode::RightControl: return hasModifier(modifiers, Modifier::Control);
            case KeyCode::LeftAlt:
            case KeyCode::RightAlt:     return hasModifier(modifiers, Modifier::Alt);
            case KeyCode::LeftGui:
            case KeyCode::RightGui:     return hasModifier(modifiers, Modifier::Super);
            case KeyCode::CapsLock:     return hasModifier(modifiers, Modifier::CapsLock);
            default:                    return false;
        }
    }();

    KeyEvent payload{};
    payload.key = key;
    payload.nativeKeyCode = event.keyCode;
    payload.nativeScanCode = event.keyCode;
    payload.modifiers = modifiers;
    payload.action = pressed ? KeyAction::Press : KeyAction::Release;

    Event keyEvent = makeEvent(pressed ? EventType::KeyDown : EventType::KeyUp, m_id);
    keyEvent.payload = payload;
    m_queue->push(std::move(keyEvent));
}

void CocoaWindow::handleMouseButton(NSEvent* event, bool isDown) {
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    MouseButton button = MouseButton::Unknown;
    switch (event.buttonNumber) {
        case 0: button = MouseButton::Left; break;
        case 1: button = MouseButton::Right; break;
        case 2: button = MouseButton::Middle; break;
        case 3: button = MouseButton::X1; break;
        case 4: button = MouseButton::X2; break;
        default: break;
    }
    m_dragging = isDown;
    const NSPoint local = m_view != nil ? [m_view convertPoint:event.locationInWindow fromView:nil] : NSZeroPoint;

    MouseButtonEvent payload{};
    payload.button = button;
    payload.position = Point{static_cast<int32_t>(std::lround(local.x)),
                             static_cast<int32_t>(std::lround(local.y))};
    payload.modifiers = modifiersFromFlags(event.modifierFlags);
    payload.clickCount = static_cast<uint8_t>(std::max<NSInteger>(1, event.clickCount));

    Event mouseEvent = makeEvent(isDown ? EventType::MouseDown : EventType::MouseUp, m_id);
    mouseEvent.payload = payload;
    m_queue->push(std::move(mouseEvent));
    m_lastMouse = payload.position;
    m_hasLastMouse = true;
}

void CocoaWindow::pushMouseMove(Point position, NSEvent* event) {
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    MouseMoveEvent payload{};
    payload.position = position;
    payload.delta = m_hasLastMouse ? Point{position.x - m_lastMouse.x, position.y - m_lastMouse.y} : Point{0, 0};
    payload.modifiers = modifiersFromFlags(event != nil ? event.modifierFlags : 0);
    payload.isDragging = m_dragging;
    m_lastMouse = position;
    m_hasLastMouse = true;

    Event moveEvent = makeEvent(EventType::MouseMoved, m_id);
    moveEvent.payload = payload;
    m_queue->push(std::move(moveEvent));
}

void CocoaWindow::handleMouseMoved(NSEvent* event) {
    if (m_view == nil) {
        return;
    }
    const NSPoint local = [m_view convertPoint:event.locationInWindow fromView:nil];
    pushMouseMove(Point{static_cast<int32_t>(std::lround(local.x)), static_cast<int32_t>(std::lround(local.y))}, event);
}

void CocoaWindow::handleDrag(NSEvent* event) {
    if (m_view == nil || m_queue == nullptr || m_destroyed) {
        return;
    }
    const NSPoint local = [m_view convertPoint:event.locationInWindow fromView:nil];
    m_dragging = true;
    pushMouseMove(Point{static_cast<int32_t>(std::lround(local.x)), static_cast<int32_t>(std::lround(local.y))}, event);
}

void CocoaWindow::handleBackingScaleChanged() {
    const double previous = m_scale;
    syncScaleFactor();
    if (m_scale == previous) {
        return;
    }
    if (m_queue != nullptr && !m_destroyed) {
        ScaleFactorEvent payload{};
        payload.scaleFactor = m_scale;
        Event event = makeEvent(EventType::ScaleFactorChanged, m_id);
        event.payload = payload;
        m_queue->push(std::move(event));
    }
    handleResized();
}

void CocoaWindow::handleMouseEntered(bool entered) {
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    MouseEnterEvent payload{};
    payload.position = m_lastMouse;
    payload.entered = entered;
    Event event = makeEvent(entered ? EventType::MouseEntered : EventType::MouseLeft, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));
    if (m_cursorVisible) {
        [cursorForShape(m_cursorShape) set];
    }
}

void CocoaWindow::handleScroll(NSEvent* event) {
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    const NSPoint local = m_view != nil ? [m_view convertPoint:event.locationInWindow fromView:nil] : NSZeroPoint;

    MouseWheelEvent payload{};
    payload.position = Point{static_cast<int32_t>(std::lround(local.x)),
                             static_cast<int32_t>(std::lround(local.y))};
    // AppKit reports positive deltas when the content should move down for a
    // natural (inverted) scroll gesture; nativeBridge defines positive as
    // "content moves up/right", hence the sign flip.
    payload.deltaY = -static_cast<double>(event.scrollingDeltaY);
    payload.deltaX = -static_cast<double>(event.scrollingDeltaX);
    payload.precise = event.hasPreciseScrollingDeltas;
    payload.modifiers = modifiersFromFlags(event.modifierFlags);

    Event wheelEvent = makeEvent(EventType::MouseWheel, m_id);
    wheelEvent.payload = payload;
    m_queue->push(std::move(wheelEvent));
}

// --- Window events ---------------------------------------------------------

void CocoaWindow::handleResized() {
    const Size before = m_clientSize.get();
    syncScaleFactor();
    syncGeometry();
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    ResizeEvent payload{};
    payload.clientSize = m_clientSize.get();
    payload.windowSize = m_windowSize.get();
    payload.framebufferSize = framebufferSize();
    payload.inLiveResize = m_window != nil && m_window.inLiveResize;
    Event event = makeEvent(EventType::Resized, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));

    if (before.width != payload.clientSize.width || before.height != payload.clientSize.height) {
        syncTrackingArea();
    }
}

void CocoaWindow::handleMoved() {
    syncGeometry();
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    MoveEvent payload{};
    payload.position = m_position.get();
    payload.globalPositionKnown = true;
    Event event = makeEvent(EventType::Moved, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));
}

void CocoaWindow::handleFocusChanged(bool focused) {
    m_focused.set(focused);
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    FocusEvent payload{};
    payload.focused = focused;
    Event event = makeEvent(focused ? EventType::FocusGained : EventType::FocusLost, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));
}

void CocoaWindow::handleVisibilityChanged(bool visible) {
    m_visible.set(visible);
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    VisibilityEvent payload{};
    payload.visible = visible;
    Event event = makeEvent(visible ? EventType::Shown : EventType::Hidden, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));
}

void CocoaWindow::handleMiniaturized(bool minimized) {
    m_minimized.set(minimized);
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    StateEvent payload{};
    payload.active = minimized;
    Event event = makeEvent(minimized ? EventType::Minimized : EventType::Restored, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));
}

void CocoaWindow::handleZoomed(bool maximized) {
    m_maximized.set(maximized);
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    StateEvent payload{};
    payload.active = maximized;
    Event event = makeEvent(maximized ? EventType::Maximized : EventType::Restored, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));
}

void CocoaWindow::handleFullscreen(bool fullscreen) {
    m_fullscreen.set(fullscreen);
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    StateEvent payload{};
    payload.active = fullscreen;
    Event event = makeEvent(fullscreen ? EventType::Maximized : EventType::Restored, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));
    handleResized();
}

void CocoaWindow::handleCloseRequested() {
    if (m_queue == nullptr || m_destroyed) {
        return;
    }
    CloseRequestedEvent payload{};
    payload.cancellable = true;
    Event event = makeEvent(EventType::CloseRequested, m_id);
    event.payload = payload;
    m_queue->push(std::move(event));
}

// --- Commands --------------------------------------------------------------

Status CocoaWindow::setTitle(std::string_view title) {
    if (auto status = requireMainThread(m_uiThread, "setTitle"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "setTitle: window is gone");
    }
    m_window.title = [NSString stringWithUTF8String:std::string(title).c_str()];
    return okStatus();
}

Status CocoaWindow::resize(Size clientSize) {
    if (auto status = requireMainThread(m_uiThread, "resize"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "resize: window is gone");
    }
    if (clientSize.width <= 0 || clientSize.height <= 0) {
        return failStatus(Errc::InvalidArgument, "resize: client size must be positive");
    }
    NSRect frame = m_window.frame;
    const NSRect contentRect = [m_window contentRectForFrameRect:frame];
    frame.size.width += static_cast<CGFloat>(clientSize.width) - contentRect.size.width;
    frame.size.height += static_cast<CGFloat>(clientSize.height) - contentRect.size.height;
    [m_window setFrame:frame display:YES];
    handleResized();
    return okStatus();
}

Status CocoaWindow::move(Point position) {
    if (auto status = requireMainThread(m_uiThread, "move"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "move: window is gone");
    }
    const Rect current = cocoaFrameToDesktop(m_window.frame);
    const Rect wanted{position.x, position.y, current.width, current.height};
    [m_window setFrame:desktopRectToCocoaFrame(wanted) display:YES];
    handleMoved();
    return okStatus();
}

Status CocoaWindow::setMinSize(Size size) {
    if (auto status = requireMainThread(m_uiThread, "setMinSize"); !status) {
        return status;
    }
    m_minSize = size;
    if (m_window != nil) {
        m_window.contentMinSize = NSMakeSize(static_cast<CGFloat>(size.width), static_cast<CGFloat>(size.height));
    }
    return okStatus();
}

Status CocoaWindow::setMaxSize(Size size) {
    if (auto status = requireMainThread(m_uiThread, "setMaxSize"); !status) {
        return status;
    }
    m_maxSize = size;
    if (m_window != nil) {
        m_window.contentMaxSize = (size.width <= 0 && size.height <= 0)
                                     ? NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)
                                     : NSMakeSize(static_cast<CGFloat>(size.width), static_cast<CGFloat>(size.height));
    }
    return okStatus();
}

Status CocoaWindow::show() {
    if (auto status = requireMainThread(m_uiThread, "show"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "show: window is gone");
    }
    [m_window makeKeyAndOrderFront:nil];
    m_visible.set(true);
    return okStatus();
}

Status CocoaWindow::hide() {
    if (auto status = requireMainThread(m_uiThread, "hide"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "hide: window is gone");
    }
    [m_window orderOut:nil];
    m_visible.set(false);
    return okStatus();
}

Status CocoaWindow::minimize() {
    if (auto status = requireMainThread(m_uiThread, "minimize"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "minimize: window is gone");
    }
    [m_window miniaturize:nil];
    return okStatus();
}

Status CocoaWindow::maximize() {
    if (auto status = requireMainThread(m_uiThread, "maximize"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "maximize: window is gone");
    }
    if (!m_window.zoomed) {
        [m_window zoom:nil];
    }
    return okStatus();
}

Status CocoaWindow::restore() {
    if (auto status = requireMainThread(m_uiThread, "restore"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "restore: window is gone");
    }
    if (m_window.miniaturized) {
        [m_window deminiaturize:nil];
    }
    if (m_window.zoomed) {
        [m_window zoom:nil];
    }
    return okStatus();
}

Status CocoaWindow::setFullscreen(bool fullscreen) {
    if (auto status = requireMainThread(m_uiThread, "setFullscreen"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "setFullscreen: window is gone");
    }
    const bool isFullscreen = (m_window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (isFullscreen == fullscreen) {
        return okStatus();
    }
    [m_window toggleFullScreen:nil];
    return okStatus();
}

Status CocoaWindow::setAlwaysOnTop(bool onTop) {
    if (auto status = requireMainThread(m_uiThread, "setAlwaysOnTop"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "setAlwaysOnTop: window is gone");
    }
    m_window.level = onTop ? NSFloatingWindowLevel : NSNormalWindowLevel;
    return okStatus();
}

Status CocoaWindow::setResizable(bool resizable) {
    if (auto status = requireMainThread(m_uiThread, "setResizable"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "setResizable: window is gone");
    }
    NSWindowStyleMask mask = m_window.styleMask;
    if (resizable) {
        mask |= NSWindowStyleMaskResizable;
    } else {
        mask &= ~NSWindowStyleMaskResizable;
    }
    m_window.styleMask = mask;
    m_resizable = resizable;
    return okStatus();
}

Status CocoaWindow::setDecorations(bool decorations) {
    if (auto status = requireMainThread(m_uiThread, "setDecorations"); !status) {
        return status;
    }
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "setDecorations: window is gone");
    }
    NSWindowStyleMask mask = m_window.styleMask;
    if (decorations) {
        mask |= NSWindowStyleMaskTitled;
    } else {
        mask &= ~NSWindowStyleMaskTitled;
    }
    m_window.styleMask = mask;
    m_decorations = decorations;
    return okStatus();
}

Status CocoaWindow::setCursor(CursorShape shape) {
    if (auto status = requireMainThread(m_uiThread, "setCursor"); !status) {
        return status;
    }
    m_cursorShape = shape;
    m_cursor.set(shape);
    if (m_cursorVisible) {
        [cursorForShape(shape) set];
    }
    return okStatus();
}

Status CocoaWindow::setCursorVisible(bool visible) {
    if (auto status = requireMainThread(m_uiThread, "setCursorVisible"); !status) {
        return status;
    }
    m_cursorVisible = visible;
    if (visible) {
        [NSCursor unhide];
        [cursorForShape(m_cursorShape) set];
    } else {
        [NSCursor hide];
    }
    return okStatus();
}

Status CocoaWindow::setClipRect(const std::optional<Rect>& rect) {
    m_clipRect = rect; // Recorded for the renderer; AppKit has no portable clip.
    return okStatus();
}

Status CocoaWindow::requestAttention() {
    if (auto status = requireMainThread(m_uiThread, "requestAttention"); !status) {
        return status;
    }
    [NSApp requestUserAttention:NSCriticalRequest];
    return okStatus();
}

Status CocoaWindow::requestClose() {
    if (m_window == nil) {
        return failStatus(Errc::InvalidArgument, "requestClose: window is gone");
    }
    if (auto status = requireMainThread(m_uiThread, "requestClose"); !status) {
        return status;
    }
    // Same ordering as a click on the close button: the delegate is asked first.
    [m_window performClose:nil];
    return okStatus();
}

Status CocoaWindow::setCustomCursor(std::span<const uint8_t> bgraPixels, Size size, Point hotspot) {
    if (auto status = requireMainThread(m_uiThread, "setCustomCursor"); !status) {
        return status;
    }
    const std::size_t expected = static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) * 4u;
    if (size.width <= 0 || size.height <= 0 || bgraPixels.size() < expected) {
        return failStatus(Errc::InvalidArgument, "setCustomCursor: buffer must hold size.width*size.height*4 BGRA bytes");
    }
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                    pixelsWide:size.width
                                                                    pixelsHigh:size.height
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:size.width * 4
                                                                  bitsPerPixel:32];
    if (rep == nil || rep.bitmapData == nullptr) {
        return failStatus(Errc::OutOfMemory, "setCustomCursor: bitmap allocation failed");
    }
    std::memcpy(rep.bitmapData, bgraPixels.data(), expected);

    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(size.width, size.height)];
    [image addRepresentation:rep];
    m_customCursor = [[NSCursor alloc] initWithImage:image
                                            hotSpot:NSMakePoint(static_cast<CGFloat>(hotspot.x),
                                                                static_cast<CGFloat>(hotspot.y))];
    if (m_customCursor == nil) {
        return failStatus(Errc::PlatformFailure, "setCustomCursor: NSCursor creation failed");
    }
    [m_customCursor set];
    return okStatus();
}

Status CocoaWindow::materializeSurface(RenderApi api, std::string_view backendName, NativeHandle& surface) {
    (void)backendName;
    if (m_view == nil) {
        return failStatus(Errc::InvalidArgument, "materializeSurface: window is gone");
    }
    switch (api) {
        case RenderApi::Metal: {
            // Metal renders into the view's layer (CAMetalLayer by default).
            m_view.wantsLayer = YES;
            surface = NativeHandle{NativeHandle::Kind::CocoaNSView,
                                   reinterpret_cast<uintptr_t>((__bridge void*)m_view.layer)};
            return okStatus();
        }
        case RenderApi::Direct3D11:
        case RenderApi::Direct3D12:
            return failStatus(Errc::Unsupported, "materializeSurface: Direct3D is not available on macOS");
        default:
            // OpenGL/Vulkan/WebGPU on macOS consume the NSView directly.
            surface = NativeHandle{};
            return okStatus();
    }
}

void CocoaWindow::onSurfaceResized() { syncGeometry(); }

void CocoaWindow::onDisplayConfigurationChanged() {
    // The monitor set changed: the window may now be on a display with another
    // scale factor. ScaleFactorChanged must precede Resized (see event.h).
    handleBackingScaleChanged();
    handleMoved();
}

void CocoaWindow::onNativeDestroyed() {
    m_destroyed = true;
    m_queue = nullptr;
}

// ---------------------------------------------------------------------------
// Factory and shared helpers
// ---------------------------------------------------------------------------

Result<std::unique_ptr<WindowBackend>> createCocoaWindow(const WeakLifetimeToken& lifetime, std::thread::id uiThread,
                                                        WindowId id, EventQueue* queue,
                                                        const WindowCreateInfo& info) {
    auto window = std::make_unique<CocoaWindow>(lifetime, uiThread, id, queue, info);
    if (!window->nativeHandle().window.valid()) {
        return fail<std::unique_ptr<WindowBackend>>(
            Error{Errc::PlatformFailure, "createWindow: NSWindow creation failed"});
    }
    return std::unique_ptr<WindowBackend>(window.release());
}

void nativebridgeRefreshCocoaFlipReference() { refreshCocoaFlipReference(); }

Rect cocoaFrameToDesktopRect(double x, double y, double width, double height) {
    return cocoaFrameToDesktop(NSMakeRect(x, y, width, height));
}

} // namespace

// ---------------------------------------------------------------------------
// Objective-C implementations. The view owns no C++ state; it forwards into the
// CocoaWindow which owns it (the reverse of the usual retain graph, which keeps
// the C++ object the single owner of every native resource).
// ---------------------------------------------------------------------------

using nativebridge::detail::CocoaWindow;

@implementation NBContentView

- (BOOL)isFlipped {
    return YES; // Top-left origin, so client coordinates need no conversion.
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES; // Click-through: the activating click also reaches the app.
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window != nil) {
        [self.window makeFirstResponder:self];
    }
}

- (void)keyDown:(NSEvent*)event {
    if (self.owner != nullptr) {
        self.owner->handleKeyEvent(event, true);
    }
}

- (void)keyUp:(NSEvent*)event {
    if (self.owner != nullptr) {
        self.owner->handleKeyEvent(event, false);
    }
}

- (void)flagsChanged:(NSEvent*)event {
    if (self.owner != nullptr) {
        self.owner->handleFlagsChanged(event);
    }
}

- (void)mouseDown:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleMouseButton(event, true); }
}

- (void)mouseUp:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleMouseButton(event, false); }
}

- (void)rightMouseDown:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleMouseButton(event, true); }
}

- (void)rightMouseUp:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleMouseButton(event, false); }
}

- (void)otherMouseDown:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleMouseButton(event, true); }
}

- (void)otherMouseUp:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleMouseButton(event, false); }
}

- (void)mouseMoved:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleMouseMoved(event); }
}

- (void)mouseDragged:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleDrag(event); }
}

- (void)rightMouseDragged:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleDrag(event); }
}

- (void)otherMouseDragged:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleDrag(event); }
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    if (self.owner != nullptr) { self.owner->handleMouseEntered(true); }
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (self.owner != nullptr) { self.owner->handleMouseEntered(false); }
}

- (void)scrollWheel:(NSEvent*)event {
    if (self.owner != nullptr) { self.owner->handleScroll(event); }
}

@end

@implementation NBWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (self.owner != nullptr) {
        self.owner->handleCloseRequested();
    }
    // nativeBridge reports the request instead of deciding: the application
    // closes the window (destroy it, or call Window::requestClose()), so the
    // default here is to keep the window alive.
    return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) { self.owner->handleResized(); }
}

- (void)windowDidMove:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) { self.owner->handleMoved(); }
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) { self.owner->handleFocusChanged(true); }
}

- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) { self.owner->handleFocusChanged(false); }
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) { self.owner->handleMiniaturized(true); }
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) { self.owner->handleMiniaturized(false); }
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification {
    NSWindow* window = notification.object;
    if (self.owner != nullptr && window != nil) {
        self.owner->handleVisibilityChanged((window.occlusionState & NSWindowOcclusionStateVisible) != 0);
    }
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) { self.owner->handleFullscreen(true); }
}

- (void)windowDidExitFullScreen:(NSNotification*)notification {
    (void)notification;
    if (self.owner != nullptr) { self.owner->handleFullscreen(false); }
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    if (self.owner == nullptr) {
        return;
    }
    // Fires when the window moves to a display with a different scale factor:
    // the window emits ScaleFactorChanged and then Resized itself.
    self.owner->handleBackingScaleChanged();
}

@end

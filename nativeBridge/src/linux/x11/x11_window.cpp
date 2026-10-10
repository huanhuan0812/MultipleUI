// nativeBridge — Linux/X11 backend: window, input and event translation.
// SPDX-License-Identifier: MIT
//
// Coordinate conversion: X11 works in physical pixels while the public API is
// logical. Every value that crosses the boundary is divided (Xlib -> public) or
// multiplied (public -> Xlib) by the platform scale factor, so
//
//     physical_pixel = logical * window.scaleFactor()
//
// holds for this backend exactly as documented in include/nativebridge/event.h.

#include "linux/x11/x11_backend.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/select.h>
#include <string>
#include <utility>
#include <vector>

#include "nativebridge/window.h"

using namespace nativebridge;

namespace nativebridge::detail {
namespace {

Event makeEvent(EventType type, WindowId id) {
    Event event{};
    event.type = type;
    event.window = id;
    event.timestampNs = monotonicNowNs();
    return event;
}

/// Cursor font shape for a CursorShape. CursorShape::Hidden is handled by
/// createInvisibleCursor() because cursorfont.h has no blank bitmap.
unsigned int xCursorShape(CursorShape shape) {
    switch (shape) {
        case CursorShape::IBeam:           return XC_xterm;
        case CursorShape::Crosshair:       return XC_crosshair;
        case CursorShape::Hand:            return XC_hand2;
        case CursorShape::ResizeLeftRight: return XC_sb_h_double_arrow;
        case CursorShape::ResizeUpDown:    return XC_sb_v_double_arrow;
        case CursorShape::ResizeAll:       return XC_fleur;
        case CursorShape::NotAllowed:      return XC_X_cursor;
        case CursorShape::Hidden:
        case CursorShape::Arrow:
        default:                           return XC_left_ptr;
    }
}

/// Fully transparent 1x1 cursor, the standard way to hide the pointer on X11.
::Cursor createInvisibleCursor(Display* display, ::Window root) {
    static const char empty[1] = {0};
    Pixmap bitmap = ::XCreateBitmapFromData(display, root, empty, 1, 1);
    if (bitmap == 0) {
        return 0;
    }
    XColor black{};
    ::Cursor cursor = ::XCreatePixmapCursor(display, bitmap, bitmap, &black, &black, 0, 0);
    ::XFreePixmap(display, bitmap);
    return cursor;
}

/// X11 mouse buttons 4..7 are wheel events, not buttons.
bool isWheelButton(unsigned int button) { return button >= 4 && button <= 7; }

MouseButton mouseButtonFor(unsigned int button) {
    switch (button) {
        case Button1: return MouseButton::Left;
        case Button2: return MouseButton::Middle;
        case Button3: return MouseButton::Right;
        case 8:       return MouseButton::X1;
        case 9:       return MouseButton::X2;
        default:      return MouseButton::Unknown;
    }
}

Modifier modifiersFromState(unsigned int state) {
    Modifier modifiers = Modifier::None;
    if ((state & ShiftMask) != 0) { modifiers |= Modifier::Shift; }
    if ((state & ControlMask) != 0) { modifiers |= Modifier::Control; }
    if ((state & Mod1Mask) != 0) { modifiers |= Modifier::Alt; }
    if ((state & Mod4Mask) != 0) { modifiers |= Modifier::Super; }
    if ((state & LockMask) != 0) { modifiers |= Modifier::CapsLock; }
    if ((state & Mod2Mask) != 0) { modifiers |= Modifier::NumLock; }
    return modifiers;
}

int32_t toLogical(int pixels, double scale) {
    return static_cast<int32_t>(std::lround(static_cast<double>(pixels) / (scale > 0.0 ? scale : 1.0)));
}

int toPhysical(int32_t logical, double scale) {
    return static_cast<int>(std::lround(static_cast<double>(logical) * (scale > 0.0 ? scale : 1.0)));
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

X11Window::X11Window(X11Platform& platform, WindowId id, const WindowCreateInfo& info)
    : m_platform(platform), m_id(id), m_minSize(info.minSize), m_maxSize(info.maxSize),
      m_cursorShape(info.cursor), m_decorations(!hasStyle(info.style, WindowStyle::NoDecoration)),
      m_resizable(info.resizable) {
    Display* display = m_platform.display();
    if (display == nullptr) {
        return;
    }
    m_scale = m_platform.scaleFactor();

    XSetWindowAttributes attributes{};
    attributes.background_pixmap = 0;
    attributes.border_pixel = 0;
    attributes.event_mask = 0; // selected per window below
    const unsigned long mask = CWBackPixmap | CWBorderPixel | CWEventMask;

    m_window = ::XCreateWindow(display, m_platform.root(),
                               toPhysical(info.position.x, m_scale), toPhysical(info.position.y, m_scale),
                               static_cast<unsigned int>(toPhysical(info.clientSize.width, m_scale)),
                               static_cast<unsigned int>(toPhysical(info.clientSize.height, m_scale)),
                               0, CopyFromParent, InputOutput, CopyFromParent, mask, &attributes);
    if (m_window == 0) {
        log::error("x11: XCreateWindow failed");
        return;
    }
    m_ownWindow = true;

    // The window is the client area; the window manager draws the decoration.
    // A transparent "wrapper" is not needed, so wrapper() == window().
    m_wrapper = m_window;
    selectInput();

    (void)setTitle(info.title);
    if (m_minSize.width > 0 || m_minSize.height > 0 || m_maxSize.width > 0 || m_maxSize.height > 0) {
        updateSizeHints();
    }
    if (!m_resizable) {
        XSizeHints hints{};
        hints.flags = PMinSize | PMaxSize;
        hints.min_width = hints.max_width = toPhysical(info.clientSize.width, m_scale);
        hints.min_height = hints.max_height = toPhysical(info.clientSize.height, m_scale);
        ::XSetWMNormalHints(display, m_window, &hints);
    }
    if (hasStyle(info.style, WindowStyle::AlwaysOnTop)) {
        (void)setAlwaysOnTop(true);
    }
    if (hasStyle(info.style, WindowStyle::NoDecoration)) {
        (void)setDecorations(false);
    }

    // Take part in the WM_DELETE_WINDOW protocol.
    ::Atom protocols[1] = {m_platform.wmDeleteWindow()};
    (void)::XSetWMProtocols(display, m_window, protocols, 1);

    // Window class hint (used by task bars and window lists).
    XClassHint classHint{};
    classHint.res_name = const_cast<char*>("nativebridge");
    classHint.res_class = const_cast<char*>("NativeBridge");
    (void)::XSetClassHint(display, m_window, &classHint);

    // Input method context for committed text (may be null without an IM server).
    if (XIM im = ::XOpenIM(display, nullptr, nullptr, nullptr); im != nullptr) {
        m_ic = ::XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                           XNClientWindow, m_window, XNFocusWindow, m_window, nullptr);
    }

    syncGeometry();
    m_scaleFactor.set(m_scale);

    if (info.visible) {
        (void)show();
    }
}

X11Window::~X11Window() {
    Display* display = m_platform.display();
    if (m_customCursor != 0 && display != nullptr) {
        ::XFreeCursor(display, m_customCursor);
        m_customCursor = 0;
    }
    if (m_ic != nullptr) {
        ::XDestroyIC(m_ic);
        m_ic = nullptr;
    }
    if (m_ownWindow && m_window != 0 && display != nullptr) {
        ::XDestroyWindow(display, m_window);
        ::XFlush(display);
    }
    m_window = 0;
    notifyDestroyed(m_platform.lifetime(), m_id);
}

void X11Window::selectInput() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return;
    }
    const long events = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                        PointerMotionMask | EnterWindowMask | LeaveWindowMask | ExposureMask |
                        StructureNotifyMask | FocusChangeMask | PropertyChangeMask | VisibilityChangeMask;
    (void)::XSelectInput(display, m_window, events);
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

void X11Window::handleEvent(XEvent& event) {
    Display* display = m_platform.display();
    if (display == nullptr) {
        return;
    }

    switch (event.type) {
        case ConfigureNotify: {
            if (event.xconfigure.window != m_window) {
                break; // a synthetic event for the frame; ignore
            }
            const Size before = m_clientSize.get();
            syncGeometry();
            ResizeEvent payload{};
            payload.clientSize = m_clientSize.get();
            payload.windowSize = m_windowSize.get();
            payload.framebufferSize = framebufferSize();
            payload.inLiveResize = false;
            if (before.width != payload.clientSize.width || before.height != payload.clientSize.height) {
                Event resize = makeEvent(EventType::Resized, m_id);
                resize.payload = payload;
                m_platform.queue().push(std::move(resize));
            }
            MoveEvent movePayload{};
            movePayload.position = m_position.get();
            movePayload.globalPositionKnown = true;
            Event move = makeEvent(EventType::Moved, m_id);
            move.payload = movePayload;
            m_platform.queue().push(std::move(move));
            break;
        }

        case ClientMessage: {
            const ::Atom protocol = static_cast<::Atom>(event.xclient.data.l[0]);
            if (protocol == m_platform.wmDeleteWindow()) {
                CloseRequestedEvent payload{};
                payload.cancellable = true;
                Event close = makeEvent(EventType::CloseRequested, m_id);
                close.payload = payload;
                m_platform.queue().push(std::move(close));
            }
            break;
        }

        case KeyPress:
        case KeyRelease: {
            const bool isDown = event.type == KeyPress;
            // Xlib auto-repeat arrives as a release/press pair with identical
            // timestamps; detect it and report a single Repeat instead.
            bool isRepeat = false;
            if (!isDown && ::XPending(display) > 0) {
                XEvent next{};
                ::XPeekEvent(display, &next);
                if (next.type == KeyPress && next.xkey.time == event.xkey.time &&
                    next.xkey.keycode == event.xkey.keycode) {
                    isRepeat = true;
                }
            }
            if (!isDown && isRepeat) {
                break; // the following KeyPress emits the Repeat event
            }

            KeySym keysym = NoSymbol;
            char buffer[32]{};
            int length = 0;
            int status = 0;
            if (m_ic != nullptr) {
                length = ::Xutf8LookupString(m_ic, &event.xkey, buffer, sizeof(buffer) - 1, &keysym, &status);
            } else {
                length = ::XLookupString(&event.xkey, buffer, sizeof(buffer) - 1, &keysym, nullptr);
            }
            if (keysym == NoSymbol) {
                keysym = ::XLookupKeysym(&event.xkey, 0);
            }

            KeyEvent payload{};
            payload.key = keyCodeFromKeysym(static_cast<uint32_t>(keysym));
            payload.nativeKeyCode = static_cast<uint32_t>(keysym);
            payload.nativeScanCode = event.xkey.keycode;
            payload.keysym = static_cast<uint32_t>(keysym);
            payload.modifiers = modifiersFromState(event.xkey.state);
            payload.isRepeat = isRepeat;
            payload.action = isRepeat ? KeyAction::Repeat : (isDown ? KeyAction::Press : KeyAction::Release);
            payload.character = characterFromKeysym(static_cast<uint32_t>(keysym));

            Event key = makeEvent(isDown ? EventType::KeyDown : EventType::KeyUp, m_id);
            key.payload = payload;
            m_platform.queue().push(std::move(key));

            // Committed text: Xutf8LookupString already returns UTF-8; XLookupString
            // returns the Latin-1 byte of the current keyboard mapping.
            if (isDown && length > 0) {
                std::string text(buffer, static_cast<std::size_t>(length));
                // Filter out control characters (they are reported as keys).
                const bool printable = std::any_of(text.begin(), text.end(), [](unsigned char c) {
                    return c >= 0x20 && c != 0x7F;
                });
                if (printable && keysym != XK_Return && keysym != XK_BackSpace && keysym != XK_Tab &&
                    keysym != XK_Escape) {
                    TextInputEvent textPayload{};
                    textPayload.utf8 = std::move(text);
                    Event textEvent = makeEvent(EventType::TextInput, m_id);
                    textEvent.payload = std::move(textPayload);
                    m_platform.queue().push(std::move(textEvent));
                }
            }
            break;
        }

        case ButtonPress:
        case ButtonRelease: {
            const bool isDown = event.type == ButtonPress;
            const unsigned int button = event.xbutton.button;
            const Point position{toLogical(event.xbutton.x, m_scale), toLogical(event.xbutton.y, m_scale)};

            if (isWheelButton(button)) {
                if (!isDown) {
                    break; // wheels only report the press
                }
                MouseWheelEvent payload{};
                payload.position = position;
                const double lines = static_cast<double>(m_platform.scrollLinesPerNotch());
                // X11 reports the wheel as buttons 4..7. X.h only names 4 and 5,
                // so the horizontal ones are written as literals.
                constexpr unsigned int kButton6 = 6; // horizontal wheel left
                constexpr unsigned int kButton7 = 7; // horizontal wheel right
                switch (button) {
                    case Button4:  payload.deltaY = lines; break;  // up
                    case Button5:  payload.deltaY = -lines; break; // down
                    case kButton6: payload.deltaX = -lines; break;
                    case kButton7: payload.deltaX = lines; break;
                    default: break;
                }
                payload.precise = false;
                payload.modifiers = modifiersFromState(event.xbutton.state);
                Event wheel = makeEvent(EventType::MouseWheel, m_id);
                wheel.payload = payload;
                m_platform.queue().push(std::move(wheel));
                break;
            }

            MouseButtonEvent payload{};
            payload.button = mouseButtonFor(button);
            payload.position = position;
            payload.modifiers = modifiersFromState(event.xbutton.state);
            payload.clickCount = 1;
            Event mouse = makeEvent(isDown ? EventType::MouseDown : EventType::MouseUp, m_id);
            mouse.payload = payload;
            m_platform.queue().push(std::move(mouse));

            if (isDown) {
                m_lastMouse = position;
                m_hasLastMouse = true;
            }
            break;
        }

        case MotionNotify: {
            const Point position{toLogical(event.xmotion.x, m_scale), toLogical(event.xmotion.y, m_scale)};
            MouseMoveEvent payload{};
            payload.position = position;
            payload.delta = m_hasLastMouse
                                ? Point{position.x - m_lastMouse.x, position.y - m_lastMouse.y}
                                : Point{0, 0};
            payload.modifiers = modifiersFromState(event.xmotion.state);
            const unsigned int dragging = Button1Mask | Button2Mask | Button3Mask;
            payload.isDragging = (event.xmotion.state & dragging) != 0;
            m_lastMouse = position;
            m_hasLastMouse = true;
            Event move = makeEvent(EventType::MouseMoved, m_id);
            move.payload = payload;
            m_platform.queue().push(std::move(move));
            break;
        }

        case EnterNotify: {
            m_trackingMouse = true;
            MouseEnterEvent payload{};
            payload.position = Point{toLogical(event.xcrossing.x, m_scale), toLogical(event.xcrossing.y, m_scale)};
            payload.entered = true;
            Event enter = makeEvent(EventType::MouseEntered, m_id);
            enter.payload = payload;
            m_platform.queue().push(std::move(enter));
            applyCursor();
            break;
        }

        case LeaveNotify: {
            m_trackingMouse = false;
            MouseEnterEvent payload{};
            payload.position = Point{toLogical(event.xcrossing.x, m_scale), toLogical(event.xcrossing.y, m_scale)};
            payload.entered = false;
            Event leave = makeEvent(EventType::MouseLeft, m_id);
            leave.payload = payload;
            m_platform.queue().push(std::move(leave));
            break;
        }

        case FocusIn: {
            m_focused.set(true);
            if (m_ic != nullptr) {
                ::XSetICFocus(m_ic);
            }
            FocusEvent payload{};
            payload.focused = true;
            Event focus = makeEvent(EventType::FocusGained, m_id);
            focus.payload = payload;
            m_platform.queue().push(std::move(focus));
            break;
        }

        case FocusOut: {
            m_focused.set(false);
            if (m_ic != nullptr) {
                ::XUnsetICFocus(m_ic);
            }
            FocusEvent payload{};
            payload.focused = false;
            Event focus = makeEvent(EventType::FocusLost, m_id);
            focus.payload = payload;
            m_platform.queue().push(std::move(focus));
            break;
        }

        case MapNotify: {
            m_visible.set(true);
            VisibilityEvent payload{};
            payload.visible = true;
            Event shown = makeEvent(EventType::Shown, m_id);
            shown.payload = payload;
            m_platform.queue().push(std::move(shown));
            break;
        }

        case UnmapNotify: {
            m_visible.set(false);
            VisibilityEvent payload{};
            payload.visible = false;
            Event hidden = makeEvent(EventType::Hidden, m_id);
            hidden.payload = payload;
            m_platform.queue().push(std::move(hidden));
            break;
        }

        case PropertyNotify: {
            // Track _NET_WM_STATE for fullscreen / maximized (WM driven changes).
            if (event.xproperty.atom == m_platform.netWmState()) {
                ::Atom actualType = 0;
                int actualFormat = 0;
                unsigned long itemCount = 0;
                unsigned long bytesAfter = 0;
                unsigned char* data = nullptr;
                bool fullscreen = false;
                bool maximized = false;
                if (::XGetWindowProperty(display, m_window, m_platform.netWmState(), 0, 32, 0, XA_ATOM,
                                         &actualType, &actualFormat, &itemCount, &bytesAfter, &data) == 0 &&
                    data != nullptr) {
                    const auto* atoms = reinterpret_cast<const ::Atom*>(data);
                    for (unsigned long index = 0; index < itemCount; ++index) {
                        if (atoms[index] == m_platform.netWmStateFullscreen()) {
                            fullscreen = true;
                        }
                        if (atoms[index] == m_platform.netWmStateMaximizedHorz() ||
                            atoms[index] == m_platform.netWmStateMaximizedVert()) {
                            maximized = true;
                        }
                    }
                    ::XFree(data);
                }
                if (fullscreen != m_fullscreen.get()) {
                    m_fullscreen.set(fullscreen);
                    m_fullscreenActive = fullscreen;
                    StateEvent payload{};
                    payload.active = fullscreen;
                    Event state = makeEvent(fullscreen ? EventType::Maximized : EventType::Restored, m_id);
                    state.payload = payload;
                    m_platform.queue().push(std::move(state));
                }
                if (maximized != m_maximized.get()) {
                    m_maximized.set(maximized);
                    StateEvent payload{};
                    payload.active = maximized;
                    Event state = makeEvent(maximized ? EventType::Maximized : EventType::Restored, m_id);
                    state.payload = payload;
                    m_platform.queue().push(std::move(state));
                }
            }
            break;
        }

        case Expose:
        case ReparentNotify:
        case MapRequest:
        case DestroyNotify:
        case GravityNotify:
        case VisibilityNotify:
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void X11Window::syncGeometry() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return;
    }
    ::XWindowAttributes attributes{};
    if (::XGetWindowAttributes(display, m_window, &attributes) == 0) {
        return;
    }
    m_clientSize.set(Size{toLogical(attributes.width, m_scale), toLogical(attributes.height, m_scale)});
    m_windowSize.set(m_clientSize.get());

    // Absolute position: translate the window origin into root coordinates.
    // _NET_FRAME_EXTENTS would be required to report the *frame* origin, so the
    // client origin is reported and documented as such.
    ::Window child = 0;
    int rootX = 0;
    int rootY = 0;
    if (::XTranslateCoordinates(display, m_window, m_platform.root(), 0, 0, &rootX, &rootY, &child) != 0) {
        m_position.set(Point{toLogical(rootX, m_scale), toLogical(rootY, m_scale)});
    }
}

void X11Window::syncScaleFactor(bool emitEvent) {
    const double before = m_scaleFactor.get();
    m_scale = m_platform.scaleFactor();
    m_scaleFactor.set(m_scale);
    if (!emitEvent || before == m_scale) {
        return;
    }
    ScaleFactorEvent payload{};
    payload.scaleFactor = m_scale;
    Event event = makeEvent(EventType::ScaleFactorChanged, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void X11Window::onSurfaceResized() { syncGeometry(); }

void X11Window::onDisplayConfigurationChanged() {
    syncScaleFactor(true);
    syncGeometry();
    ResizeEvent payload{};
    payload.clientSize = m_clientSize.get();
    payload.windowSize = m_windowSize.get();
    payload.framebufferSize = framebufferSize();
    Event event = makeEvent(EventType::Resized, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void X11Window::onNativeDestroyed() {
    m_window = 0;
    m_wrapper = 0;
}

// ---------------------------------------------------------------------------
// WindowBackend interface
// ---------------------------------------------------------------------------

bool X11Window::supports(Capability capability) const noexcept {
    return m_platform.supports(capability);
}

std::string X11Window::title() const {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return {};
    }
    // Prefer _NET_WM_NAME (UTF-8); fall back to WM_NAME (Latin-1).
    ::Atom actualType = 0;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    std::string result;
    if (::XGetWindowProperty(display, m_window, m_platform.netWmName(), 0, 1024, 0,
                             m_platform.utf8String(), &actualType, &actualFormat, &itemCount, &bytesAfter,
                             &data) == 0 &&
        data != nullptr && itemCount > 0) {
        result.assign(reinterpret_cast<const char*>(data), itemCount);
        ::XFree(data);
        return result;
    }
    char* name = nullptr;
    if (::XFetchName(display, m_window, &name) != 0 && name != nullptr) {
        result.assign(name);
        ::XFree(name);
    }
    return result;
}

WindowNativeHandle X11Window::nativeHandle() const {
    WindowNativeHandle handle{};
    if (m_window != 0) {
        handle.window = NativeHandle{NativeHandle::Kind::X11Window, static_cast<uintptr_t>(m_window)};
    }
    if (m_platform.display() != nullptr) {
        handle.instance = NativeHandle{NativeHandle::Kind::X11Display,
                                       reinterpret_cast<uintptr_t>(m_platform.display())};
    }
    return handle;
}

nativebridge::Status X11Window::setTitle(std::string_view title) {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "setTitle: window is gone");
    }
    // WM_NAME for compatibility and _NET_WM_NAME for UTF-8 aware task bars.
    (void)::XStoreName(display, m_window, std::string(title).c_str());
    (void)::XChangeProperty(display, m_window, m_platform.netWmName(), m_platform.utf8String(), 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(title.data()),
                            static_cast<int>(title.size()));
    ::XFlush(display);
    return okStatus();
}

nativebridge::Status X11Window::resize(Size clientSize) {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "resize: window is gone");
    }
    if (clientSize.width <= 0 || clientSize.height <= 0) {
        return failStatus(Errc::InvalidArgument, "resize: client size must be positive");
    }
    ::XResizeWindow(display, m_window, static_cast<unsigned int>(toPhysical(clientSize.width, m_scale)),
                    static_cast<unsigned int>(toPhysical(clientSize.height, m_scale)));
    ::XFlush(display);
    syncGeometry();
    return okStatus();
}

nativebridge::Status X11Window::move(Point position) {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "move: window is gone");
    }
    ::XMoveWindow(display, m_window, toPhysical(position.x, m_scale), toPhysical(position.y, m_scale));
    ::XFlush(display);
    syncGeometry();
    return okStatus();
}

void X11Window::updateSizeHints() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return;
    }
    XSizeHints hints{};
    hints.flags = 0;
    if (m_minSize.width > 0 || m_minSize.height > 0) {
        hints.flags |= PMinSize;
        hints.min_width = toPhysical(m_minSize.width, m_scale);
        hints.min_height = toPhysical(m_minSize.height, m_scale);
    }
    if (m_maxSize.width > 0 || m_maxSize.height > 0) {
        hints.flags |= PMaxSize;
        hints.max_width = toPhysical(m_maxSize.width, m_scale);
        hints.max_height = toPhysical(m_maxSize.height, m_scale);
    }
    if (hints.flags != 0) {
        (void)::XSetWMNormalHints(display, m_window, &hints);
        ::XFlush(display);
    }
}

nativebridge::Status X11Window::setMinSize(Size size) {
    m_minSize = size;
    updateSizeHints();
    return okStatus();
}

nativebridge::Status X11Window::setMaxSize(Size size) {
    m_maxSize = size;
    updateSizeHints();
    return okStatus();
}

nativebridge::Status X11Window::show() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "show: window is gone");
    }
    ::XMapWindow(display, m_window);
    ::XFlush(display);
    m_visible.set(true);
    return okStatus();
}

nativebridge::Status X11Window::hide() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "hide: window is gone");
    }
    ::XUnmapWindow(display, m_window);
    ::XFlush(display);
    m_visible.set(false);
    return okStatus();
}

nativebridge::Status X11Window::minimize() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "minimize: window is gone");
    }
    // EWMH compliant WMs must be asked through a client message.
    (void)::XIconifyWindow(display, m_window, m_platform.screen());
    ::XFlush(display);
    return okStatus();
}

void X11Window::sendNetWmState(::Atom first, ::Atom second, bool enable) {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return;
    }
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = m_window;
    event.xclient.message_type = m_platform.netWmState();
    event.xclient.format = 32;
    // 0 = remove, 1 = add, 2 = toggle
    event.xclient.data.l[0] = enable ? 1 : 0;
    event.xclient.data.l[1] = static_cast<long>(first);
    event.xclient.data.l[2] = static_cast<long>(second);
    event.xclient.data.l[3] = 1; // source indication: application
    (void)::XSendEvent(display, m_platform.root(), 0,
                       SubstructureNotifyMask | SubstructureRedirectMask, &event);
    ::XFlush(display);
}

nativebridge::Status X11Window::maximize() {
    if (m_window == 0) {
        return failStatus(Errc::InvalidArgument, "maximize: window is gone");
    }
    sendNetWmState(m_platform.netWmStateMaximizedHorz(), m_platform.netWmStateMaximizedVert(), true);
    m_maximized.set(true);
    return okStatus();
}

nativebridge::Status X11Window::restore() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "restore: window is gone");
    }
    sendNetWmState(m_platform.netWmStateMaximizedHorz(), m_platform.netWmStateMaximizedVert(), false);
    if (m_fullscreenActive) {
        sendNetWmState(m_platform.netWmStateFullscreen(), 0, false);
        m_fullscreenActive = false;
        m_fullscreen.set(false);
    }
    m_maximized.set(false);
    ::XMapRaised(display, m_window);
    ::XFlush(display);
    return okStatus();
}

nativebridge::Status X11Window::setFullscreen(bool fullscreen) {
    if (m_window == 0) {
        return failStatus(Errc::InvalidArgument, "setFullscreen: window is gone");
    }
    sendNetWmState(m_platform.netWmStateFullscreen(), 0, fullscreen);
    m_fullscreenActive = fullscreen;
    m_fullscreen.set(fullscreen);
    return okStatus();
}

nativebridge::Status X11Window::setAlwaysOnTop(bool onTop) {
    if (m_window == 0) {
        return failStatus(Errc::InvalidArgument, "setAlwaysOnTop: window is gone");
    }
    sendNetWmState(m_platform.netWmStateAbove(), 0, onTop);
    return okStatus();
}

nativebridge::Status X11Window::setResizable(bool resizable) {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "setResizable: window is gone");
    }
    if (!resizable) {
        // Pin the size through the normal hints: X11 has no style bit for this.
        XSizeHints hints{};
        hints.flags = PMinSize | PMaxSize;
        const Size current = m_clientSize.get();
        hints.min_width = hints.max_width = toPhysical(current.width, m_scale);
        hints.min_height = hints.max_height = toPhysical(current.height, m_scale);
        (void)::XSetWMNormalHints(display, m_window, &hints);
    } else {
        updateSizeHints();
    }
    m_resizable = resizable;
    ::XFlush(display);
    return okStatus();
}

nativebridge::Status X11Window::setDecorations(bool decorations) {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "setDecorations: window is gone");
    }
    // Motif hints are the de-facto standard honoured by every major WM.
    struct MotifHints {
        unsigned long flags;
        unsigned long functions;
        unsigned long decorations;
        long inputMode;
        unsigned long status;
    };
    MotifHints hints{};
    hints.flags = 2; // MWM_HINTS_DECORATIONS
    hints.decorations = decorations ? 1UL : 0UL;
    const ::Atom motifHints = ::XInternAtom(display, "_MOTIF_WM_HINTS", 0);
    (void)::XChangeProperty(display, m_window, motifHints, motifHints, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(&hints), 5);
    m_decorations = decorations;
    ::XFlush(display);
    return okStatus();
}

void X11Window::applyCursor() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return;
    }
    ::Cursor cursor = m_customCursor;
    if (cursor == 0) {
        if (!m_cursorVisible) {
            cursor = createInvisibleCursor(display, m_platform.root());
        } else {
            if (m_currentCursor != 0) {
                ::XFreeCursor(display, m_currentCursor);
            }
            m_currentCursor = ::XCreateFontCursor(display, xCursorShape(m_cursorShape));
            cursor = m_currentCursor;
        }
    }
    if (cursor != 0) {
        (void)::XDefineCursor(display, m_window, cursor);
    }
    ::XFlush(display);
}

nativebridge::Status X11Window::setCursor(CursorShape shape) {
    m_cursorShape = shape;
    m_cursor.set(shape);
    applyCursor();
    return okStatus();
}

nativebridge::Status X11Window::setCursorVisible(bool visible) {
    m_cursorVisible = visible;
    applyCursor();
    return okStatus();
}

nativebridge::Status X11Window::setClipRect(const std::optional<Rect>& rect) {
    m_clipRect = rect;
    return okStatus();
}

nativebridge::Status X11Window::requestAttention() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "requestAttention: window is gone");
    }
    // _NET_WM_STATE_DEMANDS_ATTENTION is the EWMH way to flag the task bar entry.
    const ::Atom demandsAttention = ::XInternAtom(display, "_NET_WM_STATE_DEMANDS_ATTENTION", 0);
    sendNetWmState(demandsAttention, 0, true);
    return okStatus();
}

nativebridge::Status X11Window::requestClose() {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "requestClose: window is gone");
    }
    // Ask the window manager to close us, exactly as the titlebar button would.
    ::Atom protocols[1] = {m_platform.wmDeleteWindow()};
    (void)::XSetWMProtocols(display, m_window, protocols, 1);
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = m_window;
    event.xclient.message_type = m_platform.wmProtocols();
    event.xclient.format = 32;
    event.xclient.data.l[0] = static_cast<long>(m_platform.wmDeleteWindow());
    event.xclient.data.l[1] = CurrentTime;
    (void)::XSendEvent(display, m_window, 0, NoEventMask, &event);
    ::XFlush(display);
    return okStatus();
}

nativebridge::Status X11Window::setCustomCursor(std::span<const uint8_t> bgraPixels, Size size, Point hotspot) {
    Display* display = m_platform.display();
    if (display == nullptr || m_window == 0) {
        return failStatus(Errc::InvalidArgument, "setCustomCursor: window is gone");
    }
    const std::size_t expected = static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) * 4u;
    if (size.width <= 0 || size.height <= 0 || bgraPixels.size() < expected) {
        return failStatus(Errc::InvalidArgument, "setCustomCursor: buffer must hold width*height*4 BGRA bytes");
    }

    // X11 has no alpha cursor without the XRender extension, so the bitmap is
    // thresholded: fully transparent pixels become the mask.
    XImage* image = XCreateImage(display, DefaultVisual(display, m_platform.screen()),
                                   static_cast<unsigned int>(DefaultDepth(display, m_platform.screen())),
                                   ZPixmap, 0, nullptr, static_cast<unsigned int>(size.width),
                                   static_cast<unsigned int>(size.height), 32, 0);
    if (image == nullptr) {
        return failStatus(Errc::OutOfMemory, "setCustomCursor: XCreateImage failed");
    }
    image->data = static_cast<char*>(std::calloc(static_cast<std::size_t>(image->bytes_per_line) *
                                                     static_cast<std::size_t>(size.height),
                                                 1));
    if (image->data == nullptr) {
        XDestroyImage(image);
        return failStatus(Errc::OutOfMemory, "setCustomCursor: image buffer allocation failed");
    }

    const std::size_t maskStride = (static_cast<std::size_t>(size.width) + 7u) / 8u;
    std::vector<uint8_t> mask(maskStride * static_cast<std::size_t>(size.height), 0);
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width) +
                                       static_cast<std::size_t>(x)) * 4u;
            const uint8_t blue = bgraPixels[index + 0];
            const uint8_t green = bgraPixels[index + 1];
            const uint8_t red = bgraPixels[index + 2];
            const uint8_t alpha = bgraPixels[index + 3];
            const unsigned long pixel = (static_cast<unsigned long>(red) << 16) |
                                        (static_cast<unsigned long>(green) << 8) |
                                        static_cast<unsigned long>(blue);
            XPutPixel(image, x, y, alpha > 127 ? pixel : 0UL);
            if (alpha <= 127) {
                mask[static_cast<std::size_t>(y) * maskStride + static_cast<std::size_t>(x) / 8u] |=
                    static_cast<uint8_t>(1u << (x % 8));
            }
        }
    }

    Pixmap source = ::XCreatePixmap(display, m_platform.root(), static_cast<unsigned int>(size.width),
                                    static_cast<unsigned int>(size.height),
                                    static_cast<unsigned int>(DefaultDepth(display, m_platform.screen())));
    GC gc = ::XCreateGC(display, source, 0, nullptr);
    (void)XPutImage(display, source, gc, image, 0, 0, 0, 0, static_cast<unsigned int>(size.width),
                      static_cast<unsigned int>(size.height));
    ::XFreeGC(display, gc);
    XDestroyImage(image); // frees image->data as well

    Pixmap maskPixmap = ::XCreateBitmapFromData(display, m_platform.root(),
                                                reinterpret_cast<const char*>(mask.data()),
                                                static_cast<unsigned int>(size.width),
                                                static_cast<unsigned int>(size.height));
    XColor foreground{};
    foreground.red = 0xFFFF;
    foreground.green = 0xFFFF;
    foreground.blue = 0xFFFF;
    foreground.flags = DoRed | DoGreen | DoBlue;

    ::Cursor cursor = ::XCreatePixmapCursor(display, source, maskPixmap, &foreground, &foreground,
                                            static_cast<unsigned int>(hotspot.x),
                                            static_cast<unsigned int>(hotspot.y));
    ::XFreePixmap(display, source);
    if (maskPixmap != 0) {
        ::XFreePixmap(display, maskPixmap);
    }
    if (cursor == 0) {
        return failStatus(Errc::PlatformFailure, "setCustomCursor: XCreatePixmapCursor failed");
    }
    if (m_customCursor != 0) {
        ::XFreeCursor(display, m_customCursor);
    }
    m_customCursor = cursor;
    ::XDefineCursor(display, m_window, cursor);
    ::XFlush(display);
    return okStatus();
}

nativebridge::Status X11Window::materializeSurface(RenderApi api, std::string_view backendName, NativeHandle& surface) {
    (void)backendName;
    if (m_window == 0) {
        return failStatus(Errc::InvalidArgument, "materializeSurface: window is gone");
    }
    switch (api) {
        case RenderApi::Direct3D11:
        case RenderApi::Direct3D12:
            return failStatus(Errc::Unsupported, "materializeSurface: Direct3D is not available on Linux");
        case RenderApi::Metal:
            return failStatus(Errc::Unsupported, "materializeSurface: Metal is not available on Linux");
        default:
            // GLX/EGL/Vulkan on X11 consume the XID (and the Display*) directly.
            surface = NativeHandle{};
            return okStatus();
    }
}

} // namespace nativebridge::detail

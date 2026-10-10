// nativeBridge — Linux/Wayland backend (libwayland-client + xdg-shell only).
// SPDX-License-Identifier: MIT
//
// ===========================================================================
// KNOWN WAYLAND LIMITATIONS (also documented in include/nativebridge/window.h
// and surfaced through Platform::supports(Capability) so callers never have to
// guess):
//
// 1. NO GLOBAL WINDOW POSITION. Wayland intentionally hides absolute screen
//    coordinates from clients:
//      * Window::position() always returns {0,0},
//      * MoveEvent::globalPositionKnown is false,
//      * Capability::WindowPosition is NOT advertised,
//      * Window::move() returns Errc::Unsupported,
//      * Window::release() returns Errc::Unsupported.
//    Placement requires an out-of-scope protocol (xdg-foreign, layer-shell).
// 2. DECORATIONS ARE COMPOSITOR OWNED. Without the optional xdg-decoration
//    protocol the server decides whether to draw a titlebar - possibly none at
//    all. setDecorations() therefore returns Errc::Unsupported and windowSize()
//    equals clientSize() because the client cannot measure server side chrome.
// 3. CLIPBOARD DEPENDS ON FOCUS. A client may only set or read the selection
//    while one of its surfaces has keyboard focus; the payload is transferred
//    through a file descriptor handed over by wl_data_device. getText() must
//    consequently run on the polling thread.
// 4. REFRESH RATE is only reported by wl_output version 4 and later; older
//    compositors report 0 Hz.
//
// DEPENDENCIES: libwayland-client and the xdg-shell protocol only. There is no
// xkbcommon dependency (the standard evdev -> X keysym table is used, so X11 and
// Wayland yield identical KeyCode values) and no libwayland-cursor (client side
// cursors are therefore reported as Unsupported).
//
// THREADING MODEL
// ---------------
// One wl_display per process, driven exclusively from the UI thread. pump()
// never blocks: it flushes, reads what is already available and dispatches it.
// wait() blocks in poll(2) up to the requested timeout. Subscriber callbacks
// therefore always run on the polling thread, as documented in event.h.
// ===========================================================================

#include "linux/wayland/wayland_backend.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/backends.h"
#include "nativebridge/system.h"

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

// ---------------------------------------------------------------------------
// evdev scancode -> X11 keysym
// ---------------------------------------------------------------------------
//
// wl_keyboard::key delivers the evdev scancode (X11 keycode - 8). The table is
// the standard Xorg "evdev" ruleset for keys with a fixed position; the
// alphanumeric block already sits at its ASCII/keysym value.

constexpr uint32_t kXK_Escape        = 0xFF1B;
constexpr uint32_t kXK_BackSpace     = 0xFF08;
constexpr uint32_t kXK_Tab           = 0xFF09;
constexpr uint32_t kXK_Return        = 0xFF0D;
constexpr uint32_t kXK_Delete        = 0xFFFF;
constexpr uint32_t kXK_Home          = 0xFF50;
constexpr uint32_t kXK_Left          = 0xFF51;
constexpr uint32_t kXK_Up            = 0xFF52;
constexpr uint32_t kXK_Right         = 0xFF53;
constexpr uint32_t kXK_Down          = 0xFF54;
constexpr uint32_t kXK_Prior         = 0xFF55;
constexpr uint32_t kXK_Next          = 0xFF56;
constexpr uint32_t kXK_End           = 0xFF57;
constexpr uint32_t kXK_Insert        = 0xFF63;
constexpr uint32_t kXK_Num_Lock      = 0xFF7F;
constexpr uint32_t kXK_KP_Enter      = 0xFF8D;
constexpr uint32_t kXK_KP_Multiply   = 0xFFAA;
constexpr uint32_t kXK_KP_Add        = 0xFFAB;
constexpr uint32_t kXK_KP_Subtract   = 0xFFAD;
constexpr uint32_t kXK_KP_Decimal    = 0xFFAE;
constexpr uint32_t kXK_KP_Divide     = 0xFFAF;
constexpr uint32_t kXK_KP_Home       = 0xFF95;
constexpr uint32_t kXK_KP_Left       = 0xFF96;
constexpr uint32_t kXK_KP_Up         = 0xFF97;
constexpr uint32_t kXK_KP_Right      = 0xFF98;
constexpr uint32_t kXK_KP_Down       = 0xFF99;
constexpr uint32_t kXK_KP_Prior      = 0xFF9A;
constexpr uint32_t kXK_KP_Next       = 0xFF9B;
constexpr uint32_t kXK_KP_End        = 0xFF9C;
constexpr uint32_t kXK_KP_Insert     = 0xFF9E;
constexpr uint32_t kXK_KP_Delete     = 0xFF9F;
constexpr uint32_t kXK_KP_Equal      = 0xFFBD;
constexpr uint32_t kXK_KP_0          = 0xFFB0;
constexpr uint32_t kXK_F1            = 0xFFBE;
constexpr uint32_t kXK_F11           = 0xFFC8;
constexpr uint32_t kXK_F12           = 0xFFC9;
constexpr uint32_t kXK_Shift_L       = 0xFFE1;
constexpr uint32_t kXK_Shift_R       = 0xFFE2;
constexpr uint32_t kXK_Control_L     = 0xFFE3;
constexpr uint32_t kXK_Control_R     = 0xFFE4;
constexpr uint32_t kXK_Caps_Lock     = 0xFFE5;
constexpr uint32_t kXK_Meta_L        = 0xFFE7;
constexpr uint32_t kXK_Meta_R        = 0xFFE8;
constexpr uint32_t kXK_Alt_L         = 0xFFE9;
constexpr uint32_t kXK_Alt_R         = 0xFFEA;
constexpr uint32_t kXK_Super_L       = 0xFFEB;
constexpr uint32_t kXK_Super_R       = 0xFFEC;
constexpr uint32_t kXK_Scroll_Lock   = 0xFF14;
constexpr uint32_t kXK_Print         = 0xFF61;
constexpr uint32_t kXK_Pause         = 0xFF13;
constexpr uint32_t kXK_ISO_Level3_Shift = 0xFE03;
constexpr uint32_t kXK_Zenkaku_Hankaku  = 0xFF21;
constexpr uint32_t kXK_Muhenkan      = 0xFF22;
constexpr uint32_t kXK_Henkan        = 0xFF23;
constexpr uint32_t kXK_Hiragana      = 0xFF25;
constexpr uint32_t kXK_Hangul        = 0xFF31;
constexpr uint32_t kXK_Hangul_Hanja  = 0xFF34;
constexpr uint32_t kXK_AudioMute     = 0x1008FF12;
constexpr uint32_t kXK_AudioLowerVolume = 0x1008FF11;
constexpr uint32_t kXK_AudioRaiseVolume = 0x1008FF13;
constexpr uint32_t kXK_AudioPlay     = 0x1008FF14;
constexpr uint32_t kXK_AudioStop     = 0x1008FF15;
constexpr uint32_t kXK_AudioPrev     = 0x1008FF16;
constexpr uint32_t kXK_AudioNext     = 0x1008FF17;

/// Fixed keysyms per evdev scancode 0..127; 0 means "no fixed keysym".
constexpr uint32_t kEvdevKeysyms[128] = {
    0,              kXK_Escape,     0x31,           0x32,           0x33,           0x34,
    0x35,           0x36,           0x37,           0x38,           0x39,           0x30,
    0x2D,           0x3D,           kXK_BackSpace,  kXK_Tab,        0x71,           0x77,
    0x65,           0x72,           0x74,           0x79,           0x75,           0x69,
    0x6F,           0x70,           0x5B,           0x5D,           kXK_Return,     kXK_Control_L,
    0x61,           0x73,           0x64,           0x66,           0x67,           0x68,
    0x6A,           0x6B,           0x6C,           0x3B,           0x27,           0x60,
    kXK_Shift_L,    0x5C,           0x7A,           0x78,           0x63,           0x76,
    0x62,           0x6E,           0x6D,           0x2C,           0x2E,           0x2F,
    kXK_Shift_R,    kXK_KP_Multiply, kXK_Alt_L,     0x20,           kXK_Caps_Lock,  kXK_F1,
    kXK_F1 + 1,     kXK_F1 + 2,     kXK_F1 + 3,     kXK_F1 + 4,     kXK_F1 + 5,     kXK_F1 + 6,
    kXK_F1 + 7,     kXK_F1 + 8,     kXK_F1 + 9,     kXK_Num_Lock,   kXK_Scroll_Lock, kXK_KP_0 + 7,
    kXK_KP_0 + 8,   kXK_KP_0 + 9,   kXK_KP_Subtract, kXK_KP_0 + 4,  kXK_KP_0 + 5,   kXK_KP_0 + 6,
    kXK_KP_Add,     kXK_KP_0 + 1,   kXK_KP_0 + 2,   kXK_KP_0 + 3,   kXK_KP_0,       kXK_KP_Decimal,
    0,              0,              0x3C,           kXK_F11,        kXK_F12,        0,
    0,              0,              0,              0,              0,              0,
    0,              0,              0,              0,              0,              0,
    0,              0,              0,              0,              0,              0,
    0,              0,              0,              0,              0,              0,
    kXK_KP_Enter,   kXK_Control_R,  kXK_KP_Divide,  kXK_Print,      kXK_Alt_R,      0,
    kXK_Home,       kXK_Up,         kXK_Prior,      kXK_Left,       kXK_Right,      kXK_End,
    kXK_Down,       kXK_Next,       kXK_Insert,     kXK_Delete,     0,              0,
    0,              0,              0,              kXK_Pause,
};

Modifier modifiersFromXkbState(uint32_t depressed, uint32_t locked) {
    // Standard X11 modifier indices used by the xkb "evdev" ruleset.
    Modifier modifiers = Modifier::None;
    if ((depressed & 0x01u) != 0) { modifiers |= Modifier::Shift; }
    if ((depressed & 0x04u) != 0) { modifiers |= Modifier::Control; }
    if ((depressed & 0x08u) != 0) { modifiers |= Modifier::Alt; }
    if ((depressed & 0x40u) != 0) { modifiers |= Modifier::Super; }
    if ((locked & 0x02u) != 0) { modifiers |= Modifier::CapsLock; }
    if ((locked & 0x10u) != 0) { modifiers |= Modifier::NumLock; }
    return modifiers;
}

/// Wayland pointer button codes are Linux input event codes (BTN_*).
MouseButton mouseButtonFromEvdev(uint32_t code) {
    switch (code) {
        case 0x110: return MouseButton::Left;   // BTN_LEFT
        case 0x111: return MouseButton::Right;  // BTN_RIGHT
        case 0x112: return MouseButton::Middle; // BTN_MIDDLE
        case 0x113: return MouseButton::X1;     // BTN_SIDE
        case 0x114: return MouseButton::X2;     // BTN_EXTRA
        default:    return MouseButton::Unknown;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Key mapping helpers (declared in wayland_backend.h)
// ---------------------------------------------------------------------------

uint32_t waylandKeysymFromEvdev(uint32_t evdev) noexcept {
    constexpr std::size_t tableSize = sizeof(kEvdevKeysyms) / sizeof(kEvdevKeysyms[0]);
    if (evdev < tableSize) {
        return kEvdevKeysyms[evdev];
    }
    return 0;
}

KeyCode waylandKeyCodeFromEvdev(uint32_t evdev) noexcept {
    const uint32_t keysym = waylandKeysymFromEvdev(evdev);
    if (keysym == 0) {
        return KeyCode::Unknown;
    }
    return keyCodeFromKeysym(keysym);
}

uint32_t waylandCharacterFromEvdev(uint32_t evdev) noexcept {
    const uint32_t keysym = waylandKeysymFromEvdev(evdev);
    return keysym == 0 ? 0u : characterFromKeysym(keysym);
}

// ---------------------------------------------------------------------------
// wl_registry / wl_output / wl_seat / input listeners
// ---------------------------------------------------------------------------

namespace {

void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->handleGlobal(registry, name, interface, version);
    }
}

void registryGlobalRemove(void* data, wl_registry*, uint32_t name) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->handleGlobalRemove(name);
    }
}

constexpr wl_registry_listener kRegistryListener = {&registryGlobal, &registryGlobalRemove};

void outputGeometry(void* data, wl_output*, int32_t x, int32_t y, int32_t physicalWidth, int32_t physicalHeight,
                    int32_t subpixel, const char* make, const char* model, int32_t transform) {
    // The (x, y) a compositor reports are private and may be identical for every
    // output. They are deliberately NOT used as DisplayInfo::bounds; see
    // WaylandPlatform::rebuildDisplays() for the documented layout strategy.
    (void)data;
    (void)x;
    (void)y;
    (void)physicalWidth;
    (void)physicalHeight;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

void outputMode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onOutputMode(flags, width, height, refresh);
    }
}

void outputDone(void* data, wl_output*) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->markDisplaysDirty();
    }
}

void outputScale(void* data, wl_output*, int32_t factor) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onOutputScale(factor);
    }
}

void outputName(void* data, wl_output*, const char* name) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr && name != nullptr) {
        platform->onOutputName(name);
    }
}

void outputDescription(void* data, wl_output*, const char* description) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr && description != nullptr) {
        platform->onOutputDescription(description);
    }
}

constexpr wl_output_listener kOutputListener = {
    &outputGeometry, &outputMode, &outputDone, &outputScale, &outputName, &outputDescription,
};

void seatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onSeatCapabilities(seat, capabilities);
    }
}

void seatName(void*, wl_seat*, const char*) {}

constexpr wl_seat_listener kSeatListener = {&seatCapabilities, &seatName};

void pointerEnter(void* data, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy) {
    auto* platform = static_cast<WaylandPlatform*>(data);
    if (platform == nullptr) {
        return;
    }
    platform->setPointerFocus(platform->windowForSurface(surface), serial);
    platform->onPointerPosition(wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}

void pointerLeave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onPointerLeave();
    }
}

void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onPointerPosition(wl_fixed_to_double(sx), wl_fixed_to_double(sy));
    }
}

void pointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->rememberSerial(serial);
        platform->onPointerButton(button, state);
    }
}

void pointerAxis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onPointerAxis(axis, wl_fixed_to_double(value), false);
    }
}

void pointerFrame(void* data, wl_pointer*) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->flushPointerEvents();
    }
}

void pointerAxisSource(void* data, wl_pointer*, uint32_t axisSource) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->setAxisSource(axisSource);
    }
}

void pointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}

void pointerAxisDiscrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onPointerAxisDiscrete(axis, discrete);
    }
}

constexpr wl_pointer_listener kPointerListener = {
    &pointerEnter,     &pointerLeave,      &pointerMotion, &pointerButton, &pointerAxis,
    &pointerFrame,     &pointerAxisSource, &pointerAxisStop, &pointerAxisDiscrete,
};

void keyboardKeymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) {
    // nativeBridge maps evdev scancodes directly, so the keymap is unused; the
    // compositor still requires the descriptor to be closed to avoid a leak.
    if (fd >= 0) {
        ::close(fd);
    }
}

void keyboardEnter(void* data, wl_keyboard*, uint32_t, wl_surface* surface, wl_array*) {
    auto* platform = static_cast<WaylandPlatform*>(data);
    if (platform != nullptr) {
        platform->onKeyboardFocus(platform->windowForSurface(surface), true);
    }
}

void keyboardLeave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onKeyboardFocus(nullptr, false);
    }
}

void keyboardKey(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onKeyboardKey(key, state);
    }
}

void keyboardModifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t, uint32_t locked, uint32_t) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onKeyboardModifiers(depressed, locked);
    }
}

void keyboardRepeatInfo(void*, wl_keyboard*, int32_t, int32_t) {}

constexpr wl_keyboard_listener kKeyboardListener = {
    &keyboardKeymap, &keyboardEnter, &keyboardLeave, &keyboardKey, &keyboardModifiers, &keyboardRepeatInfo,
};

void dataOfferMimeType(void* data, wl_data_offer*, const char* mimeType) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onDataOfferMimeType(mimeType);
    }
}

void dataOfferSourceActions(void*, wl_data_offer*, uint32_t) {}
void dataOfferAction(void*, wl_data_offer*, uint32_t) {}

constexpr wl_data_offer_listener kDataOfferListener = {
    &dataOfferMimeType, &dataOfferSourceActions, &dataOfferAction,
};

void dataOffer(void*, wl_data_device*, wl_data_offer* offer) {
    // An offer for a drag-and-drop or for the selection arrives through
    // dataSelection(); offers announced here are drained and destroyed.
    if (offer != nullptr) {
        wl_data_offer_destroy(offer);
    }
}

void dataEnter(void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* offer) {
    if (offer != nullptr) {
        wl_data_offer_destroy(offer);
    }
}

void dataLeave(void*, wl_data_device*) {}
void dataMotion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void dataDrop(void*, wl_data_device*) {}

void dataSelection(void* data, wl_data_device*, wl_data_offer* offer) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onSelectionOffer(offer);
    }
}

constexpr wl_data_device_listener kDataDeviceListener = {
    &dataOffer, &dataEnter, &dataLeave, &dataMotion, &dataDrop, &dataSelection,
};

void dataSourceTarget(void*, wl_data_source*, const char*) {}

void dataSourceSend(void* data, wl_data_source*, const char* mimeType, int32_t fd) {
    auto* platform = static_cast<WaylandPlatform*>(data);
    if (platform != nullptr) {
        platform->onDataSourceSend(mimeType, fd);
        return;
    }
    if (fd >= 0) {
        ::close(fd);
    }
}

void dataSourceCancelled(void* data, wl_data_source* source) {
    if (auto* platform = static_cast<WaylandPlatform*>(data); platform != nullptr) {
        platform->onDataSourceCancelled(source);
    }
}

void dataSourceDndDropPerformed(void*, wl_data_source*) {}
void dataSourceDndFinished(void*, wl_data_source*) {}
void dataSourceAction(void*, wl_data_source*, uint32_t) {}

constexpr wl_data_source_listener kDataSourceListener = {
    &dataSourceTarget,          &dataSourceSend,        &dataSourceCancelled,
    &dataSourceDndDropPerformed, &dataSourceDndFinished, &dataSourceAction,
};

#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
void wmBasePing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
constexpr xdg_wm_base_listener kWmBaseListener = {&wmBasePing};

void xdgSurfaceConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    // Acknowledge the configure event: without this the surface never becomes
    // mapped and no frame callbacks arrive.
    xdg_surface_ack_configure(surface, serial);
    if (auto* window = static_cast<WaylandWindow*>(data); window != nullptr) {
        window->onConfigure(0, 0, false, false, false);
    }
}

constexpr xdg_surface_listener kXdgSurfaceListener = {&xdgSurfaceConfigure};

void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
    auto* window = static_cast<WaylandWindow*>(data);
    if (window == nullptr) {
        return;
    }
    bool maximized = false;
    bool fullscreen = false;
    bool resizing = false;
    bool activated = false;
    const auto* stateData = static_cast<const uint32_t*>(states->data);
    const std::size_t stateCount = states->size / sizeof(uint32_t);
    for (std::size_t index = 0; index < stateCount; ++index) {
        switch (stateData[index]) {
            case XDG_TOPLEVEL_STATE_MAXIMIZED:        maximized = true; break;
            case XDG_TOPLEVEL_STATE_FULLSCREEN:       fullscreen = true; break;
            case XDG_TOPLEVEL_STATE_RESIZING:         resizing = true; break;
            case XDG_TOPLEVEL_STATE_ACTIVATED:        activated = true; break;
            case XDG_TOPLEVEL_STATE_TILED_LEFT:
            case XDG_TOPLEVEL_STATE_TILED_RIGHT:
            case XDG_TOPLEVEL_STATE_TILED_TOP:
            case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            default:
                break;
        }
    }
    (void)activated;
    window->onConfigure(width, height, maximized, fullscreen, resizing);
}

void toplevelClose(void* data, xdg_toplevel*) {
    if (auto* window = static_cast<WaylandWindow*>(data); window != nullptr) {
        window->onCloseRequested();
    }
}

void toplevelConfigureBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevelWmCapabilities(void*, xdg_toplevel*, wl_array*) {}

constexpr xdg_toplevel_listener kToplevelListener = {
    &toplevelConfigure, &toplevelClose, &toplevelConfigureBounds, &toplevelWmCapabilities,
};
#endif // NATIVEBRIDGE_HAVE_XDG_SHELL

/// Write a whole buffer to a file descriptor (clipboard transfer helper).
Status writeAllToFd(int fd, std::string_view data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(fd, data.data() + written, data.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return failStatus(Errc::PlatformFailure, "wayland clipboard: write failed",
                          static_cast<int64_t>(errno));
    }
    return okStatus();
}

/// Facade handed out by Platform::clipboard().
class WaylandClipboard final : public Clipboard {
public:
    explicit WaylandClipboard(WaylandPlatform& platform) : m_platform(platform) {}

    [[nodiscard]] Result<std::string> getText() override { return m_platform.clipboardGetText(); }
    [[nodiscard]] Status setText(std::string_view utf8Text) override {
        return m_platform.clipboardSetText(utf8Text);
    }
    [[nodiscard]] Status clear() override { return m_platform.clipboardClear(); }
    [[nodiscard]] bool hasText() override { return m_platform.clipboardHasText(); }
    [[nodiscard]] std::string_view backendName() const noexcept override { return "wayland"; }

private:
    WaylandPlatform& m_platform;
};

} // namespace

// ---------------------------------------------------------------------------
// WaylandOutputState bookkeeping
// ---------------------------------------------------------------------------

void WaylandPlatform::onOutputMode(uint32_t flags, int32_t width, int32_t height, int32_t refreshMilliHz) {
    // find the output whose listener is currently being dispatched: the listener
    // stores it in m_currentOutput before calling in.
    WaylandOutputState* state = m_currentOutput;
    if (state == nullptr) {
        return;
    }
    if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
        state->pixelWidth = width;
        state->pixelHeight = height;
        state->refreshMilliHz = refreshMilliHz > 0 ? static_cast<uint32_t>(refreshMilliHz) : 0;
        state->hasMode = true;
    }
    m_displaysDirty = true;
}

void WaylandPlatform::onOutputScale(int32_t factor) {
    if (m_currentOutput != nullptr && factor > 0) {
        m_currentOutput->scale = static_cast<uint32_t>(factor);
        m_scale = std::max(m_scale, static_cast<double>(factor));
    }
    m_displaysDirty = true;
}

void WaylandPlatform::onOutputName(const char* name) {
    if (m_currentOutput != nullptr) {
        m_currentOutput->info.name = name;
    }
}

void WaylandPlatform::onOutputDescription(const char* description) {
    if (m_currentOutput != nullptr && m_currentOutput->info.name.empty()) {
        m_currentOutput->info.name = description;
    }
}

// ---------------------------------------------------------------------------
// Platform lifecycle
// ---------------------------------------------------------------------------

WaylandPlatform::WaylandPlatform() {
    if (!isUiThread()) {
        m_fatal = Error{Errc::PlatformFailure, "wayland backend: the Platform must be created on its UI thread"};
        return;
    }
    m_lifetime->setDestroyCallback([this](WindowId id) { windowDestroyed(id); });
    connect();
}

void WaylandPlatform::connect() {
    const char* socketName = std::getenv("WAYLAND_DISPLAY");
    m_display = wl_display_connect(nullptr); // honours WAYLAND_DISPLAY / WAYLAND_SOCKET
    if (m_display == nullptr) {
        m_fatal = Error{Errc::ConnectionFailed,
                        std::string("wl_display_connect failed (WAYLAND_DISPLAY=") +
                            (socketName != nullptr ? socketName : "(unset)") + ")"};
        return;
    }
    m_registry = wl_display_get_registry(m_display);
    if (m_registry == nullptr) {
        m_fatal = Error{Errc::PlatformFailure, "wl_display_get_registry returned null"};
        return;
    }
    wl_registry_add_listener(m_registry, &kRegistryListener, this);

    // First roundtrip: learns every global. Second: lets wl_output listeners
    // deliver geometry/mode/scale before the first display snapshot is built.
    if (wl_display_roundtrip(m_display) < 0 || wl_display_roundtrip(m_display) < 0) {
        m_fatal = Error{Errc::ProtocolError, "wl_display_roundtrip failed during startup"};
        return;
    }
    if (m_compositor == nullptr) {
        m_fatal = Error{Errc::Unsupported, "the compositor does not expose wl_compositor"};
        return;
    }
    m_hasXdgShell =
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
        m_wmBase != nullptr;
#else
        false;
#endif
    if (!m_hasXdgShell) {
        log::warn("wayland: xdg-shell is unavailable (built without the generated protocol), "
                  "window creation will report Errc::Unsupported");
    }
    rebuildDisplays();
}

WaylandPlatform::~WaylandPlatform() {
    m_lifetime->invalidate();
    std::vector<WaylandWindow*> windows;
    {
        std::lock_guard lock(m_windowsMutex);
        windows = m_windows;
        m_windows.clear();
    }
    for (WaylandWindow* window : windows) {
        window->onNativeDestroyed();
    }

    m_clipboardFacade.reset();
    if (m_selectionOffer != nullptr) {
        wl_data_offer_destroy(m_selectionOffer);
        m_selectionOffer = nullptr;
    }
    if (m_dataSource != nullptr) {
        wl_data_source_destroy(m_dataSource);
        m_dataSource = nullptr;
    }
    if (m_dataDevice != nullptr) {
        wl_data_device_destroy(m_dataDevice);
        m_dataDevice = nullptr;
    }
    if (m_dataDeviceManager != nullptr) {
        wl_data_device_manager_destroy(m_dataDeviceManager);
        m_dataDeviceManager = nullptr;
    }
    destroySeatObjects();
    for (WaylandOutputState& output : m_outputs) {
        if (output.output != nullptr) {
            wl_output_destroy(output.output);
            output.output = nullptr;
        }
    }
    m_outputs.clear();
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_wmBase != nullptr) {
        xdg_wm_base_destroy(m_wmBase);
        m_wmBase = nullptr;
    }
#endif // NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_shm != nullptr) {
        wl_shm_destroy(m_shm);
        m_shm = nullptr;
    }
    if (m_compositor != nullptr) {
        wl_compositor_destroy(m_compositor);
        m_compositor = nullptr;
    }
    if (m_registry != nullptr) {
        wl_registry_destroy(m_registry);
        m_registry = nullptr;
    }
    if (m_display != nullptr) {
        wl_display_flush(m_display);
        wl_display_disconnect(m_display);
        m_display = nullptr;
    }
}

void WaylandPlatform::destroySeatObjects() {
    if (m_pointer != nullptr) {
        wl_pointer_destroy(m_pointer);
        m_pointer = nullptr;
    }
    if (m_keyboard != nullptr) {
        wl_keyboard_destroy(m_keyboard);
        m_keyboard = nullptr;
    }
    if (m_seat != nullptr) {
        wl_seat_destroy(m_seat);
        m_seat = nullptr;
    }
}

bool WaylandPlatform::supports(Capability capability) const noexcept {
    switch (capability) {
        case Capability::WindowPosition:
            return false; // documented limitation 1
        case Capability::WindowDecoration:
            return false; // documented limitation 2
        case Capability::AlwaysOnTop:
            return false; // no xdg-shell request exists for it
        case Capability::Transparency:
            return true;  // the compositor honours ARGB surfaces
        case Capability::ClientSideResize:
        case Capability::ClipboardText:
        case Capability::MultiDisplay:
        case Capability::GlobalCursor:
        case Capability::TextInput:
        case Capability::DpiChangeEvents:
            return m_display != nullptr;
        case Capability::ScreenCaptureLock:
            return false;
    }
    return false;
}

std::string WaylandPlatform::describe() const {
    std::string out = "wayland: WAYLAND_DISPLAY=";
    const char* socketName = std::getenv("WAYLAND_DISPLAY");
    out += socketName != nullptr ? socketName : "(unset)";
    out += ", outputs=" + std::to_string(m_outputs.size());
    out += std::string(", xdg-shell=") + (m_hasXdgShell ? "yes" : "no (window creation unsupported)");
    out += ", scale=" + std::to_string(m_scale);
    out += ", capabilities: no global position, compositor decorations, focus dependent clipboard";
    return out;
}

void WaylandPlatform::rebuildDisplays() {
    // LAYOUT STRATEGY
    // --------------
    // Wayland gives a client no usable global monitor layout (wl_output geometry
    // is compositor private and may be {0,0} for every output). nativeBridge
    // therefore assigns each output a deterministic horizontal band, in the order
    // the registry announced them, and documents that the per-monitor positions
    // are an approximation - which is exactly why Capability::WindowPosition is
    // not advertised on this backend.
    DisplayList updated;
    updated.reserve(m_outputs.size());
    int32_t offsetX = 0;
    for (std::size_t index = 0; index < m_outputs.size(); ++index) {
        WaylandOutputState& state = m_outputs[index];
        const double scale = state.scale > 0 ? static_cast<double>(state.scale) : 1.0;
        const int32_t logicalWidth = static_cast<int32_t>(
            std::lround(static_cast<double>(state.pixelWidth > 0 ? state.pixelWidth : 1920) / scale));
        const int32_t logicalHeight = static_cast<int32_t>(
            std::lround(static_cast<double>(state.pixelHeight > 0 ? state.pixelHeight : 1080) / scale));

        DisplayInfo info{};
        info.id = static_cast<DisplayId>(state.globalName != 0 ? state.globalName : index + 1);
        info.name = state.info.name.empty() ? ("output-" + std::to_string(index)) : state.info.name;
        info.isPrimary = index == 0;
        info.bounds = Rect{offsetX, 0, logicalWidth, logicalHeight};
        // The compositor reserves nothing the client can measure (no work area
        // protocol in core Wayland), so the work area equals the full bounds.
        info.workArea = info.bounds;
        info.scaleFactor = scale;
        info.pixelSize = PixelSize{static_cast<uint32_t>(state.pixelWidth), static_cast<uint32_t>(state.pixelHeight)};
        info.refreshRateHz = state.refreshMilliHz / 1000u;
        info.bitDepth = 32;
        info.rotationDegrees = 0;
        state.logicalOrigin = Point{offsetX, 0};
        state.info.bounds = info.bounds;
        state.info.workArea = info.workArea;
        state.info.scaleFactor = info.scaleFactor;
        state.info.refreshRateHz = info.refreshRateHz;
        state.info.id = info.id;
        state.info.isPrimary = info.isPrimary;
        state.info.pixelSize = info.pixelSize;

        offsetX += logicalWidth;
        updated.push_back(std::move(info));
    }

    const bool changed =
        updated.size() != m_displays.size() ||
        !std::equal(updated.begin(), updated.end(), m_displays.begin(),
                    [](const DisplayInfo& a, const DisplayInfo& b) {
                        return a.id == b.id && a.bounds == b.bounds && a.scaleFactor == b.scaleFactor &&
                               a.refreshRateHz == b.refreshRateHz;
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

void WaylandPlatform::refreshDisplays() {
    if (!m_displaysDirty && !m_displays.empty()) {
        return;
    }
    m_displaysDirty = false;
    rebuildDisplays();
}

void WaylandPlatform::handleGlobal(wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (interface == nullptr) {
        return;
    }
    const std::string_view iface(interface);
    const auto bind = [&](const wl_interface* target, uint32_t wanted) {
        return wl_registry_bind(registry, name, target, std::min(version, wanted));
    };

    if (iface == wl_compositor_interface.name && m_compositor == nullptr) {
        m_compositor = static_cast<wl_compositor*>(bind(&wl_compositor_interface, 4));
    } else if (iface == wl_shm_interface.name && m_shm == nullptr) {
        m_shm = static_cast<wl_shm*>(bind(&wl_shm_interface, 1));
    } else if (iface == wl_seat_interface.name && m_seat == nullptr) {
        m_seat = static_cast<wl_seat*>(bind(&wl_seat_interface, 5));
        if (m_seat != nullptr) {
            wl_seat_add_listener(m_seat, &kSeatListener, this);
        }
    } else if (iface == wl_data_device_manager_interface.name && m_dataDeviceManager == nullptr) {
        m_dataDeviceManager = static_cast<wl_data_device_manager*>(bind(&wl_data_device_manager_interface, 3));
    } else if (iface == wl_output_interface.name) {
        WaylandOutputState state{};
        state.globalName = name;
        state.output = static_cast<wl_output*>(bind(&wl_output_interface, 4));
        if (state.output != nullptr) {
            wl_output_add_listener(state.output, &kOutputListener, this);
            m_outputs.push_back(state);
            m_displaysDirty = true;
        }
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    } else if (iface == xdg_wm_base_interface.name && m_wmBase == nullptr) {
        m_wmBase = static_cast<xdg_wm_base*>(bind(&xdg_wm_base_interface, 2));
        if (m_wmBase != nullptr) {
            xdg_wm_base_add_listener(m_wmBase, &kWmBaseListener, this);
        }
#endif
    }
}

void WaylandPlatform::handleGlobalRemove(uint32_t name) {
    const auto it = std::find_if(m_outputs.begin(), m_outputs.end(),
                                 [name](const WaylandOutputState& state) { return state.globalName == name; });
    if (it != m_outputs.end()) {
        if (it->output != nullptr) {
            wl_output_destroy(it->output);
        }
        m_outputs.erase(it);
        m_displaysDirty = true;
        rebuildDisplays();
    }
}

void WaylandPlatform::onSeatCapabilities(wl_seat* seat, uint32_t capabilities) {
    const bool hasPointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

    if (hasPointer && m_pointer == nullptr) {
        m_pointer = wl_seat_get_pointer(seat);
        if (m_pointer != nullptr) {
            wl_pointer_add_listener(m_pointer, &kPointerListener, this);
        }
    } else if (!hasPointer && m_pointer != nullptr) {
        wl_pointer_destroy(m_pointer);
        m_pointer = nullptr;
        m_pointerFocus = nullptr;
    }

    if (hasKeyboard && m_keyboard == nullptr) {
        m_keyboard = wl_seat_get_keyboard(seat);
        if (m_keyboard != nullptr) {
            wl_keyboard_add_listener(m_keyboard, &kKeyboardListener, this);
        }
    } else if (!hasKeyboard && m_keyboard != nullptr) {
        wl_keyboard_destroy(m_keyboard);
        m_keyboard = nullptr;
        m_keyboardFocusWindow = nullptr;
    }

    // The clipboard lives on the seat's data device.
    if (m_dataDeviceManager != nullptr && m_dataDevice == nullptr) {
        m_dataDevice = wl_data_device_manager_get_data_device(m_dataDeviceManager, seat);
        if (m_dataDevice != nullptr) {
            wl_data_device_add_listener(m_dataDevice, &kDataDeviceListener, this);
            wl_data_device_set_selection(m_dataDevice, nullptr, 0); // release any stale offer
        }
    }
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

std::size_t WaylandPlatform::dispatchPending() {
    if (m_display == nullptr) {
        return 0;
    }
    // prepare_read/read_events/cancel_read is the documented pattern for a
    // non-blocking dispatch: it guarantees the socket is never read from a
    // thread that has not prepared the read itself.
    std::size_t handled = 0;
    while (wl_display_prepare_read(m_display) == 0) {
        if (wl_display_flush(m_display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(m_display);
            m_fatal = Error::platform("wayland: wl_display_flush failed", static_cast<int64_t>(errno));
            return handled;
        }
        pollfd descriptor{};
        descriptor.fd = wl_display_get_fd(m_display);
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 0);
        if (ready <= 0 || (descriptor.revents & (POLLERR | POLLHUP)) != 0) {
            wl_display_cancel_read(m_display);
            break;
        }
        if (wl_display_read_events(m_display) < 0) {
            m_fatal = Error::platform("wayland: wl_display_read_events failed", static_cast<int64_t>(errno));
            return handled;
        }
        handled += static_cast<std::size_t>(std::max(0, wl_display_dispatch_pending(m_display)));
    }
    handled += static_cast<std::size_t>(std::max(0, wl_display_dispatch_pending(m_display)));
    handled += static_cast<std::size_t>(std::max(0, wl_display_flush(m_display)));
    return handled;
}

void WaylandPlatform::pump() { (void)dispatchPending(); }

std::size_t WaylandPlatform::wait(std::chrono::milliseconds timeout) {
    if (m_display == nullptr) {
        return 0;
    }
    (void)dispatchPending();
    if (!m_queue.empty()) {
        return m_queue.size();
    }

    // Blocking read: prepare, poll with the requested timeout, then either read
    // or cancel - the only correct way to wait on a wl_display from one thread.
    if (wl_display_prepare_read(m_display) != 0) {
        (void)wl_display_dispatch_pending(m_display);
        return m_queue.size();
    }
    if (wl_display_flush(m_display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(m_display);
        m_fatal = Error::platform("wayland: wl_display_flush failed", static_cast<int64_t>(errno));
        return m_queue.size();
    }
    pollfd descriptor{};
    descriptor.fd = wl_display_get_fd(m_display);
    descriptor.events = POLLIN;
    int waitMs = -1;
    if (timeout.count() >= 0) {
        waitMs = static_cast<int>(std::min<int64_t>(timeout.count(), 60 * 1000));
    }
    const int ready = ::poll(&descriptor, 1, waitMs);
    if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
        if (wl_display_read_events(m_display) < 0) {
            m_fatal = Error::platform("wayland: wl_display_read_events failed", static_cast<int64_t>(errno));
            return m_queue.size();
        }
        (void)wl_display_dispatch_pending(m_display);
    } else {
        wl_display_cancel_read(m_display);
    }
    return m_queue.size();
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

Result<std::unique_ptr<WindowBackend>> WaylandPlatform::createWindow(const WindowCreateInfo& info) {
    if (m_display == nullptr) {
        return fail<std::unique_ptr<WindowBackend>>(
            Error{Errc::ConnectionFailed, "createWindow: no Wayland connection"});
    }
    if (!isUiThread()) {
        return fail<std::unique_ptr<WindowBackend>>(Error{
            Errc::PlatformFailure, "createWindow: Wayland objects are bound to the thread that created the Platform"});
    }
    if (!m_hasXdgShell) {
        return fail<std::unique_ptr<WindowBackend>>(Error::unsupported(
            "createWindow: this build has no xdg-shell protocol code (install wayland-protocols and rebuild)"));
    }

    const WindowId id = nextWindowId();
    auto window = std::make_unique<WaylandWindow>(*this, id, info);
    if (window->surface() == nullptr) {
        return fail<std::unique_ptr<WindowBackend>>(
            Error{Errc::PlatformFailure, "createWindow: wl_compositor.create_surface failed"});
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

void WaylandPlatform::windowDestroyed(WindowId id) {
    std::lock_guard lock(m_windowsMutex);
    const auto it = std::find_if(m_windows.begin(), m_windows.end(),
                                 [id](const WaylandWindow* window) { return window->id() == id; });
    if (it != m_windows.end()) {
        if (m_pointerFocus == *it) {
            m_pointerFocus = nullptr;
        }
        if (m_keyboardFocusWindow == *it) {
            m_keyboardFocusWindow = nullptr;
        }
        m_windows.erase(it);
    }
}

void WaylandPlatform::destroyWindow(WindowBackend* window) {
    if (window != nullptr) {
        windowDestroyed(window->id());
    }
}

WaylandWindow* WaylandPlatform::windowForSurface(wl_surface* surface) const {
    if (surface == nullptr) {
        return nullptr;
    }
    std::lock_guard lock(m_windowsMutex);
    for (WaylandWindow* window : m_windows) {
        if (window->surface() == surface) {
            return window;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Input routing
// ---------------------------------------------------------------------------

void WaylandPlatform::setPointerFocus(WaylandWindow* window, uint32_t enterSerial) {
    m_pointerFocus = window;
    m_enterSerial = enterSerial;
    m_lastSerial = enterSerial;
    if (window != nullptr) {
        window->onPointerEnterSerial(enterSerial);
    }
}

void WaylandPlatform::onPointerPosition(double x, double y) {
    if (m_pointerFocus == nullptr) {
        return;
    }
    const Point position{static_cast<int32_t>(std::lround(x)), static_cast<int32_t>(std::lround(y))};
    m_pointerPosition = position;

    MouseMoveEvent payload{};
    payload.position = position;
    payload.delta = Point{position.x - m_lastPointer.x, position.y - m_lastPointer.y};
    payload.modifiers = m_modifiers;
    payload.isDragging = anyButtonPressed();
    m_hasLastPointer = true;
    m_lastPointer = position;

    Event event = makeEvent(EventType::MouseMoved, m_pointerFocus->id());
    event.payload = payload;
    m_queue.push(std::move(event));
}

void WaylandPlatform::onPointerLeave() {
    if (m_pointerFocus == nullptr) {
        return;
    }
    MouseEnterEvent payload{};
    payload.position = m_pointerPosition;
    payload.entered = false;
    Event event = makeEvent(EventType::MouseLeft, m_pointerFocus->id());
    event.payload = payload;
    m_queue.push(std::move(event));
    m_pointerFocus = nullptr;
}

void WaylandPlatform::trackPressedButton(uint32_t button, bool pressed) {
    const uint32_t bit = 1u << (button & 0x1Fu);
    if (pressed) {
        m_pressedButtons |= bit;
    } else {
        m_pressedButtons &= ~bit;
    }
}

void WaylandPlatform::onPointerButton(uint32_t button, uint32_t state) {
    if (m_pointerFocus == nullptr) {
        return;
    }
    const bool pressed = waylandKeyIsPressed(state);
    trackPressedButton(button, pressed);

    MouseButtonEvent payload{};
    payload.button = mouseButtonFromEvdev(button);
    payload.position = m_pointerPosition;
    payload.modifiers = m_modifiers;
    payload.clickCount = 1;

    Event event = makeEvent(pressed ? EventType::MouseDown : EventType::MouseUp, m_pointerFocus->id());
    event.payload = payload;
    m_queue.push(std::move(event));
}

void WaylandPlatform::onPointerAxis(uint32_t axis, double value, bool discrete) {
    (void)discrete;
    // wl_pointer.axis carries the value in the surface's logical space; the sign
    // already means "content moves" for a natural scroll, matching nativeBridge's
    // convention (positive = content moves up/right).
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        m_pendingScrollY += value;
    } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        m_pendingScrollX += value;
    }
    // wl_pointer.axis delivers pixels; a wheel notch is reported as ~15 px by
    // most compositors, so convert to nativeBridge lines using the documented
    // ratio and mark trackpad (continuous) sources as precise.
    m_pendingScrollPrecise = (m_axisSource == WL_POINTER_AXIS_SOURCE_FINGER) ||
                             (m_axisSource == WL_POINTER_AXIS_SOURCE_CONTINUOUS);
}

void WaylandPlatform::onPointerAxisDiscrete(uint32_t axis, int32_t steps) {
    const double lines = static_cast<double>(steps) * static_cast<double>(m_scrollLinesPerNotch);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        m_pendingScrollY += lines;
    } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        m_pendingScrollX += lines;
    }
    m_pendingScrollPrecise = false;
}

void WaylandPlatform::flushPointerEvents() {
    if (m_pointerFocus == nullptr) {
        m_pendingScrollX = 0.0;
        m_pendingScrollY = 0.0;
        return;
    }
    if (m_pendingScrollX == 0.0 && m_pendingScrollY == 0.0) {
        return;
    }
    MouseWheelEvent payload{};
    payload.position = m_pointerPosition;
    if (m_pendingScrollPrecise) {
        // Continuous (trackpad) sources report pixels; convert to lines with the
        // conventional 15 px per line.
        constexpr double kPixelsPerLine = 15.0;
        payload.deltaX = m_pendingScrollX / kPixelsPerLine;
        payload.deltaY = m_pendingScrollY / kPixelsPerLine;
    } else {
        payload.deltaX = m_pendingScrollX;
        payload.deltaY = m_pendingScrollY;
    }
    payload.precise = m_pendingScrollPrecise;
    payload.modifiers = m_modifiers;

    m_pendingScrollX = 0.0;
    m_pendingScrollY = 0.0;

    Event event = makeEvent(EventType::MouseWheel, m_pointerFocus->id());
    event.payload = payload;
    m_queue.push(std::move(event));
}

void WaylandPlatform::onKeyboardFocus(WaylandWindow* window, bool entered) {
    if (window != nullptr) {
        m_keyboardFocusWindow = window;
        const FocusEvent payload{true};
        Event event = makeEvent(EventType::FocusGained, window->id());
        event.payload = payload;
        m_queue.push(std::move(event));
        return;
    }
    if (m_keyboardFocusWindow != nullptr) {
        const FocusEvent payload{false};
        Event event = makeEvent(EventType::FocusLost, m_keyboardFocusWindow->id());
        event.payload = payload;
        m_queue.push(std::move(event));
    }
    m_keyboardFocusWindow = nullptr;
}

void WaylandPlatform::onKeyboardKey(uint32_t evdevKey, uint32_t state) {
    if (m_keyboardFocusWindow == nullptr) {
        return;
    }
    const bool pressed = waylandKeyIsPressed(state);

    KeyEvent payload{};
    payload.key = waylandKeyCodeFromEvdev(evdevKey);
    payload.nativeScanCode = evdevKey;
    payload.keysym = waylandKeysymFromEvdev(evdevKey);
    payload.nativeKeyCode = payload.keysym;
    payload.modifiers = m_modifiers;
    payload.action = pressed ? KeyAction::Press : KeyAction::Release;
    payload.character = pressed ? waylandCharacterFromEvdev(evdevKey) : 0;

    Event key = makeEvent(pressed ? EventType::KeyDown : EventType::KeyUp, m_keyboardFocusWindow->id());
    key.payload = payload;
    m_queue.push(std::move(key));

    if (pressed) {
        const uint32_t character = waylandCharacterFromEvdev(evdevKey);
        if (character >= 0x20 && character != 0x7F) {
            TextInputEvent text{};
            if (character < 0x80) {
                text.utf8.push_back(static_cast<char>(character));
            } else if (character <= 0x7FF) {
                text.utf8.push_back(static_cast<char>(0xC0 | (character >> 6)));
                text.utf8.push_back(static_cast<char>(0x80 | (character & 0x3F)));
            } else {
                text.utf8.push_back(static_cast<char>(0xE0 | (character >> 12)));
                text.utf8.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
                text.utf8.push_back(static_cast<char>(0x80 | (character & 0x3F)));
            }
            Event textEvent = makeEvent(EventType::TextInput, m_keyboardFocusWindow->id());
            textEvent.payload = std::move(text);
            m_queue.push(std::move(textEvent));
        }
    }
}

void WaylandPlatform::onKeyboardModifiers(uint32_t depressed, uint32_t locked) {
    m_modifiers = modifiersFromXkbState(depressed, locked);
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

Clipboard& WaylandPlatform::clipboard() {
    if (m_clipboardFacade == nullptr) {
        m_clipboardFacade = std::make_unique<WaylandClipboard>(*this);
    }
    return *m_clipboardFacade;
}

void WaylandPlatform::onSelectionOffer(wl_data_offer* offer) {
    if (m_selectionOffer == offer) {
        return;
    }
    // The previous offer must be released before the new one is adopted.
    if (m_selectionOffer != nullptr) {
        wl_data_offer_destroy(m_selectionOffer);
    }
    m_selectionOffer = offer;
    m_selectionMimeType.clear();
    m_offerHasText = false;
    m_ownSelection = false;

    if (offer == nullptr) {
        return;
    }
    wl_data_offer_add_listener(offer, &kDataOfferListener, this);
}

void WaylandPlatform::onDataOfferMimeType(const char* mimeType) {
    if (mimeType == nullptr || m_selectionOffer == nullptr) {
        return;
    }
    const std::string_view mime(mimeType);
    for (const char* candidate : kTextMimeTypes) {
        if (mime == candidate) {
            // Prefer the first (best) flavour in kTextMimeTypes.
            if (!m_offerHasText || m_selectionMimeType.empty()) {
                m_selectionMimeType = mimeType;
                m_offerHasText = true;
            }
            return;
        }
    }
}

Result<std::string> WaylandPlatform::clipboardGetText() {
    if (m_display == nullptr) {
        return fail<std::string>(Error{Errc::ConnectionFailed, "clipboard: no Wayland connection"});
    }
    if (!isUiThread()) {
        return fail<std::string>(Error{Errc::PlatformFailure,
                                       "clipboard: getText must run on the thread that created the Platform"});
    }
    if (m_ownSelection) {
        // We are the owner: serve the copy we hold, no round trip needed.
        return m_ownClipboardText;
    }
    if (m_selectionOffer == nullptr) {
        // Receive the offer list if it has not been dispatched yet.
        (void)dispatchPending();
    }
    if (m_selectionOffer == nullptr) {
        return fail<std::string>(Error{Errc::NotFound, "clipboard: the clipboard is empty"});
    }
    if (!m_offerHasText) {
        return fail<std::string>(Error{Errc::NotFound, "clipboard: the selection holds no text flavour"});
    }
    if (!hasKeyboardFocus()) {
        // Documented limitation 3: without keyboard focus the compositor will not
        // service the request, so fail early with a precise reason.
        return fail<std::string>(
            Error{Errc::PermissionDenied, "clipboard: reading requires keyboard focus on Wayland"});
    }

    // Create the pipe ourselves, hand the write end to the compositor and read
    // from the read end. pipe2 with O_CLOEXEC is Linux specific, which is fine
    // for this backend.
    int fds[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0) {
#else
    if (::pipe(fds) != 0) {
#endif
        return fail<std::string>(Error::platform("clipboard: pipe failed", static_cast<int64_t>(errno)));
    }
    wl_data_offer_receive(m_selectionOffer, m_selectionMimeType.c_str(), fds[1]);
    (void)wl_display_flush(m_display);
    ::close(fds[1]); // the compositor owns the write end now

    // Read until EOF, keeping the connection alive so the transfer can complete.
    std::string data;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::read(fds[0], buffer, sizeof(buffer));
        if (count > 0) {
            data.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Give the compositor a chance to write more.
            (void)dispatchPending();
            continue;
        }
        break;
    }
    ::close(fds[0]);
    if (data.empty()) {
        return fail<std::string>(Error{Errc::NotFound, "clipboard: the selection owner returned no data"});
    }
    return data;
}

Status WaylandPlatform::clipboardSetText(std::string_view utf8Text) {
    if (m_display == nullptr || m_dataDevice == nullptr) {
        return failStatus(Errc::ConnectionFailed, "clipboard: no Wayland data device");
    }
    if (!isUiThread()) {
        return failStatus(Errc::PlatformFailure,
                          "clipboard: setText must run on the thread that created the Platform");
    }
    if (!hasKeyboardFocus()) {
        // Documented limitation 3.
        return failStatus(Errc::PermissionDenied, "clipboard: setting the selection requires keyboard focus on Wayland");
    }

    m_ownClipboardText.assign(utf8Text);
    if (m_dataSource != nullptr) {
        wl_data_source_destroy(m_dataSource);
        m_dataSource = nullptr;
    }
    m_dataSource = wl_data_device_manager_create_data_source(m_dataDeviceManager);
    if (m_dataSource == nullptr) {
        return failStatus(Errc::PlatformFailure, "clipboard: create_data_source failed");
    }
    wl_data_source_add_listener(m_dataSource, &kDataSourceListener, this);
    for (const char* mime : kTextMimeTypes) {
        wl_data_source_offer(m_dataSource, mime);
    }
    // The compositor copies the data when a paste happens, so ownership is
    // announced with the serial of a real input event (0 would be rejected).
    wl_data_device_set_selection(m_dataDevice, m_dataSource, m_lastSerial);
    m_ownSelection = true;
    (void)wl_display_flush(m_display);
    return okStatus();
}

Status WaylandPlatform::clipboardClear() {
    if (m_display == nullptr || m_dataDevice == nullptr) {
        return failStatus(Errc::ConnectionFailed, "clipboard: no Wayland data device");
    }
    m_ownClipboardText.clear();
    m_ownSelection = false;
    wl_data_device_set_selection(m_dataDevice, nullptr, m_lastSerial);
    (void)wl_display_flush(m_display);
    return okStatus();
}

bool WaylandPlatform::clipboardHasText() {
    if (m_ownSelection) {
        return true;
    }
    return m_selectionOffer != nullptr && m_offerHasText;
}

void WaylandPlatform::onDataSourceSend(const char* mimeType, int32_t fd) {
    if (fd < 0) {
        return;
    }
    const std::string_view requested = mimeType != nullptr ? std::string_view(mimeType) : std::string_view{};
    bool accepted = false;
    for (const char* candidate : kTextMimeTypes) {
        if (requested == candidate) {
            accepted = true;
            break;
        }
    }
    if (accepted) {
        (void)writeAllToFd(fd, m_ownClipboardText);
    }
    ::close(fd);
}

void WaylandPlatform::onDataSourceCancelled(wl_data_source* source) {
    if (source == m_dataSource) {
        m_ownSelection = false;
        m_ownClipboardText.clear();
    }
}

// ---------------------------------------------------------------------------
// WaylandWindow
// ---------------------------------------------------------------------------

WaylandWindow::WaylandWindow(WaylandPlatform& platform, WindowId id, const WindowCreateInfo& info)
    : m_platform(platform), m_id(id), m_minSize(info.minSize), m_maxSize(info.maxSize),
      m_cursorShape(info.cursor) {
    m_title.set(std::string(info.title));
    m_pendingWidth = info.clientSize.width;
    m_pendingHeight = info.clientSize.height;
    m_clientSize.set(info.clientSize);
    m_scaleFactor.set(platform.sessionScaleFactor());
    m_lastScale = platform.sessionScaleFactor();

    createSurface();
    if (m_surface != nullptr && info.visible) {
        wl_surface_commit(m_surface);
        m_visible.set(true);
    }
}

WaylandWindow::~WaylandWindow() {
    destroySurface();
    notifyDestroyed(m_platform.lifetime(), m_id);
}

void WaylandWindow::createSurface() {
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    wl_compositor* compositor = m_platform.compositor();
    xdg_wm_base* wmBase = m_platform.wmBase();
    if (compositor == nullptr || wmBase == nullptr) {
        return;
    }
    m_surface = wl_compositor_create_surface(compositor);
    if (m_surface == nullptr) {
        log::error("wayland: wl_compositor.create_surface failed");
        return;
    }
    m_xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, m_surface);
    if (m_xdgSurface == nullptr) {
        return;
    }
    xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);
    m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
    if (m_toplevel == nullptr) {
        return;
    }
    xdg_toplevel_add_listener(m_toplevel, &kToplevelListener, this);
    xdg_toplevel_set_title(m_toplevel, m_title.get().c_str());
    xdg_toplevel_set_app_id(m_toplevel, "nativebridge.example");
    if (m_minSize.width > 0 || m_minSize.height > 0) {
        xdg_toplevel_set_min_size(m_toplevel, m_minSize.width, m_minSize.height);
    }
    if (m_maxSize.width > 0 || m_maxSize.height > 0) {
        xdg_toplevel_set_max_size(m_toplevel, m_maxSize.width, m_maxSize.height);
    }

    // Announce the outputs we know about. This is only a hint (the compositor
    // decides which output the surface is on); the real scale factor arrives
    // later as a wl_surface.enter event, which is handled by the platform.
    for (const WaylandOutputState& output : m_platform.outputsForWindow()) {
        if (output.output != nullptr) {
            wl_surface_enter_output(m_surface, output.output);
        }
    }
    // The initial commit maps the surface; the configure event follows and must
    // be acknowledged before the first buffer is attached.
    wl_surface_commit(m_surface);
    if (wl_display_roundtrip(m_platform.display()) < 0) {
        log::warn("wayland: roundtrip while mapping the window failed");
    }
#endif
}

void WaylandWindow::destroySurface() {
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_toplevel != nullptr) {
        xdg_toplevel_destroy(m_toplevel);
        m_toplevel = nullptr;
    }
    if (m_xdgSurface != nullptr) {
        xdg_surface_destroy(m_xdgSurface);
        m_xdgSurface = nullptr;
    }
#endif
    if (m_surface != nullptr) {
        wl_surface_destroy(m_surface);
        m_surface = nullptr;
    }
}

bool WaylandWindow::supports(Capability capability) const noexcept {
    // WaylandWindow forwards to the platform except for the two capabilities that
    // are per backend constants, so the two layers can never disagree.
    if (capability == Capability::WindowPosition || capability == Capability::WindowDecoration) {
        return false;
    }
    return m_platform.supports(capability);
}

WindowNativeHandle WaylandWindow::nativeHandle() const {
    WindowNativeHandle handle{};
    if (m_surface != nullptr) {
        handle.window = NativeHandle{NativeHandle::Kind::WaylandSurface,
                                     reinterpret_cast<uintptr_t>(m_surface)};
        handle.view = handle.window;
    }
    if (m_platform.display() != nullptr) {
        handle.instance = NativeHandle{NativeHandle::Kind::WaylandDisplay,
                                       reinterpret_cast<uintptr_t>(m_platform.display())};
    }
    return handle;
}

void WaylandWindow::onConfigure(int32_t width, int32_t height, bool maximized, bool fullscreen, bool resizing) {
    m_configured = true;

    if (width > 0 && height > 0) {
        m_pendingWidth = width;
        m_pendingHeight = height;
    }
    // A zero size in the configure event means "you choose"; keep the requested
    // size so the window does not collapse.
    if (m_pendingWidth <= 0 || m_pendingHeight <= 0) {
        m_pendingWidth = std::max(m_pendingWidth, 1);
        m_pendingHeight = std::max(m_pendingHeight, 1);
    }

    const bool sizeChanged = m_clientSize.get().width != m_pendingWidth ||
                             m_clientSize.get().height != m_pendingHeight;
    m_clientSize.set(Size{m_pendingWidth, m_pendingHeight});

    if (maximized != m_maximized.get()) {
        m_maximized.set(maximized);
        StateEvent payload{};
        payload.active = maximized;
        Event state = makeEvent(maximized ? EventType::Maximized : EventType::Restored, m_id);
        state.payload = payload;
        m_platform.queue().push(std::move(state));
    }
    if (fullscreen != m_fullscreen.get()) {
        m_fullscreen.set(fullscreen);
        StateEvent payload{};
        payload.active = fullscreen;
        Event state = makeEvent(fullscreen ? EventType::Maximized : EventType::Restored, m_id);
        state.payload = payload;
        m_platform.queue().push(std::move(state));
    }
    if (maximized || fullscreen) {
        m_minimized.set(false);
    }

    if (sizeChanged) {
        pushResize();
    }
    (void)resizing;
}

void WaylandWindow::pushResize() {
    ResizeEvent payload{};
    payload.clientSize = m_clientSize.get();
    payload.windowSize = m_clientSize.get(); // equal on Wayland
    payload.framebufferSize = framebufferSize();
    payload.inLiveResize = false;
    Event event = makeEvent(EventType::Resized, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void WaylandWindow::onCloseRequested() {
    CloseRequestedEvent payload{};
    // xdg_toplevel.close cannot be vetoed: the compositor has already decided.
    payload.cancellable = false;
    Event event = makeEvent(EventType::CloseRequested, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
}

void WaylandWindow::onEnterOutput(const WaylandOutputState& output) {
    const double scale = output.scale > 0 ? static_cast<double>(output.scale) : 1.0;
    if (scale == m_lastScale) {
        return;
    }
    m_lastScale = scale;
    m_scaleFactor.set(scale);

    // Contract: ScaleFactorChanged is emitted before the following Resized event.
    ScaleFactorEvent scalePayload{};
    scalePayload.scaleFactor = scale;
    scalePayload.displayBounds = output.info.bounds;
    Event scaleEvent = makeEvent(EventType::ScaleFactorChanged, m_id);
    scaleEvent.payload = scalePayload;
    m_platform.queue().push(std::move(scaleEvent));

    pushResize();
}

void WaylandWindow::onSurfaceResized() {
    // Nothing to derive: Wayland sizes the surface from the configure event.
}

void WaylandWindow::onDisplayConfigurationChanged() {
    // The session scale may have changed (an output was added or rescaled).
    const double scale = m_platform.sessionScaleFactor();
    if (scale != m_lastScale) {
        m_lastScale = scale;
        m_scaleFactor.set(scale);
        ScaleFactorEvent payload{};
        payload.scaleFactor = scale;
        Event event = makeEvent(EventType::ScaleFactorChanged, m_id);
        event.payload = payload;
        m_platform.queue().push(std::move(event));
    }
    pushResize();
}

void WaylandWindow::onNativeDestroyed() {
    m_destroyed = true;
    m_surface = nullptr;
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    m_xdgSurface = nullptr;
    m_toplevel = nullptr;
#endif
}

// --- Commands ---------------------------------------------------------------

Status WaylandWindow::setTitle(std::string_view title) {
    if (m_surface == nullptr) {
        return failStatus(Errc::InvalidArgument, "setTitle: window is gone");
    }
    m_title.set(std::string(title));
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_toplevel != nullptr) {
        xdg_toplevel_set_title(m_toplevel, m_title.get().c_str());
        (void)wl_display_flush(m_platform.display());
        return okStatus();
    }
#endif
    return failStatus(Errc::Unsupported, "setTitle: no xdg_toplevel (xdg-shell unavailable)");
}

Status WaylandWindow::resize(Size clientSize) {
    if (m_surface == nullptr) {
        return failStatus(Errc::InvalidArgument, "resize: window is gone");
    }
    if (clientSize.width <= 0 || clientSize.height <= 0) {
        return failStatus(Errc::InvalidArgument, "resize: client size must be positive");
    }
    // Wayland has no resize request: the client states the size it is going to
    // use and the compositor answers with a configure event. The size is applied
    // immediately so the caller sees the effect without an event round trip.
    m_pendingWidth = clientSize.width;
    m_pendingHeight = clientSize.height;
    m_clientSize.set(clientSize);
    pushResize();
    return okStatus();
}

Status WaylandWindow::move(Point position) {
    (void)position;
    // Documented limitation 1: Wayland provides no request to position a
    // toplevel, so this reports Unsupported instead of pretending to work.
    return failStatus(Errc::Unsupported,
                      "move: Wayland does not allow clients to set a global window position");
}

Status WaylandWindow::setMinSize(Size size) {
    m_minSize = size;
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_toplevel != nullptr) {
        xdg_toplevel_set_min_size(m_toplevel, size.width, size.height);
        (void)wl_display_flush(m_platform.display());
        return okStatus();
    }
#endif
    return failStatus(Errc::Unsupported, "setMinSize: no xdg_toplevel");
}

Status WaylandWindow::setMaxSize(Size size) {
    m_maxSize = size;
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_toplevel != nullptr) {
        xdg_toplevel_set_max_size(m_toplevel, size.width, size.height);
        (void)wl_display_flush(m_platform.display());
        return okStatus();
    }
#endif
    return failStatus(Errc::Unsupported, "setMaxSize: no xdg_toplevel");
}

Status WaylandWindow::show() {
    if (m_surface == nullptr) {
        return failStatus(Errc::InvalidArgument, "show: window is gone");
    }
    wl_surface_commit(m_surface);
    (void)wl_display_flush(m_platform.display());
    m_visible.set(true);
    VisibilityEvent payload{};
    payload.visible = true;
    Event event = makeEvent(EventType::Shown, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
    return okStatus();
}

Status WaylandWindow::hide() {
    if (m_surface == nullptr) {
        return failStatus(Errc::InvalidArgument, "hide: window is gone");
    }
    // Attaching a null buffer unmaps the surface; there is no "unmap" request.
    wl_surface_attach(m_surface, nullptr, 0, 0);
    wl_surface_commit(m_surface);
    (void)wl_display_flush(m_platform.display());
    m_visible.set(false);
    VisibilityEvent payload{};
    payload.visible = false;
    Event event = makeEvent(EventType::Hidden, m_id);
    event.payload = payload;
    m_platform.queue().push(std::move(event));
    return okStatus();
}

Status WaylandWindow::minimize() {
    // xdg-shell deliberately has no minimize request: compositors decide when a
    // window is minimized (for example from the server side decoration).
    return failStatus(Errc::Unsupported, "minimize: xdg-shell has no minimize request");
}

Status WaylandWindow::maximize() {
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_toplevel == nullptr) {
        return failStatus(Errc::Unsupported, "maximize: no xdg_toplevel");
    }
    xdg_toplevel_set_maximized(m_toplevel);
    m_maximizedActive = true;
    (void)wl_display_flush(m_platform.display());
    return okStatus();
#else
    return failStatus(Errc::Unsupported, "maximize: xdg-shell unavailable");
#endif
}

Status WaylandWindow::restore() {
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_toplevel == nullptr) {
        return failStatus(Errc::Unsupported, "restore: no xdg_toplevel");
    }
    if (m_maximizedActive) {
        xdg_toplevel_unset_maximized(m_toplevel);
        m_maximizedActive = false;
    }
    if (m_fullscreenActive) {
        xdg_toplevel_unset_fullscreen(m_toplevel);
        m_fullscreenActive = false;
    }
    (void)wl_display_flush(m_platform.display());
    return okStatus();
#else
    return failStatus(Errc::Unsupported, "restore: xdg-shell unavailable");
#endif
}

Status WaylandWindow::setFullscreen(bool fullscreen) {
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_toplevel == nullptr) {
        return failStatus(Errc::Unsupported, "setFullscreen: no xdg_toplevel");
    }
    if (fullscreen) {
        xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
    } else {
        xdg_toplevel_unset_fullscreen(m_toplevel);
    }
    m_fullscreenActive = fullscreen;
    (void)wl_display_flush(m_platform.display());
    return okStatus();
#else
    return failStatus(Errc::Unsupported, "setFullscreen: xdg-shell unavailable");
#endif
}

Status WaylandWindow::setAlwaysOnTop(bool onTop) {
    (void)onTop;
    // xdg-shell has no "keep above" state; it needs wlr-layer-shell or a
    // compositor specific protocol, both out of scope.
    return failStatus(Errc::Unsupported, "setAlwaysOnTop: xdg-shell has no such state");
}

Status WaylandWindow::setDecorations(bool decorations) {
    (void)decorations;
    // Documented limitation 2: decorations belong to the compositor. Without the
    // optional xdg-decoration protocol there is nothing to negotiate.
    return failStatus(Errc::Unsupported,
                      "setDecorations: Wayland decorations are compositor owned (xdg-decoration is not wired)");
}

Status WaylandWindow::setResizable(bool resizable) {
    (void)resizable;
    // A client cannot forbid resizing: min == max is the closest approximation,
    // and nativeBridge does not silently clamp the user's window.
    return failStatus(Errc::Unsupported, "setResizable: xdg-shell has no resizable state");
}

Status WaylandWindow::setCursor(CursorShape shape) {
    if (shape != m_cursorShape) {
        m_cursorShape = shape;
        m_cursor.set(shape);
    }
    // Setting a cursor requires a wl_cursor from libwayland-cursor plus a fresh
    // input serial; without that dependency only the shape is recorded.
    if (m_platform.pointerFocus() != this) {
        return okStatus(); // recorded, applied when the pointer enters
    }
    return failStatus(Errc::Unsupported,
                      "setCursor: named cursors need libwayland-cursor (excluded by the dependency policy)");
}

Status WaylandWindow::setCursorVisible(bool visible) {
    m_cursorVisible = visible;
    if (!visible) {
        // Hiding the pointer is expressible with a null cursor surface only if
        // libwayland-cursor is available.
        return failStatus(Errc::Unsupported, "setCursorVisible(false): needs libwayland-cursor");
    }
    return okStatus();
}

Status WaylandWindow::setClipRect(const std::optional<Rect>& rect) {
    m_clipRect = rect;
    return okStatus();
}

Status WaylandWindow::requestAttention() {
    // xdg-activation is the protocol for this and is out of scope; without it the
    // only portable hint is a title change, which nativeBridge does not fake.
    return failStatus(Errc::Unsupported, "requestAttention: needs the xdg-activation protocol");
}

Status WaylandWindow::requestClose() {
#ifdef NATIVEBRIDGE_HAVE_XDG_SHELL
    if (m_toplevel == nullptr) {
        return failStatus(Errc::InvalidArgument, "requestClose: window is gone");
    }
    // There is no server side "close me" request; emit the same event the
    // compositor would, and let the application destroy the window.
    onCloseRequested();
    return okStatus();
#else
    return failStatus(Errc::Unsupported, "requestClose: xdg-shell unavailable");
#endif
}

Status WaylandWindow::setCustomCursor(std::span<const uint8_t> bgraPixels, Size size, Point hotspot) {
    (void)bgraPixels;
    (void)size;
    (void)hotspot;
    return failStatus(Errc::Unsupported,
                      "setCustomCursor: needs libwayland-cursor (excluded by the dependency policy)");
}

Status WaylandWindow::materializeSurface(RenderApi api, std::string_view backendName, NativeHandle& surface) {
    (void)backendName;
    if (m_surface == nullptr) {
        return failStatus(Errc::InvalidArgument, "materializeSurface: window is gone");
    }
    switch (api) {
        case RenderApi::Direct3D11:
        case RenderApi::Direct3D12:
            return failStatus(Errc::Unsupported, "materializeSurface: Direct3D is not available on Linux");
        case RenderApi::Metal:
            return failStatus(Errc::Unsupported, "materializeSurface: Metal is not available on Linux");
        default:
            // EGL/Vulkan/WebGPU create their own wl_egl_window or use the
            // wl_surface directly; nativeBridge deliberately does not link a
            // graphics API (see include/nativebridge/surface.h).
            surface = NativeHandle{NativeHandle::Kind::WaylandSurface,
                                   reinterpret_cast<uintptr_t>(m_surface)};
            return okStatus();
    }
}

// ---------------------------------------------------------------------------
// Factory and registration
// ---------------------------------------------------------------------------

Result<std::unique_ptr<PlatformBackend>> createWaylandPlatform() {
    auto platform = std::make_unique<WaylandPlatform>();
    if (platform->fatalError().has_value()) {
        return fail<std::unique_ptr<PlatformBackend>>(*platform->fatalError());
    }
    return static_cast<std::unique_ptr<PlatformBackend>>(std::move(platform));
}

namespace {

/// Referenced from src/common/platform.cpp so this object file stays linked and
/// the registration below always runs.
[[maybe_unused]] const bool kRegistered = [] {
    registerBackend("wayland", &createWaylandPlatform);
    return true;
}();

} // namespace

} // namespace nativebridge::detail

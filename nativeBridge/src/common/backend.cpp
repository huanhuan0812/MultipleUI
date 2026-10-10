// nativeBridge — platform independent implementation.
// SPDX-License-Identifier: MIT
//
// backend.cpp — logging, backend registry, keysym mapping, event introspection.

#include "common/backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nativebridge::detail {
namespace {

bool loggingEnabled() {
#if defined(NATIVEBRIDGE_ENABLE_LOGGING)
    return true;
#elif !defined(NDEBUG)
    return true;
#else
    // Opt-in at runtime in release builds.
    static const bool enabled = std::getenv("NATIVEBRIDGE_LOG") != nullptr;
    return enabled;
#endif
}

void emit(const char* level, std::string_view message) {
    if (!loggingEnabled()) {
        return;
    }
    std::fprintf(stderr, "[nativebridge][%s] %.*s\n", level, static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

} // namespace

namespace log {
void info(std::string_view message) { emit("info", message); }
void warn(std::string_view message) { emit("warn", message); }
void error(std::string_view message) { emit("error", message); }
} // namespace log

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

namespace {

struct Registry {
    std::mutex                                          mutex;
    std::vector<std::pair<std::string, BackendFactory>> entries;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

} // namespace

void registerBackend(std::string_view name, BackendFactory factory) {
    if (name.empty() || factory == nullptr) {
        return;
    }
    auto& reg = registry();
    std::lock_guard lock(reg.mutex);
    for (auto& [existing, _] : reg.entries) {
        if (existing == name) {
            return; // Already registered (defensive: TU could be linked twice).
        }
    }
    reg.entries.emplace_back(std::string(name), factory);
}

BackendFactory findBackend(std::string_view name) {
    auto& reg = registry();
    std::lock_guard lock(reg.mutex);
    for (auto& [existing, factory] : reg.entries) {
        if (existing == name) {
            return factory;
        }
    }
    return nullptr;
}

std::vector<std::string> registeredBackendNames() {
    auto& reg = registry();
    std::lock_guard lock(reg.mutex);
    std::vector<std::string> names;
    names.reserve(reg.entries.size());
    for (auto& [name, _] : reg.entries) {
        names.push_back(name);
    }
    return names;
}

std::string_view compiledDefaultLinuxBackend() {
#if defined(NATIVEBRIDGE_LINUX_BACKEND_X11)
    return "x11";
#elif defined(NATIVEBRIDGE_LINUX_BACKEND_WAYLAND)
    return "wayland";
#else
    return {};
#endif
}

// ---------------------------------------------------------------------------
// Keysym -> KeyCode mapping (shared by X11 and Wayland)
// ---------------------------------------------------------------------------
//
// The table is expressed in terms of latin-1 keysyms (the X11 keysym values for
// ASCII are identical to the ASCII code points) and the X11 "function" keysyms
// from the 0xFF00..0xFFFF range. Wayland clients derive the same keysyms from
// the evdev keycode with the standard xkb "evdev" ruleset, so one table serves
// both backends.

namespace {

constexpr uint32_t kXK_BackSpace   = 0xFF08;
constexpr uint32_t kXK_Tab         = 0xFF09;
constexpr uint32_t kXK_Return      = 0xFF0D;
constexpr uint32_t kXK_Pause       = 0xFF13;
constexpr uint32_t kXK_Scroll_Lock = 0xFF14;
constexpr uint32_t kXK_Sys_Req     = 0xFF15;
constexpr uint32_t kXK_Escape      = 0xFF1B;
constexpr uint32_t kXK_Delete      = 0xFFFF;
constexpr uint32_t kXK_Home        = 0xFF50;
constexpr uint32_t kXK_Left        = 0xFF51;
constexpr uint32_t kXK_Up          = 0xFF52;
constexpr uint32_t kXK_Right       = 0xFF53;
constexpr uint32_t kXK_Down        = 0xFF54;
constexpr uint32_t kXK_Prior       = 0xFF55; // Page Up
constexpr uint32_t kXK_Next        = 0xFF56; // Page Down
constexpr uint32_t kXK_End         = 0xFF57;
constexpr uint32_t kXK_Begin       = 0xFF58;
constexpr uint32_t kXK_Print       = 0xFF61;
constexpr uint32_t kXK_Insert      = 0xFF63;
constexpr uint32_t kXK_Menu        = 0xFF67;
constexpr uint32_t kXK_Num_Lock    = 0xFF7F;
constexpr uint32_t kXK_KP_Space    = 0xFF80;
constexpr uint32_t kXK_KP_Tab      = 0xFF89;
constexpr uint32_t kXK_KP_Enter    = 0xFF8D;
constexpr uint32_t kXK_KP_Home     = 0xFF95;
constexpr uint32_t kXK_KP_Left     = 0xFF96;
constexpr uint32_t kXK_KP_Up       = 0xFF97;
constexpr uint32_t kXK_KP_Right    = 0xFF98;
constexpr uint32_t kXK_KP_Down     = 0xFF99;
constexpr uint32_t kXK_KP_Prior    = 0xFF9A;
constexpr uint32_t kXK_KP_Next     = 0xFF9B;
constexpr uint32_t kXK_KP_End      = 0xFF9C;
constexpr uint32_t kXK_KP_Begin    = 0xFF9D;
constexpr uint32_t kXK_KP_Insert   = 0xFF9E;
constexpr uint32_t kXK_KP_Delete   = 0xFF9F;
constexpr uint32_t kXK_KP_Equal    = 0xFFBD;
constexpr uint32_t kXK_KP_Multiply = 0xFFAA;
constexpr uint32_t kXK_KP_Add      = 0xFFAB;
constexpr uint32_t kXK_KP_Separator = 0xFFAC;
constexpr uint32_t kXK_KP_Subtract = 0xFFAD;
constexpr uint32_t kXK_KP_Decimal  = 0xFFAE;
constexpr uint32_t kXK_KP_Divide   = 0xFFAF;
constexpr uint32_t kXK_KP_0        = 0xFFB0;
constexpr uint32_t kXK_KP_1        = 0xFFB1;
constexpr uint32_t kXK_KP_2        = 0xFFB2;
constexpr uint32_t kXK_KP_3        = 0xFFB3;
constexpr uint32_t kXK_KP_4        = 0xFFB4;
constexpr uint32_t kXK_KP_5        = 0xFFB5;
constexpr uint32_t kXK_KP_6        = 0xFFB6;
constexpr uint32_t kXK_KP_7        = 0xFFB7;
constexpr uint32_t kXK_KP_8        = 0xFFB8;
constexpr uint32_t kXK_KP_9        = 0xFFB9;
constexpr uint32_t kXK_F1          = 0xFFBE;
constexpr uint32_t kXK_F2          = 0xFFBF;
constexpr uint32_t kXK_F3          = 0xFFC0;
constexpr uint32_t kXK_F4          = 0xFFC1;
constexpr uint32_t kXK_F5          = 0xFFC2;
constexpr uint32_t kXK_F6          = 0xFFC3;
constexpr uint32_t kXK_F7          = 0xFFC4;
constexpr uint32_t kXK_F8          = 0xFFC5;
constexpr uint32_t kXK_F9          = 0xFFC6;
constexpr uint32_t kXK_F10         = 0xFFC7;
constexpr uint32_t kXK_F11         = 0xFFC8;
constexpr uint32_t kXK_F12         = 0xFFC9;
constexpr uint32_t kXK_F13         = 0xFFCA;
constexpr uint32_t kXK_F14         = 0xFFCB;
constexpr uint32_t kXK_F15         = 0xFFCC;
constexpr uint32_t kXK_F16         = 0xFFCD;
constexpr uint32_t kXK_F17         = 0xFFCE;
constexpr uint32_t kXK_F18         = 0xFFCF;
constexpr uint32_t kXK_F19         = 0xFFD0;
constexpr uint32_t kXK_F20         = 0xFFD1;
constexpr uint32_t kXK_F21         = 0xFFD2;
constexpr uint32_t kXK_F22         = 0xFFD3;
constexpr uint32_t kXK_F23         = 0xFFD4;
constexpr uint32_t kXK_F24         = 0xFFD5;
constexpr uint32_t kXK_Shift_L     = 0xFFE1;
constexpr uint32_t kXK_Shift_R     = 0xFFE2;
constexpr uint32_t kXK_Control_L   = 0xFFE3;
constexpr uint32_t kXK_Control_R   = 0xFFE4;
constexpr uint32_t kXK_Caps_Lock   = 0xFFE5;
constexpr uint32_t kXK_Shift_Lock  = 0xFFE6;
constexpr uint32_t kXK_Meta_L      = 0xFFE7;
constexpr uint32_t kXK_Meta_R      = 0xFFE8;
constexpr uint32_t kXK_Alt_L       = 0xFFE9;
constexpr uint32_t kXK_Alt_R       = 0xFFEA;
constexpr uint32_t kXK_Super_L     = 0xFFEB;
constexpr uint32_t kXK_Super_R     = 0xFFEC;
constexpr uint32_t kXK_Hyper_L     = 0xFFED;
constexpr uint32_t kXK_Hyper_R     = 0xFFEE;
constexpr uint32_t kXK_Mode_switch = 0xFF7E; // AltGr
constexpr uint32_t kXK_ISO_Level3_Shift = 0xFE03;
constexpr uint32_t kXK_Multi_key   = 0xFF20; // Compose
constexpr uint32_t kXK_Henkan      = 0xFF23;
constexpr uint32_t kXK_Muhenkan    = 0xFF22;
constexpr uint32_t kXK_Kana_Lock   = 0xFF2D;
constexpr uint32_t kXK_Hangul      = 0xFF31;
constexpr uint32_t kXK_Hangul_Hanja = 0xFF34;
constexpr uint32_t kXK_Zenkaku_Hankaku = 0xFF21;
constexpr uint32_t kXK_Undo        = 0xFF65;
constexpr uint32_t kXK_Redo        = 0xFF66;
constexpr uint32_t kXK_Find        = 0xFF68;
constexpr uint32_t kXK_Cancel      = 0xFF69;
constexpr uint32_t kXK_Help        = 0xFF6A;
constexpr uint32_t kXK_Break       = 0xFF6B;
constexpr uint32_t kXK_AudioMute   = 0x1008FF12;
constexpr uint32_t kXK_AudioLowerVolume = 0x1008FF11;
constexpr uint32_t kXK_AudioRaiseVolume = 0x1008FF13;
constexpr uint32_t kXK_AudioPlay   = 0x1008FF14;
constexpr uint32_t kXK_AudioStop   = 0x1008FF15;
constexpr uint32_t kXK_AudioPrev   = 0x1008FF16;
constexpr uint32_t kXK_AudioNext   = 0x1008FF17;
constexpr uint32_t kXK_Eject       = 0x1008FF2C;

struct KeysymEntry {
    uint32_t keysym;
    KeyCode  key;
};

constexpr std::pair<uint32_t, KeyCode> kKeysymTable[]{
    {kXK_BackSpace, KeyCode::Backspace},
    {kXK_Tab, KeyCode::Tab},
    {kXK_Return, KeyCode::Enter},
    {0xFF0A, KeyCode::Enter},          // Linefeed
    {kXK_Pause, KeyCode::Pause},
    {kXK_Scroll_Lock, KeyCode::ScrollLock},
    {kXK_Sys_Req, KeyCode::PrintScreen},
    {kXK_Escape, KeyCode::Escape},
    {kXK_Delete, KeyCode::Delete},
    {kXK_Home, KeyCode::Home},
    {kXK_Left, KeyCode::ArrowLeft},
    {kXK_Up, KeyCode::ArrowUp},
    {kXK_Right, KeyCode::ArrowRight},
    {kXK_Down, KeyCode::ArrowDown},
    {kXK_Prior, KeyCode::PageUp},
    {kXK_Next, KeyCode::PageDown},
    {kXK_End, KeyCode::End},
    {kXK_Begin, KeyCode::Home},
    {kXK_Print, KeyCode::PrintScreen},
    {kXK_Insert, KeyCode::Insert},
    {kXK_Menu, KeyCode::Application},
    {kXK_Num_Lock, KeyCode::NumLock},
    {kXK_KP_Space, KeyCode::Space},
    {kXK_KP_Tab, KeyCode::Tab},
    {kXK_KP_Enter, KeyCode::NumpadEnter},
    {kXK_KP_Home, KeyCode::Numpad7},
    {kXK_KP_Left, KeyCode::Numpad4},
    {kXK_KP_Up, KeyCode::Numpad8},
    {kXK_KP_Right, KeyCode::Numpad6},
    {kXK_KP_Down, KeyCode::Numpad2},
    {kXK_KP_Prior, KeyCode::Numpad9},
    {kXK_KP_Next, KeyCode::Numpad3},
    {kXK_KP_End, KeyCode::Numpad1},
    {kXK_KP_Begin, KeyCode::Numpad5},
    {kXK_KP_Insert, KeyCode::Numpad0},
    {kXK_KP_Delete, KeyCode::NumpadDecimal},
    {kXK_KP_Equal, KeyCode::NumpadEqual},
    {kXK_KP_Multiply, KeyCode::NumpadMultiply},
    {kXK_KP_Add, KeyCode::NumpadAdd},
    {kXK_KP_Separator, KeyCode::NumpadComma},
    {kXK_KP_Subtract, KeyCode::NumpadSubtract},
    {kXK_KP_Decimal, KeyCode::NumpadDecimal},
    {kXK_KP_Divide, KeyCode::NumpadDivide},
    {kXK_KP_0, KeyCode::Numpad0},
    {kXK_KP_1, KeyCode::Numpad1},
    {kXK_KP_2, KeyCode::Numpad2},
    {kXK_KP_3, KeyCode::Numpad3},
    {kXK_KP_4, KeyCode::Numpad4},
    {kXK_KP_5, KeyCode::Numpad5},
    {kXK_KP_6, KeyCode::Numpad6},
    {kXK_KP_7, KeyCode::Numpad7},
    {kXK_KP_8, KeyCode::Numpad8},
    {kXK_KP_9, KeyCode::Numpad9},
    {kXK_F1, KeyCode::F1},
    {kXK_F2, KeyCode::F2},
    {kXK_F3, KeyCode::F3},
    {kXK_F4, KeyCode::F4},
    {kXK_F5, KeyCode::F5},
    {kXK_F6, KeyCode::F6},
    {kXK_F7, KeyCode::F7},
    {kXK_F8, KeyCode::F8},
    {kXK_F9, KeyCode::F9},
    {kXK_F10, KeyCode::F10},
    {kXK_F11, KeyCode::F11},
    {kXK_F12, KeyCode::F12},
    {kXK_F13, KeyCode::F13},
    {kXK_F14, KeyCode::F14},
    {kXK_F15, KeyCode::F15},
    {kXK_F16, KeyCode::F16},
    {kXK_F17, KeyCode::F17},
    {kXK_F18, KeyCode::F18},
    {kXK_F19, KeyCode::F19},
    {kXK_F20, KeyCode::F20},
    {kXK_F21, KeyCode::F21},
    {kXK_F22, KeyCode::F22},
    {kXK_F23, KeyCode::F23},
    {kXK_F24, KeyCode::F24},
    {kXK_Shift_L, KeyCode::LeftShift},
    {kXK_Shift_R, KeyCode::RightShift},
    {kXK_Control_L, KeyCode::LeftControl},
    {kXK_Control_R, KeyCode::RightControl},
    {kXK_Caps_Lock, KeyCode::CapsLock},
    {kXK_Shift_Lock, KeyCode::CapsLock},
    {kXK_Meta_L, KeyCode::LeftGui},
    {kXK_Meta_R, KeyCode::RightGui},
    {kXK_Alt_L, KeyCode::LeftAlt},
    {kXK_Alt_R, KeyCode::RightAlt},
    {kXK_Super_L, KeyCode::LeftGui},
    {kXK_Super_R, KeyCode::RightGui},
    {kXK_Hyper_L, KeyCode::LeftGui},
    {kXK_Hyper_R, KeyCode::RightGui},
    {kXK_Mode_switch, KeyCode::RightAlt},
    {kXK_ISO_Level3_Shift, KeyCode::RightAlt},
    {kXK_Multi_key, KeyCode::Application},
    {kXK_Henkan, KeyCode::International4},
    {kXK_Muhenkan, KeyCode::International5},
    {kXK_Kana_Lock, KeyCode::Lang1},
    {kXK_Hangul, KeyCode::Lang1},
    {kXK_Hangul_Hanja, KeyCode::Lang2},
    {kXK_Zenkaku_Hankaku, KeyCode::International2},
    {kXK_Undo, KeyCode::Undo},
    {kXK_Redo, KeyCode::Again},
    {kXK_Find, KeyCode::Find},
    {kXK_Cancel, KeyCode::Stop},
    {kXK_Help, KeyCode::Help},
    {kXK_Break, KeyCode::Pause},
    {kXK_AudioMute, KeyCode::Mute},
    {kXK_AudioLowerVolume, KeyCode::VolumeDown},
    {kXK_AudioRaiseVolume, KeyCode::VolumeUp},
    {kXK_AudioPlay, KeyCode::MediaPlayPause},
    {kXK_AudioStop, KeyCode::MediaStop},
    {kXK_AudioPrev, KeyCode::MediaPrevious},
    {kXK_AudioNext, KeyCode::MediaNext},
    {kXK_Eject, KeyCode::MediaEject},
    {0x20, KeyCode::Space},
    {0x27, KeyCode::Apostrophe},
    {0x2C, KeyCode::Comma},
    {0x2D, KeyCode::Minus},
    {0x2E, KeyCode::Period},
    {0x2F, KeyCode::Slash},
    {0x30, KeyCode::Digit0},
    {0x31, KeyCode::Digit1},
    {0x32, KeyCode::Digit2},
    {0x33, KeyCode::Digit3},
    {0x34, KeyCode::Digit4},
    {0x35, KeyCode::Digit5},
    {0x36, KeyCode::Digit6},
    {0x37, KeyCode::Digit7},
    {0x38, KeyCode::Digit8},
    {0x39, KeyCode::Digit9},
    {0x3B, KeyCode::Semicolon},
    {0x3D, KeyCode::Equal},
    {0x5B, KeyCode::LeftBracket},
    {0x5C, KeyCode::Backslash},
    {0x5D, KeyCode::RightBracket},
    {0x60, KeyCode::Grave},
};

/// Lower case letters are mapped arithmetically in keyCodeFromKeysym().
constexpr KeyCode letterKeyCode(uint32_t asciiLower) noexcept {
    // asciiLower is 'a'..'z' -> KeyCode::A..Z which are 0x04..0x1D.
    return static_cast<KeyCode>(static_cast<uint32_t>(KeyCode::A) + (asciiLower - static_cast<uint32_t>('a')));
}

} // namespace

KeyCode keyCodeFromKeysym(uint32_t keysym) noexcept {
    if (keysym == 0) {
        return KeyCode::Unknown;
    }
    if (keysym >= static_cast<uint32_t>('a') && keysym <= static_cast<uint32_t>('z')) {
        return letterKeyCode(keysym);
    }
    if (keysym >= static_cast<uint32_t>('A') && keysym <= static_cast<uint32_t>('Z')) {
        return letterKeyCode(keysym - static_cast<uint32_t>('A') + static_cast<uint32_t>('a'));
    }
    for (const auto& [entryKeysym, entryKey] : kKeysymTable) {
        if (entryKeysym == keysym) {
            return entryKey;
        }
    }
    return KeyCode::Unknown;
}

uint32_t characterFromKeysym(uint32_t keysym) noexcept {
    switch (keysym) {
        case kXK_Return:
        case kXK_KP_Enter:
            return '\r';
        case kXK_Tab:
        case kXK_KP_Tab:
            return '\t';
        case kXK_BackSpace:
            return 0x08;
        case kXK_Escape:
            return 0x1B;
        default:
            break;
    }
    // Latin-1 keysyms are their own code points.
    if (keysym >= 0x20 && keysym <= 0x7E) {
        return keysym;
    }
    if (keysym >= 0xA0 && keysym <= 0xFF) {
        return keysym;
    }
    // Unicode keysyms: 0x01000000 | codepoint.
    if ((keysym & 0xFF000000u) == 0x01000000u) {
        return keysym & 0x00FFFFFFu;
    }
    return 0;
}

} // namespace nativebridge::detail

// ---------------------------------------------------------------------------
// Event introspection
// ---------------------------------------------------------------------------

namespace nativebridge {

std::string_view toString(EventType type) noexcept {
    switch (type) {
        case EventType::WindowCreated:      return "WindowCreated";
        case EventType::WindowDestroying:   return "WindowDestroying";
        case EventType::Resized:            return "Resized";
        case EventType::Moved:              return "Moved";
        case EventType::CloseRequested:     return "CloseRequested";
        case EventType::FocusGained:        return "FocusGained";
        case EventType::FocusLost:          return "FocusLost";
        case EventType::Shown:              return "Shown";
        case EventType::Hidden:             return "Hidden";
        case EventType::Minimized:          return "Minimized";
        case EventType::Restored:           return "Restored";
        case EventType::Maximized:          return "Maximized";
        case EventType::ScaleFactorChanged: return "ScaleFactorChanged";
        case EventType::KeyDown:            return "KeyDown";
        case EventType::KeyUp:              return "KeyUp";
        case EventType::TextInput:          return "TextInput";
        case EventType::MouseDown:          return "MouseDown";
        case EventType::MouseUp:            return "MouseUp";
        case EventType::MouseMoved:         return "MouseMoved";
        case EventType::MouseEntered:       return "MouseEntered";
        case EventType::MouseLeft:          return "MouseLeft";
        case EventType::MouseWheel:         return "MouseWheel";
        case EventType::DisplayChanged:     return "DisplayChanged";
        case EventType::All:                return "All";
    }
    return "Unknown";
}

std::string_view toString(KeyCode code) noexcept {
    switch (code) {
        case KeyCode::Unknown: return "Unknown";
        case KeyCode::A: return "A"; case KeyCode::B: return "B"; case KeyCode::C: return "C";
        case KeyCode::D: return "D"; case KeyCode::E: return "E"; case KeyCode::F: return "F";
        case KeyCode::G: return "G"; case KeyCode::H: return "H"; case KeyCode::I: return "I";
        case KeyCode::J: return "J"; case KeyCode::K: return "K"; case KeyCode::L: return "L";
        case KeyCode::M: return "M"; case KeyCode::N: return "N"; case KeyCode::O: return "O";
        case KeyCode::P: return "P"; case KeyCode::Q: return "Q"; case KeyCode::R: return "R";
        case KeyCode::S: return "S"; case KeyCode::T: return "T"; case KeyCode::U: return "U";
        case KeyCode::V: return "V"; case KeyCode::W: return "W"; case KeyCode::X: return "X";
        case KeyCode::Y: return "Y"; case KeyCode::Z: return "Z";
        case KeyCode::Digit0: return "Digit0"; case KeyCode::Digit1: return "Digit1";
        case KeyCode::Digit2: return "Digit2"; case KeyCode::Digit3: return "Digit3";
        case KeyCode::Digit4: return "Digit4"; case KeyCode::Digit5: return "Digit5";
        case KeyCode::Digit6: return "Digit6"; case KeyCode::Digit7: return "Digit7";
        case KeyCode::Digit8: return "Digit8"; case KeyCode::Digit9: return "Digit9";
        case KeyCode::Enter: return "Enter";
        case KeyCode::Escape: return "Escape";
        case KeyCode::Backspace: return "Backspace";
        case KeyCode::Tab: return "Tab";
        case KeyCode::Space: return "Space";
        case KeyCode::Minus: return "Minus";
        case KeyCode::Equal: return "Equal";
        case KeyCode::LeftBracket: return "LeftBracket";
        case KeyCode::RightBracket: return "RightBracket";
        case KeyCode::Backslash: return "Backslash";
        case KeyCode::NonUsHash: return "NonUsHash";
        case KeyCode::Semicolon: return "Semicolon";
        case KeyCode::Apostrophe: return "Apostrophe";
        case KeyCode::Grave: return "Grave";
        case KeyCode::Comma: return "Comma";
        case KeyCode::Period: return "Period";
        case KeyCode::Slash: return "Slash";
        case KeyCode::CapsLock: return "CapsLock";
        case KeyCode::F1: return "F1"; case KeyCode::F2: return "F2"; case KeyCode::F3: return "F3";
        case KeyCode::F4: return "F4"; case KeyCode::F5: return "F5"; case KeyCode::F6: return "F6";
        case KeyCode::F7: return "F7"; case KeyCode::F8: return "F8"; case KeyCode::F9: return "F9";
        case KeyCode::F10: return "F10"; case KeyCode::F11: return "F11"; case KeyCode::F12: return "F12";
        case KeyCode::F13: return "F13"; case KeyCode::F14: return "F14"; case KeyCode::F15: return "F15";
        case KeyCode::F16: return "F16"; case KeyCode::F17: return "F17"; case KeyCode::F18: return "F18";
        case KeyCode::F19: return "F19"; case KeyCode::F20: return "F20"; case KeyCode::F21: return "F21";
        case KeyCode::F22: return "F22"; case KeyCode::F23: return "F23"; case KeyCode::F24: return "F24";
        case KeyCode::PrintScreen: return "PrintScreen";
        case KeyCode::ScrollLock: return "ScrollLock";
        case KeyCode::Pause: return "Pause";
        case KeyCode::Insert: return "Insert";
        case KeyCode::Home: return "Home";
        case KeyCode::PageUp: return "PageUp";
        case KeyCode::Delete: return "Delete";
        case KeyCode::End: return "End";
        case KeyCode::PageDown: return "PageDown";
        case KeyCode::ArrowRight: return "ArrowRight";
        case KeyCode::ArrowLeft: return "ArrowLeft";
        case KeyCode::ArrowDown: return "ArrowDown";
        case KeyCode::ArrowUp: return "ArrowUp";
        case KeyCode::NumLock: return "NumLock";
        case KeyCode::NumpadDivide: return "NumpadDivide";
        case KeyCode::NumpadMultiply: return "NumpadMultiply";
        case KeyCode::NumpadSubtract: return "NumpadSubtract";
        case KeyCode::NumpadAdd: return "NumpadAdd";
        case KeyCode::NumpadEnter: return "NumpadEnter";
        case KeyCode::Numpad0: return "Numpad0"; case KeyCode::Numpad1: return "Numpad1";
        case KeyCode::Numpad2: return "Numpad2"; case KeyCode::Numpad3: return "Numpad3";
        case KeyCode::Numpad4: return "Numpad4"; case KeyCode::Numpad5: return "Numpad5";
        case KeyCode::Numpad6: return "Numpad6"; case KeyCode::Numpad7: return "Numpad7";
        case KeyCode::Numpad8: return "Numpad8"; case KeyCode::Numpad9: return "Numpad9";
        case KeyCode::NumpadDecimal: return "NumpadDecimal";
        case KeyCode::NumpadEqual: return "NumpadEqual";
        case KeyCode::NumpadComma: return "NumpadComma";
        case KeyCode::NonUsBackslash: return "NonUsBackslash";
        case KeyCode::Application: return "Application";
        case KeyCode::Power: return "Power";
        case KeyCode::Execute: return "Execute";
        case KeyCode::Help: return "Help";
        case KeyCode::Menu: return "Menu";
        case KeyCode::Select: return "Select";
        case KeyCode::Stop: return "Stop";
        case KeyCode::Again: return "Again";
        case KeyCode::Undo: return "Undo";
        case KeyCode::Cut: return "Cut";
        case KeyCode::Copy: return "Copy";
        case KeyCode::Paste: return "Paste";
        case KeyCode::Find: return "Find";
        case KeyCode::Mute: return "Mute";
        case KeyCode::VolumeUp: return "VolumeUp";
        case KeyCode::VolumeDown: return "VolumeDown";
        case KeyCode::International1: return "International1";
        case KeyCode::International2: return "International2";
        case KeyCode::International3: return "International3";
        case KeyCode::International4: return "International4";
        case KeyCode::International5: return "International5";
        case KeyCode::International6: return "International6";
        case KeyCode::Lang1: return "Lang1"; case KeyCode::Lang2: return "Lang2";
        case KeyCode::Lang3: return "Lang3"; case KeyCode::Lang4: return "Lang4";
        case KeyCode::Lang5: return "Lang5";
        case KeyCode::NumpadLeftParen: return "NumpadLeftParen";
        case KeyCode::NumpadRightParen: return "NumpadRightParen";
        case KeyCode::MediaPlayPause: return "MediaPlayPause";
        case KeyCode::MediaStop: return "MediaStop";
        case KeyCode::MediaNext: return "MediaNext";
        case KeyCode::MediaPrevious: return "MediaPrevious";
        case KeyCode::MediaEject: return "MediaEject";
        case KeyCode::LeftControl: return "LeftControl";
        case KeyCode::LeftShift: return "LeftShift";
        case KeyCode::LeftAlt: return "LeftAlt";
        case KeyCode::LeftGui: return "LeftGui";
        case KeyCode::RightControl: return "RightControl";
        case KeyCode::RightShift: return "RightShift";
        case KeyCode::RightAlt: return "RightAlt";
        case KeyCode::RightGui: return "RightGui";
    }
    return "Unknown";
}

KeyCode mapVirtualKey(uint32_t platformKey) noexcept { return detail::keyCodeFromKeysym(platformKey); }

namespace {

std::string modifierString(Modifier modifiers) {
    std::string out;
    const auto add = [&out](std::string_view name) {
        if (!out.empty()) {
            out += '+';
        }
        out += name;
    };
    if (hasModifier(modifiers, Modifier::Shift)) { add("Shift"); }
    if (hasModifier(modifiers, Modifier::Control)) { add("Ctrl"); }
    if (hasModifier(modifiers, Modifier::Alt)) { add("Alt"); }
    if (hasModifier(modifiers, Modifier::Super)) { add("Super"); }
    if (hasModifier(modifiers, Modifier::CapsLock)) { add("Caps"); }
    if (hasModifier(modifiers, Modifier::NumLock)) { add("Num"); }
    return out.empty() ? std::string("none") : out;
}

std::string pointString(Point p) {
    return "(" + std::to_string(p.x) + "," + std::to_string(p.y) + ")";
}

std::string sizeString(Size s, double scale) {
    const PixelSize px = detail::logicalToPhysical(s, scale);
    return std::to_string(s.width) + "x" + std::to_string(s.height) + " logical (" +
           std::to_string(px.width) + "x" + std::to_string(px.height) + " px)";
}

} // namespace

std::string describe(const Event& event) {
    std::string out = std::string(toString(event.type));
    out += " window=" + std::to_string(event.window);

    switch (event.type) {
        case EventType::WindowCreated:
            if (const auto* e = event.asCreated()) {
                out += " client=" + sizeString(e->clientSize, e->scaleFactor) +
                       " scale=" + std::to_string(e->scaleFactor);
            }
            break;
        case EventType::Resized:
            if (const auto* e = event.asResize()) {
                out += " client=" + std::to_string(e->clientSize.width) + "x" + std::to_string(e->clientSize.height) +
                       " window=" + std::to_string(e->windowSize.width) + "x" + std::to_string(e->windowSize.height) +
                       " framebuffer=" + std::to_string(e->framebufferSize.width) + "x" +
                       std::to_string(e->framebufferSize.height) +
                       (e->inLiveResize ? " live" : "");
            }
            break;
        case EventType::Moved:
            if (const auto* e = event.asMove()) {
                out += " pos=" + pointString(e->position) +
                       (e->globalPositionKnown ? "" : " (position unavailable on this backend)");
            }
            break;
        case EventType::ScaleFactorChanged:
            if (const auto* e = event.asScaleFactor()) {
                out += " scale=" + std::to_string(e->scaleFactor) +
                       " display=" + std::to_string(e->displayBounds.width) + "x" +
                       std::to_string(e->displayBounds.height) + "@" +
                       std::to_string(e->displayBounds.x) + "," + std::to_string(e->displayBounds.y);
            }
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            if (const auto* e = event.asKey()) {
                out += " key=" + std::string(toString(e->key)) +
                       " action=" + (e->action == KeyAction::Press ? "press"
                                      : e->action == KeyAction::Repeat ? "repeat" : "release") +
                       " mods=" + modifierString(e->modifiers);
                if (e->character != 0 && e->character >= 0x20 && e->character < 0x7F) {
                    out += std::string(" char='") + static_cast<char>(e->character) + "'";
                }
            }
            break;
        case EventType::TextInput:
            if (const auto* e = event.asTextInput()) {
                out += " text=\"" + e->utf8 + "\"";
            }
            break;
        case EventType::MouseDown:
        case EventType::MouseUp:
            if (const auto* e = event.asMouseButton()) {
                out += " button=" + std::to_string(static_cast<int>(e->button)) +
                       " pos=" + pointString(e->position) +
                       " clicks=" + std::to_string(e->clickCount) +
                       " mods=" + modifierString(e->modifiers);
            }
            break;
        case EventType::MouseMoved:
            if (const auto* e = event.asMouseMove()) {
                out += " pos=" + pointString(e->position) +
                       " delta=" + pointString(e->delta) +
                       " mods=" + modifierString(e->modifiers) +
                       (e->isDragging ? " dragging" : "");
            }
            break;
        case EventType::MouseEntered:
        case EventType::MouseLeft:
            if (const auto* e = event.asMouseEnter()) {
                out += std::string(e->entered ? " entered" : " left") + " pos=" + pointString(e->position);
            }
            break;
        case EventType::MouseWheel:
            if (const auto* e = event.asMouseWheel()) {
                out += " delta=(" + std::to_string(e->deltaX) + "," + std::to_string(e->deltaY) + ")" +
                       (e->precise ? " precise" : "") + " pos=" + pointString(e->position);
            }
            break;
        case EventType::FocusGained:
        case EventType::FocusLost:
        case EventType::Shown:
        case EventType::Hidden:
        case EventType::Minimized:
        case EventType::Restored:
        case EventType::Maximized:
        case EventType::DisplayChanged:
        case EventType::WindowDestroying:
        case EventType::CloseRequested:
        case EventType::All: // never delivered as an event type
            break;
    }
    return out;
}

} // namespace nativebridge

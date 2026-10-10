// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// keycode.h — keyboard vocabulary and virtual-key mapping.
//
// THREADING MODEL
// ---------------
// Everything here is a compile-time constant or a pure function. Callable from
// any thread.
//
// MAPPING POLICY
// --------------
// KeyCode values are *logical* keys derived from the USB HID Usage Tables
// (Keyboard/Keypad page 0x07). Using HID usage ids as the canonical numbering
// keeps the mapping lossless on the three desktop platforms:
//
//   Windows : WM_KEYDOWN virtual key  -> KeyCode   (see mapVirtualKey)
//   macOS   : NSEvent keyCode (ANSI)  -> KeyCode   (see mapVirtualKey)
//   X11     : X11 keysym              -> KeyCode   (see mapVirtualKey)
//   Wayland : evdev keycode -> keysym -> KeyCode   (see mapVirtualKey)
//
// Platform-specific functions live in the platform translation units; the
// public API only ever exposes `nativebridge::KeyCode`.

#pragma once

#include <cstdint>
#include <string_view>

namespace nativebridge {

/// Logical (layout independent) keys, numbered after USB HID usage ids.
enum class KeyCode : uint32_t {
    Unknown = 0x00,

    // --- Letters (HID 0x04 .. 0x1D) --------------------------------------
    A = 0x04, B = 0x05, C = 0x06, D = 0x07, E = 0x08, F = 0x09, G = 0x0A,
    H = 0x0B, I = 0x0C, J = 0x0D, K = 0x0E, L = 0x0F, M = 0x10, N = 0x11,
    O = 0x12, P = 0x13, Q = 0x14, R = 0x15, S = 0x16, T = 0x17, U = 0x18,
    V = 0x19, W = 0x1A, X = 0x1B, Y = 0x1C, Z = 0x1D,

    // --- Digits (HID 0x1E .. 0x27) ---------------------------------------
    Digit1 = 0x1E, Digit2 = 0x1F, Digit3 = 0x20, Digit4 = 0x21, Digit5 = 0x22,
    Digit6 = 0x23, Digit7 = 0x24, Digit8 = 0x25, Digit9 = 0x26, Digit0 = 0x27,

    Enter = 0x28,
    Escape = 0x29,
    Backspace = 0x2A,
    Tab = 0x2B,
    Space = 0x2C,
    Minus = 0x2D,
    Equal = 0x2E,
    LeftBracket = 0x2F,
    RightBracket = 0x30,
    Backslash = 0x31,
    NonUsHash = 0x32,
    Semicolon = 0x33,
    Apostrophe = 0x34,
    Grave = 0x35,
    Comma = 0x36,
    Period = 0x37,
    Slash = 0x38,
    CapsLock = 0x39,

    // --- Function keys (HID 0x3A .. 0x45) --------------------------------
    F1 = 0x3A, F2 = 0x3B, F3 = 0x3C, F4 = 0x3D, F5 = 0x3E, F6 = 0x3F,
    F7 = 0x40, F8 = 0x41, F9 = 0x42, F10 = 0x43, F11 = 0x44, F12 = 0x45,

    PrintScreen = 0x46,
    ScrollLock = 0x47,
    Pause = 0x48,
    Insert = 0x49,
    Home = 0x4A,
    PageUp = 0x4B,
    Delete = 0x4C,
    End = 0x4D,
    PageDown = 0x4E,
    ArrowRight = 0x4F,
    ArrowLeft = 0x50,
    ArrowDown = 0x51,
    ArrowUp = 0x52,

    NumLock = 0x53,
    NumpadDivide = 0x54,
    NumpadMultiply = 0x55,
    NumpadSubtract = 0x56,
    NumpadAdd = 0x57,
    NumpadEnter = 0x58,
    Numpad1 = 0x59, Numpad2 = 0x5A, Numpad3 = 0x5B, Numpad4 = 0x5C,
    Numpad5 = 0x5D, Numpad6 = 0x5E, Numpad7 = 0x5F, Numpad8 = 0x60,
    Numpad9 = 0x61, Numpad0 = 0x62, NumpadDecimal = 0x63,
    NonUsBackslash = 0x64,
    Application = 0x65,
    Power = 0x66,
    NumpadEqual = 0x67,

    F13 = 0x68, F14 = 0x69, F15 = 0x6A, F16 = 0x6B, F17 = 0x6C, F18 = 0x6D,
    F19 = 0x6E, F20 = 0x6F, F21 = 0x70, F22 = 0x71, F23 = 0x72, F24 = 0x73,

    Execute = 0x74, Help = 0x75, Menu = 0x76, Select = 0x77, Stop = 0x78,
    Again = 0x79, Undo = 0x7A, Cut = 0x7B, Copy = 0x7C, Paste = 0x7D,
    Find = 0x7E, Mute = 0x7F, VolumeUp = 0x80, VolumeDown = 0x81,
    NumpadComma = 0x85,
    International1 = 0x87,   // Ro / ろ (JIS)
    International2 = 0x88,   // Katakana/Hiragana
    International3 = 0x89,   // Yen
    International4 = 0x8A,   // Henkan
    International5 = 0x8B,   // Muhenkan
    International6 = 0x8C,   // Numpad JP comma
    Lang1 = 0x90,            // Hangul / Kana
    Lang2 = 0x91,            // Hanja / Eisu
    Lang3 = 0x92,
    Lang4 = 0x93,
    Lang5 = 0x94,

    NumpadLeftParen = 0xB6,
    NumpadRightParen = 0xB7,

    // Consumer/media keys live in the HID 0xC0..0xCF range (0xE0..0xE7 is
    // reserved for the modifier keys below).
    MediaPlayPause = 0xC0,
    MediaStop = 0xC1,
    MediaNext = 0xC2,
    MediaPrevious = 0xC3,
    MediaEject = 0xC4,

    LeftControl = 0xE0,
    LeftShift = 0xE1,
    LeftAlt = 0xE2,
    LeftGui = 0xE3,   // Windows key / Command / Super
    RightControl = 0xE4,
    RightShift = 0xE5,
    RightAlt = 0xE6,  // AltGr
    RightGui = 0xE7,
};

/// Modifier key state bitmask (bit per modifier).
enum class Modifier : uint32_t {
    None    = 0u,
    Shift   = 1u << 0u,
    Control = 1u << 1u,
    Alt     = 1u << 2u,   ///< Left or right Alt / Option. AltGr is reported here too.
    Super   = 1u << 3u,   ///< Windows / Command / Super.
    CapsLock = 1u << 4u,
    NumLock  = 1u << 5u,
};

[[nodiscard]] constexpr Modifier operator|(Modifier a, Modifier b) noexcept {
    return static_cast<Modifier>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr Modifier operator&(Modifier a, Modifier b) noexcept {
    return static_cast<Modifier>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr Modifier& operator|=(Modifier& a, Modifier b) noexcept { a = a | b; return a; }

/// True when the given modifier bit is set.
[[nodiscard]] constexpr bool hasModifier(Modifier set, Modifier bit) noexcept {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(bit)) != 0u;
}

/// Key event action.
enum class KeyAction : uint8_t {
    Press,     ///< Key went down (auto-repeat excluded unless flag set).
    Repeat,    ///< Auto-repeat generated while the key is held.
    Release,   ///< Key went up.
};

/// Mouse buttons, normalized to the physical button index.
enum class MouseButton : uint8_t {
    Unknown = 0,
    Left = 1,
    Middle = 2,
    Right = 3,
    X1 = 4, ///< "Back" side button.
    X2 = 5, ///< "Forward" side button.
};

/// Stable textual name of a key (English, e.g. "A", "F5", "LeftShift").
[[nodiscard]] std::string_view toString(KeyCode code) noexcept;

/// Translate a platform virtual key/scancode/keysym into a KeyCode.
/// Returns KeyCode::Unknown when the mapping is unknown.
[[nodiscard]] KeyCode mapVirtualKey(uint32_t platformKey) noexcept;

} // namespace nativebridge

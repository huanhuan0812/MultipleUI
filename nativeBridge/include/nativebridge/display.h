// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// display.h — monitor / output enumeration.
//
// THREADING MODEL
// ---------------
// * DisplayInfo is a plain value type, safe to copy across threads.
// * Platform::displays() may be called from any thread. It is a *snapshot*
//   taken under the backend lock; it is refreshed on every pollEvents() pass,
//   and a DisplayChanged event is queued (window == 0) whenever the set of
//   monitors, their geometry or their scale factor changed.
// * Platform::primaryDisplay() returns the same snapshot, filtered.
//
// DPI / SCALE SEMANTICS
// ---------------------
// * bounds/workArea are logical units; the same space Window::position() uses.
// * scaleFactor = physical pixels per logical unit:
//     Windows -> monitor DPI / 96.0
//     macOS   -> backingScaleFactor (1.0 or 2.0, occasionally 1.5)
//     X11     -> derived from Xft.dpi (fallback 96) ; identical for all monitors
//     Wayland -> wl_output scale (integer) ; fractional scale needs
//                wp-fractional-scale-v1 and is reported as the integer scale
// * moving a window between displays with different scale factors makes the
//   backend emit exactly one ScaleFactorChanged event before the subsequent
//   Resize event, so renderers can recreate DPI dependent resources first.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nativebridge/geometry.h"

namespace nativebridge {

/// Opaque display identifier. Stable for the lifetime of the display, but not
/// across sessions; never a raw platform id.
using DisplayId = uint64_t;
inline constexpr DisplayId kInvalidDisplayId = 0;

/// A single monitor / output.
struct DisplayInfo {
    DisplayId   id{kInvalidDisplayId};
    std::string name;             ///< Human readable, UTF-8 (may be empty).
    bool        isPrimary{false};
    Rect        bounds{};         ///< Logical bounds in the virtual desktop space.
    Rect        workArea{};       ///< Logical bounds minus taskbar/dock/menubar.
    double      scaleFactor{1.0}; ///< Physical pixels per logical unit.
    PixelSize   pixelSize{};      ///< Physical resolution.
    uint32_t    refreshRateHz{0}; ///< 0 when unknown (common on Wayland w/o wl_output v4).
    uint32_t    bitDepth{0};      ///< Bits per pixel, 0 when unknown.
    /// Rotation in degrees (0/90/180/270), 0 when unknown/reported as identity.
    uint32_t    rotationDegrees{0};

    [[nodiscard]] bool isValid() const noexcept { return id != kInvalidDisplayId; }
};

/// Result wrapper for enumerating displays: never throws, empty vector = none.
using DisplayList = std::vector<DisplayInfo>;

} // namespace nativebridge

// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// geometry.h — shared value types for points, sizes and rectangles.
//
// THREADING MODEL
// ---------------
// Pure value types. No shared state; freely copyable across threads.
//
// COORDINATE SPACE
// ----------------
// * Every coordinate exposed by nativeBridge is a *logical* (DIP) coordinate
//   in the platform's virtual desktop space, with the origin at the top-left
//   of the primary display and +Y pointing down.
// * Physical pixels are obtained by multiplying with the scale factor of the
//   display the window is on (see nativebridge::Window::scaleFactor()).
// * On macOS the logical space equals Cocoa "points"; on Windows it is the
//   per-monitor-DPI-aware coordinate space; on X11 it is the X server
//   coordinate space divided by the Xft.dpi derived scale; on Wayland it is
//   the xdg-shell logical size (wl_surface coordinates).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace nativebridge {

/// A point in logical desktop coordinates (top-left origin, +Y down).
struct Point {
    int32_t x{0};
    int32_t y{0};

    friend constexpr bool operator==(const Point&, const Point&) noexcept = default;
    [[nodiscard]] constexpr Point operator+(const Point& o) const noexcept { return {x + o.x, y + o.y}; }
    [[nodiscard]] constexpr Point operator-(const Point& o) const noexcept { return {x - o.x, y - o.y}; }
};

/// A size in logical units (width/height are always >= 0 for valid sizes).
struct Size {
    int32_t width{0};
    int32_t height{0};

    friend constexpr bool operator==(const Size&, const Size&) noexcept = default;
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }
};

/// Axis-aligned rectangle, top-left inclusive / bottom-right exclusive.
struct Rect {
    int32_t x{0};
    int32_t y{0};
    int32_t width{0};
    int32_t height{0};

    friend constexpr bool operator==(const Rect&, const Rect&) noexcept = default;

    [[nodiscard]] constexpr int32_t right() const noexcept { return x + width; }
    [[nodiscard]] constexpr int32_t bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr Point origin() const noexcept { return {x, y}; }
    [[nodiscard]] constexpr Size size() const noexcept { return {width, height}; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return width <= 0 || height <= 0; }

    [[nodiscard]] constexpr bool contains(Point p) const noexcept {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }
    [[nodiscard]] constexpr bool intersects(const Rect& o) const noexcept {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
    [[nodiscard]] static constexpr Rect fromCorners(Point a, Point b) noexcept {
        return Rect{std::min(a.x, b.x), std::min(a.y, b.y),
                    std::abs(a.x - b.x), std::abs(a.y - b.y)};
    }
};

/// Inclusive integer range helper (e.g. allowed window sizes).
struct SizeRange {
    Size min{0, 0};
    Size max{0, 0}; ///< {0,0} means "no maximum".
};

/// Physical pixel dimensions of a drawable surface.
struct PixelSize {
    uint32_t width{0};
    uint32_t height{0};

    friend constexpr bool operator==(const PixelSize&, const PixelSize&) noexcept = default;
};

} // namespace nativebridge

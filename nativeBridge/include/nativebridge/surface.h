// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// surface.h — bridge for upper layer renderers.
//
// THREADING MODEL
// ---------------
// * SurfaceDescriptor is a value type: it may be created on the polling thread
//   and handed to a render thread.
// * The renderer owns its own graphics context. nativeBridge never creates,
//   binds or destroys a GL/Vulkan/Metal/D3D context, and never calls into a
//   graphics API.
// * SurfaceBridge::describe() may be called from any thread; the returned
//   handle stays valid until the window is destroyed.
//
// SCOPE
// -----
// This header intentionally contains *no* rendering code. It only answers
// "which native objects must I pass to my graphics API for this window?" and
// "which surface size should I use?".

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "nativebridge/geometry.h"
#include "nativebridge/result.h"
#include "nativebridge/window.h"

namespace nativebridge {

/// Which graphics API a descriptor is being requested for. The bridge adjusts
/// the returned native objects (e.g. it allocates a wl_egl_window for GL/Vulkan
/// on Wayland, and returns the CAMetalLayer-ready NSView on macOS).
enum class RenderApi : uint8_t {
    Generic,   ///< No API specific object; raw window/view/surface handles.
    OpenGL,
    OpenGLES,
    Vulkan,
    Metal,
    Direct3D11,
    Direct3D12,
    WebGPU,
};

/// Everything a renderer needs to create its own swapchain.
///
/// Field validity per platform/API (see README "Native interop"):
/// | field      | Win32 (D3D/GL) | macOS (Metal/GL) | X11 (GL/Vulkan) | Wayland (GL/Vulkan) |
/// |------------|----------------|------------------|-----------------|---------------------|
/// | window     | HWND           | NSWindow*        | XID             | wl_surface*         |
/// | view       | 0 (HWND is it) | NSView*          | 0               | 0                   |
/// | display    | 0              | 0                | Display*        | wl_display*         |
/// | apiSurface | 0              | CAMetalLayer*    | 0               | wl_egl_window*      |
struct SurfaceDescriptor {
    NativeHandle window{};      ///< Top level native window.
    NativeHandle view{};        ///< Content view if the API needs a view rather than a window.
    NativeHandle display{};     ///< Connection object (X11 Display* / wl_display*).
    NativeHandle apiSurface{};  ///< API specific surface object (wl_egl_window, CAMetalLayer, ...).

    PixelSize  framebufferSize{}; ///< Physical pixels; use for swapchain extent.
    Size       logicalSize{};     ///< Logical size in DIP.
    double     scaleFactor{1.0};
    uint32_t   preferredBitDepth{32};
    RenderApi  api{RenderApi::Generic};
};

/// Interface handed to upper layer renderers.
///
/// THREADING: describe() is safe to call from any thread; the returned
/// descriptor must be refreshed after every Resize/ScaleFactorChanged event.
class SurfaceBridge {
public:
    virtual ~SurfaceBridge() = default;

    /// Build a descriptor for the given API. Returns Errc::Unsupported when the
    /// combination (window + API + backend) cannot be expressed, e.g. OpenGL on
    /// a Wayland surface when the caller asked for a wl_egl_window but the
    /// window was created without one.
    [[nodiscard]] virtual Result<SurfaceDescriptor> describe(RenderApi api) = 0;

    /// Recompute framebuffer/logical size after a resize; call before the next
    /// describe() if you cache descriptors.
    [[nodiscard]] virtual Status refresh() = 0;

    /// Physical size the swapchain must use right now.
    [[nodiscard]] virtual PixelSize framebufferSize() const = 0;

    /// Backend name: "win32" | "cocoa" | "x11" | "wayland".
    [[nodiscard]] virtual std::string_view backendName() const noexcept = 0;
};

/// Convenience free function: describes the window's surface for `api`.
/// Equivalent to Platform::surfaceBridge(window).describe(api).
[[nodiscard]] Result<SurfaceDescriptor> describeSurface(const Window& window, RenderApi api);

} // namespace nativebridge

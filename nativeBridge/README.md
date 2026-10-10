# nativeBridge

`nativeBridge` is a C++20 cross-platform **system abstraction library**: it wraps
the window system, the event loop, monitors, and the common system APIs of
Windows, macOS and Linux behind one clean object oriented API, so that upper
layer UI code needs **no platform macros at all**.

It deliberately stops at the boundary of graphics: it hands your renderer the
native surface handles and never calls into OpenGL / Vulkan / Metal / Direct3D.

```cpp
#include "nativebridge/nativebridge.h"

auto platform = nativebridge::Platform::create().value();   // Result<unique_ptr<Platform>>
auto window   = platform->createWindow({.title = "hello", .clientSize = {1280, 720}}).value();

window.addEventListener([](const nativebridge::Event& event) {
    std::printf("%s\n", nativebridge::describe(event).c_str());   // no platform macro anywhere
});

while (platform->pollEvents() > 0) { /* ... */ }
```

---

## 1. Contents

| Module | Header | What it provides |
|---|---|---|
| Result | [`include/nativebridge/result.h`](include/nativebridge/result.h) | `Result<T>`, `Status`, `Errc`, `Error` (never throws across the API) |
| Keys | [`include/nativebridge/keycode.h`](include/nativebridge/keycode.h) | `KeyCode` (USB HID usage ids), `Modifier`, `MouseButton`, `KeyAction` |
| Events | [`include/nativebridge/event.h`](include/nativebridge/event.h) | `Event`, `EventType`, typed payloads, `describe(event)` |
| Window | [`include/nativebridge/window.h`](include/nativebridge/window.h) | `Window`, `WindowCreateInfo`, `Capability`, `NativeHandle` |
| Display | [`include/nativebridge/display.h`](include/nativebridge/display.h) | `DisplayInfo` (bounds, work area, scale, refresh rate) |
| System | [`include/nativebridge/system.h`](include/nativebridge/system.h) | clocks, paths, environment, argv, `Clipboard` |
| Surface | [`include/nativebridge/surface.h`](include/nativebridge/surface.h) | `SurfaceBridge`, `SurfaceDescriptor` — **no rendering** |
| Facade | [`include/nativebridge/nativebridge.h`](include/nativebridge/nativebridge.h) | `Platform` factory, event loop, windows, displays, clipboard |

Source layout:

```
src/common/          platform independent: dispatch, event queue, keysym map, time, paths, process
src/win32/           Windows 10+ (Win32, per-monitor DPI v2)
src/macos/           macOS 11+ (Cocoa / AppKit, ARC)
src/linux/x11/       Linux, X11 (Xlib + EWMH)
src/linux/wayland/   Linux, Wayland (libwayland-client + xdg-shell only)
examples/            create_window.cpp
tests/               smoke_test.cpp
```

---

## 2. Build

Requirements: CMake ≥ 3.20, a C++20 compiler, and (on Linux) the X11 and/or
Wayland development packages.

### macOS (Cocoa)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/nativebridge_example_create_window
ctest --test-dir build --output-on-failure
```

Needs the Command Line Tools or Xcode (`clang++`); links `-framework Cocoa`,
`-framework AppKit`, `-framework CoreGraphics` automatically. Objective-C++
sources are compiled with `-fobjc-arc`.

### Windows (Win32)

```bat
:: Visual Studio 2019/2022, x64 Native Tools Command Prompt
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\nativebridge_example_create_window.exe
ctest --test-dir build -C Release --output-on-failure
```

MinGW-w64 works as well:

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Links `user32 gdi32 shell32 ole32 oleaut32 dwmapi shcore` automatically. The
process is made per-monitor-DPI-aware (v2) at startup.

### Linux — X11

```sh
sudo apt install build-essential cmake libx11-dev     # Debian/Ubuntu
# sudo dnf install gcc-c++ cmake libX11-devel          # Fedora

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNATIVEBRIDGE_LINUX_BACKEND=X11
cmake --build build -j
./build/nativebridge_example_create_window
```

### Linux — Wayland

```sh
sudo apt install build-essential cmake libwayland-dev wayland-protocols
# sudo dnf install gcc-c++ cmake wayland-devel wayland-protocols-devel

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNATIVEBRIDGE_LINUX_BACKEND=WAYLAND
cmake --build build -j
```

`xdg-shell` client code is generated at configure time by `wayland-scanner` from
`wayland-protocols`, because xdg-shell is what turns a `wl_surface` into a
toplevel window. If either tool is missing:
* `-DNATIVEBRIDGE_LINUX_BACKEND=WAYLAND` **fails at configure time** with an
  explicit message (rather than producing a library that cannot create windows),
* `AUTO` silently drops the Wayland backend and uses X11.

### Linux — both, with run-time selection (default)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release            # NATIVEBRIDGE_LINUX_BACKEND=AUTO
cmake --build build -j

./build/nativebridge_example_create_window                          # Wayland first, X11 fallback
NATIVEBRIDGE_LINUX_BACKEND=x11     ./build/nativebridge_example_create_window
NATIVEBRIDGE_LINUX_BACKEND=wayland ./build/nativebridge_example_create_window
```

### CMake options

| Option | Default | Meaning |
|---|---|---|
| `NATIVEBRIDGE_LINUX_BACKEND` | `AUTO` | `AUTO` (compile both, prefer Wayland, fall back to X11), `X11`, `WAYLAND` |
| `NATIVEBRIDGE_BUILD_EXAMPLES` | `ON` | build `examples/` |
| `NATIVEBRIDGE_BUILD_TESTS` | `ON` | build `tests/` and register `ctest` |
| `NATIVEBRIDGE_ENABLE_LOGGING` | `OFF` | always emit diagnostics (always on in Debug builds) |
| `NATIVEBRIDGE_INSTALL` | `ON` | install rules + CMake package export |

Backend selection order on Linux (`AUTO`):
1. `PlatformCreateInfo::backend` when set explicitly,
2. `NATIVEBRIDGE_LINUX_BACKEND=x11|wayland` (environment, case insensitive),
3. the compile-time option,
4. otherwise try Wayland, then X11, and report *all* failures if both fail.

Consume the library from another CMake project:

```cmake
find_package(nativeBridge REQUIRED)      # after install / export
target_link_libraries(app PRIVATE nativeBridge::nativeBridge)
```

---

## 3. Threading model

The rules are repeated in every public header, next to the affected declaration.

| Object | Thread rule |
|---|---|
| `Platform` | Created, polled and destroyed on the thread that created it (the **UI thread**). |
| `Window` | Same UI thread for create/destroy/commands. Snapshot getters (`title()`, `clientSize()`, `scaleFactor()`, …) are callable from any thread and return the state as of the last poll. |
| Subscriber callbacks | Always invoked **on the polling thread**, synchronously, from `pollEvents()` / `waitEvents()`, in registration order. Never from a platform thread. |
| Callback re-entrancy | A callback must not destroy the window it is being dispatched for, and must not call `pollEvents()` again. Use `removeEventListener()` outside the callback. |
| `Clipboard::getText()` | Polling thread only: X11 and Wayland need the live connection to serve/collect the selection. |
| `Window::release()` | Transfers ownership so the window may be moved to another thread. Unsupported on Wayland. |

`std::expected` note: if the standard library has no `<expected>` (e.g. libc++
in C++20 mode, which is the default on macOS), `result.h` falls back to a bundled
minimal implementation and reports `internal::expected` in `compiledFeatures()`.

Coordinates and DPI: all public geometry is **logical** (DIP, top-left origin,
+Y down); `physical_pixel = logical * window.scaleFactor()`. A DPI change emits
exactly one `ScaleFactorChanged` **before** the following `Resized`, so renderers
can rebuild DPI dependent resources first.

---

## 4. Native interop (`Window::nativeHandle()`)

`NativeHandle::kind` tells the renderer what `value` points at.

| `kind` | `value` is | Typical use |
|---|---|---|
| `Win32Hwnd` | `HWND` | D3D11/12 swapchain, WGL, Vulkan `Win32SurfaceCreateInfoKHR` |
| `CocoaNSWindow` | `NSWindow*` | window identity / AppKit interop |
| `CocoaNSView` | `NSView*` (`CAMetalLayer*` after `RenderApi::Metal`) | `CAMetalLayer`, `NSOpenGLContext` |
| `X11Window` | `XID` | GLX, EGL, Vulkan `XlibSurfaceCreateInfoKHR` |
| `X11Display` | `Display*` | the connection for the above |
| `WaylandDisplay` | `wl_display*` | EGL/Vulkan connection |
| `WaylandSurface` | `wl_surface*` | EGL/Vulkan/Wayou; the renderer creates its own `wl_egl_window` |

`Platform::describeSurface(window, RenderApi)` returns a `SurfaceDescriptor`
(`window`, `view`, `display`, `apiSurface`, `framebufferSize`, `logicalSize`,
`scaleFactor`). **nativeBridge never creates or binds a graphics context.**

---

## 5. Platform behaviour and known limitations

`Platform::supports(Capability)` and `Window::supports(Capability)` answer
truthfully at run time; operations that a backend cannot provide return
`Errc::Unsupported` (never an exception, never a silent lie).

| Capability | Win32 | Cocoa | X11 | Wayland |
|---|---|---|---|---|
| `WindowPosition` (global move) | yes | yes | yes | **no** — Wayland hides absolute position; `move()` → `Unsupported`, `position()` → `{0,0}`, `MoveEvent::globalPositionKnown` → `false`, `release()` → `Unsupported` |
| `WindowDecoration` (client control) | yes | yes | yes (`_MOTIF_WM_HINTS`) | **no** — compositor owned; `setDecorations()` → `Unsupported`; without `xdg-decoration` the server may draw no titlebar at all; `windowSize() == clientSize()` |
| `ClipboardText` | yes | yes | yes (selection owned by this process) | yes, but **requires keyboard focus**: `getText()` without focus → `Errc::PermissionDenied`; payload travels over a file descriptor |
| `AlwaysOnTop` | yes | yes | yes (`_NET_WM_STATE_ABOVE`) | **no** (no xdg-shell state) |
| `Transparency` | yes (`WS_EX_LAYERED`) | yes | **no** (needs a 32-bit visual + compositor) | yes (ARGB surface) |
| `DpiChangeEvents` | yes (`WM_DPICHANGED`) | yes (`windowDidChangeBackingProperties`) | property change only (single session scale) | yes (`wl_surface.enter`) |
| `MultiDisplay` | yes (hotplug) | yes (hotplug) | snapshot, no hotplug events | yes (`wl_output` add/remove) |
| `GlobalCursor` | yes | yes | yes | partial (named cursors need `libwayland-cursor`, which is excluded) |
| `TextInput` | yes (`WM_CHAR`) | yes (`NSTextInputClient`-free, `characters`) | yes (`XIM`/`Xutf8LookupString`) | yes (evdev → keysym, no xkbcommon) |
| `ScreenCaptureLock` | never (capture is out of scope) | never | never | never |

Other per-backend notes:

* **macOS / Windows**: windows must be created on the main/UI thread; commands
  from another thread fail with `Errc::PlatformFailure` instead of deadlocking.
* **X11**: no per-monitor DPI concept — the session scale comes from `Xft.dpi`
  (fallback: screen DPI, then 96) and is reported for every display. Refresh
  rates need XRandR and are reported as `0`.
* **Wayland**: refresh rate needs `wl_output` v4; monitor *positions* are an
  approximation (a deterministic horizontal band per output in registry order),
  which is the second reason `WindowPosition` is not advertised.
* **X11 macro collisions**: `Xlib.h` defines object-like macros named `None`,
  `Status`, `True`, `False`, `Bool` and `CursorShape`, which clash with
  `Modifier::None`, `nativebridge::Status`, `CursorShape::*`, … If your
  translation unit includes an X11 header **before** nativeBridge, add the
  workaround used by the backend itself:

```cpp
#include <X11/Xlib.h>
#undef None
#undef Status
#undef True
#undef False
#undef Bool
#undef CursorShape
#include "nativebridge/nativebridge.h"
```

  (NativeBridge's own public headers never include a platform header, so
  including nativeBridge first is always safe.)

---

## 6. Examples and tests

```sh
./build/nativebridge_example_create_window
```

Creates a 1024×640 window, prints platform/system/display information as it
starts, prints every event, and maps shortcuts onto API calls:

| Key | Action |
|---|---|
| `Esc` / window close | quit |
| `F1` | toggle full screen |
| `F2` | toggle always-on-top |
| `F3` | resize the client area to 960×540 |
| `F4` | center on the current display |
| `F5` | hide the window for 500 ms (exercises `hide()`/`show()` events) |
| `F6` | clipboard round trip |
| `F7` | print display information |
| `F8` | describe the native surface for a renderer |

```sh
./build/nativebridge_smoke_test             # default: no window is created
NATIVEBRIDGE_SMOKE_RUN_WINDOW=1 ./build/nativebridge_smoke_test
NATIVEBRIDGE_SMOKE_REQUIRE_DISPLAY=1 ./build/nativebridge_smoke_test  # headless CI: fail instead of skip
```

The smoke test is dependency free, prints `PASS`/`FAIL`/`SKIP` per check and
returns the number of failures as its exit status. Checks that need a live
display server are skipped (not failed) unless
`NATIVEBRIDGE_SMOKE_REQUIRE_DISPLAY=1` is set, which makes it usable in
containers without a window server.

---

## 7. Verification status

| Backend | Status | How it was checked |
|---|---|---|
| `src/common` | **verified** | compiled with `-Wall -Wextra -Wpedantic`, exercised by the smoke test |
| macOS (Cocoa) | **verified** | built with Apple clang 21 (C++20, ARC) and **run**: platform creation, window creation, resize/title/frame/scale, `nativeHandle()`, `describeSurface()`, monitor enumeration (model name, 1470×956 logical @2x, 60 Hz, work area), clipboard round trip, cleanup |
| Linux X11 | **compiled and linked** (no X server on the build host, so no run) | compiled with `-Wall -Wextra` against real X11 headers (libx11 1.8.13) and linked into the smoke test binary, which registers `backend:x11` |
| Linux Wayland | **未验证 / NOT verified** | no `wayland-client` headers or `wayland-scanner` available on the build host; the code was written against the documented libwayland-client 1.20+ and xdg-shell APIs and reviewed by hand (preprocessor-resolved brace balance, no compilation) |
| Windows Win32 | **未验证 / NOT verified** | no MSVC or MinGW-w64 toolchain on the build host; written against the documented Win32 API (`CreateWindowExW`, `WM_DPICHANGED`, `PeekMessage`, `EnumDisplayMonitors`, clipboard) |

Everything in section 5 that is marked "no" is enforced by `supports()` and by
the command returning `Errc::Unsupported`, and is covered by the smoke test
(`supports()` consistency checks).

## 8. Out of scope (by design)

Rendering pipelines and graphics API calls; UI controls, layout, animation and
styling; mobile platforms (iOS/Android/HarmonyOS); networking, databases and
serialisation; screen capture; image/file clipboards.

## License

MIT.

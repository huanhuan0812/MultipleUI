// nativeBridge — internal backend factories.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API.
//
// One factory per backend, declared with external linkage. src/common/platform.cpp
// references them explicitly (see keepBackendReferencesAlive()) so that the
// linker cannot drop a backend object file from the static library: a backend
// that is never referenced by the facade would otherwise be discarded, and its
// load time self registration would never run.
//
// The corresponding platform translation units define these and register them
// through detail::registerBackend().

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/backend.h"

namespace nativebridge::detail {

/// Compile time list of the backends that were actually compiled in. These
/// macros are set by CMake.
[[nodiscard]] std::vector<std::string> compiledBackendNames();

#if defined(NATIVEBRIDGE_BACKEND_WIN32)
[[nodiscard]] Result<std::unique_ptr<PlatformBackend>> createWin32Platform();
#endif

#if defined(NATIVEBRIDGE_BACKEND_COCOA)
[[nodiscard]] Result<std::unique_ptr<PlatformBackend>> createCocoaPlatform();
#endif

#if defined(NATIVEBRIDGE_BACKEND_X11)
[[nodiscard]] Result<std::unique_ptr<PlatformBackend>> createX11Platform();
#endif

#if defined(NATIVEBRIDGE_BACKEND_WAYLAND)
[[nodiscard]] Result<std::unique_ptr<PlatformBackend>> createWaylandPlatform();
#endif

} // namespace nativebridge::detail

// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// system.h — clock, filesystem locations, environment, clipboard.
//
// THREADING MODEL
// ---------------
// * The free functions (monotonicTime, systemTime, executablePath(), homeDirectory(),
//   environment(), commandLineArguments()) are thread safe and callable from any
//   thread at any time, before or after a Platform exists.
// * Clipboard access goes through Platform (Platform::clipboard()), because a
//   clipboard is owned by a display-server connection. Clipboard::getText()
//   may only be called from the polling thread; Clipboard::setText() may be
//   called from any thread and is queued to the polling thread.
// * On macOS, clipboard and path APIs must not be used before NSApplication is
//   initialized; doing so returns Errc::PlatformFailure rather than crashing.
//
// PATH ENCODING
// -------------
// All paths are UTF-8 encoded std::string on every platform (Windows converts
// to/from UTF-16 internally). Use PathString::value for the OS-native form.

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nativebridge/result.h"

namespace nativebridge {

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/// Monotonic, high resolution duration since an unspecified epoch.
using Nanoseconds = std::chrono::nanoseconds;

/// Monotonic clock reading; unaffected by wall-clock adjustments.
/// Resolution: >= 1 ns on all supported platforms (QueryPerformanceCounter /
/// clock_gettime(CLOCK_MONOTONIC) / mach_absolute_time).
[[nodiscard]] Nanoseconds monotonicTime() noexcept;

/// Wall clock (Unix epoch) reading with the best available precision.
[[nodiscard]] std::chrono::system_clock::time_point systemTime() noexcept;

/// Local time zone offset applicable at the given instant.
[[nodiscard]] std::chrono::minutes localTimeZoneOffset(std::chrono::system_clock::time_point tp) noexcept;

/// Format an instant as local time using a strftime pattern; returns
/// Err::InvalidArgument for an unusable pattern.
[[nodiscard]] Result<std::string> formatTime(std::chrono::system_clock::time_point tp, std::string_view strftimePattern);

// ---------------------------------------------------------------------------
// Process / filesystem locations
// ---------------------------------------------------------------------------

/// Absolute path of the running executable (UTF-8). Resolves symlinks.
[[nodiscard]] Result<std::string> executablePath();

/// Absolute path of the directory containing the executable.
[[nodiscard]] Result<std::string> applicationDirectory();

/// Per-user data directory (roaming on Windows), without trailing separator.
///   Windows: %APPDATA%            macOS: ~/Library/Application Support
///   Linux:   $XDG_DATA_HOME or ~/.local/share
[[nodiscard]] Result<std::string> userDataDirectory();

/// Per-user configuration directory.
[[nodiscard]] Result<std::string> userConfigDirectory();

/// Per-user cache directory.
///   Windows: %LOCALAPPDATA%       macOS: ~/Library/Caches
///   Linux:   $XDG_CACHE_HOME or ~/.cache
[[nodiscard]] Result<std::string> userCacheDirectory();

/// Temporary directory (TMPDIR/TEMP, /tmp fallback).
[[nodiscard]] Result<std::string> temporaryDirectory();

/// Current user's home directory.
[[nodiscard]] Result<std::string> homeDirectory();

/// Join two path components with the platform separator and normalize
/// duplicate separators on the seam. Not a full path canonicalizer.
[[nodiscard]] std::string joinPath(std::string_view base, std::string_view leaf);

/// Platform path separator character ('\\' on Windows, '/' elsewhere).
/// Declared here, defined in the platform specific system translation unit, so
/// that this header contains no platform preprocessor condition at all.
[[nodiscard]] char pathSeparator() noexcept;

// ---------------------------------------------------------------------------
// Environment / command line
// ---------------------------------------------------------------------------

/// Read an environment variable. std::nullopt when it is not set.
[[nodiscard]] std::optional<std::string> environment(std::string_view name);

/// Set/overwrite an environment variable (also visible to child processes).
[[nodiscard]] Status setEnvironment(std::string_view name, std::string_view value);

/// Unset an environment variable; succeeds when it was already absent.
[[nodiscard]] Status unsetEnvironment(std::string_view name);

/// Full environment as name=value pairs. Order is unspecified.
[[nodiscard]] std::vector<std::string> environmentVariables();

/// Process arguments including argv[0].
[[nodiscard]] std::vector<std::string> commandLineArguments();

/// Number of logical CPU cores (>= 1).
[[nodiscard]] uint32_t processorCount() noexcept;

// ---------------------------------------------------------------------------
// Clipboard (text only; images/files are out of scope by design)
// ---------------------------------------------------------------------------

/// Clipboard access bound to a display-server connection.
///
/// THREADING: getText() must run on the Platform polling thread (X11 and
/// Wayland need the live connection to serve/collect the selection).
/// setText() is thread safe and queued. On Wayland both directions require one
/// of the client's surfaces to hold keyboard focus, otherwise the compositor
/// answers with no data and getText() returns Err::NotFound.
class Clipboard {
public:
    virtual ~Clipboard() = default;

    /// Current clipboard text as UTF-8. Err::NotFound when the clipboard is
    /// empty or holds a non-text flavour.
    [[nodiscard]] virtual Result<std::string> getText() = 0;

    /// Put UTF-8 text on the clipboard. Ownership semantics:
    ///  * Windows/macOS: the OS copies the data, the string may be freed at once.
    ///  * X11: the process becomes selection owner and must keep serving until
    ///    another client takes ownership; nativeBridge keeps the text alive in
    ///    the backend and answers SelectionRequest during pollEvents().
    ///  * Wayland: the compositor copies the data during the next commit.
    [[nodiscard]] virtual Status setText(std::string_view utf8Text) = 0;

    /// Clear the clipboard (best effort; always succeeds on X11/Wayland because
    /// taking ownership of nothing is allowed).
    [[nodiscard]] virtual Status clear() = 0;

    /// True when the clipboard currently holds text (cheap hint, no copy).
    [[nodiscard]] virtual bool hasText() = 0;

    /// Backend name for diagnostics: "win32", "cocoa", "x11", "wayland".
    [[nodiscard]] virtual std::string_view backendName() const noexcept = 0;
};

} // namespace nativebridge

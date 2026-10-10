// nativeBridge — cross-platform system abstraction library.
// SPDX-License-Identifier: MIT
//
// result.h — unified error handling.
//
// Design notes
// ------------
// * This library never throws across its public API boundary. Every operation
//   that can fail returns either `Result<T>` or `Status`.
// * `Result<T>` is an alias of `std::expected<T, Error>` (C++23 vocabulary type
//   shipped by libc++/libstdc++/MSVC STL and usable from C++20 mode). If the
//   standard library used to build the project does not provide <expected>,
//   define NATIVEBRIDGE_USE_INTERNAL_EXPECTED=1 and a minimal compatible
//   implementation from this header is used instead.
// * Operations that a platform genuinely cannot provide (for example: the
//   global absolute position of a Wayland toplevel) return
//   `Errc::Unsupported` instead of pretending to succeed or throwing.
//
// THREADING MODEL
// ---------------
// `Error` and `Result<T>` are plain value types with no shared state; they may
// be created, copied and destroyed on any thread.

#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#    include <expected>
#    define NATIVEBRIDGE_HAS_STD_EXPECTED 1
#endif

#if !defined(NATIVEBRIDGE_HAS_STD_EXPECTED)
// ---------------------------------------------------------------------------
// Minimal std::expected work-alike (only the subset nativeBridge uses).
// Enabled automatically when the toolchain lacks <expected>.
// ---------------------------------------------------------------------------
#    include <optional>
#    include <variant>

namespace nativebridge::detail {

template <class E>
class Unexpected {
public:
    constexpr explicit Unexpected(E e) noexcept(std::is_nothrow_move_constructible_v<E>)
        : m_error(std::move(e)) {}

    [[nodiscard]] constexpr const E& error() const& noexcept { return m_error; }
    [[nodiscard]] constexpr E& error() & noexcept { return m_error; }
    [[nodiscard]] constexpr E&& error() && noexcept { return std::move(m_error); }

private:
    E m_error;
};

template <class E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> unexpected(E&& e) {
    return Unexpected<std::decay_t<E>>(std::forward<E>(e));
}

struct UnexpectT {
    explicit UnexpectT() = default;
};
inline constexpr UnexpectT unexpect{};

} // namespace nativebridge::detail

namespace nativebridge {

// Forward declarations: `fail()` below needs these before Expected is complete.
template <class E>
[[nodiscard]] constexpr detail::Unexpected<std::decay_t<E>> unexpected(E&& e) {
    return detail::Unexpected<std::decay_t<E>>(std::forward<E>(e));
}
inline constexpr detail::UnexpectT unexpect{};

template <class T, class E>
class Expected {
public:
    using value_type = T;
    using error_type = E;

    constexpr Expected() = default;
    constexpr Expected(const T& v) : m_storage(v) {}
    constexpr Expected(T&& v) : m_storage(std::move(v)) {}
    constexpr Expected(detail::UnexpectT, E e) : m_storage(std::move(e)) {}
    constexpr Expected(detail::Unexpected<E> u) : m_storage(std::move(u).error()) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return std::holds_alternative<T>(m_storage);
    }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    constexpr T& operator*() & { return std::get<T>(m_storage); }
    constexpr const T& operator*() const& { return std::get<T>(m_storage); }
    constexpr T&& operator*() && { return std::get<T>(std::move(m_storage)); }
    constexpr T* operator->() { return &std::get<T>(m_storage); }
    constexpr const T* operator->() const { return &std::get<T>(m_storage); }

    constexpr T& value() & { return std::get<T>(m_storage); }
    constexpr const T& value() const& { return std::get<T>(m_storage); }

    constexpr E& error() & { return std::get<E>(m_storage); }
    constexpr const E& error() const& { return std::get<E>(m_storage); }
    constexpr E&& error() && { return std::get<E>(std::move(m_storage)); }

    constexpr T value_or(T fallback) const { return has_value() ? std::get<T>(m_storage) : std::move(fallback); }

private:
    std::variant<T, E> m_storage{};
};

template <class E>
class Expected<void, E> {
public:
    using value_type = void;
    using error_type = E;

    constexpr Expected() = default;
    constexpr Expected(detail::UnexpectT, E e) : m_error(std::move(e)) {}
    constexpr Expected(detail::Unexpected<E> u) : m_error(std::move(u).error()) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return !m_error.has_value(); }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    constexpr E& error() & { return *m_error; }
    constexpr const E& error() const& { return *m_error; }
    constexpr E&& error() && { return std::move(*m_error); }

private:
    std::optional<E> m_error{};
};

} // namespace nativebridge

#else // NATIVEBRIDGE_HAS_STD_EXPECTED

#    include <expected>

namespace nativebridge::detail {
using std::unexpect;
using std::unexpected;
template <class T, class E>
using Expected = std::expected<T, E>;
} // namespace nativebridge::detail

namespace nativebridge {
using detail::Expected;
using detail::unexpect;
using detail::unexpected;
} // namespace nativebridge

#endif // NATIVEBRIDGE_HAS_STD_EXPECTED

namespace nativebridge {

/// Stable, platform-independent error codes.
enum class Errc {
    Ok = 0,
    Unknown,             ///< Unclassified failure.
    InvalidArgument,     ///< Caller passed an out-of-range or mutually exclusive argument.
    NotFound,            ///< A resource (display, window id, clipboard owner) does not exist.
    AlreadyExists,       ///< Duplicate registration of the same resource.
    Unsupported,         ///< The current platform/backend cannot provide this feature.
    NotImplemented,      ///< Feature is designed but not yet wired on this backend.
    PlatformFailure,     ///< The platform API reported an error (see Error::detail).
    ConnectionFailed,    ///< Could not connect to the display server.
    ProtocolError,       ///< A wire-protocol level failure (Wayland/X11).
    OutOfMemory,         ///< Allocation or GPU/system resource exhaustion.
    PermissionDenied,    ///< OS refused the operation.
    Timeout,             ///< Waiting for an event or resource exceeded its budget.
    Cancelled,           ///< Operation was aborted (window closed while waiting).
};

/// Human readable name of an error code (never null).
[[nodiscard]] constexpr std::string_view toString(Errc code) noexcept {
    switch (code) {
        case Errc::Ok:               return "Ok";
        case Errc::Unknown:          return "Unknown";
        case Errc::InvalidArgument:  return "InvalidArgument";
        case Errc::NotFound:         return "NotFound";
        case Errc::AlreadyExists:    return "AlreadyExists";
        case Errc::Unsupported:      return "Unsupported";
        case Errc::NotImplemented:   return "NotImplemented";
        case Errc::PlatformFailure:  return "PlatformFailure";
        case Errc::ConnectionFailed: return "ConnectionFailed";
        case Errc::ProtocolError:    return "ProtocolError";
        case Errc::OutOfMemory:      return "OutOfMemory";
        case Errc::PermissionDenied: return "PermissionDenied";
        case Errc::Timeout:          return "Timeout";
        case Errc::Cancelled:        return "Cancelled";
    }
    return "Unknown";
}

/// Rich error object: stable code + human readable message + optional OS detail.
///
/// THREADING: value type, safe to pass between threads.
struct Error {
    Errc        code{Errc::Unknown};
    std::string message{};  ///< UTF-8, short, English, no trailing newline.
    int64_t     native{0};  ///< Raw OS error (errno / GetLastError / wl_display error), 0 if none.

    Error() = default;

    Error(Errc c, std::string msg, int64_t nativeCode = 0)
        : code(c), message(std::move(msg)), native(nativeCode) {}

    [[nodiscard]] static Error unsupported(std::string_view what) {
        return Error{Errc::Unsupported, std::string(what) + ": not supported by this platform/backend"};
    }
    [[nodiscard]] static Error notImplemented(std::string_view what) {
        return Error{Errc::NotImplemented, std::string(what) + ": not implemented for this backend"};
    }
    [[nodiscard]] static Error invalid(std::string_view what) {
        return Error{Errc::InvalidArgument, std::string(what)};
    }
    [[nodiscard]] static Error platform(std::string_view what, int64_t nativeCode) {
        return Error{Errc::PlatformFailure, std::string(what), nativeCode};
    }

    /// "Code: message" — convenient for logging.
    [[nodiscard]] std::string describe() const {
        return std::string(toString(code)) + ": " + message;
    }
};

/// `std::expected` alias with the library error type.
template <class T>
using Result = Expected<T, Error>;

/// Result of an operation with no payload.
using Status = Result<void>;

/// Build a failed `Result<T>`.
template <class T = void>
[[nodiscard]] auto fail(Error e) {
    return Result<T>(unexpected(std::move(e)));
}

/// Convenience: failed status with a code and message.
[[nodiscard]] inline Status failStatus(Errc code, std::string message, int64_t native = 0) {
    return Status(unexpected(Error{code, std::move(message), native}));
}

/// Convenience: successful status.
[[nodiscard]] inline Status okStatus() noexcept { return Status{}; }

} // namespace nativebridge

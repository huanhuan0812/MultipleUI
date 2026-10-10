// nativeBridge — internal thread safe snapshot cache.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API.
//
// Window::title(), clientSize(), scaleFactor() and friends are callable from any
// thread (see the threading model in include/nativebridge/window.h). Backends
// keep the authoritative state on the UI thread and mirror it into this cache,
// which is what the any-thread getters read.

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace nativebridge::detail {

/// Mutex protected single value with a copy-on-read API.
template <class T>
class Snapshot {
public:
    Snapshot() = default;
    explicit Snapshot(T initial) : m_value(std::move(initial)) {}

    void set(const T& value) {
        std::lock_guard lock(m_mutex);
        m_value = value;
    }

    void set(T&& value) {
        std::lock_guard lock(m_mutex);
        m_value = std::move(value);
    }

    /// Apply a mutation to the stored value under the lock.
    template <class Fn>
    void update(Fn&& fn) {
        std::lock_guard lock(m_mutex);
        fn(m_value);
    }

    [[nodiscard]] T get() const {
        std::lock_guard lock(m_mutex);
        return m_value;
    }

private:
    mutable std::mutex m_mutex;
    T                  m_value{};
};

/// Snapshot of a simple flag type.
using Flag = Snapshot<bool>;

} // namespace nativebridge::detail

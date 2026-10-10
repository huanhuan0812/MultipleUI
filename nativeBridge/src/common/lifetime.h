// nativeBridge — cross-backend lifetime token.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API.
//
// Problem this solves
// -------------------
// A WindowBackend sometimes has to tell its Platform that it died (to remove
// itself from a bookkeeping list), but the Platform implementation class is
// deliberately private to its backend translation unit and the Window can be
// destroyed from any thread - including after the Platform is gone.
//
// Solution: the Platform owns a shared LifetimeToken and hands a weak_ptr to
// every window. ~WindowBackend locks the weak_ptr: if the platform is still
// alive the callback runs, otherwise the lock fails and nothing happens. There
// is therefore no window that can dereference a destroyed Platform.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "nativebridge/window.h"

namespace nativebridge::detail {

/// Shared "is my platform still alive?" handle with a deregistration callback.
class LifetimeToken {
public:
    using DestroyCallback = std::function<void(WindowId)>;

    LifetimeToken() = default;
    ~LifetimeToken() { invalidate(); }

    LifetimeToken(const LifetimeToken&) = delete;
    LifetimeToken& operator=(const LifetimeToken&) = delete;

    /// Register the callback invoked when a window is destroyed.
    void setDestroyCallback(DestroyCallback callback) {
        std::lock_guard lock(m_mutex);
        m_callback = std::move(callback);
    }

    /// Invoke the callback; no-op when the platform is already gone.
    void notifyWindowDestroyed(WindowId id) {
        DestroyCallback callback;
        {
            std::lock_guard lock(m_mutex);
            callback = m_callback;
        }
        if (callback) {
            callback(id);
        }
    }

    /// Called by the Platform destructor: further notifications become no-ops and
    /// in-flight ones finish before the platform state is torn down.
    void invalidate() {
        std::lock_guard lock(m_mutex);
        m_callback = nullptr;
        m_invalidated = true;
    }

    [[nodiscard]] bool invalidated() const {
        std::lock_guard lock(m_mutex);
        return m_invalidated;
    }

private:
    mutable std::mutex m_mutex;
    DestroyCallback    m_callback;
    bool               m_invalidated{false};
};

using LifetimeTokenPtr = std::shared_ptr<LifetimeToken>;
using WeakLifetimeToken = std::weak_ptr<LifetimeToken>;

/// Convenience: notify through a weak token, tolerating an expired platform.
inline void notifyDestroyed(const WeakLifetimeToken& token, WindowId id) {
    if (const LifetimeTokenPtr alive = token.lock()) {
        alive->notifyWindowDestroyed(id);
    }
}

} // namespace nativebridge::detail

// nativeBridge — internal listener registry.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API. The public headers only see the incomplete type
// nativebridge::DispatchRegistry; this is where it is defined.
//
// THREADING MODEL
// ---------------
// * add()/remove() may be called from any thread.
// * dispatch() must be called from the UI thread (Platform::pollEvents()) and
//   happens with the registry lock released, so a listener may subscribe or
//   unsubscribe re-entrantly. A listener removed during a dispatch pass still
//   sees the event it was already scheduled for, but not later ones.

#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#include "nativebridge/window.h"

namespace nativebridge {

class DispatchRegistry {
public:
    DispatchRegistry() = default;

    DispatchRegistry(const DispatchRegistry&) = delete;
    DispatchRegistry& operator=(const DispatchRegistry&) = delete;

    /// Register a listener; returns kInvalidSubscription for an empty listener.
    [[nodiscard]] SubscriptionToken add(EventType type, EventListener listener) {
        if (!listener) {
            return kInvalidSubscription;
        }
        std::lock_guard lock(m_mutex);
        const SubscriptionToken token = m_nextToken++;
        m_subscriptions.push_back(EventSubscription{token, type, std::move(listener)});
        return token;
    }

    /// Remove a subscription; false when the token is unknown.
    bool remove(SubscriptionToken token) {
        if (token == kInvalidSubscription) {
            return false;
        }
        std::lock_guard lock(m_mutex);
        const auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(),
                                     [token](const EventSubscription& subscription) {
                                         return subscription.token == token;
                                     });
        if (it == m_subscriptions.end()) {
            return false;
        }
        m_subscriptions.erase(it);
        return true;
    }

    /// Invoke every listener registered for `event.type`, in registration order.
    /// Listeners registered with EventType::All receive every event.
    void dispatch(const Event& event) {
        std::vector<EventListener> matching;
        {
            std::lock_guard lock(m_mutex);
            for (const auto& subscription : m_subscriptions) {
                if (subscription.type == event.type || subscription.type == EventType::All) {
                    matching.push_back(subscription.listener);
                }
            }
        }
        for (const auto& listener : matching) {
            if (listener) {
                listener(event);
            }
        }
    }

    [[nodiscard]] std::size_t listenerCount() const {
        std::lock_guard lock(m_mutex);
        return m_subscriptions.size();
    }

private:
    mutable std::mutex             m_mutex;
    std::vector<EventSubscription> m_subscriptions;
    uint64_t                       m_nextToken{1};
};

} // namespace nativebridge

// nativeBridge — internal event queue.
// SPDX-License-Identifier: MIT
//
// NOT PART OF THE PUBLIC API.
//
// A small, lock protected FIFO used by every backend to hand events from the
// platform message source to the UI thread. Backends push from wherever the OS
// delivers messages; Platform::pollEvents()/waitEvents() pop on the UI thread.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "nativebridge/event.h"

namespace nativebridge::detail {

/// Thread safe event FIFO with a "not empty" condition variable.
class EventQueue {
public:
    /// Append an event; wakes a thread blocked in waitForEvents().
    void push(Event event) {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(event));
            ++m_produced;
        }
        m_cv.notify_all();
    }

    /// Append every event of `events`.
    void pushAll(std::vector<Event> events) {
        if (events.empty()) {
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            for (auto& event : events) {
                m_queue.push_back(std::move(event));
            }
            m_produced += events.size();
        }
        m_cv.notify_all();
    }

    /// Move all queued events out of the queue.
    [[nodiscard]] std::vector<Event> drain() {
        std::vector<Event> out;
        {
            std::lock_guard lock(m_mutex);
            if (m_queue.empty()) {
                return out;
            }
            out.reserve(m_queue.size());
            while (!m_queue.empty()) {
                out.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
            }
        }
        return out;
    }

    /// Block until the queue is non-empty, `timeout` expires or `wake` is
    /// called. `timeout` < 0 blocks indefinitely. Returns true when the queue
    /// is non-empty.
    bool waitForEvents(std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_mutex);
        const auto ready = [this] { return !m_queue.empty() || m_wake; };
        if (timeout.count() < 0) {
            m_cv.wait(lock, ready);
        } else if (!m_cv.wait_for(lock, timeout, ready)) {
            return false;
        }
        m_wake = false;
        return !m_queue.empty();
    }

    /// Interrupt a blocked waitForEvents() from any thread (used to service
    /// pending commands and during shutdown).
    void wake() {
        {
            std::lock_guard lock(m_mutex);
            m_wake = true;
        }
        m_cv.notify_all();
    }

    /// Close the queue: waitForEvents() returns immediately from now on.
    void close() {
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
            m_wake = true;
        }
        m_cv.notify_all();
    }

    [[nodiscard]] bool isClosed() const {
        std::lock_guard lock(m_mutex);
        return m_closed;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    void clear() {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
    }

    /// Total number of events ever pushed (monotonic, lock free read).
    [[nodiscard]] uint64_t producedCount() const noexcept { return m_produced.load(std::memory_order_relaxed); }

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::deque<Event>       m_queue;
    std::atomic<uint64_t>   m_produced{0};
    bool                    m_wake{false};
    bool                    m_closed{false};
};

} // namespace nativebridge::detail

// nativeBridge — platform independent implementation.
// SPDX-License-Identifier: MIT
//
// time.cpp — high resolution clock, wall clock, local time formatting.

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

#include "nativebridge/event.h"
#include "nativebridge/system.h"

namespace nativebridge {

Nanoseconds monotonicTime() noexcept {
    // std::chrono::steady_clock is CLOCK_MONOTONIC / mach_absolute_time /
    // QueryPerformanceCounter on the three supported platforms; all three have
    // sub-microsecond resolution, which satisfies the documented contract.
    return std::chrono::duration_cast<Nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

uint64_t monotonicNowNs() noexcept { return static_cast<uint64_t>(monotonicTime().count()); }

std::chrono::system_clock::time_point systemTime() noexcept { return std::chrono::system_clock::now(); }

std::chrono::minutes localTimeZoneOffset(std::chrono::system_clock::time_point tp) noexcept {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    std::tm utc{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0 || gmtime_s(&utc, &seconds) != 0) {
        return std::chrono::minutes{0};
    }
#else
    if (localtime_r(&seconds, &local) == nullptr || gmtime_r(&seconds, &utc) == nullptr) {
        return std::chrono::minutes{0};
    }
#endif
    const std::time_t localEpoch = std::mktime(&local);         // interprets `local` as local time
    const std::time_t utcAsLocal = std::mktime(&utc);           // interprets `utc` as local time
    if (localEpoch == static_cast<std::time_t>(-1) || utcAsLocal == static_cast<std::time_t>(-1)) {
        return std::chrono::minutes{0};
    }
    const double diffSeconds = std::difftime(localEpoch, utcAsLocal);
    return std::chrono::minutes{static_cast<int64_t>(diffSeconds / 60.0 + (diffSeconds < 0 ? -0.5 : 0.5))};
}

Result<std::string> formatTime(std::chrono::system_clock::time_point tp, std::string_view strftimePattern) {
    if (strftimePattern.empty()) {
        return fail<std::string>(Error::invalid("formatTime: empty pattern"));
    }
    const std::time_t seconds = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0) {
        return fail<std::string>(Error::platform("formatTime: localtime_s failed", static_cast<int64_t>(errno)));
    }
#else
    if (localtime_r(&seconds, &local) == nullptr) {
        return fail<std::string>(Error::platform("formatTime: localtime_r failed", static_cast<int64_t>(errno)));
    }
#endif
    // The pattern has to be NUL terminated for strftime.
    const std::string pattern(strftimePattern);
    std::string buffer(256, '\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::size_t written = std::strftime(buffer.data(), buffer.size(), pattern.c_str(), &local);
        if (written > 0) {
            buffer.resize(written);
            return buffer;
        }
        if (buffer.size() > 64 * 1024) {
            break;
        }
        buffer.assign(buffer.size() * 4, '\0');
    }
    return fail<std::string>(Error::invalid("formatTime: pattern produced no output or exceeds 64 KiB"));
}

} // namespace nativebridge

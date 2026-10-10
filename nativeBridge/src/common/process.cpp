// nativeBridge — platform independent implementation.
// SPDX-License-Identifier: MIT
//
// process.cpp — environment block, command line arguments, CPU count.

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include "nativebridge/system.h"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#elif defined(__APPLE__)
#    include <crt_externs.h>
#else
extern "C" char** environ; // POSIX
#endif

namespace nativebridge {
namespace {

// Reserved for future path normalization helpers used by this TU.
[[maybe_unused]] std::string trimTrailingSeparators(std::string path) {
    const auto isSeparator = [](char c) { return c == '/' || c == '\\'; };
    while (path.size() > 1 && isSeparator(path.back())) {
        path.pop_back();
    }
    return path;
}

} // namespace

std::vector<std::string> environmentVariables() {
    std::vector<std::string> result;
#if defined(_WIN32)
    LPWCH block = ::GetEnvironmentStringsW();
    if (block == nullptr) {
        return result;
    }
    for (const wchar_t* cursor = block; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
        // Skip the hidden "=C:=C:\..." per-drive entries.
        if (cursor[0] == L'=') {
            continue;
        }
        const int needed =
            ::WideCharToMultiByte(CP_UTF8, 0, cursor, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            continue;
        }
        std::string entry(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, cursor, -1, entry.data(), needed, nullptr, nullptr);
        result.push_back(std::move(entry));
    }
    ::FreeEnvironmentStringsW(block);
#else
#    if defined(__APPLE__)
    char** const block = *_NSGetEnviron();
#    else
    char** const block = environ;
#    endif
    if (block == nullptr) {
        return result;
    }
    for (char** cursor = block; *cursor != nullptr; ++cursor) {
        result.emplace_back(*cursor);
    }
#endif
    return result;
}

std::vector<std::string> commandLineArguments() {
    std::vector<std::string> result;
#if defined(_WIN32)
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv != nullptr) {
        result.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            const int needed = ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (needed <= 1) {
                result.emplace_back();
                continue;
            }
            std::string arg(static_cast<std::size_t>(needed - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), needed, nullptr, nullptr);
            result.push_back(std::move(arg));
        }
        ::LocalFree(argv);
    }
#elif defined(__APPLE__)
    const int argc = *_NSGetArgc();
    char** const argv = *_NSGetArgv();
    if (argc > 0 && argv != nullptr) {
        result.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            result.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }
    }
#else
    // Linux: /proc/self/cmdline is the authoritative source and also works when
    // the process was started with a modified argv.
    if (FILE* file = std::fopen("/proc/self/cmdline", "rb"); file != nullptr) {
        std::string data;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            data.append(buffer, read);
        }
        std::fclose(file);
        std::size_t start = 0;
        while (start <= data.size()) {
            const std::size_t end = data.find('\0', start);
            if (end == std::string::npos) {
                break;
            }
            result.emplace_back(data.substr(start, end - start));
            start = end + 1;
        }
    }
#endif
    return result;
}

uint32_t processorCount() noexcept {
    const unsigned int count = std::thread::hardware_concurrency();
    return count == 0 ? 1u : static_cast<uint32_t>(count);
}

} // namespace nativebridge

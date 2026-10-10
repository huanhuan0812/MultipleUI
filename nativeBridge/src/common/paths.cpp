// nativeBridge — platform independent implementation.
// SPDX-License-Identifier: MIT
//
// paths.cpp — filesystem locations, path joining and environment access.
//
// Windows specifics live in the `#if defined(_WIN32)` branches below; every
// branch stores UTF-8 in std::string and only converts to UTF-16 at the OS
// boundary, so the public API is encoding-identical on all platforms.

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
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
#    include <shlobj.h>
#    include <userenv.h>
#else
#    include <pwd.h>
#    include <unistd.h>
#    include <sys/types.h>
#    include <climits>
#    include <cstdio>
#    if defined(__APPLE__)
#        include <mach-o/dyld.h>
#        include <cstdint>
#    endif
#endif

namespace nativebridge {
namespace {

#if defined(_WIN32)
/// UTF-16 -> UTF-8 (lossless, no exceptions).
std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

/// UTF-8 -> UTF-16.
std::wstring toUtf16(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string trimTrailingSeparators(std::string path) {
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }
    return path;
}

Result<std::string> knownFolder(REFKNOWNFOLDERID folder, std::string_view what) {
    PWSTR raw = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(folder, KF_FLAG_CREATE, nullptr, &raw);
    if (FAILED(hr) || raw == nullptr) {
        if (raw != nullptr) {
            ::CoTaskMemFree(raw);
        }
        return fail<std::string>(Error::platform(what, static_cast<int64_t>(hr)));
    }
    std::string result = trimTrailingSeparators(toUtf8(raw));
    ::CoTaskMemFree(raw);
    if (result.empty()) {
        return fail<std::string>(Error{Errc::PlatformFailure, std::string(what) + ": empty path"});
    }
    return result;
}

Result<std::string> environmentValue(const wchar_t* name, std::string_view what) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return fail<std::string>(Error::platform(what, static_cast<int64_t>(::GetLastError())));
    }
    std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, buffer.data(), needed);
    if (written == 0 || written >= needed) {
        return fail<std::string>(Error::platform(what, static_cast<int64_t>(::GetLastError())));
    }
    buffer.resize(written);
    return trimTrailingSeparators(toUtf8(buffer));
}
#else // !_WIN32

std::string trimTrailingSeparators(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

/// XDG style lookup: use $variable when set to a non-empty absolute path.
/// Only referenced by the Linux branches below.
[[maybe_unused]] Result<std::string> xdgDirectory(const char* variable, std::string_view fallbackSuffix, std::string_view what) {
    if (const char* value = std::getenv(variable); value != nullptr && value[0] == '/') {
        return trimTrailingSeparators(std::string(value));
    }
    auto home = homeDirectory();
    if (!home) {
        return fail<std::string>(Error{Errc::NotFound, std::string(what) + ": home directory unavailable"});
    }
    return trimTrailingSeparators(*home + std::string(fallbackSuffix));
}

/// $TMPDIR (macOS/BSD) or /tmp.
Result<std::string> posixTemporaryDirectory() {
    if (const char* value = std::getenv("TMPDIR"); value != nullptr && value[0] != '\0') {
        return trimTrailingSeparators(std::string(value));
    }
    return std::string("/tmp");
}

#endif // _WIN32

} // namespace

// ---------------------------------------------------------------------------
// Environment / command line — shared implementation via getenv/setenv.
// ---------------------------------------------------------------------------

std::optional<std::string> environment(std::string_view name) {
    if (name.empty() || name.find('=') != std::string_view::npos) {
        return std::nullopt;
    }
#if defined(_WIN32)
    const std::wstring wideName = toUtf16(name);
    const DWORD needed = ::GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(wideName.c_str(), buffer.data(), needed);
    if (written == 0) {
        return std::nullopt;
    }
    buffer.resize(written);
    return toUtf8(buffer);
#else
    const std::string key(name);
    if (const char* value = std::getenv(key.c_str()); value != nullptr) {
        return std::string(value);
    }
    return std::nullopt;
#endif
}

Status setEnvironment(std::string_view name, std::string_view value) {
    if (name.empty() || name.find('=') != std::string_view::npos) {
        return failStatus(Errc::InvalidArgument, "setEnvironment: invalid variable name");
    }
#if defined(_WIN32)
    const std::wstring key = toUtf16(name);
    const std::wstring val = toUtf16(value);
    if (::SetEnvironmentVariableW(key.c_str(), val.c_str()) == 0) {
        return failStatus(Errc::PlatformFailure, "setEnvironment failed", static_cast<int64_t>(::GetLastError()));
    }
#else
    const std::string key(name);
    const std::string val(value);
    if (::setenv(key.c_str(), val.c_str(), 1) != 0) {
        return failStatus(Errc::PlatformFailure, "setenv failed", static_cast<int64_t>(errno));
    }
#endif
    return okStatus();
}

Status unsetEnvironment(std::string_view name) {
    if (name.empty() || name.find('=') != std::string_view::npos) {
        return failStatus(Errc::InvalidArgument, "unsetEnvironment: invalid variable name");
    }
#if defined(_WIN32)
    const std::wstring key = toUtf16(name);
    if (::SetEnvironmentVariableW(key.c_str(), nullptr) == 0) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ENVVAR_NOT_FOUND) {
            return failStatus(Errc::PlatformFailure, "unsetEnvironment failed", static_cast<int64_t>(error));
        }
    }
#else
    const std::string key(name);
    if (::unsetenv(key.c_str()) != 0) {
        return failStatus(Errc::PlatformFailure, "unsetenv failed", static_cast<int64_t>(errno));
    }
#endif
    return okStatus();
}

char pathSeparator() noexcept {
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

std::string joinPath(std::string_view base, std::string_view leaf) {
    if (base.empty()) {
        return std::string(leaf);
    }
    if (leaf.empty()) {
        return std::string(base);
    }
    std::string out(base);
    const char sep = pathSeparator();
    const bool baseHasSep = out.back() == '/' || out.back() == '\\';
    const bool leafHasSep = leaf.front() == '/' || leaf.front() == '\\';
    if (baseHasSep && leafHasSep) {
        out.pop_back();
    } else if (!baseHasSep && !leafHasSep) {
        out.push_back(sep);
    }
    out.append(leaf);
    return out;
}

// ---------------------------------------------------------------------------
// Executable / application location
// ---------------------------------------------------------------------------

Result<std::string> executablePath() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return fail<std::string>(Error::platform("GetModuleFileNameW failed", static_cast<int64_t>(::GetLastError())));
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        if (buffer.size() >= 64 * 1024) {
            return fail<std::string>(Error{Errc::PlatformFailure, "executablePath: path exceeds 64 KiB"});
        }
        buffer.resize(buffer.size() * 2);
    }
    return toUtf8(buffer);
#elif defined(__APPLE__)
    uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return fail<std::string>(Error{Errc::PlatformFailure, "executablePath: _NSGetExecutablePath failed"});
    }
    // The returned path may contain symlinks; resolve them for a stable answer.
    char resolved[PATH_MAX];
    if (::realpath(buffer.data(), resolved) != nullptr) {
        return std::string(resolved);
    }
    return std::string(buffer.data());
#else
    std::vector<char> buffer(PATH_MAX, '\0');
    const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written <= 0) {
        return fail<std::string>(Error::platform("executablePath: readlink(/proc/self/exe) failed",
                                                 static_cast<int64_t>(errno)));
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
#endif
}

Result<std::string> applicationDirectory() {
    auto exe = executablePath();
    if (!exe) {
        return exe;
    }
    const std::size_t pos = exe->find_last_of("/\\");
    if (pos == std::string::npos) {
        return *exe;
    }
    return exe->substr(0, pos);
}

// ---------------------------------------------------------------------------
// Well known directories
// ---------------------------------------------------------------------------

Result<std::string> homeDirectory() {
#if defined(_WIN32)
    if (auto profile = environmentValue(L"USERPROFILE", "homeDirectory"); profile) {
        return profile;
    }
    return knownFolder(FOLDERID_Profile, "homeDirectory");
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return trimTrailingSeparators(std::string(home));
    }
    // Fallback for daemons with a scrubbed environment.
    long bufferSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufferSize <= 0) {
        bufferSize = 4096;
    }
    std::vector<char> buffer(static_cast<std::size_t>(bufferSize), '\0');
    passwd entry{};
    passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result) == 0 && result != nullptr &&
        result->pw_dir != nullptr) {
        return trimTrailingSeparators(std::string(result->pw_dir));
    }
    return fail<std::string>(Error{Errc::NotFound, "homeDirectory: $HOME is not set"});
#endif
}

Result<std::string> userDataDirectory() {
#if defined(_WIN32)
    return knownFolder(FOLDERID_RoamingAppData, "userDataDirectory");
#elif defined(__APPLE__)
    auto home = homeDirectory();
    if (!home) {
        return home;
    }
    return trimTrailingSeparators(*home + "/Library/Application Support");
#else
    return xdgDirectory("XDG_DATA_HOME", "/.local/share", "userDataDirectory");
#endif
}

Result<std::string> userConfigDirectory() {
#if defined(_WIN32)
    return knownFolder(FOLDERID_RoamingAppData, "userConfigDirectory");
#elif defined(__APPLE__)
    auto home = homeDirectory();
    if (!home) {
        return home;
    }
    return trimTrailingSeparators(*home + "/Library/Application Support");
#else
    return xdgDirectory("XDG_CONFIG_HOME", "/.config", "userConfigDirectory");
#endif
}

Result<std::string> userCacheDirectory() {
#if defined(_WIN32)
    auto local = knownFolder(FOLDERID_LocalAppData, "userCacheDirectory");
    if (!local) {
        return local;
    }
    return trimTrailingSeparators(*local + "\\Temp");
#elif defined(__APPLE__)
    auto home = homeDirectory();
    if (!home) {
        return home;
    }
    return trimTrailingSeparators(*home + "/Library/Caches");
#else
    return xdgDirectory("XDG_CACHE_HOME", "/.cache", "userCacheDirectory");
#endif
}

Result<std::string> temporaryDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (written == 0) {
            return fail<std::string>(Error::platform("GetTempPathW failed", static_cast<int64_t>(::GetLastError())));
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return trimTrailingSeparators(toUtf8(buffer));
#else
    return posixTemporaryDirectory();
#endif
}

} // namespace nativebridge

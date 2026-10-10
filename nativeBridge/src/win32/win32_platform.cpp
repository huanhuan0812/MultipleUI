// nativeBridge — Windows (Win32) backend: platform, message pump, clipboard.
// SPDX-License-Identifier: MIT
//
// THREADING MODEL
// ---------------
// The Win32 message queue belongs to the thread that created it, and a window
// procedure always runs on that thread. Consequently:
//  * Platform::create() records the calling thread as the UI thread,
//  * pump()/wait() only ever run there (Platform documents the same contract),
//  * commands issued from another thread are rejected with Err::PlatformFailure
//    instead of blocking forever in SendMessage,
//  * subscriber callbacks therefore always run on the polling thread.
//
// DPI MODEL (see also win32_common.cpp)
// -------------------------------------
// The process is per-monitor-DPI-aware (v2 when available). In that mode Win32
// already reports input in the logical space nativeBridge exposes, so the only
// conversion left is ScaleFactor = EffectiveDpi / 96 for the framebuffer size.

#include "win32/win32_backend.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include "common/backends.h"
#include "nativebridge/system.h"

using namespace nativebridge;

namespace nativebridge::detail {

// ---------------------------------------------------------------------------
// Monitor enumeration
// ---------------------------------------------------------------------------

namespace {

BOOL CALLBACK monitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
    auto* out = reinterpret_cast<DisplayList*>(userData);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == FALSE) {
        return TRUE;
    }

    DisplayInfo display{};
    // A monitor handle is stable for the session, which is the documented
    // contract of DisplayId (never a raw platform id).
    display.id = static_cast<DisplayId>(reinterpret_cast<uintptr_t>(monitor));
    display.name = toUtf8(info.szDevice);
    display.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    display.bounds = Rect{info.rcMonitor.left, info.rcMonitor.top,
                          info.rcMonitor.right - info.rcMonitor.left,
                          info.rcMonitor.bottom - info.rcMonitor.top};
    display.workArea = Rect{info.rcWork.left, info.rcWork.top,
                            info.rcWork.right - info.rcWork.left,
                            info.rcWork.bottom - info.rcWork.top};

    const POINT centre{(info.rcMonitor.left + info.rcMonitor.right) / 2,
                       (info.rcMonitor.top + info.rcMonitor.bottom) / 2};
    const uint32_t dpi = dpiForMonitorAt(centre);
    display.scaleFactor = static_cast<double>(dpi) / 96.0;
    display.pixelSize = PixelSize{static_cast<uint32_t>(static_cast<double>(display.bounds.width) * display.scaleFactor),
                                  static_cast<uint32_t>(static_cast<double>(display.bounds.height) * display.scaleFactor)};
    display.bitDepth = 32;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) != FALSE) {
        display.refreshRateHz = mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
        display.bitDepth = mode.dmBitsPerPel != 0 ? mode.dmBitsPerPel : 32;
        switch (mode.dmDisplayOrientation) {
            case DMDO_90:  display.rotationDegrees = 90; break;
            case DMDO_180: display.rotationDegrees = 180; break;
            case DMDO_270: display.rotationDegrees = 270; break;
            default:       display.rotationDegrees = 0; break;
        }
    }
    out->push_back(std::move(display));
    return TRUE;
}

} // namespace

void Win32Platform::rebuildDisplays() {
    DisplayList updated;
    if (::EnumDisplayMonitors(nullptr, nullptr, &monitorEnumProc, reinterpret_cast<LPARAM>(&updated)) == FALSE) {
        m_fatal = Error::platform("EnumDisplayMonitors failed", static_cast<int64_t>(::GetLastError()));
        return;
    }
    // Primary monitor first, so displays().front() is the primary one.
    std::stable_partition(updated.begin(), updated.end(), [](const DisplayInfo& d) { return d.isPrimary; });

    const bool changed =
        updated.size() != m_displays.size() ||
        !std::equal(updated.begin(), updated.end(), m_displays.begin(),
                    [](const DisplayInfo& a, const DisplayInfo& b) {
                        return a.id == b.id && a.bounds == b.bounds && a.scaleFactor == b.scaleFactor &&
                               a.workArea == b.workArea;
                    });
    m_displays = std::move(updated);

    if (changed && !m_displays.empty()) {
        Event event{};
        event.type = EventType::DisplayChanged;
        event.window = 0;
        event.timestampNs = monotonicNowNs();
        event.payload = DisplayChangedEvent{true};
        m_queue.push(std::move(event));
    }
}

void Win32Platform::refreshDisplays() {
    // No rate limiting: WM_DISPLAYCHANGE already wakes the pump and
    // EnumDisplayMonitors is cheap.
    rebuildDisplays();
}

// ---------------------------------------------------------------------------
// Platform lifecycle
// ---------------------------------------------------------------------------

Win32Platform::Win32Platform() {
    if (!isUiThread()) {
        m_fatal = Error{Errc::PlatformFailure, "win32 backend: the Platform must be created on its UI thread"};
        return;
    }
    enableDpiAwareness();
    m_lifetime->setDestroyCallback([this](WindowId id) { windowDestroyed(id); });
    if (!win32RegisterWindowClass(m_windowClass, m_className)) {
        m_fatal = Error::platform("RegisterClassExW failed", static_cast<int64_t>(::GetLastError()));
        return;
    }
    m_classRegistered = true;
    rebuildDisplays();
}

Win32Platform::~Win32Platform() {
    // Windows are documented to die before their Platform; invalidating the
    // lifetime token makes a surviving Window a harmless no-op.
    m_lifetime->invalidate();
    std::vector<Win32Window*> windows;
    {
        std::lock_guard lock(m_windowsMutex);
        windows = m_windows;
        m_windows.clear();
    }
    for (Win32Window* window : windows) {
        window->onNativeDestroyed();
    }
    m_clipboard.reset();
}

bool Win32Platform::supports(Capability capability) const noexcept {
    switch (capability) {
        case Capability::ScreenCaptureLock:
            return false; // capture is out of scope
        default:
            return true;
    }
}

std::string Win32Platform::describe() const {
    std::string out = "win32: monitors=" + std::to_string(m_displays.size()) + ", DPI mode=per-monitor-v2";
    if (!m_displays.empty()) {
        const DisplayInfo& primary = m_displays.front();
        out += ", primary=" + std::to_string(primary.bounds.width) + "x" +
               std::to_string(primary.bounds.height) + " logical @" + std::to_string(primary.scaleFactor) + "x";
        if (primary.refreshRateHz != 0) {
            out += " " + std::to_string(primary.refreshRateHz) + "Hz";
        }
        if (!primary.name.empty()) {
            out += " (" + primary.name + ")";
        }
    }
    return out;
}

Result<std::unique_ptr<WindowBackend>> Win32Platform::createWindow(const WindowCreateInfo& info) {
    if (!isUiThread()) {
        return fail<std::unique_ptr<WindowBackend>>(Error{
            Errc::PlatformFailure,
            "createWindow: Win32 windows are bound to the thread that created the Platform"});
    }
    if (!m_classRegistered) {
        return fail<std::unique_ptr<WindowBackend>>(
            Error{Errc::PlatformFailure, "createWindow: the nativeBridge window class is not registered"});
    }

    const WindowId id = nextWindowId();
    auto window = std::make_unique<Win32Window>(*this, id, info);
    if (window->hwnd() == nullptr) {
        return fail<std::unique_ptr<WindowBackend>>(
            Error::platform("CreateWindowExW failed", static_cast<int64_t>(::GetLastError())));
    }
    {
        std::lock_guard lock(m_windowsMutex);
        m_windows.push_back(window.get());
    }

    Event created = Event{};
    created.type = EventType::WindowCreated;
    created.window = id;
    created.timestampNs = monotonicNowNs();
    CreatedEvent payload{};
    payload.clientSize = window->clientSize();
    payload.windowSize = window->windowSize();
    payload.scaleFactor = window->scaleFactor();
    created.payload = payload;
    m_queue.push(std::move(created));

    return std::unique_ptr<WindowBackend>(window.release());
}

void Win32Platform::windowDestroyed(WindowId id) {
    std::lock_guard lock(m_windowsMutex);
    const auto it = std::find_if(m_windows.begin(), m_windows.end(),
                                 [id](const Win32Window* window) { return window->id() == id; });
    if (it != m_windows.end()) {
        m_windows.erase(it);
    }
}

void Win32Platform::destroyWindow(WindowBackend* window) {
    if (window != nullptr) {
        windowDestroyed(window->id());
    }
}

HWND Win32Platform::hwndForWindowId(WindowId id) const {
    std::lock_guard lock(m_windowsMutex);
    for (const Win32Window* window : m_windows) {
        if (window->id() == id) {
            return window->hwnd();
        }
    }
    return nullptr;
}

void Win32Platform::broadcastDisplayChange() {
    rebuildDisplays();
    refreshScaleFactors();
}

void Win32Platform::refreshScaleFactors() {
    std::vector<Win32Window*> windows;
    {
        std::lock_guard lock(m_windowsMutex);
        windows = m_windows;
    }
    for (Win32Window* window : windows) {
        window->onDisplayConfigurationChanged();
    }
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

void Win32Platform::pump() {
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        if (message.message == WM_QUIT) {
            // Surface the quit request as a fatal condition so pollEvents()
            // callers stop looping, without tearing the platform down.
            m_fatal = Error{Errc::Cancelled, "the Win32 message queue received WM_QUIT"};
            continue;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

std::size_t Win32Platform::wait(std::chrono::milliseconds timeout) {
    // GetMessage cannot express a timeout, so block in MsgWaitForMultipleObjects
    // (zero handles) which wakes for messages, the timeout, or the queue being
    // non-empty - exactly the semantics waitEvents() documents.
    const DWORD waitMs = timeout.count() < 0 ? INFINITE : static_cast<DWORD>(timeout.count());
    const DWORD result = ::MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT) {
        pump();
    }
    return m_queue.size();
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

namespace {

/// RAII owner of the Win32 clipboard lock (never left open on an error path).
class ClipboardLock {
public:
    explicit ClipboardLock(HWND owner) : m_owner(owner) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (::OpenClipboard(m_owner) != FALSE) {
                m_open = true;
                return;
            }
            ::Sleep(5); // another process may hold the clipboard briefly
        }
    }
    ~ClipboardLock() {
        if (m_open) {
            ::CloseClipboard();
        }
    }
    ClipboardLock(const ClipboardLock&) = delete;
    ClipboardLock& operator=(const ClipboardLock&) = delete;
    [[nodiscard]] bool isOpen() const noexcept { return m_open; }

private:
    HWND m_owner{nullptr};
    bool m_open{false};
};

class Win32Clipboard final : public Clipboard {
public:
    [[nodiscard]] Result<std::string> getText() override {
        ClipboardLock lock(nullptr);
        if (!lock.isOpen()) {
            return fail<std::string>(
                Error::platform("clipboard: OpenClipboard failed", static_cast<int64_t>(::GetLastError())));
        }
        if (::IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE) {
            return fail<std::string>(Error{Errc::NotFound, "clipboard: no text on the clipboard"});
        }
        HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
        if (handle == nullptr) {
            return fail<std::string>(
                Error::platform("clipboard: GetClipboardData failed", static_cast<int64_t>(::GetLastError())));
        }
        const auto* text = static_cast<const wchar_t*>(::GlobalLock(handle));
        if (text == nullptr) {
            return fail<std::string>(
                Error::platform("clipboard: GlobalLock failed", static_cast<int64_t>(::GetLastError())));
        }
        std::string utf8 = toUtf8(std::wstring(text));
        ::GlobalUnlock(handle);
        return utf8;
    }

    [[nodiscard]] Status setText(std::string_view utf8Text) override {
        const std::wstring wide = toUtf16(utf8Text);
        const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);

        HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory == nullptr) {
            return failStatus(Errc::OutOfMemory, "clipboard: GlobalAlloc failed");
        }
        if (void* target = ::GlobalLock(memory); target != nullptr) {
            std::memcpy(target, wide.c_str(), bytes);
            ::GlobalUnlock(memory);
        } else {
            ::GlobalFree(memory);
            return failStatus(Errc::PlatformFailure, "clipboard: GlobalLock failed");
        }

        ClipboardLock lock(nullptr);
        if (!lock.isOpen()) {
            ::GlobalFree(memory);
            return failStatus(Errc::PlatformFailure, "clipboard: OpenClipboard failed");
        }
        if (::EmptyClipboard() == FALSE || ::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
            ::GlobalFree(memory);
            return failStatus(Errc::PlatformFailure, "clipboard: SetClipboardData failed");
        }
        // Ownership moved to the clipboard: do not free `memory` here.
        return okStatus();
    }

    [[nodiscard]] Status clear() override {
        ClipboardLock lock(nullptr);
        if (!lock.isOpen()) {
            return failStatus(Errc::PlatformFailure, "clipboard: OpenClipboard failed");
        }
        if (::EmptyClipboard() == FALSE) {
            return failStatus(Errc::PlatformFailure, "clipboard: EmptyClipboard failed");
        }
        return okStatus();
    }

    [[nodiscard]] bool hasText() override {
        ClipboardLock lock(nullptr);
        return lock.isOpen() && ::IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    }

    [[nodiscard]] std::string_view backendName() const noexcept override { return "win32"; }
};

} // namespace

Clipboard& Win32Platform::clipboard() {
    if (m_clipboard == nullptr) {
        m_clipboard = std::make_unique<Win32Clipboard>();
    }
    return *m_clipboard;
}

// ---------------------------------------------------------------------------
// Factory and registration
// ---------------------------------------------------------------------------

Result<std::unique_ptr<PlatformBackend>> createWin32Platform() {
    return std::unique_ptr<PlatformBackend>(std::make_unique<Win32Platform>());
}

namespace {

/// Referenced from src/common/platform.cpp so this object file stays in the link
/// and the registration below is always executed.
[[maybe_unused]] const bool kRegistered = [] {
    registerBackend("win32", &createWin32Platform);
    return true;
}();

} // namespace

} // namespace nativebridge::detail

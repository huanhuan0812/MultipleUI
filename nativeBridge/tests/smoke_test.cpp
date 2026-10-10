// nativeBridge smoke test.
// SPDX-License-Identifier: MIT
//
// A dependency-free test binary: it exercises the public API and reports
// PASS/FAIL/SKIP per check, returning the number of failures as exit status.
//
// Design notes
// ------------
// * Everything here is platform-macro free; the same source builds on Windows,
//   macOS, X11 and Wayland.
// * Checks that need a live display server are SKIPPED (not failed) when the
//   platform cannot be created, so the suite is usable in CI containers. Set
//   NATIVEBRIDGE_SMOKE_REQUIRE_DISPLAY=1 to turn a missing display into a
//   failure, and NATIVEBRIDGE_SMOKE_RUN_WINDOW=1 to also *create* a window
//   (which requires a real window server and is off by default).
//
// Run: ./build/nativebridge_smoke_test

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nativebridge/nativebridge.h"

namespace {

using namespace nativebridge;

int g_failures = 0;
int g_checks = 0;
int g_skips = 0;

void check(bool condition, std::string_view what) {
    ++g_checks;
    if (condition) {
        std::printf("  [PASS] %.*s\n", static_cast<int>(what.size()), what.data());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %.*s\n", static_cast<int>(what.size()), what.data());
    }
}

void skip(std::string_view what, std::string_view why) {
    ++g_skips;
    std::printf("  [SKIP] %.*s (%s)\n", static_cast<int>(what.size()), what.data(),
                std::string(why).c_str());
}

template <class T>
void checkOk(const Result<T>& result, std::string_view what) {
    if (result) {
        check(true, what);
    } else {
        ++g_checks;
        ++g_failures;
        std::printf("  [FAIL] %.*s -> %s\n", static_cast<int>(what.size()), what.data(),
                    result.error().describe().c_str());
    }
}

bool envFlag(const char* name) {
    const auto value = environment(name);
    return value.has_value() && (*value == "1" || *value == "true" || *value == "yes");
}

// ---------------------------------------------------------------------------
// 1. Error type
// ---------------------------------------------------------------------------

void testResult() {
    std::printf("[result]\n");

    Result<int> good = 42;
    check(good.has_value() && *good == 42, "Result<int> carries a value");
    check(static_cast<bool>(good), "Result<int> converts to bool");

    Result<int> bad = fail<int>(Error::unsupported("test feature"));
    check(!bad.has_value(), "failed Result reports no value");
    check(bad.error().code == Errc::Unsupported, "Errc::Unsupported is preserved");
    check(bad.error().message.find("test feature") != std::string::npos, "error message is preserved");
    check(!bad.error().describe().empty(), "Error::describe() is non-empty");

    Status status = okStatus();
    check(status.has_value(), "okStatus() succeeds");
    Status failure = failStatus(Errc::NotFound, "missing");
    check(!failure.has_value() && failure.error().code == Errc::NotFound, "failStatus() fails as expected");

    check(toString(Errc::Timeout) == "Timeout", "Errc has a stable name");
}

// ---------------------------------------------------------------------------
// 2. Key codes
// ---------------------------------------------------------------------------

void testKeyCodes() {
    std::printf("[keycode]\n");

    check(toString(KeyCode::A) == "A", "KeyCode::A has a name");
    check(toString(KeyCode::ArrowLeft) == "ArrowLeft", "arrow keys have names");
    check(static_cast<uint32_t>(KeyCode::A) == 0x04, "KeyCode values follow USB HID usage ids");
    check(static_cast<uint32_t>(KeyCode::Z) == 0x1D, "letters are contiguous");

    const Modifier modifiers = Modifier::Shift | Modifier::Control;
    check(hasModifier(modifiers, Modifier::Shift) && hasModifier(modifiers, Modifier::Control),
          "modifier bits compose");
    check(!hasModifier(modifiers, Modifier::Alt), "unset modifier bits stay unset");

    // The X11/Wayland keysym mapping is exposed through mapVirtualKey.
    check(mapVirtualKey('a') == KeyCode::A, "keysym 'a' maps to KeyCode::A");
    check(mapVirtualKey(0xFF51) == KeyCode::ArrowLeft, "keysym XK_Left maps to ArrowLeft");
    check(mapVirtualKey(0x0020) == KeyCode::Space, "keysym space maps to Space");
    check(mapVirtualKey(0xDEADBEEF) == KeyCode::Unknown, "unknown keysyms map to Unknown");
}

// ---------------------------------------------------------------------------
// 3. System module (works without a display server)
// ---------------------------------------------------------------------------

void testSystem() {
    std::printf("[system]\n");

    const auto start = monotonicTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto elapsed = monotonicTime() - start;
    check(elapsed > std::chrono::milliseconds(10), "monotonicTime() advances while sleeping");
    check(elapsed < std::chrono::seconds(5), "monotonicTime() is not absurdly far off");

    const auto wall = systemTime();
    check(wall.time_since_epoch().count() > 0, "systemTime() returns a plausible epoch");
    const auto offset = localTimeZoneOffset(wall);
    check(offset >= std::chrono::minutes(-14 * 60) && offset <= std::chrono::minutes(14 * 60),
          "localTimeZoneOffset() is within +-14 h");

    const auto formatted = formatTime(wall, "%Y-%m-%d");
    check(formatted.has_value() && formatted->size() == 10, "formatTime() honours the pattern");
    check(!formatTime(wall, std::string_view{}).has_value(), "formatTime() rejects an empty pattern");

    checkOk(executablePath(), "executablePath()");
    checkOk(applicationDirectory(), "applicationDirectory()");
    checkOk(homeDirectory(), "homeDirectory()");
    checkOk(temporaryDirectory(), "temporaryDirectory()");
    checkOk(userDataDirectory(), "userDataDirectory()");
    checkOk(userConfigDirectory(), "userConfigDirectory()");
    checkOk(userCacheDirectory(), "userCacheDirectory()");

    const auto joined = joinPath("base", "leaf");
    check(joined.find("leaf") != std::string::npos, "joinPath() keeps the leaf");
    check(joined.size() > std::string("baseleaf").size(), "joinPath() inserts a separator");

    check(processorCount() >= 1, "processorCount() is at least 1");
    check(!commandLineArguments().empty(), "commandLineArguments() has at least argv[0]");

    const auto uniqueName = std::string("NATIVEBRIDGE_SMOKE_TEST_VAR");
    checkOk(setEnvironment(uniqueName, "42"), "setEnvironment()");
    const auto read = environment(uniqueName);
    check(read.has_value() && *read == "42", "environment() reads back what was set");
    checkOk(unsetEnvironment(uniqueName), "unsetEnvironment()");
    check(!environment(uniqueName).has_value(), "environment() reports the variable as unset");
    check(!setEnvironment("", "x").has_value(), "setEnvironment() rejects an empty name");
}

// ---------------------------------------------------------------------------
// 4. Platform level checks (need a display server)
// ---------------------------------------------------------------------------

void testPlatformSurface() {
    std::printf("[platform]\n");

    const std::vector<std::string> available = Platform::availableBackends();
    check(!available.empty(), "at least one backend is compiled in");
    std::printf("        compiled backends:");
    for (const auto& name : available) {
        std::printf(" %s", name.c_str());
    }
    std::printf("\n");

    auto created = Platform::create();
    if (!created) {
        if (envFlag("NATIVEBRIDGE_SMOKE_REQUIRE_DISPLAY")) {
            ++g_checks;
            ++g_failures;
            std::printf("  [FAIL] Platform::create() -> %s\n", created.error().describe().c_str());
        } else {
            skip("Platform::create()", "no display server available");
            for (const char* name : {"Platform::describe()", "Platform::displays()", "Platform::createWindow()",
                                     "Platform::clipboard()", "SurfaceBridge"}) {
                skip(name, "platform unavailable");
            }
        }
        return;
    }

    std::unique_ptr<Platform> platform = std::move(*created);
    check(true, "Platform::create()");
    check(!platform->backendName().empty(), "backendName() is not empty");
    check(!platform->describe().empty(), "describe() is not empty");
    check(!Platform::availableBackends().empty(), "availableBackends() is not empty");
    check(platform->backendName() == "win32" || platform->backendName() == "cocoa" ||
              platform->backendName() == "x11" || platform->backendName() == "wayland",
          "backendName() is one of the four known backends");

    (void)platform->pollEvents();
    check(true, "pollEvents() on an empty queue returns");

    const auto& displays = platform->displays();
    if (displays.empty()) {
        skip("displays()", "the backend reported no monitors");
    } else {
        check(true, std::string("displays() reported " + std::to_string(displays.size()) + " monitor(s)"));
        bool primaryFound = platform->primaryDisplay().has_value();
        check(primaryFound, "primaryDisplay() returns a monitor when displays exist");
        for (const auto& display : displays) {
            check(display.bounds.width > 0 && display.bounds.height > 0, "every display has a positive size");
            check(display.scaleFactor > 0.0, "every display has a positive scale factor");
        }
        const auto atOrigin = platform->displayAt(Point{0, 0});
        check(atOrigin.has_value(), "displayAt({0,0}) finds a monitor");
    }

    if (!envFlag("NATIVEBRIDGE_SMOKE_RUN_WINDOW")) {
        skip("createWindow()", "set NATIVEBRIDGE_SMOKE_RUN_WINDOW=1 to open a real window");
    } else {
        WindowCreateInfo info{};
        info.title = "nativeBridge smoke test";
        info.clientSize = Size{640, 400};
        info.visible = true;
        auto windowResult = platform->createWindow(info);
        if (!windowResult) {
            ++g_checks;
            ++g_failures;
            std::printf("  [FAIL] createWindow() -> %s\n", windowResult.error().describe().c_str());
        } else {
            Window window = std::move(*windowResult);
            check(window.isValid(), "createWindow() returns a valid window");
            check(window.clientSize().width > 0 && window.clientSize().height > 0, "client area has a size");
            check(window.framebufferSize().width >= static_cast<uint32_t>(window.clientSize().width),
                  "framebuffer is at least as large as the client area");
            check(window.scaleFactor() > 0.0, "window scale factor is positive");
            check(window.nativeHandle().window.valid(), "nativeHandle() is populated");
            checkOk(window.setTitle("nativeBridge smoke test (renamed)"), "setTitle()");
            checkOk(window.resize(Size{800, 500}), "resize()");
            checkOk(window.show(), "show()");
            (void)platform->pollEvents();
            const auto descriptor = platform->describeSurface(window, RenderApi::Generic);
            checkOk(descriptor, "describeSurface() for a generic renderer");
        }
    }

    if (!platform->supports(Capability::ClipboardText)) {
        skip("clipboard()", "backend reports no clipboard support");
    } else {
        auto& clipboard = platform->clipboard();
        check(clipboard.backendName() == platform->backendName(), "clipboard reports the backend name");
        const std::string text = "nativeBridge smoke test";
        auto written = clipboard.setText(text);
        if (!written) {
            skip("clipboard round trip", written.error().describe());
        } else {
            auto readBack = clipboard.getText();
            if (!readBack) {
                // Wayland legitimately refuses without keyboard focus.
                skip("clipboard round trip", readBack.error().describe());
            } else {
                check(*readBack == text, "clipboard round trip returns the same text");
            }
        }
    }

    (void)platform->pollEvents();
}

} // namespace

int main() {
    std::printf("nativeBridge %s smoke test\n", std::string(version()).c_str());
    std::printf("features:");
    for (const auto& feature : compiledFeatures()) {
        std::printf(" %s", feature.c_str());
    }
    std::printf("\n\n");

    testResult();
    testKeyCodes();
    testSystem();
    testPlatformSurface();

    std::printf("\n%d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skips);
    if (g_failures == 0) {
        std::printf("SMOKE TEST PASSED\n");
    } else {
        std::printf("SMOKE TEST FAILED\n");
    }
    return g_failures;
}

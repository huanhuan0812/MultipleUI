// nativeBridge example — create a window, process events, print them.
// SPDX-License-Identifier: MIT
//
// Build (see README.md for the full matrix):
//   cmake -S . -B build && cmake --build build
//   ./build/nativebridge_example_create_window
//
// The example is intentionally platform-macro free: there is not a single
// #ifdef on a platform name in this file.
//
// Controls
//   close the window (titlebar / Cmd+W / Alt+F4) or press Escape to quit
//   F1   toggle full screen
//   F2   toggle always-on-top
//   F3   resize the client area to 960x540
//   F4   center the window
//   F5   hide the window for one second (exercises hide/show + events)
//   F6   put text on the clipboard and read it back
//   F7   print display information
//   F8   describe the native surface (renderer interop, no rendering here)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "nativebridge/nativebridge.h"

namespace {

using namespace nativebridge;

const char* yesNo(bool value) { return value ? "yes" : "no"; }

void printPlatformInfo(const Platform& platform) {
    std::printf("--- platform -------------------------------------------------\n");
    std::printf("%s\n", platform.describe().c_str());
    std::printf("  supports: position=%s decorations=%s clipboard=%s multiDisplay=%s dpiEvents=%s\n",
                yesNo(platform.supports(Capability::WindowPosition)),
                yesNo(platform.supports(Capability::WindowDecoration)),
                yesNo(platform.supports(Capability::ClipboardText)),
                yesNo(platform.supports(Capability::MultiDisplay)),
                yesNo(platform.supports(Capability::DpiChangeEvents)));
    std::printf("  features:");
    for (const auto& feature : compiledFeatures()) {
        std::printf(" %s", feature.c_str());
    }
    std::printf("\n");
}

void printDisplays(const Platform& platform) {
    std::printf("--- displays (%zu) -------------------------------------------\n", platform.displays().size());
    for (const DisplayInfo& display : platform.displays()) {
        std::printf("  #%llu %-24s %s %4dx%-4d @%d,%-4d scale=%.2f px=%ux%u %uHz\n",
                    static_cast<unsigned long long>(display.id),
                    display.name.empty() ? "(unnamed)" : display.name.c_str(),
                    display.isPrimary ? "primary" : "       ",
                    display.bounds.width, display.bounds.height,
                    display.bounds.x, display.bounds.y,
                    display.scaleFactor,
                    display.pixelSize.width, display.pixelSize.height,
                    display.refreshRateHz);
        std::printf("        work area %dx%d @%d,%d\n",
                    display.workArea.width, display.workArea.height,
                    display.workArea.x, display.workArea.y);
    }
}

void printSurface(Platform& platform, Window& window) {
    std::printf("--- surface ---------------------------------------------------\n");
    auto descriptor = platform.describeSurface(window, RenderApi::Generic);
    if (!descriptor) {
        std::printf("  describeSurface failed: %s\n", descriptor.error().describe().c_str());
        return;
    }
    // NOTE: backendName() returns a string_view; it must be materialised into a
    // std::string before being passed to a C varargs function.
    const std::string bridgeName(platform.surfaceBridge(window).backendName());
    std::printf("  backend=%s window.kind=%u window.value=0x%llx view.kind=%u view.value=0x%llx\n",
                bridgeName.c_str(),
                static_cast<unsigned>(descriptor->window.kind),
                static_cast<unsigned long long>(descriptor->window.value),
                static_cast<unsigned>(descriptor->view.kind),
                static_cast<unsigned long long>(descriptor->view.value));
    std::printf("  framebuffer=%ux%u logical=%dx%d scale=%.2f\n",
                descriptor->framebufferSize.width, descriptor->framebufferSize.height,
                descriptor->logicalSize.width, descriptor->logicalSize.height,
                descriptor->scaleFactor);
}

void printSystemInfo() {
    std::printf("--- system ---------------------------------------------------\n");
    const auto executable = executablePath();
    const auto data = userDataDirectory();
    const auto cache = userCacheDirectory();
    const auto temp = temporaryDirectory();
    std::printf("  executable      : %s\n", executable ? executable->c_str() : executable.error().describe().c_str());
    std::printf("  user data       : %s\n", data ? data->c_str() : data.error().describe().c_str());
    std::printf("  user cache      : %s\n", cache ? cache->c_str() : cache.error().describe().c_str());
    std::printf("  temporary       : %s\n", temp ? temp->c_str() : temp.error().describe().c_str());
    std::printf("  logical cores   : %u\n", processorCount());
    const auto arguments = commandLineArguments();
    std::printf("  arguments (%zu)  :", arguments.size());
    for (const auto& argument : arguments) {
        std::printf(" [%s]", argument.c_str());
    }
    std::printf("\n");

    const auto start = monotonicTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto elapsed = monotonicTime() - start;
    std::printf("  monotonic clock : %lld ns total, 5 ms sleep measured as %.3f ms\n",
                static_cast<long long>(monotonicTime().count()),
                std::chrono::duration<double, std::milli>(elapsed).count());
    const auto formatted = formatTime(systemTime(), "%Y-%m-%d %H:%M:%S %Z");
    std::printf("  local time      : %s (UTC offset %lld min)\n",
                formatted ? formatted->c_str() : formatted.error().describe().c_str(),
                static_cast<long long>(localTimeZoneOffset(systemTime()).count()));
}

/// Demonstrate the clipboard round trip.
void clipboardRoundTrip(Platform& platform) {
    const std::string text = "nativeBridge clipboard round trip";
    auto written = platform.clipboard().setText(text);
    std::printf("clipboard: setText -> %s\n", written ? "ok" : written.error().describe().c_str());
    auto read = platform.clipboard().getText();
    if (!read) {
        std::printf("clipboard: getText -> %s\n", read.error().describe().c_str());
        return;
    }
    std::printf("clipboard: getText -> \"%s\" (%s)\n", read->c_str(), *read == text ? "round trip ok" : "MISMATCH");
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // Unbuffered stdout: the example is frequently run in the background or
    // piped to a log, where line buffering would delay every diagnostic.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("nativeBridge %s example (create_window)\n", std::string(version()).c_str());

    auto platformResult = Platform::create();
    if (!platformResult) {
        std::fprintf(stderr, "Platform::create failed:\n%s\n", platformResult.error().describe().c_str());
        return 1;
    }
    std::unique_ptr<Platform> platform = std::move(*platformResult);

    printPlatformInfo(*platform);
    printSystemInfo();
    printDisplays(*platform);

    WindowCreateInfo info{};
    info.title = "nativeBridge — create_window example";
    info.clientSize = Size{1024, 640};
    info.minSize = Size{320, 240};
    info.visible = true;
    info.centered = true;

    auto windowResult = platform->createWindow(info);
    if (!windowResult) {
        std::fprintf(stderr, "createWindow failed: %s\n", windowResult.error().describe().c_str());
        return 1;
    }
    Window window = std::move(*windowResult);

    std::printf("--- window ---------------------------------------------------\n");
    const std::string windowBackend(window.backendName());
    std::printf("  id=%llu backend=%s client=%dx%d framebuffer=%ux%u scale=%.2f position=%d,%d\n",
                static_cast<unsigned long long>(window.id()),
                windowBackend.c_str(),
                window.clientSize().width, window.clientSize().height,
                window.framebufferSize().width, window.framebufferSize().height,
                window.scaleFactor(), window.position().x, window.position().y);
    std::printf("  supported: release=%s\n", yesNo(window.supports(Capability::ClientSideResize)));

    // Window level listener: every event type, printed as it is dispatched.
    const SubscriptionToken loggingToken =
        window.addEventListener([](const Event& event) { std::printf("[event] %s\n", describe(event).c_str()); });
    (void)loggingToken; // Tokens are only needed to unsubscribe again.

    // Platform level listener: monitor changes have no window.
    const SubscriptionToken displayToken = platform->addEventListener(EventType::DisplayChanged, [](const Event&) {
        std::printf("[platform] the monitor configuration changed\n");
    });
    (void)displayToken;

    // Application state driven by the shortcuts and window events below.
    bool running = true;
    bool hidePending = false;
    std::chrono::steady_clock::time_point showAt{};

    auto shortcutHandler = [&](const Event& event) {
        const KeyEvent* key = event.asKey();
        if (key == nullptr || key->isRepeat) {
            return;
        }
        switch (key->key) {
            case KeyCode::Escape:
                running = false;
                break;
            case KeyCode::F1:
                std::printf("[app] toggling fullscreen -> %s\n", window.isFullscreen() ? "off" : "on");
                (void)window.setFullscreen(!window.isFullscreen());
                break;
            case KeyCode::F2:
                std::printf("[app] toggling always-on-top -> on\n");
                (void)window.setAlwaysOnTop(true);
                break;
            case KeyCode::F3:
                std::printf("[app] resizing the client area to 960x540\n");
                (void)window.resize(Size{960, 540});
                break;
            case KeyCode::F4:
                std::printf("[app] centering on the current display\n");
                (void)window.center();
                break;
            case KeyCode::F5:
                std::printf("[app] hiding the window for 500 ms\n");
                (void)window.hide();
                hidePending = true;
                showAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                break;
            case KeyCode::F6:
                clipboardRoundTrip(*platform);
                break;
            case KeyCode::F7:
                printDisplays(*platform);
                break;
            case KeyCode::F8:
                printSurface(*platform, window);
                break;
            default:
                break;
        }
    };

    // Registered on the wildcard slot: the handler only reacts to key events, so
    // no per-type subscription is required.
    const SubscriptionToken shortcutToken = window.addEventListener([&](const Event& event) {
        if (event.type == EventType::KeyDown) {
            shortcutHandler(event);
        }
    });
    (void)shortcutToken;

    const SubscriptionToken closeToken = window.addEventListener(EventType::CloseRequested, [&running](const Event&) {
        std::printf("[app] close requested, shutting down\n");
        running = false;
    });
    (void)closeToken;

    std::printf("--- entering the event loop (close the window or press Escape) -\n");
    while (running) {
        // pollEvents() collects the platform messages and dispatches the
        // subscribers on this thread; it returns the number of raw events seen.
        (void)platform->pollEvents();

        if (hidePending && std::chrono::steady_clock::now() >= showAt) {
            hidePending = false;
            (void)window.show();
        }

        // Nothing to do right now: block until the next platform event instead
        // of spinning, but never wait longer than one frame.
        (void)platform->waitEvents(std::chrono::milliseconds(16));
    }

    std::printf("--- shutting down -------------------------------------------\n");
    window = Window{};       // Window is move-only: reset by move assigning an empty one
    platform.reset();
    std::printf("clean exit\n");
    return 0;
}

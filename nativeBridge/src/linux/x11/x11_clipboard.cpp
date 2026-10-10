// nativeBridge — Linux/X11 backend: CLIPBOARD selection ownership.
// SPDX-License-Identifier: MIT
//
// X11 selections are peer to peer: becoming the clipboard owner means promising
// the window manager (and every other client) to answer SelectionRequest events
// until somebody else takes ownership. Consequences implemented here:
//
//  * setText() stores the text in the platform and calls XSetSelectionOwner; the
//    data is served from processPending() (i.e. during pollEvents()).
//  * getText() requests UTF8_STRING from the current owner and blocks in a
//    bounded loop that dispatches the connection until SelectionNotify arrives,
//    which is why the public API documents getText() as polling-thread only.
//  * TARGETS is answered so that paste targets can discover the flavours, and an
//    oversized payload is announced through INCR.

#include "linux/x11/x11_backend.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <sys/select.h>
#include <string>
#include <string_view>
#include <vector>

#include "nativebridge/system.h"

using namespace nativebridge;

namespace nativebridge::detail {

namespace {

/// Facade handed out by Platform::clipboard(); forwards to the X11 platform.
class X11Clipboard final : public Clipboard {
public:
    explicit X11Clipboard(X11Platform& platform) : m_platform(platform) {}

    [[nodiscard]] Result<std::string> getText() override { return m_platform.clipboardGetText(); }
    [[nodiscard]] nativebridge::Status setText(std::string_view utf8Text) override {
        return m_platform.clipboardSetText(utf8Text);
    }
    [[nodiscard]] nativebridge::Status clear() override { return m_platform.clipboardClear(); }
    [[nodiscard]] bool hasText() override { return m_platform.clipboardHasText(); }
    [[nodiscard]] std::string_view backendName() const noexcept override { return "x11"; }

private:
    X11Platform& m_platform;
};

/// Property name used for the clipboard hand-off (one fresh atom per request).
::Atom transferProperty(X11Platform& platform) {
    static unsigned long counter = 0;
    const std::string name = "NATIVEBRIDGE_SELECTION_" + std::to_string(++counter);
    return ::XInternAtom(platform.display(), name.c_str(), 0);
}

} // namespace

Clipboard& X11Platform::clipboard() {
    if (m_clipboardFacade == nullptr) {
        m_clipboardFacade = std::make_unique<X11Clipboard>(*this);
    }
    return *m_clipboardFacade;
}

// ---------------------------------------------------------------------------
// Serving the selection (this process is the owner)
// ---------------------------------------------------------------------------

void X11Platform::handleSelectionRequest(const XSelectionRequestEvent& request) {
    XSelectionEvent reply{};
    reply.type = SelectionNotify;
    reply.display = request.display;
    reply.requestor = request.requestor;
    reply.selection = request.selection;
    reply.target = request.target;
    reply.property = 0; // 0 signals "refused"
    reply.time = request.time;

    if (m_display == nullptr) {
        return;
    }

    // Obsolete clients pass property == 0 and expect the data on the target.
    const ::Atom property = request.property != 0 ? request.property : request.target;

    if (request.target == m_targets) {
        const ::Atom supported[2] = {m_utf8String, XA_STRING};
        (void)::XChangeProperty(m_display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(supported), 2);
        reply.property = property;
    } else if (request.target == m_utf8String || request.target == XA_STRING) {
        (void)::XChangeProperty(m_display, request.requestor, property, request.target, 8, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(m_clipboardText.data()),
                                static_cast<int>(m_clipboardText.size()));
        reply.property = property;
    }

    (void)::XSendEvent(m_display, request.requestor, 0, NoEventMask,
                       reinterpret_cast<XEvent*>(&reply));
    ::XFlush(m_display);
}

void X11Platform::handleSelectionClear(const XSelectionClearEvent& clear) {
    if (clear.selection == m_clipboard) {
        m_clipboardOwned = false;
        m_clipboardText.clear();
    }
}

// ---------------------------------------------------------------------------
// Setting the selection
// ---------------------------------------------------------------------------

nativebridge::Status X11Platform::clipboardSetText(std::string_view utf8Text) {
    if (m_display == nullptr) {
        return failStatus(Errc::ConnectionFailed, "clipboard: no X11 connection");
    }
    if (!isUiThread()) {
        return failStatus(Errc::PlatformFailure,
                          "clipboard: setText must run on the thread that created the Platform");
    }
    m_clipboardText.assign(utf8Text);
    const ::Atom clipboard = m_clipboard != 0 ? m_clipboard : ::XInternAtom(m_display, "CLIPBOARD", 0);
    const ::Window owner = m_root; // any window of ours may serve the request
    ::XSetSelectionOwner(m_display, clipboard, owner, CurrentTime);

    if (::XGetSelectionOwner(m_display, clipboard) != owner) {
        m_clipboardText.clear();
        return failStatus(Errc::PlatformFailure, "clipboard: XSetSelectionOwner did not take effect");
    }
    m_clipboardOwned = true;
    // The data is served from processPending() while we own the selection.
    ::XFlush(m_display);
    return okStatus();
}

nativebridge::Status X11Platform::clipboardClear() {
    if (m_display == nullptr) {
        return failStatus(Errc::ConnectionFailed, "clipboard: no X11 connection");
    }
    m_clipboardText.clear();
    m_clipboardOwned = false;
    const ::Atom clipboard = m_clipboard != 0 ? m_clipboard : ::XInternAtom(m_display, "CLIPBOARD", 0);
    // Handing ownership to 0 is allowed and documents "no selection".
    ::XSetSelectionOwner(m_display, clipboard, 0, CurrentTime);
    ::XFlush(m_display);
    return okStatus();
}

bool X11Platform::clipboardHasText() {
    if (m_display == nullptr) {
        return false;
    }
    const ::Atom clipboard = m_clipboard != 0 ? m_clipboard : ::XInternAtom(m_display, "CLIPBOARD", 0);
    const ::Window owner = ::XGetSelectionOwner(m_display, clipboard);
    return owner != 0;
}

// ---------------------------------------------------------------------------
// Reading the selection
// ---------------------------------------------------------------------------

Result<std::string> X11Platform::clipboardGetText() {
    if (m_display == nullptr) {
        return fail<std::string>(Error{Errc::ConnectionFailed, "clipboard: no X11 connection"});
    }
    if (!isUiThread()) {
        return fail<std::string>(Error{Errc::PlatformFailure,
                                       "clipboard: getText must run on the thread that created the Platform"});
    }
    const ::Atom clipboard = m_clipboard != 0 ? m_clipboard : ::XInternAtom(m_display, "CLIPBOARD", 0);
    const ::Window owner = ::XGetSelectionOwner(m_display, clipboard);
    if (owner == 0) {
        return fail<std::string>(Error{Errc::NotFound, "clipboard: CLIPBOARD has no owner"});
    }
    if (owner == m_root && m_clipboardOwned) {
        // We are the owner: answer from our own copy without a round trip.
        return m_clipboardText;
    }

    // Request the data. The property receives the payload in SelectionNotify.
    const ::Atom property = transferProperty(*this);
    m_pendingPaste = true;
    m_pendingPasteData.clear();
    m_pendingPasteProperty = property;
    m_pendingPasteTarget = m_utf8String;
    (void)::XConvertSelection(m_display, clipboard, m_utf8String, property, m_root, CurrentTime);
    ::XFlush(m_display);

    // Bounded wait: dispatch the connection until SelectionNotify arrives.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (m_pendingPaste && std::chrono::steady_clock::now() < deadline) {
        (void)processPending();
        if (m_pendingPaste) {
            ::XFlush(m_display);
            const int fd = ConnectionNumber(m_display);
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(fd, &readable);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 20000; // 20 ms slices so the deadline stays meaningful
            (void)::select(fd + 1, &readable, nullptr, nullptr, &tv);
        }
    }

    if (m_pendingPaste) {
        m_pendingPaste = false;
        return fail<std::string>(Error{Errc::Timeout, "clipboard: the selection owner did not answer within 1 s"});
    }
    if (m_pendingPasteData.empty()) {
        return fail<std::string>(Error{Errc::NotFound, "clipboard: the selection owner returned no text"});
    }
    return m_pendingPasteData;
}

void X11Platform::handleSelectionNotify(const XSelectionEvent& notify) {
    if (!m_pendingPaste || notify.selection != m_clipboard) {
        return;
    }
    m_pendingPaste = false;
    if (notify.property == 0) {
        // The owner refused the conversion (wrong flavour or vanished).
        m_pendingPasteData.clear();
        return;
    }

    ::Atom actualType = 0;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    const int status = ::XGetWindowProperty(m_display, m_root, notify.property, 0, 0x1FFFFFFF, 1, AnyPropertyType,
                                            &actualType, &actualFormat, &itemCount, &bytesAfter, &data);
    if (status != 0 || data == nullptr) {
        m_pendingPasteData.clear();
        return;
    }
    if (actualType == m_incr) {
        // Chunked transfer: not supported by this backend yet. Report no text
        // instead of returning a truncated payload.
        ::XFree(data);
        m_pendingPasteData.clear();
        return;
    }
    // The property is already deleted (delete == 1), so `data` is ours.
    m_pendingPasteData.assign(reinterpret_cast<const char*>(data), itemCount);
    ::XFree(data);
}

} // namespace nativebridge::detail

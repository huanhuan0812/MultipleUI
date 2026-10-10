# nativeBridge — CMake configuration.
# SPDX-License-Identifier: MIT
#
# Public options
# --------------
#   NATIVEBRIDGE_LINUX_BACKEND = AUTO | X11 | WAYLAND   (default AUTO)
#       AUTO     compile both backends, prefer Wayland at run time, fall back
#                to X11 when Wayland is unavailable
#       X11      compile the X11 backend only
#       WAYLAND  compile the Wayland backend only
#   NATIVEBRIDGE_BUILD_EXAMPLES = ON|OFF   (default ON)
#   NATIVEBRIDGE_BUILD_TESTS    = ON|OFF   (default ON)
#   NATIVEBRIDGE_ENABLE_LOGGING = ON|OFF   (default OFF, always on in Debug)
#
# Environment overrides at run time (Linux, AUTO/X11/WAYLAND builds that
# contain more than one backend):
#   NATIVEBRIDGE_LINUX_BACKEND=x11|wayland

cmake_minimum_required(VERSION 3.20)

project(nativeBridge
    VERSION 0.1.0
    DESCRIPTION "C++20 cross-platform system abstraction library (window, events, displays, system, surface bridge)"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

set(NATIVEBRIDGE_LINUX_BACKEND "AUTO" CACHE STRING
    "Linux backend selection: AUTO, X11 or WAYLAND")
set_property(CACHE NATIVEBRIDGE_LINUX_BACKEND PROPERTY STRINGS AUTO X11 WAYLAND)

option(NATIVEBRIDGE_BUILD_EXAMPLES "Build the nativeBridge examples" ON)
option(NATIVEBRIDGE_BUILD_TESTS    "Build the nativeBridge tests"    ON)
option(NATIVEBRIDGE_ENABLE_LOGGING "Always enable nativeBridge diagnostics" OFF)
option(NATIVEBRIDGE_INSTALL        "Generate install rules"          ON)

string(TOUPPER "${NATIVEBRIDGE_LINUX_BACKEND}" NATIVEBRIDGE_LINUX_BACKEND_UPPER)
if(NOT NATIVEBRIDGE_LINUX_BACKEND_UPPER MATCHES "^(AUTO|X11|WAYLAND)$")
    message(FATAL_ERROR
        "NATIVEBRIDGE_LINUX_BACKEND must be AUTO, X11 or WAYLAND (got '${NATIVEBRIDGE_LINUX_BACKEND}')")
endif()

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------

set(NATIVEBRIDGE_PUBLIC_HEADERS
    include/nativebridge/result.h
    include/nativebridge/geometry.h
    include/nativebridge/keycode.h
    include/nativebridge/event.h
    include/nativebridge/window.h
    include/nativebridge/display.h
    include/nativebridge/system.h
    include/nativebridge/surface.h
    include/nativebridge/nativebridge.h)

set(NATIVEBRIDGE_COMMON_SOURCES
    src/common/backend.cpp
    src/common/platform.cpp
    src/common/window.cpp
    src/common/time.cpp
    src/common/paths.cpp
    src/common/process.cpp)

add_library(nativeBridge STATIC
    ${NATIVEBRIDGE_COMMON_SOURCES}
    ${NATIVEBRIDGE_PUBLIC_HEADERS})

add_library(nativeBridge::nativeBridge ALIAS nativeBridge)

# The internal headers are not installed but must be visible to the backends.
target_include_directories(nativeBridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(nativeBridge PUBLIC cxx_std_20)

if(NOT MSVC)
    target_compile_options(nativeBridge PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NATIVEBRIDGE_ENABLE_LOGGING)
    target_compile_definitions(nativeBridge PRIVATE NATIVEBRIDGE_ENABLE_LOGGING=1)
endif()

# On a single-backend Linux build the runtime selection collapses to that one
# backend; the definition lets src/common/platform.cpp skip the AUTO logic.
if(UNIX AND NOT APPLE)
    if(NATIVEBRIDGE_LINUX_BACKEND_UPPER STREQUAL "X11")
        target_compile_definitions(nativeBridge PRIVATE NATIVEBRIDGE_LINUX_SINGLE_BACKEND=1)
    elseif(NATIVEBRIDGE_LINUX_BACKEND_UPPER STREQUAL "WAYLAND")
        target_compile_definitions(nativeBridge PRIVATE NATIVEBRIDGE_LINUX_SINGLE_BACKEND=1)
    endif()
endif()

# ---------------------------------------------------------------------------
# Platform backends
# ---------------------------------------------------------------------------

set(NATIVEBRIDGE_PLATFORM_NAME "unsupported")
set(NATIVEBRIDGE_ENABLE_OBJC OFF)

if(WIN32)
    set(NATIVEBRIDGE_PLATFORM_NAME "win32")
    target_sources(nativeBridge PRIVATE
        src/win32/win32_common.cpp
        src/win32/win32_platform.cpp
        src/win32/win32_window.cpp)
    target_compile_definitions(nativeBridge PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX
        NATIVEBRIDGE_BACKEND_WIN32=1)
    target_link_libraries(nativeBridge PRIVATE
        user32 gdi32 shell32 shlobj32 ole32 oleaut32 dwmapi uxtheme)

elseif(APPLE)
    set(NATIVEBRIDGE_PLATFORM_NAME "cocoa")
    set(NATIVEBRIDGE_ENABLE_OBJC ON)
    target_sources(nativeBridge PRIVATE
        src/macos/cocoa_platform.mm
        src/macos/cocoa_window.mm)
    target_compile_definitions(nativeBridge PRIVATE NATIVEBRIDGE_BACKEND_COCOA=1)
    target_compile_options(nativeBridge PRIVATE -fobjc-arc)
    target_link_libraries(nativeBridge PRIVATE
        "-framework Cocoa" "-framework Foundation" "-framework AppKit" "-framework CoreGraphics")

elseif(UNIX)
    find_package(PkgConfig REQUIRED)

    set(NATIVEBRIDGE_WANT_X11 OFF)
    set(NATIVEBRIDGE_WANT_WAYLAND OFF)
    if(NATIVEBRIDGE_LINUX_BACKEND_UPPER STREQUAL "X11")
        set(NATIVEBRIDGE_WANT_X11 ON)
    elseif(NATIVEBRIDGE_LINUX_BACKEND_UPPER STREQUAL "WAYLAND")
        set(NATIVEBRIDGE_WANT_WAYLAND ON)
    else()
        set(NATIVEBRIDGE_WANT_X11 ON)
        set(NATIVEBRIDGE_WANT_WAYLAND ON)
    endif()

    if(NATIVEBRIDGE_WANT_X11)
        pkg_check_modules(X11 QUIET x11)
        if(NOT X11_FOUND)
            if(NATIVEBRIDGE_LINUX_BACKEND_UPPER STREQUAL "X11")
                message(FATAL_ERROR "NATIVEBRIDGE_LINUX_BACKEND=X11 but the x11 development package was not found")
            endif()
            message(STATUS "nativeBridge: X11 development package not found, skipping the X11 backend")
            set(NATIVEBRIDGE_WANT_X11 OFF)
        endif()
    endif()

    if(NATIVEBRIDGE_WANT_WAYLAND)
        pkg_check_modules(WAYLAND QUIET wayland-client)
        if(NOT WAYLAND_FOUND)
            if(NATIVEBRIDGE_LINUX_BACKEND_UPPER STREQUAL "WAYLAND")
                message(FATAL_ERROR
                    "NATIVEBRIDGE_LINUX_BACKEND=WAYLAND but wayland-client was not found")
            endif()
            message(STATUS "nativeBridge: wayland-client not found, skipping the Wayland backend")
            set(NATIVEBRIDGE_WANT_WAYLAND OFF)
        endif()
    endif()

    if(NOT NATIVEBRIDGE_WANT_X11 AND NOT NATIVEBRIDGE_WANT_WAYLAND)
        message(FATAL_ERROR
            "nativeBridge: no Linux backend available (install libx11-dev and/or libwayland-dev)")
    endif()

    set(NATIVEBRIDGE_PLATFORM_NAME "linux")

    if(NATIVEBRIDGE_WANT_X11)
        target_sources(nativeBridge PRIVATE
            src/linux/x11/x11_platform.cpp
            src/linux/x11/x11_window.cpp
            src/linux/x11/x11_clipboard.cpp)
        target_include_directories(nativeBridge PRIVATE ${X11_INCLUDE_DIRS})
        target_link_libraries(nativeBridge PRIVATE ${X11_LIBRARIES})
        target_compile_definitions(nativeBridge PRIVATE NATIVEBRIDGE_BACKEND_X11=1)
        message(STATUS "nativeBridge: enabling the X11 backend")
    endif()

    if(NATIVEBRIDGE_WANT_WAYLAND)
        target_sources(nativeBridge PRIVATE src/linux/wayland/wayland_backend.cpp)
        target_include_directories(nativeBridge PRIVATE ${WAYLAND_INCLUDE_DIRS})
        target_link_libraries(nativeBridge PRIVATE ${WAYLAND_LIBRARIES})
        target_compile_definitions(nativeBridge PRIVATE NATIVEBRIDGE_BACKEND_WAYLAND=1)
        message(STATUS "nativeBridge: enabling the Wayland backend")

        # xdg-shell is a protocol description, not a library: the generated
        # client code is required to create windows. wayland-scanner and
        # wayland-protocols provide it. If they are missing, WAYLAND is not a
        # usable configuration and configuring fails with a clear message
        # (in AUTO mode the X11 backend is used instead).
        find_program(NATIVEBRIDGE_WAYLAND_SCANNER wayland-scanner)
        pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
        set(NATIVEBRIDGE_XDG_SHELL_READY OFF)
        if(NATIVEBRIDGE_WAYLAND_SCANNER AND WAYLAND_PROTOCOLS_FOUND)
            set(NATIVEBRIDGE_XDG_SHELL_XML
                "${WAYLAND_PROTOCOLS_PREFIX}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
            if(EXISTS "${NATIVEBRIDGE_XDG_SHELL_XML}")
                set(NATIVEBRIDGE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
                file(MAKE_DIRECTORY "${NATIVEBRIDGE_GENERATED_DIR}")
                add_custom_command(
                    OUTPUT
                        "${NATIVEBRIDGE_GENERATED_DIR}/xdg-shell-client-protocol.h"
                        "${NATIVEBRIDGE_GENERATED_DIR}/xdg-shell-protocol.c"
                    COMMAND ${NATIVEBRIDGE_WAYLAND_SCANNER} client-header
                            "${NATIVEBRIDGE_XDG_SHELL_XML}"
                            "${NATIVEBRIDGE_GENERATED_DIR}/xdg-shell-client-protocol.h"
                    COMMAND ${NATIVEBRIDGE_WAYLAND_SCANNER} private-code
                            "${NATIVEBRIDGE_XDG_SHELL_XML}"
                            "${NATIVEBRIDGE_GENERATED_DIR}/xdg-shell-protocol.c"
                    DEPENDS "${NATIVEBRIDGE_XDG_SHELL_XML}"
                    COMMENT "Generating xdg-shell protocol code")
                target_sources(nativeBridge PRIVATE
                    "${NATIVEBRIDGE_GENERATED_DIR}/xdg-shell-protocol.c")
                target_include_directories(nativeBridge PRIVATE "${NATIVEBRIDGE_GENERATED_DIR}")
                target_compile_definitions(nativeBridge PRIVATE NATIVEBRIDGE_HAVE_XDG_SHELL=1)
                set(NATIVEBRIDGE_XDG_SHELL_READY ON)
                message(STATUS "nativeBridge: xdg-shell protocol code will be generated")
            else()
                message(STATUS "nativeBridge: ${NATIVEBRIDGE_XDG_SHELL_XML} not found")
            endif()
        else()
            message(STATUS "nativeBridge: wayland-scanner or wayland-protocols not found")
        endif()

        if(NOT NATIVEBRIDGE_XDG_SHELL_READY)
            if(NATIVEBRIDGE_LINUX_BACKEND_UPPER STREQUAL "WAYLAND")
                message(FATAL_ERROR
                    "NATIVEBRIDGE_LINUX_BACKEND=WAYLAND needs wayland-scanner and the "
                    "wayland-protocols package (for xdg-shell). Install them, or build "
                    "with -DNATIVEBRIDGE_LINUX_BACKEND=X11 / AUTO.")
            endif()
            message(STATUS "nativeBridge: disabling the Wayland backend (xdg-shell is required)")
            set(NATIVEBRIDGE_WANT_WAYLAND OFF)
            if(NOT NATIVEBRIDGE_WANT_X11)
                message(FATAL_ERROR "nativeBridge: no usable Linux backend left")
            endif()
        endif()
    endif()

    # The Linux desktops need dl/pthread and, for the X11 backend, an Xlib that
    # is thread safe (XInitThreads is called by the backend).
    find_package(Threads REQUIRED)
    target_link_libraries(nativeBridge PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
else()
    message(FATAL_ERROR "nativeBridge: unsupported platform")
endif()

message(STATUS "nativeBridge: platform backend = ${NATIVEBRIDGE_PLATFORM_NAME}")

# ---------------------------------------------------------------------------
# Examples and tests
# ---------------------------------------------------------------------------

if(NATIVEBRIDGE_BUILD_EXAMPLES)
    add_executable(nativebridge_example_create_window examples/create_window.cpp)
    target_link_libraries(nativebridge_example_create_window PRIVATE nativeBridge::nativeBridge)
endif()

if(NATIVEBRIDGE_BUILD_TESTS)
    enable_testing()
    add_executable(nativebridge_smoke_test tests/smoke_test.cpp)
    target_link_libraries(nativebridge_smoke_test PRIVATE nativeBridge::nativeBridge)
    add_test(NAME nativebridge_smoke COMMAND nativebridge_smoke_test)
    set_tests_properties(nativebridge_smoke PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "NATIVEBRIDGE_SMOKE_HEADLESS=0")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

if(NATIVEBRIDGE_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS nativeBridge
        EXPORT nativeBridgeTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/nativebridge
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h")
    install(EXPORT nativeBridgeTargets
        NAMESPACE nativeBridge::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nativeBridge)
endif()

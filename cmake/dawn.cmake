# Dawn (webgpu.h) for the WebGPU backend, imported as the rex::webgpu target.
#
# WebAssembly: Emscripten's emdawnwebgpu port (--use-port=emdawnwebgpu).
# Native: the prebuilt Dawn release of the same revision the port pins, so
# webgpu.h is identical on both sides. It is downloaded once per SDK checkout
# into out/deps and checked against the release's SHA-256. The prebuilt
# releases are Dawn's own GitHub nightlies (unsigned, best effort), for Linux
# x64, Windows x64 and macOS. Elsewhere (Windows or Linux on ARM64), build Dawn
# from https://dawn.googlesource.com/dawn at REXGLUE_DAWN_REVISION and point
# REXGLUE_DAWN_DIR at its install prefix.

# Keep in step with Emscripten's emdawnwebgpu port (tools/ports/emdawnwebgpu.py
# in the pinned emsdk), whose _VERSION names this release.
set(REXGLUE_DAWN_RELEASE "v20260423.175430")
set(REXGLUE_DAWN_REVISION "31e25af254ab572c77054edec4946d2244e184dd")

set(REXGLUE_DAWN_DIR "" CACHE PATH
    "Install prefix of a Dawn build to use instead of downloading the pinned release")
# The prebuilt Linux release leaves out Wayland surfaces (DAWN_USE_WAYLAND is off
# by default), so on Linux windows must be X11 (XWayland on a Wayland desktop,
# --video_driver=x11). Turn on for a Dawn built with DAWN_USE_WAYLAND.
option(REXGLUE_DAWN_WAYLAND "The Dawn in use supports Wayland surfaces" OFF)

add_library(rex_webgpu INTERFACE)
add_library(rex::webgpu ALIAS rex_webgpu)

if(EMSCRIPTEN)
    target_compile_options(rex_webgpu INTERFACE --use-port=emdawnwebgpu)
    target_link_options(rex_webgpu INTERFACE --use-port=emdawnwebgpu)
    return()
endif()

if(NOT REXGLUE_DAWN_DIR)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND
       CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_dawn_platform "ubuntu-latest")
        set(_dawn_sha256 "ac76fac090162dc1ecea5ed0f28a557bb8f49efc47faab01886105ace82b7b64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND WIN32)
        set(_dawn_platform "windows-latest")
        set(_dawn_sha256 "fbfd98f7b2faf47987c80dcc4178cb042af09752b1f6f1541600fca8902258ef")
    elseif(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        set(_dawn_platform "macos-15-intel")
        set(_dawn_sha256 "c1c53665e71413e04c62136f36b3b0fd69691d1d6f5bdca23d453f9885430d28")
    elseif(APPLE)
        set(_dawn_platform "macos-latest")
        set(_dawn_sha256 "8d1db80315ab55ad2c61857d1c7ae4c242a9cc67d0639c709a052fc6af3a7bac")
    else()
        message(FATAL_ERROR
            "No prebuilt Dawn for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}. Build Dawn "
            "${REXGLUE_DAWN_REVISION} from https://dawn.googlesource.com/dawn and set "
            "REXGLUE_DAWN_DIR to its install prefix, or turn off REXGLUE_USE_WEBGPU.")
    endif()

    set(_dawn_name "Dawn-${REXGLUE_DAWN_REVISION}-${_dawn_platform}-Release")
    set(_dawn_deps "${REXGLUE_ROOT}/out/deps")
    set(REXGLUE_DAWN_DIR "${_dawn_deps}/${_dawn_name}")
    if(NOT EXISTS "${REXGLUE_DAWN_DIR}/include/webgpu/webgpu.h")
        set(_dawn_archive "${_dawn_deps}/${_dawn_name}.tar.gz")
        message(STATUS "Downloading Dawn ${REXGLUE_DAWN_RELEASE} (${_dawn_platform})")
        file(DOWNLOAD
            "https://github.com/google/dawn/releases/download/${REXGLUE_DAWN_RELEASE}/${_dawn_name}.tar.gz"
            "${_dawn_archive}"
            EXPECTED_HASH SHA256=${_dawn_sha256}
            STATUS _dawn_status
            SHOW_PROGRESS)
        list(GET _dawn_status 0 _dawn_status_code)
        if(NOT _dawn_status_code EQUAL 0)
            file(REMOVE "${_dawn_archive}")
            message(FATAL_ERROR "Downloading Dawn failed: ${_dawn_status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_dawn_archive}" DESTINATION "${_dawn_deps}")
        file(REMOVE "${_dawn_archive}")
    endif()
endif()

# DawnTargets.cmake links Threads::Threads without finding it.
find_package(Threads REQUIRED)
find_package(Dawn CONFIG REQUIRED PATHS "${REXGLUE_DAWN_DIR}" NO_DEFAULT_PATH)
target_link_libraries(rex_webgpu INTERFACE dawn::webgpu_dawn)
if(REXGLUE_DAWN_WAYLAND)
    target_compile_definitions(rex_webgpu INTERFACE REX_DAWN_HAS_WAYLAND=1)
endif()
message(STATUS "Dawn: ${REXGLUE_DAWN_DIR}")

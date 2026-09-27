# SPDX-License-Identifier: GPL-3.0-only
#
# PrismLauncher BigScreen — gamepad-navigable fullscreen front-end.
# Standalone target: does not affect the normal Qt Widgets build unless
# Launcher_BUILD_BIGSCREEN is turned on.

# SDL2/fmt: system packages via pkg-config on Linux (apt/pacman — what the
# Linux CI job and every Linux dev machine already has), vcpkg CONFIG
# packages on Windows (the "bigscreen" feature in vcpkg.json, enabled by
# build-bigscreen.yml's Windows job with -DVCPKG_MANIFEST_FEATURES=bigscreen).
# This repo's Windows triplets link static (cmake/vcpkg-triplets/x64-windows.cmake),
# so vcpkg's SDL2 only provides SDL2::SDL2-static — hence the genex picking
# whichever of the two targets exists. No SDL2main: main.cpp defines
# SDL_MAIN_HANDLED and provides its own entry point.
if(WIN32)
    find_package(SDL2 CONFIG REQUIRED)
    find_package(fmt CONFIG REQUIRED)
    set(BIGSCREEN_SDL2_TARGET $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>)
    set(BIGSCREEN_FMT_TARGET fmt::fmt)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(FMT REQUIRED IMPORTED_TARGET fmt)
    set(BIGSCREEN_SDL2_TARGET PkgConfig::SDL2)
    set(BIGSCREEN_FMT_TARGET PkgConfig::FMT)
endif()
find_package(OpenGL REQUIRED)

# MSVC-only compile settings shared by every BigScreen target (none of them
# get these from anywhere else — only prismlauncher_bigscreen itself links
# Qt, and the two static libs below don't):
#  - /utf-8: sources contain UTF-8 string literals (em dashes, icon glyph
#    byte escapes next to text); MSVC otherwise reads them as the build
#    machine's ANSI code page.
#  - NOMINMAX/WIN32_LEAN_AND_MEAN: SDL_syswm/Qt/launcher headers can pull in
#    <windows.h>, whose min/max macros break every std::min/std::max call.
function(bigscreen_apply_platform_settings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)

add_library(bigscreen_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(bigscreen_imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(bigscreen_imgui PUBLIC ${BIGSCREEN_SDL2_TARGET} OpenGL::GL)
bigscreen_apply_platform_settings(bigscreen_imgui)
# GLES3, not desktop GL — see the matching comment in main.cpp (SDL_GL_CONTEXT_PROFILE_MASK)
# for why: real ARM hardware only exposes GLES through EGL, no desktop GL/GLX at all, and
# imgui_impl_opengl3.cpp picks its GL function loader/GLSL dialect via this exact macro at
# compile time — there's no runtime desktop/GLES switch available. PUBLIC: main.cpp's own
# glsl_version string ("#version 300 es") and SDL context attributes must agree with this.
# Not on Windows: that build runs desktop GL 3.3 core (see main.cpp), where the backend's own
# bundled GL3 loader (its default with no ES macro defined) is exactly what's needed.
if(NOT WIN32)
    target_compile_definitions(bigscreen_imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
endif()

target_sources(bigscreen_imgui PRIVATE
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(bigscreen_imgui PUBLIC ${IMGUI_DIR}/misc/cpp)

# --- ImGuiFullscreen toolkit, vendored from PCSX2 (GPL-3.0+) with a compat
# layer for the small "PCSX2 common" utilities it depends on. See
# thirdparty/pcsx2_common/THIRDPARTY.md and the BigScreen plan (M1).
set(PCSX2_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pcsx2_common)

add_library(bigscreen_fullscreen_toolkit STATIC
    ${PCSX2_COMMON_DIR}/ImGui/ImGuiFullscreen.cpp
    ${PCSX2_COMMON_DIR}/HostCompat.cpp
    ${PCSX2_COMMON_DIR}/EmuFoldersCompat.cpp
    ${PCSX2_COMMON_DIR}/FileSystemCompat.cpp
    ${PCSX2_COMMON_DIR}/StbImageImpl.cpp
    ${PCSX2_COMMON_DIR}/FullscreenUICompat.cpp
    ${PCSX2_COMMON_DIR}/GS/Renderers/Common/GLCompat.cpp
)
bigscreen_apply_platform_settings(bigscreen_fullscreen_toolkit)
target_include_directories(bigscreen_fullscreen_toolkit PUBLIC
    ${PCSX2_COMMON_DIR}
)
target_include_directories(bigscreen_fullscreen_toolkit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/stb
)
target_link_libraries(bigscreen_fullscreen_toolkit PUBLIC bigscreen_imgui ${BIGSCREEN_FMT_TARGET} OpenGL::GL)
# PCSX2 defines this globally for their build; ImGuiFullscreen.cpp relies on
# ImVec2 +/-/* operators it provides but Dear ImGui only compiles in when
# asked (opt-in, to avoid clashing with a project's own vector math types).
# PUBLIC: BigScreen's own screens (main.cpp) use these operators too, e.g.
# drawing the top status bar directly with the foreground draw list.
target_compile_definitions(bigscreen_fullscreen_toolkit PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_compile_definitions(bigscreen_fullscreen_toolkit PRIVATE
    BIGSCREEN_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

# --- The BigScreen app itself, reusing PrismLauncher's core (instances,
# accounts, launching, settings) via Launcher_logic — no network/RPC hop,
# just linking the same static library the Qt Widgets UI links, and never
# instantiating a QWidget. See the BigScreen plan (M2).
# WIN32: GUI subsystem on Windows (ignored elsewhere), same as the desktop
# launcher's own executable — no console window popping up alongside the
# fullscreen one; main.cpp attaches to a parent terminal's console only when
# there is one. WinMain → main() comes from Qt through Launcher_logic, again
# exactly as for the desktop launcher.
add_executable(prismlauncher_bigscreen WIN32
    main.cpp
    gui/GuiManager.cpp
    core/DialogHelpers.cpp
    core/BigScreenLaunchController.cpp
)
set_target_properties(prismlauncher_bigscreen PROPERTIES AUTOMOC ON)
target_link_libraries(prismlauncher_bigscreen PRIVATE bigscreen_imgui bigscreen_fullscreen_toolkit Launcher_logic)
bigscreen_apply_platform_settings(prismlauncher_bigscreen)
if(WIN32)
    # UTF-8 as the process ANSI code page — see common/FileSystem.h's note
    # for why BigScreen needs this and the desktop launcher (all QString/
    # QFile) doesn't. Merged by the MSVC toolchain into the manifest the
    # linker generates anyway.
    target_sources(prismlauncher_bigscreen PRIVATE packaging/prismlauncher_bigscreen.manifest)
endif()

# --- Optional Steamworks integration (ISteamUtils::ShowFloatingGamepadTextInput
# specifically) — lets Steam show its own on-screen keyboard for our
# ImGui::InputText fields when running under Steam Input / Big Picture,
# where there's no physical keyboard. Vendored under thirdparty/steamworks/
# (public headers + the redistributable_bin shared libraries only — not the
# rest of the SDK) rather than a system package: there is no distro package
# for this, Valve only ships it via partner.steamgames.com. Entirely
# optional: if the vendored SDK isn't present (a fresh checkout without it
# committed, or before it's added at all), this whole block is skipped and
# prismlauncher_bigscreen builds exactly as before — see the
# BIGSCREEN_HAVE_STEAMWORKS guards in main.cpp.
set(STEAMWORKS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/steamworks)
if(WIN32)
    # Valve only ships an x64 Windows build (no win-arm64 in the SDK), so an
    # ARM64 Windows build simply points at a lib that doesn't exist and
    # falls through to the "not found" branch below.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(STEAMWORKS_LIB_ARCH win-arm64-unsupported)
    else()
        set(STEAMWORKS_LIB_ARCH win64)
    endif()
    set(STEAMWORKS_LIB ${STEAMWORKS_DIR}/lib/${STEAMWORKS_LIB_ARCH}/steam_api64.lib)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(STEAMWORKS_LIB_ARCH linuxarm64)
    set(STEAMWORKS_LIB ${STEAMWORKS_DIR}/lib/${STEAMWORKS_LIB_ARCH}/libsteam_api.so)
else()
    set(STEAMWORKS_LIB_ARCH linux64)
    set(STEAMWORKS_LIB ${STEAMWORKS_DIR}/lib/${STEAMWORKS_LIB_ARCH}/libsteam_api.so)
endif()
if(EXISTS ${STEAMWORKS_LIB} AND EXISTS ${STEAMWORKS_DIR}/include/steam/steam_api.h)
    target_include_directories(prismlauncher_bigscreen PRIVATE ${STEAMWORKS_DIR}/include)
    target_link_libraries(prismlauncher_bigscreen PRIVATE ${STEAMWORKS_LIB})
    target_compile_definitions(prismlauncher_bigscreen PRIVATE BIGSCREEN_HAVE_STEAMWORKS)
    if(WIN32)
        # No rpath on Windows: the DLL just has to sit next to the exe.
        # Copied at build time so a plain local build runs as-is, and so CI
        # packaging picks it up from the same output directory.
        add_custom_command(TARGET prismlauncher_bigscreen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${STEAMWORKS_DIR}/lib/${STEAMWORKS_LIB_ARCH}/steam_api64.dll
                $<TARGET_FILE_DIR:prismlauncher_bigscreen>
        )
    endif()
    # Both entries go into BUILD_RPATH, not just the absolute one — INSTALL_RPATH
    # is a dead property here: BigScreen has no cmake --install step (CI
    # packages straight out of the build tree — see build-bigscreen.yml's
    # own "No cmake --install step first" comment), so it's never actually
    # applied to the shipped binary. Without $ORIGIN also in BUILD_RPATH,
    # the binary CI packages would carry only the CI runner's own absolute
    # checkout path baked in (real on the runner at build/package time, so
    # sharun's own ldd-based bundling step would still succeed there — but
    # meaningless, and never valid, on the end user's actual machine once
    # downloaded) — a real, confirmed bug (`readelf -d` on a local build
    # showed exactly this: RUNPATH pointing at .../PrismLauncher-bigscreen/...,
    # nothing resolvable once shipped), found while investigating a report
    # that Steam's on-screen keyboard didn't appear on any of three real
    # devices. $ORIGIN covers the packaged case (CI copies libsteam_api.so
    # next to the binary in the same directory before running sharun, so
    # $ORIGIN resolves there); the absolute path covers a plain local
    # `cmake --build` with no packaging step at all — same "absolute
    # compile-time path for local dev, relative lookup once actually
    # packaged" duality BIGSCREEN_RESOURCES_DIR already uses for resources/.
    set_target_properties(prismlauncher_bigscreen PROPERTIES
        BUILD_RPATH "$ORIGIN;${STEAMWORKS_DIR}/lib/${STEAMWORKS_LIB_ARCH}"
    )
else()
    message(STATUS "BigScreen: Steamworks SDK not found under ${STEAMWORKS_DIR} — building without Steam on-screen-keyboard integration.")
endif()

# JavaCheck.jar and NewLaunch(Legacy).jar aren't link dependencies (they're
# found at runtime via Application::getJarPath() — JavaChecker runs
# JavaCheck.jar as an external `java -jar` subprocess to verify a JRE,
# LauncherPartLaunch does the same with NewLaunch(Legacy).jar to bootstrap
# Minecraft itself), so CMake doesn't know prismlauncher_bigscreen needs
# them — without this, `cmake --build --target prismlauncher_bigscreen`
# never builds them: Java install verification silently fails (no
# JavaCheck.jar), and even a verified Java can't actually launch anything
# (no NewLaunch.jar — "Launcher library could not be found").
add_dependencies(prismlauncher_bigscreen JavaCheck NewLaunch NewLaunchLegacy)

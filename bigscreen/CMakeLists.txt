# SPDX-License-Identifier: GPL-3.0-only
#
# PrismLauncher BigScreen — gamepad-navigable fullscreen front-end.
# Standalone target: does not affect the normal Qt Widgets build unless
# Launcher_BUILD_BIGSCREEN is turned on.

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(OpenGL REQUIRED)

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
target_link_libraries(bigscreen_imgui PUBLIC PkgConfig::SDL2 OpenGL::GL)

target_sources(bigscreen_imgui PRIVATE
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(bigscreen_imgui PUBLIC ${IMGUI_DIR}/misc/cpp)

# --- ImGuiFullscreen toolkit, vendored from PCSX2 (GPL-3.0+) with a compat
# layer for the small "PCSX2 common" utilities it depends on. See
# thirdparty/pcsx2_common/THIRDPARTY.md and the BigScreen plan (M1).
pkg_check_modules(FMT REQUIRED IMPORTED_TARGET fmt)

set(PCSX2_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pcsx2_common)

add_library(bigscreen_fullscreen_toolkit STATIC
    ${PCSX2_COMMON_DIR}/ImGui/ImGuiFullscreen.cpp
    ${PCSX2_COMMON_DIR}/HostCompat.cpp
    ${PCSX2_COMMON_DIR}/EmuFoldersCompat.cpp
    ${PCSX2_COMMON_DIR}/StbImageImpl.cpp
    ${PCSX2_COMMON_DIR}/FullscreenUICompat.cpp
)
target_include_directories(bigscreen_fullscreen_toolkit PUBLIC
    ${PCSX2_COMMON_DIR}
)
target_include_directories(bigscreen_fullscreen_toolkit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/stb
)
target_link_libraries(bigscreen_fullscreen_toolkit PUBLIC bigscreen_imgui PkgConfig::FMT OpenGL::GL)
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
add_executable(prismlauncher_bigscreen
    main.cpp
    gui/GuiManager.cpp
    core/DialogHelpers.cpp
    core/BigScreenLaunchController.cpp
)
set_target_properties(prismlauncher_bigscreen PROPERTIES AUTOMOC ON)
target_link_libraries(prismlauncher_bigscreen PRIVATE bigscreen_imgui bigscreen_fullscreen_toolkit Launcher_logic)

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

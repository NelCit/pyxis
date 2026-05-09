# pyxis_app — single executable, viewer + headless. Plan §1 / §3 / §41.

add_executable(pyxis WIN32)
add_executable(Pyxis::App ALIAS pyxis)

# pyxis_app is the consumer of the renderer + platform DLLs; it links them
# PUBLIC so transitive consumers (the unit test executable) see the right
# include paths.
pyxis_set_target_compile_options(pyxis)
# The Application owns SceneWorldFacade::Init() which catches Flecs's
# bad_alloc internally; the renderer/platform perimeter is exception-free.
# pyxis_app proper has no need for exceptions either, so keep them off.
pyxis_disable_exceptions_and_rtti(pyxis)

target_sources(pyxis PRIVATE
    Main.cpp

    Private/Application.h
    Private/Application.cpp
    Private/CliArgs.h
    Private/CliArgs.cpp
    Private/Config/Configuration.h
    Private/Config/Configuration.cpp
    Private/Output/ExrWriter.h
    Private/Output/ExrWriter.cpp
    Private/Output/TextureReadback.h
    Private/Output/TextureReadback.cpp
    Private/Render/AovTextures.h
    Private/Render/AovTextures.cpp
    Private/Render/HardcodedCubeScene.h
    Private/Render/HardcodedCubeScene.cpp
    Private/Scene/SceneResolver.h
    Private/Scene/SceneResolver.cpp
    Private/UsdDirectEngine/UsdDirectEngine.h
    Private/UsdDirectEngine/UsdDirectEngine.cpp
    Private/HydraEngine/HydraEngine.h
    Private/HydraEngine/HydraEngine.cpp
    Private/HeadlessMode.h
    Private/HeadlessMode.cpp
    Private/ViewerMode.h
    Private/ViewerMode.cpp
    Private/ImGuiHost.h
    Private/ImGuiHost.cpp
    Private/StbImageWrite.cpp
)

# stb_image_write's IMPLEMENTATION TU has missing-field-initializer
# patterns inside the third-party header. Disable the warning + clang-
# tidy on that single file rather than relax it project-wide.
set_source_files_properties(Private/StbImageWrite.cpp PROPERTIES
    COMPILE_OPTIONS    "-Wno-missing-field-initializers"
    SKIP_LINTING       ON)

target_include_directories(pyxis PRIVATE
    Private
)

find_package(Stb REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)

# Plan §29.1 overlay order: pyxis loads <exe-dir>/Resources/parameters.
# default.json as the on-disk default. Copy the source-of-truth to the
# binary's Resources/ alongside the shaders Slang.cmake puts there.
set(_pyxis_app_resources_src "${CMAKE_SOURCE_DIR}/resources")
set(_pyxis_app_resources_dst "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/Resources")
add_custom_command(TARGET pyxis POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_pyxis_app_resources_src}/parameters.default.json"
        "${_pyxis_app_resources_dst}/parameters.default.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_pyxis_app_resources_src}/parameters.schema.json"
        "${_pyxis_app_resources_dst}/parameters.schema.json"
    # §29.4.a default-startup-scene + its sibling lat-long EXR. Both
    # files ship version-controlled in resources/scenes/ and copy
    # next to pyxis.exe so AssetLocator::LocateResource("scenes/...")
    # finds them at runtime.
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${_pyxis_app_resources_dst}/scenes"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_pyxis_app_resources_src}/scenes/default.usd"
        "${_pyxis_app_resources_dst}/scenes/default.usd"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_pyxis_app_resources_src}/scenes/default_sky.exr"
        "${_pyxis_app_resources_dst}/scenes/default_sky.exr"
    BYPRODUCTS
        "${_pyxis_app_resources_dst}/parameters.default.json"
        "${_pyxis_app_resources_dst}/parameters.schema.json"
        "${_pyxis_app_resources_dst}/scenes/default.usd"
        "${_pyxis_app_resources_dst}/scenes/default_sky.exr"
    COMMENT "Copying Resources/ (parameters.{default,schema}.json, scenes/default.usd + default_sky.exr) to <bin>/Resources/")
unset(_pyxis_app_resources_src)
unset(_pyxis_app_resources_dst)

# ----------------------------------------------------------------------------
# USD plugin tree (M4). USD discovers its prim-type plugins (UsdGeom,
# UsdLux, UsdShade, etc.) by walking `<dir-of-usd-DLLs>/usd/<plugin>/
# resources/plugInfo.json`. vcpkg copies the usd_*.dll files next to
# pyxis.exe but NOT the matching `usd/` plugin tree, so without this
# copy step UsdStage::Open(...) hangs trying to resolve schema TfTypes.
#
# Debug vs Release: vcpkg ships separate trees under
# vcpkg_installed/x64-windows/{,debug/}bin/usd/. Both trees are
# structurally identical; only their referenced DLLs differ in linkage
# (debug-vs-release CRT). copy_directory mirrors the right one based
# on $<CONFIG>.
# ----------------------------------------------------------------------------
set(_pyxis_usd_plugin_src
    "$<IF:$<CONFIG:Debug>,${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/debug/bin/usd,${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin/usd>")
set(_pyxis_usd_plugin_dst
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/usd")

# USD's PlugRegistry walks the path in PXR_PLUGINPATH_NAME for a
# top-level `plugInfo.json` aggregator that lists per-plugin
# subdirs via `Includes: ["*/resources/"]`. vcpkg ships that
# aggregator at `lib/usd/plugInfo.json` (Linux convention) but the
# actual plugin tree lives at `bin/usd/<plugin>/resources/` on
# Windows; the aggregator's wildcard there resolves to nothing.
# We write our own one-line aggregator next to the copied tree so
# USD's wildcard finds the real per-plugin plugInfo.jsons.
set(_pyxis_usd_aggregator "${CMAKE_CURRENT_BINARY_DIR}/usd_plugInfo.json")
file(WRITE "${_pyxis_usd_aggregator}" "{\n    \"Includes\": [ \"*/resources/\" ]\n}\n")

add_custom_command(TARGET pyxis POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${_pyxis_usd_plugin_src}"
        "${_pyxis_usd_plugin_dst}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_pyxis_usd_aggregator}"
        "${_pyxis_usd_plugin_dst}/plugInfo.json"
    COMMENT "Copying vcpkg USD plugin tree (44 plugins, ~7 MB) + aggregator plugInfo.json to <bin>/usd/")
unset(_pyxis_usd_plugin_src)
unset(_pyxis_usd_plugin_dst)
unset(_pyxis_usd_aggregator)

# ImGui (with Vulkan + GLFW backends) — built lazily by the helper in
# _cmake/Thirdparty.cmake. pyxis_app/ViewerMode is the only consumer
# in M1; the FPS panel + dockspace land here. The helper assumes
# find_package(Vulkan) + find_package(glfw3) have already populated
# the corresponding imported targets (the GLFW include path is what
# imgui_impl_glfw.cpp needs at compile time, not just link time).
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
pyxis_thirdparty_require_imgui()

target_link_libraries(pyxis PRIVATE
    Pyxis::Platform
    Pyxis::Renderer
    Pyxis::UsdIngest                 # M4 — UsdDirectEngine + HydraEngine (M4 stub) wrap StageWalker.
    Pyxis::Hydra                     # M4 — pyxis_hydra plugInfo.json + Sync impls register at process load.
    pyxis_imgui
    Vulkan::Vulkan        # vkCmdBeginRendering / vkCmdEndRendering / descriptor-pool calls
    nlohmann_json::nlohmann_json     # parameters.json + effective-config writer (§26 / §27)
    unofficial::tinyexr::tinyexr     # headless EXR writer (§41 M2)
)

# stb_image_write is header-only; vcpkg's `Stb` config exports the
# include dir as Stb_INCLUDE_DIR but no link target. ViewerMode.cpp's
# --screenshot path #defines STB_IMAGE_WRITE_IMPLEMENTATION in exactly
# one TU and writes PNGs.
target_include_directories(pyxis PRIVATE ${Stb_INCLUDE_DIR})

# WIN32 + the standard /SUBSYSTEM:CONSOLE for our CLI tool. The WIN32 flag
# above defaults to /SUBSYSTEM:WINDOWS, which we override here so headless
# stdout/stderr print to the host console.
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set_target_properties(pyxis PROPERTIES
        WIN32_EXECUTABLE FALSE)
    target_link_options(pyxis PRIVATE /SUBSYSTEM:CONSOLE)
endif()

# install(TARGETS pyxis ... ) — wired in M11 alongside the install rules
# from §49.  M0 leaves it out so we don't accidentally `install` an
# incomplete tree.

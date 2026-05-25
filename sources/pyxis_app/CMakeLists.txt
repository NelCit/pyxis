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
    Private/Output/AovExrSaver.h
    Private/Output/AovExrSaver.cpp
    Private/Output/SaveFilePicker.h
    Private/Output/SaveFilePicker.cpp
    Private/Output/BackbufferScreenshot.h
    Private/Output/BackbufferScreenshot.cpp
    Private/Output/ExrWriter.h
    Private/Output/ExrWriter.cpp
    Private/Output/PngWriter.h
    Private/Output/PngWriter.cpp
    Private/Output/TextureReadback.h
    Private/Output/TextureReadback.cpp
    Private/Camera/FlyCameraController.h
    Private/Camera/FlyCameraController.cpp
    Private/Render/AovTextures.h
    Private/Render/AovTextures.cpp
    Private/Render/HardcodedCubeScene.h
    Private/Render/HardcodedCubeScene.cpp
    Private/Scene/SceneResolver.h
    Private/Scene/SceneResolver.cpp
    Private/EditorPanel.cpp
    Private/IngestUsd.h
    Private/IngestUsd.cpp
    Private/VariantParser.h
    Private/VariantParser.cpp
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

# RFC 0006: IngestUsd.cpp calls UsdStage::Open / authors variant selections, so
# pyxis_app sees USD headers + links the nv-usd 25.11 import libs directly. We use
# the includes+libs-only entry point (NOT pyxis_use_nv_usd): pyxis_app stays on the
# exception-free perimeter (/EHs-c- + _HAS_EXCEPTIONS=0 from
# pyxis_disable_exceptions_and_rtti above). USD's headers are catch-free, so they
# compile without exceptions enabled — exactly as they did against vcpkg USD on
# main, where the include path arrived globally via the vcpkg toolchain.
include(PyxisNvUsd)
pyxis_nv_usd_includes_and_libs(pyxis)

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
# nv-usd runtime staging (RFC 0006). USD discovers its prim-type plugins
# (UsdGeom, UsdLux, UsdShade, …) by walking `<dir-of-usd-DLLs>/usd/<plugin>/
# resources/plugInfo.json`. We mirror nv-usd's standard relative layout next to
# pyxis.exe: the usd_*.dll go beside the exe, and the plugin tree goes to
# `<bin>/usd/`, so USD's PlugRegistry finds it automatically (no env var needed).
#
# nv-usd ships ONE release build (no debug tree, unlike vcpkg), so there is no
# $<CONFIG> branching, and it ships its own `lib/usd/plugInfo.json` aggregator
# with the correct `Includes: ["*/resources/"]` wildcard — we copy it verbatim
# rather than writing our own.
#
# DLLs staged next to pyxis.exe:
#   - PXR_USD_ROOT/lib/*.dll   — usd_*.dll core + Alembic/draco/OpenSubdiv
#   - PXR_USD_ROOT/bin/*.dll   — Imath, MaterialX*, tbb*, zlib support libs
#   - PYTHON_ROOT/python312.dll + python3.dll — nv-usd 25.11 is a py312 build
# ----------------------------------------------------------------------------
set(_pyxis_usd_plugin_dst "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/usd")
set(_pyxis_exe_dir        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")

add_custom_command(TARGET pyxis POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${PXR_USD_ROOT}/lib/usd"
        "${_pyxis_usd_plugin_dst}"
    COMMENT "Copying nv-usd plugin tree (lib/usd/, incl. aggregator plugInfo.json) to <bin>/usd/")

# Stage the nv-usd + python runtime DLLs next to pyxis.exe. Globbed at configure
# time (CONFIGURE_DEPENDS re-globs on rebuild if the set changes); each file is a
# copy_if_different so incremental builds skip unchanged DLLs.
file(GLOB _pyxis_nv_usd_dlls CONFIGURE_DEPENDS
    "${PXR_USD_ROOT}/lib/*.dll" "${PXR_USD_ROOT}/bin/*.dll")
foreach(_dll IN LISTS _pyxis_nv_usd_dlls)
    add_custom_command(TARGET pyxis POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "${_pyxis_exe_dir}")
endforeach()
foreach(_pydll python312.dll python3.dll)
    add_custom_command(TARGET pyxis POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PYXIS_NV_USD_PYTHON_ROOT}/${_pydll}" "${_pyxis_exe_dir}")
endforeach()

unset(_pyxis_usd_plugin_dst)
unset(_pyxis_exe_dir)
unset(_pyxis_nv_usd_dlls)

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
    Pyxis::UsdIngest                 # M4 — IngestUsd() unified entry point shares StageWalker across both adapters.
    Pyxis::Hydra                     # M4 — pyxis_hydra plugInfo.json + Sync impls register at process load.
    pyxis_imgui
    Vulkan::Vulkan        # vkCmdBeginRendering / vkCmdEndRendering / descriptor-pool calls
    nlohmann_json::nlohmann_json     # parameters.json + effective-config writer (§26 / §27)
    unofficial::tinyexr::tinyexr     # headless EXR writer (§41 M2)
)

# Windows-only system libs. psapi (Process Status API) — used by
# ImGuiHost's perf-panel RAM row (GetProcessMemoryInfo). Available on
# every Windows SDK; no third-party dep needed.
if(WIN32)
    target_link_libraries(pyxis PRIVATE psapi)
endif()

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

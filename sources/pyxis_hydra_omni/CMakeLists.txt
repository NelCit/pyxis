# pyxis_hydra_omni — RFC 0004 Stage 3. The Omniverse Kit Hydra render delegate.
#
# Built OUT-OF-TREE from the main Pyxis vcpkg build: this CMakeLists is a
# self-contained project configured against Kit's nv-usd 25.11 (pulled via
# Packman to PXR_USD_ROOT), NOT vcpkg USD 26.3. It links Pyxis's USD-free
# renderer public surface (prebuilt) — never USD into the renderer (§1).
#
# Configure (from a clean checkout, after _tools/omniverse/fetch_nv_usd.ps1):
#   cmake -S sources/pyxis_hydra_omni -B build/omni -G Ninja ^
#         -DCMAKE_BUILD_TYPE=Release ^
#         -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
#         -DPXR_USD_ROOT=D:/pyxis_external/usd-deps/usd
#   cmake --build build/omni
#
# nv-usd is a Release/MD build, so this project is Release/MD to match the CRT.

cmake_minimum_required(VERSION 3.24)
project(pyxis_hydra_omni LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)        # Pyxis public headers use std::expected (C++23). nv-usd 25.11 builds at 23.
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")  # /MD to match nv-usd release.

# nv-usd 25.11 (Kit 110.1.1, Packman). We deliberately do NOT use pxrConfig.cmake:
# it find_dependency()s TBB/OpenSubdiv/MaterialX/Imath/Python3 Config files that
# the standalone package doesn't ship. The usd_*.lib are DLL import libs (their
# Python/TBB deps are already resolved inside the USD DLLs), and every header we
# need is bundled under include/, so direct wiring is fully self-contained.
set(PXR_USD_ROOT "D:/pyxis_external/usd-deps/usd" CACHE PATH "Kit nv-usd 25.11 root")
if(NOT EXISTS "${PXR_USD_ROOT}/include/pxr/pxr.h")
    message(FATAL_ERROR
        "pyxis_hydra_omni: nv-usd not found at PXR_USD_ROOT='${PXR_USD_ROOT}'.\n"
        "Run _tools/omniverse/fetch_nv_usd.ps1 first (it packman-pulls nv-usd 25.11),\n"
        "or pass -DPXR_USD_ROOT=<path-to-usd-with-include/pxr/pxr.h>.")
endif()

# ----------------------------------------------------------------------------
# Pyxis's prebuilt, USD-free renderer (RFC 0004 C4). pyxis_renderer /
# pyxis_platform are built by the main vcpkg build (build/dev) — we consume
# their public headers + Release import libs here. They never see USD (§1); only
# this shim does. Release/MD to match both nv-usd and the vcpkg Release CRT.
# ----------------------------------------------------------------------------
set(PYXIS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Pyxis repo root")
set(PYXIS_BUILD_DIR "${PYXIS_REPO_ROOT}/build/dev"
    CACHE PATH "Pyxis main build dir (prebuilt renderer libs + fetched nvrhi)")
set(_pyxis_renderer_lib "${PYXIS_BUILD_DIR}/lib/Release/pyxis_renderer.lib")
set(_pyxis_platform_lib "${PYXIS_BUILD_DIR}/lib/Release/pyxis_platform.lib")
if(NOT EXISTS "${_pyxis_renderer_lib}")
    message(FATAL_ERROR
        "pyxis_hydra_omni: prebuilt pyxis_renderer not found at ${_pyxis_renderer_lib}.\n"
        "Build it first: cmake --build build/dev --config Release "
        "--target pyxis_renderer pyxis_platform")
endif()

# nv-usd is a py312 build: its headers pull <pyconfig.h> via tf/pySafePython.h,
# and inlined Tf python glue references Python symbols. Use the SAME Python
# 3.12 Kit ships (Packman) — a different minor would ABI-mismatch the USD libs.
set(PYXIS_OMNI_PYTHON_ROOT "D:/packman-repo/chk/python/3.12.13+nv1-windows-x86_64"
    CACHE PATH "Kit Python 3.12 root (provides pyconfig.h + python312.lib)")
if(NOT EXISTS "${PYXIS_OMNI_PYTHON_ROOT}/include/pyconfig.h")
    message(FATAL_ERROR
        "pyxis_hydra_omni: Python 3.12 not found at '${PYXIS_OMNI_PYTHON_ROOT}'.\n"
        "nv-usd 25.11 is a py312 build; pass -DPYXIS_OMNI_PYTHON_ROOT=<python3.12-root>.")
endif()

# usd_usdGeom is needed by the smoke harness (authors a UsdGeomMesh); harmless in
# the DLL. tbb is linked explicitly (inlined Tf/Vt/Work template code).
# usd_boost + usd_python: the Usd*/UsdGeom headers (used by the smoke harness)
# instantiate pxr_boost::python converter statics for Sdf/Tf types.
set(_usd_libs usd_hd usd_hf usd_hdsi usd_sdf usd_tf usd_vt usd_gf usd_plug
              usd_usd usd_usdGeom usd_usdImaging usd_usdShade usd_usdLux
              usd_sdr usd_boost usd_python tbb)

# Shared configuration applied to both the delegate DLL and the smoke harness exe.
function(pyxis_omni_configure tgt)
    target_include_directories(${tgt} SYSTEM PRIVATE
        "${PXR_USD_ROOT}/include"
        "${PYXIS_OMNI_PYTHON_ROOT}/include"
        "${PYXIS_REPO_ROOT}/sources/pyxis_renderer/Public"
        "${PYXIS_REPO_ROOT}/sources/pyxis_platform/Public"
        "${PYXIS_BUILD_DIR}/_deps/nvrhi-src/include"
        # ONLY the hlslpp subdir from vcpkg — NOT the bare include/, which also
        # has vcpkg USD 26.3 that would shadow nv-usd 25.11 (pxrInternal_v0_26_3
        # vtable vs the 25.11 import libs). nv-usd must be the sole pxr/ provider.
        "${PYXIS_BUILD_DIR}/vcpkg_installed/x64-windows/include/hlslpp")
    target_include_directories(${tgt} PRIVATE Private)
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(${tgt} PRIVATE /EHsc /GR)
        target_compile_definitions(${tgt} PRIVATE
            _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1 NOMINMAX)
    endif()
    # USD lib dir on the search path so auto-referenced libs (tbb12.lib via
    # pragma) resolve.
    target_link_directories(${tgt} PRIVATE
        "${PXR_USD_ROOT}/lib" "${PYXIS_OMNI_PYTHON_ROOT}/libs")
    foreach(_lib IN LISTS _usd_libs)
        target_link_libraries(${tgt} PRIVATE "${PXR_USD_ROOT}/lib/${_lib}.lib")
    endforeach()
    target_link_libraries(${tgt} PRIVATE
        "${PYXIS_OMNI_PYTHON_ROOT}/libs/python312.lib"
        "${_pyxis_renderer_lib}" "${_pyxis_platform_lib}")
endfunction()

add_library(pyxis_hydra_omni SHARED
    Private/HdPyxisOmniRenderDelegate.cpp
    Private/HdPyxisOmniRendererPlugin.cpp
    Private/PyxisEngine.cpp
)
pyxis_omni_configure(pyxis_hydra_omni)

# Headless HdEngine smoke (RFC 0004 C4-full): drives a USD stage through the
# delegate and verifies geometry reaches GpuScene + a frame renders. Compiles
# the delegate + engine sources directly so it can instantiate the delegate.
add_executable(pyxis_hydra_omni_smoke
    tests/HdEngineSmoke.cpp
    Private/HdPyxisOmniRenderDelegate.cpp
    Private/PyxisEngine.cpp
)
pyxis_omni_configure(pyxis_hydra_omni_smoke)

# ----------------------------------------------------------------------------
# plugInfo.json — written next to the DLL so a Hydra host (Kit / usdview) finds
# the delegate. LibraryPath is relative to <out>/resources/, so "../" + dll.
# ----------------------------------------------------------------------------
set(PYXIS_OMNI_PLUGIN_LIBPATH "../pyxis_hydra_omni.dll")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json"
    @ONLY)
add_custom_command(TARGET pyxis_hydra_omni POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:pyxis_hydra_omni>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json"
        "$<TARGET_FILE_DIR:pyxis_hydra_omni>/resources/plugInfo.json"
    COMMENT "Staging pyxis_hydra_omni plugInfo.json")

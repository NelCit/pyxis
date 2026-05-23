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

set(CMAKE_CXX_STANDARD 17)        # nv-usd 25.11 headers compile cleanly at C++17.
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

add_library(pyxis_hydra_omni SHARED
    Private/HdPyxisOmniRenderDelegate.cpp
    Private/HdPyxisOmniRendererPlugin.cpp
)
target_include_directories(pyxis_hydra_omni SYSTEM PRIVATE "${PXR_USD_ROOT}/include")
target_include_directories(pyxis_hydra_omni PRIVATE Private)

# nv-usd is a py312 build: its headers pull <pyconfig.h> via tf/pySafePython.h,
# and inlined Tf python glue references Python symbols. Use the SAME Python
# 3.12 Kit ships (Packman) — a different minor would ABI-mismatch the USD libs.
set(PYXIS_OMNI_PYTHON_ROOT "D:/packman-repo/chk/python/3.12.13+nv1-windows-x86_64"
    CACHE PATH "Kit Python 3.12 root (provides pyconfig.h + python312.lib)")
if(NOT EXISTS "${PYXIS_OMNI_PYTHON_ROOT}/include/pyconfig.h")
    message(FATAL_ERROR
        "pyxis_hydra_omni: Python 3.12 not found at '${PYXIS_OMNI_PYTHON_ROOT}'.\n"
        "nv-usd 25.11 is a py312 build; pass -DPYXIS_OMNI_PYTHON_ROOT=<python3.12-root>\n"
        "(fetch_nv_usd.ps1 records the path Kit's Packman pulled it to).")
endif()
target_include_directories(pyxis_hydra_omni SYSTEM PRIVATE "${PYXIS_OMNI_PYTHON_ROOT}/include")

# USD throws + uses RTTI; the §30.1 exception-free perimeter is renderer/platform
# only. This is the USD-facing shim, like pyxis_hydra.
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(pyxis_hydra_omni PRIVATE /EHsc /GR)
    # USD's tf/type uses std::is_pod (removed-in-C++20); silence in this DLL.
    target_compile_definitions(pyxis_hydra_omni PRIVATE
        _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1 NOMINMAX)
endif()

# USD ships its libraries with a usd_ prefix (monolithic-style naming). Link the
# import libs the delegate + plugin touch; tbb is pulled in transitively by
# inlined Tf/Vt/Work template code so it is linked explicitly.
# USD's lib dir on the search path so auto-referenced libs (e.g. tbb12.lib,
# pulled in by tbb.lib's #pragma comment(lib)) resolve.
target_link_directories(pyxis_hydra_omni PRIVATE
    "${PXR_USD_ROOT}/lib" "${PYXIS_OMNI_PYTHON_ROOT}/libs")

set(_usd_libs usd_hd usd_hf usd_hdsi usd_sdf usd_tf usd_vt usd_gf usd_plug
              usd_usd usd_usdImaging tbb)
foreach(_lib IN LISTS _usd_libs)
    target_link_libraries(pyxis_hydra_omni PRIVATE "${PXR_USD_ROOT}/lib/${_lib}.lib")
endforeach()
target_link_libraries(pyxis_hydra_omni PRIVATE
    "${PYXIS_OMNI_PYTHON_ROOT}/libs/python312.lib")

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

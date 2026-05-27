# PyxisNvUsd.cmake — wire a target against Kit's nv-usd 25.11 (RFC 0006).
#
# Replaces find_package(pxr) (vcpkg OpenUSD 26.3) with Kit's nv-usd, so the same
# USD-consuming sources build for both the standalone host and Omniverse Kit and
# the Hydra delegate stops being forked across two USD versions. nv-usd lives
# under <repo>/build/omniverse/usd-deps/usd (acquired by _tools/omniverse/setup.ps1,
# the same root RFC 0004 already uses). The renderer/platform stay USD-free (§1)
# and never call this.
#
#   include(PyxisNvUsd)
#   pyxis_use_nv_usd(pyxis_hydra)   # adds includes + links the nv-usd + python libs
#
# We deliberately do NOT use pxrConfig.cmake: it find_dependency()s
# TBB/OpenSubdiv/MaterialX/Imath/Python3 Config files the standalone package
# doesn't ship. The usd_*.lib are DLL import libs (Python/TBB resolved inside the
# USD DLLs), and every header lives under include/, so direct wiring is
# self-contained — exactly as pyxis_hydra_omni has proven.

if(DEFINED _PYXIS_NV_USD_INCLUDED)
    return()
endif()
set(_PYXIS_NV_USD_INCLUDED ON)

set(PYXIS_REPO_ROOT_FOR_USD "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "Pyxis repo root")

set(PXR_USD_ROOT "${PYXIS_REPO_ROOT_FOR_USD}/build/omniverse/usd-deps/usd"
    CACHE PATH "Kit nv-usd 25.11 root (default: <repo>/build/omniverse/usd-deps/usd)")
set(PYXIS_NV_USD_PYTHON_ROOT "D:/packman-repo/chk/python/3.12.13+nv1-windows-x86_64"
    CACHE PATH "Kit Python 3.12 root (nv-usd 25.11 is a py312 build: pyconfig.h + python312.lib)")

if(NOT EXISTS "${PXR_USD_ROOT}/include/pxr/pxr.h")
    message(FATAL_ERROR
        "PyxisNvUsd: nv-usd not found at PXR_USD_ROOT='${PXR_USD_ROOT}'.\n"
        "Run _tools/omniverse/setup.ps1 (packman-pulls nv-usd 25.11), or pass\n"
        "-DPXR_USD_ROOT=<path-with-include/pxr/pxr.h>.")
endif()
if(NOT EXISTS "${PYXIS_NV_USD_PYTHON_ROOT}/include/pyconfig.h")
    message(FATAL_ERROR
        "PyxisNvUsd: Python 3.12 not found at '${PYXIS_NV_USD_PYTHON_ROOT}'.\n"
        "nv-usd 25.11 is a py312 build; pass -DPYXIS_NV_USD_PYTHON_ROOT=<python3.12-root>.")
endif()

# The full nv-usd lib set the ingest adapters need. usd_boost + usd_python carry
# the pxr_boost::python converter statics the Usd*/UsdGeom headers instantiate.
set(PYXIS_NV_USD_LIBS
    usd_hd usd_hf usd_hdsi usd_sdf usd_tf usd_vt usd_gf usd_plug usd_arch usd_js
    usd_work usd_trace usd_usd usd_usdGeom usd_usdImaging usd_usdShade usd_usdLux
    usd_usdRender usd_usdVol usd_usdUtils usd_sdr usd_ar usd_kind usd_pcp usd_ndr
    usd_usdSkel usd_usdSkelImaging usd_boost usd_python tbb
    CACHE INTERNAL "nv-usd import libs Pyxis ingest links")

# nv-usd include path + import libs only — no EH/RTTI flags, no _HAS_EXCEPTIONS
# change. SYSTEM includes so USD's warnings don't trip /WX. This is the half that
# pyxis_app needs: it calls UsdStage::Open directly (so it must see the headers
# and link the DLLs' import libs) but stays on the exception-free perimeter
# (/EHs-c- + _HAS_EXCEPTIONS=0, exactly as it did against vcpkg USD on main —
# USD's headers are catch-free, so they compile fine without exceptions enabled).
function(pyxis_nv_usd_includes_and_libs tgt)
    target_include_directories(${tgt} SYSTEM PRIVATE
        "${PXR_USD_ROOT}/include"
        "${PYXIS_NV_USD_PYTHON_ROOT}/include")
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        # std::is_pod deprecation (C++20+) fires from tf headers regardless of EH.
        target_compile_definitions(${tgt} PRIVATE
            _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1 NOMINMAX)
    endif()
    target_link_directories(${tgt} PRIVATE
        "${PXR_USD_ROOT}/lib" "${PYXIS_NV_USD_PYTHON_ROOT}/libs")
    foreach(_lib IN LISTS PYXIS_NV_USD_LIBS)
        if(EXISTS "${PXR_USD_ROOT}/lib/${_lib}.lib")
            target_link_libraries(${tgt} PRIVATE "${PXR_USD_ROOT}/lib/${_lib}.lib")
        endif()
    endforeach()
    target_link_libraries(${tgt} PRIVATE
        "${PYXIS_NV_USD_PYTHON_ROOT}/libs/python312.lib")
endfunction()

# Apply nv-usd to an ingest-side `tgt` (material_translation / hydra / usd_ingest):
# includes + libs + USD's exception/RTTI requirements. These libs already use
# pyxis_enable_exceptions_and_rtti, so /EHsc /GR here is consistent.
function(pyxis_use_nv_usd tgt)
    pyxis_nv_usd_includes_and_libs(${tgt})
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        # USD throws + uses RTTI.
        target_compile_options(${tgt} PRIVATE /EHsc /GR)
    endif()
endfunction()

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
# Repo-relative omniverse working dir (gitignored). The Kit SDK + nv-usd live
# under <repo>/build/omniverse — no sibling external folder.
set(PYXIS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Pyxis repo root")
set(PXR_USD_ROOT "${PYXIS_REPO_ROOT}/build/omniverse/usd-deps/usd"
    CACHE PATH "Kit nv-usd 25.11 root (default: <repo>/build/omniverse/usd-deps/usd)")
if(NOT EXISTS "${PXR_USD_ROOT}/include/pxr/pxr.h")
    message(FATAL_ERROR
        "pyxis_hydra_omni: nv-usd not found at PXR_USD_ROOT='${PXR_USD_ROOT}'.\n"
        "Run _tools/omniverse/setup.ps1 first (it packman-pulls nv-usd 25.11 into\n"
        "build/omniverse/usd-deps), or pass -DPXR_USD_ROOT=<path-with-include/pxr/pxr.h>.")
endif()

# ----------------------------------------------------------------------------
# Pyxis's prebuilt, USD-free renderer (RFC 0004 C4). pyxis_renderer /
# pyxis_platform are built by the main vcpkg build (build/dev) — we consume
# their public headers + Release import libs here. They never see USD (§1); only
# this shim does. Release/MD to match both nv-usd and the vcpkg Release CRT.
# ----------------------------------------------------------------------------
set(PYXIS_BUILD_DIR "${PYXIS_REPO_ROOT}/build/dev"
    CACHE PATH "Pyxis main build dir (prebuilt renderer libs + fetched nvrhi)")
set(_pyxis_renderer_lib "${PYXIS_BUILD_DIR}/lib/Release/pyxis_renderer.lib")
set(_pyxis_platform_lib "${PYXIS_BUILD_DIR}/lib/Release/pyxis_platform.lib")
# §25.O.3 — the delegate ingests via the SAME StageWalker as the standalone
# (pyxis_usd_ingest), so the World Lobby populates the GpuScene byte-identically.
# Both build/dev and build/omni are nv-usd 25.11, so this prebuilt DLL is ABI-
# compatible; its runtime DLL (+ transitive deps) live in build/dev/bin/Release,
# already on the harness PATH.
set(_pyxis_usd_ingest_lib "${PYXIS_BUILD_DIR}/lib/Release/pyxis_usd_ingest.lib")
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

# Carbonite SDK headers (carb/PluginUtils.h, omni/ext/IExt.h) for the native IExt
# registration plugin. Shipped with the Kit SDK under .../kit/dev/include. No
# carb import lib exists/needed: Carbonite resolves framework symbols at plugin
# load via the CARB_PLUGIN_IMPL boilerplate.
set(PYXIS_OMNI_CARB_INCLUDE
    "${PYXIS_REPO_ROOT}/build/omniverse/kit-app-template/_build/windows-x86_64/release/kit/dev/include"
    CACHE PATH "Kit Carbonite SDK include dir (carb/, omni/ext/)")

# usd_usdGeom is needed by the smoke harness (authors a UsdGeomMesh); harmless in
# the DLL. tbb is linked explicitly (inlined Tf/Vt/Work template code).
# usd_boost + usd_python: the Usd*/UsdGeom headers (used by the smoke harness)
# instantiate pxr_boost::python converter statics for Sdf/Tf types.
set(_usd_libs usd_hd usd_hf usd_hdsi usd_sdf usd_tf usd_vt usd_gf usd_plug
              usd_usd usd_usdGeom usd_usdImaging usd_usdShade usd_usdLux
              usd_usdUtils usd_sdr usd_boost usd_python tbb)

# Shared configuration applied to both the delegate DLL and the smoke harness exe.
function(pyxis_omni_configure tgt)
    target_include_directories(${tgt} SYSTEM PRIVATE
        "${PXR_USD_ROOT}/include"
        "${PYXIS_OMNI_PYTHON_ROOT}/include"
        "${PYXIS_REPO_ROOT}/sources/pyxis_renderer/Public"
        "${PYXIS_REPO_ROOT}/sources/pyxis_platform/Public"
        # RFC 0006: the single Pyxis Hydra delegate + engine live in pyxis_hydra.
        # The Kit DLL compiles those .cpp directly (below) and the glue includes
        # their headers — Private for the delegate/engine/render-param, Public for
        # the HdPyxisRendererPlugin registration header.
        "${PYXIS_REPO_ROOT}/sources/pyxis_hydra/Private"
        "${PYXIS_REPO_ROOT}/sources/pyxis_hydra/Public"
        # RFC 0006 §25.O.3 — the delegate now translates materials through the
        # SHARED pyxis_material_translation::FromUsdShade (same code path as
        # pyxis_usd_ingest's StageWalker), so MDL/MaterialX/UsdPreviewSurface all
        # resolve identically across both adapters. Its one .cpp is compiled into
        # each delegate target below; this exposes its public header.
        "${PYXIS_REPO_ROOT}/sources/pyxis_material_translation/Public"
        # §25.O.3 — StageWalker public header: the delegate ingests the stage
        # through the SAME StageWalker as the standalone (byte-identical scene).
        "${PYXIS_REPO_ROOT}/sources/pyxis_usd_ingest/Public"
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
        "${_pyxis_renderer_lib}" "${_pyxis_platform_lib}" "${_pyxis_usd_ingest_lib}")
endfunction()

# RFC 0006: compile the SINGLE Pyxis Hydra delegate + engine + plugin from
# pyxis_hydra (the fork's HdPyxisOmni* / PyxisEngine copies were deleted). Only
# the Kit-specific glue (host driver + viewport bridge) is local to this module.
set(_pyxis_hydra_src "${PYXIS_REPO_ROOT}/sources/pyxis_hydra")
# RFC 0006 §25.O.3 — the shared material translator (one .cpp), compiled into
# every target that compiles the delegate so the delegate's Sync impls call the
# SAME FromUsdShade as StageWalker (byte-identical MDL/MaterialX/UsdPreviewSurface
# → OpenPBR across both ingest adapters).
set(_pyxis_mattrans_src
    "${PYXIS_REPO_ROOT}/sources/pyxis_material_translation/Private/UsdShadeToOpenPBR.cpp")
add_library(pyxis_hydra_omni SHARED
    "${_pyxis_hydra_src}/Private/HdPyxisRenderDelegate.cpp"
    "${_pyxis_hydra_src}/Private/HdPyxisRendererPlugin.cpp"
    "${_pyxis_hydra_src}/Private/PyxisEngine.cpp"
    "${_pyxis_mattrans_src}"
    Private/PyxisHydraHost.cpp
    Private/PyxisViewportBridge.cpp
)
pyxis_omni_configure(pyxis_hydra_omni)
# Export PyxisViewportBridge::Create so the C++17 panel DLL can link it.
target_compile_definitions(pyxis_hydra_omni PRIVATE PYXIS_VIEWPORT_BRIDGE_EXPORTS)

# ----------------------------------------------------------------------------
# omni.hydra.pyxis.plugin.dll — native Carbonite IExt (RFC 0004). Replaces the
# former Python startup module: on extension load it points USD's PlugRegistry
# at the staged plugInfo.json, so the delegate is discoverable. The `*.plugin`
# filename matches the extension's `[[native.plugin]] path = "bin/*.plugin"`.
# It needs only USD's Plug/Tf + Carbonite headers — NOT the renderer/platform
# libs, Python, or NVRHI — so it gets its own minimal wiring (not
# pyxis_omni_configure, which links the full renderer stack).
# ----------------------------------------------------------------------------
if(NOT EXISTS "${PYXIS_OMNI_CARB_INCLUDE}/carb/PluginUtils.h")
    message(FATAL_ERROR
        "pyxis_hydra_omni: Carbonite headers not found at\n"
        "  '${PYXIS_OMNI_CARB_INCLUDE}/carb/PluginUtils.h'.\n"
        "Run _tools/omniverse/setup.ps1 (it bootstraps the Kit SDK), or pass\n"
        "-DPYXIS_OMNI_CARB_INCLUDE=<.../kit/dev/include>.")
endif()
add_library(pyxis_hydra_omni_ext SHARED Private/PyxisExtModule.cpp)
# Carbonite headers (carb/PluginUtils.h -> ... -> omni/String.inl) are validated
# against MSVC cl at C++17; strict clang-cl C++23 rejects a legal getline()
# overload there. This module needs no C++23 (USD + Win32 only), so pin it to 17.
set_target_properties(pyxis_hydra_omni_ext PROPERTIES
    OUTPUT_NAME "omni.hydra.pyxis.plugin" PREFIX ""
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(pyxis_hydra_omni_ext SYSTEM PRIVATE
    "${PXR_USD_ROOT}/include"
    "${PYXIS_OMNI_PYTHON_ROOT}/include"
    "${PYXIS_OMNI_CARB_INCLUDE}")
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(pyxis_hydra_omni_ext PRIVATE /EHsc /GR)
    target_compile_definitions(pyxis_hydra_omni_ext PRIVATE
        _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1 NOMINMAX)
endif()
target_link_directories(pyxis_hydra_omni_ext PRIVATE
    "${PXR_USD_ROOT}/lib" "${PYXIS_OMNI_PYTHON_ROOT}/libs")
# Plug.RegisterPlugins + TfType reflection: link Plug/Tf and their transitive
# nv-usd deps. python312 + boost satisfy Tf's inlined python glue statics.
foreach(_lib IN ITEMS usd_plug usd_tf usd_js usd_work usd_trace usd_arch
                      usd_boost usd_python tbb)
    target_link_libraries(pyxis_hydra_omni_ext PRIVATE "${PXR_USD_ROOT}/lib/${_lib}.lib")
endforeach()
target_link_libraries(pyxis_hydra_omni_ext PRIVATE
    "${PYXIS_OMNI_PYTHON_ROOT}/libs/python312.lib")

# ----------------------------------------------------------------------------
# omni.hydra.pyxis.panel.plugin.dll — in-Kit viewport panel (RFC 0004 Option A,
# Stage 2+). A second native IExt in the omni.hydra.pyxis extension (the
# [[native.plugin]] glob matches it too). C++17 like the registration plugin
# (Carbonite headers). It talks to the C++23 render core ONLY through the POD
# PyxisViewportBridge boundary, linking pyxis_hydra_omni.dll for Create().
# ----------------------------------------------------------------------------
add_library(pyxis_hydra_omni_panel SHARED Private/PyxisViewportPanel.cpp)
set_target_properties(pyxis_hydra_omni_panel PROPERTIES
    OUTPUT_NAME "omni.hydra.pyxis.panel.plugin" PREFIX ""
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# omni.ui's Callback.h pulls boost/preprocessor.hpp — a Packman package
# (boost-preprocessor). Resolve its include root by glob under the Packman cache.
if(NOT PYXIS_OMNI_BOOST_PP_INCLUDE)
    if(DEFINED ENV{PM_PACKAGES_ROOT})
        file(GLOB _boost_pp_dirs "$ENV{PM_PACKAGES_ROOT}/chk/boost-preprocessor/*"
                                 "$ENV{PM_PACKAGES_ROOT}/boost-preprocessor/*")
    endif()
    if(NOT _boost_pp_dirs)
        file(GLOB _boost_pp_dirs "D:/packman-repo/chk/boost-preprocessor/*")
    endif()
    foreach(_d IN LISTS _boost_pp_dirs)
        if(EXISTS "${_d}/boost/preprocessor.hpp")
            set(PYXIS_OMNI_BOOST_PP_INCLUDE "${_d}")
            break()
        endif()
    endforeach()
endif()
if(NOT PYXIS_OMNI_BOOST_PP_INCLUDE)
    message(FATAL_ERROR
        "pyxis_hydra_omni: boost/preprocessor.hpp (Packman boost-preprocessor) not found.\n"
        "Pass -DPYXIS_OMNI_BOOST_PP_INCLUDE=<dir-containing-boost/preprocessor.hpp>.")
endif()
# Private/kit_stubs FIRST: supplies carb/imaging/IImaging.h, which omni.ui needs
# but Kit 110's public SDK omits (see the stub header for the ABI rationale).
target_include_directories(pyxis_hydra_omni_panel SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private/kit_stubs"
    "${PYXIS_OMNI_CARB_INCLUDE}"
    "${PYXIS_OMNI_BOOST_PP_INCLUDE}")
target_include_directories(pyxis_hydra_omni_panel PRIVATE Private)
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(pyxis_hydra_omni_panel PRIVATE /EHsc /GR)
    target_compile_definitions(pyxis_hydra_omni_panel PRIVATE
        _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1 NOMINMAX)
endif()
# Links the bridge's exported Create() from pyxis_hydra_omni.dll (its import lib).
target_link_libraries(pyxis_hydra_omni_panel PRIVATE pyxis_hydra_omni)

# omni.ui C++ classes (Window / ImageWithProvider / DynamicTextureProvider). Kit
# ships omni.ui.lib inside the omni.ui EXTENSION's bin (registry-pulled), not the
# kit/dev tree. build.ps1 resolves the editor's exts so it exists; locate it by
# glob (the version is pinned to Kit 110.1.1). Headers are already on the include
# path via PYXIS_OMNI_CARB_INCLUDE (kit/dev/include/omni/ui/...).
if(NOT PYXIS_OMNI_UI_LIB)
    file(GLOB _omni_ui_libs
        "${PYXIS_REPO_ROOT}/build/omniverse/kit-app-template/_build/windows-x86_64/release/extscache/omni.ui-*/bin/omni.ui.lib")
    if(_omni_ui_libs)
        list(GET _omni_ui_libs 0 PYXIS_OMNI_UI_LIB)
    endif()
endif()
if(NOT PYXIS_OMNI_UI_LIB OR NOT EXISTS "${PYXIS_OMNI_UI_LIB}")
    message(FATAL_ERROR
        "pyxis_hydra_omni: omni.ui.lib not found under .../release/extscache/omni.ui-*/bin/.\n"
        "Launch the editor once (or let build.ps1 resolve exts) so omni.ui is pulled,\n"
        "or pass -DPYXIS_OMNI_UI_LIB=<path-to-omni.ui.lib>.")
endif()
message(STATUS "pyxis_hydra_omni: omni.ui.lib = ${PYXIS_OMNI_UI_LIB}")
target_link_libraries(pyxis_hydra_omni_panel PRIVATE "${PYXIS_OMNI_UI_LIB}")

# Headless HdEngine smoke (RFC 0004 C4-full): drives a USD stage through the
# delegate and verifies geometry reaches GpuScene + a frame renders. Compiles
# the delegate + engine sources directly so it can instantiate the delegate.
add_executable(pyxis_hydra_omni_smoke
    tests/HdEngineSmoke.cpp
    "${_pyxis_hydra_src}/Private/HdPyxisRenderDelegate.cpp"
    "${_pyxis_hydra_src}/Private/PyxisEngine.cpp"
    "${_pyxis_mattrans_src}"
    Private/PyxisHydraHost.cpp
)
pyxis_omni_configure(pyxis_hydra_omni_smoke)

# RFC 0006 — headless render of a REAL scene (World Lobby) through the generic
# Hydra path (no omni.hydra.pxr, no compatibilityMode). Proves Pyxis works in
# Omniverse depending only on Hydra. Same source set as the smoke harness.
add_executable(pyxis_hydra_omni_lobby
    tests/WorldLobbyHeadless.cpp
    "${_pyxis_hydra_src}/Private/HdPyxisRenderDelegate.cpp"
    "${_pyxis_hydra_src}/Private/PyxisEngine.cpp"
    "${_pyxis_mattrans_src}"
    Private/PyxisHydraHost.cpp
)
pyxis_omni_configure(pyxis_hydra_omni_lobby)

# Headless proof that USD's PlugRegistry resolves + loads the delegate from its
# staged plugInfo.json (the pxr-engine load path) — catches LibraryPath/Root +
# dependency bugs without a Kit viewport. Links only Plug/Tf (+ nv-usd transitive).
# Headless proof that delegate teardown releases the Vulkan device + VRAM across
# create/destroy cycles (prerequisite for exclusive renderer activation in Kit).
add_executable(pyxis_multicycle_vram_test
    tests/MultiCycleVramTest.cpp
    "${_pyxis_hydra_src}/Private/HdPyxisRenderDelegate.cpp"
    "${_pyxis_hydra_src}/Private/PyxisEngine.cpp"
    "${_pyxis_mattrans_src}"
    Private/PyxisHydraHost.cpp
)
pyxis_omni_configure(pyxis_multicycle_vram_test)
# Optional AddressSanitizer build to pinpoint memory corruption in the delegate's
# Hydra ingest (configure with -DPYXIS_OMNI_ASAN=ON). Instruments OUR delegate
# code; ASAN's dynamic runtime also tracks the renderer's allocations.
if(PYXIS_OMNI_ASAN)
    target_compile_options(pyxis_multicycle_vram_test PRIVATE -fsanitize=address)
    set(_asan_lib_dir "C:/Program Files/LLVM/lib/clang/22/lib/windows")
    target_link_directories(pyxis_multicycle_vram_test PRIVATE "${_asan_lib_dir}")
    target_link_libraries(pyxis_multicycle_vram_test PRIVATE
        "${_asan_lib_dir}/clang_rt.asan_dynamic-x86_64.lib")
    target_link_options(pyxis_multicycle_vram_test PRIVATE
        "/wholearchive:${_asan_lib_dir}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
endif()

add_executable(pyxis_plugin_load_test tests/PluginLoadTest.cpp)
target_include_directories(pyxis_plugin_load_test SYSTEM PRIVATE "${PXR_USD_ROOT}/include")
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(pyxis_plugin_load_test PRIVATE /EHsc /GR)
    target_compile_definitions(pyxis_plugin_load_test PRIVATE
        _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1 NOMINMAX)
endif()
target_link_directories(pyxis_plugin_load_test PRIVATE "${PXR_USD_ROOT}/lib")
foreach(_lib IN ITEMS usd_plug usd_tf usd_js usd_work usd_trace usd_arch usd_boost tbb)
    target_link_libraries(pyxis_plugin_load_test PRIVATE "${PXR_USD_ROOT}/lib/${_lib}.lib")
endforeach()

# ctest: drive the smoke via run_smoke.ps1 (sets the nv-usd / Pyxis runtime PATH
# + shaders, then runs the exe; exit 0 = pass). Run with: ctest --test-dir build/omni
enable_testing()
add_test(NAME HdEngineSmoke
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PYXIS_REPO_ROOT}/_tools/omniverse/run_smoke.ps1")
set_tests_properties(HdEngineSmoke PROPERTIES TIMEOUT 180 LABELS "omniverse;integration"
    RESOURCE_LOCK "pyxis_gpu")

# ----------------------------------------------------------------------------
# §25.O.3 INGEST-PARITY REGRESSION SUITE (run_parity.ps1 + parity_check.py).
#
# Renders each fixture through BOTH adapters (the Hydra delegate + pyxis.exe
# --ingest usd_direct) and HARD-GATES that lights + materials are byte-identical
# and the image matches within a tight MAD/correlation tolerance. The delegate
# ingests via the SAME StageWalker as the standalone, so any divergence is a real
# regression. Runs locally with: ctest --test-dir build/omni  (CI does not build
# the omni targets — they need the Kit SDK + nv-usd from packman + a GPU).
#
# Parity.WorldLobby is the comprehensive scene; the golden-fixture cases give
# focused coverage of individual features (subsets, analytic geom, curves, points,
# camera intrinsics incl. orthographic, lights, displayColor) — each is the SAME
# .usda the standalone golden suite renders, all using the camera at /World/Camera.
# ----------------------------------------------------------------------------
add_test(NAME Parity.WorldLobby
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PYXIS_REPO_ROOT}/_tools/omniverse/run_parity.ps1")
set_tests_properties(Parity.WorldLobby PROPERTIES TIMEOUT 300 LABELS "omniverse;integration;parity"
    RESOURCE_LOCK "pyxis_gpu")

# Curated golden fixtures (static, camera at /World/Camera) covering the delegate
# ingest paths. 256x256 to match the golden config; the comparison is delegate-vs-
# standalone (not against a stored baseline), so the resolution only needs to agree.
set(_pyxis_parity_fixtures
    geom_triangle_mesh        # basic mesh + triangulation
    geom_quad_ngon            # n-gon fan triangulation
    geom_geom_subset          # face-bound UsdGeomSubset -> per-subset material
    geom_holes                # holeIndices
    geom_lefthanded           # winding / orientation
    geom_normals_facevarying  # authored faceVarying normals
    geom_two_meshes_one_xform # multiple meshes under one xform
    geom_displaycolor_per_vertex   # displayColor fallback material
    geom_doubleSided_quad     # doubleSided
    analytic_sphere_only      # implicit Sphere -> tessellated by StageWalker
    geom_analytic_primitives  # sphere/cube/cone/cylinder/capsule mix
    geom_basis_curves         # BasisCurves ribbons
    geom_points               # Points -> sprites
    camera_focal_lengths      # focal length intrinsic
    camera_aperture_wide      # horizontal aperture / aspect
    camera_orthographic       # orthographic projection mode
    camera_roll_z             # rolled camera transform
    light_dome                # dome / IBL
    light_distant_lambert     # distant light
    light_disk                # disk area light
    light_cylinder)           # cylinder area light
foreach(_fx IN LISTS _pyxis_parity_fixtures)
    set(_fx_usd "${PYXIS_REPO_ROOT}/tests/golden-tests-data/${_fx}/fixture.usda")
    if(EXISTS "${_fx_usd}")
        add_test(NAME "Parity.${_fx}"
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${PYXIS_REPO_ROOT}/_tools/omniverse/run_parity.ps1"
                    -Scene "${_fx_usd}" -Camera "/World/Camera"
                    -Width 256 -Height 256 -Name "${_fx}")
        set_tests_properties("Parity.${_fx}" PROPERTIES
            TIMEOUT 180 LABELS "omniverse;integration;parity"
            RESOURCE_LOCK "pyxis_gpu")
    endif()
endforeach()

# ----------------------------------------------------------------------------
# plugInfo.json — written next to the DLL so a Hydra host (Kit / usdview) finds
# the delegate. LibraryPath is relative to <out>/resources/, so "../" + dll.
# ----------------------------------------------------------------------------
# LibraryPath is resolved relative to Root (".." = <ext>/bin/, since plugInfo.json
# lives in <ext>/bin/resources/). The DLL sits in <ext>/bin/, so LibraryPath is
# just the filename — NOT "../pyxis_hydra_omni.dll", which would resolve to
# <ext>/pyxis_hydra_omni.dll (one dir too high -> USD "module could not be found").
set(PYXIS_OMNI_PLUGIN_LIBPATH "pyxis_hydra_omni.dll")
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

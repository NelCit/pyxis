# pyxis_hydra — SHARED. Plan §3 / §7 / §25.
#
# Hydra render delegate ingest adapter. Loadable as a USD plugin so
# usdview / any HdEngine host can pick "Pyxis" as a renderer.
#
# /EHsc + /GR — USD throws + uses RTTI heavily. The §30.1
# exception-free perimeter applies to renderer/platform; this is an
# ingest-side lib that crosses into USD's world.

add_library(pyxis_hydra SHARED)
add_library(Pyxis::Hydra ALIAS pyxis_hydra)

pyxis_define_module(pyxis_hydra)
pyxis_enable_exceptions_and_rtti(pyxis_hydra)

target_sources(pyxis_hydra PUBLIC
    FILE_SET pyxis_hydra_public TYPE HEADERS
    BASE_DIRS Public
    FILES
        Public/Pyxis/Hydra/HydraApi.h
        Public/Pyxis/Hydra/HdPyxisRendererPlugin.h
)

target_sources(pyxis_hydra PRIVATE
    Private/HdPyxisRendererPlugin.cpp
    Private/HdPyxisRenderDelegate.h
    Private/HdPyxisRenderDelegate.cpp
    Private/HdPyxisRenderParam.h
    # RFC 0006: the delegate now OWNS its render engine (own Vulkan device +
    # GpuScene + PyxisRenderer + GpuInteropExporter), so the single delegate
    # source loads as a plugin in any HdEngine host (usdview / Omniverse Kit).
    # This is the engine the pyxis_hydra_omni Kit fork used to carry separately.
    Private/PyxisEngine.h
    Private/PyxisEngine.cpp
)

# RFC 0006: link Kit's nv-usd 25.11 (not vcpkg USD 26.3) so this delegate is the
# single source compiled for both the standalone host and Omniverse Kit. The full
# usd_* import set (incl. hd/hdsi/hf/usdImaging) + python312 + the std::is_pod
# silence + /EHsc /GR come from pyxis_use_nv_usd.
include(PyxisNvUsd)

target_link_libraries(pyxis_hydra
    PUBLIC
        Pyxis::Renderer
        Pyxis::MaterialTranslation     # Sync impls call FromUsdShade.
        Pyxis::UsdIngest               # §25.O.3 — ingest the stage via StageWalker (same as standalone).
    PRIVATE
        # RFC 0006: PyxisEngine stands up a Pyxis Vulkan device + GpuScene +
        # renderer + GpuInteropExporter. Platform links nvrhi PUBLIC, so the
        # nvrhi headers PyxisEngine.cpp needs ride along transitively.
        Pyxis::Platform
)
pyxis_use_nv_usd(pyxis_hydra)

# ----------------------------------------------------------------------------
# plugInfo.json — written next to pyxis_hydra.dll at build time so Hydra's
# plugin registry finds it. The registry walks every PXR_PLUGINPATH_NAME
# entry (an env var the host application sets) for `plugInfo.json` files
# and ingests them; we ship one keyed on `LibraryPath` pointing at the dll.
#
# `Root: ".."` makes LibraryPath relative to the *parent* of `resources/`,
# so `<bin>/Resources/usd/hdPyxis/resources/plugInfo.json` resolves
# `LibraryPath` relative to `<bin>/Resources/usd/hdPyxis/`. The dll lives
# at `<bin>/pyxis_hydra.dll`, so LibraryPath is `../../../pyxis_hydra.dll`.
# ----------------------------------------------------------------------------
set(PYXIS_HYDRA_PLUGIN_LIBPATH "../../../pyxis_hydra.dll")
set(_pyxis_hydra_plugin_dst
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/Resources/usd/hdPyxis/resources")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    @ONLY)
add_custom_command(TARGET pyxis_hydra POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_pyxis_hydra_plugin_dst}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
        "${_pyxis_hydra_plugin_dst}/plugInfo.json"
    COMMENT "Copying pyxis_hydra plugInfo.json to <bin>/Resources/usd/hdPyxis/resources/")
unset(_pyxis_hydra_plugin_dst)
unset(PYXIS_HYDRA_PLUGIN_LIBPATH)

# ----------------------------------------------------------------------------
# RFC 0007 — headless Hydra tests (moved from the retired pyxis_hydra_omni).
#
# The delegate / PyxisEngine classes are NOT exported from pyxis_hydra.dll
# (WINDOWS_EXPORT_ALL_SYMBOLS OFF, no __declspec, they live in Private/), so each
# test EXE compiles the delegate + engine + the shared material translator + the
# host driver directly (the same source set the old build/omni targets used),
# rather than linking the DLL. They link the renderer/platform/ingest libs and
# wire nv-usd exactly as the delegate does (pyxis_use_nv_usd).
#
# These need a GPU + the nv-usd runtime, so they carry the `requires_gpu` ctest
# label and CI (no GPU) excludes them (build.yml: ctest -LE "m2|requires_gpu").
# The test executables still BUILD in CI — only their ctest run is skipped.
# ----------------------------------------------------------------------------
set(_pyxis_hydra_delegate_src
    Private/HdPyxisRenderDelegate.cpp
    Private/PyxisEngine.cpp
    ${CMAKE_SOURCE_DIR}/sources/pyxis_material_translation/Private/UsdShadeToOpenPBR.cpp
    Private/PyxisHydraHost.cpp)

# Shared wiring for a test exe that drives the delegate through PyxisHydraHost.
function(pyxis_hydra_test tgt)
    target_include_directories(${tgt} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Private")
    # The host/tests reach the renderer + platform + ingest + material
    # translator public headers; Pyxis::Platform carries nvrhi's headers
    # transitively (PyxisEngine.cpp includes them). nv-usd + /EHsc /GR + the
    # is_pod silence come from pyxis_use_nv_usd, matching the delegate target.
    target_link_libraries(${tgt} PRIVATE
        Pyxis::Renderer Pyxis::MaterialTranslation Pyxis::UsdIngest Pyxis::Platform)
    pyxis_use_nv_usd(${tgt})
    # Match pyxis_set_target_compile_options: define WIN32_LEAN_AND_MEAN (so the
    # delegate/engine's guarded `#define` is skipped — clang-tidy would otherwise
    # flag it as a non-PYXIS_ macro) + _CRT_SECURE_NO_WARNINGS (fopen in the BMP
    # writers). These TUs aren't pyxis_define_module modules, so we set them here.
    target_compile_definitions(${tgt} PRIVATE
        WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # Half-float decode, BMP magic numbers, and dense single-letter pixel loops
    # trip clang-tidy's identifier-length / rounding rules the same way the
    # unit-test harness's GpuInteropRoundTrip / PyxisRenderToExportedImage do;
    # take the same documented SKIP_LINTING escape (the delegate/engine/host
    # sources ARE linted via the pyxis_hydra target above).
    get_target_property(_srcs ${tgt} SOURCES)
    foreach(_s IN LISTS _srcs)
        if(_s MATCHES "tests/")
            set_source_files_properties(${_s} PROPERTIES SKIP_LINTING ON)
        endif()
    endforeach()
endfunction()

# Headless HdEngine smoke (in-memory stage -> delegate -> GpuScene -> render +
# host-AOV composite). Compiles the delegate + engine directly.
add_executable(pyxis_hydra_omni_smoke
    tests/HdEngineSmoke.cpp ${_pyxis_hydra_delegate_src})
pyxis_hydra_test(pyxis_hydra_omni_smoke)

# Headless World Lobby render through the generic Hydra path (StageWalker ingest).
add_executable(pyxis_hydra_omni_lobby
    tests/WorldLobbyHeadless.cpp ${_pyxis_hydra_delegate_src})
pyxis_hydra_test(pyxis_hydra_omni_lobby)

# Headless proof delegate teardown releases the Vulkan device + VRAM each cycle.
add_executable(pyxis_multicycle_vram_test
    tests/MultiCycleVramTest.cpp ${_pyxis_hydra_delegate_src})
pyxis_hydra_test(pyxis_multicycle_vram_test)

# Headless proof USD's PlugRegistry resolves + loads the delegate from its
# staged plugInfo.json (the pxr-engine load path). Only needs Plug/Tf; it links
# the nv-usd import libs (pyxis_use_nv_usd) but NOT the renderer stack.
add_executable(pyxis_plugin_load_test tests/PluginLoadTest.cpp)
pyxis_use_nv_usd(pyxis_plugin_load_test)
target_compile_definitions(pyxis_plugin_load_test PRIVATE
    WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set_source_files_properties(tests/PluginLoadTest.cpp PROPERTIES SKIP_LINTING ON)

# ----------------------------------------------------------------------------
# ctest — GPU + nv-usd-runtime integration tests (RFC 0007). All labelled
# `requires_gpu` so CI (no GPU) skips them via ctest -LE "m2|requires_gpu"; they
# run locally on a GPU workstation with: ctest --test-dir build/dev -L parity
# (or -L requires_gpu for the full set). RESOURCE_LOCK serialises GPU access.
# ----------------------------------------------------------------------------
if(PYXIS_BUILD_TESTS)
    # enable_testing() at the top level runs AFTER this subdirectory is added, so
    # register tests for this directory scope explicitly (idempotent).
    enable_testing()

    # HdEngine smoke via run_smoke.ps1 (sets the nv-usd / Pyxis runtime PATH +
    # shaders, then runs the exe; exit 0 = pass).
    add_test(NAME HdEngineSmoke
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/_tools/omniverse/run_smoke.ps1")
    set_tests_properties(HdEngineSmoke PROPERTIES
        TIMEOUT 180 LABELS "omniverse;integration;requires_gpu"
        RESOURCE_LOCK "pyxis_gpu")

    # §25.O.3 ingest-parity suite: render each fixture through BOTH adapters (the
    # Hydra delegate + pyxis.exe --ingest usd_direct) and hard-gate lights +
    # materials byte-identical and the image within a tight MAD/correlation
    # tolerance. The delegate ingests via the SAME StageWalker as the standalone,
    # so any divergence is a real regression.
    add_test(NAME Parity.WorldLobby
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/_tools/omniverse/run_parity.ps1")
    set_tests_properties(Parity.WorldLobby PROPERTIES
        TIMEOUT 300 LABELS "omniverse;integration;parity;requires_gpu"
        RESOURCE_LOCK "pyxis_gpu")

    # Display-parity: the "Omniverse viewer == Pyxis engine viewer" invariant.
    # Renders the World Lobby through ALL THREE Pyxis display paths — headless
    # (pyxis.exe --output), standalone viewer (pyxis.exe --screenshot), and the
    # Kit/Omniverse pxr viewport (capture) — and asserts the displayed pixels
    # match (each applies the sRGB OETF exactly once). Regression guard for the
    # color double/zero-encode bug. The CPU-level proof is the AovColorEncode /
    # ColorEncoding unit tests; this is the end-to-end GPU confirmation.
    add_test(NAME DisplayParity.WorldLobby
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/_tools/omniverse/run_display_parity.ps1")
    set_tests_properties(DisplayParity.WorldLobby PROPERTIES
        TIMEOUT 360 LABELS "omniverse;integration;parity;requires_gpu"
        RESOURCE_LOCK "pyxis_gpu")

    # Curated golden fixtures (static, camera at /World/Camera) covering the
    # delegate ingest paths. 256x256 to match the golden config; the comparison
    # is delegate-vs-standalone, so the resolution only needs to agree.
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
        set(_fx_usd "${CMAKE_SOURCE_DIR}/tests/golden-tests-data/${_fx}/fixture.usda")
        if(EXISTS "${_fx_usd}")
            add_test(NAME "Parity.${_fx}"
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                        -File "${CMAKE_SOURCE_DIR}/_tools/omniverse/run_parity.ps1"
                        -Scene "${_fx_usd}" -Camera "/World/Camera"
                        -Width 256 -Height 256 -Name "${_fx}")
            set_tests_properties("Parity.${_fx}" PROPERTIES
                TIMEOUT 180 LABELS "omniverse;integration;parity;requires_gpu"
                RESOURCE_LOCK "pyxis_gpu")
        endif()
    endforeach()
endif()

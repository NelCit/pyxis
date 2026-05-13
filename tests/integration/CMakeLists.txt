# Pyxis integration tests — CMake-driven, spawn pyxis.exe and check
# its outputs. Plan §35 / §41 M2.
#
# Today this directory ships a single test: the M2 byte-identical EXR
# contract. Future contributors add image-diff tests here as
# milestones bring more determinism (Moana subset M8a, full pass
# M10's nightly regression, etc.). The full Python regression harness
# (`pyxis_regression`) lands at M10 per plan §3 and sits next to this
# one as a separate target.

# §41 M2 exit: "pyxis --headless --config tests/fixtures/headless_
# triangle.json writes a deterministic EXR; rerun gives byte-identical
# output." The cmake script runs pyxis twice and `cmake -E
# compare_files` the two outputs.
add_test(
    NAME    M2.HeadlessByteEqualEXR
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/fixtures/headless_triangle.json
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m2_byte_equal
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m2_byte_equal.cmake
)
set_tests_properties(M2.HeadlessByteEqualEXR PROPERTIES
    LABELS    "integration;m2"
    TIMEOUT   60   # M2 fixture is 640x360, two runs; finishes in seconds.
)

# §41 M4 exit: "the tiny USD renders identically in standalone Pyxis
# (both adapters); regression image diff Hydra-vs-USD-direct = 0."
# Drives the §29.4.a-resolved bundled default.usd through both
# adapters and asserts byte-equal EXR output.
add_test(
    NAME    M4.AdapterParityByteEqualEXR
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE=${CMAKE_SOURCE_DIR}/tests/fixtures/headless_triangle.json
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m4_adapter_parity
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m4_adapter_parity.cmake
)
set_tests_properties(M4.AdapterParityByteEqualEXR PROPERTIES
    LABELS    "integration;m4"
    TIMEOUT   90   # Two USD-loading runs (~2-3 s each on a warm cache).
)

# §41 M5 — exercises the GPU material path with three distinct
# UsdPreviewSurface materials. Verifies (a) determinism across re-runs
# and (b) the rendered EXR DIFFERS from the bundled default-scene
# render, proving the closesthit actually picks up per-instance
# materials. See m5_material_path.cmake header for the full failure-
# mode catalogue.
add_test(
    NAME    M5.MaterialPathRendersDistinctColors
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/headless_triangle.json
            -DMATERIAL_SCENE=${CMAKE_SOURCE_DIR}/tests/fixtures/m5_three_materials.usd
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m5_material_path
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m5_material_path.cmake
)
set_tests_properties(M5.MaterialPathRendersDistinctColors PROPERTIES
    LABELS    "integration;m5"
    TIMEOUT   120  # Three USD-loading runs.
)

# §41 M6 — exercises the UsdGeomPointInstancer expansion path. The
# fixture authors a single instancer with 100 instances of one cube
# prototype; a working M6 emits 100 AppendInstance calls all
# referencing one BLAS (per §15 sharing rule). See
# m6_native_instancing.cmake header for the full failure-mode
# catalogue.
add_test(
    NAME    M6.NativeInstancingExpandsPointInstancer
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/headless_triangle.json
            -DINSTANCED_SCENE=${CMAKE_SOURCE_DIR}/tests/fixtures/m6_instanced_grid.usd
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m6_native_instancing
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m6_native_instancing.cmake
)
set_tests_properties(M6.NativeInstancingExpandsPointInstancer PROPERTIES
    LABELS    "integration;m6"
    TIMEOUT   120  # Three USD-loading runs.
)

# §41 M7 — exercises the full M7-simple lighting path (UsdLux*
# translation → light buffer upload → closesthit NdotL Lambert +
# Dome ambient). The lit fixture authors a single triangle facing
# the camera with a Sun (Distant -Z) + Sky (Dome) — its EXR must
# differ from the unlit twin (same triangle, no lights). See
# m7_lighting.cmake header for the full failure-mode catalogue.
add_test(
    NAME    M7.LightingMovesPixels
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/headless_triangle.json
            -DLIT_SCENE=${CMAKE_SOURCE_DIR}/tests/fixtures/m7_lit_scene.usd
            -DUNLIT_SCENE=${CMAKE_SOURCE_DIR}/tests/fixtures/m7_lit_scene_unlit.usd
            -DCOLOR_DOME_SCENE=${CMAKE_SOURCE_DIR}/tests/fixtures/m7_color_only_dome.usd
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m7_lighting
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m7_lighting.cmake
)
set_tests_properties(M7.LightingMovesPixels PROPERTIES
    LABELS    "integration;m7"
    TIMEOUT   180  # Four USD-loading runs (lit ×2 + unlit + color-dome).
)

# §41 M7 follow-up — exercises the AOV inspector path end-to-end. One
# headless run with --save-aov all dumps every raw AOV (color HDR,
# normal, depth, primId, materialId, baseColor, worldPos) into
# `<prefix>_<aov>.exr` files; the test asserts (a) every expected
# file exists and is non-empty, (b) AOVs are byte-distinct from each
# other (proves the writes are independent — a regression that wired
# every UAV to the same texture would silently produce identical
# files), and (c) a re-run of one AOV is byte-deterministic. The lit
# fixture is the same M7 scene used by LightingMovesPixels so all
# seven AOVs have non-trivial content (a hit triangle + sun + dome).
add_test(
    NAME    M7.AovInspectorWritesAllAovs
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/headless_triangle.json
            # m6 fixture (100 instances of 1 prototype mesh, 1 material) gives
            # the AOV inspector enough per-pixel variation that primId
            # genuinely differs from materialId — m7_lit_scene.usd has 1:1
            # entity counts and the two integer AOVs collapse to identical
            # content there, defeating the pairwise byte-distinctness check.
            -DAOV_SCENE=${CMAKE_SOURCE_DIR}/tests/fixtures/m6_instanced_grid.usd
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m7_aov_inspector
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m7_aov_inspector.cmake
)
set_tests_properties(M7.AovInspectorWritesAllAovs PROPERTIES
    LABELS    "integration;m7"
    TIMEOUT   120  # Two USD-loading runs (one --save-aov all + one
                   # determinism re-run).
)

# §41 M8a — World Lobby smoke. The framework test for "renderer
# survives an Omniverse-Collected production-style scene + emits a
# plausible EXR." Differs from M2..M7 fixtures: the scene is NOT
# bundled in the repo (3.4 GB compressed → .gitignore'd at
# resources/scenes/world_lobby/). Conditionally registered: if the
# entry-point .usd doesn't exist at config time, we emit a STATUS
# line and skip — no test is added — so a clean clone still
# `ctest`s green without the user setup.
#
# Setup steps the user must run ONCE (before reconfigure):
#   1. Download Collected_World_Lobby.zip (Omniverse Marketplace).
#   2. Extract to <repo>/resources/scenes/world_lobby/ (excluding
#      the .usdz duplicate and .thumbs/ saves ~1.7 GB).
#   3. Re-run `cmake -S . -B build/dev` to pick up the test.
#
# Override path: pass -DM8_SCENE_PATH=<absolute path to .usd> to
# point at a different location (e.g. a sibling directory outside
# the repo). The path is resolved at configure time, not at test
# time, so a missing scene at configure time silently de-registers.
set(_m8_default_scene
    "${CMAKE_SOURCE_DIR}/resources/scenes/world_lobby/World_Lobby.usd")
set(M8_SCENE_PATH "${_m8_default_scene}" CACHE FILEPATH
    "Path to the M8a stress-test scene .usd (default: World Lobby under \
resources/scenes/world_lobby/). Test is skipped if the file doesn't exist.")
if(EXISTS "${M8_SCENE_PATH}")
    add_test(
        NAME    M8a.WorldLobbySmoke
        COMMAND ${CMAKE_COMMAND}
                -DPYXIS_EXE=$<TARGET_FILE:pyxis>
                -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m8_world_lobby.json
                -DSCENE_PATH=${M8_SCENE_PATH}
                -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m8_world_lobby
                -P ${CMAKE_SOURCE_DIR}/tests/integration/m8_world_lobby_smoke.cmake
    )
    set_tests_properties(M8a.WorldLobbySmoke PROPERTIES
        LABELS    "integration;m8;m8a;requires_external_scene"
        TIMEOUT   600  # First run can take 60-90s on a cold cache (1256
                       # textures via synchronous stb_image decode); 10
                       # min ceiling tolerates a slow first build pass.
    )
    message(STATUS "M8a.WorldLobbySmoke registered (scene: ${M8_SCENE_PATH})")

    # §41 M10 — Python image-regression harness.
    #
    # Distinct from M8a.WorldLobbySmoke (which only asserts "did not
    # crash + non-black"). This one diffs the produced EXR against a
    # checked-in baseline (tests/regression/m8_world_lobby/baseline.exr)
    # and fails on RMSE / MAE / max-abs-delta drift past the per-
    # fixture tolerances. The harness lives under _tools/ rather than
    # a CMake script because it needs numpy + OpenEXR for the pixel-
    # diff, plus jq-style JSON config parsing.
    #
    # Python interpreter discovered via find_package(Python3); a CI
    # without Python3 + the _tools/requirements.txt installed will
    # skip silently (Python3_FOUND = FALSE).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(
            NAME    M10.WorldLobbyRegression
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/_tools/run_regression.py
                    --pyxis $<TARGET_FILE:pyxis>
                    --fixture ${CMAKE_SOURCE_DIR}/tests/regression/m8_world_lobby
                    --output-dir ${CMAKE_BINARY_DIR}/tests/regression/m8_world_lobby
                    --repo-root ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(M10.WorldLobbyRegression PROPERTIES
            LABELS    "regression;m10;requires_external_scene;requires_python"
            TIMEOUT   600  # Same cold-cache budget as M8a.WorldLobbySmoke.
        )
        message(STATUS "M10.WorldLobbyRegression registered (baseline: "
                "${CMAKE_SOURCE_DIR}/tests/regression/m8_world_lobby/baseline.exr)")
    else()
        message(STATUS
            "M10.WorldLobbyRegression skipped — Python3 interpreter not found. "
            "Install Python 3.10+ and `pip install -r _tools/requirements.txt`.")
    endif()
else()
    message(STATUS
        "M8a.WorldLobbySmoke skipped — no scene at ${M8_SCENE_PATH}. "
        "Extract Collected_World_Lobby.zip into resources/scenes/world_lobby/ "
        "and reconfigure to enable.")
endif()

# OpenPBR Shader Playground smoke. Companion to M8a.WorldLobbySmoke for
# §11 OpenPBR coverage: every material in the playground is authored
# in the OpenPBR shader graph the renderer's closesthit is canonicalised
# on. Same conditional-registration pattern: scene path is a cache
# variable; if the .usda doesn't exist at configure time, the test
# silently de-registers so a clean clone still `ctest`s green.
#
# Setup steps the user must run ONCE (before reconfigure):
#   1. Download OpenPBRShaderPlayground-1.0.zip (SIGGRAPH 2024 sample).
#   2. Extract to <repo>/resources/scenes/openpbr_playground/ so the
#      entry-point lands at
#      .../openpbr_playground/ShdrPlygrnd/ShdrPlygrnd_OpenPBR.usda.
#   3. Re-run `cmake -S . -B build/dev` to pick up the test.
#
# Override path: pass -DOPENPBR_PLAYGROUND_SCENE_PATH=<absolute .usda>
# to point at an already-extracted copy outside the repo (e.g. directly
# at ~/Downloads/OpenPBRShaderPlayground-1.0/ShdrPlygrnd/
# ShdrPlygrnd_OpenPBR.usda). Resolved at configure time, not at test
# time, so a missing scene at configure time silently de-registers.
set(_openpbr_playground_default_scene
    "${CMAKE_SOURCE_DIR}/resources/scenes/openpbr_playground/ShdrPlygrnd/ShdrPlygrnd_OpenPBR.usda")
set(OPENPBR_PLAYGROUND_SCENE_PATH "${_openpbr_playground_default_scene}" CACHE FILEPATH
    "Path to the OpenPBR Shader Playground entry-point .usda (default: \
ShdrPlygrnd_OpenPBR.usda under resources/scenes/openpbr_playground/). \
Test is skipped if the file doesn't exist.")
if(EXISTS "${OPENPBR_PLAYGROUND_SCENE_PATH}")
    add_test(
        NAME    OpenPbrPlayground.Smoke
        COMMAND ${CMAKE_COMMAND}
                -DPYXIS_EXE=$<TARGET_FILE:pyxis>
                -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/openpbr_playground.json
                -DSCENE_PATH=${OPENPBR_PLAYGROUND_SCENE_PATH}
                -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/openpbr_playground
                -P ${CMAKE_SOURCE_DIR}/tests/integration/openpbr_playground_smoke.cmake
    )
    set_tests_properties(OpenPbrPlayground.Smoke PROPERTIES
        LABELS    "integration;openpbr;requires_external_scene"
        TIMEOUT   600  # ~50 textured materials + 47 USD layers — same
                       # cold-cache budget as M8a.WorldLobbySmoke.
    )
    message(STATUS "OpenPbrPlayground.Smoke registered (scene: ${OPENPBR_PLAYGROUND_SCENE_PATH})")
else()
    message(STATUS
        "OpenPbrPlayground.Smoke skipped — no scene at ${OPENPBR_PLAYGROUND_SCENE_PATH}. "
        "Extract OpenPBRShaderPlayground-1.0.zip into resources/scenes/openpbr_playground/ "
        "(or pass -DOPENPBR_PLAYGROUND_SCENE_PATH=<absolute path>) and reconfigure to enable.")
endif()

# §41 M13 / V2.A.3 — analytic prim + curves + points framework test.
# Loads a self-contained .usda fixture (one of each prim type) headless;
# proves StageWalker's geom-type dispatch + AnalyticGeom tessellators
# round-trip into the existing MeshDesc / BLAS / TLAS pipeline. Fixture
# ships in-tree (no external scene download), so unlike M8a.WorldLobby
# this always registers.
add_test(
    NAME    M13.AnalyticGeomSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m13_geom.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m13_geom.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m13_geom
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m13_geom_smoke.cmake
)
set_tests_properties(M13.AnalyticGeomSmoke PROPERTIES
    LABELS    "integration;m13"
    TIMEOUT   120  # In-tree fixture is ~7 prims; budget is generous.
)

# §41 M14 / V2.A.6 + V2.A.17 — advanced xformOps + prim state filter.
# Authored fixture exercises xformOp:transform (full 4×4),
# xformOp:rotateXYZ (Maya-style chain), xformOp:orient (quaternion),
# and `active=false` (must NOT render). In-tree fixture, always
# registered.
add_test(
    NAME    M14.XformStateSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m14_xform_state.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m14_xform_state.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m14_xform_state
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m14_xform_state_smoke.cmake
)
set_tests_properties(M14.XformStateSmoke PROPERTIES
    LABELS    "integration;m14"
    TIMEOUT   120
)

# §41 M14c / V2.A.31 — bounding-box hints. Authored `extent` on a Mesh
# triggers the StageWalker scene-bounds aggregate log line.
add_test(
    NAME    M14.ExtentsSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m14c_extents.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m14c_extents.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m14c_extents
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m14c_extents_smoke.cmake
)
set_tests_properties(M14.ExtentsSmoke PROPERTIES
    LABELS    "integration;m14"
    TIMEOUT   120
)

# §41 M15 / V2.A.5 — UsdVolVolume detect/warn/skip stub. Full OpenVDB
# rendering is a follow-up milestone; this proves a volume-bearing
# scene doesn't crash and that the rest of the geometry still renders.
add_test(
    NAME    M15.VolumeStubSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m15_volume_stub.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m15_volume_stub.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m15_volume_stub
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m15_volume_stub_smoke.cmake
)
set_tests_properties(M15.VolumeStubSmoke PROPERTIES
    LABELS    "integration;m15"
    TIMEOUT   120
)

# §41 M16 / V2.A.10 + V2.A.11 — texture memory reporting smoke. Reuses
# the M13 geom fixture (no UDIM, exercises the texture cache).
add_test(
    NAME    M16.TextureReportSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m13_geom.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m13_geom.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m16_texture_report
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m16_texture_report_smoke.cmake
)
set_tests_properties(M16.TextureReportSmoke PROPERTIES
    LABELS    "integration;m16"
    TIMEOUT   120
)

# §41 M17 / V2.A.12 — ArResolver visibility smoke.
add_test(
    NAME    M17.ArResolverSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m14c_extents.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m14c_extents.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m17_ar
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m17_ar_resolver_smoke.cmake
)
set_tests_properties(M17.ArResolverSmoke PROPERTIES
    LABELS    "integration;m17"
    TIMEOUT   120
)

# §41 M18 / V2.A.8 + V2.A.18 — material translation health log smoke.
# Uses the M5 three-materials fixture (all UsdPreviewSurface).
add_test(
    NAME    M18.MaterialHealthSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m18_material_health.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m5_three_materials.usd
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m18_material_health
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m18_material_health_smoke.cmake
)
set_tests_properties(M18.MaterialHealthSmoke PROPERTIES
    LABELS    "integration;m18"
    TIMEOUT   120
)

# §41 M19 / V2.A.4 + V2.A.13 — --frame N stub smoke.
add_test(
    NAME    M19.FrameFlagSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m14c_extents.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m14c_extents.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m19_frame
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m19_frame_flag_smoke.cmake
)
set_tests_properties(M19.FrameFlagSmoke PROPERTIES
    LABELS    "integration;m19"
    TIMEOUT   120
)

# §41 M20 / V2.A.4 — NURBS + Skel detect/warn/skip stub.
add_test(
    NAME    M20.NurbsSkelStubSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m20_nurbs_skel.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m20_nurbs_skel.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m20_nurbs_skel
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m20_nurbs_skel_smoke.cmake
)
set_tests_properties(M20.NurbsSkelStubSmoke PROPERTIES
    LABELS    "integration;m20"
    TIMEOUT   120
)

# §41 M21 / V2.A.27 — UsdRender schema detection stub.
add_test(
    NAME    M21.UsdRenderStubSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m21_usdrender.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m21_usdrender.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/m21_usdrender
            -P ${CMAKE_SOURCE_DIR}/tests/integration/m21_usdrender_smoke.cmake
)
set_tests_properties(M21.UsdRenderStubSmoke PROPERTIES
    LABELS    "integration;m21"
    TIMEOUT   120
)

# V2.A.15 full impl — UsdStage::OpenMasked smoke.
add_test(
    NAME    V2A15.PopulationMaskSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/m13_geom.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/m13_geom.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/va15_mask
            -P ${CMAKE_SOURCE_DIR}/tests/integration/va15_population_mask_smoke.cmake
)
set_tests_properties(V2A15.PopulationMaskSmoke PROPERTIES
    LABELS    "integration;v2a15"
    TIMEOUT   120
)

# V2.A.13 — UsdTimeCode propagation smoke.
add_test(
    NAME    V2A13.TimeCodeSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/va13_animated_xform.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/va13_animated_xform.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/va13_timecode
            -P ${CMAKE_SOURCE_DIR}/tests/integration/va13_timecode_smoke.cmake
)
set_tests_properties(V2A13.TimeCodeSmoke PROPERTIES
    LABELS    "integration;v2a13"
    TIMEOUT   120
)

# V2.A.4 — UsdSkel multi-frame headless smoke.
add_test(
    NAME    V2A4.SkelMultiFrameSmoke
    COMMAND ${CMAKE_COMMAND}
            -DPYXIS_EXE=$<TARGET_FILE:pyxis>
            -DFIXTURE_CONFIG=${CMAKE_SOURCE_DIR}/tests/fixtures/va4_skel_animated.json
            -DSCENE_PATH=${CMAKE_SOURCE_DIR}/tests/fixtures/va4_skel_animated.usda
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/tests/integration/va4_skel
            -P ${CMAKE_SOURCE_DIR}/tests/integration/va4_skel_multiframe_smoke.cmake
)
set_tests_properties(V2A4.SkelMultiFrameSmoke PROPERTIES
    LABELS    "integration;v2a4"
    TIMEOUT   240
)

# §41 M28 / V2.A.28 — USD loading audit. Runs the pure-Python audit
# tool against an in-tree fixture (one of each STUB type + standard
# LOADED types); the tool returns exit 0 because every authored type
# is either LOADED or STUB. Future scenes that author UNHANDLED prim
# types fail the audit + this test.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(
        NAME    M28.UsdLoadingAuditSelfTest
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/_tools/usd_loading_audit.py
                ${CMAKE_SOURCE_DIR}/tests/fixtures/m21_usdrender.usda
    )
    set_tests_properties(M28.UsdLoadingAuditSelfTest PROPERTIES
        LABELS    "integration;m28;requires_python"
        TIMEOUT   30
    )

    # Python-side unit tests for the audit tool's classify + parser.
    add_test(
        NAME    M28.UsdLoadingAuditUnit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/_tools/test_usd_loading_audit.py
    )
    set_tests_properties(M28.UsdLoadingAuditUnit PROPERTIES
        LABELS    "integration;m28;requires_python"
        TIMEOUT   30
    )

    # ---- Golden tests (pixel-equal regression vs checked-in baseline) ----
    # One ctest entry per subdir under tests/golden-tests-data/. Each
    # runs _tools/run_goldens.py with --filter <name> so failures
    # report on the specific golden, not the whole suite.
    file(GLOB _golden_dirs RELATIVE ${CMAKE_SOURCE_DIR}/tests/golden-tests-data
         ${CMAKE_SOURCE_DIR}/tests/golden-tests-data/*)
    foreach(_golden_dir IN LISTS _golden_dirs)
        set(_golden_full ${CMAKE_SOURCE_DIR}/tests/golden-tests-data/${_golden_dir})
        if(NOT _golden_dir STREQUAL "_shared"
                AND NOT _golden_dir STREQUAL "README.md"
                AND IS_DIRECTORY ${_golden_full}
                AND EXISTS ${_golden_full}/fixture.usda)
            add_test(
                NAME    Golden.${_golden_dir}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/_tools/run_goldens.py
                        --pyxis $<TARGET_FILE:pyxis>
                        --output-root ${CMAKE_BINARY_DIR}/tests/golden-tests-produced
                        --filter ${_golden_dir}
            )
            set_tests_properties(Golden.${_golden_dir} PROPERTIES
                LABELS  "integration;golden;requires_python"
                TIMEOUT 120
            )
        endif()
    endforeach()
endif()

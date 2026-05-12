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

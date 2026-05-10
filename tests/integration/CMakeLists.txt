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

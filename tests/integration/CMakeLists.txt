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

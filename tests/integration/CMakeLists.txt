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

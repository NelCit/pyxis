# Pyxis M2 byte-equal-EXR test.
#
# Plan §41 M2 exit: "pyxis --headless --config tests/fixtures/headless_
# triangle.json writes a deterministic EXR; rerun gives byte-identical
# output." Driven from CTest via `cmake -P`; takes the pyxis executable
# path, the fixture path, and a working dir as -D arguments so the
# CMake build script doesn't have to know about CTest's runtime.
#
# Failure modes (each surfaces as a CTest FAIL with a precise reason):
#   - pyxis run A or B exited non-zero
#   - the two EXRs differ byte-for-byte (§33.7 contract violation)
#   - the EXR file is empty / missing
#
# We deliberately compare the EXR file as a whole (header + payload)
# because tinyexr's ZIP compression is deterministic for a fixed input
# and library version, so any byte difference is a real determinism
# regression worth flagging.

cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m2_byte_equal: -DPYXIS_EXE=<path-to-pyxis.exe> required")
endif()
if(NOT DEFINED FIXTURE)
    message(FATAL_ERROR "m2_byte_equal: -DFIXTURE=<path-to-headless_triangle.json> required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m2_byte_equal: -DOUTPUT_DIR=<work-dir> required")
endif()

# Fresh working dir each run so a previous failure doesn't poison the
# byte-equal compare.
file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(_runA_exr "${OUTPUT_DIR}/run_a.exr")
set(_runB_exr "${OUTPUT_DIR}/run_b.exr")

# Run A.
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE}" --output "${_runA_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcA
    OUTPUT_VARIABLE   _outA
    ERROR_VARIABLE    _outA)
if(NOT _rcA EQUAL 0)
    message(FATAL_ERROR "m2_byte_equal: pyxis run A failed (rc=${_rcA})\n${_outA}")
endif()
if(NOT EXISTS "${_runA_exr}")
    message(FATAL_ERROR "m2_byte_equal: run A finished but produced no EXR at ${_runA_exr}")
endif()

# Run B (same args, different output path).
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE}" --output "${_runB_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcB
    OUTPUT_VARIABLE   _outB
    ERROR_VARIABLE    _outB)
if(NOT _rcB EQUAL 0)
    message(FATAL_ERROR "m2_byte_equal: pyxis run B failed (rc=${_rcB})\n${_outB}")
endif()
if(NOT EXISTS "${_runB_exr}")
    message(FATAL_ERROR "m2_byte_equal: run B finished but produced no EXR at ${_runB_exr}")
endif()

# Byte-for-byte compare. CMake's `compare_files` exits 0 on match,
# non-zero on differ, and that's exactly the M2 contract.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_runA_exr}" "${_runB_exr}"
    RESULT_VARIABLE _rcCmp)
if(NOT _rcCmp EQUAL 0)
    file(SIZE "${_runA_exr}" _szA)
    file(SIZE "${_runB_exr}" _szB)
    message(FATAL_ERROR
        "m2_byte_equal: EXR mismatch (§33.7 byte-identical contract violated).\n"
        "  ${_runA_exr}  (${_szA} bytes)\n"
        "  ${_runB_exr}  (${_szB} bytes)\n"
        "Re-runs of the same fixture produced different output.")
endif()

file(SIZE "${_runA_exr}" _szA)
message(STATUS "m2_byte_equal OK: pyxis re-runs produced byte-identical ${_szA}-byte EXR")

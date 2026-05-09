# Pyxis M4 cross-adapter byte-equal-EXR test (§25.O.3 P0 invariant).
#
# Plan §41 M4 exit: "the tiny USD renders identically in standalone
# Pyxis (both adapters) ... Regression image diff Hydra-vs-USD-direct
# = 0." Drives both `--ingest hydra` and `--ingest usd_direct`
# against the same fixture (the bundled §29.4.a default-scene chain
# resolves to <bin>/Resources/scenes/default.usd) and asserts the
# two EXRs are byte-identical.
#
# Failure modes (each surfaces as a CTest FAIL with a precise reason):
#   - either ingest run exited non-zero
#   - the two EXRs differ byte-for-byte (§25.O.3 P0 violation)
#   - the EXR file is implausibly small (header-only stub)

cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m4_adapter_parity: -DPYXIS_EXE=<path-to-pyxis.exe> required")
endif()
if(NOT DEFINED FIXTURE)
    message(FATAL_ERROR "m4_adapter_parity: -DFIXTURE=<path-to-config.json> required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m4_adapter_parity: -DOUTPUT_DIR=<work-dir> required")
endif()

# Fresh working dir each run so a previous failure doesn't poison the
# byte-equal compare.
file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(_hydra_exr "${OUTPUT_DIR}/hydra.exr")
set(_direct_exr "${OUTPUT_DIR}/usd_direct.exr")

# Hydra adapter run.
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE}"
                            --ingest hydra --output "${_hydra_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcHydra
    OUTPUT_VARIABLE   _outHydra
    ERROR_VARIABLE    _outHydra)
if(NOT _rcHydra EQUAL 0)
    message(FATAL_ERROR "m4_adapter_parity: --ingest hydra run failed (rc=${_rcHydra})\n${_outHydra}")
endif()
if(NOT EXISTS "${_hydra_exr}")
    message(FATAL_ERROR "m4_adapter_parity: hydra run produced no EXR at ${_hydra_exr}")
endif()

# USD-direct adapter run.
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE}"
                            --ingest usd_direct --output "${_direct_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcDirect
    OUTPUT_VARIABLE   _outDirect
    ERROR_VARIABLE    _outDirect)
if(NOT _rcDirect EQUAL 0)
    message(FATAL_ERROR
        "m4_adapter_parity: --ingest usd_direct run failed (rc=${_rcDirect})\n${_outDirect}")
endif()
if(NOT EXISTS "${_direct_exr}")
    message(FATAL_ERROR "m4_adapter_parity: usd_direct run produced no EXR at ${_direct_exr}")
endif()

# Byte-for-byte compare. The §25.O.3 contract is RMSE = 0 — for
# matched-version tinyexr ZIP-compressed EXRs that means literal byte
# identity.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_hydra_exr}" "${_direct_exr}"
    RESULT_VARIABLE _rcCmp)
if(NOT _rcCmp EQUAL 0)
    file(SIZE "${_hydra_exr}" _szHydra)
    file(SIZE "${_direct_exr}" _szDirect)
    message(FATAL_ERROR
        "m4_adapter_parity: cross-adapter EXR mismatch (§25.O.3 P0 invariant violated).\n"
        "  ${_hydra_exr}      (${_szHydra} bytes)\n"
        "  ${_direct_exr}     (${_szDirect} bytes)\n"
        "Both adapters MUST emit GpuScene mutations in identical order with "
        "identical data given the same .usd input. Most likely culprits: "
        "SdfPath sort order divergence, mesh primvar extraction differences, "
        "or camera matrix conversion drift between StageWalker (USD-direct) "
        "and HdPyxisMesh/Camera::Sync (Hydra).")
endif()

file(SIZE "${_hydra_exr}" _szHydra)

# Sanity floor: a 640x360 ZIP-compressed RGBA EXR should be on the
# order of tens of KB. If both runs wrote a tiny / empty file the
# byte-equal compare above would trivially pass — reject anything
# implausibly small.
set(_min_plausible_bytes 1024)
if(_szHydra LESS _min_plausible_bytes)
    message(FATAL_ERROR
        "m4_adapter_parity: EXR is implausibly small (${_szHydra} bytes < ${_min_plausible_bytes}); "
        "byte-equal compare passed but the file is almost certainly empty / a header-only stub.")
endif()

message(STATUS "m4_adapter_parity OK: hydra + usd_direct produced byte-identical ${_szHydra}-byte EXR")

# Pyxis M6 native-instancing integration test.
#
# Drives the §29.4.a-resolvable m6_instanced_grid.usd fixture through
# the USD-direct adapter. The fixture authors a single
# UsdGeomPointInstancer with one cube prototype + 100 instances in a
# 10x10 grid; StageWalker's M6 PointInstancer expansion is the only
# code path that can render this scene as anything other than a flat
# grey miss-shader image.
#
# The test:
#   1. Renders the fixture headless via --ingest usd_direct.
#   2. Renders it again to a separate output (determinism check —
#      same input must yield byte-identical EXR per §33.7).
#   3. Renders the bundled default.usd (grey-ground baseline).
#   4. Asserts the instanced-grid EXR DIFFERS from the default-scene
#      EXR. If StageWalker silently dropped the PointInstancer (e.g.
#      a regression in EmitPointInstancer's prototype resolution or
#      a breakage in the §15 BLAS sharing path), the grid wouldn't
#      render and both EXRs would collapse to the grey baseline.
#
# Failure modes (each surfaces as a CTest FAIL with a precise reason):
#   - any pyxis run exited non-zero
#   - either EXR is missing or implausibly small
#   - the two instanced-grid runs disagree (determinism violation)
#   - the instanced-grid EXR is byte-identical to the default-scene
#     EXR (instancing path silently broken)

cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m6_native_instancing: -DPYXIS_EXE=<path-to-pyxis.exe> required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m6_native_instancing: -DFIXTURE_CONFIG=<headless-config.json> required")
endif()
if(NOT DEFINED INSTANCED_SCENE)
    message(FATAL_ERROR "m6_native_instancing: -DINSTANCED_SCENE=<m6_instanced_grid.usd> required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m6_native_instancing: -DOUTPUT_DIR=<work-dir> required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(_runA_exr    "${OUTPUT_DIR}/instanced_a.exr")
set(_runB_exr    "${OUTPUT_DIR}/instanced_b.exr")
set(_default_exr "${OUTPUT_DIR}/default.exr")

# ----- Run A: instanced fixture -------------------------------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${INSTANCED_SCENE}"
                            --ingest usd_direct --output "${_runA_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcA
    OUTPUT_VARIABLE   _outA
    ERROR_VARIABLE    _outA)
if(NOT _rcA EQUAL 0)
    message(FATAL_ERROR "m6_native_instancing: run A failed (rc=${_rcA})\n${_outA}")
endif()
if(NOT EXISTS "${_runA_exr}")
    message(FATAL_ERROR "m6_native_instancing: run A produced no EXR at ${_runA_exr}")
endif()

# ----- Audit closeout: assert StageWalker actually expanded the 100
# instances. The previous "differs from default scene" check would
# also pass if EmitPointInstancer crashed after emitting a single
# instance — a partial regression slipping through. Grep the
# StageWalker info-log line emitted at WalkFile completion for the
# expected counters.
#
# Expected log line shape (single line, see StageWalker::WalkFile):
#   "StageWalker: <path> walked — 1 meshes, 100 instances (1 instancers), ..."
string(REGEX MATCH "StageWalker:[^\n]*1 meshes, 100 instances \\(1 instancers\\)" _statsMatch "${_outA}")
if(NOT _statsMatch)
    message(FATAL_ERROR
        "m6_native_instancing: StageWalker stats line missing or wrong shape.\n"
        "Expected '1 meshes, 100 instances (1 instancers)' (the m6_instanced_grid.usd "
        "fixture authors a single PointInstancer with 100 cubes of one prototype).\n"
        "Actual stdout:\n${_outA}")
endif()

# ----- Run B: same fixture (determinism re-run) ---------------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${INSTANCED_SCENE}"
                            --ingest usd_direct --output "${_runB_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcB
    OUTPUT_VARIABLE   _outB
    ERROR_VARIABLE    _outB)
if(NOT _rcB EQUAL 0)
    message(FATAL_ERROR "m6_native_instancing: run B failed (rc=${_rcB})\n${_outB}")
endif()

# ----- Run default: bundled default.usd via §29.4.a chain -----------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --ingest usd_direct --output "${_default_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcDefault
    OUTPUT_VARIABLE   _outDefault
    ERROR_VARIABLE    _outDefault)
if(NOT _rcDefault EQUAL 0)
    message(FATAL_ERROR "m6_native_instancing: default-scene run failed (rc=${_rcDefault})\n${_outDefault}")
endif()

# ----- Sanity floor: EXR file size ----------------------------------
file(SIZE "${_runA_exr}" _szA)
set(_min_plausible_bytes 1024)
if(_szA LESS _min_plausible_bytes)
    message(FATAL_ERROR
        "m6_native_instancing: instanced EXR is implausibly small (${_szA} bytes < "
        "${_min_plausible_bytes}); render likely produced an empty / header-only file.")
endif()

# ----- Determinism: A must equal B ----------------------------------
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_runA_exr}" "${_runB_exr}"
    RESULT_VARIABLE _rcSame)
if(NOT _rcSame EQUAL 0)
    file(SIZE "${_runB_exr}" _szB)
    message(FATAL_ERROR
        "m6_native_instancing: re-run produced different EXR (§33.7 determinism violated).\n"
        "  ${_runA_exr}  (${_szA} bytes)\n"
        "  ${_runB_exr}  (${_szB} bytes)")
endif()

# ----- Native-instancing path moves pixels: A must DIFFER from default
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_runA_exr}" "${_default_exr}"
    RESULT_VARIABLE _rcSameAsDefault)
if(_rcSameAsDefault EQUAL 0)
    file(SIZE "${_default_exr}" _szDefault)
    message(FATAL_ERROR
        "m6_native_instancing: instanced EXR is byte-identical to the default-scene EXR.\n"
        "  ${_runA_exr}        (${_szA} bytes)\n"
        "  ${_default_exr}     (${_szDefault} bytes)\n"
        "Both renders collapsed to the same image, which means StageWalker's "
        "UsdGeomPointInstancer expansion is silently broken — the 100-instance "
        "grid never materialised in the TLAS. Most likely culprits: "
        "EmitPointInstancer dropped on a regression, prototype mesh registration "
        "failed, the §15 BLAS sharing pathway broke, or the consumed-prototypes "
        "set wrongly suppressed the prototype mesh entirely.")
endif()

message(STATUS
    "m6_native_instancing OK: 100-instance grid EXR (${_szA} bytes) is deterministic "
    "across re-runs and visibly differs from the default-scene render.")

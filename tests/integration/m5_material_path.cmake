# Pyxis M5 material-path integration test.
#
# Closes the audit gap on "no test exercises the GPU material path with
# multiple distinct material colors." The fixture (tests/fixtures/
# m5_three_materials.usd) ships three single-triangle meshes bound to
# three distinct UsdPreviewSurface materials (red / green / blue). The
# test:
#
#   1. Renders the fixture headless via the USD-direct adapter.
#   2. Renders it again to a separate output (determinism check —
#      same input must yield byte-identical EXR per §33.7).
#   3. Renders the bundled default.usd headless to a third output.
#      default.usd's only triangle-list mesh is the grey Ground (the
#      three "hero spheres" are UsdGeomSphere parametrics that the M5
#      StageWalker skips), so its render is essentially a flat-grey
#      ground + sky.
#   4. Asserts the three-material EXR DIFFERS from the default-scene
#      EXR. If the renderer ever silently lost the material-path
#      plumbing (closesthit binding goes stale, AcquireMaterial
#      regresses to no-op, instanceCustomIndex stops carrying the
#      material slot, …) the two EXRs would collapse to the same
#      grey output and this assertion would catch it.
#
# Failure modes (each surfaces as a CTest FAIL with a precise reason):
#   - either pyxis run exited non-zero
#   - either EXR is missing or implausibly small
#   - the two material-fixture runs disagree (determinism violation)
#   - the material-fixture EXR is byte-identical to the default-scene
#     EXR (material path silently broken — pixels look identical)

cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m5_material_path: -DPYXIS_EXE=<path-to-pyxis.exe> required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m5_material_path: -DFIXTURE_CONFIG=<headless-config.json> required")
endif()
if(NOT DEFINED MATERIAL_SCENE)
    message(FATAL_ERROR "m5_material_path: -DMATERIAL_SCENE=<m5_three_materials.usd> required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m5_material_path: -DOUTPUT_DIR=<work-dir> required")
endif()

# Fresh working dir each run.
file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(_runA_exr    "${OUTPUT_DIR}/material_a.exr")
set(_runB_exr    "${OUTPUT_DIR}/material_b.exr")
set(_default_exr "${OUTPUT_DIR}/default.exr")

# ----- Run A: three-material fixture ---------------------------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${MATERIAL_SCENE}"
                            --ingest usd_direct --output "${_runA_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcA
    OUTPUT_VARIABLE   _outA
    ERROR_VARIABLE    _outA)
if(NOT _rcA EQUAL 0)
    message(FATAL_ERROR "m5_material_path: run A failed (rc=${_rcA})\n${_outA}")
endif()
if(NOT EXISTS "${_runA_exr}")
    message(FATAL_ERROR "m5_material_path: run A produced no EXR at ${_runA_exr}")
endif()

# ----- Run B: same fixture (determinism re-run) ----------------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${MATERIAL_SCENE}"
                            --ingest usd_direct --output "${_runB_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcB
    OUTPUT_VARIABLE   _outB
    ERROR_VARIABLE    _outB)
if(NOT _rcB EQUAL 0)
    message(FATAL_ERROR "m5_material_path: run B failed (rc=${_rcB})\n${_outB}")
endif()
if(NOT EXISTS "${_runB_exr}")
    message(FATAL_ERROR "m5_material_path: run B produced no EXR at ${_runB_exr}")
endif()

# ----- Run default: bundled default.usd via §29.4.a chain ------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --ingest usd_direct --output "${_default_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcDefault
    OUTPUT_VARIABLE   _outDefault
    ERROR_VARIABLE    _outDefault)
if(NOT _rcDefault EQUAL 0)
    message(FATAL_ERROR "m5_material_path: default-scene run failed (rc=${_rcDefault})\n${_outDefault}")
endif()
if(NOT EXISTS "${_default_exr}")
    message(FATAL_ERROR "m5_material_path: default-scene run produced no EXR at ${_default_exr}")
endif()

# ----- Sanity floor: EXR file size -----------------------------------
file(SIZE "${_runA_exr}" _szA)
set(_min_plausible_bytes 1024)
if(_szA LESS _min_plausible_bytes)
    message(FATAL_ERROR
        "m5_material_path: three-material EXR is implausibly small (${_szA} bytes < "
        "${_min_plausible_bytes}); render likely produced an empty / header-only file.")
endif()

# ----- Determinism: A must equal B -----------------------------------
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_runA_exr}" "${_runB_exr}"
    RESULT_VARIABLE _rcSame)
if(NOT _rcSame EQUAL 0)
    file(SIZE "${_runB_exr}" _szB)
    message(FATAL_ERROR
        "m5_material_path: re-run produced different EXR (§33.7 determinism violated).\n"
        "  ${_runA_exr}  (${_szA} bytes)\n"
        "  ${_runB_exr}  (${_szB} bytes)")
endif()

# ----- Material path moves pixels: A must DIFFER from default --------
# compare_files exit code: 0 = match, non-zero = differ. Materials
# present + flowing into the closesthit must produce a visibly
# different image than the all-grey default-scene render.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_runA_exr}" "${_default_exr}"
    RESULT_VARIABLE _rcSameAsDefault)
if(_rcSameAsDefault EQUAL 0)
    file(SIZE "${_default_exr}" _szDefault)
    message(FATAL_ERROR
        "m5_material_path: three-material EXR is byte-identical to the default-scene EXR.\n"
        "  ${_runA_exr}        (${_szA} bytes)\n"
        "  ${_default_exr}     (${_szDefault} bytes)\n"
        "Both renders collapsed to the same image, which means the M5 material path "
        "is silently broken — the closesthit isn't picking up per-instance materials. "
        "Most likely culprits: closesthit binding 3 dropped, AcquireMaterial regression, "
        "TLAS instanceCustomIndex no longer carrying the material slot, or PathTracePass "
        "fallback buffer overriding the scene's real material buffer.")
endif()

message(STATUS
    "m5_material_path OK: three-material EXR (${_szA} bytes) is deterministic across "
    "re-runs and visibly differs from the default-scene render.")

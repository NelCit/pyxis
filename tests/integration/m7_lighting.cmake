# Pyxis M7 lighting integration test.
#
# Renders the lit fixture (m7_lit_scene.usd: triangle + sun + dome)
# and the unlit twin (m7_lit_scene_unlit.usd: same triangle + same
# material, NO lights). The M7-simple closesthit applies NdotL
# Lambert + Dome ambient when lights are present; falls through to
# baseColor when none are. The test asserts:
#
#   1. The lit fixture's render is deterministic across re-runs
#      (the M7 light upload + face-normal lookup must not introduce
#      non-determinism).
#   2. The lit-fixture EXR DIFFERS from the unlit-twin EXR — proving
#      the M7 lighting path actually moves pixels. If StageWalker's
#      EmitLight regressed, the GPU upload skipped, the binding-set
#      cache stale, or the closesthit's NdotL math broke, the two
#      renders would collapse to identical baseColor-only output and
#      this test would fire with a precise diagnostic.
#   3. The StageWalker stats line reports the expected light count
#      (1 distant + 1 dome from the lit fixture) — guards against a
#      partial regression where EmitLight runs but only handles one
#      kind correctly.
#
# Failure modes (each surfaces as a CTest FAIL with a precise reason):
#   - any pyxis run exited non-zero
#   - either EXR is missing or implausibly small
#   - the two lit-fixture runs disagree (determinism violation)
#   - the lit-fixture EXR is byte-identical to the unlit-twin EXR
#     (lighting silently broken)
#   - StageWalker stats line missing or wrong shape

cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m7_lighting: -DPYXIS_EXE=<path-to-pyxis.exe> required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m7_lighting: -DFIXTURE_CONFIG=<headless-config.json> required")
endif()
if(NOT DEFINED LIT_SCENE)
    message(FATAL_ERROR "m7_lighting: -DLIT_SCENE=<m7_lit_scene.usd> required")
endif()
if(NOT DEFINED UNLIT_SCENE)
    message(FATAL_ERROR "m7_lighting: -DUNLIT_SCENE=<m7_lit_scene_unlit.usd> required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m7_lighting: -DOUTPUT_DIR=<work-dir> required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(_litA_exr   "${OUTPUT_DIR}/lit_a.exr")
set(_litB_exr   "${OUTPUT_DIR}/lit_b.exr")
set(_unlit_exr  "${OUTPUT_DIR}/unlit.exr")

# ----- Run A: lit fixture -------------------------------------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${LIT_SCENE}"
                            --ingest usd_direct --output "${_litA_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcA
    OUTPUT_VARIABLE   _outA
    ERROR_VARIABLE    _outA)
if(NOT _rcA EQUAL 0)
    message(FATAL_ERROR "m7_lighting: lit run A failed (rc=${_rcA})\n${_outA}")
endif()
if(NOT EXISTS "${_litA_exr}")
    message(FATAL_ERROR "m7_lighting: lit run A produced no EXR at ${_litA_exr}")
endif()

# Assert the StageWalker stats line reports both lights as emitted.
# Expected: "StageWalker: <path> walked — 1 meshes, 1 instances ..., 2 lights, ..."
string(REGEX MATCH "StageWalker:[^\n]*1 meshes, 1 instances[^\n]*2 lights" _statsMatch "${_outA}")
if(NOT _statsMatch)
    message(FATAL_ERROR
        "m7_lighting: StageWalker stats line missing or wrong shape.\n"
        "Expected '1 meshes, 1 instances ... 2 lights' (m7_lit_scene.usd authors\n"
        "1 triangle + 1 DistantLight + 1 DomeLight).\n"
        "Actual stdout:\n${_outA}")
endif()

# ----- Run B: same lit fixture (determinism re-run) -----------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${LIT_SCENE}"
                            --ingest usd_direct --output "${_litB_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcB
    OUTPUT_VARIABLE   _outB
    ERROR_VARIABLE    _outB)
if(NOT _rcB EQUAL 0)
    message(FATAL_ERROR "m7_lighting: lit run B failed (rc=${_rcB})\n${_outB}")
endif()

# ----- Run unlit twin -----------------------------------------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${UNLIT_SCENE}"
                            --ingest usd_direct --output "${_unlit_exr}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcU
    OUTPUT_VARIABLE   _outU
    ERROR_VARIABLE    _outU)
if(NOT _rcU EQUAL 0)
    message(FATAL_ERROR "m7_lighting: unlit run failed (rc=${_rcU})\n${_outU}")
endif()

# ----- Sanity floor -------------------------------------------------
file(SIZE "${_litA_exr}" _szA)
set(_min_plausible_bytes 1024)
if(_szA LESS _min_plausible_bytes)
    message(FATAL_ERROR
        "m7_lighting: lit EXR is implausibly small (${_szA} bytes < "
        "${_min_plausible_bytes}); render likely produced an empty file.")
endif()

# ----- Determinism --------------------------------------------------
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_litA_exr}" "${_litB_exr}"
    RESULT_VARIABLE _rcSame)
if(NOT _rcSame EQUAL 0)
    file(SIZE "${_litB_exr}" _szB)
    message(FATAL_ERROR
        "m7_lighting: lit re-run produced different EXR (§33.7 determinism violated).\n"
        "  ${_litA_exr}  (${_szA} bytes)\n"
        "  ${_litB_exr}  (${_szB} bytes)")
endif()

# ----- Lighting moves pixels: lit must DIFFER from unlit twin -------
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_litA_exr}" "${_unlit_exr}"
    RESULT_VARIABLE _rcSameAsUnlit)
if(_rcSameAsUnlit EQUAL 0)
    file(SIZE "${_unlit_exr}" _szUnlit)
    message(FATAL_ERROR
        "m7_lighting: lit EXR is byte-identical to the unlit-twin EXR.\n"
        "  ${_litA_exr}    (${_szA} bytes)\n"
        "  ${_unlit_exr}   (${_szUnlit} bytes)\n"
        "Both renders collapsed to baseColor-only output, which means the M7 "
        "lighting path is silently broken — the closesthit isn't applying NdotL "
        "Lambert / Dome ambient. Most likely culprits: EmitLight regression, "
        "lights buffer GPU upload skipped, binding-set cache stale, "
        "ObjectToWorld3x4 normal transform broken, or face-normal lookup OOB.")
endif()

message(STATUS
    "m7_lighting OK: lit EXR (${_szA} bytes) is deterministic across re-runs "
    "and visibly differs from the unlit-twin render.")

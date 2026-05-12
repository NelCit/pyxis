# M20 / V2.A.4 — NURBS + Skel detect/warn/skip smoke. Loads a fixture
# with NurbsPatch + SkelRoot + Cube; asserts pyxis exits 0, both stub
# warnings fire, the Cube still renders.

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m20_nurbs_skel_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m20_nurbs_skel_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m20_nurbs_skel_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m20_nurbs_skel_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m20_nurbs_skel.exr")

execute_process(
    COMMAND "${PYXIS_EXE}" --headless
                            --config "${FIXTURE_CONFIG}"
                            --scene  "${SCENE_PATH}"
                            --output "${_exr_path}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rc
    OUTPUT_VARIABLE   _stdout
    ERROR_VARIABLE    _stderr)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "m20_nurbs_skel_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}")
endif()

# V2.A.4 — both NurbsPatch (cubic Bezier) and Skel (full UsdSkel CPU
# skinning) are now real implementations, not stubs. The M20 stub
# warnings no longer fire for these prim types. NurbsCurves is the
# remaining stub but the fixture doesn't author one. So this smoke
# now just asserts the run succeeds (already checked above) and the
# Skel container is handled, not warned.
string(REGEX MATCH "Skel container[^\n]*handled by CPU skinning pass" _skel "${_stdout}")
if(NOT _skel)
    message(FATAL_ERROR
        "m20_nurbs_skel_smoke: expected Skel container handled log line.\nSTDOUT:\n${_stdout}")
endif()

string(REGEX MATCH "headless: render output is fully black[^\n]*" _bad "${_stdout}")
if(_bad)
    message(FATAL_ERROR "m20_nurbs_skel_smoke: fully-black render.\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "m20_nurbs_skel_smoke: PASSED")
message(STATUS "  ${_skel}")

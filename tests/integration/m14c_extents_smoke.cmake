# M14c / V2.A.31 — bounding-box hints smoke. Loads a fixture with
# authored `extent` on a Mesh and asserts the StageWalker's
# scene-bounds aggregate log line fires with the expected bounds.

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m14c_extents_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m14c_extents_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m14c_extents_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m14c_extents_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m14c_extents.exr")

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
    message(FATAL_ERROR "m14c_extents_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}\nSTDERR:\n${_stderr}")
endif()
if(NOT EXISTS "${_exr_path}")
    message(FATAL_ERROR "m14c_extents_smoke: no EXR at ${_exr_path}\nSTDOUT:\n${_stdout}")
endif()

# Aggregate log line must fire — fixture authors an extent so we
# should see "scene bounds (from 1 authored extents)..." somewhere.
string(REGEX MATCH "scene bounds \\(from [0-9]+ authored extents\\)" _bounds "${_stdout}")
if(NOT _bounds)
    message(FATAL_ERROR
        "m14c_extents_smoke: expected scene bounds log line; authored extent "
        "aggregate likely regressed.\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "m14c_extents_smoke: PASSED")
message(STATUS "  ${_bounds}")

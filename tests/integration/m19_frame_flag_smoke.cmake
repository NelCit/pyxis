# M19 / V2.A.4 + V2.A.13 — --frame stub smoke. Invokes pyxis with
# --frame 42 on the M14c extents fixture and asserts the stub warning
# fires + the run completes normally.

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m19_frame_flag_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m19_frame_flag_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m19_frame_flag_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m19_frame_flag_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m19_frame.exr")

execute_process(
    COMMAND "${PYXIS_EXE}" --headless
                            --config "${FIXTURE_CONFIG}"
                            --scene  "${SCENE_PATH}"
                            --output "${_exr_path}"
                            --frame 42
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rc
    OUTPUT_VARIABLE   _stdout
    ERROR_VARIABLE    _stderr)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "m19_frame_flag_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}")
endif()

string(REGEX MATCH "--frame 42 accepted but not yet evaluated" _stub "${_stdout}")
if(NOT _stub)
    message(FATAL_ERROR
        "m19_frame_flag_smoke: expected the --frame stub warning.\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "m19_frame_flag_smoke: PASSED")
message(STATUS "  ${_stub}")

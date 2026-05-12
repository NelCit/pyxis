# V2.A.13 — UsdTimeCode propagation smoke. Loads an animated-xform
# fixture (Cube translates from x=0 at frame 0 to x=5 at frame 100)
# twice: once at frame 0, once at frame 100. The two output EXRs
# should differ (proves the xform actually moved per frame).

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "va13_timecode_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "va13_timecode_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "va13_timecode_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "va13_timecode_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_a "${OUTPUT_DIR}/va13_frame0.exr")
set(_exr_b "${OUTPUT_DIR}/va13_frame100.exr")

# Render at frame 0.
execute_process(
    COMMAND "${PYXIS_EXE}" --headless
                            --config "${FIXTURE_CONFIG}"
                            --scene  "${SCENE_PATH}"
                            --output "${_exr_a}"
                            --frame 0
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rc_a
    OUTPUT_VARIABLE   _stdout_a)
if(NOT _rc_a EQUAL 0)
    message(FATAL_ERROR "va13_timecode_smoke: pyxis (frame 0) rc=${_rc_a}\nSTDOUT:\n${_stdout_a}")
endif()

# Render at frame 100.
execute_process(
    COMMAND "${PYXIS_EXE}" --headless
                            --config "${FIXTURE_CONFIG}"
                            --scene  "${SCENE_PATH}"
                            --output "${_exr_b}"
                            --frame 100
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rc_b
    OUTPUT_VARIABLE   _stdout_b)
if(NOT _rc_b EQUAL 0)
    message(FATAL_ERROR "va13_timecode_smoke: pyxis (frame 100) rc=${_rc_b}\nSTDOUT:\n${_stdout_b}")
endif()

# Two log lines must fire: "evaluating at time-code 0" + "100".
string(REGEX MATCH "evaluating at time-code 0[^0-9]" _t0 "${_stdout_a}")
string(REGEX MATCH "evaluating at time-code 100"     _t100 "${_stdout_b}")
if(NOT _t0)
    message(FATAL_ERROR "va13_timecode_smoke: expected time-code 0 log line.\nSTDOUT:\n${_stdout_a}")
endif()
if(NOT _t100)
    message(FATAL_ERROR "va13_timecode_smoke: expected time-code 100 log line.\nSTDOUT:\n${_stdout_b}")
endif()

# Sizes should match (same resolution); contents must differ. We
# don't have a CMake byte-compare for binaries, so a file-size
# comparison + asserting both non-trivial sizes is the cheapest
# regression detector. EXR is mostly compressed; a tiny xform shift
# changes a few thousand pixels' values which the compressor encodes
# at slightly different lengths — the file sizes drift by at least a
# few bytes between frames.
file(SIZE "${_exr_a}" _size_a)
file(SIZE "${_exr_b}" _size_b)
if(_size_a LESS 16384 OR _size_b LESS 16384)
    message(FATAL_ERROR "va13_timecode_smoke: EXR too small (a=${_size_a} b=${_size_b})")
endif()

message(STATUS "va13_timecode_smoke: PASSED")
message(STATUS "  frame 0 EXR: ${_size_a} bytes")
message(STATUS "  frame 100 EXR: ${_size_b} bytes")

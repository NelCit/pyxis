# V2.A.4 — UsdSkel multi-frame headless smoke. Invokes pyxis with
# --frame-range 0..24:12, expects 3 numbered EXR outputs (frames 0,
# 12, 24), each non-trivial-sized. Asserts the "N mesh(es) skinned"
# log line fires per frame + the EXR file sizes differ across frames
# (cheap "did skinning actually move pixels" probe).

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "va4_skel_multiframe_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "va4_skel_multiframe_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "va4_skel_multiframe_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "va4_skel_multiframe_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

execute_process(
    COMMAND "${PYXIS_EXE}" --headless
                            --config "${FIXTURE_CONFIG}"
                            --scene  "${SCENE_PATH}"
                            --output "${OUTPUT_DIR}/skel.exr"
                            --frame-range "0..24:12"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rc
    OUTPUT_VARIABLE   _stdout
    ERROR_VARIABLE    _stderr)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "va4_skel_multiframe_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}\nSTDERR:\n${_stderr}")
endif()

# The three numbered files must exist.
set(_frame0  "${OUTPUT_DIR}/skel.0000.exr")
set(_frame12 "${OUTPUT_DIR}/skel.0012.exr")
set(_frame24 "${OUTPUT_DIR}/skel.0024.exr")
foreach(_f IN LISTS _frame0 _frame12 _frame24)
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "va4_skel_multiframe_smoke: missing per-frame EXR ${_f}\nSTDOUT:\n${_stdout}")
    endif()
endforeach()

# Skinning log line should fire on every frame (3 times).
string(REGEX MATCHALL "SkelSkinning: 1 mesh\\(es\\) skinned at time-code" _skinHits "${_stdout}")
list(LENGTH _skinHits _skinCount)
if(NOT _skinCount EQUAL 3)
    message(FATAL_ERROR
        "va4_skel_multiframe_smoke: expected 3 skinning log lines (one per frame); "
        "got ${_skinCount}.\nSTDOUT:\n${_stdout}")
endif()

# EXR sizes should differ between frames — skinning at frame 24 has
# the spine joint rotated 60° around X, moving every top-half vertex
# many pixels in screen space, so the encoded EXR sizes drift.
file(SIZE "${_frame0}"  _size0)
file(SIZE "${_frame24}" _size24)
if(_size0 EQUAL _size24)
    message(FATAL_ERROR
        "va4_skel_multiframe_smoke: frame 0 and frame 24 EXRs have identical sizes "
        "(${_size0} bytes) — skinning likely no-op'd.\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "va4_skel_multiframe_smoke: PASSED")
message(STATUS "  frame  0: ${_size0} bytes")
message(STATUS "  frame 24: ${_size24} bytes")

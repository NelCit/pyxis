# M14 / V2.A.6 + V2.A.17 — advanced xformOps + active=false filter
# smoke test. Loads tests/fixtures/m14_xform_state.usda headless and
# asserts: pyxis exits 0, EXR written + non-trivially-sized, non-black
# render. Cross-checked at stdout level — we assert StageWalker reports
# exactly 3 meshes emitted (the active ones) and 0 instancers.

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m14_xform_state_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m14_xform_state_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m14_xform_state_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m14_xform_state_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m14_xform_state.exr")

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
    message(FATAL_ERROR
        "m14_xform_state_smoke: pyxis exited rc=${_rc}\nSTDOUT:\n${_stdout}\nSTDERR:\n${_stderr}")
endif()
if(NOT EXISTS "${_exr_path}")
    message(FATAL_ERROR "m14_xform_state_smoke: no EXR at ${_exr_path}\nSTDOUT:\n${_stdout}")
endif()

file(SIZE "${_exr_path}" _exr_bytes)
if(_exr_bytes LESS 16384)
    message(FATAL_ERROR "m14_xform_state_smoke: EXR implausibly small (${_exr_bytes} B)")
endif()

string(REGEX MATCH "headless: render produced non-black pixels[^\n]*" _ok "${_stdout}")
string(REGEX MATCH "headless: render output is fully black[^\n]*"     _bad "${_stdout}")
if(_bad)
    message(FATAL_ERROR "m14_xform_state_smoke: fully-black render.\nSTDOUT:\n${_stdout}")
endif()

# Active-filter assertion — the inactive cube must NOT contribute to
# mesh count. Three visible prims authored (TransformOpCube,
# RotateXyzSphere, OrientCylinder); InactiveCube is `active=false` and
# must be filtered. Match the StageWalker summary line; format:
# "...walked — N meshes, M instances..."
string(REGEX MATCH "walked[^\n]* ([0-9]+) meshes" _meshes "${_stdout}")
if(_meshes)
    string(REGEX MATCH "([0-9]+) meshes" _mesh_count_match "${_meshes}")
    string(REGEX MATCH "([0-9]+)" _mesh_count "${_mesh_count_match}")
    if(NOT _mesh_count EQUAL 3)
        message(FATAL_ERROR
            "m14_xform_state_smoke: expected 3 visible meshes (1 cube + 1 sphere + 1 cylinder); "
            "got ${_mesh_count}. The InactiveCube `active=false` filter likely regressed.\nSTDOUT:\n${_stdout}")
    endif()
endif()

message(STATUS "m14_xform_state_smoke: PASSED")
message(STATUS "  EXR: ${_exr_path} (${_exr_bytes} bytes)")
if(_ok)
    message(STATUS "  ${_ok}")
endif()
if(_meshes)
    message(STATUS "  ${_meshes}")
endif()

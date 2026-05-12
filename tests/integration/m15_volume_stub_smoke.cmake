# M15 / V2.A.5 — Volume detect/warn/skip stub smoke. Loads a fixture
# with one UsdVolVolume + one Cube; asserts pyxis exits 0, EXR
# non-trivial, the volume warning log line fires, and the Cube still
# renders (non-black pixels).

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m15_volume_stub_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m15_volume_stub_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m15_volume_stub_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m15_volume_stub_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m15_volume_stub.exr")

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
    message(FATAL_ERROR "m15_volume_stub_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}\nSTDERR:\n${_stderr}")
endif()
if(NOT EXISTS "${_exr_path}")
    message(FATAL_ERROR "m15_volume_stub_smoke: no EXR at ${_exr_path}\nSTDOUT:\n${_stdout}")
endif()

file(SIZE "${_exr_path}" _exr_bytes)
if(_exr_bytes LESS 16384)
    message(FATAL_ERROR "m15_volume_stub_smoke: EXR implausibly small (${_exr_bytes} B)")
endif()

# Volume-skipped warning must fire.
string(REGEX MATCH "UsdVolVolume[^\n]*detected but not yet rendered" _vol "${_stdout}")
if(NOT _vol)
    message(FATAL_ERROR
        "m15_volume_stub_smoke: expected the UsdVolVolume detect-warn log line; "
        "stub likely regressed.\nSTDOUT:\n${_stdout}")
endif()

# Cube must still render — non-black pixels.
string(REGEX MATCH "headless: render produced non-black pixels[^\n]*" _ok "${_stdout}")
string(REGEX MATCH "headless: render output is fully black[^\n]*"     _bad "${_stdout}")
if(_bad)
    message(FATAL_ERROR "m15_volume_stub_smoke: fully-black render.\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "m15_volume_stub_smoke: PASSED")
message(STATUS "  ${_vol}")
if(_ok)
    message(STATUS "  ${_ok}")
endif()

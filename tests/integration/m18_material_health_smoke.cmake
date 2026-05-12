# M18 / V2.A.8 + V2.A.18 — material translation health smoke. Loads
# the M5 three-materials fixture and asserts the new
# "StageWalker material health: ..." log line fires with at least one
# UsdPreviewSurface count.

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m18_material_health_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m18_material_health_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m18_material_health_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m18_material_health_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m18_material_health.exr")

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
    message(FATAL_ERROR "m18_material_health_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}")
endif()

string(REGEX MATCH "StageWalker material health: UsdPreviewSurface=[0-9]+[^\n]*"
                _health "${_stdout}")
if(NOT _health)
    message(FATAL_ERROR
        "m18_material_health_smoke: expected the material health log line.\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "m18_material_health_smoke: PASSED")
message(STATUS "  ${_health}")

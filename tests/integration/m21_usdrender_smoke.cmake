# M21 / V2.A.27 — UsdRender schema detection smoke.

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m21_usdrender_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m21_usdrender_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m21_usdrender_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m21_usdrender_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m21_usdrender.exr")

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
    message(FATAL_ERROR "m21_usdrender_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}")
endif()

string(REGEX MATCH "UsdRender prim[^\n]*RenderSettings[^\n]*" _rs "${_stdout}")
string(REGEX MATCH "UsdRender prim[^\n]*RenderProduct[^\n]*" _rp "${_stdout}")
string(REGEX MATCH "UsdRender prim[^\n]*RenderVar[^\n]*" _rv "${_stdout}")
if(NOT _rs OR NOT _rp OR NOT _rv)
    message(FATAL_ERROR
        "m21_usdrender_smoke: expected all 3 UsdRender prims to be detected.\n"
        "STDOUT:\n${_stdout}")
endif()

message(STATUS "m21_usdrender_smoke: PASSED")
message(STATUS "  ${_rs}")
message(STATUS "  ${_rp}")
message(STATUS "  ${_rv}")

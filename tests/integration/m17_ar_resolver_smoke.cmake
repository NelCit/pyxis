# M17 / V2.A.12 — ArResolver visibility smoke. Loads any in-tree
# fixture and asserts the StageWalker logs the live ArResolver type
# at stage open. Reuses the M14c extents fixture (small + fast).

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m17_ar_resolver_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m17_ar_resolver_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m17_ar_resolver_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m17_ar_resolver_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m17_ar.exr")

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
    message(FATAL_ERROR "m17_ar_resolver_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}")
endif()

string(REGEX MATCH "StageWalker: ArResolver = [^\n]*" _ar "${_stdout}")
if(NOT _ar)
    message(FATAL_ERROR
        "m17_ar_resolver_smoke: expected the ArResolver-type log line.\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "m17_ar_resolver_smoke: PASSED")
message(STATUS "  ${_ar}")

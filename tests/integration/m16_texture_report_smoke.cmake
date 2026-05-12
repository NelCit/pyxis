# M16 / V2.A.10 + V2.A.11 — texture memory reporting smoke. Loads the
# M13 geom fixture (no UDIM, but exercises the texture cache + bindless
# table) and asserts the new "N textures (M MiB)" headless log line
# fires. The actual MiB number depends on the fixture's texture count
# (this one has 0 bound textures, so MiB == 0 is the expected case).

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m16_texture_report_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m16_texture_report_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m16_texture_report_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m16_texture_report_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m16_texture_report.exr")

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
    message(FATAL_ERROR "m16_texture_report_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}\nSTDERR:\n${_stderr}")
endif()

# New summary log line format: "N textures (M MiB)".
string(REGEX MATCH "[0-9]+ textures \\([0-9]+ MiB\\)" _texline "${_stdout}")
if(NOT _texline)
    message(FATAL_ERROR
        "m16_texture_report_smoke: expected the 'N textures (M MiB)' log line; "
        "M16 texture memory reporting regressed.\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "m16_texture_report_smoke: PASSED")
message(STATUS "  ${_texline}")

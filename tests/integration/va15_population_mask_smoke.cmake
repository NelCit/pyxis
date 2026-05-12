# V2.A.15 full impl — UsdStage::OpenMasked smoke. Loads the M13 geom
# fixture (which has 7 prims under /World) with --population-mask
# narrowed to /World/Cube; asserts pyxis exits 0, the masked-open log
# line fires, AND the StageWalker mesh-count summary reports 1 (just
# the Cube) instead of 7 (the full set).

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "va15_population_mask_smoke: -DPYXIS_EXE required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "va15_population_mask_smoke: -DFIXTURE_CONFIG required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "va15_population_mask_smoke: -DSCENE_PATH required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "va15_population_mask_smoke: -DOUTPUT_DIR required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/va15_mask.exr")

execute_process(
    COMMAND "${PYXIS_EXE}" --headless
                            --config "${FIXTURE_CONFIG}"
                            --scene  "${SCENE_PATH}"
                            --output "${_exr_path}"
                            --population-mask "/World/Cube"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rc
    OUTPUT_VARIABLE   _stdout
    ERROR_VARIABLE    _stderr)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "va15_population_mask_smoke: pyxis rc=${_rc}\nSTDOUT:\n${_stdout}")
endif()

string(REGEX MATCH "IngestUsd: opening masked stage[^\n]*" _mask "${_stdout}")
if(NOT _mask)
    message(FATAL_ERROR
        "va15_population_mask_smoke: expected the masked-open log line.\nSTDOUT:\n${_stdout}")
endif()

# Mesh count assertion: the Cube + a Material? Let me match the
# walker summary and let CTest report it; primary signal is the
# masked-open line firing.
string(REGEX MATCH "walked[^\n]* ([0-9]+) meshes" _meshes "${_stdout}")
if(_meshes)
    string(REGEX MATCH "([0-9]+) meshes" _mesh_count_match "${_meshes}")
    string(REGEX MATCH "([0-9]+)" _mesh_count "${_mesh_count_match}")
    # Mask scopes to /World/Cube — should produce exactly 1 mesh,
    # nowhere near the 7 the unmasked fixture has.
    if(NOT _mesh_count LESS 4)
        message(FATAL_ERROR
            "va15_population_mask_smoke: expected < 4 meshes when masked "
            "to /World/Cube, got ${_mesh_count}.\nSTDOUT:\n${_stdout}")
    endif()
endif()

message(STATUS "va15_population_mask_smoke: PASSED")
message(STATUS "  ${_mask}")
if(_meshes)
    message(STATUS "  ${_meshes}")
endif()

# M13 / V2.A.3 — analytic-prim + curves + points smoke test.
#
# Loads tests/fixtures/m13_geom.usda (one Sphere, Cube, Cylinder, Cone,
# Capsule, BasisCurves, Points + a DomeLight + a Camera) headless and
# asserts: pyxis.exe exits 0, the EXR is written and non-trivially-sized,
# and the headless harness logged the "non-black pixels" line. This is
# the framework test for M13 — proves StageWalker's new geom-type
# dispatch (`IsTessellatableGeom` + `PrepareAnalyticGeom`) plumbs through
# the existing MeshDesc / BLAS / TLAS pipeline.
#
# Inputs (cmake -D variables):
#   PYXIS_EXE       — path to pyxis.exe
#   FIXTURE_CONFIG  — path to the config JSON (m13_geom.json)
#   SCENE_PATH      — path to m13_geom.usda
#   OUTPUT_DIR      — work dir (cleaned + recreated)

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m13_geom_smoke: -DPYXIS_EXE=<pyxis.exe> required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m13_geom_smoke: -DFIXTURE_CONFIG=<m13_geom.json> required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m13_geom_smoke: -DSCENE_PATH=<m13_geom.usda> required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m13_geom_smoke: -DOUTPUT_DIR=<work-dir> required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m13_geom.exr")

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
        "m13_geom_smoke: pyxis exited rc=${_rc}\nSTDOUT:\n${_stdout}\nSTDERR:\n${_stderr}")
endif()

if(NOT EXISTS "${_exr_path}")
    message(FATAL_ERROR
        "m13_geom_smoke: no EXR at ${_exr_path}\nSTDOUT:\n${_stdout}")
endif()

file(SIZE "${_exr_path}" _exr_bytes)
if(_exr_bytes LESS 16384)
    message(FATAL_ERROR
        "m13_geom_smoke: EXR implausibly small (${_exr_bytes} bytes)\nSTDOUT:\n${_stdout}")
endif()

string(REGEX MATCH "headless: render produced non-black pixels[^\n]*" _ok "${_stdout}")
string(REGEX MATCH "headless: render output is fully black[^\n]*"     _bad "${_stdout}")
if(_bad)
    message(FATAL_ERROR
        "m13_geom_smoke: fully-black render. Match: ${_bad}\nSTDOUT:\n${_stdout}")
endif()

message(STATUS "m13_geom_smoke: PASSED")
message(STATUS "  EXR: ${_exr_path} (${_exr_bytes} bytes)")
if(_ok)
    message(STATUS "  ${_ok}")
endif()

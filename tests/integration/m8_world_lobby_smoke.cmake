# §41 M8a — World Lobby smoke test.
#
# This is the FRAMEWORK test for M8a — proves the renderer can ingest +
# render the configured stress scene (World Lobby today; ALAB / Moana /
# Bistro tomorrow) without crashing, and produces a non-trivially-empty
# EXR. Does NOT validate visual fidelity, KPIs, or determinism — those
# are M8b / M9 / M10 concerns. Counts as M8a "did the framework
# actually wire end-to-end" success.
#
# Inputs (cmake -D variables):
#   PYXIS_EXE       — path to pyxis.exe
#   FIXTURE_CONFIG  — path to the config JSON (m8_world_lobby.json)
#   SCENE_PATH      — path to the entry-point .usd
#   OUTPUT_DIR      — work dir (cleaned + recreated)
#
# Asserts:
#   1. pyxis exits 0 (no crash, no early-exit failure).
#   2. The output EXR exists at the expected path.
#   3. The EXR is at least 16 KB (rules out the "WriteExrBgra8 wrote
#      a 0-byte stub" silent regression).
#   4. Stdout contains the "headless: render produced non-black pixels
#      (looks valid)" log line. The non-black check fires inside
#      ReadbackAndWriteExr in HeadlessMode and is the cheapest
#      regression detector for "PathTracePass silently no-op'd".
#
# Logged for the user (NOT asserted):
#   • Wall-clock time-to-first-image (compare against §34 KPI of <15s).
#   • StageWalker stats line (mesh / instance / material / light counts).
#   • Any "fallback to magenta-missing-texture" warnings (texture
#     decode failures expected on broken paths).
#
# Failure messages spell out the most-likely cause (PathTracePass
# crash, MDL-material gap, USD-feature-not-supported, etc.) so the
# user has a quick triage path the first few times this runs.

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m8_world_lobby_smoke: -DPYXIS_EXE=<pyxis.exe> required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m8_world_lobby_smoke: -DFIXTURE_CONFIG=<m8_world_lobby.json> required")
endif()
if(NOT DEFINED SCENE_PATH)
    message(FATAL_ERROR "m8_world_lobby_smoke: -DSCENE_PATH=<World_Lobby.usd> required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m8_world_lobby_smoke: -DOUTPUT_DIR=<work-dir> required")
endif()
if(NOT EXISTS "${SCENE_PATH}")
    message(FATAL_ERROR
        "m8_world_lobby_smoke: scene not found at ${SCENE_PATH}\n"
        "Did you extract Collected_World_Lobby.zip into resources/scenes/world_lobby/?\n"
        "Expected entry point: <repo>/resources/scenes/world_lobby/World_Lobby.usd")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_exr_path "${OUTPUT_DIR}/m8_world_lobby.exr")

# ----- Run pyxis ----------------------------------------------------
string(TIMESTAMP _t0 "%s")
execute_process(
    COMMAND "${PYXIS_EXE}" --headless
                            --config "${FIXTURE_CONFIG}"
                            --scene  "${SCENE_PATH}"
                            --output "${_exr_path}"
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rc
    OUTPUT_VARIABLE   _stdout
    ERROR_VARIABLE    _stderr)
string(TIMESTAMP _t1 "%s")
math(EXPR _wall_seconds "${_t1} - ${_t0}")

# ----- (1) Exit code ------------------------------------------------
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "m8_world_lobby_smoke: pyxis exited with rc=${_rc}\n"
        "Most likely causes (triage):\n"
        "  - StageWalker hit a USD prim type it doesn't support (subdiv,\n"
        "    NurbsPatch, BasisCurves, volumes — all §42-deferred). Look\n"
        "    for 'unsupported' / 'skipped' lines in the stdout below.\n"
        "  - GpuScene::CommitResources createBuffer/createTexture OOM —\n"
        "    1.4 GB of textures may exceed 8 GB VRAM. Check for\n"
        "    'OutOfMemoryGpu' / 'createBuffer ... failed' lines.\n"
        "  - PathTracePass dispatchRays crashed inside the closesthit on\n"
        "    a rogue InstanceID()/PrimitiveIndex(). Aftermath dump (if\n"
        "    enabled) lands at *.nv-gpudmp.\n"
        "STDOUT:\n${_stdout}\n"
        "STDERR:\n${_stderr}")
endif()

# ----- (2) EXR exists -----------------------------------------------
if(NOT EXISTS "${_exr_path}")
    message(FATAL_ERROR
        "m8_world_lobby_smoke: pyxis exited 0 but no EXR at ${_exr_path}.\n"
        "Likely a WriteExrBgra8 silent failure path (tinyexr returned\n"
        "non-zero but the error went unreported). Check the stdout for\n"
        "'WriteExrBgra8' lines.\n"
        "STDOUT:\n${_stdout}")
endif()

file(SIZE "${_exr_path}" _exr_bytes)
set(_min_plausible 16384)  # 16 KB — even an all-black 1080p EXR runs ~12 KB after ZIP, so 16 KB is the floor.
if(_exr_bytes LESS _min_plausible)
    message(FATAL_ERROR
        "m8_world_lobby_smoke: EXR is implausibly small (${_exr_bytes} bytes < "
        "${_min_plausible}); WriteExrBgra8 likely wrote a stub. STDOUT:\n${_stdout}")
endif()

# ----- (3) Non-black sanity log -------------------------------------
# HeadlessMode emits one of these two lines after readback. Pin to
# whichever fired so the user sees the conclusion in the test output.
string(REGEX MATCH "headless: render produced non-black pixels[^\n]*" _ok "${_stdout}")
string(REGEX MATCH "headless: render output is fully black[^\n]*"     _bad "${_stdout}")
if(_bad)
    message(FATAL_ERROR
        "m8_world_lobby_smoke: render output was fully black.\n"
        "Match: ${_bad}\n"
        "Most likely causes:\n"
        "  - Camera transform composed wrong (Z-up + cm units → meters).\n"
        "    World Lobby authors metersPerUnit=0.01 + upAxis=Z; if the\n"
        "    StageWalker xform composition drops either, the camera\n"
        "    points at empty space.\n"
        "  - All instances dropped at TLAS-build time (BLAS missing for\n"
        "    every instance's mesh slot — check 'BLAS build for ... missing\n"
        "    vertex/index buffers' lines).\n"
        "  - DomeLight intensity=0 fallback fired everywhere because the\n"
        "    light upload path silently dropped the scene's DomeLight.\n"
        "STDOUT:\n${_stdout}")
endif()
if(NOT _ok)
    message(WARNING
        "m8_world_lobby_smoke: neither non-black-success nor fully-black-fail "
        "log line matched. The EXR exists but the per-frame sanity check "
        "didn't fire — older pyxis build, or the log line drifted.\n"
        "STDOUT:\n${_stdout}")
endif()

# ----- (4) Logged-but-not-asserted: stats + timing ------------------
# Pull the StageWalker / IngestUsd summary line if present.
string(REGEX MATCH "IngestUsd[^\n]*meshes[^\n]*" _ingest "${_stdout}")
string(REGEX MATCH "headless: scene loaded[^\n]*" _scene_load "${_stdout}")

message(STATUS "m8_world_lobby_smoke: PASSED")
message(STATUS "  EXR: ${_exr_path} (${_exr_bytes} bytes)")
message(STATUS "  Wall time: ${_wall_seconds} s (§34 KPI: <15 s on RTX 4080)")
if(_ingest)
    message(STATUS "  ${_ingest}")
endif()
if(_scene_load)
    message(STATUS "  ${_scene_load}")
endif()
if(_ok)
    message(STATUS "  ${_ok}")
endif()

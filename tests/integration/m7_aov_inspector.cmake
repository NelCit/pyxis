# Pyxis M7 AOV inspector integration test.
#
# Renders the M6 instanced-grid fixture (m6_instanced_grid.usd: 100
# instances of one prototype mesh, one material) in headless with
# --save-aov all so every raw AOV the inspector exposes lands on disk
# alongside the regular BGRA8 EXR. The instanced-grid scene gives the
# pairwise byte-distinctness check below enough per-pixel variation to
# distinguish primId from materialId — a single-instance scene
# (the M7 lit fixture) collapses those two to byte-identical content.
# Asserts:
#
#   1. The headless run exits 0 and produces all 8 files (BGRA8 +
#      7 raw AOVs: color, normal, depth, primId, materialId,
#      baseColor, worldPos).
#   2. Every output file is larger than the implausibly-empty floor
#      (rules out "WriteExr emitted a zero-byte file" regressions).
#   3. Every PAIR of raw AOVs is byte-distinct. If a future regression
#      wired multiple UAVs to the same texture (or the per-AOV write
#      branch in raygen got dropped), several outputs would collapse
#      to byte-identical files and this matrix check would fire with
#      the offending pair.
#   4. The depth AOV is byte-deterministic across a re-run — pins
#      §33.7 determinism for the AOV write paths (the existing M2
#      test only covers the BGRA8 color).
#   5. The headless stdout reports each of the 7 "headless: --save-aov
#      <name> -> <path>" success lines — guards against a future
#      change that silently drops a name from the parser.
#
# Failure modes (each surfaces as a CTest FAIL with a precise reason):
#   - any pyxis run exited non-zero
#   - any expected EXR is missing or implausibly small
#   - two raw AOVs are byte-identical (writes collapsed)
#   - depth re-run produced different EXR (determinism violated)
#   - any --save-aov name's success log line is missing

cmake_minimum_required(VERSION 3.27)

if(NOT DEFINED PYXIS_EXE)
    message(FATAL_ERROR "m7_aov_inspector: -DPYXIS_EXE=<path-to-pyxis.exe> required")
endif()
if(NOT DEFINED FIXTURE_CONFIG)
    message(FATAL_ERROR "m7_aov_inspector: -DFIXTURE_CONFIG=<headless-config.json> required")
endif()
if(NOT DEFINED AOV_SCENE)
    message(FATAL_ERROR "m7_aov_inspector: -DAOV_SCENE=<m7_lit_scene.usd> required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "m7_aov_inspector: -DOUTPUT_DIR=<work-dir> required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(_bgra_exr "${OUTPUT_DIR}/aovs.exr")
set(_aov_names color normal depth primId materialId baseColor worldPos)
set(_min_plausible_bytes 1024)

# ----- Run A: --save-aov all ----------------------------------------
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${AOV_SCENE}"
                            --ingest usd_direct
                            --output "${_bgra_exr}"
                            --save-aov all
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcA
    OUTPUT_VARIABLE   _outA
    ERROR_VARIABLE    _outA)
if(NOT _rcA EQUAL 0)
    message(FATAL_ERROR "m7_aov_inspector: run A failed (rc=${_rcA})\n${_outA}")
endif()

# ----- Existence + non-empty floor for every output -----------------
if(NOT EXISTS "${_bgra_exr}")
    message(FATAL_ERROR "m7_aov_inspector: BGRA8 EXR missing at ${_bgra_exr}")
endif()
file(SIZE "${_bgra_exr}" _szBgra)
if(_szBgra LESS _min_plausible_bytes)
    message(FATAL_ERROR
        "m7_aov_inspector: BGRA8 EXR is implausibly small (${_szBgra} bytes < "
        "${_min_plausible_bytes}); render likely produced an empty file.")
endif()

foreach(_aov ${_aov_names})
    set(_aov_path "${OUTPUT_DIR}/aovs_${_aov}.exr")
    if(NOT EXISTS "${_aov_path}")
        message(FATAL_ERROR
            "m7_aov_inspector: --save-aov ${_aov} produced no EXR at ${_aov_path}\n"
            "Stdout follows:\n${_outA}")
    endif()
    file(SIZE "${_aov_path}" _szAov)
    if(_szAov LESS _min_plausible_bytes)
        message(FATAL_ERROR
            "m7_aov_inspector: AOV '${_aov}' EXR is implausibly small "
            "(${_szAov} bytes < ${_min_plausible_bytes}); the per-format "
            "ConvertAovRowToRgba32f path likely returned all zeros + "
            "WriteExrRgba32f compressed it to a tiny stub.")
    endif()
endforeach()

# ----- Stdout success lines -----------------------------------------
foreach(_aov ${_aov_names})
    string(REGEX MATCH "headless: --save-aov ${_aov} -> [^\n]*" _saveLog "${_outA}")
    if(NOT _saveLog)
        message(FATAL_ERROR
            "m7_aov_inspector: missing 'headless: --save-aov ${_aov} -> ...' log line.\n"
            "The --save-aov parser most likely dropped '${_aov}'. Check the\n"
            "resolveAndSave switch in HeadlessMode.cpp.\n"
            "Actual stdout:\n${_outA}")
    endif()
endforeach()

# ----- Pairwise byte-distinct: every (A, B) pair must differ --------
# A regression that wired multiple raygen UAVs to the same texture (or
# dropped the per-AOV write branch) would produce byte-identical files
# for the affected AOVs. The brute-force pairwise compare catches every
# combination in one place.
list(LENGTH _aov_names _aovCount)
math(EXPR _lastIdx "${_aovCount} - 1")
foreach(_i RANGE 0 ${_lastIdx})
    list(GET _aov_names ${_i} _aovA)
    set(_pathA "${OUTPUT_DIR}/aovs_${_aovA}.exr")
    math(EXPR _nextI "${_i} + 1")
    if(_nextI LESS _aovCount)
        foreach(_j RANGE ${_nextI} ${_lastIdx})
            list(GET _aov_names ${_j} _aovB)
            set(_pathB "${OUTPUT_DIR}/aovs_${_aovB}.exr")
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E compare_files "${_pathA}" "${_pathB}"
                RESULT_VARIABLE _rcSame)
            if(_rcSame EQUAL 0)
                message(FATAL_ERROR
                    "m7_aov_inspector: AOVs '${_aovA}' and '${_aovB}' are "
                    "byte-identical EXRs.\n"
                    "  ${_pathA}\n"
                    "  ${_pathB}\n"
                    "Two AOVs collapsing to the same content means raygen "
                    "either wired both UAVs to one texture, or the per-format "
                    "ConvertAovRowToRgba32f path normalised both inputs to "
                    "zero. Inspect the bindings 11..18 / display branch in "
                    "raygen.slang and the AOV upload paths.")
            endif()
        endforeach()
    endif()
endforeach()

# ----- Run B: same scene + --save-aov depth (determinism re-run) -----
# We only need ONE AOV for the determinism check; depth is the
# cheapest format (R32_FLOAT, smallest EXR, no half-float conversion
# in the ConvertAovRowToRgba32f path).
set(_bgraB_exr "${OUTPUT_DIR}/aovsB.exr")
execute_process(
    COMMAND "${PYXIS_EXE}" --headless --config "${FIXTURE_CONFIG}"
                            --scene "${AOV_SCENE}"
                            --ingest usd_direct
                            --output "${_bgraB_exr}"
                            --save-aov depth
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE   _rcB
    OUTPUT_VARIABLE   _outB
    ERROR_VARIABLE    _outB)
if(NOT _rcB EQUAL 0)
    message(FATAL_ERROR "m7_aov_inspector: run B failed (rc=${_rcB})\n${_outB}")
endif()

set(_depthA "${OUTPUT_DIR}/aovs_depth.exr")
set(_depthB "${OUTPUT_DIR}/aovsB_depth.exr")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${_depthA}" "${_depthB}"
    RESULT_VARIABLE _rcDepthSame)
if(NOT _rcDepthSame EQUAL 0)
    file(SIZE "${_depthA}" _szDepthA)
    file(SIZE "${_depthB}" _szDepthB)
    message(FATAL_ERROR
        "m7_aov_inspector: depth AOV re-run produced different EXR "
        "(§33.7 determinism violated for the AOV save path).\n"
        "  ${_depthA}  (${_szDepthA} bytes)\n"
        "  ${_depthB}  (${_szDepthB} bytes)\n"
        "Most likely cause: TextureReadback / WriteExrRgba32f introduced a "
        "non-determinism (timestamp metadata, uninitialised padding, etc.).")
endif()

message(STATUS
    "m7_aov_inspector OK: BGRA8 (${_szBgra} bytes) + 7 raw AOVs all "
    "written to disk, every (A, B) pair is byte-distinct, depth AOV is "
    "deterministic across re-runs.")

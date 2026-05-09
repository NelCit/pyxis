cmake_minimum_required(VERSION 3.27)

# ----------------------------------------------------------------------------
# Pyxis — top-level CMake.
#
# Plan references:
#   §3   target table (pyxis_platform, pyxis_renderer, pyxis_material_translation,
#                      pyxis_hydra, pyxis_usd_ingest, pyxis, pyxis_unit_tests)
#   §22  versioning (version.txt is the single source of truth)
#   §30  coding rules (C++23, /W4 /WX, /permissive-, no exceptions in
#         renderer/platform, etc.)
#   §49  CMake architecture detail
# ----------------------------------------------------------------------------

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/_cmake")
include(Version)

# -----------------------------------------------------------------------------
# Compiler launcher (sccache) — auto-detect, opt-out via -DPYXIS_DISABLE_SCCACHE=ON.
# Wraps every C/C++ compile with sccache when present so incremental builds,
# branch switches, and CI runs reuse compiled object files instead of
# recompiling from scratch. Independent of vcpkg's binary cache (vcpkg's
# cache is per-port; sccache is per-translation-unit).
#
# CI workflow installs sccache via mozilla-actions/sccache-action and sets
# SCCACHE_GHA_ENABLED=true so sccache uses GitHub Actions cache as backend.
# Local dev: install sccache from https://github.com/mozilla/sccache (or
# `winget install Mozilla.sccache`) and it is picked up automatically; no
# environment fiddling required.
# -----------------------------------------------------------------------------
option(PYXIS_DISABLE_SCCACHE "Skip the auto-detected sccache compiler launcher." OFF)
if(NOT PYXIS_DISABLE_SCCACHE)
    find_program(PYXIS_SCCACHE_PROGRAM sccache)
    if(PYXIS_SCCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER   "${PYXIS_SCCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${PYXIS_SCCACHE_PROGRAM}")
        message(STATUS "Pyxis: sccache compiler launcher enabled (${PYXIS_SCCACHE_PROGRAM})")
    endif()
endif()

# Read version.txt so project() gets the right number.
pyxis_read_version()

project(Pyxis
    VERSION  ${PYXIS_VERSION_STRING}
    DESCRIPTION "USD-native real-time path tracer"
    HOMEPAGE_URL "https://github.com/pyxis/pyxis"
    LANGUAGES C CXX
)

# Pyxis is Windows-x64-only in v1 (plan §5).
if(NOT WIN32)
    message(FATAL_ERROR "Pyxis v1 is Windows-only — see plan §5. Configure aborted.")
endif()

# Refuse the historical MSVC cl.exe path. Plan §30.1 / §49 mandate clang-cl.
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(WARNING
        "Pyxis is designed for clang-cl (plan §30.1). Detected MSVC cl.exe — "
        "configure may proceed but reviewers expect clang-cl. Use "
        "`cmake --preset dev` (which pins _cmake/clang-cl.toolchain.cmake).")
endif()

# Generate the compile_commands.json database for clang-tidy / IDEs.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output layout: keep all binaries in a single bin/ for the dev convenience
# of running pyxis.exe alongside the SHARED dlls without an install step.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# C++23 across the board (plan §30.1).
set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Build options ---------------------------------------------------------------
option(PYXIS_BUILD_TESTS              "Build pyxis_unit_tests"            ON)
option(PYXIS_BUILD_HYDRA_DELEGATE     "Build pyxis_hydra ingest adapter"  OFF)
option(PYXIS_BUILD_USD_INGEST         "Build pyxis_usd_ingest adapter"    OFF)
option(PYXIS_BUILD_VIEWER             "Build viewer-mode UI (ImGui)"      OFF)
option(PYXIS_DEBUG_TOOLS              "Enable debug tools (Flecs Explorer, hot-reload, etc.)" OFF)
option(PYXIS_ENABLE_AFTERMATH         "Enable NVIDIA Aftermath (Debug-only crash dumps)" OFF)
option(PYXIS_ENABLE_TRACY             "Enable Tracy CPU/GPU profiling client" OFF)

# Plan §37 / §36.2 — clang-tidy gates the build. Wired through
# CMAKE_CXX_CLANG_TIDY so ninja invokes clang-tidy on every C++ TU it
# compiles. Default ON: every build (dev or ci) runs the linter and a
# new diagnostic fails the build, matching the §37 CI table. Pass
# -DPYXIS_ENABLE_CLANG_TIDY=OFF for a fast-iteration build that skips
# the linter (incremental rebuilds only re-run clang-tidy on changed
# TUs anyway, so the steady-state cost is small).
option(PYXIS_ENABLE_CLANG_TIDY        "Run clang-tidy on every C++ TU (matches §37 CI)" ON)
if(PYXIS_ENABLE_CLANG_TIDY)
    find_program(PYXIS_CLANG_TIDY_EXE clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${PYXIS_CLANG_TIDY_EXE}"
        # Honour our repo-root .clang-tidy (§36.2).
        "--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy"
        # Restrict header diagnostics to our own sources so we don't trip on
        # vcpkg / Vulkan / Flecs headers we don't control. The character
        # class `[/\\]` is mandatory: on Windows clang-tidy emits absolute
        # header paths with backslashes for the prefix, so the historical
        # forward-slash-only regex silently skipped every Public/ header
        # and let bad-naming / enum-size issues sneak through the build.
        "--header-filter=.*[/\\\\]sources[/\\\\]pyxis_.*"
        # Plan §37: any new diagnostic fails the build.
        "--warnings-as-errors=*")
    message(STATUS "Pyxis: clang-tidy enabled (${PYXIS_CLANG_TIDY_EXE})")
endif()

# Helpers ---------------------------------------------------------------------
include(Compiler)
include(Vulkan)
include(Thirdparty)

pyxis_thirdparty_setup()

# Subdirectories. Active modules and their landing milestone:
#   M0–M3.5  — pyxis_platform, pyxis_renderer, pyxis_app
#   M4       — pyxis_material_translation, pyxis_hydra, pyxis_usd_ingest
#   M5       — TextureCache + OpenPBR shader expansion (existing module fills)
add_subdirectory(sources/pyxis_platform)
add_subdirectory(sources/pyxis_renderer)

# M4 ingest stack. material_translation must precede hydra +
# usd_ingest because both depend on it. All three pull OpenUSD via
# vcpkg (the ~hour-long compile that lands at M4 — see vcpkg.json's
# `usd[imaging,materialx]` entry + the GHA cache wiring in
# .github/workflows/build.yml).
add_subdirectory(sources/pyxis_material_translation)
add_subdirectory(sources/pyxis_hydra)
add_subdirectory(sources/pyxis_usd_ingest)

add_subdirectory(sources/pyxis_app)

if(PYXIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    # Integration tests (CMake-driven, spawn pyxis.exe). Gated on the
    # pyxis executable being built — the M2 byte-equal-EXR test is an
    # exit-criterion (§41 M2) and must not silently disappear when
    # PYXIS_BUILD_VIEWER is OFF (its default). pyxis.exe ships both
    # viewer and headless modes; presence of the target is the right
    # signal here.
    if(TARGET pyxis)
        add_subdirectory(tests/integration)
    endif()
endif()

# Install ---------------------------------------------------------------------
include(GNUInstallDirs)
install(FILES LICENSE NOTICE README.md DESTINATION .)

message(STATUS "Pyxis ${PYXIS_VERSION_STRING} configured.")

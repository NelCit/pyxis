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
option(PYXIS_DEBUG_TOOLS              "Enable debug tools (Flecs Explorer, hot-reload, etc.)"
       $<IF:$<CONFIG:Debug>,ON,OFF>)
option(PYXIS_ENABLE_AFTERMATH         "Enable NVIDIA Aftermath (Debug-only crash dumps)" OFF)
option(PYXIS_ENABLE_TRACY             "Enable Tracy CPU/GPU profiling client" OFF)

# Helpers ---------------------------------------------------------------------
include(Compiler)
include(Vulkan)
include(Thirdparty)

pyxis_thirdparty_setup()

# Subdirectories — only the M0-active modules wire in for now. Later
# milestones add the rest behind their respective options:
#   M3  — pyxis_renderer/Private/Shaders/, Slang FetchContent
#   M4  — pyxis_hydra (PYXIS_BUILD_HYDRA_DELEGATE), pyxis_usd_ingest, pyxis_material_translation
#   M5  — TextureCache + OpenPBR + the rest of pyxis_renderer
add_subdirectory(sources/pyxis_platform)
add_subdirectory(sources/pyxis_renderer)
add_subdirectory(sources/pyxis_app)

if(PYXIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()

# Install ---------------------------------------------------------------------
include(GNUInstallDirs)
install(FILES LICENSE NOTICE README.md DESTINATION .)

message(STATUS "Pyxis ${PYXIS_VERSION_STRING} configured.")

# Pyxis clang-cl toolchain — plan §30.1 / §49.
#
# This toolchain forces the C++ frontend to clang-cl on Windows and disables
# the implicit MSBuild fallback so that vcpkg-installed dependencies pick up
# the same flags as our targets. Apply via:
#
#   cmake --toolchain _cmake/clang-cl.toolchain.cmake ...
#
# CMakePresets.json pins this toolchain on the `dev` and `dev-release` presets.

# Find clang-cl on PATH (LLVM.LLVM winget package puts it there).
find_program(PYXIS_CLANG_CL clang-cl REQUIRED)

set(CMAKE_C_COMPILER   "${PYXIS_CLANG_CL}" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER "${PYXIS_CLANG_CL}" CACHE STRING "" FORCE)

# clang-cl is an MSVC-driver clang; tell CMake it speaks the MSVC ABI.
set(CMAKE_C_COMPILER_ID   "Clang"        CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_ID "Clang"        CACHE STRING "" FORCE)
set(CMAKE_C_SIMULATE_ID   "MSVC"         CACHE STRING "" FORCE)
set(CMAKE_CXX_SIMULATE_ID "MSVC"         CACHE STRING "" FORCE)

# Pyxis is C++23 across the board (§30.1). CMake projects override
# CMAKE_CXX_STANDARD on individual targets where required (e.g. third-party
# brought in via FetchContent that needs <= C++20).
set(CMAKE_CXX_STANDARD          23 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL   "" FORCE)
set(CMAKE_CXX_EXTENSIONS        OFF CACHE BOOL  "" FORCE)

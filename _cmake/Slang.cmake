# Pyxis Slang shader build helper — plan §10 / §23.
#
# Exposes:
#   pyxis_compile_slang_shader(
#       TARGET         <cmake-target-that-depends-on-the-output>
#       SOURCE         <repo-relative path to .slang>
#       ENTRY_POINT    <name>           # e.g. main / vertexMain
#       STAGE          <vertex|fragment|compute|raygen|closesthit|miss|anyhit>
#       OUTPUT         <path under <bin>/Resources/shaders/...>
#   )
#
# Each call adds a custom command that runs slangc with the §23 invocation
# flags and adds the output .spv to TARGET's source list so ninja tracks
# the dependency. Per §10:
#   -matrix-layout-row-major  (row-major matrices, row-vector mul)
#   -O3                       (release optimisation; Debug overrides to -g -O0)
#   -profile sm_6_6           (SM 6.6 features available)
#   -target spirv             (Vulkan SPIR-V output)
#   -emit-spirv-directly      (no DXC roundtrip)
#
# ShaderMake (the permutation expansion driver per §10) is deferred to M3
# when permutations actually matter. M1 has one .slang per stage with no
# variants.

include_guard(GLOBAL)

function(pyxis_compile_slang_shader)
    set(options "")
    set(oneValueArgs TARGET SOURCE ENTRY_POINT STAGE OUTPUT)
    set(multiValueArgs DEFINES)
    cmake_parse_arguments(SHADER "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SHADER_TARGET)        message(FATAL_ERROR "pyxis_compile_slang_shader: TARGET required") endif()
    if(NOT SHADER_SOURCE)        message(FATAL_ERROR "pyxis_compile_slang_shader: SOURCE required") endif()
    if(NOT SHADER_ENTRY_POINT)   message(FATAL_ERROR "pyxis_compile_slang_shader: ENTRY_POINT required") endif()
    if(NOT SHADER_STAGE)         message(FATAL_ERROR "pyxis_compile_slang_shader: STAGE required") endif()
    if(NOT SHADER_OUTPUT)        message(FATAL_ERROR "pyxis_compile_slang_shader: OUTPUT required") endif()

    pyxis_thirdparty_require_slang()
    if(NOT EXISTS "${PYXIS_SLANG_COMPILER}")
        message(FATAL_ERROR "Pyxis: PYXIS_SLANG_COMPILER not set or missing — run pyxis_thirdparty_setup() first.")
    endif()

    # Map our stage names to slangc's -stage values.
    set(_slangStage "${SHADER_STAGE}")

    # Resolve absolute paths.
    if(NOT IS_ABSOLUTE "${SHADER_SOURCE}")
        set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SOURCE}")
    else()
        set(_src "${SHADER_SOURCE}")
    endif()
    if(NOT IS_ABSOLUTE "${SHADER_OUTPUT}")
        set(_out "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/Resources/shaders/${SHADER_OUTPUT}")
    else()
        set(_out "${SHADER_OUTPUT}")
    endif()

    # Build the slangc command line. The flags mirror §23.
    set(_slangcArgs
        "${_src}"
        -profile sm_6_6
        -target spirv
        -emit-spirv-directly
        -matrix-layout-row-major
        -entry "${SHADER_ENTRY_POINT}"
        -stage "${_slangStage}"
        -o "${_out}"
    )
    foreach(d ${SHADER_DEFINES})
        list(APPEND _slangcArgs -D "${d}")
    endforeach()

    # Debug shaders get -g -O0; Release gets -O3.
    set(_slangcArgsDebug   ${_slangcArgs} -g -O0)
    set(_slangcArgsRelease ${_slangcArgs} -O3)

    # Make sure the output directory exists at build time (the directory
    # is per-config, so we touch it inside the custom command).
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/Resources/shaders"
        COMMAND "${PYXIS_SLANG_COMPILER}"
                $<IF:$<CONFIG:Debug>,${_slangcArgsDebug},${_slangcArgsRelease}>
        DEPENDS "${_src}"
        COMMENT "Compiling Slang shader ${SHADER_SOURCE} (${SHADER_STAGE}/${SHADER_ENTRY_POINT})"
        VERBATIM
        COMMAND_EXPAND_LISTS
    )

    # Attach the .spv as a custom-target source so ninja realises it must
    # be regenerated when the .slang changes, and so the linker target
    # depends on the file existing.
    target_sources(${SHADER_TARGET} PRIVATE "${_out}")
    set_source_files_properties("${_out}" PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
endfunction()

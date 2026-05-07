# Pyxis version generation — plan §22.2.
#
# Reads <repo>/version.txt and:
#   - exposes PYXIS_VERSION_MAJOR / _MINOR / _PATCH / _STRING CMake variables.
#   - generates Public/Pyxis/Renderer/Version.h with the matching macros.
#   - generates a tiny Private/Version.cpp that resolves the git SHA at build
#     time (without baking it into a public macro that defeats ccache).
#
# Re-run: a custom command depends on version.txt + .git/HEAD so a commit /
# rebase invalidates the generated SHA. The git probe is best-effort; on a
# tarball clone with no .git the SHA falls back to "unknown".

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# pyxis_read_version()
#   Reads version.txt; sets PYXIS_VERSION_* in the parent scope.
# ---------------------------------------------------------------------------
function(pyxis_read_version)
    set(_versionFile "${CMAKE_SOURCE_DIR}/version.txt")
    if(NOT EXISTS "${_versionFile}")
        message(FATAL_ERROR "Pyxis: missing version.txt at ${_versionFile} (plan §22.2).")
    endif()

    file(READ "${_versionFile}" _content)
    string(STRIP "${_content}" _content)
    if(NOT _content MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR
            "Pyxis: version.txt must be of the form 'MAJOR.MINOR.PATCH' (got '${_content}').")
    endif()

    set(PYXIS_VERSION_MAJOR  "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(PYXIS_VERSION_MINOR  "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(PYXIS_VERSION_PATCH  "${CMAKE_MATCH_3}" PARENT_SCOPE)
    set(PYXIS_VERSION_STRING "${_content}"      PARENT_SCOPE)
endfunction()


# ---------------------------------------------------------------------------
# pyxis_generate_version_header(targetName)
#   Generates a public header at ${CMAKE_CURRENT_BINARY_DIR}/Generated/Public/
#   and adds it to the named target's PUBLIC include path.
# ---------------------------------------------------------------------------
function(pyxis_generate_version_header targetName)
    if(NOT TARGET ${targetName})
        message(FATAL_ERROR "pyxis_generate_version_header: '${targetName}' is not a target.")
    endif()

    pyxis_read_version()

    set(_outDir "${CMAKE_CURRENT_BINARY_DIR}/Generated/Public/Pyxis/Renderer")
    set(_outHeader "${_outDir}/Version.h")
    file(MAKE_DIRECTORY "${_outDir}")

    configure_file(
        "${CMAKE_SOURCE_DIR}/_cmake/Version.h.in"
        "${_outHeader}"
        @ONLY
    )

    target_include_directories(${targetName}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/Generated/Public>
    )
endfunction()


# ---------------------------------------------------------------------------
# pyxis_generate_version_source(targetName)
#   Generates Version.cpp implementing GetVersionEncoded / GetVersionString /
#   GetVersionGitSha. The git SHA is captured at build time via a custom
#   command; ccache stays warm because only the .cpp recompiles.
# ---------------------------------------------------------------------------
function(pyxis_generate_version_source targetName)
    if(NOT TARGET ${targetName})
        message(FATAL_ERROR "pyxis_generate_version_source: '${targetName}' is not a target.")
    endif()

    pyxis_read_version()

    set(_outDir "${CMAKE_CURRENT_BINARY_DIR}/Generated/Private")
    file(MAKE_DIRECTORY "${_outDir}")

    set(_stamp  "${_outDir}/.version.stamp")
    set(_outCpp "${_outDir}/Version.cpp")

    set(_gitDeps)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.git/HEAD")
        list(APPEND _gitDeps "${CMAKE_SOURCE_DIR}/.git/HEAD")
    endif()

    add_custom_command(
        OUTPUT  "${_outCpp}" "${_stamp}"
        COMMAND ${CMAKE_COMMAND}
                -D "PYXIS_VERSION_STRING=${PYXIS_VERSION_STRING}"
                -D "PYXIS_VERSION_ENCODED_MAJOR=${PYXIS_VERSION_MAJOR}"
                -D "PYXIS_VERSION_ENCODED_MINOR=${PYXIS_VERSION_MINOR}"
                -D "PYXIS_VERSION_ENCODED_PATCH=${PYXIS_VERSION_PATCH}"
                -D "PYXIS_VERSION_OUT=${_outCpp}"
                -D "PYXIS_VERSION_REPO=${CMAKE_SOURCE_DIR}"
                -P "${CMAKE_SOURCE_DIR}/_cmake/Version.write.cmake"
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS "${CMAKE_SOURCE_DIR}/version.txt" ${_gitDeps}
                "${CMAKE_SOURCE_DIR}/_cmake/Version.write.cmake"
        VERBATIM
    )

    target_sources(${targetName} PRIVATE "${_outCpp}")
endfunction()

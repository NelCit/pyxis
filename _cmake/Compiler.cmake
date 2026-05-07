# Pyxis compiler helpers — plan §30.1 / §30.3 / §49.
#
# Exposes:
#   pyxis_set_target_compile_options(target)        — /W4 /WX, MSVC-equiv warnings,
#                                                     /permissive-, /Zc:* tightening,
#                                                     C++23, /utf-8.
#   pyxis_treat_thirdparty_as_external(target)      — /external:I includes for the
#                                                     given target so vendored deps
#                                                     don't trip /W4 /WX.
#   pyxis_define_module(name)                       — declares Public/ + Private/
#                                                     include dirs, sets the
#                                                     PYXIS_<NAME>_API export macro.
#   pyxis_disable_exceptions_and_rtti(target)       — /EHs-c- + /GR- for the
#                                                     renderer / platform perimeter.
#   pyxis_enable_exceptions_and_rtti(target)        — /EHsc + /GR for the Hydra /
#                                                     USD ingest layers (USD needs
#                                                     both — §30.1).

include_guard(GLOBAL)

# -----------------------------------------------------------------------------
# pyxis_set_target_compile_options
# -----------------------------------------------------------------------------
function(pyxis_set_target_compile_options target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "pyxis_set_target_compile_options: '${target}' is not a target.")
    endif()

    target_compile_features(${target} PUBLIC cxx_std_23)

    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /utf-8

            # clang-cl extras: warning parity, MSVC-style diag format, helpful
            # extras that the GCC -Wall -Wextra path also gives us. Long-form
            # POSIX flags (-f*) go through /clang: because the clang-cl driver
            # rejects them otherwise (and we want -Werror=unknown-argument to
            # stay on so genuinely-bad flags fail builds).
            $<$<CXX_COMPILER_ID:Clang>:-Wno-microsoft-include>
            $<$<CXX_COMPILER_ID:Clang>:-Wno-pragma-pack>
            # Newer clang-cl already implements many MSVC `/Zc:*` modes by
            # default and warns when the user repeats them. We keep the flags
            # because §30.1 mandates them on cl.exe, but suppress the noise.
            $<$<CXX_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>
            # -Wmissing-field-initializers is kept on. Vulkan struct sites
            # use the zero-init + .sType = … pattern so every field is
            # explicitly initialised; reviewers reject the
            # `{ VK_STRUCTURE_TYPE_FOO }` shorthand.
            $<$<CXX_COMPILER_ID:Clang>:/clang:-Werror=date-time>     # §46.2 reproducible builds
            $<$<CXX_COMPILER_ID:Clang>:/clang:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.>  # §46.2 PDB hygiene
        )
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()

    # Make sure consumers of the target use the same C++ standard.
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD          23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )
endfunction()


# -----------------------------------------------------------------------------
# pyxis_treat_thirdparty_as_external
# -----------------------------------------------------------------------------
function(pyxis_treat_thirdparty_as_external target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "pyxis_treat_thirdparty_as_external: '${target}' is not a target.")
    endif()
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /external:W0
            /external:anglebrackets
        )
    endif()
endfunction()


# -----------------------------------------------------------------------------
# pyxis_disable_exceptions_and_rtti
#   §30.1 — renderer / platform perimeter has no exceptions, no RTTI.
# -----------------------------------------------------------------------------
function(pyxis_disable_exceptions_and_rtti target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "pyxis_disable_exceptions_and_rtti: '${target}' is not a target.")
    endif()
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        # Strip any default /EHsc / /GR before adding the negative options so
        # both forms cannot coexist on the command line.
        target_compile_options(${target} PRIVATE
            /EHs-c-
            /GR-
            /Zc:throwingNew   # §49 — keep operator new's standard contract.
        )
        target_compile_definitions(${target} PRIVATE
            PYXIS_NO_EXCEPTIONS=1
            _HAS_EXCEPTIONS=0
        )
    endif()
endfunction()


# -----------------------------------------------------------------------------
# pyxis_enable_exceptions_and_rtti
#   §30.1 — Hydra / USD ingest / material translation need both.
# -----------------------------------------------------------------------------
function(pyxis_enable_exceptions_and_rtti target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "pyxis_enable_exceptions_and_rtti: '${target}' is not a target.")
    endif()
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /EHsc
            /GR
        )
    endif()
endfunction()


# -----------------------------------------------------------------------------
# pyxis_define_module
#   Standard layout for a Pyxis CMake target:
#     <module>/Public/  ─ public headers, narrow surface
#     <module>/Private/ ─ everything else
#
#   Sets:
#     - target_include_directories(... PUBLIC Public/  PRIVATE Private/)
#     - PYXIS_<NAME>_API + PYXIS_<NAME>_BUILDING_DLL define for SHARED targets.
# -----------------------------------------------------------------------------
function(pyxis_define_module target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "pyxis_define_module: '${target}' is not a target.")
    endif()

    string(TOUPPER "${target}" upper)
    string(REPLACE "PYXIS_" "" upperShort "${upper}")

    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Public>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Private
    )

    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "SHARED_LIBRARY")
        target_compile_definitions(${target}
            PRIVATE PYXIS_${upperShort}_BUILDING_DLL
            INTERFACE PYXIS_${upperShort}_USING_DLL
        )
        set_target_properties(${target} PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS OFF
        )
    endif()

    pyxis_set_target_compile_options(${target})
endfunction()

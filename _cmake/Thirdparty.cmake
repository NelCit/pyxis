# Pyxis third-party fetch — plan §4 / §49.
#
# Single source of truth for FetchContent-driven dependencies. vcpkg
# manifest mode (vcpkg.json) handles spdlog, glfw, gtest, nlohmann-json,
# tinyexr, stb, mikktspace, hlslpp, tracy, moodycamel-concurrentqueue,
# flecs (with the rest feature in Debug — plan §4 / §30.11).
#
# This file declares the deps that we build from source so we control the
# configuration:
#   - NVRHI       (Vulkan backend only)
#   - Slang        (M3+; needed for shader compile pipeline)
#   - ShaderMake   (M3+; drives Slang)
#   - OpenUSD      (M4+; ingest adapters)
#   - MaterialX    (M5+; via OpenUSD or direct)
#
# CI's _tools/check_pins.py walks this file and rejects any GIT_TAG that is
# shorter than 40 chars or matches master|main|HEAD (plan §49). PRs that
# bump a SHA must update vcpkg.json's builtin-baseline in lockstep.

include_guard(GLOBAL)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# ---------------------------------------------------------------------------
# pyxis_thirdparty_setup() — declare every fetch and call MakeAvailable for
# the deps the active milestone actually needs. M0 only needs NVRHI; M3+
# pulls in Slang/ShaderMake; M4+ pulls in OpenUSD/MaterialX. The granular
# triggers live in their respective module CMakeLists.txt.
# ---------------------------------------------------------------------------
function(pyxis_thirdparty_setup)
    # ── NVRHI ──────────────────────────────────────────────────────────────
    # Pin to a known-good NVRHI release. Update by reviewing
    # https://github.com/NVIDIAGameWorks/nvrhi/releases and pinning a full
    # 40-char SHA (plan §49 rule). Using a placeholder SHA here so M0 builds
    # don't accidentally drift; bump in a follow-up commit when the real
    # baseline is chosen.
    FetchContent_Declare(
        nvrhi
        GIT_REPOSITORY https://github.com/NVIDIAGameWorks/nvrhi.git
        GIT_TAG        0000000000000000000000000000000000000000  # TODO(@pyxis-build-team, §49): pin
        GIT_SHALLOW    FALSE
    )
    set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
    # M0 doesn't actually need NVRHI's source until the platform layer wires
    # the device manager to it. The declare-only call here keeps configure
    # fast; pyxis_platform/CMakeLists.txt calls FetchContent_MakeAvailable
    # when the platform target is built.
endfunction()

# ---------------------------------------------------------------------------
# pyxis_thirdparty_require_nvrhi() — pulled in by pyxis_platform.
# ---------------------------------------------------------------------------
function(pyxis_thirdparty_require_nvrhi)
    if(NOT TARGET nvrhi_vk)
        FetchContent_MakeAvailable(nvrhi)
    endif()
endfunction()

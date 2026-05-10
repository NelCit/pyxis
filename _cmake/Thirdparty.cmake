# Pyxis third-party fetch — plan §4 / §49.
#
# Single source of truth for FetchContent-driven dependencies. vcpkg
# manifest mode (vcpkg.json) handles spdlog, fmt, glfw3, gtest, nlohmann-
# json, tinyexr, stb, mikktspace, hlslpp, tracy, concurrentqueue, flecs,
# imgui (with docking + vulkan-binding + glfw-binding features).
#
# This file declares the deps that we build / unpack from source so we
# control configuration:
#   - NVRHI       — Vulkan backend only, static link.   M1+
#   - Slang       — prebuilt Windows binaries from GitHub Releases. M1+ (pulled
#                   early; the plan §41 originally landed Slang at M3 but
#                   we get the build pipeline in earlier).
#   - ShaderMake  — declared at M3 (permutation expansion driver).
#   - OpenUSD     — M4+ (ingest adapters).
#   - MaterialX   — M5+ (material translation).
#
# CI's _tools/check_pins.py walks this file and rejects any GIT_TAG that
# is shorter than 40 chars or matches master|main|HEAD (plan §49). PRs
# that bump a SHA must update vcpkg.json's builtin-baseline in lockstep.

include_guard(GLOBAL)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# ===========================================================================
# Pinned versions (single source of truth — bump together with any
# matching vcpkg.json baseline change; CI gates on _tools/check_pins.py).
# ===========================================================================

# NVRHI — Vulkan-backed RHI. Pinned to a recent commit on main (NVRHI does
# not cut versioned tags). Bump by reviewing
# https://github.com/NVIDIAGameWorks/nvrhi/commits/main and updating both
# the GIT_TAG and any code change that depends on the new behaviour.
set(PYXIS_NVRHI_GIT_TAG "54100464714de88a5a5059d25808f5ccb914ad7d")

# ImGui — docking branch. Fetched via FetchContent because vcpkg's
# imgui[vulkan-binding] feature transitively pulls vulkan-loader whose
# archive download on github.com is 502'ing intermittently. Source-tree
# fetch lets us compile the imgui_impl_vulkan.cpp + imgui_impl_glfw.cpp
# backends directly against our system Vulkan SDK + the vcpkg glfw3.
set(PYXIS_IMGUI_GIT_TAG "c51f1a6e47b8b5b11ca13490c461842c96bc4ca2")

# ImPlot was attempted at v0.16 (the last tagged release) but its
# source uses three ImGui API surfaces ImGui 1.92 removed
# (AddPolyline arg order, ImGuiColorEditFlags_AlphaPreview,
# ImGuiWindow::CalcFontSize). Bumping ImPlot to a main-HEAD SHA
# that's been updated for ImGui 1.92 needs an internet-verifiable
# pinning we don't yet have. Tracked as a follow-up; until then
# the perf-panel chart uses an ImDrawList-based custom drawer
# (see ImGuiHost::BuildFpsPanel) which gives us multi-series
# overlay + auto-scaling without the dep.

# Slang — prebuilt Windows x86_64 binary release. Pulled via URL mode so
# we don't pay Slang's full source build (~10–15 min on a fresh clone).
# slangc.exe lives at <slang_root>/bin/slangc.exe after unpack.
set(PYXIS_SLANG_VERSION "2026.8")
set(PYXIS_SLANG_URL
    "https://github.com/shader-slang/slang/releases/download/v${PYXIS_SLANG_VERSION}/slang-${PYXIS_SLANG_VERSION}-windows-x86_64.zip")
# URL_HASH is intentionally unset for now — pin the SHA256 in a follow-up
# once we have a known-clean reference download. CI's __pyxis-strict__
# branch will fail without it; main is tolerant.

# ===========================================================================
# pyxis_thirdparty_setup() — declarations only. Each consumer module calls
# the pyxis_thirdparty_require_<name>() helper below at first use.
# ===========================================================================
function(pyxis_thirdparty_setup)
    FetchContent_Declare(
        nvrhi
        GIT_REPOSITORY https://github.com/NVIDIAGameWorks/nvrhi.git
        GIT_TAG        ${PYXIS_NVRHI_GIT_TAG}
        GIT_SHALLOW    FALSE
    )
    set(NVRHI_INSTALL        OFF CACHE BOOL "" FORCE)
    set(NVRHI_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VULKAN    ON  CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX11      OFF CACHE BOOL "" FORCE)
    set(NVRHI_WITH_DX12      OFF CACHE BOOL "" FORCE)
    # RTXMU = NVIDIA RTX Memory Utility. With this on, NVRHI routes
    # every BLAS through RTXMU's suballocation pool + scratch pool +
    # async compaction queue (plan §16). NVRHI public API is
    # unchanged; the consumer-side win is "we set ALLOW_COMPACTION
    # at build time, RTXMU does the compaction copy when the GPU
    # retires the build". RTXMU is vendored as a submodule inside
    # nvrhi/rtxmu/ — no extra fetch needed.
    set(NVRHI_WITH_RTXMU     ON  CACHE BOOL "" FORCE)
    set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        slang_prebuilt
        URL ${PYXIS_SLANG_URL}
    )

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        ${PYXIS_IMGUI_GIT_TAG}
        GIT_SHALLOW    FALSE
    )
endfunction()


# ===========================================================================
# pyxis_thirdparty_require_nvrhi() — pulled in by pyxis_platform's device
# manager (it wraps a VkDevice into nvrhi::IDevice).
# Disables clang-tidy + /WX inside NVRHI's source tree so we don't gate
# our build on third-party diagnostics.
# ===========================================================================
function(pyxis_thirdparty_require_nvrhi)
    if(TARGET nvrhi)
        return()
    endif()

    # Save + clear CMAKE_CXX_CLANG_TIDY so clang-tidy doesn't lint NVRHI's
    # sources. The variable is a *directory property* once set, so clearing
    # it for the FetchContent subdirectory's lifetime is enough.
    set(_savedTidy "${CMAKE_CXX_CLANG_TIDY}")
    set(CMAKE_CXX_CLANG_TIDY "")

    FetchContent_MakeAvailable(nvrhi)

    # Pyxis-local NVRHI patch: gate the NV linear-swept-spheres
    # pipeline-create flag on the extension being enabled.
    #
    # Upstream NVRHI (commit 54100464) unconditionally chains a
    # PipelineCreateFlags2CreateInfoKHR struct with the
    # `eRayTracingAllowSpheresAndLinearSweptSpheresNV` bit set into
    # *every* RT pipeline create. The struct itself requires
    # VK_KHR_maintenance5 and the flag bit requires
    # VK_NV_ray_tracing_linear_swept_spheres — Pyxis pulls in neither
    # (LSS is not exposed on most consumer Ada+ drivers, e.g. NVIDIA
    # 596.36 / RTX 4070 Laptop), so vkCreateRayTracingPipelinesKHR
    # fails on every ray-tracing pipeline creation.
    #
    # Upstream PR (drop this block when the SHA bump pulls it in):
    #     https://github.com/NVIDIA-RTX/NVRHI fix/lss-flag-gating
    #
    # The fix mirrors the existing NV_cluster_acceleration_structure
    # gate a few lines below: condition the flag-set and the pNext
    # chain on the extension being enabled. Two string-replaces; both
    # are idempotent because the buggy substring is gone after the
    # first successful run.
    #
    # Hardening: if the file exists but neither the buggy nor the
    # already-patched form is present, the upstream source has
    # reshaped this function (a SHA bump landed an unrelated change to
    # the same lines). FATAL_ERROR rather than silently producing a
    # build that crashes at first RT pipeline create.
    set(_rtCpp "${nvrhi_SOURCE_DIR}/src/vulkan/vulkan-raytracing.cpp")
    if(EXISTS "${_rtCpp}")
        file(READ "${_rtCpp}" _rtCppContent)
        set(_buggyFlagSet
"        auto pipelineFlags2 = vk::PipelineCreateFlags2CreateInfoKHR();
        pipelineFlags2.setFlags(vk::PipelineCreateFlagBits2::eRayTracingAllowSpheresAndLinearSweptSpheresNV);")
        set(_fixedFlagSet
"        auto pipelineFlags2 = vk::PipelineCreateFlags2CreateInfoKHR();
        if (m_Context.extensions.NV_ray_tracing_linear_swept_spheres) pipelineFlags2.setFlags(vk::PipelineCreateFlagBits2::eRayTracingAllowSpheresAndLinearSweptSpheresNV);")
        set(_buggyPNextChain
"            .setPLibraryInfo(&libraryInfo)
            .setPNext(&pipelineFlags2);")
        set(_fixedPNextChain
"            .setPLibraryInfo(&libraryInfo);
        if (m_Context.extensions.NV_ray_tracing_linear_swept_spheres) pipelineInfo.setPNext(&pipelineFlags2);")
        string(FIND "${_rtCppContent}" "${_buggyFlagSet}" _buggyAt)
        string(FIND "${_rtCppContent}" "${_fixedFlagSet}" _fixedAt)
        if(_buggyAt GREATER -1)
            string(REPLACE "${_buggyFlagSet}"  "${_fixedFlagSet}"  _rtCppContent "${_rtCppContent}")
            string(REPLACE "${_buggyPNextChain}" "${_fixedPNextChain}" _rtCppContent "${_rtCppContent}")
            file(WRITE "${_rtCpp}" "${_rtCppContent}")
            message(STATUS "Pyxis: NVRHI patched (NV-LSS pipeline-create flag gated on extension)")
        elseif(_fixedAt GREATER -1)
            message(STATUS "Pyxis: NVRHI already patched (NV-LSS gate present); nothing to do")
        else()
            message(FATAL_ERROR
                "Pyxis: NVRHI source at ${_rtCpp} matches neither the buggy nor the "
                "patched form of the NV-LSS pipeline-create gate. The upstream commit "
                "(PYXIS_NVRHI_GIT_TAG=${PYXIS_NVRHI_GIT_TAG}) likely reshaped "
                "createRayTracingPipeline. Investigate before continuing — building "
                "without the gate produces a binary that crashes at the first RT "
                "pipeline create on any device that does not advertise "
                "VK_NV_ray_tracing_linear_swept_spheres.")
        endif()
    else()
        message(FATAL_ERROR
            "Pyxis: NVRHI source path not found: ${_rtCpp}. NVRHI's source layout "
            "changed and the LSS gate patch can no longer locate its target. Update "
            "this patch block in _cmake/Thirdparty.cmake before continuing.")
    endif()

    # NVRHI's targets aren't ours; strip clang-tidy + suppress its own
    # /W4 /WX warnings if any leaked out (NVRHI is /W3 internally).
    foreach(t IN ITEMS nvrhi nvrhi_vk vma)
        if(TARGET ${t})
            set_target_properties(${t} PROPERTIES CXX_CLANG_TIDY "")
            if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
                target_compile_options(${t} PRIVATE /W3 /WX-)
            endif()
        endif()
    endforeach()

    # Restore clang-tidy for downstream targets.
    set(CMAKE_CXX_CLANG_TIDY "${_savedTidy}" PARENT_SCOPE)

    if(NOT TARGET nvrhi_vk)
        message(FATAL_ERROR
            "Pyxis: NVRHI fetch succeeded but nvrhi_vk target is missing. "
            "Did NVRHI rename the Vulkan backend target?")
    endif()
    message(STATUS "Pyxis: NVRHI ready (nvrhi + nvrhi_vk)")
endfunction()


# ===========================================================================
# pyxis_thirdparty_require_slang() — pulled in by the renderer's shader
# build rule. Exposes PYXIS_SLANG_COMPILER as a CACHE FILEPATH that
# `_cmake/Slang.cmake`'s pyxis_compile_slang_shader() reads.
# Build-tool only; we do NOT link Slang into Pyxis at M1.
# ===========================================================================
function(pyxis_thirdparty_require_slang)
    if(DEFINED CACHE{PYXIS_SLANG_COMPILER} AND EXISTS "$CACHE{PYXIS_SLANG_COMPILER}")
        return()
    endif()

    FetchContent_MakeAvailable(slang_prebuilt)

    # The release zip unpacks to <root>/{bin,include,lib,docs}.
    set(_slangc "${slang_prebuilt_SOURCE_DIR}/bin/slangc.exe")
    if(NOT EXISTS "${_slangc}")
        message(FATAL_ERROR
            "Pyxis: Slang prebuilt unpacked at ${slang_prebuilt_SOURCE_DIR} "
            "but slangc.exe not found at expected path ${_slangc}. "
            "Did the release zip layout change?")
    endif()
    set(PYXIS_SLANG_COMPILER "${_slangc}" CACHE FILEPATH "Slang compiler (slangc.exe)" FORCE)
    set(PYXIS_SLANG_INCLUDE  "${slang_prebuilt_SOURCE_DIR}/include" CACHE PATH "Slang include dir" FORCE)
    message(STATUS "Pyxis: Slang ${PYXIS_SLANG_VERSION} at ${PYXIS_SLANG_COMPILER}")
endfunction()


# ===========================================================================
# pyxis_thirdparty_require_hlslpp() — pulled in by pyxis_renderer's public
# headers (MeshDesc / InstanceDesc / CameraDesc use hlslpp::float3 /
# float4 / float4x4 per plan §10 / §23). Header-only.
#
# vcpkg ships an `unofficial::hlslpp::hlslpp` target via
# `share/hlslpp/hlslpp-config.cmake`, but as of vcpkg port 3.8 that
# config has a typo (`INTERACE IMPORTED` instead of `INTERFACE
# IMPORTED`) which makes find_package(hlslpp CONFIG) silently produce
# no usable target. Locate the umbrella header directly with
# find_path() and wrap it in our own INTERFACE target so we don't
# depend on upstream fixing the typo. When vcpkg corrects it we can
# switch back to find_package without changing call sites.
# ===========================================================================
function(pyxis_thirdparty_require_hlslpp)
    if(TARGET pyxis_hlslpp)
        return()
    endif()

    # vcpkg ships the umbrella header at
    # ${VCPKG_INSTALLED_DIR}/<triplet>/include/hlslpp/hlsl++.h, one level
    # below the standard include root. PATH_SUFFIXES "hlslpp" tells
    # find_path() to look one level deeper than the prefix paths
    # CMAKE_PREFIX_PATH already includes.
    find_path(PYXIS_HLSLPP_INCLUDE
        NAMES         "hlsl++.h"
        PATH_SUFFIXES "hlslpp"
        DOC           "Directory containing hlsl++.h (vcpkg ships it under include/hlslpp/)")
    if(NOT PYXIS_HLSLPP_INCLUDE)
        message(FATAL_ERROR
            "Pyxis: hlslpp headers not found. Expected vcpkg to install "
            "include/hlslpp/hlsl++.h — is hlslpp listed in vcpkg.json?")
    endif()

    add_library(pyxis_hlslpp INTERFACE)
    target_include_directories(pyxis_hlslpp SYSTEM INTERFACE "${PYXIS_HLSLPP_INCLUDE}")
    message(STATUS "Pyxis: hlslpp at ${PYXIS_HLSLPP_INCLUDE}")
endfunction()


# ===========================================================================
# pyxis_thirdparty_require_imgui() — pulled in by pyxis_app's ViewerMode.
# Builds a `pyxis_imgui` static library combining ImGui's core + the
# Vulkan + GLFW backends. `find_package(Vulkan)` and `find_package(glfw3)`
# must already have been called by the caller (pyxis_platform's CMake).
# ===========================================================================
function(pyxis_thirdparty_require_imgui)
    if(TARGET pyxis_imgui)
        return()
    endif()

    set(_savedTidy "${CMAKE_CXX_CLANG_TIDY}")
    set(CMAKE_CXX_CLANG_TIDY "")
    FetchContent_MakeAvailable(imgui)

    add_library(pyxis_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    )
    target_include_directories(pyxis_imgui SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_compile_definitions(pyxis_imgui PUBLIC
        IMGUI_DISABLE_OBSOLETE_FUNCTIONS
        IMGUI_DISABLE_OBSOLETE_KEYIO
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES   # we link Vulkan via the Vulkan SDK loader
    )
    target_link_libraries(pyxis_imgui PUBLIC Vulkan::Vulkan glfw)

    set_target_properties(pyxis_imgui PROPERTIES CXX_CLANG_TIDY "")
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(pyxis_imgui PRIVATE /W3 /WX- /wd4244 /wd4267)
    endif()

    set(CMAKE_CXX_CLANG_TIDY "${_savedTidy}" PARENT_SCOPE)
    message(STATUS "Pyxis: ImGui ready (pyxis_imgui static lib with Vulkan + GLFW backends)")
endfunction()

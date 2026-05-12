# pyxis_renderer — plan §1 / §3.
#
# SHARED. flecs is linked PRIVATE so no Flecs header escapes Public/ (§30.11).
# spdlog is reached through pyxis::Logging::Get() in pyxis_platform; we do
# not link spdlog directly here (§33.10).

add_library(pyxis_renderer SHARED)
add_library(Pyxis::Renderer ALIAS pyxis_renderer)

pyxis_define_module(pyxis_renderer)
pyxis_disable_exceptions_and_rtti(pyxis_renderer)

# ----------------------------------------------------------------------------
# Public surface (M0 slice — narrow). Plan §18.1 lists the eventual
# inventory; M0 ships only what the SceneWorldInit unit test, the platform
# device manager and pyxis_app actually need.
# ----------------------------------------------------------------------------
target_sources(pyxis_renderer PUBLIC
    FILE_SET pyxis_renderer_public TYPE HEADERS
    BASE_DIRS Public
    FILES
        Public/Pyxis/Renderer/RendererApi.h
        Public/Pyxis/Renderer/Forward.h
        Public/Pyxis/Renderer/SceneWorldFacade.h

        # M1 public surface.
        Public/Pyxis/Renderer/Profiler.h
        Public/Pyxis/Renderer/PyxisRenderer.h
        Public/Pyxis/Renderer/Descs/FrameProfile.h
        Public/Pyxis/Renderer/Descs/RendererCreateDesc.h
        Public/Pyxis/Renderer/Descs/RenderSettings.h
        Public/Pyxis/Renderer/Descs/RenderTargets.h

        # M3 public surface — foundation for Expected<T>-returning verbs.
        Public/Pyxis/Renderer/Error.h

        # M3 public Desc PODs. Byte-stable per §18.9 / §22.3 — adding
        # fields requires the trailing _reserved trick; reordering or
        # narrowing types is a major-version break. See each header
        # for milestone-specific notes (M3 only consumes the base
        # block of OpenPBRMaterialDesc, etc.).
        Public/Pyxis/Renderer/Descs/MeshDesc.h
        Public/Pyxis/Renderer/Descs/InstanceDesc.h
        Public/Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h
        Public/Pyxis/Renderer/Descs/CameraDesc.h
        Public/Pyxis/Renderer/Descs/LightDesc.h
        Public/Pyxis/Renderer/Descs/TextureKey.h
        Public/Pyxis/Renderer/Descs/GpuSceneCreateDesc.h
        Public/Pyxis/Renderer/Descs/FrameStats.h

        # M3 GpuScene — canonical scene-mutation API. Stub impl in
        # Private/GpuScene/GpuScene.cpp; real bodies land in subsequent
        # M3 commits.
        Public/Pyxis/Renderer/GpuScene.h
)

# ----------------------------------------------------------------------------
# Private — SceneWorld and the Flecs phase pipeline (§8 / §30.11).
# ----------------------------------------------------------------------------
target_sources(pyxis_renderer PRIVATE
    # M3 — Error / PYXIS_ERROR implementation (foundation for Expected<T>).
    Private/Errors/Error.cpp

    # M3 — GpuScene canonical scene-mutation API. PIMPL'd per §18.9;
    # mesh upload + BLAS build + TLAS rebuild are wired here at M3,
    # materials + textures fill in at M5+. Per-verb bodies are split
    # into sibling files; Internal.h holds the shared `struct Impl` +
    # hashing/packing helpers every verb file needs.
    Private/GpuScene/Internal.h
    Private/GpuScene/GpuScene.cpp
    Private/GpuScene/Mesh.cpp
    Private/GpuScene/Material.cpp
    Private/GpuScene/Texture.cpp
    Private/GpuScene/Instance.cpp
    Private/GpuScene/Light.cpp
    Private/GpuScene/Commit.cpp
    Private/GpuScene/DdsParser.h
    Private/GpuScene/DdsParser.cpp

    # M1 — RenderGraph + Passes + Profiler + Renderer.
    Private/Diagnostics/CommandListMarker.h

    Private/RenderGraph/IRenderPass.h
    Private/RenderGraph/PassContext.h
    Private/RenderGraph/RenderGraph.h
    Private/RenderGraph/RenderGraph.cpp

    Private/Passes/PathTracePass.h
    Private/Passes/PathTracePass.cpp

    Private/Profiler/Profiler.cpp

    Private/Renderers/PyxisRenderer.cpp

    # M5 — stb_image IMPLEMENTATION TU. The renderer's TextureCache
    # decodes 8/16-bit PNG/JPG/etc via stb_image; the IMPLEMENTATION
    # macro must land in EXACTLY one TU per binary. Same pattern
    # pyxis_app uses for stb_image_write (its --screenshot path).
    # Single-source-of-truth lives at Private/Materials/StbImageRead.cpp.
    Private/Materials/StbImageRead.cpp
    Private/Materials/MaterialFlag.h

    # Scene/.
    Private/Scene/World.h
    Private/Scene/World.cpp
    Private/Scene/Phases.h
    Private/Scene/Phases.cpp
    Private/Scene/SceneWorldFacade.cpp

    Private/Scene/HandleBimap.h
    Private/Scene/HandleBimap.cpp

    Private/Scene/Components/Geom.h
    Private/Scene/Components/Transform.h
    Private/Scene/Components/Visibility.h
    Private/Scene/Components/MaterialRef.h
    Private/Scene/Components/MeshRef.h
    Private/Scene/Components/BlasRef.h
    Private/Scene/Components/MaterialGpu.h
    Private/Scene/Components/TextureGpu.h
    Private/Scene/Components/LightParams.h
    Private/Scene/Components/Dirty.h

    Private/Scene/Systems/Pipeline.h
    Private/Scene/Systems/Pipeline.cpp
    Private/Scene/Systems/UploadDirtyTextures.cpp
    Private/Scene/Systems/UploadDirtyMaterials.cpp
    Private/Scene/Systems/ExtractDirtyMeshes.cpp
    Private/Scene/Systems/BuildDirtyBlas.cpp
    Private/Scene/Systems/RebuildTlas.cpp
    Private/Scene/Systems/UpdateBindlessTable.cpp
    Private/Scene/Systems/ClearDirtyFlags.cpp

    Private/Scene/Queries/QueryCache.h
    Private/Scene/Queries/QueryCache.cpp
)

# ----------------------------------------------------------------------------
# Dependencies.
# ----------------------------------------------------------------------------
find_package(flecs CONFIG REQUIRED)

# hlslpp is header-only and lives in our public Descs (MeshDesc /
# InstanceDesc / CameraDesc use float3 / float4x4). Linked PUBLIC so
# consumers (pyxis_app, pyxis_unit_tests, future pyxis_hydra) see the
# include path.
pyxis_thirdparty_require_hlslpp()

# Flecs is PRIVATE — never propagated to dependents (plan §49). hlslpp
# is PUBLIC — it leaks through Public/Descs/* and consumers must see it.
# resources/shaders/ is on the private include path so C++ TUs can
# `#include "ShaderInterop.slang"` and pick up the shared structs
# (CameraUniforms, HitInfo, ...) directly. The Slang file branches on
# __cplusplus, so the same header satisfies both the slangc shader
# build and the renderer's C++ side without a hand-maintained C++
# shadow.
target_include_directories(pyxis_renderer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/resources/shaders
)

# stb_image is header-only; vcpkg's `Stb` config exports the include
# dir as Stb_INCLUDE_DIR (no link target). pyxis_app already pulls
# this for stb_image_write; pyxis_renderer pulls it independently
# for stb_image (decode side).
find_package(Stb REQUIRED)
target_include_directories(pyxis_renderer PRIVATE ${Stb_INCLUDE_DIR})

# tinyexr — HDR / EXR decode for M7 dome environment maps. pyxis_app
# already links it for EXR output; the renderer pulls it independently
# for the input / decode side.
find_package(tinyexr CONFIG REQUIRED)

# Tracy — optional CPU/GPU profiling backend. OFF by default (plan
# §34.2: "Tracy (optional, off by default)"); enable per-build via
# `cmake -DPYXIS_TRACY=ON`. When ON, the renderer links against the
# vcpkg-built Tracy::TracyClient SHARED library and the Profiler's
# RAII scopes feed Tracy zones in addition to the in-process ring.
# When OFF, the Tracy include is unreachable and the zone macros
# compile to nothing (TRACY_ENABLE undefined) — zero overhead.
option(PYXIS_TRACY "Enable the Tracy profiler client (CPU + GPU zones)." OFF)
if(PYXIS_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_compile_definitions(pyxis_renderer PRIVATE PYXIS_TRACY=1 TRACY_ENABLE=1)
    target_link_libraries(pyxis_renderer PRIVATE Tracy::TracyClient)
    message(STATUS "Pyxis: Tracy profiler enabled (Tracy::TracyClient linked).")
endif()

# stb_image's IMPLEMENTATION TU has missing-field-initializer
# patterns inside the third-party header (same as stb_image_write
# in pyxis_app). Disable the warning + clang-tidy on that single
# file rather than relax it project-wide.
set_source_files_properties(Private/Materials/StbImageRead.cpp PROPERTIES
    COMPILE_OPTIONS    "-Wno-missing-field-initializers"
    SKIP_LINTING       ON)

target_link_libraries(pyxis_renderer
    PUBLIC
        Pyxis::Platform
        pyxis_hlslpp
    PRIVATE
        $<TARGET_NAME_IF_EXISTS:flecs::flecs_static>
        $<TARGET_NAME_IF_EXISTS:flecs::flecs>
        unofficial::tinyexr::tinyexr
)

# Debug-only: PYXIS_DEBUG_TOOLS toggles Flecs Explorer (§30.11).
target_compile_definitions(pyxis_renderer PRIVATE
    $<$<BOOL:${PYXIS_DEBUG_TOOLS}>:PYXIS_DEBUG_TOOLS=1>
)

# Versioning (§22.2): pyxis_renderer.dll is the canonical owner of the
# generated Version.h / Version.cpp. Version.h is PYXIS_RENDERER_API
# decorated, so it lives here, not in pyxis_platform.
include(Version)
pyxis_generate_version_header(pyxis_renderer)
pyxis_generate_version_source(pyxis_renderer)

# Shaders — M3 ray-tracing pipeline (raygen / miss / closesthit)
# compiled by Slang at build time (plan §10 / §23). The .spv lands in
# <bin>/Resources/shaders/ and is loaded by PathTracePass at startup.
include(Slang)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/raygen.slang
    ENTRY_POINT main
    STAGE       raygen
    OUTPUT      raygen.spv
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/miss.slang
    ENTRY_POINT main
    STAGE       miss
    OUTPUT      miss.spv
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/closesthit.slang
    ENTRY_POINT main
    STAGE       closesthit
    OUTPUT      closesthit.spv
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/anyhit.slang
    ENTRY_POINT main
    STAGE       anyhit
    OUTPUT      anyhit.spv
)
# M9-fidelity shadow rays — second miss shader stitched into the
# shader binding table at miss-index 1. Closesthit traces a shadow
# ray per direct light with RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
# RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH; this miss writes
# payload.color=1.0 (=visible) iff no opaque hit blocks the path.
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/shadow_miss.slang
    ENTRY_POINT main
    STAGE       miss
    OUTPUT      shadow_miss.spv
)

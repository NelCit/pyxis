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
)

# ----------------------------------------------------------------------------
# Private — SceneWorld and the Flecs phase pipeline (§8 / §30.11).
# ----------------------------------------------------------------------------
target_sources(pyxis_renderer PRIVATE
    # M1 — RenderGraph + Passes + Profiler + Renderer.
    Private/Diagnostics/CommandListMarker.h

    Private/RenderGraph/IRenderPass.h
    Private/RenderGraph/PassContext.h
    Private/RenderGraph/RenderGraph.h
    Private/RenderGraph/RenderGraph.cpp

    Private/Passes/TrianglePass.h
    Private/Passes/TrianglePass.cpp

    Private/Profiler/Profiler.cpp

    Private/Renderers/PyxisRenderer.cpp

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

# Flecs is PRIVATE — never propagated to dependents (plan §49).
target_link_libraries(pyxis_renderer
    PUBLIC
        Pyxis::Platform
    PRIVATE
        $<TARGET_NAME_IF_EXISTS:flecs::flecs_static>
        $<TARGET_NAME_IF_EXISTS:flecs::flecs>
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

# Shaders — M1 triangle vert + frag compiled by Slang at build time
# (plan §10 / §23).
include(Slang)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/triangle.vert.slang
    ENTRY_POINT main
    STAGE       vertex
    OUTPUT      triangle.vert.spv
)
pyxis_compile_slang_shader(
    TARGET      pyxis_renderer
    SOURCE      ${CMAKE_SOURCE_DIR}/resources/shaders/triangle.frag.slang
    ENTRY_POINT main
    STAGE       fragment
    OUTPUT      triangle.frag.spv
)

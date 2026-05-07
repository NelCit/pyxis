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
)

# ----------------------------------------------------------------------------
# Private — SceneWorld and the Flecs phase pipeline (§8 / §30.11).
# ----------------------------------------------------------------------------
target_sources(pyxis_renderer PRIVATE
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

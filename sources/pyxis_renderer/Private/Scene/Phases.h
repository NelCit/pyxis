// Pyxis renderer — custom Flecs phase pipeline tags.
//
// Plan §8.2 / §30.11. Built-in flecs::OnUpdate etc. are *not* used so the
// per-phase ordering is deterministic and explicit; reordering or
// inserting between phases requires an RFC (§44).
//
// The phases below run in order during `world.progress()`, gating the
// systems registered with `.kind(<phase>)`.

#pragma once

#include <flecs.h>

namespace pyxis::scene {

// The GpuScene commit systems registered on each phase (RFC 0009,
// GpuScene/Commit.cpp::RegisterCommitPipeline):
struct PhaseUploadTextures {};   // Sys_UploadTextures
struct PhaseUploadMaterials {};  // Sys_UploadMaterials / _UploadLights / _InstanceSideTables / _UploadVolumes
struct PhaseExtractMeshes {};    // Sys_UploadMeshes + the 5 mesh side-table systems
struct PhaseBuildBlas {};        // Sys_BuildBlas
struct PhaseRebuildTlas {};      // Sys_RebuildTlas
struct PhaseUpdateBindless {};   // reserved: the bindless descriptor table is bound by
                                 // PathTracePass (the render graph), not GpuScene, so
                                 // this phase carries no commit system in v1.
struct PhaseClearDirty {};       // Sys_ClearDirty

// Returns the registered Flecs entity for a phase tag. Phases are
// constructed at SceneWorld::Init time and registered into the custom
// pipeline in `RegisterPhasePipeline()` below.
flecs::entity GetPhase(flecs::world& world, const char* tagName);

// Builds the pipeline that runs the phases in the order listed above.
// The returned entity is the world's `set_pipeline(...)` argument.
flecs::entity RegisterPhasePipeline(flecs::world& world);

}  // namespace pyxis::scene

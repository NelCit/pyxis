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

struct PhaseUploadTextures {};   // System_UploadDirtyTextures
struct PhaseUploadMaterials {};  // System_UploadDirtyMaterials
struct PhaseExtractMeshes {};    // System_ExtractDirtyMeshes
struct PhaseBuildBlas {};        // System_BuildDirtyBlas
struct PhaseRebuildTlas {};      // System_RebuildTlas
struct PhaseUpdateBindless {};   // System_UpdateBindlessTable
struct PhaseClearDirty {};       // System_ClearDirtyFlags

// Returns the registered Flecs entity for a phase tag. Phases are
// constructed at SceneWorld::Init time and registered into the custom
// pipeline in `RegisterPhasePipeline()` below.
flecs::entity GetPhase(flecs::world& world, const char* tagName);

// Builds the pipeline that runs the phases in the order listed above.
// The returned entity is the world's `set_pipeline(...)` argument.
flecs::entity RegisterPhasePipeline(flecs::world& world);

}  // namespace pyxis::scene

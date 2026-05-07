// Pyxis renderer — custom Flecs phase pipeline registration.
//
// Plan §8.2 / §30.11. The phases are linked with `flecs::DependsOn` pairs so
// Flecs's pipeline scheduler runs them strictly in declared order. We then
// build a custom pipeline that matches each phase tag and bind it as the
// active pipeline of the world.

#include "Scene/Phases.h"

namespace pyxis::scene {

namespace {

template <class Tag>
flecs::entity RegisterPhase(flecs::world& world,
                            const char*    name,
                            flecs::entity  dependsOn = flecs::entity{}) {
    flecs::entity phase = world.entity<Tag>().add(flecs::Phase);
    if (name && *name) {
        phase.set_name(name);
    }
    if (dependsOn) {
        phase.add(flecs::DependsOn, dependsOn);
    }
    return phase;
}

}  // namespace

flecs::entity GetPhase(flecs::world& world, const char* tagName) {
    return world.lookup(tagName);
}

flecs::entity RegisterPhasePipeline(flecs::world& world) {
    // Phases in canonical order (§8.2).
    flecs::entity uploadTextures  = RegisterPhase<PhaseUploadTextures> (world, "PhaseUploadTextures");
    flecs::entity uploadMaterials = RegisterPhase<PhaseUploadMaterials>(world, "PhaseUploadMaterials", uploadTextures);
    flecs::entity extractMeshes   = RegisterPhase<PhaseExtractMeshes>  (world, "PhaseExtractMeshes",   uploadMaterials);
    flecs::entity buildBlas       = RegisterPhase<PhaseBuildBlas>      (world, "PhaseBuildBlas",       extractMeshes);
    flecs::entity rebuildTlas     = RegisterPhase<PhaseRebuildTlas>    (world, "PhaseRebuildTlas",     buildBlas);
    flecs::entity updateBindless  = RegisterPhase<PhaseUpdateBindless> (world, "PhaseUpdateBindless",  rebuildTlas);
    flecs::entity clearDirty      = RegisterPhase<PhaseClearDirty>     (world, "PhaseClearDirty",      updateBindless);
    (void)clearDirty;

    // Custom pipeline matching every phase tag in order.
    flecs::entity pipeline = world.pipeline()
        .with(flecs::System)
        .with(flecs::Phase).cascade(flecs::DependsOn)
        .build();

    world.set_pipeline(pipeline);
    return pipeline;
}

}  // namespace pyxis::scene

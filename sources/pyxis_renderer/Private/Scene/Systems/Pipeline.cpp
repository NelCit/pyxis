// Pyxis renderer — system registration.
//
// Plan §8.2 / §30.11. Each per-phase system lives in its own .cpp and
// exposes a `Run<Verb>(...)` free function; this file wires every one of
// them into the custom pipeline.

#include "Scene/Systems/Pipeline.h"

#include "Scene/Phases.h"

namespace pyxis::scene {

// Forward decls — implementations live one .cpp away each (§30.11 names
// them System_VerbObject; the public-facing free function is `Run...`).
void RunUploadDirtyTextures (flecs::iter& it);
void RunUploadDirtyMaterials(flecs::iter& it);
void RunExtractDirtyMeshes  (flecs::iter& it);
void RunBuildDirtyBlas      (flecs::iter& it);
void RunRebuildTlas         (flecs::iter& it);
void RunUpdateBindlessTable (flecs::iter& it);
void RunClearDirtyFlags     (flecs::iter& it);

void RegisterSystems(flecs::world& world) {
    world.system("System_UploadDirtyTextures")
        .kind<PhaseUploadTextures>()
        .run([](flecs::iter& it) { RunUploadDirtyTextures(it); });

    world.system("System_UploadDirtyMaterials")
        .kind<PhaseUploadMaterials>()
        .run([](flecs::iter& it) { RunUploadDirtyMaterials(it); });

    world.system("System_ExtractDirtyMeshes")
        .kind<PhaseExtractMeshes>()
        .run([](flecs::iter& it) { RunExtractDirtyMeshes(it); });

    world.system("System_BuildDirtyBlas")
        .kind<PhaseBuildBlas>()
        .run([](flecs::iter& it) { RunBuildDirtyBlas(it); });

    world.system("System_RebuildTlas")
        .kind<PhaseRebuildTlas>()
        .run([](flecs::iter& it) { RunRebuildTlas(it); });

    world.system("System_UpdateBindlessTable")
        .kind<PhaseUpdateBindless>()
        .run([](flecs::iter& it) { RunUpdateBindlessTable(it); });

    world.system("System_ClearDirtyFlags")
        .kind<PhaseClearDirty>()
        .run([](flecs::iter& it) { RunClearDirtyFlags(it); });
}

}  // namespace pyxis::scene

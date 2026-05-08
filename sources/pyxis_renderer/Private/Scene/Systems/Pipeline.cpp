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
void RunUploadDirtyTextures (flecs::iter& iter);
void RunUploadDirtyMaterials(flecs::iter& iter);
void RunExtractDirtyMeshes  (flecs::iter& iter);
void RunBuildDirtyBlas      (flecs::iter& iter);
void RunRebuildTlas         (flecs::iter& iter);
void RunUpdateBindlessTable (flecs::iter& iter);
void RunClearDirtyFlags     (flecs::iter& iter);

void RegisterSystems(flecs::world& world) {
    world.system("System_UploadDirtyTextures")
        .kind<PhaseUploadTextures>()
        .run([](flecs::iter& iter) { RunUploadDirtyTextures(iter); });

    world.system("System_UploadDirtyMaterials")
        .kind<PhaseUploadMaterials>()
        .run([](flecs::iter& iter) { RunUploadDirtyMaterials(iter); });

    world.system("System_ExtractDirtyMeshes")
        .kind<PhaseExtractMeshes>()
        .run([](flecs::iter& iter) { RunExtractDirtyMeshes(iter); });

    world.system("System_BuildDirtyBlas")
        .kind<PhaseBuildBlas>()
        .run([](flecs::iter& iter) { RunBuildDirtyBlas(iter); });

    world.system("System_RebuildTlas")
        .kind<PhaseRebuildTlas>()
        .run([](flecs::iter& iter) { RunRebuildTlas(iter); });

    world.system("System_UpdateBindlessTable")
        .kind<PhaseUpdateBindless>()
        .run([](flecs::iter& iter) { RunUpdateBindlessTable(iter); });

    world.system("System_ClearDirtyFlags")
        .kind<PhaseClearDirty>()
        .run([](flecs::iter& iter) { RunClearDirtyFlags(iter); });
}

}  // namespace pyxis::scene

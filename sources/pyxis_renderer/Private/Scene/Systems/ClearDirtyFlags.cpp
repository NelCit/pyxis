// Pyxis renderer — System_ClearDirtyFlags.
//
// Plan §8.1 / §30.11. Removes every Dirty<T> tag at the end of the
// CommitResources tick so the next frame's queries don't double-process
// the same entities. M0 ships the body — `Dirty<T>` removal is the only
// per-phase work that has to be live from day 0 (otherwise Tick() under
// the unit test would not be a no-op against an empty world: the test
// asserts `TickCount > 0` after world.progress()).
//
// The tag removal is idempotent on an empty archetype set, so this body
// works correctly when no other system has tagged anything.

#include "Scene/Components/Dirty.h"

#include <flecs.h>

namespace pyxis::scene {

void RunClearDirtyFlags(flecs::iter& it) {
    flecs::world world = it.world();

    // Remove every named Dirty<*> tag from every entity that holds it.
    // M0: zero entities carry these tags, so each pass is a no-op; the
    // structure is in place for M3+ to attach tags from the real systems.
    world.each<DirtyTopology>  ([](flecs::entity e, DirtyTopology  &) { e.remove<DirtyTopology>();   });
    world.each<DirtyTransform> ([](flecs::entity e, DirtyTransform &) { e.remove<DirtyTransform>();  });
    world.each<DirtyMaterial>  ([](flecs::entity e, DirtyMaterial  &) { e.remove<DirtyMaterial>();   });
    world.each<DirtyVisibility>([](flecs::entity e, DirtyVisibility&) { e.remove<DirtyVisibility>(); });
    world.each<DirtyTexture>   ([](flecs::entity e, DirtyTexture   &) { e.remove<DirtyTexture>();    });
    world.each<DirtyLight>     ([](flecs::entity e, DirtyLight     &) { e.remove<DirtyLight>();      });
    world.each<DirtyTextureGpu>([](flecs::entity e, DirtyTextureGpu&) { e.remove<DirtyTextureGpu>(); });
}

}  // namespace pyxis::scene

// Pyxis renderer — System_ClearDirtyFlags.
//
// Plan §8.1 / §30.11. Removes every Dirty<T> tag at the end of the
// CommitResources tick so the next frame's queries don't double-process
// the same entities.
//
// M0 ships a no-op body. The phase pipeline traverses this system once
// per Tick — the SceneWorldInit unit test asserts world.progress() runs
// cleanly without any entity carrying a Dirty<*> tag. M3+ adds the real
// cached `flecs::query<Dirty<T>>` per tag (built once in QueryCache, not
// per-frame — §30.11) and clears them here.

#include <flecs.h>

namespace pyxis::scene {

void RunClearDirtyFlags(flecs::iter& /*it*/) {
  // M0: no entity carries Dirty<*>. The real removal loop lands in M3+
  // alongside the cached queries (§30.11 — no per-frame query construction).
}

}  // namespace pyxis::scene

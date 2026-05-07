// Pyxis renderer — System_BuildDirtyBlas (M0 no-op).
//
// Plan §8.1 / §16. Builds (and optionally compacts) BLAS entries for
// dirty meshes. M0 stub.

#include <flecs.h>

namespace pyxis::scene {

void RunBuildDirtyBlas(flecs::iter& /*it*/) {
    // M0: no-op. M3+ (cube path-trace) wires the real batched build with
    // the size-split flag policy from §16.
}

}  // namespace pyxis::scene

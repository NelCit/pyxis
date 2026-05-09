// Pyxis renderer — System_ExtractDirtyMeshes (M0 no-op).
//
// Plan §8.1 / §14.5. Reserves vertex/index pool ranges for newly-dirty
// meshes and stages the buffer copies. M0 stub.

#include <flecs.h>

namespace pyxis::scene {

void RunExtractDirtyMeshes(flecs::iter& /*it*/) {
  // M0: no-op. Real path lands at M3 (cube path-trace) when the vertex
  // pool first carries data.
}

}  // namespace pyxis::scene

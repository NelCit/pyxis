// Pyxis renderer — System_RebuildTlas (M0 no-op).
//
// Plan §8.1 / §16 / §16.5. Rebuilds (or refits) the TLAS over Instance
// entities with Visibility = true. M0 stub.

#include <flecs.h>

namespace pyxis::scene {

void RunRebuildTlas(flecs::iter& /*it*/) {
  // M0: no-op. M3+ adds the real two-tier TLAS (static + dynamic) with
  // the §16.5 sharding rule for Moana.
}

}  // namespace pyxis::scene

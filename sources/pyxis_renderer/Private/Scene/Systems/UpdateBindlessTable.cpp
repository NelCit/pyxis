// Pyxis renderer — System_UpdateBindlessTable (M0 no-op).
//
// Plan §8.1 / §5 / §33.3. Batches DescriptorTableManager writes for slots
// vacated framesInFlight frames ago. M0 stub.

#include <flecs.h>

namespace pyxis::scene {

void RunUpdateBindlessTable(flecs::iter& /*it*/) {
  // M0: no-op. M5+ lights up the descriptor-indexing path.
}

}  // namespace pyxis::scene

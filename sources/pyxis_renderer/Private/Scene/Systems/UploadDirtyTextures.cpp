// Pyxis renderer — System_UploadDirtyTextures (M0 no-op).
//
// Plan §8.1 / §13. Drains pending texture uploads into the bindless table.
// M0 ships a no-op so the phase pipeline survives world.progress() without
// touching the GPU; M5+ replaces this body with the real upload path.

#include <flecs.h>

namespace pyxis::scene {

void RunUploadDirtyTextures(flecs::iter& /*it*/) {
  // M0: no-op. Real queries (Texture + Dirty<Texture>) and the
  // associated GPU upload pipeline land in M5.
}

}  // namespace pyxis::scene

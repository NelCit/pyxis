// Pyxis renderer — System_UploadDirtyMaterials (M0 no-op).
//
// Plan §8.1 / §11. Packs CPU-side OpenPBRMaterialDesc into the GPU layout
// OpenPBRMaterialGPU and uploads. M0 stub.

#include <flecs.h>

namespace pyxis::scene {

void RunUploadDirtyMaterials(flecs::iter& /*it*/) {
  // M0: no-op. M5+ wires the real OpenPBR upload + dedupe path.
}

}  // namespace pyxis::scene

// Pyxis renderer — MaterialGpu component.
//
// Slot index in the Material GPU pool (§14.5, plan §8). Attached to a
// Material entity once `System_UploadDirtyMaterials` has packed the
// CPU-side `OpenPBRMaterialDesc` into the GPU layout `OpenPBRMaterialGPU`.

#pragma once

#include <cstdint>

namespace pyxis::scene {

struct MaterialGpu {
    uint32_t gpuSlot = 0;
};

}  // namespace pyxis::scene

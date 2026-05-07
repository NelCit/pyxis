// Pyxis renderer — TextureGpu component.
//
// Bindless slot index for an uploaded texture (§5 / §13). Attached to a
// Texture entity once `System_UploadDirtyTextures` finishes the upload.

#pragma once

#include <cstdint>

namespace pyxis::scene {

struct TextureGpu {
    uint32_t bindlessSlot = 0;
};

}  // namespace pyxis::scene

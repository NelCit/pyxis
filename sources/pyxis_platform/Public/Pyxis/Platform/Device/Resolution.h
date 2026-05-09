// Pyxis platform — backbuffer resolution descriptor.
//
// Used by both the windowed VkDeviceManager and the headless variant.

#pragma once

#include <cstdint>

namespace pyxis {

struct Resolution {
  uint32_t width = 1920;
  uint32_t height = 1080;
};

}  // namespace pyxis

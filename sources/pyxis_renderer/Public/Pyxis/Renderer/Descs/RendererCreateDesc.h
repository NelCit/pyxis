// Pyxis renderer — RendererCreateDesc POD.
// Plan §18.4.

#pragma once

#include <cstdint>
#include <string_view>

namespace pyxis {

struct RendererCreateDesc {
  uint32_t initialWidth = 1920;
  uint32_t initialHeight = 1080;

  // Resource search path the runtime appends to AssetLocator's
  // Resources/ to find <root>/Resources/shaders/*.spv. Empty → use
  // the default (next to pyxis.exe). Hot-reload (M3+) treats this
  // as the watched root.
  std::string_view shaderSearchPath;
};

}  // namespace pyxis

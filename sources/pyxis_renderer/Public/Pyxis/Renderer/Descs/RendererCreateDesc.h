// Pyxis renderer — RendererCreateDesc POD.
// Plan §18.4.

#pragma once

#include <cstdint>
#include <string_view>

namespace pyxis {

struct RendererCreateDesc {
  uint32_t initialWidth = 1920;
  uint32_t initialHeight = 1080;

  // Active frames-in-flight count. Capped at MAX_FRAMES_IN_FLIGHT
  // (= 3, see Forward.h). PyxisRenderer threads this into every
  // PassContext so passes can size per-FIF rings; today only the
  // path-tracer's picker readback uses it (asserts FIF == 1 since
  // the mapBuffer path lacks an EventQuery — see PathTracePass.cpp).
  // Caller (viewer / headless) typically passes
  // IDeviceManager::GetFramesInFlight().
  uint32_t framesInFlight = 1;

  // Resource search path the runtime appends to AssetLocator's
  // Resources/ to find <root>/Resources/shaders/*.spv. Empty → use
  // the default (next to pyxis.exe). Hot-reload (M3+) treats this
  // as the watched root.
  std::string_view shaderSearchPath;
};

}  // namespace pyxis

// Pyxis renderer — RenderSettings POD (M3 subset).
//
// Plan §18.4 / §21. The M3 surface ships render resolution, the
// backbuffer clear colour, and the §29.5 feature-mask seed. The full
// path-trace knob set (samplesPerFrame, maxBounces, RR, firefly
// clamp, low-discrepancy sampling, tone-map operator, debug view,
// AOV mask) wires in progressively from M5 onward as each subsystem
// lands.

#pragma once

#include <cstdint>

namespace pyxis {

struct RenderSettings {
  uint32_t width = 1920;
  uint32_t height = 1080;

  // Backbuffer clear before any pass. Linear sRGB (§25.I.2 color
  // pipeline). M1 uses this to verify the swapchain present path
  // before any draw lands; a known non-black colour also makes
  // "did the swapchain even resize?" obvious in the viewer.
  float clearColor[4] = {0.05f, 0.05f, 0.06f, 1.0f};

  // §29.5 feature mask seed. M3 ships PathTracePass as the only
  // mandatory pass plus the ImGui overlay; the rest of the §29.5
  // toggle table (`accumulation`, `nee`, `mis`, `denoise`,
  // `toneMap`, `taa`, `motionVectors`, `aovs.*`) fills in at M5+.
  struct Features {
    bool imguiOverlay = true;  // PYXIS_DEBUG_TOOLS-gated
  } features;
};

}  // namespace pyxis

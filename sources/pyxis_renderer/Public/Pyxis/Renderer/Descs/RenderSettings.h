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

  // M7 follow-up — viewer-driven AOV inspector + pixel picker. The
  // raygen reads these out of CameraUniforms each frame. Mirrors the
  // DEBUG_VIEW_* constants in resources/shaders/ShaderInterop.slang.
  // Headless mode leaves these at defaults (Color, no mouse hover)
  // so byte-equal regression artefacts stay stable.
  enum class DebugView : uint32_t {
    Color       = 0,   // post-tonemap radiance
    Normal      = 1,   // (n*0.5+0.5)
    Depth       = 2,   // 1/depth grayscale
    InstanceId  = 3,   // hashed colour per slot
    MaterialId  = 4,   // hashed colour per material
    BaseColor   = 5,   // raw OpenPBR baseColor (pre-shading albedo)
    WorldPos    = 6,   // 10-unit-period fract of world hit position
    // Tier 1 Hydra-canonical AOVs — exposed as inspector views too so
    // the editor can sanity-check what Hydra delegates pull.
    Alpha       = 7,   // 1.0 on hit, 0.0 on miss (binary today)
    ElementId   = 8,   // hashed colour per face within a BLAS
    NormalEye   = 9,   // eye-space normal as (n*0.5+0.5)
    WorldPosEye = 10,  // sin-encoded eye-space position
  };
  DebugView debugView = DebugView::Color;

  // Mouse pixel for the picker. 0xFFFFFFFF on either axis = "no
  // hover"; raygen short-circuits the pick write.
  static constexpr uint32_t MOUSE_PIXEL_NONE = 0xFFFFFFFFu;
  uint32_t mousePixelX = MOUSE_PIXEL_NONE;
  uint32_t mousePixelY = MOUSE_PIXEL_NONE;

  // WorldPos AOV display period (scene units, typically meters).
  // The display branch in raygen.slang encodes worldPos via
  // sin(p * 2pi / worldPosPeriod) so a smaller value gives finer
  // bands. 10 m is a sensible default for human-scale scenes;
  // crank to ~50 m for Bistro-scale (or down to ~0.1 m for a unit
  // cube). 0 falls through to PathTracePass's default of 10.
  float worldPosPeriod = 10.0f;
};

}  // namespace pyxis

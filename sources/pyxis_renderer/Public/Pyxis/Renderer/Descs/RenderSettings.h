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

  // §29.5 feature mask seed. M3 ships RaytracedLightingPass as the only
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
    PrimId      = 3,   // hashed colour per slot — Hydra's HdAovTokens->primId
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
  // The display transform in tonemap.slang encodes worldPos via
  // sin(p * 2pi / worldPosPeriod) so a smaller value gives finer
  // bands. 10 m is a sensible default for human-scale scenes;
  // crank to ~50 m for World Lobby-scale (or down to ~0.1 m for a unit
  // cube). 0 falls through to TonemapPass's default of 10.
  float worldPosPeriod = 10.0f;

  // Deterministic supersampling factor (SSAA). When > 1, the caller
  // renders the whole frame at `ssaaFactor`× the {width, height} per
  // axis (allocating super-res AOVs) and binds an output-resolution
  // `RenderTargets::colorResolved`; the renderer's SsaaResolvePass
  // box-downsamples the super-res color into it. 1 = off (no resolve;
  // the caller uses RenderTargets::color directly). Noise-free AA —
  // no jitter, no accumulation. Clamp to a sane ceiling (4) at the
  // call site; the renderer honours whatever it's given.
  uint32_t ssaaFactor = 1;

  // Q3 OpenPBR-complete (openpbr-complete-design.md "Control
  // surface") — bitmask gating the OpenPBR closure blocks of
  // openpbr_material.slang. Each bit selects a specialization-constant
  // pipeline variant (built lazily, cached); a bit that is OFF
  // produces EXACTLY the same image as that feature's weight being 0
  // on every material (gate-OFF == weight-0 semantics). Bits:
  //   bit 0 (0x01) — coat layer (GGX + absorption + darkening +
  //                  roughening)
  //   bit 1 (0x02) — fuzz (Zeltner LTC sheen)
  //   bit 2 (0x04) — transmission (translucent base)
  //   bit 3 (0x08) — subsurface (SSS-as-diffuse fallback)
  //   bit 4 (0x10) — anisotropy (anisotropic GGX)
  //   bit 5 (0x20) — energy-preserving (EON) diffuse; OFF = Lambert
  // Default 0x3F = all six ON — image-identical to the pre-toggle
  // renderer, so headless EXR goldens and §25.O.3 adapter parity stay
  // stable. The value is a literal because this public header cannot
  // include ShaderInterop.slang (§18.9 — the renderer is the only
  // consumer of the interop file); a static_assert in
  // PyxisRenderer.cpp pins it to shaderinterop::OPENPBR_FEATURES_ALL
  // so the two cannot drift.
  uint32_t openPbrFeatureMask = 0x3Fu;

  // Auto-exposure (AutoExposurePass) — consumes two §22.3 reserved tail slots
  // (no sizeof growth). When non-zero, the renderer reduces the frame's HDR
  // radiance to a geometric-mean luminance and TonemapPass derives an exposure
  // that lands that average on `autoExposureKey` (middle grey); the camera's
  // manual exposure then rides on top as a bias. Lets a scene with hot UsdLux
  // lights and no authored camera exposure (the OpenPBR Playground) display
  // without clipping to white. 0 (OFF) by default — headless EXR goldens and
  // §25.O.3 adapter parity stay byte-equal (and the reduction is integer, so
  // even ON is deterministic). Exposed via the viewer's ImGui editor + the
  // Omniverse Render Settings `pyxis:autoExposure` toggle. uint32 (not bool)
  // keeps the frozen-tail layout POD-clean.
  uint32_t autoExposure = 0u;
  // Target middle-grey the auto exposure maps the average luminance to
  // (0.18 = the photographic 18% grey card). Ignored when autoExposure is 0.
  float autoExposureKey = 0.18f;

  // §22.3 reserved tail — future RenderSettings additions consume
  // these slots (becoming typed members at MINOR) instead of growing
  // sizeof. Must stay zeroed.
  uint32_t _reserved[2] = {0u, 0u};
};

}  // namespace pyxis

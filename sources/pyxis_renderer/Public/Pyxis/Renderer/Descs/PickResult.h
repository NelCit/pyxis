// Pyxis renderer — PickResult POD.
//
// Plan §18.1 (public surface) + §19.4 (picking AOV reservation).
// Returned by PyxisRenderer::LastPickResult(); populated by the
// raygen each frame at the pixel matching RenderSettings::mousePixel.
// One-frame stale (the renderer copies the GPU pick buffer into a
// staging buffer, then maps it on the NEXT frame — so the result the
// app reads at frame N reflects the cursor position from frame N-1).
//
// Layout mirrors `pyxis::shaderinterop::PickResult` byte-for-byte
// (80 bytes, 5 rows of 16). Asserted at the bottom of this header.

#pragma once

#include <cstdint>

namespace pyxis {

struct PickResult {
  // Row 0 — final radiance (HDR, pre-tonemap) + ray distance.
  float    colorR = 0.0f;
  float    colorG = 0.0f;
  float    colorB = 0.0f;
  float    depth  = -1.0f;       // -1 sentinel = no hit at this pixel

  // Row 1 — world-space normal + instance id.
  float    normalX = 0.0f;
  float    normalY = 0.0f;
  float    normalZ = 0.0f;
  uint32_t instanceId = 0xFFFFFFFFu;  // ~0u = no hit / picker disabled

  // Row 2 — raw OpenPBR baseColor (pre-shading) + material slot.
  float    baseColorR = 0.0f;
  float    baseColorG = 0.0f;
  float    baseColorB = 0.0f;
  uint32_t materialId = 0xFFFFFFFFu;  // ~0u = no hit

  // Row 3 — world-space hit position + pad. Pad name follows §22.3 /
  // §30.2 convention; NOLINT for the same reason CameraDesc::_reserved
  // does (POD-only field, layout-locked, no `Pyxis` prefix).
  float    worldHitX = 0.0f;
  float    worldHitY = 0.0f;
  float    worldHitZ = 0.0f;
  // NOLINTNEXTLINE(readability-identifier-naming)
  uint32_t _pickPad0 = 0u;

  // Row 4 — pixel coords (cursor pixel the raygen sampled, in AOV
  // space). Mirrors the mousePixel{X,Y} the renderer was given via
  // RenderSettings, so the editor's hover readout can show
  // "Picker @ (1234, 567)" without tracking its own copy.
  uint32_t pixelX    = 0xFFFFFFFFu;
  uint32_t pixelY    = 0xFFFFFFFFu;
  // NOLINTNEXTLINE(readability-identifier-naming)
  uint32_t _pickPad1 = 0u;
  // NOLINTNEXTLINE(readability-identifier-naming)
  uint32_t _pickPad2 = 0u;
};

static_assert(sizeof(PickResult) == 80,
              "pyxis::PickResult must be 80 bytes — must match "
              "shaderinterop::PickResult byte-for-byte.");

}  // namespace pyxis

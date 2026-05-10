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

// Sentinel constants used across the picker / AOV inspector. Hoisted
// here (next to the struct that holds them) so every site reads from
// one source of truth instead of inlining 0xFFFFFFFFu literals. Same
// 32-bit value but different semantic meaning per field — keeping
// them named makes intent explicit at every read site.
inline constexpr uint32_t INSTANCE_ID_NONE = 0xFFFFFFFFu;  // ray missed / picker disabled
inline constexpr uint32_t MATERIAL_ID_NONE = 0xFFFFFFFFu;  // ray missed / material unbound
inline constexpr uint32_t PICK_PIXEL_NONE  = 0xFFFFFFFFu;  // cursor outside viewport / no hover

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
  uint32_t instanceId = INSTANCE_ID_NONE;

  // Row 2 — raw OpenPBR baseColor (pre-shading) + material slot.
  float    baseColorR = 0.0f;
  float    baseColorG = 0.0f;
  float    baseColorB = 0.0f;
  uint32_t materialId = MATERIAL_ID_NONE;

  // Row 3 — world-space hit position + 1 pad word. Bottom row's 3
  // trailing pads land in `_pickPad`. POD-layout-locked; the §22.3 /
  // §30.2 underscore-named pads need a NOLINT since the public-field
  // naming rule (camelCase) doesn't accept leading underscores.
  float    worldHitX = 0.0f;
  float    worldHitY = 0.0f;
  float    worldHitZ = 0.0f;
  // NOLINTNEXTLINE(readability-identifier-naming)
  uint32_t _pickPad0 = 0u;

  // Row 4 — pixel coords (cursor pixel the raygen sampled, in AOV
  // space). Mirrors the mousePixel{X,Y} the renderer was given via
  // RenderSettings, so the editor's hover readout can show
  // "Picker @ (1234, 567)" without tracking its own copy.
  uint32_t pixelX    = PICK_PIXEL_NONE;
  uint32_t pixelY    = PICK_PIXEL_NONE;
  // NOLINTNEXTLINE(readability-identifier-naming)
  uint32_t _pickPad1[2] = {0u, 0u};
};

static_assert(sizeof(PickResult) == 80,
              "pyxis::PickResult must be 80 bytes — must match "
              "shaderinterop::PickResult byte-for-byte.");

}  // namespace pyxis

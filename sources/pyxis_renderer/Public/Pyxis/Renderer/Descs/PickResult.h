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
// (32 bytes, 2 rows of 16). Asserted at the bottom of this header.

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
};

static_assert(sizeof(PickResult) == 32,
              "pyxis::PickResult must be 32 bytes — must match "
              "shaderinterop::PickResult byte-for-byte.");

}  // namespace pyxis

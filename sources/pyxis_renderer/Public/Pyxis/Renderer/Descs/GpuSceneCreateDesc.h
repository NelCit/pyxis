// Pyxis renderer — GpuScene constructor descriptor.
//
// Plan §18.4 + §33.1. Sized so the defaults are reasonable for a
// viewer; the headless path raises `framesInFlight` to 3 (§33.7
// byte-equal contract).
//
// `bindlessCapacity` 80 000 covers Moana hero textures + UDIM tiles
// per §5; lower for unit tests if you want. `stagingMib` is the
// upload-queue ring budget (§33.4). `compactBlas` defaults true
// because the §16 split rules require it for any mesh ≥ 64k tris;
// set false only for diagnostic profiling builds where you want to
// see uncompacted BLAS sizes.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>

namespace pyxis {

struct GpuSceneCreateDesc {
  uint32_t bindlessCapacity = 80'000;
  uint32_t stagingMib = 256;
  uint32_t framesInFlight = 2;  // ≤ MAX_FRAMES_IN_FLIGHT (§33.1).
  bool compactBlas = true;
  // V2.A.14 — CPU-side BCn encoding for textures decoded via stb_image.
  // When true (the default since the V2.A.14 follow-up), the texture
  // pipeline encodes each decoded RGBA8 buffer to a block-compressed
  // variant via stb_dxt.h before GPU upload:
  //   - Role::BaseColor / Emission  → BC1 (8 bytes / 4×4 block, sRGB).
  //   - Role::NormalMap              → BC5 (16 b/4×4, two-channel).
  //   - Role::RoughnessMetallic      → BC4 (8 b/4×4, single-channel).
  // Reduces VRAM ~6× for the lobby-scale texture set (§17 budget
  // target). Textures with width or height not a multiple of 4 fall
  // back to the uncompressed RGBA8 path (BCn blocks require 4×4
  // alignment). Set false (CLI: `--no-compress-textures`) for
  // diagnostic builds that want pixel-equal output against the
  // uncompressed path.
  bool compressTextures = true;
};

}  // namespace pyxis

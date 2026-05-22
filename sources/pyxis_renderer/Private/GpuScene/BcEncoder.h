// Pyxis renderer — V2.A.14 BCn texture encoder.
//
// Splits the `EncodeBCn` helper out of Commit.cpp so the unit-test
// harness can call it without dragging in GpuScene::Impl + stb_image
// + tinyexr. Kept in `pyxis::gpuscene_detail` (Private — not part of
// §18.1's public surface).
//
// Encoder selection is keyed by `TextureKey::Role`:
//   BaseColor / Emission           → BC1_UNORM_SRGB (8 B / 4×4 block)
//   NormalMap                       → BC5_UNORM     (16 B; RG only)
//   RoughnessMetallic + default     → BC1_UNORM     (8 B; RGB = AO/rough/metal)
//
// Requires `width` and `height` both multiples of 4. Callers gate on
// that (e.g. Commit.cpp logs + skips otherwise).

#pragma once

#include <Pyxis/Renderer/Descs/TextureKey.h>

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pyxis::gpuscene_detail {

void EncodeBCn(const std::uint8_t* srcRgba,
               std::uint32_t       width,
               std::uint32_t       height,
               TextureKey::Role    role,
               std::vector<std::uint8_t>& outBlocks,
               nvrhi::Format&      outFormat,
               std::size_t&        outBlockBytes) noexcept;

}  // namespace pyxis::gpuscene_detail

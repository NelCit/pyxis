// Pyxis V2.A.14 — BCn texture encoder unit tests.
//
// Pins `gpuscene_detail::EncodeBCn` from `BcEncoder.h`:
//   • Role → format mapping
//       BaseColor / Emission       → BC1_UNORM_SRGB (8 B per 4×4 block)
//       NormalMap                   → BC5_UNORM     (16 B per block)
//       RoughnessMetallic + default → BC4_UNORM     (8 B per block)
//   • Output buffer size = blocksWide × blocksTall × blockBytes
//   • Encoder runs against synthetic 4×4 and 8×4 RGBA8 buffers
//     (smallest legal sizes — `width % 4 == 0` && `height % 4 == 0`
//     gate lives in Commit.cpp).
//
// The §22.3 §35 "unit tests may peek into Private/" exemption applies:
// BcEncoder.cpp is compiled directly into pyxis_unit_tests so the test
// doesn't need to import the symbol across pyxis_renderer.dll's
// boundary (which would require widening the public surface — see
// CLAUDE.md "Architectural rule #1").

#include "GpuScene/BcEncoder.h"

#include <Pyxis/Renderer/Descs/TextureKey.h>

#include <nvrhi/nvrhi.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

// Build a synthetic RGBA8 buffer with deterministic gradient content.
// width and height MUST be multiples of 4 (caller-side BCn gate).
std::vector<std::uint8_t> MakeRgbaGradient(std::uint32_t width, std::uint32_t height)
{
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u);
  for (std::uint32_t row = 0; row < height; ++row)
  {
    for (std::uint32_t col = 0; col < width; ++col)
    {
      const std::size_t offset = (static_cast<std::size_t>(row) * width + col) * 4u;
      pixels[offset + 0] = static_cast<std::uint8_t>(col * 16u);
      pixels[offset + 1] = static_cast<std::uint8_t>(row * 16u);
      pixels[offset + 2] = static_cast<std::uint8_t>((col + row) * 8u);
      pixels[offset + 3] = 255u;
    }
  }
  return pixels;
}

}  // namespace

TEST(BcEncoderV2A14, BaseColorEncodesToBc1Srgb) {
  const auto src = MakeRgbaGradient(4, 4);
  std::vector<std::uint8_t> encoded;
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  std::size_t   blockBytes = 0;

  pyxis::gpuscene_detail::EncodeBCn(src.data(), 4, 4,
                                    pyxis::TextureKey::Role::BaseColor,
                                    encoded, format, blockBytes);

  EXPECT_EQ(format, nvrhi::Format::BC1_UNORM_SRGB);
  EXPECT_EQ(blockBytes, 8u);
  // 4×4 source = 1 block, BC1 = 8 bytes total.
  EXPECT_EQ(encoded.size(), 8u);
}

TEST(BcEncoderV2A14, EmissionEncodesToBc1Srgb) {
  const auto src = MakeRgbaGradient(4, 4);
  std::vector<std::uint8_t> encoded;
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  std::size_t   blockBytes = 0;

  pyxis::gpuscene_detail::EncodeBCn(src.data(), 4, 4,
                                    pyxis::TextureKey::Role::Emission,
                                    encoded, format, blockBytes);

  EXPECT_EQ(format, nvrhi::Format::BC1_UNORM_SRGB);
  EXPECT_EQ(blockBytes, 8u);
  EXPECT_EQ(encoded.size(), 8u);
}

TEST(BcEncoderV2A14, NormalMapEncodesToBc5) {
  const auto src = MakeRgbaGradient(4, 4);
  std::vector<std::uint8_t> encoded;
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  std::size_t   blockBytes = 0;

  pyxis::gpuscene_detail::EncodeBCn(src.data(), 4, 4,
                                    pyxis::TextureKey::Role::NormalMap,
                                    encoded, format, blockBytes);

  EXPECT_EQ(format, nvrhi::Format::BC5_UNORM);
  EXPECT_EQ(blockBytes, 16u);
  // 4×4 source = 1 block, BC5 = 16 bytes total.
  EXPECT_EQ(encoded.size(), 16u);
}

TEST(BcEncoderV2A14, RoughnessMetallicEncodesToBc4) {
  const auto src = MakeRgbaGradient(4, 4);
  std::vector<std::uint8_t> encoded;
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  std::size_t   blockBytes = 0;

  pyxis::gpuscene_detail::EncodeBCn(src.data(), 4, 4,
                                    pyxis::TextureKey::Role::RoughnessMetallic,
                                    encoded, format, blockBytes);

  EXPECT_EQ(format, nvrhi::Format::BC4_UNORM);
  EXPECT_EQ(blockBytes, 8u);
  EXPECT_EQ(encoded.size(), 8u);
}

TEST(BcEncoderV2A14, OutputSizeScalesWithBlockCount) {
  // 8×4 source = 2 blocks horizontally × 1 block vertically = 2 blocks.
  const auto src = MakeRgbaGradient(8, 4);
  std::vector<std::uint8_t> encoded;
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  std::size_t   blockBytes = 0;

  pyxis::gpuscene_detail::EncodeBCn(src.data(), 8, 4,
                                    pyxis::TextureKey::Role::BaseColor,
                                    encoded, format, blockBytes);

  EXPECT_EQ(format, nvrhi::Format::BC1_UNORM_SRGB);
  EXPECT_EQ(blockBytes, 8u);
  // 2 blocks × 8 bytes = 16 bytes total.
  EXPECT_EQ(encoded.size(), 16u);
}

TEST(BcEncoderV2A14, EncodedOutputIsNonZeroForGradient) {
  // Sanity: stb_dxt produced *something* — the gradient input is
  // non-uniform so the encoded block must not be all zeros (an
  // all-zero BC1 block decodes to black, which would mean the encoder
  // silently failed to consume the source).
  const auto src = MakeRgbaGradient(4, 4);
  std::vector<std::uint8_t> encoded;
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  std::size_t   blockBytes = 0;

  pyxis::gpuscene_detail::EncodeBCn(src.data(), 4, 4,
                                    pyxis::TextureKey::Role::BaseColor,
                                    encoded, format, blockBytes);

  ASSERT_EQ(encoded.size(), 8u);
  bool anyNonZero = false;
  for (const std::uint8_t byte : encoded)
  {
    if (byte != 0u)
    {
      anyNonZero = true;
      break;
    }
  }
  EXPECT_TRUE(anyNonZero);
}

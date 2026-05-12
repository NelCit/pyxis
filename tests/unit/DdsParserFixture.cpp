// Pyxis V2.A.14 — DDS / BCn parser unit test.
//
// Hand-craft minimal valid DDS byte streams (BC1 / BC3 / BC5 / BC7
// via DX10 ext) + a few negative cases (truncated, bad magic,
// unsupported FourCC) and assert the parser maps each to the right
// `nvrhi::Format` + pixel offset + bytes-per-block.

#include "GpuScene/DdsParser.h"

#include <Pyxis/Renderer/Descs/TextureKey.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// Build a 128-byte DDS header with width / height + a FourCC code in
// the dwFourCC slot at byte 84. Bytes after the header are zeroed.
std::vector<std::uint8_t> BuildDdsHeader(std::uint32_t width,
                                          std::uint32_t height,
                                          std::uint32_t fourCC,
                                          std::size_t pixelDataBytes = 64u)
{
  std::vector<std::uint8_t> bytes(128u + pixelDataBytes, std::uint8_t{0});
  std::memcpy(bytes.data(), "DDS ", 4);
  // dwSize = 124 at offset 4 — not strictly required for our parser
  // but real DDS files always have it.
  const std::uint32_t headerSize = 124u;
  std::memcpy(bytes.data() + 4u, &headerSize, 4);
  std::memcpy(bytes.data() + 12u, &height, 4);
  std::memcpy(bytes.data() + 16u, &width, 4);
  std::memcpy(bytes.data() + 84u, &fourCC, 4);
  return bytes;
}

// Build a DX10-extended DDS — adds a 20-byte DDS_HEADER_DXT10 with the
// given DXGI format code right after the 128-byte standard header.
std::vector<std::uint8_t> BuildDx10Dds(std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint32_t dxgiFormat,
                                        std::size_t pixelDataBytes = 64u)
{
  // FourCC = "DX10".
  std::vector<std::uint8_t> bytes = BuildDdsHeader(width, height, 0x30315844u,
                                                    20u + pixelDataBytes);
  // dxgiFormat at offset 128.
  std::memcpy(bytes.data() + 128u, &dxgiFormat, 4);
  return bytes;
}

}  // namespace

TEST(DdsParserFixture, Bc1FourCcParsesAsBc1Unorm)
{
  const auto bytes = BuildDdsHeader(64, 64, 0x31545844u);  // "DXT1"
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::NormalMap);
  ASSERT_TRUE(parsed.success);
  EXPECT_EQ(parsed.width,  64u);
  EXPECT_EQ(parsed.height, 64u);
  EXPECT_EQ(parsed.format, nvrhi::Format::BC1_UNORM);
  EXPECT_EQ(parsed.bytesPerBlock, 8u);
  EXPECT_EQ(parsed.pixelOffset,  128u);
}

TEST(DdsParserFixture, Bc1WithBaseColorRoleEmitsSrgb)
{
  const auto bytes = BuildDdsHeader(32, 32, 0x31545844u);  // "DXT1"
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::BaseColor);
  ASSERT_TRUE(parsed.success);
  EXPECT_EQ(parsed.format, nvrhi::Format::BC1_UNORM_SRGB);
}

TEST(DdsParserFixture, Bc3FourCcParsesAsBc3)
{
  const auto bytes = BuildDdsHeader(8, 8, 0x35545844u);  // "DXT5"
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::Emission);
  ASSERT_TRUE(parsed.success);
  EXPECT_EQ(parsed.format, nvrhi::Format::BC3_UNORM_SRGB);
  EXPECT_EQ(parsed.bytesPerBlock, 16u);
}

TEST(DdsParserFixture, Bc5FourCcParsesAsBc5)
{
  const auto bytes = BuildDdsHeader(16, 16, 0x32495441u);  // "ATI2"
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::NormalMap);
  ASSERT_TRUE(parsed.success);
  EXPECT_EQ(parsed.format, nvrhi::Format::BC5_UNORM);
  EXPECT_EQ(parsed.bytesPerBlock, 16u);
}

TEST(DdsParserFixture, Bc7Dx10DxgiCodeRoutes)
{
  const auto bytes = BuildDx10Dds(8, 8, /*dxgiFormat=*/98u);
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::NormalMap);
  ASSERT_TRUE(parsed.success);
  EXPECT_EQ(parsed.format, nvrhi::Format::BC7_UNORM);
  EXPECT_EQ(parsed.bytesPerBlock, 16u);
  EXPECT_EQ(parsed.pixelOffset,  148u);  // 128 header + 20 DXT10 ext
}

TEST(DdsParserFixture, Bc7Dx10SrgbCodeWithBaseColorEmitsSrgb)
{
  const auto bytes = BuildDx10Dds(8, 8, /*dxgiFormat=*/99u);  // BC7_SRGB
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::BaseColor);
  ASSERT_TRUE(parsed.success);
  EXPECT_EQ(parsed.format, nvrhi::Format::BC7_UNORM_SRGB);
}

TEST(DdsParserFixture, BadMagicFails)
{
  std::vector<std::uint8_t> bytes(128u + 64u, std::uint8_t{0});
  std::memcpy(bytes.data(), "JUNK", 4);
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::BaseColor);
  EXPECT_FALSE(parsed.success);
}

TEST(DdsParserFixture, TruncatedFails)
{
  std::vector<std::uint8_t> bytes(64u, std::uint8_t{0});  // < 128
  std::memcpy(bytes.data(), "DDS ", 4);
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::BaseColor);
  EXPECT_FALSE(parsed.success);
}

TEST(DdsParserFixture, UnsupportedFourCcFails)
{
  // "FAKE" — not a recognized BCn FourCC.
  const auto bytes = BuildDdsHeader(8, 8, 0x454B4146u);
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::BaseColor);
  EXPECT_FALSE(parsed.success);
}

TEST(DdsParserFixture, UnsupportedDxgiFormatFails)
{
  // DXGI format 100 — not a BCn code.
  const auto bytes = BuildDx10Dds(8, 8, /*dxgiFormat=*/100u);
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::BaseColor);
  EXPECT_FALSE(parsed.success);
}

TEST(DdsParserFixture, ZeroDimensionFails)
{
  const auto bytes = BuildDdsHeader(0, 16, 0x31545844u);
  const auto parsed = pyxis::gpuscene_detail::ParseDds(
      std::span<const std::uint8_t>{bytes.data(), bytes.size()},
      pyxis::TextureKey::Role::BaseColor);
  EXPECT_FALSE(parsed.success);
}

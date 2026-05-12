// Pyxis renderer — DDS / BCn parser implementation.

#include "GpuScene/DdsParser.h"

#include <cstring>

namespace pyxis::gpuscene_detail {

namespace {

[[nodiscard]] std::uint32_t ReadU32LE(const std::uint8_t* base, std::size_t offset) noexcept
{
  std::uint32_t value = 0;
  std::memcpy(&value, base + offset, 4u);
  return value;
}

}  // namespace

DdsParsedTexture ParseDds(std::span<const std::uint8_t> bytes,
                          TextureKey::Role role) noexcept
{
  DdsParsedTexture out;
  if (bytes.size() < 128u)
    return out;
  if (std::memcmp(bytes.data(), "DDS ", 4) != 0)
    return out;

  const std::uint8_t* hdr = bytes.data();
  out.width  = ReadU32LE(hdr, 16u);
  out.height = ReadU32LE(hdr, 12u);
  const std::uint32_t fourCC = ReadU32LE(hdr, 84u);

  const bool isSrgb = (role == TextureKey::Role::BaseColor
                       || role == TextureKey::Role::Emission);
  std::uint32_t pixelOffset = 128u;

  if (fourCC == 0x31545844u)  // "DXT1"
  {
    out.format = isSrgb ? nvrhi::Format::BC1_UNORM_SRGB : nvrhi::Format::BC1_UNORM;
    out.bytesPerBlock = 8u;
  }
  else if (fourCC == 0x35545844u)  // "DXT5"
  {
    out.format = isSrgb ? nvrhi::Format::BC3_UNORM_SRGB : nvrhi::Format::BC3_UNORM;
    out.bytesPerBlock = 16u;
  }
  else if (fourCC == 0x32495441u)  // "ATI2" (BC5)
  {
    out.format = nvrhi::Format::BC5_UNORM;
    out.bytesPerBlock = 16u;
  }
  else if (fourCC == 0x30315844u && bytes.size() >= 148u)  // "DX10"
  {
    const std::uint32_t dxgi = ReadU32LE(hdr, 128u);
    pixelOffset = 148u;
    out.bytesPerBlock = 16u;  // overridden below for BC1
    if (dxgi == 71u || dxgi == 72u)
    {
      out.format = isSrgb ? nvrhi::Format::BC1_UNORM_SRGB : nvrhi::Format::BC1_UNORM;
      out.bytesPerBlock = 8u;
    }
    else if (dxgi == 77u || dxgi == 78u)
    {
      out.format = isSrgb ? nvrhi::Format::BC3_UNORM_SRGB : nvrhi::Format::BC3_UNORM;
    }
    else if (dxgi == 83u)
    {
      out.format = nvrhi::Format::BC5_UNORM;
    }
    else if (dxgi == 98u || dxgi == 99u)
    {
      out.format = isSrgb ? nvrhi::Format::BC7_UNORM_SRGB : nvrhi::Format::BC7_UNORM;
    }
    else
    {
      return out;  // unsupported DXGI format
    }
  }
  else
  {
    return out;  // unsupported FourCC
  }

  if (out.width == 0u || out.height == 0u || bytes.size() <= pixelOffset)
    return out;

  out.pixelOffset = pixelOffset;
  out.success = true;
  return out;
}

}  // namespace pyxis::gpuscene_detail

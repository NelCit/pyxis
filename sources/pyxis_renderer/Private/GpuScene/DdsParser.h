// Pyxis renderer — DDS / BCn parser.
//
// V2.A.14. Pulled out of Commit.cpp's inline texture-decode block so
// the unit-test harness can drive it with hand-crafted byte buffers
// (no Vulkan device, no CommitResources). Parses:
//   * The 128-byte DDS_HEADER (magic + size + dims + FourCC).
//   * The optional 20-byte DXT10 extension (DXGI format).
// Maps the encoded format to a `nvrhi::Format::BCn_UNORM[_SRGB]`
// based on the texture role (sRGB EOTF for BaseColor / Emission;
// linear for everything else).
//
// Returns success=false on truncated / unsupported / non-BCn files;
// the caller falls through to the magenta-missing-texture path.

#pragma once

#include <Pyxis/Renderer/Descs/TextureKey.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>

namespace pyxis::gpuscene_detail {

struct DdsParsedTexture {
  bool          success      = false;
  std::uint32_t width        = 0;
  std::uint32_t height       = 0;
  nvrhi::Format format       = nvrhi::Format::UNKNOWN;
  std::uint32_t pixelOffset  = 0;  // byte offset where BCn block data starts
  std::uint32_t bytesPerBlock = 0;  // 8 (BC1) or 16 (BC3/5/7)
};

// Parse the DDS header bytes; `bytes` must point at the file start
// (the "DDS " magic). Returns success=true with the format /
// pixelOffset / dimensions filled in, or success=false on any
// recognisable failure mode.
[[nodiscard]] DdsParsedTexture ParseDds(std::span<const std::uint8_t> bytes,
                                        TextureKey::Role role) noexcept;

}  // namespace pyxis::gpuscene_detail

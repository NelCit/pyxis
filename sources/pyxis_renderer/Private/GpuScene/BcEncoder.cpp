// Pyxis renderer — V2.A.14 BCn texture encoder implementation.
//
// stb_dxt's `STB_DXT_IMPLEMENTATION` symbol is defined in StbDxt.cpp
// (mirrors the StbImageWrite.cpp pattern). This TU pulls declarations
// only.

#include "GpuScene/BcEncoder.h"

#include <stb_dxt.h>

#include <cstring>

namespace pyxis::gpuscene_detail {

void EncodeBCn(const std::uint8_t* srcRgba,
               std::uint32_t       width,
               std::uint32_t       height,
               TextureKey::Role    role,
               std::vector<std::uint8_t>& outBlocks,
               nvrhi::Format&      outFormat,
               std::size_t&        outBlockBytes) noexcept
{
  const std::uint32_t blocksWide  = width / 4u;
  const std::uint32_t blocksTall  = height / 4u;
  const std::uint32_t totalBlocks = blocksWide * blocksTall;

  enum class EncoderKind { Bc1, Bc4, Bc5 };
  EncoderKind encoder = EncoderKind::Bc1;
  switch (role)
  {
    case TextureKey::Role::BaseColor:
    case TextureKey::Role::Emission:
      encoder       = EncoderKind::Bc1;
      outFormat     = nvrhi::Format::BC1_UNORM_SRGB;
      outBlockBytes = 8u;
      break;
    case TextureKey::Role::NormalMap:
      encoder       = EncoderKind::Bc5;
      outFormat     = nvrhi::Format::BC5_UNORM;
      outBlockBytes = 16u;
      break;
    case TextureKey::Role::RoughnessMetallic:
    default:
      // ORM packs AO/roughness/metallic in R/G/B; the closesthit reads
      // roughness from .g and metallic from .b. BC4 is single-channel —
      // it would drop two of the three channels, silently zeroing
      // roughness + metallic. BC1 (linear RGB) keeps all three at the
      // same 8-byte block size. BC1's slight per-channel quantisation is
      // imperceptible on linear ORM data (vs the sRGB warm-cast that
      // makes us skip BC1 for BaseColor).
      encoder       = EncoderKind::Bc1;
      outFormat     = nvrhi::Format::BC1_UNORM;
      outBlockBytes = 8u;
      break;
  }
  outBlocks.assign(static_cast<std::size_t>(totalBlocks) * outBlockBytes, 0u);

  alignas(16) std::uint8_t blockRgba[64];
  std::uint8_t blockR[16];
  std::uint8_t blockRg[32];

  for (std::uint32_t blockY = 0; blockY < blocksTall; ++blockY)
  {
    for (std::uint32_t blockX = 0; blockX < blocksWide; ++blockX)
    {
      for (std::uint32_t row = 0; row < 4u; ++row)
      {
        const std::uint32_t srcY    = blockY * 4u + row;
        const std::uint32_t srcCol  = blockX * 4u;
        const std::size_t   srcOffs =
            (static_cast<std::size_t>(srcY) * width + srcCol) * 4u;
        std::memcpy(&blockRgba[static_cast<std::size_t>(row) * 16u],
                    srcRgba + srcOffs, 16u);
      }

      const std::size_t blockIdx =
          static_cast<std::size_t>(blockY) * blocksWide + blockX;
      std::uint8_t* dst = outBlocks.data() + blockIdx * outBlockBytes;

      switch (encoder)
      {
        case EncoderKind::Bc1:
          stb_compress_dxt_block(dst, blockRgba, /*alpha*/ 0, STB_DXT_NORMAL);
          break;
        case EncoderKind::Bc4:
          // BC4 needs 16-byte single-channel block. Pack .g per the
          // OpenPBR ORM convention (roughness in green).
          for (std::uint32_t pixel = 0; pixel < 16u; ++pixel)
            blockR[pixel] = blockRgba[pixel * 4u + 1u];
          stb_compress_bc4_block(dst, blockR);
          break;
        case EncoderKind::Bc5:
          // BC5 packs R + G of a normal map; Z reconstructed in shader.
          for (std::uint32_t pixel = 0; pixel < 16u; ++pixel)
          {
            blockRg[pixel * 2u + 0u] = blockRgba[pixel * 4u + 0u];
            blockRg[pixel * 2u + 1u] = blockRgba[pixel * 4u + 1u];
          }
          stb_compress_bc5_block(dst, blockRg);
          break;
      }
    }
  }
}

}  // namespace pyxis::gpuscene_detail

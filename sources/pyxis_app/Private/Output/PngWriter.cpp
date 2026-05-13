// Pyxis app — PNG writer implementation.

#include "Output/PngWriter.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stb_image_write.h>
#include <string>
#include <vector>

namespace pyxis::app {

namespace {

namespace fs = std::filesystem;

// 256-entry linear→sRGB encode LUT. Built once at first WritePngBgra8
// call (no thread-safety concerns — the headless path is single-
// threaded, and the LUT is content-deterministic so a benign race
// would still produce identical bytes).
//
// sRGB encode (linear → display) per IEC 61966-2-1:
//   if c <= 0.0031308   :   c' = 12.92 * c
//   else                :   c' = 1.055 * pow(c, 1/2.4) - 0.055
// then quantise to 8-bit. Pre-baked so the hot path is one indexed
// load per channel; the float math runs exactly 256 times across
// the process lifetime.
const std::array<uint8_t, 256>& LinearToSrgbLut() noexcept
{
  static const std::array<uint8_t, 256> LUT = []() {
    std::array<uint8_t, 256> table{};
    for (int idx = 0; idx < 256; ++idx)
    {
      const double linear = static_cast<double>(idx) / 255.0;
      double srgb = 0.0;
      if (linear <= 0.0031308)
      {
        srgb = 12.92 * linear;
      }
      else
      {
        srgb = (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
      }
      const double quantised = std::round(srgb * 255.0);
      table[static_cast<std::size_t>(idx)] =
          static_cast<uint8_t>(quantised < 0.0 ? 0.0 : (quantised > 255.0 ? 255.0 : quantised));
    }
    return table;
  }();
  return LUT;
}

}  // namespace

std::expected<void, std::string> WritePngBgra8(std::string_view filePath, uint32_t width,
                                               uint32_t height, const void* bgra8Pixels,
                                               std::size_t rowPitchBytes) noexcept
{
  if (filePath.empty())
    return std::unexpected{std::string{"WritePngBgra8: empty path"}};
  if (bgra8Pixels == nullptr)
    return std::unexpected{std::string{"WritePngBgra8: null source"}};
  if (width == 0 || height == 0)
    return std::unexpected{std::string{"WritePngBgra8: zero dim"}};
  if (rowPitchBytes < static_cast<std::size_t>(width) * 4u)
    return std::unexpected{std::string{"WritePngBgra8: rowPitch < width*4"}};

  const fs::path target{std::string{filePath}};
  if (target.has_parent_path())
  {
    std::error_code errorCode;
    fs::create_directories(target.parent_path(), errorCode);
  }

  const std::array<uint8_t, 256>& lut = LinearToSrgbLut();

  // BGRA8 → RGBA8 swizzle + per-channel sRGB encode via LUT. Alpha
  // bypasses the encode (sRGB only applies to colour channels).
  std::vector<uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u);
  const auto* src = static_cast<const uint8_t*>(bgra8Pixels);
  for (uint32_t row = 0; row < height; ++row)
  {
    const uint8_t* srcRow = src + (static_cast<std::size_t>(row) * rowPitchBytes);
    uint8_t* dstRow = rgba.data() + (static_cast<std::size_t>(row) * width * 4u);
    for (uint32_t col = 0; col < width; ++col)
    {
      const uint8_t blue  = srcRow[col * 4 + 0];
      const uint8_t green = srcRow[col * 4 + 1];
      const uint8_t red   = srcRow[col * 4 + 2];
      const uint8_t alpha = srcRow[col * 4 + 3];
      dstRow[col * 4 + 0] = lut[red];
      dstRow[col * 4 + 1] = lut[green];
      dstRow[col * 4 + 2] = lut[blue];
      dstRow[col * 4 + 3] = alpha;
    }
  }

  // Pin stb's deflate level + filter strategy for byte-equal output
  // across runs. Default (-1, 8) is already deterministic, but
  // pinning guards against a future stb upgrade flipping the
  // default mid-flight.
  stbi_write_png_compression_level = 8;
  stbi_write_force_png_filter = 0;  // no per-row filter (filter type 0 — None).

  const std::string targetUtf8 = target.generic_string();
  const int wrote = stbi_write_png(targetUtf8.c_str(), static_cast<int>(width),
                                   static_cast<int>(height), 4, rgba.data(),
                                   static_cast<int>(width * 4));
  if (wrote == 0)
    return std::unexpected{std::string{"WritePngBgra8: stbi_write_png failed for "} + targetUtf8};

  Logging::Get().Info(log::APP, "WritePng: " + targetUtf8 + " (" + std::to_string(width) + "x"
                                    + std::to_string(height) + " sRGB-encoded RGBA8)");
  return {};
}

}  // namespace pyxis::app

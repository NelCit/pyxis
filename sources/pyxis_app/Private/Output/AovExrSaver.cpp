// Pyxis app — AOV-to-EXR saver implementation.

#include "Output/AovExrSaver.h"

#include "Output/ExrWriter.h"
#include "Output/TextureReadback.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace pyxis::app {

namespace {

// IEEE 754 binary16 -> binary32. Public-domain bit-fiddle; handles
// zero / subnormal / inf / NaN so an unexpected NaN sample doesn't
// poison the EXR. Same code that previously lived in ViewerMode.
float HalfToFloat(uint16_t halfBits) noexcept {
  const uint32_t sign     = (halfBits & 0x8000u) << 16;
  const uint32_t exponent = (halfBits & 0x7C00u) >> 10;
  const uint32_t mantissa = (halfBits & 0x03FFu);
  uint32_t bits = sign;
  if (exponent == 0)
  {
    if (mantissa != 0)
    {
      uint32_t mant = mantissa;
      uint32_t expo = 1;
      while ((mant & 0x0400u) == 0)
      {
        mant <<= 1;
        ++expo;
      }
      mant &= 0x03FFu;
      bits |= ((127u - 15u - expo + 1u) << 23) | (mant << 13);
    }
  }
  else if (exponent == 0x1Fu)
  {
    bits |= 0x7F800000u | (mantissa << 13);
  }
  else
  {
    bits |= ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
  }
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

// Repack one row of a freshly-readback staging texture into the
// interleaved RGBA float32 layout WriteExrRgba32f wants. Per-AOV
// format dispatch:
//   RGBA16_FLOAT     : half-to-float every channel
//   RGBA32_FLOAT     : memcpy fast path
//   R32_FLOAT        : grayscale RGB(d, d, d, 1)
//   R32_UINT         : float-cast RGB(id, id, id, 1)
// Unrecognised formats fill zeros and the caller's WriteExr will
// still produce a valid (if useless) EXR — the SaveAovAsExr return
// value flags the bad format.
bool ConvertAovRowToRgba32f(nvrhi::Format format, const void* srcRow, uint32_t width,
                            float* dstRgbaRow) noexcept {
  switch (format)
  {
    case nvrhi::Format::RGBA16_FLOAT:
    {
      const auto* src = static_cast<const uint16_t*>(srcRow);
      for (uint32_t col = 0; col < width; ++col)
      {
        dstRgbaRow[col * 4 + 0] = HalfToFloat(src[col * 4 + 0]);
        dstRgbaRow[col * 4 + 1] = HalfToFloat(src[col * 4 + 1]);
        dstRgbaRow[col * 4 + 2] = HalfToFloat(src[col * 4 + 2]);
        dstRgbaRow[col * 4 + 3] = HalfToFloat(src[col * 4 + 3]);
      }
      return true;
    }
    case nvrhi::Format::RGBA32_FLOAT:
    {
      std::memcpy(dstRgbaRow, srcRow,
                  static_cast<std::size_t>(width) * 4u * sizeof(float));
      return true;
    }
    case nvrhi::Format::R32_FLOAT:
    {
      const auto* src = static_cast<const float*>(srcRow);
      for (uint32_t col = 0; col < width; ++col)
      {
        const float depthVal = src[col];
        dstRgbaRow[col * 4 + 0] = depthVal;
        dstRgbaRow[col * 4 + 1] = depthVal;
        dstRgbaRow[col * 4 + 2] = depthVal;
        dstRgbaRow[col * 4 + 3] = 1.0f;
      }
      return true;
    }
    case nvrhi::Format::R32_UINT:
    {
      const auto* src = static_cast<const uint32_t*>(srcRow);
      for (uint32_t col = 0; col < width; ++col)
      {
        const float idAsFloat = static_cast<float>(src[col]);
        dstRgbaRow[col * 4 + 0] = idAsFloat;
        dstRgbaRow[col * 4 + 1] = idAsFloat;
        dstRgbaRow[col * 4 + 2] = idAsFloat;
        dstRgbaRow[col * 4 + 3] = 1.0f;
      }
      return true;
    }
    default:
      std::memset(dstRgbaRow, 0, static_cast<std::size_t>(width) * 4u * sizeof(float));
      return false;
  }
}

}  // namespace

std::expected<void, std::string> SaveAovAsExr(nvrhi::IDevice* device,
                                              nvrhi::ICommandList* commandList,
                                              nvrhi::ITexture* aov,
                                              std::string_view debugName,
                                              std::string_view filePath) noexcept {
  if (device == nullptr)
    return std::unexpected{std::string{"SaveAovAsExr: null device"}};
  if (commandList == nullptr)
    return std::unexpected{std::string{"SaveAovAsExr: null commandList"}};
  if (aov == nullptr)
    return std::unexpected{std::string{"SaveAovAsExr: null source texture"}};
  if (filePath.empty())
    return std::unexpected{std::string{"SaveAovAsExr: empty target path"}};

  // Stash the debug name as a null-terminated cstring on the stack —
  // RecordCopy's contract is `const char*`.
  const std::string debugNameOwned{debugName};

  commandList->open();
  auto readbackResult =
      TextureReadback::RecordCopy(device, commandList, aov, debugNameOwned.c_str());
  commandList->close();
  device->executeCommandList(commandList);
  device->waitForIdle();
  if (!readbackResult)
  {
    return std::unexpected{"SaveAovAsExr: " + readbackResult.error()};
  }

  TextureReadback readback = std::move(*readbackResult);
  if (auto mapResult = readback.Map(); !mapResult)
  {
    return std::unexpected{"SaveAovAsExr map: " + mapResult.error()};
  }

  const uint32_t aovWidth  = readback.Width();
  const uint32_t aovHeight = readback.Height();
  const std::size_t srcRowPitch = readback.RowPitch();
  std::vector<float> rgbaFloats(static_cast<std::size_t>(aovWidth) * aovHeight * 4u);
  const auto* srcBytes = static_cast<const uint8_t*>(readback.Data());
  bool formatOk = true;
  for (uint32_t row = 0; row < aovHeight; ++row)
  {
    const void* srcRow = srcBytes + (static_cast<std::size_t>(row) * srcRowPitch);
    float* dstRow = rgbaFloats.data() + (static_cast<std::size_t>(row) * aovWidth * 4u);
    if (!ConvertAovRowToRgba32f(readback.Format(), srcRow, aovWidth, dstRow))
    {
      formatOk = false;
    }
  }
  if (!formatOk)
  {
    return std::unexpected{
        "SaveAovAsExr: unsupported source format (only RGBA16F / RGBA32F / R32F / "
        "R32_UINT are recognised)"};
  }

  const std::size_t dstRowPitch =
      static_cast<std::size_t>(aovWidth) * 4u * sizeof(float);
  if (auto writeResult = WriteExrRgba32f(filePath, aovWidth, aovHeight,
                                         rgbaFloats.data(), dstRowPitch);
      !writeResult)
  {
    return std::unexpected{"SaveAovAsExr write: " + writeResult.error()};
  }
  return {};
}

}  // namespace pyxis::app

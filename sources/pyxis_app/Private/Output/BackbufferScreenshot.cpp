// Pyxis app — viewer screenshot helper implementation.

#include "Output/BackbufferScreenshot.h"

#include "Output/TextureReadback.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <stb_image_write.h>
#include <string>
#include <vector>

namespace pyxis::app {

bool CaptureBackbufferToPng(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
                            nvrhi::ITexture* backbuffer, std::string_view pngPath) noexcept {
  auto& log = Logging::Get();
  if (!device || !commandList || !backbuffer || pngPath.empty())
    return false;

  auto readback =
      TextureReadback::RecordCopy(device, commandList, backbuffer, "screenshot-staging");
  if (!readback)
  {
    log.Error(log::APP, "screenshot: " + readback.error());
    return false;
  }

  commandList->setTextureState(backbuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
  commandList->commitBarriers();
  commandList->close();
  device->executeCommandList(commandList);
  device->waitForIdle();

  if (auto mapResult = readback->Map(); !mapResult)
  {
    log.Error(log::APP, "screenshot: " + mapResult.error());
    return false;
  }

  // BGRA8 → RGBA8 swizzle, contiguous row pitch for stb_image_write.
  const std::size_t imageWidth = readback->Width();
  const std::size_t imageHeight = readback->Height();
  const std::size_t rowPitch = readback->RowPitch();
  std::vector<uint8_t> rgba(imageWidth * imageHeight * 4);
  const auto* src = static_cast<const uint8_t*>(readback->Data());
  for (std::size_t row = 0; row < imageHeight; ++row)
  {
    const uint8_t* srcRow = src + row * rowPitch;
    uint8_t* dstRow = rgba.data() + (row * imageWidth * 4);
    for (std::size_t col = 0; col < imageWidth; ++col)
    {
      dstRow[col * 4 + 0] = srcRow[col * 4 + 2];  // R ← B
      dstRow[col * 4 + 1] = srcRow[col * 4 + 1];  // G ← G
      dstRow[col * 4 + 2] = srcRow[col * 4 + 0];  // B ← R
      dstRow[col * 4 + 3] = srcRow[col * 4 + 3];  // A ← A
    }
  }

  const std::string path{pngPath};
  const int wrote =
      stbi_write_png(path.c_str(), static_cast<int>(imageWidth), static_cast<int>(imageHeight), 4,
                     rgba.data(), static_cast<int>(imageWidth * 4));
  if (wrote == 0)
  {
    log.Error(log::APP, "screenshot: stbi_write_png failed");
    return false;
  }

  // Quick sanity: any pixel non-black? Lets the caller assert "the
  // triangle actually rendered" without a separate harness.
  bool anyNonBlack = false;
  for (std::size_t pix = 0; pix + 2 < rgba.size(); pix += 4)
  {
    if (rgba[pix] != 0 || rgba[pix + 1] != 0 || rgba[pix + 2] != 0)
    {
      anyNonBlack = true;
      break;
    }
  }
  std::string msg = "screenshot: wrote ";
  msg += std::to_string(imageWidth) + "x" + std::to_string(imageHeight);
  msg += " PNG to ";
  msg.append(pngPath);
  msg += anyNonBlack ? "  (non-black pixels present — render OK)"
                     : "  (image is fully black — render likely broken)";
  log.Info(log::APP, msg);
  return true;
}

}  // namespace pyxis::app

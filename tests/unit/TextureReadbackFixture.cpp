// Pyxis platform — TextureReadback helper tests.
//
// pyxis::TextureReadback (pyxis_platform/Public) is the shared GPU->CPU readback
// RAII used by the viewer --screenshot, headless EXR, AOV-EXR saver, and the Hydra
// delegate's per-frame color readback. These tests pin its contract: it copies a
// texture to CPU, strips the staging row pitch tightly (CopyTightlyInto), and
// REUSES its staging texture across RecordCopy calls (the per-frame path must not
// reallocate). Skips cleanly on CPU-only CI.

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Gpu/TextureReadback.h>

#include <nvrhi/nvrhi.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using pyxis::CreateHeadlessDeviceManager;
using pyxis::DeviceCreationParams;
using pyxis::DeviceManagerCreateStatus;
using pyxis::IDeviceManager;
using pyxis::Resolution;
using pyxis::TextureReadback;

namespace {

constexpr uint32_t TEX_WIDTH = 32;
constexpr uint32_t TEX_HEIGHT = 16;

std::unique_ptr<IDeviceManager> MakeDevice() {
  DeviceCreationParams params;
  params.framesInFlight = 1;
  params.applicationName = "pyxis_readback_test";
  DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
  std::unique_ptr<IDeviceManager> manager(
      CreateHeadlessDeviceManager(params, Resolution{TEX_WIDTH, TEX_HEIGHT}, &status));
  if (!manager || status != DeviceManagerCreateStatus::Ok || manager->GetDevice() == nullptr)
    return nullptr;
  return manager;
}

// A render-target texture cleared to `color`, ready to read back.
nvrhi::TextureHandle MakeClearedTexture(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
                                        const nvrhi::Color& color) {
  nvrhi::TextureDesc desc;
  desc.width = TEX_WIDTH;
  desc.height = TEX_HEIGHT;
  desc.format = nvrhi::Format::RGBA8_UNORM;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isRenderTarget = true;
  desc.isUAV = true;
  desc.initialState = nvrhi::ResourceStates::RenderTarget;
  desc.keepInitialState = true;
  desc.debugName = "readback-test-src";
  nvrhi::TextureHandle tex = device->createTexture(desc);
  if (!tex)
    return nullptr;
  commandList->open();
  commandList->clearTextureFloat(tex, nvrhi::AllSubresources, color);
  commandList->close();
  device->executeCommandList(commandList);
  device->waitForIdle();
  return tex;
}

}  // namespace

TEST(TextureReadback, CopiesAndStripsRowPitch) {
  const std::unique_ptr<IDeviceManager> manager = MakeDevice();
  if (!manager)
    GTEST_SKIP() << "No Vulkan device (CPU-only CI).";
  nvrhi::IDevice* device = manager->GetDevice();
  const nvrhi::CommandListHandle commandList = device->createCommandList();

  // (1.0, 0.5, 0.25, 1.0) -> 8-bit UNORM (255, 128, 64, 255).
  const nvrhi::TextureHandle tex =
      MakeClearedTexture(device, commandList, nvrhi::Color(1.0f, 0.5f, 0.25f, 1.0f));
  ASSERT_NE(tex, nullptr);

  TextureReadback readback;
  commandList->open();
  // Caller transitions the source to CopySource before RecordCopy (same contract
  // PyxisEngine uses); RecordCopy then records the copyTexture.
  commandList->setTextureState(tex, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
  commandList->commitBarriers();
  ASSERT_TRUE(readback.RecordCopy(device, commandList, tex, "test").has_value());
  commandList->close();
  device->executeCommandList(commandList);
  device->waitForIdle();

  ASSERT_TRUE(readback.Map().has_value());
  EXPECT_EQ(readback.Width(), TEX_WIDTH);
  EXPECT_EQ(readback.Height(), TEX_HEIGHT);
  EXPECT_EQ(readback.Format(), nvrhi::Format::RGBA8_UNORM);
  EXPECT_GE(readback.RowPitch(), static_cast<size_t>(TEX_WIDTH) * 4u);  // pitch >= tight row

  std::vector<uint8_t> tight;
  readback.CopyTightlyInto(tight);
  ASSERT_EQ(tight.size(), static_cast<size_t>(TEX_WIDTH) * TEX_HEIGHT * 4u);  // pitch stripped
  // ±1: the GPU's float->UNORM clear rounding is implementation-defined at .5
  // boundaries (0.5*255 = 127.5 -> 127 here). The point is the readback is
  // faithful + tightly packed, not exact GPU rounding.
  for (size_t pixel = 0; pixel < tight.size(); pixel += 4) {
    EXPECT_NEAR(tight[pixel + 0], 255, 1) << "pixel=" << pixel;  // 1.0
    EXPECT_NEAR(tight[pixel + 1], 128, 1) << "pixel=" << pixel;  // 0.5
    EXPECT_NEAR(tight[pixel + 2], 64, 1) << "pixel=" << pixel;   // 0.25
    EXPECT_NEAR(tight[pixel + 3], 255, 1) << "pixel=" << pixel;  // 1.0
  }
  readback.Unmap();
}

TEST(TextureReadback, ReusesStagingAcrossCalls) {
  const std::unique_ptr<IDeviceManager> manager = MakeDevice();
  if (!manager)
    GTEST_SKIP() << "No Vulkan device (CPU-only CI).";
  nvrhi::IDevice* device = manager->GetDevice();
  const nvrhi::CommandListHandle commandList = device->createCommandList();

  TextureReadback readback;  // one instance, reused (the per-frame pattern)
  for (int iter = 0; iter < 5; ++iter) {
    const float gray = static_cast<float>(iter) / 8.0f;  // distinct value each "frame"
    const nvrhi::TextureHandle tex =
        MakeClearedTexture(device, commandList, nvrhi::Color(gray, gray, gray, 1.0f));
    ASSERT_NE(tex, nullptr) << "iter " << iter;

    commandList->open();
    commandList->setTextureState(tex, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->commitBarriers();
    ASSERT_TRUE(readback.RecordCopy(device, commandList, tex, "reuse").has_value()) << "iter " << iter;
    commandList->close();
    device->executeCommandList(commandList);
    device->waitForIdle();

    ASSERT_TRUE(readback.Map().has_value()) << "iter " << iter;
    std::vector<uint8_t> tight;
    readback.CopyTightlyInto(tight);
    ASSERT_EQ(tight.size(), static_cast<size_t>(TEX_WIDTH) * TEX_HEIGHT * 4u);
    const int expect = static_cast<int>(std::lround(gray * 255.0f));
    EXPECT_NEAR(tight[0], expect, 1) << "iter " << iter;  // reused staging still reads correctly
    readback.Unmap();
  }
}

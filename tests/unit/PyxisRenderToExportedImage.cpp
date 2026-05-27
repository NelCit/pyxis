// RFC 0004 Stage 3 (C4) — the render data-path.
//
// Proves the core C4 claim on real hardware: PyxisRenderer renders directly
// into a GpuInteropExporter-created (Win32-shareable) texture. Combined with
// the C3 cross-device round-trip (which proved a second device imports + reads
// such a texture), this establishes the full Kit viewport handoff: Pyxis
// renders -> exportable image -> Kit imports, zero host copy.
//
// Method: allocate the exportable RGBA16F color target, clear it to a magenta
// sentinel, drive a real RenderFrame into it, then read it back and assert the
// renderer OVERWROTE the sentinel (i.e. it actually wrote into the shared
// texture) with finite values. Skips on CPU-only CI.

#include <gtest/gtest.h>

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Interop/GpuInteropExporter.h>
#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <memory>

using namespace pyxis;

namespace {

constexpr uint32_t kW = 128;
constexpr uint32_t kH = 128;
constexpr uint32_t VK_FORMAT_R16G16B16A16_SFLOAT_ = 97;  // matches exporter table.

float HalfToFloat(uint16_t half) {
  const uint32_t sign = (half >> 15) & 0x1u;
  const uint32_t exp = (half >> 10) & 0x1Fu;
  const uint32_t mant = half & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign << 31;
    } else {
      int e = -1;
      uint32_t m = mant;
      do {
        ++e;
        m <<= 1;
      } while ((m & 0x400u) == 0);
      const uint32_t fe = static_cast<uint32_t>(127 - 15 - e);
      bits = (sign << 31) | (fe << 23) | ((m & 0x3FFu) << 13);
    }
  } else if (exp == 0x1F) {
    bits = (sign << 31) | (0xFFu << 23) | (mant << 13);
  } else {
    bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

}  // namespace

TEST(PyxisRenderToExportedImage, RenderFrameWritesIntoExportableTexture) {
  DeviceCreationParams params;
  params.framesInFlight = 1;  // PyxisRenderer picker path asserts FIF == 1.
  params.applicationName = "pyxis_c4_test";
  const Resolution res{kW, kH};
  DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
  std::unique_ptr<IDeviceManager> dm(CreateHeadlessDeviceManager(params, res, &status));
  if (!dm || status != DeviceManagerCreateStatus::Ok || dm->GetDevice() == nullptr)
    GTEST_SKIP() << "No Vulkan device (CPU-only CI).";
  nvrhi::IDevice* device = dm->GetDevice();

  auto exporter = GpuInteropExporter::Create(dm->GetVulkanContext(), device);
  if (!exporter)
    GTEST_SKIP() << "External-memory interop unavailable on this adapter.";

  // The exportable color target Kit would import — Pyxis renders straight into it.
  const ExportedImage shared =
      exporter->CreateExportableImage(kW, kH, VK_FORMAT_R16G16B16A16_SFLOAT_, true);
  ASSERT_TRUE(shared.IsValid());
  nvrhi::ITexture* colorTarget = shared.texture;

  // Real renderer stack.
  Profiler profiler{device};
  GpuSceneCreateDesc sceneDesc{};
  sceneDesc.framesInFlight = 1;
  GpuScene scene{device, profiler, sceneDesc};
  RendererCreateDesc rendererDesc{};
  rendererDesc.initialWidth = kW;
  rendererDesc.initialHeight = kH;
  rendererDesc.framesInFlight = 1;
  PyxisRenderer renderer{device, scene, profiler, rendererDesc};

  // A non-degenerate camera so the path tracer dispatches rays (an empty scene
  // then renders the miss/background — enough to overwrite the sentinel).
  CameraDesc cam{};
  cam.viewFromWorld = hlslpp::float4x4::identity();
  cam.projFromView = hlslpp::float4x4::identity();
  scene.SetCamera(cam);

  // A minimal triangle so CommitResources builds a TLAS — without geometry the
  // path tracer has nothing to dispatch against and never writes the target.
  // (Arrays must outlive CommitResources; the descs borrow spans.)
  const std::array<hlslpp::float3, 3> positions{
      hlslpp::float3{0.f, 0.f, 2.f}, hlslpp::float3{1.f, 0.f, 2.f},
      hlslpp::float3{0.f, 1.f, 2.f}};
  const std::array<uint32_t, 3> indices{0, 1, 2};
  MeshDesc meshDesc;
  meshDesc.positions = positions;
  meshDesc.indices = indices;
  meshDesc.debugName = "c4.triangle";
  auto meshHandle = scene.CreateMesh(meshDesc);
  ASSERT_TRUE(meshHandle.has_value());
  InstanceDesc instDesc;
  instDesc.mesh = *meshHandle;
  instDesc.worldFromLocal = hlslpp::float4x4::identity();
  instDesc.debugName = "c4.instance";
  ASSERT_TRUE(scene.AppendInstance(instDesc).has_value());

  // Staging readback target.
  nvrhi::TextureDesc stagingDesc = colorTarget->getDesc();
  nvrhi::StagingTextureHandle staging =
      device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
  ASSERT_TRUE(staging != nullptr);

  // The path tracer writes HDR radiance into colorHdr; bind a real (regular)
  // one (the headless path does too).
  nvrhi::TextureDesc hdrDesc;
  hdrDesc.width = kW;
  hdrDesc.height = kH;
  hdrDesc.format = nvrhi::Format::RGBA16_FLOAT;
  hdrDesc.dimension = nvrhi::TextureDimension::Texture2D;
  hdrDesc.isRenderTarget = true;
  hdrDesc.isUAV = true;
  hdrDesc.isShaderResource = true;
  hdrDesc.debugName = "test.colorHdr";
  hdrDesc.initialState = nvrhi::ResourceStates::RenderTarget;
  hdrDesc.keepInitialState = true;
  nvrhi::TextureHandle colorHdr = device->createTexture(hdrDesc);

  // Verify the EXPORTED (adopted, shareable) texture — the real C4 claim that
  // Pyxis renders into a texture Kit can import. It's bound as colorHdr below
  // (where the path tracer writes radiance).
  nvrhi::ITexture* verifyTex = colorTarget;

  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  // Magenta sentinel so we can prove the renderer overwrote it.
  cl->clearTextureFloat(verifyTex, nvrhi::AllSubresources, nvrhi::Color(1.f, 0.f, 1.f, 1.f));
  auto commitResult = scene.CommitResources(cl);
  ASSERT_TRUE(static_cast<bool>(commitResult)) << "CommitResources failed";

  // The path tracer writes HDR radiance into colorHdr — so the exported
  // (shareable) texture is bound THERE; `color` (the display target a tonemap
  // pass would fill) gets a throwaway texture since this minimal graph has no
  // tonemap. Kit imports the colorHdr radiance (or a tonemapped variant later).
  RenderTargets targets{};
  targets.color = colorHdr;        // regular display target (throwaway here)
  targets.colorHdr = colorTarget;  // exported, shareable == verifyTex
  RenderSettings settings{};
  settings.width = kW;
  settings.height = kH;
  renderer.RenderFrame(cl, settings, targets);
  cl->setTextureState(verifyTex, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
  cl->commitBarriers();
  cl->copyTexture(staging, nvrhi::TextureSlice(), verifyTex, nvrhi::TextureSlice());
  cl->close();
  device->executeCommandList(cl);
  device->waitForIdle();
  device->runGarbageCollection();

  size_t rowPitch = 0;
  const void* mapped =
      device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
  ASSERT_NE(mapped, nullptr);

  // Scan: every pixel must be finite, and at least one must differ from the
  // magenta sentinel (proving RenderFrame wrote into the shared texture).
  const auto* base = static_cast<const uint8_t*>(mapped);
  bool anyOverwritten = false;
  bool allFinite = true;
  for (uint32_t y = 0; y < kH; ++y) {
    const auto* row = reinterpret_cast<const uint16_t*>(base + size_t(y) * rowPitch);
    for (uint32_t x = 0; x < kW; ++x) {
      const float r = HalfToFloat(row[x * 4 + 0]);
      const float g = HalfToFloat(row[x * 4 + 1]);
      const float b = HalfToFloat(row[x * 4 + 2]);
      if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
        allFinite = false;
      // Sentinel was (1,0,1); "overwritten" = clearly not that.
      if (!(r > 0.9f && g < 0.1f && b > 0.9f))
        anyOverwritten = true;
    }
  }
  device->unmapStagingTexture(staging);

  EXPECT_TRUE(allFinite) << "RenderFrame produced non-finite pixels in the shared texture";
  EXPECT_TRUE(anyOverwritten)
      << "RenderFrame did not overwrite the magenta sentinel — it did not render "
         "into the exportable texture";
}

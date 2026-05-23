// Pyxis Omniverse Hydra delegate — render engine. RFC 0004 Stage 3 (C4).

#include "PyxisEngine.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Interop/GpuInteropExporter.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Error.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>

#include <nvrhi/nvrhi.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pyxis_omni {

namespace {
constexpr uint32_t VK_FORMAT_R16G16B16A16_SFLOAT_ = 97;  // matches exporter table.
}

struct PyxisEngine::Impl {
  // Declaration order = construction order; destroyed in reverse so the
  // exporter (which destroys its VkImages on the device) tears down before the
  // device manager frees the device.
  std::unique_ptr<pyxis::IDeviceManager> deviceManager;
  std::unique_ptr<pyxis::Profiler> profiler;
  std::unique_ptr<pyxis::GpuScene> scene;
  std::unique_ptr<pyxis::PyxisRenderer> renderer;
  std::unique_ptr<pyxis::GpuInteropExporter> exporter;
  nvrhi::CommandListHandle commandList;
  nvrhi::TextureHandle displayColor;  // throwaway `color` target (no tonemap pass yet).
  pyxis::ExportedImage exportedColor{};
  pyxis::ExportedSemaphore timeline{};
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t frameValue = 0;
  bool valid = false;
};

PyxisEngine::PyxisEngine() : _impl(std::make_unique<Impl>()) {}
PyxisEngine::~PyxisEngine() = default;

bool PyxisEngine::Initialize(uint32_t width, uint32_t height) noexcept {
  auto& log = pyxis::Logging::Get();
  Impl& impl = *_impl;
  impl.width = width;
  impl.height = height;

  pyxis::DeviceCreationParams params;
  params.framesInFlight = 1;
  params.applicationName = "pyxis.hydra.omni";
  const pyxis::Resolution res{width, height};
  pyxis::DeviceManagerCreateStatus status = pyxis::DeviceManagerCreateStatus::Unknown;
  impl.deviceManager.reset(pyxis::CreateHeadlessDeviceManager(params, res, &status));
  if (!impl.deviceManager || status != pyxis::DeviceManagerCreateStatus::Ok ||
      impl.deviceManager->GetDevice() == nullptr) {
    log.Error(pyxis::log::APP, "PyxisEngine: device creation failed");
    return false;
  }
  nvrhi::IDevice* device = impl.deviceManager->GetDevice();

  impl.exporter =
      pyxis::GpuInteropExporter::Create(impl.deviceManager->GetVulkanContext(), device);
  if (!impl.exporter) {
    log.Error(pyxis::log::APP, "PyxisEngine: external-memory interop unavailable");
    return false;
  }

  impl.profiler = std::make_unique<pyxis::Profiler>(device);
  pyxis::GpuSceneCreateDesc sceneDesc{};
  sceneDesc.framesInFlight = 1;
  impl.scene = std::make_unique<pyxis::GpuScene>(device, *impl.profiler, sceneDesc);
  pyxis::RendererCreateDesc rendererDesc{};
  rendererDesc.initialWidth = width;
  rendererDesc.initialHeight = height;
  rendererDesc.framesInFlight = 1;
  impl.renderer =
      std::make_unique<pyxis::PyxisRenderer>(device, *impl.scene, *impl.profiler, rendererDesc);

  // The path tracer writes radiance into colorHdr — bind the exportable image
  // there. `color` is a throwaway display target (no tonemap pass in v1 graph).
  impl.exportedColor =
      impl.exporter->CreateExportableImage(width, height, VK_FORMAT_R16G16B16A16_SFLOAT_, true);
  if (!impl.exportedColor.IsValid()) {
    log.Error(pyxis::log::APP, "PyxisEngine: exportable color allocation failed");
    return false;
  }
  impl.timeline = impl.exporter->CreateExportableTimelineSemaphore();

  nvrhi::TextureDesc displayDesc;
  displayDesc.width = width;
  displayDesc.height = height;
  displayDesc.format = nvrhi::Format::RGBA16_FLOAT;
  displayDesc.dimension = nvrhi::TextureDimension::Texture2D;
  displayDesc.isRenderTarget = true;
  displayDesc.isUAV = true;
  displayDesc.isShaderResource = true;
  displayDesc.debugName = "pyxis.omni.displayColor";
  displayDesc.initialState = nvrhi::ResourceStates::RenderTarget;
  displayDesc.keepInitialState = true;
  impl.displayColor = device->createTexture(displayDesc);

  impl.commandList = device->createCommandList();
  impl.valid = true;
  log.Info(pyxis::log::APP, "PyxisEngine: initialised (renders into exportable color)");
  return true;
}

void PyxisEngine::RenderFrame() noexcept {
  Impl& impl = *_impl;
  if (!impl.valid)
    return;
  nvrhi::IDevice* device = impl.deviceManager->GetDevice();

  impl.commandList->open();
  if (auto commit = impl.scene->CommitResources(impl.commandList); !commit) {
    std::string msg = "PyxisEngine: CommitResources failed: ";
    msg.append(commit.error().message.View());
    pyxis::Logging::Get().Error(pyxis::log::APP, msg);
    impl.commandList->close();
    return;
  }
  pyxis::RenderTargets targets{};
  targets.color = impl.displayColor;             // throwaway display target
  targets.colorHdr = impl.exportedColor.texture;  // exportable, shared with Kit
  pyxis::RenderSettings settings{};
  settings.width = impl.width;
  settings.height = impl.height;
  impl.renderer->RenderFrame(impl.commandList, settings, targets);
  // Leave the shared image in a layout the Kit side can sample after import.
  impl.commandList->setTextureState(impl.exportedColor.texture, nvrhi::AllSubresources,
                                    nvrhi::ResourceStates::ShaderResource);
  impl.commandList->commitBarriers();
  impl.commandList->close();
  device->executeCommandList(impl.commandList);

  // Publish "frame N ready" so the Kit side waits before sampling the image.
  ++impl.frameValue;
  impl.exporter->SignalTimeline(impl.timeline, impl.frameValue);
}

bool PyxisEngine::ReadbackColorHdr(std::vector<uint8_t>& outRgba16f, uint32_t& outWidth,
                                   uint32_t& outHeight) noexcept {
  Impl& impl = *_impl;
  if (!impl.valid || impl.exportedColor.texture == nullptr)
    return false;
  nvrhi::IDevice* device = impl.deviceManager->GetDevice();

  nvrhi::TextureDesc stagingDesc = impl.exportedColor.texture->getDesc();
  nvrhi::StagingTextureHandle staging =
      device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
  if (!staging)
    return false;

  impl.commandList->open();
  impl.commandList->setTextureState(impl.exportedColor.texture, nvrhi::AllSubresources,
                                    nvrhi::ResourceStates::CopySource);
  impl.commandList->commitBarriers();
  impl.commandList->copyTexture(staging, nvrhi::TextureSlice(), impl.exportedColor.texture,
                                nvrhi::TextureSlice());
  impl.commandList->close();
  device->executeCommandList(impl.commandList);
  device->waitForIdle();
  device->runGarbageCollection();

  size_t rowPitch = 0;
  const void* mapped =
      device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
  if (mapped == nullptr)
    return false;
  outWidth = impl.width;
  outHeight = impl.height;
  outRgba16f.resize(static_cast<size_t>(impl.width) * impl.height * 8u);  // 4 halfs/pixel
  const auto* src = static_cast<const uint8_t*>(mapped);
  const size_t tightRow = static_cast<size_t>(impl.width) * 8u;
  for (uint32_t y = 0; y < impl.height; ++y)
    std::memcpy(outRgba16f.data() + y * tightRow, src + y * rowPitch, tightRow);
  device->unmapStagingTexture(staging);
  return true;
}

const pyxis::ExportedImage& PyxisEngine::ExportedColor() const noexcept {
  return _impl->exportedColor;
}
const pyxis::ExportedSemaphore& PyxisEngine::Timeline() const noexcept { return _impl->timeline; }
uint64_t PyxisEngine::LastSignaledValue() const noexcept { return _impl->frameValue; }
bool PyxisEngine::IsValid() const noexcept { return _impl->valid; }
pyxis::GpuScene* PyxisEngine::Scene() const noexcept { return _impl->scene.get(); }
pyxis::Profiler* PyxisEngine::ProfilerPtr() const noexcept { return _impl->profiler.get(); }
uint64_t PyxisEngine::LastInstanceCount() const noexcept {
  return _impl->scene ? _impl->scene->LastFrameStats().instanceCount : 0;
}
uint64_t PyxisEngine::LastMeshCount() const noexcept {
  return _impl->scene ? _impl->scene->LastFrameStats().meshCount : 0;
}
uint64_t PyxisEngine::LastMaterialCount() const noexcept {
  return _impl->scene ? _impl->scene->LastFrameStats().materialCount : 0;
}
uint64_t PyxisEngine::LastLightCount() const noexcept {
  return _impl->scene ? _impl->scene->LastFrameStats().lightCount : 0;
}

}  // namespace pyxis_omni

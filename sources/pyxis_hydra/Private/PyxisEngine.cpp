// Pyxis Hydra delegate — render engine.

#include "PyxisEngine.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Gpu/TextureReadback.h>
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

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
// Directory containing this DLL. In Kit the host is kit.exe (not next to our
// shaders), so PyxisRenderer's shaderSearchPath must point at <ext>/bin where
// build.ps1 stages Resources/shaders.
std::string ThisModuleDir() {
  HMODULE module = nullptr;
  ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModuleDir), &module);
  wchar_t wide[MAX_PATH] = {};
  const DWORD len = ::GetModuleFileNameW(module, wide, MAX_PATH);
  std::wstring path(wide, len);
  const size_t slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos)
    path.resize(slash);
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string utf8(bytes > 0 ? bytes - 1 : 0, '\0');
  if (bytes > 0)
    ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
  return utf8;
}
}  // namespace

namespace pyxis::hydra {

namespace {
// VkFormat value for R16G16B16A16_SFLOAT (matches the exporter's format table);
// spelled as a constant so this TU need not pull <vulkan/vulkan.h>.
constexpr uint32_t EXPORT_COLOR_VK_FORMAT = 97;
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
  // Double-buffered readback (RFC 0008 "Why rejected": the Kit viewport AOV must be
  // read back to CPU — no portable GPU-AOV path exists in this stack). Two staging
  // textures ping-pong: each frame records frame N's copy into slot `cur` and MAPS
  // frame N-1's already-finished copy from the other slot, so the render thread
  // never stalls waiting for the readback copy. The shared pyxis::TextureReadback
  // reuses each slot's staging across frames (no per-frame alloc). EventQuery per
  // slot lets us wait for just that slot's copy (the prev slot is normally already
  // done -> no real stall). First frame / post-resize falls back to reading `cur`.
  pyxis::TextureReadback readback[2];
  nvrhi::EventQueryHandle readbackQuery[2];
  nvrhi::EventQueryHandle renderQuery;  // RFC 0008: "render retired" for the GL path
  bool readbackSlotValid[2] = {false, false};
  uint32_t readbackSlotW[2] = {0, 0};
  uint32_t readbackSlotH[2] = {0, 0};
  int readbackCur = 0;
  std::string shaderDir;  // backs rendererDesc.shaderSearchPath (string_view).
  pyxis::ExportedImage exportedColor{};
  pyxis::ExportedSemaphore timeline{};
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t frameValue = 0;
  bool valid = false;
};

PyxisEngine::PyxisEngine() : _impl(std::make_unique<Impl>()) {}

PyxisEngine::~PyxisEngine() {
  // Drain the GPU before tearing anything down. RenderFrame submits async work
  // (path-trace dispatch, and on heavy scenes RTXMU BLAS compaction copies that
  // retire later); destroying the renderer/scene/exporter/device while that work
  // is still in flight corrupts memory and crashes (STATUS_STACK_BUFFER_OVERRUN,
  // heavy-scene only — the smoke path happened to be safe because ReadbackColorHdr
  // already waits). This is also the teardown Kit hits when the user switches
  // renderers away from Pyxis, so it must be crash-free. waitForIdle blocks until
  // the GPU retires everything; runGarbageCollection then releases deferred
  // resources before Impl's members destruct (reverse order, device last).
  if (std::getenv("PYXIS_OMNI_DBG") != nullptr)
    std::fprintf(stderr, "PYXISDBG ~PyxisEngine: tearing down Vulkan device + GpuScene\n");
  if (_impl && _impl->deviceManager) {
    if (nvrhi::IDevice* device = _impl->deviceManager->GetDevice()) {
      device->waitForIdle();
      device->runGarbageCollection();
    }
  }
}

void PyxisEngine::WaitIdle() noexcept {
  if (_impl && _impl->deviceManager) {
    if (nvrhi::IDevice* device = _impl->deviceManager->GetDevice()) {
      device->waitForIdle();
      device->runGarbageCollection();
    }
  }
}

bool PyxisEngine::Initialize(uint32_t width, uint32_t height) noexcept {
  auto& log = pyxis::Logging::Get();
  Impl& impl = *_impl;
  impl.width = width;
  impl.height = height;

  pyxis::DeviceCreationParams params;
  params.framesInFlight = 1;
  params.applicationName = "pyxis.hydra";
  // Diagnostic: PYXIS_OMNI_VK_VALIDATION=1 turns on Vulkan validation layers to
  // catch GPU-side API misuse (e.g. an AS/vertex/descriptor buffer sized wrong
  // from the Hydra ingest data). Off by default (no perf hit in normal use).
  params.enableValidation = (std::getenv("PYXIS_OMNI_VK_VALIDATION") != nullptr);
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
  // Find the path-tracer shaders relative to this DLL (build.ps1 stages them at
  // <ext>/bin/Resources/shaders) — the host kit.exe is not next to them. The
  // passes resolve shaders through AssetLocator, which by default uses the EXE
  // dir (kit.exe) where our shaders do NOT live; point its Resources/ base at
  // our bin/Resources so PathTracePass/SsaaResolvePass actually find the .spv.
  // (rendererDesc.shaderSearchPath is currently unused by the renderer.)
  impl.shaderDir = ThisModuleDir();
  rendererDesc.shaderSearchPath = impl.shaderDir;
  pyxis::AssetLocator::SetResourcesDirectoryOverride(impl.shaderDir + "/Resources");
  impl.renderer =
      std::make_unique<pyxis::PyxisRenderer>(device, *impl.scene, *impl.profiler, rendererDesc);

  // The path tracer writes radiance into colorHdr — bind the exportable image
  // there. `color` is a throwaway display target (no tonemap pass in v1 graph).
  impl.exportedColor =
      impl.exporter->CreateExportableImage(width, height, EXPORT_COLOR_VK_FORMAT, true);
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
  displayDesc.debugName = "pyxis.hydra.displayColor";
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

  // Bracket the frame so the Profiler advances its slot ring and recycles timer
  // queries. The standalone app drives BeginFrame/EndFrame; the delegate must do
  // it too, or the per-frame scope queries are never released and the NVRHI
  // query pool exhausts after a while ("Insufficient query pool space"). EndFrame
  // must run on EVERY return path to keep the pair balanced.
  impl.profiler->BeginFrame();

  impl.commandList->open();
  if (auto commit = impl.scene->CommitResources(impl.commandList); !commit) {
    std::string msg = "PyxisEngine: CommitResources failed: ";
    msg.append(commit.error().message.View());
    pyxis::Logging::Get().Error(pyxis::log::APP, msg);
    impl.commandList->close();
    impl.profiler->EndFrame();
    return;
  }
  pyxis::RenderTargets targets{};
  // Export the ACES-TONEMAPPED `color` in LINEAR display space (range [0,1], NO
  // OETF). This is the Hydra color-AOV convention: the consumer applies the sRGB
  // OETF. EMPIRICALLY VERIFIED — the Kit pxr viewport's display transfer function
  // is exactly sRGB (HdxColorCorrectionTask), so HdPyxisRenderDelegate writes this
  // linear buffer to the color AOV verbatim and the viewport encodes it (matching
  // the standalone pyxis.exe, which sRGB-encodes the same linear buffer in its PNG
  // writer; the WorldLobbyHeadless harness likewise sRGB-encodes in WriteBmp).
  // PYXIS_OMNI_EXPORT_LINEAR_HDR=1 exports raw (untonemapped) radiance instead, for
  // a host that does its own exposure/tonemap.
  if (std::getenv("PYXIS_OMNI_EXPORT_LINEAR_HDR") != nullptr) {
    targets.colorHdr = impl.exportedColor.texture;  // raw HDR, exported
    targets.color = impl.displayColor;              // tonemapped (throwaway)
  } else {
    targets.color = impl.exportedColor.texture;  // tonemapped, exported (host presents verbatim)
    targets.colorHdr = impl.displayColor;        // raw HDR (throwaway)
  }
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
  // Targeted "render retired" marker so the GL-interop path (RFC 0008) can wait for
  // just this submission before GL reads the shared image (no full device flush).
  if (!impl.renderQuery)
    impl.renderQuery = device->createEventQuery();
  device->resetEventQuery(impl.renderQuery);
  device->setEventQuery(impl.renderQuery, nvrhi::CommandQueue::Graphics);
  impl.profiler->EndFrame();  // recycles this frame's timer-query slot

  // Publish "frame N ready" so the Kit side waits before sampling the image.
  ++impl.frameValue;
  impl.exporter->SignalTimeline(impl.timeline, impl.frameValue);
}

void PyxisEngine::Resize(uint32_t width, uint32_t height) noexcept {
  Impl& impl = *_impl;
  if (!impl.valid || width == 0 || height == 0)
    return;
  if (width == impl.width && height == impl.height)
    return;
  nvrhi::IDevice* device = impl.deviceManager->GetDevice();
  device->waitForIdle();
  device->runGarbageCollection();

  impl.width = width;
  impl.height = height;

  // Recreate the exportable color image (the texture the path tracer writes +
  // the host samples) and the throwaway display target at the new size. RELEASE
  // the previous exportable image first — the exporter owns its VkImage +
  // VkDeviceMemory + Win32 HANDLE and only frees them on ReleaseImage/teardown,
  // so without this every resize leaks an image+memory+handle for the life of the
  // (persisted) engine.
  if (impl.exportedColor.texture != nullptr)
    impl.exporter->ReleaseImage(impl.exportedColor);
  impl.exportedColor =
      impl.exporter->CreateExportableImage(width, height, EXPORT_COLOR_VK_FORMAT, true);
  // Invalidate the double-buffered readback slots: a frame N-1 captured at the old
  // size must not be presented at the new size. The helper re-creates each slot's
  // staging automatically on the size change; this just forces the first post-resize
  // read to be synchronous (read `cur`) instead of the stale prev slot.
  impl.readbackSlotValid[0] = false;
  impl.readbackSlotValid[1] = false;

  nvrhi::TextureDesc displayDesc;
  displayDesc.width = width;
  displayDesc.height = height;
  displayDesc.format = nvrhi::Format::RGBA16_FLOAT;
  displayDesc.dimension = nvrhi::TextureDimension::Texture2D;
  displayDesc.isRenderTarget = true;
  displayDesc.isUAV = true;
  displayDesc.isShaderResource = true;
  displayDesc.debugName = "pyxis.hydra.displayColor";
  displayDesc.initialState = nvrhi::ResourceStates::RenderTarget;
  displayDesc.keepInitialState = true;
  impl.displayColor = device->createTexture(displayDesc);

  impl.renderer->Resize(width, height);
}

uint32_t PyxisEngine::Width() const noexcept { return _impl->width; }
uint32_t PyxisEngine::Height() const noexcept { return _impl->height; }

bool PyxisEngine::ReadbackColorHdr(std::vector<uint8_t>& outRgba16f, uint32_t& outWidth,
                                   uint32_t& outHeight) noexcept {
  Impl& impl = *_impl;
  if (!impl.valid || impl.exportedColor.texture == nullptr)
    return false;
  nvrhi::IDevice* device = impl.deviceManager->GetDevice();

  const int cur = impl.readbackCur;
  const int prev = cur ^ 1;

  // 1. Record frame N's copy into slot `cur` (staging reused across frames).
  impl.commandList->open();
  impl.commandList->setTextureState(impl.exportedColor.texture, nvrhi::AllSubresources,
                                    nvrhi::ResourceStates::CopySource);
  impl.commandList->commitBarriers();
  if (!impl.readback[cur].RecordCopy(device, impl.commandList, impl.exportedColor.texture,
                                     "pyxis.hydra.readback")) {
    impl.commandList->close();
    return false;
  }
  impl.commandList->close();
  device->executeCommandList(impl.commandList);
  if (!impl.readbackQuery[cur])
    impl.readbackQuery[cur] = device->createEventQuery();
  device->resetEventQuery(impl.readbackQuery[cur]);
  device->setEventQuery(impl.readbackQuery[cur], nvrhi::CommandQueue::Graphics);
  impl.readbackSlotValid[cur] = true;
  impl.readbackSlotW[cur] = impl.width;
  impl.readbackSlotH[cur] = impl.height;

  // 2. Pick the slot to PRESENT: frame N-1 (prev) if it's valid and same-size — its
  // copy was submitted last frame and is normally already retired, so waiting on it
  // doesn't stall (the pipelining win). On the first frame / right after a resize,
  // prev is invalid → read `cur` synchronously (one stall, then pipelined again).
  const bool usePrev = impl.readbackSlotValid[prev] && impl.readbackSlotW[prev] == impl.width
                       && impl.readbackSlotH[prev] == impl.height;
  const int readSlot = usePrev ? prev : cur;

  device->waitEventQuery(impl.readbackQuery[readSlot]);
  if (!impl.readback[readSlot].Map())
    return false;
  outWidth = impl.readbackSlotW[readSlot];
  outHeight = impl.readbackSlotH[readSlot];
  impl.readback[readSlot].CopyTightlyInto(outRgba16f);  // strips row pitch; reuses capacity.
  impl.readback[readSlot].Unmap();

  impl.readbackCur = prev;  // ping-pong for next frame.
  return true;
}

const pyxis::ExportedImage& PyxisEngine::ExportedColor() const noexcept {
  return _impl->exportedColor;
}

void PyxisEngine::WaitRenderComplete() noexcept {
  const Impl& impl = *_impl;
  if (!impl.valid || !impl.renderQuery)
    return;
  // Block until the most recent RenderFrame submission has retired, so a GL importer
  // (RFC 0008) reads finished pixels from the shared image. Targeted (this submit),
  // not a full device flush.
  impl.deviceManager->GetDevice()->waitEventQuery(impl.renderQuery);
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

}  // namespace pyxis::hydra

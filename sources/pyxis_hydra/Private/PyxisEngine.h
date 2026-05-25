// Pyxis Hydra delegate — render engine.
//
// USD-free render driver owned by HdPyxisRenderDelegate. It owns a Pyxis Vulkan
// device (separate from any host's, per §32), a GpuScene, a PyxisRenderer and a
// GpuInteropExporter, and renders frames into an exportable color image a host
// (Omniverse Kit) can import with zero host copy. The HdRenderPass drives it.
//
// This is the single delegate engine shared by both the standalone/usdview
// plugin (pyxis_hydra.dll) and the Omniverse Kit extension (RFC 0006 collapsed
// the former pyxis_hydra_omni fork onto these sources). PIMPL so this header
// stays free of both nvrhi and USD types.

#pragma once

#include <Pyxis/Platform/Interop/GpuInteropExporter.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pyxis {
class GpuScene;
class Profiler;
}  // namespace pyxis

namespace pyxis::hydra {

class PyxisEngine {
 public:
  PyxisEngine();
  ~PyxisEngine();
  PyxisEngine(const PyxisEngine&) = delete;
  PyxisEngine& operator=(const PyxisEngine&) = delete;

  // Stand up the Pyxis device + renderer + exporter and allocate the exportable
  // color target at the given size. Returns false (logged) if anything fails —
  // e.g. no Vulkan device, or external-memory interop unsupported.
  [[nodiscard]] bool Initialize(uint32_t width, uint32_t height) noexcept;

  // Render one frame into the exportable color image and signal the timeline
  // semaphore so the host can wait before sampling. No-op if !IsValid().
  void RenderFrame() noexcept;

  // Resize the render targets + exportable image to (width, height). Cheap no-op
  // if already that size. Called when the host viewport's AOV buffer dimensions
  // differ from the current render size (so the delegate renders at the viewport
  // resolution + aspect rather than a fixed default that gets cropped).
  void Resize(uint32_t width, uint32_t height) noexcept;

  // Block until the GPU retires all submitted work. Required before mutating the
  // scene in a way that drops live GPU resources (e.g. GpuScene::Clear on a
  // stage change while the engine is persisted), per the GpuScene::Clear
  // contract. No-op if !IsValid().
  void WaitIdle() noexcept;

  [[nodiscard]] uint32_t Width() const noexcept;
  [[nodiscard]] uint32_t Height() const noexcept;

  // The exportable color image (Win32 handle + timeline) the host imports. Valid
  // after a successful Initialize().
  [[nodiscard]] const pyxis::ExportedImage& ExportedColor() const noexcept;
  [[nodiscard]] const pyxis::ExportedSemaphore& Timeline() const noexcept;
  [[nodiscard]] uint64_t LastSignaledValue() const noexcept;

  [[nodiscard]] bool IsValid() const noexcept;

  // Read the rendered exportable color image back to the host (RGBA16F bytes).
  // For headless verification (the Kit path samples it on the GPU instead).
  // Returns false if !IsValid(). `outRgba16f` is width*height*8 bytes, tightly
  // packed (4 halfs/pixel).
  [[nodiscard]] bool ReadbackColorHdr(std::vector<uint8_t>& outRgba16f, uint32_t& outWidth,
                                      uint32_t& outHeight) noexcept;

  // Borrowed accessors so the delegate can hand the shared scene/profiler to
  // prim Sync impls (via the render param). Valid after Initialize().
  [[nodiscard]] pyxis::GpuScene* Scene() const noexcept;
  [[nodiscard]] pyxis::Profiler* ProfilerPtr() const noexcept;

  // Last committed scene counts (from GpuScene::LastFrameStats) — used to verify
  // the prim adapters fed geometry through to Pyxis.
  [[nodiscard]] uint64_t LastInstanceCount() const noexcept;
  [[nodiscard]] uint64_t LastMeshCount() const noexcept;
  [[nodiscard]] uint64_t LastMaterialCount() const noexcept;
  [[nodiscard]] uint64_t LastLightCount() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace pyxis::hydra

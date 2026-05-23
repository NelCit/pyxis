// Pyxis Omniverse Hydra delegate — render engine. RFC 0004 Stage 3 (C4).
//
// USD-free render driver living inside the Kit extension. It owns a Pyxis
// Vulkan device (separate from Kit's, per §32), a GpuScene, a PyxisRenderer and
// a GpuInteropExporter, and renders frames into an exportable color image that
// Kit imports (zero host copy). This is exactly the sequence proven by
// tests/unit/PyxisRenderToExportedImage.cpp; the HdRenderPass drives it.
//
// PIMPL so this header stays free of both nvrhi and USD types (the RenderPass
// TU includes USD; this engine TU includes nvrhi + the Pyxis public API).

#pragma once

#include <Pyxis/Platform/Interop/GpuInteropExporter.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pyxis {
class GpuScene;
class Profiler;
}  // namespace pyxis

namespace pyxis_omni {

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
  // semaphore so the Kit side can wait before sampling. No-op if !IsValid().
  void RenderFrame() noexcept;

  // The exportable color image (Win32 handle + timeline) Kit imports. Valid
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
  // the FSD prim adapters fed geometry through to Pyxis.
  [[nodiscard]] uint64_t LastInstanceCount() const noexcept;
  [[nodiscard]] uint64_t LastMeshCount() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace pyxis_omni

// Pyxis renderer — frame rendering API.
//
// Plan §18.6. The renderer takes a `GpuScene&` as the canonical scene
// input. M3 wires the §9 v1 graph's first real pass (PathTracePass);
// later milestones extend it to PathTrace → Accumulation → ToneMap →
// AovResolve → DebugView → CopyToHydraBuffer → Present.

#pragma once

#include <Pyxis/Renderer/Descs/FrameProfile.h>
#include <Pyxis/Renderer/Descs/PickResult.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>
#include <memory>

namespace nvrhi {
class IDevice;
class ICommandList;
}  // namespace nvrhi

namespace pyxis {

// RenderGraph lives in Private/. Forward-declared here so PyxisRenderer
// can hold one by unique_ptr without exposing the definition publicly —
// the dtor is defined in PyxisRenderer.cpp where the type is complete.
class RenderGraph;

class PYXIS_RENDERER_API PyxisRenderer final {
 public:
  PyxisRenderer(nvrhi::IDevice* device, GpuScene& scene, Profiler& profiler,
                const RendererCreateDesc& desc);
  ~PyxisRenderer();

  PyxisRenderer(const PyxisRenderer&) = delete;
  PyxisRenderer& operator=(const PyxisRenderer&) = delete;

  // Renders one frame. Caller's responsibility: every render-target in
  // `targets` is in nvrhi::ResourceStates::RenderTarget when the call
  // returns (so the device manager's EndFrame can blit / present).
  // Threading: render thread only.
  void RenderFrame(nvrhi::ICommandList* commandList, const RenderSettings& settings,
                   const RenderTargets& targets);

  // Resize internal AOV / accumulation buffers. M3 path-trace writes
  // into a caller-allocated AOV color texture (no internal buffers
  // yet), so this is still a no-op; M5+ accumulation wires here.
  void Resize(uint32_t width, uint32_t height);

  // Resets accumulation. No-op until the M5+ accumulation buffer
  // exists — the M3 path-tracer renders one sample per frame straight
  // to the AOV color texture.
  void ResetAccumulation();

  [[nodiscard]] FrameProfile LastFrameProfile() const;

  // M7 follow-up — last successful pixel-pick readback. The renderer
  // copies the GPU's pick UAV into a staging buffer at the end of
  // each RenderFrame and maps it on the NEXT call, so the result
  // returned here lags the cursor by one frame (acceptable for a
  // hover readout). Caller must have supplied RenderTargets::pickResult
  // for the picker to have run; otherwise this returns the default-
  // constructed PickResult (depth=-1, instanceId=~0u).
  [[nodiscard]] PickResult LastPickResult() const noexcept;

  // Re-load every render-pass's shaders from disk and rebuild the
  // pipeline-state objects. The Slang compiler isn't linked into the
  // runtime — the .spv files this picks up are produced by ShaderMake
  // at build time, so the click-effect is "rebuild shaders externally
  // (cmake --build --target pyxis_renderer_shaders), then click
  // Reload to pick them up". Returns true iff every pass succeeded.
  // Caller must ensure no in-flight command buffer references the
  // current pipeline (typical use: device->waitForIdle() before).
  // Render thread only — no thread-safety on the underlying RenderGraph.
  [[nodiscard]] bool ReloadShaders() noexcept;

 private:
  Profiler* _profiler = nullptr;
  std::unique_ptr<RenderGraph> _graph;
  // Borrowed pointer to the PathTracePass the graph owns — kept here
  // so LastPickResult() can forward without a dynamic_cast or a graph-
  // walk. Set in the ctor; cleared in the dtor (the RenderGraph
  // unique_ptr drops its passes first by member-order).
  // Forward-declared as IRenderPass* so the public header doesn't
  // pull in the private PathTracePass header — the impl casts on
  // demand.
  class IRenderPass* _pathTracePass = nullptr;
  uint64_t _frameIndex = 0;
};

}  // namespace pyxis

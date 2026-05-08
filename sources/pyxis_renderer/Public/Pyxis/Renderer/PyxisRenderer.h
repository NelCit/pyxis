// Pyxis renderer — frame rendering API.
//
// Plan §18.6. The renderer takes a `GpuScene&` as the canonical scene
// input; the M1 hard-coded triangle path is preserved for now and
// continues to ignore the scene argument until M3's PathTracePass
// replaces TrianglePass with the real RT-pipeline dispatch. The
// signature change here lands the §18.6 final shape so consumers
// (pyxis_app, future pyxis_hydra / pyxis_usd_ingest) compile against
// it from M3 onward without further churn.

#pragma once

#include <Pyxis/Renderer/Descs/FrameProfile.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
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
    PyxisRenderer(nvrhi::IDevice*            device,
                  GpuScene&                  scene,
                  Profiler&                  profiler,
                  const RendererCreateDesc&  desc);
    ~PyxisRenderer();

    PyxisRenderer(const PyxisRenderer&)            = delete;
    PyxisRenderer& operator=(const PyxisRenderer&) = delete;

    // Renders one frame. Caller's responsibility: every render-target in
    // `targets` is in nvrhi::ResourceStates::RenderTarget when the call
    // returns (so the device manager's EndFrame can blit / present).
    // Threading: render thread only.
    void RenderFrame(nvrhi::ICommandList*  commandList,
                     const RenderSettings& settings,
                     const RenderTargets&  targets);

    // Resize internal AOV / accumulation buffers. M1 has no internal
    // off-screen buffers (the triangle draws straight into the
    // backbuffer), so this is a no-op for now. M3+ wires the
    // accumulation buffer here.
    void Resize(uint32_t width, uint32_t height);

    // Resets accumulation. M1 no-op; the M3+ path-tracer hooks here.
    void ResetAccumulation();

    [[nodiscard]] FrameProfile LastFrameProfile() const;

private:
    Profiler*                    _profiler   = nullptr;
    std::unique_ptr<RenderGraph> _graph;
    uint64_t                     _frameIndex = 0;
};

}  // namespace pyxis

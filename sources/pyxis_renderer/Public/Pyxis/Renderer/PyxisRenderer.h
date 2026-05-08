// Pyxis renderer — frame rendering API.
//
// Plan §18.6 (M1 subset). M1 surface: ctor takes (device, profiler,
// create-desc); RenderFrame populates a command list with a clear +
// triangle draw into RenderTargets::color; Resize / ResetAccumulation
// / LastFrameProfile follow §18.6 verbatim. GpuScene is M3+; M1's
// renderer renders the hard-coded triangle without consuming a scene.

#pragma once

#include <Pyxis/Renderer/Descs/FrameProfile.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>

namespace nvrhi {
class IDevice;
class ICommandList;
}  // namespace nvrhi

namespace pyxis {

class PYXIS_RENDERER_API PyxisRenderer final {
public:
    PyxisRenderer(nvrhi::IDevice*            device,
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
    struct Impl;
    Impl* _impl = nullptr;
};

}  // namespace pyxis

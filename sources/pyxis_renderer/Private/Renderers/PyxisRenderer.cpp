// Pyxis renderer — PyxisRenderer implementation (M1).
//
// Plan §18.6. Owns a RenderGraph + a Profiler reference. M1 adds one
// pass (TrianglePass); M3+ adds the path-trace + accumulation + tone-
// map + AOV-resolve chain.

#include <Pyxis/Renderer/PyxisRenderer.h>

#include "Passes/TrianglePass.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/RenderGraph.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include <nvrhi/nvrhi.h>

namespace pyxis {

PyxisRenderer::PyxisRenderer(nvrhi::IDevice*           device,
                             GpuScene&                 scene,
                             Profiler&                 profiler,
                             const RendererCreateDesc& /*desc*/)
    : _profiler(&profiler),
      _graph(std::make_unique<RenderGraph>(device, &profiler)) {
    // `scene` parameter is held by the renderer once PathTracePass
    // arrives at M3; for now the M1 TrianglePass ignores it and we
    // suppress the unused-parameter diagnostic with a void-cast so
    // the public §18.6 signature is fully populated from this point
    // forward.
    (void)scene;
    _graph->AddPass(std::make_unique<TrianglePass>(device));
    Logging::Get().Info(log::RENDER, "PyxisRenderer: initialised (TrianglePass registered)");
}

// Out-of-line dtor lives here so unique_ptr<RenderGraph>'s deleter sees
// the complete RenderGraph type (the public header only forward-declares
// it). Same reason `=default` works here but wouldn't in the header.
PyxisRenderer::~PyxisRenderer() = default;

void PyxisRenderer::RenderFrame(nvrhi::ICommandList*  commandList,
                                const RenderSettings& settings,
                                const RenderTargets&  targets) {
    if (!_graph || !commandList) return;
    PassContext context{};
    context.commandList    = commandList;
    context.profiler       = _profiler;
    context.settings       = &settings;
    context.targets        = &targets;
    context.frameIndex     = _frameIndex++;
    // M1 active runtime: 1 frame in flight (cap = MAX_FRAMES_IN_FLIGHT = 3).
    context.framesInFlight = 1;

    const Profiler::CpuScope frameScope(*_profiler, "render.frame.cpu");
    _graph->Execute(commandList, context);
}

void PyxisRenderer::Resize(uint32_t /*width*/, uint32_t /*height*/) {
    // TODO(M3): forward to RenderGraph for pass-local target resize +
    // accumulation reset. M1's TrianglePass keys its framebuffer cache
    // on nvrhi::ITexture* identity, so a swapchain rebuild already
    // invalidates the cached entries naturally.
}

void PyxisRenderer::ResetAccumulation() {
    // TODO(M3): clear PathTracePass's accumulation buffer so the next
    // frame starts from sample 0. No-op until the buffer exists.
}

FrameProfile PyxisRenderer::LastFrameProfile() const {
    return _profiler->LastFrameProfile();
}

}  // namespace pyxis

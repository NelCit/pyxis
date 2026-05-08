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
#include <Pyxis/Renderer/Profiler.h>

#include <nvrhi/nvrhi.h>

#include <memory>

namespace pyxis {

struct PyxisRenderer::Impl {
    Profiler*                    profiler = nullptr;
    std::unique_ptr<RenderGraph> graph;
    uint64_t                     frameIndex = 0;
};

PyxisRenderer::PyxisRenderer(nvrhi::IDevice*           device,
                             Profiler&                 profiler,
                             const RendererCreateDesc& /*desc*/)
    : _impl(new Impl()) {
    _impl->profiler = &profiler;
    _impl->graph    = std::make_unique<RenderGraph>(device, &profiler);
    _impl->graph->AddPass(std::make_unique<TrianglePass>(device));
    Logging::Get().Info(log::RENDER, "PyxisRenderer: initialised (TrianglePass registered)");
}

PyxisRenderer::~PyxisRenderer() { delete _impl; }

void PyxisRenderer::RenderFrame(nvrhi::ICommandList*  commandList,
                                const RenderSettings& settings,
                                const RenderTargets&  targets) {
    if (!_impl || !_impl->graph || !commandList) return;
    PassContext context{};
    context.commandList    = commandList;
    context.profiler       = _impl->profiler;
    context.settings       = &settings;
    context.targets        = &targets;
    context.frameIndex     = _impl->frameIndex++;
    // M1 active runtime: 1 frame in flight (cap = MAX_FRAMES_IN_FLIGHT = 3).
    context.framesInFlight = 1;

    const Profiler::CpuScope frameScope(*_impl->profiler, "render.frame.cpu");
    _impl->graph->Execute(commandList, context);
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

FrameProfile PyxisRenderer::LastFrameProfile() const               { return _impl->profiler->LastFrameProfile(); }

}  // namespace pyxis

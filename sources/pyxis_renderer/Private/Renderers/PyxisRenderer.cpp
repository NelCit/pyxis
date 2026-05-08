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

void PyxisRenderer::RenderFrame(nvrhi::ICommandList*  cl,
                                const RenderSettings& settings,
                                const RenderTargets&  targets) {
    if (!_impl || !_impl->graph || !cl) return;
    PassContext ctx{};
    ctx.commandList    = cl;
    ctx.profiler       = _impl->profiler;
    ctx.settings       = &settings;
    ctx.targets        = &targets;
    ctx.frameIndex     = _impl->frameIndex++;
    ctx.framesInFlight = 2;

    const Profiler::CpuScope frameScope(*_impl->profiler, "render.frame.cpu");
    _impl->graph->Execute(cl, ctx);
}

void PyxisRenderer::Resize(uint32_t /*w*/, uint32_t /*h*/)        { /* M1 no-op */ }
void PyxisRenderer::ResetAccumulation()                            { /* M1 no-op */ }
FrameProfile PyxisRenderer::LastFrameProfile() const               { return _impl->profiler->LastFrameProfile(); }

}  // namespace pyxis

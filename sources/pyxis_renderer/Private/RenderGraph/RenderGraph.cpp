// Pyxis renderer — RenderGraph (M1 minimal).

#include "RenderGraph/RenderGraph.h"

#include "Diagnostics/CommandListMarker.h"
#include "RenderGraph/PassContext.h"

#include <Pyxis/Renderer/Profiler.h>

namespace pyxis {

RenderGraph::RenderGraph(nvrhi::IDevice* device, Profiler* profiler) noexcept
    : _device(device), _profiler(profiler) {}

RenderGraph::~RenderGraph() = default;

void RenderGraph::AddPass(std::unique_ptr<IRenderPass> pass) {
    if (pass) _passes.push_back(std::move(pass));
}

void RenderGraph::Execute(nvrhi::ICommandList* commandList, const PassContext& ctx) {
    for (auto& pass : _passes) {
        const std::string_view name = pass->Name();
        const CommandListMarker mark(commandList, name);
        if (_profiler) {
            const Profiler::GpuScope gpu(*_profiler, commandList, name);
            pass->Execute(commandList, ctx);
        } else {
            pass->Execute(commandList, ctx);
        }
    }
}

}  // namespace pyxis

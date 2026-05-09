// Pyxis renderer — RenderGraph (M1 minimal).

#include "RenderGraph/RenderGraph.h"

#include "RenderGraph/PassContext.h"

#include <Pyxis/Renderer/Profiler.h>

namespace pyxis {

RenderGraph::RenderGraph(nvrhi::IDevice* device, Profiler* profiler) noexcept
    : _device(device), _profiler(profiler) {}

RenderGraph::~RenderGraph() = default;

void RenderGraph::AddPass(std::unique_ptr<IRenderPass> pass) {
  if (pass)
    _passes.push_back(std::move(pass));
}

bool RenderGraph::ReloadShaders() noexcept {
  bool allOk = true;
  for (auto& pass : _passes)
  {
    if (!pass->ReloadShaders())
      allOk = false;
  }
  return allOk;
}

void RenderGraph::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  // Profiler::GpuScope already brackets the command list with NVRHI's
  // beginMarker/endMarker (Profiler.cpp), so RenderDoc / Aftermath get
  // exactly one named region per pass — no need to wrap a second
  // CommandListMarker on top.
  for (auto& pass : _passes)
  {
    const std::string_view name = pass->Name();
    if (_profiler)
    {
      const Profiler::GpuScope gpuScope(*_profiler, commandList, name);
      pass->Execute(commandList, context);
    }
    else
    {
      pass->Execute(commandList, context);
    }
  }
}

}  // namespace pyxis

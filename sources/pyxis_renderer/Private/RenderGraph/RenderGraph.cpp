// Pyxis renderer — RenderGraph (M1 minimal).

#include "RenderGraph/RenderGraph.h"

#include "RenderGraph/PassContext.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
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
  if (!allOk)
  {
    // P6 review — the per-pass reload is old-on-failure but NOT
    // transactional across passes: a partial reload can leave the two RT
    // pipelines (RaytracedGBuffer + RaytracedLighting) built from
    // DIFFERENT generations of the shared camera_ray / shading modules,
    // breaking the "both raygens compute bit-identical primary rays"
    // contract the visibility-buffer split depends on (the lighting pass
    // reconstructs worldHit from a hitT the GBuffer pipeline traced).
    Logging::Get().Error(
        log::RENDER,
        "RenderGraph::ReloadShaders: partial reload — at least one pass kept its old "
        "pipeline while others may have swapped. The two RT pipelines can now be built "
        "from different generations of the shared camera_ray/shading modules (mismatched "
        "primary-ray math). Fix the failing shader and reload again.");
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

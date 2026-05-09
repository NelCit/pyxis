// Pyxis renderer — PassContext.
//
// Plan §9.2. The bag of per-frame state every pass needs in
// Execute(). Passes never get a *back-pointer* to the RenderGraph or
// the GpuScene through here — those are wired at construction time
// (PathTracePass takes `GpuScene&` in its ctor). The fields below
// are the minimum every pass shares: command list, profiler scope
// target, settings, AOV bindings, monotonic frame index, active
// frames-in-flight depth (for ring sizing inside passes).

#pragma once

#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include <cstdint>

namespace nvrhi {
class ICommandList;
}

namespace pyxis {

class Profiler;

struct PassContext {
  nvrhi::ICommandList* commandList = nullptr;
  Profiler* profiler = nullptr;
  const RenderSettings* settings = nullptr;
  const RenderTargets* targets = nullptr;
  uint64_t frameIndex = 0;
  // Default 0 to flush out anyone who forgot to wire it through —
  // PyxisRenderer::RenderFrame always sets the real value. A pass
  // that depends on this should assert framesInFlight > 0 in its
  // Execute().
  uint32_t framesInFlight = 0;
};

}  // namespace pyxis

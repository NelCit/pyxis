// Pyxis renderer — IRenderPass.
//
// Plan §9.1. The minimal pass interface: Name() returns the dotted-
// lower-case profiler key, Execute() is allocation-free per §30.10
// (preallocate in the pass ctor / on Resize, never inside the
// per-frame body). Passes load their own SPIR-V from
// <bin>/Resources/shaders/ in their ctor; a shared ShaderLibrary
// lands at M5+ when there's more than one pass that wants the same
// shader. The §9.2 RenderGraph::Builder + Declare() phase (explicit
// resource registry + automatic barriers) is also a M5+ addition,
// when there's more than one pass to coordinate.

#pragma once

#include <string_view>

namespace nvrhi {
class ICommandList;
}

namespace pyxis {

struct PassContext;

class IRenderPass {
 public:
  virtual ~IRenderPass() = default;
  [[nodiscard]] virtual std::string_view Name() const = 0;
  virtual void Execute(nvrhi::ICommandList* commandList, const PassContext& context) = 0;

  // Editor-driven shader reload (M7 follow-up). Default no-op; passes
  // that load shaders from disk override to re-read .spv + rebuild
  // their pipeline. Caller must have waited the device idle before
  // calling so no in-flight command buffer references the old
  // pipeline / shader table. Returns true on success; false on any
  // step (file read, createShader, createRayTracingPipeline) so the
  // editor can surface a one-shot log line.
  [[nodiscard]] virtual bool ReloadShaders() noexcept { return true; }
};

}  // namespace pyxis

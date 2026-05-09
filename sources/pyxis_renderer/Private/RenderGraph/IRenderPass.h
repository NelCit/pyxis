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
};

}  // namespace pyxis

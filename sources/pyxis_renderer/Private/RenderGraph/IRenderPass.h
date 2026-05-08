// Pyxis renderer — IRenderPass.
//
// Plan §9.1. M1 ships the minimum: ctor takes IDevice* + ShaderLibrary
// (deferred to M3 — TrianglePass loads its own SPIR-V at M1), Name()
// returns the dotted-lower-case profiler key, Execute() is allocation-
// free per §30.10. The §9.2 RenderGraph::Builder + Declare() phase
// (resource registry + automatic barriers) lands at M3 when there's
// more than one pass to coordinate.

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
    virtual void Execute(nvrhi::ICommandList* cl, const PassContext& ctx) = 0;
};

}  // namespace pyxis

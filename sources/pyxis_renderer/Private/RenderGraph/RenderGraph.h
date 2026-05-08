// Pyxis renderer — RenderGraph (M1 minimal).
//
// Plan §9.2. M1 ships only the AddPass + Execute loop with per-pass
// CommandListMarker + Profiler::GpuScope wrapping. The §9.2
// Compile / Builder / barrier-resolution / resource-registry layers
// land at M3+ when there's more than one pass and barrier coordination
// actually matters.

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <memory>
#include <string_view>
#include <vector>

namespace nvrhi {
class ICommandList;
class IDevice;
}

namespace pyxis {

class Profiler;
struct PassContext;

class RenderGraph final {
public:
    RenderGraph(nvrhi::IDevice* device, Profiler* profiler) noexcept;
    ~RenderGraph();

    RenderGraph(const RenderGraph&)            = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Construction (called from PyxisRenderer::Init).
    void AddPass(std::unique_ptr<IRenderPass> pass);

    // Per-frame execute (called from PyxisRenderer::RenderFrame).
    void Execute(nvrhi::ICommandList* cl, const PassContext& ctx);

private:
    // _device is used by M3+'s RenderGraph::Compile to allocate barriers /
    // imported resources; M1's minimal Execute doesn't read it.
    [[maybe_unused]] nvrhi::IDevice*           _device   = nullptr;
    Profiler*                                  _profiler = nullptr;
    std::vector<std::unique_ptr<IRenderPass>>  _passes;
};

}  // namespace pyxis

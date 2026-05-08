// Pyxis renderer — PassContext.
// Plan §9.2 (M1 subset). The full PassContext (graph*, scene*,
// settings*, frameIndex, framesInFlight, renderResolution) lands at
// M3+; M1's TrianglePass needs only the command list + the active
// RenderTargets, so PassContext stays narrow.

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
    nvrhi::ICommandList*  commandList    = nullptr;
    Profiler*             profiler       = nullptr;
    const RenderSettings* settings       = nullptr;
    const RenderTargets*  targets        = nullptr;
    uint64_t              frameIndex     = 0;
    uint32_t              framesInFlight = 2;
};

}  // namespace pyxis

// Pyxis renderer — PyxisRenderer implementation.
//
// Plan §18.6. Owns a RenderGraph + a Profiler reference. M3 wires
// PathTracePass as the only pass; the §9 v1 graph (Accumulation →
// ToneMap → AovResolve → DebugView → CopyToHydraBuffer → Present)
// fills in at M5+.

#include "Passes/PathTracePass.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/RenderGraph.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>

#include <nvrhi/nvrhi.h>

namespace pyxis {

PyxisRenderer::PyxisRenderer(nvrhi::IDevice* device, GpuScene& scene, Profiler& profiler,
                             const RendererCreateDesc& /*desc*/)
    : _profiler(&profiler), _graph(std::make_unique<RenderGraph>(device, &profiler)) {
  // PathTracePass runs only when the supplied scene has a TLAS +
  // camera; before that (e.g. an empty scene), the pass early-outs
  // and the output buffer is left untouched.
  auto pathTrace = std::make_unique<PathTracePass>(device, scene);
  _pathTracePass = pathTrace.get();
  _graph->AddPass(std::move(pathTrace));
  Logging::Get().Info(log::RENDER, "PyxisRenderer: initialised (PathTracePass registered)");
}

// Out-of-line dtor lives here so unique_ptr<RenderGraph>'s deleter sees
// the complete RenderGraph type (the public header only forward-declares
// it). Same reason `=default` works here but wouldn't in the header.
PyxisRenderer::~PyxisRenderer() = default;

void PyxisRenderer::RenderFrame(nvrhi::ICommandList* commandList, const RenderSettings& settings,
                                const RenderTargets& targets) {
  if (!_graph || !commandList)
    return;
  PassContext context{};
  context.commandList = commandList;
  context.profiler = _profiler;
  context.settings = &settings;
  context.targets = &targets;
  context.frameIndex = _frameIndex++;
  // Active runtime: 1 frame in flight (cap = MAX_FRAMES_IN_FLIGHT = 3).
  // Headless raises this to 3 for §33.7 byte-equal EXR; the viewer
  // stays at 1 until the pacing knobs land at M11.
  context.framesInFlight = 1;

  const Profiler::CpuScope frameScope(*_profiler, "render.frame.cpu");
  _graph->Execute(commandList, context);
}

void PyxisRenderer::Resize(uint32_t /*width*/, uint32_t /*height*/) {
  // No-op until M5+ adds an internal accumulation buffer that's
  // sized off the render resolution. PathTracePass writes into a
  // caller-allocated AOV color texture, so swapchain rebuilds
  // already invalidate cached pass state through the pass's own
  // texture-identity cache.
}

void PyxisRenderer::ResetAccumulation() {
  // No-op until M5+ adds an accumulation buffer to clear. The M3
  // path-tracer renders one sample per frame straight to the AOV.
}

FrameProfile PyxisRenderer::LastFrameProfile() const {
  return _profiler->LastFrameProfile();
}

bool PyxisRenderer::ReloadShaders() noexcept {
  if (!_graph)
    return false;
  return _graph->ReloadShaders();
}

PickResult PyxisRenderer::LastPickResult() const noexcept {
  if (_pathTracePass == nullptr)
    return {};
  // Static cast safe: we constructed _pathTracePass as a PathTracePass*
  // in the ctor and never reassign it. PathTracePass derives from
  // IRenderPass non-virtually so the static cast round-trips cleanly
  // (no RTTI involved — the renderer build forbids /GR via §30 anyway).
  return static_cast<const PathTracePass*>(_pathTracePass)->GetLastPickResult();
}

}  // namespace pyxis

// Pyxis renderer — PyxisRenderer implementation.
//
// Plan §18.6. Owns a RenderGraph + a Profiler reference. M3 wires
// PathTracePass as the only pass; the §9 v1 graph (Accumulation →
// ToneMap → AovResolve → DebugView → CopyToHydraBuffer → Present)
// fills in at M5+.

#include "Passes/BlitToSrgbPass.h"
#include "Passes/PathTracePass.h"
#include "Passes/SsaaResolvePass.h"
#include "Passes/TonemapPass.h"
#include "RenderGraph/IRenderPass.h"
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
                             const RendererCreateDesc& desc)
    : _profiler(&profiler),
      _graph(std::make_unique<RenderGraph>(device, &profiler)),
      _framesInFlight(desc.framesInFlight) {
  // PathTracePass runs only when the supplied scene has a TLAS +
  // camera; before that (e.g. an empty scene), the pass early-outs
  // and the output buffer is left untouched.
  auto pathTrace = std::make_unique<PathTracePass>(device, scene);
  _pathTracePass = pathTrace.get();
  _graph->AddPass(std::move(pathTrace));
  // Tonemap runs next (P3 pass split): the display transform (exposure 2^stops +
  // Narkowicz ACES on COLOR, the 10 debug-view encodes) extracted from raygen's
  // inline branch. Reads the fp32 linearColor scratch PathTracePass writes
  // (threaded via PassContext::linearColor) + the raw AOVs; writes the BGRA8
  // display target (targets.color). No-ops when either texture is unbound.
  _graph->AddPass(std::make_unique<TonemapPass>(device, scene));
  // SSAA resolve runs next: it box-downsamples the super-res LINEAR color AOV into
  // a base-res LINEAR intermediate (it owns the texture; RenderFrame threads it via
  // PassContext::colorLinearResolved). No-ops at ssaaFactor < 2.
  auto ssaa = std::make_unique<SsaaResolvePass>(device);
  _ssaaPass = ssaa.get();
  _graph->AddPass(std::move(ssaa));
  // BlitToSrgb is the final present-encode: linear -> sRGB OETF into colorResolved
  // (the present target). Reads the SSAA intermediate at factor > 1, else `color`
  // directly. No-ops when colorResolved is unbound (Kit never binds it; only the
  // standalone viewer does). Split from SsaaResolvePass so each pass has one job.
  _graph->AddPass(std::make_unique<BlitToSrgbPass>(device));
  Logging::Get().Info(log::RENDER,
                      "PyxisRenderer: initialised (PathTrace + Tonemap + SsaaResolve + "
                      "BlitToSrgb registered)");
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
  // Active FIF comes from RendererCreateDesc (default 1) — caller
  // typically passes IDeviceManager::GetFramesInFlight(). Headless
  // raises this to 3 for §33.7 byte-equal EXR; viewer stays at 1
  // until the pacing knobs land at M11. PathTracePass's picker
  // readback asserts == 1 since the mapBuffer-without-fence path
  // would race past that.
  context.framesInFlight = _framesInFlight;
  // P3 pass split — the full-render-res fp32 LINEAR radiance scratch the raygen
  // writes (binding 2) and TonemapPass reads. PathTracePass owns the texture
  // (EnsureLinearColor — created here on the CPU path before the graph runs,
  // NEVER inside a pass Execute; recreated on size change only). Sized off the
  // display target so the ray-dispatch extents are byte-identical to when the
  // raygen wrote targets.color directly. Stays null when no display target is
  // bound — PathTrace + Tonemap then no-op (nothing to display into), matching
  // the old targets.color early-out.
  if (_pathTracePass != nullptr && targets.color != nullptr) {
    const nvrhi::TextureDesc& colorDesc = targets.color->getDesc();
    context.linearColor = static_cast<PathTracePass*>(_pathTracePass)
                              ->EnsureLinearColor(colorDesc.width, colorDesc.height);
  }
  // At SSAA factor > 1, give SsaaResolvePass its base-res LINEAR downsample target
  // (it owns the texture) and thread it to BlitToSrgbPass via the context. At
  // factor 1 it stays null and BlitToSrgbPass reads `color` directly.
  if (settings.ssaaFactor > 1u && _ssaaPass != nullptr && targets.colorResolved != nullptr) {
    const uint32_t baseWidth = settings.width / settings.ssaaFactor;
    const uint32_t baseHeight = settings.height / settings.ssaaFactor;
    context.colorLinearResolved =
        static_cast<SsaaResolvePass*>(_ssaaPass)->EnsureLinearOutput(baseWidth, baseHeight);
  }

  const Profiler::CpuScope frameScope(*_profiler, "render.frame.cpu");
  // The graph runs PathTrace → SsaaResolve → BlitToSrgb. SsaaResolve no-ops at
  // factor < 2; BlitToSrgb no-ops when colorResolved is unbound (Kit's path), so
  // the headless / Omniverse paths are unaffected.
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

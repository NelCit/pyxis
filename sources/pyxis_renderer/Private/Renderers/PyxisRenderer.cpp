// Pyxis renderer — PyxisRenderer implementation.
//
// Plan §18.6. Owns a RenderGraph + a Profiler reference. P4 pass split
// (design D1): the graph runs RaytracedGBuffer → RaytracedLighting →
// Tonemap → SsaaResolve → BlitToSrgb, all linear (§9).

#include "Passes/BlitToSrgbPass.h"
#include "Passes/RaytracedGBufferPass.h"
#include "Passes/RaytracedLightingPass.h"
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
  // RaytracedGBufferPass runs first (P4 pass split): one primary ray per
  // pixel into the visibility buffer it owns (threaded via
  // PassContext::visibility). Runs only when the supplied scene has a
  // TLAS + camera; before that (e.g. an empty scene), the pass
  // early-outs — and so does the lighting pass, on the same gates, so
  // the stale visibility records are never consumed.
  auto gbuffer = std::make_unique<RaytracedGBufferPass>(device, scene);
  _gbufferPass = gbuffer.get();
  _graph->AddPass(std::move(gbuffer));
  // RaytracedLightingPass shades the first hit it reads from the
  // visibility buffer (all shadow / AO / reflection / transparency
  // continuation rays trace from there) and writes the fp32 linearColor
  // scratch + the raw AOVs + the pick buffer.
  auto lighting = std::make_unique<RaytracedLightingPass>(device, scene);
  _lightingPass = lighting.get();
  _graph->AddPass(std::move(lighting));
  // Tonemap runs next (P3 pass split): the display transform (exposure 2^stops +
  // Narkowicz ACES on COLOR, the 10 debug-view encodes) extracted from raygen's
  // inline branch. Reads the fp32 linearColor scratch RaytracedLightingPass
  // writes (threaded via PassContext::linearColor) + the raw AOVs; writes the
  // BGRA8 display target (targets.color). No-ops when either texture is unbound.
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
                      "PyxisRenderer: initialised (RaytracedGBuffer + RaytracedLighting + "
                      "Tonemap + SsaaResolve + BlitToSrgb registered)");
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
  // until the pacing knobs land at M11. RaytracedLightingPass's picker
  // readback asserts == 1 since the mapBuffer-without-fence path
  // would race past that.
  context.framesInFlight = _framesInFlight;
  // P3/P4 pass split — the renderer-internal scratch resources, both sized
  // off the display target so the two ray dispatches and the tonemap run at
  // identical extents:
  //   * visibility (P4): the per-pixel VisibilityGpu records
  //     RaytracedGBufferPass writes and RaytracedLightingPass reads. Owned by
  //     the GBuffer pass (EnsureVisibilityBuffer).
  //   * linearColor (P3): the fp32 LINEAR radiance the lighting raygen writes
  //     and TonemapPass reads. Owned by the lighting pass (EnsureLinearColor).
  // Both Ensure* calls run here on the CPU path before the graph executes —
  // NEVER inside a pass Execute; recreated on size change only. Both stay
  // null when no display target is bound — the RT passes + Tonemap then
  // no-op (nothing to display into), matching the old targets.color early-out.
  if (targets.color != nullptr) {
    const nvrhi::TextureDesc& colorDesc = targets.color->getDesc();
    if (_gbufferPass != nullptr) {
      context.visibility = static_cast<RaytracedGBufferPass*>(_gbufferPass)
                               ->EnsureVisibilityBuffer(colorDesc.width, colorDesc.height);
    }
    if (_lightingPass != nullptr) {
      context.linearColor = static_cast<RaytracedLightingPass*>(_lightingPass)
                                ->EnsureLinearColor(colorDesc.width, colorDesc.height);
    }
  }
  // P5 (design D2) — spec-constant pipeline variants: make sure the
  // ACTIVE camera projection mode's RT pipelines exist before the graph
  // walks. The perspective variant is built in each pass's ctor; the
  // orthographic one materializes here the first frame the camera
  // reports it. Creation happens on this CPU frame path only — never
  // inside a pass Execute (§30.10); both hooks no-op once built.
  if (_gbufferPass != nullptr) {
    static_cast<RaytracedGBufferPass*>(_gbufferPass)->EnsureProjectionPipeline();
  }
  if (_lightingPass != nullptr) {
    static_cast<RaytracedLightingPass*>(_lightingPass)->EnsureProjectionPipeline();
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
  // The graph runs RaytracedGBuffer → RaytracedLighting → Tonemap →
  // SsaaResolve → BlitToSrgb. SsaaResolve no-ops at factor < 2; BlitToSrgb
  // no-ops when colorResolved is unbound (Kit's path), so the headless /
  // Omniverse paths are unaffected.
  _graph->Execute(commandList, context);
}

void PyxisRenderer::Resize(uint32_t /*width*/, uint32_t /*height*/) {
  // No-op until M5+ adds an internal accumulation buffer that's
  // sized off the render resolution. The RT passes write into
  // caller-allocated / pass-owned resources, so swapchain rebuilds
  // already invalidate cached pass state through each pass's own
  // identity cache (and the Ensure* calls re-size the scratch).
}

void PyxisRenderer::ResetAccumulation() {
  // No-op until M5+ adds an accumulation buffer to clear. The
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
  if (_lightingPass == nullptr)
    return {};
  // Static cast safe: we constructed _lightingPass as a
  // RaytracedLightingPass* in the ctor and never reassign it.
  // RaytracedLightingPass derives from IRenderPass non-virtually so the
  // static cast round-trips cleanly (no RTTI involved — the renderer
  // build forbids /GR via §30 anyway).
  return static_cast<const RaytracedLightingPass*>(_lightingPass)->GetLastPickResult();
}

}  // namespace pyxis

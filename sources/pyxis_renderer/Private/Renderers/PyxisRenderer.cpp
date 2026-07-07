// Pyxis renderer — PyxisRenderer implementation.
//
// Plan §18.6. Owns a RenderGraph + a Profiler reference. P4 pass split
// (design D1): the graph runs RaytracedGBuffer → RaytracedLighting →
// Tonemap → SsaaResolve → BlitToSrgb, all linear (§9).

#include "Dlss/DlssProvider.h"
#include "Passes/AccumulationPass.h"
#include "Passes/AmbientOcclusionPass.h"
#include "Passes/AutoExposurePass.h"
#include "Passes/BlitToSrgbPass.h"
#include "Passes/CompositePass.h"
#include "Passes/DenoiseAoPass.h"
#include "Passes/DenoiseAtrousPass.h"
#include "Passes/DenoiseHistoryFixPass.h"
#include "Passes/DenoiseShadowPass.h"
#include "Passes/DenoiseTemporalPass.h"
#include "Passes/DirectLightingPass.h"
#include "Passes/DlssPass.h"
#include "Passes/IndirectDiffusePass.h"
#include "Passes/RaytracedGBufferPass.h"
#include "Passes/ReflectionsPass.h"
#include "Passes/SharcResolvePass.h"
#include "Passes/SceneBindings.h"
#include "Passes/SsaaResolvePass.h"
#include "Passes/TaaPass.h"
#include "Passes/TonemapPass.h"
#include "Passes/TranslucencyPass.h"
#include "RenderGraph/IRenderPass.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/RenderGraph.h"
#include "Scene/SceneResources.h"  // RFC 0003 — feeds SceneBindings::Update.

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>

// Dual-language interop header (renderer-private include path) — Q2
// pulls the OPENPBR_FEATURE_* bits for the lighting pass's
// feature-mask variant selection below.
#include "ShaderInterop.slang"

#include <nvrhi/nvrhi.h>

#include <cstring>
#include <string>
#include <string_view>

// Q3 (openpbr-complete-design.md "Control surface") — drift guard.
// RenderSettings::openPbrFeatureMask defaults to the literal 0x3F
// because the public header cannot include ShaderInterop.slang; this
// TU sees both headers, so pin the literal to the interop constant
// (and the POD's default to it) so neither can drift silently.
static_assert(pyxis::shaderinterop::OPENPBR_FEATURES_ALL == 0x3Fu,
              "OPENPBR_FEATURES_ALL changed — update the RenderSettings::openPbrFeatureMask "
              "default literal (Public/Pyxis/Renderer/Descs/RenderSettings.h) in lockstep.");
static_assert(pyxis::RenderSettings{}.openPbrFeatureMask
                  == pyxis::shaderinterop::OPENPBR_FEATURES_ALL,
              "RenderSettings::openPbrFeatureMask must default to OPENPBR_FEATURES_ALL — "
              "headless goldens and §25.O.3 adapter parity depend on all-features-on.");

namespace pyxis {

namespace {

// DLSS Stage 1 (rtx-realtime-alignment-design.md) — human-readable name
// for the DENOISER_* constants (RenderSettings.h), used only by the
// "denoiser: requested=... effective=..." log line below.
const char* DenoiserName(uint32_t value) noexcept {
  switch (value)
  {
    case DENOISER_DLSS: return "Dlss";
    case DENOISER_BUILTIN: return "Builtin";
    case DENOISER_OFF: return "Off";
    default: return "Unknown";
  }
}

// Copies a (possibly-truncated) std::string_view into the ABI-safe
// inline-owning ErrorMessage buffer — same truncate-not-allocate contract
// Error.h's own MakeError uses.
ErrorMessage MakeStatusMessage(std::string_view text) noexcept {
  ErrorMessage message{};
  const std::size_t copyLen = text.size() < ErrorMessage::CAPACITY
                                 ? text.size()
                                 : ErrorMessage::CAPACITY - 1;
  std::memcpy(message.data.data(), text.data(), copyLen);
  message.size = static_cast<uint16_t>(copyLen);
  return message;
}

}  // namespace

PyxisRenderer::PyxisRenderer(nvrhi::IDevice* device, GpuScene& scene, Profiler& profiler,
                             const RendererCreateDesc& desc, const VulkanContext* vulkanContext)
    : _profiler(&profiler),
      _scene(&scene),
      // RTX-alignment design (WP2-core) — the shared Set-0 SceneBindings,
      // constructed BEFORE `_graph` / the two RT passes below: both hold a
      // `SceneBindings&` from their ctor (they need `Layout()` immediately
      // to build their pipelines' globalBindingLayouts). Initializer order
      // here MUST match declaration order (PyxisRenderer.h) — §30 /W4
      // -Wreorder is warning-as-error.
      _sceneBindings(std::make_unique<SceneBindings>(device)),
      _graph(std::make_unique<RenderGraph>(device, &profiler)),
      _framesInFlight(desc.framesInFlight),
      // DLSS Stage 1 (rtx-realtime-alignment-design.md) — constructed here
      // (not lazily) so Probe() below always runs, regardless of whether
      // RenderFrame is ever called on this instance.
      _dlssProvider(std::make_unique<DlssProvider>()) {
  // DLSS Stage 1 — run the (idempotent) capability probe once at startup
  // so its detailed step-by-step result lands in the log immediately
  // (DLL discovery / symbol resolution / "Stage 2 pending" — see
  // DlssProvider::Probe()), independent of whether a frame ever renders.
  // RenderFrame's own per-frame "denoiser: requested=... effective=..."
  // line (below) reads the SAME cached Availability, so this doesn't
  // duplicate work.
  _dlssProvider->Probe();
  // DLSS Stage 2a — complete device interop now that a VulkanContext is
  // available (Stage 1's Probe() above only got as far as
  // ProbeStage::DeviceInteropPending: DLL discovery + symbol resolution,
  // no slInit). `vulkanContext` is null for any caller that hasn't been
  // updated to pass one (ViewerMode.cpp / HeadlessMode.cpp both do) —
  // Initialize() itself also no-ops gracefully if Probe() never reached
  // DeviceInteropPending (SDK not staged), so this is safe to call
  // unconditionally whenever a context IS available.
  if (vulkanContext != nullptr)
    _dlssProvider->Initialize(*vulkanContext);
  // RaytracedGBufferPass runs first (P4 pass split, extended per WP2-
  // core): one primary ray per pixel into the visibility buffer it owns
  // (threaded via PassContext::visibility), plus — new in WP2 — the
  // material G-buffer / denoiser guides and every id/geometry AOV + the
  // pixel-picker latch (moved here from the lighting pass). Runs only
  // when the supplied scene has a TLAS + camera; before that (e.g. an
  // empty scene), the pass early-outs — and so does the lighting pass,
  // on the same gates, so the stale visibility records are never
  // consumed.
  auto gbuffer = std::make_unique<RaytracedGBufferPass>(device, scene, *_sceneBindings);
  _gbufferPass = gbuffer.get();
  _graph->AddPass(std::move(gbuffer));
  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
  // the five Phase A signal passes run between RaytracedGBuffer and
  // RaytracedLighting. Each re-derives the primary hit from
  // RaytracedGBufferPass's visibility buffer (PassContext::visibility) —
  // some also read its gNormalRoughness guide (PassContext::gNormalRoughness)
  // — and writes its own demodulated signal, currently UNCONSUMED by
  // RaytracedLightingPass (which still owns the final image byte-for-byte
  // unchanged): DirectLighting (diffuse+specular, shadowed), IndirectDiffuse
  // (dome-mip ambient, no AO), AmbientOcclusion (RTAO), Reflections
  // (single-bounce mirror/rough-fallback), Translucency (the ported
  // front-to-back transparent-segment loop, segment 1 onward).
  auto directLighting = std::make_unique<DirectLightingPass>(device, scene, *_sceneBindings);
  _directLightingPass = directLighting.get();
  _graph->AddPass(std::move(directLighting));
  // SHARC world-space radiance cache (rtx-realtime-alignment-design.md,
  // 2026-07-07) — OPTIONAL infinite-bounce indirect-diffuse GI, gated on
  // RealTimeQuality::passMask bit 9 (PASS_MASK_SHARC_GI). SharcResolvePass owns
  // the three cache buffers + runs the per-frame Resolve compute; it MUST
  // precede IndirectDiffusePass so this frame's query reads freshly-resolved
  // data, and IndirectDiffusePass is ctor-injected a reference to reach the
  // buffers (which it binds in Set 1 and updates/queries when giMode is on).
  // No-op + byte-identical builtin path when the bit is clear.
  auto sharcResolve = std::make_unique<SharcResolvePass>(device);
  SharcResolvePass* const sharcResolveRaw = sharcResolve.get();
  _graph->AddPass(std::move(sharcResolve));
  auto indirectDiffuse =
      std::make_unique<IndirectDiffusePass>(device, scene, *_sceneBindings, *sharcResolveRaw);
  _indirectDiffusePass = indirectDiffuse.get();
  _graph->AddPass(std::move(indirectDiffuse));
  auto ambientOcclusion = std::make_unique<AmbientOcclusionPass>(device, scene, *_sceneBindings);
  _ambientOcclusionPass = ambientOcclusion.get();
  _graph->AddPass(std::move(ambientOcclusion));
  // ReflectionsPass also queries the SHARC cache (rtx-realtime-alignment-
  // design.md, 2026-07-07) at glossy reflection hits to supply the indirect-GI
  // term its cheap reflection-hit shade lacks (the too-dark glossy/metal
  // surfaces). Shares SharcResolvePass's buffers; no-op when giMode is off.
  auto reflections =
      std::make_unique<ReflectionsPass>(device, scene, *_sceneBindings, *sharcResolveRaw);
  _reflectionsPass = reflections.get();
  _graph->AddPass(std::move(reflections));
  auto translucency = std::make_unique<TranslucencyPass>(device, scene, *_sceneBindings);
  _translucencyPass = translucency.get();
  _graph->AddPass(std::move(translucency));
  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
  // the denoiser chain runs between TranslucencyPass and CompositePass:
  // SIGMA-class spatial filter on the direct diffuse/specular signal,
  // then ReLAX-class temporal accumulation + history-fix + à-trous on the
  // indirect-diffuse/reflections signals. All four are screen-space
  // COMPUTE passes (no SceneBindings). Gated end-to-end on
  // RealTimeQuality::passMask bit 5 (PASS_MASK_DENOISE, each pass's own
  // Execute() checks it). CompositePass is ctor-injected the first
  // (DenoiseShadowPass) and last (DenoiseAtrousPass) stage's pointers
  // below and picks raw-vs-denoised internally per the passMask bit —
  // PassContext::gDirectDiffuse/etc. themselves stay the RAW signals
  // throughout (this chain's own inputs; see the RenderFrame doc comment
  // on why redirecting those fields there would be a self-feedback bug).
  auto denoiseShadow = std::make_unique<DenoiseShadowPass>(device);
  DenoiseShadowPass* const denoiseShadowRaw = denoiseShadow.get();
  _denoiseShadowPass = denoiseShadowRaw;
  _graph->AddPass(std::move(denoiseShadow));
  auto denoiseTemporal = std::make_unique<DenoiseTemporalPass>(device);
  DenoiseTemporalPass* const denoiseTemporalRaw = denoiseTemporal.get();
  _denoiseTemporalPass = denoiseTemporalRaw;
  _graph->AddPass(std::move(denoiseTemporal));
  // DenoiseHistoryFixPass / DenoiseAtrousPass are ctor-injected a
  // reference to the pass they consume (same structural-dependency
  // convention every pass's `SceneBindings&` ctor param uses) so they
  // read that pass's CURRENT-frame output directly instead of round-
  // tripping through PassContext.
  auto denoiseHistoryFix = std::make_unique<DenoiseHistoryFixPass>(device, *denoiseTemporalRaw);
  DenoiseHistoryFixPass* const denoiseHistoryFixRaw = denoiseHistoryFix.get();
  _denoiseHistoryFixPass = denoiseHistoryFixRaw;
  _graph->AddPass(std::move(denoiseHistoryFix));
  auto denoiseAtrous = std::make_unique<DenoiseAtrousPass>(device, *denoiseHistoryFixRaw);
  DenoiseAtrousPass* const denoiseAtrousRaw = denoiseAtrous.get();
  _denoiseAtrousPass = denoiseAtrousRaw;
  _graph->AddPass(std::move(denoiseAtrous));
  // Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
  // 2026-07-06), work item 1 — DenoiseAoPass runs alongside the four
  // passes above (independent of the diffuse/specular chain — it only
  // touches gAo), gated on the SAME PASS_MASK_DENOISE bit. CompositePass
  // is ctor-injected its pointer too and picks its Output() instead of the
  // raw context.gAo when the bit is set — identical raw-vs-denoised
  // pattern to the shadow/atrous pointers above.
  auto denoiseAo = std::make_unique<DenoiseAoPass>(device);
  DenoiseAoPass* const denoiseAoRaw = denoiseAo.get();
  _denoiseAoPass = denoiseAoRaw;
  _graph->AddPass(std::move(denoiseAo));
  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
  // CompositePass replaces the retired RaytracedLightingPass megakernel:
  // it recombines the five signal passes' outputs (+ the G-buffer's
  // gAlbedo/gEmissive) into the final linear radiance, writing the fp32
  // linearColor scratch + the colorHdr / alpha AOVs (which move here from
  // the retired pass — transparency-composited coverage is only known
  // once every signal has been combined).
  auto composite = std::make_unique<CompositePass>(device, scene, *_sceneBindings, denoiseShadowRaw,
                                                   denoiseAtrousRaw, denoiseAoRaw);
  _compositePass = composite.get();
  _graph->AddPass(std::move(composite));
  // RTX-alignment design (rtx-realtime-alignment-design.md), "KEY FINDING
  // (2026-07-06): no true accumulation buffer" — AccumulationPass runs
  // next, between CompositePass and DlssPass: a true progressive running-
  // mean average of the raw composite radiance (see the pass's own header
  // for the full rationale). Gated on RealTimeQuality::passMask bit 7
  // (PASS_MASK_ACCUMULATE), clear by default (pure passthrough — byte-
  // identical to today's behaviour for every existing config/golden).
  auto accumulation = std::make_unique<AccumulationPass>(device);
  _accumulationPass = accumulation.get();
  _graph->AddPass(std::move(accumulation));
  // DLSS Stage 2a (rtx-realtime-alignment-design.md) — DlssPass runs next,
  // between CompositePass and AutoExposurePass: no-ops unless the
  // effective denoiser resolves to Dlss (RenderFrame's own resolution
  // logic below), in which case it upscales CompositePass's render-
  // resolution output to display resolution and redirects
  // PassContext::linearColor to that result (see PassContext.h's comment
  // on why that field is `mutable`) so AutoExposurePass/TonemapPass below
  // consume the upscaled image without knowing DLSS ran.
  auto dlssPass =
      std::make_unique<DlssPass>(device, *_dlssProvider, scene, *_sceneBindings);
  _dlssPass = dlssPass.get();
  _graph->AddPass(std::move(dlssPass));
  // AutoExposurePass (optional) runs between composite and tonemap: when
  // RenderSettings::autoExposure is set it reduces the fp32 linearColor to a
  // geometric-mean luminance (into the 8-byte stats buffer it owns, threaded to
  // TonemapPass via PassContext::autoExposureStats). No-op + zero overhead when
  // disabled (the default), so the byte-equal contract is untouched.
  auto autoExposure = std::make_unique<AutoExposurePass>(device);
  _autoExposurePass = autoExposure.get();
  _graph->AddPass(std::move(autoExposure));
  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
  // TaaPass runs next, BEFORE Tonemap (on the linear pre-tonemap color,
  // matching NRD guidance — see taa.slang's file header), gated on
  // RealTimeQuality::passMask bit 6 (PASS_MASK_TAA). Default OFF in
  // headless (the §33.7 byte-equal contract needs a fixed image); the
  // viewer default flip is Phase C's calibration work. No-op + zero
  // overhead when disabled.
  auto taa = std::make_unique<TaaPass>(device);
  _taaPass = taa.get();
  _graph->AddPass(std::move(taa));
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
                      "PyxisRenderer: initialised (RaytracedGBuffer + 5 signal passes + "
                      "5-pass denoiser chain (Shadow/Temporal/HistoryFix/Atrous/Ao) + Composite + "
                      "Accumulation + Dlss + AutoExposure + Taa + Tonemap + SsaaResolve + "
                      "BlitToSrgb registered)");
}

// Out-of-line dtor lives here so unique_ptr<RenderGraph>'s (and, since
// WP2-core, unique_ptr<SceneBindings>'s) deleter sees the complete type
// (the public header only forward-declares both). Same reason
// `=default` works here but wouldn't in the header.
PyxisRenderer::~PyxisRenderer() = default;

void PyxisRenderer::RenderFrame(nvrhi::ICommandList* commandList, const RenderSettings& settings,
                                const RenderTargets& targets) {
  if (!_graph || !commandList)
    return;

  // DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected
  // stance" + "DLSS scope includes upscaling") — resolve the AUTHORED
  // denoiser request against DlssProvider's (cached, cheap) capability
  // probe. Stage 1's probe never returns usable=true (no device interop
  // yet — that's Stage 2), so DENOISER_DLSS always downgrades to
  // DENOISER_BUILTIN; DENOISER_OFF is an explicit user choice, never a
  // downgrade. Re-derived every frame (a handful of comparisons) rather
  // than cached, since the authored value can change frame-to-frame (the
  // viewer's ImGui combo).
  const DlssProvider::Availability& dlssAvailability = _dlssProvider->Probe();
  const uint32_t requestedDenoiser = settings.realTimeQuality.denoiser;
  uint32_t effectiveDenoiser = requestedDenoiser;
  std::string_view downgradeReason;
  if (requestedDenoiser == DENOISER_DLSS && !dlssAvailability.usable)
  {
    effectiveDenoiser = DENOISER_BUILTIN;
    downgradeReason = dlssAvailability.reason;
  }

  // DLSS Stage 2b — graceful ladder: RR (Ray Reconstruction) -> SR +
  // builtin denoiser -> builtin native. `IsRRUsable()` only ever returns
  // true once DlssProvider::Initialize confirmed BOTH SR and RR usable
  // (see its own doc comment), so this is cheap and safe to re-derive
  // every frame exactly like `dlssAvailability` above — no separate probe.
  const bool dlssUsesRR = _dlssProvider->IsRRUsable();

  // DLSS Stage 2a/2b — two-resolution pipeline. `renderWidth`/`renderHeight`
  // default to native (== the display target's own dims) so the
  // Builtin/Off path stays byte-for-byte identical to Stage 1. A usable
  // provider can still fail to produce a resolution for THIS frame (no
  // display target bound at all — e.g. a device-only smoke test — or
  // slDLSSGetOptimalSettings/slDLSSDGetOptimalSettings itself failing);
  // either downgrades to Builtin for this frame rather than running the
  // graph at an undefined resolution.
  uint32_t renderWidth = settings.width;
  uint32_t renderHeight = settings.height;
  if (effectiveDenoiser == DENOISER_DLSS)
  {
    if (targets.color == nullptr)
    {
      effectiveDenoiser = DENOISER_BUILTIN;
      downgradeReason = "no display target bound this frame";
    }
    else
    {
      const nvrhi::TextureDesc& displayDescForRes = targets.color->getDesc();
      // RR runs "in the same Performance Quality Mode set for DLSS"
      // (ProgrammingGuideDLSS_RR.md 3.0) but exposes its own optimal-
      // settings entry point — query the one that matches the rung this
      // frame will actually evaluate.
      const DlssProvider::RenderResolution optimal =
          dlssUsesRR
              ? _dlssProvider->GetOptimalRenderResolutionRR(displayDescForRes.width,
                                                            displayDescForRes.height,
                                                            settings.realTimeQuality.dlssExecMode)
              : _dlssProvider->GetOptimalRenderResolution(displayDescForRes.width,
                                                          displayDescForRes.height,
                                                          settings.realTimeQuality.dlssExecMode);
      if (optimal.width == 0u || optimal.height == 0u)
      {
        effectiveDenoiser = DENOISER_BUILTIN;
        downgradeReason = dlssUsesRR ? "slDLSSDGetOptimalSettings failed this frame"
                                     : "slDLSSGetOptimalSettings failed this frame";
      }
      else
      {
        renderWidth = optimal.width;
        renderHeight = optimal.height;
      }
    }
  }

  // Log once at construction and again only when the resolution actually
  // changes (the viewer calls RenderFrame ~60x/sec — logging every frame
  // would drown the log).
  if (!_dlssStatusLogged || requestedDenoiser != _dlssStatus.requestedDenoiser
      || effectiveDenoiser != _dlssStatus.effectiveDenoiser)
  {
    std::string line = std::string{"denoiser: requested="} + DenoiserName(requestedDenoiser)
                      + " effective=" + DenoiserName(effectiveDenoiser);
    if (effectiveDenoiser != requestedDenoiser)
      line += " (reason: " + std::string{downgradeReason} + ")";
    if (effectiveDenoiser == DENOISER_DLSS)
      line += " renderRes=" + std::to_string(renderWidth) + "x" + std::to_string(renderHeight);
    Logging::Get().Info(log::RENDER, line);
    _dlssStatusLogged = true;
  }
  _dlssStatus.requestedDenoiser = requestedDenoiser;
  _dlssStatus.effectiveDenoiser = effectiveDenoiser;
  _dlssStatus.reason = MakeStatusMessage(downgradeReason);

  // DLSS Stage 2b — log the active ladder rung once per change, same
  // change-gated pattern as the "denoiser: requested=..." line above (not
  // merged into it: that line's own gate is requested/effective denoiser
  // changing, which won't fire on an RR<->SR flip that happens with
  // DENOISER_DLSS staying the effective value throughout).
  const bool dlssActiveThisFrame = (effectiveDenoiser == DENOISER_DLSS);
  if (dlssActiveThisFrame
      && (!_dlssRungLogged || dlssUsesRR != _dlssLastRungWasRR))
  {
    if (dlssUsesRR)
      Logging::Get().Info(log::RENDER, "dlss: RR active");
    else
      Logging::Get().Info(log::RENDER, "dlss: SR+builtin (RR unsupported: "
                                           + _dlssProvider->LastRRResult().reason + ")");
    _dlssRungLogged = true;
    _dlssLastRungWasRR = dlssUsesRR;
  }

  // Force/mask the denoise+TAA passMask bits on a LOCAL settings copy
  // based on the FINAL effective denoiser:
  //   - DENOISER_BUILTIN: authored passMask bits pass through unchanged
  //     (today's pre-Stage-2a behaviour).
  //   - DENOISER_DLSS + RR unusable (SR rung): rtx-realtime-alignment-
  //     design.md's documented Stage 2a semantic — "builtin denoise at
  //     renderRes + DLSS-SR upscale": the ReLAX/SIGMA chain is forced ON
  //     regardless of the authored bit (DLSS-SR alone only upscales, it
  //     doesn't denoise), TAA is forced OFF (DLSS's own temporal
  //     accumulation replaces it — running both would double-blend
  //     history at two different resolutions).
  //   - DENOISER_DLSS + RR usable (RR rung, Stage 2b): the OPPOSITE
  //     denoise forcing — RR IS the denoiser (ProgrammingGuideDLSS_RR.md's
  //     whole premise: it consumes RAW noisy per-signal radiance +
  //     G-buffer guides instead of a pre-denoised image), so
  //     PASS_MASK_DENOISE is forced OFF so CompositePass feeds it the raw
  //     signals. TAA stays forced OFF for the same reason as the SR rung.
  //   - DENOISER_OFF: both forced OFF regardless of the authored passMask
  //     (unchanged from Stage 1).
  // A copy (not a const_cast) because PassContext::settings is threaded to
  // every pass in the graph — CompositePass in particular re-checks this
  // SAME passMask bit to decide raw-vs-denoised (see its Execute()), so
  // gating here is sufficient without touching CompositePass/Denoise*/
  // TaaPass source (their existing self-gate on PASS_MASK_DENOISE /
  // PASS_MASK_TAA does the rest).
  RenderSettings effectiveSettings = settings;
  if (dlssActiveThisFrame)
  {
    if (dlssUsesRR)
      effectiveSettings.realTimeQuality.passMask &= ~shaderinterop::PASS_MASK_DENOISE;
    else
      effectiveSettings.realTimeQuality.passMask |= shaderinterop::PASS_MASK_DENOISE;
    effectiveSettings.realTimeQuality.passMask &= ~shaderinterop::PASS_MASK_TAA;
  }
  else if (effectiveDenoiser == DENOISER_OFF)
  {
    effectiveSettings.realTimeQuality.passMask &=
        ~(shaderinterop::PASS_MASK_DENOISE | shaderinterop::PASS_MASK_TAA);
  }

  // RTX-alignment design (rtx-realtime-alignment-design.md), "KEY FINDING
  // (2026-07-06): no true accumulation buffer" — PASS_MASK_ACCUMULATE is
  // an UNCONDITIONAL override on top of every branch above (checked
  // against the AUTHORED `settings`, not `effectiveSettings`, so it can't
  // be masked out by whatever the DLSS/builtin logic just decided).
  // AccumulationPass (registered between CompositePass and DlssPass)
  // accumulates the composite output CompositePass writes to linearColor;
  // the denoiser's own EMA history and TAA's own history are BOTH already
  // temporal averages, so leaving either enabled while this pass ALSO
  // averages across frames would double-average and bias the converged
  // result away from the true unbiased estimate this pass exists to reach
  // (see AccumulationPass.h's file header). Forcing both off here — the
  // same "local copy, single gate" shape the DLSS forcing above uses —
  // means CompositePass reads the raw (undenoised) signal-pass outputs
  // and TaaPass's own passMask self-gate no-ops, without either pass's
  // source needing to know AccumulationPass exists.
  if ((settings.realTimeQuality.passMask & shaderinterop::PASS_MASK_ACCUMULATE) != 0u)
  {
    effectiveSettings.realTimeQuality.passMask &=
        ~(shaderinterop::PASS_MASK_DENOISE | shaderinterop::PASS_MASK_TAA);
  }

  PassContext context{};
  context.commandList = commandList;
  context.profiler = _profiler;
  context.settings = &effectiveSettings;
  context.targets = &targets;
  context.frameIndex = _frameIndex++;
  // Active FIF comes from RendererCreateDesc (default 1) — caller
  // typically passes IDeviceManager::GetFramesInFlight(). Headless
  // raises this to 3 for §33.7 byte-equal EXR; viewer stays at 1
  // until the pacing knobs land at M11. RaytracedLightingPass's picker
  // readback asserts == 1 since the mapBuffer-without-fence path
  // would race past that.
  context.framesInFlight = _framesInFlight;
  // P3/P4 pass split — the renderer-internal scratch resources, sized off
  // `renderWidth`/`renderHeight` (== the display target's own dims unless
  // DLSS-SR is active, in which case they're the smaller optimal render
  // resolution resolved above — DLSS Stage 2a's two-resolution pipeline)
  // so the ray dispatches, signal passes, denoiser chain, and composite
  // all run at identical (render-resolution) extents:
  //   * visibility (P4): the per-pixel VisibilityGpu records
  //     RaytracedGBufferPass writes and RaytracedLightingPass reads. Owned by
  //     the GBuffer pass (EnsureVisibilityBuffer).
  //   * linearColor (P3): the fp32 LINEAR radiance the lighting raygen writes
  //     and TonemapPass reads. Owned by the lighting pass (EnsureLinearColor).
  //     DlssPass (Stage 2a) redirects this to its OWN display-resolution
  //     output during the graph walk when active — see PassContext.h's
  //     comment on why `linearColor` is `mutable`.
  // Every Ensure* call below runs here on the CPU path before the graph
  // executes — NEVER inside a pass Execute; recreated on a size change
  // only. All stay null when no display target is bound — the RT passes +
  // Tonemap then no-op (nothing to display into), matching the old
  // targets.color early-out.
  if (targets.color != nullptr) {
    const nvrhi::TextureDesc& colorDesc = targets.color->getDesc();
    // DLSS Stage 2a — the display-resolution output DlssPass upscales
    // into, allocated only when it's actually going to run this frame
    // (saves the extra RGBA32F allocation on every machine without the
    // SDK staged, i.e. every machine today). CPU frame path only, same
    // §30.10 discipline as every other Ensure* call here.
    if (_dlssPass != nullptr && effectiveDenoiser == DENOISER_DLSS) {
      auto* const dlss = static_cast<DlssPass*>(_dlssPass);
      dlss->EnsureOutput(colorDesc.width, colorDesc.height);
      dlss->EnsureDepthScratch(renderWidth, renderHeight);
    }
    if (_gbufferPass != nullptr) {
      context.visibility = static_cast<RaytracedGBufferPass*>(_gbufferPass)
                               ->EnsureVisibilityBuffer(renderWidth, renderHeight);
    }
    if (_compositePass != nullptr) {
      context.linearColor = static_cast<CompositePass*>(_compositePass)
                                ->EnsureLinearColor(renderWidth, renderHeight);
    }
    // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
    // thread RaytracedGBufferPass's material G-buffer into PassContext for
    // the five signal passes (only AmbientOcclusionPass / ReflectionsPass
    // read gNormalRoughness today; the rest are plumbed per the WP2
    // contract for later phases — see PassContext.h's doc comment).
    // gViewZ/gMotionVector are simply the caller's own optional
    // RenderTargets pointers (already cross-DLL, already in scope here) —
    // no new accessor needed for those two.
    if (_gbufferPass != nullptr) {
      auto* const gbuf = static_cast<RaytracedGBufferPass*>(_gbufferPass);
      context.gAlbedo = gbuf->Albedo();
      context.gNormalRoughness = gbuf->NormalRoughness();
      context.gEmissive = gbuf->Emissive();
      context.gSpecularAlbedo = gbuf->SpecularAlbedo();
    }
    context.gViewZ = targets.viewZAov;
    context.gMotionVector = targets.motionVector;
    // (Re)size the five signal passes' own outputs at the same cadence as
    // visibility/linearColor above — CPU frame path only, never inside
    // Execute (§30.10). RTX-alignment design, WP2-final — CompositePass is
    // now their real consumer, so (unlike WP2-signals) the Ensure* return
    // values ARE threaded into PassContext (mirrors the gAlbedo /
    // gNormalRoughness / gEmissive pattern above).
    if (_directLightingPass != nullptr) {
      auto* const directLighting = static_cast<DirectLightingPass*>(_directLightingPass);
      context.gDirectDiffuse = directLighting->EnsureOutputs(renderWidth, renderHeight);
      context.gDirectSpecular = directLighting->Specular();
    }
    if (_indirectDiffusePass != nullptr) {
      context.gIndirectDiffuse = static_cast<IndirectDiffusePass*>(_indirectDiffusePass)
                                     ->EnsureOutput(renderWidth, renderHeight);
    }
    if (_ambientOcclusionPass != nullptr) {
      context.gAo = static_cast<AmbientOcclusionPass*>(_ambientOcclusionPass)
                        ->EnsureOutput(renderWidth, renderHeight);
    }
    if (_reflectionsPass != nullptr) {
      auto* const reflections = static_cast<ReflectionsPass*>(_reflectionsPass);
      context.gReflections = reflections->EnsureOutput(renderWidth, renderHeight);
      context.gReflectionWeight = reflections->ReflectionWeight();
    }
    if (_translucencyPass != nullptr) {
      context.gTranslucency = static_cast<TranslucencyPass*>(_translucencyPass)
                                  ->EnsureOutput(renderWidth, renderHeight);
    }
    // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
    // size the denoiser chain's own outputs at the same cadence as every
    // other pass's scratch above (CPU frame path only, never inside
    // Execute — §30.10). DenoiseTemporalPass is NOT sized here: its
    // ping-pong history (DenoiserResources) self-sizes inside its own
    // Execute() the first time it runs each frame, and its output is
    // consumed only by DenoiseHistoryFixPass (ctor-injected reference,
    // within the SAME graph walk) — not by CompositePass directly, so
    // PassContext never needs a pointer to it.
    if (_denoiseShadowPass != nullptr) {
      static_cast<DenoiseShadowPass*>(_denoiseShadowPass)
          ->EnsureOutputs(renderWidth, renderHeight);
    }
    if (_denoiseHistoryFixPass != nullptr) {
      static_cast<DenoiseHistoryFixPass*>(_denoiseHistoryFixPass)
          ->EnsureOutputs(renderWidth, renderHeight);
    }
    if (_denoiseAtrousPass != nullptr) {
      static_cast<DenoiseAtrousPass*>(_denoiseAtrousPass)
          ->EnsureOutputs(renderWidth, renderHeight);
    }
    // NOTE: context.gDirectDiffuse / gDirectSpecular / gIndirectDiffuse /
    // gReflections stay the RAW signal-pass outputs assigned above — they
    // are the denoiser chain's OWN inputs (DenoiseShadowPass /
    // DenoiseTemporalPass read them from this SAME context). Redirecting
    // these fields to the denoised outputs here, before the graph runs,
    // would make the denoiser passes read their own (not-yet-written)
    // output as their input instead of the real raw signal — CompositePass
    // itself is ctor-injected the denoiser passes' pointers and picks
    // raw-vs-denoised per RealTimeQuality::passMask internally (see its
    // Execute()), so no PassContext field needs to carry both meanings.
    // RTX-alignment design (WP2-core) — refresh the shared Set-0
    // SceneBindings ONCE per frame (CPU path, before the graph walks):
    // uploads CameraUniforms + MotionVectorCameraUniforms (the single
    // now-shared derivation both RT passes used to duplicate
    // independently) + (WP2-final) RealTimeQualityUniforms, and rebuilds
    // the cached Set-0 binding set iff the scene's borrowed resources
    // changed. Every RT/compute pass in the graph consumes the result via
    // PassContext::sceneBindingSet instead of building their own Set-0
    // sets. Null when the scene has no TLAS yet — every such pass
    // early-outs on that, mirroring the pre-WP2 `res.tlas == nullptr` gate.
    if (_sceneBindings && _scene != nullptr && _scene->HasCamera()) {
      const SceneResources sceneRes = detail::SceneResourcesAccess::Get(*_scene);
      // DLSS Stage 2a — jitter must be ON whenever DLSS-SR is active even
      // though PASS_MASK_TAA is masked off above (TaaPass and DLSS never
      // both run — DlssPass.cpp feeds Streamline this EXACT SAME
      // per-frame offset via Passes/CameraJitter.h). DENOISER_OFF still
      // drops jitter entirely (no consumer wants it), matching Stage 1.
      const bool taaJitterEnabled =
          ((effectiveSettings.realTimeQuality.passMask & shaderinterop::PASS_MASK_TAA) != 0u)
          || (effectiveDenoiser == DENOISER_DLSS);
      // outputHeight feeds the ray-cone pixelSpreadRadians derivation — the
      // ACTUAL trace resolution (renderHeight), not the display height, so
      // texture-LOD footprints match what's really being rasterized/traced
      // at when DLSS-SR shrinks the internal resolution.
      context.sceneBindingSet =
          _sceneBindings
              ->Update(commandList, _scene->GetCamera(), renderHeight, sceneRes,
                       effectiveSettings, context.frameIndex, taaJitterEnabled,
                       effectiveSettings.seed)
              .Get();
    }
  }
  // Thread the AutoExposurePass's stats buffer to TonemapPass (AutoExposurePass
  // accumulates into it when enabled; TonemapPass reads it). Always available
  // (created once in the pass ctor); both passes no-op when auto-exposure is off.
  if (_autoExposurePass != nullptr) {
    context.autoExposureStats =
        static_cast<AutoExposurePass*>(_autoExposurePass)->StatsBuffer();
  }
  // P5 (design D2) / Q2 — spec-constant pipeline variants: make sure
  // the ACTIVE camera projection mode's RT/compute pipelines exist before
  // the graph walks. The GBuffer pass and CompositePass stay
  // projection-only (WP2-final — the retired RaytracedLightingPass was
  // the one pipeline keyed on (projectionMode, OpenPBR feature mask); the
  // five signal passes + CompositePass carry no OpenPBR feature-mask
  // runtime toggle). The perspective variants are built in each pass's
  // ctor; other keys materialize here the first frame they're requested.
  // Creation happens on this CPU frame path only — never inside a pass
  // Execute (§30.10); every hook no-ops once built.
  const uint32_t projectionMode = _scene != nullptr && _scene->HasCamera()
                                      ? _scene->GetCamera().projectionMode
                                      : 0u;
  if (_gbufferPass != nullptr) {
    static_cast<RaytracedGBufferPass*>(_gbufferPass)->EnsureProjectionPipeline();
  }
  // RTX-alignment design, WP2-signals — the five signal passes are
  // projection-mode-only (no OpenPBR feature-mask runtime toggle in
  // Phase A; see each pass's .h file header), same EnsureProjectionPipeline
  // shape as RaytracedGBufferPass.
  if (_directLightingPass != nullptr) {
    static_cast<DirectLightingPass*>(_directLightingPass)->EnsureProjectionPipeline();
  }
  if (_indirectDiffusePass != nullptr) {
    static_cast<IndirectDiffusePass*>(_indirectDiffusePass)->EnsureProjectionPipeline();
  }
  if (_ambientOcclusionPass != nullptr) {
    static_cast<AmbientOcclusionPass*>(_ambientOcclusionPass)->EnsureProjectionPipeline();
  }
  if (_reflectionsPass != nullptr) {
    // Specular MODEL GAP fix (rtx-realtime-alignment-design.md, 2026-07-07)
    // — lazily build the (projectionMode, stochasticReflections) variant
    // PASS_MASK_STOCHASTIC_REFLECTIONS selects this frame, same CPU-path-
    // only / never-inside-Execute discipline every other EnsureProjection-
    // Pipeline call here follows.
    const bool stochasticReflections =
        (effectiveSettings.realTimeQuality.passMask
         & shaderinterop::PASS_MASK_STOCHASTIC_REFLECTIONS) != 0u;
    static_cast<ReflectionsPass*>(_reflectionsPass)
        ->EnsureProjectionPipeline(stochasticReflections);
  }
  if (_translucencyPass != nullptr) {
    static_cast<TranslucencyPass*>(_translucencyPass)->EnsureProjectionPipeline();
  }
  // WP2-final — CompositePass replaces the retired RaytracedLightingPass
  // as the linearColor writer; same EnsureProjectionPipeline shape.
  if (_compositePass != nullptr) {
    static_cast<CompositePass*>(_compositePass)->EnsureProjectionPipeline();
  }
  // P6 review — pass-health degradation. The per-pass gates (_shadersOk,
  // latched variant-build failures) are pass-LOCAL and can fail
  // asymmetrically (one pipeline's .spv missing/corrupt, one lazy
  // variant build failing): without this, CompositePass would recombine
  // signals from a visibility buffer the GBuffer pass never wrote, and
  // Tonemap would display a linearColor CompositePass never wrote. Nulling
  // the scratch pointers here makes every downstream consumer take its
  // existing null early-out, degrading to the pre-split behavior of an
  // untouched display output instead of garbage frames.
  const bool gbufferOk = _gbufferPass != nullptr
                         && static_cast<RaytracedGBufferPass*>(_gbufferPass)
                                ->IsOperational(projectionMode);
  const bool compositeOk =
      _compositePass != nullptr
      && static_cast<CompositePass*>(_compositePass)->IsOperational(projectionMode);
  if (!gbufferOk)
    context.visibility = nullptr;
  if (!gbufferOk || !compositeOk)
    context.linearColor = nullptr;
  // Note: context.sceneBindingSet is NOT gated on gbufferOk/compositeOk —
  // it depends only on SceneBindings' own construction + TLAS/camera
  // availability (orthogonal to shader/pipeline health). A broken RT/
  // compute pipeline already stops that pass via the visibility/
  // linearColor null gates above; a valid sceneBindingSet with a broken
  // pipeline is inert (never dereferenced).
  // At SSAA factor > 1, give SsaaResolvePass its base-res LINEAR downsample target
  // (it owns the texture) and thread it to BlitToSrgbPass via the context. At
  // factor 1 it stays null and BlitToSrgbPass reads `color` directly.
  if (effectiveSettings.ssaaFactor > 1u && _ssaaPass != nullptr && targets.colorResolved != nullptr) {
    const uint32_t baseWidth = effectiveSettings.width / effectiveSettings.ssaaFactor;
    const uint32_t baseHeight = effectiveSettings.height / effectiveSettings.ssaaFactor;
    context.colorLinearResolved =
        static_cast<SsaaResolvePass*>(_ssaaPass)->EnsureLinearOutput(baseWidth, baseHeight);
  }

  const Profiler::CpuScope frameScope(*_profiler, "render.frame.cpu");
  // The graph runs RaytracedGBuffer → [DirectLighting, IndirectDiffuse,
  // AmbientOcclusion, Reflections, Translucency] → 5-pass denoiser chain
  // (Shadow, Temporal, HistoryFix, Atrous, Ao) → Composite → Accumulation →
  // Dlss → AutoExposure → Taa → Tonemap → SsaaResolve →
  // BlitToSrgb. Accumulation no-ops unless PASS_MASK_ACCUMULATE is set (in
  // which case denoise+TAA are already forced off above); Dlss no-ops
  // unless effective denoiser == Dlss (in which case Taa's own passMask
  // bit is already forced off above — they never both run); SsaaResolve
  // no-ops at factor < 2; BlitToSrgb no-ops when colorResolved is unbound
  // (Kit's path), so the headless / Omniverse paths are unaffected.
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
  // RTX-alignment design (rtx-realtime-alignment-design.md), "KEY FINDING
  // (2026-07-06): no true accumulation buffer" — AccumulationPass now
  // owns the one true accumulation buffer this method was always reserved
  // for (plan §18.6's doc comment predates the pass by name but describes
  // exactly this). Callers (viewer camera/settings-change handlers) call
  // this so the NEXT frame starts a fresh running mean instead of
  // blending against a stale one. No-op when the pass failed to
  // construct (shader/pipeline load failure — same defensive null check
  // every other borrowed pass pointer in this file uses).
  if (_accumulationPass != nullptr)
    static_cast<AccumulationPass*>(_accumulationPass)->Reset();
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
  // RTX-alignment design (WP2-core) — the pixel-picker latch moved from
  // RaytracedLightingPass to RaytracedGBufferPass (it moved with the
  // id/geometry AOVs it reads from). See raytraced_gbuffer.slang's file
  // header for the one documented WP2-core deviation this carries
  // (colorR/G/B now reflect unlit baseColor, not the final lit
  // radiance).
  if (_gbufferPass == nullptr)
    return {};
  // Static cast safe: we constructed _gbufferPass as a
  // RaytracedGBufferPass* in the ctor and never reassign it.
  // RaytracedGBufferPass derives from IRenderPass non-virtually so the
  // static cast round-trips cleanly (no RTTI involved — the renderer
  // build forbids /GR via §30 anyway).
  return static_cast<const RaytracedGBufferPass*>(_gbufferPass)->GetLastPickResult();
}

nvrhi::ITexture* PyxisRenderer::DebugSignalTexture(std::string_view name) const noexcept {
  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals.
  // Static casts safe by the same construction-time-only-assignment
  // argument LastPickResult's cast above documents.
  if (name == "directDiffuse") {
    return _directLightingPass != nullptr
               ? static_cast<const DirectLightingPass*>(_directLightingPass)->Diffuse()
               : nullptr;
  }
  if (name == "directSpecular") {
    return _directLightingPass != nullptr
               ? static_cast<const DirectLightingPass*>(_directLightingPass)->Specular()
               : nullptr;
  }
  if (name == "indirectDiffuse") {
    return _indirectDiffusePass != nullptr
               ? static_cast<const IndirectDiffusePass*>(_indirectDiffusePass)->Output()
               : nullptr;
  }
  if (name == "ao") {
    return _ambientOcclusionPass != nullptr
               ? static_cast<const AmbientOcclusionPass*>(_ambientOcclusionPass)->Output()
               : nullptr;
  }
  if (name == "reflections") {
    return _reflectionsPass != nullptr
               ? static_cast<const ReflectionsPass*>(_reflectionsPass)->Output()
               : nullptr;
  }
  if (name == "translucency") {
    return _translucencyPass != nullptr
               ? static_cast<const TranslucencyPass*>(_translucencyPass)->Output()
               : nullptr;
  }
  // WP2-final — CompositePass's own recombined pre-tonemap radiance
  // (the same texture threaded through PassContext::linearColor), for
  // --save-aov sanity dumps of the composite path.
  if (name == "composite") {
    return _compositePass != nullptr
               ? static_cast<const CompositePass*>(_compositePass)->Output()
               : nullptr;
  }
  return nullptr;
}

DlssStatus PyxisRenderer::GetDlssStatus() const noexcept {
  return _dlssStatus;
}

}  // namespace pyxis

// Pyxis renderer — frame rendering API.
//
// Plan §18.6. The renderer takes a `GpuScene&` as the canonical scene
// input. RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
// the retired RaytracedLightingPass megakernel is replaced by five Phase A
// signal passes (DirectLighting / IndirectDiffuse / AmbientOcclusion /
// Reflections / Translucency) + CompositePass; the linear §9 graph is now
// RaytracedGBuffer → [DirectLighting, IndirectDiffuse, AmbientOcclusion,
// Reflections, Translucency] → Composite → Tonemap → SsaaResolve →
// BlitToSrgb.

#pragma once

#include <Pyxis/Platform/Device/VulkanContext.h>
#include <Pyxis/Renderer/Descs/DlssStatus.h>
#include <Pyxis/Renderer/Descs/FrameProfile.h>
#include <Pyxis/Renderer/Descs/PickResult.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace nvrhi {
class IDevice;
class ICommandList;
class ITexture;
}  // namespace nvrhi

namespace pyxis {

// RenderGraph lives in Private/. Forward-declared here so PyxisRenderer
// can hold one by unique_ptr without exposing the definition publicly —
// the dtor is defined in PyxisRenderer.cpp where the type is complete.
class RenderGraph;
// SceneBindings lives in Private/Passes/ (RTX-alignment design, WP2-core
// — the shared Set-0 immutable binding layout every RT pass consumes).
// Forward-declared here for the same unique_ptr-to-incomplete-type
// reason as RenderGraph above.
class SceneBindings;
// DlssProvider lives in Private/Dlss/ (DLSS Stage 1 — see that header's
// file comment). Forward-declared here for the same unique_ptr-to-
// incomplete-type reason as RenderGraph / SceneBindings above.
class DlssProvider;

class PYXIS_RENDERER_API PyxisRenderer final {
 public:
  // DLSS Stage 2a (rtx-realtime-alignment-design.md) — `vulkanContext` is
  // the SAME VulkanContext the caller's IDeviceManager used to create the
  // VkInstance/VkDevice (IDeviceManager::GetVulkanContext()). Optional
  // (defaulted; a MINOR-additive trailing parameter — no existing call
  // site's source changes) because DlssProvider gracefully degrades to
  // Builtin when it's null (mirrors every other "SDK isn't staged"
  // degradation path) — but a caller that never passes it can never reach
  // effective=Dlss, since DlssProvider::Initialize needs it to call
  // slSetVulkanInfo. See DlssProvider.h for the full Stage 1 (discovery-
  // only) vs Stage 2a (real device interop) split.
  PyxisRenderer(nvrhi::IDevice* device, GpuScene& scene, Profiler& profiler,
                const RendererCreateDesc& desc, const VulkanContext* vulkanContext = nullptr);
  ~PyxisRenderer();

  PyxisRenderer(const PyxisRenderer&) = delete;
  PyxisRenderer& operator=(const PyxisRenderer&) = delete;

  // Renders one frame. Caller's responsibility: every render-target in
  // `targets` is in nvrhi::ResourceStates::RenderTarget when the call
  // returns (so the device manager's EndFrame can blit / present).
  // Threading: render thread only.
  void RenderFrame(nvrhi::ICommandList* commandList, const RenderSettings& settings,
                   const RenderTargets& targets);

  // Resize internal AOV / accumulation buffers. M3 path-trace writes
  // into a caller-allocated AOV color texture (no internal buffers
  // yet), so this is still a no-op; M5+ accumulation wires here.
  void Resize(uint32_t width, uint32_t height);

  // Resets accumulation. No-op until the M5+ accumulation buffer
  // exists — the M3 path-tracer renders one sample per frame straight
  // to the AOV color texture.
  void ResetAccumulation();

  [[nodiscard]] FrameProfile LastFrameProfile() const;

  // M7 follow-up — last successful pixel-pick readback. The renderer
  // copies the GPU's pick UAV into a staging buffer at the end of
  // each RenderFrame and maps it on the NEXT call, so the result
  // returned here lags the cursor by one frame (acceptable for a
  // hover readout). Caller must have supplied RenderTargets::pickResult
  // for the picker to have run; otherwise this returns the default-
  // constructed PickResult (depth=-1, instanceId=~0u).
  [[nodiscard]] PickResult LastPickResult() const noexcept;

  // Re-load every render-pass's shaders from disk and rebuild the
  // pipeline-state objects. The Slang compiler isn't linked into the
  // runtime — the .spv files this picks up are produced by ShaderMake
  // at build time, so the click-effect is "rebuild shaders externally
  // (cmake --build --target pyxis_renderer_shaders), then click
  // Reload to pick them up". Returns true iff every pass succeeded.
  // Caller must ensure no in-flight command buffer references the
  // current pipeline (typical use: device->waitForIdle() before).
  // Render thread only — no thread-safety on the underlying RenderGraph.
  [[nodiscard]] bool ReloadShaders() noexcept;

  // RTX-alignment design (rtx-realtime-alignment-design.md) — debug/
  // diagnostic accessor for the five Phase A signal passes' outputs
  // (DirectLightingPass / IndirectDiffusePass / AmbientOcclusionPass /
  // ReflectionsPass / TranslucencyPass) PLUS (WP2-final) the
  // CompositePass's own recombined pre-tonemap radiance. These are
  // renderer-internal scratch (not RenderTargets AOVs), so a
  // RenderTargets field per signal isn't warranted. `name` is one of:
  // "directDiffuse", "directSpecular", "indirectDiffuse", "ao",
  // "reflections", "translucency", "composite". Returns null for an
  // unrecognised name or when the owning pass isn't operational. Raw
  // opaque pointer — §18.9 (same convention RenderTargets' ITexture*
  // fields use); intended for --save-aov / debug dumps, render thread
  // only (no different a contract than RenderFrame's own targets
  // pointers).
  [[nodiscard]] nvrhi::ITexture* DebugSignalTexture(std::string_view name) const noexcept;

  // DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected
  // stance" + "DLSS scope includes upscaling") — the {requested, effective}
  // denoiser resolution computed by the LAST RenderFrame call (constructor
  // defaults reflect "no RenderFrame call yet", matching the only outcome
  // Stage 1 can ever reach). Lets the viewer's status line show the exact
  // same resolution RenderFrame's own "denoiser: requested=... effective=..."
  // log line reports. Render thread only, same contract as LastPickResult.
  [[nodiscard]] DlssStatus GetDlssStatus() const noexcept;

 private:
  Profiler* _profiler = nullptr;
  // Borrowed pointer to the scene the ctor received — RenderFrame reads
  // the active camera's projectionMode to drive the per-variant
  // IsOperational pass-health probes (P6 review). Same lifetime contract
  // as the pass pointers below: the caller keeps the GpuScene alive for
  // the renderer's lifetime (§18.6).
  GpuScene* _scene = nullptr;
  // RTX-alignment design (WP2-core) — the shared Set-0 SceneBindings
  // (camera cbuffers, TLAS, scene tables, bindless textures), owned
  // here and constructed BEFORE `_graph` below (the two RT passes hold
  // a `SceneBindings&` from their ctor, needed immediately to build
  // their pipelines' globalBindingLayouts). Declared BEFORE `_graph` so
  // it ALSO outlives it on teardown (members destruct in reverse
  // declaration order): `_graph` owns the passes that borrow this
  // pointer, so it must be torn down first. RenderFrame refreshes this
  // once per frame (SceneBindings::Update, on the CPU path before the
  // graph walks) and threads the resulting binding set through
  // PassContext::sceneBindingSet. Same forward-declared-incomplete-type
  // pattern as `_graph` — the dtor is defined in PyxisRenderer.cpp.
  std::unique_ptr<SceneBindings> _sceneBindings;
  std::unique_ptr<RenderGraph> _graph;
  // Borrowed pointer to the RaytracedGBufferPass the graph owns — kept
  // here so RenderFrame can ask it for its visibility buffer
  // (EnsureVisibilityBuffer) and thread it into PassContext::visibility
  // for the lighting pass (P4 pass split), and so LastPickResult() can
  // forward without a dynamic_cast or a graph-walk (RTX-alignment
  // design WP2-core moved the pixel-picker latch to this pass — see
  // RaytracedGBufferPass::GetLastPickResult). Forward-declared as
  // IRenderPass* so the public header doesn't pull in the private pass
  // header — the impl casts on demand.
  class IRenderPass* _gbufferPass = nullptr;
  // Borrowed pointer to the CompositePass the graph owns — kept so
  // RenderFrame can drive EnsureLinearColor / EnsureProjectionPipeline
  // (RTX-alignment design, WP2-final — replaces the retired
  // RaytracedLightingPass, which filled this same role pre-WP2-final).
  // Set in the ctor; cleared in the dtor (the RenderGraph unique_ptr
  // drops its passes first by member-order). Same forward-declared-
  // IRenderPass* + impl-side cast pattern as _gbufferPass.
  class IRenderPass* _compositePass = nullptr;
  // Borrowed pointer to the SsaaResolvePass the graph owns — kept so RenderFrame
  // can ask it for its base-res LINEAR downsample target (EnsureLinearOutput) and
  // thread it into PassContext::colorLinearResolved for BlitToSrgbPass. Same
  // forward-declared-IRenderPass* + impl-side cast pattern as _lightingPass.
  class IRenderPass* _ssaaPass = nullptr;
  // Borrowed pointer to the AutoExposurePass the graph owns — kept so RenderFrame
  // can thread its stats buffer into PassContext::autoExposureStats for
  // TonemapPass. Same forward-declared-IRenderPass* + impl-side cast pattern.
  class IRenderPass* _autoExposurePass = nullptr;
  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
  // borrowed pointers to the five Phase A signal passes the graph owns
  // (registered between _gbufferPass and _lightingPass), kept so
  // RenderFrame can drive their per-frame Ensure*/EnsureProjectionPipeline
  // hooks and thread the G-buffer guides into PassContext, and so
  // DebugSignalTexture() can forward without a graph-walk. Same
  // forward-declared-IRenderPass* + impl-side cast pattern as the pointers
  // above.
  class IRenderPass* _directLightingPass = nullptr;
  class IRenderPass* _indirectDiffusePass = nullptr;
  class IRenderPass* _ambientOcclusionPass = nullptr;
  class IRenderPass* _reflectionsPass = nullptr;
  class IRenderPass* _translucencyPass = nullptr;
  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
  // borrowed pointers to the four denoiser-chain passes (registered
  // between _translucencyPass and _compositePass) and TaaPass (registered
  // between _autoExposurePass and TonemapPass), all gated on
  // RealTimeQuality::passMask bits 5 / 6 respectively. Kept so RenderFrame
  // can redirect PassContext::gDirectDiffuse/gDirectSpecular/
  // gIndirectDiffuse/gReflections to the denoised outputs when the chain
  // is enabled (CompositePass itself is unaware of the denoiser — it just
  // reads whichever pointer RenderFrame put in the context, exactly like
  // every other signal). Same forward-declared-IRenderPass* + impl-side
  // cast pattern as the pointers above.
  class IRenderPass* _denoiseShadowPass = nullptr;
  class IRenderPass* _denoiseTemporalPass = nullptr;
  class IRenderPass* _denoiseHistoryFixPass = nullptr;
  class IRenderPass* _denoiseAtrousPass = nullptr;
  class IRenderPass* _taaPass = nullptr;
  // DLSS Stage 2a (rtx-realtime-alignment-design.md) — registered between
  // _compositePass and _autoExposurePass. Borrowed pointer, same
  // forward-declared-IRenderPass* + impl-side cast pattern as every other
  // pass pointer above. RenderFrame queries EnsureOutput on the CPU frame
  // path (never inside Execute) so the display-resolution texture exists
  // at the right size before the graph walk redirects
  // PassContext::linearColor to it (see PassContext.h's own comment on
  // why that field is `mutable`).
  class IRenderPass* _dlssPass = nullptr;
  uint64_t _frameIndex = 0;
  // Active frames-in-flight count from RendererCreateDesc. Threaded
  // into every PassContext so passes can size per-FIF rings.
  // Today only RaytracedLightingPass's picker readback consumes it
  // (asserts FIF == 1; bumping requires EventQuery support).
  uint32_t _framesInFlight = 1;

  // DLSS Stage 1 (rtx-realtime-alignment-design.md) — the capability probe
  // (DLL discovery + Streamline symbol resolution only; no device interop
  // until Stage 2). Owned here so its ONE probe runs once per renderer
  // instance regardless of how many frames render; constructed + probed
  // eagerly in the ctor so the detailed probe-result log line appears at
  // startup even if RenderFrame is never called (e.g. a device-only smoke
  // test). Forward-declared-DlssProvider* + impl-side unique_ptr, same
  // pattern as _sceneBindings/_graph above.
  std::unique_ptr<DlssProvider> _dlssProvider;
  // Snapshot of the last RenderFrame's {requested, effective} denoiser
  // resolution — GetDlssStatus() returns a copy of this.
  DlssStatus _dlssStatus;
  // True once RenderFrame has logged the "denoiser: requested=...
  // effective=..." line at least once. Cleared never — the line re-logs
  // only when the resolution actually changes (see RenderFrame's own
  // comment), so this just distinguishes "never logged" from "logged and
  // unchanged since".
  bool _dlssStatusLogged = false;
};

}  // namespace pyxis

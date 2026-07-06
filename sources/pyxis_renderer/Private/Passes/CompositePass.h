// Pyxis renderer — CompositePass, ONE shader file (RTX-alignment design,
// rtx-realtime-alignment-design.md, WP2-final).
//
// Recombines the five Phase A signal passes' outputs (DirectLighting,
// IndirectDiffuse, AmbientOcclusion, Reflections, Translucency) plus
// RaytracedGBufferPass's material G-buffer (gAlbedo / gEmissive) into the
// final linear radiance the retired RaytracedLightingPass megakernel used
// to compute directly inside its raygen. Runs after TranslucencyPass, before
// AutoExposurePass / TonemapPass — see composite.slang's file header for the
// exact recombination formula and its documented parity notes vs the old
// megakernel (reflections are now single-bounce direct+env per the
// signal-pass semantics; the SpecWeight factor uses the same
// OpenPBRReflectionWeight helper the old closesthit applied, computed by
// ReflectionsPass at the primary surface and threaded in via
// gReflectionWeight).
//
// A COMPUTE pass, not an RT pass: no TraceRay of its own for the signal
// recombination. It only reconstructs the camera ray (BuildCameraRay) and
// samples the dome background (SampleDomeBackground) for MISS pixels —
// exactly what the old megakernel's raygen did for its primary-miss case,
// NOT the signal recombination (miss pixels never populated any signal
// texture meaningfully). Binds the shared Set-0 SceneBindings layout
// (camera + lights + dome) alongside its own Set-1 signal-texture I/O — the
// same two-set shape every RT pass uses, but on a compute pipeline
// (SceneBindings' Set-0 layout visibility mask was broadened to include
// Compute for this — see SceneBindings.h's doc comment).
//
// Also writes the colorHdr and alpha AOVs (RenderTargets), which move here
// from the retired RaytracedLightingPass per the WP2-final contract:
// transparency-composited coverage is only fully known once this pass has
// combined the primary surface's signals with the translucency layer.
//
// Bindings — Set 0 is the shared SceneBindings layout. Set 1 (this pass's
// own I/O, matched to composite.slang's `[[vk::binding(N, 1)]]`
// declarations):
//   set=1, binding=0  : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1  : Texture2D<float4> gAlbedo          (GBuffer guide, SRV)
//   set=1, binding=2  : Texture2D<float4> gEmissive        (GBuffer guide, SRV)
//   set=1, binding=3  : Texture2D<float4> gAo               (AmbientOcclusionPass, SRV)
//   set=1, binding=4  : Texture2D<float4> gDirectDiffuse    (DirectLightingPass, SRV)
//   set=1, binding=5  : Texture2D<float4> gDirectSpecular   (DirectLightingPass, SRV)
//   set=1, binding=6  : Texture2D<float4> gIndirectDiffuse  (IndirectDiffusePass, SRV)
//   set=1, binding=7  : Texture2D<float4> gReflections      (ReflectionsPass, SRV)
//   set=1, binding=8  : Texture2D<float4> gReflectionWeight (ReflectionsPass, SRV)
//   set=1, binding=9  : Texture2D<float4> gTranslucency     (TranslucencyPass, SRV)
//   set=1, binding=10 : RWTexture2D<float4> gLinearColor    (RGBA32F, UAV — TonemapPass input)
//   set=1, binding=11 : RWTexture2D<float4> gOutColorHdr    (RGBA16F, UAV — colorHdr AOV)
//   set=1, binding=12 : RWTexture2D<float>  gOutAlpha       (R8_UNORM, UAV — alpha AOV)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class GpuScene;
class SceneBindings;
struct RenderTargets;
class DenoiseShadowPass;
class DenoiseAtrousPass;
class DenoiseAoPass;

class CompositePass final : public IRenderPass {
 public:
  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
  // `denoiseShadowPass` / `denoiseAtrousPass` are OPTIONAL (nullable)
  // borrowed pointers to the denoiser chain's first and last stages.
  // When RealTimeQuality::passMask bit 5 (PASS_MASK_DENOISE) is set,
  // Execute() reads gDirectDiffuse/gDirectSpecular from
  // `denoiseShadowPass` and gIndirectDiffuse/gReflections from
  // `denoiseAtrousPass` INSTEAD of the raw signal-pass outputs
  // PassContext carries — those PassContext fields stay the raw signals
  // (the denoiser chain's own inputs; see PyxisRenderer::RenderFrame's
  // doc comment on why they can't be overwritten there). Null pointers
  // (or a clear passMask bit) fall back to the pre-Phase-B raw-signal
  // path exactly.
  //
  // `denoiseAoPass` (noise-floor + vegetation spec, rtx-realtime-
  // alignment-design.md, 2026-07-06, work item 1) is the same optional/
  // nullable shape: when PASS_MASK_DENOISE is set, Execute() reads gAo
  // from `denoiseAoPass` instead of the raw context.gAo.
  CompositePass(nvrhi::IDevice* device, GpuScene& scene, SceneBindings& sceneBindings,
               DenoiseShadowPass* denoiseShadowPass = nullptr,
               DenoiseAtrousPass* denoiseAtrousPass = nullptr,
               DenoiseAoPass* denoiseAoPass = nullptr);
  ~CompositePass() override;
  CompositePass(const CompositePass&) = delete;
  CompositePass& operator=(const CompositePass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.Composite"; }

  // Re-load the compute shader's .spv from disk, recreate both projection
  // variants' pipelines. Same contract as every other pass's ReloadShaders.
  [[nodiscard]] bool ReloadShaders() noexcept override;

  // The full-render-resolution fp32 LINEAR radiance scratch this pass
  // writes (Set 1 binding 10, gLinearColor) and TonemapPass reads — the
  // same role RaytracedLightingPass::EnsureLinearColor filled before its
  // retirement. RGBA32F so the display transform sees bit-identical values
  // to the in-shader float3 the old megakernel's raygen wrote. Owned here
  // (this pass has the device + is the writer); allocated/cached at
  // `width` x `height`, recreated on a size change only — NEVER inside
  // Execute. PyxisRenderer calls this each RenderFrame and threads the
  // result through PassContext::linearColor.
  [[nodiscard]] nvrhi::ITexture* EnsureLinearColor(uint32_t width, uint32_t height);

  // Same per-projection-mode variant scheme as every other pass — this
  // pass's PROJECTION_MODE axis only matters for the miss-pixel dome
  // background's BuildCameraRay reconstruction. Called on PyxisRenderer's
  // CPU frame path, never inside Execute (§30.10).
  void EnsureProjectionPipeline();

  [[nodiscard]] bool IsOperational(uint32_t projectionMode) const noexcept {
    if (!_shadersOk)
      return false;
    const std::size_t variant = (projectionMode == 1u) ? 1u : 0u;
    if (_pipelines[variant])
      return true;
    return !_variantBuildFailed[variant];
  }

  // Debug/diagnostic accessor — DebugSignalTexture("composite") sanity
  // dump of the pre-tonemap composited radiance. Same convention as the
  // five signal passes' own Output() accessors.
  [[nodiscard]] nvrhi::ITexture* Output() const noexcept { return _linearColor.Get(); }

 private:
  // Build the Set-1 binding set for the supplied signal textures + AOV
  // targets. Cached via an FNV1a-64 hash over every borrowed pointer
  // (same scheme TonemapPass uses — there are too many bound resources
  // for a fixed-size snapshot array to stay readable).
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::IBuffer* visibility, nvrhi::ITexture* albedo, nvrhi::ITexture* emissive,
      nvrhi::ITexture* ambientOcclusion, nvrhi::ITexture* directDiffuse,
      nvrhi::ITexture* directSpecular, nvrhi::ITexture* indirectDiffuse,
      nvrhi::ITexture* reflections, nvrhi::ITexture* reflectionWeight,
      nvrhi::ITexture* translucency, nvrhi::ITexture* linearColor, nvrhi::ITexture* colorHdr,
      nvrhi::ITexture* alphaAov);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;
  SceneBindings* _sceneBindings = nullptr;  // borrowed; owns the shared Set-0 layout + set.
  // Phase B — see the ctor's doc comment. Borrowed, nullable.
  DenoiseShadowPass* _denoiseShadowPass = nullptr;
  DenoiseAtrousPass* _denoiseAtrousPass = nullptr;
  DenoiseAoPass* _denoiseAoPass = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _passLayout;

  static constexpr std::size_t PROJECTION_VARIANT_COUNT = 2;
  std::array<nvrhi::ComputePipelineHandle, PROJECTION_VARIANT_COUNT> _pipelines;
  std::array<bool, PROJECTION_VARIANT_COUNT> _variantBuildFailed{};

  nvrhi::TextureHandle _linearColor;
  uint32_t _linearColorW = 0;
  uint32_t _linearColorH = 0;
  bool _linearColorCreateFailedLogged = false;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _shadersOk = false;

  // Tiny no-UAV fallbacks for the two AOVs this pass owns (moved here from
  // RaytracedLightingPass) — bound when the caller doesn't supply that AOV
  // (headless mode + the M2-era color-only paths).
  nvrhi::TextureHandle _fallbackColorHdrAov;
  nvrhi::TextureHandle _fallbackAlphaAov;
};

}  // namespace pyxis

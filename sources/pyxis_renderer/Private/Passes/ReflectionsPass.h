// Pyxis renderer — ReflectionsPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, WP2-signals — Phase A signal split).
//
// RTX-RT single-bounce reflections: reads the PRIMARY surface's normal +
// roughness straight from RaytracedGBufferPass's gNormalRoughness guide
// (no material re-eval needed for the primary surface — only geometry).
// If roughness < REFLECTION_MAX_ROUGHNESS (ShaderInterop.slang):
// traces ONE mirror-direction ray; at the hit, evaluates the FULL
// material (EvaluateSurfaceMaterial) and runs the shared direct-light
// loop WITHOUT shadow rays (EvaluateDirectLightSample, traceShadow =
// false — deliberately cheap per the design's Phase A cost cap) plus
// emissive; misses sample the dome background (SampleDomeBackground,
// shared with the primary/reflection-continuation miss paths). Rougher
// surfaces skip the trace entirely and take a roughness-proportional
// dome-mip sample instead (the "rough fallback" RTX-RT documents).
// Output gReflections.rgb = radiance, .a = hitT (miss / fallback: -1).
//
// Intentional look change vs the existing megakernel's recursive mirror
// (documented in the design doc): single bounce, direct-only (no
// shadows) at the reflection hit — matches RTX-RT's maxReflectionBounces
// = 1, not a regression.
//
// Bindings — Set 0 is the shared SceneBindings layout. Set 1 (this
// pass's own I/O, matched to reflections.slang's `[[vk::binding(N, 1)]]`
// declarations):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : Texture2D<float4> gNormalRoughness (GBuffer guide, SRV)
//   set=1, binding=2 : RWTexture2D<float4> gReflections (RGBA16F)
//   set=1, binding=3 : RWTexture2D<float4> gReflectionWeight (RGBA16F, WP2-final —
//                       CompositePass's SpecWeight; see reflections.slang)

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
class SharcResolvePass;

class ReflectionsPass final : public IRenderPass {
 public:
  ReflectionsPass(nvrhi::IDevice* device, GpuScene& scene, SceneBindings& sceneBindings,
                  SharcResolvePass& sharc);
  ~ReflectionsPass() override;
  ReflectionsPass(const ReflectionsPass&) = delete;
  ReflectionsPass& operator=(const ReflectionsPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.Reflections"; }

  [[nodiscard]] nvrhi::ITexture* EnsureOutput(uint32_t width, uint32_t height);
  // Specular MODEL GAP fix (rtx-realtime-alignment-design.md, 2026-07-07) —
  // `stochasticReflections` selects the SAME axis Execute() reads from
  // RealTimeQuality::passMask bit 8 (PASS_MASK_STOCHASTIC_REFLECTIONS) each
  // frame; lazily builds that (projectionMode, stochasticReflections)
  // variant the first time it's requested, same shape the projection-only
  // axis already used before this fix (orthographic was the only lazy
  // variant; now toggling the stochastic bit at runtime can ALSO trigger a
  // lazy build, e.g. a viewer hotkey — see Execute()'s own fallback).
  void EnsureProjectionPipeline(bool stochasticReflections = false);

  // NOTE: does not account for the stochastic axis (checks the
  // stochastic-OFF variant only) — nothing outside this pass currently
  // queries ReflectionsPass::IsOperational (unlike RaytracedGBufferPass /
  // CompositePass's own IsOperational, which gate PyxisRenderer's frame-
  // health fallback), so the extra axis was not worth this accessor's API
  // churn. Execute() itself independently no-ops on a missing/failed
  // variant regardless of which axis caused it.
  [[nodiscard]] bool IsOperational(uint32_t projectionMode) const noexcept {
    if (!_shadersOk)
      return false;
    const std::size_t variant = VariantIndex(projectionMode, /*stochasticReflections=*/false);
    if (_pipelines[variant] && _shaderTables[variant])
      return true;
    return !_variantBuildFailed[variant];
  }

  [[nodiscard]] nvrhi::ITexture* Output() const noexcept { return _output.Get(); }
  // WP2-final — CompositePass's SpecWeight input (see reflections.slang's
  // RayGenMain / OpenPBRReflectionWeight). Same lifetime contract as Output().
  [[nodiscard]] nvrhi::ITexture* ReflectionWeight() const noexcept {
    return _reflectionWeight.Get();
  }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::IBuffer* visibility,
                                                              nvrhi::ITexture* normalRoughness);

  // Specular MODEL GAP fix (2026-07-07) — the pipeline-variant space grew
  // from a single projectionMode axis (2 variants) to projectionMode x
  // stochasticReflections (4 variants), same "spec constant folded at
  // pipeline-creation time" shape as SPEC_ID_PROJECTION_MODE. Kept as a
  // free function (not a member) so PROJECTION_VARIANT_COUNT below stays a
  // simple compile-time array size.
  [[nodiscard]] static constexpr std::size_t VariantIndex(uint32_t projectionMode,
                                                           bool stochasticReflections) noexcept {
    return (projectionMode == 1u ? 1u : 0u) * 2u + (stochasticReflections ? 1u : 0u);
  }

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;
  SceneBindings* _sceneBindings = nullptr;
  SharcResolvePass* _sharc = nullptr;  // SHARC cache (hash+resolved) queried at glossy hits

  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _closestHitShader;
  nvrhi::ShaderHandle _missShader;
  // Occlusion-aware-ambient follow-up (rtx-realtime-alignment-design.md,
  // 2026-07-06) — the short AO ray ClosestHitMain fires at the reflection
  // hit gets its OWN hit group (any-hit only) + miss shader; see
  // reflections.slang's file header for why this can't share the primary
  // reflection ray's HitGroupDefault/MissMain.
  nvrhi::ShaderHandle _aoAnyHitShader;
  nvrhi::ShaderHandle _aoMissShader;
  nvrhi::BindingLayoutHandle _passLayout;

  // 2 (projectionMode) x 2 (stochasticReflections) — see VariantIndex above.
  static constexpr std::size_t PROJECTION_VARIANT_COUNT = 4;
  std::array<nvrhi::rt::PipelineHandle, PROJECTION_VARIANT_COUNT> _pipelines;
  std::array<nvrhi::rt::ShaderTableHandle, PROJECTION_VARIANT_COUNT> _shaderTables;
  std::array<bool, PROJECTION_VARIANT_COUNT> _variantBuildFailed{};

  nvrhi::TextureHandle _output;
  // WP2-final — CompositePass's SpecWeight input, created/resized in
  // lockstep with `_output` (same EnsureOutput call).
  nvrhi::TextureHandle _reflectionWeight;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;

  std::unordered_map<nvrhi::IBuffer*, nvrhi::BindingSetHandle> _bindingSetCache;
  std::array<const void*, 2> _lastBindings{};

  bool _shadersOk = false;
};

}  // namespace pyxis

// Pyxis renderer — TranslucencyPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, WP2-signals — Phase A signal split).
//
// Ports the existing front-to-back transparent-segment loop (previously
// only in raytraced_lighting_raygen.slang) into its own signal: re-shades
// the primary hit via the shared ShadeSurfaceHit (bit-identical inputs —
// same visibility-buffer reconstruction every RT pass uses) to get its
// `transmittance` (the SAME value the real composite gates on — reused,
// not re-derived, so "is this surface transmissive" never drifts from
// the real pipeline's answer), then continues the SAME segment loop
// (identical epsilons / break thresholds / REFL_MAX_SEGMENTS /
// MAX_TRANSPARENT_SEGMENTS caps) from segment 1 onward — i.e., only the
// radiance from BEHIND the primary surface (the primary's own opaque
// lit/emitted contribution is DirectLighting/IndirectDiffuse/
// Reflections' job in this split architecture). Reuses the existing
// megakernel closesthit/miss/anyhit machinery verbatim (ShadeSurfaceHit
// itself fires its own shadow/AO/reflection sub-rays at each segment,
// exactly as today) — the "pragmatic Phase A port" the design's WP2
// contract table calls for.
//
// gTranslucency.rgb = the segment-1-onward front-to-back composite
// (premultiplied by the primary surface's transmittance — "premultiplied
// transmitted radiance"). gTranslucency.a = coverage, defined as
// `1 - max(transmittance channels)` at the primary hit (0 = fully
// see-through primary / nothing to report here, 1 = fully opaque primary
// / no translucency) — reuses the same transmittance value already
// computed, no second material evaluation needed.
//
// Bindings — Set 0 is the shared SceneBindings layout. Set 1 (this
// pass's own I/O, matched to translucency.slang's `[[vk::binding(N, 1)]]`
// declarations):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : RWTexture2D<float4> gTranslucency (RGBA16F)

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

class TranslucencyPass final : public IRenderPass {
 public:
  // `sharc` supplies the SHARC radiance-cache buffers this pass's terminal
  // through-glass segment QUERIES (read-only) when PASS_MASK_SHARC_GI is on —
  // same structural-dependency convention ReflectionsPass uses. Borrowed;
  // owned by PyxisRenderer, outlives this pass.
  TranslucencyPass(nvrhi::IDevice* device, GpuScene& scene, SceneBindings& sceneBindings,
                   SharcResolvePass& sharc);
  ~TranslucencyPass() override;
  TranslucencyPass(const TranslucencyPass&) = delete;
  TranslucencyPass& operator=(const TranslucencyPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.Translucency"; }

  [[nodiscard]] nvrhi::ITexture* EnsureOutput(uint32_t width, uint32_t height);
  void EnsureProjectionPipeline();

  [[nodiscard]] bool IsOperational(uint32_t projectionMode) const noexcept {
    if (!_shadersOk)
      return false;
    const std::size_t variant = (projectionMode == 1u) ? 1u : 0u;
    if (_pipelines[variant] && _shaderTables[variant])
      return true;
    return !_variantBuildFailed[variant];
  }

  [[nodiscard]] nvrhi::ITexture* Output() const noexcept { return _output.Get(); }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::IBuffer* visibility);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;
  SceneBindings* _sceneBindings = nullptr;
  SharcResolvePass* _sharc = nullptr;  // borrowed — see ctor doc.

  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _closestHitShader;
  nvrhi::ShaderHandle _missShader;
  // ShadeSurfaceHit's own shadow/AO rays hardcode MissShaderIndex = 1 —
  // this pipeline's shader table needs a matching slot (see
  // translucency.slang's ShadowMissMain doc comment for how this was
  // found: an OOB shader-table read crashed the GPU without it).
  nvrhi::ShaderHandle _shadowMissShader;
  nvrhi::ShaderHandle _anyHitShader;
  nvrhi::BindingLayoutHandle _passLayout;

  static constexpr std::size_t PROJECTION_VARIANT_COUNT = 2;
  std::array<nvrhi::rt::PipelineHandle, PROJECTION_VARIANT_COUNT> _pipelines;
  std::array<nvrhi::rt::ShaderTableHandle, PROJECTION_VARIANT_COUNT> _shaderTables;
  std::array<bool, PROJECTION_VARIANT_COUNT> _variantBuildFailed{};

  nvrhi::TextureHandle _output;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;

  std::unordered_map<nvrhi::IBuffer*, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _shadersOk = false;
};

}  // namespace pyxis

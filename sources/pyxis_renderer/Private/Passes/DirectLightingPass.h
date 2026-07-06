// Pyxis renderer — DirectLightingPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, WP2-signals — Phase A signal split).
//
// Re-derives the primary hit's full material (EvaluateSurfaceMaterial,
// shared with every other RT pass) from the visibility buffer, loops
// every non-Dome light (EvaluateDirectLightSample, shading.slang — the
// SAME selection/attenuation/shadow-ray math ShadeSurfaceHit's own loop
// uses), and accumulates the diffuse/specular-SEPARATED closure
// (EvaluateOpenPBRDirectSplit, openpbr_material.slang) into two
// demodulated outputs. One shadow ray per light, same flags/bias as
// ShadeSurfaceHit's loop. `.a` of gDirectDiffuse carries the closest
// occluder distance for the DOMINANT light (highest raw intensity among
// enabled non-Dome lights; 0 = unshadowed) — a future SIGMA (shadow
// denoiser) guide, unconsumed today.
//
// Demodulation: gDirectDiffuse.rgb divides by the primary surface's
// baseColor (guarded — DemodulateByAlbedo zeroes a channel instead of
// dividing when albedo <= ~0, e.g. pure metal). gDirectSpecular has no
// comparably clean per-channel albedo to demodulate against (the
// specular lobe's effective albedo is view/roughness-dependent — see
// GgxDirectionalAlbedoAB) — stored UN-demodulated, a documented Phase A
// simplification (the design's own contract text anticipates this: "if
// no clean spec albedo exists, store un-demodulated and note it").
//
// Bindings — Set 0 is the shared SceneBindings layout. Set 1 (this
// pass's own I/O, matched to direct_lighting.slang's
// `[[vk::binding(N, 1)]]` declarations):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : RWTexture2D<float4> gDirectDiffuse  (RGBA16F, demodulated)
//   set=1, binding=2 : RWTexture2D<float4> gDirectSpecular (RGBA16F, NOT demodulated)

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

class DirectLightingPass final : public IRenderPass {
 public:
  DirectLightingPass(nvrhi::IDevice* device, GpuScene& scene, SceneBindings& sceneBindings);
  ~DirectLightingPass() override;
  DirectLightingPass(const DirectLightingPass&) = delete;
  DirectLightingPass& operator=(const DirectLightingPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.DirectLighting"; }

  // (Re)creates BOTH outputs at the display target's dims, one resize
  // cadence (mirrors RaytracedGBufferPass::EnsureVisibilityBuffer's
  // multi-texture pattern). Returns the diffuse target (non-null iff
  // both textures were created); callers that need the specular target
  // use Specular() after a successful call.
  [[nodiscard]] nvrhi::ITexture* EnsureOutputs(uint32_t width, uint32_t height);
  void EnsureProjectionPipeline();

  [[nodiscard]] bool IsOperational(uint32_t projectionMode) const noexcept {
    if (!_shadersOk)
      return false;
    const std::size_t variant = (projectionMode == 1u) ? 1u : 0u;
    if (_pipelines[variant] && _shaderTables[variant])
      return true;
    return !_variantBuildFailed[variant];
  }

  [[nodiscard]] nvrhi::ITexture* Diffuse() const noexcept { return _diffuse.Get(); }
  [[nodiscard]] nvrhi::ITexture* Specular() const noexcept { return _specular.Get(); }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::IBuffer* visibility);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;
  SceneBindings* _sceneBindings = nullptr;

  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _shadowMissShader;
  nvrhi::ShaderHandle _anyHitShader;
  nvrhi::BindingLayoutHandle _passLayout;

  static constexpr std::size_t PROJECTION_VARIANT_COUNT = 2;
  std::array<nvrhi::rt::PipelineHandle, PROJECTION_VARIANT_COUNT> _pipelines;
  std::array<nvrhi::rt::ShaderTableHandle, PROJECTION_VARIANT_COUNT> _shaderTables;
  std::array<bool, PROJECTION_VARIANT_COUNT> _variantBuildFailed{};

  nvrhi::TextureHandle _diffuse;
  nvrhi::TextureHandle _specular;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;

  std::unordered_map<nvrhi::IBuffer*, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _shadersOk = false;
};

}  // namespace pyxis
